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
	VentureLogMailer *mailer;
	VentureWebServer *server;
	gchar *directory;
} Fixture;
static void setup(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GSocketListener) listener = g_socket_listener_new();
	guint16 port = g_socket_listener_add_any_inet_port(listener, NULL, &error);
	g_assert_no_error(error);
	g_socket_listener_close(listener);
	f->directory = g_dir_make_tmp("venture-mail-surfaces-XXXXXX", &error);
	g_assert_no_error(error);
	f->config = venture_config_new();
	g_object_set(f->config, "state-dir", f->directory, "server-bind-address", "127.0.0.1", "server-port", (gint64)port, "security-require-auth", FALSE, NULL);
	f->db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->db);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->mailer = venture_log_mailer_new();
	venture_context_set_mailer(f->context, VENTURE_MAILER(f->mailer));
	f->server = venture_web_server_new(f->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(f->server, &error));
	g_assert_no_error(error);
}
static void teardown(Fixture *f, gconstpointer unused)
{
	venture_web_server_stop(f->server);
	g_clear_object(&f->server); g_clear_object(&f->context); g_clear_object(&f->db);
	g_clear_object(&f->config); g_clear_object(&f->mailer);
	venture_test_remove_tree(f->directory); g_free(f->directory);
}
static void http_done(GObject *source, GAsyncResult *result, gpointer data)
{
	Result *r = data;
	r->bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &r->error);
	r->done = TRUE;
}
static guint request(Fixture *f, const gchar *method, const gchar *path, const gchar *body, gchar **out)
{
	g_autoptr(SoupSession) session = soup_session_new_with_options("timeout", 15, NULL);
	g_autofree gchar *url = g_strconcat(venture_web_server_get_base_url(f->server), path, NULL);
	g_autoptr(SoupMessage) message = soup_message_new(method, url);
	Result result;
	memset(&result, 0, sizeof(result));
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	if (body) {
		g_autoptr(GBytes) bytes = g_bytes_new(body, strlen(body));
		soup_message_set_request_body_from_bytes(message, "application/json", bytes);
	}
	soup_session_send_and_read_async(session, message, G_PRIORITY_DEFAULT, NULL, http_done, &result);
	while (!result.done) g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(result.error);
	if (out) *out = g_strndup(g_bytes_get_data(result.bytes, NULL), g_bytes_get_size(result.bytes));
	g_bytes_unref(result.bytes);
	return soup_message_get_status(message);
}
static void cli_done(GObject *source, GAsyncResult *result, gpointer data)
{
	Result *r = data;
	g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, &r->out, &r->err, &r->error);
	r->done = TRUE;
}
static gboolean cli_timeout(gpointer process) { g_subprocess_force_exit(process); return G_SOURCE_CONTINUE; }
static gchar *cli(Fixture *f, const gchar *const *args)
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
	g_subprocess_launcher_setenv(launcher, "VENTURE_TOKEN", "mail-fixture", TRUE);
	process = g_subprocess_launcher_spawnv(launcher, (const gchar *const *)argv->pdata, &error);
	g_assert_no_error(error);
	timeout = g_timeout_add_seconds(30, cli_timeout, process);
	g_subprocess_communicate_utf8_async(process, NULL, NULL, cli_done, &result);
	while (!result.done) g_main_context_iteration(NULL, TRUE);
	g_source_remove(timeout);
	g_assert_no_error(result.error);
	if (!g_subprocess_get_successful(process)) g_test_message("CLI failure: %s", result.err);
	g_assert_true(g_subprocess_get_successful(process));
	g_free(result.err);
	return result.out;
}
static void test_cli_delivery(Fixture *f, gconstpointer unused)
{
	const gchar *send[] = { "mail", "send", "to=reader@example.test", "subject=Receipt", "body=Hello", NULL };
	const gchar *deliver[] = { "mail", "deliver", "--limit", "1", NULL };
	const gchar *list[] = { "mail", "list", "state=uncertain", NULL };
	g_autofree gchar *sent = cli(f, send), *delivery = NULL, *listed = NULL, *retry_arg = NULL, *retried = NULL;
	g_autoptr(JsonNode) node = json_from_string(sent, NULL);
	g_autoptr(GError) uncertain = g_error_new_literal(VENTURE_ERROR, VENTURE_ERROR_MAIL_UNCERTAIN, "Reply lost");
	gint64 id = venture_json_object_get_int(json_node_get_object(node), "id", 0);
	const gchar *retry[] = { "mail", "retry", NULL, NULL };
	g_assert_cmpint(id, >, 0);
	venture_log_mailer_set_error(f->mailer, uncertain);
	delivery = cli(f, deliver);
	g_assert_nonnull(strstr(delivery, "attempted"));
	listed = cli(f, list);
	g_assert_nonnull(strstr(listed, "uncertain"));
	g_assert_cmpuint(venture_log_mailer_get_messages(f->mailer)->len, ==, 1);
	retry_arg = g_strdup_printf("%" G_GINT64_FORMAT, id); retry[2] = retry_arg;
	retried = cli(f, retry);
	g_assert_nonnull(strstr(retried, "true"));
	venture_log_mailer_set_error(f->mailer, NULL);
	g_clear_pointer(&delivery, g_free); delivery = cli(f, deliver);
	g_assert_cmpuint(venture_log_mailer_get_messages(f->mailer)->len, ==, 2);
}
static void test_api_guards(Fixture *f, gconstpointer unused)
{
	g_assert_cmpuint(request(f, "POST", "/api/v1/mail/deliver", "{\"limit\":0}", NULL), ==, 422);
	g_assert_cmpuint(request(f, "POST", "/api/v1/mail_messages/999999/retry", "{}", NULL), ==, 404);
	g_assert_cmpuint(request(f, "POST", "/api/v1/invoices/999999/send", "{}", NULL), ==, 404);
	g_assert_cmpuint(request(f, "POST", "/api/v1/mail/send", "{\"to\":\"reader@example.test\",\"subject\":\"Hello\",\"state\":\"sent\"}", NULL), ==, 422);
	venture_config_set_module_enabled(f->config, "mail", FALSE);
	g_assert_cmpuint(request(f, "POST", "/api/v1/mail/deliver", "{}", NULL), ==, 404);
	g_assert_cmpuint(request(f, "GET", "/api/v1/mail_message", NULL, NULL), ==, 404);
	venture_config_set_module_enabled(f->config, "mail", TRUE);
}
/* A policy plugin can refuse mail even for an install owner; the SMTP test
 * must check that refusal before contacting its transport. */
