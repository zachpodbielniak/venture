/*
 * test-automation.c - The automation engine's dispatch contract
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * These tests exist because of where podomation's dispatch can put a
 * handler rather than because of what a handler computes. The engine picks
 * between a direct call and an async wrapper based on a timeout it holds in
 * its own configuration, and the async wrapper runs a synchronous handler on
 * a worker thread while the caller blocks in a nested main loop. VENTURE's
 * pod module writes to the database and reads a cascade guard, so which of
 * those two paths it lands on is not a detail -- it decides whether the
 * whole automation subsystem is thread-safe.
 */

#include <venture.h>

#include <glib.h>

typedef struct
{
	VentureConfig	*config;
	VentureDatabase	*database;
	VentureContext	*context;
	gchar		*state_dir;
} Fixture;

static void
fixture_set_up(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;

	(void)user_data;

	fixture->state_dir = g_dir_make_tmp("venture-automation-XXXXXX", &error);
	g_assert_no_error(error);

	fixture->config = venture_config_new();
	g_object_set(fixture->config, "state-dir", fixture->state_dir, NULL);
	g_object_set(fixture->config, "automation-enabled", TRUE, NULL);

	fixture->database = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);

	g_assert_true(venture_database_migrate(fixture->database,
		venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);

	fixture->context = venture_context_new(fixture->config, fixture->database);
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

	if (NULL != fixture->state_dir)
	{
		g_autoptr(GFile) directory = NULL;

		directory = g_file_new_for_path(fixture->state_dir);
		g_file_delete(directory, NULL, NULL);
		g_clear_pointer(&fixture->state_dir, g_free);
	}
}

typedef struct
{
	GThread	*handler_thread;
	guint	 saves;
} ThreadWitness;

static void
on_entity_saved(
	VentureDatabase	*database,
	VentureEntity	*entity,
	gboolean	 created,
	gpointer	 user_data
){
	ThreadWitness *witness = user_data;

	(void)database;
	(void)created;

	/* Only the record the rule creates, not the one that triggered it. */
	if (!VENTURE_IS_IDEA(entity))
		return;

	witness->handler_thread = g_thread_self();
	witness->saves++;
}

/*
 * A pod handler must run on the thread that dispatched it.
 *
 * What breaks if this regresses: VentureAutomation calls pod_engine_new()
 * and podomation's default handler_timeout_seconds is 30. Any non-zero
 * timeout selects the async dispatch path, which fires handle_event_async
 * and blocks the caller in g_main_loop_run() on the default context.
 * VenturePodModule implements only the synchronous handle_event, so
 * podomation's default wrapper runs it via g_task_run_in_thread -- and that
 * handler calls venture_database_save() and reads the `dispatching` cascade
 * guard, a plain guint. The symptoms are not a crash: they are automations
 * that silently stop firing for some records because a non-atomic counter
 * was mutated from two threads, and HTTP requests dispatched re-entrantly
 * from inside the nested loop.
 *
 * The fix is one line in venture_automation_build_engine() setting the
 * timeout to zero. This test is what stops it being tidied away again.
 */
static void
test_automation_handlers_run_on_the_calling_thread(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureAutomation) automation = NULL;
	g_autoptr(VentureTicket) ticket = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *pods_path = NULL;
	ThreadWitness witness = { NULL, 0 };
	static const gchar *const rules =
		"pod watcher = venture->new();\n"
		"watcher->on_created => venture->create(\"idea\","
		" \"title=raised by automation\");\n";

	(void)user_data;

	pods_path = g_build_filename(fixture->state_dir, "automations.pod", NULL);
	g_assert_true(g_file_set_contents(pods_path, rules, -1, &error));
	g_assert_no_error(error);

	automation = venture_automation_new(fixture->context, &error);
	g_assert_no_error(error);
	g_assert_nonnull(automation);

	g_assert_true(venture_automation_start(automation, &error));
	g_assert_no_error(error);

	g_signal_connect(fixture->database, "entity-saved",
	                 G_CALLBACK(on_entity_saved), &witness);

	ticket = venture_ticket_new();
	g_object_set(ticket, "title", "trigger", NULL);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(ticket), NULL, &error));
	g_assert_no_error(error);

	/* The rule ran at all... */
	g_assert_cmpuint(witness.saves, ==, 1);

	/* ...and it ran here, not on a worker thread podomation borrowed. */
	g_assert_true(witness.handler_thread == g_thread_self());

	venture_automation_stop(automation);
}

