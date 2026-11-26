/*
 * venture-assignee.c - Whom a record may be handed to
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * One definition of "a member of this organization", used twice: by the
 * save validator that refuses a name outside it, and by the list the
 * picker and the members lookup offer. Two definitions would drift, and
 * the drift shows up as a picker offering somebody the save then refuses.
 */

#include "venture.h"

#include <string.h>

#define VENTURE_ASSIGNEE_STATE_KEY "venture-assignee-installed"

/* ==========================================================================
 * Who counts
 * ========================================================================== */

/*
 * Whether memberships exist at all. With organization access off the type
 * is not enabled and nothing reads a membership row: every active user
 * reads every organization, so every active user may be given its work.
 */
static gboolean
venture_assignee_memberships_on(void)
{
	return venture_entity_registry_is_type_enabled(
		venture_entity_registry_get_default(), "organization_membership");
}

/*
 * Whether @user reads every organization by role: an owner or an
 * administrator of the install. Not in a hosted install, where those roles
 * belong to the operator and a workspace's people are its members.
 */
static gboolean
venture_assignee_reads_everything(
	VentureDatabase	*database,
	VentureEntity	*user
){
	VentureUserRole role;

	if (venture_tenant_service_is_enabled(venture_tenant_service_get(database)))
		return FALSE;

	role = VENTURE_USER_ROLE_VIEWER;
	g_object_get(user, "role", &role, NULL);

	return (VENTURE_USER_ROLE_OWNER == role) || (VENTURE_USER_ROLE_ADMIN == role);
}

/* An active user row, not deleted. */
static gboolean
venture_assignee_user_active(VentureEntity *user)
{
	gboolean active;

	active = FALSE;

	if ((NULL == user) || venture_entity_is_deleted(user))
		return FALSE;

	g_object_get(user, "active", &active, NULL);

	return active;
}

/* Whether @user_id holds an active membership of @organization_id. */
static gboolean
venture_assignee_has_membership(
	VentureDatabase	*database,
	gint64		 user_id,
	gint64		 organization_id
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureEntity) membership = NULL;

	query = venture_query_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP);

	if (!venture_query_add_filter_int(query, "user-id", VENTURE_FILTER_OP_EQ,
	                                  user_id, NULL) ||
	    !venture_query_add_filter_int(query, "organization-id",
	                                  VENTURE_FILTER_OP_EQ, organization_id,
	                                  NULL) ||
	    !venture_query_add_filter_string(query, "active", VENTURE_FILTER_OP_EQ,
	                                     "true", NULL))
		return FALSE;

	membership = venture_database_find_one(database, query, NULL);

	return (NULL != membership) && !venture_entity_is_deleted(membership);
}

/* The user named @username, deleted or not; %NULL when there is none. */
static VentureEntity *
venture_assignee_find_user(
	VentureDatabase	*database,
	const gchar	*username
){
	g_autoptr(VentureQuery) query = NULL;

	query = venture_query_new(VENTURE_TYPE_USER);

	if (!venture_query_add_filter_string(query, "username",
	                                     VENTURE_FILTER_OP_EQ, username, NULL))
		return NULL;

	return venture_database_find_one(database, query, NULL);
}

gboolean
venture_assignee_is_member(
	VentureDatabase	*database,
	gint64		 organization_id,
	const gchar	*username
){
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(VentureEntity) user = NULL;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), FALSE);

	if (venture_string_is_empty(username))
		return FALSE;

	/* Trusted: whether somebody belongs to an organization is not hidden
	 * by who is saving, and a saver who cannot read the users table must
	 * still be able to name a colleague. */
	internal = venture_access_policy_enter(
		venture_database_get_access_policy(database), NULL);

	user = venture_assignee_find_user(database, username);

	if (!venture_assignee_user_active(user))
		return FALSE;

	if (!venture_assignee_memberships_on())
		return TRUE;

	if (venture_assignee_reads_everything(database, user))
		return TRUE;

	return venture_assignee_has_membership(database,
		venture_entity_get_id(user), organization_id);
}

