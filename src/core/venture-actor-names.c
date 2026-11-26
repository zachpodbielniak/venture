/*
 * venture-actor-names.c - Who an "API token #N" belongs to, for a reader
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Resolved when a page is drawn, not when the change is written. The
 * alternative -- an actor_user_id column on audit_entry stamped at audit
 * time -- would survive a deleted token, but it needs a migration on both
 * backends, a backfill that could only guess for rows whose token is
 * already gone, and every one of the seven hand-filled VentureActor sites
 * (AGENTS.md) to carry a new member. A row whose token or owner has been
 * deleted reads "API token #N", exactly as every row did before this
 * existed, which is an honest answer rather than a lost one.
 *
 * Who may learn the owner is decided here, once, for every surface. The
 * rule is the one the access records already draw: a member of an
 * organization reads that organization's memberships, and so the people
 * in it. A reader is told the owner when the reader is a global owner or
 * administrator (who reads the users page anyway), the owner themself, or
 * an active member of some organization the owner is also an active
 * member of. Anybody else -- the customer of a hosted install looking at
 * a change the operator's token made, say -- sees "API token #N".
 */

#include "venture.h"

#include <string.h>

struct _VentureActorNames
{
	VentureDatabase		*database;

	/* The reader, copied when the cache was made: the lookups below run
	 * in a trusted scope of their own, which hides the reader from the
	 * policy while they do. */
	VentureAuthPrincipal	*reader;

	/* Token id -> owner's username, or NULL when the reader is not to
	 * be told (or there is nobody to tell them about). Present at all
	 * once the token has been looked at, so it is looked at once. */
	GHashTable		*by_token;

	/* User id -> the set of organizations they are an active member of,
	 * so a reader's own memberships and an owner named by several tokens
	 * are each read once. */
	GHashTable		*organizations;

	/* User id -> the version of their picture, or NULL for none and for
	 * a person this reader may not see. Present once looked at. */
	GHashTable		*avatars;

	/* The string a page asked about -> that account's id, or 0 when
	 * there is nobody to draw (unknown, or not this reader's to see).
	 * The value is a g_new'd gint64: a pointer is not wide enough for
	 * an id on every architecture this builds for, and 0 has to be
	 * stored, so a missing value cannot mean "not cached". Made on the
	 * first ask and freed with the cache. */
	GHashTable		*by_username;
};

VentureActorNames *
venture_actor_names_new(VentureDatabase *database)
{
	VentureActorNames *self;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);

	self = g_new0(VentureActorNames, 1);
	self->database = g_object_ref(database);
	self->reader = venture_auth_principal_copy(venture_access_policy_get_actor(
		venture_database_get_access_policy(database)));
	self->by_token = g_hash_table_new_full(g_int64_hash, g_int64_equal,
	                                       g_free, g_free);
	self->organizations = g_hash_table_new_full(g_int64_hash, g_int64_equal,
	                                            g_free,
	                                            (GDestroyNotify)g_hash_table_unref);
	self->avatars = g_hash_table_new_full(g_int64_hash, g_int64_equal,
	                                      g_free, g_free);

	return self;
}

void
venture_actor_names_free(VentureActorNames *self)
{
	if (NULL == self)
		return;

	g_clear_pointer(&self->by_token, g_hash_table_unref);
	g_clear_pointer(&self->organizations, g_hash_table_unref);
	g_clear_pointer(&self->avatars, g_hash_table_unref);
	g_clear_pointer(&self->by_username, g_hash_table_unref);
	g_clear_pointer(&self->reader, venture_auth_principal_free);
	g_clear_object(&self->database);
	g_free(self);
}

gint64
venture_actor_names_parse_token(const gchar *actor)
{
	gint64 id;

	if ((NULL == actor) ||
	    !g_str_has_prefix(actor, VENTURE_ACTOR_NAMES_TOKEN_PREFIX))
		return 0;

	/* The whole remainder must be the number: "API token #4 (old)" is
	 * free text somebody typed, not a token actor. */
	id = 0;

	if (!g_ascii_string_to_signed(actor + strlen(VENTURE_ACTOR_NAMES_TOKEN_PREFIX),
	                              10, 1, G_MAXINT64, &id, NULL))
		return 0;

	return id;
}

/*
 * The organizations @user_id is an active member of, read once per user
 * per cache. Called inside the trusted scope. A read that fails is an
 * empty set, so a failure hides an owner rather than showing one.
 */
