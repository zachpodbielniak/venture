/*
 * venture-planner.c - The planner module's rules and run-sheets
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Save validators for events and what hangs off them, the run-sheet with
 * its running times, and the preparation tickets. Deliberately absent:
 * any write from one record to another. Ticking an agenda item done does
 * not touch its event, and an attendee's answer does not move the event's
 * status; either would be a derived write -- a version bump under whoever
 * holds the event.
 */

#include "venture.h"

#include <string.h>

#define VENTURE_PLANNER_STATE_KEY "venture-planner-installed"

/* ==========================================================================
 * Shared
 * ========================================================================== */

/* An integer property's value on @entity, 0 on a missing @entity. */
static gint64
planner_int(
	VentureEntity	*entity,
	const gchar	*property
){
	gint64 value;

	value = 0;

	if (NULL != entity)
		g_object_get(entity, property, &value, NULL);

	return value;
}

/* An enum property's value on @entity. */
static gint
planner_int_enum(
	VentureEntity	*entity,
	const gchar	*property
){
	gint value;

	value = 0;
	g_object_get(entity, property, &value, NULL);

	return value;
}

/*
 * Refuses a reference into another organization, judged only when the
 * value is being written -- the rule every reference follows, so a row
 * pointing somewhere since moved stays editable. A target that does not
 * exist is the generic reference check's to refuse, with its own words;
 * a type that is not registered (its module off) is that check's too.
 */
static gboolean
planner_same_organization(
	VentureDatabase	 *database,
	VentureEntity	 *entity,
	VentureEntity	 *previous,
	const gchar	 *property,
	const gchar	 *type_name,
	const gchar	 *label,
	GError		**error
){
	g_autoptr(VentureEntity) target = NULL;
	gint64 target_id;
	GType type;

	target_id = planner_int(entity, property);

	if (target_id <= 0)
		return TRUE;

	if ((NULL != previous) && (planner_int(previous, property) == target_id))
		return TRUE;

	type = venture_entity_registry_lookup(venture_entity_registry_get_default(), type_name);

	if (G_TYPE_INVALID == type)
		return TRUE;

	target = venture_database_get(database, type, target_id, NULL);

	if ((NULL != target) &&
	    (venture_entity_get_organization_id(target) !=
	     venture_entity_get_organization_id(entity)))
	{
		venture_set_error_validation(error, label,
			"#%" G_GINT64_FORMAT " belongs to another organization", target_id);
		return FALSE;
	}

	return TRUE;
}

/*
 * @when as the calendar date it is in @timezone, encoded the way every
 * date in VENTURE is: midnight UTC on that day. An instant already at
 * midnight UTC is a stored date and stays that day whatever the zone
 * (venture_time_to_date_string()).
 */
static GDateTime *
planner_calendar_date(
	GDateTime	*when,
	GTimeZone	*timezone
){
	g_autofree gchar *day = NULL;

	day = venture_time_to_date_string(when, timezone);

	return venture_time_from_string(day, NULL);
}

/* The event an agenda item or attendee hangs off, which must exist and
 * be the row's own organization's: it is part of the event, not a
 * reference out of it. */
static VentureEntity *
planner_parent_event(
	VentureDatabase	 *database,
	VentureEntity	 *entity,
	GError		**error
){
	g_autoptr(VentureEntity) event = NULL;
	gint64 event_id;

	event_id = planner_int(entity, "event-id");

	if (event_id <= 0)
	{
		venture_set_error_validation(error, "Event", "is required");
		return NULL;
	}

	event = venture_database_get(database, VENTURE_TYPE_EVENT, event_id, NULL);

	if (NULL == event)
	{
		venture_set_error_validation(error, "Event",
			"#%" G_GINT64_FORMAT " does not exist", event_id);
		return NULL;
	}

	if (venture_entity_get_organization_id(event) !=
	    venture_entity_get_organization_id(entity))
	{
		venture_set_error_validation(error, "Event",
			"#%" G_GINT64_FORMAT " belongs to another organization", event_id);
		return NULL;
	}

	return g_steal_pointer(&event);
}

/* ==========================================================================
 * Events
 * ========================================================================== */

/*
 * An event's rules, in the order a person would fix them:
 *
 *  - a start;
 *  - an all-day event's times are calendar dates. Both are normalised to
 *    midnight UTC on the day they fall on in the configured zone, the rule
 *    every date in VENTURE follows; its end is the *last* day, inclusive,
 *    as a person writes "1 to 3 March", and an empty end is the start's
 *    day. The calendar turns it into the exclusive end a feed wants;
 *  - an end not before the start (equal is a moment, which is fine);
 *  - an end date for repeating only on an event that repeats, and not
 *    before its first day;
 *  - its venture, goal, milestone, contact and company in its own
 *    organization.
 */
