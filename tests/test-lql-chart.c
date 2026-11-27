/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * test-lql-chart.c - The Chart toggle on list pages
 *
 * Copyright (C) 2026 Zach Podbielniak
 *
 * What the rest of ticket #9 of the LQL epic promises (docs/lql.org,
 * "Charts on list pages"), and what breaks if it regresses:
 *
 *   - /e/<type>?view=chart and /tickets?view=chart draw the page's own
 *     records as a chart: the URL's field filters and the bar's q, then a
 *     summarize by the field picked. The counts are the list's rows.
 *   - Money is one series per currency, never one figure across them.
 *   - The chart reads what the list shows its caller, and nothing more.
 *   - A bad pick, and a query past query.max_rows or query.max_bytes, is
 *     a problem drawn in the bar (400), never a 500.
 *   - With scripting off, the controls are a GET form that carries the
 *     page's filters and comes back as the same chart.
 */

#include <venture.h>
#include <libsoup/soup.h>
#include <string.h>
#include "venture-test-util.h"

#define TEST_PASSWORD "chart-long-password"

/* A filter whose every row builds 600 kB: past a 1 MiB query.max_bytes. */
#define FILTER_OVER_BUDGET "length(concat(title, repeat(\"x\", 600000))) > 0"

/* --- The fixture ----------------------------------------------------------- */

typedef struct
{
	gchar			*directory;
	VentureConfig		*config;
	VentureDatabase		*db;
	VentureContext		*context;
	VentureWebServer	*server;
	SoupSession		*session;
	gint64			 org;
	gint64			 ada_id;
	gint64			 ana_id;
	gint64			 fin_id;
	gchar			*cookie;
	gchar			*ada_secret;
} Fixture;

/* Read workers or not: the chart reads the same either way. */
typedef struct
{
	gboolean	workers;
} Variant;

static const Variant plain = { FALSE };
static const Variant with_workers = { TRUE };

/* The tickets: three to do, two in progress, one done; ada has three. */
typedef struct
{
	const gchar		*title;
	VentureTicketStatus	 status;
	VenturePriority		 priority;
	const gchar		*assignee;
} TicketSeed;

static const TicketSeed tickets[] = {
	{ "Crash on save",  VENTURE_TICKET_STATUS_TODO,        VENTURE_PRIORITY_HIGH,   "ada" },
	{ "Slow search",    VENTURE_TICKET_STATUS_TODO,        VENTURE_PRIORITY_NORMAL, "ana" },
	{ "Typo on login",  VENTURE_TICKET_STATUS_TODO,        VENTURE_PRIORITY_HIGH,   "ada" },
	{ "Old outage",     VENTURE_TICKET_STATUS_IN_PROGRESS, VENTURE_PRIORITY_NORMAL, "ada" },
	{ "Nobody's job",   VENTURE_TICKET_STATUS_IN_PROGRESS, VENTURE_PRIORITY_HIGH,   NULL  },
	{ "Shipped thing",  VENTURE_TICKET_STATUS_DONE,        VENTURE_PRIORITY_NORMAL, "ana" }
};

/* The invoices: shipping in two currencies, 15.00 USD and 7.00 EUR. */
typedef struct
{
	const gchar	*number;
	gint64		 shipping;
	const gchar	*currency;
	const gchar	*owner;
} InvoiceSeed;

static const InvoiceSeed invoices[] = {
	{ "CHART-1", 1000, "USD", "ada" },
	{ "CHART-2",  500, "USD", "fin" },
	{ "CHART-3",  700, "EUR", "fin" }
};

/* A user with a password, a member of the business in @org_role. */
static gint64
fixture_user(
	Fixture		*f,
	const gchar	*username,
	VentureUserRole	 role,
	gint		 org_role
){
	g_autoptr(VentureUser) user = NULL;
	g_autoptr(VentureEntity) member = NULL;
	gint64 id;

	user = venture_user_new();
	g_object_set(user, "username", username, "role", role, "active", TRUE, NULL);
	g_assert_true(venture_user_set_password(user, TEST_PASSWORD, 100000, NULL));
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(user), NULL, NULL));
	id = venture_entity_get_id(VENTURE_ENTITY(user));

	member = g_object_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP, "organization-id", f->org,
		"user-id", id, "active", TRUE, "role", org_role, NULL);
	g_assert_true(venture_database_save(f->db, member, NULL, NULL));

	return id;
}

