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
static guint request_full(Fixture *f, const gchar *method, const gchar *path, const gchar *body,
	const gchar *content_type, gchar **out)
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
	return request_full(f, method, path, body, "application/json", out);
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

/* The inbound actions an operator reaches from a page or the command line:
 * dismissing and converting an unmatched sender and syncing one account.
 * If a route loses its handler the buttons post into a 404, and if the CLI
 * verbs drift from the routes the skill documents commands that fail. */
static void test_inbound_actions(Fixture *f, gconstpointer unused)
{
	gint64 org = venture_context_get_default_organization_id(f->context);
	g_autoptr(VentureEntity) keep = g_object_new(VENTURE_TYPE_MAIL_UNMATCHED_SENDER, "organization-id", org, "address", "keep@else.test", "name", "Keep", "seen", (gint64)1, NULL);
	g_autoptr(VentureEntity) quiet = g_object_new(VENTURE_TYPE_MAIL_UNMATCHED_SENDER, "organization-id", org, "address", "quiet@else.test", "seen", (gint64)1, NULL);
	g_autoptr(VentureEntity) account = g_object_new(VENTURE_TYPE_MAIL_ACCOUNT, "organization-id", org, "address", "ops@example.test",
		"imap-host", "imap.example.test", "username", "ops", "imap-port", (gint64)993, "imap-tls", "tls", "secret-env", "VENTURE_IMAP_SURFACES_MISSING", "active", TRUE, NULL);
	g_autoptr(VentureEntity) reread = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *keep_id = NULL, *quiet_id = NULL, *account_arg = NULL, *dismissed = NULL, *contact = NULL, *synced = NULL;
	g_autofree gchar *page = NULL, *sender_page = NULL, *path = NULL;
	const gchar *dismiss[] = { "mail", "dismiss", NULL, NULL };
	const gchar *convert[] = { "mail", "contact", NULL, NULL };
	const gchar *sync_one[] = { "mail", "sync", NULL, NULL };
	gboolean is_dismissed = FALSE;
	g_assert_true(venture_database_save(f->db, keep, NULL, &error));
	g_assert_true(venture_database_save(f->db, quiet, NULL, &error));
	g_assert_true(venture_database_save(f->db, account, NULL, &error));
	g_assert_no_error(error);
	keep_id = g_strdup_printf("%" G_GINT64_FORMAT, venture_entity_get_id(keep));
	quiet_id = g_strdup_printf("%" G_GINT64_FORMAT, venture_entity_get_id(quiet));
	account_arg = g_strdup_printf("account_id=%" G_GINT64_FORMAT, venture_entity_get_id(account));
	/* The pages carry the buttons. */
	path = g_strdup_printf("/e/mail_account/%" G_GINT64_FORMAT, venture_entity_get_id(account));
	g_assert_cmpuint(request(f, "GET", path, NULL, &page), ==, 200);
	g_assert_nonnull(strstr(page, "/sync\">"));
	g_assert_nonnull(strstr(page, "Sync now"));
	g_clear_pointer(&path, g_free);
	path = g_strdup_printf("/e/mail_unmatched_sender/%s", quiet_id);
	g_assert_cmpuint(request(f, "GET", path, NULL, &sender_page), ==, 200);
	g_assert_nonnull(strstr(sender_page, "Create contact"));
	g_assert_nonnull(strstr(sender_page, "Dismiss"));
	/* A button's form post lands back on a page. */
	g_clear_pointer(&path, g_free);
	path = g_strdup_printf("/mail_accounts/%" G_GINT64_FORMAT "/sync", venture_entity_get_id(account));
	g_assert_cmpuint(request(f, "POST", path, NULL, NULL), ==, 302);
	g_assert_cmpuint(request(f, "POST", "/mail_unmatched_senders/999999/dismiss", NULL, NULL), ==, 404);
	dismiss[2] = quiet_id;
	dismissed = cli(f, dismiss);
	g_assert_nonnull(strstr(dismissed, "\"dismissed\""));
	reread = venture_database_get(f->db, VENTURE_TYPE_MAIL_UNMATCHED_SENDER, venture_entity_get_id(quiet), &error);
	g_object_get(reread, "dismissed", &is_dismissed, NULL);
	g_assert_true(is_dismissed);
	convert[2] = keep_id;
	contact = cli(f, convert);
	g_assert_nonnull(strstr(contact, "keep@else.test"));
	g_clear_object(&reread);
	reread = venture_database_get(f->db, VENTURE_TYPE_MAIL_UNMATCHED_SENDER, venture_entity_get_id(keep), &error);
	g_assert_true(venture_entity_is_deleted(reread));
	/* An account outside the operator allowlist is reported without resolving
	 * its obsolete environment label or contacting the provider. */
	sync_one[2] = account_arg;
	synced = cli(f, sync_one);
	g_assert_nonnull(strstr(synced, "operator allowlist"));
	g_assert_null(strstr(synced, "VENTURE_IMAP_SURFACES_MISSING"));
	g_assert_nonnull(strstr(synced, "\"accounts\""));
}

