/*
 * venture-planner-roadmap.c - Goals, milestones and epics on one timeline
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Reads the three levels once each, bounded, and assembles them in the
 * order they are drawn: each goal, then the milestones naming it with the
 * epics planned into each, then the goal's epics in no milestone of its;
 * after every goal, the milestones naming none (with their epics), then the
 * epics in neither. Progress is the figure each record's own page shows --
 * a goal's work (venture_goals_work_progress()), a milestone's tickets
 * (venture_factory_milestone_progress()), an epic's tree
 * (venture_ticket_tree_progress()) -- so the roadmap cannot disagree with
 * them. Nothing is written.
 */

#include "venture.h"

#include <string.h>

/* --- Rows ------------------------------------------------------------------- */

static void
roadmap_row_free(gpointer data)
{
	VenturePlannerRoadmapRow *row = data;

	if (NULL == row)
		return;

	g_free(row->title);
	g_clear_pointer(&row->start, g_date_time_unref);
	g_clear_pointer(&row->end, g_date_time_unref);
	g_free(row->status);
	g_free(row->url);
	g_free(row);
}

void
venture_planner_roadmap_query_init(VenturePlannerRoadmapQuery *query)
{
	g_return_if_fail(NULL != query);

	memset(query, 0, sizeof(*query));
}

void
venture_planner_roadmap_free(VenturePlannerRoadmap *roadmap)
{
	if (NULL == roadmap)
		return;

	g_clear_pointer(&roadmap->rows, g_ptr_array_unref);
	g_clear_pointer(&roadmap->notes, g_ptr_array_unref);
	g_clear_pointer(&roadmap->from, g_date_time_unref);
	g_clear_pointer(&roadmap->to, g_date_time_unref);
	g_clear_pointer(&roadmap->now, g_date_time_unref);
	g_free(roadmap);
}

static gint64
roadmap_int(
	VentureEntity	*entity,
	const gchar	*property
){
	gint64 value = 0;

	g_object_get(entity, property, &value, NULL);

	return value;
}

static gint
roadmap_enum(
	VentureEntity	*entity,
	const gchar	*property
){
	gint value = 0;

	g_object_get(entity, property, &value, NULL);

	return value;
}

/* A new row for @entity: its title, dates and page; the caller fills the
 * status, the progress and the depth. */
static VenturePlannerRoadmapRow *
roadmap_row_new(
	VentureEntity	*entity,
	const gchar	*type,
	const gchar	*record_type,
	const gchar	*title_property,
	const gchar	*due_property,
	gint		 depth
){
	VenturePlannerRoadmapRow *row;
	GDateTime *created;

	row = g_new0(VenturePlannerRoadmapRow, 1);
	row->type = type;
	row->record_type = record_type;
	row->id = venture_entity_get_id(entity);
	row->depth = depth;
	g_object_get(entity, title_property, &row->title, due_property, &row->end, NULL);

	if (venture_string_is_empty(row->title))
	{
		g_free(row->title);
		row->title = venture_entity_get_display_name(entity);
	}

	/* A record's bar begins the day it was written down: nothing else
	 * says when work toward it started. */
	created = venture_entity_get_created_at(entity);
	row->start = (NULL != created) ? g_date_time_ref(created) : NULL;
	row->url = g_strdup_printf("/e/%s/%" G_GINT64_FORMAT, record_type, row->id);

	return row;
}

/* Whether a row is past due: a due date before now and not finished. */
static void
roadmap_row_judge(
	VenturePlannerRoadmapRow	*row,
	GDateTime			*now
){
	row->overdue = !row->done && (NULL != row->end) && (g_date_time_compare(row->end, now) < 0);
}

/* --- Reading the levels ------------------------------------------------------- */

