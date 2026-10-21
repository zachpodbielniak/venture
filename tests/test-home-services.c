/*
 * test-home-services.c - The high-severity findings from walking a small
 * plumbing and heating business through a week in the web UI.
 *
 * Each case is what the owner hit: an accepted estimate became an invoice
 * that never fell due; a printed invoice was headed with the server's name
 * and not the business's; a caller's number typed with dashes found nothing,
 * while every search listed unrelated subscriptions; a technician added on
 * the Users page signed in to "ask an owner for access"; a job given to
 * "Mike" never reached the user "mike"; a plan with no price had no way to
 * add one.
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
	VentureWebServer *server;
	gchar *dir;
	gint64 org;
	gint64 company;
} Fixture;

typedef struct
{
	GBytes *bytes;
	GError *error;
	gboolean done;
} Response;

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
	g_object_set(company, "name", "Example Household", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(company), f->org);
	save(f, VENTURE_ENTITY(company));
	f->company = venture_entity_get_id(VENTURE_ENTITY(company));
	f->dir = g_dir_make_tmp("venture-home-services-XXXXXX", NULL);
	g_object_set(f->config, "state-dir", f->dir, "server-bind-address", "127.0.0.1",
		"server-port", (gint64)0, "security-require-auth", FALSE, NULL);
	f->server = venture_web_server_new(f->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(f->server, &error));
	g_assert_no_error(error);
}

static void
teardown(Fixture *f, gconstpointer data)
{
	(void)data;
	venture_web_server_stop(f->server);
	g_clear_object(&f->server);
	venture_test_remove_tree(f->dir);
	g_clear_pointer(&f->dir, g_free);
	g_clear_object(&f->context);
	g_clear_object(&f->db);
	g_clear_object(&f->config);
}

static void
response_done(GObject *source, GAsyncResult *result, gpointer data)
{
	Response *response = data;
	response->bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &response->error);
	response->done = TRUE;
}

/* The server shares this context, so the request runs on the loop. */
static guint
request(Fixture *f, const gchar *method, const gchar *path, const gchar *form,
	gchar **location, gchar **out)
{
	g_autoptr(SoupSession) session = soup_session_new_with_options("timeout", 15, NULL);
	g_autoptr(SoupMessage) message = NULL;
	g_autofree gchar *url = g_strconcat(venture_web_server_get_base_url(f->server), path, NULL);
	Response response;
	memset(&response, 0, sizeof(response));
	message = soup_message_new(method, url);
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	if (form != NULL)
	{
		g_autoptr(GBytes) payload = g_bytes_new(form, strlen(form));
		soup_message_set_request_body_from_bytes(message, "application/x-www-form-urlencoded", payload);
	}
	soup_session_send_and_read_async(session, message, G_PRIORITY_DEFAULT, NULL, response_done, &response);
	while (!response.done)
		g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(response.error);
	if (out != NULL)
		*out = response.bytes != NULL
			? g_strndup(g_bytes_get_data(response.bytes, NULL), g_bytes_get_size(response.bytes))
			: g_strdup("");
	if (location != NULL)
		*location = g_strdup(soup_message_headers_get_one(soup_message_get_response_headers(message), "Location"));
	g_clear_pointer(&response.bytes, g_bytes_unref);
	return soup_message_get_status(message);
}

static GPtrArray *
rows(Fixture *f, GType type)
{
	g_autoptr(VentureQuery) query = venture_query_new(type);
	g_autoptr(GError) error = NULL;
	GPtrArray *found;
	venture_query_set_limit(query, 0);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	found = venture_database_find(f->db, query, &error);
	g_assert_no_error(error);
	g_assert_nonnull(found);
	return found;
}

static gchar *
compose_form(Fixture *f, gboolean send)
{
	return g_strdup_printf("compose-form=1&company-id=%" G_GINT64_FORMAT
		"&line-0-description=Replace+water+heater&line-0-quantity=1&line-0-unit-price=1450+USD%s",
		f->company, send ? "&send=1" : "");
}

/* A draft quote with one line, made the way the sheet makes it. */
static gint64
draft_quote(Fixture *f)
{
	g_autofree gchar *form = compose_form(f, FALSE);
	g_autoptr(GPtrArray) quotes = NULL;
	g_assert_cmpuint(request(f, "POST", "/quotes/compose", form, NULL, NULL) / 100, ==, 3);
	quotes = rows(f, VENTURE_TYPE_QUOTE);
	g_assert_cmpuint(quotes->len, ==, 1);
	return venture_entity_get_id(g_ptr_array_index(quotes, 0));
}

