/*
 * test-tax-filing-pages.c - An owner sets up and prepares a tax filing by
 * picking their own vendors and fiscal periods, after a preview.
 *
 * The tax filings page asked for a "Vendor id" and a "Fiscal period id"
 * as bare numbers. An owner does not know either, and a number typed by
 * hand could name another business's vendor or period. If this regresses
 * the owner is back to guessing database ids, or is offered someone
 * else's suppliers.
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

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
	gint64 own;
	gint64 other;
	gint64 harbor;
	gint64 maple;
	gint64 foreign_vendor;
	gint64 period;
	gint64 foreign_period;
	gint64 customer;
	gchar *period_name;
	gchar *viewer;
	gchar *editor;
} Fixture;

static void
save(Fixture *f, VentureEntity *record)
{
	g_autoptr(GError) error = NULL;
	g_assert_true(venture_database_save(f->database, record, NULL, &error));
	g_assert_no_error(error);
}

static void
field(VentureEntity *record, const gchar *name, const gchar *value)
{
	g_autoptr(GError) error = NULL;
	g_assert_true(venture_entity_set_field_from_string(record, name, value, &error));
	g_assert_no_error(error);
}

static gint64
supplier(Fixture *f, gint64 org, const gchar *name)
{
	g_autoptr(VentureEntity) company = g_object_new(VENTURE_TYPE_COMPANY, "organization-id", org,
		"name", name, "kind", VENTURE_COMPANY_KIND_SUPPLIER, NULL);
	save(f, company);
	return venture_entity_get_id(company);
}

/* The first period of a fiscal year @name starting on @start, for @org. */
static gint64
first_period(Fixture *f, gint64 org, const gchar *name, const gchar *start, gchar **period_name)
{
	g_autoptr(VentureEntity) year = g_object_new(VENTURE_TYPE_FISCAL_YEAR, "organization-id", org,
		"name", name, NULL);
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_FISCAL_PERIOD);
	g_autoptr(VentureEntity) period = NULL;
	g_autoptr(GError) error = NULL;
	field(year, "start-at", start);
	save(f, year);
	venture_query_set_organization(query, org);
	venture_query_add_order(query, "start-at", VENTURE_SORT_ASCENDING, NULL);
	period = venture_database_find_one(f->database, query, &error);
	g_assert_no_error(error);
	g_assert_nonnull(period);
	if (period_name != NULL)
		g_object_get(period, "name", period_name, NULL);
	return venture_entity_get_id(period);
}

/* A paid bill: what a 1099-NEC pack totals. */
static void
pay(Fixture *f, gint64 vendor, const gchar *number, const gchar *amount, const gchar *date)
{
	g_autoptr(VentureEntity) bill = g_object_new(VENTURE_TYPE_VENDOR_BILL,
		"organization-id", f->own, "number", number, "company-id", vendor,
		"currency", "USD", "status", "draft", NULL);
	g_autoptr(VentureEntity) line = NULL;
	g_autoptr(VentureEntity) event = NULL;
	g_autoptr(VentureEntity) payment = NULL;
	field(bill, "bill-date", date);
	save(f, bill);
	line = g_object_new(VENTURE_TYPE_VENDOR_BILL_LINE, "organization-id", f->own,
		"bill-id", venture_entity_get_id(bill), "description", "Tuning parts",
		"quantity", "1", "category", "services", NULL);
	field(line, "unit-price", amount);
	save(f, line);
	event = g_object_new(VENTURE_TYPE_VENDOR_BILL_EVENT, "organization-id", f->own,
		"bill-id", venture_entity_get_id(bill), "vendor-id", vendor,
		"kind", "approve", "state", "approved", NULL);
	field(event, "date", date);
	save(f, event);
	payment = g_object_new(VENTURE_TYPE_BILL_PAYMENT, "organization-id", f->own,
		"vendor-id", vendor, "bill-id", venture_entity_get_id(bill), "method", "transfer", NULL);
	field(payment, "amount", amount);
	field(payment, "date", date);
	save(f, payment);
}

