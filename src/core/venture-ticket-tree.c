/*
 * venture-ticket-tree.c - Epics, stories and their pieces
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The hierarchy rule, the walk down a tree and the rollup. Deliberately
 * absent: any write from one ticket to another. A child does not take its
 * epic's milestone or goal -- that would be a derived write, a second save
 * nobody made and a version bump under whoever holds the child -- and an
 * epic whose pieces are all done is not closed for the person who owns
 * it. The figures are read, every time, from the rows.
 */

#include "venture.h"

#include <string.h>

#define VENTURE_TICKET_TREE_STATE_KEY "venture-ticket-tree-installed"

/* Parent ids are read in batches of this many, so an IN list stays well
 * under every backend's bound on bound parameters. */
#define TICKET_TREE_IN_BATCH (500)

/* ==========================================================================
 * The rule
 * ========================================================================== */

/* The issue type's nick, for a sentence. */
static const gchar *
ticket_tree_nick(VentureIssueType type)
{
	const gchar *nick;

	nick = venture_enum_to_nick(VENTURE_TYPE_ISSUE_TYPE, (gint)type);

	return (NULL != nick) ? nick : "ticket";
}

const gchar *
venture_ticket_tree_parent_allowed(
	VentureIssueType	child,
	gboolean		has_parent,
	VentureIssueType	parent
){
	switch (child)
	{
	case VENTURE_ISSUE_TYPE_EPIC:
		/* The top of a tree. An epic under an epic is a story that has
		 * not been called one, and every rollup would count it twice. */
		if (has_parent)
			return "an epic is the top of its tree and takes no parent; make "
			       "it a story to put it under another epic";
		return NULL;

	case VENTURE_ISSUE_TYPE_STORY:
		/* A story may stand alone -- a request in the queue nobody has
		 * planned yet -- but once it is part of something, it is part
		 * of an epic. */
		if (has_parent && (VENTURE_ISSUE_TYPE_EPIC != parent))
			return "a story's parent must be an epic";
		return NULL;

	case VENTURE_ISSUE_TYPE_SUBTASK:
		/* A piece of something. Not of an epic: an epic is broken into
		 * stories and tasks first, and a subtask of a subtask is a
		 * task's piece written one level too deep. */
		if (!has_parent)
			return "a subtask needs a parent: the task, story, bug or "
			       "research ticket it is a piece of";

		if ((VENTURE_ISSUE_TYPE_TASK != parent) &&
		    (VENTURE_ISSUE_TYPE_STORY != parent) &&
		    (VENTURE_ISSUE_TYPE_BUG != parent) &&
		    (VENTURE_ISSUE_TYPE_RESEARCH != parent))
			return "a subtask's parent must be a task, story, bug or "
			       "research ticket";
		return NULL;

	case VENTURE_ISSUE_TYPE_TASK:
	case VENTURE_ISSUE_TYPE_BUG:
	case VENTURE_ISSUE_TYPE_RESEARCH:
	default:
		if (has_parent &&
		    (VENTURE_ISSUE_TYPE_EPIC != parent) &&
		    (VENTURE_ISSUE_TYPE_STORY != parent))
			return "its parent must be an epic or a story, or none";
		return NULL;
	}
}

/* An integer property's value on @entity, 0 on a missing @entity. */
static gint64
ticket_tree_int(
	VentureEntity	*entity,
	const gchar	*property
){
	gint64 value;

	value = 0;

	if (NULL != entity)
		g_object_get(entity, property, &value, NULL);

	return value;
}

static VentureIssueType
ticket_tree_type(VentureEntity *entity)
{
	VentureIssueType type;

	type = VENTURE_ISSUE_TYPE_TASK;
	g_object_get(entity, "issue-type", &type, NULL);

	return type;
}

/*
 * The live children of @id, at most VENTURE_TICKET_TREE_MAX_NODES of them,
 * read under whatever scope is current. NULL with @error set on a read
 * that failed or came back at the bound.
 */