static void
business_details(Fixture *f)
{
	g_autoptr(VentureEntity) business = venture_database_get(f->db, VENTURE_TYPE_ORGANIZATION, f->org, NULL);
	g_autoptr(VentureEntity) company = venture_database_get(f->db, VENTURE_TYPE_COMPANY, f->company, NULL);
	g_object_set(business, "name", "Example Plumbing & Heating", "address", "12 Example Way\nSpringfield",
		"phone", "(555) 010-0142", "email", "office@example.test", NULL);
	save(f, business);
	g_object_set(company, "address", "34 Sample Court\nSpringfield", "phone", "555.010.0177", NULL);
	save(f, company);
}

/* An accepted estimate's invoice is due on the composer's terms, so it
 * ages and reminders fall due instead of sitting under "No due date". */
static void
test_accepted_quote_invoice_due(Fixture *f, gconstpointer data)
{
	gint64 id = draft_quote(f);
	g_autofree gchar *share = g_strdup_printf("/quotes/%" G_GINT64_FORMAT "/share", id);
	g_autofree gchar *accept = g_strdup_printf("/quotes/%" G_GINT64_FORMAT "/accept", id);
	g_autoptr(GPtrArray) invoices = NULL;
	g_autoptr(GDateTime) issued = NULL, due = NULL;
	(void)data;
	g_assert_cmpuint(request(f, "POST", share, "", NULL, NULL) / 100, ==, 3);
	g_assert_cmpuint(request(f, "POST", accept, "accepted_by=Example+Customer", NULL, NULL) / 100, ==, 3);
	invoices = rows(f, VENTURE_TYPE_INVOICE);
	g_assert_cmpuint(invoices->len, ==, 1);
	g_object_get(g_ptr_array_index(invoices, 0), "issued-at", &issued, "due-at", &due, NULL);
	g_assert_nonnull(issued);
	g_assert_nonnull(due);
	g_assert_cmpint(g_date_time_difference(due, issued) / G_TIME_SPAN_DAY, ==, 30);
}

/* The printed invoice is from the business, to the customer, and fits a
 * phone: no server title, no internal "sent" badge. */
static void
test_printed_invoice_names_parties(Fixture *f, gconstpointer data)
{
	g_autofree gchar *form = compose_form(f, FALSE), *page = NULL, *path = NULL;
	g_autoptr(GPtrArray) invoices = NULL;
	(void)data;
	business_details(f);
	g_assert_cmpuint(request(f, "POST", "/invoices/compose", form, NULL, NULL) / 100, ==, 3);
	invoices = rows(f, VENTURE_TYPE_INVOICE);
	path = g_strdup_printf("/invoices/%" G_GINT64_FORMAT "/print", venture_entity_get_id(g_ptr_array_index(invoices, 0)));
	g_assert_cmpuint(request(f, "GET", path, NULL, NULL, &page), ==, 200);
	g_assert_nonnull(strstr(page, "Example Plumbing &amp; Heating"));
	g_assert_nonnull(strstr(page, "12 Example Way"));
	g_assert_nonnull(strstr(page, "(555) 010-0142"));
	g_assert_nonnull(strstr(page, "office@example.test"));
	g_assert_nonnull(strstr(page, "Billed to"));
	g_assert_nonnull(strstr(page, "34 Sample Court"));
	g_assert_nonnull(strstr(page, "table-wrap"));
	g_assert_nonnull(strstr(page, "width=device-width"));
	g_assert_null(strstr(page, "class=\"status\""));
}

/* The printed quote names both parties and has a line to sign; the public
 * page names the customer without repeating their contact details. */
