/*
 * venture-goals-records.c - Goals and the steps toward them
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "venture.h"

/* ==========================================================================
 * Goals
 *
 * A measurable target: a number that starts at one value and should reach
 * another, perhaps by a date. A profession skill from 1 to 300, 5,000 gold
 * saved, 10 kg lost, 12 titles shipped -- nothing here knows which. What
 * is measured is `metric` in `unit`; how far along it is follows from the
 * three values.
 *
 * The target may be below the start (a weight to lose, a backlog to burn
 * down), so progress is always (current - start) / (target - start) and
 * never current / target. The validator refuses a target equal to the
 * start, because that goal has no distance to cover and every percentage
 * of it is a division by zero.
 *
 * `parent-id` nests goals: "reach 300" over "reach 75", "reach 150".
 * The tree is held to the same rules as a category tree -- no loops, one
 * organization -- by the same check (venture-category.c).
 *
 * `achieved-at` is derived from the status, the factory's rule for a date:
 * stamped when the goal becomes achieved and it is empty, kept when one is
 * given, cleared when the goal leaves achieved.
 * ========================================================================== */

static const VentureFieldDecl venture_goal_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", "What you are aiming for"),
	VENTURE_FIELD_REF("venture-id", "Venture", "Optional: the venture it is for",
	                  "venture", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("parent-id", "Part of", "Optional: the larger goal this is a stage of",
	                  "goal", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("category-id", "Category", "Optional: where it is filed",
	                  "category", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("metric", "Metric",
	              "What is measured: skill level, savings, weight, titles shipped",
	              VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_INDEXED | VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD("unit", "Unit", "Optional: what one of the metric is -- level, gold, kg",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("start-value", "Start", "Where it stood when the goal was set",
	              VENTURE_FIELD_KIND_DOUBLE, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("current-value", "Current", "Where it stands now; update it as you go",
	              VENTURE_FIELD_KIND_DOUBLE, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("target-value", "Target",
	              "Where it should end; may be below the start, never equal to it",
	              VENTURE_FIELD_KIND_DOUBLE, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("due-on", "Due", "Optional: the date it should be reached by",
	              VENTURE_FIELD_KIND_DATE, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_ENUM("status", "Status", "Active until you pause, achieve or abandon it",
	                   venture_goal_status_get_type, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("achieved-at", "Achieved",
	              "When it was reached; stamped when the status becomes achieved",
	              VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("tags", "Tags", "Comma separated",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD_TEXT("notes", "Notes", NULL)
};

VENTURE_DEFINE_ENTITY_WITH_CODE(VentureGoal, venture_goal, venture_goal_fields,
	venture_entity_class_set_labels(VENTURE_ENTITY_CLASS(klass), "Goal", NULL);)

/* ==========================================================================
 * Goal steps
 *
 * One ordered piece of the way to a goal: "craft 20 Minor Healing Potions
 * to go from 1 to 25", "put 500 aside in March", "run three 5 km weeks".
 * `position` orders them; `from-value` and `to-value` say which stretch of
 * the goal's metric the step covers, and are for reading -- nothing moves
 * the goal's current value when a step is done (see venture-goals.c).
 *
 * A step may name a recipe (the production module) and a number of
 * repetitions: that is what the goal_materials report turns into a
 * shopping list. `repetitions` counts crafts -- batches of the recipe --
 * not units made; 0 means the step does not say.
 *
 * `done` is named for completion so the zero value, not done, is the safe
 * one: a step whose box nobody ticked is still ahead, and still on the
 * shopping list. `done-at` follows it as `achieved-at` follows a goal.
 * ========================================================================== */

static const VentureFieldDecl venture_goal_step_fields[] = {
	VENTURE_FIELD_REF("goal-id", "Goal", "The goal this step belongs to",
	                  "goal", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("position", "Order", "Where it comes among the goal's steps; lowest first",
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_NAME("name", "Name", "What this step is"),
	VENTURE_FIELD("from-value", "From", "Optional: where on the goal's metric it starts",
	              VENTURE_FIELD_KIND_DOUBLE, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("to-value", "To", "Optional: where on the goal's metric it ends",
	              VENTURE_FIELD_KIND_DOUBLE, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("recipe-id", "Recipe", "Optional: a recipe this step crafts",
	                  "recipe", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("repetitions", "Crafts",
	              "How many times to make the recipe (batches, not units); 0 if unsaid",
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("done", "Done", "Finished; a done step leaves the shopping list",
	              VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("done-at", "Done at", "When it was finished; stamped when marked done",
	              VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("notes", "Notes", NULL)
};

VENTURE_DEFINE_ENTITY_WITH_CODE(VentureGoalStep, venture_goal_step, venture_goal_step_fields,
	venture_entity_class_set_labels(VENTURE_ENTITY_CLASS(klass), "Goal step", NULL);)
