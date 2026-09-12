/*
 * venture-sla.c - Service levels on tickets
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "venture.h"

/*
 * Warning starts inside the last fifth of a target. A fixed fraction
 * rather than a fixed number of minutes, because a one-hour and a
 * two-week target want very different warnings.
 */
#define VENTURE_SLA_WARNING_FRACTION (5)

VentureEntity *
venture_sla_find_policy(
	VentureContext		*context,
	VentureTicketKind	 kind,
	VenturePriority		 priority
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) policies = NULL;
	VentureEntity *best;
	gint best_score;
	guint i;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);

	if (!venture_context_module_enabled(context, "tickets"))
		return NULL;

	query = venture_query_new(VENTURE_TYPE_SLA_POLICY);
	venture_query_add_filter_string(query, "active", VENTURE_FILTER_OP_EQ,
	                                "true", NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	venture_query_set_limit(query, 0);
	policies = venture_database_find(venture_context_get_database(context),
	                                 query, NULL);

	if (NULL == policies)
		return NULL;

	best = NULL;
	best_score = -1;

	for (i = 0; i < policies->len; i++)
	{
		VentureEntity *policy;
		VentureTicketKind policy_kind;
		VenturePriority policy_priority;
		gboolean all_kinds = FALSE;
		gboolean all_priorities = FALSE;
		gint score;

		policy = g_ptr_array_index(policies, i);
		g_object_get(policy,
		             "kind", &policy_kind, "all-kinds", &all_kinds,
		             "priority", &policy_priority,
		             "all-priorities", &all_priorities, NULL);

		if (!all_kinds && (policy_kind != kind))
			continue;

		if (!all_priorities && (policy_priority != priority))
			continue;

		/* Two points for naming both, one for either, none for a
		 * catch-all; the first of equals wins because the list is by
		 * id. */
		score = (all_kinds ? 0 : 1) + (all_priorities ? 0 : 1);

		if (score > best_score)
		{
			best = policy;
			best_score = score;
		}
	}

	return (NULL != best) ? g_object_ref(best) : NULL;
}

/*
 * The state of one clock: none without a due time, met once answered,
 * then ok, warning or breached by how much of the target is left.
 */
static VentureSlaState
venture_sla_clock(
	GDateTime	*due,
	GDateTime	*from,
	GDateTime	*now,
	gint64		*out_remaining
){
	GTimeSpan remaining;
	GTimeSpan total;

	*out_remaining = 0;

	if (NULL == due)
		return VENTURE_SLA_STATE_NONE;

	remaining = g_date_time_difference(due, now);
	*out_remaining = remaining / G_TIME_SPAN_SECOND;

	if (remaining < 0)
		return VENTURE_SLA_STATE_BREACHED;

	total = (NULL != from) ? g_date_time_difference(due, from) : 0;

	if ((total > 0) && (remaining * VENTURE_SLA_WARNING_FRACTION <= total))
		return VENTURE_SLA_STATE_WARNING;

	return VENTURE_SLA_STATE_OK;
}

