/*
 * venture-web-planning.inc - An epic's pieces and a goal's work
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Included by venture-web-server.c. Two blocks of a record's page, drawn
 * before the generic panels: an epic's or a story's tree, grouped by
 * issue type, and the work planned toward a goal. Neither computes a
 * figure: the rollup is venture_ticket_tree_progress(), a milestone's
 * progress venture_factory_milestone_progress() and a goal's work
 * venture_goals_work_progress(), so the page, the reports and the API
 * cannot disagree.
 */

/* "3 of 5 done", the percentage, and the bar, for one rollup. */
static void
venture_web_planning_append_bar(
	GString		*content,
	gint64		 done,
	gint64		 total
){
	gdouble width;

	width = (total > 0) ? ((gdouble)done * 100.0) / (gdouble)total : 0.0;

	g_string_append_printf(content,
		"<div class=\"bar-row\"><span class=\"bar-label\">%" G_GINT64_FORMAT
		" of %" G_GINT64_FORMAT " done</span><span class=\"bar-track\">"
		"<span class=\"bar-fill\" style=\"width:%.1f%%\"></span></span>"
		"<span class=\"bar-value\">", done, total, width);

	/* Nothing to count is no figure, not 0%. */
	if (total > 0)
		g_string_append_printf(content, "%.0f%%", width);
	else
		g_string_append(content, "\xe2\x80\x94");

	g_string_append(content, "</span></div>");
}

/* A ticket's title as a link to it. */
static void
venture_web_planning_append_ticket_link(
	GString		*content,
	VentureEntity	*ticket
){
	g_autofree gchar *title = NULL;
	gint64 id;

	id = venture_entity_get_id(ticket);
	g_object_get(ticket, "title", &title, NULL);
	g_string_append_printf(content, "<a href=\"/e/ticket/%" G_GINT64_FORMAT
	                       "\">", id);
	venture_html_escape_append(content,
		venture_string_is_empty(title) ? "Untitled" : title);
	g_string_append(content, "</a>");
}

/*
 * An epic's or a story's page: everything beneath it, grouped by shape,
 * each group with its own count, and the rollup on top. The related
 * panel lists the direct children; this is the whole tree and the number.
 */
