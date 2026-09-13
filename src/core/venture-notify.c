/*
 * venture-notify.c - Watches, mentions and the inbox
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "venture.h"

#include <string.h>

/*
 * A JSON scalar as text: a string as itself, a number or a boolean
 * spelled out, null and anything else as %NULL. The shared coercion in
 * the JSON utilities is private to that file.
 */
static gchar *
venture_notify_node_to_text(JsonNode *node)
{
	if ((NULL == node) || !JSON_NODE_HOLDS_VALUE(node))
		return NULL;

	switch (json_node_get_value_type(node))
	{
	case G_TYPE_STRING:
		return g_strdup(json_node_get_string(node));
	case G_TYPE_INT64:
		return g_strdup_printf("%" G_GINT64_FORMAT, json_node_get_int(node));
	case G_TYPE_DOUBLE:
		return g_strdup_printf("%g", json_node_get_double(node));
	case G_TYPE_BOOLEAN:
		return g_strdup(json_node_get_boolean(node) ? "true" : "false");
	default:
		return NULL;
	}
}

/*
 * The types no inbox should hear about. A notification about a
 * notification would loop; a watch row is bookkeeping; the audit log is
 * its own record of everything; chat is private; a chunk or a crossref is
 * derived, and a corpus reindex would write a thousand rows about it.
 */
static gboolean
venture_notify_type_is_quiet(const gchar *type_name)
{
	static const gchar *const quiet[] = {
		"notification", "watch", "audit_entry", "chat_thread",
		"chat_message", "kb_chunk", "kb_link", "federation_replica", "federation_peer", "federation_grant", "api_token",
		"ledger_entry", NULL
	};
	gsize i;

	if (NULL == type_name)
		return TRUE;

	for (i = 0; NULL != quiet[i]; i++)
	{
		if (0 == g_strcmp0(quiet[i], type_name))
			return TRUE;
	}

	return FALSE;
}

/*
 * The rank a role holds, so "at least admin" can be a comparison. Owner
 * is the enum's zero, so the enum's own order cannot be used.
 */
static gint
venture_notify_role_rank(VentureUserRole role)
{
	switch (role)
	{
	case VENTURE_USER_ROLE_OWNER:   return 4;
	case VENTURE_USER_ROLE_ADMIN:   return 3;
	case VENTURE_USER_ROLE_EDITOR:  return 2;
	case VENTURE_USER_ROLE_SERVICE: return 2;
	case VENTURE_USER_ROLE_VIEWER:
	default:                        return 1;
	}
}

/* --- Watching -------------------------------------------------------------- */

/*
 * The one watch row for a user and a record, if there is one.
 */
static VentureEntity *
venture_notify_find_watch(
	VentureContext	*context,
	gint64		 user_id,
	const gchar	*target_type,
	gint64		 target_id
){
	g_autoptr(VentureQuery) query = NULL;

	query = venture_query_new(VENTURE_TYPE_WATCH);

	if (!venture_query_add_filter_int(query, "user-id", VENTURE_FILTER_OP_EQ,
	                                  user_id, NULL) ||
	    !venture_query_add_filter_string(query, "target-type",
	                                     VENTURE_FILTER_OP_EQ, target_type,
	                                     NULL) ||
	    !venture_query_add_filter_int(query, "target-id", VENTURE_FILTER_OP_EQ,
	                                  target_id, NULL))
		return NULL;

	venture_query_set_limit(query, 1);

	return venture_database_find_one(venture_context_get_database(context),
	                                 query, NULL);
}

gboolean
venture_notify_watch(
	VentureContext	 *context,
	gint64		  user_id,
	const gchar	 *target_type,
	gint64		  target_id,
	GError		**error
){
	g_autoptr(VentureEntity) existing = NULL;
	g_autoptr(VentureEntity) target = NULL;
	g_autoptr(VentureWatch) watch = NULL;
	g_autofree gchar *label = NULL;
	GType entity_type;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), FALSE);
	g_return_val_if_fail(NULL != target_type, FALSE);

	if (0 == user_id)
	{
		g_set_error_literal(error, VENTURE_ERROR,
		                    VENTURE_ERROR_INVALID_ARGUMENT,
		                    "Only a signed-in user can watch a record");
		return FALSE;
	}

	/* The record must be one that exists, through the mask: a type that
	 * is off is not one anybody can be told about. */
	entity_type = venture_entity_registry_lookup(
		venture_context_get_entity_registry(context), target_type);

	if (G_TYPE_INVALID == entity_type)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		            "There is no record type called \"%s\"", target_type);
		return FALSE;
	}

	target = venture_database_get(venture_context_get_database(context),
	                              entity_type, target_id, error);

	if (NULL == target)
		return FALSE;

	existing = venture_notify_find_watch(context, user_id, target_type,
	                                     target_id);

	if (NULL != existing)
		return TRUE;

	label = venture_entity_get_display_name(target);

	watch = venture_watch_new();
	g_object_set(watch,
	             "user-id", user_id,
	             "target-type", venture_entity_get_entity_name(target),
	             "target-id", target_id,
	             "target-label", label,
	             NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(watch),
		venture_entity_get_organization_id(target));

	return venture_database_save(venture_context_get_database(context),
	                             VENTURE_ENTITY(watch), NULL, error);
}