static void
test_printed_quote_names_parties(Fixture *f, gconstpointer data)
{
	gint64 id;
	g_autofree gchar *print = NULL, *page = NULL, *share = NULL, *token = NULL, *link = NULL, *public_page = NULL;
	g_autoptr(VentureEntity) quote = NULL;
	(void)data;
	business_details(f);
	id = draft_quote(f);
	print = g_strdup_printf("/quotes/%" G_GINT64_FORMAT "/print", id);
	g_assert_cmpuint(request(f, "GET", print, NULL, NULL, &page), ==, 200);
	g_assert_nonnull(strstr(page, "Example Plumbing &amp; Heating"));
	g_assert_nonnull(strstr(page, "Prepared for"));
	g_assert_nonnull(strstr(page, "34 Sample Court"));
	g_assert_nonnull(strstr(page, "Accepted by"));
	share = g_strdup_printf("/quotes/%" G_GINT64_FORMAT "/share", id);
	g_assert_cmpuint(request(f, "POST", share, "", NULL, NULL) / 100, ==, 3);
	quote = venture_database_get(f->db, VENTURE_TYPE_QUOTE, id, NULL);
	g_object_get(quote, "acceptance-token", &token, NULL);
	link = g_strconcat("/q/", token, NULL);
	g_assert_cmpuint(request(f, "GET", link, NULL, NULL, &public_page), ==, 200);
	g_assert_nonnull(strstr(public_page, "Example Plumbing &amp; Heating"));
	g_assert_nonnull(strstr(public_page, "Example Household"));
	g_assert_null(strstr(public_page, "34 Sample Court"));
}

/* A caller's number finds them however it was typed, and a search lists
 * only records that match it. */
static void
test_search_by_phone(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureContact) contact = venture_contact_new();
	g_autofree gchar *dashes = NULL, *short_form = NULL, *name = NULL, *household = NULL;
	(void)data;
	business_details(f);
	g_object_set(contact, "name", "Example Tenant", "phone", "319.555.0177", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(contact), f->org);
	save(f, VENTURE_ENTITY(contact));
	g_assert_cmpuint(request(f, "GET", "/search?q=319-555-0177", NULL, NULL, &dashes), ==, 200);
	g_assert_nonnull(strstr(dashes, "Example Tenant"));
	g_assert_cmpuint(request(f, "GET", "/search?q=5550177", NULL, NULL, &short_form), ==, 200);
	g_assert_nonnull(strstr(short_form, "Example Tenant"));
	/* The household's own number, stored with dots, on the company. */
	g_assert_cmpuint(request(f, "GET", "/search?q=%28555%29+010+0177", NULL, NULL, &household), ==, 200);
	g_assert_nonnull(strstr(household, "Example Household"));
	g_assert_cmpuint(request(f, "GET", "/search?q=Tenant", NULL, NULL, &name), ==, 200);
	g_assert_nonnull(strstr(name, "Example Tenant"));
	/* A membership has no searchable text, so it never matches a word. */
	g_assert_null(strstr(name, "Organization membership #"));
}

/* Adding a person on the Users page lets them work in the business, and
 * pickers name them. */
static void
test_new_user_can_work(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP);
	g_autoptr(GPtrArray) memberships = NULL;
	g_autoptr(VentureQuery) users = venture_query_new(VENTURE_TYPE_USER);
	g_autoptr(VentureEntity) user = NULL;
	g_autofree gchar *location = NULL, *page = NULL;
	gint role = -1;
	guint i, found = 0;
	(void)data;
	g_assert_cmpuint(request(f, "POST", "/users", "username=examplet&password=correct-horse-battery&role=editor",
		&location, NULL) / 100, ==, 3);
	g_assert_nonnull(strstr(location, "created-access-ok"));
	venture_query_add_filter_string(users, "username", VENTURE_FILTER_OP_EQ, "examplet", NULL);
	user = venture_database_find_one(f->db, users, NULL);
	g_assert_nonnull(user);
	venture_query_set_limit(query, 0);
	memberships = venture_database_find(f->db, query, NULL);
	for (i = 0; i < memberships->len; i++)
	{
		gint64 user_id = 0;
		g_object_get(g_ptr_array_index(memberships, i), "user-id", &user_id, "role", &role, NULL);
		if (user_id == venture_entity_get_id(user))
		{
			found++;
			g_assert_cmpint(role, ==, VENTURE_ORGANIZATION_ROLE_EDITOR);
			g_assert_cmpint(venture_entity_get_organization_id(g_ptr_array_index(memberships, i)), ==, f->org);
		}
	}
	g_assert_cmpuint(found, ==, 1);
	g_assert_cmpuint(request(f, "GET", "/e/organization_membership/new", NULL, NULL, &page), ==, 200);
	g_assert_nonnull(strstr(page, ">examplet<"));
}

