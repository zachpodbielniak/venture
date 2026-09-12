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

#include "venture-test-util.h"

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

	/*
	 * The state directory was freed and never removed, so each run left
	 * one /tmp/venture-work-* per test holding whatever the runner had
	 * written into it.
	 */
	venture_test_remove_tree(fixture->state_dir);
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

/*
 * Waits for every run this service started to stop being live.
 *
 * A run happens on the service's own thread, and the thread is joined by
 * the finalizer -- but the finalizer runs when the last reference goes,
 * which a test that never turns the main loop does not reach until it
 * returns. So the worker was still calling g_mkdir_with_parents() for the
 * run's workspace while fixture_tear_down() was removing the directory
 * above it: strace shows the rmdir and the mkdir interleaved, and which
 * one won decided whether a /tmp/venture-work-* directory survived the
 * run.
 *
 * Draining first is what makes the teardown deterministic. It is also the
 * honest shape for a test that starts background work: asserting on a
 * record and walking away leaves a thread writing into a directory
 * nobody owns any more.
 *
 * Bounded rather than open-ended, because a test that can hang is worse
 * than one that fails -- the run here fails almost at once (there is no
 * repository to clone), so this normally returns in milliseconds.
 */
static void
settle_runs(
	VentureWorkService	*service
){
	gint64 deadline;

	deadline = g_get_monotonic_time() + (5 * G_USEC_PER_SEC);

	while ((venture_work_service_count_live(service) > 0) &&
	       (g_get_monotonic_time() < deadline))
		g_main_context_iteration(NULL, FALSE);

	g_assert_cmpuint(venture_work_service_count_live(service), ==, 0);
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

	/*
	 * This one usually drains on its own -- two seconds of iterating the
	 * loop is far longer than the run lasts -- but "usually" is not a
	 * thing to leave a background thread's lifetime resting on.
	 */
	settle_runs(service);
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

	settle_runs(service);
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

/*
 * A harness session is opened, records what it is, and refuses to be
 * pointed anywhere it was not allowed.
 *
 * The allow-list is the security-critical half of this feature: a session
 * with a host path is a coding agent with write access to a tree on this
 * machine, so the default is that there is no such path and every
 * configured root is compared after resolution -- a symlink or a `..`
 * must not walk out of one.
 */
static void
test_work_session_workspace_is_gated(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureWorkService) service = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *allowed = NULL;
	g_autofree gchar *inside = NULL;
	g_autofree gchar *sibling = NULL;
	g_autofree gchar *escape = NULL;
	VentureAgentSessionSpec spec;
	gint64 session_id;

	(void)user_data;

	service = venture_work_service_new(fixture->context, &error);
	g_assert_no_error(error);
	g_assert_nonnull(service);

	allowed = g_build_filename(fixture->state_dir, "allowed", NULL);
	inside = g_build_filename(allowed, "checkout", NULL);
	sibling = g_build_filename(fixture->state_dir, "allowed-secrets", NULL);
	g_assert_cmpint(g_mkdir_with_parents(inside, 0755), ==, 0);
	g_assert_cmpint(g_mkdir_with_parents(sibling, 0755), ==, 0);

	memset(&spec, 0, sizeof(spec));
	spec.name = "Fix the checkout";
	spec.provider = "claude-code";
	spec.workspace = inside;

	/* Nothing configured: a host path is refused outright, and the
	 * message says what to set rather than only that it failed. */
	session_id = venture_work_service_session_open(service, &spec, &error);
	g_assert_cmpint(session_id, ==, 0);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_assert_nonnull(strstr(error->message, "workspace_roots"));
	g_clear_error(&error);

	{
		const gchar *roots[] = { NULL, NULL };

		roots[0] = allowed;
		g_object_set(fixture->config, "forge-workspace-roots", roots, NULL);
	}

	/* Under the root: allowed, and the path is stored resolved. */
	session_id = venture_work_service_session_open(service, &spec, &error);
	g_assert_no_error(error);
	g_assert_cmpint(session_id, >, 0);

	{
		g_autoptr(VentureEntity) session = NULL;
		g_autofree gchar *workspace = NULL;
		g_autofree gchar *provider = NULL;
		gboolean cloned = TRUE;

		session = venture_database_get(fixture->database,
			VENTURE_TYPE_AGENT_SESSION, session_id, NULL);
		g_assert_nonnull(session);
		g_object_get(session, "workspace", &workspace, "provider", &provider,
		             "cloned", &cloned, NULL);
		g_assert_cmpstr(provider, ==, "claude-code");
		g_assert_nonnull(strstr(workspace, "checkout"));
		g_assert_false(cloned);
	}

	/*
	 * A sibling whose name starts with the root's is not under it. Without
	 * the separator check "/srv/venture-secrets" passes as "/srv/venture".
	 */
	spec.workspace = sibling;
	g_assert_cmpint(venture_work_service_session_open(service, &spec, &error),
	                ==, 0);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_clear_error(&error);

	/* And a path that climbs out of the root is refused after it is
	 * resolved, not before. */
	escape = g_build_filename(allowed, "..", "allowed-secrets", NULL);
	spec.workspace = escape;
	g_assert_cmpint(venture_work_service_session_open(service, &spec, &error),
	                ==, 0);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_clear_error(&error);

	/* A relative path is not a workspace at all. */
	spec.workspace = "checkout";
	g_assert_cmpint(venture_work_service_session_open(service, &spec, &error),
	                ==, 0);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);

	/* A session with neither a path nor a repository is legal and has no
	 * working directory: it can plan, not edit. */
	spec.workspace = NULL;
	spec.name = "Just thinking";
	session_id = venture_work_service_session_open(service, &spec, &error);
	g_assert_no_error(error);
	g_assert_cmpint(session_id, >, 0);

	/* A provider is not optional: a session without one has nothing to
	 * run and would fail on its first turn instead of here. */
	spec.provider = "";
	g_assert_cmpint(venture_work_service_session_open(service, &spec, &error),
	                ==, 0);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);
}