gboolean
venture_notify_unwatch(
	VentureContext	 *context,
	gint64		  user_id,
	const gchar	 *target_type,
	gint64		  target_id,
	GError		**error
){
	g_autoptr(VentureEntity) existing = NULL;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), FALSE);

	existing = venture_notify_find_watch(context, user_id, target_type,
	                                     target_id);

	if (NULL == existing)
		return TRUE;

	/* Purged rather than soft-deleted: a watch carries no history, and a
	 * soft-deleted one would still be a row the watcher query has to
	 * remember to skip. */
	return venture_database_purge(venture_context_get_database(context),
	                              existing, NULL, error);
}

gboolean
venture_notify_is_watching(
	VentureContext	*context,
	gint64		 user_id,
	const gchar	*target_type,
	gint64		 target_id
){
	g_autoptr(VentureEntity) existing = NULL;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), FALSE);

	if ((0 == user_id) || (NULL == target_type))
		return FALSE;

	existing = venture_notify_find_watch(context, user_id, target_type,
	                                     target_id);

	return (NULL != existing);
}

GArray *
venture_notify_list_watchers(
	VentureContext	*context,
	const gchar	*target_type,
	gint64		 target_id
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	GArray *users;
	guint i;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);

	users = g_array_new(FALSE, FALSE, sizeof(gint64));

	if (NULL == target_type)
		return users;

	query = venture_query_new(VENTURE_TYPE_WATCH);

	if (!venture_query_add_filter_string(query, "target-type",
	                                     VENTURE_FILTER_OP_EQ, target_type,
	                                     NULL) ||
	    !venture_query_add_filter_int(query, "target-id", VENTURE_FILTER_OP_EQ,
	                                  target_id, NULL))
		return users;

	venture_query_set_limit(query, 0);
	rows = venture_database_find(venture_context_get_database(context), query,
	                             NULL);

	if (NULL == rows)
		return users;

	for (i = 0; i < rows->len; i++)
	{
		gint64 user_id = 0;

		g_object_get(g_ptr_array_index(rows, i), "user-id", &user_id, NULL);

		if (0 != user_id)
			g_array_append_val(users, user_id);
	}

	return users;
}

GPtrArray *
venture_notify_list_watched(
	VentureContext	*context,
	gint64		 user_id,
	guint		 limit
){
	g_autoptr(VentureQuery) query = NULL;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);

	query = venture_query_new(VENTURE_TYPE_WATCH);

	if (!venture_query_add_filter_int(query, "user-id", VENTURE_FILTER_OP_EQ,
	                                  user_id, NULL))
		return NULL;

	venture_query_add_order(query, "id", VENTURE_SORT_DESCENDING, NULL);
	venture_query_set_limit(query, limit);

	return venture_database_find(venture_context_get_database(context), query,
	                             NULL);
}

/* --- Sending --------------------------------------------------------------- */

gboolean
venture_notify_send(
	VentureContext		 *context,
	gint64			  user_id,
	VentureNotificationKind	  kind,
	const gchar		 *title,
	const gchar		 *body,
	const gchar		 *target_type,
	gint64			  target_id,
	const gchar		 *target_label,
	const gchar		 *actor,
	GError			**error
){
	g_autoptr(VentureNotification) notification = NULL;
	g_autoptr(GDateTime) now = NULL;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), FALSE);
	g_return_val_if_fail(NULL != title, FALSE);

	if (0 == user_id)
		return TRUE;

	now = venture_time_now();

	notification = venture_notification_new();
	g_object_set(notification,
	             "user-id", user_id,
	             "kind", kind,
	             "title", title,
	             "body", body,
	             "target-type", target_type,
	             "target-id", target_id,
	             "target-label", target_label,
	             "actor", actor,
	             "occurred-at", now,
	             NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(notification),
		venture_context_get_default_organization_id(context));

	/* Written as the system: the actor is a field of the notification,
	 * not the author of the row, and an inbox entry attributed to the
	 * person it tells about would read as if they wrote it. */
	return venture_database_save(venture_context_get_database(context),
	                             VENTURE_ENTITY(notification), NULL, error);
}

