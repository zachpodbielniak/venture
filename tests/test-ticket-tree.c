/*
 * test-ticket-tree.c - Epics, stories and their pieces; milestones and goals
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A ticket tree is worth having only if its shape holds and its numbers
 * are honest. The shape: an epic tops a tree, a story hangs under an epic,
 * a subtask under the piece it splits, and no loop -- judged only when
 * written, so a row stored before the rule stays editable. The numbers:
 * an epic's rollup and a goal's work count every piece once at any depth
 * and leave cancelled work out of both halves. And a milestone is
 * planning, so it must work with nothing but tickets switched on. These
 * tests hold all three.
 */

#include <venture.h>

#include <libsoup/soup.h>
#include <string.h>

#include "venture-test-util.h"

typedef struct
{
	VentureConfig	*config;
	VentureDatabase	*database;
	VentureContext	*context;
	gint64		 organization_id;
} Fixture;

#define ID(record) (venture_entity_get_id(VENTURE_ENTITY(record)))

/* Saves, expecting success; the refusal's reason is the failure. */
static void
save(
	Fixture		*fixture,
	gpointer	 record
){
	g_autoptr(GError) error = NULL;

	if (!venture_database_save(fixture->database, VENTURE_ENTITY(record), NULL, &error))
		g_error("save refused: %s", error->message);
}

/* Saves, expecting a refusal whose message holds @fragment. */
static void
save_refused(
	Fixture		*fixture,
	gpointer	 record,
	const gchar	*fragment
){
	g_autoptr(GError) error = NULL;
	gboolean saved;

	saved = venture_database_save(fixture->database, VENTURE_ENTITY(record), NULL, &error);
	g_assert_false(saved);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);

	if (NULL == strstr(error->message, fragment))
		g_error("expected \"%s\" in: %s", fragment, error->message);
}

static void
module_registry_reset(void)
{
	g_autoptr(VentureConfig) everything = NULL;
	g_autoptr(VentureModuleRegistry) registry = NULL;

	/* The entity registry is process-wide; a test that switched a module
	 * off must not leave it off for the next. */
	everything = venture_config_new();
	registry = venture_module_registry_new();
	venture_module_registry_register_builtins(registry);
	venture_module_registry_configure(registry, everything, NULL);
	venture_module_registry_apply(registry, venture_entity_registry_get_default());
}

