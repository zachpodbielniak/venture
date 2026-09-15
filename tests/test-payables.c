/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <string.h>
#include <libsoup/soup.h>
#include "venture-test-util.h"
#include "venture-test-accounting.h"

static void
test_records(void)
{
	static const gchar *const names[] = {
		"vendor_bill", "vendor_bill_line", "bill_payment",
		"bill_payment_allocation", "vendor_credit", "vendor_bill_event"
	};
	guint i;

	/* Every generated surface must discover the same financial records. */
	for (i = 0; i < G_N_ELEMENTS(names); i++)
		g_assert_cmpuint(venture_entity_registry_lookup(
			venture_entity_registry_get_default(), names[i]), !=, G_TYPE_INVALID);
}


typedef struct
{
	VentureDatabase *db;
	VentureContext *context;
	VentureConfig *config;
	gint64 org;
	gint64 vendor;
} Fixture;

static void
save(Fixture *f, VentureEntity *e)
{
	g_autoptr(GError) error = NULL;
	gboolean ok = venture_database_save(f->db, e, NULL, &error);
	g_assert_no_error(error);
	g_assert_true(ok);
}

static void
field(VentureEntity *e, const gchar *name, const gchar *value)
{
	g_autoptr(GError) error = NULL;
	g_assert_true(venture_entity_set_field_from_string(e, name, value, &error));
	g_assert_no_error(error);
}

static VentureEntity *
record(Fixture *f, const gchar *name)
{
	VentureEntity *e = venture_entity_registry_create(venture_entity_registry_get_default(), name, NULL);
	g_assert_nonnull(e);
	venture_entity_set_organization_id(e, f->org);
	return e;
}

static void
setup(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) vendor = NULL;
	f->config = venture_config_new();
	f->db = venture_test_accounting_database(&error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->db);
	f->org = venture_context_get_default_organization_id(f->context);
	vendor = record(f, "company");
	field(vendor, "name", "Supplier");
	field(vendor, "kind", "supplier");
	save(f, vendor);
	f->vendor = venture_entity_get_id(vendor);
}

static void
teardown(Fixture *f, gconstpointer unused)
{
	g_clear_object(&f->context);
	venture_test_accounting_database_cleanup(f->db);
	g_clear_object(&f->db);
	g_clear_object(&f->config);
}

static VentureEntity *
bill(Fixture *f, const gchar *number)
{
	VentureEntity *e = record(f, "vendor_bill");
	g_autoptr(VentureEntity) line = NULL;
	g_object_set(e, "number", number, "company-id", f->vendor,
		"currency", "USD", "status", "draft", NULL);
	field(e, "bill-date", "2026-01-01");
	field(e, "due-date", "2026-01-31");
	save(f, e);
	line = record(f, "vendor_bill_line");
	g_object_set(line, "bill-id", venture_entity_get_id(e),
		"description", "Supplies", "quantity", "2.5", "category", "supplies", NULL);
	field(line, "unit-price", "36 USD");
	field(line, "tax-amount", "10 USD");
	save(f, line);
	return e;
}

static VentureEntity *
event(Fixture *f, VentureEntity *b, const gchar *kind, const gchar *date)
{
	VentureEntity *e = record(f, "vendor_bill_event");
	g_object_set(e, "bill-id", venture_entity_get_id(b), "vendor-id", f->vendor,
		"kind", kind, "state", "approved", NULL);
	field(e, "date", date);
	return e;
}

static void
approve(Fixture *f, VentureEntity *b)
{
	g_autoptr(VentureEntity) e = event(f, b, "approve", "2026-01-01");
	save(f, e);
}

static void
status(Fixture *f, VentureEntity *b, const gchar *expected)
{
	g_autoptr(VentureEntity) stored = venture_database_get(f->db, G_OBJECT_TYPE(b), venture_entity_get_id(b), NULL);
	g_autofree gchar *actual = NULL;
	g_assert_nonnull(stored);
	g_object_get(stored, "status", &actual, NULL);
	g_assert_cmpstr(actual, ==, expected);
}

static gint64
count(Fixture *f, const gchar *name)
{
	g_autoptr(VentureQuery) q = venture_query_new(venture_entity_registry_lookup(venture_entity_registry_get_default(), name));
	venture_query_set_organization(q, f->org);
	return venture_database_count(f->db, q, NULL);
}

static VentureEntity *
payment(Fixture *f, VentureEntity *b, const gchar *amount, const gchar *date)
{
	VentureEntity *e = record(f, "bill_payment");
	g_object_set(e, "vendor-id", f->vendor, "bill-id", venture_entity_get_id(b), "method", "transfer", NULL);
	field(e, "amount", amount);
	field(e, "date", date);
	return e;
}

static void
test_state_guard(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) b = bill(f, "GUARD");
	g_autoptr(GError) error = NULL;
	g_object_set(b, "status", "paid", NULL);
	g_assert_false(venture_database_save(f->db, b, NULL, &error));
	g_assert_nonnull(error);
	g_assert_nonnull(strstr(error->message, "VenturePayablesService"));
	status(f, b, "draft");
}

/* A secondary organization's chart is namespaced by the core posting rules.
 * Approval must reuse it or the same payable balance splits across accounts. */
static void
test_existing_namespaced_chart(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) organization = record(f, "organization");
	g_autoptr(VentureEntity) vendor = NULL;
	g_autoptr(VentureEntity) b = NULL;
	const gchar *codes[] = { "2000", "6900" };
	guint i;
	field(organization, "name", "Secondary organization");
	save(f, organization);
	f->org = venture_entity_get_id(organization);
	vendor = record(f, "company");
	field(vendor, "name", "Secondary supplier");
	field(vendor, "kind", "supplier");
	save(f, vendor);
	f->vendor = venture_entity_get_id(vendor);
	for (i = 0; i < G_N_ELEMENTS(codes); i++)
	{
		g_autoptr(VentureEntity) account = record(f, "account");
		g_autofree gchar *code = g_strdup_printf("%" G_GINT64_FORMAT ":%s", f->org, codes[i]);
		g_object_set(account, "code", code, "name", codes[i], "active", TRUE, NULL);
		field(account, "kind", i == 0 ? "liability" : "expense");
		save(f, account);
	}
	b = bill(f, "SCOPED");
	approve(f, b);
	status(f, b, "approved");
	g_assert_cmpint(count(f, "account"), ==, 2);
}

