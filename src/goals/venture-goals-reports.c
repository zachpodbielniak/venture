/*
 * venture-goals-reports.c - How far along each goal is, and what is left to buy
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Two reports. goal_progress answers "how far along am I, and when will I
 * get there at this pace": one row per goal with the percentage covered,
 * what remains, the steps done and a straight-line forecast. goal_materials
 * answers "what do I still need to buy": the recipe steps not yet done,
 * multiplied out into components, less what is on the shelf, priced. The
 * figures are arithmetic over the records, the same every time -- nothing
 * here writes.
 */

#include "venture.h"

#include <math.h>
#include <string.h>

/* A forecast further out than this is no forecast: a pace that would
 * take a century is "not at this pace", and a GDateTime cannot hold much
 * more anyway. */
#define GOALS_FORECAST_MAX_DAYS (36525.0)

/* A pace measured over less than this is no pace: a goal created a minute
 * ago with progress already recorded (the usual way one is entered) would
 * extend "half done in a minute" to "done in another minute" and forecast
 * today. A day is the smallest span a calendar-date forecast can mean. */
#define GOALS_FORECAST_MIN_SECONDS (86400.0)

/* Step ids are read in batches of this many, so an IN list stays well
 * under every backend's bound on bound parameters. */
#define GOALS_IN_BATCH (500)

/* ==========================================================================
 * Shared
 * ========================================================================== */

/* The organisation the report reads, checked to exist, as every report in
 * the recent modules does: "no goals" about an organisation that is not
 * there would be a different and wrong answer. */
static gboolean
goals_report_organization(
	VentureContext	 *context,
	JsonObject	 *options,
	gint64		 *out_id,
	GError		**error
){
	g_autoptr(VentureEntity) organization = NULL;
	gint64 id;

	id = venture_context_get_default_organization_id(context);

	if (NULL != options)
		id = venture_json_object_get_int(options, "organization_id", id);

	organization = venture_database_get(venture_context_get_database(context),
	                                    VENTURE_TYPE_ORGANIZATION, id, NULL);

	if ((NULL == organization) || venture_entity_is_deleted(organization))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		            "Organization #%" G_GINT64_FORMAT " not found", id);
		return FALSE;
	}

	*out_id = id;

	return TRUE;
}

static gint64
goals_report_int(
	VentureEntity	*entity,
	const gchar	*property
){
	gint64 value;

	value = 0;
	g_object_get(entity, property, &value, NULL);

	return value;
}

/* A goal that must exist, live, in @organization_id -- asked for by id,
 * so a missing one is refused rather than answered as an empty list. */
static gboolean
goals_report_require_goal(
	VentureDatabase	 *database,
	gint64		  organization_id,
	gint64		  goal_id,
	GError		**error
){
	g_autoptr(VentureEntity) goal = NULL;

	goal = venture_database_get(database, VENTURE_TYPE_GOAL, goal_id, NULL);

	if ((NULL == goal) || venture_entity_is_deleted(goal) ||
	    (venture_entity_get_organization_id(goal) != organization_id))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		            "Goal #%" G_GINT64_FORMAT " not found", goal_id);
		return FALSE;
	}

	return TRUE;
}

/* @ids as the text operands an IN filter takes. */
static GPtrArray *
goals_report_operands(GArray *ids)
{
	GPtrArray *operands;
	guint i;

	operands = g_ptr_array_new_with_free_func(g_free);

	for (i = 0; i < ids->len; i++)
		g_ptr_array_add(operands, g_strdup_printf("%" G_GINT64_FORMAT,
			g_array_index(ids, gint64, i)));

	return operands;
}

/*
 * The live goals of the organisation, narrowed in the query to @venture_id,
 * to goals filed in @categories, to the goals in @ids and to the statuses
 * @statuses marks (each %NULL or 0 for no narrowing), bounded like every
 * report here and refused past the bound rather than truncated: fewer
 * goals presented as all of them is the failure this avoids.
 *
 * Every narrowing is in the query, never applied to the rows afterwards:
 * the bound counts what was fetched, so a filter run in C after it let
 * an organisation past the bound be refused even when the question was
 * narrowed -- by a refusal that advised narrowing.
 */
static GPtrArray *
goals_report_goals(
	VentureDatabase	 *database,
	gint64		  organization_id,
	gint64		  venture_id,
	GArray		 *categories,
	GArray		 *ids,
	const gboolean	 *statuses,
	GError		**error
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) goals = NULL;

	query = venture_query_new(VENTURE_TYPE_GOAL);
	venture_query_set_organization(query, organization_id);
	venture_query_set_limit(query, (guint)venture_aggregate_get_max_rows() + 1);

	if ((0 != venture_id) &&
	    !venture_query_add_filter_int(query, "venture-id", VENTURE_FILTER_OP_EQ,
	                                  venture_id, error))
		return NULL;

	if (NULL != categories)
	{
		g_autoptr(GPtrArray) operands = NULL;

		/* An empty set asked for is nothing, never everything. */
		if (0 == categories->len)
			return g_ptr_array_new_with_free_func(g_object_unref);

		operands = goals_report_operands(categories);

		if (!venture_query_add_filter(query, "category-id", VENTURE_FILTER_OP_IN,
		                              operands, error))
			return NULL;
	}

	if (NULL != ids)
	{
		g_autoptr(GPtrArray) operands = NULL;

		/* An empty set asked for is nothing, never everything. */
		if (0 == ids->len)
			return g_ptr_array_new_with_free_func(g_object_unref);

		operands = goals_report_operands(ids);

		if (!venture_query_add_filter(query, "id", VENTURE_FILTER_OP_IN,
		                              operands, error))
			return NULL;
	}

	if (NULL != statuses)
	{
		g_autoptr(GPtrArray) operands = NULL;
		gint status;

		operands = g_ptr_array_new_with_free_func(g_free);

		for (status = 0; status <= VENTURE_GOAL_STATUS_ABANDONED; status++)
			if (statuses[status])
				g_ptr_array_add(operands, g_strdup(venture_enum_to_nick(
					VENTURE_TYPE_GOAL_STATUS, status)));

		/* No status at all asked for ("status=,") matches no goal,
		 * and IN cannot be given an empty list to say so. */
		if (0 == operands->len)
			return g_ptr_array_new_with_free_func(g_object_unref);

		if (!venture_query_add_filter(query, "status", VENTURE_FILTER_OP_IN,
		                              operands, error))
			return NULL;
	}

	if (!venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, error))
		return NULL;

	goals = venture_database_find(database, query, error);

	if (NULL == goals)
		return NULL;

	if (goals->len > (guint)venture_aggregate_get_max_rows())
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "More than %d goals match; narrow by venture_id, category_id "
		            "or goal_id", venture_aggregate_get_max_rows());
		return NULL;
	}

	return g_steal_pointer(&goals);
}

