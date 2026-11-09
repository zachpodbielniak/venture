/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * test-parallel.c - Record reads answered on worker threads
 *
 * The read workers are fenced by a gate the main thread holds except
 * while it sleeps in poll(), and each worker judges as its own reader
 * through its own read-only connection. These tests hold the three things
 * that make that safe and the one that makes it worth having:
 *
 *   - a job never runs beside code on the main thread, nested loops
 *     included (a wrong gate is a crash under load and nowhere else);
 *   - a worker's answer is byte for byte the main thread's;
 *   - readers on different threads never see each other's records;
 *   - jobs really do run side by side.
 */
#include <venture.h>
#include <libsoup/soup.h>
#include <string.h>

#include "venture-test-util.h"

#define RENDEZVOUS (3)

/* --- The gate ------------------------------------------------------------ */

typedef struct
{
	GMutex	 lock;
	GCond	 cond;
	gint	 started;
	gint	 inside;
	gint	 max_inside;
	gint	 done;
	gboolean main_busy;	/* the main thread is running code */
	gboolean overlapped;	/* a job ran while it was */
	gboolean wait_for_company;
} GateProbe;

static void
probe_job(gpointer data)
{
	GateProbe *probe = data;
	gint64 deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;

	g_mutex_lock(&probe->lock);
	probe->started++;
	probe->inside++;
	probe->max_inside = MAX(probe->max_inside, probe->inside);
	if (probe->main_busy)
		probe->overlapped = TRUE;
	g_cond_broadcast(&probe->cond);
	/* Wait for company: run one at a time and this times out alone. */
	while (probe->wait_for_company && probe->max_inside < RENDEZVOUS)
		if (!g_cond_wait_until(&probe->cond, &probe->lock, deadline))
			break;
	probe->inside--;
	g_mutex_unlock(&probe->lock);
}

static void
probe_done(gpointer data)
{
	GateProbe *probe = data;

	g_mutex_lock(&probe->lock);
	probe->done++;
	g_mutex_unlock(&probe->lock);
}

static gint
probe_get(GateProbe *probe, gint *field)
{
	gint value;

	g_mutex_lock(&probe->lock);
	value = *field;
	g_mutex_unlock(&probe->lock);
	return value;
}

static void
probe_iterate_until_done(GateProbe *probe, gint jobs)
{
	gint64 deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;

	while (probe_get(probe, &probe->done) < jobs && g_get_monotonic_time() < deadline)
		g_main_context_iteration(NULL, TRUE);
	g_assert_cmpint(probe_get(probe, &probe->done), ==, jobs);
}

/*
 * Breaks if a job can run while the main thread is running code: the
 * main thread works for a while without iterating its loop, and no job
 * queued meanwhile may start until it goes idle.
 */
static void
test_gate_waits_for_idle_main(void)
{
	GateProbe probe;
	gint64 until;

	memset(&probe, 0, sizeof probe);
	g_mutex_init(&probe.lock);
	g_cond_init(&probe.cond);
	venture_parallel_install(2);

	g_mutex_lock(&probe.lock);
	probe.main_busy = TRUE;
	g_mutex_unlock(&probe.lock);
	venture_parallel_submit(probe_job, NULL, probe_done, &probe);
	venture_parallel_submit(probe_job, NULL, probe_done, &probe);
	until = g_get_monotonic_time() + 200 * G_TIME_SPAN_MILLISECOND;
	while (g_get_monotonic_time() < until)
		g_thread_yield();
	g_assert_cmpint(probe_get(&probe, &probe.started), ==, 0);
	g_mutex_lock(&probe.lock);
	probe.main_busy = FALSE;
	g_mutex_unlock(&probe.lock);

	probe_iterate_until_done(&probe, 2);
	g_assert_false(probe.overlapped);

	venture_parallel_uninstall();
	g_mutex_clear(&probe.lock);
	g_cond_clear(&probe.cond);
}

/* Breaks if jobs are serialised: none would ever see RENDEZVOUS of them
 * inside the gate at once. */
static void
test_gate_runs_jobs_together(void)
{
	GateProbe probe;
	guint i;

	memset(&probe, 0, sizeof probe);
	g_mutex_init(&probe.lock);
	g_cond_init(&probe.cond);
	probe.wait_for_company = TRUE;
	venture_parallel_install(RENDEZVOUS);

	for (i = 0; i < RENDEZVOUS; i++)
		venture_parallel_submit(probe_job, NULL, probe_done, &probe);
	probe_iterate_until_done(&probe, RENDEZVOUS);
	g_assert_cmpint(probe.max_inside, ==, RENDEZVOUS);
	g_assert_cmpuint(venture_parallel_get_max_concurrent(), >=, RENDEZVOUS);

	venture_parallel_uninstall();
	g_mutex_clear(&probe.lock);
	g_cond_clear(&probe.cond);
}