gint
venture_notify_broadcast(
	VentureContext		 *context,
	VentureUserRole		  minimum_role,
	VentureNotificationKind	  kind,
	const gchar		 *title,
	const gchar		 *body,
	const gchar		 *target_type,
	gint64			  target_id,
	const gchar		 *target_label,
	const gchar		 *actor,
	GError			**error
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) users = NULL;
	gint sent;
	guint i;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), -1);

	query = venture_query_new(VENTURE_TYPE_USER);
	venture_query_set_limit(query, 0);
	users = venture_database_find(venture_context_get_database(context), query,
	                              error);

	if (NULL == users)
		return -1;

	sent = 0;

	for (i = 0; i < users->len; i++)
	{
		VentureEntity *user;
		VentureUserRole role;
		gboolean active = FALSE;

		user = g_ptr_array_index(users, i);
		g_object_get(user, "role", &role, "active", &active, NULL);

		if (!active)
			continue;

		if (venture_notify_role_rank(role) <
		    venture_notify_role_rank(minimum_role))
			continue;

		if (!venture_notify_send(context, venture_entity_get_id(user), kind,
		                         title, body, target_type, target_id,
		                         target_label, actor, error))
			return -1;

		sent++;
	}

	return sent;
}

/* --- Reading --------------------------------------------------------------- */

static VentureQuery *
venture_notify_inbox_query(
	gint64		user_id,
	gboolean	unread_only
){
	VentureQuery *query;

	query = venture_query_new(VENTURE_TYPE_NOTIFICATION);

	if (!venture_query_add_filter_int(query, "user-id", VENTURE_FILTER_OP_EQ,
	                                  user_id, NULL))
	{
		g_object_unref(query);
		return NULL;
	}

	if (unread_only)
		venture_query_add_filter_string(query, "read-at",
		                                VENTURE_FILTER_OP_IS_NULL, NULL,
		                                NULL);

	return query;
}

gint64
venture_notify_unread_count(
	VentureContext	*context,
	gint64		 user_id
){
	g_autoptr(VentureQuery) query = NULL;
	gint64 count;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), 0);

	if (0 == user_id)
		return 0;

	query = venture_notify_inbox_query(user_id, TRUE);

	if (NULL == query)
		return 0;

	count = venture_database_count(venture_context_get_database(context),
	                               query, NULL);

	return (count > 0) ? count : 0;
}

GPtrArray *
venture_notify_list(
	VentureContext	 *context,
	gint64		  user_id,
	gboolean	  unread_only,
	guint		  limit,
	GError		**error
){
	g_autoptr(VentureQuery) query = NULL;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);

	query = venture_notify_inbox_query(user_id, unread_only);

	if (NULL == query)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_FAILED,
		                    "Cannot build the inbox query");
		return NULL;
	}

	venture_query_add_order(query, "id", VENTURE_SORT_DESCENDING, NULL);
	venture_query_set_limit(query, (0 == limit) ? 50 : limit);

	return venture_database_find(venture_context_get_database(context), query,
	                             error);
}

gint
venture_notify_mark_read(
	VentureContext	 *context,
	gint64		  user_id,
	gint64		  notification_id,
	GError		**error
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GDateTime) now = NULL;
	guint i;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), -1);

	query = venture_notify_inbox_query(user_id, TRUE);

	if (NULL == query)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_FAILED,
		                    "Cannot build the inbox query");
		return -1;
	}

	/* Scoped to the caller's inbox before the id is applied, which is
	 * what makes somebody else's notification simply not there. */
	if (0 != notification_id)
	{
		if (!venture_query_add_filter_int(query, "id", VENTURE_FILTER_OP_EQ,
		                                  notification_id, error))
			return -1;
	}

	venture_query_set_limit(query, 0);
	rows = venture_database_find(venture_context_get_database(context), query,
	                             error);

	if (NULL == rows)
		return -1;

	if ((0 != notification_id) && (0 == rows->len))
	{
		/* Either read already or not theirs; both read as "nothing to
		 * do" from the outside, and a found-but-read one is fine. */
		g_autoptr(VentureEntity) any = NULL;
		g_autoptr(VentureQuery) check = NULL;

		check = venture_notify_inbox_query(user_id, FALSE);
		venture_query_add_filter_int(check, "id", VENTURE_FILTER_OP_EQ,
		                             notification_id, NULL);
		any = venture_database_find_one(venture_context_get_database(context),
		                                check, NULL);

		if (NULL == any)
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
			            "There is no notification %" G_GINT64_FORMAT
			            " in your inbox", notification_id);
			return -1;
		}

		return 0;
	}

	now = venture_time_now();

	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *row;

		row = g_ptr_array_index(rows, i);
		g_object_set(row, "read-at", now, NULL);

		if (!venture_database_save(venture_context_get_database(context), row,
		                           NULL, error))
			return -1;
	}

	return (gint)rows->len;
}

