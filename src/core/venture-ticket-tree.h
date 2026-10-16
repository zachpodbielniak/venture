/*
 * venture-ticket-tree.h - Epics, stories and their pieces
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A ticket's `parent-id` and its `issue-type` together make a tree: an
 * epic is broken into stories and tasks, a story into tasks, and a task
 * into subtasks. This file holds the one rule for which shapes may hang
 * under which (a save validator, judged only when the parent or the issue
 * type is written, so a row stored before the rule stays editable), the
 * bounded walk down a tree, and the rollup an epic's page and the
 * `epic_progress` report read. Nothing here writes one ticket because
 * another changed: a child does not inherit its epic's milestone or goal,
 * and an epic is not closed because its pieces are. See docs/tickets.org.
 */

#ifndef VENTURE_TICKET_TREE_H
#define VENTURE_TICKET_TREE_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>

G_BEGIN_DECLS

/**
 * VENTURE_TICKET_TREE_MAX_NODES:
 *
 * The most tickets one walk down a tree, or one goal's work, will read.
 * Past it the answer is refused rather than totalled short: a rollup of
 * part of an epic presented as the whole is the failure the bound avoids.
 */
#define VENTURE_TICKET_TREE_MAX_NODES (10000)

/**
 * VentureTicketTreeProgress:
 * @total: tickets counted, cancelled ones and epics left out
 * @done: of those, the ones done
 * @points_total: the story points of the tickets counted
 * @points_done: the story points of the ones done
 *
 * How far along a set of tickets is. Cancelled work is neither work left
 * nor work done, the rule the milestone forecast follows; an epic is a
 * container, done when its pieces are, so it is never counted itself.
 */
typedef struct
{
	gint64	total;
	gint64	done;
	gint64	points_total;
	gint64	points_done;
} VentureTicketTreeProgress;

/**
 * venture_ticket_tree_install:
 * @context: the wiring
 *
 * Installs the ticket save validator that holds the hierarchy: an epic
 * takes no parent; a story's parent, when it has one, is an epic; a task,
 * bug or research ticket hangs under an epic, a story or nothing; a
 * subtask needs a parent that is a task, story, bug or research ticket. A
 * change of issue type that would leave an existing child under a parent
 * it may not have is refused naming the child, and a parent that would
 * close a loop is refused by venture_category_check_tree_node(). Judged
 * only when `parent-id` or `issue-type` is written (a new ticket, or a
 * value that changed). Called once by the context; a second context over
 * the same database installs nothing twice.
 */
void
venture_ticket_tree_install(VentureContext *context);

/**
 * venture_ticket_tree_parent_allowed:
 * @child: the issue type of the ticket being placed
 * @has_parent: whether it names a parent
 * @parent: the parent's issue type; ignored without @has_parent
 *
 * The hierarchy as a pure function, so the validator, a picker and a test
 * read one table.
 *
 * Returns: (nullable): %NULL when @child may sit there, else a sentence
 *   saying why not, owned by the library
 */
const gchar *
venture_ticket_tree_parent_allowed(
	VentureIssueType	child,
	gboolean		has_parent,
	VentureIssueType	parent
);

/**
 * venture_ticket_tree_descendants:
 * @database: the database to read
 * @root_id: the ticket whose tree to walk; it is not itself returned
 * @error: (out) (optional): return location for a #GError
 *
 * Every live ticket beneath @root_id, breadth first, each once however the
 * rows point, read under the caller's access scope. Bounded by
 * %VENTURE_CATEGORY_MAX_DEPTH levels and %VENTURE_TICKET_TREE_MAX_NODES
 * tickets; past the second it refuses rather than returning part.
 *
 * Returns: (transfer full) (element-type VentureEntity) (nullable): the
 *   descendants, or %NULL with @error set
 */
GPtrArray *
venture_ticket_tree_descendants(
	VentureDatabase	 *database,
	gint64		  root_id,
	GError		**error
);

/**
 * venture_ticket_tree_progress_add:
 * @progress: the running count
 * @ticket: one ticket
 *
 * Counts @ticket into @progress by the rule #VentureTicketTreeProgress
 * describes: a cancelled ticket or an epic adds nothing.
 */
void
venture_ticket_tree_progress_add(
	VentureTicketTreeProgress	*progress,
	VentureEntity			*ticket
);

/**
 * venture_ticket_tree_progress:
 * @database: the database to read
 * @root_id: the epic or story whose pieces to count
 * @out: (out caller-allocates): where the figures go; zeroed first
 * @error: (out) (optional): return location for a #GError
 *
 * The rollup of everything beneath @root_id: tickets done of total and
 * story points done of total, cancelled work and epics left out. Arithmetic
 * over the rows, nothing stored.
 *
 * Returns: %TRUE with @out filled, %FALSE with @error set
 */
gboolean
venture_ticket_tree_progress(
	VentureDatabase			 *database,
	gint64				  root_id,
	VentureTicketTreeProgress	 *out,
	GError				**error
);

/**
 * venture_ticket_tree_epic_progress:
 * @context: the wiring
 * @period: (nullable): unused beyond the result's heading
 * @options: (nullable): `milestone_id`, `goal_id`, `venture_id`,
 *   `organization_id`; see docs/tickets.org
 * @error: (out) (optional): return location for a #GError
 *
 * One row per live epic of the organization, narrowed in the query to the
 * epics planned into `milestone_id`, linked to `goal_id` or filed under
 * `venture_id`: its pieces done of total, their story points and the
 * percentage done. The epic's own milestone and goal are what narrow it;
 * its children's are never consulted, because they do not inherit them.
 *
 * Returns: (transfer full) (nullable): the result, or %NULL with @error set
 */
VentureReportResult *
venture_ticket_tree_epic_progress(
	VentureContext		 *context,
	VentureDateRange	 *period,
	JsonObject		 *options,
	GError			**error
);

/**
 * venture_ticket_tree_register_reports:
 * @registry: the report registry
 *
 * Registers `epic_progress`, which belongs to the tickets module.
 */
void
venture_ticket_tree_register_reports(VentureReportRegistry *registry);

G_END_DECLS

#endif /* VENTURE_TICKET_TREE_H */
