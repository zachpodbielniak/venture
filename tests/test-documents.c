/*
 * test-documents.c - One guided page to add invoice/quote lines, tax and send.
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <venture.h>
#include <string.h>
#include <libsoup/soup.h>
#include "venture-test-util.h"

typedef struct
{
	VentureDatabase *db;
	VentureConfig *config;
	VentureContext *context;
	gint64 org;
	gint64 company;
} Fixture;

static void
save(Fixture *f, VentureEntity *record)
{
	g_autoptr(GError) error = NULL;
	g_assert_true(venture_database_save(f->db, record, NULL, &error));
	g_assert_no_error(error);
}

static void
setup(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureCompany) company = NULL;
	(void)data;
	f->config = venture_config_new();
	f->db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->db);
	f->org = venture_context_get_default_organization_id(f->context);
	company = venture_company_new();
	g_object_set(company, "name", "Buyer", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(company), f->org);
	save(f, VENTURE_ENTITY(company));
	f->company = venture_entity_get_id(VENTURE_ENTITY(company));
}

static void
teardown(Fixture *f, gconstpointer data)
{
	(void)data;
	g_clear_object(&f->context);
	g_clear_object(&f->db);
	g_clear_object(&f->config);
}

static void
actor_init(VentureActor *actor)
{
	actor->kind = VENTURE_ACTOR_KIND_USER;
	actor->name = "clerk";
	actor->prompt = NULL;
	actor->request_id = NULL;
	actor->approved_by = NULL;
}

static JsonObject *
invoice_spec(Fixture *f, gboolean send)
{
	g_autoptr(JsonBuilder) builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "company_id");
	json_builder_add_int_value(builder, f->company);
	json_builder_set_member_name(builder, "terms");
	json_builder_add_string_value(builder, "Net 30");
	json_builder_set_member_name(builder, "due_days");
	json_builder_add_int_value(builder, 30);
	json_builder_set_member_name(builder, "send");
	json_builder_add_boolean_value(builder, send);
	json_builder_set_member_name(builder, "lines");
	json_builder_begin_array(builder);
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "description");
	json_builder_add_string_value(builder, "Hours");
	json_builder_set_member_name(builder, "quantity");
	json_builder_add_double_value(builder, 1.5);
	json_builder_set_member_name(builder, "unit_price");
	json_builder_add_string_value(builder, "100 USD");
	json_builder_set_member_name(builder, "discount_percent");
	json_builder_add_int_value(builder, 10);
	json_builder_set_member_name(builder, "tax_percent");
	json_builder_add_int_value(builder, 5);
	json_builder_end_object(builder);
	json_builder_end_array(builder);
	json_builder_end_object(builder);
	return json_object_ref(json_node_get_object(json_builder_get_root(builder)));
}

static void
test_compose_invoice_and_send(Fixture *f, gconstpointer data)
{
	g_autoptr(JsonObject) spec = invoice_spec(f, TRUE);
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(GPtrArray) lines = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureMoney) amount = NULL;
	VentureActor actor;
	gint status;
	(void)data;
	actor_init(&actor);
	invoice = venture_document_service_compose_invoice(venture_document_service_get(f->db),
		f->org, spec, &actor, &error);
	g_assert_no_error(error);
	g_assert_nonnull(invoice);
	/* Canonical approval dates belong to a detached request, not caller JSON. */
	g_assert_false(json_object_has_member(spec, "issued_at"));
	g_assert_false(json_object_has_member(spec, "due_at"));
	g_object_get(invoice, "status", &status, NULL);
	g_assert_cmpint(status, ==, VENTURE_INVOICE_STATUS_SENT);
	query = venture_query_new(VENTURE_TYPE_INVOICE_LINE);
	venture_query_add_filter_int(query, "invoice-id", VENTURE_FILTER_OP_EQ,
		venture_entity_get_id(invoice), NULL);
	lines = venture_database_find(f->db, query, &error);
	g_assert_cmpuint(lines->len, ==, 1);
	amount = venture_invoice_line_get_amount(g_ptr_array_index(lines, 0), &error);
	g_assert_no_error(error);
	/* 1.5 * 100.00, 10% discount, 5% tax: 150 - 15 = 135 + 6.75 = 141.75 */
	g_assert_cmpint(venture_money_get_amount(amount), ==, 14175);
}

