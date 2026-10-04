/*
 * venture-comment.c - A discussion on any record
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "venture.h"

#include <string.h>

#define VENTURE_COMMENT_STATE_KEY "venture-comment-installed"

/* --- Small readers --------------------------------------------------------- */

static VentureAccessPolicy *
comment_policy(VentureContext *context)
{
	return venture_database_get_access_policy(venture_context_get_database(context));
}

/*
 * The record a comment names, through the mask: a record of a type whose
 * module is off is not there, for a comment as for anything else.
 */
static VentureEntity *
comment_load_subject(
	VentureDatabase	 *database,
	const gchar	 *subject_type,
	gint64		  subject_id,
	GError		**error
){
	GType type;

	type = venture_entity_registry_lookup(venture_entity_registry_get_default(),
	                                      (NULL != subject_type) ? subject_type : "");

	if ((G_TYPE_INVALID == type) || (subject_id <= 0))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		                    "No such record");
		return NULL;
	}

	{
		GError *local = NULL;
		VentureEntity *subject;

		/* A missing row is NULL with no error; a refusal must say why. */
		subject = venture_database_get(database, type, subject_id, &local);

		if ((NULL == subject) && (NULL == local))
			g_set_error_literal(&local, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
			                    "No such record");

		if (NULL != local)
			g_propagate_error(error, local);

		return subject;
	}
}

/* A comment under the current scope; missing or unreadable is not found. */
static VentureEntity *
comment_load(
	VentureContext	 *context,
	gint64		  comment_id,
	GError		**error
){
	GError *local = NULL;
	VentureEntity *comment;

	comment = venture_database_get(venture_context_get_database(context),
	                               VENTURE_TYPE_COMMENT, comment_id, &local);

	if ((NULL == comment) && (NULL == local))
		g_set_error(&local, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		            "No comment %" G_GINT64_FORMAT, comment_id);

	if (NULL != local)
		g_propagate_error(error, local);

	return comment;
}

/* An account, read whatever the current scope: who wrote a comment and
 * who may be mentioned are facts about the install, not the reader. */
static VentureEntity *
comment_load_user(
	VentureDatabase	*database,
	gint64		 user_id
){
	g_autoptr(VentureAccessScope) internal = NULL;

	if (user_id <= 0)
		return NULL;

	internal = venture_access_policy_enter(venture_database_get_access_policy(database), NULL);

	return venture_database_get(database, VENTURE_TYPE_USER, user_id, NULL);
}

static VentureEntity *
comment_load_user_by_name(
	VentureDatabase	*database,
	const gchar	*username
){
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(VentureQuery) query = NULL;

	if (venture_string_is_empty(username))
		return NULL;

	internal = venture_access_policy_enter(venture_database_get_access_policy(database), NULL);
	query = venture_query_new(VENTURE_TYPE_USER);

	if (!venture_query_add_filter_string(query, "username",
	                                     VENTURE_FILTER_OP_EQ, username, NULL))
		return NULL;

	venture_query_set_limit(query, 1);

	return venture_database_find_one(database, query, NULL);
}

/* "Ruth Ellery" to "RE", "zach" to "Z": the avatar's two letters. */
static gchar *
comment_initials(const gchar *name)
{
	g_autoptr(GString) out = NULL;
	const gchar *p;
	gboolean word_start = TRUE;

	out = g_string_new(NULL);

	for (p = name; (NULL != p) && ('\0' != *p) && (g_utf8_strlen(out->str, -1) < 2);
	     p = g_utf8_next_char(p))
	{
		gunichar c = g_utf8_get_char(p);

		if (!g_unichar_isalnum(c))
		{
			word_start = TRUE;
			continue;
		}

		if (word_start)
			g_string_append_unichar(out, g_unichar_toupper(c));

		word_start = FALSE;
	}

	if (0 == out->len)
		g_string_append_c(out, '?');

	return g_string_free(g_steal_pointer(&out), FALSE);
}

/* The names a comment's "mentions" field holds, as a vector. */
static GStrv
comment_stored_mentions(VentureEntity *comment)
{
	g_autofree gchar *mentions = NULL;

	g_object_get(comment, "mentions", &mentions, NULL);

	if (venture_string_is_empty(mentions))
		return g_new0(gchar *, 1);

	return g_strsplit(mentions, ",", -1);
}

/* --- Who may read ---------------------------------------------------------- */

gboolean
venture_comment_user_can_read(
	VentureContext	*context,
	gint64		 user_id,
	VentureEntity	*subject
){
	g_autoptr(VentureEntity) user = NULL;
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autofree gchar *username = NULL;
	VentureAuthPrincipal principal;
	VentureUserRole role = VENTURE_USER_ROLE_VIEWER;
	gboolean active = FALSE;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), FALSE);
	g_return_val_if_fail(VENTURE_IS_ENTITY(subject), FALSE);

	user = comment_load_user(venture_context_get_database(context), user_id);

	if ((NULL == user) || venture_entity_is_deleted(user))
		return FALSE;

	g_object_get(user, "username", &username, "role", &role, "active", &active,
	             NULL);

	if (!active)
		return FALSE;

	/*
	 * The account as its own session would present it. A trusted scope
	 * is entered first, so the question is asked of the policy alone and
	 * not narrowed by whoever's request is running -- the writer's
	 * organization boundary is not the reader's.
	 */
	principal.user_id = user_id;
	principal.token_id = 0;
	principal.role = role;
	principal.name = username;
	principal.authenticated = TRUE;

	internal = venture_access_policy_enter(comment_policy(context), NULL);

	return venture_access_policy_can(comment_policy(context), &principal,
	                                 "read", subject, NULL);
}