/*
 * The live steps of the goals in @goals, keyed by nothing -- the caller
 * groups them. Read in batches by goal id, so a report over many goals
 * issues a handful of queries and never one per goal.
 */
static GPtrArray *
goals_report_steps(
	VentureDatabase	 *database,
	gint64		  organization_id,
	GPtrArray	 *goals,
	GError		**error
){
	g_autoptr(GPtrArray) steps = NULL;
	guint offset;

	steps = g_ptr_array_new_with_free_func(g_object_unref);

	for (offset = 0; offset < goals->len; offset += GOALS_IN_BATCH)
	{
		g_autoptr(VentureQuery) query = NULL;
		g_autoptr(GPtrArray) ids = NULL;
		g_autoptr(GPtrArray) found = NULL;
		guint i;

		ids = g_ptr_array_new_with_free_func(g_free);

		for (i = offset; (i < goals->len) && (i < offset + GOALS_IN_BATCH); i++)
			g_ptr_array_add(ids, g_strdup_printf("%" G_GINT64_FORMAT,
				venture_entity_get_id(g_ptr_array_index(goals, i))));

		query = venture_query_new(VENTURE_TYPE_GOAL_STEP);
		venture_query_set_organization(query, organization_id);
		venture_query_set_limit(query, 0);

		if (!venture_query_add_filter(query, "goal-id", VENTURE_FILTER_OP_IN, ids,
		                              error) ||
		    !venture_query_add_order(query, "position", VENTURE_SORT_ASCENDING, error) ||
		    !venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, error))
			return NULL;

		found = venture_database_find(database, query, error);

		if (NULL == found)
			return NULL;

		for (i = 0; i < found->len; i++)
			g_ptr_array_add(steps, g_object_ref(g_ptr_array_index(found, i)));
	}

	return g_steal_pointer(&steps);
}

/* The report's calendar day: as_of's UTC date when one is given (a date
 * is stored as its midnight UTC, docs/money-calendar), else today in the
 * configured zone, encoded the same way. */
static GDateTime *
goals_report_day(
	VentureContext	*context,
	GDateTime	*as_of
){
	g_autoptr(GDateTime) utc = NULL;

	if (NULL == as_of)
		return venture_time_today(venture_context_get_timezone(context));

	utc = g_date_time_to_utc(as_of);

	return g_date_time_new_utc(g_date_time_get_year(utc), g_date_time_get_month(utc),
	                           g_date_time_get_day_of_month(utc), 0, 0, 0.0);
}

/* A calendar date's midnight UTC -- the stored form of a DATE field, kept
 * even if a writer handed in a time of day. */
static GDateTime *
goals_report_midnight(GDateTime *when)
{
	g_autoptr(GDateTime) utc = NULL;

	utc = g_date_time_to_utc(when);

	return g_date_time_new_utc(g_date_time_get_year(utc), g_date_time_get_month(utc),
	                           g_date_time_get_day_of_month(utc), 0, 0, 0.0);
}

static void
goals_report_append(
	GString		*note,
	const gchar	*text
){
	if (note->len > 0)
		g_string_append(note, "; ");

	g_string_append(note, text);
}

/* ==========================================================================
 * goal_progress
 * ========================================================================== */

/* Parses `status`: one nick or a comma-separated list. NULL is every
 * status. */
static gboolean
goals_progress_statuses(
	JsonObject	 *options,
	gboolean	 *out_wanted,
	gboolean	 *out_all,
	GError		**error
){
	g_auto(GStrv) parts = NULL;
	const gchar *text;
	guint i;

	*out_all = TRUE;
	text = (NULL != options) ? venture_json_object_get_string(options, "status", NULL) : NULL;

	if (venture_string_is_empty(text))
		return TRUE;

	*out_all = FALSE;
	parts = g_strsplit(text, ",", -1);

	for (i = 0; NULL != parts[i]; i++)
	{
		gint value;

		g_strstrip(parts[i]);

		if ('\0' == parts[i][0])
			continue;

		if (!venture_enum_from_nick(VENTURE_TYPE_GOAL_STATUS, parts[i], &value) ||
		    (value < 0) || (value > VENTURE_GOAL_STATUS_ABANDONED))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			            "status is active, paused, achieved or abandoned (or several, "
			            "comma separated), not \"%s\"", parts[i]);
			return FALSE;
		}

		out_wanted[value] = TRUE;
	}

	return TRUE;
}

typedef struct
{
	gint64	done;
	gint64	total;
} GoalsStepCount;

/* One goal's row; the path is its sort key. */
typedef struct
{
	gchar		*path;
	VentureEntity	*goal;
} GoalsProgressRow;

static void
goals_progress_row_free(gpointer data)
{
	GoalsProgressRow *row;

	row = data;
	g_free(row->path);
	g_object_unref(row->goal);
	g_free(row);
}

static gint
goals_progress_row_compare(
	gconstpointer	a,
	gconstpointer	b
){
	return g_utf8_collate((*(GoalsProgressRow *const *)a)->path,
	                      (*(GoalsProgressRow *const *)b)->path);
}

/*
 * The straight-line forecast: the goal covered @fraction of its distance
 * between its creation and @as_of, so at that pace the rest takes
 * elapsed x (1 - fraction) / fraction longer. NULL -- no forecast -- when
 * there is no pace to extend (nothing covered, going backwards, or less
 * than GOALS_FORECAST_MIN_SECONDS to measure it over) or it lands past
 * GOALS_FORECAST_MAX_DAYS; @out_reason says which.
 */
static GDateTime *
goals_progress_forecast(
	GDateTime	 *created,
	GDateTime	 *as_of,
	gdouble		  fraction,
	const gchar	**out_reason
){
	gdouble elapsed;
	gdouble rest;

	*out_reason = NULL;

	if ((NULL == created) || (fraction <= 0.0))
	{
		*out_reason = "no progress yet to forecast from";
		return NULL;
	}

	elapsed = (gdouble)g_date_time_difference(as_of, created) / (gdouble)G_TIME_SPAN_SECOND;

	if (elapsed <= 0.0)
	{
		*out_reason = "as_of is before the goal was set";
		return NULL;
	}

	if (elapsed < GOALS_FORECAST_MIN_SECONDS)
	{
		*out_reason = "too soon to forecast";
		return NULL;
	}

	rest = elapsed * (1.0 - fraction) / fraction;

	if (!isfinite(rest) || (rest / 86400.0 > GOALS_FORECAST_MAX_DAYS))
	{
		*out_reason = "more than a century away at this pace";
		return NULL;
	}

	return g_date_time_add_seconds(as_of, rest);
}