static void
fixture_set_up(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;

	(void)user_data;

	fixture->config = venture_config_new();
	fixture->database = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	fixture->context = venture_context_new(fixture->config, fixture->database);
	g_assert_true(venture_database_migrate(fixture->database,
		venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);

	fixture->organization_id =
		venture_context_get_default_organization_id(fixture->context);
}

static void
fixture_tear_down(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	(void)user_data;

	g_clear_object(&fixture->context);
	g_clear_object(&fixture->database);
	g_clear_object(&fixture->config);
	module_registry_reset();
}

/* --- Records --------------------------------------------------------------- */

/* A ticket of @type under @parent_id (0 for none), not yet saved. */
static VentureEntity *
ticket_new(
	Fixture			*fixture,
	const gchar		*title,
	VentureIssueType	 type,
	gint64			 parent_id
){
	VentureEntity *ticket;

	ticket = VENTURE_ENTITY(venture_ticket_new());
	g_object_set(ticket, "title", title, "issue-type", type,
	             "parent-id", parent_id, NULL);
	venture_entity_set_organization_id(ticket, fixture->organization_id);

	return ticket;
}

/* A ticket of @type under @parent_id, saved, with a status and points. */
static VentureEntity *
ticket_make(
	Fixture			*fixture,
	const gchar		*title,
	VentureIssueType	 type,
	gint64			 parent_id,
	VentureTicketStatus	 status,
	gint64			 points
){
	VentureEntity *ticket;

	ticket = ticket_new(fixture, title, type, parent_id);
	g_object_set(ticket, "status", status, "story-points", points, NULL);
	save(fixture, ticket);

	return ticket;
}

static VentureEntity *
reread(
	Fixture		*fixture,
	GType		 type,
	gint64		 id
){
	VentureEntity *record;

	record = venture_database_get(fixture->database, type, id, NULL);
	g_assert_nonnull(record);

	return record;
}

/* --- The shape ---------------------------------------------------------- */

/*
 * Every child type against no parent and a parent of every type, through
 * a real save, and the pure table agreeing with the validator. What
 * breaks: a story filed under a task, a subtask with nothing to split,
 * an epic nested in an epic and counted twice by every rollup -- or the
 * picker and the save disagreeing about which is which.
 */
static void
test_pairs(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	static const VentureIssueType types[] = {
		VENTURE_ISSUE_TYPE_TASK, VENTURE_ISSUE_TYPE_SUBTASK,
		VENTURE_ISSUE_TYPE_STORY, VENTURE_ISSUE_TYPE_EPIC,
		VENTURE_ISSUE_TYPE_BUG, VENTURE_ISSUE_TYPE_RESEARCH
	};
	/* allowed[child][parent + 1]; column 0 is "no parent". The columns
	 * follow types[]: task, subtask, story, epic, bug, research. */
	static const gboolean allowed[6][7] = {
		/* task */     { TRUE,  FALSE, FALSE, TRUE,  TRUE,  FALSE, FALSE },
		/* subtask */  { FALSE, TRUE,  FALSE, TRUE,  FALSE, TRUE,  TRUE  },
		/* story */    { TRUE,  FALSE, FALSE, FALSE, TRUE,  FALSE, FALSE },
		/* epic */     { TRUE,  FALSE, FALSE, FALSE, FALSE, FALSE, FALSE },
		/* bug */      { TRUE,  FALSE, FALSE, TRUE,  TRUE,  FALSE, FALSE },
		/* research */ { TRUE,  FALSE, FALSE, TRUE,  TRUE,  FALSE, FALSE }
	};
	g_autoptr(VentureEntity) epic = NULL;
	g_autoptr(VentureEntity) story = NULL;
	g_autoptr(VentureEntity) task = NULL;
	gint64 parents[6];
	guint c;
	guint p;

	(void)user_data;

	/* One valid parent of every shape. */
	epic = ticket_make(fixture, "Epic", VENTURE_ISSUE_TYPE_EPIC, 0,
	                   VENTURE_TICKET_STATUS_TODO, 0);
	story = ticket_make(fixture, "Story", VENTURE_ISSUE_TYPE_STORY, ID(epic),
	                    VENTURE_TICKET_STATUS_TODO, 0);
	task = ticket_make(fixture, "Task", VENTURE_ISSUE_TYPE_TASK, ID(story),
	                   VENTURE_TICKET_STATUS_TODO, 0);

	for (p = 0; p < G_N_ELEMENTS(types); p++)
	{
		g_autoptr(VentureEntity) parent = NULL;

		switch (types[p])
		{
		case VENTURE_ISSUE_TYPE_EPIC:
			parents[p] = ID(epic);
			continue;
		case VENTURE_ISSUE_TYPE_STORY:
			parents[p] = ID(story);
			continue;
		case VENTURE_ISSUE_TYPE_TASK:
			parents[p] = ID(task);
			continue;
		case VENTURE_ISSUE_TYPE_SUBTASK:
			parent = ticket_make(fixture, "Subtask", types[p], ID(task),
			                     VENTURE_TICKET_STATUS_TODO, 0);
			break;
		default:
			parent = ticket_make(fixture, "Loose", types[p], 0,
			                     VENTURE_TICKET_STATUS_TODO, 0);
			break;
		}

		parents[p] = ID(parent);
	}

	for (c = 0; c < G_N_ELEMENTS(types); c++)
	{
		for (p = 0; p <= G_N_ELEMENTS(types); p++)
		{
			g_autoptr(VentureEntity) child = NULL;
			g_autoptr(GError) error = NULL;
			gboolean expected;
			gboolean saved;
			gint64 parent_id;

			parent_id = (0 == p) ? 0 : parents[p - 1];
			expected = allowed[c][p];

			/* The pure table says the same as the save. */
			g_assert_cmpint(NULL == venture_ticket_tree_parent_allowed(types[c],
				0 != p, (0 == p) ? VENTURE_ISSUE_TYPE_TASK : types[p - 1]),
				==, expected);

			child = ticket_new(fixture, "Child", types[c], parent_id);
			saved = venture_database_save(fixture->database, child, NULL, &error);

			if (saved != expected)
				g_error("child %d under %s: expected %s, got %s", (gint)types[c],
				        (0 == p) ? "nothing" : venture_enum_to_nick(
				        	VENTURE_TYPE_ISSUE_TYPE, (gint)types[p - 1]),
				        expected ? "saved" : "refused",
				        saved ? "saved" : error->message);

			if (!expected)
			{
				g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
				g_assert_nonnull(strstr(error->message, "Part of"));
			}
		}
	}
}

/*
 * A ticket cannot be its own parent and cannot be put under one of its
 * own pieces. What breaks: a loop that every walk, rollup and goal count
 * would then go round -- bounded, and silently wrong.
 */
static void
test_loop(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) epic = NULL;
	g_autoptr(VentureEntity) story = NULL;
	g_autoptr(VentureEntity) task = NULL;
	g_autoptr(VentureEntity) subtask = NULL;

	(void)user_data;

	epic = ticket_make(fixture, "Epic", VENTURE_ISSUE_TYPE_EPIC, 0,
	                   VENTURE_TICKET_STATUS_TODO, 0);
	story = ticket_make(fixture, "Story", VENTURE_ISSUE_TYPE_STORY, ID(epic),
	                    VENTURE_TICKET_STATUS_TODO, 0);
	task = ticket_make(fixture, "Task", VENTURE_ISSUE_TYPE_TASK, ID(story),
	                   VENTURE_TICKET_STATUS_TODO, 0);
	subtask = ticket_make(fixture, "Subtask", VENTURE_ISSUE_TYPE_SUBTASK, ID(task),
	                      VENTURE_TICKET_STATUS_TODO, 0);

	g_object_set(story, "parent-id", ID(story), NULL);
	save_refused(fixture, story, "own parent");

	/* Under its own grandchild: refused as a loop, before its shape is
	 * even asked about. */
	g_object_set(story, "parent-id", ID(subtask), NULL);
	save_refused(fixture, story, "close a loop");
}