/* "Acme", or "organization #4" when it has no name or has gone. */
static gchar *
venture_assignee_organization_words(
	VentureDatabase	*database,
	gint64		 organization_id
){
	g_autoptr(VentureEntity) organization = NULL;
	g_autofree gchar *name = NULL;

	if (organization_id > 0)
		organization = venture_database_get(database, VENTURE_TYPE_ORGANIZATION,
		                                    organization_id, NULL);

	if (NULL != organization)
		name = venture_entity_get_display_name(organization);

	if (!venture_string_is_empty(name))
		return g_steal_pointer(&name);

	return g_strdup_printf("organization #%" G_GINT64_FORMAT, organization_id);
}

gboolean
venture_assignee_check(
	VentureDatabase		 *database,
	gint64			  organization_id,
	const gchar		 *field,
	const gchar		 *value,
	VentureColumnFlags	  flags,
	GError			**error
){
	g_auto(GStrv) names = NULL;
	gsize i;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), FALSE);
	g_return_val_if_fail(NULL != field, FALSE);

	if (venture_string_is_empty(value))
		return TRUE;

	/* One name, or a rota of them; the rota is read the way routing
	 * reads it, blanks between commas dropped. */
	if (0 != (flags & VENTURE_COLUMN_FLAG_MEMBER_USERNAMES))
		names = g_strsplit(value, ",", -1);
	else
	{
		names = g_new0(gchar *, 2);
		names[0] = g_strdup(value);
	}

	for (i = 0; NULL != names[i]; i++)
	{
		g_autofree gchar *name = NULL;
		g_autofree gchar *where = NULL;

		name = g_strstrip(g_strdup(names[i]));

		if (venture_string_is_empty(name))
			continue;

		if ((0 != (flags & VENTURE_COLUMN_FLAG_MEMBER_PLACEHOLDER)) &&
		    (0 == g_strcmp0(name, VENTURE_ASSIGNEE_PLACEHOLDER_ME)))
			continue;

		if (venture_assignee_is_member(database, organization_id, name))
			continue;

		/* The field's wire name and the value, which is what a client
		 * needs to say what went wrong; the organization's name is for
		 * the person reading it. */
		{
			g_autoptr(VentureAccessScope) internal = NULL;

			internal = venture_access_policy_enter(
				venture_database_get_access_policy(database), NULL);
			where = venture_assignee_organization_words(database,
			                                            organization_id);
		}

		venture_set_error_validation(error, field,
			"\"%s\" is not an active member of %s; choose a member%s",
			name, where,
			(0 != (flags & VENTURE_COLUMN_FLAG_MEMBER_USERNAMES))
				? "" : ", or leave it unassigned");
		return FALSE;
	}

	return TRUE;
}

/* ==========================================================================
 * The list
 * ========================================================================== */

/* Whether @user matches @needle (already lowercased) anywhere in its
 * username or display name. */
static gboolean
venture_assignee_matches(
	VentureEntity	*user,
	const gchar	*needle
){
	g_autofree gchar *username = NULL;
	g_autofree gchar *display = NULL;
	g_autofree gchar *folded_username = NULL;
	g_autofree gchar *folded_display = NULL;

	if (venture_string_is_empty(needle))
		return TRUE;

	g_object_get(user, "username", &username, "display-name", &display, NULL);

	folded_username = g_utf8_casefold((NULL != username) ? username : "", -1);
	folded_display = g_utf8_casefold((NULL != display) ? display : "", -1);

	return (NULL != strstr(folded_username, needle)) ||
	       (NULL != strstr(folded_display, needle));
}