/* An API token for @user_id; its secret. */
static gchar *
fixture_token(
	Fixture		*f,
	const gchar	*name,
	gint64		 user_id,
	VentureUserRole	 role
){
	g_autoptr(VentureApiToken) token = venture_api_token_new();
	gchar *secret;

	g_object_set(token, "name", name, "user-id", user_id, "role", role, NULL);
	secret = venture_api_token_generate(token);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(token), NULL, NULL));

	return secret;
}

typedef struct
{
	gboolean	 done;
	GBytes		*body;
	GError		*error;
} Outcome;

static void
request_done(
	GObject		*source,
	GAsyncResult	*result,
	gpointer	 data
){
	Outcome *outcome = data;

	outcome->body = soup_session_send_and_read_finish(SOUP_SESSION(source), result,
	                                                  &outcome->error);
	outcome->done = TRUE;
}

/*
 * One request to the fixture's server, driving the main context by hand
 * (the server runs on it): @cookie or @bearer as the credential, @form a
 * urlencoded POST body. Redirects are not followed.
 */
static guint
fixture_request(
	Fixture		 *f,
	const gchar	 *method,
	const gchar	 *path,
	const gchar	 *cookie,
	const gchar	 *bearer,
	const gchar	 *form,
	gchar		**out_body,
	gchar		**out_set_cookie
){
	g_autoptr(SoupMessage) message = NULL;
	g_autofree gchar *url = NULL;
	Outcome outcome = { FALSE, NULL, NULL };
	SoupMessageHeaders *headers;

	url = g_strconcat(venture_web_server_get_base_url(f->server), path, NULL);
	message = soup_message_new(method, url);
	g_assert_nonnull(message);
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	headers = soup_message_get_request_headers(message);

	if (NULL != cookie)
		soup_message_headers_append(headers, "Cookie", cookie);

	if (NULL != bearer)
	{
		g_autofree gchar *value = g_strconcat("Bearer ", bearer, NULL);

		soup_message_headers_append(headers, "Authorization", value);
	}

	if (NULL != form)
	{
		g_autoptr(GBytes) bytes = g_bytes_new(form, strlen(form));

		soup_message_set_request_body_from_bytes(message,
			"application/x-www-form-urlencoded", bytes);
	}

	soup_session_send_and_read_async(f->session, message, G_PRIORITY_DEFAULT, NULL,
	                                 request_done, &outcome);

	while (!outcome.done)
		g_main_context_iteration(NULL, TRUE);

	if (NULL != outcome.error)
		g_error("%s %s: %s", method, path, outcome.error->message);

	if (NULL != out_body)
		*out_body = g_strndup(g_bytes_get_data(outcome.body, NULL),
		                      g_bytes_get_size(outcome.body));

	if (NULL != out_set_cookie)
		*out_set_cookie = g_strdup(soup_message_headers_get_one(
			soup_message_get_response_headers(message), "Set-Cookie"));

	g_bytes_unref(outcome.body);

	return soup_message_get_status(message);
}

/* @username's browser session: the cookie to send. */
static gchar *
fixture_login(
	Fixture		*f,
	const gchar	*username
){
	g_autofree gchar *set_cookie = NULL;
	g_autofree gchar *login = NULL;
	gchar *semicolon;

	login = g_strdup_printf("username=%s&password=%s", username, TEST_PASSWORD);
	g_assert_cmpuint(fixture_request(f, "POST", "/login", NULL, NULL, login, NULL,
	                                 &set_cookie), ==, SOUP_STATUS_FOUND);
	g_assert_nonnull(set_cookie);
	semicolon = strchr(set_cookie, ';');

	if (NULL != semicolon)
		*semicolon = '\0';

	return g_steal_pointer(&set_cookie);
}

/* GET @path in @cookie's browser session, wanting @expect; the page. */
static gchar *
page_as(
	Fixture		*f,
	const gchar	*cookie,
	const gchar	*path,
	guint		 expect
){
	gchar *body = NULL;
	guint status;

	status = fixture_request(f, "GET", path, cookie, NULL, NULL, &body, NULL);

	if (status != expect)
		g_error("GET %s: %u, wanted %u\n%s", path, status, expect, body);

	return body;
}

/* GET @path in ada's browser session; the page. */
static gchar *
page(
	Fixture		*f,
	const gchar	*path,
	guint		 expect
){
	return page_as(f, f->cookie, path, expect);
}