static gboolean
venture_planner_validate_event(
	VentureDatabase	 *database,
	VentureEntity	 *entity,
	VentureEntity	 *previous,
	gpointer	  user_data,
	GError		**error
){
	g_autoptr(GDateTime) starts = NULL;
	g_autoptr(GDateTime) ends = NULL;
	g_autoptr(GDateTime) until = NULL;
	GTimeZone *timezone;
	VentureEventRecurrence recurrence;
	gboolean all_day;

	timezone = user_data;

	g_object_get(entity, "starts-at", &starts, "ends-at", &ends,
	             "recurs-until", &until, "recurrence", &recurrence,
	             "all-day", &all_day, NULL);

	/* --- When --- */

	if (NULL == starts)
	{
		venture_set_error_validation(error, "Starts", "is required");
		return FALSE;
	}

	if (all_day)
	{
		g_autoptr(GDateTime) first = NULL;
		g_autoptr(GDateTime) last = NULL;

		first = planner_calendar_date(starts, timezone);
		last = (NULL != ends) ? planner_calendar_date(ends, timezone)
		                      : g_date_time_ref(first);

		if (g_date_time_compare(last, first) < 0)
		{
			g_autofree gchar *a = venture_time_to_date_string(first, NULL);
			g_autofree gchar *b = venture_time_to_date_string(last, NULL);

			venture_set_error_validation(error, "Ends",
				"is %s, before the first day %s; an all-day event ends on its "
				"last day", b, a);
			return FALSE;
		}

		/* Written back only when they differ, so a re-save of a stored
		 * event is not a change. */
		if (!venture_time_equal(first, starts))
			g_object_set(entity, "starts-at", first, NULL);

		if (!venture_time_equal(last, ends))
			g_object_set(entity, "ends-at", last, NULL);

		g_clear_pointer(&starts, g_date_time_unref);
		starts = g_steal_pointer(&first);
	}
	else if ((NULL != ends) && (g_date_time_compare(ends, starts) < 0))
	{
		g_autofree gchar *a = venture_time_to_string(starts);
		g_autofree gchar *b = venture_time_to_string(ends);

		venture_set_error_validation(error, "Ends",
			"is %s, before the start %s", b, a);
		return FALSE;
	}

	/* --- Repeating --- */

	if (NULL != until)
	{
		g_autoptr(GDateTime) first_day = NULL;

		if (VENTURE_EVENT_RECURRENCE_NONE == recurrence)
		{
			venture_set_error_validation(error, "Repeats until",
				"is only for an event that repeats; choose how often it repeats, "
				"or clear the date");
			return FALSE;
		}

		first_day = planner_calendar_date(starts, timezone);

		if (g_date_time_compare(until, first_day) < 0)
		{
			venture_set_error_validation(error, "Repeats until",
				"is before the event's first day");
			return FALSE;
		}
	}

	/* --- Reminder and budget --- */

	{
		gint64 remind;
		g_autoptr(VentureMoney) budget = NULL;

		remind = planner_int(entity, "remind-minutes");

		if ((remind < 0) || (remind > VENTURE_PLANNER_MAX_REMIND_MINUTES))
		{
			venture_set_error_validation(error, "Remind (minutes before)",
				"is %" G_GINT64_FORMAT " minutes; it takes 0 (no reminder) to %d "
				"(four weeks)", remind, VENTURE_PLANNER_MAX_REMIND_MINUTES);
			return FALSE;
		}

		g_object_get(entity, "budget", &budget, NULL);

		if ((NULL != budget) && venture_money_is_negative(budget))
		{
			venture_set_error_validation(error, "Budget",
				"cannot be negative");
			return FALSE;
		}
	}

	/* --- Where it belongs --- */

	return planner_same_organization(database, entity, previous, "venture-id",
	                                 "venture", "Venture", error) &&
	       planner_same_organization(database, entity, previous, "goal-id",
	                                 "goal", "Goal", error) &&
	       planner_same_organization(database, entity, previous, "milestone-id",
	                                 "milestone", "Milestone", error) &&
	       planner_same_organization(database, entity, previous, "contact-id",
	                                 "contact", "Contact", error) &&
	       planner_same_organization(database, entity, previous, "company-id",
	                                 "company", "Company", error);
}

/* ==========================================================================
 * Agenda items and attendees
 * ========================================================================== */

/* An agenda item: an event of its own organization, and minutes that are
 * not negative (0 is a moment: "Photo"). */
