/*
 * venture-goals-records.h - Goals and the steps toward them
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The goals module's two record types. A goal is a measurable target --
 * a number that starts somewhere and should end somewhere else by a date;
 * a goal step is one ordered piece of the way there, optionally a recipe
 * to craft some number of times. Both are field tables and nothing else:
 * the rules that span rows live in venture-goals.c as save validators, so
 * every writer obeys them.
 */

#ifndef VENTURE_GOALS_RECORDS_H
#define VENTURE_GOALS_RECORDS_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

G_BEGIN_DECLS

#define VENTURE_TYPE_GOAL (venture_goal_get_type())
VENTURE_DECLARE_ENTITY(VentureGoal, venture_goal, GOAL)

#define VENTURE_TYPE_GOAL_STEP (venture_goal_step_get_type())
VENTURE_DECLARE_ENTITY(VentureGoalStep, venture_goal_step, GOAL_STEP)

G_END_DECLS

#endif /* VENTURE_GOALS_RECORDS_H */