/*
 * A turn is stored before the agent sees it, and a closed session takes
 * no more.
 */
static void
test_work_session_turns_are_recorded(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureWorkService) service = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) turns = NULL;
	g_autoptr(GError) error = NULL;
	VentureAgentSessionSpec spec;
	gint64 session_id;

	(void)user_data;

	service = venture_work_service_new(fixture->context, &error);
	g_assert_no_error(error);

	memset(&spec, 0, sizeof(spec));
	spec.name = "Planning";
	spec.provider = "claude-code";
	session_id = venture_work_service_session_open(service, &spec, &error);
	g_assert_no_error(error);
	g_assert_cmpint(session_id, >, 0);

	/* An empty prompt is not a turn. */
	g_assert_false(venture_work_service_session_send(service, session_id, "",
	                                                 &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);

	g_assert_true(venture_work_service_session_send(service, session_id,
		"list the files", &error));
	g_assert_no_error(error);

	/* Stored immediately, so a turn that never comes back still says
	 * what was asked. */
	query = venture_query_new(VENTURE_TYPE_AGENT_TURN);
	g_assert_true(venture_query_add_filter_int(query, "session-id",
		VENTURE_FILTER_OP_EQ, session_id, NULL));
	turns = venture_database_find(fixture->database, query, NULL);
	g_assert_nonnull(turns);
	g_assert_cmpuint(turns->len, ==, 1);

	{
		g_autofree gchar *body = NULL;
		VentureChatRole role;

		g_object_get(g_ptr_array_index(turns, 0), "role", &role,
		             "body", &body, NULL);
		g_assert_cmpint(role, ==, VENTURE_CHAT_ROLE_USER);
		g_assert_cmpstr(body, ==, "list the files");
	}

	/* A second turn while one is in flight is refused rather than
	 * interleaved. */
	g_assert_false(venture_work_service_session_send(service, session_id,
		"and again", &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
	g_clear_error(&error);

	venture_work_service_session_close(service, session_id);

	{
		g_autoptr(VentureEntity) session = NULL;
		VentureAgentSessionState state;

		session = venture_database_get(fixture->database,
			VENTURE_TYPE_AGENT_SESSION, session_id, NULL);
		g_object_get(session, "state", &state, NULL);
		g_assert_cmpint(state, ==, VENTURE_AGENT_SESSION_STATE_CLOSED);
	}

	g_assert_false(venture_work_service_session_send(service, session_id,
		"one more", &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
}

int
main(
	int	  argc,
	char	**argv
){
	g_test_init(&argc, &argv, NULL);

#define ADD(path, func) \
	g_test_add(path, Fixture, NULL, fixture_set_up, func, fixture_tear_down)

	ADD("/work/session-workspace-is-gated",
	    test_work_session_workspace_is_gated);
	ADD("/work/session-turns-are-recorded",
	    test_work_session_turns_are_recorded);

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