static GHashTable *
venture_actor_names_organizations_of(
	VentureActorNames	*self,
	gint64			 user_id
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	GHashTable *set;
	gint64 *key;
	guint i;

	set = g_hash_table_lookup(self->organizations, &user_id);

	if (NULL != set)
		return set;

	set = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	key = g_new(gint64, 1);
	*key = user_id;
	g_hash_table_insert(self->organizations, key, set);

	query = venture_query_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP);
	venture_query_set_limit(query, 0);

	if (!venture_query_add_filter_int(query, "user-id", VENTURE_FILTER_OP_EQ,
	                                  user_id, NULL) ||
	    !venture_query_add_filter_string(query, "active",
	                                     VENTURE_FILTER_OP_EQ, "true", NULL))
		return set;

	rows = venture_database_find(self->database, query, NULL);

	for (i = 0; (NULL != rows) && (i < rows->len); i++)
	{
		gint64 *organization;

		organization = g_new(gint64, 1);
		*organization = venture_entity_get_organization_id(
			g_ptr_array_index(rows, i));
		g_hash_table_add(set, organization);
	}

	return set;
}

/*
 * Whether the reader may be told that @owner_id is behind a token. Called
 * inside the trusted scope; the reader was copied before it was entered.
 */
static gboolean
venture_actor_names_reader_may_know(
	VentureActorNames	*self,
	gint64			 owner_id
){
	const VentureAuthPrincipal *reader;
	GHashTable *mine;
	GHashTable *theirs;
	GHashTableIter iter;
	gpointer organization;

	reader = self->reader;

	/* Trusted internal work has no reader to tell, and an unauthenticated
	 * request is told nothing: neither is a person looking at a page. */
	if ((NULL == reader) || !reader->authenticated)
		return FALSE;

	/* The users page is theirs already. */
	if (venture_access_policy_is_administrator(reader))
		return TRUE;

	if ((reader->user_id > 0) && (reader->user_id == owner_id))
		return TRUE;

	/* With organization access off there is one boundary, the install,
	 * and every signed-in person is inside it. */
	if (!venture_entity_registry_is_type_enabled(
		venture_entity_registry_get_default(), "organization_membership"))
		return TRUE;

	if (reader->user_id <= 0)
		return FALSE;

	mine = venture_actor_names_organizations_of(self, reader->user_id);
	theirs = venture_actor_names_organizations_of(self, owner_id);

	g_hash_table_iter_init(&iter, mine);

	while (g_hash_table_iter_next(&iter, &organization, NULL))
	{
		if (g_hash_table_contains(theirs, organization))
			return TRUE;
	}

	return FALSE;
}

/*
 * Token -> its owner's username, or NULL. The token and the user are
 * owner-only records, so they are read as trusted internal work; what the
 * reader is then told is venture_actor_names_reader_may_know()'s call,
 * not the record policy's.
 */
static gchar *
venture_actor_names_resolve(
	VentureActorNames	*self,
	gint64			 token_id
){
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(VentureEntity) token = NULL;
	g_autoptr(VentureEntity) user = NULL;
	g_autofree gchar *username = NULL;
	gint64 user_id;

	/* Asked first so a page for nobody costs no reads at all. */
	if ((NULL == self->reader) || !self->reader->authenticated)
		return NULL;

	internal = venture_access_policy_enter(
		venture_database_get_access_policy(self->database), NULL);

	token = venture_database_get(self->database, VENTURE_TYPE_API_TOKEN,
	                             token_id, NULL);

	/* A deleted token reads as it always did. A revoked or expired one
	 * still says whose it was: that is history, not authority. */
	if ((NULL == token) || venture_entity_is_deleted(token))
		return NULL;

	user_id = 0;
	g_object_get(token, "user-id", &user_id, NULL);

	if (user_id <= 0)
		return NULL;

	user = venture_database_get(self->database, VENTURE_TYPE_USER, user_id,
	                            NULL);

	if ((NULL == user) || venture_entity_is_deleted(user))
		return NULL;

	g_object_get(user, "username", &username, NULL);

	if (venture_string_is_empty(username))
		return NULL;

	if (!venture_actor_names_reader_may_know(self, user_id))
		return NULL;

	return g_steal_pointer(&username);
}

