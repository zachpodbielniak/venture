/*
 * venture-calendar-sources.c - The calendar's built-in sources
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Each source reads records its own module already keeps and turns them
 * into calendar items; none writes anything. Every read goes through a
 * VentureQuery scoped to the query's organizations and bounded by
 * venture_aggregate_get_max_rows(): a source that finds more than that is
 * refused with a note (venture_calendar_result_refuse()) and adds nothing,
 * because a calendar that drew the first twenty thousand would look
 * complete and not be. Time comparisons are filters on date-time fields,
 * which the query emitter normalises (six fraction digits); nothing here
 * compares stored time text itself.
 *
 * A source is registered with the record type it reads, and the
 * registry skips it while that type is hidden -- so the milestone source
 * follows the milestone type to whichever module owns it, and no module
 * name is spelled here except where a source needs a whole module (the
 * money calendar, which is many types).
 */

#include "venture.h"

#include <string.h>

/* ==========================================================================
 * Shared plumbing
 * ========================================================================== */

/* Narrows one query of a source to what the calendar was asked: status,
 * venture, assignee. Called on every query the source makes. */
typedef gboolean (*CalendarNarrowFunc)(
	VentureQuery			 *query,
	const VentureCalendarQuery	 *ask,
	gpointer			  data,
	GError				**error
);

static gint64
cal_int(VentureEntity *e, const gchar *field)
{
	gint64 value = 0;

	g_object_get(e, field, &value, NULL);

	return value;
}

static gint
cal_enum(VentureEntity *e, const gchar *field)
{
	gint value = 0;

	g_object_get(e, field, &value, NULL);

	return value;
}

static gchar *
cal_text(VentureEntity *e, const gchar *field)
{
	gchar *value = NULL;

	g_object_get(e, field, &value, NULL);

	return value;
}

static GDateTime *
cal_time(VentureEntity *e, const gchar *field)
{
	GDateTime *value = NULL;

	g_object_get(e, field, &value, NULL);

	return value;
}

/* An enum property's nick, as the API spells it. */
static const gchar *
cal_nick(VentureEntity *e, const gchar *field)
{
	GParamSpec *pspec;

	pspec = g_object_class_find_property(G_OBJECT_GET_CLASS(e), field);

	if ((NULL == pspec) || !G_IS_PARAM_SPEC_ENUM(pspec))
		return NULL;

	return venture_enum_to_nick(pspec->value_type, cal_enum(e, field));
}

static gint
cal_bound(void)
{
	return venture_aggregate_get_max_rows();
}

/* The instant overdue is judged against. */
static GDateTime *
cal_now(const VentureCalendarQuery *ask)
{
	return (NULL != ask->now) ? g_date_time_ref(ask->now) : venture_time_now();
}

/* A stable feed identifier for a record: its UUID, which no id reuse or
 * restore changes. */
static gchar *
cal_uid(VentureEntity *e)
{
	return g_strdup_printf("%s@venture", venture_entity_get_uuid(e));
}

/* A query of @type over the asked organizations, one past the bound, in
 * id order so a refusal's count is the same every time. */
static VentureQuery *
cal_query_new(
	GType				  type,
	const VentureCalendarQuery	 *ask,
	CalendarNarrowFunc		  narrow,
	gpointer			  data,
	GError				**error
){
	g_autoptr(VentureQuery) query = NULL;

	query = venture_query_new(type);
	venture_query_set_organization_tree(query, ask->organization_ids, ask->n_organizations);
	venture_query_set_limit(query, (guint)cal_bound() + 1);

	if (!venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, error))
		return NULL;

	if ((NULL != narrow) && !narrow(query, ask, data, error))
		return NULL;

	return g_steal_pointer(&query);
}

static gboolean
cal_filter_time(
	VentureQuery	 *query,
	const gchar	 *field,
	VentureFilterOp	  op,
	GDateTime	 *when,
	GError		**error
){
	g_autofree gchar *text = NULL;

	text = venture_time_to_string(when);

	return venture_query_add_filter_string(query, field, op, text, error);
}

/* Runs @query and appends its rows to @rows. */
static gboolean
cal_find_into(
	VentureContext	 *context,
	VentureQuery	 *query,
	GPtrArray	 *rows,
	GError		**error
){
	g_autoptr(GPtrArray) found = NULL;
	guint i;

	found = venture_database_find(venture_context_get_database(context), query, error);

	if (NULL == found)
		return FALSE;

	for (i = 0; i < found->len; i++)
		g_ptr_array_add(rows, g_object_ref(g_ptr_array_index(found, i)));

	return TRUE;
}

/*
 * The rows of @type dated in the asked span: those whose @start_field
 * falls inside it, and -- with an @end_field -- those that began before it
 * and are still running at its start. Two queries rather than one because
 * a filter cannot say "or"; they cannot overlap, so nothing is counted
 * twice. The caller compares the count with the bound.
 */
static gboolean
cal_span_rows(
	VentureContext			 *context,
	const VentureCalendarQuery	 *ask,
	GType				  type,
	const gchar			 *start_field,
	const gchar			 *end_field,
	CalendarNarrowFunc		  narrow,
	gpointer			  data,
	GPtrArray			 *rows,
	GError				**error
){
	g_autoptr(VentureQuery) inside = NULL;

	inside = cal_query_new(type, ask, narrow, data, error);

	if ((NULL == inside) ||
	    !cal_filter_time(inside, start_field, VENTURE_FILTER_OP_GTE, ask->from, error) ||
	    !cal_filter_time(inside, start_field, VENTURE_FILTER_OP_LT, ask->to, error) ||
	    !cal_find_into(context, inside, rows, error))
		return FALSE;

	if (NULL != end_field)
	{
		g_autoptr(VentureQuery) running = NULL;

		/* Greater or equal, not greater: an all-day end is its last day,
		 * at midnight, and a trip ending on the span's first day is on
		 * it. A timed item that ended exactly at the start is dropped by
		 * the fetch's overlap check. */
		running = cal_query_new(type, ask, narrow, data, error);

		if ((NULL == running) ||
		    !cal_filter_time(running, start_field, VENTURE_FILTER_OP_LT, ask->from, error) ||
		    !cal_filter_time(running, end_field, VENTURE_FILTER_OP_GTE, ask->from, error) ||
		    !cal_find_into(context, running, rows, error))
			return FALSE;
	}

	return TRUE;
}