static void
venture_web_append_ticket_tree_block(
	VentureWebServer	*self,
	GString			*content,
	VentureEntity		*record
){
	static const VentureIssueType order[] = {
		VENTURE_ISSUE_TYPE_STORY, VENTURE_ISSUE_TYPE_TASK,
		VENTURE_ISSUE_TYPE_BUG, VENTURE_ISSUE_TYPE_RESEARCH,
		VENTURE_ISSUE_TYPE_SUBTASK
	};
	g_autoptr(GPtrArray) descendants = NULL;
	g_autoptr(GError) error = NULL;
	VentureTicketTreeProgress progress;
	VentureIssueType type;
	guint g;
	guint i;

	g_object_get(record, "issue-type", &type, NULL);

	if ((VENTURE_ISSUE_TYPE_EPIC != type) && (VENTURE_ISSUE_TYPE_STORY != type))
		return;

	g_string_append(content, "<div class=\"card ticket-tree\"><div class=\"card-head\">"
	                         "<h2>Pieces</h2></div><div class=\"card-body\">");

	descendants = venture_ticket_tree_descendants(
		venture_context_get_database(self->context),
		venture_entity_get_id(record), &error);

	if (NULL == descendants)
	{
		g_string_append(content, "<div class=\"notice negative\" role=\"alert\">");
		venture_html_escape_append(content, error->message);
		g_string_append(content, "</div></div></div>");
		return;
	}

	if (0 == descendants->len)
	{
		g_string_append(content, "<p class=\"muted\">Nothing is part of this yet. "
		                         "Set a ticket's Part of to this one to break it up.</p>"
		                         "</div></div>");
		return;
	}

	memset(&progress, 0, sizeof(progress));

	for (i = 0; i < descendants->len; i++)
		venture_ticket_tree_progress_add(&progress,
		                                 g_ptr_array_index(descendants, i));

	g_string_append_printf(content,
		"<div class=\"stat-row\">"
		"<div class=\"stat\"><span class=\"stat-value\">%" G_GINT64_FORMAT
		"</span><span class=\"stat-label\">Tickets</span></div>"
		"<div class=\"stat\"><span class=\"stat-value\">%" G_GINT64_FORMAT
		"</span><span class=\"stat-label\">Done</span></div>"
		"<div class=\"stat\"><span class=\"stat-value\">%" G_GINT64_FORMAT
		" / %" G_GINT64_FORMAT "</span><span class=\"stat-label\">Points done"
		"</span></div></div>",
		progress.total, progress.done, progress.points_done,
		progress.points_total);
	venture_web_planning_append_bar(content, progress.done, progress.total);

	/* --- One group per shape, in the order the work is broken up --- */

	for (g = 0; g < G_N_ELEMENTS(order); g++)
	{
		VentureTicketTreeProgress group;
		const gchar *nick;
		guint shown;

		memset(&group, 0, sizeof(group));
		shown = 0;

		for (i = 0; i < descendants->len; i++)
		{
			VentureIssueType child_type;

			g_object_get(g_ptr_array_index(descendants, i), "issue-type",
			             &child_type, NULL);

			if (child_type != order[g])
				continue;

			shown++;
			venture_ticket_tree_progress_add(&group,
			                                 g_ptr_array_index(descendants, i));
		}

		if (0 == shown)
			continue;

		nick = venture_enum_to_nick(VENTURE_TYPE_ISSUE_TYPE, (gint)order[g]);
		g_string_append(content, "<h3>");
		venture_html_escape_append(content, nick);
		g_string_append_printf(content, " <span class=\"muted\">%" G_GINT64_FORMAT
		                       " of %" G_GINT64_FORMAT " done</span></h3>",
		                       group.done, group.total);
		g_string_append(content, "<div class=\"table-wrap\"><table class=\"data\">"
		                         "<thead><tr><th>Ticket</th><th>Status</th>"
		                         "<th class=\"num\">Points</th></tr></thead><tbody>");

		for (i = 0; i < descendants->len; i++)
		{
			VentureEntity *child;
			VentureIssueType child_type;
			VentureTicketStatus status;
			gint64 points;

			child = g_ptr_array_index(descendants, i);
			g_object_get(child, "issue-type", &child_type, "status", &status,
			             "story-points", &points, NULL);

			if (child_type != order[g])
				continue;

			g_string_append_printf(content, "<tr data-href=\"/e/ticket/%"
			                       G_GINT64_FORMAT "\"><td>",
			                       venture_entity_get_id(child));
			venture_web_planning_append_ticket_link(content, child);
			g_string_append(content, "</td><td><span class=\"badge\">");
			venture_html_escape_append(content,
				venture_enum_to_nick(VENTURE_TYPE_TICKET_STATUS, (gint)status));
			g_string_append_printf(content, "</span></td><td class=\"num\">%"
			                       G_GINT64_FORMAT "</td></tr>", points);
		}

		g_string_append(content, "</tbody></table></div>");
	}

	g_string_append(content, "<p class=\"muted\">Every ticket beneath this one, "
	                         "at any depth; cancelled ones are listed but not "
	                         "counted.</p></div></div>");
}

/*
 * A goal's work: the milestones and epics naming it, each with its
 * progress, and the tickets of all of it counted once. Separate from the
 * goal's own progress card above, which is the metric a person reads off;
 * this never moves it.
 */