/*
 * A row that predates the rule -- a task under a task, written before the
 * validator existed -- keeps saving while its parent and type are left
 * alone, and is held to the rule the moment either is written. What
 * breaks: every old ticket becoming uneditable on upgrade, or the rule
 * never applying to anything that was ever wrong.
 */
static void
test_legacy_row_editable(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) first = NULL;
	g_autoptr(VentureEntity) second = NULL;
	g_autoptr(VentureEntity) stored = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *sql = NULL;

	(void)user_data;

	first = ticket_make(fixture, "First", VENTURE_ISSUE_TYPE_TASK, 0,
	                    VENTURE_TICKET_STATUS_TODO, 0);
	second = ticket_make(fixture, "Second", VENTURE_ISSUE_TYPE_TASK, 0,
	                     VENTURE_TICKET_STATUS_TODO, 0);

	/* Behind the validator's back, as an old install's rows are. */
	sql = g_strdup_printf("UPDATE tickets SET parent_id = %" G_GINT64_FORMAT
	                      " WHERE id = %" G_GINT64_FORMAT, ID(first), ID(second));
	g_assert_true(venture_database_execute(fixture->database, sql, NULL, &error));
	g_assert_no_error(error);

	stored = reread(fixture, VENTURE_TYPE_TICKET, ID(second));
	g_object_set(stored, "title", "Second, renamed", "story-points", (gint64)3, NULL);
	save(fixture, stored);

	/* Touching the type is writing the hierarchy: judged now. */
	g_object_set(stored, "issue-type", VENTURE_ISSUE_TYPE_BUG, NULL);
	save_refused(fixture, stored, "epic or a story");

	/* And moving it out from under the task fixes it. */
	g_object_set(stored, "parent-id", (gint64)0, NULL);
	save(fixture, stored);
}

/*
 * Changing a ticket's type so that a piece already under it may no longer
 * be there is refused, naming the piece. What breaks: a story turned
 * into a task with tasks still under it, leaving a tree the validator
 * would refuse to create -- and nobody told which ticket to move.
 */
static void
test_type_change_orphans(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) epic = NULL;
	g_autoptr(VentureEntity) story = NULL;
	g_autoptr(VentureEntity) task = NULL;
	g_autoptr(VentureEntity) subtask = NULL;
	g_autofree gchar *named = NULL;

	(void)user_data;

	epic = ticket_make(fixture, "Epic", VENTURE_ISSUE_TYPE_EPIC, 0,
	                   VENTURE_TICKET_STATUS_TODO, 0);
	story = ticket_make(fixture, "Story", VENTURE_ISSUE_TYPE_STORY, ID(epic),
	                    VENTURE_TICKET_STATUS_TODO, 0);
	task = ticket_make(fixture, "Wire the export", VENTURE_ISSUE_TYPE_TASK,
	                   ID(story), VENTURE_TICKET_STATUS_TODO, 0);
	subtask = ticket_make(fixture, "Piece", VENTURE_ISSUE_TYPE_SUBTASK, ID(task),
	                      VENTURE_TICKET_STATUS_TODO, 0);

	/* A story turned bug: a bug may sit under the epic, but the task
	 * under it may not sit under a bug. */
	named = g_strdup_printf("#%" G_GINT64_FORMAT " (Wire the export)", ID(task));
	g_object_set(story, "issue-type", VENTURE_ISSUE_TYPE_BUG, NULL);
	save_refused(fixture, story, named);
	save_refused(fixture, story, "Issue type");

	/* An epic with a story under it cannot become a story. */
	g_clear_object(&story);
	story = reread(fixture, VENTURE_TYPE_TICKET, ID(epic));
	g_object_set(story, "issue-type", VENTURE_ISSUE_TYPE_STORY, NULL);
	save_refused(fixture, story, "a story's parent must be an epic");

	/* A task with a subtask may become a bug: a subtask may split one. */
	g_clear_object(&story);
	story = reread(fixture, VENTURE_TYPE_TICKET, ID(task));
	g_object_set(story, "issue-type", VENTURE_ISSUE_TYPE_BUG, NULL);
	save(fixture, story);
}

/* --- The numbers -------------------------------------------------------- */

/*
 * An epic's rollup counts every piece at any depth and leaves cancelled
 * work out of both halves; epic_progress reports the same figures and is
 * narrowed by the epic's own milestone, never its children's. What
 * breaks: a cancelled ticket read as done (a cut plan looking finished),
 * or a deep subtask missing from the count.
 */