/* Adds an IN filter over nicks. */
static gboolean
cal_filter_in(
	VentureQuery	 *query,
	const gchar	 *field,
	VentureFilterOp	  op,
	const gchar	 *const *values,
	GError		**error
){
	g_autoptr(GPtrArray) list = NULL;
	guint i;

	list = g_ptr_array_new_with_free_func(g_free);

	for (i = 0; NULL != values[i]; i++)
		g_ptr_array_add(list, g_strdup(values[i]));

	return venture_query_add_filter(query, field, op, list, error);
}

static gboolean
cal_filter_venture(
	VentureQuery			 *query,
	const VentureCalendarQuery	 *ask,
	GError				**error
){
	if (0 == ask->venture_id)
		return TRUE;

	return venture_query_add_filter_int(query, "venture-id", VENTURE_FILTER_OP_EQ,
	                                    ask->venture_id, error);
}

/* A whole-day item: a calendar date start and the day after as its end. */
static void
cal_item_whole_days(
	VentureCalendarItem	*item,
	gint			 days
){
	item->all_day = TRUE;
	g_clear_pointer(&item->end, g_date_time_unref);
	item->end = g_date_time_add_days(item->start, MAX(days, 1));
}

/* A dated deadline: whole-day when it is a date, a moment otherwise. */
static void
cal_item_due(
	VentureCalendarItem	*item,
	GDateTime		*due
){
	if (venture_time_is_calendar_date(due))
		cal_item_whole_days(item, 1);
}

/* Past due: before today for a date, before now for a time. */
static gboolean
cal_past(
	GDateTime			*due,
	const VentureCalendarQuery	*ask
){
	g_autoptr(GDateTime) now = cal_now(ask);

	if (venture_time_is_calendar_date(due))
	{
		g_autoptr(GDateTime) end_of_day = g_date_time_add_days(due, 1);

		return g_date_time_compare(end_of_day, now) <= 0;
	}

	return g_date_time_compare(due, now) < 0;
}

/* The name of a record a calendar item relates to, or NULL. */
static gchar *
cal_record_name(
	VentureContext	*context,
	const gchar	*type_name,
	gint64		 id
){
	g_autoptr(VentureEntity) record = NULL;
	GType type;

	if (venture_string_is_empty(type_name) || (id <= 0))
		return NULL;

	type = venture_entity_registry_lookup(venture_context_get_entity_registry(context), type_name);

	if (G_TYPE_INVALID == type)
		return NULL;

	record = venture_database_get(venture_context_get_database(context), type, id, NULL);

	if ((NULL == record) || venture_entity_is_deleted(record))
		return NULL;

	return g_strdup(venture_entity_get_display_name(record));
}

/* ==========================================================================
 * Events
 * ========================================================================== */

typedef struct
{
	gboolean	 recurring;
	GPtrArray	*ids;	/* the assignee's events, as text; NULL for anybody's */
} EventNarrow;

static gboolean
event_narrow(
	VentureQuery			 *query,
	const VentureCalendarQuery	 *ask,
	gpointer			  data,
	GError				**error
){
	EventNarrow *narrow = data;

	if (!ask->include_done &&
	    !venture_query_add_filter_string(query, "status", VENTURE_FILTER_OP_NE, "cancelled", error))
		return FALSE;

	if (!venture_query_add_filter_string(query, "recurrence",
	                                     narrow->recurring ? VENTURE_FILTER_OP_NE : VENTURE_FILTER_OP_EQ,
	                                     "none", error))
		return FALSE;

	if ((NULL != narrow->ids) &&
	    !venture_query_add_filter(query, "id", VENTURE_FILTER_OP_IN, narrow->ids, error))
		return FALSE;

	return cal_filter_venture(query, ask, error);
}

/*
 * The events an assignee has: those they organize and those they are an
 * attendee of, as a set of ids. Read without a date, so bounded like
 * everything else; NULL with *refused set past the bound.
 */
static GPtrArray *
event_assignee_ids(
	VentureContext			 *context,
	const VentureCalendarQuery	 *ask,
	gboolean			 *refused,
	GError				**error
){
	g_autoptr(GPtrArray) organized = NULL;
	g_autoptr(GPtrArray) attending = NULL;
	g_autoptr(GHashTable) seen = NULL;
	g_autoptr(VentureQuery) mine = NULL;
	g_autoptr(VentureQuery) invited = NULL;
	GPtrArray *ids;
	guint i;

	*refused = FALSE;
	organized = g_ptr_array_new_with_free_func(g_object_unref);
	attending = g_ptr_array_new_with_free_func(g_object_unref);

	mine = cal_query_new(VENTURE_TYPE_EVENT, ask, NULL, NULL, error);

	if ((NULL == mine) ||
	    !venture_query_add_filter_int(mine, "organizer-user-id", VENTURE_FILTER_OP_EQ,
	                                  ask->assignee_user_id, error) ||
	    !cal_find_into(context, mine, organized, error))
		return NULL;

	invited = cal_query_new(VENTURE_TYPE_EVENT_ATTENDEE, ask, NULL, NULL, error);

	if ((NULL == invited) ||
	    !venture_query_add_filter_int(invited, "user-id", VENTURE_FILTER_OP_EQ,
	                                  ask->assignee_user_id, error) ||
	    !cal_find_into(context, invited, attending, error))
		return NULL;

	if ((organized->len > (guint)cal_bound()) || (attending->len > (guint)cal_bound()))
	{
		*refused = TRUE;
		return NULL;
	}

	seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	ids = g_ptr_array_new_with_free_func(g_free);

	for (i = 0; i < organized->len; i++)
		g_hash_table_add(seen, g_strdup_printf("%" G_GINT64_FORMAT,
			venture_entity_get_id(g_ptr_array_index(organized, i))));

	for (i = 0; i < attending->len; i++)
		g_hash_table_add(seen, g_strdup_printf("%" G_GINT64_FORMAT,
			cal_int(g_ptr_array_index(attending, i), "event-id")));

	{
		GHashTableIter iter;
		gpointer key;

		g_hash_table_iter_init(&iter, seen);

		while (g_hash_table_iter_next(&iter, &key, NULL))
			g_ptr_array_add(ids, g_strdup(key));
	}

	return ids;
}