gboolean
venture_comment_type_accepts(
	GType		  type,
	GError		**error
){
	g_autofree gchar *label = NULL;

	if ((G_TYPE_INVALID == type) || !g_type_is_a(type, VENTURE_TYPE_ENTITY))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		                    "No such record type");
		return FALSE;
	}

	/* Workspace administration is driven by its own declared actions; a
	 * thread on it would be a second, unaudited channel beside them. */
	if (venture_entity_type_is_commentable(type) &&
	    (VENTURE_DATA_CLASS_TENANT_ADMIN != venture_data_class_for_type(type)))
		return TRUE;

	label = venture_entity_type_dup_label(type, TRUE);
	venture_set_error_validation(error, "subject_type",
	                             "%s do not take comments", label);

	return FALSE;
}

gboolean
venture_comment_subject_accepts(
	VentureDatabase	*database,
	VentureEntity	*subject
){
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), FALSE);
	g_return_val_if_fail(VENTURE_IS_ENTITY(subject), FALSE);

	if (!venture_comment_type_accepts(G_OBJECT_TYPE(subject), NULL) ||
	    venture_entity_is_deleted(subject))
		return FALSE;

	/*
	 * A private record -- somebody's mail account, an optional personal
	 * import -- is kept out of every organization-wide excerpt. A comment
	 * on one would reach the inbox, a webhook and the activity of
	 * anybody it named, so a private record takes none.
	 */
	return !venture_access_policy_record_is_personal(
		venture_database_get_access_policy(database), subject);
}

/* --- Mentions -------------------------------------------------------------- */

/* Collects every @name the renderer reads as a candidate -- which leaves
 * out code -- and declines all of them, so nothing is drawn. */
static gchar *
comment_collect_name(
	const gchar	*username,
	gpointer	 user_data
){
	GPtrArray *names = user_data;
	guint i;

	for (i = 0; i < names->len; i++)
	{
		if (0 == g_strcmp0(g_ptr_array_index(names, i), username))
			return NULL;
	}

	g_ptr_array_add(names, g_strdup(username));

	return NULL;
}

GStrv
venture_comment_resolve_mentions(
	VentureContext	*context,
	VentureEntity	*subject,
	const gchar	*body
){
	g_autoptr(GPtrArray) names = NULL;
	g_autoptr(GPtrArray) kept = NULL;
	g_autofree gchar *ignored = NULL;
	VentureMarkdownOptions options;
	guint i;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);
	g_return_val_if_fail(VENTURE_IS_ENTITY(subject), NULL);

	names = g_ptr_array_new_with_free_func(g_free);
	kept = g_ptr_array_new_with_free_func(g_free);

	/*
	 * The renderer decides what is a mention, so a name inside a code
	 * span is never a mention here either: a chip and a notification
	 * always come from the same reading of the text.
	 */
	options.mention = comment_collect_name;
	options.reference = NULL;
	options.user_data = names;
	ignored = venture_markdown_to_html(body, &options);

	/* A bound on how many people one comment can ping. */
	for (i = 0; (i < names->len) && (kept->len < 50); i++)
	{
		g_autoptr(VentureEntity) user = NULL;

		user = comment_load_user_by_name(venture_context_get_database(context),
		                                 g_ptr_array_index(names, i));

		if ((NULL == user) ||
		    !venture_comment_user_can_read(context, venture_entity_get_id(user),
		                                   subject))
			continue;

		g_ptr_array_add(kept, g_strdup(g_ptr_array_index(names, i)));
	}

	g_ptr_array_add(kept, NULL);

	return (GStrv)g_ptr_array_free(g_steal_pointer(&kept), FALSE);
}

/* --- Rendering ------------------------------------------------------------- */

typedef struct
{
	VentureContext		*context;
	const gchar *const	*mentions;
	GStrv			 resolved;
} CommentRender;

static gchar *
comment_render_mention(
	const gchar	*username,
	gpointer	 user_data
){
	CommentRender *render = user_data;
	const gchar *const *allowed;
	g_autoptr(VentureEntity) user = NULL;
	g_autofree gchar *display = NULL;

	allowed = (NULL != render->mentions) ? render->mentions
	                                     : (const gchar *const *)render->resolved;

	if ((NULL == allowed) || !g_strv_contains(allowed, username))
		return NULL;

	user = comment_load_user_by_name(venture_context_get_database(render->context),
	                                 username);

	if (NULL != user)
		g_object_get(user, "display-name", &display, NULL);

	return g_strdup(!venture_string_is_empty(display) ? display : username);
}