static gint64
payables_balance(Fixture *f, const gchar *code, const gchar *cutoff)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ACCOUNT);
	g_autoptr(GPtrArray) found = NULL;
	g_autoptr(GDateTime) date = venture_time_from_string(cutoff, NULL);
	g_autoptr(VentureMoney) balance = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *scoped = g_strdup_printf("%" G_GINT64_FORMAT ":%s", f->org, code);
	VentureEntity *account = NULL;
	guint i;

	venture_query_set_limit(query, 0);
	found = venture_database_find(f->db, query, &error);
	g_assert_no_error(error);
	g_assert_nonnull(found);
	for (i = 0; i < found->len; i++)
	{
		g_autofree gchar *actual = NULL;
		g_object_get(g_ptr_array_index(found, i), "code", &actual, NULL);
		if (g_strcmp0(actual, code) == 0 || g_strcmp0(actual, scoped) == 0)
			account = g_ptr_array_index(found, i);
	}
	g_assert_nonnull(account);
	balance = venture_posting_service_account_balance(venture_database_get_posting_service(f->db),
		venture_entity_get_id(account), f->org, "USD", date, &error);
	g_assert_no_error(error);
	return venture_money_get_amount(balance);
}

/* Recoverable purchase tax is an asset, not extra expense. Non-recoverable
 * tax remains in the expense cost, matching the historical bill line. */
static void
test_recoverable_tax(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) recoverable = record(f, "tax_code");
	g_autoptr(VentureEntity) consumed = record(f, "tax_code");
	g_autoptr(VentureEntity) billed = record(f, "vendor_bill");
	g_autoptr(VentureEntity) other = record(f, "vendor_bill");
	g_autoptr(VentureEntity) recover_line = NULL;
	g_autoptr(VentureEntity) consume_line = NULL;

	(void)unused;
	g_object_set(recoverable, "code", "VAT-R", "name", "Recoverable VAT",
		"jurisdiction", "EU", "rate-numerator", (gint64)10, "rate-denominator", (gint64)100,
		"recoverable", TRUE, "active", TRUE, NULL);
	save(f, recoverable);
	g_object_set(consumed, "code", "VAT-N", "name", "Non-recoverable VAT",
		"jurisdiction", "EU", "rate-numerator", (gint64)10, "rate-denominator", (gint64)100,
		"recoverable", FALSE, "active", TRUE, NULL);
	save(f, consumed);
	g_object_set(billed, "number", "VAT-REC", "company-id", f->vendor,
		"currency", "USD", "status", "draft", NULL);
	field(billed, "bill-date", "2026-01-01");
	save(f, billed);
	recover_line = record(f, "vendor_bill_line");
	g_object_set(recover_line, "bill-id", venture_entity_get_id(billed),
		"description", "Services", "quantity", "1",
		"tax-code-id", venture_entity_get_id(recoverable), NULL);
	field(recover_line, "unit-price", "100 USD");
	field(recover_line, "tax-amount", "10 USD");
	save(f, recover_line);
	approve(f, billed);
	status(f, billed, "approved");
	g_assert_cmpint(payables_balance(f, "2000", "2026-01-01T23:59:59Z"), ==, -11000);
	g_assert_cmpint(payables_balance(f, "6900", "2026-01-01T23:59:59Z"), ==, 10000);
	g_assert_cmpint(payables_balance(f, "1300", "2026-01-01T23:59:59Z"), ==, 1000);
	g_object_set(other, "number", "VAT-EXP", "company-id", f->vendor,
		"currency", "USD", "status", "draft", NULL);
	field(other, "bill-date", "2026-01-02");
	save(f, other);
	consume_line = record(f, "vendor_bill_line");
	g_object_set(consume_line, "bill-id", venture_entity_get_id(other),
		"description", "Meals", "quantity", "1",
		"tax-code-id", venture_entity_get_id(consumed), NULL);
	field(consume_line, "unit-price", "100 USD");
	field(consume_line, "tax-amount", "10 USD");
	save(f, consume_line);
	{
		g_autoptr(VentureEntity) e = event(f, other, "approve", "2026-01-02");
		save(f, e);
	}
	g_assert_cmpint(payables_balance(f, "6900", "2026-01-02T23:59:59Z"), ==, 21000);
	g_assert_cmpint(payables_balance(f, "1300", "2026-01-02T23:59:59Z"), ==, 1000);
	g_assert_cmpint(payables_balance(f, "2000", "2026-01-02T23:59:59Z"), ==, -22000);
}

static void
test_approval(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) b = bill(f, "APPROVE");
	g_autoptr(VentureEntity) stored = NULL;
	g_autoptr(VentureMoney) amount = NULL;
	g_autoptr(VentureQuery) q = NULL;
	g_autoptr(GError) error = NULL;
	approve(f, b);
	status(f, b, "approved");
	g_assert_cmpint(count(f, "journal"), ==, 1);
	q = venture_query_new(VENTURE_TYPE_VENDOR_BILL_EVENT);
	stored = venture_database_find_one(f->db, q, &error);
	g_assert_no_error(error);
	g_object_get(stored, "amount", &amount, NULL);
	g_assert_nonnull(amount);
	g_assert_cmpint(venture_money_get_amount(amount), ==, 10000);
	g_clear_object(&stored);
	stored = venture_database_get(f->db, G_OBJECT_TYPE(b), venture_entity_get_id(b), NULL);
	field(stored, "due-date", "2026-02-28");
	g_assert_false(venture_database_save(f->db, stored, NULL, &error));
	g_assert_nonnull(error);
}

static void
test_partial_payment(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) b = bill(f, "PARTIAL");
	g_autoptr(VentureEntity) p = NULL;
	approve(f, b);
	p = payment(f, b, "40 USD", "2026-01-15");
	save(f, p);
	status(f, b, "partially_paid");
	g_assert_cmpint(count(f, "bill_payment_allocation"), ==, 1);
	g_clear_object(&p);
	p = payment(f, b, "70 USD", "2026-02-15");
	save(f, p);
	status(f, b, "paid");
	g_assert_cmpint(count(f, "bill_payment_allocation"), ==, 2);
	{
		g_autoptr(VentureQuery) q = venture_query_new(VENTURE_TYPE_VENDOR_CREDIT);
		g_autoptr(VentureEntity) credit = NULL;
		g_autoptr(VentureMoney) remaining = NULL;
		g_autoptr(GError) error = NULL;
		venture_query_set_organization(q, f->org);
		venture_query_add_filter_int(q, "payment-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(p), NULL);
		credit = venture_database_find_one(f->db, q, &error);
		g_assert_no_error(error);
		g_assert_nonnull(credit);
		g_object_get(credit, "remaining", &remaining, NULL);
		g_assert_cmpint(venture_money_get_amount(remaining), ==, 1000);
		field(credit, "remaining", "500 USD");
		g_assert_false(venture_database_save(f->db, credit, NULL, &error));
		g_assert_nonnull(error);
	}
}

static void
test_currency(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) b = bill(f, "CURRENCY");
	g_autoptr(VentureEntity) p = NULL;
	g_autoptr(GError) error = NULL;
	approve(f, b);
	p = payment(f, b, "40 EUR", "2026-01-15");
	g_assert_false(venture_database_save(f->db, p, NULL, &error));
	g_assert_nonnull(error);
	g_assert_cmpint(count(f, "bill_payment"), ==, 0);
	status(f, b, "approved");
}

