/*
 * venture-goals.c - The goals module's rules
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Save validators for goals and their steps. Deliberately absent: any
 * write from one record to another. A step marked done does not move its
 * goal's current value, and a current value that reaches the target does
 * not mark the goal achieved. Either would be a derived write -- a second
 * save the person did not make, a version bump that makes the object
 * they are holding conflict with itself, and a number that changes under
 * them. How far a step moves a goal is not knowable from the step (the
 * last 25 levels of a skill take longer than the first 25; a savings step
 * may overshoot), so the person who knows says so, on the goal.
 */

#include "venture.h"

#include <string.h>

#define VENTURE_GOALS_STATE_KEY "venture-goals-installed"

/* ==========================================================================
 * Shared
 * ========================================================================== */

gdouble
venture_goals_fraction(
	gdouble	start,
	gdouble	current,
	gdouble	target
){
	gdouble distance;

	distance = target - start;

	/* The validator refuses a goal with no distance; a row written
	 * behind its back reads as no progress rather than a division by
	 * zero. */
	if (0.0 == distance)
		return 0.0;

	return (current - start) / distance;
}

/* An integer property's value on @entity, 0 on a missing @entity. */
static gint64
goals_int(
	VentureEntity	*entity,
	const gchar	*property
){
	gint64 value;

	value = 0;

	if (NULL != entity)
		g_object_get(entity, property, &value, NULL);

	return value;
}

/*
 * Whether the production module is on. Recipes are its type; a hidden
 * type reads as not registered. Asked of the process-wide registry, the
 * thing the module switches mask, because a validator has a database and
 * not a context.
 */
static gboolean
goals_production_enabled(void)
{
	return G_TYPE_INVALID != venture_entity_registry_lookup(
		venture_entity_registry_get_default(), "recipe");
}

/*
 * Refuses a reference into another organization. The generic reference
 * check only asks whether the target exists; a goal in one organization
 * filed under another's venture would be reported in books that are not
 * its own. Only a value being written is judged, the rule every reference
 * follows, so a row pointing somewhere since moved stays editable.
 */
static gboolean
goals_same_organization(
	VentureDatabase	 *database,
	VentureEntity	 *entity,
	VentureEntity	 *previous,
	const gchar	 *property,
	GType		  target_type,
	const gchar	 *label,
	GError		**error
){
	g_autoptr(VentureEntity) target = NULL;
	gint64 target_id;

	target_id = goals_int(entity, property);

	if (target_id <= 0)
		return TRUE;

	if ((NULL != previous) && (goals_int(previous, property) == target_id))
		return TRUE;

	/* A target that does not exist is the reference check's to refuse,
	 * with its own message; this one only judges where it lives. */
	target = venture_database_get(database, target_type, target_id, NULL);

	if ((NULL != target) &&
	    (venture_entity_get_organization_id(target) !=
	     venture_entity_get_organization_id(entity)))
	{
		venture_set_error_validation(error, label,
			"#%" G_GINT64_FORMAT " belongs to another organization",
			target_id);
		return FALSE;
	}

	return TRUE;
}

/*
 * The factory's rule for a date that follows a state, shared by a goal's
 * achieved-at and a step's done-at:
 *
 *  - in the state (@now_in TRUE): an empty stamp is filled with now; a
 *    stamp given is kept, so "I finished this last Tuesday" survives;
 *  - leaving the state (@was_in TRUE, @now_in FALSE): the stamp is
 *    cleared, because a goal set back to active has not been achieved;
 *  - never in it: a stamp typed in is refused rather than kept, because a
 *    report reading it would count something that did not happen.
 */
static gboolean
goals_derive_stamp(
	VentureEntity	 *entity,
	const gchar	 *property,
	const gchar	 *label,
	gboolean	  now_in,
	gboolean	  was_in,
	const gchar	 *refusal,
	GError		**error
){
	g_autoptr(GDateTime) stamp = NULL;

	g_object_get(entity, property, &stamp, NULL);

	if (now_in)
	{
		if (NULL == stamp)
		{
			g_autoptr(GDateTime) now = NULL;

			now = venture_time_now();
			g_object_set(entity, property, now, NULL);
		}

		return TRUE;
	}

	if (was_in)
	{
		g_object_set(entity, property, NULL, NULL);
		return TRUE;
	}

	if (NULL != stamp)
	{
		venture_set_error_validation(error, label, "%s", refusal);
		return FALSE;
	}

	return TRUE;
}

/* ==========================================================================
 * Goals
 * ========================================================================== */

/*
 * A goal's rules, in the order a person would fix them:
 *
 *  - a target that differs from the start. There is no way to tell a
 *    target left empty from one of 0 in a double field, so a goal of 0
 *    to 0 is refused as having no target -- the same answer either way;
 *  - achieved-at follows the status (goals_derive_stamp());
 *  - a parent that is a goal of the same organization and not the goal
 *    itself or one beneath it: venture_category_check_tree_node(), the
 *    one definition of a loop;
 *  - its venture and category in its own organization. That a category
 *    groups goals is the category module's own reference check.
 */
