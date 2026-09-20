/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <string.h>
#include "venture-test-util.h"

typedef struct {
	gboolean done;
	GError *error;
	GBytes *bytes;
	gchar *out, *err;
} Result;
typedef struct {
	VentureConfig *config;
	VentureDatabase *db;
	VentureContext *context;
	VentureWebServer *server;
	gchar *directory;
	gint64 org;
} Fixture;
static void setup(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GSocketListener) listener = g_socket_listener_new();
	guint16 port = g_socket_listener_add_any_inet_port(listener, NULL, &error);
	(void)unused;
	g_assert_no_error(error);
	g_socket_listener_close(listener);
	f->directory = g_dir_make_tmp("venture-calendar-surfaces-XXXXXX", &error);
	g_assert_no_error(error);
	f->config = venture_config_new();
	g_object_set(f->config, "state-dir", f->directory, "server-bind-address", "127.0.0.1", "server-port", (gint64)port, "security-require-auth", FALSE, NULL);
	f->db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->db);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->org = venture_context_get_default_organization_id(f->context);
	f->server = venture_web_server_new(f->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(f->server, &error));
	g_assert_no_error(error);
}
static void teardown(Fixture *f, gconstpointer unused)
{
	(void)unused;
	venture_web_server_stop(f->server);
	g_clear_object(&f->server); g_clear_object(&f->context); g_clear_object(&f->db);
	g_clear_object(&f->config);
	venture_test_remove_tree(f->directory); g_free(f->directory);
}
static void http_done(GObject *source, GAsyncResult *result, gpointer data)
{
	Result *r = data;
	r->bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &r->error);
	r->done = TRUE;
}
static guint request_as(Fixture *f, const gchar *method, const gchar *path, const gchar *content_type, const gchar *body, gchar **out)
{
	g_autoptr(SoupSession) session = soup_session_new_with_options("timeout", 15, NULL);
	g_autofree gchar *url = g_strconcat(venture_web_server_get_base_url(f->server), path, NULL);
	g_autoptr(SoupMessage) message = soup_message_new(method, url);
	Result result;
	memset(&result, 0, sizeof(result));
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	if (body) {
		g_autoptr(GBytes) bytes = g_bytes_new(body, strlen(body));
		soup_message_set_request_body_from_bytes(message, content_type, bytes);
	}
	soup_session_send_and_read_async(session, message, G_PRIORITY_DEFAULT, NULL, http_done, &result);
	while (!result.done) g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(result.error);
	if (out) *out = g_strndup(g_bytes_get_data(result.bytes, NULL), g_bytes_get_size(result.bytes));
	g_bytes_unref(result.bytes);
	return soup_message_get_status(message);
}
static guint request(Fixture *f, const gchar *method, const gchar *path, const gchar *body, gchar **out)
{
	return request_as(f, method, path, "application/json", body, out);
}
static void cli_done(GObject *source, GAsyncResult *result, gpointer data)
{
	Result *r = data;
	g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, &r->out, &r->err, &r->error);
	r->done = TRUE;
}
static gboolean cli_timeout(gpointer process) { g_subprocess_force_exit(process); return G_SOURCE_CONTINUE; }
static gchar *cli(Fixture *f, const gchar *const *args, gboolean expect_success)
{
	g_autoptr(GSubprocessLauncher) launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE);
	g_autoptr(GSubprocess) process = NULL;
	g_autoptr(GPtrArray) argv = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GError) error = NULL;
	Result result;
	guint i, timeout;
	memset(&result, 0, sizeof(result));
	g_ptr_array_add(argv, g_canonicalize_filename("build/debug/venturectl", NULL));
	g_ptr_array_add(argv, g_strdup("--server")); g_ptr_array_add(argv, g_strdup(venture_web_server_get_base_url(f->server)));
	g_ptr_array_add(argv, g_strdup("-f")); g_ptr_array_add(argv, g_strdup("json"));
	for (i = 0; args[i]; i++) g_ptr_array_add(argv, g_strdup(args[i]));
	g_ptr_array_add(argv, NULL);
	g_subprocess_launcher_setenv(launcher, "VENTURE_TOKEN", "calendar-fixture", TRUE);
	process = g_subprocess_launcher_spawnv(launcher, (const gchar *const *)argv->pdata, &error);
	g_assert_no_error(error);
	timeout = g_timeout_add_seconds(30, cli_timeout, process);
	g_subprocess_communicate_utf8_async(process, NULL, NULL, cli_done, &result);
	while (!result.done) g_main_context_iteration(NULL, TRUE);
	g_source_remove(timeout);
	g_assert_no_error(result.error);
	if (g_subprocess_get_successful(process) != expect_success) g_test_message("CLI: %s / %s", result.out, result.err);
	g_assert_true(g_subprocess_get_successful(process) == expect_success);
	if (!expect_success) { g_free(result.out); return g_steal_pointer(&result.err); }
	g_free(result.err);
	return result.out;
}
static void save(Fixture *f, VentureEntity *e)
{
	g_autoptr(GError) error = NULL;
	g_assert_true(venture_database_save(f->db, e, NULL, &error));
	g_assert_no_error(error);
}