static GPtrArray *
ticket_tree_children(
	VentureDatabase	 *database,
	GPtrArray	 *parent_ids,
	guint		  limit,
	GError		**error
){
	g_autoptr(VentureQuery) query = NULL;

	query = venture_query_new(VENTURE_TYPE_TICKET);
	venture_query_set_limit(query, limit);

	if (!venture_query_add_filter(query, "parent-id", VENTURE_FILTER_OP_IN,
	                              parent_ids, error) ||
	    !venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, error))
		return NULL;

	return venture_database_find(database, query, error);
}

/*
 * The hierarchy, judged only when it is written:
 *
 *  - a new ticket, or one whose parent or issue type changed, must sit
 *    where venture_ticket_tree_parent_allowed() says it may. A row stored
 *    before the rule existed, edited for its title, is left alone -- the
 *    reference rule, so nothing old becomes uneditable;
 *  - a parent written must not close a loop, be in another organization
 *    or sit too deep: venture_category_check_tree_node(), the one
 *    definition of a loop for every self-nesting tree;
 *  - an issue type that changed must still be a parent its existing
 *    children may have. The refusal names one child, so the person knows
 *    what to move first.
 *
 * Every read is under a trusted scope: a loop is a loop, and a child the
 * saver cannot see is still a child.
 */
static gboolean
venture_ticket_tree_validate(
	VentureDatabase	 *database,
	VentureEntity	 *entity,
	VentureEntity	 *previous,
	gpointer	  user_data,
	GError		**error
){
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(VentureEntity) parent = NULL;
	VentureIssueType type;
	const gchar *refusal;
	gboolean parent_written;
	gboolean type_written;
	gint64 parent_id;

	(void)user_data;

	parent_id = ticket_tree_int(entity, "parent-id");
	type = ticket_tree_type(entity);

	parent_written = (NULL == previous) ||
	                 (ticket_tree_int(previous, "parent-id") != parent_id);
	type_written = (NULL == previous) || (ticket_tree_type(previous) != type);

	if (!parent_written && !type_written)
		return TRUE;

	internal = venture_access_policy_enter(
		venture_database_get_access_policy(database), NULL);

	/* --- Where it sits --- */

	if (parent_id > 0)
	{
		if (parent_written &&
		    !venture_category_check_tree_node(database, entity, error))
			return FALSE;

		parent = venture_database_get(database, VENTURE_TYPE_TICKET, parent_id,
		                              NULL);
	}

	/*
	 * A parent that is gone was refused by the reference check when it
	 * was written; one the ticket kept is not judged, as a reference kept
	 * never is. Its shape is unknown, so only "no parent at all" is
	 * judged then.
	 */
	if ((parent_id <= 0) || (NULL != parent))
	{
		refusal = venture_ticket_tree_parent_allowed(type, parent_id > 0,
			(NULL != parent) ? ticket_tree_type(parent) : VENTURE_ISSUE_TYPE_TASK);

		if (NULL != refusal)
		{
			if (NULL != parent)
				venture_set_error_validation(error, "Part of",
					"#%" G_GINT64_FORMAT " is a %s, and %s", parent_id,
					ticket_tree_nick(ticket_tree_type(parent)), refusal);
			else
				venture_set_error_validation(error, "Part of", "%s", refusal);

			return FALSE;
		}
	}

	/* --- What hangs under it --- */

	if (type_written && (NULL != previous) && (venture_entity_get_id(entity) > 0))
	{
		g_autoptr(GPtrArray) ids = NULL;
		g_autoptr(GPtrArray) children = NULL;
		guint i;

		ids = g_ptr_array_new_with_free_func(g_free);
		g_ptr_array_add(ids, g_strdup_printf("%" G_GINT64_FORMAT,
		                                     venture_entity_get_id(entity)));
		children = ticket_tree_children(database, ids,
		                                VENTURE_TICKET_TREE_MAX_NODES, error);

		if (NULL == children)
			return FALSE;

		for (i = 0; i < children->len; i++)
		{
			VentureEntity *child;
			g_autofree gchar *title = NULL;
			VentureIssueType child_type;

			child = g_ptr_array_index(children, i);
			child_type = ticket_tree_type(child);
			refusal = venture_ticket_tree_parent_allowed(child_type, TRUE, type);

			if (NULL == refusal)
				continue;

			g_object_get(child, "title", &title, NULL);
			venture_set_error_validation(error, "Issue type",
				"cannot become %s while %s #%" G_GINT64_FORMAT " (%s) is part "
				"of it: %s. Move or retype the %s first",
				ticket_tree_nick(type), ticket_tree_nick(child_type),
				venture_entity_get_id(child),
				venture_string_is_empty(title) ? "untitled" : title, refusal,
				ticket_tree_nick(child_type));
			return FALSE;
		}
	}

	return TRUE;
}