static gboolean
venture_planner_validate_agenda_item(
	VentureDatabase	 *database,
	VentureEntity	 *entity,
	VentureEntity	 *previous,
	gpointer	  user_data,
	GError		**error
){
	g_autoptr(VentureEntity) event = NULL;

	(void)previous;
	(void)user_data;

	event = planner_parent_event(database, entity, error);

	if (NULL == event)
		return FALSE;

	if (planner_int(entity, "minutes") < 0)
	{
		venture_set_error_validation(error, "Minutes",
			"cannot be negative; use 0 for a moment");
		return FALSE;
	}

	return TRUE;
}

/*
 * An attendee: an event of its own organization, and somebody -- a
 * contact, a user, a name or an address; a row with none of the four
 * invites nobody. An address given must at least look like one. The
 * contact must be the organization's own.
 */
static gboolean
venture_planner_validate_attendee(
	VentureDatabase	 *database,
	VentureEntity	 *entity,
	VentureEntity	 *previous,
	gpointer	  user_data,
	GError		**error
){
	g_autoptr(VentureEntity) event = NULL;
	g_autofree gchar *name = NULL;
	g_autofree gchar *email = NULL;

	(void)user_data;

	event = planner_parent_event(database, entity, error);

	if (NULL == event)
		return FALSE;

	g_object_get(entity, "name", &name, "email", &email, NULL);

	if ((planner_int(entity, "contact-id") <= 0) &&
	    (planner_int(entity, "user-id") <= 0) &&
	    venture_string_is_empty(name) && venture_string_is_empty(email))
	{
		venture_set_error_validation(error, "Name",
			"is required when no contact, user or email is given: an attendee "
			"has to be somebody");
		return FALSE;
	}

	if (!venture_string_is_empty(email) && (NULL == strchr(email, '@')))
	{
		venture_set_error_validation(error, "Email",
			"\"%s\" is not an email address", email);
		return FALSE;
	}

	return planner_same_organization(database, entity, previous, "contact-id",
	                                 "contact", "Contact", error);
}

/* An expense's event is its own organization's, judged when written: the
 * budget card totals expenses by the event's organization, so one filed
 * elsewhere would be counted by nobody. */
static gboolean
venture_planner_validate_expense(
	VentureDatabase	 *database,
	VentureEntity	 *entity,
	VentureEntity	 *previous,
	gpointer	  user_data,
	GError		**error
){
	(void)user_data;

	return planner_same_organization(database, entity, previous, "event-id",
	                                 "event", "Event", error);
}

/* ==========================================================================
 * The run-sheet
 * ========================================================================== */

static void
planner_agenda_line_free(gpointer data)
{
	VenturePlannerAgendaLine *line = data;

	if (NULL == line)
		return;

	g_clear_object(&line->item);
	g_clear_pointer(&line->starts, g_date_time_unref);
	g_clear_pointer(&line->ends, g_date_time_unref);
	g_free(line);
}

void
venture_planner_agenda_free(VenturePlannerAgenda *agenda)
{
	if (NULL == agenda)
		return;

	g_clear_pointer(&agenda->lines, g_ptr_array_unref);
	g_free(agenda);
}

VenturePlannerAgenda *
venture_planner_agenda(
	VentureDatabase	 *database,
	VentureEntity	 *event,
	GError		**error
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) items = NULL;
	g_autoptr(GDateTime) starts = NULL;
	g_autoptr(GDateTime) ends = NULL;
	VenturePlannerAgenda *agenda;
	gboolean all_day;
	gint64 offset;
	guint i;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	g_return_val_if_fail(VENTURE_IS_EVENT(event), NULL);

	g_object_get(event, "starts-at", &starts, "ends-at", &ends,
	             "all-day", &all_day, NULL);

	query = venture_query_new(VENTURE_TYPE_EVENT_AGENDA_ITEM);
	venture_query_set_organization(query, venture_entity_get_organization_id(event));
	venture_query_set_limit(query, VENTURE_PLANNER_MAX_AGENDA);

	if (!venture_query_add_filter_int(query, "event-id", VENTURE_FILTER_OP_EQ,
	                                  venture_entity_get_id(event), error) ||
	    !venture_query_add_order(query, "position", VENTURE_SORT_ASCENDING, error) ||
	    !venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, error))
		return NULL;

	items = venture_database_find(database, query, error);

	if (NULL == items)
		return NULL;

	agenda = g_new0(VenturePlannerAgenda, 1);
	agenda->lines = g_ptr_array_new_with_free_func(planner_agenda_line_free);

	/* Each item starts where the one before it ended: the event's start
	 * plus every earlier item's minutes. */
	offset = 0;

	for (i = 0; i < items->len; i++)
	{
		VenturePlannerAgendaLine *line;
		gint64 minutes;

		line = g_new0(VenturePlannerAgendaLine, 1);
		line->item = g_object_ref(g_ptr_array_index(items, i));
		line->offset_minutes = offset;
		minutes = MAX(planner_int(line->item, "minutes"), 0);

		if (!all_day && (NULL != starts))
		{
			line->starts = g_date_time_add_minutes(starts, (gint)offset);
			line->ends = g_date_time_add_minutes(starts, (gint)(offset + minutes));
		}

		offset += minutes;
		g_ptr_array_add(agenda->lines, line);
	}

	agenda->total_minutes = offset;

	/* Only a timed event with an end can be overrun; an all-day event's
	 * day has no clock to run past. */
	if (!all_day && (NULL != starts) && (NULL != ends))
	{
		g_autoptr(GDateTime) finish = g_date_time_add_minutes(starts, (gint)offset);

		agenda->overruns = g_date_time_compare(finish, ends) > 0;
	}

	return agenda;
}

