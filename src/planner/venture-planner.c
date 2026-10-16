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
}
