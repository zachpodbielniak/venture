/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <stripe-glib.h>
#include <string.h>
#include <libsoup/soup.h>
#include "venture-test-util.h"

typedef struct
{
	VentureDatabase *database;
	VentureConfig *config;
	VentureContext *context;
	gint64 organization_id;
	gint64 customer_id;
	gint64 product_id;
} Fixture;

static void
save(Fixture *f, VentureEntity *record)
{
	g_autoptr(GError) error = NULL;
	gboolean ok;

	ok = venture_database_save(f->database, record, NULL, &error);
	g_assert_no_error(error);
	g_assert_true(ok);
}

static void
set_up(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureCompany) customer = NULL;

	f->config = venture_config_new();
	g_object_set(f->config, "stripe-enabled", TRUE, NULL);
	f->database = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->database,
		venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->database);
	f->organization_id = venture_context_get_default_organization_id(f->context);
	customer = venture_company_new();
	g_object_set(customer, "name", "Customer", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(customer), f->organization_id);
	save(f, VENTURE_ENTITY(customer));
	f->customer_id = venture_entity_get_id(VENTURE_ENTITY(customer));
	{
		g_autoptr(VentureProduct) product = venture_product_new();
		g_object_set(product, "name", "Work", NULL);
		venture_entity_set_organization_id(VENTURE_ENTITY(product), f->organization_id);
		save(f, VENTURE_ENTITY(product));
		f->product_id = venture_entity_get_id(VENTURE_ENTITY(product));
	}
}

static void
tear_down(Fixture *f, gconstpointer data)
{
	g_clear_object(&f->context);
	g_clear_object(&f->database);
	g_clear_object(&f->config);
}

static GType
record_type(const gchar *name)
{
	GType type;

	type = venture_entity_registry_lookup(venture_entity_registry_get_default(), name);
	g_assert_cmpuint(type, !=, G_TYPE_INVALID);
	return type;
}

static VentureEntity *
record_new(Fixture *f, const gchar *name)
{
	VentureEntity *record;

	record = g_object_new(record_type(name), NULL);
	venture_entity_set_organization_id(record, f->organization_id);
	return record;
}

static GPtrArray *
rows(Fixture *f, const gchar *name)
{
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GError) error = NULL;
	GPtrArray *found;

	query = venture_query_new(record_type(name));
	venture_query_set_limit(query, 0);
	venture_query_set_include_deleted(query, TRUE);
	g_assert_true(venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, &error));
	found = venture_database_find(f->database, query, &error);
	g_assert_no_error(error);
	g_assert_nonnull(found);
	return found;
}

static void
money_field(VentureEntity *record, const gchar *field, const gchar *amount)
{
	g_autoptr(GError) error = NULL;

	g_assert_true(venture_entity_set_field_from_string(record, field, amount, &error));
	g_assert_no_error(error);
}

static VentureEntity *
invoice_new(Fixture *f, const gchar *number, const gchar *issued, const gchar *amount)
{
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(VentureEntity) line = NULL;

	invoice = record_new(f, "invoice");
	g_object_set(invoice, "number", number, "company-id", f->customer_id, NULL);
	money_field(invoice, "issued-at", issued);
	money_field(invoice, "due-at", issued);
	save(f, invoice);
	line = record_new(f, "invoice_line");
	g_object_set(line, "invoice-id", venture_entity_get_id(invoice),
		"description", "Work", "quantity", 1.0, "product-id", f->product_id, NULL);
	money_field(line, "unit-price", amount);
	save(f, line);
	g_object_set(invoice, "status", VENTURE_INVOICE_STATUS_SENT, NULL);
	save(f, invoice);
	return g_steal_pointer(&invoice);
}

typedef struct
{
	gboolean done;
	GBytes *bytes;
	GError *error;
	gchar *out;
	gchar *err;
} SurfaceResult;

static void
http_done(GObject *source, GAsyncResult *result, gpointer data)
{
	SurfaceResult *response;

	response = data;
	response->bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &response->error);
	response->done = TRUE;
}

static gchar *signature(const gchar *body);

static guint
http_request(VentureWebServer *server, const gchar *method, const gchar *path,
	const gchar *content_type, const gchar *body, gchar **out)
{
	g_autoptr(SoupSession) session = NULL;
	g_autoptr(SoupMessage) message = NULL;
	g_autofree gchar *url = NULL;
	SurfaceResult response;
	guint status;

	memset(&response, 0, sizeof(response));
	session = soup_session_new_with_options("timeout", 15, NULL);
	url = g_strconcat(venture_web_server_get_base_url(server), path, NULL);
	message = soup_message_new(method, url);
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	if (body && !g_strcmp0(path, "/webhooks/stripe"))
	{
		g_autofree gchar *sig = !g_strcmp0(body, "{}") ? g_strdup("t=0,v1=invalid") : signature(body);
		soup_message_headers_append(soup_message_get_request_headers(message), "Stripe-Signature", sig);
	}

	if (body != NULL)
	{
		g_autoptr(GBytes) bytes = NULL;

		bytes = g_bytes_new(body, strlen(body));
		soup_message_set_request_body_from_bytes(message, content_type, bytes);
	}
	soup_session_send_and_read_async(session, message, G_PRIORITY_DEFAULT, NULL, http_done, &response);
	while (!response.done)
		g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(response.error);
	if (out != NULL)
		*out = g_strndup(g_bytes_get_data(response.bytes, NULL), g_bytes_get_size(response.bytes));
	status = soup_message_get_status(message);
	g_clear_pointer(&response.bytes, g_bytes_unref);
	return status;
}

static void
cli_done(GObject *source, GAsyncResult *result, gpointer data)
{
	SurfaceResult *response;

	response = data;
	g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, &response->out, &response->err, &response->error);
	response->done = TRUE;
}

static gboolean
cli_timeout(gpointer data)
{
	g_subprocess_force_exit(G_SUBPROCESS(data));
	return G_SOURCE_CONTINUE;
}

