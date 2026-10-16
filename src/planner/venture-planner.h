/*
 * venture-planner.h - The planner module's rules and run-sheets
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Save validators for events, agenda items and attendees; the run-sheet
 * of an event with each item's running start time; the event's
 * preparation tickets. The calendar over every dated record is
 * venture-calendar-view.h.
 */

#ifndef VENTURE_PLANNER_H
#define VENTURE_PLANNER_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>

G_BEGIN_DECLS

/**
 * VENTURE_PLANNER_MAX_AGENDA:
 *
 * The most agenda items or preparation tickets one event page reads. A
 * run-sheet, not a ledger: past it the list pages are the way to page
 * through them.
 */
#define VENTURE_PLANNER_MAX_AGENDA (500)

/**
 * VenturePlannerAgendaLine:
 * @item: (transfer full): the agenda item
 * @offset_minutes: minutes from the event's start to this item's
 * @starts: (nullable): when it starts, %NULL for an all-day event, whose
 *   run-sheet has no clock
 * @ends: (nullable): when it ends, %NULL likewise
 *
 * One line of a run-sheet.
 */
typedef struct
{
	VentureEntity	*item;
	gint64		 offset_minutes;
	GDateTime	*starts;
	GDateTime	*ends;
} VenturePlannerAgendaLine;

/**
 * VenturePlannerAgenda:
 * @lines: (element-type VenturePlannerAgendaLine): the items in order
 * @total_minutes: every item's minutes added up
 * @overruns: the run-sheet ends after the event does
 *
 * An event's run-sheet.
 */
typedef struct
{
	GPtrArray	*lines;
	gint64		 total_minutes;
	gboolean	 overruns;
} VenturePlannerAgenda;

/**
 * venture_planner_agenda_free:
 * @agenda: (nullable): an agenda
 */
void venture_planner_agenda_free(VenturePlannerAgenda *agenda);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(VenturePlannerAgenda, venture_planner_agenda_free)

/**
 * venture_planner_agenda:
 * @database: the database
 * @event: an event
 * @error: (out) (optional): the error
 *
 * The event's agenda items by position (then id), each with its running
 * start: the event's start plus every earlier item's minutes. Computed on
 * every call, never stored, so lengthening one item moves every item after
 * it. An all-day event's lines carry offsets but no clock times.
 *
 * Returns: (transfer full) (nullable): the run-sheet
 */
VenturePlannerAgenda *
venture_planner_agenda(
	VentureDatabase	 *database,
	VentureEntity	 *event,
	GError		**error
);

/**
 * venture_planner_prep_tickets:
 * @database: the database
 * @event: an event
 * @out_done: (out) (optional): how many are done
 * @out_total: (out) (optional): how many count: every one but the
 *   cancelled, which did not happen
 * @error: (out) (optional): the error
 *
 * The tickets naming @event in their `event-id`: the work to prepare it.
 * Ordinary tickets -- assigned, due and tracked on the board like any
 * other -- so the planner adds a reference, not a second task list.
 *
 * Returns: (transfer full) (nullable) (element-type VentureEntity): the
 *   tickets, cancelled included, by due date
 */
GPtrArray *
venture_planner_prep_tickets(
	VentureDatabase	 *database,
	VentureEntity	 *event,
	guint		 *out_done,
	guint		 *out_total,
	GError		**error
);

/**
 * VENTURE_PLANNER_RSVP_COUNT:
 *
 * How many answers an attendee can give: the length of the counts array
 * venture_planner_attendees() fills, indexed by #VentureAttendeeRsvp.
 */
#define VENTURE_PLANNER_RSVP_COUNT (4)

/**
 * venture_planner_attendees:
 * @database: the database
 * @event: an event
 * @counts: (out caller-allocates) (array fixed-size=4) (optional): how many
 *   gave each #VentureAttendeeRsvp answer
 * @error: (out) (optional): the error
 *
 * The event's attendees, organizers first, then by id, with the answers
 * counted -- one count for the page and the API alike.
 *
 * Returns: (transfer full) (nullable) (element-type VentureEntity): the
 *   attendees
 */
GPtrArray *
venture_planner_attendees(
	VentureDatabase	 *database,
	VentureEntity	 *event,
	guint		 *counts,
	GError		**error
);

/* --- Occurrences ------------------------------------------------------------ */

/**
 * VENTURE_PLANNER_MAX_REMIND_MINUTES:
 *
 * The longest an event's reminder may lead it by: four weeks. The sweep
 * reads events starting within this horizon, so the bound is what keeps
 * its query narrow.
 */
#define VENTURE_PLANNER_MAX_REMIND_MINUTES (40320)