typedef struct
{
	GateProbe	*probe;
	gboolean	 finished;
} NestedRun;

static gboolean
nested_stop(gpointer data)
{
	g_main_loop_quit(data);
	return G_SOURCE_REMOVE;
}

/* Runs a nested main loop for a while, as a handler waiting on a webhook
 * test or a feeds push does, with a job queued. */
static gboolean
nested_handler(gpointer data)
{
	NestedRun *run = data;
	g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);

	g_mutex_lock(&run->probe->lock);
	run->probe->main_busy = TRUE;
	g_mutex_unlock(&run->probe->lock);
	venture_parallel_submit(probe_job, NULL, probe_done, run->probe);
	g_timeout_add(200, nested_stop, loop);
	g_main_loop_run(loop);
	g_mutex_lock(&run->probe->lock);
	run->probe->main_busy = FALSE;
	g_mutex_unlock(&run->probe->lock);
	run->finished = TRUE;
	return G_SOURCE_REMOVE;
}

/* Breaks if a nested loop opens the gate: the handler running it is half
 * way through its work, and a job would run beside it. */
static void
test_gate_stays_shut_in_nested_loop(void)
{
	GateProbe probe;
	NestedRun run;
	gint64 deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;

	memset(&probe, 0, sizeof probe);
	g_mutex_init(&probe.lock);
	g_cond_init(&probe.cond);
	run.probe = &probe;
	run.finished = FALSE;
	venture_parallel_install(1);

	g_idle_add(nested_handler, &run);
	while (!run.finished && g_get_monotonic_time() < deadline)
		g_main_context_iteration(NULL, TRUE);
	g_assert_true(run.finished);
	probe_iterate_until_done(&probe, 1);
	g_assert_false(probe.overlapped);

	venture_parallel_uninstall();
	g_mutex_clear(&probe.lock);
	g_cond_clear(&probe.cond);
}

/* --- Reads through a real server ----------------------------------------- */

typedef struct
{
	gchar			*directory;
	VentureConfig		*config;
	VentureDatabase		*database;
	VentureContext		*context;
	VentureWebServer	*server;
	gint64			 organizations[2];
	gchar			*owner_secret;
	gchar			*member_secret[2];	/* editor of one business each */
	gchar			*viewer_secret;		/* viewer: judged row by row */
	gint64			 viewer_user;
	gint64			 some_company;
} Fixture;

static gchar *
mint_token(VentureDatabase *db, gint64 user_id, VentureUserRole role)
{
	g_autoptr(VentureApiToken) token = venture_api_token_new();
	gchar *secret;

	g_object_set(token, "name", "parallel", "user-id", user_id, "role", role, NULL);
	secret = venture_api_token_generate(token);
	g_assert_true(venture_database_save(db, VENTURE_ENTITY(token), NULL, NULL));
	return secret;
}

static gint64
make_user(VentureDatabase *db, const gchar *name, VentureUserRole role)
{
	g_autoptr(VentureEntity) user = g_object_new(VENTURE_TYPE_USER, "username", name, "active", TRUE,
		"role", role, NULL);
	g_assert_true(venture_database_save(db, user, NULL, NULL));
	return venture_entity_get_id(user);
}

static void
make_member(VentureDatabase *db, gint64 user, gint64 organization, gint role)
{
	g_autoptr(VentureEntity) member = g_object_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP, "user-id", user,
		"organization-id", organization, "active", TRUE, "role", role, NULL);
	g_assert_true(venture_database_save(db, member, NULL, NULL));
}