/*
 * A ticket's end time is never before its due time. Judged only when
 * either is written, the reference rule, so a row stored backwards stays
 * editable for everything else. Only the order is judged: an end time
 * with no due time is kept.
 */
static gboolean
venture_ticket_until_validate(
	VentureDatabase	 *database,
	VentureEntity	 *entity,
	VentureEntity	 *previous,
	gpointer	  user_data,
	GError		**error
){
	g_autoptr(GDateTime) due = NULL;
	g_autoptr(GDateTime) until = NULL;
	g_autoptr(GDateTime) was_due = NULL;
	g_autoptr(GDateTime) was_until = NULL;

	(void)database;
	(void)user_data;

	g_object_get(entity, "due-at", &due, "until-at", &until, NULL);

	if ((NULL == due) || (NULL == until) || (g_date_time_compare(until, due) >= 0))
		return TRUE;

	if (NULL != previous)
	{
		g_object_get(previous, "due-at", &was_due, "until-at", &was_until, NULL);

		if ((NULL != was_due) && (NULL != was_until) &&
		    g_date_time_equal(was_due, due) && g_date_time_equal(was_until, until))
			return TRUE;
	}

	venture_set_error_validation(error, "Until", "is before Due; work ends after it starts");
	return FALSE;
}

void
venture_ticket_tree_install(VentureContext *context)
{
	VentureDatabase *database;

	g_return_if_fail(VENTURE_IS_CONTEXT(context));

	database = venture_context_get_database(context);

	/* The tests build several contexts over one database; validators are
	 * per database, so the second one must add nothing. */
	if (NULL != g_object_get_data(G_OBJECT(database),
	                              VENTURE_TICKET_TREE_STATE_KEY))
		return;

	g_object_set_data(G_OBJECT(database), VENTURE_TICKET_TREE_STATE_KEY,
	                  GINT_TO_POINTER(1));

	venture_database_add_save_validator(database, VENTURE_TYPE_TICKET,
	                                    venture_ticket_tree_validate, NULL,
	                                    NULL);
	venture_database_add_save_validator(database, VENTURE_TYPE_TICKET,
	                                    venture_ticket_until_validate, NULL,
	                                    NULL);
}

/* ==========================================================================
 * The walk and the rollup
 * ========================================================================== */

