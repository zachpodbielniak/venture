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

/* The prices a plan offers, active or not, oldest first. */
static GPtrArray *
plan_prices(Fixture *f, gint64 plan)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_PLAN_PRICE);
	venture_query_add_filter_int(query, "plan-id", VENTURE_FILTER_OP_EQ, plan, NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	return venture_database_find(f->database, query, NULL);
}

/*
 * A plan is made with its prices -- one per period -- on one page, and
 * is offered to a new subscription the moment it is saved. If this
 * regresses, a plan is a name with nothing to charge, made on one page,
 * priced on another, and missing from the one place it is needed.
 */
static void
test_plan_sheet(Fixture *f, gconstpointer data)
{
	g_autofree gchar *page = NULL, *body = NULL, *location = NULL, *name = NULL;
	g_autoptr(GPtrArray) prices = NULL;
	g_autoptr(VentureEntity) plan = NULL;
	gboolean active = FALSE;
	gint64 plan_id;

	g_autoptr(VentureEntity) studio = g_object_new(VENTURE_TYPE_VENTURE, "name", "Harrow Studio", "slug", "harrow", NULL);
	g_autofree gchar *form = NULL, *option = NULL;
	gint64 venture = 0;

	(void)data;
	save(f, studio);
	g_assert_cmpuint(request(f, "GET", "/plans/new", NULL, NULL, &page), ==, 200);
	g_assert_nonnull(strstr(page, "name=\"price-0-interval\""));
	/* The plan is a venture's product: the sheet asks whose. */
	g_assert_nonnull(strstr(page, "<select name=\"venture-id\""));
	g_assert_nonnull(strstr(page, "Shared by every venture"));
	g_assert_nonnull(strstr(page, "<option value=\"quarter\">Every 3 months</option>"));

	form = g_strdup_printf("venture-id=%" G_GINT64_FORMAT "&name=Growth&description=For+teams"
		"&price-0-interval=month&price-0-amount=49&price-0-per-seat=true"
		"&price-2-interval=year&price-2-amount=490&price-2-per-seat=true&price-2-trial-days=14",
		venture_entity_get_id(studio));
	g_assert_cmpuint(request(f, "POST", "/plans/new", "application/x-www-form-urlencoded",
		form, &body), ==, 303);

	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_PLAN);
		venture_query_add_filter_string(query, "name", VENTURE_FILTER_OP_EQ, "Growth", NULL);
		plan = venture_database_find_one(f->database, query, NULL);
	}
	g_assert_nonnull(plan);
	g_object_get(plan, "active", &active, "venture-id", &venture, NULL);
	g_assert_true(active);
	g_assert_cmpint(venture, ==, venture_entity_get_id(studio));
	plan_id = venture_entity_get_id(plan);
	prices = plan_prices(f, plan_id);
	g_assert_cmpuint(prices->len, ==, 2);
	name = venture_entity_get_display_name(g_ptr_array_index(prices, 1));
	g_assert_cmpstr(name, ==, "$490.00 a year per seat");

	/* Offered straight away, under the plan's name. */
	g_clear_pointer(&page, g_free);
	g_assert_cmpuint(request(f, "GET", "/billing/subscriptions/new", NULL, NULL, &page), ==, 200);
	g_assert_nonnull(strstr(page, "<optgroup label=\"Growth\">"));
	g_assert_nonnull(strstr(page, "$49.00 a month per seat</option>"));
	/* Each price says whose it is, so choosing a customer narrows them. */
	option = g_strdup_printf("data-venture=\"%" G_GINT64_FORMAT "\" data-plan=", venture_entity_get_id(studio));
	g_assert_nonnull(strstr(page, option));
}

/*
 * A plan's page lists its prices and adds one for another period, and a
 * price that is retired stops being offered without touching the
 * customers already on it. A plan with no price yet is named on the new
 * subscription page with a way to price it, not silently left out.
 */