/* How many records GET @path (a list API path) answers, as ada. */
static guint
api_count(
	Fixture		*f,
	const gchar	*path
){
	g_autofree gchar *body = NULL;
	g_autoptr(JsonNode) answer = NULL;
	guint status;

	status = fixture_request(f, "GET", path, NULL, f->ada_secret, NULL, &body, NULL);

	if (SOUP_STATUS_OK != status)
		g_error("GET %s: %u\n%s", path, status, body);

	answer = venture_json_parse(body, NULL);
	g_assert_nonnull(answer);

	return json_array_get_length(json_object_get_array_member(json_node_get_object(answer),
	                                                         "records"));
}

/* Saves @entity, failing the test on refusal. */
static void
persist(
	Fixture		*f,
	VentureEntity	*entity
){
	g_autoptr(GError) error = NULL;

	if (!venture_database_save(f->db, entity, NULL, &error))
		g_error("save %s: %s", G_OBJECT_TYPE_NAME(entity), error->message);
}

static void
fixture_setup(
	Fixture		*f,
	gconstpointer	 data
){
	const Variant *variant = data;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) customer = NULL;
	guint i;

	f->directory = g_dir_make_tmp("venture-lql-chart-XXXXXX", NULL);
	f->config = venture_config_new();
	g_object_set(f->config, "state-dir", f->directory, "server-bind-address", "127.0.0.1",
	             "server-port", (gint64)0, "security-require-auth", TRUE,
	             "security-session-secret-env", "VENTURE_TEST_SESSION_SECRET", NULL);

	if (variant->workers)
	{
		g_autofree gchar *uri = g_strdup_printf("sqlite://%s/venture.db", f->directory);

		g_object_set(f->config, "server-read-workers", (gint64)2, NULL);
		f->db = venture_database_new(uri, NULL);
	}
	else
		f->db = venture_database_new("sqlite://:memory:", NULL);

	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), NULL));
	f->context = venture_context_new(f->config, f->db);
	f->org = venture_context_get_default_organization_id(f->context);

	/* ada owns the business; ana edits it but is no finance member, so
	 * invoices are not hers to read; fin is the finance member. */
	f->ada_id = fixture_user(f, "ada", VENTURE_USER_ROLE_ADMIN, VENTURE_ORGANIZATION_ROLE_OWNER);
	f->ana_id = fixture_user(f, "ana", VENTURE_USER_ROLE_EDITOR, VENTURE_ORGANIZATION_ROLE_EDITOR);
	f->fin_id = fixture_user(f, "fin", VENTURE_USER_ROLE_EDITOR, VENTURE_ORGANIZATION_ROLE_FINANCE);
	f->ada_secret = fixture_token(f, "ada-token", f->ada_id, VENTURE_USER_ROLE_ADMIN);

	for (i = 0; i < G_N_ELEMENTS(tickets); i++)
	{
		g_autoptr(VentureEntity) ticket = NULL;

		ticket = g_object_new(VENTURE_TYPE_TICKET, "title", tickets[i].title,
			"organization-id", f->org, "status", tickets[i].status,
			"priority", tickets[i].priority, "assignee", tickets[i].assignee,
			"kind", VENTURE_TICKET_KIND_INTERNAL, NULL);
		persist(f, ticket);
	}

	customer = g_object_new(VENTURE_TYPE_COMPANY, "organization-id", f->org,
		"name", "Acme Anvils", "customer", TRUE, NULL);
	g_assert_true(venture_entity_set_field_from_string(customer, "kind", "customer", NULL));
	persist(f, customer);

	for (i = 0; i < G_N_ELEMENTS(invoices); i++)
	{
		g_autoptr(VentureEntity) invoice = NULL;
		g_autoptr(GDateTime) issued = g_date_time_new_now_utc();
		VentureMoney *shipping;

		shipping = venture_money_new_for_currency(invoices[i].shipping, invoices[i].currency);
		invoice = g_object_new(VENTURE_TYPE_INVOICE, "organization-id", f->org,
			"company-id", venture_entity_get_id(customer), "number", invoices[i].number,
			"issued-at", issued, "due-at", issued, "owner", invoices[i].owner,
			"shipping-amount", shipping, NULL);
		venture_money_free(shipping);
		persist(f, invoice);
	}

	f->server = venture_web_server_new(f->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(f->server, &error));
	g_assert_no_error(error);
	f->session = soup_session_new();
	f->cookie = fixture_login(f, "ada");
}

static void
fixture_teardown(
	Fixture		*f,
	gconstpointer	 data
){
	(void)data;

	if (NULL != f->server)
		venture_web_server_stop(f->server);

	g_clear_object(&f->session);
	g_clear_object(&f->server);
	g_clear_object(&f->context);
	g_clear_object(&f->db);
	g_clear_object(&f->config);
	g_clear_pointer(&f->cookie, g_free);
	g_clear_pointer(&f->ada_secret, g_free);
	venture_test_remove_tree(f->directory);
	g_clear_pointer(&f->directory, g_free);
}