/* ==========================================================================
 * Preparation tickets
 * ========================================================================== */

GPtrArray *
venture_planner_prep_tickets(
	VentureDatabase	 *database,
	VentureEntity	 *event,
	guint		 *out_done,
	guint		 *out_total,
	GError		**error
){
	g_autoptr(VentureQuery) query = NULL;
	GPtrArray *tickets;
	guint done;
	guint total;
	guint i;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	g_return_val_if_fail(VENTURE_IS_EVENT(event), NULL);

	query = venture_query_new(VENTURE_TYPE_TICKET);
	venture_query_set_organization(query, venture_entity_get_organization_id(event));
	venture_query_set_limit(query, VENTURE_PLANNER_MAX_AGENDA);

	if (!venture_query_add_filter_int(query, "event-id", VENTURE_FILTER_OP_EQ,
	                                  venture_entity_get_id(event), error) ||
	    !venture_query_add_order(query, "due-at", VENTURE_SORT_ASCENDING, error) ||
	    !venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, error))
		return NULL;

	tickets = venture_database_find(database, query, error);

	if (NULL == tickets)
		return NULL;

	done = 0;
	total = 0;

	/* A cancelled ticket did not happen: it is listed, and counted
	 * neither as done nor as still to do. */
	for (i = 0; i < tickets->len; i++)
	{
		VentureTicketStatus status;

		g_object_get(g_ptr_array_index(tickets, i), "status", &status, NULL);

		if (VENTURE_TICKET_STATUS_CANCELLED == status)
			continue;

		total++;

		if (VENTURE_TICKET_STATUS_DONE == status)
			done++;
	}

	if (NULL != out_done)
		*out_done = done;

	if (NULL != out_total)
		*out_total = total;

	return tickets;
}

/* ==========================================================================
 * Attendees
 * ========================================================================== */

/* Organizers lead the list; the rest keep the order they were added in. */
static gint
planner_attendee_compare(
	gconstpointer	a,
	gconstpointer	b
){
	VentureEntity *left = *(VentureEntity *const *)a;
	VentureEntity *right = *(VentureEntity *const *)b;
	gboolean left_runs;
	gboolean right_runs;
	gint64 left_id;
	gint64 right_id;

	left_runs = VENTURE_ATTENDEE_ROLE_ORGANIZER == (VentureAttendeeRole)planner_int_enum(left, "role");
	right_runs = VENTURE_ATTENDEE_ROLE_ORGANIZER == (VentureAttendeeRole)planner_int_enum(right, "role");

	if (left_runs != right_runs)
		return left_runs ? -1 : 1;

	left_id = venture_entity_get_id(left);
	right_id = venture_entity_get_id(right);

	return (left_id < right_id) ? -1 : (left_id > right_id);
}

GPtrArray *
venture_planner_attendees(
	VentureDatabase	 *database,
	VentureEntity	 *event,
	guint		 *counts,
	GError		**error
){
	g_autoptr(VentureQuery) query = NULL;
	GPtrArray *attendees;
	guint i;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	g_return_val_if_fail(VENTURE_IS_EVENT(event), NULL);

	query = venture_query_new(VENTURE_TYPE_EVENT_ATTENDEE);
	venture_query_set_organization(query, venture_entity_get_organization_id(event));
	venture_query_set_limit(query, VENTURE_PLANNER_MAX_AGENDA);

	if (!venture_query_add_filter_int(query, "event-id", VENTURE_FILTER_OP_EQ,
	                                  venture_entity_get_id(event), error) ||
	    !venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, error))
		return NULL;

	attendees = venture_database_find(database, query, error);

	if (NULL == attendees)
		return NULL;

	g_ptr_array_sort(attendees, planner_attendee_compare);

	if (NULL != counts)
	{
		memset(counts, 0, sizeof(guint) * VENTURE_PLANNER_RSVP_COUNT);

		for (i = 0; i < attendees->len; i++)
		{
			gint rsvp = planner_int_enum(g_ptr_array_index(attendees, i), "rsvp");

			if ((rsvp >= 0) && (rsvp < VENTURE_PLANNER_RSVP_COUNT))
				counts[rsvp]++;
		}
	}

	return attendees;
}