static gint
venture_assignee_compare_users(
	gconstpointer	a,
	gconstpointer	b
){
	g_autofree gchar *left = NULL;
	g_autofree gchar *right = NULL;

	g_object_get(*(VentureEntity *const *)a, "username", &left, NULL);
	g_object_get(*(VentureEntity *const *)b, "username", &right, NULL);

	return g_strcmp0(left, right);
}

/* Adds @user to @out once, if it is an active user. */
static void
venture_assignee_collect(
	GPtrArray	*out,
	GHashTable	*seen,
	VentureEntity	*user
){
	gint64 id;

	if (!venture_assignee_user_active(user))
		return;

	id = venture_entity_get_id(user);

	if (g_hash_table_contains(seen, &id))
		return;

	g_hash_table_add(seen, g_memdup2(&id, sizeof(id)));
	g_ptr_array_add(out, g_object_ref(user));
}

/* Every active user of the install, with no regard to organization. */
static gboolean
venture_assignee_collect_all(
	VentureDatabase	 *database,
	GPtrArray	 *out,
	GHashTable	 *seen,
	gboolean	  only_everything_readers,
	GError		**error
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) users = NULL;
	guint i;

	query = venture_query_new(VENTURE_TYPE_USER);
	venture_query_set_limit(query, 0);

	if (!venture_query_add_filter_string(query, "active", VENTURE_FILTER_OP_EQ,
	                                     "true", error))
		return FALSE;

	users = venture_database_find(database, query, error);

	if (NULL == users)
		return FALSE;

	for (i = 0; i < users->len; i++)
	{
		VentureEntity *user;

		user = g_ptr_array_index(users, i);

		if (only_everything_readers &&
		    !venture_assignee_reads_everything(database, user))
			continue;

		venture_assignee_collect(out, seen, user);
	}

	return TRUE;
}

GPtrArray *
venture_assignee_list_members(
	VentureDatabase	 *database,
	gint64		  organization_id,
	const gchar	 *search,
	guint		  offset,
	guint		  limit,
	guint		 *out_total,
	GError		**error
){
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(GPtrArray) members = NULL;
	g_autoptr(GHashTable) seen = NULL;
	g_autofree gchar *needle = NULL;
	GPtrArray *page;
	guint matched;
	guint i;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);

	if ((0 == limit) || (limit > VENTURE_ASSIGNEE_MEMBERS_MAX))
		limit = VENTURE_ASSIGNEE_MEMBERS_MAX;

	/* Trusted for the same reason as venture_assignee_is_member(): who
	 * may see this list is the caller's question, answered before it. */
	internal = venture_access_policy_enter(
		venture_database_get_access_policy(database), NULL);

	members = g_ptr_array_new_with_free_func(g_object_unref);
	seen = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);

	if (!venture_assignee_memberships_on())
	{
		if (!venture_assignee_collect_all(database, members, seen, FALSE, error))
			return NULL;
	}
	else
	{
		g_autoptr(VentureQuery) query = NULL;
		g_autoptr(GPtrArray) rows = NULL;

		query = venture_query_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP);
		venture_query_set_limit(query, 0);

		if (!venture_query_add_filter_int(query, "organization-id",
		                                  VENTURE_FILTER_OP_EQ, organization_id,
		                                  error) ||
		    !venture_query_add_filter_string(query, "active",
		                                     VENTURE_FILTER_OP_EQ, "true",
		                                     error))
			return NULL;

		rows = venture_database_find(database, query, error);

		if (NULL == rows)
			return NULL;

		for (i = 0; i < rows->len; i++)
		{
			g_autoptr(VentureEntity) user = NULL;
			gint64 user_id;

			user_id = 0;
			g_object_get(g_ptr_array_index(rows, i), "user-id", &user_id, NULL);

			if (user_id <= 0)
				continue;

			user = venture_database_get(database, VENTURE_TYPE_USER, user_id,
			                            NULL);
			venture_assignee_collect(members, seen, user);
		}

		/* The owner and administrators read every organization, and the
		 * validator accepts them everywhere; so does the list. */
		if (!venture_assignee_collect_all(database, members, seen, TRUE, error))
			return NULL;
	}

	g_ptr_array_sort(members, venture_assignee_compare_users);

	if (!venture_string_is_empty(search))
		needle = g_utf8_casefold(search, -1);

	page = g_ptr_array_new_with_free_func(g_object_unref);
	matched = 0;

	for (i = 0; i < members->len; i++)
	{
		VentureEntity *user;

		user = g_ptr_array_index(members, i);

		if (!venture_assignee_matches(user, needle))
			continue;

		if ((matched >= offset) && (page->len < limit))
			g_ptr_array_add(page, g_object_ref(user));

		matched++;
	}

	if (NULL != out_total)
		*out_total = matched;

	return page;
}