static void
test_rollup_excludes_cancelled(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) milestone = NULL;
	g_autoptr(VentureEntity) epic = NULL;
	g_autoptr(VentureEntity) other = NULL;
	g_autoptr(VentureEntity) story = NULL;
	g_autoptr(VentureEntity) task = NULL;
	g_autoptr(VentureEntity) dropped = NULL;
	g_autoptr(VentureEntity) subtask = NULL;
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(JsonObject) options = NULL;
	g_autoptr(GError) error = NULL;
	VentureTicketTreeProgress progress;
	VentureReport *report;
	const GValue *value;
	gint64 total;
	gint64 done;

	(void)user_data;

	milestone = VENTURE_ENTITY(venture_milestone_new());
	g_object_set(milestone, "name", "1.0", NULL);
	venture_entity_set_organization_id(milestone, fixture->organization_id);
	save(fixture, milestone);

	epic = ticket_new(fixture, "Checkout", VENTURE_ISSUE_TYPE_EPIC, 0);
	g_object_set(epic, "milestone-id", ID(milestone), NULL);
	save(fixture, epic);
	other = ticket_make(fixture, "Search", VENTURE_ISSUE_TYPE_EPIC, 0,
	                    VENTURE_TICKET_STATUS_TODO, 0);

	story = ticket_make(fixture, "Pay by card", VENTURE_ISSUE_TYPE_STORY, ID(epic),
	                    VENTURE_TICKET_STATUS_DONE, 3);
	task = ticket_make(fixture, "Card form", VENTURE_ISSUE_TYPE_TASK, ID(story),
	                   VENTURE_TICKET_STATUS_TODO, 5);
	dropped = ticket_make(fixture, "Cheques", VENTURE_ISSUE_TYPE_TASK, ID(epic),
	                      VENTURE_TICKET_STATUS_CANCELLED, 8);
	subtask = ticket_make(fixture, "Validate", VENTURE_ISSUE_TYPE_SUBTASK, ID(task),
	                      VENTURE_TICKET_STATUS_DONE, 1);

	g_assert_true(venture_ticket_tree_progress(fixture->database, ID(epic),
	                                           &progress, &error));
	g_assert_no_error(error);
	g_assert_cmpint(progress.total, ==, 3);
	g_assert_cmpint(progress.done, ==, 2);
	g_assert_cmpint(progress.points_total, ==, 9);
	g_assert_cmpint(progress.points_done, ==, 4);

	/* Nothing was written down the tree: the story did not inherit the
	 * epic's milestone. */
	{
		g_autoptr(VentureEntity) stored = NULL;
		gint64 inherited;

		stored = reread(fixture, VENTURE_TYPE_TICKET, ID(story));
		g_object_get(stored, "milestone-id", &inherited, NULL);
		g_assert_cmpint(inherited, ==, 0);
	}

	/* A milestone's progress leaves the cancelled one out too. */
	g_object_set(dropped, "milestone-id", ID(milestone), NULL);
	save(fixture, dropped);
	venture_factory_milestone_progress(fixture->database, ID(milestone), &total,
	                                   &done);
	g_assert_cmpint(total, ==, 1);
	g_assert_cmpint(done, ==, 0);

	/* --- The report, narrowed by the epic's milestone --- */

	report = venture_report_registry_lookup(
		venture_context_get_report_registry(fixture->context), "epic_progress");
	g_assert_nonnull(report);

	options = json_object_new();
	json_object_set_int_member(options, "milestone_id", ID(milestone));
	result = venture_report_generate(report, fixture->context, NULL, options,
	                                 &error);
	g_assert_no_error(error);
	g_assert_cmpuint(venture_report_result_get_row_count(result), ==, 1);
	value = venture_report_result_get_cell(result, 0, "total");
	g_assert_cmpfloat(g_value_get_double(value), ==, 3.0);
	value = venture_report_result_get_cell(result, 0, "points");
	g_assert_cmpfloat(g_value_get_double(value), ==, 9.0);
	value = venture_report_result_get_cell(result, 0, "percent");
	g_assert_cmpfloat_with_epsilon(g_value_get_double(value), 2.0 / 3.0, 1e-9);

	/* Unnarrowed: both epics, and the empty one has no percentage rather
	 * than 0%. */
	g_clear_object(&result);
	result = venture_report_generate(report, fixture->context, NULL, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(venture_report_result_get_row_count(result), ==, 2);
	g_assert_null(venture_report_result_get_cell(result, 1, "percent"));
}

/*
 * A goal's work is the tickets naming it, everything beneath them and the
 * tickets of milestones naming it -- each once, however many ways it is
 * linked -- and reading it never moves the goal. What breaks: a story
 * that is both under a linked epic and on a linked milestone counted
 * twice, an epic's pieces missing because they do not carry the goal, or
 * the goal's current value changing under its owner.
 */
