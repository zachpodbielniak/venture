/*
 * venture-goals.h - Goals, their steps, progress and the materials still needed
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The goals module keeps measurable targets. A `goal` is a number that
 * starts somewhere and should reach somewhere else, perhaps by a date; its
 * `goal_step` rows are the ordered way there, some of them recipes to
 * craft a number of times. Two reports read them: `goal_progress` (how
 * far along each goal is, and when it will get there at the pace so far)
 * and `goal_materials` (the shopping list for the steps still ahead).
 * Nothing in the module writes one record because another changed: a
 * step marked done does not move its goal, and a goal whose current value
 * reaches the target is not marked achieved. See docs/goals.org.
 */

#ifndef VENTURE_GOALS_H
#define VENTURE_GOALS_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>

G_BEGIN_DECLS

/**
 * venture_goals_install:
 * @context: the wiring
 *
 * Installs the save validators for `goal` (a target that differs from the
 * start, a parent tree without loops in one organization, references in
 * the goal's organization, `achieved-at` derived from the status) and
 * `goal_step` (a goal in the step's organization, a recipe only while the
 * production module is on and only the organization's own, repetitions
 * not negative, a stretch that runs the goal's way, `done-at` derived
 * from `done`). Called once by the context; a second context over the
 * same database installs nothing twice.
 */
void
venture_goals_install(VentureContext *context);

/**
 * venture_goals_fraction:
 * @start: where the goal started
 * @current: where it stands
 * @target: where it should end; must differ from @start
 *
 * How far from @start toward @target @current is, as a fraction: 0 at the
 * start, 1 at the target, above 1 past it and below 0 behind the start.
 * The same formula serves a target above the start (a skill to raise) and
 * below it (a weight to lose), which is why it is never
 * @current / @target. Not clamped: the report shows the raw figure, and
 * only a bar stops at full.
 *
 * Returns: the fraction, or 0 when @target equals @start
 */
gdouble
venture_goals_fraction(
	gdouble	start,
	gdouble	current,
	gdouble	target
);

/**
 * venture_goals_progress:
 * @context: the wiring
 * @period: (nullable): unused beyond the result's heading; goals are not
 *   dated by a period
 * @options: (nullable): `venture_id`, `category_id` (and everything filed
 *   beneath it), `status` (one nick or several, comma separated),
 *   `as_of`, `organization_id`; see docs/goals.org
 * @error: (out) (optional): return location for a #GError
 *
 * One row per goal: its path in the goal tree, metric and unit, the three
 * values, the percentage covered and what remains, its steps done of
 * total, its due date and the days left, and a forecast date -- a straight
 * line from the start value at the goal's creation through the current
 * value at @as_of (now by default) extended to the target. The forecast
 * is blank for a goal with no progress or already achieved, and a note
 * says when it falls after the due date.
 *
 * Returns: (transfer full) (nullable): the result, or %NULL with @error set
 */
VentureReportResult *
venture_goals_progress(
	VentureContext		 *context,
	VentureDateRange	 *period,
	JsonObject		 *options,
	GError			**error
);

/**
 * venture_goals_materials:
 * @context: the wiring
 * @period: (nullable): unused beyond the result's heading
 * @options: (nullable): `goal_id` (the goal and its sub-goals),
 *   `venture_id`, `price_source`, `as_of`, `include_on_hand` (true by
 *   default), `organization_id`; see docs/goals.org
 * @error: (out) (optional): return location for a #GError
 *
 * The shopping list for the steps still ahead: for every step not done
 * that names a recipe and a number of repetitions, each consumed
 * component's quantity times the repetitions and each reusable
 * component's quantity once, summed per product across steps (reusables
 * by their largest need, not their sum), less what is on hand across all
 * stock; priced at the latest observation from @price_source when the
 * market module is on, else at the recorded cost. One total per currency.
 * A product with no price is named, never priced at zero. Refused while
 * the production module is off.
 *
 * Returns: (transfer full) (nullable): the result, or %NULL with @error set
 */
VentureReportResult *
venture_goals_materials(
	VentureContext		 *context,
	VentureDateRange	 *period,
	JsonObject		 *options,
	GError			**error
);

/**
 * venture_goals_register_reports:
 * @registry: the report registry
 *
 * Registers `goal_progress` and `goal_materials`. Both belong to the goals
 * module, so switching it off hides them.
 */
void
venture_goals_register_reports(VentureReportRegistry *registry);

G_END_DECLS

#endif /* VENTURE_GOALS_H */