static gboolean
reject_credit(VentureDatabase *db, VentureEntity *e, VentureEntity *old, gpointer data, GError **error)
{
	Fixture *f = data;
	/* The first payment write really happened inside the transaction. */
	g_assert_cmpint(count(f, "bill_payment"), ==, 1);
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_DATABASE, "Injected after the payment write");
	return FALSE;
}

static void
test_atomic(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) b = bill(f, "ATOMIC");
	g_autoptr(VentureEntity) p = NULL;
	g_autoptr(GError) error = NULL;
	gint64 audits;
	gint64 journals;
	approve(f, b);
	audits = count(f, "audit_entry");
	journals = count(f, "journal");
	p = payment(f, b, "100 USD", "2026-02-01");
	venture_database_add_save_validator(f->db, VENTURE_TYPE_VENDOR_CREDIT, reject_credit, f, NULL);
	g_assert_false(venture_database_save(f->db, p, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_DATABASE);
	g_assert_false(venture_entity_is_persisted(p));
	g_assert_cmpint(count(f, "bill_payment"), ==, 0);
	g_assert_cmpint(count(f, "vendor_credit"), ==, 0);
	g_assert_cmpint(count(f, "bill_payment_allocation"), ==, 0);
	g_assert_cmpint(count(f, "journal"), ==, journals);
	g_assert_cmpint(count(f, "audit_entry"), ==, audits);
	status(f, b, "approved");
}

static gint64
metric(VentureReportResult *result, const gchar *key)
{
	GPtrArray *metrics = venture_report_result_get_metrics(result);
	guint i;
	for (i = 0; i < metrics->len; i++)
	{
		VentureMetric *m = g_ptr_array_index(metrics, i);
		if (g_strcmp0(key, venture_metric_get_key(m)) == 0)
			return venture_money_get_amount(venture_metric_get_money(m));
	}
	g_assert_not_reached();
}