/* One occurrence of an event as an item. @occurrence_start is the event's
 * own start for a single event. */
static VentureCalendarItem *
event_item(
	VentureEntity	*event,
	GDateTime	*occurrence_start,
	gboolean	 recurring
){
	g_autoptr(GDateTime) starts = NULL;
	g_autoptr(GDateTime) ends = NULL;
	g_autofree gchar *name = NULL;
	g_autofree gchar *where = NULL;
	VentureCalendarItem *item;
	gboolean all_day;

	g_object_get(event, "starts-at", &starts, "ends-at", &ends, "all-day", &all_day,
	             "name", &name, "location", &where, NULL);

	item = venture_calendar_item_new("event", name, occurrence_start, "event",
	                                 venture_entity_get_id(event));
	item->subkind = g_strdup(cal_nick(event, "kind"));
	item->status = g_strdup(cal_nick(event, "status"));

	if (!venture_string_is_empty(where))
		item->detail = g_strdup(where);

	if (all_day)
	{
		/* The stored end is the last day, inclusive. */
		gint days = 1;

		if (NULL != ends)
			days = (gint)(g_date_time_difference(ends, starts) / G_TIME_SPAN_DAY) + 1;

		cal_item_whole_days(item, days);
	}
	else if ((NULL != ends) && (g_date_time_compare(ends, starts) > 0))
	{
		item->end = g_date_time_add(occurrence_start, g_date_time_difference(ends, starts));
	}

	/* A recurring event's occurrences share the record but not the
	 * feed's identity: each is the series' UUID and its day. */
	if (recurring)
	{
		g_autoptr(GDateTime) utc = g_date_time_to_utc(occurrence_start);
		g_autofree gchar *stamp = g_date_time_format(utc, "%Y%m%d");

		item->uid = g_strdup_printf("%s-%s@venture", venture_entity_get_uuid(event), stamp);
	}
	else
	{
		item->uid = cal_uid(event);
	}

	return item;
}

/* Expands one series into the span, at most
 * VENTURE_CALENDAR_MAX_OCCURRENCES; returns how many were added. */
static guint
event_expand(
	VentureEntity			*event,
	const VentureCalendarQuery	*ask,
	GPtrArray			*items,
	gboolean			*capped
){
	g_autoptr(GDateTime) starts = NULL;
	g_autoptr(GDateTime) ends = NULL;
	g_autoptr(GDateTime) until = NULL;
	g_autoptr(GDateTime) stop = NULL;
	VentureEventRecurrence recurrence;
	GTimeSpan length;
	gboolean all_day;
	gint64 n;
	guint added;
	guint steps;

	g_object_get(event, "starts-at", &starts, "ends-at", &ends, "recurs-until", &until,
	             "recurrence", &recurrence, "all-day", &all_day, NULL);

	if (NULL == starts)
		return 0;

	length = 0;

	if (NULL != ends)
		length = g_date_time_difference(ends, starts) + (all_day ? G_TIME_SPAN_DAY : 0);

	/* The last start allowed: the span's end, or the day after
	 * recurs-until, whichever is first (the until day itself may hold
	 * an occurrence at any hour). */
	stop = g_date_time_ref(ask->to);

	if (NULL != until)
	{
		g_autoptr(GDateTime) after = g_date_time_add_days(until, 1);

		if (g_date_time_compare(after, stop) < 0)
		{
			g_clear_pointer(&stop, g_date_time_unref);
			stop = g_steal_pointer(&after);
		}
	}

	added = 0;
	steps = 0;

	for (n = venture_planner_occurrence_first_index(starts, length, recurrence, ask->from); ; n++)
	{
		g_autoptr(GDateTime) occurrence = venture_planner_occurrence_nth(starts, recurrence, n);
		VentureCalendarItem *item;

		if ((NULL == occurrence) || (g_date_time_compare(occurrence, stop) >= 0))
			break;

		/* The backstop: a span longer than the calendar allows, or a
		 * first index that undershot by far, still ends. */
		if (++steps > VENTURE_CALENDAR_MAX_OCCURRENCES + 64)
		{
			*capped = TRUE;
			break;
		}

		item = event_item(event, occurrence, TRUE);

		if ((g_date_time_compare(item->start, ask->to) < 0) &&
		    ((g_date_time_compare(item->start, ask->from) >= 0) ||
		     ((NULL != item->end) && (g_date_time_compare(item->end, ask->from) > 0))))
		{
			if (added >= VENTURE_CALENDAR_MAX_OCCURRENCES)
			{
				venture_calendar_item_free(item);
				*capped = TRUE;
				break;
			}

			g_ptr_array_add(items, item);
			added++;
		}
		else
		{
			venture_calendar_item_free(item);
		}
	}

	return added;
}