/* ==========================================================================
 * Occurrences
 * ========================================================================== */

GDateTime *
venture_planner_occurrence_nth(
	GDateTime		*starts,
	VentureEventRecurrence	 recurrence,
	gint64			 n
){
	g_return_val_if_fail(NULL != starts, NULL);

	switch (recurrence)
	{
		case VENTURE_EVENT_RECURRENCE_DAILY:
			return g_date_time_add_days(starts, (gint)n);
		case VENTURE_EVENT_RECURRENCE_WEEKLY:
			return g_date_time_add_weeks(starts, (gint)n);
		case VENTURE_EVENT_RECURRENCE_MONTHLY:
			return g_date_time_add_months(starts, (gint)n);
		case VENTURE_EVENT_RECURRENCE_YEARLY:
			return g_date_time_add_years(starts, (gint)n);
		case VENTURE_EVENT_RECURRENCE_NONE:
		default:
			return (0 == n) ? g_date_time_ref(starts) : NULL;
	}
}

gint64
venture_planner_occurrence_first_index(
	GDateTime		*starts,
	GTimeSpan		 length,
	VentureEventRecurrence	 recurrence,
	GDateTime		*from
){
	GTimeSpan behind;
	gint64 n;

	g_return_val_if_fail(NULL != starts, 0);
	g_return_val_if_fail(NULL != from, 0);

	behind = g_date_time_difference(from, starts) - length;

	if (behind <= 0)
		return 0;

	switch (recurrence)
	{
		case VENTURE_EVENT_RECURRENCE_DAILY:
			n = behind / G_TIME_SPAN_DAY;
			break;
		case VENTURE_EVENT_RECURRENCE_WEEKLY:
			n = behind / (7 * G_TIME_SPAN_DAY);
			break;
		case VENTURE_EVENT_RECURRENCE_MONTHLY:
			/* A month is at most 31 days, so this undershoots. */
			n = behind / (31 * G_TIME_SPAN_DAY);
			break;
		case VENTURE_EVENT_RECURRENCE_YEARLY:
			n = behind / (366 * G_TIME_SPAN_DAY);
			break;
		case VENTURE_EVENT_RECURRENCE_NONE:
		default:
			return 0;
	}

	/* One step further back for the occurrence straddling @from. */
	return MAX(n - 1, 0);
}

GDateTime *
venture_planner_next_occurrence(
	VentureEntity	*event,
	GDateTime	*after
){
	g_autoptr(GDateTime) starts = NULL;
	g_autoptr(GDateTime) until = NULL;
	g_autoptr(GDateTime) stop = NULL;
	VentureEventRecurrence recurrence;
	gint64 n;
	guint steps;

	g_return_val_if_fail(VENTURE_IS_EVENT(event), NULL);
	g_return_val_if_fail(NULL != after, NULL);

	g_object_get(event, "starts-at", &starts, "recurs-until", &until,
	             "recurrence", &recurrence, NULL);

	if (NULL == starts)
		return NULL;

	/* The until day itself may hold an occurrence at any hour. */
	if ((NULL != until) && (VENTURE_EVENT_RECURRENCE_NONE != recurrence))
		stop = g_date_time_add_days(until, 1);

	/* Bounded: the first index undershoots by at most a few steps. */
	for (n = venture_planner_occurrence_first_index(starts, 0, recurrence, after), steps = 0;
	     steps < 64; n++, steps++)
	{
		g_autoptr(GDateTime) occurrence = NULL;

		occurrence = venture_planner_occurrence_nth(starts, recurrence, n);

		if ((NULL == occurrence) ||
		    ((NULL != stop) && (g_date_time_compare(occurrence, stop) >= 0)))
			return NULL;

		if (g_date_time_compare(occurrence, after) > 0)
			return g_steal_pointer(&occurrence);
	}

	return NULL;
}

/* ==========================================================================
 * Reminders
 * ========================================================================== */

#define VENTURE_PLANNER_SWEEP_KEY "venture-planner-reminder-sweep"

