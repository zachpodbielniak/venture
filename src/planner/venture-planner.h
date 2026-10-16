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