GPtrArray *
venture_ticket_tree_descendants(
	VentureDatabase	 *database,
	gint64		  root_id,
	GError		**error
){
	g_autoptr(GPtrArray) found = NULL;
	g_autoptr(GArray) frontier = NULL;
	g_autoptr(GHashTable) seen = NULL;
	guint depth;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);

	found = g_ptr_array_new_with_free_func(g_object_unref);
	frontier = g_array_new(FALSE, FALSE, sizeof(gint64));
	seen = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);

	g_array_append_val(frontier, root_id);
	g_hash_table_add(seen, g_memdup2(&root_id, sizeof(root_id)));

	/*
	 * A level at a time. The seen set is what stops a loop written behind
	 * the validator's back (by SQL, or before the rule) from walking
	 * forever; the depth bound stops a chain that is merely absurd.
	 */
	for (depth = 0; (frontier->len > 0) && (depth < VENTURE_CATEGORY_MAX_DEPTH);
	     depth++)
	{
		g_autoptr(GArray) next = NULL;
		guint offset;

		next = g_array_new(FALSE, FALSE, sizeof(gint64));

		for (offset = 0; offset < frontier->len; offset += TICKET_TREE_IN_BATCH)
		{
			g_autoptr(GPtrArray) ids = NULL;
			g_autoptr(GPtrArray) children = NULL;
			guint i;

			ids = g_ptr_array_new_with_free_func(g_free);

			for (i = offset; (i < frontier->len) &&
			     (i < offset + TICKET_TREE_IN_BATCH); i++)
				g_ptr_array_add(ids, g_strdup_printf("%" G_GINT64_FORMAT,
					g_array_index(frontier, gint64, i)));

			/* One past what may still be taken, so reaching the bound
			 * is seen rather than silently cut. */
			children = ticket_tree_children(database, ids,
				VENTURE_TICKET_TREE_MAX_NODES - found->len + 1, error);

			if (NULL == children)
				return NULL;

			for (i = 0; i < children->len; i++)
			{
				VentureEntity *child;
				gint64 id;

				child = g_ptr_array_index(children, i);
				id = venture_entity_get_id(child);

				if (g_hash_table_contains(seen, &id))
					continue;

				if (found->len >= VENTURE_TICKET_TREE_MAX_NODES)
				{
					g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
					            "Ticket #%" G_GINT64_FORMAT " has more than %d "
					            "tickets beneath it; break it up rather than "
					            "count part of it", root_id,
					            VENTURE_TICKET_TREE_MAX_NODES);
					return NULL;
				}

				g_hash_table_add(seen, g_memdup2(&id, sizeof(id)));
				g_array_append_val(next, id);
				g_ptr_array_add(found, g_object_ref(child));
			}
		}

		g_array_set_size(frontier, 0);
		g_array_append_vals(frontier, next->data, next->len);
	}

	return g_steal_pointer(&found);
}

void
venture_ticket_tree_progress_add(
	VentureTicketTreeProgress	*progress,
	VentureEntity			*ticket
){
	VentureTicketStatus status;
	VentureIssueType type;
	gint64 points;

	g_return_if_fail(NULL != progress);
	g_return_if_fail(VENTURE_IS_ENTITY(ticket));

	g_object_get(ticket, "status", &status, "issue-type", &type,
	             "story-points", &points, NULL);

	/* Cancelled work left the plan; an epic is the plan. */
	if ((VENTURE_TICKET_STATUS_CANCELLED == status) ||
	    (VENTURE_ISSUE_TYPE_EPIC == type))
		return;

	progress->total++;
	progress->points_total += MAX(points, 0);

	if (VENTURE_TICKET_STATUS_DONE == status)
	{
		progress->done++;
		progress->points_done += MAX(points, 0);
	}
}

gboolean
venture_ticket_tree_progress(
	VentureDatabase			 *database,
	gint64				  root_id,
	VentureTicketTreeProgress	 *out,
	GError				**error
){
	g_autoptr(GPtrArray) descendants = NULL;
	guint i;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), FALSE);
	g_return_val_if_fail(NULL != out, FALSE);

	memset(out, 0, sizeof(*out));
	descendants = venture_ticket_tree_descendants(database, root_id, error);

	if (NULL == descendants)
		return FALSE;

	for (i = 0; i < descendants->len; i++)
		venture_ticket_tree_progress_add(out, g_ptr_array_index(descendants, i));

	return TRUE;
}

/* ==========================================================================
 * epic_progress
 * ========================================================================== */

/* The organization the report reads, checked to exist: "no epics" about
 * an organization that is not there would be a wrong answer. */