void
venture_sla_status(
	VentureEntity		*ticket,
	GDateTime		*now,
	VentureSlaStatus	*out
){
	g_autoptr(GDateTime) local_now = NULL;
	g_autoptr(GDateTime) respond_by = NULL;
	g_autoptr(GDateTime) resolve_by = NULL;
	g_autoptr(GDateTime) responded_at = NULL;
	g_autoptr(GDateTime) resolved_at = NULL;
	GDateTime *created;
	VentureTicketStatus status;

	g_return_if_fail(VENTURE_IS_TICKET(ticket));
	g_return_if_fail(NULL != out);

	out->has_policy = FALSE;
	out->first_response = VENTURE_SLA_STATE_NONE;
	out->resolution = VENTURE_SLA_STATE_NONE;
	out->first_response_remaining = 0;
	out->resolution_remaining = 0;
	out->responded = FALSE;
	out->closed = FALSE;

	if (NULL == now)
	{
		local_now = venture_time_now();
		now = local_now;
	}

	g_object_get(ticket,
	             "first-response-due-at", &respond_by,
	             "resolution-due-at", &resolve_by,
	             "first-responded-at", &responded_at,
	             "resolved-at", &resolved_at,
	             "status", &status, NULL);
	created = venture_entity_get_created_at(ticket);

	out->has_policy = (NULL != respond_by) || (NULL != resolve_by);
	out->responded = (NULL != responded_at);
	out->closed = (VENTURE_TICKET_STATUS_DONE == status) ||
	              (VENTURE_TICKET_STATUS_CANCELLED == status);

	/* A clock that was met stops where it was met: a reply an hour early
	 * is not "an hour left" forever. */
	if (out->responded)
	{
		if (NULL != respond_by)
			out->first_response = (g_date_time_compare(responded_at,
			                                           respond_by) <= 0)
				? VENTURE_SLA_STATE_OK : VENTURE_SLA_STATE_BREACHED;
	}
	else
	{
		out->first_response = venture_sla_clock(respond_by, created, now,
			&out->first_response_remaining);
	}

	if (out->closed)
	{
		if (NULL != resolve_by)
		{
			GDateTime *finished;

			finished = (NULL != resolved_at) ? resolved_at
			                                 : venture_entity_get_updated_at(ticket);
			out->resolution = ((NULL == finished) ||
			                   (g_date_time_compare(finished, resolve_by) <= 0))
				? VENTURE_SLA_STATE_OK : VENTURE_SLA_STATE_BREACHED;
		}
	}
	else
	{
		out->resolution = venture_sla_clock(resolve_by, created, now,
			&out->resolution_remaining);
	}
}

JsonNode *
venture_sla_status_to_json(const VentureSlaStatus *status)
{
	g_autoptr(JsonBuilder) builder = NULL;

	g_return_val_if_fail(NULL != status, NULL);

	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "has_policy");
	json_builder_add_boolean_value(builder, status->has_policy);
	json_builder_set_member_name(builder, "first_response");
	json_builder_add_string_value(builder,
		venture_enum_to_nick(VENTURE_TYPE_SLA_STATE,
		                     (gint)status->first_response));
	json_builder_set_member_name(builder, "first_response_remaining_seconds");
	json_builder_add_int_value(builder, status->first_response_remaining);
	json_builder_set_member_name(builder, "resolution");
	json_builder_add_string_value(builder,
		venture_enum_to_nick(VENTURE_TYPE_SLA_STATE,
		                     (gint)status->resolution));
	json_builder_set_member_name(builder, "resolution_remaining_seconds");
	json_builder_add_int_value(builder, status->resolution_remaining);
	json_builder_set_member_name(builder, "responded");
	json_builder_add_boolean_value(builder, status->responded);
	json_builder_set_member_name(builder, "closed");
	json_builder_add_boolean_value(builder, status->closed);
	json_builder_end_object(builder);

	return json_builder_get_root(builder);
}

/* --- The hooks ------------------------------------------------------------- */

/*
 * Stamps the due times on a ticket the first time it is saved under a
 * policy, and the resolved time when it closes. A validator rather than
 * before_save because it needs the context to find the policy, and
 * because it runs for every writer.
 */