static GError *deny_mail_write(VentureAccessPolicy *policy, const VentureAuthPrincipal *actor,
	const gchar *action, VentureEntity *entity, gpointer unused)
{
	if (VENTURE_IS_MAIL_MESSAGE(entity) && !g_strcmp0(action, "write"))
		return g_error_new_literal(VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED, "Mail denied by policy");
	return NULL;
}
static void test_smtp_policy(Fixture *f, gconstpointer unused)
{
	VentureAccessPolicy *policy = venture_database_get_access_policy(f->db);
	gulong handler = g_signal_connect(policy, "decide", G_CALLBACK(deny_mail_write), NULL);
	g_assert_cmpuint(request(f, "POST", "/api/v1/mail/test", "{\"to\":\"reader@example.test\"}", NULL), ==, 403);
	g_assert_cmpuint(venture_log_mailer_get_messages(f->mailer)->len, ==, 0);
	g_signal_handler_disconnect(policy, handler);
	g_assert_cmpuint(request(f, "POST", "/api/v1/mail/test", "{\"to\":\"reader@example.test\"}", NULL), ==, 200);
	g_assert_cmpuint(venture_log_mailer_get_messages(f->mailer)->len, ==, 1);
}
static void test_automation_sweep(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureAutomation) automation = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GVariant) result = NULL;
	g_autofree gchar *org = g_strdup_printf("%" G_GINT64_FORMAT, venture_context_get_default_organization_id(f->context));
	const gchar *args[] = { org, "1", NULL };
	g_assert_cmpuint(request(f, "POST", "/api/v1/mail/send", "{\"to\":\"reader@example.test\",\"subject\":\"Automation\",\"text_body\":\"Hello\"}", NULL), ==, 202);
	automation = venture_automation_new(f->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_automation_invoke(automation, "mail_deliver", args, &result, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(venture_log_mailer_get_messages(f->mailer)->len, ==, 1);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/mail-surfaces/cli-delivery", Fixture, NULL, setup, test_cli_delivery, teardown);
	g_test_add("/mail-surfaces/api-guards", Fixture, NULL, setup, test_api_guards, teardown);
	g_test_add("/mail-surfaces/automation-sweep", Fixture, NULL, setup, test_automation_sweep, teardown);
	g_test_add("/mail-surfaces/smtp-policy", Fixture, NULL, setup, test_smtp_policy, teardown);
	return g_test_run();
}