/* The instant an occurrence begins for a person: a timed one at its
 * start; an all-day one at midnight of its day in the configured zone,
 * not at the midnight UTC its date is stored as. */
static GDateTime *
planner_occurrence_instant(
	GDateTime	*occurrence,
	gboolean	 all_day,
	GTimeZone	*timezone
){
	g_autoptr(GDateTime) utc = NULL;
	g_autoptr(GTimeZone) zone = NULL;

	if (!all_day)
		return g_date_time_ref(occurrence);

	utc = g_date_time_to_utc(occurrence);
	zone = (NULL != timezone) ? g_time_zone_ref(timezone) : g_time_zone_new_utc();

	return g_date_time_new(zone,
	                       g_date_time_get_year(utc), g_date_time_get_month(utc),
	                       g_date_time_get_day_of_month(utc), 0, 0, 0);
}

/* Adds @user_id to @recipients once. */
static void
planner_add_recipient(
	GArray	*recipients,
	gint64	 user_id
){
	guint i;

	if (user_id <= 0)
		return;

	for (i = 0; i < recipients->len; i++)
		if (g_array_index(recipients, gint64, i) == user_id)
			return;

	g_array_append_val(recipients, user_id);
}

/* The events worth looking at: reminders on, not cancelled, either
 * repeating or starting within the reminder horizon. Two queries,
 * because a query has no OR; each is bounded and the sweep says so when
 * a bound is reached rather than pretending it looked at everything. */
static gboolean
planner_reminder_candidates(
	VentureDatabase	 *database,
	gint64		  organization_id,
	GDateTime	 *now,
	gboolean	  repeating,
	GPtrArray	 *into,
	GError		**error
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	gint bound;
	guint i;

	bound = venture_aggregate_get_max_rows();
	query = venture_query_new(VENTURE_TYPE_EVENT);

	if (organization_id > 0)
		venture_query_set_organization(query, organization_id);

	if (!venture_query_add_filter_int(query, "remind-minutes", VENTURE_FILTER_OP_GT, 0, error) ||
	    !venture_query_add_filter_string(query, "status", VENTURE_FILTER_OP_NE, "cancelled", error) ||
	    !venture_query_add_filter_string(query, "recurrence",
	                                     repeating ? VENTURE_FILTER_OP_NE : VENTURE_FILTER_OP_EQ,
	                                     "none", error))
		return FALSE;

	if (!repeating)
	{
		g_autoptr(GDateTime) low = g_date_time_add_days(now, -1);
		g_autoptr(GDateTime) high = g_date_time_add_minutes(now, VENTURE_PLANNER_MAX_REMIND_MINUTES + 24 * 60);
		g_autofree gchar *low_text = venture_time_to_string(low);
		g_autofree gchar *high_text = venture_time_to_string(high);

		/* A day either side: an all-day event's stored midnight UTC is
		 * up to a day from the midnight it begins at in the zone. */
		if (!venture_query_add_filter_string(query, "starts-at", VENTURE_FILTER_OP_GT, low_text, error) ||
		    !venture_query_add_filter_string(query, "starts-at", VENTURE_FILTER_OP_LTE, high_text, error))
			return FALSE;
	}

	if (!venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, error))
		return FALSE;

	venture_query_set_limit(query, bound + 1);
	rows = venture_database_find(database, query, error);

	if (NULL == rows)
		return FALSE;

	if (rows->len > (guint)bound)
	{
		g_message("planner: more than %d %s events have reminders; the sweep "
		          "looked at the first %d by id", bound,
		          repeating ? "repeating" : "upcoming", bound);
		g_ptr_array_set_size(rows, bound);
	}

	for (i = 0; i < rows->len; i++)
		g_ptr_array_add(into, g_object_ref(g_ptr_array_index(rows, i)));

	return TRUE;
}

/* Tells everybody on @event about the occurrence at @instant. Returns how
 * many were told. */