/* Writes one goal. */
static void
goals_progress_write(
	VentureReportResult	*result,
	GoalsProgressRow	*row,
	GoalsStepCount		*steps,
	GDateTime		*as_of,
	GDateTime		*day,
	GTimeZone		*zone
){
	g_autoptr(GString) note = NULL;
	g_autoptr(GDateTime) due = NULL;
	g_autoptr(GDateTime) forecast = NULL;
	g_autoptr(GDateTime) due_day = NULL;
	g_autofree gchar *metric = NULL;
	g_autofree gchar *unit = NULL;
	VentureGoalStatus status;
	gdouble start;
	gdouble current;
	gdouble target;
	gdouble fraction;
	gdouble direction;
	gboolean open;

	g_object_get(row->goal, "metric", &metric, "unit", &unit, "start-value", &start,
	             "current-value", &current, "target-value", &target, "due-on", &due,
	             "status", &status, NULL);

	note = g_string_new(NULL);
	fraction = venture_goals_fraction(start, current, target);
	direction = (target >= start) ? 1.0 : -1.0;
	open = (VENTURE_GOAL_STATUS_ACTIVE == status) || (VENTURE_GOAL_STATUS_PAUSED == status);

	venture_report_result_begin_row(result);
	venture_report_result_set_text(result, "goal", row->path);

	if (!venture_string_is_empty(metric))
		venture_report_result_set_text(result, "metric", metric);

	if (!venture_string_is_empty(unit))
		venture_report_result_set_text(result, "unit", unit);

	venture_report_result_set_number(result, "start", start);
	venture_report_result_set_number(result, "current", current);
	venture_report_result_set_number(result, "target", target);

	/* Raw: past the target reads above 100%, behind the start below 0.
	 * Only a bar stops at full. */
	venture_report_result_set_number(result, "percent", fraction);

	/* In the goal's own direction: positive is still to go, negative is
	 * past the target. A weight goal of 90 to 80 at 85 has 5 to go, not
	 * -5. */
	venture_report_result_set_number(result, "remaining", (target - current) * direction);
	venture_report_result_set_number(result, "steps_done", (gdouble)steps->done);
	venture_report_result_set_number(result, "steps_total", (gdouble)steps->total);
	venture_report_result_set_text(result, "status",
		venture_enum_to_nick(VENTURE_TYPE_GOAL_STATUS, (gint)status));

	if (NULL != due)
	{
		g_autofree gchar *due_text = NULL;
		gint64 days;

		due_day = goals_report_midnight(due);
		due_text = venture_time_to_date_string(due_day, zone);
		venture_report_result_set_text(result, "due_on", due_text);
		days = g_date_time_difference(due_day, day) / G_TIME_SPAN_DAY;
		venture_report_result_set_number(result, "days_left", (gdouble)days);

		if (open && (days < 0) && (fraction < 1.0))
			goals_report_append(note, "overdue");
	}

	/* --- The forecast: open goals short of their target only --- */

	if (VENTURE_GOAL_STATUS_ACHIEVED == status)
		;
	else if (VENTURE_GOAL_STATUS_ABANDONED == status)
		;
	else if (fraction >= 1.0)
		goals_report_append(note, "target reached; mark it achieved");
	else if (VENTURE_GOAL_STATUS_PAUSED == status)
		goals_report_append(note, "paused; no forecast");
	else
	{
		const gchar *reason;

		forecast = goals_progress_forecast(
			venture_entity_get_created_at(VENTURE_ENTITY(row->goal)), as_of,
			fraction, &reason);

		if (NULL == forecast)
			goals_report_append(note, reason);
		else
		{
			g_autofree gchar *forecast_text = NULL;
			g_autoptr(GDateTime) forecast_day = NULL;

			forecast_text = venture_time_to_date_string(forecast, zone);
			venture_report_result_set_text(result, "forecast", forecast_text);

			/* Compared as calendar days, as the due date is one. */
			forecast_day = venture_time_from_string(forecast_text, NULL);

			if ((NULL != due_day) && (NULL != forecast_day) &&
			    (g_date_time_compare(forecast_day, due_day) > 0))
				goals_report_append(note, "forecast is after the due date");
		}
	}

	if (note->len > 0)
		venture_report_result_set_text(result, "note", note->str);
}