/* --- Mentions -------------------------------------------------------------- */

GStrv
venture_notify_extract_mentions(const gchar *text)
{
	g_autoptr(GPtrArray) names = NULL;
	const gchar *cursor;

	names = g_ptr_array_new_with_free_func(g_free);

	for (cursor = text; (NULL != cursor) && ('\0' != *cursor); cursor++)
	{
		const gchar *start;
		const gchar *end;
		g_autofree gchar *name = NULL;
		guint i;
		gboolean seen;

		if ('@' != *cursor)
			continue;

		/* An @ inside a word -- an email address -- names nobody. */
		if ((cursor > text) &&
		    (g_ascii_isalnum(cursor[-1]) || ('.' == cursor[-1])))
			continue;

		start = cursor + 1;
		end = start;

		while (g_ascii_isalnum(*end) || ('_' == *end) || ('-' == *end) ||
		       ('.' == *end))
			end++;

		/* Trailing punctuation belongs to the sentence. */
		while ((end > start) && ('.' == end[-1]))
			end--;

		if (end == start)
			continue;

		name = g_strndup(start, (gsize)(end - start));
		seen = FALSE;

		for (i = 0; i < names->len; i++)
		{
			if (0 == g_strcmp0(g_ptr_array_index(names, i), name))
			{
				seen = TRUE;
				break;
			}
		}

		if (!seen)
			g_ptr_array_add(names, g_steal_pointer(&name));

		cursor = end - 1;
	}

	g_ptr_array_add(names, NULL);

	return (GStrv)g_ptr_array_free(g_steal_pointer(&names), FALSE);
}

gint64
venture_notify_user_id_for_username(
	VentureContext	*context,
	const gchar	*username
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureEntity) user = NULL;
	gboolean active = FALSE;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), 0);

	if (venture_string_is_empty(username))
		return 0;

	query = venture_query_new(VENTURE_TYPE_USER);

	if (!venture_query_add_filter_string(query, "username",
	                                     VENTURE_FILTER_OP_EQ, username, NULL))
		return 0;

	venture_query_set_limit(query, 1);
	user = venture_database_find_one(venture_context_get_database(context),
	                                 query, NULL);

	if (NULL == user)
		return 0;

	g_object_get(user, "active", &active, NULL);

	return active ? venture_entity_get_id(user) : 0;
}

/* --- JSON ------------------------------------------------------------------ */