static gchar *
run_cli(const gchar *const *argv, const gchar *input, gboolean success)
{
	g_autoptr(GSubprocess) process = NULL;
	g_autoptr(GSubprocessLauncher) launcher = NULL;
	g_autoptr(GError) error = NULL;
	SurfaceResult response;
	guint timeout;

	memset(&response, 0, sizeof(response));
	launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE);
	/* MCP insists on a token even when the private loopback fixture has
	 * authentication disabled. Never inherit a real install's credential. */
	g_subprocess_launcher_setenv(launcher, "VENTURE_TOKEN", "receivables-test-only", TRUE);
	process = g_subprocess_launcher_spawnv(launcher, argv, &error);
	g_assert_no_error(error);
	timeout = g_timeout_add_seconds(30, cli_timeout, process);
	g_subprocess_communicate_utf8_async(process, input, NULL, cli_done, &response);
	while (!response.done)
		g_main_context_iteration(NULL, TRUE);
	g_source_remove(timeout);
	g_assert_no_error(response.error);
	if (g_subprocess_get_successful(process) != success)
		g_test_message("CLI stdout: %s; stderr: %s", response.out, response.err);
	g_assert_cmpint(g_subprocess_get_successful(process), ==, success);
	g_free(response.err);
	return response.out;
}

static VentureWebServer *
start_server(Fixture *f, gchar **state_dir)
{
	g_autoptr(GSocketListener) listener = NULL;
	g_autoptr(GError) error = NULL;
	VentureWebServer *server;
	guint16 port;

	*state_dir = g_dir_make_tmp("venture-stripe-XXXXXX", &error);
	g_assert_no_error(error);
	listener = g_socket_listener_new();
	port = g_socket_listener_add_any_inet_port(listener, NULL, &error);
	g_assert_no_error(error);
	g_socket_listener_close(listener);
	g_object_set(f->config, "state-dir", *state_dir, "server-bind-address", "127.0.0.1",
		"server-port", (gint64)port, "security-require-auth", FALSE, NULL);
	server = venture_web_server_new(f->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(server, &error));
	g_assert_no_error(error);
	return server;
}

static void
test_records(void)
{
	static const gchar *const names[] = {
		"stripe_price_link", "stripe_customer_link", "stripe_checkout", "stripe_event",
		"processor_payout", "processor_payout_item", "processor_dispute", "processor_exception"
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(names); i++)
		g_assert_cmpuint(venture_entity_registry_lookup(
			venture_entity_registry_get_default(), names[i]), !=, G_TYPE_INVALID);
}

static void
test_missing_key(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureDatabase) db = NULL;
	g_autoptr(VentureStripeService) service = NULL;

	g_unsetenv("VENTURE_STRIPE_SECRET_KEY");
	db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	service = venture_stripe_service_new(db, 1, NULL, &error);
	g_assert_null(service);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
	g_assert_nonnull(strstr(error->message, "VENTURE_STRIPE_SECRET_KEY"));
	g_setenv("VENTURE_STRIPE_SECRET_KEY", "offline", TRUE);
}

typedef struct { GObject parent; guint calls; guint customers; guint checkouts; gchar *key; const gchar *price_json; gint64 price_amount; } FakeTransport;
typedef struct { GObjectClass parent; } FakeTransportClass;
GType fake_transport_get_type(void);
static void fake_iface(StripeTransportInterface *iface);
G_DEFINE_TYPE_WITH_CODE(FakeTransport, fake_transport, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(STRIPE_TYPE_TRANSPORT, fake_iface))
static StripeResponse *
fake_send(StripeTransport *transport, const StripeHttpRequest *request,
	GCancellable *cancellable, GError **error)
{
	FakeTransport *self = (FakeTransport *)transport;
	(void)cancellable; (void)error;
	self->calls++;
	if (g_str_has_suffix(request->url, "/prices/price_offline"))
	{
		g_autofree gchar *body = NULL;
		g_assert_cmpstr(request->method, ==, "GET");
		body = g_strdup_printf("{\"id\":\"price_offline\",\"object\":\"price\",\"active\":true,\"type\":\"one_time\",\"billing_scheme\":\"per_unit\",\"unit_amount\":%" G_GINT64_FORMAT ",\"currency\":\"usd\"}", self->price_amount ? self->price_amount : 10000);
		return stripe_response_new(200, self->price_json ? self->price_json : body, NULL, NULL);
	}
	if (g_str_has_suffix(request->url, "/customers"))
	{
		self->customers++;
		g_assert_nonnull(strstr(request->body, "venture_company_uuid"));
		return stripe_response_new(200, "{\"id\":\"cus_offline\",\"object\":\"customer\"}", NULL, NULL);
	}
	g_assert_true(strstr(request->body, "price_offline") || strstr(request->body, "price_data"));
	g_assert_null(strstr(request->body, "metadata"));
	g_free(self->key);
	self->key = g_strdup(request->idempotency_key);
	self->checkouts++;
	if (self->checkouts > 1) return stripe_response_new(200, "{\"id\":\"cs_second\",\"url\":\"https://checkout.stripe.com/second\"}", NULL, NULL);
	return stripe_response_new(200, "{\"id\":\"cs_offline\",\"object\":\"checkout.session\",\"url\":\"https://checkout.stripe.com/offline\"}", NULL, NULL);
}
static void fake_iface(StripeTransportInterface *iface) { iface->send = fake_send; }
static void fake_finalize(GObject *object)
{
	g_free(((FakeTransport *)object)->key);
	G_OBJECT_CLASS(fake_transport_parent_class)->finalize(object);
}
static void fake_transport_class_init(FakeTransportClass *klass) { G_OBJECT_CLASS(klass)->finalize = fake_finalize; }
static void fake_transport_init(FakeTransport *self) { (void)self; }

static void
environment(void)
{
	g_setenv("VENTURE_STRIPE_SECRET_KEY", "offline", TRUE);
	g_setenv("VENTURE_STRIPE_PUBLISHABLE_KEY", "offline", TRUE);
	g_setenv("VENTURE_STRIPE_WEBHOOK_SECRET", "offline", TRUE);
	g_setenv("VENTURE_STRIPE_API_VERSION", "2025-03-31.basil", TRUE);
	g_setenv("VENTURE_STRIPE_SUCCESS_URL", "https://example.org/success", TRUE);
	g_setenv("VENTURE_STRIPE_CANCEL_URL", "https://example.org/cancel", TRUE);
}