/* ==========================================================================
 * The validator
 * ========================================================================== */

/*
 * Every field flagged as naming members, judged when it is written. The
 * reference rule: a field keeping the value it had, in the organization
 * it was in, is left alone -- so a ticket whose assignee left the company
 * last year still takes a new title. A record moved to another
 * organization is judged again, because the name it keeps is now a claim
 * about different people.
 */
static gboolean
venture_assignee_validate(
	VentureDatabase	 *database,
	VentureEntity	 *entity,
	VentureEntity	 *previous,
	gpointer	  user_data,
	GError		**error
){
	VentureEntityClass *klass;
	g_autofree GParamSpec **properties = NULL;
	gint64 organization_id;
	gboolean moved;
	guint n_properties;
	guint i;

	(void)user_data;

	klass = VENTURE_ENTITY_GET_CLASS(entity);
	properties = venture_entity_class_list_persistent_properties(klass,
	                                                             &n_properties);
	organization_id = venture_entity_get_organization_id(entity);
	moved = (NULL != previous) &&
	        (venture_entity_get_organization_id(previous) != organization_id);

	for (i = 0; i < n_properties; i++)
	{
		g_autofree gchar *value = NULL;
		g_autofree gchar *wire = NULL;
		VentureColumnFlags flags;
		const gchar *name;

		name = properties[i]->name;
		flags = venture_entity_class_get_column_flags(klass, name);

		if (0 == (flags & (VENTURE_COLUMN_FLAG_MEMBER_USERNAME |
		                   VENTURE_COLUMN_FLAG_MEMBER_USERNAMES)))
			continue;

		if (G_TYPE_STRING != properties[i]->value_type)
			continue;

		g_object_get(entity, name, &value, NULL);

		if (venture_string_is_empty(value))
			continue;

		if ((NULL != previous) && !moved)
		{
			g_autofree gchar *was = NULL;

			g_object_get(previous, name, &was, NULL);

			if (0 == g_strcmp0(was, value))
				continue;
		}

		/* The name a client wrote it under: assignee, not "Assignee". */
		wire = g_strdup(name);
		g_strdelimit(wire, "-", '_');

		if (!venture_assignee_check(database, organization_id, wire, value,
		                            flags, error))
			return FALSE;
	}

	return TRUE;
}

void
venture_assignee_install(VentureContext *context)
{
	VentureDatabase *database;

	g_return_if_fail(VENTURE_IS_CONTEXT(context));

	database = venture_context_get_database(context);

	/* The tests build several contexts over one database; validators are
	 * per database, so the second one must add nothing. */
	if (NULL != g_object_get_data(G_OBJECT(database), VENTURE_ASSIGNEE_STATE_KEY))
		return;

	g_object_set_data(G_OBJECT(database), VENTURE_ASSIGNEE_STATE_KEY,
	                  GINT_TO_POINTER(1));

	/* On the base type: a plugin's record type that flags a field is held
	 * to the same rule the day it registers, with nothing listed here. */
	venture_database_add_save_validator(database, VENTURE_TYPE_ENTITY,
	                                    venture_assignee_validate, NULL, NULL);
}