static void
fixture_build(Fixture *f, gint64 workers)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *uri = NULL;
	gint64 owner, editors[2];
	guint i, j;

	f->directory = g_dir_make_tmp("venture-parallel-XXXXXX", NULL);
	uri = g_strdup_printf("sqlite://%s/venture.db", f->directory);
	f->config = venture_config_new();
	g_object_set(f->config, "state-dir", f->directory, "server-bind-address", "127.0.0.1",
		"server-port", (gint64)0, "server-read-workers", workers, NULL);
	f->database = venture_database_new(uri, &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->database, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->database);

	f->organizations[0] = venture_context_get_default_organization_id(f->context);
	{
		g_autoptr(VentureEntity) other = g_object_new(VENTURE_TYPE_ORGANIZATION, "name", "Second business",
			"slug", "second", NULL);
		g_assert_true(venture_database_save(f->database, other, NULL, NULL));
		f->organizations[1] = venture_entity_get_id(other);
	}

	owner = make_user(f->database, "owner", VENTURE_USER_ROLE_OWNER);
	f->owner_secret = mint_token(f->database, owner, VENTURE_USER_ROLE_OWNER);
	for (i = 0; i < 2; i++)
	{
		g_autofree gchar *name = g_strdup_printf("editor-%u", i);
		editors[i] = make_user(f->database, name, VENTURE_USER_ROLE_EDITOR);
		make_member(f->database, editors[i], f->organizations[i], VENTURE_ORGANIZATION_ROLE_EDITOR);
		f->member_secret[i] = mint_token(f->database, editors[i], VENTURE_USER_ROLE_EDITOR);
	}
	f->viewer_user = make_user(f->database, "viewer", VENTURE_USER_ROLE_VIEWER);
	make_member(f->database, f->viewer_user, f->organizations[0], VENTURE_ORGANIZATION_ROLE_VIEWER);
	f->viewer_secret = mint_token(f->database, f->viewer_user, VENTURE_USER_ROLE_VIEWER);

	/* Each business's customers are named after it, so a response can be
	 * checked for a row from the wrong one by its text alone. */
	for (i = 0; i < 2; i++)
		for (j = 0; j < 30; j++)
		{
			g_autofree gchar *name = g_strdup_printf("Customer of business %u number %u", i, j);
			g_autoptr(VentureEntity) company = g_object_new(VENTURE_TYPE_COMPANY, "name", name,
				"organization-id", f->organizations[i],
				"owner-user-id", (0 == i && 0 == j % 3) ? f->viewer_user : (gint64)0, NULL);
			g_assert_true(venture_database_save(f->database, company, NULL, NULL));
			f->some_company = venture_entity_get_id(company);
		}

	f->server = venture_web_server_new(f->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(f->server, &error));
	g_assert_no_error(error);
}

static void
fixture_free(Fixture *f)
{
	guint i;

	venture_web_server_stop(f->server);
	g_clear_object(&f->server);
	g_clear_object(&f->context);
	g_clear_object(&f->database);
	g_clear_object(&f->config);
	g_free(f->owner_secret);
	for (i = 0; i < 2; i++)
		g_free(f->member_secret[i]);
	g_free(f->viewer_secret);
	venture_test_remove_tree(f->directory);
	g_free(f->directory);
}

typedef struct
{
	SoupMessage	*message;
	GBytes		*body;
	GError		*error;
	gboolean	 done;
	guint		*outstanding;
} Call;

static void
call_done(GObject *source, GAsyncResult *result, gpointer data)
{
	Call *call = data;

	call->body = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &call->error);
	call->done = TRUE;
	if (NULL != call->outstanding)
		(*call->outstanding)--;
}

static Call *
call_start(SoupSession *session, Fixture *f, const gchar *method, const gchar *path,
	const gchar *secret, const gchar *json, guint *outstanding)
{
	Call *call = g_new0(Call, 1);
	g_autofree gchar *url = g_strconcat(venture_web_server_get_base_url(f->server), path, NULL);
	g_autofree gchar *bearer = g_strconcat("Bearer ", secret, NULL);

	call->message = soup_message_new(method, url);
	call->outstanding = outstanding;
	soup_message_headers_replace(soup_message_get_request_headers(call->message), "Authorization", bearer);
	if (NULL != json)
	{
		g_autoptr(GBytes) bytes = g_bytes_new(json, strlen(json));
		soup_message_set_request_body_from_bytes(call->message, "application/json", bytes);
	}
	if (NULL != outstanding)
		(*outstanding)++;
	soup_session_send_and_read_async(session, call->message, G_PRIORITY_DEFAULT, NULL, call_done, call);
	return call;
}

static void
call_free(Call *call)
{
	g_clear_object(&call->message);
	g_clear_pointer(&call->body, g_bytes_unref);
	g_clear_error(&call->error);
	g_free(call);
}

static void
drain(guint *outstanding)
{
	gint64 deadline = g_get_monotonic_time() + 60 * G_TIME_SPAN_SECOND;

	while (*outstanding > 0 && g_get_monotonic_time() < deadline)
		g_main_context_iteration(NULL, TRUE);
	g_assert_cmpuint(*outstanding, ==, 0);
}