/* A bearer for a real member. The org-access middleware sends a principal
 * with no membership to /account before the page runs, so the editor gate
 * is only reachable through a member. The global role is what the gate
 * checks; the organization role is owner so the books stay writable. */
static gchar *
issue_token(Fixture *f, const gchar *name, VentureUserRole role)
{
	g_autoptr(VentureApiToken) token = venture_api_token_new();
	g_autoptr(VentureUser) user = venture_user_new();
	g_autoptr(VentureEntity) member = NULL;
	g_autoptr(GError) error = NULL;
	gchar *secret;
	g_object_set(user, "username", name, "role", role, "active", TRUE, NULL);
	g_assert_true(venture_user_set_password(user, "tax-pages-test-password", 1000, &error));
	g_assert_no_error(error);
	save(f, VENTURE_ENTITY(user));
	member = g_object_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP,
		"user-id", venture_entity_get_id(VENTURE_ENTITY(user)),
		"organization-id", f->own, "role", VENTURE_ORGANIZATION_ROLE_OWNER,
		"active", TRUE, NULL);
	save(f, member);
	g_object_set(token, "name", name, "role", role,
		"user-id", venture_entity_get_id(VENTURE_ENTITY(user)), NULL);
	secret = venture_api_token_generate(token);
	save(f, VENTURE_ENTITY(token));
	return secret;
}