static void
test_reports(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) b = bill(f, "REPORT");
	g_autoptr(VentureEntity) p = NULL;
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(GTimeZone) tz = g_time_zone_new_utc();
	g_autoptr(VentureDateRange) period = venture_date_range_parse("2026-01", tz, 1, NULL);
	g_autoptr(JsonObject) options = json_object_new();
	g_autoptr(GError) error = NULL;
	VentureReport *report;
	approve(f, b);
	p = payment(f, b, "40 USD", "2026-01-15");
	save(f, p);
	g_clear_object(&p);
	p = payment(f, b, "60 USD", "2026-02-15");
	save(f, p);
	report = venture_report_registry_lookup(venture_context_get_report_registry(f->context), "payables");
	g_assert_nonnull(report);
	result = venture_report_generate(report, f->context, period, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	g_assert_cmpint(metric(result, "outstanding"), ==, 6000);
	g_clear_object(&result);
	report = venture_report_registry_lookup(venture_context_get_report_registry(f->context), "vendor_statement");
	g_assert_nonnull(report);
	json_object_set_int_member(options, "vendor_id", f->vendor);
	result = venture_report_generate(report, f->context, period, options, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	g_assert_cmpint(metric(result, "balance"), ==, 6000);
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
	g_subprocess_launcher_setenv(launcher, "VENTURE_TOKEN", "payables-test-only", TRUE);
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

	*state_dir = g_dir_make_tmp("venture-payables-XXXXXX", &error);
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
test_surfaces(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) b = bill(f, "REST");
	g_autoptr(VentureEntity) web_bill = bill(f, "WEB");
	g_autoptr(VentureWebServer) server = NULL;
	g_autofree gchar *state_dir = NULL;
	g_autofree gchar *out = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *cli_path = g_canonicalize_filename("build/debug/venturectl", NULL);
	const gchar *argv[] = { cli_path, "--server", NULL, "bill", "pay", "1", "amount=60 USD", "date=2026-02-15", NULL };
	server = start_server(f, &state_dir);
	g_assert_cmpuint(http_request(server, "POST", "/api/v1/vendor_bill/1/approve", "application/json", "{\"date\":\"2026-01-01\"}", &out), ==, 201);
	status(f, b, "approved");
	g_assert_cmpuint(http_request(server, "POST", "/api/v1/vendor_bill/1/pay", "application/json", "{\"date\":\"2026-01-15\",\"amount\":\"40 USD\"}", NULL), ==, 201);
	status(f, b, "partially_paid");
	argv[2] = venture_web_server_get_base_url(server);
	g_clear_pointer(&out, g_free);
	out = run_cli(argv, NULL, TRUE);
	status(f, b, "paid");
	g_assert_cmpuint(http_request(server, "GET", "/api/v1/reports/vendor_statement?period=2026-01&vendor_id=1", NULL, NULL, NULL), ==, 200);
	g_clear_pointer(&out, g_free);
	g_assert_cmpuint(http_request(server, "GET", "/e/vendor_bill/2", NULL, NULL, &out), ==, 200);
	g_assert_nonnull(strstr(out, "/bills/2/approve"));
	g_assert_cmpuint(http_request(server, "POST", "/bills/2/approve", "application/x-www-form-urlencoded", "", NULL), ==, 302);
	g_assert_cmpuint(http_request(server, "POST", "/bills/2/pay", "application/x-www-form-urlencoded", "", NULL), ==, 302);
	status(f, web_bill, "paid");
	{
		g_autoptr(VentureEntity) staged_bill = bill(f, "STAGED");
		g_autoptr(GPtrArray) pending = NULL;
		const gchar *mcp[] = { cli_path, "--server", venture_web_server_get_base_url(server), "mcp", NULL };
		const gchar *input = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{},\"clientInfo\":{\"name\":\"payables-test\",\"version\":\"1\"}}}\n"
			"{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n"
			"{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":{\"name\":\"venture_create\",\"arguments\":{\"type\":\"vendor_bill_event\",\"values\":{\"bill_id\":3,\"vendor_id\":1,\"kind\":\"approve\",\"state\":\"approved\",\"date\":\"2026-01-01\"}}}}\n";
		g_clear_pointer(&out, g_free);
		out = run_cli(mcp, input, TRUE);
		g_assert_null(strstr(out, "\"isError\":true"));
		status(f, staged_bill, "draft");
		pending = venture_confirmation_store_list_pending(venture_context_get_confirmations(f->context));
		g_assert_cmpuint(pending->len, ==, 1);
		path = g_strdup_printf("/api/v1/confirmations/%s/approve", venture_confirmation_get_id(g_ptr_array_index(pending, 0)));
		g_assert_cmpuint(http_request(server, "POST", path, "application/json", "{}", NULL), ==, 200);
		status(f, staged_bill, "approved");
	}
	{
		g_autoptr(VentureEntity) cli_bill = bill(f, "CLI-APPROVE");
		const gchar *approve_argv[] = { cli_path, "--server", venture_web_server_get_base_url(server),
			"bill", "approve", "4", "date=2026-01-01", NULL };
		g_clear_pointer(&out, g_free);
		out = run_cli(approve_argv, NULL, TRUE);
		status(f, cli_bill, "approved");
	}
	venture_web_server_stop(server);
	venture_test_remove_tree(state_dir);
}

static void
test_paid_line_expense(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) b = bill(f, "EXPENSE");
	g_autoptr(VentureEntity) p = NULL;
	g_autoptr(VentureEntity) e = record(f, "expense");
	g_autoptr(VentureEntity) line = NULL;
	g_autoptr(VentureQuery) q = venture_query_new(VENTURE_TYPE_VENDOR_BILL_LINE);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *external = NULL;
	gint64 journals;
	approve(f, b);
	p = payment(f, b, "100 USD", "2026-02-15");
	save(f, p);
	line = venture_database_find_one(f->db, q, &error);
	g_assert_no_error(error);
	external = g_strdup_printf("bill_line:%s", venture_entity_get_uuid(line));
	g_object_set(e, "external-id", external, "description", "Supplies", NULL);
	field(e, "amount", "100 USD");
	field(e, "occurred-at", "2026-02-15");
	journals = count(f, "journal");
	save(f, e);
	g_assert_cmpint(count(f, "journal"), ==, journals);
	/* Backfill must not offer the same paid cash projection for a second
	 * posting, even when automatic source journals are enabled. */
	{
		g_autoptr(GPtrArray) unposted = venture_autojournal_service_unposted(
			venture_database_get_autojournal_service(f->db), f->org, &error);
		g_assert_no_error(error);
		g_assert_nonnull(unposted);
		g_assert_cmpuint(unposted->len, ==, 0);
	}
	field(e, "amount", "200 USD");
	g_assert_false(venture_database_save(f->db, e, NULL, &error));
	g_assert_nonnull(error);
	g_clear_error(&error);
	g_assert_false(venture_database_delete(f->db, e, NULL, &error));
	g_assert_nonnull(error);
	g_clear_error(&error);
	{
		g_autoptr(VentureJournal) repost = venture_posting_service_post_document(
			venture_database_get_posting_service(f->db), "expense", e, NULL, &error);
		g_assert_null(repost);
		g_assert_nonnull(error);
		g_clear_error(&error);
	}
	{
		g_autoptr(VentureEntity) refund = record(f, "bill_refund");
		g_autoptr(VentureEntity) allocation = NULL;
		g_autoptr(VentureQuery) aq = venture_query_new(VENTURE_TYPE_BILL_PAYMENT_ALLOCATION);
		allocation = venture_database_find_one(f->db, aq, &error);
		g_assert_no_error(error);
		g_object_set(refund, "vendor-id", f->vendor, "allocation-id", venture_entity_get_id(allocation), NULL);
		field(refund, "date", "2026-03-01");
		field(refund, "amount", "10 USD");
		save(f, refund);
		g_assert_cmpint(count(f, "expense"), ==, 2);
		g_assert_cmpint(count(f, "journal"), ==, journals + 1);
		g_clear_object(&p);
		p = payment(f, b, "10 USD", "2026-03-02");
		save(f, p);
		g_assert_cmpint(count(f, "expense"), ==, 3);
		status(f, b, "paid");
	}

}

static gint64
account_balance(Fixture *f, const gchar *code, const gchar *date)
{
	g_autoptr(VentureQuery) q = venture_query_new(VENTURE_TYPE_ACCOUNT);
	g_autoptr(VentureEntity) account = NULL;
	g_autoptr(VentureMoney) money = NULL;
	g_autoptr(GDateTime) cutoff = venture_time_from_string(date, NULL);
	g_autoptr(GError) error = NULL;
	venture_query_set_organization(q, f->org);
	venture_query_add_filter_string(q, "code", VENTURE_FILTER_OP_EQ, code, NULL);
	account = venture_database_find_one(f->db, q, &error);
	g_assert_no_error(error);
	if (account == NULL)
	{
		g_autofree gchar *scoped = g_strdup_printf("%" G_GINT64_FORMAT ":%s", f->org, code);
		g_clear_object(&q);
		q = venture_query_new(VENTURE_TYPE_ACCOUNT);
		venture_query_set_organization(q, f->org);
		g_assert_true(venture_query_add_filter_string(q, "code", VENTURE_FILTER_OP_EQ, scoped, &error));
		account = venture_database_find_one(f->db, q, &error);
		g_assert_no_error(error);
	}
	g_assert_nonnull(account);
	money = venture_posting_service_account_balance(venture_database_get_posting_service(f->db),
		venture_entity_get_id(account), f->org, "USD", cutoff, &error);
	g_assert_no_error(error);
	g_assert_nonnull(money);
	return venture_money_get_amount(money);
}

static void
test_periods(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) b = bill(f, "PERIOD");
	g_autoptr(VentureEntity) p = NULL;
	g_autoptr(VentureEntity) year = record(f, "fiscal_year");
	g_autoptr(VentureEntity) period = NULL;
	g_autoptr(VentureQuery) q = venture_query_new(VENTURE_TYPE_FISCAL_PERIOD);
	g_autoptr(GError) error = NULL;
	g_object_set(year, "name", "2026", NULL);
	field(year, "start-at", "2026-01-01");
	save(f, year);
	approve(f, b);
	p = payment(f, b, "40 USD", "2026-01-15");
	save(f, p);
	venture_query_set_organization(q, f->org);
	venture_query_add_order(q, "start-at", VENTURE_SORT_ASCENDING, NULL);
	period = venture_database_find_one(f->db, q, &error);
	g_assert_no_error(error);
	field(period, "state", "closed");
	{
		VentureActor actor;
		actor.kind = VENTURE_ACTOR_KIND_USER;
		actor.name = "payables-test";
		actor.prompt = NULL;
		actor.request_id = NULL;
		actor.approved_by = NULL;
		g_assert_true(venture_database_save(f->db, period, &actor, &error));
		g_assert_no_error(error);
	}
	g_clear_object(&p);
	p = payment(f, b, "10 USD", "2026-01-20");
	g_assert_false(venture_database_save(f->db, p, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
	g_clear_error(&error);
	g_clear_object(&p);
	p = payment(f, b, "60 USD", "2026-02-15");
	save(f, p);
	status(f, b, "paid");
	g_assert_cmpint(account_balance(f, "2000", "2026-01-31T23:59:59Z"), ==, -6000);
	g_assert_cmpint(account_balance(f, "2000", "2026-02-28T23:59:59Z"), ==, 0);
	g_assert_cmpint(account_balance(f, "1000", "2026-02-28T23:59:59Z"), ==, -10000);
	g_assert_cmpint(account_balance(f, "6900", "2026-02-28T23:59:59Z"), ==, 10000);
}

static void
test_credit_void_refund(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) b = bill(f, "CREDIT");
	g_autoptr(VentureEntity) second = bill(f, "VOID");
	g_autoptr(VentureEntity) p = NULL;
	g_autoptr(VentureEntity) credit = record(f, "vendor_credit");
	g_autoptr(VentureEntity) allocation = record(f, "bill_payment_allocation");
	g_autoptr(VentureEntity) refund = record(f, "bill_refund");
	g_autoptr(VentureEntity) cash_allocation = NULL;
	g_autoptr(VentureEntity) void_request = NULL;
	g_autoptr(VentureQuery) q = venture_query_new(VENTURE_TYPE_BILL_PAYMENT_ALLOCATION);
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureMoney) balance = NULL;
	approve(f, b);
	approve(f, second);
	void_request = event(f, second, "void", "2026-01-02");
	save(f, void_request);
	status(f, second, "void");
	g_object_set(credit, "vendor-id", f->vendor, "kind", "credit_note", NULL);
	field(credit, "date", "2026-01-02");
	field(credit, "amount", "20 USD");
	save(f, credit);
	g_object_set(allocation, "bill-id", venture_entity_get_id(b), "credit-id", venture_entity_get_id(credit), NULL);
	field(allocation, "date", "2026-01-03");
	field(allocation, "amount", "20 USD");
	save(f, allocation);
	p = payment(f, b, "80 USD", "2026-01-15");
	g_object_set(p, "external-id", "bank-42", NULL);
	save(f, p);
	status(f, b, "paid");
	venture_query_add_filter_int(q, "payment-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(p), NULL);
	cash_allocation = venture_database_find_one(f->db, q, NULL);
	g_assert_nonnull(cash_allocation);
	g_object_set(refund, "vendor-id", f->vendor, "allocation-id", venture_entity_get_id(cash_allocation), NULL);
	field(refund, "date", "2026-02-01");
	field(refund, "amount", "10 USD");
	save(f, refund);
	status(f, b, "partially_paid");
	balance = venture_payables_service_bill_balance(venture_payables_service_get(f->db), venture_entity_get_id(b), NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpint(venture_money_get_amount(balance), ==, 1000);
	g_clear_object(&p);
	p = payment(f, b, "10 USD", "2026-02-15");
	g_object_set(p, "external-id", "bank-42", NULL);
	g_assert_false(venture_database_save(f->db, p, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_ALREADY_EXISTS);
	venture_test_accounting_roundtrip(f->db, f->org);

}

static void
test_batch(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) first = bill(f, "BATCH-1");
	g_autoptr(VentureEntity) second = bill(f, "BATCH-2");
	g_autoptr(VentureEntity) p = record(f, "bill_payment");
	g_autoptr(GPtrArray) allocations = g_ptr_array_new_with_free_func(g_object_unref);
	g_autoptr(GError) error = NULL;
	guint i;
	approve(f, first);
	approve(f, second);
	g_object_set(p, "vendor-id", f->vendor, "method", "transfer", NULL);
	field(p, "amount", "210 USD");
	field(p, "date", "2026-02-01");
	for (i = 0; i < 2; i++)
	{
		VentureEntity *a = record(f, "bill_payment_allocation");
		g_object_set(a, "bill-id", venture_entity_get_id(i == 0 ? first : second), NULL);
		field(a, "amount", "100 USD");
		g_ptr_array_add(allocations, a);
	}
	g_assert_true(venture_payables_service_apply_payment(venture_payables_service_get(f->db),
		VENTURE_BILL_PAYMENT(p), allocations, NULL, &error));
	g_assert_no_error(error);
	status(f, first, "paid");
	status(f, second, "paid");
	g_assert_cmpint(count(f, "bill_payment"), ==, 1);
	g_assert_cmpint(count(f, "bill_payment_allocation"), ==, 2);
	g_assert_cmpint(account_balance(f, "2000", "2026-02-28"), ==, 1000);
}

static void
test_bulk_workbench(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) first = bill(f, "BULK-1");
	g_autoptr(VentureEntity) second = bill(f, "BULK-2");
	g_autoptr(GArray) ids = g_array_new(FALSE, FALSE, sizeof(gint64));
	g_autoptr(GDateTime) date = NULL;
	g_autoptr(GError) error = NULL;
	gint64 a;
	gint64 b;
	approve(f, first);
	approve(f, second);
	a = venture_entity_get_id(first);
	b = venture_entity_get_id(second);
	g_array_append_val(ids, a);
	g_array_append_val(ids, b);
	date = g_date_time_new_from_iso8601("2026-02-01T00:00:00Z", NULL);
	g_assert_true(venture_payables_service_pay_bills(venture_payables_service_get(f->db),
		ids, date, "transfer", "transfer", "run-1", NULL, &error));
	g_assert_no_error(error);
	status(f, first, "paid");
	status(f, second, "paid");
	g_assert_cmpint(count(f, "bill_payment"), ==, 1);
	g_assert_false(venture_payables_service_execute_payment(venture_payables_service_get(f->db),
		"wire-unknown", VENTURE_BILL_PAYMENT(payment(f, first, "1 USD", "2026-02-02")),
		NULL, NULL, &error));
	g_assert_nonnull(error);
}

static void
test_migration(Fixture *f, gconstpointer unused)
{
	g_autoptr(OrmResult) result = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) b = bill(f, "MIGRATION");
	result = venture_database_query_raw(f->db, "SELECT CAST(COUNT(*) AS BIGINT) FROM schema_migrations WHERE version = 110", NULL, &error);
	g_assert_no_error(error);
	g_assert_true(orm_result_next(result));
	g_assert_cmpint(orm_row_get_integer(orm_result_get_row(result), 0), ==, 1);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	status(f, b, "draft");
	g_assert_cmpint(count(f, "vendor_bill_event"), ==, 0);
	g_assert_cmpint(count(f, "vendor_bill_line"), ==, 1);
}

