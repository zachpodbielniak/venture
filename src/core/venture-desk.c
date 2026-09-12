/*
 * venture-desk.c - The workdesk: macros, worklogs, sprints, bulk edits
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
venture_desk_node_to_text(JsonNode *node)
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

/* --- Macros ---------------------------------------------------------------- */

/*
 * "{me}", "{ticket}" and "{title}" in a macro's text, filled in. Plain
 * substitution -- a macro is a canned reply, not a template language.
 */
static gchar *
venture_desk_fill(
	const gchar	*text,
	VentureEntity	*ticket,
	const gchar	*me
){
	g_autoptr(GString) out = NULL;
	g_autofree gchar *title = NULL;
	g_autofree gchar *number = NULL;
	const gchar *cursor;

	if (NULL == text)
		return NULL;

	g_object_get(ticket, "title", &title, NULL);
	number = g_strdup_printf("#%" G_GINT64_FORMAT, venture_entity_get_id(ticket));
	out = g_string_new(NULL);

	for (cursor = text; '\0' != *cursor; cursor++)
	{
		if (g_str_has_prefix(cursor, "{me}"))
		{
			g_string_append(out, venture_string_is_empty(me) ? "" : me);
			cursor += strlen("{me}") - 1;
		}
		else if (g_str_has_prefix(cursor, "{ticket}"))
		{
			g_string_append(out, number);
			cursor += strlen("{ticket}") - 1;
		}
		else if (g_str_has_prefix(cursor, "{title}"))
		{
			g_string_append(out, (NULL != title) ? title : "");
			cursor += strlen("{title}") - 1;
		}
		else
		{
			g_string_append_c(out, *cursor);
		}
	}

	return g_string_free(g_steal_pointer(&out), FALSE);
}

/*
 * The union of two comma-separated tag lists, the existing order kept
 * and each new tag once.
 */
static gchar *
venture_desk_merge_tags(
	const gchar	*existing,
	const gchar	*added
){
	g_autoptr(GPtrArray) tags = NULL;
	g_auto(GStrv) old_parts = NULL;
	g_auto(GStrv) new_parts = NULL;
	g_autoptr(GString) out = NULL;
	gsize i;
	guint j;

	tags = g_ptr_array_new_with_free_func(g_free);
	old_parts = g_strsplit((NULL != existing) ? existing : "", ",", -1);
	new_parts = g_strsplit((NULL != added) ? added : "", ",", -1);

	for (i = 0; NULL != old_parts[i]; i++)
	{
		g_autofree gchar *tag = NULL;

		tag = g_strstrip(g_strdup(old_parts[i]));

		if (!venture_string_is_empty(tag))
			g_ptr_array_add(tags, g_steal_pointer(&tag));
	}

	for (i = 0; NULL != new_parts[i]; i++)
	{
		g_autofree gchar *tag = NULL;
		gboolean seen;

		tag = g_strstrip(g_strdup(new_parts[i]));

		if (venture_string_is_empty(tag))
			continue;

		seen = FALSE;

		for (j = 0; j < tags->len; j++)
		{
			if (0 == g_ascii_strcasecmp(g_ptr_array_index(tags, j), tag))
				seen = TRUE;
		}

		if (!seen)
			g_ptr_array_add(tags, g_steal_pointer(&tag));
	}

	out = g_string_new(NULL);

	for (j = 0; j < tags->len; j++)
	{
		if (j > 0)
			g_string_append(out, ", ");

		g_string_append(out, g_ptr_array_index(tags, j));
	}

	return g_string_free(g_steal_pointer(&out), FALSE);
}