/*
 * A #type/id reference, under the reader's own scope: a record they may
 * not open stays the text it was, which says nothing about whether it
 * exists.
 */
static gchar *
comment_render_reference(
	const gchar	*type_name,
	gint64		 id,
	gpointer	 user_data
){
	CommentRender *render = user_data;
	g_autoptr(VentureEntity) record = NULL;
	GType type;

	type = venture_entity_registry_lookup(venture_entity_registry_get_default(),
	                                      type_name);

	if (G_TYPE_INVALID == type)
		return NULL;

	record = venture_database_get(venture_context_get_database(render->context),
	                              type, id, NULL);

	if ((NULL == record) || venture_entity_is_deleted(record))
		return NULL;

	return venture_entity_get_display_name(record);
}

gchar *
venture_comment_render(
	VentureContext		*context,
	VentureEntity		*subject,
	const gchar		*body,
	const gchar *const	*mentions
){
	VentureMarkdownOptions options;
	CommentRender render;
	gchar *html;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);
	g_return_val_if_fail(VENTURE_IS_ENTITY(subject), NULL);

	render.context = context;
	render.mentions = mentions;
	render.resolved = (NULL == mentions)
		? venture_comment_resolve_mentions(context, subject, body) : NULL;

	options.mention = comment_render_mention;
	options.reference = comment_render_reference;
	options.user_data = &render;

	html = venture_markdown_to_html(body, &options);
	g_strfreev(render.resolved);

	return html;
}

/* --- The validator --------------------------------------------------------- */

/*
 * Holds every writer to the rules. Runs inside the lock, under the scope
 * the write was made in, which is where the author comes from.
 */