/* A job given to "Mike " is on the user mike's day. */
static void
test_owner_matches_user(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureUser) user = venture_user_new();
	g_autoptr(VentureEntity) activity = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *owner = NULL;
	(void)data;
	g_object_set(user, "username", "mike", "role", VENTURE_USER_ROLE_EDITOR, "active", TRUE, NULL);
	g_assert_true(venture_user_set_password(user, "correct-horse-battery", 100000, &error));
	save(f, VENTURE_ENTITY(user));
	activity = g_object_new(venture_entity_registry_lookup(venture_entity_registry_get_default(), "activity"),
		"subject", "Replace water heater", "owner", "Mike ", NULL);
	venture_entity_set_organization_id(activity, f->org);
	save(f, activity);
	g_object_get(activity, "owner", &owner, NULL);
	g_assert_cmpstr(owner, ==, "mike");
}

/* A plan with no price says so and links straight to adding one. */
static void
test_plan_add_price(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) plan = g_object_new(VENTURE_TYPE_PLAN, "name", "Example Comfort Club", NULL);
	g_autofree gchar *path = NULL, *page = NULL, *link = NULL;
	(void)data;
	venture_entity_set_organization_id(plan, f->org);
	save(f, plan);
	path = g_strdup_printf("/e/plan/%" G_GINT64_FORMAT, venture_entity_get_id(plan));
	link = g_strdup_printf("/e/plan_price/new?plan_id=%" G_GINT64_FORMAT, venture_entity_get_id(plan));
	g_assert_cmpuint(request(f, "GET", path, NULL, NULL, &page), ==, 200);
	g_assert_nonnull(strstr(page, link));
	g_assert_nonnull(strstr(page, "no price yet"));
}

/* List rows must carry the facts a dispatcher reads on a phone, even when
 * the optional contact and property are populated. */
static void
test_phone_jobs_list(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) job = g_object_new(VENTURE_TYPE_JOB, "title", "Repair tap",
		"company-id", f->company, "service-address", "56 Example Lane", NULL);
	g_autoptr(GDateTime) start = g_date_time_new_from_iso8601("2026-11-12T09:00:00Z", NULL);
	g_autofree gchar *page = NULL;
	(void)data;
	venture_entity_set_organization_id(job, f->org);
	g_object_set(job, "window-start", start, NULL);
	save(f, job);
	g_assert_cmpuint(request(f, "GET", "/e/job", NULL, NULL, &page), ==, 200);
	g_assert_nonnull(strstr(page, "data-label=\"Service address\""));
	g_assert_nonnull(strstr(page, "56 Example Lane"));
	g_assert_nonnull(strstr(page, "data-label=\"Customer\""));
	g_assert_nonnull(strstr(page, "Example Household"));
	g_assert_nonnull(strstr(page, "2026-11-12"));
}

static gint64
issued_invoice(Fixture *f)
{
	g_autofree gchar *form = compose_form(f, FALSE), *path = NULL;
	g_autoptr(GPtrArray) invoices = NULL;
	gint64 id;
	g_assert_cmpuint(request(f, "POST", "/invoices/compose", form, NULL, NULL) / 100, ==, 3);
	invoices = rows(f, VENTURE_TYPE_INVOICE);
	id = venture_entity_get_id(g_ptr_array_index(invoices, 0));
	path = g_strdup_printf("/invoices/%" G_GINT64_FORMAT "/status", id);
	g_assert_cmpuint(request(f, "POST", path, "to=sent", NULL, NULL) / 100, ==, 3);
	return id;
}

/* Recording cash is a reviewed form, not an irreversible one-tap receipt.
 * The generated save must still perform a real partial allocation. */