gboolean
venture_desk_apply_macro(
	VentureContext		 *context,
	VentureEntity		 *ticket,
	VentureEntity		 *macro,
	const VentureActor	 *actor,
	GError			**error
){
	VentureDatabase *database;
	g_autofree gchar *body = NULL;
	g_autofree gchar *assignee = NULL;
	g_autofree gchar *add_tags = NULL;
	const gchar *me;
	gboolean internal = FALSE;
	gboolean apply_status = FALSE;
	gboolean apply_priority = FALSE;
	gboolean active = FALSE;
	VentureTicketStatus status;
	VenturePriority priority;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), FALSE);
	g_return_val_if_fail(VENTURE_IS_TICKET(ticket), FALSE);
	g_return_val_if_fail(VENTURE_IS_MACRO(macro), FALSE);

	database = venture_context_get_database(context);
	me = (NULL != actor) ? actor->name : NULL;

	g_object_get(macro,
	             "body", &body, "internal", &internal,
	             "apply-status", &apply_status, "status", &status,
	             "apply-priority", &apply_priority, "priority", &priority,
	             "assignee", &assignee, "add-tags", &add_tags,
	             "active", &active, NULL);

	if (!active)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT,
		                    "That macro is switched off");
		return FALSE;
	}

	if (!venture_database_begin(database, error))
		return FALSE;

	/*
	 * The changes first, then the reply. The other order reads better
	 * in the timeline, but a visible reply stamps the ticket's first
	 * response from inside the comment's save, and the ticket in hand
	 * would then be a version behind its own row: the second save would
	 * be a conflict with a change this same call made.
	 */
	if (apply_status)
		g_object_set(ticket, "status", status, NULL);

	if (apply_priority)
		g_object_set(ticket, "priority", priority, NULL);

	if (!venture_string_is_empty(assignee))
	{
		g_autofree gchar *who = NULL;

		who = venture_desk_fill(assignee, ticket, me);

		if (!venture_string_is_empty(who))
			g_object_set(ticket, "assignee", who, NULL);
	}

	if (!venture_string_is_empty(add_tags))
	{
		g_autofree gchar *existing = NULL;
		g_autofree gchar *merged = NULL;

		g_object_get(ticket, "tags", &existing, NULL);
		merged = venture_desk_merge_tags(existing, add_tags);
		g_object_set(ticket, "tags", merged, NULL);
	}

	if (!venture_database_save(database, ticket, actor, error))
	{
		venture_database_rollback(database);
		return FALSE;
	}

	if (!venture_string_is_empty(body))
	{
		g_autoptr(VentureTicketComment) comment = NULL;
		g_autofree gchar *text = NULL;
		g_autoptr(GDateTime) now = NULL;

		text = venture_desk_fill(body, ticket, me);
		now = venture_time_now();

		comment = venture_ticket_comment_new();
		g_object_set(comment,
		             "ticket-id", venture_entity_get_id(ticket),
		             "body", text,
		             "author", me,
		             "internal", internal,
		             "occurred-at", now,
		             NULL);
		venture_entity_set_organization_id(VENTURE_ENTITY(comment),
			venture_entity_get_organization_id(ticket));

		if (!venture_database_save(database, VENTURE_ENTITY(comment), actor,
		                           error))
		{
			venture_database_rollback(database);
			return FALSE;
		}
	}

	return venture_database_commit(database, error);
}

GPtrArray *
venture_desk_list_macros(VentureContext *context)
{
	g_autoptr(VentureQuery) query = NULL;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);

	if (!venture_context_module_enabled(context, "tickets"))
		return g_ptr_array_new_with_free_func(g_object_unref);

	query = venture_query_new(VENTURE_TYPE_MACRO);
	venture_query_add_filter_string(query, "active", VENTURE_FILTER_OP_EQ,
	                                "true", NULL);
	venture_query_add_order(query, "position", VENTURE_SORT_ASCENDING, NULL);
	venture_query_add_order(query, "name", VENTURE_SORT_ASCENDING, NULL);
	venture_query_set_limit(query, 0);

	return venture_database_find(venture_context_get_database(context), query,
	                             NULL);
}

VentureEntity *
venture_desk_find_macro(
	VentureContext	*context,
	const gchar	*name_or_id
){
	g_autoptr(VentureQuery) query = NULL;
	gchar *end;
	gint64 id;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);

	if (venture_string_is_empty(name_or_id))
		return NULL;

	id = g_ascii_strtoll(name_or_id, &end, 10);

	if ((id > 0) && ('\0' == *end))
		return venture_database_get(venture_context_get_database(context),
		                            VENTURE_TYPE_MACRO, id, NULL);

	query = venture_query_new(VENTURE_TYPE_MACRO);

	if (!venture_query_add_filter_string(query, "name", VENTURE_FILTER_OP_EQ,
	                                     name_or_id, NULL))
		return NULL;

	venture_query_set_limit(query, 1);

	return venture_database_find_one(venture_context_get_database(context),
	                                 query, NULL);
}

/* --- Worklogs -------------------------------------------------------------- */

gdouble
venture_desk_logged_hours(
	VentureDatabase	*database,
	gint64		 ticket_id
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	gdouble total;
	guint i;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), 0.0);

	query = venture_query_new(VENTURE_TYPE_WORKLOG);

	if (!venture_query_add_filter_int(query, "ticket-id", VENTURE_FILTER_OP_EQ,
	                                  ticket_id, NULL))
		return 0.0;

	venture_query_set_limit(query, 0);
	rows = venture_database_find(database, query, NULL);

	if (NULL == rows)
		return 0.0;

	total = 0.0;

	for (i = 0; i < rows->len; i++)
	{
		gdouble hours = 0.0;

		g_object_get(g_ptr_array_index(rows, i), "hours", &hours, NULL);
		total += hours;
	}

	return total;
}

