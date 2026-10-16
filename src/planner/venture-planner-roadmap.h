/*
 * venture-planner-roadmap.h - Goals, the milestones toward them and the
 * epics in those, on one timeline
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The roadmap is the planning hierarchy drawn against time: a goal, the
 * milestones naming it, the epics planned into each milestone (or naming
 * the goal directly), then whatever has no goal. It is computed here, once
 * -- the page /roadmap, its twin /api/v1/roadmap and `venturectl roadmap`
 * all read venture_planner_roadmap() -- and nothing is stored: a bar is a
 * record's own dates, its fill the same progress the record's page shows.
 */

#ifndef VENTURE_PLANNER_ROADMAP_H
#define VENTURE_PLANNER_ROADMAP_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/**
 * VENTURE_PLANNER_ROADMAP_MAX_ROWS:
 *
 * The most goals, milestones or epics one roadmap reads of each. Every epic
 * row walks its tree for its progress, so the bound is what keeps a page
 * view from walking ten thousand trees; past it the roadmap is refused and
 * says how to narrow it, never drawn from the first N.
 */
#define VENTURE_PLANNER_ROADMAP_MAX_ROWS (500)

/**
 * VENTURE_PLANNER_ROADMAP_MAX_DAYS:
 *
 * The widest window a roadmap may be drawn over: three years.
 */
#define VENTURE_PLANNER_ROADMAP_MAX_DAYS (1096)

/**
 * VenturePlannerRoadmapRow:
 * @type: goal, milestone or epic -- the record type, epics being tickets
 * @record_type: the record type behind the row: goal, milestone or ticket
 * @id: the record's id
 * @title: its name
 * @depth: how far it is indented: a goal 0, its milestones 1, their epics
 *   2; an epic directly under a goal is 1, and rows with no goal start at 0
 * @start: (nullable): when the bar begins: the record's creation
 * @end: (nullable): when it is due; %NULL draws a start marker only
 * @status: the record's status nick
 * @done: finished, whichever way: achieved, completed, done, cancelled
 * @overdue: due before now and not finished
 * @work_done: tickets finished beneath it
 * @work_total: tickets beneath it, cancelled ones left out
 * @url: the record's page
 *
 * One bar on the roadmap.
 */
typedef struct
{
	const gchar	*type;
	const gchar	*record_type;
	gint64		 id;
	gchar		*title;
	gint		 depth;
	GDateTime	*start;
	GDateTime	*end;
	gchar		*status;
	gboolean	 done;
	gboolean	 overdue;
	gint64		 work_done;
	gint64		 work_total;
	gchar		*url;
} VenturePlannerRoadmapRow;

/**
 * VenturePlannerRoadmapQuery:
 * @organization_id: the organization read
 * @goal_id: one goal and what hangs off it, or 0 for everything
 * @venture_id: one venture's records, or 0
 * @include_done: also show finished goals, milestones and epics
 * @from: (nullable): the window's first day; %NULL is the first of last month
 * @to: (nullable): the window's end, exclusive; %NULL is seven months on
 * @now: (nullable): the instant overdue is judged against; %NULL is now
 *
 * What a roadmap is asked for. Filled field by field after
 * venture_planner_roadmap_query_init(), which zeroes it.
 */
typedef struct
{
	gint64		 organization_id;
	gint64		 goal_id;
	gint64		 venture_id;
	gboolean	 include_done;
	GDateTime	*from;
	GDateTime	*to;
	GDateTime	*now;
} VenturePlannerRoadmapQuery;

/**
 * venture_planner_roadmap_query_init:
 * @query: (out caller-allocates): the query to clear
 */
void venture_planner_roadmap_query_init(VenturePlannerRoadmapQuery *query);

/**
 * VenturePlannerRoadmap:
 * @rows: (element-type VenturePlannerRoadmapRow): the rows, in the order
 *   they are drawn
 * @notes: (element-type utf8): what was left out and why
 * @from: the window's first instant
 * @to: the window's end, exclusive
 * @now: the instant judged against
 *
 * A roadmap: rows and the window they are drawn in.
 */
typedef struct
{
	GPtrArray	*rows;
	GPtrArray	*notes;
	GDateTime	*from;
	GDateTime	*to;
	GDateTime	*now;
} VenturePlannerRoadmap;

/**
 * venture_planner_roadmap_free:
 * @roadmap: (nullable): a roadmap
 */
void venture_planner_roadmap_free(VenturePlannerRoadmap *roadmap);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(VenturePlannerRoadmap, venture_planner_roadmap_free)

/**
 * venture_planner_roadmap:
 * @context: the context
 * @query: what is asked
 * @error: (out) (optional): the error
 *
 * Builds the roadmap. Goals are read only while the goals module is on and
 * milestones and epics only while tickets is; a hidden level is skipped and
 * its rows move up. Refused with %VENTURE_ERROR_VALIDATION while the planner
 * is off, for a window ending before it begins or wider than
 * %VENTURE_PLANNER_ROADMAP_MAX_DAYS, and past
 * %VENTURE_PLANNER_ROADMAP_MAX_ROWS of any one kind.
 *
 * Returns: (transfer full) (nullable): the roadmap
 */
VenturePlannerRoadmap *
venture_planner_roadmap(
	VentureContext				 *context,
	const VenturePlannerRoadmapQuery	 *query,
	GError					**error
);

/**
 * venture_planner_roadmap_fraction:
 * @roadmap: a roadmap
 * @when: an instant
 *
 * Where @when falls across the window, 0 at its start and 1 at its end,
 * clamped: the page positions every bar and tick with it.
 *
 * Returns: the fraction
 */
gdouble
venture_planner_roadmap_fraction(
	const VenturePlannerRoadmap	*roadmap,
	GDateTime			*when
);

/**
 * venture_planner_roadmap_to_json:
 * @roadmap: a roadmap
 *
 * The one rendering of a roadmap as data.
 *
 * Returns: (transfer full): a JSON object
 */
JsonNode *venture_planner_roadmap_to_json(const VenturePlannerRoadmap *roadmap);

G_END_DECLS

#endif /* VENTURE_PLANNER_ROADMAP_H */