static void test_organization_settings(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) key = g_bytes_new_static("01234567890123456789012345678901", 32);
	g_autoptr(VentureIntegrationConnection) connection = NULL;
	g_autofree gchar *path = NULL, *page = NULL, *form = NULL;
	gint64 org = venture_context_get_default_organization_id(f->context);
	(void)unused;
	g_assert_true(venture_integration_service_set_key(venture_integration_service_get(f->db), key, &error));
	g_object_set(f->config, "mail-allowed-endpoints", "smtp.example.invalid:587", NULL);
	path = g_strdup_printf("/organizations/%" G_GINT64_FORMAT "/settings/mail", org);
	g_assert_cmpuint(request(f, "GET", path, NULL, &page), ==, 200);
	g_assert_nonnull(strstr(page, "Organization mail is unconfigured"));
	g_assert_nonnull(strstr(page, "name=\"password\" type=\"password\""));
	g_clear_pointer(&page, g_free);
	g_assert_cmpuint(request_full(f, "POST", path,
		"operation=configure&version=0&connection_id=0&host=smtp.example.invalid&from=billing%40example.invalid&username=tenant-account&password=unique-fixture-secret",
		"application/x-www-form-urlencoded", &page), ==, 200);
	g_assert_nonnull(strstr(page, "SMTP settings saved"));
	g_assert_null(strstr(page, "unique-fixture-secret"));
	g_assert_null(strstr(page, "tenant-account"));
	g_assert_nonnull(strstr(page, "name=\"password\" type=\"password\" autocomplete=\"new-password\" value=\"\""));
	connection = venture_integration_service_find(venture_integration_service_get(f->db), org, "smtp", &error);
	g_assert_no_error(error);
	g_assert_nonnull(connection);
	g_clear_pointer(&page, g_free);
	/* A stale creation form cannot replace even this same account. */
	g_assert_cmpuint(request_full(f, "POST", path,
		"operation=configure&version=0&connection_id=0&host=smtp.example.invalid&from=billing%40example.invalid&username=tenant-account&password=stale-fixture-secret",
		"application/x-www-form-urlencoded", &page), ==, 400);
	g_assert_null(strstr(page, "stale-fixture-secret"));
	g_clear_pointer(&page, g_free);
	form = g_strdup_printf("operation=disconnect&connection_id=%" G_GINT64_FORMAT "&version=%" G_GINT64_FORMAT,
		venture_entity_get_id(VENTURE_ENTITY(connection)), venture_entity_get_version(VENTURE_ENTITY(connection)));
	g_assert_cmpuint(request_full(f, "POST", path, form, "application/x-www-form-urlencoded", &page), ==, 200);
	g_assert_nonnull(strstr(page, "Disconnected"));
	g_assert_nonnull(strstr(page, "Organization mail is unconfigured"));
}
int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/mail-surfaces/cli-delivery", Fixture, NULL, setup, test_cli_delivery, teardown);
	g_test_add("/mail-surfaces/api-guards", Fixture, NULL, setup, test_api_guards, teardown);
	g_test_add("/mail-surfaces/automation-sweep", Fixture, NULL, setup, test_automation_sweep, teardown);
	g_test_add("/mail-surfaces/smtp-policy", Fixture, NULL, setup, test_smtp_policy, teardown);
	g_test_add("/mail-surfaces/inbound-actions", Fixture, NULL, setup, test_inbound_actions, teardown);
	g_test_add("/mail-surfaces/organization-settings", Fixture, NULL, setup, test_organization_settings, teardown);
	return g_test_run();
}