static gboolean
ticket_tree_report_organization(
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

VentureReportResult *
venture_ticket_tree_epic_progress(
	VentureContext		 *context,
	VentureDateRange	 *period,
	JsonObject		 *options,
	GError			**error
){
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) epics = NULL;
	VentureDatabase *database;
	gint64 organization_id;
	gint64 milestone_id;
	gint64 goal_id;
	gint64 venture_id;
	guint max_rows;
	guint i;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);

	database = venture_context_get_database(context);

	if (!ticket_tree_report_organization(context, options, &organization_id,
	                                     error))
		return NULL;

	milestone_id = (NULL != options)
		? venture_json_object_get_int(options, "milestone_id", 0) : 0;
	goal_id = (NULL != options)
		? venture_json_object_get_int(options, "goal_id", 0) : 0;
	venture_id = (NULL != options)
		? venture_json_object_get_int(options, "venture_id", 0) : 0;

	/* --- The epics, narrowed in the query --- */

	/* Every narrowing is in the query, never applied to the rows after:
	 * the bound counts what was fetched, so a filter run in C would have
	 * an organization past it refused however narrowly it asked. */
	max_rows = venture_aggregate_get_max_rows();
	query = venture_query_new(VENTURE_TYPE_TICKET);
	venture_query_set_organization(query, organization_id);
	venture_query_set_limit(query, max_rows + 1);

	if (!venture_query_add_filter_string(query, "issue-type", VENTURE_FILTER_OP_EQ,
	        ticket_tree_nick(VENTURE_ISSUE_TYPE_EPIC), error) ||
	    ((0 != milestone_id) &&
	     !venture_query_add_filter_int(query, "milestone-id", VENTURE_FILTER_OP_EQ,
	                                   milestone_id, error)) ||
	    ((0 != goal_id) &&
	     !venture_query_add_filter_int(query, "goal-id", VENTURE_FILTER_OP_EQ,
	                                   goal_id, error)) ||
	    ((0 != venture_id) &&
	     !venture_query_add_filter_int(query, "venture-id", VENTURE_FILTER_OP_EQ,
	                                   venture_id, error)) ||
	    !venture_query_add_order(query, "title", VENTURE_SORT_ASCENDING, error) ||
	    !venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, error))
		return NULL;

	epics = venture_database_find(database, query, error);

	if (NULL == epics)
		return NULL;

	if (epics->len > max_rows)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "More than %u epics match; narrow by milestone_id, goal_id "
		            "or venture_id", max_rows);
		return NULL;
	}

	/* --- The answer --- */

	result = venture_report_result_new("Epic progress", period);
	venture_report_result_add_column(result, "epic", "Epic", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "status", "Status", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "done", "Done", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "total", "Tickets", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "points_done", "Points done", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "points", "Points", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "percent", "Complete", VENTURE_REPORT_COLUMN_PERCENT);

	for (i = 0; i < epics->len; i++)
	{
		VentureEntity *epic;
		VentureTicketTreeProgress progress;
		g_autofree gchar *title = NULL;
		g_autofree gchar *label = NULL;
		VentureTicketStatus status;

		epic = g_ptr_array_index(epics, i);
		g_object_get(epic, "title", &title, "status", &status, NULL);

		if (!venture_ticket_tree_progress(database, venture_entity_get_id(epic),
		                                  &progress, error))
			return NULL;

		label = g_strdup_printf("#%" G_GINT64_FORMAT " %s",
		                        venture_entity_get_id(epic),
		                        venture_string_is_empty(title) ? "Untitled" : title);

		venture_report_result_begin_row(result);
		venture_report_result_set_text(result, "epic", label);
		venture_report_result_set_text(result, "status",
			venture_enum_to_nick(VENTURE_TYPE_TICKET_STATUS, (gint)status));
		venture_report_result_set_number(result, "done", (gdouble)progress.done);
		venture_report_result_set_number(result, "total", (gdouble)progress.total);
		venture_report_result_set_number(result, "points_done",
		                                 (gdouble)progress.points_done);
		venture_report_result_set_number(result, "points",
		                                 (gdouble)progress.points_total);

		/* Nothing counted is an empty cell, never 0%: an epic not yet
		 * broken up is not an epic none of whose work is done. */
		if (progress.total > 0)
			venture_report_result_set_number(result, "percent",
				(gdouble)progress.done / (gdouble)progress.total);
	}

	venture_report_result_append_note(result,
		"Every ticket beneath each epic counts once, at any depth: its stories, "
		"their tasks and the subtasks under those. Cancelled tickets are left "
		"out of both halves -- neither work left nor work done.");
	venture_report_result_append_note(result,
		"milestone_id, goal_id and venture_id narrow by the epic's own fields. "
		"A child does not inherit its epic's milestone or goal, so a piece "
		"planned elsewhere still counts under its epic.");

	return g_steal_pointer(&result);
}