static void
test_goal_work_counts_once(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) goal = NULL;
	g_autoptr(VentureEntity) stored_goal = NULL;
	g_autoptr(VentureEntity) milestone = NULL;
	g_autoptr(VentureEntity) epic = NULL;
	g_autoptr(VentureEntity) story = NULL;
	g_autoptr(VentureEntity) task = NULL;
	g_autoptr(VentureEntity) loose = NULL;
	g_autoptr(VentureEntity) dropped = NULL;
	g_autoptr(VentureEntity) elsewhere = NULL;
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(GError) error = NULL;
	VentureGoalWork work;
	VentureReport *report;
	gdouble current;

	(void)user_data;

	goal = VENTURE_ENTITY(venture_goal_new());
	g_object_set(goal, "name", "Launch", "start-value", 0.0, "current-value", 10.0,
	             "target-value", 100.0, NULL);
	venture_entity_set_organization_id(goal, fixture->organization_id);
	save(fixture, goal);

	milestone = VENTURE_ENTITY(venture_milestone_new());
	g_object_set(milestone, "name", "Beta", "goal-id", ID(goal), NULL);
	venture_entity_set_organization_id(milestone, fixture->organization_id);
	save(fixture, milestone);

	epic = ticket_new(fixture, "Onboarding", VENTURE_ISSUE_TYPE_EPIC, 0);
	g_object_set(epic, "goal-id", ID(goal), "milestone-id", ID(milestone), NULL);
	save(fixture, epic);

	/* Under the linked epic and on the linked milestone: once. */
	story = ticket_new(fixture, "Sign-up", VENTURE_ISSUE_TYPE_STORY, ID(epic));
	g_object_set(story, "milestone-id", ID(milestone),
	             "status", VENTURE_TICKET_STATUS_DONE, NULL);
	save(fixture, story);
	task = ticket_make(fixture, "Email check", VENTURE_ISSUE_TYPE_TASK, ID(story),
	                   VENTURE_TICKET_STATUS_TODO, 2);

	/* Naming the goal and the milestone both: once. */
	loose = ticket_new(fixture, "Docs", VENTURE_ISSUE_TYPE_TASK, 0);
	g_object_set(loose, "goal-id", ID(goal), "milestone-id", ID(milestone),
	             "status", VENTURE_TICKET_STATUS_DONE, NULL);
	save(fixture, loose);

	dropped = ticket_make(fixture, "Fax", VENTURE_ISSUE_TYPE_TASK, ID(epic),
	                      VENTURE_TICKET_STATUS_CANCELLED, 0);
	elsewhere = ticket_make(fixture, "Unrelated", VENTURE_ISSUE_TYPE_TASK, 0,
	                        VENTURE_TICKET_STATUS_TODO, 0);

	g_assert_true(venture_goals_work_progress(fixture->database, ID(goal), &work,
	                                          &error));
	g_assert_no_error(error);
	g_assert_cmpint(work.milestones, ==, 1);
	g_assert_cmpint(work.epics, ==, 1);
	g_assert_cmpint(work.tickets.total, ==, 3);
	g_assert_cmpint(work.tickets.done, ==, 2);

	/* goal_progress carries the same figures, and the goal itself did not
	 * move. */
	report = venture_report_registry_lookup(
		venture_context_get_report_registry(fixture->context), "goal_progress");
	g_assert_nonnull(report);
	result = venture_report_generate(report, fixture->context, NULL, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(venture_report_result_get_row_count(result), ==, 1);
	g_assert_cmpfloat(g_value_get_double(venture_report_result_get_cell(result, 0,
		"tickets_total")), ==, 3.0);
	g_assert_cmpfloat(g_value_get_double(venture_report_result_get_cell(result, 0,
		"tickets_done")), ==, 2.0);

	stored_goal = reread(fixture, VENTURE_TYPE_GOAL, ID(goal));
	g_object_get(stored_goal, "current-value", &current, NULL);
	g_assert_cmpfloat(current, ==, 10.0);
	g_assert_cmpint(venture_entity_get_version(stored_goal), ==,
	                venture_entity_get_version(goal));
}

/* --- Milestones belong to tickets --------------------------------------- */

/*
 * With the factory and the forge switched off a milestone is still a
 * record, filed under the tickets module, and its completed-at still
 * follows its status. What breaks: a tickets-only install that cannot
 * plan an increment, or a milestone marked completed with no date that
 * no report would ever see.
 */
static void
test_milestone_without_factory(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) milestone = NULL;
	g_autoptr(VentureEntity) stored = NULL;
	g_autoptr(GDateTime) completed = NULL;
	VentureModuleRegistry *modules;

	(void)user_data;

	modules = venture_context_get_modules(fixture->context);
	g_assert_true(g_strv_contains(venture_module_get_entity_names(
		venture_module_registry_lookup(modules, "tickets")), "milestone"));
	g_assert_false(g_strv_contains(venture_module_get_entity_names(
		venture_module_registry_lookup(modules, "factory")), "milestone"));
	g_assert_true(g_strv_contains(venture_module_get_reports(
		venture_module_registry_lookup(modules, "tickets")), "epic_progress"));

	venture_config_set_module_enabled(fixture->config, "factory", FALSE);
	venture_config_set_module_enabled(fixture->config, "forge", FALSE);
	g_assert_false(venture_context_module_enabled(fixture->context, "factory"));
	g_assert_false(venture_context_module_enabled(fixture->context, "forge"));
	g_assert_true(venture_context_module_enabled(fixture->context, "tickets"));
	g_assert_true(VENTURE_TYPE_MILESTONE == venture_entity_registry_lookup(
		venture_context_get_entity_registry(fixture->context), "milestone"));
	g_assert_true(G_TYPE_INVALID == venture_entity_registry_lookup(
		venture_context_get_entity_registry(fixture->context), "release"));

	milestone = VENTURE_ENTITY(venture_milestone_new());
	g_object_set(milestone, "name", "Q1", "status",
	             VENTURE_MILESTONE_STATUS_COMPLETED, NULL);
	venture_entity_set_organization_id(milestone, fixture->organization_id);
	save(fixture, milestone);

	stored = reread(fixture, VENTURE_TYPE_MILESTONE, ID(milestone));
	g_object_get(stored, "completed-at", &completed, NULL);
	g_assert_nonnull(completed);

	/* Reopened, the date goes with the state. */
	g_clear_pointer(&completed, g_date_time_unref);
	g_object_set(stored, "status", VENTURE_MILESTONE_STATUS_ACTIVE, NULL);
	save(fixture, stored);
	g_object_get(stored, "completed-at", &completed, NULL);
	g_assert_null(completed);
}