static void
test_closed_draft_removal(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) b = bill(f, "CLOSED-DRAFT");
	g_autoptr(VentureEntity) year = record(f, "fiscal_year");
	g_autoptr(VentureEntity) period = NULL;
	g_autoptr(VentureQuery) q = venture_query_new(VENTURE_TYPE_FISCAL_PERIOD);
	g_autoptr(GError) error = NULL;
	VentureActor actor;
	actor.kind = VENTURE_ACTOR_KIND_USER;
	actor.name = "payables-test";
	actor.prompt = NULL;
	actor.request_id = NULL;
	actor.approved_by = NULL;
	g_object_set(year, "name", "2026", NULL);
	field(year, "start-at", "2026-01-01");
	save(f, year);
	venture_query_set_organization(q, f->org);
	venture_query_add_order(q, "start-at", VENTURE_SORT_ASCENDING, NULL);
	period = venture_database_find_one(f->db, q, &error);
	g_assert_no_error(error);
	field(period, "state", "closed");
	g_assert_true(venture_database_save(f->db, period, &actor, &error));
	g_assert_no_error(error);
	g_assert_false(venture_database_delete(f->db, b, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
}

static void
test_module_off(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) b = bill(f, "OFF");
	g_autoptr(GDateTime) date = venture_time_from_string("2026-01-01", NULL);
	g_autoptr(GError) error = NULL;
	venture_config_set_module_enabled(f->config, "payables", FALSE);
	venture_config_set_module_enabled(f->config, "supplier_portal", FALSE);
	g_assert_cmpuint(venture_entity_registry_lookup(venture_entity_registry_get_default(), "vendor_bill"), ==, G_TYPE_INVALID);
	g_assert_null(venture_report_registry_lookup(venture_context_get_report_registry(f->context), "payables"));
	g_assert_false(venture_payables_service_transition(venture_payables_service_get(f->db), VENTURE_VENDOR_BILL(b), "approved", date, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
	venture_config_set_module_enabled(f->config, "payables", TRUE);
	venture_config_set_module_enabled(f->config, "supplier_portal", TRUE);
	status(f, b, "draft");
}

static void
test_no_due_and_cutoff(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) b = bill(f, "NO-DUE");
	g_autoptr(VentureEntity) later = bill(f, "LATER");
	g_autoptr(VentureEntity) later_event = NULL;
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(GTimeZone) tz = g_time_zone_new_utc();
	g_autoptr(VentureDateRange) period = venture_date_range_parse("2026-01", tz, 1, NULL);
	g_autoptr(GError) error = NULL;
	VentureReport *report;
	const GValue *cell;
	g_object_set(b, "due-date", NULL, NULL);
	save(f, b);
	approve(f, b);
	later_event = event(f, later, "approve", "2026-03-01");
	save(f, later_event);
	report = venture_report_registry_lookup(venture_context_get_report_registry(f->context), "payables");
	result = venture_report_generate(report, f->context, period, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	g_assert_cmpint(metric(result, "outstanding"), ==, 10000);
	cell = venture_report_result_get_cell(result, 5, "age");
	g_assert_cmpstr(g_value_get_string(cell), ==, "No due date");
	cell = venture_report_result_get_cell(result, 5, "amount");
	g_assert_cmpint(venture_money_get_amount(g_value_get_boxed(cell)), ==, 10000);
}

static void
test_disabled_migration(void)
{
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureModuleRegistry) modules = venture_module_registry_new();
	g_autoptr(VentureEntityRegistry) registry = venture_entity_registry_new();
	g_autoptr(VentureDatabase) db = NULL;
	g_autoptr(OrmInspector) inspector = NULL;
	g_autoptr(VentureOrganization) org = venture_organization_new();
	g_autoptr(VentureCompany) vendor = venture_company_new();
	g_autoptr(VentureEntity) stored = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *directory = g_dir_make_tmp("venture-payables-upgrade-XXXXXX", &error);
	g_autofree gchar *uri = g_strdup_printf("sqlite://%s/books.db", directory);
	g_autofree gchar *name = NULL;
	gint64 vendor_id;
	g_assert_no_error(error);
	venture_entity_registry_register_builtins(registry);
	venture_module_registry_register_builtins(modules);
	venture_config_set_module_enabled(config, "payables", FALSE);
	venture_config_set_module_enabled(config, "supplier_portal", FALSE);
	venture_config_set_module_enabled(config, "recurring", FALSE);
	venture_config_set_module_enabled(config, "goods", FALSE);
	g_assert_true(venture_module_registry_configure(modules, config, &error));
	g_assert_no_error(error);
	venture_module_registry_apply(modules, registry);
	db = venture_database_new(uri, &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(db, registry, &error));
	g_assert_no_error(error);
	inspector = orm_inspector_new(venture_database_get_connection(db), &error);
	g_assert_no_error(error);
	g_assert_false(orm_inspector_has_table(inspector, "vendor_bills", NULL, &error));
	g_assert_no_error(error);
	g_object_set(org, "name", "Legacy organization", "slug", "legacy-payables", NULL);
	g_assert_true(venture_database_save(db, VENTURE_ENTITY(org), NULL, &error));
	g_assert_no_error(error);
	g_object_set(vendor, "organization-id", venture_entity_get_id(VENTURE_ENTITY(org)), "name", "Legacy supplier",
		"kind", VENTURE_COMPANY_KIND_SUPPLIER, NULL);
	g_assert_true(venture_database_save(db, VENTURE_ENTITY(vendor), NULL, &error));
	g_assert_no_error(error);
	vendor_id = venture_entity_get_id(VENTURE_ENTITY(vendor));
	g_clear_object(&inspector);
	g_clear_object(&db);
	venture_config_set_module_enabled(config, "payables", TRUE);
	g_assert_true(venture_module_registry_configure(modules, config, &error));
	g_assert_no_error(error);
	venture_module_registry_apply(modules, registry);
	db = venture_database_new(uri, &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(db, registry, &error));
	g_assert_no_error(error);
	stored = venture_database_get(db, VENTURE_TYPE_COMPANY, vendor_id, &error);
	g_assert_no_error(error);
	g_assert_nonnull(stored);
	g_object_get(stored, "name", &name, NULL);
	g_assert_cmpstr(name, ==, "Legacy supplier");
	inspector = orm_inspector_new(venture_database_get_connection(db), &error);
	g_assert_no_error(error);
	g_assert_true(orm_inspector_has_table(inspector, "vendor_bills", NULL, &error));
	g_assert_no_error(error);
	g_clear_object(&inspector);
	g_clear_object(&db);
	venture_test_remove_tree(directory);
}

static void
test_organization_uniqueness(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) b = bill(f, "SHARED-NUMBER");
	g_autoptr(VentureEntity) duplicate = record(f, "vendor_bill");
	g_autoptr(VentureOrganization) organization = venture_organization_new();
	g_autoptr(VentureCompany) vendor = venture_company_new();
	g_autoptr(VentureEntity) other_bill = NULL;
	g_autoptr(VentureEntity) p = NULL;
	g_autoptr(GError) error = NULL;
	Fixture other = *f;
	g_object_set(duplicate, "number", "SHARED-NUMBER", "company-id", f->vendor, "status", "draft", "currency", "USD", NULL);
	field(duplicate, "bill-date", "2026-01-01");
	g_assert_false(venture_database_save(f->db, duplicate, NULL, &error));
	g_assert_nonnull(error);
	g_clear_error(&error);
	g_object_set(organization, "name", "Second organization", "slug", "second-payables", NULL);
	save(f, VENTURE_ENTITY(organization));
	other.org = venture_entity_get_id(VENTURE_ENTITY(organization));
	g_object_set(vendor, "organization-id", other.org, "name", "Other supplier", "kind", VENTURE_COMPANY_KIND_SUPPLIER, NULL);
	save(f, VENTURE_ENTITY(vendor));
	other.vendor = venture_entity_get_id(VENTURE_ENTITY(vendor));
	other_bill = bill(&other, "SHARED-NUMBER");
	approve(f, b);
	approve(&other, other_bill);
	g_assert_cmpint(count(f, "vendor_bill"), ==, 1);
	g_assert_cmpint(count(&other, "vendor_bill"), ==, 1);
	p = payment(f, other_bill, "10 USD", "2026-02-01");
	g_assert_false(venture_database_save(f->db, p, NULL, &error));
	g_assert_nonnull(error);
	g_assert_cmpint(count(f, "bill_payment"), ==, 0);
	status(f, b, "approved");
	status(&other, other_bill, "approved");
}