/* Configured fiscal periods must accept an ordinary draft: its invoice
 * date is a calendar date before the first save, not only after sending. */
static void
test_compose_invoice_dates(Fixture *f, gconstpointer data)
{
	g_autoptr(GDateTime) today = venture_settlement_service_today(venture_settlement_service_get(f->db));
	g_autoptr(GDateTime) start = g_date_time_new_utc(g_date_time_get_year(today), 1, 1, 0, 0, 0);
	g_autoptr(GDateTime) end = g_date_time_add_years(start, 1);
	g_autoptr(VentureEntity) year = g_object_new(VENTURE_TYPE_FISCAL_YEAR,
		"name", "Current year", "organization-id", f->org,
		"start-at", start, "end-at", end, "period-length", 1, NULL);
	g_autoptr(JsonObject) spec = invoice_spec(f, FALSE);
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(GDateTime) issued = NULL, due = NULL, expected_due = g_date_time_add_days(today, 30);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *issued_text = g_date_time_format(start, "%Y-01-15");
	g_autofree gchar *due_text = g_date_time_format(start, "%Y-02-15");
	g_autofree gchar *outside = g_strdup_printf("%d-12-31", g_date_time_get_year(today) - 1);

	(void)data;
	save(f, year);
	invoice = venture_document_service_compose_invoice(venture_document_service_get(f->db), f->org, spec, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(invoice);
	g_object_get(invoice, "issued-at", &issued, "due-at", &due, NULL);
	g_assert_true(g_date_time_equal(issued, today));
	g_assert_true(g_date_time_equal(due, expected_due));
	g_clear_object(&invoice);
	g_clear_pointer(&issued, g_date_time_unref);
	g_clear_pointer(&due, g_date_time_unref);
	json_object_set_string_member(spec, "issued_at", issued_text);
	json_object_set_string_member(spec, "due_at", due_text);
	json_object_set_boolean_member(spec, "send", TRUE);
	invoice = venture_document_service_compose_invoice(venture_document_service_get(f->db), f->org, spec, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(invoice);
	g_object_get(invoice, "issued-at", &issued, "due-at", &due, NULL);
	{
		g_autofree gchar *actual_issued = venture_time_to_date_string(issued, NULL);
		g_autofree gchar *actual_due = venture_time_to_date_string(due, NULL);
		g_assert_cmpstr(actual_issued, ==, issued_text);
		g_assert_cmpstr(actual_due, ==, due_text);
	}
	g_clear_object(&invoice);
	json_object_set_string_member(spec, "due_at", "not a date");
	invoice = venture_document_service_compose_invoice(venture_document_service_get(f->db), f->org, spec, NULL, &error);
	g_assert_null(invoice);
	g_assert_nonnull(error);
	g_clear_error(&error);
	json_object_set_string_member(spec, "due_at", "2026-02-30");
	invoice = venture_document_service_compose_invoice(venture_document_service_get(f->db), f->org, spec, NULL, &error);
	g_assert_null(invoice);
	g_assert_nonnull(error);
	g_clear_error(&error);
	json_object_set_int_member(spec, "due_at", 7);
	invoice = venture_document_service_compose_invoice(venture_document_service_get(f->db), f->org, spec, NULL, &error);
	g_assert_null(invoice);
	g_assert_nonnull(error);
	g_clear_error(&error);
	json_object_remove_member(spec, "due_at");
	json_object_set_int_member(spec, "due_days", G_MAXINT64);
	invoice = venture_document_service_compose_invoice(venture_document_service_get(f->db), f->org, spec, NULL, &error);
	g_assert_null(invoice);
	g_assert_nonnull(error);
	g_clear_error(&error);
	json_object_set_int_member(spec, "due_days", 0);
	invoice = venture_document_service_compose_invoice(venture_document_service_get(f->db), f->org, spec, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(invoice);
	g_clear_pointer(&due, g_date_time_unref);
	g_object_get(invoice, "due-at", &due, NULL);
	g_assert_null(due);
	g_clear_object(&invoice);
	json_object_set_string_member(spec, "due_at", issued_text);
	json_object_set_string_member(spec, "issued_at", due_text);
	invoice = venture_document_service_compose_invoice(venture_document_service_get(f->db), f->org, spec, NULL, &error);
	g_assert_null(invoice);
	g_assert_nonnull(error);
	g_clear_error(&error);
	json_object_remove_member(spec, "due_at");
	json_object_set_string_member(spec, "issued_at", outside);
	invoice = venture_document_service_compose_invoice(venture_document_service_get(f->db), f->org, spec, NULL, &error);
	g_assert_null(invoice);
	g_assert_nonnull(error);
}

static void
test_compose_quote_and_send(Fixture *f, gconstpointer data)
{
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(JsonObject) spec = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) quote = NULL;
	g_autoptr(VentureMoney) total = NULL;
	VentureActor actor;
	gint status;
	(void)data;
	actor_init(&actor);
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "company_id");
	json_builder_add_int_value(builder, f->company);
	json_builder_set_member_name(builder, "currency");
	json_builder_add_string_value(builder, "USD");
	json_builder_set_member_name(builder, "send");
	json_builder_add_boolean_value(builder, TRUE);
	json_builder_set_member_name(builder, "lines");
	json_builder_begin_array(builder);
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "description");
	json_builder_add_string_value(builder, "Work");
	json_builder_set_member_name(builder, "quantity");
	json_builder_add_int_value(builder, 2);
	json_builder_set_member_name(builder, "unit_price");
	json_builder_add_string_value(builder, "50 USD");
	json_builder_set_member_name(builder, "tax_percent");
	json_builder_add_int_value(builder, 10);
	json_builder_end_object(builder);
	json_builder_end_array(builder);
	json_builder_end_object(builder);
	spec = json_object_ref(json_node_get_object(json_builder_get_root(builder)));
	quote = venture_document_service_compose_quote(venture_document_service_get(f->db),
		f->org, spec, &actor, &error);
	g_assert_no_error(error);
	g_assert_nonnull(quote);
	g_object_get(quote, "status", &status, "total", &total, NULL);
	g_assert_cmpint(status, ==, VENTURE_QUOTE_SENT);
	g_assert_cmpint(venture_money_get_amount(total), ==, 11000);
}