VentureReportResult *
venture_goals_progress(
	VentureContext		 *context,
	VentureDateRange	 *period,
	JsonObject		 *options,
	GError			**error
){
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(GPtrArray) goals = NULL;
	g_autoptr(GPtrArray) steps = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GHashTable) counts = NULL;
	g_autoptr(GArray) categories = NULL;
	g_autoptr(GDateTime) as_of = NULL;
	g_autoptr(GDateTime) day = NULL;
	g_autoptr(GError) as_of_error = NULL;
	VentureDatabase *database;
	GTimeZone *zone;
	gboolean wanted[VENTURE_GOAL_STATUS_ABANDONED + 1] = { FALSE, FALSE, FALSE, FALSE };
	gboolean every_status;
	gint64 organization_id;
	gint64 venture_id;
	gint64 category_id;
	guint i;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);

	database = venture_context_get_database(context);
	zone = venture_context_get_timezone(context);

	if (!goals_report_organization(context, options, &organization_id, error))
		return NULL;

	/* --- The question --- */

	if (!goals_progress_statuses(options, wanted, &every_status, error))
		return NULL;

	as_of = venture_period_report_as_of(options, &as_of_error);

	if (NULL != as_of_error)
	{
		g_propagate_error(error, g_steal_pointer(&as_of_error));
		return NULL;
	}

	day = goals_report_day(context, as_of);

	/* The moment the pace is measured to: as_of when given, else now. */
	if (NULL == as_of)
		as_of = venture_time_now();

	venture_id = (NULL != options) ? venture_json_object_get_int(options, "venture_id", 0) : 0;
	category_id = (NULL != options) ? venture_json_object_get_int(options, "category_id", 0) : 0;

	/* A category and everything filed beneath it, as a person means
	 * "professions" when the tree has Professions / Alchemy under it. */
	if (0 != category_id)
	{
		categories = venture_category_descendants(database, VENTURE_TYPE_CATEGORY,
		                                          category_id, TRUE, error);

		if (NULL == categories)
			return NULL;
	}

	/* --- The goals, and their steps --- */

	goals = goals_report_goals(database, organization_id, venture_id, categories,
	                           NULL, every_status ? NULL : wanted, error);

	if (NULL == goals)
		return NULL;

	rows = g_ptr_array_new_with_free_func(goals_progress_row_free);

	for (i = 0; i < goals->len; i++)
	{
		VentureEntity *goal;
		GoalsProgressRow *row;

		goal = g_ptr_array_index(goals, i);

		row = g_new0(GoalsProgressRow, 1);
		row->goal = g_object_ref(goal);

		/* The path, computed and never stored: renaming a parent goal
		 * renames every row beneath it on the next run. */
		row->path = venture_category_path(database, VENTURE_TYPE_GOAL,
		                                  venture_entity_get_id(goal), error);

		if (NULL == row->path)
		{
			goals_progress_row_free(row);
			return NULL;
		}

		g_ptr_array_add(rows, row);
	}

	g_ptr_array_sort(rows, goals_progress_row_compare);

	{
		g_autoptr(GPtrArray) chosen = NULL;

		chosen = g_ptr_array_new();

		for (i = 0; i < rows->len; i++)
			g_ptr_array_add(chosen, ((GoalsProgressRow *)g_ptr_array_index(rows, i))->goal);

		steps = goals_report_steps(database, organization_id, chosen, error);

		if (NULL == steps)
			return NULL;
	}

	counts = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, g_free);

	for (i = 0; i < steps->len; i++)
	{
		VentureEntity *step;
		GoalsStepCount *count;
		gboolean done;
		gint64 goal_id;

		step = g_ptr_array_index(steps, i);
		goal_id = goals_report_int(step, "goal-id");
		g_object_get(step, "done", &done, NULL);
		count = g_hash_table_lookup(counts, &goal_id);

		if (NULL == count)
		{
			count = g_new0(GoalsStepCount, 1);
			g_hash_table_insert(counts, g_memdup2(&goal_id, sizeof(goal_id)), count);
		}

		count->total++;

		if (done)
			count->done++;
	}

	/* --- The answer --- */

	result = venture_report_result_new("Goal progress", period);
	venture_report_result_add_column(result, "goal", "Goal", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "metric", "Metric", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "unit", "Unit", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "start", "Start", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "current", "Current", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "target", "Target", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "percent", "Done", VENTURE_REPORT_COLUMN_PERCENT);
	venture_report_result_add_column(result, "remaining", "Remaining", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "steps_done", "Steps done", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "steps_total", "Steps", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "due_on", "Due", VENTURE_REPORT_COLUMN_DATE);
	venture_report_result_add_column(result, "days_left", "Days left", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "forecast", "Forecast", VENTURE_REPORT_COLUMN_DATE);
	venture_report_result_add_column(result, "status", "Status", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "note", "Note", VENTURE_REPORT_COLUMN_TEXT);

	for (i = 0; i < rows->len; i++)
	{
		GoalsProgressRow *row;
		GoalsStepCount *count;
		GoalsStepCount none = { 0, 0 };
		gint64 goal_id;

		row = g_ptr_array_index(rows, i);
		goal_id = venture_entity_get_id(row->goal);
		count = g_hash_table_lookup(counts, &goal_id);
		goals_progress_write(result, row, (NULL != count) ? count : &none, as_of, day,
		                     zone);
	}

	venture_report_result_append_note(result,
		"Done is (current - start) / (target - start), so a target below the "
		"start -- a weight to lose -- reads the same way as one above it. It is "
		"not clamped: past the target reads above 100%, behind the start below "
		"0%. Remaining is measured in the goal's own direction.");
	venture_report_result_append_note(result,
		"The forecast is a straight line from the start value when the goal was "
		"created to the current value at as_of (now by default), extended to the "
		"target. It is only drawn for active goals with some progress, at least a "
		"day after the goal was created. The current "
		"value is the one on the goal today: as_of moves the moment the pace is "
		"measured to, not the value read.");
	venture_report_result_append_note(result,
		"Days left count calendar days from as_of's date (today by default) to "
		"the due date; negative is overdue.");

	return g_steal_pointer(&result);
}

/* ==========================================================================
 * goal_materials
 * ========================================================================== */

/* One product's need across the steps. */
typedef struct
{
	gint64	 product_id;
	gchar	*name;
	gint64	 consumed;
	gint64	 reusable;
} GoalsNeed;

static void
goals_need_free(gpointer data)
{
	GoalsNeed *need;

	need = data;
	g_free(need->name);
	g_free(need);
}

static gint
goals_need_compare(
	gconstpointer	a,
	gconstpointer	b
){
	const GoalsNeed *left;
	const GoalsNeed *right;
	gint order;

	left = *(GoalsNeed *const *)a;
	right = *(GoalsNeed *const *)b;
	order = g_utf8_collate(left->name, right->name);

	if (0 != order)
		return order;

	return (left->product_id < right->product_id) ? -1
	     : (left->product_id > right->product_id) ? 1 : 0;
}

/* The option as a boolean: a JSON boolean or number, or one of the words
 * a form, a URL or a model sends. Anything else is refused rather than
 * read as either answer. */
static gboolean
goals_materials_flag(
	JsonObject	 *options,
	const gchar	 *member,
	gboolean	  fallback,
	gboolean	 *out_value,
	GError		**error
){
	static const gchar *const yes[] = { "true", "yes", "on", "1", NULL };
	static const gchar *const no[] = { "false", "no", "off", "0", NULL };
	JsonNode *node;
	const gchar *text;

	*out_value = fallback;

	if ((NULL == options) || !json_object_has_member(options, member))
		return TRUE;

	node = json_object_get_member(options, member);

	if (JSON_NODE_HOLDS_VALUE(node) &&
	    (G_TYPE_STRING == json_node_get_value_type(node)))
	{
		g_autofree gchar *lowered = NULL;

		text = json_node_get_string(node);
		lowered = g_ascii_strdown(text, -1);

		if (g_strv_contains(yes, lowered))
		{
			*out_value = TRUE;
			return TRUE;
		}

		if (g_strv_contains(no, lowered))
		{
			*out_value = FALSE;
			return TRUE;
		}
	}
	else if (JSON_NODE_HOLDS_VALUE(node))
	{
		*out_value = venture_json_object_get_bool(options, member, fallback);
		return TRUE;
	}

	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
	            "%s is true or false", member);
	return FALSE;
}

/* The components of @recipe_id, read once per report however many steps
 * name the recipe. */
static GPtrArray *
goals_materials_components(
	VentureDatabase	 *database,
	GHashTable	 *cache,
	gint64		  recipe_id,
	GError		**error
){
	g_autoptr(VentureQuery) query = NULL;
	GPtrArray *components;

	components = g_hash_table_lookup(cache, &recipe_id);

	if (NULL != components)
		return components;

	query = venture_query_new(VENTURE_TYPE_RECIPE_COMPONENT);
	venture_query_set_limit(query, 0);

	if (!venture_query_add_filter_int(query, "recipe-id", VENTURE_FILTER_OP_EQ,
	                                  recipe_id, error) ||
	    !venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, error))
		return NULL;

	components = venture_database_find(database, query, error);

	if (NULL == components)
		return NULL;

	g_hash_table_insert(cache, g_memdup2(&recipe_id, sizeof(recipe_id)), components);

	return components;
}