/* ==========================================================================
 * Registration
 *
 * A small report class rather than a function report, so the report
 * carries its parameter schema: the schema is what the assistant's report
 * tool, the options form and a widget's options are judged by.
 * ========================================================================== */

#define VENTURE_TYPE_TICKET_TREE_REPORT (venture_ticket_tree_report_get_type())

G_DECLARE_FINAL_TYPE(VentureTicketTreeReport, venture_ticket_tree_report,
                     VENTURE, TICKET_TREE_REPORT, VentureReport)

struct _VentureTicketTreeReport
{
	VentureReport	parent_instance;
};

G_DEFINE_FINAL_TYPE(VentureTicketTreeReport, venture_ticket_tree_report,
                    VENTURE_TYPE_REPORT)

static VentureReportResult *
venture_ticket_tree_report_generate(
	VentureReport		 *self,
	VentureContext		 *context,
	VentureDateRange	 *period,
	JsonObject		 *options,
	GError			**error
){
	(void)self;

	return venture_ticket_tree_epic_progress(context, period, options, error);
}

static JsonNode *
venture_ticket_tree_report_parameters(VentureReport *self)
{
	(void)self;

	return venture_json_parse(
		"{\"type\":\"object\",\"properties\":{"
		"\"milestone_id\":{\"type\":\"integer\",\"description\":\"Only epics "
		"planned into this milestone\"},"
		"\"goal_id\":{\"type\":\"integer\",\"description\":\"Only epics "
		"linked to this goal\"},"
		"\"venture_id\":{\"type\":\"integer\",\"description\":\"Only this "
		"venture's epics\"},"
		"\"organization_id\":{\"type\":\"integer\",\"description\":\"The legal "
		"entity; defaults to the default organization\"}}}", NULL);
}

static void
venture_ticket_tree_report_class_init(VentureTicketTreeReportClass *klass)
{
	VentureReportClass *report_class;

	report_class = VENTURE_REPORT_CLASS(klass);
	report_class->generate = venture_ticket_tree_report_generate;
	report_class->describe_parameters = venture_ticket_tree_report_parameters;
}

static void
venture_ticket_tree_report_init(VentureTicketTreeReport *self)
{
	(void)self;
}

void
venture_ticket_tree_register_reports(VentureReportRegistry *registry)
{
	VentureTicketTreeReport *report;

	g_return_if_fail(VENTURE_IS_REPORT_REGISTRY(registry));

	report = g_object_new(VENTURE_TYPE_TICKET_TREE_REPORT,
		"name", "epic_progress",
		"title", "Epic progress",
		"description", "Each epic's pieces done of total, at any depth, with "
		"their story points and the percentage complete; cancelled work left out",
		NULL);
	venture_data_class_declare_resource(G_OBJECT(report), VENTURE_DATA_CLASS_TENANT);
	venture_report_registry_add(registry, VENTURE_REPORT(report));
}