JsonNode *
venture_notify_to_json(VentureEntity *notification)
{
	g_autoptr(JsonBuilder) builder = NULL;
	g_autofree gchar *title = NULL;
	g_autofree gchar *body = NULL;
	g_autofree gchar *actor = NULL;
	g_autofree gchar *target_type = NULL;
	g_autofree gchar *target_label = NULL;
	g_autoptr(GDateTime) read_at = NULL;
	g_autoptr(GDateTime) occurred_at = NULL;
	VentureNotificationKind kind;
	gint64 target_id = 0;

	g_return_val_if_fail(VENTURE_IS_NOTIFICATION(notification), NULL);

	g_object_get(notification,
	             "kind", &kind, "title", &title, "body", &body,
	             "actor", &actor, "target-type", &target_type,
	             "target-id", &target_id, "target-label", &target_label,
	             "read-at", &read_at, "occurred-at", &occurred_at,
	             NULL);

	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "id");
	json_builder_add_int_value(builder, venture_entity_get_id(notification));
	json_builder_set_member_name(builder, "kind");
	json_builder_add_string_value(builder,
		venture_enum_to_nick(VENTURE_TYPE_NOTIFICATION_KIND, (gint)kind));
	json_builder_set_member_name(builder, "title");
	json_builder_add_string_value(builder, title);
	json_builder_set_member_name(builder, "body");

	if (NULL != body)
		json_builder_add_string_value(builder, body);
	else
		json_builder_add_null_value(builder);

	json_builder_set_member_name(builder, "actor");

	if (NULL != actor)
		json_builder_add_string_value(builder, actor);
	else
		json_builder_add_null_value(builder);

	json_builder_set_member_name(builder, "target_type");

	if (NULL != target_type)
		json_builder_add_string_value(builder, target_type);
	else
		json_builder_add_null_value(builder);

	json_builder_set_member_name(builder, "target_id");
	json_builder_add_int_value(builder, target_id);
	json_builder_set_member_name(builder, "target_label");

	if (NULL != target_label)
		json_builder_add_string_value(builder, target_label);
	else
		json_builder_add_null_value(builder);

	json_builder_set_member_name(builder, "url");

	if ((NULL != target_type) && (0 != target_id))
	{
		g_autofree gchar *url = NULL;

		url = g_strdup_printf("/e/%s/%" G_GINT64_FORMAT, target_type,
		                      target_id);
		json_builder_add_string_value(builder, url);
	}
	else
	{
		json_builder_add_null_value(builder);
	}

	json_builder_set_member_name(builder, "read");
	json_builder_add_boolean_value(builder, (NULL != read_at));
	json_builder_set_member_name(builder, "occurred_at");

	if (NULL != occurred_at)
	{
		g_autofree gchar *when = NULL;

		when = venture_time_to_string(occurred_at);
		json_builder_add_string_value(builder, when);
	}
	else
	{
		json_builder_add_null_value(builder);
	}

	json_builder_end_object(builder);

	return json_builder_get_root(builder);
}

/* --- The audit hook -------------------------------------------------------- */

/*
 * "Status: todo -> in progress; Assignee: -> zach", from an audit diff.
 * The first few changed fields, because an inbox line is a line.
 */
static gchar *
venture_notify_describe_diff(const gchar *diff_text)
{
	g_autoptr(JsonNode) diff = NULL;
	g_autoptr(GString) out = NULL;
	g_autoptr(GList) members = NULL;
	JsonObject *object;
	GList *cursor;
	guint shown;

	if (venture_string_is_empty(diff_text))
		return NULL;

	diff = venture_json_parse(diff_text, NULL);

	if ((NULL == diff) || !JSON_NODE_HOLDS_OBJECT(diff))
		return NULL;

	object = json_node_get_object(diff);
	members = json_object_get_members(object);
	out = g_string_new(NULL);
	shown = 0;

	for (cursor = members; NULL != cursor; cursor = cursor->next)
	{
		const gchar *member;
		JsonNode *change;
		g_autofree gchar *from = NULL;
		g_autofree gchar *to = NULL;
		g_autofree gchar *label = NULL;
		gchar *walk;

		member = cursor->data;
		change = json_object_get_member(object, member);

		/* The identity spine moves on every save and says nothing. */
		if ((0 == g_strcmp0(member, "updated-at")) ||
		    (0 == g_strcmp0(member, "version")) ||
		    (0 == g_strcmp0(member, "updated_at")))
			continue;

		if (shown >= 4)
		{
			g_string_append(out, "; \xe2\x80\xa6");
			break;
		}

		if ((NULL != change) && JSON_NODE_HOLDS_OBJECT(change))
		{
			JsonObject *pair;

			pair = json_node_get_object(change);
			from = venture_notify_node_to_text(
				json_object_get_member(pair, "from"));
			to = venture_notify_node_to_text(
				json_object_get_member(pair, "to"));
		}

		label = g_strdup(member);

		for (walk = label; '\0' != *walk; walk++)
		{
			if (('-' == *walk) || ('_' == *walk))
				*walk = ' ';
		}

		if (g_ascii_islower(label[0]))
			label[0] = g_ascii_toupper(label[0]);

		if (shown > 0)
			g_string_append(out, "; ");

		g_string_append(out, label);
		g_string_append(out, ": ");
		g_string_append(out, venture_string_is_empty(from) ? "\xe2\x80\x94"
		                                                   : from);
		g_string_append(out, " \xe2\x86\x92 ");
		g_string_append(out, venture_string_is_empty(to) ? "\xe2\x80\x94"
		                                                 : to);
		shown++;
	}

	if (0 == out->len)
		return NULL;

	return g_string_free(g_steal_pointer(&out), FALSE);
}