/* ==========================================================================
 * HTTP
 * ========================================================================== */

typedef struct
{
	VentureConfig		*config;
	VentureDatabase		*database;
	VentureContext		*context;
	VentureWebServer	*server;
	SoupSession		*session;
	gchar			*state_dir;
	gchar			*cookie;
	guint16			 port;
} ServerFixture;

typedef struct
{
	gboolean	 done;
	GBytes		*body;
	GError		*error;
} RequestResult;

static void
request_done(
	GObject		*source,
	GAsyncResult	*result,
	gpointer	 user_data
){
	RequestResult *outcome;

	outcome = user_data;
	outcome->body = soup_session_send_and_read_finish(SOUP_SESSION(source),
	                                                  result, &outcome->error);
	outcome->done = TRUE;
}

static guint
server_request(
	ServerFixture	 *fixture,
	const gchar	 *method,
	const gchar	 *path,
	const gchar	 *content_type,
	const gchar	 *body,
	gchar		**out_body,
	gchar		**out_set_cookie
){
	g_autoptr(SoupMessage) message = NULL;
	g_autofree gchar *url = NULL;
	RequestResult outcome = { FALSE, NULL, NULL };

	url = g_strdup_printf("http://127.0.0.1:%u%s", fixture->port, path);
	message = soup_message_new(method, url);
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);

	if (NULL != fixture->cookie)
		soup_message_headers_append(soup_message_get_request_headers(message),
		                            "Cookie", fixture->cookie);

	if (NULL != body)
	{
		g_autoptr(GBytes) bytes = NULL;

		bytes = g_bytes_new(body, strlen(body));
		soup_message_set_request_body_from_bytes(message, content_type, bytes);
	}

	soup_session_send_and_read_async(fixture->session, message,
	                                 G_PRIORITY_DEFAULT, NULL, request_done,
	                                 &outcome);

	while (!outcome.done)
		g_main_context_iteration(NULL, TRUE);

	if (NULL != outcome.error)
		g_error("%s %s: %s", method, path, outcome.error->message);

	if (NULL != out_body)
		*out_body = g_strndup(g_bytes_get_data(outcome.body, NULL),
		                      g_bytes_get_size(outcome.body));

	if (NULL != out_set_cookie)
		*out_set_cookie = g_strdup(soup_message_headers_get_one(
			soup_message_get_response_headers(message), "Set-Cookie"));

	g_clear_pointer(&outcome.body, g_bytes_unref);
	g_clear_error(&outcome.error);

	return soup_message_get_status(message);
}

static void
server_fixture_set_up(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureUser) user = NULL;
	g_autofree gchar *set_cookie = NULL;
	gchar *semicolon;

	(void)user_data;

	g_setenv("VENTURE_TEST_SESSION_SECRET", "ticket-tree-test-secret", TRUE);

	fixture->state_dir = g_dir_make_tmp("venture-ticket-tree-XXXXXX", NULL);

	fixture->config = venture_config_new();
	/* Port 0: the kernel picks a free one, read back after the start. */
	g_object_set(fixture->config,
	             "state-dir", fixture->state_dir,
	             "server-bind-address", "127.0.0.1",
	             "server-port", (gint64)0,
	             "security-session-secret-env", "VENTURE_TEST_SESSION_SECRET",
	             "security-password-iterations", (gint64)100000,
	             NULL);

	fixture->database = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	fixture->context = venture_context_new(fixture->config, fixture->database);
	g_assert_true(venture_database_migrate(fixture->database,
		venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);

	fixture->server = venture_web_server_new(fixture->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(fixture->server, &error));
	g_assert_no_error(error);
	fixture->port = venture_web_server_get_port(fixture->server);
	fixture->session = soup_session_new();

	user = venture_user_new();
	g_object_set(user, "username", "owner", "role", VENTURE_USER_ROLE_OWNER,
	             "active", TRUE, NULL);
	g_assert_true(venture_user_set_password(user, "owner-password-1", 100000,
	                                        NULL));
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(user), NULL, NULL));

	server_request(fixture, "POST", "/login", "application/x-www-form-urlencoded",
	               "username=owner&password=owner-password-1", NULL, &set_cookie);
	g_assert_nonnull(set_cookie);
	semicolon = strchr(set_cookie, ';');

	if (NULL != semicolon)
		*semicolon = '\0';

	fixture->cookie = g_steal_pointer(&set_cookie);
}

static void
server_fixture_tear_down(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	(void)user_data;

	if (NULL != fixture->server)
		venture_web_server_stop(fixture->server);

	g_clear_pointer(&fixture->cookie, g_free);
	g_clear_object(&fixture->session);
	g_clear_object(&fixture->server);
	g_clear_object(&fixture->context);
	g_clear_object(&fixture->database);
	g_clear_object(&fixture->config);

	if (NULL != fixture->state_dir)
	{
		venture_test_remove_tree(fixture->state_dir);
		g_clear_pointer(&fixture->state_dir, g_free);
	}

	g_unsetenv("VENTURE_TEST_SESSION_SECRET");
	module_registry_reset();
}