/*
 * A record written by an automation must not trigger the automation again.
 *
 * What breaks if this regresses: a rule bound to on_created that creates a
 * record is an unbounded loop with a disk-full at the end of it. The guard
 * is a depth counter in venture_automation_emit_entity_event(), and it is
 * only sound while handlers stay on one thread -- which is what the test
 * above pins. This one pins the behaviour that depends on it.
 */
static void
test_automation_does_not_cascade(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureAutomation) automation = NULL;
	g_autoptr(VentureTicket) ticket = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *pods_path = NULL;
	static const gchar *const rules =
		"pod watcher = venture->new();\n"
		"watcher->on_created => venture->create(\"idea\","
		" \"title=raised by automation\");\n";

	(void)user_data;

	pods_path = g_build_filename(fixture->state_dir, "automations.pod", NULL);
	g_assert_true(g_file_set_contents(pods_path, rules, -1, &error));
	g_assert_no_error(error);

	automation = venture_automation_new(fixture->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_automation_start(automation, &error));
	g_assert_no_error(error);

	ticket = venture_ticket_new();
	g_object_set(ticket, "title", "trigger", NULL);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(ticket), NULL, &error));
	g_assert_no_error(error);

	/* Exactly one, not one per generation. */
	query = venture_query_new(VENTURE_TYPE_IDEA);
	g_assert_cmpint(venture_database_count(fixture->database, query, NULL),
	                ==, 1);

	venture_automation_stop(automation);
}

/*
 * Every rule file shipped in data/examples/ parses.
 *
 * These are documentation people copy into a live rules file, and the
 * Automations page validates with this same parser -- so a shipped example
 * that does not parse is a bug report waiting to be filed by whoever
 * followed it. The directory is walked rather than listed, so a new example
 * is covered the moment it is added.
 */
static void
test_automation_shipped_examples_parse(void)
{
	g_autoptr(GDir) dir = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *name;
	guint checked;

	dir = g_dir_open(VENTURE_TEST_EXAMPLES, 0, &error);

	g_assert_no_error(error);
	g_assert_nonnull(dir);

	checked = 0;

	while (NULL != (name = g_dir_read_name(dir)))
	{
		g_autofree gchar *path = NULL;
		g_autofree gchar *source = NULL;
		g_autofree gchar *message = NULL;

		if (!g_str_has_suffix(name, ".pod"))
			continue;

		path = g_build_filename(VENTURE_TEST_EXAMPLES, name, NULL);

		g_assert_true(g_file_get_contents(path, &source, NULL, &error));
		g_assert_no_error(error);

		if (!venture_automation_validate_dsl(source, &message))
		{
			g_error("%s does not parse: %s", name,
			        (NULL != message) ? message : "no reason given");
		}

		checked++;
	}

	/* A walk that found nothing passes silently, and would keep passing
	 * if the directory moved. */
	g_assert_cmpuint(checked, >, 0);
}

int
main(
	int	  argc,
	char	**argv
){
	g_test_init(&argc, &argv, NULL);

	g_test_add("/automation/handlers-run-on-the-calling-thread", Fixture, NULL,
	           fixture_set_up,
	           test_automation_handlers_run_on_the_calling_thread,
	           fixture_tear_down);
	g_test_add("/automation/does-not-cascade", Fixture, NULL,
	           fixture_set_up, test_automation_does_not_cascade,
	           fixture_tear_down);

	g_test_add_func("/automation/shipped-examples-parse",
	                test_automation_shipped_examples_parse);

	return g_test_run();
}