static void
test_plan_prices(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) plan = NULL;
	g_autoptr(GPtrArray) prices = NULL;
	g_autofree gchar *page = NULL, *body = NULL, *path = NULL, *form = NULL, *retire = NULL;
	gboolean active = TRUE;

	(void)data;
	plan = g_object_new(VENTURE_TYPE_PLAN, "name", "Unpriced", "code", "unpriced", "active", TRUE, NULL);
	save(f, plan);

	g_assert_cmpuint(request(f, "GET", "/billing/subscriptions/new", NULL, NULL, &page), ==, 200);
	g_assert_nonnull(strstr(page, "Unpriced"));
	g_assert_nonnull(strstr(page, "has no price yet"));

	/* A price made on the ordinary form starts offered. */
	g_clear_pointer(&page, g_free);
	g_assert_cmpuint(request(f, "GET", "/e/plan_price/new", NULL, NULL, &page), ==, 200);
	g_assert_nonnull(strstr(page, "name=\"active\" value=\"true\" checked"));

	path = g_strdup_printf("/e/plan/%" G_GINT64_FORMAT, venture_entity_get_id(plan));
	g_clear_pointer(&page, g_free);
	g_assert_cmpuint(request(f, "GET", path, NULL, NULL, &page), ==, 200);
	g_assert_nonnull(strstr(page, "Add a price"));

	form = g_strdup_printf("/plans/%" G_GINT64_FORMAT "/prices", venture_entity_get_id(plan));
	g_assert_cmpuint(request(f, "POST", form, "application/x-www-form-urlencoded",
		"action=add&interval=half_year&amount=150", &body), ==, 303);
	prices = plan_prices(f, venture_entity_get_id(plan));
	g_assert_cmpuint(prices->len, ==, 1);

	retire = g_strdup_printf("action=retire&price=%" G_GINT64_FORMAT,
		venture_entity_get_id(g_ptr_array_index(prices, 0)));
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(request(f, "POST", form, "application/x-www-form-urlencoded", retire, &body), ==, 303);
	g_clear_pointer(&prices, g_ptr_array_unref);
	prices = plan_prices(f, venture_entity_get_id(plan));
	g_object_get(g_ptr_array_index(prices, 0), "active", &active, NULL);
	g_assert_false(active);
}

/*
 * A plan's page offers discounts: "20%" or "10" off, for the first N
 * periods or every one. The subscription pages offer the plan's
 * discounts and a way to skip a free trial, and a subscription's page
 * lists the invoices it has issued. If this regresses, a discount is
 * typed as two fields and a subscription's bills are nowhere on its page.
 */
static void
test_plan_discounts(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) plan = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) discounts = NULL;
	g_autoptr(VentureMoney) amount = NULL;
	g_autofree gchar *page = NULL, *body = NULL, *path = NULL, *form = NULL, *sub = NULL;
	gint64 percent = 0, periods = 0;

	(void)data;
	{
		g_autoptr(VentureQuery) q = venture_query_new(VENTURE_TYPE_PLAN);
		plan = venture_database_find_one(f->database, q, NULL);
	}
	g_assert_nonnull(plan);
	path = g_strdup_printf("/e/plan/%" G_GINT64_FORMAT, venture_entity_get_id(plan));
	g_assert_cmpuint(request(f, "GET", path, NULL, NULL, &page), ==, 200);
	g_assert_nonnull(strstr(page, "Discounts"));
	g_assert_nonnull(strstr(page, "Add a discount"));

	form = g_strdup_printf("/plans/%" G_GINT64_FORMAT "/discounts", venture_entity_get_id(plan));
	g_assert_cmpuint(request(f, "POST", form, "application/x-www-form-urlencoded",
		"action=add&name=Launch+offer&off=20%25&periods=3&code=LAUNCH", &body), ==, 303);
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(request(f, "POST", form, "application/x-www-form-urlencoded",
		"action=add&name=Loyalty&off=10", &body), ==, 303);
	query = venture_query_new(VENTURE_TYPE_PLAN_DISCOUNT);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	discounts = venture_database_find(f->database, query, NULL);
	g_assert_cmpuint(discounts->len, ==, 2);
	g_object_get(g_ptr_array_index(discounts, 0), "percent-off", &percent, "periods", &periods, NULL);
	g_assert_cmpint(percent, ==, 20);
	g_assert_cmpint(periods, ==, 3);
	g_object_get(g_ptr_array_index(discounts, 1), "amount-off", &amount, "periods", &periods, NULL);
	g_assert_nonnull(amount);
	g_assert_cmpint(venture_money_get_amount(amount), ==, 1000);
	g_assert_cmpint(periods, ==, 0);

	/* An amount that is neither is refused, in words. */
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(request(f, "POST", form, "application/x-www-form-urlencoded",
		"action=add&name=Bad&off=lots", &body), ==, 422);

	g_clear_pointer(&page, g_free);
	g_assert_cmpuint(request(f, "GET", "/billing/subscriptions/new", NULL, NULL, &page), ==, 200);
	g_assert_nonnull(strstr(page, "name=\"discount-id\""));
	g_assert_nonnull(strstr(page, "Launch offer \xe2\x80\x94 20% off the first 3 periods"));
	g_assert_nonnull(strstr(page, "name=\"skip-trial\""));

	/* The fixture's subscription was started without a trial, so its
	 * first invoice is listed on its page. */
	sub = g_strdup_printf("/e/customer_subscription/%" G_GINT64_FORMAT, f->subscription);
	g_clear_pointer(&page, g_free);
	g_assert_cmpuint(request(f, "GET", sub, NULL, NULL, &page), ==, 200);
	g_assert_nonnull(strstr(page, "<h2>Invoices</h2>"));
	g_assert_nonnull(strstr(page, "href=\"/e/invoice/"));
}