/* --- Helpers ------------------------------------------------------------------ */

/* @text URL-escaped, for a query string. */
static gchar *
escaped(const gchar *text)
{
	return g_uri_escape_string(text, NULL, FALSE);
}

/* What a chart page drew, as the JSON it carries; fails without one. */
static JsonNode *
drawn(const gchar *html)
{
	static const gchar open[] = "<script type=\"application/json\" data-chart-data>";
	const gchar *start;
	const gchar *end;
	g_autofree gchar *text = NULL;
	JsonNode *node;

	start = strstr(html, open);

	if (NULL == start)
		g_error("no chart data on the page:\n%s", html);

	start += strlen(open);
	end = strstr(start, "</script>");
	g_assert_nonnull(end);
	text = g_strndup(start, (gsize)(end - start));
	node = venture_json_parse(text, NULL);
	g_assert_nonnull(node);

	return node;
}

/* The point labelled @label of @data's only group, as a count; -1 when
 * the chart has no such label. */
static gint64
count_of(
	JsonNode	*data,
	const gchar	*label
){
	JsonObject *root = json_node_get_object(data);
	JsonArray *labels = json_object_get_array_member(root, "labels");
	JsonArray *groups = json_object_get_array_member(root, "groups");
	JsonArray *values;
	guint i;

	g_assert_cmpuint(json_array_get_length(groups), ==, 1);
	values = json_object_get_array_member(json_array_get_object_element(groups, 0), "values");

	for (i = 0; i < json_array_get_length(labels); i++)
		if (0 == g_strcmp0(json_array_get_string_element(labels, i), label))
			return (gint64)json_node_get_double(json_array_get_element(values, i));

	return -1;
}

/* The message of the problem a page draws in its bar; NULL for none. */
static gchar *
problem_message(const gchar *html)
{
	const gchar *start;
	const gchar *end;

	start = strstr(html, "<p class=\"lql-problem-message\">");

	if (NULL == start)
		return NULL;

	end = strstr(start, "</p>");
	g_assert_nonnull(end);

	return g_strndup(start, (gsize)(end - start));
}

/* How many times @needle occurs in @haystack. */
static guint
count_substring(
	const gchar	*haystack,
	const gchar	*needle
){
	const gchar *p;
	guint n = 0;

	for (p = strstr(haystack, needle); NULL != p; p = strstr(p + strlen(needle), needle))
		n++;

	return n;
}

/* --- Counts --------------------------------------------------------------------- */

/*
 * /e/ticket?view=chart&chart_by=status counts what the list lists: each
 * status's bar is the number of records the list API gives for it, and
 * the subtitle is the list's own count. The bar and its chips stay, and
 * the toggle says Chart.
 *
 * Breaks if the chart reads a different set from the list (another
 * organization, a page of it), or the page stops drawing the bar.
 */
static void
test_counts_match_list(
	Fixture		*f,
	gconstpointer	 data
){
	static const gchar *const statuses[] = { "todo", "in_progress", "done", NULL };
	g_autofree gchar *html = NULL;
	g_autofree gchar *list = NULL;
	g_autoptr(JsonNode) chart = NULL;
	guint i;

	(void)data;

	html = page(f, "/e/ticket?view=chart&chart_by=status", 200);
	chart = drawn(html);

	for (i = 0; NULL != statuses[i]; i++)
	{
		g_autofree gchar *path = g_strdup_printf("/api/v1/ticket?status=%s", statuses[i]);

		g_assert_cmpint(count_of(chart, statuses[i]), ==, (gint64)api_count(f, path));
	}

	g_assert_cmpint(count_of(chart, "todo"), ==, 3);
	g_assert_cmpint(count_of(chart, "in_progress"), ==, 2);
	g_assert_cmpint(count_of(chart, "done"), ==, 1);

	/* The subtitle agrees with the table's. */
	list = page(f, "/e/ticket", 200);
	g_assert_nonnull(strstr(list, "6 records"));
	g_assert_nonnull(strstr(html, "6 records"));

	/* Bars by default, the bar and chips kept, the toggle on Chart, no
	 * table of rows. */
	g_assert_nonnull(strstr(html, "<figure class=\"chart chart-bar\">"));
	g_assert_nonnull(strstr(html, "data-lql-bar"));
	g_assert_nonnull(strstr(html, ">Mine</a>"));
	g_assert_nonnull(strstr(html, "class=\"seg active\" href=\"/e/ticket?chart_by=status&amp;view=chart\""));
	g_assert_null(strstr(html, "data-list"));

	/* The table's page links to the chart. */
	g_assert_nonnull(strstr(list, "href=\"/e/ticket?view=chart\" data-chart-toggle"));

	/* Split by a date: one bucket, this month, all six. */
	{
		g_autoptr(GDateTime) now = g_date_time_new_now_utc();
		g_autofree gchar *month = g_date_time_format(now, "%Y-%m");
		g_autofree gchar *dated = page(f, "/e/ticket?view=chart&chart_by=created_at:month", 200);
		g_autoptr(JsonNode) buckets = drawn(dated);

		g_assert_cmpint(count_of(buckets, month), ==, 6);
	}

	/* And by the assignee: nobody's ticket is its own bar. */
	{
		g_autofree gchar *whose = page(f, "/e/ticket?view=chart&chart_by=assignee", 200);
		g_autoptr(JsonNode) people = drawn(whose);

		g_assert_cmpint(count_of(people, "ada"), ==, 3);
		g_assert_cmpint(count_of(people, "ana"), ==, 2);
		g_assert_cmpint(count_of(people, "(none)"), ==, 1);
	}
}