/* Every live inventory item of @product_id in the organisation. */
static GPtrArray *
goals_materials_items(
	VentureDatabase	 *database,
	gint64		  organization_id,
	gint64		  product_id,
	GError		**error
){
	g_autoptr(VentureQuery) query = NULL;

	query = venture_query_new(VENTURE_TYPE_INVENTORY_ITEM);
	venture_query_set_organization(query, organization_id);
	venture_query_set_limit(query, 0);

	if (!venture_query_add_filter_int(query, "product-id", VENTURE_FILTER_OP_EQ,
	                                  product_id, error) ||
	    !venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, error))
		return NULL;

	return venture_database_find(database, query, error);
}

/*
 * Units of the product on hand at @as_of, across every location: the
 * question is what has to be bought, and stock in the bank still counts
 * though it is not in the bag. The note says so.
 */
static gboolean
goals_materials_on_hand(
	VentureDatabase	 *database,
	GPtrArray	 *items,
	GDateTime	 *as_of,
	gint64		 *out_units,
	GError		**error
){
	VentureInventoryService *inventory;
	guint i;

	*out_units = 0;
	inventory = venture_inventory_service_get(database);

	for (i = 0; i < items->len; i++)
	{
		g_autoptr(GError) local_error = NULL;
		gint64 units;

		units = venture_inventory_service_on_hand(inventory,
			venture_entity_get_id(g_ptr_array_index(items, i)), as_of, &local_error);

		if (NULL != local_error)
		{
			g_propagate_error(error, g_steal_pointer(&local_error));
			return FALSE;
		}

		if (__builtin_add_overflow(*out_units, units, out_units))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			            "More stock is on hand than can be counted");
			return FALSE;
		}
	}

	return TRUE;
}

/*
 * What one unit costs to buy. With the market module on, the latest
 * observation from @source at or before @as_of and nothing else -- falling
 * back to a recorded cost would mix two kinds of number in one total
 * without saying which, the rule recipe_margin keeps. With it off, the
 * recorded cost: the first inventory item's carrying cost, else the
 * product's own. Nothing found is *out_price NULL, never zero.
 */
static gboolean
goals_materials_price(
	VentureDatabase	 *database,
	gint64		  organization_id,
	VentureEntity	 *product,
	GPtrArray	 *items,
	gboolean	  market,
	const gchar	 *source,
	GDateTime	 *as_of,
	VentureMoney	**out_price,
	GError		**error
){
	guint i;

	*out_price = NULL;

	if (market)
		return venture_market_latest_price(database, organization_id,
		                                   venture_entity_get_id(product), source,
		                                   as_of, out_price, NULL, error);

	for (i = 0; (i < items->len) && (NULL == *out_price); i++)
		g_object_get(g_ptr_array_index(items, i), "unit-cost", out_price, NULL);

	if (NULL == *out_price)
		g_object_get(product, "cost", out_price, NULL);

	return TRUE;
}

/* Adds @value into the currency's total in @totals. */
static gboolean
goals_materials_total(
	GHashTable		 *totals,
	const VentureMoney	 *value,
	GError			**error
){
	VentureMoney *total;
	VentureMoney *next;

	total = g_hash_table_lookup(totals, venture_money_get_currency(value));

	if (NULL == total)
	{
		g_hash_table_insert(totals, g_strdup(venture_money_get_currency(value)),
		                    venture_money_copy(value));
		return TRUE;
	}

	next = venture_money_add(total, value, error);

	if (NULL == next)
		return FALSE;

	g_hash_table_insert(totals, g_strdup(venture_money_get_currency(value)), next);

	return TRUE;
}

/*
 * The goals whose steps count: every goal in the organisation, narrowed to
 * @goal_id and its sub-goals and to @venture_id. Achieved and abandoned
 * goals are dropped -- their steps are not ahead of anybody. The set is
 * always explicit, never "every goal", so a step of a closed goal is never
 * counted. *@out_ids holds the goals in id order, for the steps' query.
 */
static gboolean
goals_materials_goals(
	VentureDatabase	 *database,
	gint64		  organization_id,
	gint64		  goal_id,
	gint64		  venture_id,
	GArray		**out_ids,
	GHashTable	**out_names,
	GError		**error
){
	g_autoptr(GArray) subtree = NULL;
	g_autoptr(GPtrArray) goals = NULL;
	g_autoptr(GArray) ids = NULL;
	g_autoptr(GHashTable) names = NULL;
	gboolean open[VENTURE_GOAL_STATUS_ABANDONED + 1];
	gint status;
	guint i;

	if (0 != goal_id)
	{
		if (!goals_report_require_goal(database, organization_id, goal_id, error))
			return FALSE;

		subtree = venture_category_descendants(database, VENTURE_TYPE_GOAL, goal_id,
		                                       TRUE, error);

		if (NULL == subtree)
			return FALSE;
	}

	/* Every status but the two that close a goal, so a status added
	 * later is counted as open until somebody says otherwise. */
	for (status = 0; status <= VENTURE_GOAL_STATUS_ABANDONED; status++)
		open[status] = (VENTURE_GOAL_STATUS_ACHIEVED != status) &&
		               (VENTURE_GOAL_STATUS_ABANDONED != status);

	goals = goals_report_goals(database, organization_id, venture_id, NULL,
	                           subtree, open, error);

	if (NULL == goals)
		return FALSE;

	ids = g_array_new(FALSE, FALSE, sizeof(gint64));
	names = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, g_free);

	for (i = 0; i < goals->len; i++)
	{
		VentureEntity *goal;
		gint64 id;

		goal = g_ptr_array_index(goals, i);
		id = venture_entity_get_id(goal);

		g_array_append_val(ids, id);
		g_hash_table_insert(names, g_memdup2(&id, sizeof(id)),
		                    venture_entity_get_display_name(goal));
	}

	*out_ids = g_steal_pointer(&ids);
	*out_names = g_steal_pointer(&names);

	return TRUE;
}

/* Writes one product's line; adds its cost to @totals or names it in
 * @unpriced. */