static gchar *
signature(const gchar *body)
{
	g_autofree gchar *signed_text = NULL;
	g_autofree gchar *mac = NULL;
	gint64 timestamp = g_get_real_time() / G_USEC_PER_SEC;
	signed_text = g_strdup_printf("%" G_GINT64_FORMAT ".%s", timestamp, body);
	mac = g_compute_hmac_for_string(G_CHECKSUM_SHA256, (const guchar *)"offline", 7, signed_text, -1);
	return g_strdup_printf("t=%" G_GINT64_FORMAT ",v1=%s", timestamp, mac);
}

static GError *
fail_posting(VenturePostingService *posting, VentureJournal *journal, GPtrArray *lines, gpointer data)
{
	(void)posting; (void)journal; (void)lines; (void)data;
	return g_error_new_literal(VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Injected settlement failure");
}

/* Signature-authenticated delivery must not inherit an incidental browser's
 * local write veto. The exact HTTP capability boundary supplies its authority. */
static GError *
deny_stripe_event(VentureAccessPolicy *policy, const VentureAuthPrincipal *actor,
	const gchar *action, VentureEntity *entity, gpointer unused)
{
	if (G_OBJECT_TYPE(entity) == venture_stripe_event_get_type() && !g_strcmp0(action, "write"))
		return g_error_new_literal(VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED, "local event write denied");
	return NULL;
}

static void
test_flow(Fixture *f, gconstpointer data)
{
	const gchar *mode = data;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureStripeService) service = NULL;
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(VentureEntity) price = NULL;
	g_autoptr(VentureStripeCheckout) checkout = NULL;
	g_autoptr(GObject) transport = g_object_new(fake_transport_get_type(), NULL);
	g_autoptr(GBytes) bytes = NULL;
	g_autoptr(GPtrArray) events = NULL;
	g_autoptr(GPtrArray) payments = NULL;
	g_autoptr(GPtrArray) audits_before = NULL;
	g_autoptr(GPtrArray) audits_after = NULL;
	g_autofree gchar *body = NULL;
	g_autofree gchar *sig = NULL;
	g_autofree gchar *key = NULL;
	g_autoptr(VentureMoney) balance = NULL;
	gulong handler = 0;
	gboolean ok;

	service = venture_stripe_service_new(f->database, f->organization_id, STRIPE_TRANSPORT(transport), &error);
	g_assert_no_error(error);
	g_assert_nonnull(service);
	if (!g_strcmp0(mode, "precision-mismatch") || !g_strcmp0(mode, "overflow-mismatch"))
		((FakeTransport *)transport)->price_amount = 100;
	invoice = invoice_new(f, "stripe-test", "2026-01-01", (!g_strcmp0(mode, "precision-mismatch") || !g_strcmp0(mode, "overflow-mismatch")) ? "1.0000 USD" : "100 USD");
	price = record_new(f, "stripe_price_link");
	g_object_set(price, "product-id", f->product_id, "stripe-price-id", "price_offline", NULL);
	save(f, price);
	if (!g_strcmp0(mode, "portal"))
	{
		g_autoptr(VentureWebServer) server = NULL;
		g_autoptr(VentureEntity) access = NULL;
		g_autofree gchar *state_dir = NULL, *token = NULL, *path = NULL, *form = NULL;
		VenturePortalService *portal = venture_portal_service_get(f->database);
		access = venture_portal_service_invite(portal, f->organization_id, f->customer_id,
			"customer@example.test", NULL, &error);
		g_assert_no_error(error);
		g_object_get(access, "token", &token, NULL);
		venture_context_set_stripe_service(f->context, service);
		server = start_server(f, &state_dir);
		g_object_set(f->config, "security-require-auth", TRUE, NULL);
		path = g_strdup_printf("/portal/%s", token);
		form = g_strdup_printf("invoice_id=%" G_GINT64_FORMAT "&amount=0.01", venture_entity_get_id(invoice));
		/* The invitation authorizes Checkout, never a submitted receipt. */
		g_assert_cmpuint(http_request(server, "POST", path, "application/x-www-form-urlencoded", form, NULL), ==, 303);
		g_assert_cmpuint(((FakeTransport *)transport)->checkouts, ==, 1);
		balance = venture_settlement_service_invoice_balance(venture_settlement_service_get(f->database),
			venture_entity_get_id(invoice), NULL, &error);
		g_assert_no_error(error);
		g_assert_cmpint(venture_money_get_amount(balance), ==, 10000);
		g_assert_true(venture_portal_service_revoke(portal, VENTURE_CUSTOMER_PORTAL_ACCESS(access), NULL, &error));
		g_assert_no_error(error);
		g_assert_cmpuint(http_request(server, "POST", path, "application/x-www-form-urlencoded", form, NULL), ==, 404);
		g_assert_cmpuint(((FakeTransport *)transport)->checkouts, ==, 1);
		venture_web_server_stop(server);
		g_clear_object(&server);
		venture_test_remove_tree(state_dir);
		return;
	}
	if (!g_strcmp0(mode, "surfaces"))
	{
		g_autoptr(VentureWebServer) server = NULL;
		g_autofree gchar *state_dir = NULL;
		g_autofree gchar *path = NULL;
		g_autofree gchar *out = NULL;
		g_autofree gchar *id = g_strdup_printf("%" G_GINT64_FORMAT, venture_entity_get_id(invoice));
		g_autofree gchar *binary = g_build_filename("build", "debug", "venturectl", NULL);
		const gchar *argv[] = { binary, "--server", NULL, "invoice", "checkout", id, NULL };
		venture_context_set_stripe_service(f->context, service);
		server = start_server(f, &state_dir);
		path = g_strdup_printf("/api/v1/invoices/%s/checkout", id);
		g_assert_cmpuint(http_request(server, "POST", path, "application/json", "{}", &out), ==, 200);
		g_assert_nonnull(strstr(out, "https://checkout.stripe.com/offline"));
		g_clear_pointer(&out, g_free);
		argv[2] = venture_web_server_get_base_url(server);
		out = run_cli(argv, NULL, TRUE);
		g_assert_nonnull(strstr(out, "https://checkout.stripe.com/offline"));
		g_clear_pointer(&out, g_free);
		g_clear_pointer(&path, g_free);
		path = g_strdup_printf("/e/invoice/%s", id);
		g_assert_cmpuint(http_request(server, "GET", path, NULL, NULL, &out), ==, 200);
		g_assert_nonnull(strstr(out, "Pay with Stripe"));
		handler = g_signal_connect(venture_database_get_access_policy(f->database), "decide", G_CALLBACK(deny_stripe_event), NULL);
		g_object_set(f->config, "security-require-auth", TRUE, NULL);
		g_assert_cmpuint(http_request(server, "POST", "/webhooks/stripe", "application/json", "{}", NULL), ==, 400);
		g_assert_cmpuint(http_request(server, "POST", "/webhooks/stripe", "application/json",
			"{\"id\":\"evt_http\",\"type\":\"checkout.session.completed\",\"data\":{\"object\":{\"id\":\"cs_unknown\"}}}", NULL), ==, 200);
		g_assert_cmpuint(http_request(server, "POST", "/webhooks/stripe", "application/json",
			"{\"id\":\"evt_http\",\"type\":\"checkout.session.completed\",\"data\":{\"object\":{\"id\":\"cs_unknown\"}}}", NULL), ==, 200);
		g_signal_handler_disconnect(venture_database_get_access_policy(f->database), handler);
		g_object_set(f->config, "security-require-auth", FALSE, NULL);
		venture_config_set_module_enabled(f->config, "stripe", FALSE);
		g_clear_pointer(&out, g_free);
		g_assert_cmpuint(http_request(server, "GET", path, NULL, NULL, &out), ==, 200);
		g_assert_null(strstr(out, "Pay with Stripe"));
		venture_config_set_module_enabled(f->config, "stripe", TRUE);
		venture_web_server_stop(server);
		g_clear_object(&server);
		venture_test_remove_tree(state_dir);
		return;
	}
	checkout = venture_stripe_service_checkout(service, venture_entity_get_id(invoice), NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(checkout);
	key = g_strdup_printf("venture-invoice-%s-%" G_GINT64_FORMAT, venture_entity_get_uuid(invoice), venture_entity_get_version(invoice));
	g_assert_cmpstr(((FakeTransport *)transport)->key, ==, key);
	if (!g_strcmp0(mode, "customer-reuse"))
	{
		g_autoptr(VentureEntity) second = invoice_new(f, "second", "2026-01-01", "100 USD");
		g_autoptr(VentureStripeCheckout) second_checkout = venture_stripe_service_checkout(service, venture_entity_get_id(second), NULL, &error);
		g_assert_no_error(error);
		g_assert_nonnull(second_checkout);
		g_assert_cmpuint(((FakeTransport *)transport)->calls, ==, 3);
		return;
	}
	if (!g_strcmp0(mode, "module-off"))
	{
		venture_config_set_module_enabled(f->config, "stripe", FALSE);
		g_assert_false(venture_stripe_service_can_checkout(service, venture_entity_get_id(invoice), &error));
		g_assert_nonnull(error);
		venture_config_set_module_enabled(f->config, "stripe", TRUE);
		return;
	}
	if (!g_strcmp0(mode, "immutable"))
	{
		g_assert_false(venture_database_delete(f->database, VENTURE_ENTITY(checkout), NULL, &error));
		g_assert_nonnull(strstr(error->message, "VentureStripeService"));
		return;
	}
	if (!g_strcmp0(mode, "checkout"))
	{
		g_autoptr(VentureStripeCheckout) again = venture_stripe_service_checkout(service, venture_entity_get_id(invoice), NULL, &error);
		g_assert_no_error(error);
		g_assert_cmpuint(((FakeTransport *)transport)->calls, ==, 2);
		g_assert_cmpint(venture_entity_get_id(VENTURE_ENTITY(again)), ==, venture_entity_get_id(VENTURE_ENTITY(checkout)));
		return;
	}
	body = g_strdup_printf("{\"id\":\"evt_offline\",\"type\":\"checkout.session.completed\",\"data\":{\"object\":{\"id\":\"%s\",\"amount_total\":%" G_GINT64_FORMAT ",\"currency\":\"%s\",\"payment_status\":\"paid\",\"payment_intent\":\"pi_offline\"}}}",
		!g_strcmp0(mode, "unknown-session") ? "cs_unknown" : "cs_offline",
		!g_strcmp0(mode, "overflow-mismatch") ? G_MAXINT64 : (gint64)(!g_strcmp0(mode, "amount-mismatch") ? 9999 : 10000),
		!g_strcmp0(mode, "currency-mismatch") ? "eur" : "usd");
	bytes = g_bytes_new(body, strlen(body));
	sig = signature(body);
	if (!g_strcmp0(mode, "rollback"))
		handler = g_signal_connect(venture_database_get_posting_service(f->database), "posting", G_CALLBACK(fail_posting), NULL);
	audits_before = rows(f, "audit_entry");
	ok = venture_stripe_service_handle_webhook(service, bytes, !g_strcmp0(mode, "bad-signature") ? "t=0,v1=invalid" : sig, &error);
	if (handler) g_signal_handler_disconnect(venture_database_get_posting_service(f->database), handler);
	events = rows(f, "stripe_event");
	payments = rows(f, "payment");
	if (!g_strcmp0(mode, "bad-signature") || !g_strcmp0(mode, "rollback"))
	{
		g_assert_false(ok);
		g_assert_nonnull(error);
		g_assert_cmpuint(events->len, ==, 0);
		g_assert_cmpuint(payments->len, ==, 0);
		audits_after = rows(f, "audit_entry");
		g_assert_cmpuint(audits_after->len, ==, audits_before->len + (!g_strcmp0(mode, "bad-signature") ? 1 : 0));
		return;
	}
	g_assert_cmpuint(events->len, ==, 1);
	if (strstr(mode, "mismatch"))
	{
		g_assert_false(ok);
		g_assert_nonnull(strstr(error->message, "mismatch"));
		g_assert_cmpuint(payments->len, ==, 0);
		return;
	}
	g_assert_true(ok);
	g_assert_no_error(error);
	if (!g_strcmp0(mode, "unknown-session"))
	{
		g_autofree gchar *result = NULL;
		g_object_get(g_ptr_array_index(events, 0), "result", &result, NULL);
		g_assert_cmpstr(result, ==, "ignored");
		g_assert_cmpuint(payments->len, ==, 0);
		return;
	}
	g_assert_cmpuint(payments->len, ==, 1);
	{
		g_autofree gchar *method = NULL;
		g_autofree gchar *reference = NULL;
		g_object_get(g_ptr_array_index(payments, 0), "method", &method, "reference", &reference, NULL);
		g_assert_cmpstr(method, ==, "stripe");
		g_assert_cmpstr(reference, ==, "pi_offline");
	}

	balance = venture_settlement_service_invoice_balance(venture_settlement_service_get(f->database), venture_entity_get_id(invoice), NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpint(venture_money_get_amount(balance), ==, 0);
	g_clear_pointer(&audits_before, g_ptr_array_unref);
	audits_before = rows(f, "audit_entry");
	g_assert_true(venture_stripe_service_handle_webhook(service, bytes, sig, &error));
	g_assert_no_error(error);
	g_clear_pointer(&payments, g_ptr_array_unref);
	payments = rows(f, "payment");
	g_assert_cmpuint(payments->len, ==, 1);
	audits_after = rows(f, "audit_entry");
	g_assert_cmpuint(audits_after->len, ==, audits_before->len);
}

/* Provider calls cannot be rolled back when local authorization later fails. */
static GError *deny_invoice_write(VentureAccessPolicy *policy, const VentureAuthPrincipal *actor,
	const gchar *action, VentureEntity *entity, gpointer unused)
{
	if (VENTURE_IS_INVOICE(entity) && !g_strcmp0(action, "write"))
		return g_error_new_literal(VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED, "permission denied");
	return NULL;
}

static void
test_eligibility(Fixture *f, gconstpointer data)
{
	const gchar *rule = data;
	g_autoptr(VentureAccessScope) access = NULL;
	VentureAuthPrincipal principal = { 0 };
	gulong policy_handler = 0;
	g_autoptr(VentureEntity) invoice = record_new(f, "invoice");
	g_autoptr(VentureEntity) price = record_new(f, "stripe_price_link");
	g_autoptr(VentureStripeService) service = NULL;
	g_autoptr(VentureStripeCheckout) checkout = NULL;
	g_autoptr(GObject) transport = g_object_new(fake_transport_get_type(), NULL);
	g_autoptr(GError) error = NULL;
	guint i, count = !g_strcmp0(rule, "exactly one") ? 2 : 1;

	service = venture_stripe_service_new(f->database, f->organization_id, STRIPE_TRANSPORT(transport), &error);
	g_assert_no_error(error);
	g_object_set(invoice, "number", "eligibility", "company-id", f->customer_id, NULL);
	money_field(invoice, "issued-at", "2026-01-01");
	save(f, invoice);
	for (i = 0; i < count; i++)
	{
		g_autoptr(VentureEntity) line = record_new(f, "invoice_line");
		g_object_set(line, "invoice-id", venture_entity_get_id(invoice), "product-id", f->product_id,
			"description", "Work", "quantity", !g_strcmp0(rule, "integral") ? 0.5 : 1.0, NULL);
		money_field(line, "unit-price", "100 USD");
		save(f, line);
	}
	if (g_strcmp0(rule, "status sent"))
	{
		g_object_set(invoice, "status", VENTURE_INVOICE_STATUS_SENT, NULL);
		save(f, invoice);
	}
	if (g_strcmp0(rule, "stripe_price_link"))
	{
		g_object_set(price, "product-id", f->product_id, "stripe-price-id", "price_offline", NULL);
		save(f, price);
	}
	if (!g_strcmp0(rule, "organization"))
	{
		g_clear_object(&service);
		service = venture_stripe_service_new(f->database, f->organization_id + 1, STRIPE_TRANSPORT(transport), &error);
		g_assert_no_error(error);
	}
	if (!g_strcmp0(rule, "open balance"))
	{
		/* Damaged imported issue evidence must not let a catalog amount
		 * replace the authoritative balance. Normal writers freeze it. */
		g_assert_true(orm_connection_execute(venture_database_get_connection(f->database),
			"UPDATE invoice_events SET amount_amount = 0", &error));
		g_assert_no_error(error);
	}
	if (!g_strcmp0(rule, "deleted"))
	{
		g_autoptr(VentureEntity) customer = venture_database_get(f->database, VENTURE_TYPE_COMPANY, f->customer_id, &error);
		g_assert_no_error(error);
		g_assert_true(venture_database_delete(f->database, customer, NULL, &error));
		g_assert_no_error(error);
	}
	if (!g_strcmp0(rule, "permission"))
	{
		VentureAccessPolicy *policy = venture_database_get_access_policy(f->database);
		principal.authenticated = TRUE;
		principal.role = VENTURE_USER_ROLE_OWNER;
		access = venture_access_policy_enter(policy, &principal);
		policy_handler = g_signal_connect(policy, "decide", G_CALLBACK(deny_invoice_write), NULL);
	}
	checkout = venture_stripe_service_checkout(service, venture_entity_get_id(invoice), NULL, &error);
	g_assert_null(checkout);
	g_assert_nonnull(error);
	g_assert_nonnull(strstr(error->message, rule));
	g_assert_cmpuint(((FakeTransport *)transport)->calls, ==, 0);
	if (policy_handler) g_signal_handler_disconnect(venture_database_get_access_policy(f->database), policy_handler);
}

/* An invalid legacy catalog must preserve its rows and close its migration
 * transaction; otherwise startup replaces the useful error with a warning. */
static void
test_price_migration_rollback(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *sql = g_strdup_printf(
		"DROP INDEX uq_stripe_price_links_organization_product_id;"
		"INSERT INTO stripe_price_links (uuid, organization_id, product_id, stripe_price_id) VALUES ('legacy-price-a', %" G_GINT64_FORMAT ", %" G_GINT64_FORMAT ", 'price_a'), ('legacy-price-b', %" G_GINT64_FORMAT ", %" G_GINT64_FORMAT ", 'price_b')",
		f->organization_id, f->product_id, f->organization_id, f->product_id);
	g_autoptr(OrmResult) rows = NULL;
	g_assert_true(venture_database_execute(f->database, sql, NULL, &error));
	g_assert_no_error(error);
	g_assert_false(venture_period_constraints_migrate(venture_database_get_connection(f->database), venture_stripe_price_link_get_type(), &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_MIGRATION);
	g_clear_error(&error);
	g_assert_false(orm_connection_in_transaction(venture_database_get_connection(f->database)));
	rows = venture_database_query_raw(f->database, "SELECT CAST(COUNT(*) AS BIGINT) FROM stripe_price_links", NULL, &error);
	g_assert_no_error(error);
	g_assert_true(orm_result_next(rows));
	g_assert_cmpint(orm_row_get_integer(orm_result_get_row(rows), 0), ==, 2);
}

static void
test_no_keys(void)
{
	const gchar *argv[] = { "git", "grep", "-l", "-E", "(sk|rk)_(live|test)_[A-Za-z0-9]+", "--", "src", "data", "docs", NULL };
	g_autoptr(GError) error = NULL;
	g_autofree gchar *out = NULL;
	gint status;
	g_assert_true(g_spawn_sync(NULL, (gchar **)argv, NULL, G_SPAWN_SEARCH_PATH,
		NULL, NULL, &out, NULL, &status, &error));
	g_assert_no_error(error);
	g_assert_cmpstr(out, ==, "");
	g_assert_false(g_spawn_check_wait_status(status, NULL));
}

static void
test_customer_link_owner(void)
{
	g_autoptr(VentureStripeCustomerLink) link = venture_stripe_customer_link_new();
	g_autoptr(GError) error = NULL;
	g_object_set(link, "stripe-customer-id", "cus_offline", NULL);
	g_assert_false(venture_entity_before_save(VENTURE_ENTITY(link), &error));
	g_assert_nonnull(error);
	g_clear_error(&error);
	g_object_set(link, "company-id", (gint64)1, "contact-id", (gint64)2, NULL);
	g_assert_false(venture_entity_before_save(VENTURE_ENTITY(link), &error));
	g_assert_nonnull(error);
	g_clear_error(&error);
	g_object_set(link, "contact-id", (gint64)0, NULL);
	g_assert_true(venture_entity_before_save(VENTURE_ENTITY(link), &error));
	g_assert_no_error(error);
}

static void
test_price_uniqueness(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) first = record_new(f, "stripe_price_link");
	g_autoptr(VentureEntity) duplicate = record_new(f, "stripe_price_link");
	g_autoptr(VentureEntity) other_product = record_new(f, "product");
	g_autoptr(VentureEntity) other_org = record_new(f, "organization");
	g_autoptr(GError) error = NULL;
	(void)data;
	g_object_set(first, "product-id", f->product_id, "stripe-price-id", "price_shared", NULL);
	save(f, first);
	g_object_set(other_product, "name", "Second", NULL);
	save(f, other_product);
	g_object_set(duplicate, "product-id", venture_entity_get_id(other_product), "stripe-price-id", "price_shared", NULL);
	g_assert_false(venture_database_save(f->database, duplicate, NULL, &error));
	g_assert_nonnull(error);
	g_clear_error(&error);
	g_object_set(other_org, "name", "Second organization", "slug", "stripe-second", NULL);
	save(f, other_org);
	venture_entity_set_organization_id(other_product, venture_entity_get_id(other_org));
	save(f, other_product);
	venture_entity_set_organization_id(duplicate, venture_entity_get_id(other_org));
	save(f, duplicate);
}

static void
test_dependency_pin(void)
{
	const gchar *argv[] = { "git", "-C", "deps/stripe-glib", "rev-parse", "--show-toplevel", NULL };
	g_autofree gchar *out = NULL;
	g_autoptr(GError) error = NULL;
	gint status;

	if (!g_file_test("deps/stripe-glib/.git", G_FILE_TEST_EXISTS))
		return;
	g_assert_true(g_spawn_sync(NULL, (gchar **)argv, NULL, G_SPAWN_SEARCH_PATH,
		NULL, NULL, &out, NULL, &status, &error));
	g_assert_no_error(error);
	g_assert_true(g_spawn_check_wait_status(status, &error));
	g_clear_pointer(&out, g_free);
	{
		const gchar *head[] = { "git", "-C", "deps/stripe-glib", "rev-parse", "HEAD", NULL };
		g_assert_true(g_spawn_sync(NULL, (gchar **)head, NULL, G_SPAWN_SEARCH_PATH,
			NULL, NULL, &out, NULL, &status, &error));
		g_assert_cmpstr(g_strstrip(out), ==, "e32797673f0e7780dede9ed23260a673d2936b7f");
	}
}

static void
test_module_start(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureModuleRegistry) modules = venture_module_registry_new();
	(void)data;
	g_unsetenv("VENTURE_STRIPE_SECRET_KEY");
	g_assert_false(venture_context_start_stripe(f->context, &error));
	g_assert_nonnull(strstr(error->message, "VENTURE_STRIPE_SECRET_KEY"));
	g_clear_error(&error);
	g_setenv("VENTURE_STRIPE_SECRET_KEY", "offline", TRUE);
	g_assert_true(venture_context_start_stripe(f->context, &error));
	g_assert_no_error(error);
	g_assert_nonnull(venture_context_get_stripe_service(f->context));
	g_assert_cmpuint(g_type_from_name("StripeBundledYamlParser"), ==, G_TYPE_INVALID);
	g_assert_cmpuint(g_type_from_name("YamlParser"), !=, G_TYPE_INVALID);
	venture_module_registry_register_builtins(modules);
	venture_config_set_module_enabled(f->config, "receivables", FALSE);
	g_assert_false(venture_module_registry_configure(modules, f->config, &error));
	g_assert_nonnull(error);
	venture_config_set_module_enabled(f->config, "receivables", TRUE);
}

/* A linked remote Price must charge exactly the local invoice, including
 * quantity and API currency exponents. Every refusal precedes remote writes. */
static void
test_remote_price(Fixture *f, gconstpointer data)
{
	const gchar *mode = data;
	const gchar *currency = "USD";
	const gchar *unit_text = "100.0000 USD";
	gboolean success = !g_strcmp0(mode, "exact") || !g_strcmp0(mode, "quantity") ||
		!g_strcmp0(mode, "JPY") || !g_strcmp0(mode, "ISK") || !g_strcmp0(mode, "UGX") || !g_strcmp0(mode, "MGA");
	gint64 remote_amount = 10000;
	g_autoptr(VentureEntity) invoice = record_new(f, "invoice");
	g_autoptr(VentureEntity) line = record_new(f, "invoice_line");
	g_autoptr(VentureEntity) price = record_new(f, "stripe_price_link");
	g_autoptr(GObject) transport = g_object_new(fake_transport_get_type(), NULL);
	FakeTransport *fake = (FakeTransport *)transport;
	g_autoptr(VentureStripeService) service = NULL;
	g_autoptr(VentureStripeCheckout) checkout = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *json = NULL;
	g_autofree gchar *amount_text = NULL;

	if (!g_strcmp0(mode, "JPY") || !g_strcmp0(mode, "MGA")) remote_amount = 100;
	if (!g_strcmp0(mode, "JPY") || !g_strcmp0(mode, "ISK") || !g_strcmp0(mode, "UGX") || !g_strcmp0(mode, "MGA"))
	{
		currency = mode;
		amount_text = g_strdup_printf("100 %s", currency);
		unit_text = amount_text;
	}
	if (!g_strcmp0(mode, "amount")) remote_amount = 10001;
	if (!g_strcmp0(mode, "overflow")) remote_amount = G_MAXINT64;
	g_object_set(invoice, "number", "remote-price", "company-id", f->customer_id, NULL);
	money_field(invoice, "issued-at", "2026-01-01");
	save(f, invoice);
	g_object_set(line, "invoice-id", venture_entity_get_id(invoice), "product-id", f->product_id,
		"description", "Price validation", "quantity", (!g_strcmp0(mode, "quantity") || !g_strcmp0(mode, "overflow")) ? 2.0 : 1.0, NULL);
	money_field(line, "unit-price", unit_text);
	if (!g_strcmp0(mode, "discount")) g_object_set(line, "discount-percent", (gint64)1, NULL);
	if (!g_strcmp0(mode, "tax")) g_object_set(line, "tax-percent", (gint64)1, NULL);
	save(f, line);
	g_object_set(invoice, "status", VENTURE_INVOICE_STATUS_SENT, NULL);
	save(f, invoice);
	g_object_set(price, "product-id", f->product_id, "stripe-price-id", "price_offline", NULL);
	save(f, price);
	json = g_strdup_printf("{\"id\":\"price_offline\",\"object\":\"price\",\"active\":%s,\"type\":\"%s\",\"billing_scheme\":\"%s\",\"unit_amount\":%" G_GINT64_FORMAT ",\"currency\":\"%s\",\"transform_quantity\":%s}",
		!g_strcmp0(mode, "inactive") ? "false" : "true", !g_strcmp0(mode, "recurring") ? "recurring" : "one_time",
		!g_strcmp0(mode, "tiered") ? "tiered" : "per_unit", remote_amount, !g_strcmp0(mode, "currency") ? "EUR" : currency,
		!g_strcmp0(mode, "transform") ? "{\"divide_by\":10,\"round\":\"up\"}" : "null");
	fake->price_json = !g_strcmp0(mode, "decimal") ? "{\"id\":\"price_offline\",\"object\":\"price\",\"active\":true,\"type\":\"one_time\",\"billing_scheme\":\"per_unit\",\"unit_amount\":null,\"unit_amount_decimal\":\"10000.5\",\"currency\":\"usd\"}" : json;
	service = venture_stripe_service_new(f->database, f->organization_id, STRIPE_TRANSPORT(transport), &error);
	g_assert_no_error(error);
	checkout = venture_stripe_service_checkout(service, venture_entity_get_id(invoice), NULL, &error);
	/* Ordinary invoices charge the open balance via Checkout price_data;
	 * a catalog Price is not required and cannot replace remaining. */
	g_assert_no_error(error);
	g_assert_nonnull(checkout);
	g_assert_cmpuint(fake->customers, ==, 1);
	g_assert_cmpuint(fake->checkouts, ==, 1);
	if (success)
	{
		g_autofree gchar *body = g_strdup_printf("{\"id\":\"evt_units\",\"type\":\"checkout.session.completed\",\"data\":{\"object\":{\"id\":\"cs_offline\",\"amount_total\":%" G_GINT64_FORMAT ",\"currency\":\"%s\",\"payment_status\":\"paid\",\"payment_intent\":\"pi_units\"}}}", remote_amount * (!g_strcmp0(mode, "quantity") ? 2 : 1), currency);
		g_autofree gchar *sig = signature(body);
		g_autoptr(GBytes) bytes = g_bytes_new(body, strlen(body));
		g_autoptr(VentureMoney) balance = NULL;
		g_assert_true(venture_stripe_service_handle_webhook(service, bytes, sig, &error));
		g_assert_no_error(error);
		balance = venture_settlement_service_invoice_balance(venture_settlement_service_get(f->database), venture_entity_get_id(invoice), NULL, &error);
		g_assert_no_error(error);
		g_assert_cmpint(venture_money_get_amount(balance), ==, 0);
	}
}

static gint64
account_code(Fixture *f, const gchar *code)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ACCOUNT);
	g_autoptr(VentureEntity) account = NULL;
	venture_query_set_organization(query, f->organization_id);
	g_assert_true(venture_query_add_filter_string(query, "code", VENTURE_FILTER_OP_EQ, code, NULL));
	account = venture_database_find_one(f->database, query, NULL);
	g_assert_nonnull(account);
	return venture_entity_get_id(account);
}