/*
 * The bar's q and the URL's field filters both narrow the chart, on
 * /e/ticket and on /tickets: a chart counts only the rows the page lists.
 *
 * Breaks if the chart drops q or a field filter.
 */
static void
test_filter(
	Fixture		*f,
	gconstpointer	 data
){
	g_autofree gchar *q = escaped("assignee == \"ada\"");
	g_autofree gchar *path = NULL;
	g_autofree gchar *html = NULL;
	g_autoptr(JsonNode) chart = NULL;

	(void)data;

	/* q alone: ada's three. */
	path = g_strconcat("/e/ticket?view=chart&chart_by=status&q=", q, NULL);
	html = page(f, path, 200);
	chart = drawn(html);
	g_assert_cmpint(count_of(chart, "todo"), ==, 2);
	g_assert_cmpint(count_of(chart, "in_progress"), ==, 1);
	g_assert_cmpint(count_of(chart, "done"), ==, -1);
	g_assert_nonnull(strstr(html, "3 records"));

	/* q and a field filter: ada's high-priority ones. */
	g_clear_pointer(&path, g_free);
	g_clear_pointer(&html, g_free);
	g_clear_pointer(&chart, json_node_unref);
	path = g_strconcat("/e/ticket?view=chart&chart_by=status&priority=high&q=", q, NULL);
	html = page(f, path, 200);
	chart = drawn(html);
	g_assert_cmpint(count_of(chart, "todo"), ==, 2);
	g_assert_cmpint(count_of(chart, "in_progress"), ==, -1);

	/* A field filter alone. */
	g_clear_pointer(&html, g_free);
	g_clear_pointer(&chart, json_node_unref);
	html = page(f, "/e/ticket?view=chart&chart_by=status&priority=high", 200);
	chart = drawn(html);
	g_assert_cmpint(count_of(chart, "todo"), ==, 2);
	g_assert_cmpint(count_of(chart, "in_progress"), ==, 1);

	/* The ticket page: the same chart of the same filter. */
	g_clear_pointer(&path, g_free);
	g_clear_pointer(&html, g_free);
	g_clear_pointer(&chart, json_node_unref);
	path = g_strconcat("/tickets?view=chart&chart_by=status&q=", q, NULL);
	html = page(f, path, 200);
	chart = drawn(html);
	g_assert_cmpint(count_of(chart, "todo"), ==, 2);
	g_assert_cmpint(count_of(chart, "in_progress"), ==, 1);
	g_assert_cmpint(count_of(chart, "done"), ==, -1);
	g_assert_nonnull(strstr(html, ">Chart</a>"));
	g_assert_null(strstr(html, "data-board"));

	/* A search the chart cannot say in LQL is refused, not dropped. */
	{
		g_autofree gchar *refused = page(f, "/e/ticket?view=chart&search=crash", 400);
		g_autofree gchar *message = problem_message(refused);

		g_assert_nonnull(message);
		g_assert_nonnull(strstr(message, "search=crash"));
	}
}

/* --- Money ------------------------------------------------------------------------ */

/*
 * Totals of a money field are one series per currency: the invoice chart
 * of sum:shipping_amount over USD and EUR draws two, 15.00 USD and
 * 7.00 EUR, and never the 22.00 of adding them.
 *
 * Breaks if the chart sums money with sum() or across currencies.
 */