const gchar *
venture_actor_names_lookup(
	VentureActorNames	*self,
	const gchar		*actor
){
	gpointer known;
	gint64 token_id;
	gint64 *key;
	gchar *username;

	g_return_val_if_fail(NULL != self, NULL);

	token_id = venture_actor_names_parse_token(actor);

	if (0 == token_id)
		return NULL;

	if (g_hash_table_lookup_extended(self->by_token, &token_id, NULL, &known))
		return known;

	username = venture_actor_names_resolve(self, token_id);
	key = g_new(gint64, 1);
	*key = token_id;
	g_hash_table_insert(self->by_token, key, username);

	return username;
}

gchar *
venture_actor_names_format(
	const gchar	*actor,
	const gchar	*username
){
	if (NULL == actor)
		return NULL;

	if (venture_string_is_empty(username))
		return g_strdup(actor);

	/* The stored actor stays in the text, so a person can still match
	 * the line against whoami, a webhook body or the tokens page. */
	return g_strdup_printf("%s (%s)", username, actor);
}

gchar *
venture_actor_names_display(
	VentureActorNames	*self,
	const gchar		*actor
){
	g_return_val_if_fail(NULL != self, NULL);

	return venture_actor_names_format(actor,
		venture_actor_names_lookup(self, actor));
}

gchar *
venture_actor_names_display_lead(
	VentureActorNames	*self,
	const gchar		*actor,
	const gchar		*text
){
	g_autofree gchar *shown = NULL;
	const gchar *username;
	gsize length;

	g_return_val_if_fail(NULL != self, NULL);

	if (NULL == text)
		return NULL;

	if (venture_string_is_empty(actor) || !g_str_has_prefix(text, actor))
		return g_strdup(text);

	/* "API token #1" heads "API token #12 updated ..." too; only a whole
	 * actor counts. */
	length = strlen(actor);

	if (g_ascii_isdigit(text[length]))
		return g_strdup(text);

	username = venture_actor_names_lookup(self, actor);

	if (NULL == username)
		return g_strdup(text);

	shown = venture_actor_names_format(actor, username);

	return g_strconcat(shown, text + length, NULL);
}

gboolean
venture_actor_names_may_see_user(
	VentureActorNames	*self,
	gint64			 user_id
){
	g_autoptr(VentureAccessScope) internal = NULL;

	g_return_val_if_fail(NULL != self, FALSE);

	/* Asked first so a page for nobody costs no reads at all. */
	if ((user_id <= 0) || (NULL == self->reader) || !self->reader->authenticated)
		return FALSE;

	/* The memberships are owner-only records: read them as trusted work.
	 * What the reader is told is still reader_may_know()'s call. */
	internal = venture_access_policy_enter(
		venture_database_get_access_policy(self->database), NULL);

	return venture_actor_names_reader_may_know(self, user_id);
}

/*
 * The stored version of @user_id's picture, or NULL. Called inside the
 * trusted scope: a user row is owner-only to the record policy.
 */
static gchar *
venture_actor_names_read_avatar(
	VentureActorNames	*self,
	gint64			 user_id
){
	g_autoptr(VentureEntity) user = NULL;
	g_autofree gchar *version = NULL;

	user = venture_database_get(self->database, VENTURE_TYPE_USER, user_id,
	                            NULL);

	if ((NULL == user) || venture_entity_is_deleted(user))
		return NULL;

	g_object_get(user, "avatar-version", &version, NULL);

	/* Only a version the store could have written: anything else in the
	 * column would become part of an address on somebody's page. The
	 * validator keeps other writers out; this is the second fence. */
	if (!venture_avatar_version_is_valid(version))
		return NULL;

	return g_steal_pointer(&version);
}

const gchar *
venture_actor_names_avatar_version(
	VentureActorNames	*self,
	gint64			 user_id
){
	g_autoptr(VentureAccessScope) internal = NULL;
	gpointer known;
	gchar *version;
	gint64 *key;

	g_return_val_if_fail(NULL != self, NULL);

	if (user_id <= 0)
		return NULL;

	if (g_hash_table_lookup_extended(self->avatars, &user_id, NULL, &known))
		return known;

	version = NULL;

	/* Who may see the person comes first: a reader who may not is not
	 * told whether there is a picture to see either. */
	if (venture_actor_names_may_see_user(self, user_id))
	{
		internal = venture_access_policy_enter(
			venture_database_get_access_policy(self->database), NULL);
		version = venture_actor_names_read_avatar(self, user_id);
	}

	key = g_new(gint64, 1);
	*key = user_id;
	g_hash_table_insert(self->avatars, key, version);

	return version;
}

