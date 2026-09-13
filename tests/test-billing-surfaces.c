/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <libsoup/soup.h>
#include <string.h>
#include "venture-test-util.h"

typedef struct
{
	VentureDatabase *database;
	VentureConfig *config;
	VentureContext *context;
	VentureWebServer *server;
	SoupSession *session;
	gchar *state_dir;
	gchar *url;
	gint64 subscription;
} Fixture;

static void
save(Fixture *f, VentureEntity *e)
{
	g_autoptr(GError) error = NULL;
	venture_entity_set_organization_id(e, 1);
	g_assert_true(venture_database_save(f->database, e, NULL, &error));
	g_assert_no_error(error);
}

static void
setup(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GSocketListener) probe = g_socket_listener_new();
	g_autoptr(VentureCompany) company = venture_company_new();
	g_autoptr(VenturePlan) plan = venture_plan_new();
	g_autoptr(VenturePlanPrice) price = venture_plan_price_new();
	g_autoptr(VentureBillingRequest) action = venture_billing_request_new();
	guint port = g_socket_listener_add_any_inet_port(probe, NULL, &error);
	g_assert_no_error(error);
	g_clear_object(&probe);
	f->state_dir = g_dir_make_tmp("venture-billing-surfaces-XXXXXX", NULL);
	f->url = g_strdup_printf("http://127.0.0.1:%u", port);
	f->config = venture_config_new();
	g_object_set(f->config, "state-dir", f->state_dir, "server-bind-address", "127.0.0.1",
		"server-port", (gint64)port, "security-require-auth", FALSE, NULL);
	f->database = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->database);
	g_assert_true(venture_database_migrate(f->database, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	g_object_set(company, "name", "Lightsite customer", NULL);
	save(f, VENTURE_ENTITY(company));
	g_object_set(plan, "name", "Starter", "code", "starter", "active", TRUE, NULL);
	save(f, VENTURE_ENTITY(plan));
	g_object_set(price, "plan-id", venture_entity_get_id(VENTURE_ENTITY(plan)), "currency", "USD", "active", TRUE, NULL);
	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(price), "amount", "30 USD", NULL));
	save(f, VENTURE_ENTITY(price));
	g_object_set(action, "action", "start", "company-id", venture_entity_get_id(VENTURE_ENTITY(company)),
		"plan-price-id", venture_entity_get_id(VENTURE_ENTITY(price)), NULL);
	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(action), "at", "2026-01-01", NULL));
	save(f, VENTURE_ENTITY(action));
	g_object_get(action, "subscription-id", &f->subscription, NULL);
	f->server = venture_web_server_new(f->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(f->server, &error));
	g_assert_no_error(error);
	f->session = soup_session_new();
}

static void
teardown(Fixture *f, gconstpointer data)
{
	venture_web_server_stop(f->server);
	g_clear_object(&f->session);
	g_clear_object(&f->server);
	g_clear_object(&f->context);
	g_clear_object(&f->database);
	g_clear_object(&f->config);
	venture_test_remove_tree(f->state_dir);
	g_free(f->state_dir);
	g_free(f->url);
}

typedef struct
{
	gboolean done;
	GBytes *body;
	GError *error;
} Request;

static void
request_done(GObject *source, GAsyncResult *result, gpointer data)
{
	Request *request = data;
	request->body = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &request->error);
	request->done = TRUE;
}

static guint
request(Fixture *fixture, const gchar *method, const gchar *path, const gchar *mime, const gchar *body, gchar **response)
{
	g_autofree gchar *url = g_strconcat(fixture->url, path, NULL);
	g_autoptr(SoupMessage) message = soup_message_new(method, url);
	Request result = { FALSE, NULL, NULL };
	guint status;

	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	if (NULL != body)
	{
		g_autoptr(GBytes) bytes = g_bytes_new(body, strlen(body));
		soup_message_set_request_body_from_bytes(message, mime, bytes);
	}
	soup_session_send_and_read_async(fixture->session, message, G_PRIORITY_DEFAULT, NULL, request_done, &result);
	while (!result.done)
		g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(result.error);
	*response = g_strndup(g_bytes_get_data(result.body, NULL), g_bytes_get_size(result.body));
	status = soup_message_get_status(message);
	g_bytes_unref(result.body);
	return status;
}

static void
test_rest(Fixture *f, gconstpointer data)
{
	g_autofree gchar *body = NULL;
	g_autofree gchar *path = g_strdup_printf("/api/v1/customer_subscriptions/%" G_GINT64_FORMAT "/renew", f->subscription);
	g_autoptr(VentureQuery) q = venture_query_new(VENTURE_TYPE_INVOICE);
	g_assert_cmpuint(request(f, "POST", path, "application/json", "{\"at\":\"2026-02-01\"}", &body), ==, 200);
	g_assert_cmpint(venture_database_count(f->database, q, NULL), ==, 2);
}

static void
test_web(Fixture *f, gconstpointer data)
{
	g_autofree gchar *body = NULL;
	g_autofree gchar *path = g_strdup_printf("/e/customer_subscription/%" G_GINT64_FORMAT, f->subscription);
	g_assert_cmpuint(request(f, "GET", path, NULL, NULL, &body), ==, 200);
	g_assert_nonnull(strstr(body, "billing-action"));
	g_assert_nonnull(strstr(body, "Subscriptions"));
}