static guint
count_documents(Fixture *f, GType type)
{
	g_autoptr(VentureQuery) query = venture_query_new(type);
	return (guint)venture_database_count(f->db, query, NULL);
}

static void
test_invalid_compose(Fixture *f, gconstpointer data)
{
	static const gchar *const payloads[] = {
		"{\"lines\":[null]}",
		"{\"lines\":[7]}",
		"{\"lines\":[{\"description\":\"Work\",\"quantity\":[],\"unit_price\":\"1 USD\"}]}",
		"{\"lines\":[{\"description\":\"Work\",\"quantity\":1,\"unit_price\":\"invalid\"}]}"
	};
	guint i;
	(void)data;
	/* Invalid request bodies must return an error, not trigger a GLib critical or leave headers. */
	for (i = 0; i < G_N_ELEMENTS(payloads); i++)
	{
		g_autoptr(JsonParser) parser = json_parser_new();
		g_autoptr(GError) error = NULL;
		g_autoptr(VentureEntity) invoice = NULL;
		g_autoptr(VentureEntity) quote = NULL;
		JsonObject *spec;
		g_assert_true(json_parser_load_from_data(parser, payloads[i], -1, &error));
		spec = json_node_get_object(json_parser_get_root(parser));
		json_object_set_int_member(spec, "company_id", f->company);
		invoice = venture_document_service_compose_invoice(venture_document_service_get(f->db),
			f->org, spec, NULL, &error);
		g_assert_null(invoice);
		g_assert_nonnull(error);
		g_clear_error(&error);
		quote = venture_document_service_compose_quote(venture_document_service_get(f->db),
			f->org, spec, NULL, &error);
		g_assert_null(quote);
		g_assert_nonnull(error);
		g_assert_cmpuint(count_documents(f, VENTURE_TYPE_INVOICE), ==, 0);
		g_assert_cmpuint(count_documents(f, VENTURE_TYPE_QUOTE), ==, 0);
	}
}