/*
 * Whether an audit diff touched one field.
 */
static gboolean
venture_notify_diff_has(
	const gchar	*diff_text,
	const gchar	*field
){
	g_autoptr(JsonNode) diff = NULL;

	if (venture_string_is_empty(diff_text))
		return FALSE;

	diff = venture_json_parse(diff_text, NULL);

	if ((NULL == diff) || !JSON_NODE_HOLDS_OBJECT(diff))
		return FALSE;

	return json_object_has_member(json_node_get_object(diff), field);
}

/*
 * Tells everybody watching a record that it changed, except whoever
 * changed it: nobody needs telling what they just did.
 */
static void
venture_notify_watchers(
	VentureContext		*context,
	const gchar		*target_type,
	gint64			 target_id,
	const gchar		*target_label,
	const gchar		*actor,
	gint64			 actor_user_id,
	const gchar		*title,
	const gchar		*body
){
	g_autoptr(GArray) watchers = NULL;
	guint i;

	watchers = venture_notify_list_watchers(context, target_type, target_id);

	for (i = 0; i < watchers->len; i++)
	{
		gint64 user_id;

		user_id = g_array_index(watchers, gint64, i);

		if (user_id == actor_user_id)
			continue;

		venture_notify_send(context, user_id, VENTURE_NOTIFICATION_KIND_WATCHED,
		                    title, body, target_type, target_id, target_label,
		                    actor, NULL);
	}
}

/*
 * A ticket handed to somebody: they hear about it and follow it from
 * then on. The assignee is a username, so a name that is not a user is
 * simply nobody to tell.
 */
static void
venture_notify_assignment(
	VentureContext	*context,
	VentureEntity	*ticket,
	const gchar	*actor,
	gint64		 actor_user_id
){
	g_autofree gchar *assignee = NULL;
	g_autofree gchar *title = NULL;
	g_autofree gchar *label = NULL;
	gint64 assignee_id;

	g_object_get(ticket, "assignee", &assignee, NULL);
	assignee_id = venture_notify_user_id_for_username(context, assignee);

	if (0 == assignee_id)
		return;

	venture_notify_watch(context, assignee_id, "ticket",
	                     venture_entity_get_id(ticket), NULL);

	if (assignee_id == actor_user_id)
		return;

	label = venture_entity_get_display_name(ticket);
	title = g_strdup_printf("%s assigned you: %s",
	                        venture_string_is_empty(actor) ? "Somebody" : actor,
	                        label);
	venture_notify_send(context, assignee_id, VENTURE_NOTIFICATION_KIND_ASSIGNED,
	                    title, NULL, "ticket", venture_entity_get_id(ticket),
	                    label, actor, NULL);
}

/*
 * A comment: the people named in it are told and start following; the
 * people following are told. The comment's target for the inbox is the
 * ticket, which is the page anybody would open.
 */