static guint
planner_remind_people(
	VentureContext	*context,
	VentureEntity	*event,
	GDateTime	*instant
){
	g_autoptr(GArray) recipients = NULL;
	g_autoptr(GPtrArray) attendees = NULL;
	g_autoptr(GDateTime) local = NULL;
	g_autofree gchar *name = NULL;
	g_autofree gchar *where = NULL;
	g_autofree gchar *when = NULL;
	g_autofree gchar *title = NULL;
	g_autofree gchar *body = NULL;
	guint told;
	guint i;

	recipients = g_array_new(FALSE, FALSE, sizeof(gint64));
	planner_add_recipient(recipients, planner_int(event, "organizer-user-id"));

	attendees = venture_planner_attendees(venture_context_get_database(context), event, NULL, NULL);

	for (i = 0; (NULL != attendees) && (i < attendees->len); i++)
	{
		VentureEntity *attendee = g_ptr_array_index(attendees, i);

		/* Somebody who said no is not coming; telling them is noise. */
		if (VENTURE_ATTENDEE_RSVP_NO == planner_int_enum(attendee, "rsvp"))
			continue;

		planner_add_recipient(recipients, planner_int(attendee, "user-id"));
	}

	g_object_get(event, "name", &name, "location", &where, NULL);
	local = g_date_time_to_timezone(instant, venture_context_get_timezone(context));
	when = g_date_time_format(local, "%Y-%m-%d %H:%M");
	title = g_strdup_printf("Starting soon: %s", venture_string_is_empty(name) ? "Event" : name);
	body = venture_string_is_empty(where)
		? g_strdup_printf("Starts %s.", when)
		: g_strdup_printf("Starts %s at %s.", when, where);

	told = 0;

	for (i = 0; i < recipients->len; i++)
	{
		gint64 user_id = g_array_index(recipients, gint64, i);
		g_autoptr(GError) local_error = NULL;

		/* The same rule a mention follows: only somebody who may read
		 * the event is told about it, or a reminder would leak its name. */
		if (!venture_comment_user_can_read(context, user_id, event))
			continue;

		if (!venture_notify_send(context, user_id, VENTURE_NOTIFICATION_KIND_REMINDER,
		                         title, body, "event", venture_entity_get_id(event),
		                         name, NULL, &local_error))
		{
			g_message("planner: reminder for event %" G_GINT64_FORMAT " to user %"
			          G_GINT64_FORMAT " not written: %s", venture_entity_get_id(event),
			          user_id, local_error->message);
			continue;
		}

		told++;
	}

	return told;
}

gint
venture_planner_reminder_sweep(
	VentureContext	 *context,
	gint64		  organization_id,
	GDateTime	 *now,
	GError		**error
){
	VentureDatabase *database;
	g_autoptr(GPtrArray) events = NULL;
	g_autoptr(GDateTime) clock = NULL;
	GTimeZone *timezone;
	gint reminded;
	guint i;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), -1);

	if (!venture_context_module_enabled(context, "planner"))
		return 0;

	database = venture_context_get_database(context);

	/* A save below re-enters through automation, which may draw a page
	 * that sweeps: the second sweep would remind about the same event
	 * before the first had stamped it. */
	if (NULL != g_object_get_data(G_OBJECT(database), VENTURE_PLANNER_SWEEP_KEY))
		return 0;

	clock = (NULL != now) ? g_date_time_ref(now) : venture_time_now();
	timezone = venture_context_get_timezone(context);
	events = g_ptr_array_new_with_free_func(g_object_unref);

	if (!planner_reminder_candidates(database, organization_id, clock, FALSE, events, error) ||
	    !planner_reminder_candidates(database, organization_id, clock, TRUE, events, error))
		return -1;

	g_object_set_data(G_OBJECT(database), VENTURE_PLANNER_SWEEP_KEY, GINT_TO_POINTER(1));
	reminded = 0;

	for (i = 0; i < events->len; i++)
	{
		VentureEntity *event = g_ptr_array_index(events, i);
		g_autoptr(GDateTime) occurrence = NULL;
		g_autoptr(GDateTime) instant = NULL;
		g_autoptr(GDateTime) told_for = NULL;
		g_autoptr(GDateTime) search_from = NULL;
		gboolean all_day;
		gint64 lead;

		g_object_get(event, "all-day", &all_day, "reminded-for", &told_for, NULL);
		lead = planner_int(event, "remind-minutes");

		/* The next occurrence that has not begun for a person. An
		 * all-day date is a day behind or ahead of its local midnight,
		 * so search from a day earlier and judge by the instant. */
		search_from = g_date_time_add_days(clock, all_day ? -2 : 0);
		occurrence = venture_planner_next_occurrence(event, search_from);

		while (NULL != occurrence)
		{
			GDateTime *next;

			instant = planner_occurrence_instant(occurrence, all_day, timezone);

			if (g_date_time_compare(instant, clock) > 0)
				break;

			g_clear_pointer(&instant, g_date_time_unref);
			next = venture_planner_next_occurrence(event, occurrence);
			g_date_time_unref(occurrence);
			occurrence = next;
		}

		if ((NULL == occurrence) || (NULL == instant))
			continue;

		/* Not yet within the lead, or this occurrence was told already. */
		if (g_date_time_difference(instant, clock) > lead * G_TIME_SPAN_MINUTE)
			continue;

		if ((NULL != told_for) && venture_time_equal(told_for, occurrence))
			continue;

		planner_remind_people(context, event, instant);

		/* Stamped as the system, so the inbox's audit handler does not
		 * also tell watchers that the event "changed". */
		g_object_set(event, "reminded-for", occurrence, NULL);

		if (!venture_database_save(database, event, NULL, error))
		{
			g_object_set_data(G_OBJECT(database), VENTURE_PLANNER_SWEEP_KEY, NULL);
			return -1;
		}

		reminded++;
	}

	g_object_set_data(G_OBJECT(database), VENTURE_PLANNER_SWEEP_KEY, NULL);

	return reminded;
}