static gboolean
source_events(
	VentureContext			 *context,
	const VentureCalendarQuery	 *ask,
	VentureCalendarResult		 *result,
	gpointer			  user_data,
	GError				**error
){
	g_autoptr(GPtrArray) single = NULL;
	g_autoptr(GPtrArray) series = NULL;
	g_autoptr(GPtrArray) ids = NULL;
	g_autoptr(GPtrArray) items = NULL;
	g_autoptr(VentureQuery) repeating = NULL;
	EventNarrow narrow;
	gboolean capped;
	guint i;

	(void)user_data;

	/* --- Whose: the organizer's and the attendees' --- */

	if ((0 != ask->assignee_user_id) || !venture_string_is_empty(ask->assignee_username))
	{
		gboolean refused;

		if (0 == ask->assignee_user_id)
		{
			venture_calendar_result_note(result,
				"Events: left out, because they are assigned by user and the "
				"assignee is not one");
			return TRUE;
		}

		ids = event_assignee_ids(context, ask, &refused, error);

		if (refused)
		{
			venture_calendar_result_refuse(result, "Events", cal_bound());
			return TRUE;
		}

		if (NULL == ids)
			return FALSE;

		if (0 == ids->len)
			return TRUE;
	}

	/* --- One-off events, and the series --- */

	single = g_ptr_array_new_with_free_func(g_object_unref);
	series = g_ptr_array_new_with_free_func(g_object_unref);
	narrow.ids = ids;

	narrow.recurring = FALSE;

	if (!cal_span_rows(context, ask, VENTURE_TYPE_EVENT, "starts-at", "ends-at",
	                   event_narrow, &narrow, single, error))
		return FALSE;

	/* A series is read whole up to the span's end: its first start can
	 * be years ago. Its until is judged in C, because "no end date"
	 * cannot be an alternative in one filter. */
	narrow.recurring = TRUE;
	repeating = cal_query_new(VENTURE_TYPE_EVENT, ask, event_narrow, &narrow, error);

	if ((NULL == repeating) ||
	    !cal_filter_time(repeating, "starts-at", VENTURE_FILTER_OP_LT, ask->to, error) ||
	    !cal_find_into(context, repeating, series, error))
		return FALSE;

	if (single->len + series->len > (guint)cal_bound())
	{
		venture_calendar_result_refuse(result, "Events", cal_bound());
		return TRUE;
	}

	/* --- The items --- */

	items = g_ptr_array_new_with_free_func((GDestroyNotify)venture_calendar_item_free);

	for (i = 0; i < single->len; i++)
	{
		VentureEntity *event = g_ptr_array_index(single, i);
		g_autoptr(GDateTime) starts = cal_time(event, "starts-at");

		if (NULL != starts)
			g_ptr_array_add(items, event_item(event, starts, FALSE));
	}

	capped = FALSE;

	for (i = 0; i < series->len; i++)
	{
		g_autoptr(GDateTime) until = cal_time(g_ptr_array_index(series, i), "recurs-until");

		if ((NULL != until) && (g_date_time_compare(until, ask->from) < 0))
		{
			/* Ended before the span: the until day is a date, so an
			 * occurrence on it starts at or after its midnight. */
			g_autoptr(GDateTime) after = g_date_time_add_days(until, 1);

			if (g_date_time_compare(after, ask->from) <= 0)
				continue;
		}

		event_expand(g_ptr_array_index(series, i), ask, items, &capped);

		if (items->len > (guint)cal_bound())
		{
			venture_calendar_result_refuse(result, "Events", cal_bound());
			return TRUE;
		}
	}

	if (capped)
		venture_calendar_result_note(result,
			"Events: a repeating event was cut at %d occurrences; ask for a "
			"shorter span", VENTURE_CALENDAR_MAX_OCCURRENCES);

	for (i = 0; i < items->len; i++)
		venture_calendar_result_add(result, g_steal_pointer(&g_ptr_array_index(items, i)));

	return TRUE;
}

/* ==========================================================================
 * Tickets
 * ========================================================================== */

static gboolean
ticket_narrow(
	VentureQuery			 *query,
	const VentureCalendarQuery	 *ask,
	gpointer			  data,
	GError				**error
){
	static const gchar *const open_only[] = { "done", "cancelled", NULL };
	static const gchar *const not_cancelled[] = { "cancelled", NULL };

	(void)data;

	if (!cal_filter_in(query, "status", VENTURE_FILTER_OP_NOT_IN,
	                   ask->include_done ? not_cancelled : open_only, error))
		return FALSE;

	if (!venture_string_is_empty(ask->assignee_username) &&
	    !venture_query_add_filter_string(query, "assignee", VENTURE_FILTER_OP_EQ,
	                                     ask->assignee_username, error))
		return FALSE;

	return cal_filter_venture(query, ask, error);
}

static gboolean
source_tickets(
	VentureContext			 *context,
	const VentureCalendarQuery	 *ask,
	VentureCalendarResult		 *result,
	gpointer			  user_data,
	GError				**error
){
	g_autoptr(GPtrArray) rows = NULL;
	guint i;

	(void)user_data;

	/* Tickets are assigned by name; a user with no name has none. */
	if ((0 != ask->assignee_user_id) && venture_string_is_empty(ask->assignee_username))
		return TRUE;

	rows = g_ptr_array_new_with_free_func(g_object_unref);

	if (!cal_span_rows(context, ask, VENTURE_TYPE_TICKET, "due-at", NULL,
	                   ticket_narrow, NULL, rows, error))
		return FALSE;

	if (rows->len > (guint)cal_bound())
	{
		venture_calendar_result_refuse(result, "Tickets", cal_bound());
		return TRUE;
	}

	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *ticket = g_ptr_array_index(rows, i);
		g_autoptr(GDateTime) due = cal_time(ticket, "due-at");
		g_autofree gchar *title = cal_text(ticket, "title");
		g_autofree gchar *assignee = cal_text(ticket, "assignee");
		VentureCalendarItem *item;
		gint status;

		if (NULL == due)
			continue;

		status = cal_enum(ticket, "status");
		item = venture_calendar_item_new("ticket", title, due, "ticket",
		                                 venture_entity_get_id(ticket));
		cal_item_due(item, due);
		item->subkind = g_strdup(cal_nick(ticket, "issue-type"));
		item->status = g_strdup(cal_nick(ticket, "status"));
		item->overdue = (VENTURE_TICKET_STATUS_DONE != status) &&
		                (VENTURE_TICKET_STATUS_CANCELLED != status) && cal_past(due, ask);
		item->uid = cal_uid(ticket);

		if (!venture_string_is_empty(assignee))
			item->detail = g_strdup_printf("Assigned to %s", assignee);

		venture_calendar_result_add(result, item);
	}

	return TRUE;
}

/* ==========================================================================
 * Activities
 * ========================================================================== */

typedef struct
{
	gboolean	 timed;	/* the starts-at pass; else due-at with no start */
} ActivityNarrow;

static gboolean
activity_narrow(
	VentureQuery			 *query,
	const VentureCalendarQuery	 *ask,
	gpointer			  data,
	GError				**error
){
	static const gchar *const kinds[] = { "task", "call", "meeting", "followup", NULL };
	static const gchar *const planned[] = { "planned", NULL };
	static const gchar *const planned_or_done[] = { "planned", "done", NULL };
	ActivityNarrow *narrow = data;

	/* Email is a record of a message, not a plan: it has no place on a
	 * calendar. */
	if (!cal_filter_in(query, "kind", VENTURE_FILTER_OP_IN, kinds, error) ||
	    !cal_filter_in(query, "status", VENTURE_FILTER_OP_IN,
	                   ask->include_done ? planned_or_done : planned, error))
		return FALSE;

	if (!narrow->timed &&
	    !venture_query_add_filter(query, "starts-at", VENTURE_FILTER_OP_IS_NULL, NULL, error))
		return FALSE;

	if (!venture_string_is_empty(ask->assignee_username) &&
	    !venture_query_add_filter_string(query, "owner", VENTURE_FILTER_OP_EQ,
	                                     ask->assignee_username, error))
		return FALSE;

	return TRUE;
}