/* One GET, waited for; returns "STATUS BODY". */
static gchar *
fetch(Fixture *f, const gchar *path, const gchar *secret)
{
	g_autoptr(SoupSession) session = soup_session_new_with_options("timeout", 30, NULL);
	guint outstanding = 0;
	Call *call = call_start(session, f, "GET", path, secret, NULL, &outstanding);
	gchar *answer;
	gsize size = 0;
	const gchar *data;

	drain(&outstanding);
	g_assert_no_error(call->error);
	data = g_bytes_get_data(call->body, &size);
	answer = g_strdup_printf("%u %.*s", soup_message_get_status(call->message), (gint)size, data);
	call_free(call);
	return answer;
}

/*
 * Breaks if a worker answers differently from the main thread: the same
 * requests, by the same readers, against the same data, once with the
 * workers and once without, must be the same bytes. The viewer is judged
 * row by row (its role reads only what it owns), the owner by the SQL
 * shortcut; a single record, a missing one and a page past the end too.
 */
static void
test_worker_answers_match_main_thread(void)
{
	Fixture with;
	Fixture without;
	g_autofree gchar *record = NULL;
	const gchar *paths[] = {
		"/api/v1/company?limit=7&offset=3",
		"/api/v1/company?limit=50",
		"/api/v1/company?limit=5&offset=1000",
		"/api/v1/company/999999",
		NULL, /* the record, filled below */
	};
	guint p, r;

	memset(&with, 0, sizeof with);
	memset(&without, 0, sizeof without);
	fixture_build(&with, 4);
	g_assert_true(venture_database_has_parallel_reads(with.database));
	record = g_strdup_printf("/api/v1/company/%" G_GINT64_FORMAT, with.some_company);
	paths[4] = record;
	{
		guint64 before = venture_parallel_get_jobs_run();
		g_autofree gchar *probe = fetch(&with, paths[0], with.owner_secret);
		g_assert_cmpuint(venture_parallel_get_jobs_run(), >, before);
	}

	fixture_build(&without, 0);
	g_assert_false(venture_database_has_parallel_reads(without.database));

	for (p = 0; p < G_N_ELEMENTS(paths); p++)
	{
		const gchar *secrets_with[] = { with.owner_secret, with.viewer_secret, with.member_secret[1] };
		const gchar *secrets_without[] = { without.owner_secret, without.viewer_secret, without.member_secret[1] };

		for (r = 0; r < G_N_ELEMENTS(secrets_with); r++)
		{
			g_autofree gchar *a = fetch(&with, paths[p], secrets_with[r]);
			g_autofree gchar *b = fetch(&without, paths[p], secrets_without[r]);
			g_autofree gchar *a_masked = NULL;
			g_autofree gchar *b_masked = NULL;
			g_autoptr(GRegex) stamps = g_regex_new("\"(uuid|created_at|updated_at)\" : \"[^\"]*\"", 0, 0, NULL);

			/* Two databases: the identities and stamps differ, nothing else. */
			a_masked = g_regex_replace_literal(stamps, a, -1, 0, "\"-\" : \"-\"", 0, NULL);
			b_masked = g_regex_replace_literal(stamps, b, -1, 0, "\"-\" : \"-\"", 0, NULL);
			g_assert_cmpstr(a_masked, ==, b_masked);
		}
	}

	fixture_free(&without);
	fixture_free(&with);
}

/*
 * Breaks if one worker's reader leaks into another's. Two editors of two
 * businesses list customers concurrently, many times over, each asking
 * both for their own business and for the other's. Their own must hold
 * exactly their thirty rows; the other's must be empty -- the access
 * policy refuses every row -- unless a thread judged as somebody else,
 * which is what a shared actor or list cache between threads would do.
 */
static void
test_readers_never_cross(void)
{
	Fixture f;
	g_autoptr(SoupSession) session = soup_session_new_with_options("timeout", 60, "max-conns-per-host", 32, NULL);
	g_autoptr(GPtrArray) calls = g_ptr_array_new_with_free_func((GDestroyNotify)call_free);
	guint outstanding = 0;
	guint i;

	memset(&f, 0, sizeof f);
	fixture_build(&f, 4);
	for (i = 0; i < 200; i++)
	{
		guint reader = i % 2;
		guint asked = (0 == (i / 2) % 2) ? reader : 1 - reader;
		g_autofree gchar *path = g_strdup_printf("/api/v1/company?limit=40&organization_id=%" G_GINT64_FORMAT,
			f.organizations[asked]);

		g_ptr_array_add(calls, call_start(session, &f, "GET", path, f.member_secret[reader], NULL, &outstanding));
	}
	drain(&outstanding);

	for (i = 0; i < calls->len; i++)
	{
		Call *call = g_ptr_array_index(calls, i);
		guint reader = i % 2;
		gboolean own = (0 == (i / 2) % 2);
		gsize size = 0;
		const gchar *data;
		g_autofree gchar *text = NULL;
		g_autofree gchar *foreign = g_strdup_printf("business %u ", 1 - reader);

		g_assert_no_error(call->error);
		data = g_bytes_get_data(call->body, &size);
		text = g_strndup(data, size);
		if (200 != soup_message_get_status(call->message) ||
		    NULL != strstr(text, foreign) ||
		    NULL == strstr(text, own ? "\"total\" : 30" : "\"total\" : 0"))
			g_error("request %u (editor of business %u, %s): %u %.300s", i, reader,
			        own ? "own business" : "the other business",
			        soup_message_get_status(call->message), text);
	}
	fixture_free(&f);
}