static gboolean
comment_validate(
	VentureDatabase	 *database,
	VentureEntity	 *entity,
	VentureEntity	 *previous,
	gpointer	  user_data,
	GError		**error
){
	VentureAccessPolicy *policy;
	const VentureAuthPrincipal *actor;
	g_autoptr(VentureEntity) subject = NULL;
	g_autofree gchar *subject_type = NULL;
	g_autofree gchar *body = NULL;
	g_autofree gchar *previous_body = NULL;
	gint64 subject_id = 0;
	gint64 parent_id = 0;
	gboolean body_changed;

	(void)user_data;

	policy = venture_database_get_access_policy(database);
	g_object_get(entity, "subject-type", &subject_type, "subject-id", &subject_id,
	             "parent-id", &parent_id, "body", &body, NULL);

	/* What a comment is attached to, and who wrote it, are fixed when it
	 * is made. A comment moved to another record would carry its words
	 * into a conversation they were never part of. */
	if (NULL != previous)
	{
		g_autofree gchar *was_type = NULL;
		g_autofree gchar *was_author = NULL;
		g_autofree gchar *author = NULL;
		gint64 was_id = 0;
		gint64 was_parent = 0;
		gint64 was_user = 0;
		gint64 user = 0;

		g_object_get(previous, "subject-type", &was_type, "subject-id", &was_id,
		             "parent-id", &was_parent, "author", &was_author,
		             "author-user-id", &was_user, "body", &previous_body, NULL);
		g_object_get(entity, "author", &author, "author-user-id", &user, NULL);

		if ((0 != g_strcmp0(was_type, subject_type)) || (was_id != subject_id) ||
		    (was_parent != parent_id))
		{
			venture_set_error_validation(error, "subject_id",
				"A comment stays on the record and in the thread it was written in");
			return FALSE;
		}

		if ((0 != g_strcmp0(was_author, author)) || (was_user != user))
		{
			venture_set_error_validation(error, "author",
				"A comment keeps the author it was written by");
			return FALSE;
		}
	}

	/* The words: something, and not a document. */
	if (NULL != body)
		g_strstrip(body);

	if (venture_string_is_empty(body))
	{
		venture_set_error_validation(error, "body",
		                             "A comment needs something written in it");
		return FALSE;
	}

	if (strlen(body) > VENTURE_COMMENT_MAX_LENGTH)
	{
		venture_set_error_validation(error, "body",
			"A comment is at most %d characters; a longer text belongs in a "
			"document or a knowledge-base article", VENTURE_COMMENT_MAX_LENGTH);
		return FALSE;
	}

	/* The record it is on, read as the system: whether the writer may see
	 * it is the policy's question, asked before this ran. */
	{
		g_autoptr(VentureAccessScope) internal = NULL;
		GType type;

		type = venture_entity_registry_lookup(venture_entity_registry_get_default(),
		                                      (NULL != subject_type) ? subject_type : "");

		if (!venture_comment_type_accepts(type, error))
			return FALSE;

		internal = venture_access_policy_enter(policy, NULL);
		subject = venture_database_get(database, type, subject_id, NULL);
	}

	/* Written onto a record that is gone, a comment could never be read;
	 * an existing comment on a record deleted since stays editable. */
	if ((NULL == subject) || ((NULL == previous) && venture_entity_is_deleted(subject)))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		            "No %s with id %" G_GINT64_FORMAT,
		            subject_type, subject_id);
		return FALSE;
	}

	if ((NULL == previous) && !venture_comment_subject_accepts(database, subject))
	{
		venture_set_error_validation(error, "subject_id",
		                             "A private record takes no comments");
		return FALSE;
	}

	if (NULL == previous)
	{
		g_autofree gchar *label = NULL;
		gint64 organization;

		/* The comment lives where its record does, whatever the client
		 * said: a comment filed under another organization would be
		 * read by people who cannot read what it is about. */
		organization = VENTURE_IS_ORGANIZATION(subject)
			? venture_entity_get_id(subject)
			: venture_entity_get_organization_id(subject);
		venture_entity_set_organization_id(entity, organization);

		label = venture_entity_get_display_name(subject);
		g_object_set(entity, "subject-label", label, "edited-at", NULL, NULL);

		/* One level of replies: an answer to an answer joins the thread
		 * of the comment that started it. */
		if (0 != parent_id)
		{
			g_autoptr(VentureAccessScope) internal = NULL;
			g_autoptr(VentureEntity) parent = NULL;
			g_autofree gchar *parent_type = NULL;
			gint64 parent_subject = 0;
			gint64 grandparent = 0;

			internal = venture_access_policy_enter(policy, NULL);
			parent = venture_database_get(database, VENTURE_TYPE_COMMENT,
			                              parent_id, NULL);

			if (NULL != parent)
				g_object_get(parent, "subject-type", &parent_type,
				             "subject-id", &parent_subject,
				             "parent-id", &grandparent, NULL);

			if ((NULL == parent) || venture_entity_is_deleted(parent) ||
			    (0 != g_strcmp0(parent_type, subject_type)) ||
			    (parent_subject != subject_id))
			{
				venture_set_error_validation(error, "parent_id",
					"A reply answers a comment on the same record that "
					"has not been deleted");
				return FALSE;
			}

			if (0 != grandparent)
				g_object_set(entity, "parent-id", grandparent, NULL);
		}

		/*
		 * Who wrote it is whoever the write is made as. Whatever a
		 * client sent is overwritten rather than compared, so a forged
		 * author is not refused -- it is simply not believed. A token
		 * writes as "API token #N" and its account is its owner's.
		 * Trusted internal work with no principal keeps what the
		 * caller set, which is how a seed or an import names one.
		 */
		actor = venture_access_policy_get_actor(policy);

		if (NULL != actor)
			g_object_set(entity,
			             "author", !venture_string_is_empty(actor->name)
			                       	? actor->name : "local",
			             "author-user-id", actor->user_id, NULL);
		else
		{
			g_autofree gchar *author = NULL;

			g_object_get(entity, "author", &author, NULL);

			if (venture_string_is_empty(author))
				g_object_set(entity, "author", "system", NULL);
		}
	}

	body_changed = (NULL == previous) || (0 != g_strcmp0(previous_body, body));
	g_object_set(entity, "body", body, NULL);

	if (NULL != previous)
	{
		g_autoptr(GDateTime) was_edited = NULL;
		g_autoptr(GDateTime) now = NULL;

		/* Edited is the save's to stamp, when the words change and
		 * only then; a hand-set value is put back. */
		g_object_get(previous, "edited-at", &was_edited, NULL);
		now = body_changed ? venture_time_now() : NULL;
		g_object_set(entity, "edited-at", body_changed ? now : was_edited, NULL);
	}

	/* Who it names, read again whenever the words change, and only the
	 * names that may read the record. The context is the last one built
	 * over this database, as for every validator that needs one. */
	if (body_changed)
	{
		VentureContext *context;
		g_auto(GStrv) mentions = NULL;
		g_autofree gchar *joined = NULL;

		context = g_object_get_data(G_OBJECT(database), VENTURE_COMMENT_STATE_KEY);

		if (NULL != context)
			mentions = venture_comment_resolve_mentions(context, subject, body);

		joined = (NULL != mentions) ? g_strjoinv(",", mentions) : g_strdup("");
		g_object_set(entity, "mentions",
		             venture_string_is_empty(joined) ? NULL : joined, NULL);
	}
	else
	{
		g_autofree gchar *kept = NULL;

		g_object_get(previous, "mentions", &kept, NULL);
		g_object_set(entity, "mentions", kept, NULL);
	}

	return TRUE;
}

void
venture_comment_install(VentureContext *context)
{
	VentureDatabase *database;
	gboolean first;

	g_return_if_fail(VENTURE_IS_CONTEXT(context));

	database = venture_context_get_database(context);
	first = (NULL == g_object_get_data(G_OBJECT(database),
	                                   VENTURE_COMMENT_STATE_KEY));

	/*
	 * The tests build several contexts over one database. Validators are
	 * per database, so only the first adds one; the context the
	 * validator resolves mentions with is the last built, held weakly --
	 * the context owns the database, so a strong reference would be a
	 * cycle.
	 */
	g_object_set_data(G_OBJECT(database), VENTURE_COMMENT_STATE_KEY, context);

	if (first)
		venture_database_add_save_validator(database, VENTURE_TYPE_COMMENT,
		                                    comment_validate, NULL, NULL);
}