/* What an activity is about: its related record, else its deal, company
 * or contact. */
static gchar *
activity_about(
	VentureContext	*context,
	VentureEntity	*activity
){
	g_autofree gchar *related_type = cal_text(activity, "related-type");
	gchar *name;

	name = cal_record_name(context, related_type, cal_int(activity, "related-id"));

	if (NULL == name)
		name = cal_record_name(context, "deal", cal_int(activity, "deal-id"));

	if (NULL == name)
		name = cal_record_name(context, "company", cal_int(activity, "company-id"));

	if (NULL == name)
		name = cal_record_name(context, "contact", cal_int(activity, "contact-id"));

	return name;
}

static gboolean
source_activities(
	VentureContext			 *context,
	const VentureCalendarQuery	 *ask,
	VentureCalendarResult		 *result,
	gpointer			  user_data,
	GError				**error
){
	g_autoptr(GPtrArray) rows = NULL;
	ActivityNarrow timed;
	ActivityNarrow untimed;
	guint i;

	(void)user_data;

	/* Activities are owned by name; a user with no name has none. */
	if ((0 != ask->assignee_user_id) && venture_string_is_empty(ask->assignee_username))
		return TRUE;

	rows = g_ptr_array_new_with_free_func(g_object_unref);
	timed.timed = TRUE;
	untimed.timed = FALSE;

	/* Planned with a time: starts-at to ends-at. Planned with only a due
	 * date: on that date. */
	if (!cal_span_rows(context, ask, VENTURE_TYPE_ACTIVITY, "starts-at", "ends-at",
	                   activity_narrow, &timed, rows, error) ||
	    !cal_span_rows(context, ask, VENTURE_TYPE_ACTIVITY, "due-at", NULL,
	                   activity_narrow, &untimed, rows, error))
		return FALSE;

	if (rows->len > (guint)cal_bound())
	{
		venture_calendar_result_refuse(result, "Activities", cal_bound());
		return TRUE;
	}

	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *activity = g_ptr_array_index(rows, i);
		g_autoptr(GDateTime) starts = cal_time(activity, "starts-at");
		g_autoptr(GDateTime) ends = cal_time(activity, "ends-at");
		g_autoptr(GDateTime) due = cal_time(activity, "due-at");
		g_autofree gchar *subject = cal_text(activity, "subject");
		g_autofree gchar *about = NULL;
		VentureCalendarItem *item;
		GDateTime *when;

		when = (NULL != starts) ? starts : due;

		if (NULL == when)
			continue;

		item = venture_calendar_item_new("activity", subject, when, "activity",
		                                 venture_entity_get_id(activity));
		item->subkind = g_strdup(cal_nick(activity, "kind"));
		item->status = g_strdup(cal_nick(activity, "status"));
		item->uid = cal_uid(activity);

		if (NULL != starts)
		{
			if ((NULL != ends) && (g_date_time_compare(ends, starts) > 0))
				item->end = g_date_time_ref(ends);
		}
		else
		{
			cal_item_due(item, due);
		}

		item->overdue = (VENTURE_ACTIVITY_STATUS_PLANNED == cal_enum(activity, "status")) &&
		                cal_past(when, ask);

		about = activity_about(context, activity);

		if (NULL != about)
			item->detail = g_strdup_printf("Re: %s", about);

		venture_calendar_result_add(result, item);
	}

	return TRUE;
}

/* ==========================================================================
 * Milestones and goals: dated targets
 * ========================================================================== */

static gboolean
milestone_narrow(
	VentureQuery			 *query,
	const VentureCalendarQuery	 *ask,
	gpointer			  data,
	GError				**error
){
	static const gchar *const open_only[] = { "completed", "cancelled", NULL };
	static const gchar *const not_cancelled[] = { "cancelled", NULL };

	(void)data;

	return cal_filter_in(query, "status", VENTURE_FILTER_OP_NOT_IN,
	                     ask->include_done ? not_cancelled : open_only, error) &&
	       cal_filter_venture(query, ask, error);
}

static gboolean
goal_narrow(
	VentureQuery			 *query,
	const VentureCalendarQuery	 *ask,
	gpointer			  data,
	GError				**error
){
	static const gchar *const open_only[] = { "active", "paused", NULL };
	static const gchar *const with_achieved[] = { "active", "paused", "achieved", NULL };

	(void)data;

	return cal_filter_in(query, "status", VENTURE_FILTER_OP_IN,
	                     ask->include_done ? with_achieved : open_only, error) &&
	       cal_filter_venture(query, ask, error);
}

/* A record with a due date and a name, open while its status is not
 * @closed: a milestone or a goal. */
static gboolean
source_dated(
	VentureContext			 *context,
	const VentureCalendarQuery	 *ask,
	VentureCalendarResult		 *result,
	GType				  type,
	const gchar			 *kind,
	const gchar			 *label,
	CalendarNarrowFunc		  narrow,
	const gchar			 *closed,
	GError				**error
){
	g_autoptr(GPtrArray) rows = NULL;
	guint i;

	rows = g_ptr_array_new_with_free_func(g_object_unref);

	if (!cal_span_rows(context, ask, type, "due-on", NULL, narrow, NULL, rows, error))
		return FALSE;

	if (rows->len > (guint)cal_bound())
	{
		venture_calendar_result_refuse(result, label, cal_bound());
		return TRUE;
	}

	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(rows, i);
		g_autoptr(GDateTime) due = cal_time(row, "due-on");
		g_autofree gchar *name = cal_text(row, "name");
		VentureCalendarItem *item;

		if (NULL == due)
			continue;

		item = venture_calendar_item_new(kind, name, due, kind, venture_entity_get_id(row));
		cal_item_whole_days(item, 1);
		item->status = g_strdup(cal_nick(row, "status"));
		item->overdue = (0 != g_strcmp0(item->status, closed)) && cal_past(due, ask);
		item->uid = cal_uid(row);
		item->detail = g_strdup_printf("%s due", label);
		venture_calendar_result_add(result, item);
	}

	return TRUE;
}