/* Saves @record under the default organization, expecting success. */
static void
server_save(
	ServerFixture	*fixture,
	gpointer	 record
){
	g_autoptr(GError) error = NULL;

	venture_entity_set_organization_id(VENTURE_ENTITY(record),
		venture_context_get_default_organization_id(fixture->context));

	if (!venture_database_save(fixture->database, VENTURE_ENTITY(record), NULL, &error))
		g_error("save refused: %s", error->message);
}

/*
 * The doors: an epic's page draws its pieces, a goal's page its work, the
 * API forwards milestone_id to epic_progress (an option dropped on the way
 * answers a different question), the record API applies the hierarchy,
 * and a milestone's page and forecast answer with the factory off. What
 * breaks: a block that never renders, an option the API never passes, a
 * door around the rule, or milestones gated on a module they left.
 */
static void
test_http(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) goal = NULL;
	g_autoptr(VentureEntity) milestone = NULL;
	g_autoptr(VentureEntity) epic = NULL;
	g_autoptr(VentureEntity) other = NULL;
	g_autoptr(VentureEntity) story = NULL;
	g_autofree gchar *body = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *request = NULL;

	(void)user_data;

	goal = VENTURE_ENTITY(venture_goal_new());
	g_object_set(goal, "name", "Ship it", "start-value", 0.0,
	             "target-value", 1.0, NULL);
	server_save(fixture, goal);
	milestone = VENTURE_ENTITY(venture_milestone_new());
	g_object_set(milestone, "name", "Gamma", "goal-id", ID(goal), NULL);
	server_save(fixture, milestone);
	epic = VENTURE_ENTITY(venture_ticket_new());
	g_object_set(epic, "title", "Big rewrite", "issue-type", VENTURE_ISSUE_TYPE_EPIC,
	             "goal-id", ID(goal), "milestone-id", ID(milestone), NULL);
	server_save(fixture, epic);
	other = VENTURE_ENTITY(venture_ticket_new());
	g_object_set(other, "title", "Small polish", "issue-type",
	             VENTURE_ISSUE_TYPE_EPIC, NULL);
	server_save(fixture, other);
	story = VENTURE_ENTITY(venture_ticket_new());
	g_object_set(story, "title", "Port the parser", "issue-type",
	             VENTURE_ISSUE_TYPE_STORY, "parent-id", ID(epic),
	             "status", VENTURE_TICKET_STATUS_DONE, NULL);
	server_save(fixture, story);

	/* The epic's page lists its pieces. */
	path = g_strdup_printf("/e/ticket/%" G_GINT64_FORMAT, ID(epic));
	g_assert_cmpuint(server_request(fixture, "GET", path, NULL, NULL, &body, NULL),
	                 ==, SOUP_STATUS_OK);
	g_assert_nonnull(strstr(body, "card ticket-tree"));
	g_assert_nonnull(strstr(body, "Port the parser"));
	g_assert_nonnull(strstr(body, "1 of 1 done"));

	/* The goal's page lists its milestone and epic. */
	g_clear_pointer(&body, g_free);
	g_clear_pointer(&path, g_free);
	path = g_strdup_printf("/e/goal/%" G_GINT64_FORMAT, ID(goal));
	g_assert_cmpuint(server_request(fixture, "GET", path, NULL, NULL, &body, NULL),
	                 ==, SOUP_STATUS_OK);
	g_assert_nonnull(strstr(body, "card goal-work"));
	g_assert_nonnull(strstr(body, "Gamma"));
	g_assert_nonnull(strstr(body, "Big rewrite"));

	/* The API forwards milestone_id. */
	g_clear_pointer(&body, g_free);
	g_clear_pointer(&path, g_free);
	path = g_strdup_printf("/api/v1/reports/epic_progress?period=all&milestone_id=%"
	                       G_GINT64_FORMAT, ID(milestone));
	g_assert_cmpuint(server_request(fixture, "GET", path, NULL, NULL, &body, NULL),
	                 ==, SOUP_STATUS_OK);
	g_assert_nonnull(strstr(body, "Big rewrite"));
	g_assert_null(strstr(body, "Small polish"));

	/* The record API applies the hierarchy. */
	g_clear_pointer(&body, g_free);
	request = g_strdup_printf("{\"title\":\"Nested epic\",\"issue_type\":\"epic\","
	                          "\"parent_id\":%" G_GINT64_FORMAT "}", ID(other));
	g_assert_cmpuint(server_request(fixture, "POST", "/api/v1/ticket",
	                                "application/json", request, &body, NULL),
	                 ==, 422);
	g_assert_nonnull(strstr(body, "takes no parent"));

	/* The record API takes an end time beside the due time. */
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(server_request(fixture, "POST", "/api/v1/ticket",
		"application/json", "{\"title\":\"Paint the porch\","
		"\"due_at\":\"2026-11-02T14:00:00Z\","
		"\"until_at\":\"2026-11-02T16:30:00Z\"}", &body, NULL),
		==, SOUP_STATUS_CREATED);
	g_assert_nonnull(strstr(body, "\"until_at\""));
	g_assert_nonnull(strstr(body, "2026-11-02T16:30:00"));

	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(server_request(fixture, "POST", "/api/v1/ticket",
		"application/json", "{\"title\":\"Backwards\","
		"\"due_at\":\"2026-11-02T14:00:00Z\","
		"\"until_at\":\"2026-11-02T13:00:00Z\"}", &body, NULL),
		==, 422);
	g_assert_nonnull(strstr(body, "before"));

	/* Factory and forge off: the milestone's page and forecast answer. */
	venture_config_set_module_enabled(fixture->config, "factory", FALSE);
	venture_config_set_module_enabled(fixture->config, "forge", FALSE);
	g_assert_false(venture_context_module_enabled(fixture->context, "factory"));

	g_clear_pointer(&body, g_free);
	g_clear_pointer(&path, g_free);
	path = g_strdup_printf("/e/milestone/%" G_GINT64_FORMAT, ID(milestone));
	g_assert_cmpuint(server_request(fixture, "GET", path, NULL, NULL, &body, NULL),
	                 ==, SOUP_STATUS_OK);
	g_assert_nonnull(strstr(body, "<h2>Progress</h2>"));

	g_clear_pointer(&body, g_free);
	g_clear_pointer(&path, g_free);
	path = g_strdup_printf("/api/v1/milestones/%" G_GINT64_FORMAT "/forecast",
	                       ID(milestone));
	g_assert_cmpuint(server_request(fixture, "GET", path, NULL, NULL, &body, NULL),
	                 ==, SOUP_STATUS_OK);
	g_assert_nonnull(strstr(body, "milestone_id"));
}