/* One level, bounded: refused past the bound rather than drawn in part. */
static GPtrArray *
roadmap_read(
	VentureContext				 *context,
	GType					  type,
	const VenturePlannerRoadmapQuery	 *query,
	gboolean				  epics,
	const gchar				 *label,
	GError					**error
){
	g_autoptr(VentureQuery) find = NULL;
	g_autoptr(GPtrArray) rows = NULL;

	find = venture_query_new(type);
	venture_query_set_organization(find, query->organization_id);
	venture_query_set_limit(find, VENTURE_PLANNER_ROADMAP_MAX_ROWS + 1);

	if ((query->venture_id > 0) &&
	    !venture_query_add_filter_int(find, "venture-id", VENTURE_FILTER_OP_EQ, query->venture_id, error))
		return NULL;

	if (epics && !venture_query_add_filter_string(find, "issue-type", VENTURE_FILTER_OP_EQ, "epic", error))
		return NULL;

	if (!venture_query_add_order(find, epics ? "due-at" : "due-on", VENTURE_SORT_ASCENDING, error) ||
	    !venture_query_add_order(find, "id", VENTURE_SORT_ASCENDING, error))
		return NULL;

	rows = venture_database_find(venture_context_get_database(context), find, error);

	if (NULL == rows)
		return NULL;

	if (rows->len > VENTURE_PLANNER_ROADMAP_MAX_ROWS)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		            "More than %d %s; narrow the roadmap to one goal (goal_id) or one "
		            "venture (venture_id)", VENTURE_PLANNER_ROADMAP_MAX_ROWS, label);
		return NULL;
	}

	return g_steal_pointer(&rows);
}

/* --- Making rows -------------------------------------------------------------- */

typedef struct
{
	VentureContext				*context;
	const VenturePlannerRoadmapQuery	*query;
	VenturePlannerRoadmap			*roadmap;
	GPtrArray				*milestones;
	GPtrArray				*epics;
	GHashTable				*drawn;	/* "type:id" already a row */
	GHashTable				*finished_goals;	/* goal ids left out as finished */
} RoadmapBuild;

/* Marks @type:@id drawn; FALSE if it already was, so nothing is drawn
 * twice -- an epic in a milestone of its goal is drawn under the
 * milestone, not again under the goal. */
static gboolean
roadmap_claim(
	RoadmapBuild	*build,
	const gchar	*type,
	gint64		 id
){
	gchar *key = g_strdup_printf("%s:%" G_GINT64_FORMAT, type, id);

	if (g_hash_table_contains(build->drawn, key))
	{
		g_free(key);
		return FALSE;
	}

	g_hash_table_add(build->drawn, key);

	return TRUE;
}

static gboolean
roadmap_add_epic(
	RoadmapBuild	 *build,
	VentureEntity	 *epic,
	gint		  depth,
	GError		**error
){
	VenturePlannerRoadmapRow *row;
	VentureTicketTreeProgress progress;
	VentureTicketStatus status;

	status = roadmap_enum(epic, "status");

	if (!build->query->include_done &&
	    ((VENTURE_TICKET_STATUS_DONE == status) || (VENTURE_TICKET_STATUS_CANCELLED == status)))
		return TRUE;

	if (!roadmap_claim(build, "ticket", venture_entity_get_id(epic)))
		return TRUE;

	if (!venture_ticket_tree_progress(venture_context_get_database(build->context),
	                                  venture_entity_get_id(epic), &progress, error))
		return FALSE;

	row = roadmap_row_new(epic, "epic", "ticket", "title", "due-at", depth);
	row->status = g_strdup(venture_enum_to_nick(VENTURE_TYPE_TICKET_STATUS, status));
	row->done = (VENTURE_TICKET_STATUS_DONE == status) || (VENTURE_TICKET_STATUS_CANCELLED == status);
	row->work_done = progress.done;
	row->work_total = progress.total;
	roadmap_row_judge(row, build->roadmap->now);
	g_ptr_array_add(build->roadmap->rows, row);

	return TRUE;
}