static gboolean
source_milestones(
	VentureContext			 *context,
	const VentureCalendarQuery	 *ask,
	VentureCalendarResult		 *result,
	gpointer			  user_data,
	GError				**error
){
	(void)user_data;

	return source_dated(context, ask, result, VENTURE_TYPE_MILESTONE, "milestone",
	                    "Milestone", milestone_narrow, "completed", error);
}

static gboolean
source_goals(
	VentureContext			 *context,
	const VentureCalendarQuery	 *ask,
	VentureCalendarResult		 *result,
	gpointer			  user_data,
	GError				**error
){
	(void)user_data;

	return source_dated(context, ask, result, VENTURE_TYPE_GOAL, "goal",
	                    "Goal", goal_narrow, "achieved", error);
}

/* ==========================================================================
 * Sprints: spans
 * ========================================================================== */

static gboolean
sprint_narrow(
	VentureQuery			 *query,
	const VentureCalendarQuery	 *ask,
	gpointer			  data,
	GError				**error
){
	(void)data;

	if (!ask->include_done &&
	    !venture_query_add_filter_string(query, "status", VENTURE_FILTER_OP_NE, "completed", error))
		return FALSE;

	return cal_filter_venture(query, ask, error);
}

static gboolean
source_sprints(
	VentureContext			 *context,
	const VentureCalendarQuery	 *ask,
	VentureCalendarResult		 *result,
	gpointer			  user_data,
	GError				**error
){
	g_autoptr(GPtrArray) rows = NULL;
	guint i;

	(void)user_data;

	rows = g_ptr_array_new_with_free_func(g_object_unref);

	if (!cal_span_rows(context, ask, VENTURE_TYPE_SPRINT, "starts-on", "ends-on",
	                   sprint_narrow, NULL, rows, error))
		return FALSE;

	if (rows->len > (guint)cal_bound())
	{
		venture_calendar_result_refuse(result, "Sprints", cal_bound());
		return TRUE;
	}

	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *sprint = g_ptr_array_index(rows, i);
		g_autoptr(GDateTime) starts = cal_time(sprint, "starts-on");
		g_autoptr(GDateTime) ends = cal_time(sprint, "ends-on");
		g_autofree gchar *name = cal_text(sprint, "name");
		VentureCalendarItem *item;

		if (NULL == starts)
			continue;

		item = venture_calendar_item_new("sprint", name, starts, "sprint",
		                                 venture_entity_get_id(sprint));
		item->status = g_strdup(cal_nick(sprint, "status"));
		item->uid = cal_uid(sprint);

		/* A sprint's dates are days, its end the last day of it. */
		if (venture_time_is_calendar_date(starts))
		{
			gint days = 1;

			if ((NULL != ends) && (g_date_time_compare(ends, starts) >= 0))
				days = (gint)(g_date_time_difference(ends, starts) / G_TIME_SPAN_DAY) + 1;

			cal_item_whole_days(item, days);
		}
		else if ((NULL != ends) && (g_date_time_compare(ends, starts) > 0))
		{
			item->end = g_date_time_ref(ends);
		}

		venture_calendar_result_add(result, item);
	}

	return TRUE;
}

/* ==========================================================================
 * Deals: expected closes
 * ========================================================================== */

static gboolean
deal_narrow(
	VentureQuery			 *query,
	const VentureCalendarQuery	 *ask,
	gpointer			  data,
	GError				**error
){
	static const gchar *const open_only[] = { "won", "lost", NULL };
	static const gchar *const not_lost[] = { "lost", NULL };

	(void)data;

	if (!cal_filter_in(query, "stage", VENTURE_FILTER_OP_NOT_IN,
	                   ask->include_done ? not_lost : open_only, error))
		return FALSE;

	if (((0 != ask->assignee_user_id) || !venture_string_is_empty(ask->assignee_username)) &&
	    !venture_query_add_filter_int(query, "owner-user-id", VENTURE_FILTER_OP_EQ,
	                                  ask->assignee_user_id, error))
		return FALSE;

	return cal_filter_venture(query, ask, error);
}

static gboolean
source_deals(
	VentureContext			 *context,
	const VentureCalendarQuery	 *ask,
	VentureCalendarResult		 *result,
	gpointer			  user_data,
	GError				**error
){
	g_autoptr(GPtrArray) rows = NULL;
	guint i;

	(void)user_data;

	rows = g_ptr_array_new_with_free_func(g_object_unref);

	if (!cal_span_rows(context, ask, VENTURE_TYPE_DEAL, "expected-close-at", NULL,
	                   deal_narrow, NULL, rows, error))
		return FALSE;

	if (rows->len > (guint)cal_bound())
	{
		venture_calendar_result_refuse(result, "Deals", cal_bound());
		return TRUE;
	}

	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *deal = g_ptr_array_index(rows, i);
		g_autoptr(GDateTime) closes = cal_time(deal, "expected-close-at");
		g_autofree gchar *name = cal_text(deal, "name");
		g_autoptr(VentureMoney) value = NULL;
		VentureCalendarItem *item;
		gint stage;

		if (NULL == closes)
			continue;

		stage = cal_enum(deal, "stage");
		item = venture_calendar_item_new("deal", name, closes, "deal", venture_entity_get_id(deal));
		cal_item_due(item, closes);
		item->status = g_strdup(cal_nick(deal, "stage"));
		item->overdue = (VENTURE_DEAL_STAGE_WON != stage) && (VENTURE_DEAL_STAGE_LOST != stage) &&
		                cal_past(closes, ask);
		item->uid = cal_uid(deal);

		g_object_get(deal, "value", &value, NULL);

		if (NULL != value)
		{
			g_autofree gchar *worth = venture_money_to_display_string(value, TRUE);

			item->detail = g_strdup_printf("Expected to close, worth %s", worth);
		}
		else
		{
			item->detail = g_strdup("Expected to close");
		}

		venture_calendar_result_add(result, item);
	}

	return TRUE;
}

/* ==========================================================================
 * The money calendar
 *
 * Not a second copy of its logic: the money calendar's own expander
 * answers, per organization, and its events become items. Its bills and
 * invoices carried to today when overdue stay carried.
 * ========================================================================== */