/**
 * venture_planner_occurrence_nth:
 * @starts: a series' first start
 * @recurrence: how it repeats
 * @n: which occurrence, 0 being the first
 *
 * The @n-th start of a series, always counted from the first so a month
 * that clamps to the 28th does not drag every later one with it. The
 * calendar, the reminder sweep and the CalDAV push all ask this.
 *
 * Returns: (transfer full) (nullable): the start, or %NULL past the only
 *   occurrence of an event that does not repeat
 */
GDateTime *
venture_planner_occurrence_nth(
	GDateTime		*starts,
	VentureEventRecurrence	 recurrence,
	gint64			 n
);

/**
 * venture_planner_occurrence_first_index:
 * @starts: a series' first start
 * @length: how long one occurrence lasts
 * @recurrence: how it repeats
 * @from: the instant of interest
 *
 * The first occurrence index worth trying for @from: far enough back that
 * an occurrence begun before @from and still running is not missed, never
 * before the first. Without it a weekly meeting begun ten years ago would
 * walk five hundred weeks to reach this month.
 *
 * Returns: the index, at least 0
 */
gint64
venture_planner_occurrence_first_index(
	GDateTime		*starts,
	GTimeSpan		 length,
	VentureEventRecurrence	 recurrence,
	GDateTime		*from
);

/**
 * venture_planner_next_occurrence:
 * @event: an event
 * @after: an instant
 *
 * The first start of @event strictly after @after, honouring
 * `recurs-until` (its whole last day). For an all-day event the answer is
 * the stored calendar date, midnight UTC.
 *
 * Returns: (transfer full) (nullable): the start, or %NULL when none is
 *   left
 */
GDateTime *
venture_planner_next_occurrence(
	VentureEntity	*event,
	GDateTime	*after
);

/* --- Reminders --------------------------------------------------------------- */

/**
 * venture_planner_reminder_sweep:
 * @context: the context
 * @organization_id: one organization's events, or 0 for every one
 * @now: (nullable): the instant judged against; %NULL is now
 * @error: (out) (optional): the error
 *
 * Tells people about events starting soon: for every event with
 * `remind-minutes` set, not cancelled, whose next occurrence starts within
 * that many minutes, the organizer and every attendee who is a user and
 * has not said no get a reminder in their inbox -- each only if they may
 * read the event. The occurrence is stamped in `reminded-for`, so a sweep
 * tells about it once. An occurrence that has already begun is not
 * reminded about: a late reminder is noise.
 *
 * Lazy, like the service-level sweep: run from the inbox, the board and
 * the calendar, and from `POST /api/v1/calendar/reminders`. No timer and
 * no thread -- nothing but the main thread writes the database.
 *
 * Returns: how many events were reminded about, or -1 with @error set
 */
gint
venture_planner_reminder_sweep(
	VentureContext	 *context,
	gint64		  organization_id,
	GDateTime	 *now,
	GError		**error
);

/* --- Budget -------------------------------------------------------------------- */

/**
 * venture_planner_event_spend:
 * @database: the database
 * @event: an event
 * @out_expenses: (out) (optional) (transfer full) (element-type VentureEntity):
 *   the expenses counted, newest first
 * @error: (out) (optional): the error
 *
 * What an event has cost: its expenses' amounts totalled per currency,
 * never across two. Refused past venture_aggregate_get_max_rows()
 * expenses -- a total of the first N would read as the whole cost.
 *
 * Returns: (transfer full) (nullable) (element-type VentureMoney): the
 *   totals from venture_money_totals_new(), empty when nothing was spent
 */
GPtrArray *
venture_planner_event_spend(
	VentureDatabase	 *database,
	VentureEntity	 *event,
	GPtrArray	**out_expenses,
	GError		**error
);

/**
 * venture_planner_budget_remaining:
 * @budget: the event's budget
 * @spent: (element-type VentureMoney): the totals from
 *   venture_planner_event_spend()
 * @error: (out) (optional): overflow
 *
 * What is left of @budget: the budget less what was spent in the
 * budget's own currency. Spending in another currency is not taken off --
 * nothing converts -- and the page lists it beside the figure instead.
 * Negative when the event is over budget.
 *
 * Returns: (transfer full) (nullable): the remainder, in @budget's currency
 */
VentureMoney *
venture_planner_budget_remaining(
	const VentureMoney	 *budget,
	GPtrArray		 *spent,
	GError			**error
);

/**
 * venture_planner_install:
 * @context: the context
 *
 * Installs the planner's save validators on the context's database (once
 * per database) and registers the built-in calendar sources on the
 * context.
 */
void venture_planner_install(VentureContext *context);

G_END_DECLS

#endif /* VENTURE_PLANNER_H */