static gboolean
venture_sla_validate_ticket(
	VentureDatabase	 *database,
	VentureEntity	 *entity,
	VentureEntity	 *previous,
	gpointer	  user_data,
	GError		**error
){
	VentureContext *context;
	g_autoptr(GDateTime) respond_by = NULL;
	g_autoptr(GDateTime) resolve_by = NULL;
	g_autoptr(GDateTime) resolved_at = NULL;
	VentureTicketStatus status;
	VentureTicketKind kind;
	VenturePriority priority;

	(void)database;
	(void)error;
	context = user_data;

	g_object_get(entity,
	             "first-response-due-at", &respond_by,
	             "resolution-due-at", &resolve_by,
	             "resolved-at", &resolved_at,
	             "status", &status, "kind", &kind, "priority", &priority,
	             NULL);

	/* Closing stamps the resolved time, once. Reopening leaves it, so a
	 * ticket closed and reopened keeps saying when it was first closed;
	 * the status says it is open again. */
	if (((VENTURE_TICKET_STATUS_DONE == status) ||
	     (VENTURE_TICKET_STATUS_CANCELLED == status)) &&
	    (NULL == resolved_at))
	{
		g_autoptr(GDateTime) now = NULL;
		VentureTicketStatus was = VENTURE_TICKET_STATUS_TRIAGE;

		if (NULL != previous)
			g_object_get(previous, "status", &was, NULL);

		if ((NULL == previous) ||
		    ((VENTURE_TICKET_STATUS_DONE != was) &&
		     (VENTURE_TICKET_STATUS_CANCELLED != was)))
		{
			now = venture_time_now();
			g_object_set(entity, "resolved-at", now, NULL);
		}
	}

	/* The clocks are set once. A ticket that already has either is left
	 * alone: a deadline that moved with every edit would be no deadline. */
	if ((NULL != respond_by) || (NULL != resolve_by))
		return TRUE;

	/* And only on the first save: a policy added later does not reach
	 * back and put a deadline on last month's tickets. */
	if (NULL != previous)
		return TRUE;

	{
		g_autoptr(VentureEntity) policy = NULL;
		g_autoptr(GDateTime) now = NULL;
		gdouble response_hours = 0.0;
		gdouble resolution_hours = 0.0;

		policy = venture_sla_find_policy(context, kind, priority);

		if (NULL == policy)
			return TRUE;

		g_object_get(policy,
		             "first-response-hours", &response_hours,
		             "resolution-hours", &resolution_hours, NULL);

		now = venture_time_now();

		if (response_hours > 0.0)
		{
			g_autoptr(GDateTime) due = NULL;

			due = g_date_time_add_seconds(now, response_hours * 3600.0);
			g_object_set(entity, "first-response-due-at", due, NULL);
		}

		if (resolution_hours > 0.0)
		{
			g_autoptr(GDateTime) due = NULL;

			due = g_date_time_add_seconds(now, resolution_hours * 3600.0);
			g_object_set(entity, "resolution-due-at", due, NULL);
		}
	}

	return TRUE;
}

/*
 * The first reply whoever raised the ticket can read stamps the ticket.
 * An internal note does not count -- they cannot see it -- and neither
 * does a comment by the person who raised it, which is them chasing.
 */
static void
venture_sla_on_comment_saved(
	VentureDatabase	*database,
	VentureEntity	*entity,
	gboolean	 created,
	gpointer	 user_data
){
	VentureContext *context;
	g_autoptr(VentureEntity) ticket = NULL;
	g_autoptr(VentureEntity) contact = NULL;
	g_autoptr(GDateTime) responded_at = NULL;
	g_autoptr(GDateTime) occurred_at = NULL;
	g_autofree gchar *author = NULL;
	gboolean internal = FALSE;
	gint64 ticket_id = 0;
	gint64 contact_id = 0;

	context = user_data;

	if (!created || !VENTURE_IS_TICKET_COMMENT(entity))
		return;

	g_object_get(entity, "ticket-id", &ticket_id, "internal", &internal,
	             "author", &author, "occurred-at", &occurred_at, NULL);

	if (internal || (0 == ticket_id))
		return;

	ticket = venture_database_get(database, VENTURE_TYPE_TICKET, ticket_id,
	                              NULL);

	if (NULL == ticket)
		return;

	g_object_get(ticket, "first-responded-at", &responded_at,
	             "contact-id", &contact_id, NULL);

	if (NULL != responded_at)
		return;

	/* The requester chasing is not a response. Their name is on the
	 * contact record, and the comment's author is whatever the door
	 * wrote, so this is a best-effort name match rather than a rule. */
	if ((0 != contact_id) && !venture_string_is_empty(author))
	{
		g_autofree gchar *name = NULL;
		g_autofree gchar *email = NULL;

		contact = venture_database_get(database, VENTURE_TYPE_CONTACT,
		                               contact_id, NULL);

		if (NULL != contact)
		{
			g_object_get(contact, "name", &name, "email", &email, NULL);

			if ((0 == g_strcmp0(name, author)) ||
			    (0 == g_strcmp0(email, author)))
				return;
		}
	}

	if (NULL == occurred_at)
		occurred_at = venture_time_now();

	g_object_set(ticket, "first-responded-at", occurred_at, NULL);
	venture_database_save(database, ticket, NULL, NULL);

	(void)context;
}