static void
test_currency(
	Fixture		*f,
	gconstpointer	 data
){
	g_autofree gchar *cookie = NULL;
	g_autofree gchar *html = NULL;
	g_autoptr(JsonNode) chart = NULL;
	JsonArray *groups;
	gboolean usd = FALSE;
	gboolean eur = FALSE;
	guint i;

	(void)data;

	cookie = fixture_login(f, "fin");
	html = page_as(f, cookie, "/e/invoice?view=chart&chart_by=status"
	               "&chart_value=sum:shipping_amount", 200);
	chart = drawn(html);
	groups = json_object_get_array_member(json_node_get_object(chart), "groups");
	g_assert_cmpuint(json_array_get_length(groups), ==, 2);

	for (i = 0; i < json_array_get_length(groups); i++)
	{
		JsonObject *group = json_array_get_object_element(groups, i);
		const gchar *currency = json_object_get_string_member(group, "currency");
		JsonArray *values = json_object_get_array_member(group, "values");
		JsonObject *money;

		g_assert_cmpuint(json_array_get_length(values), ==, 1);
		money = json_array_get_object_element(values, 0);
		g_assert_cmpstr(json_object_get_string_member(money, "currency"), ==, currency);

		if (0 == g_strcmp0(currency, "USD"))
		{
			usd = TRUE;
			g_assert_cmpint(json_object_get_int_member(money, "amount"), ==, 1500);
		}
		else if (0 == g_strcmp0(currency, "EUR"))
		{
			eur = TRUE;
			g_assert_cmpint(json_object_get_int_member(money, "amount"), ==, 700);
		}
	}

	g_assert_true(usd && eur);

	/* One drawing per currency, and no figure added across them. */
	g_assert_cmpuint(count_substring(html, "<figure class=\"chart chart-bar\">"), ==, 2);
	g_assert_null(strstr(html, "2200"));
	g_assert_null(strstr(html, "22.00"));
	g_assert_nonnull(strstr(html, "3 records"));

	/* The table: each currency its own figure in the one cell. */
	{
		g_autofree gchar *table = page_as(f, cookie, "/e/invoice?view=chart&chart_by=status"
		                                  "&chart_value=sum:shipping_amount&chart_kind=table",
		                                  200);

		g_assert_nonnull(strstr(table, "data-currency=\"USD\""));
		g_assert_nonnull(strstr(table, "data-currency=\"EUR\""));
		g_assert_null(strstr(table, "22.00"));
	}
}

/* --- Access ------------------------------------------------------------------------- */

/*
 * The chart reads what the list shows its caller: ana, an editor but no
 * finance member, lists no invoice, and her chart counts none; fin's
 * counts all three. The type gate holds too: /e/user stays the owner's.
 *
 * Breaks if the chart runs as somebody other than the person looking, or
 * around the list's role check.
 */
static void
test_access(
	Fixture		*f,
	gconstpointer	 data
){
	g_autofree gchar *ana = NULL;
	g_autofree gchar *fin = NULL;
	g_autofree gchar *list = NULL;
	g_autofree gchar *chart = NULL;
	g_autofree gchar *theirs = NULL;
	g_autoptr(JsonNode) drawn_theirs = NULL;
	guint status;

	(void)data;

	ana = fixture_login(f, "ana");
	fin = fixture_login(f, "fin");

	list = page_as(f, ana, "/e/invoice", 200);
	g_assert_nonnull(strstr(list, "0 records"));
	chart = page_as(f, ana, "/e/invoice?view=chart&chart_by=status"
	                "&chart_value=sum:shipping_amount", 200);
	g_assert_nonnull(strstr(chart, "0 records"));
	g_assert_null(strstr(chart, "CHART-"));
	g_assert_null(strstr(chart, "data-currency"));
	g_assert_null(strstr(chart, "\"USD\""));

	theirs = page_as(f, fin, "/e/invoice?view=chart&chart_by=owner", 200);
	drawn_theirs = drawn(theirs);
	g_assert_cmpint(count_of(drawn_theirs, "fin"), ==, 2);
	g_assert_cmpint(count_of(drawn_theirs, "ada"), ==, 1);

	/* A type the list refuses, the chart refuses the same way. */
	{
		g_autofree gchar *users_list = NULL;
		g_autofree gchar *users_chart = NULL;
		guint list_status;

		list_status = fixture_request(f, "GET", "/e/user", ana, NULL, NULL, &users_list, NULL);
		status = fixture_request(f, "GET", "/e/user?view=chart", ana, NULL, NULL,
		                         &users_chart, NULL);
		g_assert_cmpuint(list_status, !=, SOUP_STATUS_OK);
		g_assert_cmpuint(status, ==, list_status);
	}
}