static gboolean
goals_materials_line(
	VentureContext		 *context,
	VentureReportResult	 *result,
	GoalsNeed		 *need,
	gint64			  organization_id,
	gboolean		  include_on_hand,
	gboolean		  market,
	const gchar		 *source,
	GDateTime		 *as_of,
	GHashTable		 *totals,
	GString			 *unpriced,
	GError			**error
){
	VentureDatabase *database;
	g_autoptr(VentureEntity) product = NULL;
	g_autoptr(GPtrArray) items = NULL;
	g_autoptr(VentureMoney) price = NULL;
	gint64 needed;
	gint64 on_hand;
	gint64 to_acquire;

	database = venture_context_get_database(context);
	product = venture_database_get(database, VENTURE_TYPE_PRODUCT, need->product_id, NULL);
	items = goals_materials_items(database, organization_id, need->product_id, error);

	if (NULL == items)
		return FALSE;

	/* Each half is bounded on its own; together they may not be. */
	if (__builtin_add_overflow(need->consumed, need->reusable, &needed))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "The steps need more %s than can be counted; check "
		            "their repetitions", need->name);
		return FALSE;
	}

	on_hand = 0;

	if (include_on_hand && !goals_materials_on_hand(database, items, as_of, &on_hand, error))
		return FALSE;

	/* Stock below zero (an item that allows it) is owed, not a credit
	 * against the list; more than enough leaves nothing to buy. */
	to_acquire = needed - MAX(on_hand, 0);

	if (to_acquire < 0)
		to_acquire = 0;

	venture_report_result_begin_row(result);
	venture_report_result_set_text(result, "product", need->name);
	venture_report_result_set_number(result, "consumed", (gdouble)need->consumed);
	venture_report_result_set_number(result, "reusable", (gdouble)need->reusable);
	venture_report_result_set_number(result, "needed", (gdouble)needed);

	if (include_on_hand)
		venture_report_result_set_number(result, "on_hand", (gdouble)on_hand);

	venture_report_result_set_number(result, "to_acquire", (gdouble)to_acquire);

	if ((NULL != product) &&
	    !goals_materials_price(database, organization_id, product, items, market,
	                           source, as_of, &price, error))
		return FALSE;

	if (NULL == price)
	{
		/* Nothing to buy needs no price; something to buy with none is
		 * named, and its cost is left blank rather than read as zero. */
		if (to_acquire > 0)
		{
			venture_report_result_set_text(result, "note",
				market ? "no price seen; cost left blank"
				       : "no recorded cost; cost left blank");

			if (unpriced->len > 0)
				g_string_append(unpriced, ", ");

			g_string_append(unpriced, need->name);
		}

		return TRUE;
	}

	{
		g_autoptr(VentureMoney) cost = NULL;

		cost = venture_money_multiply_int(price, to_acquire, error);

		if (NULL == cost)
			return FALSE;

		venture_report_result_set_text(result, "currency", venture_money_get_currency(price));
		venture_report_result_set_money(result, "unit_price", price);
		venture_report_result_set_money(result, "cost", cost);
		venture_report_result_set_text(result, "priced_by",
			!market ? "recorded cost"
			        : (venture_string_is_empty(source) ? "prices seen, any source" : source));

		return goals_materials_total(totals, cost, error);
	}
}

/* Orders steps by id, so the report reads the same whatever order the
 * batches came back in. */
static gint
goals_step_compare(
	gconstpointer	a,
	gconstpointer	b
){
	gint64 left;
	gint64 right;

	left = venture_entity_get_id(*(VentureEntity *const *)a);
	right = venture_entity_get_id(*(VentureEntity *const *)b);

	return (left > right) - (left < right);
}

/*
 * The steps not yet done that craft something, of the goals in @goal_ids,
 * read in batches by goal id. Every narrowing is in the query -- the goal,
 * done, a recipe and a count -- so the bound counts the steps the question
 * is about: filtered in C after one capped fetch of the organisation's
 * steps, goal_id and venture_id could not get a large install under it.
 */
static GPtrArray *
goals_materials_steps(
	VentureDatabase	 *database,
	gint64		  organization_id,
	GArray		 *goal_ids,
	GError		**error
){
	g_autoptr(GPtrArray) steps = NULL;
	guint max_rows;
	guint offset;

	max_rows = (guint)venture_aggregate_get_max_rows();
	steps = g_ptr_array_new_with_free_func(g_object_unref);

	for (offset = 0; offset < goal_ids->len; offset += GOALS_IN_BATCH)
	{
		g_autoptr(VentureQuery) query = NULL;
		g_autoptr(GPtrArray) ids = NULL;
		g_autoptr(GPtrArray) found = NULL;
		guint i;

		ids = g_ptr_array_new_with_free_func(g_free);

		for (i = offset; (i < goal_ids->len) && (i < offset + GOALS_IN_BATCH); i++)
			g_ptr_array_add(ids, g_strdup_printf("%" G_GINT64_FORMAT,
				g_array_index(goal_ids, gint64, i)));

		query = venture_query_new(VENTURE_TYPE_GOAL_STEP);
		venture_query_set_organization(query, organization_id);

		/* Past the bound across every batch, not per batch. */
		venture_query_set_limit(query, max_rows + 1 - steps->len);

		if (!venture_query_add_filter(query, "goal-id", VENTURE_FILTER_OP_IN, ids,
		                              error) ||
		    !venture_query_add_filter_string(query, "done", VENTURE_FILTER_OP_EQ,
		                                     "false", error) ||
		    !venture_query_add_filter_int(query, "recipe-id", VENTURE_FILTER_OP_GT, 0,
		                                  error) ||
		    !venture_query_add_filter_int(query, "repetitions", VENTURE_FILTER_OP_GT, 0,
		                                  error) ||
		    !venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, error))
			return NULL;

		found = venture_database_find(database, query, error);

		if (NULL == found)
			return NULL;

		for (i = 0; i < found->len; i++)
			g_ptr_array_add(steps, g_object_ref(g_ptr_array_index(found, i)));

		if (steps->len > max_rows)
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			            "More than %u recipe steps match; narrow by goal_id or "
			            "venture_id", max_rows);
			return NULL;
		}
	}

	g_ptr_array_sort(steps, goals_step_compare);

	return g_steal_pointer(&steps);
}