/* A milestone and the epics planned into it. */
static gboolean
roadmap_add_milestone(
	RoadmapBuild	 *build,
	VentureEntity	 *milestone,
	gint		  depth,
	GError		**error
){
	VenturePlannerRoadmapRow *row;
	VentureMilestoneStatus status;
	gint64 id;
	guint i;

	status = roadmap_enum(milestone, "status");
	id = venture_entity_get_id(milestone);

	if (!build->query->include_done &&
	    ((VENTURE_MILESTONE_STATUS_COMPLETED == status) || (VENTURE_MILESTONE_STATUS_CANCELLED == status)))
		return TRUE;

	if (!roadmap_claim(build, "milestone", id))
		return TRUE;

	row = roadmap_row_new(milestone, "milestone", "milestone", "name", "due-on", depth);
	row->status = g_strdup(venture_enum_to_nick(VENTURE_TYPE_MILESTONE_STATUS, status));
	row->done = (VENTURE_MILESTONE_STATUS_COMPLETED == status) || (VENTURE_MILESTONE_STATUS_CANCELLED == status);
	venture_factory_milestone_progress(venture_context_get_database(build->context), id,
	                                   &row->work_total, &row->work_done);
	roadmap_row_judge(row, build->roadmap->now);
	g_ptr_array_add(build->roadmap->rows, row);

	for (i = 0; (NULL != build->epics) && (i < build->epics->len); i++)
	{
		VentureEntity *epic = g_ptr_array_index(build->epics, i);

		if ((roadmap_int(epic, "milestone-id") == id) &&
		    !roadmap_add_epic(build, epic, depth + 1, error))
			return FALSE;
	}

	return TRUE;
}

/* A goal, its milestones and the epics naming it directly. */
static gboolean
roadmap_add_goal(
	RoadmapBuild	 *build,
	VentureEntity	 *goal,
	GError		**error
){
	VenturePlannerRoadmapRow *row;
	VentureGoalWork work;
	VentureGoalStatus status;
	gint64 id;
	guint i;

	status = roadmap_enum(goal, "status");
	id = venture_entity_get_id(goal);

	/* A finished goal is left out with what hangs off it: its milestones
	 * are not promoted to the "no goal" rows below. */
	if (!build->query->include_done &&
	    ((VENTURE_GOAL_STATUS_ACHIEVED == status) || (VENTURE_GOAL_STATUS_ABANDONED == status)))
	{
		g_hash_table_add(build->finished_goals, GINT_TO_POINTER((gint)id));
		return TRUE;
	}

	if (!venture_goals_work_progress(venture_context_get_database(build->context), id, &work, error))
		return FALSE;

	roadmap_claim(build, "goal", id);
	row = roadmap_row_new(goal, "goal", "goal", "name", "due-on", 0);
	row->status = g_strdup(venture_enum_to_nick(VENTURE_TYPE_GOAL_STATUS, status));
	row->done = (VENTURE_GOAL_STATUS_ACHIEVED == status) || (VENTURE_GOAL_STATUS_ABANDONED == status);
	row->work_done = work.tickets.done;
	row->work_total = work.tickets.total;
	roadmap_row_judge(row, build->roadmap->now);
	g_ptr_array_add(build->roadmap->rows, row);

	for (i = 0; (NULL != build->milestones) && (i < build->milestones->len); i++)
	{
		VentureEntity *milestone = g_ptr_array_index(build->milestones, i);

		if ((roadmap_int(milestone, "goal-id") == id) &&
		    !roadmap_add_milestone(build, milestone, 1, error))
			return FALSE;
	}

	for (i = 0; (NULL != build->epics) && (i < build->epics->len); i++)
	{
		VentureEntity *epic = g_ptr_array_index(build->epics, i);

		if ((roadmap_int(epic, "goal-id") == id) &&
		    !roadmap_add_epic(build, epic, 1, error))
			return FALSE;
	}

	return TRUE;
}

/* --- The window ----------------------------------------------------------------- */

/* The first of @when's month, as a calendar date (midnight UTC). */
static GDateTime *
roadmap_month_start(GDateTime *when)
{
	g_autoptr(GDateTime) utc = g_date_time_to_utc(when);

	return g_date_time_new_utc(g_date_time_get_year(utc), g_date_time_get_month(utc), 1, 0, 0, 0);
}

static gboolean
roadmap_window(
	const VenturePlannerRoadmapQuery	 *query,
	VenturePlannerRoadmap			 *roadmap,
	GError					**error
){
	if (NULL != query->from)
	{
		roadmap->from = g_date_time_ref(query->from);
	}
	else
	{
		g_autoptr(GDateTime) month = roadmap_month_start(roadmap->now);

		roadmap->from = g_date_time_add_months(month, -1);
	}

	roadmap->to = (NULL != query->to) ? g_date_time_ref(query->to)
	                                  : g_date_time_add_months(roadmap->from, 7);

	if (g_date_time_compare(roadmap->to, roadmap->from) <= 0)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		                    "The roadmap's window ends before it begins");
		return FALSE;
	}

	if (g_date_time_difference(roadmap->to, roadmap->from) >
	    (GTimeSpan)VENTURE_PLANNER_ROADMAP_MAX_DAYS * G_TIME_SPAN_DAY)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		            "A roadmap covers at most %d days", VENTURE_PLANNER_ROADMAP_MAX_DAYS);
		return FALSE;
	}

	return TRUE;
}