/* Payouts clear gross receipts net of fees; a lost dispute reverses through settlement. */
static void
test_payout_dispute_chargeback(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureStripeService) service = NULL;
	g_autoptr(GObject) fake = g_object_new(fake_transport_get_type(), NULL);
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(VentureStripeCheckout) checkout = NULL;
	g_autoptr(VentureEntity) payout = NULL;
	g_autoptr(VentureEntity) dispute = NULL;
	g_autoptr(VentureMoney) gross = venture_money_new_for_currency(10000, "USD");
	g_autoptr(VentureMoney) fee = venture_money_new_for_currency(300, "USD");
	g_autoptr(VentureMoney) net = venture_money_new_for_currency(9700, "USD");
	g_autoptr(VentureMoney) disputed = venture_money_new_for_currency(3000, "USD");
	g_autoptr(GDateTime) date = NULL;
	g_autoptr(VentureMoney) balance = NULL;
	g_autoptr(GPtrArray) payments = NULL;
	gint64 payment_id;
	(void)data;
	environment();
	service = venture_stripe_service_new(f->database, f->organization_id, STRIPE_TRANSPORT(fake), &error);
	g_assert_no_error(error);
	invoice = invoice_new(f, "PROC-1", "2026-01-10", "100 USD");
	{
		g_autoptr(VentureEntity) link = record_new(f, "stripe_price_link");
		g_object_set(link, "product-id", f->product_id, "stripe-price-id", "price_offline", NULL);
		save(f, link);
	}
	checkout = venture_stripe_service_checkout(service, venture_entity_get_id(invoice), NULL, &error);
	g_assert_no_error(error);
	{
		const gchar *body = "{\"id\":\"evt_pay\",\"type\":\"checkout.session.completed\",\"data\":{\"object\":{\"id\":\"cs_offline\",\"amount_total\":10000,\"currency\":\"usd\",\"payment_status\":\"paid\",\"payment_intent\":\"pi_payout\"}}}";
		g_autofree gchar *sig = signature(body);
		g_autoptr(GBytes) bytes = g_bytes_new(body, strlen(body));
		g_assert_true(venture_stripe_service_handle_webhook(service, bytes, sig, &error));
		g_assert_no_error(error);
	}
	payments = rows(f, "payment");
	g_assert_cmpuint(payments->len, ==, 1);
	payment_id = venture_entity_get_id(g_ptr_array_index(payments, 0));
	g_object_get(g_ptr_array_index(payments, 0), "date", &date, NULL);
	g_assert_nonnull(date);
	payout = VENTURE_ENTITY(venture_stripe_service_record_payout(service, "po_test", date, gross, fee, net,
		account_code(f, "1000"), NULL, &error));
	g_assert_no_error(error);
	g_assert_nonnull(payout);
	g_assert_true(venture_stripe_service_link_payout_item(service, venture_entity_get_id(payout), payment_id, gross, NULL, &error));
	dispute = VENTURE_ENTITY(venture_stripe_service_open_dispute(service, "dp_test", payment_id, date, disputed, NULL, &error));
	g_assert_no_error(error);
	{
		g_autoptr(VentureEntity) stored = venture_database_get(f->database, VENTURE_TYPE_INVOICE, venture_entity_get_id(invoice), &error);
		g_autofree gchar *state = NULL;
		g_object_get(stored, "workflow-state", &state, NULL);
		g_assert_cmpstr(state, ==, "disputed");
	}
	g_assert_true(venture_stripe_service_lose_chargeback(service, venture_entity_get_id(dispute), date, NULL, &error));
	g_assert_no_error(error);
	/* Replaying a partial loss must not refund another portion of the receipt. */
	g_assert_false(venture_stripe_service_lose_chargeback(service, venture_entity_get_id(dispute), date, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	balance = venture_settlement_service_invoice_balance(venture_settlement_service_get(f->database),
		venture_entity_get_id(invoice), NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpint(venture_money_get_amount(balance), ==, 3000);
	g_assert_cmpuint(rows(f, "refund")->len, ==, 1);
	g_assert_cmpuint(rows(f, "processor_exception")->len, ==, 0);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/stripe/records", test_records);
	g_test_add_func("/stripe/no-keys", test_no_keys);
	g_test_add_func("/stripe/customer-link-owner", test_customer_link_owner);
	g_test_add_func("/stripe/dependency-pin", test_dependency_pin);
	g_test_add("/stripe/price-uniqueness", Fixture, NULL, set_up, test_price_uniqueness, tear_down);
	g_test_add("/stripe/module-start", Fixture, NULL, set_up, test_module_start, tear_down);

	{
		static const gchar *const rules[] = { "status sent", "organization", "open balance", "deleted", "permission" };
		guint i;
		for (i = 0; i < G_N_ELEMENTS(rules); i++)
		{
			g_autofree gchar *path = g_strdup_printf("/stripe/eligibility/%u", i);
			g_test_add(path, Fixture, rules[i], set_up, test_eligibility, tear_down);
		}
	}

	g_test_add_func("/stripe/missing-key", test_missing_key);
	{
		static const gchar *const cases[] = { "checkout", "portal", "customer-reuse", "surfaces", "module-off", "immutable", "completed-duplicate", "unknown-session", "amount-mismatch", "precision-mismatch", "overflow-mismatch", "currency-mismatch", "bad-signature", "rollback" };
		guint i;
		for (i = 0; i < G_N_ELEMENTS(cases); i++)
		{
			g_autofree gchar *path = g_strconcat("/stripe/", cases[i], NULL);
			g_test_add(path, Fixture, cases[i], set_up, test_flow, tear_down);
		}
	}
	{
		static const gchar *const cases[] = { "exact", "quantity", "amount", "currency", "inactive", "recurring", "tiered", "transform", "decimal", "overflow", "discount", "tax", "JPY", "ISK", "UGX", "MGA" };
		guint i;
		for (i = 0; i < G_N_ELEMENTS(cases); i++)
		{
			g_autofree gchar *path = g_strconcat("/stripe/remote-price/", cases[i], NULL);
			g_test_add(path, Fixture, cases[i], set_up, test_remote_price, tear_down);
		}
	}
	environment();
	g_test_add("/stripe/price-migration-rollback", Fixture, NULL, set_up, test_price_migration_rollback, tear_down);
	g_test_add("/stripe/payout-dispute-chargeback", Fixture, NULL, set_up, test_payout_dispute_chargeback, tear_down);
	return g_test_run();
}