static void
venture_web_append_goal_work_block(
	VentureWebServer	*self,
	GString			*content,
	VentureEntity		*record
){
	g_autoptr(VentureQuery) milestone_query = NULL;
	g_autoptr(VentureQuery) epic_query = NULL;
	g_autoptr(GPtrArray) milestones = NULL;
	g_autoptr(GPtrArray) epics = NULL;
	g_autoptr(GError) error = NULL;
	VentureDatabase *database;
	VentureGoalWork work;
	gint64 goal_id;
	guint i;

	database = venture_context_get_database(self->context);
	goal_id = venture_entity_get_id(record);

	g_string_append(content, "<div class=\"card goal-work\"><div class=\"card-head\">"
	                         "<h2>Work</h2></div><div class=\"card-body\">");

	if (!venture_goals_work_progress(database, goal_id, &work, &error))
	{
		g_string_append(content, "<div class=\"notice negative\" role=\"alert\">");
		venture_html_escape_append(content, error->message);
		g_string_append(content, "</div></div></div>");
		return;
	}

	if ((0 == work.milestones) && (0 == work.tickets.total) && (0 == work.epics))
	{
		g_string_append(content, "<p class=\"muted\">No work is linked yet. Set "
		                         "the Goal of a milestone or an epic to this one, "
		                         "and its tickets count here.</p></div></div>");
		return;
	}

	venture_web_planning_append_bar(content, work.tickets.done, work.tickets.total);

	/* --- Milestones naming the goal --- */

	milestone_query = venture_query_new(VENTURE_TYPE_MILESTONE);
	venture_query_set_limit(milestone_query, 200);

	if (venture_query_add_filter_int(milestone_query, "goal-id",
	                                 VENTURE_FILTER_OP_EQ, goal_id, NULL) &&
	    venture_query_add_order(milestone_query, "due-on", VENTURE_SORT_ASCENDING,
	                            NULL))
		milestones = venture_database_find(database, milestone_query, NULL);

	if ((NULL != milestones) && (milestones->len > 0))
	{
		g_string_append(content, "<h3>Milestones</h3><div class=\"table-wrap\">"
		                         "<table class=\"data\"><thead><tr><th>Milestone</th>"
		                         "<th class=\"num\">Done</th><th class=\"num\">Tickets"
		                         "</th></tr></thead><tbody>");

		for (i = 0; i < milestones->len; i++)
		{
			VentureEntity *milestone;
			g_autofree gchar *name = NULL;
			gint64 total;
			gint64 done;
			gint64 id;

			milestone = g_ptr_array_index(milestones, i);
			id = venture_entity_get_id(milestone);
			g_object_get(milestone, "name", &name, NULL);
			venture_factory_milestone_progress(database, id, &total, &done);

			g_string_append_printf(content, "<tr data-href=\"/e/milestone/%"
			                       G_GINT64_FORMAT "\"><td><a href=\"/e/milestone/%"
			                       G_GINT64_FORMAT "\">", id, id);
			venture_html_escape_append(content,
				venture_string_is_empty(name) ? "Milestone" : name);
			g_string_append_printf(content, "</a></td><td class=\"num\">%"
			                       G_GINT64_FORMAT "</td><td class=\"num\">%"
			                       G_GINT64_FORMAT "</td></tr>", done, total);
		}

		g_string_append(content, "</tbody></table></div>");
	}

	/* --- Epics naming the goal --- */

	epic_query = venture_query_new(VENTURE_TYPE_TICKET);
	venture_query_set_limit(epic_query, 200);

	if (venture_query_add_filter_int(epic_query, "goal-id", VENTURE_FILTER_OP_EQ,
	                                 goal_id, NULL) &&
	    venture_query_add_filter_string(epic_query, "issue-type",
	                                    VENTURE_FILTER_OP_EQ, "epic", NULL) &&
	    venture_query_add_order(epic_query, "title", VENTURE_SORT_ASCENDING, NULL))
		epics = venture_database_find(database, epic_query, NULL);

	if ((NULL != epics) && (epics->len > 0))
	{
		g_string_append(content, "<h3>Epics</h3><div class=\"table-wrap\">"
		                         "<table class=\"data\"><thead><tr><th>Epic</th>"
		                         "<th class=\"num\">Done</th><th class=\"num\">Tickets"
		                         "</th><th class=\"num\">Points</th></tr></thead><tbody>");

		for (i = 0; i < epics->len; i++)
		{
			VentureEntity *epic;
			VentureTicketTreeProgress tree;

			epic = g_ptr_array_index(epics, i);

			g_string_append_printf(content, "<tr data-href=\"/e/ticket/%"
			                       G_GINT64_FORMAT "\"><td>",
			                       venture_entity_get_id(epic));
			venture_web_planning_append_ticket_link(content, epic);

			if (venture_ticket_tree_progress(database, venture_entity_get_id(epic),
			                                 &tree, NULL))
				g_string_append_printf(content, "</td><td class=\"num\">%"
				                       G_GINT64_FORMAT "</td><td class=\"num\">%"
				                       G_GINT64_FORMAT "</td><td class=\"num\">%"
				                       G_GINT64_FORMAT " / %" G_GINT64_FORMAT
				                       "</td></tr>", tree.done, tree.total,
				                       tree.points_done, tree.points_total);
			else
				g_string_append(content, "</td><td class=\"num\" colspan=\"3\">"
				                         "too large to count</td></tr>");
		}

		g_string_append(content, "</tbody></table></div>");
	}

	g_string_append(content, "<p class=\"muted\">Tickets naming this goal, "
	                         "everything beneath them and the tickets of its "
	                         "milestones, each counted once; cancelled tickets and "
	                         "epics themselves are left out. The goal's current "
	                         "value is still yours to set.</p></div></div>");
}