static void
set_up_common(Fixture *f, gconstpointer data, gboolean require_auth)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) other = NULL;
	g_autoptr(VentureEntity) customer = NULL;
	g_autoptr(VentureEntity) form = NULL;
	(void)data;
	f->state_dir = g_dir_make_tmp("venture-tax-pages-XXXXXX", NULL);
	f->config = venture_config_new();
	g_object_set(f->config, "state-dir", f->state_dir, "server-bind-address", "127.0.0.1",
		"server-port", (gint64)0, "security-require-auth", require_auth, NULL);
	f->database = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->database);
	g_assert_true(venture_database_migrate(f->database, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->own = venture_context_get_default_organization_id(f->context);
	other = venture_entity_registry_create(venture_entity_registry_get_default(), "organization", NULL);
	g_object_set(other, "name", "Other Business", NULL);
	save(f, other);
	f->other = venture_entity_get_id(other);

	f->harbor = supplier(f, f->own, "Harbor Felt Supply");
	f->maple = supplier(f, f->own, "Maple Action Parts");
	f->foreign_vendor = supplier(f, f->other, "Elsewhere Strings Co");
	customer = g_object_new(VENTURE_TYPE_COMPANY, "organization-id", f->own,
		"name", "Only A Customer", "kind", VENTURE_COMPANY_KIND_CUSTOMER, NULL);
	save(f, customer);
	f->customer = venture_entity_get_id(customer);
	f->period = first_period(f, f->own, "FY 2026", "2026-01-01", &f->period_name);
	f->foreign_period = first_period(f, f->other, "Their FY 2026", "2026-01-01", NULL);

	form = g_object_new(VENTURE_TYPE_CONTRACTOR_TAX_FORM, "organization-id", f->own,
		"vendor-id", f->harbor, "tin", "99-9999999", "tin-type", "EIN",
		"legal-name", "Harbor Felt Supply LLC", "active", TRUE, NULL);
	save(f, form);
	pay(f, f->harbor, "B-1", "2500.00", "2026-03-04");
	if (require_auth)
	{
		f->viewer = issue_token(f, "tax-viewer", VENTURE_USER_ROLE_VIEWER);
		f->editor = issue_token(f, "tax-editor", VENTURE_USER_ROLE_EDITOR);
	}

	f->server = venture_web_server_new(f->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(f->server, &error));
	g_assert_no_error(error);
	f->url = g_strdup_printf("http://127.0.0.1:%u", venture_web_server_get_port(f->server));
	f->session = soup_session_new();
}

static void
set_up(Fixture *f, gconstpointer data)
{
	set_up_common(f, data, FALSE);
}

static void
set_up_auth(Fixture *f, gconstpointer data)
{
	set_up_common(f, data, TRUE);
}

static void
tear_down(Fixture *f, gconstpointer data)
{
	(void)data;
	venture_web_server_stop(f->server);
	g_clear_object(&f->session);
	g_clear_object(&f->server);
	g_clear_object(&f->context);
	g_clear_object(&f->database);
	g_clear_object(&f->config);
	venture_test_remove_tree(f->state_dir);
	g_free(f->state_dir);
	g_free(f->url);
	g_free(f->period_name);
	g_free(f->viewer);
	g_free(f->editor);
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

/* Sends @method @path (with a form @body for POST) for the fixture's own
 * business, without following a redirect. @token is a bearer when
 * authentication is on, or NULL when the server does not require it. */
static gchar *
send_request(Fixture *f, const gchar *token, const gchar *method, const gchar *path,
	const gchar *body, guint *status, gchar **location)
{
	g_autofree gchar *url = g_strconcat(f->url, path, NULL);
	g_autoptr(SoupMessage) message = soup_message_new(method, url);
	g_autofree gchar *cookie = g_strdup_printf("venture_entity=%" G_GINT64_FORMAT, f->own);
	Request result = { FALSE, NULL, NULL };
	gchar *text;
	soup_message_add_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	soup_message_headers_append(soup_message_get_request_headers(message), "Cookie", cookie);
	soup_message_headers_append(soup_message_get_request_headers(message), "Accept", "text/html");
	if (token != NULL)
	{
		g_autofree gchar *bearer = g_strconcat("Bearer ", token, NULL);
		soup_message_headers_append(soup_message_get_request_headers(message), "Authorization", bearer);
	}
	if (body != NULL)
	{
		g_autoptr(GBytes) bytes = g_bytes_new(body, strlen(body));
		soup_message_set_request_body_from_bytes(message, "application/x-www-form-urlencoded", bytes);
	}
	soup_session_send_and_read_async(f->session, message, G_PRIORITY_DEFAULT, NULL, request_done, &result);
	while (!result.done)
		g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(result.error);
	*status = soup_message_get_status(message);
	if (location != NULL)
		*location = g_strdup(soup_message_headers_get_one(soup_message_get_response_headers(message), "Location"));
	text = g_strndup(g_bytes_get_data(result.body, NULL), g_bytes_get_size(result.body));
	g_bytes_unref(result.body);
	return text;
}

static gchar *
send_as_owner(Fixture *f, const gchar *method, const gchar *path, const gchar *body,
	guint *status, gchar **location)
{
	return send_request(f, NULL, method, path, body, status, location);
}

static guint
count(Fixture *f, GType type)
{
	g_autoptr(VentureQuery) query = venture_query_new(type);
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GError) error = NULL;
	venture_query_set_limit(query, 0);
	rows = venture_database_find(f->database, query, &error);
	g_assert_no_error(error);
	return rows->len;
}

/* Given an owner with two named suppliers and a fiscal year, the page
 * should offer those names and that year's periods by name.
 * Expected: no box asks for an id, and another business's supplier and
 * a customer are not offered. Got before: "Vendor id" and "Fiscal period
 * id" number boxes and no names at all. */
static void
test_page_offers_named_choices(Fixture *f, gconstpointer data)
{
	guint status = 0;
	g_autofree gchar *page = send_as_owner(f, "GET", "/tax-filings", NULL, &status, NULL);
	(void)data;
	g_assert_cmpuint(status, ==, 200);
	g_assert_nonnull(strstr(page, "Harbor Felt Supply"));
	g_assert_nonnull(strstr(page, "Maple Action Parts"));
	g_assert_nonnull(strstr(page, f->period_name));
	g_assert_null(strstr(page, "Elsewhere Strings Co"));
	g_assert_null(strstr(page, "Only A Customer"));
	g_assert_null(strstr(page, "Vendor id"));
	g_assert_null(strstr(page, "Fiscal period id"));
	g_assert_null(strstr(page, "type=\"number\""));
	g_assert_nonnull(strstr(page, "<select name=\"vendor_id\""));
	g_assert_nonnull(strstr(page, "<select name=\"fiscal_period_id\""));
	g_assert_nonnull(strstr(page, "<select name=\"year\""));
}

/* Given a pick of one supplier and a year, the preview should name the
 * supplier, the year and what was paid, and save nothing; preparing from
 * it should make the pack for that supplier. */
static void
test_1099_preview_then_prepare(Fixture *f, gconstpointer data)
{
	guint status = 0;
	g_autofree gchar *path = g_strdup_printf("/tax-filings/preview?kind=1099&vendor_id=%" G_GINT64_FORMAT "&year=2026", f->harbor);
	g_autofree gchar *body = g_strdup_printf("kind=1099&vendor_id=%" G_GINT64_FORMAT "&year=2026", f->harbor);
	g_autofree gchar *preview = NULL;
	g_autofree gchar *done = NULL;
	g_autofree gchar *location = NULL;
	g_autofree gchar *expected = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_CONTRACTOR_TAX_PACK);
	g_autoptr(VentureEntity) pack = NULL;
	g_autoptr(GError) error = NULL;
	gint64 vendor = 0;
	gint64 year = 0;
	guint audits;
	guint activities;
	(void)data;
	audits = count(f, VENTURE_TYPE_AUDIT_ENTRY);
	activities = count(f, VENTURE_TYPE_ACTIVITY);
	preview = send_as_owner(f, "GET", path, NULL, &status, NULL);
	g_assert_cmpuint(status, ==, 200);
	g_assert_nonnull(strstr(preview, "Harbor Felt Supply"));
	g_assert_nonnull(strstr(preview, "2026"));
	g_assert_nonnull(strstr(preview, "2,500.00"));
	g_assert_nonnull(strstr(preview, "action=\"/tax-filings/prepare\""));
	g_assert_cmpuint(count(f, VENTURE_TYPE_CONTRACTOR_TAX_PACK), ==, 0);
	g_assert_cmpuint(count(f, VENTURE_TYPE_AUDIT_ENTRY), ==, audits);
	g_assert_cmpuint(count(f, VENTURE_TYPE_ACTIVITY), ==, activities);

	done = send_as_owner(f, "POST", "/tax-filings/prepare", body, &status, &location);
	g_assert_cmpuint(status / 100, ==, 3);
	pack = venture_database_find_one(f->database, query, &error);
	g_assert_no_error(error);
	g_assert_nonnull(pack);
	g_object_get(pack, "vendor-id", &vendor, "year", &year, NULL);
	g_assert_cmpint(vendor, ==, f->harbor);
	g_assert_cmpint(year, ==, 2026);
	g_assert_cmpint(venture_entity_get_organization_id(pack), ==, f->own);
	expected = g_strdup_printf("/e/contractor_tax_pack/%" G_GINT64_FORMAT, venture_entity_get_id(pack));
	g_assert_cmpstr(location, ==, expected);
}