/*
 * Breaks if reads and writes interfere: writes (on the main thread) and
 * list reads (on workers) are sent together, every one must succeed, and
 * afterwards the total must count every write.
 */
static void
test_reads_beside_writes(void)
{
	Fixture f;
	g_autoptr(SoupSession) session = soup_session_new_with_options("timeout", 60, "max-conns-per-host", 32, NULL);
	g_autoptr(GPtrArray) calls = g_ptr_array_new_with_free_func((GDestroyNotify)call_free);
	g_autofree gchar *after = NULL;
	guint outstanding = 0;
	guint i;

	memset(&f, 0, sizeof f);
	fixture_build(&f, 4);
	for (i = 0; i < 120; i++)
	{
		if (0 == i % 3)
			g_ptr_array_add(calls, call_start(session, &f, "POST", "/api/v1/company", f.owner_secret,
				"{\"name\":\"Written during reads\"}", &outstanding));
		else
			g_ptr_array_add(calls, call_start(session, &f, "GET", "/api/v1/company?limit=25",
				f.owner_secret, NULL, &outstanding));
	}
	drain(&outstanding);
	for (i = 0; i < calls->len; i++)
	{
		Call *call = g_ptr_array_index(calls, i);
		g_assert_no_error(call->error);
		g_assert_cmpuint(soup_message_get_status(call->message), ==, (0 == i % 3) ? 201 : 200);
	}

	after = fetch(&f, "/api/v1/company?limit=1&search=Written", f.owner_secret);
	g_assert_nonnull(strstr(after, "\"total\" : 40"));
	fixture_free(&f);
}

static gpointer
lease_and_try_to_write(gpointer data)
{
	VentureDatabase *db = data;
	g_autoptr(GError) error = NULL;
	g_autoptr(OrmResult) result = NULL;

	g_assert_true(venture_database_reader_begin(db, &error));
	g_assert_no_error(error);
	/* SQLite steps a statement lazily, so a refusal may surface at the
	 * first fetch rather than at the query; either way the rows stay
	 * (the caller counts them afterwards). */
	result = venture_database_query_raw(db, "DELETE FROM companies", NULL, &error);
	if (NULL != result)
	{
		while (orm_result_next(result))
			;
		g_assert_nonnull(orm_result_get_error(result));
	}
	else
		g_assert_nonnull(error);
	g_clear_pointer(&result, g_object_unref);
	venture_database_reader_end(db);
	return NULL;
}

/* Breaks if a leased connection can write: the read workers' only line
 * of defence against a write slipping into their path would then be a
 * g_critical in the code, not the database itself. */
static void
test_leased_connection_is_read_only(void)
{
	Fixture f;
	GThread *thread;
	g_autofree gchar *after = NULL;

	memset(&f, 0, sizeof f);
	fixture_build(&f, 2);
	thread = g_thread_new("lease", lease_and_try_to_write, f.database);
	g_thread_join(thread);
	after = fetch(&f, "/api/v1/company?limit=1", f.owner_secret);
	/* The default business's thirty, all still there. */
	g_assert_nonnull(strstr(after, "\"total\" : 30"));
	fixture_free(&f);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/parallel/gate-waits-for-idle-main", test_gate_waits_for_idle_main);
	g_test_add_func("/parallel/gate-runs-jobs-together", test_gate_runs_jobs_together);
	g_test_add_func("/parallel/gate-stays-shut-in-nested-loop", test_gate_stays_shut_in_nested_loop);
	g_test_add_func("/parallel/worker-answers-match-main-thread", test_worker_answers_match_main_thread);
	g_test_add_func("/parallel/readers-never-cross", test_readers_never_cross);
	g_test_add_func("/parallel/reads-beside-writes", test_reads_beside_writes);
	g_test_add_func("/parallel/leased-connection-is-read-only", test_leased_connection_is_read_only);
	return g_test_run();
}