static void
test_fractional_quote_refused(Fixture *f, gconstpointer data)
{
	g_autoptr(JsonObject) spec = invoice_spec(f, FALSE);
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) quote = NULL;
	(void)data;
	/* Whole-unit quote quantities must not silently truncate 1.5 hours to one. */
	quote = venture_document_service_compose_quote(venture_document_service_get(f->db),
		f->org, spec, NULL, &error);
	g_assert_null(quote);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_cmpuint(count_documents(f, VENTURE_TYPE_QUOTE), ==, 0);
}

typedef struct
{
	gboolean done;
	GBytes *bytes;
	GError *error;
} DocumentResponse;

static void
document_response_done(GObject *source, GAsyncResult *result, gpointer data)
{
	DocumentResponse *response = data;
	response->bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &response->error);
	response->done = TRUE;
}

static guint
http_request_full(VentureWebServer *server, const gchar *method, const gchar *path,
	const gchar *body, const gchar *cookie, gchar **set_cookie, gchar **out)
{
	g_autoptr(SoupSession) session = soup_session_new_with_options("timeout", 15, NULL);
	g_autoptr(SoupMessage) message = NULL;
	g_autofree gchar *url = g_strconcat(venture_web_server_get_base_url(server), path, NULL);
	DocumentResponse response;
	memset(&response, 0, sizeof(response));
	message = soup_message_new(method, url);
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	if (cookie != NULL)
		soup_message_headers_replace(soup_message_get_request_headers(message), "Cookie", cookie);
	if (body != NULL)
	{
		g_autoptr(GBytes) payload = g_bytes_new(body, strlen(body));
		soup_message_set_request_body_from_bytes(message,
			(g_str_has_prefix(body, "compose-form=") || g_str_has_prefix(body, "username=")) ? "application/x-www-form-urlencoded" : "application/json", payload);
	}
	/* The server shares this context, so a blocking client would deadlock the fixture. */
	soup_session_send_and_read_async(session, message, G_PRIORITY_DEFAULT, NULL, document_response_done, &response);
	while (!response.done)
		g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(response.error);
	if (out && response.bytes)
		*out = g_strndup(g_bytes_get_data(response.bytes, NULL), g_bytes_get_size(response.bytes));
	g_clear_pointer(&response.bytes, g_bytes_unref);
	if (set_cookie != NULL)
		*set_cookie = g_strdup(soup_message_headers_get_one(soup_message_get_response_headers(message), "Set-Cookie"));
	return soup_message_get_status(message);
}

static guint
http_request(VentureWebServer *server, const gchar *method, const gchar *path,
	const gchar *body, gchar **out)
{
	return http_request_full(server, method, path, body, NULL, NULL, out);
}

/* Browser issuance must propose approval before opening the document's
 * transaction, and remembered tax treatment belongs to the approved write. */