/* ==========================================================================
 * Budget
 * ========================================================================== */

GPtrArray *
venture_planner_event_spend(
	VentureDatabase	 *database,
	VentureEntity	 *event,
	GPtrArray	**out_expenses,
	GError		**error
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) expenses = NULL;
	g_autoptr(GPtrArray) totals = NULL;
	gint bound;
	guint i;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	g_return_val_if_fail(VENTURE_IS_EVENT(event), NULL);

	bound = venture_aggregate_get_max_rows();
	query = venture_query_new(VENTURE_TYPE_EXPENSE);
	venture_query_set_organization(query, venture_entity_get_organization_id(event));
	venture_query_set_limit(query, bound + 1);

	if (!venture_query_add_filter_int(query, "event-id", VENTURE_FILTER_OP_EQ,
	                                  venture_entity_get_id(event), error) ||
	    !venture_query_add_order(query, "occurred-at", VENTURE_SORT_DESCENDING, error) ||
	    !venture_query_add_order(query, "id", VENTURE_SORT_DESCENDING, error))
		return NULL;

	expenses = venture_database_find(database, query, error);

	if (NULL == expenses)
		return NULL;

	if (expenses->len > (guint)bound)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		            "More than %d expenses name this event; its spend is not totalled "
		            "from a part of them", bound);
		return NULL;
	}

	totals = venture_money_totals_new();

	for (i = 0; i < expenses->len; i++)
	{
		g_autoptr(VentureMoney) amount = NULL;

		g_object_get(g_ptr_array_index(expenses, i), "amount", &amount, NULL);

		if (!venture_money_totals_add(totals, amount, error))
			return NULL;
	}

	venture_money_totals_sort(totals, NULL);

	if (NULL != out_expenses)
		*out_expenses = g_steal_pointer(&expenses);

	return g_steal_pointer(&totals);
}

VentureMoney *
venture_planner_budget_remaining(
	const VentureMoney	 *budget,
	GPtrArray		 *spent,
	GError			**error
){
	const VentureMoney *same;

	g_return_val_if_fail(NULL != budget, NULL);

	same = (NULL != spent) ? venture_money_totals_lookup(spent, venture_money_get_currency(budget)) : NULL;

	if (NULL == same)
		return venture_money_copy(budget);

	return venture_money_subtract(budget, same, error);
}

/* ==========================================================================
 * Installation
 * ========================================================================== */

void
venture_planner_install(VentureContext *context)
{
	VentureDatabase *database;
	GTimeZone *timezone;

	g_return_if_fail(VENTURE_IS_CONTEXT(context));

	/* The calendar's sources live on each context: a plugin loaded into
	 * one adds to that one's calendar. */
	venture_calendar_register_builtin_sources(context);

	database = venture_context_get_database(context);

	/* The tests build several contexts over one database; validators are
	 * per database, so the second one must add nothing. */
	if (NULL != g_object_get_data(G_OBJECT(database), VENTURE_PLANNER_STATE_KEY))
		return;

	g_object_set_data(G_OBJECT(database), VENTURE_PLANNER_STATE_KEY,
	                  GINT_TO_POINTER(1));

	/* The zone an all-day event's day is read in: the first context's,
	 * which is the configured one. */
	timezone = venture_context_get_timezone(context);
	venture_database_add_save_validator(database, VENTURE_TYPE_EVENT,
		venture_planner_validate_event,
		(NULL != timezone) ? g_time_zone_ref(timezone) : g_time_zone_new_utc(),
		(GDestroyNotify)g_time_zone_unref);
	venture_database_add_save_validator(database, VENTURE_TYPE_EVENT_AGENDA_ITEM,
		venture_planner_validate_agenda_item, NULL, NULL);
	venture_database_add_save_validator(database, VENTURE_TYPE_EVENT_ATTENDEE,
		venture_planner_validate_attendee, NULL, NULL);
	venture_database_add_save_validator(database, VENTURE_TYPE_EXPENSE,
		venture_planner_validate_expense, NULL, NULL);
}