/* --- Writing --------------------------------------------------------------- */

VentureEntity *
venture_comment_create(
	VentureContext			 *context,
	const VentureAuthPrincipal	 *principal,
	const gchar			 *subject_type,
	gint64				  subject_id,
	gint64				  parent_id,
	const gchar			 *body,
	GError				**error
){
	g_autoptr(VentureAccessScope) scope = NULL;
	g_autoptr(VentureEntity) subject = NULL;
	g_autoptr(VentureComment) comment = NULL;
	VentureDatabase *database;
	VentureActor actor;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);
	g_return_val_if_fail(NULL != principal, NULL);

	database = venture_context_get_database(context);

	/* Everything below is asked as the writer: the record must be one
	 * they may read, or it is not found -- the same answer its page
	 * gives, which says nothing about whether it exists. */
	scope = venture_access_policy_enter(comment_policy(context), principal);
	subject = comment_load_subject(database, subject_type, subject_id, error);

	if (NULL == subject)
		return NULL;

	if (!venture_comment_type_accepts(G_OBJECT_TYPE(subject), error))
		return NULL;

	comment = venture_comment_new();
	g_object_set(comment,
	             "subject-type", venture_entity_get_entity_name(subject),
	             "subject-id", subject_id,
	             "parent-id", parent_id,
	             "body", body,
	             NULL);

	/* Placed before the save so the policy judges it in the record's
	 * organization; the validator places it again for every other
	 * writer. */
	venture_entity_set_organization_id(VENTURE_ENTITY(comment),
		VENTURE_IS_ORGANIZATION(subject) ? venture_entity_get_id(subject)
		                                 : venture_entity_get_organization_id(subject));

	venture_auth_to_actor((VentureAuthPrincipal *)principal, &actor);

	if (!venture_database_save(database, VENTURE_ENTITY(comment), &actor, error))
		return NULL;

	/* Whoever comments follows the record from now on. The inbox does
	 * this too, from the audit signal; doing it here as well means the
	 * Watch button on the page the writer returns to already says so. */
	if (principal->user_id > 0)
		venture_notify_watch(context, principal->user_id,
		                     venture_entity_get_entity_name(subject),
		                     subject_id, NULL);

	return VENTURE_ENTITY(g_steal_pointer(&comment));
}

VentureEntity *
venture_comment_get(
	VentureContext			 *context,
	const VentureAuthPrincipal	 *principal,
	gint64				  comment_id,
	GError				**error
){
	g_autoptr(VentureAccessScope) scope = NULL;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);

	if (NULL != principal)
		scope = venture_access_policy_enter(comment_policy(context), principal);

	return comment_load(context, comment_id, error);
}

VentureEntity *
venture_comment_edit(
	VentureContext			 *context,
	const VentureAuthPrincipal	 *principal,
	gint64				  comment_id,
	const gchar			 *body,
	GError				**error
){
	g_autoptr(VentureAccessScope) scope = NULL;
	g_autoptr(VentureEntity) comment = NULL;
	VentureActor actor;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);
	g_return_val_if_fail(NULL != principal, NULL);

	scope = venture_access_policy_enter(comment_policy(context), principal);
	comment = comment_load(context, comment_id, error);

	if (NULL == comment)
		return NULL;

	if (venture_entity_is_deleted(comment))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		                    "That comment was deleted");
		return NULL;
	}

	g_object_set(comment, "body", body, NULL);
	venture_auth_to_actor((VentureAuthPrincipal *)principal, &actor);

	/* The policy says whether this principal wrote it; the validator
	 * stamps the edit. */
	if (!venture_database_save(venture_context_get_database(context), comment,
	                           &actor, error))
		return NULL;

	return g_steal_pointer(&comment);
}

gboolean
venture_comment_delete(
	VentureContext			 *context,
	const VentureAuthPrincipal	 *principal,
	gint64				  comment_id,
	GError				**error
){
	g_autoptr(VentureAccessScope) scope = NULL;
	g_autoptr(VentureEntity) comment = NULL;
	VentureActor actor;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), FALSE);
	g_return_val_if_fail(NULL != principal, FALSE);

	scope = venture_access_policy_enter(comment_policy(context), principal);
	comment = comment_load(context, comment_id, error);

	if (NULL == comment)
		return FALSE;

	/* Deleting twice is not an error; the comment is gone either way. */
	if (venture_entity_is_deleted(comment))
		return TRUE;

	venture_auth_to_actor((VentureAuthPrincipal *)principal, &actor);

	return venture_database_delete(venture_context_get_database(context),
	                               comment, &actor, error);
}

/* --- Reading --------------------------------------------------------------- */

VentureEntity *
venture_comment_get_subject(
	VentureContext	 *context,
	VentureEntity	 *comment,
	GError		**error
){
	g_autofree gchar *subject_type = NULL;
	gint64 subject_id = 0;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);
	g_return_val_if_fail(VENTURE_IS_COMMENT(comment), NULL);

	g_object_get(comment, "subject-type", &subject_type,
	             "subject-id", &subject_id, NULL);

	return comment_load_subject(venture_context_get_database(context),
	                            subject_type, subject_id, error);
}