/* Rule 5: the CLI verb reaches the sweep; a broken account is reported, never contacted. */
static void test_cli_sync(Fixture *f, gconstpointer unused)
{
	const gchar *sync[] = { "calendar", "sync", NULL };
	const gchar *staged[] = { "--stage", "calendar", "sync", NULL };
	const gchar *bare[] = { "calendar", NULL };
	g_autofree gchar *empty = cli(f, sync, TRUE), *broken = NULL, *refused = NULL, *usage = NULL;
	g_autoptr(JsonNode) node = json_from_string(empty, NULL);
	g_autoptr(VentureEntity) account = NULL;
	(void)unused;
	g_assert_nonnull(node);
	g_assert_cmpint(venture_json_object_get_int(json_node_get_object(node), "accounts", -1), ==, 0);
	account = g_object_new(VENTURE_TYPE_CALENDAR_ACCOUNT, "organization-id", f->org, "url", "https://dav.venture.test/", "owner", "ben",
		"username", "ben", "secret-env", "VENTURE_CALDAV_SURFACE_MISSING", "calendar-path", "/calendars/ben/", "active", TRUE, NULL);
	save(f, account);
	broken = cli(f, sync, TRUE);
	g_assert_nonnull(strstr(broken, "operator allowlist"));
	g_assert_null(strstr(broken, "VENTURE_CALDAV_SURFACE_MISSING"));
	g_assert_nonnull(strstr(broken, "\"accounts\""));
	refused = cli(f, staged, FALSE);
	g_assert_nonnull(strstr(refused, "--stage"));
	usage = cli(f, bare, FALSE);
	g_assert_nonnull(strstr(usage, "calendar sync"));
}
static void test_api_guards(Fixture *f, gconstpointer unused)
{
	(void)unused;
	g_assert_cmpuint(request(f, "POST", "/api/v1/calendar/sync", "{\"limit\":0}", NULL), ==, 422);
	g_assert_cmpuint(request(f, "POST", "/api/v1/calendar/sync?stage=1", "{}", NULL), ==, 422);
	g_assert_cmpuint(request(f, "POST", "/api/v1/calendar/sync", "{}", NULL), ==, 200);
	venture_config_set_module_enabled(f->config, "calendar", FALSE);
	g_assert_cmpuint(request(f, "POST", "/api/v1/calendar/sync", "{}", NULL), ==, 404);
	g_assert_cmpuint(request(f, "GET", "/api/v1/calendar_account", NULL, NULL), ==, 404);
	g_assert_cmpuint(request(f, "GET", "/book/anything", NULL, NULL), ==, 404);
	venture_config_set_module_enabled(f->config, "calendar", TRUE);
}