/* --- The roadmap ---------------------------------------------------------------- */

VenturePlannerRoadmap *
venture_planner_roadmap(
	VentureContext				 *context,
	const VenturePlannerRoadmapQuery	 *query,
	GError					**error
){
	g_autoptr(VenturePlannerRoadmap) roadmap = NULL;
	g_autoptr(GPtrArray) goals = NULL;
	g_autoptr(GPtrArray) milestones = NULL;
	g_autoptr(GPtrArray) epics = NULL;
	g_autoptr(GHashTable) drawn = NULL;
	g_autoptr(GHashTable) finished = NULL;
	RoadmapBuild build;
	gboolean goals_on;
	gboolean tickets_on;
	guint i;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);
	g_return_val_if_fail(NULL != query, NULL);

	if (!venture_context_module_enabled(context, "planner"))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		                    "The planner module is disabled");
		return NULL;
	}

	roadmap = g_new0(VenturePlannerRoadmap, 1);
	roadmap->rows = g_ptr_array_new_with_free_func(roadmap_row_free);
	roadmap->notes = g_ptr_array_new_with_free_func(g_free);
	roadmap->now = (NULL != query->now) ? g_date_time_ref(query->now) : venture_time_now();

	if (!roadmap_window(query, roadmap, error))
		return NULL;

	goals_on = venture_context_module_enabled(context, "goals");
	tickets_on = venture_context_module_enabled(context, "tickets");

	/* --- Read each level once, bounded --- */

	if (goals_on)
	{
		goals = roadmap_read(context, VENTURE_TYPE_GOAL, query, FALSE, "goals", error);

		if (NULL == goals)
			return NULL;
	}
	else
	{
		g_ptr_array_add(roadmap->notes, g_strdup("Goals are not shown: the goals module is off."));
	}

	if (tickets_on)
	{
		milestones = roadmap_read(context, VENTURE_TYPE_MILESTONE, query, FALSE, "milestones", error);

		if (NULL == milestones)
			return NULL;

		epics = roadmap_read(context, VENTURE_TYPE_TICKET, query, TRUE, "epics", error);

		if (NULL == epics)
			return NULL;
	}
	else
	{
		g_ptr_array_add(roadmap->notes, g_strdup("Milestones and epics are not shown: the tickets module is off."));
	}

	if ((query->goal_id > 0) && !goals_on)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		                    "goal_id needs the goals module, which is off");
		return NULL;
	}

	/* One goal asked for: it must be the organization's, or the answer
	 * would be an empty roadmap that looks like "no work". */
	if (query->goal_id > 0)
	{
		g_autoptr(VentureEntity) goal = NULL;

		goal = venture_database_get(venture_context_get_database(context), VENTURE_TYPE_GOAL,
		                            query->goal_id, NULL);

		if ((NULL == goal) || venture_entity_is_deleted(goal) ||
		    (venture_entity_get_organization_id(goal) != query->organization_id))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
			            "No goal #%" G_GINT64_FORMAT " in this organization", query->goal_id);
			return NULL;
		}
	}

	drawn = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	finished = g_hash_table_new(g_direct_hash, g_direct_equal);
	build.context = context;
	build.query = query;
	build.roadmap = roadmap;
	build.milestones = milestones;
	build.epics = epics;
	build.drawn = drawn;
	build.finished_goals = finished;

	/* --- Goals and what hangs off them --- */

	for (i = 0; (NULL != goals) && (i < goals->len); i++)
	{
		VentureEntity *goal = g_ptr_array_index(goals, i);

		if ((query->goal_id > 0) && (venture_entity_get_id(goal) != query->goal_id))
			continue;

		if (!roadmap_add_goal(&build, goal, error))
			return NULL;
	}

	/* One goal asked for: what has no goal is not part of the answer. */
	if (query->goal_id > 0)
		return g_steal_pointer(&roadmap);

	/* --- Then what serves no goal shown --- */

	for (i = 0; (NULL != milestones) && (i < milestones->len); i++)
	{
		VentureEntity *milestone = g_ptr_array_index(milestones, i);

		if (g_hash_table_contains(finished, GINT_TO_POINTER((gint)roadmap_int(milestone, "goal-id"))))
			continue;

		if (!roadmap_add_milestone(&build, milestone, 0, error))
			return NULL;
	}

	for (i = 0; (NULL != epics) && (i < epics->len); i++)
	{
		VentureEntity *epic = g_ptr_array_index(epics, i);

		if (g_hash_table_contains(finished, GINT_TO_POINTER((gint)roadmap_int(epic, "goal-id"))))
			continue;

		if (!roadmap_add_epic(&build, epic, 0, error))
			return NULL;
	}

	return g_steal_pointer(&roadmap);
}