gchar *
venture_comment_permalink(gint64 comment_id)
{
	return g_strdup_printf("/comments/%" G_GINT64_FORMAT, comment_id);
}

gchar *
venture_comment_anchor_url(VentureEntity *comment)
{
	g_autofree gchar *subject_type = NULL;
	gint64 subject_id = 0;

	g_return_val_if_fail(VENTURE_IS_COMMENT(comment), NULL);

	g_object_get(comment, "subject-type", &subject_type,
	             "subject-id", &subject_id, NULL);

	return g_strdup_printf("/e/%s/%" G_GINT64_FORMAT "#comment-%" G_GINT64_FORMAT,
	                       subject_type, subject_id,
	                       venture_entity_get_id(comment));
}

gchar *
venture_comment_excerpt(
	VentureEntity	*comment,
	gsize		 max_chars
){
	g_autofree gchar *body = NULL;

	g_return_val_if_fail(VENTURE_IS_COMMENT(comment), NULL);

	g_object_get(comment, "body", &body, NULL);

	return venture_markdown_to_text(body, max_chars);
}

gchar *
venture_comment_author_name(
	VentureContext	*context,
	VentureEntity	*comment
){
	g_autoptr(VentureEntity) user = NULL;
	g_autofree gchar *author = NULL;
	g_autofree gchar *display = NULL;
	g_autofree gchar *username = NULL;
	gint64 user_id = 0;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);
	g_return_val_if_fail(VENTURE_IS_COMMENT(comment), NULL);

	g_object_get(comment, "author", &author, "author-user-id", &user_id, NULL);
	user = comment_load_user(venture_context_get_database(context), user_id);

	if (NULL != user)
		g_object_get(user, "display-name", &display, "username", &username, NULL);

	if (!venture_string_is_empty(display))
		return g_steal_pointer(&display);

	if (!venture_string_is_empty(username))
		return g_steal_pointer(&username);

	return g_strdup(!venture_string_is_empty(author) ? author : "Somebody");
}

static void
comment_add_time(
	JsonBuilder	*builder,
	const gchar	*member,
	GDateTime	*when
){
	json_builder_set_member_name(builder, member);

	if (NULL != when)
	{
		g_autofree gchar *text = venture_time_to_string(when);

		json_builder_add_string_value(builder, text);
	}
	else
	{
		json_builder_add_null_value(builder);
	}
}

static void
comment_add_string(
	JsonBuilder	*builder,
	const gchar	*member,
	const gchar	*value
){
	json_builder_set_member_name(builder, member);

	if (NULL != value)
		json_builder_add_string_value(builder, value);
	else
		json_builder_add_null_value(builder);
}