VentureEntity *
venture_desk_log_work(
	VentureContext		 *context,
	gint64			  ticket_id,
	gdouble			  hours,
	const gchar		 *note,
	const VentureActor	 *actor,
	GError			**error
){
	g_autoptr(VentureEntity) ticket = NULL;
	g_autoptr(VentureWorklog) worklog = NULL;
	g_autoptr(GDateTime) now = NULL;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);

	if (!(hours > 0.0))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		                    "Logged time must be more than zero hours");
		return NULL;
	}

	ticket = venture_database_get(venture_context_get_database(context),
	                              VENTURE_TYPE_TICKET, ticket_id, error);

	if (NULL == ticket)
		return NULL;

	now = venture_time_now();

	worklog = venture_worklog_new();
	g_object_set(worklog,
	             "ticket-id", ticket_id,
	             "author", (NULL != actor) ? actor->name : NULL,
	             "hours", hours,
	             "occurred-at", now,
	             "note", note,
	             NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(worklog),
		venture_entity_get_organization_id(ticket));

	if (!venture_database_save(venture_context_get_database(context),
	                           VENTURE_ENTITY(worklog), actor, error))
		return NULL;

	return VENTURE_ENTITY(g_steal_pointer(&worklog));
}

/*
 * A worklog written or removed re-sums its ticket. The sum is written
 * as the system: the audit trail already has the worklog itself, and a
 * derived figure re-attributed to the person would double their entry.
 */
static void
venture_desk_on_worklog_changed(
	VentureDatabase	*database,
	VentureEntity	*entity,
	gpointer	 user_data
){
	g_autoptr(VentureEntity) ticket = NULL;
	gint64 ticket_id = 0;
	gdouble total;
	gdouble stored = 0.0;

	(void)user_data;

	if (!VENTURE_IS_WORKLOG(entity))
		return;

	g_object_get(entity, "ticket-id", &ticket_id, NULL);

	if (0 == ticket_id)
		return;

	ticket = venture_database_get(database, VENTURE_TYPE_TICKET, ticket_id,
	                              NULL);

	if (NULL == ticket)
		return;

	total = venture_desk_logged_hours(database, ticket_id);
	g_object_get(ticket, "logged-hours", &stored, NULL);

	if (total == stored)
		return;

	g_object_set(ticket, "logged-hours", total, NULL);
	venture_database_save(database, ticket, NULL, NULL);
}

static void
venture_desk_on_entity_saved(
	VentureDatabase	*database,
	VentureEntity	*entity,
	gboolean	 created,
	gpointer	 user_data
){
	(void)created;
	venture_desk_on_worklog_changed(database, entity, user_data);
}

void
venture_desk_install(VentureContext *context)
{
	VentureDatabase *database;

	g_return_if_fail(VENTURE_IS_CONTEXT(context));

	database = venture_context_get_database(context);
	g_signal_connect(database, "entity-saved",
	                 G_CALLBACK(venture_desk_on_entity_saved), context);
	g_signal_connect(database, "entity-deleted",
	                 G_CALLBACK(venture_desk_on_worklog_changed), context);
}

/* --- Sprints --------------------------------------------------------------- */

static GPtrArray *
venture_desk_sprint_tickets(
	VentureContext	*context,
	gint64		 sprint_id
){
	g_autoptr(VentureQuery) query = NULL;

	query = venture_query_new(VENTURE_TYPE_TICKET);

	if (!venture_query_add_filter_int(query, "sprint-id", VENTURE_FILTER_OP_EQ,
	                                  sprint_id, NULL))
		return NULL;

	venture_query_add_order(query, "status", VENTURE_SORT_ASCENDING, NULL);
	venture_query_add_order(query, "board-order", VENTURE_SORT_ASCENDING, NULL);
	venture_query_set_limit(query, 0);

	return venture_database_find(venture_context_get_database(context), query,
	                             NULL);
}