/* Rule 4 over HTTP: the public page lists slots and books one; the second booking is refused. */
static void test_booking_page(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) page = NULL;
	g_autofree gchar *html = NULL, *json = NULL, *created = NULL, *taken = NULL, *formed = NULL;
	g_autoptr(JsonNode) slots = NULL;
	g_autofree gchar *first = NULL, *second = NULL, *body = NULL, *form = NULL;
	JsonArray *array;
	(void)unused;
	g_assert_cmpuint(request(f, "GET", "/book/nobody", NULL, NULL), ==, 404);
	page = g_object_new(VENTURE_TYPE_BOOKING_PAGE, "organization-id", f->org, "title", "Intro call", "slug", "intro", "owner", "ben",
		"duration-minutes", (gint64)30, "buffer-minutes", (gint64)0, "timezone", "UTC",
		"availability", "{\"mon\":\"00:00-24:00\",\"tue\":\"00:00-24:00\",\"wed\":\"00:00-24:00\",\"thu\":\"00:00-24:00\",\"fri\":\"00:00-24:00\",\"sat\":\"00:00-24:00\",\"sun\":\"00:00-24:00\"}",
		"horizon-days", (gint64)3, "active", TRUE, NULL);
	save(f, page);
	g_assert_cmpuint(request(f, "GET", "/book/intro", NULL, &html), ==, 200);
	g_assert_nonnull(strstr(html, "Intro call"));
	g_assert_nonnull(strstr(html, "<select name=\"start\""));
	g_assert_nonnull(strstr(html, "action=\"/book/intro\""));
	g_assert_cmpuint(request(f, "GET", "/book/intro?format=json", NULL, &json), ==, 200);
	slots = json_from_string(json, NULL);
	array = json_node_get_array(slots);
	g_assert_cmpuint(json_array_get_length(array), >, 2);
	first = g_strdup(venture_json_object_get_string(json_array_get_object_element(array, 0), "start", ""));
	second = g_strdup(venture_json_object_get_string(json_array_get_object_element(array, 1), "start", ""));
	body = g_strdup_printf("{\"start\":\"%s\",\"name\":\"Grace\",\"email\":\"grace@hopper.test\",\"notes\":\"Billing\"}", first);
	g_assert_cmpuint(request(f, "POST", "/book/intro", body, &created), ==, 201);
	g_assert_nonnull(strstr(created, "Intro call with Grace"));
	g_assert_cmpuint(request(f, "POST", "/book/intro", body, &taken), ==, 409);
	{
		g_autoptr(VentureQuery) q = venture_query_new(VENTURE_TYPE_ACTIVITY);
		venture_query_set_organization(q, f->org);
		g_assert_cmpint(venture_database_count(f->db, q, NULL), ==, 1);
	}
	/* A browser form books the next slot. */
	{
		g_autofree gchar *escaped = g_uri_escape_string(second, NULL, FALSE);
		form = g_strdup_printf("start=%s&name=Linus&email=linus%%40example.test&notes=", escaped);
	}
	g_assert_cmpuint(request_as(f, "POST", "/book/intro", "application/x-www-form-urlencoded", form, &formed), ==, 201);
	g_assert_nonnull(strstr(formed, "Booked"));
	g_assert_cmpuint(request_as(f, "POST", "/book/intro", "application/x-www-form-urlencoded", form, NULL), ==, 409);
	g_assert_cmpuint(request(f, "POST", "/book/intro", "{\"start\":\"\",\"name\":\"\",\"email\":\"\"}", NULL), ==, 422);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/calendar-surfaces/cli-sync", Fixture, NULL, setup, test_cli_sync, teardown);
	g_test_add("/calendar-surfaces/api-guards", Fixture, NULL, setup, test_api_guards, teardown);
	g_test_add("/calendar-surfaces/booking-page", Fixture, NULL, setup, test_booking_page, teardown);
	return g_test_run();
}