/* --- The end time ------------------------------------------------------ */

/*
 * A task's end time is its own field beside its due time, read back as
 * written, and never before the due time. Judged only when written, so a
 * row stored backwards stays editable. What breaks: an end time kept as
 * a sentence in the description, or a task that ends before it starts.
 */
static void
test_until_at(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) task = NULL;
	g_autoptr(VentureEntity) backwards = NULL;
	g_autoptr(VentureEntity) again = NULL;
	g_autoptr(GDateTime) due = NULL;
	g_autoptr(GDateTime) until = NULL;
	g_autoptr(GDateTime) early = NULL;
	g_autoptr(GDateTime) read = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *sql = NULL;
	gint64 old_id;

	(void)user_data;

	due = g_date_time_new_utc(2026, 11, 2, 14, 0, 0);
	until = g_date_time_new_utc(2026, 11, 2, 16, 30, 0);
	early = g_date_time_new_utc(2026, 11, 2, 13, 0, 0);

	task = ticket_new(fixture, "Paint the porch", VENTURE_ISSUE_TYPE_TASK, 0);
	g_object_set(task, "due-at", due, "until-at", until, NULL);
	save(fixture, task);

	again = reread(fixture, VENTURE_TYPE_TICKET, ID(task));
	g_object_get(again, "until-at", &read, NULL);
	g_assert_nonnull(read);
	g_assert_true(g_date_time_equal(read, until));

	/* An end before the due time is refused, naming the field. */
	g_object_set(again, "until-at", early, NULL);
	save_refused(fixture, again, "before");

	/* An end time alone is kept. */
	g_object_set(again, "due-at", NULL, NULL);
	save(fixture, again);

	/* A row stored backwards (by SQL) stays editable for its title. */
	backwards = ticket_new(fixture, "Old row", VENTURE_ISSUE_TYPE_TASK, 0);
	g_object_set(backwards, "due-at", due, NULL);
	save(fixture, backwards);
	sql = g_strdup_printf("UPDATE tickets SET until_at = '2026-11-02T13:00:00Z' "
	                      "WHERE id = %" G_GINT64_FORMAT, ID(backwards));
	g_assert_true(venture_database_execute(fixture->database, sql, NULL, &error));
	g_assert_no_error(error);
	old_id = ID(backwards);
	g_clear_object(&backwards);
	backwards = reread(fixture, VENTURE_TYPE_TICKET, old_id);
	g_object_set(backwards, "title", "Old row, renamed", NULL);
	save(fixture, backwards);
}

int
main(
	int	 argc,
	char	**argv
){
	g_test_init(&argc, &argv, NULL);

#define ADD(path, func) \
	g_test_add(path, Fixture, NULL, fixture_set_up, func, fixture_tear_down)

	ADD("/ticket-tree/pairs", test_pairs);
	ADD("/ticket-tree/loop", test_loop);
	ADD("/ticket-tree/legacy-row-editable", test_legacy_row_editable);
	ADD("/ticket-tree/type-change-orphans", test_type_change_orphans);
	ADD("/ticket-tree/rollup-excludes-cancelled", test_rollup_excludes_cancelled);
	ADD("/ticket-tree/goal-work-counts-once", test_goal_work_counts_once);
	ADD("/ticket-tree/milestone-without-factory", test_milestone_without_factory);
	ADD("/ticket-tree/until-at", test_until_at);
	g_test_add("/ticket-tree/http", ServerFixture, NULL, server_fixture_set_up,
	           test_http, server_fixture_tear_down);

#undef ADD

	return g_test_run();
}