/* Given a pick of one of the owner's fiscal periods, the preview should
 * name the period, and preparing should make the return for it. */
static void
test_sales_preview_then_prepare(Fixture *f, gconstpointer data)
{
	guint status = 0;
	g_autofree gchar *path = g_strdup_printf("/tax-filings/preview?kind=sales&country=US&jurisdiction=US-NY&fiscal_period_id=%" G_GINT64_FORMAT, f->period);
	g_autofree gchar *body = g_strdup_printf("kind=sales&country=US&jurisdiction=US-NY&fiscal_period_id=%" G_GINT64_FORMAT, f->period);
	g_autofree gchar *preview = NULL;
	g_autofree gchar *done = NULL;
	g_autofree gchar *location = NULL;
	g_autofree gchar *expected = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_TAX_FILING);
	g_autoptr(VentureEntity) filing = NULL;
	g_autoptr(GError) error = NULL;
	gint64 period = 0;
	guint audits;
	guint activities;
	(void)data;
	audits = count(f, VENTURE_TYPE_AUDIT_ENTRY);
	activities = count(f, VENTURE_TYPE_ACTIVITY);
	preview = send_as_owner(f, "GET", path, NULL, &status, NULL);
	g_assert_cmpuint(status, ==, 200);
	g_assert_nonnull(strstr(preview, f->period_name));
	g_assert_nonnull(strstr(preview, "US-NY"));
	g_assert_nonnull(strstr(preview, "action=\"/tax-filings/prepare\""));
	g_assert_cmpuint(count(f, VENTURE_TYPE_TAX_FILING), ==, 0);
	g_assert_cmpuint(count(f, VENTURE_TYPE_AUDIT_ENTRY), ==, audits);
	g_assert_cmpuint(count(f, VENTURE_TYPE_ACTIVITY), ==, activities);

	done = send_as_owner(f, "POST", "/tax-filings/prepare", body, &status, &location);
	g_assert_cmpuint(status / 100, ==, 3);
	filing = venture_database_find_one(f->database, query, &error);
	g_assert_no_error(error);
	g_assert_nonnull(filing);
	g_object_get(filing, "fiscal-period-id", &period, NULL);
	g_assert_cmpint(period, ==, f->period);
	g_assert_cmpint(venture_entity_get_organization_id(filing), ==, f->own);
	expected = g_strdup_printf("/e/tax_filing/%" G_GINT64_FORMAT, venture_entity_get_id(filing));
	g_assert_cmpstr(location, ==, expected);
}