static void
test_http_compose_approval(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureWebServer) server = NULL;
	g_autoptr(VentureAccountingApprovalRule) rule = venture_accounting_approval_rule_new();
	g_autoptr(VentureEntity) company = NULL;
	g_autofree gchar *dir = g_dir_make_tmp("venture-document-approval-XXXXXX", NULL);
	g_autofree gchar *alice = NULL, *bob = NULL, *body = NULL, *reply = NULL;
	g_autofree gchar *reason = NULL, *certificate = NULL;
	guint16 port = 0;
	gboolean exempt = FALSE;
	gboolean remember = data == NULL;
	guint i;
	const gchar *names[] = { "alice", "bob" };

	(void)data;
	g_assert_no_error(error);
	for (i = 0; i < G_N_ELEMENTS(names); i++)
	{
		g_autoptr(VentureUser) user = venture_user_new();
		g_object_set(user, "username", names[i], "role", VENTURE_USER_ROLE_OWNER, "active", TRUE, NULL);
		g_assert_true(venture_user_set_password(user, "correct-horse-battery", 100000, &error));
		save(f, VENTURE_ENTITY(user));
	}
	g_object_set(rule, "organization-id", f->org, "action", "post", "require-second-actor", TRUE, NULL);
	save(f, VENTURE_ENTITY(rule));
	g_object_set(f->config, "state-dir", dir, "server-bind-address", "127.0.0.1",
		"server-port", (gint64)port, "security-require-auth", TRUE, NULL);
	server = venture_web_server_new(f->context, &error);
	g_assert_true(venture_web_server_start(server, &error));
	port = venture_web_server_get_port(server);
	g_assert_cmpuint(http_request_full(server, "POST", "/login", "username=alice&password=correct-horse-battery",
		NULL, &alice, NULL), ==, 302);
	g_assert_nonnull(alice);
	g_assert_cmpuint(http_request_full(server, "POST", "/login", "username=bob&password=correct-horse-battery",
		NULL, &bob, NULL), ==, 302);
	g_assert_nonnull(bob);
	body = g_strdup_printf("compose-form=1&company-id=%" G_GINT64_FORMAT "%s"
		"&line-0-description=Work&line-0-quantity=1&line-0-unit-price=100+USD&send=1", f->company,
		remember ? "&tax-exempt=1&exempt-kind=Government&exempt-number=CERT-42&exempt-remember=1" : "");
	g_assert_cmpuint(http_request_full(server, "POST", "/invoices/compose", body, alice, NULL, &reply), ==, 403);
	g_assert_nonnull(strstr(reply, "second account"));
	g_assert_cmpuint(count_documents(f, VENTURE_TYPE_INVOICE), ==, 0);
	company = venture_database_get(f->db, VENTURE_TYPE_COMPANY, f->company, NULL);
	g_object_get(company, "tax-exempt", &exempt, NULL);
	g_assert_false(exempt);
	g_clear_pointer(&reply, g_free);
	{
		guint status = http_request_full(server, "POST", "/invoices/compose", body, bob, NULL, &reply);
		if (status != 302)
			g_test_message("Second actor: %s", reply);
		g_assert_cmpuint(status, ==, 302);
	}
	g_assert_cmpuint(count_documents(f, VENTURE_TYPE_INVOICE), ==, 1);
	g_clear_object(&company);
	company = venture_database_get(f->db, VENTURE_TYPE_COMPANY, f->company, NULL);
	g_object_get(company, "tax-exempt", &exempt, "tax-exempt-reason", &reason,
		"tax-exemption-number", &certificate, NULL);
	g_assert_cmpint(exempt, ==, remember);
	if (remember)
	{
		g_assert_cmpstr(reason, ==, "Government");
		g_assert_cmpstr(certificate, ==, "CERT-42");
	}
	venture_web_server_stop(server);
	venture_test_remove_tree(dir);
}