void
venture_sla_install(VentureContext *context)
{
	VentureDatabase *database;

	g_return_if_fail(VENTURE_IS_CONTEXT(context));

	database = venture_context_get_database(context);

	venture_database_add_save_validator(database, VENTURE_TYPE_TICKET,
	                                    venture_sla_validate_ticket,
	                                    context, NULL);
	g_signal_connect(database, "entity-saved",
	                 G_CALLBACK(venture_sla_on_comment_saved), context);
}

/* --- The sweep ------------------------------------------------------------- */

gint
venture_sla_sweep(
	VentureContext	 *context,
	guint		  limit,
	GError		**error
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) tickets = NULL;
	g_autoptr(GDateTime) now = NULL;
	g_autofree gchar *now_text = NULL;
	gint marked;
	guint i;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), -1);

	if (!venture_context_module_enabled(context, "tickets"))
		return 0;

	now = venture_time_now();
	now_text = venture_time_to_string(now);

	query = venture_query_new(VENTURE_TYPE_TICKET);

	if (!venture_query_add_filter_string(query, "sla-breached",
	                                     VENTURE_FILTER_OP_EQ, "false",
	                                     error) ||
	    !venture_query_add_filter_string(query, "resolution-due-at",
	                                     VENTURE_FILTER_OP_LT, now_text,
	                                     error) ||
	    !venture_query_add_filter_string(query, "status", VENTURE_FILTER_OP_NE,
	                                     "done", error) ||
	    !venture_query_add_filter_string(query, "status", VENTURE_FILTER_OP_NE,
	                                     "cancelled", error))
		return -1;

	venture_query_add_order(query, "resolution-due-at", VENTURE_SORT_ASCENDING,
	                        NULL);
	venture_query_set_limit(query, (0 == limit) ? 200 : limit);

	tickets = venture_database_find(venture_context_get_database(context),
	                                query, error);

	if (NULL == tickets)
		return -1;

	marked = 0;

	for (i = 0; i < tickets->len; i++)
	{
		VentureEntity *ticket;
		g_autofree gchar *label = NULL;
		g_autofree gchar *title = NULL;
		g_autofree gchar *assignee = NULL;
		g_autoptr(GArray) watchers = NULL;
		gint64 assignee_id;
		gint64 ticket_id;
		guint j;

		ticket = g_ptr_array_index(tickets, i);
		ticket_id = venture_entity_get_id(ticket);
		g_object_set(ticket, "sla-breached", TRUE, NULL);

		if (!venture_database_save(venture_context_get_database(context),
		                           ticket, NULL, error))
			return -1;

		marked++;

		label = venture_entity_get_display_name(ticket);
		title = g_strdup_printf("Service level missed: %s", label);
		g_object_get(ticket, "assignee", &assignee, NULL);
		assignee_id = venture_notify_user_id_for_username(context, assignee);

		if (0 != assignee_id)
			venture_notify_send(context, assignee_id,
			                    VENTURE_NOTIFICATION_KIND_SLA, title,
			                    "The resolution target has passed while "
			                    "the ticket is still open.",
			                    "ticket", ticket_id, label, NULL, NULL);

		watchers = venture_notify_list_watchers(context, "ticket", ticket_id);

		for (j = 0; j < watchers->len; j++)
		{
			gint64 user_id;

			user_id = g_array_index(watchers, gint64, j);

			if (user_id == assignee_id)
				continue;

			venture_notify_send(context, user_id,
			                    VENTURE_NOTIFICATION_KIND_SLA, title,
			                    "The resolution target has passed while "
			                    "the ticket is still open.",
			                    "ticket", ticket_id, label, NULL, NULL);
		}
	}

	return marked;
}