static void
venture_notify_comment(
	VentureContext	*context,
	VentureEntity	*comment,
	const gchar	*actor,
	gint64		 actor_user_id
){
	g_autoptr(VentureEntity) ticket = NULL;
	g_autofree gchar *body = NULL;
	g_autofree gchar *author = NULL;
	g_autofree gchar *label = NULL;
	g_autofree gchar *excerpt = NULL;
	g_auto(GStrv) mentions = NULL;
	g_autoptr(GArray) told = NULL;
	g_autoptr(GArray) watchers = NULL;
	gboolean internal = FALSE;
	gint64 ticket_id = 0;
	gsize i;
	guint j;

	g_object_get(comment, "ticket-id", &ticket_id, "body", &body,
	             "author", &author, "internal", &internal, NULL);

	if (0 == ticket_id)
		return;

	ticket = venture_database_get(venture_context_get_database(context),
	                              VENTURE_TYPE_TICKET, ticket_id, NULL);

	if (NULL == ticket)
		return;

	/*
	 * The comment's author is who wrote it; the audit entry's actor is
	 * the credential that carried it. For an inbox line the author is
	 * what the reader needs -- "carol commented" rather than the name
	 * of the API token her comment arrived on -- so it wins when it is
	 * set. The audit trail is unaffected and still records the actor.
	 *
	 * The same substitution has to reach actor_user_id, or the person
	 * who wrote the comment is told about their own comment.
	 */
	if (!venture_string_is_empty(author))
	{
		actor = author;
		actor_user_id = venture_notify_user_id_for_username(context, author);
	}
	else if (0 == actor_user_id)
	{
		actor_user_id = venture_notify_user_id_for_username(context, actor);
	}

	label = venture_entity_get_display_name(ticket);
	excerpt = venture_truncate(body, 140);
	told = g_array_new(FALSE, FALSE, sizeof(gint64));

	/* Whoever commented is following the thread from now on. */
	if (0 != actor_user_id)
		venture_notify_watch(context, actor_user_id, "ticket", ticket_id, NULL);

	mentions = venture_notify_extract_mentions(body);

	for (i = 0; NULL != mentions[i]; i++)
	{
		g_autofree gchar *title = NULL;
		gint64 user_id;

		user_id = venture_notify_user_id_for_username(context, mentions[i]);

		if ((0 == user_id) || (user_id == actor_user_id))
			continue;

		venture_notify_watch(context, user_id, "ticket", ticket_id, NULL);

		title = g_strdup_printf("%s mentioned you on %s",
		                        venture_string_is_empty(actor) ? "Somebody"
		                                                       : actor,
		                        label);
		venture_notify_send(context, user_id, VENTURE_NOTIFICATION_KIND_MENTION,
		                    title, excerpt, "ticket", ticket_id, label, actor,
		                    NULL);
		g_array_append_val(told, user_id);
	}

	watchers = venture_notify_list_watchers(context, "ticket", ticket_id);

	for (j = 0; j < watchers->len; j++)
	{
		g_autofree gchar *title = NULL;
		gint64 user_id;
		guint k;
		gboolean already;

		user_id = g_array_index(watchers, gint64, j);

		if (user_id == actor_user_id)
			continue;

		already = FALSE;

		for (k = 0; k < told->len; k++)
		{
			if (g_array_index(told, gint64, k) == user_id)
				already = TRUE;
		}

		if (already)
			continue;

		title = g_strdup_printf("%s %s on %s",
		                        venture_string_is_empty(actor) ? "Somebody"
		                                                       : actor,
		                        internal ? "left a note" : "commented",
		                        label);
		venture_notify_send(context, user_id, VENTURE_NOTIFICATION_KIND_WATCHED,
		                    title, excerpt, "ticket", ticket_id, label, actor,
		                    NULL);
	}
}

/*
 * Every audited change comes through here. The audit entry carries the
 * actor, the target and the diff, which is all the inbox needs -- and
 * because every writer is audited, this is the one place.
 */