/*
 * The token an actor string names. Exactly "API token #N", or the
 * reading venture_actor_names_format() gives it: "erin (API token #N)",
 * with the parenthesis the whole tail. A username that only mentions a
 * token in the middle is still a username.
 */
static gint64
venture_actor_names_token_from_text(const gchar *text)
{
	const gchar *open;
	gchar *inside;
	gsize length;
	gint64 id;

	id = venture_actor_names_parse_token(text);

	if (0 != id)
		return id;

	if (NULL == text)
		return 0;

	length = strlen(text);

	if ((length < 2) || (')' != text[length - 1]))
		return 0;

	open = strrchr(text, '(');

	if ((NULL == open) || (open == text) || (' ' != *(open - 1)))
		return 0;

	inside = g_strndup(open + 1, length - (gsize)(open + 1 - text) - 1);
	id = venture_actor_names_parse_token(inside);
	g_free(inside);

	return id;
}

/*
 * The account that owns @token_id, before the reader is considered.
 * Called so a token's picture is that person's; the caller then asks
 * venture_actor_names_may_see_user().
 */
static gint64
venture_actor_names_token_owner(
	VentureActorNames	*self,
	gint64			 token_id
){
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(VentureEntity) token = NULL;
	gint64 user_id;

	if ((NULL == self->reader) || !self->reader->authenticated)
		return 0;

	internal = venture_access_policy_enter(
		venture_database_get_access_policy(self->database), NULL);
	token = venture_database_get(self->database, VENTURE_TYPE_API_TOKEN,
	                             token_id, NULL);

	if ((NULL == token) || venture_entity_is_deleted(token))
		return 0;

	user_id = 0;
	g_object_get(token, "user-id", &user_id, NULL);

	return (user_id > 0) ? user_id : 0;
}

/*
 * The account whose username is exactly @username. A user row is
 * owner-only, so it is read as trusted work; whether the reader is told
 * is venture_actor_names_may_see_user()'s call. A deleted account is
 * nobody. An inactive one still is that person: a ticket they hold is
 * still theirs.
 */
static gint64
venture_actor_names_query_username(
	VentureActorNames	*self,
	const gchar		*username
){
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureEntity) user = NULL;

	if ((NULL == self->reader) || !self->reader->authenticated)
		return 0;

	internal = venture_access_policy_enter(
		venture_database_get_access_policy(self->database), NULL);
	query = venture_query_new(VENTURE_TYPE_USER);

	if (!venture_query_add_filter_string(query, "username",
	                                     VENTURE_FILTER_OP_EQ, username,
	                                     NULL))
		return 0;

	venture_query_set_limit(query, 1);
	user = venture_database_find_one(self->database, query, NULL);

	if ((NULL == user) || venture_entity_is_deleted(user))
		return 0;

	return venture_entity_get_id(user);
}

gint64
venture_actor_names_user_id_for(
	VentureActorNames	*self,
	const gchar		*username
){
	gpointer known;
	gint64 *stored;
	gint64 token_id;
	gint64 user_id;
	gchar *key;

	g_return_val_if_fail(NULL != self, 0);

	if (venture_string_is_empty(username))
		return 0;

	if ((NULL != self->by_username) &&
	    g_hash_table_lookup_extended(self->by_username, username, NULL,
	                                 &known))
		return *(gint64 *)known;

	token_id = venture_actor_names_token_from_text(username);

	if (0 != token_id)
		user_id = venture_actor_names_token_owner(self, token_id);
	else
		user_id = venture_actor_names_query_username(self, username);

	/* 0 either way: an account the reader may not see is drawn by its
	 * initials, the same as an account that has no picture. */
	if ((user_id > 0) && !venture_actor_names_may_see_user(self, user_id))
		user_id = 0;

	if (NULL == self->by_username)
		self->by_username = g_hash_table_new_full(g_str_hash, g_str_equal,
		                                          g_free, g_free);

	key = g_strdup(username);
	stored = g_new(gint64, 1);
	*stored = user_id;
	g_hash_table_insert(self->by_username, key, stored);

	return user_id;
}

gchar *
venture_avatar_html_for_username(
	VentureActorNames	*viewer,
	const gchar		*username,
	const gchar		*display_name,
	guint			 size
){
	const gchar *name;
	gint64 user_id;

	g_return_val_if_fail(NULL != viewer, NULL);

	user_id = venture_actor_names_user_id_for(viewer, username);
	name = !venture_string_is_empty(display_name) ? display_name : username;

	return venture_avatar_html(viewer, user_id, name, size);
}