/*
 * Cancelling says what it does to money on the button: now, with the
 * service's own figure for the days left, or at renewal with nothing more
 * billed. If this regresses, somebody cancels without knowing whether the
 * customer is owed anything, or the page promises a different credit than
 * the credit note.
 */
static void
test_cancel_buttons(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureBillingRequest) action = venture_billing_request_new();
	g_autoptr(VentureEntity) sub = NULL;
	g_autoptr(VentureMoney) credit = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(GDateTime) began = g_date_time_add_days(now, -3);
	g_autofree gchar *page = NULL, *path = NULL, *money = NULL, *expected = NULL, *body = NULL, *post = NULL;
	gint64 company = 0, price = 0, id = 0, days = 0;

	(void)data;
	path = g_strdup_printf("/e/customer_subscription/%" G_GINT64_FORMAT, f->subscription);
	g_assert_cmpuint(request(f, "GET", path, NULL, NULL, &page), ==, 200);
	/* The fixture's January period is long over: nothing is left to credit. */
	g_assert_nonnull(strstr(page, "Cancel now: nothing is credited"));
	g_assert_nonnull(strstr(page, "Cancel at renewal: nothing more is billed"));

	sub = venture_database_get(f->database, VENTURE_TYPE_CUSTOMER_SUBSCRIPTION, f->subscription, NULL);
	g_object_get(sub, "company-id", &company, "plan-price-id", &price, NULL);
	g_object_set(action, "action", "start", "company-id", company, "plan-price-id", price, "at", began, NULL);
	save(f, VENTURE_ENTITY(action));
	g_object_get(action, "subscription-id", &id, NULL);
	g_clear_object(&sub);
	sub = venture_database_get(f->database, VENTURE_TYPE_CUSTOMER_SUBSCRIPTION, id, NULL);
	credit = venture_billing_service_cancel_credit(venture_billing_service_get(f->database),
		VENTURE_CUSTOMER_SUBSCRIPTION(sub), now, &days, NULL);
	g_assert_nonnull(credit);
	g_assert_cmpint(venture_money_get_amount(credit), >, 0);
	money = venture_money_to_display_string(credit, TRUE);
	expected = g_strdup_printf("Cancel now: %s credited for the %" G_GINT64_FORMAT " days left", money, days);
	g_clear_pointer(&path, g_free);
	g_clear_pointer(&page, g_free);
	path = g_strdup_printf("/e/customer_subscription/%" G_GINT64_FORMAT, id);
	g_assert_cmpuint(request(f, "GET", path, NULL, NULL, &page), ==, 200);
	g_assert_nonnull(strstr(page, expected));

	/* Pressing it issues exactly that credit. */
	post = g_strdup_printf("/billing/subscriptions/%" G_GINT64_FORMAT "/action", id);
	g_assert_cmpuint(request(f, "POST", post, "application/x-www-form-urlencoded",
		"billing_action=cancel&at-period-end=false", &body), ==, 303);
	{
		g_autoptr(VentureQuery) q = venture_query_new(VENTURE_TYPE_CUSTOMER_CREDIT);
		g_autoptr(VentureEntity) note = venture_database_find_one(f->database, q, NULL);
		g_autoptr(VentureMoney) amount = NULL;
		g_assert_nonnull(note);
		g_object_get(note, "amount", &amount, NULL);
		g_assert_true(venture_money_equal(amount, credit));
	}
}