static void
test_staged(Fixture *f, gconstpointer data)
{
	g_autofree gchar *body = NULL;
	g_autofree gchar *path = g_strdup_printf("/api/v1/customer_subscriptions/%" G_GINT64_FORMAT "/pause?stage=1", f->subscription);
	g_autoptr(GPtrArray) pending = NULL;
	g_autoptr(VentureEntity) sub = NULL;
	g_autoptr(GError) error = NULL;
	VentureConfirmationStore *store = venture_context_get_confirmations(f->context);
	gint state;
	g_assert_cmpuint(request(f, "POST", path, "application/json", "{\"at\":\"2026-01-10\"}", &body), ==, 202);
	sub = venture_database_get(f->database, VENTURE_TYPE_CUSTOMER_SUBSCRIPTION, f->subscription, NULL);
	g_object_get(sub, "status", &state, NULL);
	g_assert_cmpint(state, ==, 1);
	pending = venture_confirmation_store_list_pending(store);
	g_assert_cmpuint(pending->len, ==, 1);
	g_assert_true(venture_confirmation_store_approve(store,
		venture_confirmation_get_id(g_ptr_array_index(pending, 0)), "owner", &error));
	g_assert_no_error(error);
	g_clear_object(&sub);
	sub = venture_database_get(f->database, VENTURE_TYPE_CUSTOMER_SUBSCRIPTION, f->subscription, NULL);
	g_object_get(sub, "status", &state, NULL);
	g_assert_cmpint(state, ==, 3);
}
typedef struct
{
	gboolean done;
	gchar *out;
	gchar *err;
	GError *error;
} CliResult;

static void
cli_done(GObject *source, GAsyncResult *result, gpointer data)
{
	CliResult *outcome = data;
	g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, &outcome->out, &outcome->err, &outcome->error);
	outcome->done = TRUE;
}

static void
test_cli(Fixture *f, gconstpointer data)
{
	g_autoptr(GSubprocessLauncher) launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE);
	g_autoptr(VentureQuery) q = venture_query_new(VENTURE_TYPE_INVOICE);
	guint i;
	g_subprocess_launcher_unsetenv(launcher, "VENTURE_TOKEN");
	for (i = 0; i < 3; i++)
	{
		g_autoptr(GError) error = NULL;
		g_autoptr(GSubprocess) child = NULL;
		CliResult result = { FALSE, NULL, NULL, NULL };
		const gchar *args[] = { "build/debug/venturectl", "--server", f->url,
			"billing", "renew", "--as-of", "2026-02-01", i == 0 ? "--dry-run" : NULL, NULL };
		child = g_subprocess_launcher_spawnv(launcher, args, &error);
		g_assert_no_error(error);
		g_subprocess_communicate_utf8_async(child, NULL, NULL, cli_done, &result);
		while (!result.done)
			g_main_context_iteration(NULL, TRUE);
		g_assert_no_error(result.error);
		g_test_message("CLI stderr: %s", result.err);
		g_assert_true(g_subprocess_get_successful(child));
		g_free(result.out);
		g_free(result.err);
		g_assert_cmpint(venture_database_count(f->database, q, NULL), ==, i == 0 ? 1 : 2);
	}
}

static void
test_assistant_stale(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureBillingRequest) proposed = venture_billing_request_new();
	g_autoptr(VentureBillingRequest) pause = venture_billing_request_new();
	g_autoptr(GError) error = NULL;
	VentureConfirmationStore *store = venture_context_get_confirmations(f->context);
	VentureConfirmation *confirmation;
	VentureActor actor;
	actor.kind = VENTURE_ACTOR_KIND_AI;
	actor.name = "assistant";
	actor.prompt = "Cancel the Lightsite subscription";
	actor.request_id = NULL;
	actor.approved_by = NULL;
	g_object_set(proposed, "organization-id", (gint64)1, "subscription-id", f->subscription, "action", "cancel", NULL);
	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(proposed), "at", "2026-01-10", NULL));
	confirmation = venture_confirmation_store_stage(store, VENTURE_AUDIT_ACTION_CREATE,
		VENTURE_ENTITY(proposed), NULL, &actor, "assistant", &error);
	g_assert_no_error(error);
	g_assert_nonnull(confirmation);
	g_object_set(pause, "subscription-id", f->subscription, "action", "pause", NULL);
	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(pause), "at", "2026-01-05", NULL));
	save(f, VENTURE_ENTITY(pause));
	g_assert_false(venture_confirmation_store_approve(store, venture_confirmation_get_id(confirmation), "owner", &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
}

static void
test_report_days(Fixture *f, gconstpointer data)
{
	g_autofree gchar *body = NULL;
	g_autoptr(JsonParser) parser = json_parser_new();
	JsonArray *rows;
	g_assert_cmpuint(request(f, "GET", "/api/v1/reports/subscriptions_due?period=2025-12&days=365", NULL, NULL, &body), ==, 200);
	g_assert_true(json_parser_load_from_data(parser, body, -1, NULL));
	rows = json_object_get_array_member(json_node_get_object(json_parser_get_root(parser)), "rows");
	g_assert_cmpuint(json_array_get_length(rows), ==, 1);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/billing-surfaces/rest", Fixture, NULL, setup, test_rest, teardown);
	g_test_add("/billing-surfaces/web", Fixture, NULL, setup, test_web, teardown);
	g_test_add("/billing-surfaces/staged", Fixture, NULL, setup, test_staged, teardown);
	g_test_add("/billing-surfaces/cli", Fixture, NULL, setup, test_cli, teardown);
	g_test_add("/billing-surfaces/assistant-stale", Fixture, NULL, setup, test_assistant_stale, teardown);
	g_test_add("/billing-surfaces/report-days", Fixture, NULL, setup, test_report_days, teardown);
	return g_test_run();
}