VentureReportResult *
venture_goals_materials(
	VentureContext		 *context,
	VentureDateRange	 *period,
	JsonObject		 *options,
	GError			**error
){
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(GPtrArray) steps = NULL;
	g_autoptr(GPtrArray) ordered = NULL;
	g_autoptr(GArray) goal_ids = NULL;
	g_autoptr(GHashTable) goal_names = NULL;
	g_autoptr(GHashTable) needs = NULL;
	g_autoptr(GHashTable) components = NULL;
	g_autoptr(GHashTable) totals = NULL;
	g_autoptr(GDateTime) as_of = NULL;
	g_autoptr(GError) as_of_error = NULL;
	g_autoptr(GString) unpriced = NULL;
	g_autoptr(GString) skipped = NULL;
	g_autoptr(GList) currencies = NULL;
	VentureDatabase *database;
	const gchar *source;
	gboolean include_on_hand;
	gboolean market;
	gint64 organization_id;
	gint64 goal_id;
	gint64 venture_id;
	guint counted;
	guint i;
	GList *link;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);

	database = venture_context_get_database(context);

	/* Recipes are the production module's. Refused, not answered with an
	 * empty list that reads as "nothing to buy". */
	if (!venture_context_module_enabled(context, "production"))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "goal_materials multiplies out the recipes goal steps "
		                    "name, and recipes belong to the production module, "
		                    "which is off; turn production on to use it");
		return NULL;
	}

	if (!goals_report_organization(context, options, &organization_id, error))
		return NULL;

	/* --- The question --- */

	market = venture_context_module_enabled(context, "market");
	source = (NULL != options)
		? venture_json_object_get_string(options, "price_source", NULL) : NULL;

	if (!venture_string_is_empty(source) && !market)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "price_source reads the market module's price "
		                    "observations, and the market module is off; leave "
		                    "it out to price at recorded costs");
		return NULL;
	}

	if (!goals_materials_flag(options, "include_on_hand", TRUE, &include_on_hand, error))
		return NULL;

	as_of = venture_period_report_as_of(options, &as_of_error);

	if (NULL != as_of_error)
	{
		g_propagate_error(error, g_steal_pointer(&as_of_error));
		return NULL;
	}

	goal_id = (NULL != options) ? venture_json_object_get_int(options, "goal_id", 0) : 0;
	venture_id = (NULL != options) ? venture_json_object_get_int(options, "venture_id", 0) : 0;

	if (!goals_materials_goals(database, organization_id, goal_id, venture_id,
	                           &goal_ids, &goal_names, error))
		return NULL;

	/* --- The steps still ahead that craft something --- */

	steps = goals_materials_steps(database, organization_id, goal_ids, error);

	if (NULL == steps)
		return NULL;

	needs = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, goals_need_free);
	components = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free,
	                                   (GDestroyNotify)g_ptr_array_unref);
	skipped = g_string_new(NULL);
	counted = 0;

	for (i = 0; i < steps->len; i++)
	{
		VentureEntity *step;
		g_autoptr(VentureEntity) recipe = NULL;
		GPtrArray *lines;
		gint64 recipe_id;
		gint64 repetitions;
		guint j;

		step = g_ptr_array_index(steps, i);
		g_object_get(step, "recipe-id", &recipe_id, "repetitions", &repetitions, NULL);

		recipe = venture_database_get(database, VENTURE_TYPE_RECIPE, recipe_id, NULL);

		/* A recipe deleted since the step named it has no components to
		 * read; the step is named rather than silently left out. */
		if ((NULL == recipe) || venture_entity_is_deleted(recipe))
		{
			g_autofree gchar *name = NULL;

			name = venture_entity_get_display_name(step);

			if (skipped->len > 0)
				g_string_append(skipped, ", ");

			g_string_append(skipped, name);
			continue;
		}

		lines = goals_materials_components(database, components, recipe_id, error);

		if (NULL == lines)
			return NULL;

		counted++;

		for (j = 0; j < lines->len; j++)
		{
			VentureEntity *line;
			GoalsNeed *need;
			gboolean reusable;
			gint64 product_id;
			gint64 quantity;

			line = g_ptr_array_index(lines, j);
			g_object_get(line, "product-id", &product_id, "quantity", &quantity,
			             "reusable", &reusable, NULL);

			if ((product_id <= 0) || (quantity <= 0))
				continue;

			need = g_hash_table_lookup(needs, &product_id);

			if (NULL == need)
			{
				g_autoptr(VentureEntity) product = NULL;

				product = venture_database_get(database, VENTURE_TYPE_PRODUCT,
				                               product_id, NULL);
				need = g_new0(GoalsNeed, 1);
				need->product_id = product_id;
				need->name = (NULL != product)
					? venture_entity_get_display_name(product)
					: g_strdup_printf("Product #%" G_GINT64_FORMAT, product_id);
				g_hash_table_insert(needs, g_memdup2(&product_id, sizeof(product_id)),
				                    need);
			}

			/*
			 * A tool is needed once, however many crafts it serves and
			 * however many steps use it: a hammer that makes twenty nails
			 * in one step makes forty in two. So the largest single need
			 * counts, not the sum. A consumed component is used up by each
			 * craft, so it multiplies and adds up.
			 */
			if (reusable)
				need->reusable = MAX(need->reusable, quantity);
			else
			{
				if ((quantity > G_MAXINT64 / repetitions) ||
				    (need->consumed > G_MAXINT64 - (quantity * repetitions)))
				{
					g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
					            "The steps need more %s than can be counted; check "
					            "their repetitions", need->name);
					return NULL;
				}

				need->consumed += quantity * repetitions;
			}
		}
	}

	/* --- The answer --- */

	result = venture_report_result_new("Goal materials", period);
	venture_report_result_add_column(result, "product", "Product", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "consumed", "Used up", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "reusable", "Tools", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "needed", "Needed", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "on_hand", "On hand", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "to_acquire", "To acquire", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "currency", "Currency", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "unit_price", "Unit price", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "cost", "Cost to acquire", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "priced_by", "Priced by", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "note", "Note", VENTURE_REPORT_COLUMN_TEXT);

	ordered = g_ptr_array_new();

	{
		GHashTableIter iter;
		gpointer value;

		g_hash_table_iter_init(&iter, needs);

		while (g_hash_table_iter_next(&iter, NULL, &value))
			g_ptr_array_add(ordered, value);
	}

	g_ptr_array_sort(ordered, goals_need_compare);
	totals = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                               (GDestroyNotify)venture_money_free);
	unpriced = g_string_new(NULL);

	for (i = 0; i < ordered->len; i++)
	{
		if (!goals_materials_line(context, result, g_ptr_array_index(ordered, i),
		                          organization_id, include_on_hand, market, source,
		                          as_of, totals, unpriced, error))
			return NULL;
	}

	/* One total per currency, never a sum across them. When something to
	 * buy has no price the total is of the priced lines only, and says
	 * so in its own label: a smaller number presented as the whole bill
	 * is the failure this avoids. */
	currencies = g_list_sort(g_hash_table_get_keys(totals), (GCompareFunc)g_strcmp0);

	for (link = currencies; NULL != link; link = link->next)
	{
		venture_report_result_begin_row(result);
		venture_report_result_set_text(result, "product",
			(0 == unpriced->len) ? "Total" : "Total of priced lines");
		venture_report_result_set_text(result, "currency", link->data);
		venture_report_result_set_money(result, "cost",
			g_hash_table_lookup(totals, link->data));

		if (unpriced->len > 0)
		{
			g_autofree gchar *note = NULL;

			note = g_strdup_printf("leaves out %s, which has no price", unpriced->str);
			venture_report_result_set_text(result, "note", note);
		}
	}

	{
		g_autofree gchar *note = NULL;

		note = g_strdup_printf("%u step%s not done, naming a recipe and a number of "
		                       "crafts, in goals that are active or paused.", counted,
		                       (1 == counted) ? "" : "s");
		venture_report_result_append_note(result, note);
	}

	venture_report_result_append_note(result,
		"Crafts are batches of the recipe, not units made: a step of 20 crafts "
		"of a recipe taking 2 of something needs 40. A tool (a reusable "
		"component) is needed once, at the largest quantity any one step asks "
		"for -- never multiplied and never summed across steps.");
	venture_report_result_append_note(result,
		"The list is gross: what an earlier step makes is not taken off what a "
		"later step needs. Craft the earlier step, and its output is on hand for "
		"the next run of this report.");

	if (include_on_hand)
		venture_report_result_append_note(result,
			"On hand counts every stock item of the product in every location, at "
			"as_of (now by default); what is in the bank counts though it is not in "
			"the bag.");
	else
		venture_report_result_append_note(result,
			"Stock on hand is not taken off: include_on_hand is false.");

	if (market)
		venture_report_result_append_note(result,
			"Unit prices are the latest price seen at or before as_of, from the named "
			"source (any source when none is named). A product never seen priced is "
			"named and its cost left blank, never read as zero.");
	else
		venture_report_result_append_note(result,
			"The market module is off: unit prices are recorded costs -- the first "
			"stock item's carrying cost, else the product's cost. A product with "
			"neither is named and its cost left blank, never read as zero.");

	if (skipped->len > 0)
	{
		g_autofree gchar *note = NULL;

		note = g_strdup_printf("Left out, because their recipe has been deleted: %s.",
		                       skipped->str);
		venture_report_result_append_note(result, note);
	}

	return g_steal_pointer(&result);
}