/*
 * The plan sheet and the plan page's "Add a price" offer a Tax select --
 * "No tax" first, then each active rate with its percent -- and the price
 * saved carries the rate. If this regresses, a price can only be taxed by
 * hand through the generic form, and nobody finds it.
 */
static void
test_plan_tax(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureTaxCode) code = venture_tax_code_new();
	g_autoptr(GPtrArray) prices = NULL;
	g_autoptr(VentureEntity) plan = NULL;
	g_autofree gchar *page = NULL, *body = NULL, *form = NULL, *post = NULL;
	gint64 code_id, tax = 0;

	(void)data;
	g_object_set(code, "code", "STD", "name", "Standard", "rate-numerator", (gint64)10,
		"rate-denominator", (gint64)100, "active", TRUE, NULL);
	save(f, VENTURE_ENTITY(code));
	code_id = venture_entity_get_id(VENTURE_ENTITY(code));
	g_assert_cmpuint(request(f, "GET", "/plans/new", NULL, NULL, &page), ==, 200);
	g_assert_nonnull(strstr(page, "name=\"price-0-tax-code-id\""));
	g_assert_nonnull(strstr(page, "<option value=\"\" data-rate=\"0\">No tax</option>"));
	g_assert_nonnull(strstr(page, "Standard \xc2\xb7 10%"));

	post = g_strdup_printf("name=Taxed&venture-id=0&price-0-interval=month&price-0-amount=20&price-0-tax-code-id=%"
		G_GINT64_FORMAT, code_id);
	g_assert_cmpuint(request(f, "POST", "/plans/new", "application/x-www-form-urlencoded", post, &body), ==, 303);
	{
		g_autoptr(VentureQuery) q = venture_query_new(VENTURE_TYPE_PLAN);
		venture_query_add_filter_string(q, "name", VENTURE_FILTER_OP_EQ, "Taxed", NULL);
		plan = venture_database_find_one(f->database, q, NULL);
	}
	g_assert_nonnull(plan);
	prices = plan_prices(f, venture_entity_get_id(plan));
	g_assert_cmpuint(prices->len, ==, 1);
	g_object_get(g_ptr_array_index(prices, 0), "tax-code-id", &tax, NULL);
	g_assert_cmpint(tax, ==, code_id);

	g_clear_pointer(&page, g_free);
	form = g_strdup_printf("/e/plan/%" G_GINT64_FORMAT, venture_entity_get_id(plan));
	g_assert_cmpuint(request(f, "GET", form, NULL, NULL, &page), ==, 200);
	g_assert_nonnull(strstr(page, "<select name=\"tax-code-id\" data-no-picker><option value=\"\" data-rate=\"0\">No tax</option>"));
	g_clear_pointer(&form, g_free);
	g_clear_pointer(&body, g_free);
	form = g_strdup_printf("/plans/%" G_GINT64_FORMAT "/prices", venture_entity_get_id(plan));
	g_assert_cmpuint(request(f, "POST", form, "application/x-www-form-urlencoded",
		"action=add&interval=year&amount=200&tax-code-id=", &body), ==, 303);
	g_ptr_array_unref(prices);
	prices = plan_prices(f, venture_entity_get_id(plan));
	g_assert_cmpuint(prices->len, ==, 2);
	g_object_get(g_ptr_array_index(prices, 1), "tax-code-id", &tax, NULL);
	g_assert_cmpint(tax, ==, 0);
}

/*
 * A retired price with customers offers to move them to one of the plan's
 * offered prices, at renewal or now; the move is the single-change path
 * for each, and a refusal is shown in words. If this regresses, retiring a
 * price strands its customers on it with no way off but one at a time.
 */