void
venture_desk_sprint_progress(
	VentureContext		*context,
	VentureEntity		*sprint,
	VentureSprintProgress	*out
){
	g_autoptr(GPtrArray) tickets = NULL;
	g_autoptr(GDateTime) starts = NULL;
	g_autoptr(GDateTime) ends = NULL;
	g_autoptr(GDateTime) now = NULL;
	guint i;

	g_return_if_fail(VENTURE_IS_CONTEXT(context));
	g_return_if_fail(VENTURE_IS_SPRINT(sprint));
	g_return_if_fail(NULL != out);

	memset(out, 0, sizeof(*out));

	g_object_get(sprint, "capacity-points", &out->capacity,
	             "starts-on", &starts, "ends-on", &ends, NULL);

	if ((NULL != starts) && (NULL != ends))
	{
		now = venture_time_now();
		out->days_total = g_date_time_difference(ends, starts) / G_TIME_SPAN_DAY;
		out->days_left = g_date_time_difference(ends, now) / G_TIME_SPAN_DAY;

		if (g_date_time_difference(ends, now) < 0)
			out->days_left = -((g_date_time_difference(now, ends)
			                    + G_TIME_SPAN_DAY - 1) / G_TIME_SPAN_DAY);
	}

	tickets = venture_desk_sprint_tickets(context, venture_entity_get_id(sprint));

	if (NULL == tickets)
		return;

	for (i = 0; i < tickets->len; i++)
	{
		VentureTicketStatus status;
		gint64 points = 0;
		gboolean finished;

		g_object_get(g_ptr_array_index(tickets, i), "status", &status,
		             "story-points", &points, NULL);
		finished = (VENTURE_TICKET_STATUS_DONE == status) ||
		           (VENTURE_TICKET_STATUS_CANCELLED == status);

		out->tickets++;
		out->points += points;

		if (finished)
		{
			out->done++;
			out->points_done += points;
		}
	}
}

JsonNode *
venture_desk_sprint_to_json(
	VentureContext	*context,
	VentureEntity	*sprint,
	gboolean	 with_tickets
){
	g_autoptr(JsonBuilder) builder = NULL;
	g_autofree gchar *name = NULL;
	g_autofree gchar *goal = NULL;
	g_autoptr(GDateTime) starts = NULL;
	g_autoptr(GDateTime) ends = NULL;
	VentureSprintStatus status;
	VentureSprintProgress progress;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);
	g_return_val_if_fail(VENTURE_IS_SPRINT(sprint), NULL);

	g_object_get(sprint, "name", &name, "goal", &goal, "status", &status,
	             "starts-on", &starts, "ends-on", &ends, NULL);
	venture_desk_sprint_progress(context, sprint, &progress);

	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "id");
	json_builder_add_int_value(builder, venture_entity_get_id(sprint));
	json_builder_set_member_name(builder, "name");
	json_builder_add_string_value(builder, name);
	json_builder_set_member_name(builder, "goal");

	if (NULL != goal)
		json_builder_add_string_value(builder, goal);
	else
		json_builder_add_null_value(builder);

	json_builder_set_member_name(builder, "status");
	json_builder_add_string_value(builder,
		venture_enum_to_nick(VENTURE_TYPE_SPRINT_STATUS, (gint)status));

	json_builder_set_member_name(builder, "starts_on");

	if (NULL != starts)
	{
		g_autofree gchar *text = NULL;

		text = venture_time_to_date_string(starts, NULL);
		json_builder_add_string_value(builder, text);
	}
	else
	{
		json_builder_add_null_value(builder);
	}

	json_builder_set_member_name(builder, "ends_on");

	if (NULL != ends)
	{
		g_autofree gchar *text = NULL;

		text = venture_time_to_date_string(ends, NULL);
		json_builder_add_string_value(builder, text);
	}
	else
	{
		json_builder_add_null_value(builder);
	}

	json_builder_set_member_name(builder, "tickets");
	json_builder_add_int_value(builder, progress.tickets);
	json_builder_set_member_name(builder, "done");
	json_builder_add_int_value(builder, progress.done);
	json_builder_set_member_name(builder, "points");
	json_builder_add_int_value(builder, progress.points);
	json_builder_set_member_name(builder, "points_done");
	json_builder_add_int_value(builder, progress.points_done);
	json_builder_set_member_name(builder, "points_remaining");
	json_builder_add_int_value(builder, progress.points - progress.points_done);
	json_builder_set_member_name(builder, "capacity");
	json_builder_add_int_value(builder, progress.capacity);
	json_builder_set_member_name(builder, "percent");
	json_builder_add_int_value(builder,
		(progress.points > 0)
			? (progress.points_done * 100) / progress.points
			: (progress.tickets > 0)
				? (progress.done * 100) / progress.tickets : 0);
	json_builder_set_member_name(builder, "days_total");
	json_builder_add_int_value(builder, progress.days_total);
	json_builder_set_member_name(builder, "days_left");
	json_builder_add_int_value(builder, progress.days_left);

	if (with_tickets)
	{
		g_autoptr(GPtrArray) tickets = NULL;
		guint i;

		tickets = venture_desk_sprint_tickets(context,
		                                      venture_entity_get_id(sprint));

		json_builder_set_member_name(builder, "items");
		json_builder_begin_array(builder);

		for (i = 0; (NULL != tickets) && (i < tickets->len); i++)
		{
			VentureEntity *ticket;
			g_autofree gchar *title = NULL;
			g_autofree gchar *assignee = NULL;
			VentureTicketStatus ticket_status;
			VenturePriority priority;
			gint64 points = 0;

			ticket = g_ptr_array_index(tickets, i);
			g_object_get(ticket, "title", &title, "assignee", &assignee,
			             "status", &ticket_status, "priority", &priority,
			             "story-points", &points, NULL);

			json_builder_begin_object(builder);
			json_builder_set_member_name(builder, "id");
			json_builder_add_int_value(builder, venture_entity_get_id(ticket));
			json_builder_set_member_name(builder, "title");
			json_builder_add_string_value(builder, title);
			json_builder_set_member_name(builder, "status");
			json_builder_add_string_value(builder,
				venture_enum_to_nick(VENTURE_TYPE_TICKET_STATUS,
				                     (gint)ticket_status));
			json_builder_set_member_name(builder, "priority");
			json_builder_add_string_value(builder,
				venture_enum_to_nick(VENTURE_TYPE_PRIORITY, (gint)priority));
			json_builder_set_member_name(builder, "assignee");

			if (NULL != assignee)
				json_builder_add_string_value(builder, assignee);
			else
				json_builder_add_null_value(builder);

			json_builder_set_member_name(builder, "points");
			json_builder_add_int_value(builder, points);
			json_builder_end_object(builder);
		}

		json_builder_end_array(builder);
	}

	json_builder_end_object(builder);

	return json_builder_get_root(builder);
}