static gboolean
venture_goals_validate_goal(
	VentureDatabase	 *database,
	VentureEntity	 *entity,
	VentureEntity	 *previous,
	gpointer	  user_data,
	GError		**error
){
	VentureGoalStatus status;
	VentureGoalStatus was;
	gdouble start;
	gdouble target;

	(void)user_data;

	g_object_get(entity, "status", &status, "start-value", &start,
	             "target-value", &target, NULL);

	/* --- The distance --- */

	if (target == start)
	{
		venture_set_error_validation(error, "Target",
			"must differ from the start (both are %g); a goal needs somewhere "
			"to get to -- a target below the start is fine, for something to "
			"reduce", start);
		return FALSE;
	}

	/* --- When it was achieved --- */

	was = VENTURE_GOAL_STATUS_ACTIVE;

	if (NULL != previous)
		g_object_get(previous, "status", &was, NULL);

	if (!goals_derive_stamp(entity, "achieved-at", "Achieved",
	                        VENTURE_GOAL_STATUS_ACHIEVED == status,
	                        VENTURE_GOAL_STATUS_ACHIEVED == was,
	                        "is for an achieved goal; set the status to achieved, "
	                        "or clear the date", error))
		return FALSE;

	/* --- Where it sits --- */

	if (!venture_category_check_tree_node(database, entity, error))
		return FALSE;

	return goals_same_organization(database, entity, previous, "venture-id",
	                               VENTURE_TYPE_VENTURE, "Venture", error) &&
	       goals_same_organization(database, entity, previous, "category-id",
	                               VENTURE_TYPE_CATEGORY, "Category", error);
}

/* ==========================================================================
 * Goal steps
 * ========================================================================== */

/*
 * A step's rules:
 *
 *  - a goal, in the step's own organization (held always: a step is part
 *    of its goal, not a reference out);
 *  - repetitions not negative -- 0 is "not said";
 *  - a stretch (from-value to to-value) that, when it moves at all, moves
 *    the way the goal does: a step from 25 down to 1 on a goal from 1 up
 *    to 300 is a typo that would read as progress backwards;
 *  - a recipe only while the production module is on, and only the
 *    organization's own. Judged only when written, so a step recorded
 *    before production was switched off stays editable;
 *  - done-at follows done (goals_derive_stamp()).
 */
static gboolean
venture_goals_validate_step(
	VentureDatabase	 *database,
	VentureEntity	 *entity,
	VentureEntity	 *previous,
	gpointer	  user_data,
	GError		**error
){
	g_autoptr(VentureEntity) goal = NULL;
	gint64 goal_id;
	gint64 recipe_id;
	gint64 repetitions;
	gdouble from_value;
	gdouble to_value;
	gboolean done;
	gboolean was_done;

	(void)user_data;

	g_object_get(entity, "goal-id", &goal_id, "recipe-id", &recipe_id,
	             "repetitions", &repetitions, "from-value", &from_value,
	             "to-value", &to_value, "done", &done, NULL);

	/* --- The goal --- */

	if (goal_id <= 0)
	{
		venture_set_error_validation(error, "Goal", "is required");
		return FALSE;
	}

	goal = venture_database_get(database, VENTURE_TYPE_GOAL, goal_id, NULL);

	if (NULL == goal)
	{
		venture_set_error_validation(error, "Goal",
			"#%" G_GINT64_FORMAT " does not exist", goal_id);
		return FALSE;
	}

	if (venture_entity_get_organization_id(goal) !=
	    venture_entity_get_organization_id(entity))
	{
		venture_set_error_validation(error, "Goal",
			"#%" G_GINT64_FORMAT " belongs to another organization", goal_id);
		return FALSE;
	}

	/* --- The numbers --- */

	if (repetitions < 0)
	{
		venture_set_error_validation(error, "Crafts",
			"cannot be negative; leave it at 0 when the step does not say");
		return FALSE;
	}

	if (to_value != from_value)
	{
		gdouble start;
		gdouble target;

		g_object_get(goal, "start-value", &start, "target-value", &target, NULL);

		if ((target - start) * (to_value - from_value) < 0.0)
		{
			venture_set_error_validation(error, "To",
				"runs from %g to %g, the opposite way to its goal (%g to %g)",
				from_value, to_value, start, target);
			return FALSE;
		}
	}

	/* --- The recipe --- */

	if ((recipe_id > 0) &&
	    ((NULL == previous) || (goals_int(previous, "recipe-id") != recipe_id)))
	{
		if (!goals_production_enabled())
		{
			venture_set_error_validation(error, "Recipe",
				"recipes belong to the production module, which is off; "
				"describe the step in its name instead, or turn production on");
			return FALSE;
		}

		if (!goals_same_organization(database, entity, previous, "recipe-id",
		                             VENTURE_TYPE_RECIPE, "Recipe", error))
			return FALSE;
	}

	/* --- When it was done --- */

	was_done = FALSE;

	if (NULL != previous)
		g_object_get(previous, "done", &was_done, NULL);

	return goals_derive_stamp(entity, "done-at", "Done at", done, was_done,
	                          "is for a done step; tick Done, or clear the date",
	                          error);
}

/* ==========================================================================
 * Installation
 * ========================================================================== */

void
venture_goals_install(VentureContext *context)
{
	VentureDatabase *database;

	g_return_if_fail(VENTURE_IS_CONTEXT(context));

	database = venture_context_get_database(context);

	/* The tests build several contexts over one database; validators are
	 * per database, so the second one must add nothing. */
	if (NULL != g_object_get_data(G_OBJECT(database), VENTURE_GOALS_STATE_KEY))
		return;

	g_object_set_data(G_OBJECT(database), VENTURE_GOALS_STATE_KEY,
	                  GINT_TO_POINTER(1));

	venture_database_add_save_validator(database, VENTURE_TYPE_GOAL,
	                                    venture_goals_validate_goal, NULL, NULL);
	venture_database_add_save_validator(database, VENTURE_TYPE_GOAL_STEP,
	                                    venture_goals_validate_step, NULL, NULL);
}