static gboolean
source_money(
	VentureContext			 *context,
	const VentureCalendarQuery	 *ask,
	VentureCalendarResult		 *result,
	gpointer			  user_data,
	GError				**error
){
	g_autoptr(VentureDateRange) range = NULL;
	g_autoptr(GDateTime) now = NULL;
	g_autoptr(GPtrArray) events = NULL;
	gsize o;
	guint i;

	(void)user_data;

	range = venture_date_range_new(ask->from, ask->to);
	now = cal_now(ask);
	events = g_ptr_array_new_with_free_func(g_object_unref);

	for (o = 0; o < ask->n_organizations; o++)
	{
		g_autoptr(GPtrArray) all = NULL;
		g_autoptr(GPtrArray) kept = NULL;

		all = venture_money_calendar_events(context, ask->organization_ids[o], range, now, error);

		if (NULL == all)
			return FALSE;

		kept = venture_money_calendar_filter(all, NULL, 0, ask->venture_id, NULL);

		for (i = 0; i < kept->len; i++)
			g_ptr_array_add(events, g_object_ref(g_ptr_array_index(kept, i)));
	}

	if (events->len > (guint)cal_bound())
	{
		venture_calendar_result_refuse(result, "Money", cal_bound());
		return TRUE;
	}

	for (i = 0; i < events->len; i++)
	{
		VentureMoneyCalendarEvent *event = g_ptr_array_index(events, i);
		g_autoptr(GDateTime) day = NULL;
		g_autoptr(VentureMoney) signed_amount = NULL;
		g_autofree gchar *amount = NULL;
		VentureCalendarItem *item;

		day = venture_time_from_string(venture_money_calendar_event_get_day(event), NULL);

		if (NULL == day)
			continue;

		item = venture_calendar_item_new("money", venture_money_calendar_event_get_title(event), day,
		                                 venture_money_calendar_event_get_record_type(event),
		                                 venture_money_calendar_event_get_record_id(event));
		cal_item_whole_days(item, 1);
		item->subkind = g_strdup(venture_money_calendar_event_get_kind(event));
		item->status = g_strdup(venture_money_calendar_event_get_direction(event));
		item->overdue = venture_money_calendar_event_get_overdue(event);
		item->uid = venture_money_calendar_event_get_uid(event);

		signed_amount = venture_money_calendar_event_signed_amount(event);
		amount = venture_money_to_display_string(signed_amount, TRUE);
		item->detail = g_strdup_printf("%s %s",
			(0 == g_strcmp0(venture_money_calendar_event_get_direction(event), "out")) ? "Out" : "In",
			amount);

		venture_calendar_result_add(result, item);
	}

	return TRUE;
}

/* ==========================================================================
 * CalDAV: imported events not already an activity
 * ========================================================================== */

static gboolean
caldav_narrow(
	VentureQuery			 *query,
	const VentureCalendarQuery	 *ask,
	gpointer			  data,
	GError				**error
){
	(void)data;

	if (!ask->include_done &&
	    !venture_query_add_filter_string(query, "status", VENTURE_FILTER_OP_NE, "cancelled", error))
		return FALSE;

	return TRUE;
}

static gboolean
source_caldav(
	VentureContext			 *context,
	const VentureCalendarQuery	 *ask,
	VentureCalendarResult		 *result,
	gpointer			  user_data,
	GError				**error
){
	g_autoptr(GPtrArray) rows = NULL;
	guint i;

	(void)user_data;

	rows = g_ptr_array_new_with_free_func(g_object_unref);

	if (!cal_span_rows(context, ask, VENTURE_TYPE_CALENDAR_EVENT, "starts-at", "ends-at",
	                   caldav_narrow, NULL, rows, error))
		return FALSE;

	if (rows->len > (guint)cal_bound())
	{
		venture_calendar_result_refuse(result, "Synced calendar", cal_bound());
		return TRUE;
	}

	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(rows, i);
		g_autoptr(GDateTime) starts = cal_time(row, "starts-at");
		g_autoptr(GDateTime) ends = cal_time(row, "ends-at");
		g_autofree gchar *subject = cal_text(row, "subject");
		g_autofree gchar *uid = cal_text(row, "uid");
		VentureCalendarItem *item;

		/* An imported event the sync turned into an activity is shown
		 * by the activity source; drawing it here too would put the
		 * same meeting on the calendar twice. */
		if ((NULL == starts) || (cal_int(row, "activity-id") > 0))
			continue;

		item = venture_calendar_item_new("caldav", subject, starts, "calendar_event",
		                                 venture_entity_get_id(row));
		item->status = g_strdup(cal_nick(row, "status"));
		item->uid = venture_string_is_empty(uid) ? cal_uid(row) : g_strdup(uid);

		if (venture_time_is_calendar_date(starts) &&
		    ((NULL == ends) || venture_time_is_calendar_date(ends)))
		{
			gint days = 1;

			/* A feed's all-day end is exclusive already. */
			if ((NULL != ends) && (g_date_time_compare(ends, starts) > 0))
				days = (gint)(g_date_time_difference(ends, starts) / G_TIME_SPAN_DAY);

			cal_item_whole_days(item, days);
		}
		else if ((NULL != ends) && (g_date_time_compare(ends, starts) > 0))
		{
			item->end = g_date_time_ref(ends);
		}

		venture_calendar_result_add(result, item);
	}

	return TRUE;
}

/* ==========================================================================
 * Jobs: arrival windows at customers' addresses
 * ========================================================================== */

static gboolean
job_narrow(
	VentureQuery			 *query,
	const VentureCalendarQuery	 *ask,
	gpointer			  data,
	GError				**error
){
	static const gchar *const open_only[] = { "cancelled", "done", "invoiced", NULL };
	static const gchar *const not_cancelled[] = { "cancelled", NULL };

	(void)data;

	return cal_filter_in(query, "status", VENTURE_FILTER_OP_NOT_IN,
	                     ask->include_done ? not_cancelled : open_only, error);
}

/* The jobs one person is sent to, as a set of ids. A job's technicians
 * are rows of their own, so "only mine" is answered from them. */