gdouble
venture_planner_roadmap_fraction(
	const VenturePlannerRoadmap	*roadmap,
	GDateTime			*when
){
	gdouble span;
	gdouble at;

	g_return_val_if_fail(NULL != roadmap, 0.0);

	if (NULL == when)
		return 0.0;

	span = (gdouble)g_date_time_difference(roadmap->to, roadmap->from);
	at = (gdouble)g_date_time_difference(when, roadmap->from);

	if (span <= 0.0)
		return 0.0;

	return CLAMP(at / span, 0.0, 1.0);
}

static void
roadmap_json_time(
	JsonBuilder	*builder,
	const gchar	*name,
	GDateTime	*when
){
	g_autofree gchar *text = (NULL != when) ? venture_time_to_string(when) : NULL;

	json_builder_set_member_name(builder, name);

	if (NULL != text)
		json_builder_add_string_value(builder, text);
	else
		json_builder_add_null_value(builder);
}

JsonNode *
venture_planner_roadmap_to_json(const VenturePlannerRoadmap *roadmap)
{
	g_autoptr(JsonBuilder) builder = NULL;
	guint i;

	g_return_val_if_fail(NULL != roadmap, NULL);

	builder = json_builder_new();
	json_builder_begin_object(builder);
	roadmap_json_time(builder, "from", roadmap->from);
	roadmap_json_time(builder, "to", roadmap->to);
	json_builder_set_member_name(builder, "notes");
	json_builder_begin_array(builder);

	for (i = 0; i < roadmap->notes->len; i++)
		json_builder_add_string_value(builder, g_ptr_array_index(roadmap->notes, i));

	json_builder_end_array(builder);
	json_builder_set_member_name(builder, "rows");
	json_builder_begin_array(builder);

	for (i = 0; i < roadmap->rows->len; i++)
	{
		VenturePlannerRoadmapRow *row = g_ptr_array_index(roadmap->rows, i);

		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "type");
		json_builder_add_string_value(builder, row->type);
		json_builder_set_member_name(builder, "record_type");
		json_builder_add_string_value(builder, row->record_type);
		json_builder_set_member_name(builder, "id");
		json_builder_add_int_value(builder, row->id);
		json_builder_set_member_name(builder, "title");
		json_builder_add_string_value(builder, (NULL != row->title) ? row->title : "");
		json_builder_set_member_name(builder, "depth");
		json_builder_add_int_value(builder, row->depth);
		roadmap_json_time(builder, "start", row->start);
		roadmap_json_time(builder, "end", row->end);
		json_builder_set_member_name(builder, "status");
		json_builder_add_string_value(builder, (NULL != row->status) ? row->status : "");
		json_builder_set_member_name(builder, "done");
		json_builder_add_boolean_value(builder, row->done);
		json_builder_set_member_name(builder, "overdue");
		json_builder_add_boolean_value(builder, row->overdue);
		json_builder_set_member_name(builder, "work_done");
		json_builder_add_int_value(builder, row->work_done);
		json_builder_set_member_name(builder, "work_total");
		json_builder_add_int_value(builder, row->work_total);
		json_builder_set_member_name(builder, "url");
		json_builder_add_string_value(builder, row->url);
		json_builder_end_object(builder);
	}

	json_builder_end_array(builder);
	json_builder_end_object(builder);

	return json_builder_get_root(builder);
}