/* ==========================================================================
 * Registration
 *
 * A small report class rather than a function report, like the other
 * modules', so each report carries its parameter schema: the schema is
 * what the assistant's report tool and the options form read, and an
 * option they cannot see is one never sent.
 * ========================================================================== */

#define VENTURE_TYPE_GOALS_REPORT (venture_goals_report_get_type())

G_DECLARE_FINAL_TYPE(VentureGoalsReport, venture_goals_report,
                     VENTURE, GOALS_REPORT, VentureReport)

struct _VentureGoalsReport
{
	VentureReport		 parent_instance;

	VentureReportFunc	 func;
	const gchar		*schema;
};

G_DEFINE_FINAL_TYPE(VentureGoalsReport, venture_goals_report, VENTURE_TYPE_REPORT)

static VentureReportResult *
venture_goals_report_generate(
	VentureReport		 *self,
	VentureContext		 *context,
	VentureDateRange	 *period,
	JsonObject		 *options,
	GError			**error
){
	return VENTURE_GOALS_REPORT(self)->func(context, period, options, error);
}

static JsonNode *
venture_goals_report_parameters(VentureReport *self)
{
	return venture_json_parse(VENTURE_GOALS_REPORT(self)->schema, NULL);
}

static void
venture_goals_report_class_init(VentureGoalsReportClass *klass)
{
	VentureReportClass *report_class;

	report_class = VENTURE_REPORT_CLASS(klass);
	report_class->generate = venture_goals_report_generate;
	report_class->describe_parameters = venture_goals_report_parameters;
}

static void
venture_goals_report_init(VentureGoalsReport *self)
{
	(void)self;
}

static void
goals_register(
	VentureReportRegistry	*registry,
	const gchar		*name,
	const gchar		*title,
	const gchar		*description,
	VentureReportFunc	 func,
	const gchar		*schema
){
	VentureGoalsReport *report;

	report = g_object_new(VENTURE_TYPE_GOALS_REPORT, "name", name, "title", title,
	                      "description", description, NULL);
	report->func = func;
	report->schema = schema;
	venture_data_class_declare_resource(G_OBJECT(report), VENTURE_DATA_CLASS_TENANT);
	venture_report_registry_add(registry, VENTURE_REPORT(report));
}

void
venture_goals_register_reports(VentureReportRegistry *registry)
{
	g_return_if_fail(VENTURE_IS_REPORT_REGISTRY(registry));

	goals_register(registry, "goal_progress", "Goal progress",
		"How far along each goal is: percent covered, what remains, steps "
		"done, days left and a straight-line forecast",
		venture_goals_progress,
		"{\"type\":\"object\",\"properties\":{"
		"\"venture_id\":{\"type\":\"integer\",\"description\":\"Only this "
		"venture's goals\"},"
		"\"category_id\":{\"type\":\"integer\",\"description\":\"Only goals "
		"filed in this category or beneath it\"},"
		"\"status\":{\"type\":\"string\",\"description\":\"Only goals with this "
		"status: active, paused, achieved or abandoned, or several comma "
		"separated; every status by default\"},"
		"\"as_of\":{\"type\":\"string\",\"description\":\"Measure the pace and "
		"the days left to this date instead of now\"},"
		"\"organization_id\":{\"type\":\"integer\",\"description\":\"The legal "
		"entity; defaults to the default organization\"}}}");

	goals_register(registry, "goal_materials", "Goal materials",
		"The shopping list for goal steps not yet done: recipe components "
		"times crafts, less stock on hand, priced, with a total per currency",
		venture_goals_materials,
		"{\"type\":\"object\",\"properties\":{"
		"\"goal_id\":{\"type\":\"integer\",\"description\":\"Only this goal "
		"and its sub-goals; every active or paused goal by default\"},"
		"\"venture_id\":{\"type\":\"integer\",\"description\":\"Only this "
		"venture's goals\"},"
		"\"price_source\":{\"type\":\"string\",\"description\":\"Price at the "
		"latest observation from this source, matched exactly; any source by "
		"default. Needs the market module\"},"
		"\"include_on_hand\":{\"type\":\"boolean\",\"description\":\"Take "
		"stock on hand in every location off what is needed; true by default\"},"
		"\"as_of\":{\"type\":\"string\",\"description\":\"Count stock and "
		"read prices at this date instead of now\"},"
		"\"organization_id\":{\"type\":\"integer\",\"description\":\"The legal "
		"entity; defaults to the default organization\"}}}");
}