static void
test_http_compose(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureWebServer) server = NULL;
	g_autofree gchar *dir = g_dir_make_tmp("venture-documents-XXXXXX", NULL);
	g_autofree gchar *body = NULL;
	guint16 port;
	g_autoptr(JsonObject) spec = invoice_spec(f, FALSE);
	g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);
	g_autofree gchar *json = NULL;
	(void)data;
	port = 0;
	g_object_set(f->config, "state-dir", dir, "server-bind-address", "127.0.0.1",
		"server-port", (gint64)port, "security-require-auth", FALSE, NULL);
	server = venture_web_server_new(f->context, &error);
	g_assert_true(venture_web_server_start(server, &error));
	port = venture_web_server_get_port(server);
	json_node_take_object(node, json_object_ref(spec));
	json = venture_json_to_string(node, FALSE);
	g_assert_cmpuint(http_request(server, "POST", "/api/v1/invoices/compose", json, NULL), ==, 200);
	g_assert_cmpuint(http_request(server, "GET", "/invoices/compose", NULL, &body), ==, 200);
	g_assert_nonnull(strstr(body, "form-grid"));
	g_assert_nonnull(strstr(body, "line-0-description"));
	g_assert_nonnull(strstr(body, "name=\"issued-at\""));
	g_assert_nonnull(strstr(body, "name=\"due-at\""));
	g_assert_null(strstr(body, "name=\"payload\""));
	{
		g_autofree gchar *form = g_strdup_printf("compose-form=1&company-id=%" G_GINT64_FORMAT
			"&issued-at=2026-03-01&due-at=2026-03-31&line-0-description=Work&line-0-quantity=2&line-0-unit-price=50+USD", f->company);
		guint status = http_request(server, "POST", "/invoices/compose", form, NULL);
		g_assert_cmpuint(status, >=, 300);
		g_assert_cmpuint(status, <, 400);
		{
			g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_INVOICE);
			g_autoptr(VentureEntity) invoice = NULL;
			g_autoptr(GDateTime) issued = NULL, due = NULL;
			g_autofree gchar *issued_text = NULL, *due_text = NULL;
			venture_query_add_order(query, "id", VENTURE_SORT_DESCENDING, NULL);
			invoice = venture_database_find_one(f->db, query, NULL);
			g_assert_nonnull(invoice);
			g_object_get(invoice, "issued-at", &issued, "due-at", &due, NULL);
			issued_text = venture_time_to_date_string(issued, NULL);
			due_text = venture_time_to_date_string(due, NULL);
			g_assert_cmpstr(issued_text, ==, "2026-03-01");
			g_assert_cmpstr(due_text, ==, "2026-03-31");
		}
	}
	venture_web_server_stop(server);
	venture_test_remove_tree(dir);
}

static guint
count_type(Fixture *f, GType type)
{
	g_autoptr(VentureQuery) query = venture_query_new(type);
	g_autoptr(GPtrArray) rows = NULL;

	venture_query_set_limit(query, 0);
	rows = venture_database_find(f->db, query, NULL);
	g_assert_nonnull(rows);
	return rows->len;
}

/*
 * The browser's sheet is refused, not quietly trimmed. A row numbered at
 * or past the reader's limit -- which the page's ever-climbing counter
 * produces after rows are added and removed -- used to vanish from the
 * invoice; and a repeating sheet could name another organization's
 * customer, onto whom "Remember for this customer" wrote an exemption.
 * What breaks if this regresses: a customer is billed for fewer lines
 * than were typed, or one tenant edits another's tax treatment.
 */