static void
test_batch_approval(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) invoice = NULL, paid = NULL, allocation = NULL;
	g_autoptr(VentureAccountingApprovalRule) rule = venture_accounting_approval_rule_new();
	g_autoptr(GPtrArray) allocations = g_ptr_array_new_with_free_func(g_object_unref);
	VentureActor actor;
	(void)data;
	invoice = bill(f, "APPROVE-BATCH");
	approve(f, invoice);
	g_object_set(rule, "organization-id", f->org, "action", "pay", "require-second-actor", TRUE, NULL);
	save(f, VENTURE_ENTITY(rule));
	paid = payment(f, invoice, "100 USD", "2026-08-11");
	g_object_set(paid, "bill-id", (gint64)0, NULL);
	allocation = record(f, "bill_payment_allocation");
	g_object_set(allocation, "bill-id", venture_entity_get_id(invoice), NULL);
	field(allocation, "amount", "100 USD");
	field(allocation, "date", "2026-08-11");
	g_ptr_array_add(allocations, g_object_ref(allocation));
	actor.kind = VENTURE_ACTOR_KIND_USER;
	actor.name = "alice";
	actor.prompt = NULL;
	actor.request_id = NULL;
	actor.approved_by = NULL;
	g_assert_false(venture_payables_service_apply_payment(venture_payables_service_get(f->db),
		VENTURE_BILL_PAYMENT(paid), allocations, &actor, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_clear_error(&error);
	/* The approved intent includes amount and explicit allocations, even
	 * though payment.bill-id is zero. */
	actor.name = "bob";
	g_assert_true(venture_entity_set_field_from_string(paid, "amount", "50 USD", &error));
	g_assert_true(venture_entity_set_field_from_string(allocation, "amount", "50 USD", &error));
	g_assert_false(venture_payables_service_apply_payment(venture_payables_service_get(f->db),
		VENTURE_BILL_PAYMENT(paid), allocations, &actor, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_clear_error(&error);
	g_assert_true(venture_entity_set_field_from_string(paid, "amount", "100 USD", &error));
	g_assert_true(venture_entity_set_field_from_string(allocation, "amount", "100 USD", &error));
	g_assert_true(venture_payables_service_apply_payment(venture_payables_service_get(f->db),
		VENTURE_BILL_PAYMENT(paid), allocations, &actor, &error));
	g_assert_no_error(error);
	status(f, invoice, "paid");
}

/* The workbench must preserve pending consent across refusal, and the same
 * batch must require a different actor before any vendor payment is written. */
static void
test_workbench_approval(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) invoice = bill(f, "WORKBENCH-CONSENT");
	g_autoptr(VentureAccountingApprovalRule) rule = venture_accounting_approval_rule_new();
	g_autoptr(GArray) ids = g_array_new(FALSE, FALSE, sizeof(gint64));
	g_autoptr(GDateTime) date = venture_time_from_string("2026-08-11", NULL);
	g_autoptr(GError) error = NULL;
	gint64 id = venture_entity_get_id(invoice);
	VentureActor actor;

	(void)data;
	approve(f, invoice);
	g_object_set(rule, "organization-id", f->org, "action", "pay", "require-second-actor", TRUE, NULL);
	save(f, VENTURE_ENTITY(rule));
	g_array_append_val(ids, id);
	actor.kind = VENTURE_ACTOR_KIND_USER;
	actor.name = "alice";
	actor.prompt = NULL;
	actor.request_id = NULL;
	actor.approved_by = NULL;
	g_assert_false(venture_payables_service_pay_bills(venture_payables_service_get(f->db),
		ids, date, "transfer", "transfer", "review", &actor, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_clear_error(&error);
	g_assert_cmpint(count(f, "bill_payment"), ==, 0);
	g_assert_cmpint(count(f, "accounting_approval"), ==, 1);
	g_assert_false(venture_payables_service_pay_bills(venture_payables_service_get(f->db),
		ids, date, "transfer", "transfer", "review", &actor, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_clear_error(&error);
	actor.name = "bob";
	g_assert_true(venture_payables_service_pay_bills(venture_payables_service_get(f->db),
		ids, date, "transfer", "transfer", "review", &actor, &error));
	g_assert_no_error(error);
	status(f, invoice, "paid");
	g_assert_cmpint(count(f, "bill_payment"), ==, 1);
}

/* Tax reports follow approved/void events, never draft lines or the current
 * status of a bill that was voided after the requested reporting period. */
static void
test_tax_report_events(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) issued = bill(f, "TAX-ISSUED");
	g_autoptr(VentureEntity) draft = bill(f, "TAX-DRAFT");
	g_autoptr(VentureEntity) future = bill(f, "TAX-FUTURE");
	g_autoptr(VentureEntity) issue_event = NULL;
	g_autoptr(VentureEntity) void_event = NULL;
	g_autoptr(GTimeZone) zone = g_time_zone_new_utc();
	const gchar *months[] = { "2026-01", "2026-02", "2026-03" };
	gint64 amounts[] = { 1000, -1000, 1000 };
	VentureReport *report;
	guint i;

	(void)data;
	approve(f, issued);
	issue_event = event(f, future, "approve", "2026-03-01");
	save(f, issue_event);
	void_event = event(f, issued, "void", "2026-02-01");
	g_object_set(void_event, "state", "void", NULL);
	save(f, void_event);
	report = venture_report_registry_lookup(venture_context_get_report_registry(f->context), "tax_liability");
	g_assert_nonnull(report);
	for (i = 0; i < G_N_ELEMENTS(months); i++)
	{
		g_autoptr(GError) error = NULL;
		g_autoptr(VentureDateRange) period = venture_date_range_parse(months[i], zone, 1, &error);
		g_autoptr(VentureReportResult) result = NULL;
		const VentureMoney *tax;

		g_assert_no_error(error);
		result = venture_report_generate(report, f->context, period, NULL, &error);
		g_assert_no_error(error);
		g_assert_nonnull(result);
		g_assert_cmpuint(venture_report_result_get_row_count(result), ==, 1);
		tax = g_value_get_boxed(venture_report_result_get_cell(result, 0, "tax"));
		g_assert_cmpint(venture_money_get_amount(tax), ==, amounts[i]);
		tax = g_value_get_boxed(venture_report_result_get_cell(result, 0, "liability"));
		g_assert_cmpint(venture_money_get_amount(tax), ==, 0);
		g_assert_cmpstr(g_value_get_string(venture_report_result_get_cell(result, 0, "direction")), ==, "input");
	}
}
/* Cash recognition keeps each frozen expense leg, ignores noncash credits,
 * reverses vendor refunds in their own period and honors dimension filters. */
static void
test_cash_basis_bill_movements(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) b = bill(f, "CASH-SPLIT");
	g_autoptr(VentureEntity) account = record(f, "account");
	g_autoptr(VentureEntity) line = record(f, "vendor_bill_line");
	g_autoptr(VentureEntity) p = NULL;
	g_autoptr(VentureEntity) credit = record(f, "vendor_credit");
	g_autoptr(VentureEntity) allocation = record(f, "bill_payment_allocation");
	g_autoptr(VentureEntity) cash_allocation = NULL;
	g_autoptr(VentureEntity) refund = record(f, "bill_refund");
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_BILL_PAYMENT_ALLOCATION);
	g_autoptr(JsonObject) options = json_object_new();
	g_autoptr(GTimeZone) zone = g_time_zone_new_utc();
	g_autoptr(GError) error = NULL;
	VentureReport *report;
	const gchar *periods[] = { "2026-02", "2026-03" };
	gint64 debits[] = { 5000, 0 }, credits[] = { 0, 1000 };
	guint i;

	(void)data;
	g_object_set(account, "code", "6917", "name", "Separate expense", "active", TRUE,
		"kind", VENTURE_ACCOUNT_KIND_EXPENSE, NULL);
	save(f, account);
	g_object_set(line, "bill-id", venture_entity_get_id(b), "description", "Second cost",
		"quantity", "1", "account-id", venture_entity_get_id(account), NULL);
	field(line, "unit-price", "100 USD");
	save(f, line);
	approve(f, b);
	p = payment(f, b, "100 USD", "2026-02-01");
	save(f, p);
	g_object_set(credit, "vendor-id", f->vendor, "kind", "credit_note", NULL);
	field(credit, "date", "2026-02-02");
	field(credit, "amount", "100 USD");
	save(f, credit);
	g_object_set(allocation, "bill-id", venture_entity_get_id(b), "credit-id", venture_entity_get_id(credit), NULL);
	field(allocation, "date", "2026-02-02");
	field(allocation, "amount", "100 USD");
	save(f, allocation);
	venture_query_add_filter_int(query, "payment-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(p), NULL);
	cash_allocation = venture_database_find_one(f->db, query, &error);
	g_assert_no_error(error);
	g_assert_nonnull(cash_allocation);
	g_object_set(refund, "vendor-id", f->vendor, "allocation-id", venture_entity_get_id(cash_allocation), NULL);
	field(refund, "date", "2026-03-01");
	field(refund, "amount", "20 USD");
	save(f, refund);
	report = venture_report_registry_lookup(venture_context_get_report_registry(f->context), "general_ledger");
	json_object_set_int_member(options, "account_id", venture_entity_get_id(account));
	json_object_set_string_member(options, "basis", "cash");
	json_object_set_string_member(options, "currency", "USD");
	for (i = 0; i < G_N_ELEMENTS(periods); i++)
	{
		g_autoptr(VentureDateRange) period = venture_date_range_parse(periods[i], zone, 1, &error);
		g_autoptr(VentureReportResult) result = NULL;
		const VentureMoney *value;

		g_assert_no_error(error);
		result = venture_report_generate(report, f->context, period, options, &error);
		g_assert_no_error(error);
		g_assert_nonnull(result);
		g_assert_cmpuint(venture_report_result_get_row_count(result), ==, 1);
		value = g_value_get_boxed(venture_report_result_get_cell(result, 0, "debits"));
		g_assert_cmpint(value->amount, ==, debits[i]);
		value = g_value_get_boxed(venture_report_result_get_cell(result, 0, "credits"));
		g_assert_cmpint(value->amount, ==, credits[i]);
		g_clear_object(&result);
		json_object_set_string_member(options, "dimension", "unrelated");
		result = venture_report_generate(report, f->context, period, options, &error);
		g_assert_no_error(error);
		g_assert_cmpuint(venture_report_result_get_row_count(result), ==, 0);
		json_object_remove_member(options, "dimension");
	}
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	venture_entity_registry_register_builtins(venture_entity_registry_get_default());
	g_test_add_func("/payables/records", test_records);
	g_test_add("/payables/state-guard", Fixture, NULL, setup, test_state_guard, teardown);
	g_test_add("/payables/approval", Fixture, NULL, setup, test_approval, teardown);
	g_test_add("/payables/recoverable-tax", Fixture, NULL, setup, test_recoverable_tax, teardown);
	g_test_add("/payables/existing-namespaced-chart", Fixture, NULL, setup, test_existing_namespaced_chart, teardown);
	g_test_add("/payables/partial-payment", Fixture, NULL, setup, test_partial_payment, teardown);
	g_test_add("/payables/currency", Fixture, NULL, setup, test_currency, teardown);
	g_test_add("/payables/atomic", Fixture, NULL, setup, test_atomic, teardown);
	g_test_add("/payables/reports", Fixture, NULL, setup, test_reports, teardown);
	g_test_add("/payables/surfaces", Fixture, NULL, setup, test_surfaces, teardown);
	g_test_add("/payables/paid-line-expense", Fixture, NULL, setup, test_paid_line_expense, teardown);
	g_test_add("/payables/periods", Fixture, NULL, setup, test_periods, teardown);
	g_test_add("/payables/credit-void-refund", Fixture, NULL, setup, test_credit_void_refund, teardown);
	g_test_add("/payables/cash-basis-bill-movements", Fixture, NULL, setup, test_cash_basis_bill_movements, teardown);
	g_test_add("/payables/tax-report-events", Fixture, NULL, setup, test_tax_report_events, teardown);
	g_test_add("/payables/workbench-approval", Fixture, NULL, setup, test_workbench_approval, teardown);
	g_test_add("/payables/batch-approval", Fixture, NULL, setup, test_batch_approval, teardown);
	g_test_add("/payables/batch", Fixture, NULL, setup, test_batch, teardown);
	g_test_add("/payables/bulk-workbench", Fixture, NULL, setup, test_bulk_workbench, teardown);
	g_test_add("/payables/migration", Fixture, NULL, setup, test_migration, teardown);
	g_test_add("/payables/closed-draft-removal", Fixture, NULL, setup, test_closed_draft_removal, teardown);
	g_test_add("/payables/module-off", Fixture, NULL, setup, test_module_off, teardown);
	g_test_add("/payables/no-due-and-cutoff", Fixture, NULL, setup, test_no_due_and_cutoff, teardown);
	g_test_add_func("/payables/disabled-migration-restart", test_disabled_migration);
	g_test_add("/payables/organization-uniqueness", Fixture, NULL, setup, test_organization_uniqueness, teardown);
	return g_test_run();
}