JsonNode *
venture_comment_to_json(
	VentureContext			*context,
	const VentureAuthPrincipal	*principal,
	VentureEntity			*comment
){
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(VentureEntity) subject = NULL;
	g_autofree gchar *subject_type = NULL;
	g_autofree gchar *subject_label = NULL;
	g_autofree gchar *body = NULL;
	g_autofree gchar *author = NULL;
	g_autofree gchar *name = NULL;
	g_autofree gchar *initials = NULL;
	g_autofree gchar *html = NULL;
	g_autofree gchar *url = NULL;
	g_autofree gchar *permalink = NULL;
	g_autoptr(GDateTime) edited_at = NULL;
	g_auto(GStrv) mentions = NULL;
	gint64 subject_id = 0;
	gint64 parent_id = 0;
	gint64 author_user_id = 0;
	gboolean deleted;
	gsize i;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);
	g_return_val_if_fail(VENTURE_IS_COMMENT(comment), NULL);

	g_object_get(comment, "subject-type", &subject_type, "subject-id", &subject_id,
	             "subject-label", &subject_label, "parent-id", &parent_id,
	             "body", &body, "author", &author,
	             "author-user-id", &author_user_id, "edited-at", &edited_at,
	             NULL);

	deleted = venture_entity_is_deleted(comment);
	name = venture_comment_author_name(context, comment);
	initials = comment_initials(name);
	mentions = comment_stored_mentions(comment);
	url = venture_comment_anchor_url(comment);
	permalink = venture_comment_permalink(venture_entity_get_id(comment));

	/* Rendered under the current scope, so a #type/id the reader may not
	 * open stays text for them. */
	if (!deleted)
	{
		subject = comment_load_subject(venture_context_get_database(context),
		                               subject_type, subject_id, NULL);

		if (NULL != subject)
			html = venture_comment_render(context, subject, body,
			                              (const gchar *const *)mentions);
	}

	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "id");
	json_builder_add_int_value(builder, venture_entity_get_id(comment));
	comment_add_string(builder, "uuid", venture_entity_get_uuid(comment));
	comment_add_string(builder, "subject_type", subject_type);
	json_builder_set_member_name(builder, "subject_id");
	json_builder_add_int_value(builder, subject_id);
	comment_add_string(builder, "subject_label", subject_label);
	json_builder_set_member_name(builder, "parent_id");

	if (0 != parent_id)
		json_builder_add_int_value(builder, parent_id);
	else
		json_builder_add_null_value(builder);

	/* A deleted comment's words are not handed out again; that it was
	 * there, and when, is what the thread needs. */
	comment_add_string(builder, "author", author);
	json_builder_set_member_name(builder, "author_user_id");
	json_builder_add_int_value(builder, author_user_id);
	comment_add_string(builder, "author_name", name);
	comment_add_string(builder, "author_initials", initials);

	/* The account behind it, for the reply box's @name and the page's
	 * badge: who the author is to this install. */
	{
		g_autoptr(VentureEntity) user = NULL;
		g_autofree gchar *username = NULL;
		VentureUserRole role = VENTURE_USER_ROLE_VIEWER;

		user = comment_load_user(venture_context_get_database(context),
		                         author_user_id);

		if (NULL != user)
			g_object_get(user, "username", &username, "role", &role, NULL);

		comment_add_string(builder, "author_username", username);
		comment_add_string(builder, "author_role", (NULL != user)
			? venture_enum_to_nick(VENTURE_TYPE_USER_ROLE, (gint)role) : NULL);
	}
	json_builder_set_member_name(builder, "via_token");
	json_builder_add_boolean_value(builder,
		(NULL != author) && g_str_has_prefix(author, "API token #"));
	comment_add_string(builder, "body", deleted ? NULL : body);
	comment_add_string(builder, "html", deleted ? NULL : html);
	json_builder_set_member_name(builder, "mentions");
	json_builder_begin_array(builder);

	for (i = 0; !deleted && (NULL != mentions[i]); i++)
		json_builder_add_string_value(builder, mentions[i]);

	json_builder_end_array(builder);
	comment_add_time(builder, "created_at", venture_entity_get_created_at(comment));
	comment_add_time(builder, "edited_at", edited_at);
	json_builder_set_member_name(builder, "deleted");
	json_builder_add_boolean_value(builder, deleted);
	comment_add_string(builder, "url", url);
	comment_add_string(builder, "permalink", permalink);

	/* What the reader may do, asked of the policy rather than guessed,
	 * so the buttons and the refusal can never disagree. */
	json_builder_set_member_name(builder, "can_edit");
	json_builder_add_boolean_value(builder, !deleted && (NULL != principal) &&
		venture_access_policy_can(comment_policy(context), principal, "write",
		                          comment, NULL));
	json_builder_set_member_name(builder, "can_delete");
	json_builder_add_boolean_value(builder, !deleted && (NULL != principal) &&
		venture_access_policy_can(comment_policy(context), principal, "delete",
		                          comment, NULL));
	json_builder_end_object(builder);

	return json_builder_get_root(builder);
}

/* Every comment on a record, deleted ones included, oldest first. */
static GPtrArray *
comment_list_all(
	VentureContext	 *context,
	const gchar	 *subject_type,
	gint64		  subject_id,
	gboolean	  include_deleted,
	GError		**error
){
	g_autoptr(VentureQuery) query = NULL;

	query = venture_query_new(VENTURE_TYPE_COMMENT);

	if (!venture_query_add_filter_string(query, "subject-type",
	                                     VENTURE_FILTER_OP_EQ, subject_type,
	                                     error) ||
	    !venture_query_add_filter_int(query, "subject-id", VENTURE_FILTER_OP_EQ,
	                                  subject_id, error))
		return NULL;

	venture_query_set_include_deleted(query, include_deleted);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	/* A bound, like every list: a thousand comments on one record is a
	 * forum, and the page would still answer. */
	venture_query_set_limit(query, 1000);

	return venture_database_find(venture_context_get_database(context), query,
	                             error);
}

JsonNode *
venture_comment_thread(
	VentureContext			 *context,
	const VentureAuthPrincipal	 *principal,
	const gchar			 *subject_type,
	gint64				  subject_id,
	GError				**error
){
	g_autoptr(VentureAccessScope) scope = NULL;
	g_autoptr(VentureEntity) subject = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(GHashTable) live_replies = NULL;
	guint i;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);

	if (NULL != principal)
		scope = venture_access_policy_enter(comment_policy(context), principal);

	subject = comment_load_subject(venture_context_get_database(context),
	                               subject_type, subject_id, error);

	if (NULL == subject)
		return NULL;

	rows = comment_list_all(context, venture_entity_get_entity_name(subject),
	                        subject_id, TRUE, error);

	if (NULL == rows)
		return NULL;

	/* Which threads still have something to say: a deleted comment
	 * with a live reply stays, as a placeholder. */
	live_replies = g_hash_table_new(g_int64_hash, g_int64_equal);

	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(rows, i);
		gint64 parent_id = 0;

		g_object_get(row, "parent-id", &parent_id, NULL);

		if ((0 != parent_id) && !venture_entity_is_deleted(row))
		{
			gint64 *key = g_new(gint64, 1);

			*key = parent_id;
			g_hash_table_add(live_replies, key);
		}
	}

	builder = json_builder_new();
	json_builder_begin_array(builder);

	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *top = g_ptr_array_index(rows, i);
		g_autoptr(JsonNode) node = NULL;
		gint64 top_id = venture_entity_get_id(top);
		gint64 parent_id = 0;
		guint j;

		g_object_get(top, "parent-id", &parent_id, NULL);

		if (0 != parent_id)
			continue;

		if (venture_entity_is_deleted(top) &&
		    !g_hash_table_contains(live_replies, &top_id))
			continue;

		node = venture_comment_to_json(context, principal, top);
		json_object_set_array_member(json_node_get_object(node), "replies",
		                             json_array_new());

		for (j = i + 1; j < rows->len; j++)
		{
			VentureEntity *reply = g_ptr_array_index(rows, j);
			gint64 reply_parent = 0;

			g_object_get(reply, "parent-id", &reply_parent, NULL);

			if ((reply_parent != top_id) || venture_entity_is_deleted(reply))
				continue;

			json_array_add_element(
				json_object_get_array_member(json_node_get_object(node),
				                             "replies"),
				venture_comment_to_json(context, principal, reply));
		}

		json_builder_add_value(builder, g_steal_pointer(&node));
	}

	json_builder_end_array(builder);

	/* The keys were allocated one by one; the table did not own them. */
	{
		GHashTableIter iter;
		gpointer key;

		g_hash_table_iter_init(&iter, live_replies);

		while (g_hash_table_iter_next(&iter, &key, NULL))
			g_free(key);
	}

	return json_builder_get_root(builder);
}