/* --- Problems --------------------------------------------------------------------- */

/*
 * A bad pick is drawn in the bar with a 400, the page's controls still
 * there to fix it with: a field the chart cannot split by, a value that
 * is no money field, a kind that is no chart.
 *
 * Breaks if a bad chart_by reaches LQL as text (a 500, or a column of
 * nothing), or is drawn as an empty chart.
 */
static void
test_bad_picks(
	Fixture		*f,
	gconstpointer	 data
){
	static const struct
	{
		const gchar	*path;
		const gchar	*words;
	} cases[] = {
		{ "/e/ticket?view=chart&chart_by=nonsense",                     "chart_by" },
		{ "/e/ticket?view=chart&chart_by=title",                        "chart_by" },
		{ "/e/ticket?view=chart&chart_by=status%29%20%7C%20fetch%20user", "chart_by" },
		{ "/e/ticket?view=chart&chart_value=sum:title",                 "chart_value" },
		{ "/e/ticket?view=chart&chart_kind=donut",                      "chart_kind" },
		{ "/tickets?view=chart&chart_by=nonsense",                      "chart_by" }
	};
	gsize i;

	(void)data;

	for (i = 0; i < G_N_ELEMENTS(cases); i++)
	{
		g_autofree gchar *html = page(f, cases[i].path, 400);
		g_autofree gchar *message = problem_message(html);

		g_assert_nonnull(message);
		g_assert_nonnull(strstr(message, cases[i].words));
		g_assert_nonnull(strstr(html, "data-lql-bar"));
		g_assert_null(strstr(html, "data-chart-data"));
	}

	/* A bad q is placed in the bar's text, as the list places it. */
	{
		g_autofree gchar *q = escaped("staus == \"todo\"");
		g_autofree gchar *path = g_strconcat("/e/ticket?view=chart&q=", q, NULL);
		g_autofree gchar *html = page(f, path, 400);

		g_assert_nonnull(strstr(html, "lql-caret"));
		g_assert_nonnull(strstr(html, "aria-invalid=\"true\""));
	}

	/* A field filter the list refuses, the chart refuses as the list
	 * does. */
	{
		guint list_status = fixture_request(f, "GET", "/e/ticket?staus=todo", f->cookie, NULL,
		                                    NULL, NULL, NULL);
		guint chart_status = fixture_request(f, "GET", "/e/ticket?view=chart&staus=todo",
		                                     f->cookie, NULL, NULL, NULL, NULL);

		g_assert_cmpuint(chart_status, ==, list_status);
	}
}

/*
 * query.max_bytes and query.max_rows bound a chart as they bound the
 * list's filter: the problem is drawn in the bar (400, naming the
 * setting), and the next request is answered as usual.
 *
 * Breaks if a chart's query runs outside the query bounds.
 */
static void
test_bounds(
	Fixture		*f,
	gconstpointer	 data
){
	g_autofree gchar *q = escaped(FILTER_OVER_BUDGET);
	g_autofree gchar *path = NULL;
	g_autofree gchar *html = NULL;
	g_autofree gchar *after = NULL;
	g_autofree gchar *rows = NULL;

	(void)data;

	g_object_set(f->config, "query-max-bytes", (gint64)1048576, NULL);
	path = g_strconcat("/e/ticket?view=chart&q=", q, NULL);
	html = page(f, path, 400);
	g_assert_nonnull(strstr(html, "lql-problem"));
	g_assert_nonnull(strstr(html, "query.max_bytes"));
	g_assert_null(strstr(html, "data-chart-data"));

	after = page(f, "/e/ticket?view=chart", 200);
	g_assert_nonnull(strstr(after, "data-chart-data"));

	/* Six tickets read past a bound of three rows. */
	g_object_set(f->config, "query-max-rows", (gint64)3, NULL);
	rows = page(f, "/e/ticket?view=chart", 400);
	g_assert_nonnull(strstr(rows, "lql-problem"));
	g_assert_nonnull(strstr(rows, "query.max_rows"));
}

/* --- Scripting off ------------------------------------------------------------ */

/*
 * The controls are a GET form: it carries the page's q, its field
 * filters and view=chart as hidden fields, marks the picks in force, and
 * submitting it -- here, with a table picked -- comes back as that chart
 * of the same records. The bar's own form carries the chart's picks, and
 * the Table link drops them.
 *
 * Breaks if a pick needs a script, or submitting the form loses the
 * filter.
 */