/* Given another business's supplier or period typed into the address,
 * the preview and the preparation should refuse it and save nothing. */
static void
test_other_business_refused(Fixture *f, gconstpointer data)
{
	guint status = 0;
	g_autofree gchar *vendor_path = g_strdup_printf("/tax-filings/preview?kind=1099&vendor_id=%" G_GINT64_FORMAT "&year=2026", f->foreign_vendor);
	g_autofree gchar *vendor_body = g_strdup_printf("kind=1099&vendor_id=%" G_GINT64_FORMAT "&year=2026", f->foreign_vendor);
	g_autofree gchar *period_path = g_strdup_printf("/tax-filings/preview?kind=sales&country=US&jurisdiction=US-NY&fiscal_period_id=%" G_GINT64_FORMAT, f->foreign_period);
	g_autofree gchar *period_body = g_strdup_printf("kind=sales&country=US&jurisdiction=US-NY&fiscal_period_id=%" G_GINT64_FORMAT, f->foreign_period);
	g_autofree gchar *a = NULL;
	g_autofree gchar *b = NULL;
	g_autofree gchar *c = NULL;
	g_autofree gchar *d = NULL;
	(void)data;
	a = send_as_owner(f, "GET", vendor_path, NULL, &status, NULL);
	g_assert_cmpuint(status, ==, 404);
	g_assert_null(strstr(a, "Elsewhere Strings Co"));
	/* The reason names the business, not a missing tax form: the refusal
	 * is the ownership check, which a form filed later would not get past. */
	g_assert_nonnull(strstr(a, "not one of this business"));
	d = send_as_owner(f, "GET", period_path, NULL, &status, NULL);
	g_assert_cmpuint(status, ==, 404);
	g_assert_nonnull(strstr(d, "not one of this business"));
	b = send_as_owner(f, "POST", "/tax-filings/prepare", vendor_body, &status, NULL);
	g_assert_cmpuint(status, ==, 404);
	g_assert_nonnull(strstr(b, "not one of this business"));
	c = send_as_owner(f, "POST", "/tax-filings/prepare", period_body, &status, NULL);
	g_assert_cmpuint(status, ==, 404);
	g_assert_nonnull(strstr(c, "not one of this business"));
	g_assert_cmpuint(count(f, VENTURE_TYPE_CONTRACTOR_TAX_PACK), ==, 0);
	g_assert_cmpuint(count(f, VENTURE_TYPE_TAX_FILING), ==, 0);
}