static void
test_invoice_payment_options(Fixture *f, gconstpointer data)
{
	gint64 id = issued_invoice(f);
	g_autofree gchar *path = g_strdup_printf("/e/invoice/%" G_GINT64_FORMAT, id);
	g_autofree gchar *page = NULL, *form_page = NULL, *form_path = NULL, *form = NULL;
	g_autoptr(GPtrArray) payments = NULL, allocations = NULL;
	g_autoptr(VentureMoney) balance = NULL;
	g_autoptr(GDateTime) today = venture_settlement_service_today(venture_settlement_service_get(f->db));
	g_autofree gchar *date = g_date_time_format(today, "%Y-%m-%d");
	(void)data;
	g_assert_cmpuint(request(f, "GET", path, NULL, NULL, &page), ==, 200);
	g_assert_nonnull(strstr(page, "Record payment</a>"));
	g_assert_null(strstr(page, "value=\"paid\""));
	form_path = g_strdup_printf("/e/payment/new?invoice_id=%" G_GINT64_FORMAT "&organization_id=%" G_GINT64_FORMAT, id, f->org);
	g_assert_cmpuint(request(f, "GET", form_path, NULL, NULL, &form_page), ==, 200);
	g_assert_nonnull(strstr(form_page, "<select name=\"method\""));
	g_assert_nonnull(strstr(form_page, "value=\"check\""));
	g_assert_nonnull(strstr(form_page, "1450.00"));
	g_assert_nonnull(strstr(form_page, date));
	form = g_strdup_printf("customer-id=%" G_GINT64_FORMAT "&invoice-id=%" G_GINT64_FORMAT
		"&organization_id=%" G_GINT64_FORMAT "&amount=400+USD&date=%s&method=check&reference=Example+receipt", f->company, id, f->org, date);
	g_assert_cmpuint(request(f, "POST", "/e/payment", form, NULL, NULL) / 100, ==, 3);
	payments = rows(f, VENTURE_TYPE_PAYMENT);
	g_assert_cmpuint(payments->len, ==, 1);
	balance = venture_settlement_service_invoice_balance(venture_settlement_service_get(f->db), id, NULL, NULL);
	g_assert_cmpint(venture_money_get_amount(balance), ==, 105000);
	g_clear_pointer(&page, g_free);
	g_assert_cmpuint(request(f, "GET", path, NULL, NULL, &page), ==, 200);
	g_assert_nonnull(strstr(page, "Record refund</a>"));
	g_assert_nonnull(strstr(page, "Example receipt"));
	/* A later full settlement retains the receipt history and its refund
	 * path; a recorded refund reopens the invoice through the same service. */
	g_free(form);
	form = g_strdup_printf("customer-id=%" G_GINT64_FORMAT "&invoice-id=%" G_GINT64_FORMAT
		"&organization_id=%" G_GINT64_FORMAT "&amount=1050+USD&date=%s&method=cash", f->company, id, f->org, date);
	g_assert_cmpuint(request(f, "POST", "/e/payment", form, NULL, NULL) / 100, ==, 3);
	g_clear_pointer(&page, g_free);
	g_assert_cmpuint(request(f, "GET", path, NULL, NULL, &page), ==, 200);
	g_assert_nonnull(strstr(page, "Record refund</a>"));
	g_assert_null(strstr(page, "Record payment</a>"));
	allocations = rows(f, VENTURE_TYPE_PAYMENT_ALLOCATION);
	g_assert_cmpuint(allocations->len, ==, 2);
	g_free(form);
	form = g_strdup_printf("customer-id=%" G_GINT64_FORMAT "&allocation-id=%" G_GINT64_FORMAT
		"&organization_id=%" G_GINT64_FORMAT "&amount=400+USD&date=%s&reference=Returned+receipt",
		f->company, venture_entity_get_id(g_ptr_array_index(allocations, 0)), f->org, date);
	g_assert_cmpuint(request(f, "POST", "/e/refund", form, NULL, NULL) / 100, ==, 3);
	g_clear_pointer(&balance, venture_money_free);
	balance = venture_settlement_service_invoice_balance(venture_settlement_service_get(f->db), id, NULL, NULL);
	g_assert_cmpint(venture_money_get_amount(balance), ==, 40000);
}

/* The customer needs the work location before a job exists. It is distinct
 * from their billing address and must be escaped on the bearer-link page. */
static void
test_quote_service_address(Fixture *f, gconstpointer data)
{
	g_autofree gchar *base = compose_form(f, FALSE);
	g_autofree gchar *form = g_strconcat(base, "&service-address=56+Example+Lane+%3Cwest%3E", NULL);
	g_autoptr(GPtrArray) quotes = NULL;
	g_autoptr(VentureEntity) quote = NULL;
	g_autofree gchar *share = NULL, *token = NULL, *path = NULL, *page = NULL;
	(void)data;
	g_assert_cmpuint(request(f, "POST", "/quotes/compose", form, NULL, NULL) / 100, ==, 3);
	quotes = rows(f, VENTURE_TYPE_QUOTE);
	share = g_strdup_printf("/quotes/%" G_GINT64_FORMAT "/share", venture_entity_get_id(g_ptr_array_index(quotes, 0)));
	g_assert_cmpuint(request(f, "POST", share, "", NULL, NULL) / 100, ==, 3);
	quote = venture_database_get(f->db, VENTURE_TYPE_QUOTE, venture_entity_get_id(g_ptr_array_index(quotes, 0)), NULL);
	g_object_get(quote, "acceptance-token", &token, NULL);
	path = g_strconcat("/q/", token, NULL);
	g_assert_cmpuint(request(f, "GET", path, NULL, NULL, &page), ==, 200);
	g_assert_nonnull(strstr(page, "Service address"));
	g_assert_nonnull(strstr(page, "56 Example Lane &lt;west&gt;"));
	g_assert_null(strstr(page, "56 Example Lane <west>"));
	{
		g_autoptr(VentureEntity) job = g_object_new(VENTURE_TYPE_JOB, "title", "Quoted repair",
			"company-id", f->company, "quote-id", venture_entity_get_id(quote), NULL);
		g_autofree gchar *address = NULL;
		venture_entity_set_organization_id(job, f->org);
		save(f, job);
		g_object_get(job, "service-address", &address, NULL);
		g_assert_cmpstr(address, ==, "56 Example Lane <west>");
	}
}

