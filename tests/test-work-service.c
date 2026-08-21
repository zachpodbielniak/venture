/*
 * test-work-service.c - The background thread's contract
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A coding run takes minutes and everything else in VENTURE takes
 * milliseconds on one main loop. This file pins the two properties that make
 * that safe: the loop keeps turning while a run is in flight, and the run
 * record survives a process that stopped in the middle.
 */

#include <venture.h>

#include <glib.h>

typedef struct
{
	VentureConfig	*config;
	VentureDatabase	*database;
	VentureContext	*context;
	gchar		*state_dir;
	gint64		 ticket_id;
	gint64		 repo_id;
} Fixture;

static void
fixture_set_up(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureForge) forge = NULL;
	g_autoptr(VentureForgeRepo) repo = NULL;
	g_autoptr(VentureForgeRule) rule = NULL;
	g_autoptr(VentureTicket) ticket = NULL;

	(void)user_data;

	fixture->state_dir = g_dir_make_tmp("venture-work-XXXXXX", &error);
	g_assert_no_error(error);

	fixture->config = venture_config_new();
	g_object_set(fixture->config, "state-dir", fixture->state_dir, NULL);
	g_object_set(fixture->config, "forge-runs-enabled", TRUE, NULL);

	fixture->database = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(fixture->database,
		venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);

	fixture->context = venture_context_new(fixture->config, fixture->database);

	forge = venture_forge_new();
	g_object_set(forge, "name", "Home", "base-url",
	             "https://git.example.com", "active", TRUE, NULL);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(forge), NULL, NULL));

	repo = venture_forge_repo_new();
	g_object_set(repo, "name", "zach/venture", "forge-id",
	             venture_entity_get_id(VENTURE_ENTITY(forge)),
	             "default-branch", "master", "active", TRUE, NULL);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(repo), NULL, NULL));
	fixture->repo_id = venture_entity_get_id(VENTURE_ENTITY(repo));

	rule = venture_forge_rule_new();
	g_object_set(rule, "name", "bugs", "repo-id", fixture->repo_id,
	             "issue-type", VENTURE_ISSUE_TYPE_BUG, "enabled", TRUE,
	             "runner", VENTURE_FORGE_RUNNER_AGENT, NULL);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(rule), NULL, NULL));

	ticket = venture_ticket_new();
	g_object_set(ticket, "title", "It crashes on save", "issue-type",
	             VENTURE_ISSUE_TYPE_BUG, "repo-id", fixture->repo_id, NULL);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(ticket), NULL, NULL));
	fixture->ticket_id = venture_entity_get_id(VENTURE_ENTITY(ticket));
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
	g_clear_pointer(&fixture->state_dir, g_free);
}

/*
 * Runs are off unless the operator turns them on.
 *
 * What breaks if this regresses: this is the one switch in the whole feature
 * that lets something spend money and push code without being watched. A
 * default of on would mean an install that merely upgraded started doing
 * that.
 */
static void
test_work_disabled_by_default(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureConfig) config = NULL;
	g_autoptr(VentureContext) context = NULL;
	g_autoptr(VentureWorkService) service = NULL;
	g_autoptr(GError) error = NULL;

	(void)user_data;

	config = venture_config_new();
	context = venture_context_new(config, fixture->database);

	service = venture_work_service_new(context, &error);

	g_assert_null(service);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
}

static gboolean
tick(gpointer user_data)
{
	guint *ticks = user_data;

	(*ticks)++;

	return G_SOURCE_CONTINUE;
}

/*
 * The main loop keeps turning while a run is in flight.
 *
 * What breaks if this regresses -- and it is the headline of this file:
 * htmx-glib's handlers cannot yield and podomation's dispatch blocks its
 * caller, so a run that happened on the main context would freeze the whole
 * server for its entire duration. Minutes, for a coding agent. The service
 * owns a thread for this reason alone, and this test is what stops somebody
 * "simplifying" it back onto the main context.
 *
 * The run itself fails almost immediately here -- there is no real
 * repository to clone -- which is fine and deliberate: what is being
 * measured is that the loop kept running, not what the run concluded.
 */
static void
test_work_does_not_block_the_main_loop(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureWorkService) service = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GMainLoop) loop = NULL;
	guint ticks = 0;
	guint source;
	gint64 run_id;
	gint64 deadline;

	(void)user_data;

	service = venture_work_service_new(fixture->context, &error);
	g_assert_no_error(error);
	g_assert_nonnull(service);

	run_id = venture_work_service_start_for_ticket(service,
	                                               fixture->ticket_id,
	                                               "tester", &error);
	g_assert_no_error(error);
	g_assert_cmpint(run_id, >, 0);

	/*
	 * The call returned before the run finished. Asserted directly rather
	 * than inferred from timing: a run executed inline would already have
	 * completed and been removed from the live set by the time
	 * start_for_ticket() returned, so a non-zero count here is proof the
	 * work went somewhere else. Measuring only the tick rate below would
	 * miss it, because a run that fails fast does not block long enough
	 * to notice.
	 */
	g_assert_cmpuint(venture_work_service_count_live(service), >, 0);

	source = g_timeout_add(10, tick, &ticks);

	deadline = g_get_monotonic_time() + (2 * G_USEC_PER_SEC);

	while ((g_get_monotonic_time() < deadline) && (ticks < 20))
		g_main_context_iteration(NULL, TRUE);

	g_source_remove(source);

	/* A blocked loop would have managed nothing at all. */
	g_assert_cmpuint(ticks, >=, 20);
}