/*
 * Active first, then planned, then completed -- and within each, the
 * soonest end first for the live ones and the latest for the finished.
 */
static gint
venture_desk_compare_sprints(
	gconstpointer	a,
	gconstpointer	b
){
	VentureEntity *left;
	VentureEntity *right;
	VentureSprintStatus left_status;
	VentureSprintStatus right_status;
	g_autoptr(GDateTime) left_ends = NULL;
	g_autoptr(GDateTime) right_ends = NULL;
	gint left_rank;
	gint right_rank;

	left = *(VentureEntity *const *)a;
	right = *(VentureEntity *const *)b;

	g_object_get(left, "status", &left_status, "ends-on", &left_ends, NULL);
	g_object_get(right, "status", &right_status, "ends-on", &right_ends, NULL);

	left_rank = (VENTURE_SPRINT_STATUS_ACTIVE == left_status) ? 0
	          : (VENTURE_SPRINT_STATUS_PLANNED == left_status) ? 1 : 2;
	right_rank = (VENTURE_SPRINT_STATUS_ACTIVE == right_status) ? 0
	           : (VENTURE_SPRINT_STATUS_PLANNED == right_status) ? 1 : 2;

	if (left_rank != right_rank)
		return left_rank - right_rank;

	if ((NULL == left_ends) || (NULL == right_ends))
		return (NULL == left_ends) - (NULL == right_ends);

	if (2 == left_rank)
		return g_date_time_compare(right_ends, left_ends);

	return g_date_time_compare(left_ends, right_ends);
}

GPtrArray *
venture_desk_list_sprints(
	VentureContext	 *context,
	const gint64	 *organization_ids,
	gsize		  n_organizations,
	GError		**error
){
	g_autoptr(VentureQuery) query = NULL;
	GPtrArray *sprints;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);

	query = venture_query_new(VENTURE_TYPE_SPRINT);
	venture_query_set_limit(query, 0);

	if ((NULL != organization_ids) && (n_organizations > 0))
		venture_query_set_organization_tree(query, organization_ids,
		                                    n_organizations);

	sprints = venture_database_find(venture_context_get_database(context),
	                                query, error);

	if (NULL == sprints)
		return NULL;

	g_ptr_array_sort(sprints, venture_desk_compare_sprints);

	return sprints;
}

/* --- Bulk edits ------------------------------------------------------------ */