static GHashTable *
job_assigned(
	VentureContext			 *context,
	const VentureCalendarQuery	 *ask,
	GError				**error
){
	g_autoptr(GHashTable) jobs = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	gint64 user = ask->assignee_user_id;
	gsize o;

	/* Asked by name alone: the person of that name, else nobody's. */
	if (0 == user)
	{
		g_autoptr(VentureQuery) people = venture_query_new(VENTURE_TYPE_USER);
		g_autoptr(VentureEntity) person = NULL;

		if (!venture_query_add_filter_string(people, "username", VENTURE_FILTER_OP_EQ,
		                                     ask->assignee_username, error))
			return NULL;
		person = venture_database_find_one(venture_context_get_database(context), people, NULL);
		if (NULL == person)
			return g_steal_pointer(&jobs);
		user = venture_entity_get_id(person);
	}

	for (o = 0; o < ask->n_organizations; o++)
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_JOB_TECHNICIAN);
		g_autoptr(GPtrArray) rows = NULL;
		guint i;

		venture_query_set_limit(query, 0);
		venture_query_set_organization(query, ask->organization_ids[o]);
		if (!venture_query_add_filter_int(query, "user-id", VENTURE_FILTER_OP_EQ, user, error))
			return NULL;
		rows = venture_database_find(venture_context_get_database(context), query, error);
		if (NULL == rows)
			return NULL;
		for (i = 0; i < rows->len; i++)
		{
			gint64 *id = g_new(gint64, 1);

			*id = cal_int(g_ptr_array_index(rows, i), "job-id");
			g_hash_table_add(jobs, id);
		}
	}

	return g_steal_pointer(&jobs);
}

static gboolean
source_jobs(
	VentureContext			 *context,
	const VentureCalendarQuery	 *ask,
	VentureCalendarResult		 *result,
	gpointer			  user_data,
	GError				**error
){
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GHashTable) mine = NULL;
	guint i;

	(void)user_data;

	rows = g_ptr_array_new_with_free_func(g_object_unref);

	if (!cal_span_rows(context, ask, VENTURE_TYPE_JOB, "window-start", "window-end",
	                   job_narrow, NULL, rows, error))
		return FALSE;

	if (rows->len > (guint)cal_bound())
	{
		venture_calendar_result_refuse(result, "Jobs", cal_bound());
		return TRUE;
	}

	if ((0 != ask->assignee_user_id) || !venture_string_is_empty(ask->assignee_username))
	{
		mine = job_assigned(context, ask, error);
		if (NULL == mine)
			return FALSE;
	}

	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *job = g_ptr_array_index(rows, i);
		g_autoptr(GDateTime) starts = cal_time(job, "window-start");
		g_autoptr(GDateTime) ends = cal_time(job, "window-end");
		g_autofree gchar *title = cal_text(job, "title");
		g_autofree gchar *customer = NULL;
		g_autofree gchar *address = cal_text(job, "service-address");
		gint64 id = venture_entity_get_id(job);
		VentureCalendarItem *item;

		if (NULL == starts)
			continue;
		if ((NULL != mine) && !g_hash_table_contains(mine, &id))
			continue;

		item = venture_calendar_item_new("job", title, starts, "job", id);
		item->status = g_strdup(cal_nick(job, "status"));
		item->uid = cal_uid(job);

		if ((NULL != ends) && (g_date_time_compare(ends, starts) > 0))
			item->end = g_date_time_ref(ends);

		/* Whose and where, which is what the day is planned by. */
		customer = cal_record_name(context, "company", cal_int(job, "company-id"));
		if (!venture_string_is_empty(customer) && !venture_string_is_empty(address))
			item->detail = g_strdup_printf("%s, %s", customer, address);
		else if (!venture_string_is_empty(customer))
			item->detail = g_steal_pointer(&customer);

		venture_calendar_result_add(result, item);
	}

	return TRUE;
}

/* ==========================================================================
 * Registration
 * ========================================================================== */

void
venture_calendar_register_builtin_sources(VentureContext *context)
{
	g_return_if_fail(VENTURE_IS_CONTEXT(context));

	/* In the order the kind filter lists them: what is planned, then
	 * what is due, then the money. */
	venture_calendar_sources_add(context, "event", "Events", "event", NULL,
		VENTURE_CALENDAR_SOURCE_BY_VENTURE | VENTURE_CALENDAR_SOURCE_BY_ASSIGNEE,
		source_events, NULL, NULL);
	venture_calendar_sources_add(context, "activity", "Activities", "activity", NULL,
		VENTURE_CALENDAR_SOURCE_BY_ASSIGNEE, source_activities, NULL, NULL);
	venture_calendar_sources_add(context, "ticket", "Tickets", "ticket", NULL,
		VENTURE_CALENDAR_SOURCE_BY_VENTURE | VENTURE_CALENDAR_SOURCE_BY_ASSIGNEE,
		source_tickets, NULL, NULL);
	venture_calendar_sources_add(context, "milestone", "Milestones", "milestone", NULL,
		VENTURE_CALENDAR_SOURCE_BY_VENTURE, source_milestones, NULL, NULL);
	venture_calendar_sources_add(context, "goal", "Goals", "goal", NULL,
		VENTURE_CALENDAR_SOURCE_BY_VENTURE, source_goals, NULL, NULL);
	venture_calendar_sources_add(context, "sprint", "Sprints", "sprint", NULL,
		VENTURE_CALENDAR_SOURCE_BY_VENTURE, source_sprints, NULL, NULL);
	venture_calendar_sources_add(context, "deal", "Deals", "deal", NULL,
		VENTURE_CALENDAR_SOURCE_BY_VENTURE | VENTURE_CALENDAR_SOURCE_BY_ASSIGNEE,
		source_deals, NULL, NULL);
	venture_calendar_sources_add(context, "job", "Jobs", "job", NULL,
		VENTURE_CALENDAR_SOURCE_BY_ASSIGNEE, source_jobs, NULL, NULL);
	venture_calendar_sources_add(context, "money", "Money", NULL, "money_calendar",
		VENTURE_CALENDAR_SOURCE_BY_VENTURE, source_money, NULL, NULL);
	venture_calendar_sources_add(context, "caldav", "Synced calendar", "calendar_event", NULL,
		VENTURE_CALENDAR_SOURCE_NONE, source_caldav, NULL, NULL);
}