/* A customer of this business is not a supplier. Preview and prepare
 * refuse the id the same way they refuse another business's vendor. */
static void
test_customer_refused(Fixture *f, gconstpointer data)
{
	guint status = 0;
	g_autofree gchar *path = g_strdup_printf("/tax-filings/preview?kind=1099&vendor_id=%" G_GINT64_FORMAT "&year=2026", f->customer);
	g_autofree gchar *body = g_strdup_printf("kind=1099&vendor_id=%" G_GINT64_FORMAT "&year=2026", f->customer);
	g_autofree gchar *preview = NULL;
	g_autofree gchar *done = NULL;
	(void)data;
	preview = send_as_owner(f, "GET", path, NULL, &status, NULL);
	g_assert_cmpuint(status, ==, 404);
	g_assert_nonnull(strstr(preview, "not one of this business"));
	g_assert_null(strstr(preview, "Only A Customer"));
	done = send_as_owner(f, "POST", "/tax-filings/prepare", body, &status, NULL);
	g_assert_cmpuint(status, ==, 404);
	g_assert_nonnull(strstr(done, "not one of this business"));
	g_assert_cmpuint(count(f, VENTURE_TYPE_CONTRACTOR_TAX_PACK), ==, 0);
}

/* With authentication on, a viewer cannot prepare and an editor can. */
static void
test_prepare_requires_editor(Fixture *f, gconstpointer data)
{
	guint status = 0;
	g_autofree gchar *body = g_strdup_printf("kind=1099&vendor_id=%" G_GINT64_FORMAT "&year=2026", f->harbor);
	g_autofree gchar *refused = NULL;
	g_autofree gchar *done = NULL;
	g_autofree gchar *location = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_CONTRACTOR_TAX_PACK);
	g_autoptr(VentureEntity) pack = NULL;
	g_autoptr(GError) error = NULL;
	gint64 vendor = 0;
	(void)data;
	refused = send_request(f, f->viewer, "POST", "/tax-filings/prepare", body, &status, NULL);
	g_assert_cmpuint(status, ==, 403);
	g_assert_cmpuint(count(f, VENTURE_TYPE_CONTRACTOR_TAX_PACK), ==, 0);
	g_assert_cmpuint(count(f, VENTURE_TYPE_TAX_FILING), ==, 0);
	(void)refused;
	done = send_request(f, f->editor, "POST", "/tax-filings/prepare", body, &status, &location);
	g_assert_cmpuint(status / 100, ==, 3);
	pack = venture_database_find_one(f->database, query, &error);
	g_assert_no_error(error);
	g_assert_nonnull(pack);
	g_object_get(pack, "vendor-id", &vendor, NULL);
	g_assert_cmpint(vendor, ==, f->harbor);
	g_assert_nonnull(location);
	(void)done;
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	venture_entity_registry_register_builtins(venture_entity_registry_get_default());
	g_test_add("/tax-filing-pages/named-choices", Fixture, NULL, set_up, test_page_offers_named_choices, tear_down);
	g_test_add("/tax-filing-pages/1099-preview-prepare", Fixture, NULL, set_up, test_1099_preview_then_prepare, tear_down);
	g_test_add("/tax-filing-pages/sales-preview-prepare", Fixture, NULL, set_up, test_sales_preview_then_prepare, tear_down);
	g_test_add("/tax-filing-pages/other-business-refused", Fixture, NULL, set_up, test_other_business_refused, tear_down);
	g_test_add("/tax-filing-pages/customer-refused", Fixture, NULL, set_up, test_customer_refused, tear_down);
	g_test_add("/tax-filing-pages/prepare-requires-editor", Fixture, NULL, set_up_auth, test_prepare_requires_editor, tear_down);
	return g_test_run();
}