gint
venture_desk_bulk_update(
	VentureContext		 *context,
	GType			  entity_type,
	const gint64		 *ids,
	gsize			  n_ids,
	JsonObject		 *changes,
	const VentureActor	 *actor,
	GError			**error
){
	VentureDatabase *database;
	g_autoptr(GList) members = NULL;
	gsize i;
	gint changed;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), -1);
	g_return_val_if_fail(NULL != changes, -1);

	if (0 == n_ids)
	{
		g_set_error_literal(error, VENTURE_ERROR,
		                    VENTURE_ERROR_INVALID_ARGUMENT,
		                    "No records were named");
		return -1;
	}

	if (n_ids > 500)
	{
		g_set_error_literal(error, VENTURE_ERROR,
		                    VENTURE_ERROR_INVALID_ARGUMENT,
		                    "At most 500 records can be changed at once");
		return -1;
	}

	members = json_object_get_members(changes);

	if (NULL == members)
	{
		g_set_error_literal(error, VENTURE_ERROR,
		                    VENTURE_ERROR_INVALID_ARGUMENT,
		                    "No changes were given");
		return -1;
	}

	database = venture_context_get_database(context);

	if (!venture_database_begin(database, error))
		return -1;

	changed = 0;

	for (i = 0; i < n_ids; i++)
	{
		g_autoptr(VentureEntity) record = NULL;
		GList *cursor;

		record = venture_database_get(database, entity_type, ids[i], error);

		if (NULL == record)
		{
			if ((NULL != error) && (NULL == *error))
				g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
				            "There is no %s %" G_GINT64_FORMAT,
				            g_type_name(entity_type), ids[i]);

			venture_database_rollback(database);
			return -1;
		}

		for (cursor = members; NULL != cursor; cursor = cursor->next)
		{
			g_autofree gchar *text = NULL;
			g_autofree gchar *property = NULL;
			const gchar *member;

			member = cursor->data;
			text = venture_desk_node_to_text(
				json_object_get_member(changes, member));

			/* The wire spelling is underscores; the property's is
			 * dashes. Either is accepted here, as in a form. */
			property = g_strdelimit(g_strdup(member), "_", '-');

			if (!venture_entity_set_field_from_string(record, property,
			                                          (NULL != text) ? text
			                                                         : "",
			                                          error))
			{
				g_prefix_error(error, "%s %" G_GINT64_FORMAT ": ",
				               venture_entity_get_entity_name(record),
				               ids[i]);
				venture_database_rollback(database);
				return -1;
			}
		}

		if (!venture_database_save(database, record, actor, error))
		{
			g_prefix_error(error, "%s %" G_GINT64_FORMAT ": ",
			               venture_entity_get_entity_name(record), ids[i]);
			venture_database_rollback(database);
			return -1;
		}

		changed++;
	}

	if (!venture_database_commit(database, error))
		return -1;

	return changed;
}

gint
venture_desk_bulk_delete(
	VentureContext		 *context,
	GType			  entity_type,
	const gint64		 *ids,
	gsize			  n_ids,
	const VentureActor	 *actor,
	GError			**error
){
	VentureDatabase *database;
	gsize i;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), -1);

	if (0 == n_ids)
	{
		g_set_error_literal(error, VENTURE_ERROR,
		                    VENTURE_ERROR_INVALID_ARGUMENT,
		                    "No records were named");
		return -1;
	}

	if (n_ids > 500)
	{
		g_set_error_literal(error, VENTURE_ERROR,
		                    VENTURE_ERROR_INVALID_ARGUMENT,
		                    "At most 500 records can be removed at once");
		return -1;
	}

	database = venture_context_get_database(context);

	if (!venture_database_begin(database, error))
		return -1;

	for (i = 0; i < n_ids; i++)
	{
		g_autoptr(VentureEntity) record = NULL;

		record = venture_database_get(database, entity_type, ids[i], error);

		if (NULL == record)
		{
			if ((NULL != error) && (NULL == *error))
				g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
				            "There is no %s %" G_GINT64_FORMAT,
				            g_type_name(entity_type), ids[i]);

			venture_database_rollback(database);
			return -1;
		}

		if (!venture_database_delete(database, record, actor, error))
		{
			venture_database_rollback(database);
			return -1;
		}
	}

	if (!venture_database_commit(database, error))
		return -1;

	return (gint)n_ids;
}

/* --- The timeline ---------------------------------------------------------- */

typedef struct
{
	GDateTime	*when;
	JsonNode	*node;
} VentureDeskEvent;

static void
venture_desk_event_free(gpointer data)
{
	VentureDeskEvent *event;

	event = data;

	if (NULL == event)
		return;

	g_clear_pointer(&event->when, g_date_time_unref);
	g_clear_pointer(&event->node, json_node_unref);
	g_free(event);
}