gint64
venture_comment_count(
	VentureContext	*context,
	const gchar	*subject_type,
	gint64		 subject_id
){
	g_autoptr(VentureQuery) query = NULL;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), 0);

	query = venture_query_new(VENTURE_TYPE_COMMENT);

	if (!venture_query_add_filter_string(query, "subject-type",
	                                     VENTURE_FILTER_OP_EQ, subject_type,
	                                     NULL) ||
	    !venture_query_add_filter_int(query, "subject-id", VENTURE_FILTER_OP_EQ,
	                                  subject_id, NULL))
		return 0;

	return MAX(venture_database_count(venture_context_get_database(context),
	                                  query, NULL), (gint64)0);
}

/* --- The composer's @ menu ------------------------------------------------- */

static gboolean
comment_prefix_matches(
	const gchar	*value,
	const gchar	*prefix
){
	g_autofree gchar *folded_value = NULL;
	g_autofree gchar *folded_prefix = NULL;
	g_auto(GStrv) words = NULL;
	gsize i;

	if (venture_string_is_empty(prefix))
		return TRUE;

	if (venture_string_is_empty(value))
		return FALSE;

	folded_value = g_utf8_casefold(value, -1);
	folded_prefix = g_utf8_casefold(prefix, -1);

	/* The start of any word: "ell" finds "Ruth Ellery". */
	words = g_strsplit_set(folded_value, " .-_", -1);

	for (i = 0; NULL != words[i]; i++)
	{
		if (g_str_has_prefix(words[i], folded_prefix))
			return TRUE;
	}

	return g_str_has_prefix(folded_value, folded_prefix);
}

JsonNode *
venture_comment_mention_candidates(
	VentureContext	*context,
	VentureEntity	*subject,
	const gchar	*prefix,
	guint		 limit
){
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) users = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	guint offered = 0;
	guint i;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);
	g_return_val_if_fail(VENTURE_IS_ENTITY(subject), NULL);

	if (0 == limit)
		limit = 8;

	internal = venture_access_policy_enter(comment_policy(context), NULL);
	query = venture_query_new(VENTURE_TYPE_USER);
	venture_query_add_filter_string(query, "active", VENTURE_FILTER_OP_EQ,
	                                "true", NULL);
	venture_query_add_order(query, "username", VENTURE_SORT_ASCENDING, NULL);
	/* People, not rows: an install with more accounts than this has a
	 * directory, and the menu narrows as somebody types. */
	venture_query_set_limit(query, 500);
	users = venture_database_find(venture_context_get_database(context), query,
	                              NULL);
	g_clear_object(&internal);

	builder = json_builder_new();
	json_builder_begin_array(builder);

	for (i = 0; (NULL != users) && (i < users->len) && (offered < limit); i++)
	{
		VentureEntity *user = g_ptr_array_index(users, i);
		g_autofree gchar *username = NULL;
		g_autofree gchar *display = NULL;
		g_autofree gchar *initials = NULL;

		g_object_get(user, "username", &username, "display-name", &display, NULL);

		if (!comment_prefix_matches(username, prefix) &&
		    !comment_prefix_matches(display, prefix))
			continue;

		/* Asked last, because it is the expensive question. */
		if (!venture_comment_user_can_read(context, venture_entity_get_id(user),
		                                   subject))
			continue;

		initials = comment_initials(!venture_string_is_empty(display)
		                            ? display : username);

		json_builder_begin_object(builder);
		comment_add_string(builder, "username", username);
		comment_add_string(builder, "name",
		                   !venture_string_is_empty(display) ? display : username);
		comment_add_string(builder, "initials", initials);
		json_builder_end_object(builder);
		offered++;
	}

	json_builder_end_array(builder);

	return json_builder_get_root(builder);
}