static void
test_no_js_round_trip(
	Fixture		*f,
	gconstpointer	 data
){
	g_autofree gchar *q = escaped("assignee == \"ada\"");
	g_autofree gchar *path = NULL;
	g_autofree gchar *html = NULL;
	g_autofree gchar *form = NULL;
	g_autofree gchar *submitted = NULL;
	g_autofree gchar *again = NULL;
	g_autoptr(JsonNode) first = NULL;
	g_autoptr(JsonNode) second = NULL;
	const gchar *start;
	const gchar *end;

	(void)data;

	path = g_strconcat("/e/ticket?view=chart&chart_by=status&priority=high&q=", q, NULL);
	html = page(f, path, 200);
	first = drawn(html);

	/* The form, as a browser would read it. */
	start = strstr(html, "<form class=\"chart-controls\" method=\"get\" action=\"/e/ticket\"");
	g_assert_nonnull(start);
	end = strstr(start, "</form>");
	g_assert_nonnull(end);
	form = g_strndup(start, (gsize)(end - start));
	g_assert_nonnull(strstr(form, "<input type=\"hidden\" name=\"view\" value=\"chart\">"));
	g_assert_nonnull(strstr(form, "<input type=\"hidden\" name=\"priority\" value=\"high\">"));
	g_assert_nonnull(strstr(form, "<input type=\"hidden\" name=\"q\" "
	                              "value=\"assignee == &quot;ada&quot;\">"));
	g_assert_null(strstr(form, "type=\"hidden\" name=\"chart_by\""));
	g_assert_nonnull(strstr(form, "<select name=\"chart_by\">"));
	g_assert_nonnull(strstr(form, "<option value=\"status\" selected>"));
	g_assert_nonnull(strstr(form, "<option value=\"count\" selected>"));
	g_assert_nonnull(strstr(form, "<option value=\"bar\" selected>"));
	g_assert_nonnull(strstr(form, "<option value=\"table\">"));
	g_assert_nonnull(strstr(form, "<option value=\"assignee\">"));
	g_assert_nonnull(strstr(form, "<button class=\"btn\" type=\"submit\">"));

	/* Submitted with a table picked: the fields in the form's order. */
	submitted = g_strconcat("/e/ticket?priority=high&q=", q,
	                        "&view=chart&chart_by=status&chart_value=count&chart_kind=table",
	                        NULL);
	again = page(f, submitted, 200);
	g_assert_nonnull(strstr(again, "data-chart-kind=\"table\""));
	g_assert_nonnull(strstr(again, "<option value=\"table\" selected>"));
	second = drawn(again);
	g_assert_cmpstr(json_object_get_string_member(json_node_get_object(second), "chart"), ==,
	                "table");
	g_assert_cmpint(count_of(first, "todo"), ==, 2);

	/* The bar's form keeps the chart, and its picks. */
	g_assert_nonnull(strstr(again, "<input type=\"hidden\" name=\"chart_kind\" value=\"table\">"));
	g_assert_nonnull(strstr(again, "<input type=\"hidden\" name=\"view\" value=\"chart\">"));

	/* The Table link: the same filters, none of the chart's picks. */
	{
		g_autofree gchar *table_href = g_strconcat("href=\"/e/ticket?priority=high&amp;q=", q,
		                                           "\"", NULL);

		g_assert_nonnull(strstr(again, table_href));
	}
}

int
main(
	int	 argc,
	char	*argv[]
){
	g_test_init(&argc, &argv, NULL);
	g_setenv("VENTURE_TEST_SESSION_SECRET", "chart-test-session-secret-0123456789", TRUE);

#define ADD(path, func) \
	g_test_add(path, Fixture, &plain, fixture_setup, func, fixture_teardown); \
	g_test_add(path "/read-workers", Fixture, &with_workers, fixture_setup, func, fixture_teardown)

	ADD("/lql-chart/counts-match-list", test_counts_match_list);
	ADD("/lql-chart/filter", test_filter);
	ADD("/lql-chart/money/one-series-per-currency", test_currency);
	ADD("/lql-chart/access/as-the-list", test_access);
	ADD("/lql-chart/problems/bad-picks", test_bad_picks);
	ADD("/lql-chart/problems/bounds", test_bounds);
	ADD("/lql-chart/no-js-round-trip", test_no_js_round_trip);

#undef ADD

	return g_test_run();
}