/*
 * A queued run is written down before it starts.
 *
 * What breaks if this regresses: the record is the only thing that survives
 * a restart, and a run that existed only in memory would leave no trace of
 * having been started at all.
 */
static void
test_work_records_the_run(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureWorkService) service = NULL;
	g_autoptr(VentureEntity) run = NULL;
	g_autoptr(GError) error = NULL;
	gint64 run_id;
	gint64 ticket_id = 0;

	(void)user_data;

	service = venture_work_service_new(fixture->context, &error);
	g_assert_no_error(error);

	run_id = venture_work_service_start_for_ticket(service,
	                                               fixture->ticket_id,
	                                               "tester", &error);
	g_assert_no_error(error);

	run = venture_database_get(fixture->database, VENTURE_TYPE_FORGE_RUN,
	                           run_id, &error);
	g_assert_no_error(error);
	g_assert_nonnull(run);

	g_object_get(run, "ticket-id", &ticket_id, NULL);
	g_assert_cmpint(ticket_id, ==, fixture->ticket_id);
}

/*
 * A ticket with no rule, and a ticket with no repository, both refuse.
 *
 * What breaks if this regresses: a repository nobody enrolled starts getting
 * AI runs. "No rule" has to be a refusal rather than a default, which is the
 * same argument the resolver makes and worth asserting at this level too.
 */
static void
test_work_refuses_without_a_rule(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureWorkService) service = NULL;
	g_autoptr(VentureTicket) bare = NULL;
	g_autoptr(VentureTicket) unruled = NULL;
	g_autoptr(GError) error = NULL;

	(void)user_data;

	service = venture_work_service_new(fixture->context, &error);
	g_assert_no_error(error);

	bare = venture_ticket_new();
	g_object_set(bare, "title", "no repository", NULL);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(bare), NULL, NULL));

	g_assert_cmpint(venture_work_service_start_for_ticket(service,
		venture_entity_get_id(VENTURE_ENTITY(bare)), "tester", &error),
		==, 0);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
	g_clear_error(&error);

	/* A repository, but no rule covering research tickets. */
	unruled = venture_ticket_new();
	g_object_set(unruled, "title", "a question", "issue-type",
	             VENTURE_ISSUE_TYPE_RESEARCH, "repo-id", fixture->repo_id,
	             NULL);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(unruled), NULL, NULL));

	g_assert_cmpint(venture_work_service_start_for_ticket(service,
		venture_entity_get_id(VENTURE_ENTITY(unruled)), "tester", &error),
		==, 0);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
}

/*
 * A run left in flight by a previous process is reconciled, not resumed.
 *
 * What breaks if this regresses: a run whose outcome nobody observed may
 * have pushed a branch, opened a pull request, or done nothing. Resuming it
 * could repeat whichever of those it managed. Interrupted is a state of its
 * own precisely because it is an absence of knowledge rather than a failure,
 * and the operator decides what to do about it.
 */
static void
test_work_reconciles_interrupted_runs(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureForgeRun) stale = NULL;
	g_autoptr(VentureWorkService) service = NULL;
	g_autoptr(VentureEntity) reloaded = NULL;
	g_autoptr(GError) error = NULL;
	VentureForgeRunState state;
	gint64 run_id;

	(void)user_data;

	stale = venture_forge_run_new();
	g_object_set(stale, "ticket-id", fixture->ticket_id, "state",
	             VENTURE_FORGE_RUN_STATE_QUEUED, NULL);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(stale), NULL, NULL));
	run_id = venture_entity_get_id(VENTURE_ENTITY(stale));

	service = venture_work_service_new(fixture->context, &error);
	g_assert_no_error(error);

	reloaded = venture_database_get(fixture->database, VENTURE_TYPE_FORGE_RUN,
	                                run_id, &error);
	g_assert_no_error(error);

	g_object_get(reloaded, "state", &state, NULL);
	g_assert_cmpint(state, ==, VENTURE_FORGE_RUN_STATE_INTERRUPTED);
}

int
main(
	int	  argc,
	char	**argv
){
	g_test_init(&argc, &argv, NULL);

#define ADD(path, func) \
	g_test_add(path, Fixture, NULL, fixture_set_up, func, fixture_tear_down)

	ADD("/work/disabled-by-default", test_work_disabled_by_default);
	ADD("/work/does-not-block-the-main-loop",
	    test_work_does_not_block_the_main_loop);
	ADD("/work/records-the-run", test_work_records_the_run);
	ADD("/work/refuses-without-a-rule", test_work_refuses_without_a_rule);
	ADD("/work/reconciles-interrupted-runs",
	    test_work_reconciles_interrupted_runs);

#undef ADD

	return g_test_run();
}