static void
test_http_compose_guards(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureWebServer) server = NULL;
	g_autofree gchar *dir = g_dir_make_tmp("venture-documents-XXXXXX", NULL);
	g_autoptr(VentureOrganization) other = venture_organization_new();
	g_autoptr(VentureCompany) stranger = venture_company_new();
	g_autoptr(VentureEntity) reread = NULL;
	gboolean exempt = TRUE;
	guint16 port;
	(void)data;
	g_object_set(other, "name", "Another tenant", "slug", "another-tenant", NULL);
	save(f, VENTURE_ENTITY(other));
	g_object_set(stranger, "name", "Their customer", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(stranger), venture_entity_get_id(VENTURE_ENTITY(other)));
	save(f, VENTURE_ENTITY(stranger));

	port = 0;
	g_object_set(f->config, "state-dir", dir, "server-bind-address", "127.0.0.1",
		"server-port", (gint64)port, "security-require-auth", FALSE, NULL);
	server = venture_web_server_new(f->context, &error);
	g_assert_true(venture_web_server_start(server, &error));
	port = venture_web_server_get_port(server);

	{
		g_autofree gchar *form = g_strdup_printf("compose-form=1&company-id=%" G_GINT64_FORMAT
			"&line-0-description=Work&line-0-quantity=1&line-0-unit-price=50+USD"
			"&line-200-description=Late&line-200-quantity=1&line-200-unit-price=9+USD", f->company);
		g_autofree gchar *reply = NULL;
		guint status = http_request(server, "POST", "/invoices/compose", form, &reply);
		g_assert_cmpuint(status, >=, 400);
		g_assert_cmpuint(status, <, 500);
		g_assert_nonnull(strstr(reply, "line-200-"));
		g_assert_cmpuint(count_type(f, VENTURE_TYPE_INVOICE), ==, 0);
	}
	{
		g_autofree gchar *form = g_strdup_printf("compose-form=1&repeat=1&company-id=%" G_GINT64_FORMAT
			"&repeat-frequency=monthly&repeat-start=2026-04-01"
			"&line-0-description=Work&line-0-quantity=1&line-0-unit-price=50+USD"
			"&line-205-description=Late&line-205-quantity=1", f->company);
		guint status = http_request(server, "POST", "/invoices/compose", form, NULL);
		g_assert_cmpuint(status, >=, 400);
		g_assert_cmpuint(status, <, 500);
		g_assert_cmpuint(count_type(f, VENTURE_TYPE_RECURRING_SCHEDULE), ==, 0);
	}
	{
		g_autofree gchar *form = g_strdup_printf("compose-form=1&repeat=1&company-id=%" G_GINT64_FORMAT
			"&repeat-frequency=monthly&repeat-start=2026-04-01"
			"&tax-exempt=1&exempt-kind=Government&exempt-number=CERT-9&exempt-remember=1"
			"&line-0-description=Work&line-0-quantity=1&line-0-unit-price=50+USD",
			venture_entity_get_id(VENTURE_ENTITY(stranger)));
		guint status = http_request(server, "POST", "/invoices/compose", form, NULL);
		g_assert_cmpuint(status, >=, 400);
		g_assert_cmpuint(status, <, 500);
		g_assert_cmpuint(count_type(f, VENTURE_TYPE_RECURRING_SCHEDULE), ==, 0);
		reread = venture_database_get(f->db, VENTURE_TYPE_COMPANY,
			venture_entity_get_id(VENTURE_ENTITY(stranger)), &error);
		g_assert_no_error(error);
		g_object_get(reread, "tax-exempt", &exempt, NULL);
		g_assert_false(exempt);
	}
	{
		/* The same sheet for this organization's own customer is filed. */
		g_autofree gchar *form = g_strdup_printf("compose-form=1&repeat=1&company-id=%" G_GINT64_FORMAT
			"&repeat-frequency=monthly&repeat-start=2026-04-01"
			"&line-0-description=Work&line-0-quantity=1&line-0-unit-price=50+USD", f->company);
		guint status = http_request(server, "POST", "/invoices/compose", form, NULL);
		g_assert_cmpuint(status, >=, 300);
		g_assert_cmpuint(status, <, 400);
		g_assert_cmpuint(count_type(f, VENTURE_TYPE_RECURRING_SCHEDULE), ==, 1);
	}
	venture_web_server_stop(server);
	venture_test_remove_tree(dir);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/documents/invoice-dates", Fixture, NULL, setup, test_compose_invoice_dates, teardown);
	g_test_add("/documents/compose-invoice", Fixture, NULL, setup, test_compose_invoice_and_send, teardown);
	g_test_add("/documents/compose-quote", Fixture, NULL, setup, test_compose_quote_and_send, teardown);
	g_test_add("/documents/invalid-compose", Fixture, NULL, setup, test_invalid_compose, teardown);
	g_test_add("/documents/fractional-quote", Fixture, NULL, setup, test_fractional_quote_refused, teardown);
	g_test_add("/documents/http-compose-approval-plain", Fixture, "plain", setup, test_http_compose_approval, teardown);
	g_test_add("/documents/http-compose-approval", Fixture, NULL, setup, test_http_compose_approval, teardown);
	g_test_add("/documents/http-compose", Fixture, NULL, setup, test_http_compose, teardown);
	g_test_add("/documents/http-compose-guards", Fixture, NULL, setup, test_http_compose_guards, teardown);
	return g_test_run();
}
