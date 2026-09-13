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
		"stripe_price_link", "stripe_customer_link", "stripe_checkout", "stripe_event"
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

typedef struct { GObject parent; guint calls; guint checkouts; gchar *key; } FakeTransport;
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
	if (g_str_has_suffix(request->url, "/customers"))
	{
		g_assert_nonnull(strstr(request->body, "venture_company_uuid"));
		return stripe_response_new(200, "{\"id\":\"cus_offline\",\"object\":\"customer\"}", NULL, NULL);
	}
	g_assert_nonnull(strstr(request->body, "price_offline"));
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
	invoice = invoice_new(f, "stripe-test", "2026-01-01", (!g_strcmp0(mode, "precision-mismatch") || !g_strcmp0(mode, "overflow-mismatch")) ? "1.0000 USD" : "100 USD");
	price = record_new(f, "stripe_price_link");
	g_object_set(price, "product-id", f->product_id, "stripe-price-id", "price_offline", NULL);
	save(f, price);
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
		g_object_set(f->config, "security-require-auth", TRUE, NULL);
		g_assert_cmpuint(http_request(server, "POST", "/webhooks/stripe", "application/json", "{}", NULL), ==, 400);
		g_assert_cmpuint(http_request(server, "POST", "/webhooks/stripe", "application/json",
			"{\"id\":\"evt_http\",\"type\":\"checkout.session.completed\",\"data\":{\"object\":{\"id\":\"cs_unknown\"}}}", NULL), ==, 200);
		g_assert_cmpuint(http_request(server, "POST", "/webhooks/stripe", "application/json",
			"{\"id\":\"evt_http\",\"type\":\"checkout.session.completed\",\"data\":{\"object\":{\"id\":\"cs_unknown\"}}}", NULL), ==, 200);
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

static void
test_eligibility(Fixture *f, gconstpointer data)
{
	const gchar *rule = data;
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
			"UPDATE invoice_events SET amount_amount = 9999", &error));
		g_assert_no_error(error);
	}
	if (!g_strcmp0(rule, "deleted"))
	{
		g_autoptr(VentureEntity) customer = venture_database_get(f->database, VENTURE_TYPE_COMPANY, f->customer_id, &error);
		g_assert_no_error(error);
		g_assert_true(venture_database_delete(f->database, customer, NULL, &error));
		g_assert_no_error(error);
	}
	checkout = venture_stripe_service_checkout(service, venture_entity_get_id(invoice), NULL, &error);
	g_assert_null(checkout);
	g_assert_nonnull(error);
	g_assert_nonnull(strstr(error->message, rule));
	g_assert_cmpuint(((FakeTransport *)transport)->calls, ==, 0);
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
	const gchar *argv[] = { "git", "-C", "deps/stripe-glib", "rev-parse", "HEAD", NULL };
	g_autofree gchar *out = NULL;
	g_autoptr(GError) error = NULL;
	gint status;
	g_assert_true(g_spawn_sync(NULL, (gchar **)argv, NULL, G_SPAWN_SEARCH_PATH,
		NULL, NULL, &out, NULL, &status, &error));
	g_assert_no_error(error);
	g_assert_true(g_spawn_check_wait_status(status, &error));
	g_assert_cmpstr(g_strstrip(out), ==, "669dfbb688794187319be86526a666b0c1ea1d59");
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
		static const gchar *const rules[] = { "status sent", "exactly one", "stripe_price_link", "integral", "organization", "open balance", "deleted" };
		guint i;
		for (i = 0; i < G_N_ELEMENTS(rules); i++)
		{
			g_autofree gchar *path = g_strdup_printf("/stripe/eligibility/%u", i);
			g_test_add(path, Fixture, rules[i], set_up, test_eligibility, tear_down);
		}
	}

	g_test_add_func("/stripe/missing-key", test_missing_key);
	{
		static const gchar *const cases[] = { "checkout", "customer-reuse", "surfaces", "module-off", "immutable", "completed-duplicate", "unknown-session", "amount-mismatch", "precision-mismatch", "overflow-mismatch", "currency-mismatch", "bad-signature", "rollback" };
		guint i;
		for (i = 0; i < G_N_ELEMENTS(cases); i++)
		{
			g_autofree gchar *path = g_strconcat("/stripe/", cases[i], NULL);
			g_test_add(path, Fixture, cases[i], set_up, test_flow, tear_down);
		}
	}
	environment();
	return g_test_run();
}