static void
test_plan_move(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) sub = venture_database_get(f->database, VENTURE_TYPE_CUSTOMER_SUBSCRIPTION, f->subscription, NULL);
	g_autoptr(VentureEntity) old = NULL;
	g_autoptr(VenturePlanPrice) next = venture_plan_price_new();
	g_autofree gchar *page = NULL, *body = NULL, *path = NULL, *form = NULL, *post = NULL;
	gint64 old_id = 0, plan = 0, pending = 0;

	(void)data;
	g_object_get(sub, "plan-price-id", &old_id, NULL);
	old = venture_database_get(f->database, VENTURE_TYPE_PLAN_PRICE, old_id, NULL);
	g_object_get(old, "plan-id", &plan, NULL);
	g_object_set(next, "plan-id", plan, "currency", "USD", "active", TRUE, NULL);
	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(next), "amount", "40 USD", NULL));
	save(f, VENTURE_ENTITY(next));
	g_object_set(old, "active", FALSE, NULL);
	save(f, old);

	path = g_strdup_printf("/e/plan/%" G_GINT64_FORMAT, plan);
	g_assert_cmpuint(request(f, "GET", path, NULL, NULL, &page), ==, 200);
	g_assert_nonnull(strstr(page, "Move its 1 customer to"));
	form = g_strdup_printf("/plans/%" G_GINT64_FORMAT "/prices/%" G_GINT64_FORMAT "/move", plan, old_id);
	g_assert_nonnull(strstr(page, form));

	/* Moving onto the retired price itself is refused, in words. */
	post = g_strdup_printf("to=%" G_GINT64_FORMAT "&at-period-end=true", old_id);
	g_assert_cmpuint(request(f, "POST", form, "application/x-www-form-urlencoded", post, &body), ==, 422);
	g_assert_nonnull(strstr(body, "another price of the same plan"));

	/* The fixture's subscription has not been renewed since January, so
	 * it cannot change terms today: the refusal names the customer. */
	g_clear_pointer(&post, g_free);
	g_clear_pointer(&body, g_free);
	post = g_strdup_printf("to=%" G_GINT64_FORMAT "&at-period-end=true", venture_entity_get_id(VENTURE_ENTITY(next)));
	g_assert_cmpuint(request(f, "POST", form, "application/x-www-form-urlencoded", post, &body), ==, 422);
	g_assert_nonnull(strstr(body, "Nobody was moved: Lightsite customer could not be moved"));

	{
		g_autoptr(VentureBillingRequest) sweep = venture_billing_request_new();
		g_autoptr(GDateTime) now = venture_time_now();
		g_object_set(sweep, "action", "renew-sweep", "at", now, NULL);
		save(f, VENTURE_ENTITY(sweep));
	}
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(request(f, "POST", form, "application/x-www-form-urlencoded", post, &body), ==, 303);
	g_clear_object(&sub);
	sub = venture_database_get(f->database, VENTURE_TYPE_CUSTOMER_SUBSCRIPTION, f->subscription, NULL);
	g_object_get(sub, "pending-plan-price-id", &pending, NULL);
	g_assert_cmpint(pending, ==, venture_entity_get_id(VENTURE_ENTITY(next)));
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
	/* The page offers the subscription's actions, and leads with what
	 * happens next. */
	g_assert_nonnull(strstr(body, "name=\"billing_action\""));
	g_assert_nonnull(strstr(body, "What happens next"));
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
	g_test_add("/billing-surfaces/plan-sheet", Fixture, NULL, setup, test_plan_sheet, teardown);
	g_test_add("/billing-surfaces/plan-prices", Fixture, NULL, setup, test_plan_prices, teardown);
	g_test_add("/billing-surfaces/plan-discounts", Fixture, NULL, setup, test_plan_discounts, teardown);
	g_test_add("/billing-surfaces/staged", Fixture, NULL, setup, test_staged, teardown);
	g_test_add("/billing-surfaces/cli", Fixture, NULL, setup, test_cli, teardown);
	g_test_add("/billing-surfaces/assistant-stale", Fixture, NULL, setup, test_assistant_stale, teardown);
	g_test_add("/billing-surfaces/report-days", Fixture, NULL, setup, test_report_days, teardown);
	g_test_add("/billing-surfaces/cancel-buttons", Fixture, NULL, setup, test_cancel_buttons, teardown);
	g_test_add("/billing-surfaces/plan-tax", Fixture, NULL, setup, test_plan_tax, teardown);
	g_test_add("/billing-surfaces/plan-move", Fixture, NULL, setup, test_plan_move, teardown);
	return g_test_run();
}