static gint
venture_desk_compare_events(
	gconstpointer	a,
	gconstpointer	b
){
	const VentureDeskEvent *left;
	const VentureDeskEvent *right;

	left = *(const VentureDeskEvent *const *)a;
	right = *(const VentureDeskEvent *const *)b;

	if ((NULL == left->when) || (NULL == right->when))
		return (NULL == right->when) - (NULL == left->when);

	return g_date_time_compare(right->when, left->when);
}

static void
venture_desk_add_event(
	GPtrArray	*events,
	GDateTime	*when,
	JsonNode	*node
){
	VentureDeskEvent *event;

	event = g_new0(VentureDeskEvent, 1);
	event->when = (NULL != when) ? g_date_time_ref(when) : NULL;
	event->node = node;
	g_ptr_array_add(events, event);
}

static void
venture_desk_builder_add_optional(
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

static void
venture_desk_builder_add_when(
	JsonBuilder	*builder,
	GDateTime	*when
){
	g_autofree gchar *text = NULL;
	g_autofree gchar *relative = NULL;

	text = (NULL != when) ? venture_time_to_string(when) : NULL;
	relative = (NULL != when) ? venture_time_to_relative_string(when) : NULL;
	venture_desk_builder_add_optional(builder, "when", text);
	venture_desk_builder_add_optional(builder, "when_relative", relative);
}

JsonNode *
venture_desk_activity(
	VentureContext	 *context,
	const gchar	 *target_type,
	gint64		  target_id,
	guint		  limit,
	GError		**error
){
	VentureDatabase *database;
	g_autoptr(GPtrArray) events = NULL;
	g_autoptr(JsonBuilder) out = NULL;
	guint i;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);
	g_return_val_if_fail(NULL != target_type, NULL);

	database = venture_context_get_database(context);
	events = g_ptr_array_new_with_free_func(venture_desk_event_free);

	if (0 == limit)
		limit = 100;

	/* The audited changes. */
	{
		g_autoptr(VentureQuery) query = NULL;
		g_autoptr(GPtrArray) entries = NULL;

		query = venture_query_new(VENTURE_TYPE_AUDIT_ENTRY);

		if (!venture_query_add_filter_string(query, "target-type",
		                                     VENTURE_FILTER_OP_EQ,
		                                     target_type, error) ||
		    !venture_query_add_filter_int(query, "target-id",
		                                  VENTURE_FILTER_OP_EQ, target_id,
		                                  error))
			return NULL;

		venture_query_add_order(query, "id", VENTURE_SORT_DESCENDING, NULL);
		venture_query_set_limit(query, limit);
		entries = venture_database_find(database, query, error);

		if (NULL == entries)
			return NULL;

		for (i = 0; i < entries->len; i++)
		{
			VentureEntity *entry;
			g_autoptr(JsonBuilder) builder = NULL;
			g_autofree gchar *actor = NULL;
			g_autofree gchar *diff = NULL;
			g_autofree gchar *approved_by = NULL;
			g_autoptr(GDateTime) when = NULL;
			VentureAuditAction action;
			VentureActorKind actor_kind;

			entry = g_ptr_array_index(entries, i);
			g_object_get(entry, "actor", &actor, "action", &action,
			             "actor-kind", &actor_kind, "diff", &diff,
			             "approved-by", &approved_by,
			             "occurred-at", &when, NULL);

			builder = json_builder_new();
			json_builder_begin_object(builder);
			venture_desk_builder_add_optional(builder, "kind", "change");
			json_builder_set_member_name(builder, "id");
			json_builder_add_int_value(builder, venture_entity_get_id(entry));
			venture_desk_builder_add_optional(builder, "action",
				venture_enum_to_nick(VENTURE_TYPE_AUDIT_ACTION, (gint)action));
			venture_desk_builder_add_optional(builder, "actor", actor);
			venture_desk_builder_add_optional(builder, "actor_kind",
				venture_enum_to_nick(VENTURE_TYPE_ACTOR_KIND,
				                     (gint)actor_kind));
			venture_desk_builder_add_optional(builder, "approved_by",
			                                  approved_by);
			venture_desk_builder_add_when(builder, when);

			/* The diff as it is, so a client can render "from -> to"
			 * per field; a summary line would lose the values. */
			json_builder_set_member_name(builder, "changes");

			if (!venture_string_is_empty(diff))
			{
				g_autoptr(JsonNode) parsed = NULL;

				parsed = venture_json_parse(diff, NULL);

				if (NULL != parsed)
					json_builder_add_value(builder, g_steal_pointer(&parsed));
				else
					json_builder_add_null_value(builder);
			}
			else
			{
				json_builder_add_null_value(builder);
			}

			json_builder_end_object(builder);
			venture_desk_add_event(events, when, json_builder_get_root(builder));
		}
	}

	/* A ticket's conversation and its time, woven in. */
	if ((0 == g_strcmp0(target_type, "ticket")) &&
	    venture_context_module_enabled(context, "tickets"))
	{
		g_autoptr(VentureQuery) query = NULL;
		g_autoptr(GPtrArray) comments = NULL;
		g_autoptr(VentureQuery) work_query = NULL;
		g_autoptr(GPtrArray) worklogs = NULL;

		query = venture_query_new(VENTURE_TYPE_TICKET_COMMENT);
		venture_query_add_filter_int(query, "ticket-id", VENTURE_FILTER_OP_EQ,
		                             target_id, NULL);
		venture_query_add_order(query, "id", VENTURE_SORT_DESCENDING, NULL);
		venture_query_set_limit(query, limit);
		comments = venture_database_find(database, query, NULL);

		for (i = 0; (NULL != comments) && (i < comments->len); i++)
		{
			VentureEntity *comment;
			g_autoptr(JsonBuilder) builder = NULL;
			g_autofree gchar *author = NULL;
			g_autofree gchar *body = NULL;
			g_autoptr(GDateTime) when = NULL;
			gboolean internal = FALSE;

			comment = g_ptr_array_index(comments, i);
			g_object_get(comment, "author", &author, "body", &body,
			             "internal", &internal, "occurred-at", &when, NULL);

			if (NULL == when)
				when = g_date_time_ref(venture_entity_get_created_at(comment));

			builder = json_builder_new();
			json_builder_begin_object(builder);
			venture_desk_builder_add_optional(builder, "kind", "comment");
			json_builder_set_member_name(builder, "id");
			json_builder_add_int_value(builder, venture_entity_get_id(comment));
			venture_desk_builder_add_optional(builder, "actor", author);
			venture_desk_builder_add_optional(builder, "body", body);
			json_builder_set_member_name(builder, "internal");
			json_builder_add_boolean_value(builder, internal);
			venture_desk_builder_add_when(builder, when);
			json_builder_end_object(builder);
			venture_desk_add_event(events, when, json_builder_get_root(builder));
		}

		work_query = venture_query_new(VENTURE_TYPE_WORKLOG);
		venture_query_add_filter_int(work_query, "ticket-id",
		                             VENTURE_FILTER_OP_EQ, target_id, NULL);
		venture_query_add_order(work_query, "id", VENTURE_SORT_DESCENDING, NULL);
		venture_query_set_limit(work_query, limit);
		worklogs = venture_database_find(database, work_query, NULL);

		for (i = 0; (NULL != worklogs) && (i < worklogs->len); i++)
		{
			VentureEntity *worklog;
			g_autoptr(JsonBuilder) builder = NULL;
			g_autofree gchar *author = NULL;
			g_autofree gchar *note = NULL;
			g_autoptr(GDateTime) when = NULL;
			gdouble hours = 0.0;

			worklog = g_ptr_array_index(worklogs, i);
			g_object_get(worklog, "author", &author, "note", &note,
			             "hours", &hours, "occurred-at", &when, NULL);

			if (NULL == when)
				when = g_date_time_ref(venture_entity_get_created_at(worklog));

			builder = json_builder_new();
			json_builder_begin_object(builder);
			venture_desk_builder_add_optional(builder, "kind", "worklog");
			json_builder_set_member_name(builder, "id");
			json_builder_add_int_value(builder, venture_entity_get_id(worklog));
			venture_desk_builder_add_optional(builder, "actor", author);
			venture_desk_builder_add_optional(builder, "body", note);
			json_builder_set_member_name(builder, "hours");
			json_builder_add_double_value(builder, hours);
			venture_desk_builder_add_when(builder, when);
			json_builder_end_object(builder);
			venture_desk_add_event(events, when, json_builder_get_root(builder));
		}
	}

	g_ptr_array_sort(events, venture_desk_compare_events);

	out = json_builder_new();
	json_builder_begin_array(out);

	for (i = 0; (i < events->len) && (i < limit); i++)
	{
		VentureDeskEvent *event;

		event = g_ptr_array_index(events, i);
		json_builder_add_value(out, json_node_ref(event->node));
	}

	json_builder_end_array(out);

	return json_builder_get_root(out);
}