static void
venture_notify_on_audit(
	VentureDatabase	*database,
	VentureEntity	*entry,
	gpointer	 user_data
){
	VentureContext *context;
	g_autofree gchar *actor = NULL;
	g_autofree gchar *target_type = NULL;
	g_autofree gchar *target_label = NULL;
	g_autofree gchar *diff = NULL;
	VentureAuditAction action;
	VentureActorKind actor_kind;
	gint64 target_id = 0;
	gint64 actor_user_id;

	(void)database;
	context = user_data;

	g_object_get(entry,
	             "action", &action, "actor-kind", &actor_kind,
	             "actor", &actor, "target-type", &target_type,
	             "target-id", &target_id, "target-label", &target_label,
	             "diff", &diff, NULL);

	if (venture_notify_type_is_quiet(target_type) || (0 == target_id))
		return;

	if ((VENTURE_AUDIT_ACTION_CREATE != action) &&
	    (VENTURE_AUDIT_ACTION_UPDATE != action) &&
	    (VENTURE_AUDIT_ACTION_DELETE != action))
		return;

	/*
	 * The system's own updates are bookkeeping -- a first-reply stamp, a
	 * logged-hours total, a breach mark, a budget's warned-at -- made in
	 * the wake of something a person did and already told about. Telling
	 * the watchers "Somebody updated" for each would double every event.
	 * A person, the AI and an automation all keep their own actor kind.
	 */
	if ((VENTURE_ACTOR_KIND_SYSTEM == actor_kind) &&
	    (VENTURE_AUDIT_ACTION_UPDATE == action))
		return;

	/* Only a person can be exempted from hearing about their own
	 * change; the AI and an automation act for somebody, and that
	 * somebody wants to know. */
	actor_user_id = (VENTURE_ACTOR_KIND_USER == actor_kind)
		? venture_notify_user_id_for_username(context, actor) : 0;

	/* A comment is about its ticket. */
	if (0 == g_strcmp0(target_type, "ticket_comment"))
	{
		g_autoptr(VentureEntity) comment = NULL;

		if (VENTURE_AUDIT_ACTION_CREATE != action)
			return;

		comment = venture_database_get(venture_context_get_database(context),
		                               VENTURE_TYPE_TICKET_COMMENT, target_id,
		                               NULL);

		if (NULL != comment)
			venture_notify_comment(context, comment, actor, actor_user_id);

		return;
	}

	if (0 == g_strcmp0(target_type, "ticket"))
	{
		g_autoptr(VentureEntity) ticket = NULL;

		ticket = venture_database_get(venture_context_get_database(context),
		                              VENTURE_TYPE_TICKET, target_id, NULL);

		if (NULL != ticket)
		{
			/* Whoever raises a ticket follows it. */
			if ((VENTURE_AUDIT_ACTION_CREATE == action) &&
			    (0 != actor_user_id))
				venture_notify_watch(context, actor_user_id, "ticket",
				                     target_id, NULL);

			if ((VENTURE_AUDIT_ACTION_CREATE == action) ||
			    venture_notify_diff_has(diff, "assignee"))
				venture_notify_assignment(context, ticket, actor,
				                          actor_user_id);
		}
	}

	/* A coding run reaching an end state tells the ticket's followers,
	 * against the ticket: the run is evidence, the ticket is the page. */
	if ((0 == g_strcmp0(target_type, "forge_run")) &&
	    venture_notify_diff_has(diff, "state"))
	{
		g_autoptr(VentureEntity) run = NULL;
		g_autoptr(VentureEntity) ticket = NULL;
		VentureForgeRunState state;
		gint64 ticket_id = 0;

		run = venture_database_get(venture_context_get_database(context),
		                           VENTURE_TYPE_FORGE_RUN, target_id, NULL);

		if (NULL == run)
			return;

		g_object_get(run, "state", &state, "ticket-id", &ticket_id, NULL);

		if ((VENTURE_FORGE_RUN_STATE_QUEUED == state) ||
		    (VENTURE_FORGE_RUN_STATE_RUNNING == state))
			return;

		ticket = venture_database_get(venture_context_get_database(context),
		                              VENTURE_TYPE_TICKET, ticket_id, NULL);

		if (NULL != ticket)
		{
			g_autofree gchar *label = NULL;
			g_autofree gchar *title = NULL;
			g_autofree gchar *summary = NULL;
			g_autofree gchar *failure = NULL;
			g_autoptr(GArray) watchers = NULL;
			guint i;

			label = venture_entity_get_display_name(ticket);
			title = g_strdup_printf("Run #%" G_GINT64_FORMAT " %s: %s",
			                        target_id,
			                        venture_enum_to_nick(
			                        	VENTURE_TYPE_FORGE_RUN_STATE,
			                        	(gint)state),
			                        label);
			g_object_get(run, "summary", &summary,
			             "failure-reason", &failure, NULL);
			watchers = venture_notify_list_watchers(context, "ticket",
			                                        ticket_id);

			for (i = 0; i < watchers->len; i++)
				venture_notify_send(context,
				                    g_array_index(watchers, gint64, i),
				                    VENTURE_NOTIFICATION_KIND_RUN, title,
				                    !venture_string_is_empty(failure)
				                    	? failure : summary,
				                    "ticket", ticket_id, label, NULL, NULL);
		}

		return;
	}

	/* And then everybody following it, whatever it is. A creation has
	 * no watchers yet, so only a change or a removal is worth the query. */
	if (VENTURE_AUDIT_ACTION_CREATE != action)
	{
		g_autofree gchar *title = NULL;
		g_autofree gchar *body = NULL;

		title = g_strdup_printf("%s %s %s",
		                        venture_string_is_empty(actor) ? "Somebody"
		                                                       : actor,
		                        (VENTURE_AUDIT_ACTION_DELETE == action)
		                        	? "deleted" : "updated",
		                        venture_string_is_empty(target_label)
		                        	? target_type : target_label);
		body = venture_notify_describe_diff(diff);
		venture_notify_watchers(context, target_type, target_id,
		                        target_label, actor, actor_user_id, title,
		                        body);
	}
}

void
venture_notify_install(VentureContext *context)
{
	g_return_if_fail(VENTURE_IS_CONTEXT(context));

	/* The context holds the database, so the database cannot outlive
	 * the context this handler points at; no reference is taken, for
	 * the same reason the validators take none. */
	g_signal_connect(venture_context_get_database(context), "audit",
	                 G_CALLBACK(venture_notify_on_audit), context);
}