/* Legacy links must reach the same priced composer as the menu. */
static void
test_one_plan_form(Fixture *f, gconstpointer data)
{
	g_autofree gchar *location = NULL, *page = NULL;
	(void)data;
	g_assert_cmpuint(request(f, "GET", "/e/plan/new", NULL, &location, NULL), ==, 303);
	g_assert_cmpstr(location, ==, "/plans/new");
	g_assert_cmpuint(request(f, "GET", location, NULL, NULL, &page), ==, 200);
	g_assert_nonnull(strstr(page, "name=\"price-0-amount\""));
	g_assert_null(strstr(page, "name=\"venture-id\" required"));
}

/* Model a pre-address database, then migrate twice: old quotes do not
 * acquire a guessed billing address and an entered work address survives. */
static void
test_quote_address_upgrade(Fixture *f, gconstpointer data)
{
	gint64 id = draft_quote(f);
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) quote = NULL;
	g_autofree gchar *address = NULL;
	(void)data;
	g_assert_true(venture_database_execute(f->db,
		"ALTER TABLE quotes DROP COLUMN service_address;"
		"DELETE FROM schema_migrations WHERE version = 943", NULL, &error));
	g_assert_no_error(error);
	/* Existing tables must gain the column even while quotes are hidden. */
	venture_entity_registry_set_type_module(venture_entity_registry_get_default(), "quote", "quotes", FALSE);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	venture_entity_registry_set_type_module(venture_entity_registry_get_default(), "quote", "quotes", TRUE);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	quote = venture_database_get(f->db, VENTURE_TYPE_QUOTE, id, &error);
	g_assert_no_error(error);
	g_assert_nonnull(quote);
	g_object_get(quote, "service-address", &address, NULL);
	g_assert_true(address == NULL || *address == '\0');
	g_object_set(quote, "service-address", "56 Example Lane", NULL);
	save(f, quote);
	g_clear_object(&quote);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	quote = venture_database_get(f->db, VENTURE_TYPE_QUOTE, id, &error);
	g_assert_no_error(error);
	g_clear_pointer(&address, g_free);
	g_object_get(quote, "service-address", &address, NULL);
	g_assert_cmpstr(address, ==, "56 Example Lane");
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/home-services/accepted-quote-invoice-due", Fixture, NULL, setup, test_accepted_quote_invoice_due, teardown);
	g_test_add("/home-services/printed-invoice-names-parties", Fixture, NULL, setup, test_printed_invoice_names_parties, teardown);
	g_test_add("/home-services/printed-quote-names-parties", Fixture, NULL, setup, test_printed_quote_names_parties, teardown);
	g_test_add("/home-services/search-by-phone", Fixture, NULL, setup, test_search_by_phone, teardown);
	g_test_add("/home-services/new-user-can-work", Fixture, NULL, setup, test_new_user_can_work, teardown);
	g_test_add("/home-services/owner-matches-user", Fixture, NULL, setup, test_owner_matches_user, teardown);
	g_test_add("/home-services/plan-add-price", Fixture, NULL, setup, test_plan_add_price, teardown);
	g_test_add("/home-services/phone-jobs-list", Fixture, NULL, setup, test_phone_jobs_list, teardown);
	g_test_add("/home-services/invoice-payment-options", Fixture, NULL, setup, test_invoice_payment_options, teardown);
	g_test_add("/home-services/quote-service-address", Fixture, NULL, setup, test_quote_service_address, teardown);
	g_test_add("/home-services/one-plan-form", Fixture, NULL, setup, test_one_plan_form, teardown);
	g_test_add("/home-services/quote-address-upgrade", Fixture, NULL, setup, test_quote_address_upgrade, teardown);
	return g_test_run();
}
