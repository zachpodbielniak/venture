/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * test-lql-lists.c - The query bar on every generic list page
 *
 * Copyright (C) 2026 Zach Podbielniak
 *
 * What ticket #8 of the LQL epic promises (docs/lql.org, "Filter bars"
 * and "Quick chips"), and what breaks if it regresses:
 *
 *   - Every generic list, /e/<type>, draws the query bar with the chips
 *     its field table gives (Open and Closed from the lifecycle
 *     enumeration, Mine from the owning field, this week and this month
 *     of the primary date), and filters by ?q= on top of whatever else
 *     the URL filters by.
 *   - Every chip of every registered type prepares, runs, and pushes down
 *     to SQL in full.
 *   - A bad filter is drawn in the bar, with a 400 and no table.
 *   - Saved views work on any type, and are neither offered nor saved for
 *     a type the caller cannot open.
 *   - The module gate holds: a type whose module is off is not fetchable
 *     from the bar, from ?q= on its list API, from a sub-pipeline in
 *     another list's filter, from /lql/assist or from a saved view; and
 *     a financial type stays the finance roles' to read.
 *   - At 50,000 rows a pushed-down filter stays inside the row bound and
 *     the time limit, which a filter that is not pushed down cannot.
 */

#include <venture.h>
#include <libsoup/soup.h>
#include <string.h>
#include "lql/venture-lql.h"
#include "venture-test-util.h"

#define TEST_PASSWORD "lists-long-password"

/* The scale test's table, and how it is spread. */
#define SCALE_ROWS (50000)
#define SCALE_ADA_EVERY (500)	/* 100 of them ada's */
#define SCALE_FRESH_EVERY (250)	/* 200 of them created now */
#define SCALE_MAX_ROWS (5000)
#define SCALE_TIMEOUT_MS (10000)

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
	gchar			*ana_secret;
	gchar			*fin_secret;
} Fixture;

/* How a fixture is set up: read workers or not, the 50k table or not. */
typedef struct
{
	gboolean	workers;
	gboolean	scale;
} Variant;

static const Variant plain = { FALSE, FALSE };
static const Variant with_workers = { TRUE, FALSE };
static const Variant at_scale = { TRUE, TRUE };

/* The companies, by name, and whose they are. */
typedef struct
{
	const gchar	*name;
	gboolean	 ada_owns;
	gboolean	 customer;
} CompanySeed;

static const CompanySeed companies[] = {
	{ "Acme Anvils",     TRUE,  TRUE  },
	{ "Bolt Brothers",   FALSE, TRUE  },
	{ "Cog Collective",  TRUE,  FALSE },
	{ "Dynamo Depot",    FALSE, FALSE }
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
 * urlencoded POST body. Redirects are not followed; *out_location gets
 * the Location of one.
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
	gchar		**out_location,
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

	if (NULL != out_location)
		*out_location = g_strdup(soup_message_headers_get_one(
			soup_message_get_response_headers(message), "Location"));

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
	g_assert_cmpuint(fixture_request(f, "POST", "/login", NULL, NULL, login, NULL, NULL,
	                                 &set_cookie), ==, SOUP_STATUS_FOUND);
	g_assert_nonnull(set_cookie);
	semicolon = strchr(set_cookie, ';');

	if (NULL != semicolon)
		*semicolon = '\0';

	return g_steal_pointer(&set_cookie);
}

/* GET @path in @cookie's browser session; the page. */
static gchar *
page_as(
	Fixture		*f,
	const gchar	*cookie,
	const gchar	*path,
	guint		 expect
){
	gchar *body = NULL;
	guint status;

	status = fixture_request(f, "GET", path, cookie, NULL, NULL, &body, NULL, NULL);

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

/* GET @path with @secret as the bearer; the JSON body, parsed. */
static JsonNode *
api(
	Fixture		*f,
	const gchar	*secret,
	const gchar	*path,
	guint		 expect
){
	g_autofree gchar *body = NULL;
	guint status;

	status = fixture_request(f, "GET", path, NULL, secret, NULL, &body, NULL, NULL);

	if (status != expect)
		g_error("GET %s: %u, wanted %u\n%s", path, status, expect, body);

	return venture_json_parse(body, NULL);
}

/* Saves @entity as nobody in particular, failing the test on refusal. */
static void
persist(
	Fixture		*f,
	VentureEntity	*entity
){
	g_autoptr(GError) error = NULL;

	if (!venture_database_save(f->db, entity, NULL, &error))
		g_error("save %s: %s", G_OBJECT_TYPE_NAME(entity), error->message);
}

/*
 * @rows companies made in one statement, as a bulk import would leave
 * them: every SCALE_ADA_EVERY-th ada's, every SCALE_FRESH_EVERY-th made
 * now and the rest in 2020. Written in SQL: saving fifty thousand records
 * one by one would test the save path, not the filter.
 */
static void
fixture_scale(
	Fixture	*f,
	guint	 rows
){
	g_autoptr(GDateTime) now = g_date_time_new_now_utc();
	g_autofree gchar *stamp = venture_time_to_string(now);
	g_autofree gchar *sql = NULL;
	g_autoptr(GError) error = NULL;

	sql = g_strdup_printf(
		"WITH RECURSIVE n(x) AS (SELECT 1 UNION ALL SELECT x + 1 FROM n WHERE x < %u) "
		"INSERT INTO companies (uuid, organization_id, created_at, updated_at, version, "
		"name, kind, customer, supplier, active, owner_user_id) "
		"SELECT printf('scale-%%08d', x), %" G_GINT64_FORMAT ", "
		"CASE WHEN x %% %d = 0 THEN '%s' ELSE '2020-01-01T00:00:00.000000Z' END, "
		"'2020-01-01T00:00:00.000000Z', 1, printf('Scale %%d', x), 'customer', 1, 0, 1, "
		"CASE WHEN x %% %d = 0 THEN %" G_GINT64_FORMAT " ELSE NULL END FROM n",
		rows, f->org, SCALE_FRESH_EVERY, stamp, SCALE_ADA_EVERY, f->ada_id);

	g_assert_true(venture_database_execute(f->db, sql, NULL, &error));
	g_assert_no_error(error);
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

	f->directory = g_dir_make_tmp("venture-lql-lists-XXXXXX", NULL);
	f->config = venture_config_new();
	g_object_set(f->config, "state-dir", f->directory, "server-bind-address", "127.0.0.1",
	             "server-port", (gint64)0, "security-require-auth", TRUE,
	             "security-session-secret-env", "VENTURE_TEST_SESSION_SECRET", NULL);

	/* Expense claims are a financial module nobody else needs: off, it
	 * is the type the module gate must keep out of every query. */
	venture_config_set_module_enabled(f->config, "claims", FALSE);

	if (variant->scale)
		g_object_set(f->config, "query-max-rows", (gint64)SCALE_MAX_ROWS,
		             "query-timeout-ms", (gint64)SCALE_TIMEOUT_MS, NULL);

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

	/* ada owns the business; ana edits it, but is no finance member, so
	 * invoices are not hers to read; fin is the finance member. */
	f->ada_id = fixture_user(f, "ada", VENTURE_USER_ROLE_ADMIN, VENTURE_ORGANIZATION_ROLE_OWNER);
	f->ana_id = fixture_user(f, "ana", VENTURE_USER_ROLE_EDITOR, VENTURE_ORGANIZATION_ROLE_EDITOR);
	f->fin_id = fixture_user(f, "fin", VENTURE_USER_ROLE_EDITOR, VENTURE_ORGANIZATION_ROLE_FINANCE);
	f->ada_secret = fixture_token(f, "ada-token", f->ada_id, VENTURE_USER_ROLE_ADMIN);
	f->ana_secret = fixture_token(f, "ana-token", f->ana_id, VENTURE_USER_ROLE_EDITOR);
	f->fin_secret = fixture_token(f, "fin-token", f->fin_id, VENTURE_USER_ROLE_EDITOR);

	for (i = 0; i < G_N_ELEMENTS(companies); i++)
	{
		g_autoptr(VentureEntity) company = NULL;

		company = g_object_new(VENTURE_TYPE_COMPANY, "organization-id", f->org,
			"name", companies[i].name, "customer", companies[i].customer, NULL);

		if (companies[i].ada_owns)
			g_object_set(company, "owner-user-id", f->ada_id, NULL);

		/* A customer by relationship, or saving sets the flag again. */
		g_assert_true(venture_entity_set_field_from_string(company, "kind",
			companies[i].customer ? "customer" : "prospect", NULL));

		persist(f, company);

		if (0 == i)
			customer = g_object_ref(company);
	}

	/* Two draft invoices to Acme, one collected by ada, one by fin. */
	for (i = 0; i < 2; i++)
	{
		g_autoptr(VentureEntity) invoice = NULL;
		g_autoptr(GDateTime) issued = g_date_time_new_now_utc();
		g_autofree gchar *number = g_strdup_printf("LISTS-%u", i + 1);

		invoice = g_object_new(VENTURE_TYPE_INVOICE, "organization-id", f->org,
			"company-id", venture_entity_get_id(customer), "number", number,
			"issued-at", issued, "due-at", issued, "owner", (0 == i) ? "ada" : "fin", NULL);
		persist(f, invoice);
	}

	if (variant->scale)
		fixture_scale(f, SCALE_ROWS);

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
	g_clear_pointer(&f->ana_secret, g_free);
	g_clear_pointer(&f->fin_secret, g_free);
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

/* An environment for ada over the whole business, without the web. */
static gboolean
allow_every_type(
	GType		  entity_type,
	gpointer	  user_data,
	GError		**error
){
	(void)entity_type;
	(void)user_data;
	(void)error;

	return TRUE;
}

static LqlEnvironment *
environment_for(
	Fixture	*f,
	gint64	 user_id
){
	VentureAuthPrincipal who;
	g_autoptr(GError) error = NULL;
	LqlEnvironment *environment;

	memset(&who, 0, sizeof who);
	who.user_id = user_id;
	who.role = VENTURE_USER_ROLE_ADMIN;
	who.authenticated = TRUE;

	environment = venture_lql_environment_new(f->context, &who, &f->org, 1,
		allow_every_type, NULL, &error);
	g_assert_no_error(error);

	return environment;
}

/*
 * Which of the seeded companies @html lists, as "1010" in seed order: a
 * row's title cell, not the name anywhere (the Filters form's company
 * pickers name every company).
 */
static gchar *
shown(const gchar *html)
{
	GString *out = g_string_new(NULL);
	guint i;

	for (i = 0; i < G_N_ELEMENTS(companies); i++)
	{
		g_autofree gchar *cell = g_strdup_printf("\">%s</a></td>", companies[i].name);

		g_string_append_c(out, (NULL != strstr(html, cell)) ? '1' : '0');
	}

	return g_string_free(out, FALSE);
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

/* The number of records a list API answer holds. */
static guint
n_records(JsonNode *answer)
{
	return json_array_get_length(json_object_get_array_member(json_node_get_object(answer),
	                                                         "records"));
}

/* The chip of @chips labelled @label; NULL when there is none. */
static const VentureLqlChip *
chip_named(
	GPtrArray	*chips,
	const gchar	*label
){
	guint i;

	for (i = 0; i < chips->len; i++)
	{
		const VentureLqlChip *chip = g_ptr_array_index(chips, i);

		if (0 == g_strcmp0(chip->label, label))
			return chip;
	}

	return NULL;
}

/* "Label=clause|Label=clause" of @type_name's chips, for one assertion. */
static gchar *
chips_of(
	Fixture		*f,
	const gchar	*type_name
){
	g_autoptr(GPtrArray) chips = NULL;
	GString *out = g_string_new(NULL);
	GType type;
	guint i;

	type = venture_entity_registry_lookup(venture_context_get_entity_registry(f->context),
	                                      type_name);
	g_assert_true(G_TYPE_INVALID != type);
	chips = venture_lql_filter_chips(type);

	for (i = 0; i < chips->len; i++)
	{
		const VentureLqlChip *chip = g_ptr_array_index(chips, i);

		g_string_append_printf(out, "%s%s=%s", (i > 0) ? "|" : "", chip->label, chip->clause);
	}

	return g_string_free(out, FALSE);
}

/* How the source took each conjunct of @text, as "EHN" (exact, hint, none). */
static gchar *
pushdowns(
	LqlEnvironment	*environment,
	const gchar	*text
){
	g_autoptr(LqlQuery) query = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GString) out = g_string_new(NULL);
	LqlFetchRequest *request;
	guint i;

	query = lql_environment_prepare(environment, text, NULL, &error);
	g_assert_no_error(error);
	request = lql_query_get_fetch_request(query);
	g_assert_nonnull(request);

	for (i = 0; i < lql_fetch_request_get_n_predicates(request); i++)
	{
		switch (lql_fetch_request_get_pushdown(request, i))
		{
		case LQL_PUSHDOWN_EXACT: g_string_append_c(out, 'E'); break;
		case LQL_PUSHDOWN_HINT:  g_string_append_c(out, 'H'); break;
		default:                 g_string_append_c(out, 'N'); break;
		}
	}

	return g_string_free(g_steal_pointer(&out), FALSE);
}

/* --- The chips, from the field table ---------------------------------------- */

/*
 * The chips the story's own list of pages gets, worked out from each
 * field table: a lifecycle enumeration gives Open and Closed, the owning
 * field Mine, the primary date its two windows.
 *
 * Breaks if the generator stops reading a declaration (the assigned
 * username flag, the owner reference, a status enumeration), or a closed
 * word goes missing.
 */
static void
test_chips_generated(
	Fixture		*f,
	gconstpointer	 data
){
	static const struct
	{
		const gchar	*type_name;
		const gchar	*chips;
	} cases[] = {
		{ "company",
		  "Mine=owner_user_id == my_id()"
		  "|Created this week=(created_at >= period(\"this_week\").start and created_at < period(\"this_week\").end)"
		  "|Created this month=(created_at >= period(\"this_month\").start and created_at < period(\"this_month\").end)" },
		{ "invoice",
		  "Open=status not in [\"paid\", \"void\"]"
		  "|Closed=status in [\"paid\", \"void\"]"
		  "|Mine=owner == me()" },
		{ "deal",
		  "Open=stage not in [\"won\", \"lost\"]"
		  "|Closed=stage in [\"won\", \"lost\"]"
		  "|Mine=owner == me()" },
		{ "ticket",
		  "Open=status not in [\"done\", \"cancelled\"]"
		  "|Closed=status in [\"done\", \"cancelled\"]"
		  "|Mine=assignee == me()" },
		{ "quote",
		  "Open=status not in [\"accepted\", \"declined\", \"expired\", \"superseded\"]"
		  "|Closed=status in [\"accepted\", \"declined\", \"expired\", \"superseded\"]" },
		{ "release",
		  "Open=status not in [\"released\", \"yanked\"]"
		  "|Closed=status in [\"released\", \"yanked\"]" }
	};
	guint i;

	(void)data;

	for (i = 0; i < G_N_ELEMENTS(cases); i++)
	{
		g_autofree gchar *got = chips_of(f, cases[i].type_name);

		/* The date windows depend on the table's dates; the cases pin
		 * the prefix, and the date chips are checked by name below. */
		if (!g_str_has_prefix(got, cases[i].chips))
			g_error("%s's chips:\n  %s\nwanted them to start\n  %s",
			        cases[i].type_name, got, cases[i].chips);
	}

	/* The primary date: when an invoice was issued, an expense occurred,
	 * a bill dated -- not a pause or a reminder ahead of it. */
	{
		g_autoptr(GPtrArray) invoice = venture_lql_filter_chips(VENTURE_TYPE_INVOICE);
		g_autoptr(GPtrArray) expense = venture_lql_filter_chips(VENTURE_TYPE_EXPENSE);
		g_autoptr(GPtrArray) bill = venture_lql_filter_chips(VENTURE_TYPE_VENDOR_BILL);
		guint j;

		for (j = 0; j < invoice->len; j++)
		{
			const VentureLqlChip *chip = g_ptr_array_index(invoice, j);

			if (NULL != strstr(chip->clause, "period("))
				g_assert_nonnull(strstr(chip->clause, "(issued_at >= "));
		}

		for (j = 0; j < expense->len; j++)
		{
			const VentureLqlChip *chip = g_ptr_array_index(expense, j);

			if (NULL != strstr(chip->clause, "period("))
				g_assert_nonnull(strstr(chip->clause, "(occurred_at >= "));
		}

		for (j = 0; j < bill->len; j++)
		{
			const VentureLqlChip *chip = g_ptr_array_index(bill, j);

			if (NULL != strstr(chip->clause, "period("))
				g_assert_nonnull(strstr(chip->clause, "(bill_date >= "));
		}
	}

	/* A company's first dates are a dunning pause and a late-fee pause:
	 * neither is when it happened, so its windows are on created_at. */
	{
		g_autoptr(GPtrArray) company = venture_lql_filter_chips(VENTURE_TYPE_COMPANY);

		g_assert_nonnull(chip_named(company, "Created this week"));
	}

	/* A label's trailing "at" is dropped: "Issued this week". */
	{
		g_autoptr(GPtrArray) invoice = venture_lql_filter_chips(VENTURE_TYPE_INVOICE);

		g_assert_nonnull(chip_named(invoice, "Issued this week"));
		g_assert_nonnull(chip_named(invoice, "Issued this month"));
	}
}

/*
 * Every chip of every registered type is a filter the bar accepts, that
 * runs as a reader, and that the source takes in full: chips are never a
 * full table scan, whatever a type's table looks like.
 *
 * Breaks if the generator writes a term for a field LQL does not have
 * (the bar would refuse its own chip), of the wrong type, or of a shape
 * the source does not push down.
 */
static void
test_chips_every_type(
	Fixture		*f,
	gconstpointer	 data
){
	g_autoptr(LqlEnvironment) environment = NULL;
	g_auto(GStrv) names = NULL;
	guint n_chips = 0;
	guint n_typed = 0;
	guint i;

	(void)data;

	environment = environment_for(f, f->ada_id);
	names = venture_entity_registry_list_names(venture_context_get_entity_registry(f->context));

	for (i = 0; NULL != names[i]; i++)
	{
		g_autoptr(GPtrArray) chips = NULL;
		GType type;
		guint j;

		type = venture_entity_registry_lookup(venture_context_get_entity_registry(f->context),
		                                      names[i]);
		chips = venture_lql_filter_chips(type);
		n_typed += (chips->len > 0) ? 1 : 0;

		for (j = 0; j < chips->len; j++)
		{
			const VentureLqlChip *chip = g_ptr_array_index(chips, j);
			g_autoptr(VentureLqlProblem) problem = NULL;
			g_autoptr(VentureLqlProblem) ran = NULL;
			g_autoptr(GArray) ids = NULL;
			g_autofree gchar *text = NULL;
			g_autofree gchar *taken = NULL;

			g_test_message("%s: %s = %s", names[i], chip->label, chip->clause);
			problem = venture_lql_filter_check(environment, names[i], chip->clause);

			if (NULL != problem)
				g_error("%s chip %s (%s) does not check: %s", names[i], chip->label,
				        chip->clause, problem->message);

			ids = venture_lql_filter_find_ids(environment, names[i], chip->clause, FALSE,
			                                  NULL, &ran);

			if (NULL == ids)
				g_error("%s chip %s (%s) does not run: %s", names[i], chip->label,
				        chip->clause, ran->message);

			/* Pushed down whole: one EXACT per conjunct, a window's two
			 * bounds included. */
			text = g_strdup_printf("fetch %s | filter %s", names[i], chip->clause);
			taken = pushdowns(environment, text);

			if ((0 == strlen(taken)) || (NULL != strpbrk(taken, "HN")))
				g_error("%s chip %s (%s) is not pushed down: %s", names[i], chip->label,
				        chip->clause, taken);

			n_chips++;
		}
	}

	/* Most types have a date at least; this is not a vacuous loop. */
	g_assert_cmpuint(n_typed, >, 100);
	g_assert_cmpuint(n_chips, >, 300);
	g_test_message("%u chips on %u types", n_chips, n_typed);
}

/* --- The list page -------------------------------------------------------------- */

/*
 * /e/company draws the bar and its chips, ?q= filters the list, a chip is
 * one click, pressed again it comes off, and the page's own URL filters
 * still apply under the bar's filter.
 *
 * Breaks if the generic list loses the bar, its chips stop being links
 * that work with scripting off, or the bar drops the page's other
 * parameters (a named view, a field filter) when it filters.
 */
static void
test_list_bar(
	Fixture		*f,
	gconstpointer	 data
){
	g_autofree gchar *plain_page = NULL;
	g_autofree gchar *filtered = NULL;
	g_autofree gchar *both = NULL;
	g_autofree gchar *off = NULL;
	g_autofree gchar *q = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *mine_href = NULL;
	g_autofree gchar *got = NULL;

	(void)data;

	/* Unfiltered: everything, the bar, its chips, and the assist hook. */
	plain_page = page(f, "/e/company", 200);
	got = shown(plain_page);
	g_assert_cmpstr(got, ==, "1111");
	g_assert_nonnull(strstr(plain_page, "data-lql-bar"));
	g_assert_nonnull(strstr(plain_page, "/lql/assist?type=company"));
	g_assert_nonnull(strstr(plain_page, ">Mine</a>"));
	g_assert_nonnull(strstr(plain_page, ">Created this week</a>"));

	/* The Mine chip as the page links it: the bar's term, URL-escaped. */
	q = escaped("owner_user_id == my_id()");
	mine_href = g_strconcat("href=\"/e/company?q=", q, "\"", NULL);
	g_assert_nonnull(strstr(plain_page, mine_href));

	/* Following it: ada's companies, the chip shown as on. */
	path = g_strconcat("/e/company?q=", q, NULL);
	filtered = page(f, path, 200);
	g_clear_pointer(&got, g_free);
	got = shown(filtered);
	g_assert_cmpstr(got, ==, "1010");
	g_assert_nonnull(strstr(filtered, "query-chip active"));
	g_assert_nonnull(strstr(filtered, "2 records"));

	/* The bar's chip now takes the term out again. */
	g_assert_nonnull(strstr(filtered, "href=\"/e/company?q=\""));

	/* A field filter in the URL stays, under the bar's filter, and the
	 * bar's form carries it along. */
	g_clear_pointer(&path, g_free);
	path = g_strconcat("/e/company?customer=true&q=", q, NULL);
	both = page(f, path, 200);
	g_clear_pointer(&got, g_free);
	got = shown(both);
	g_assert_cmpstr(got, ==, "1000");
	g_assert_nonnull(strstr(both, "<input type=\"hidden\" name=\"customer\" value=\"true\">"));
	g_assert_nonnull(strstr(both, "href=\"/e/company?customer=true&amp;q=\""));

	/* A named view's tab keeps the bar's filter apart: the view is its
	 * own parameter, the filter q. */
	off = page(f, "/e/company?customer=true", 200);
	g_clear_pointer(&got, g_free);
	got = shown(off);
	g_assert_cmpstr(got, ==, "1100");
}

/*
 * A filter that cannot run is drawn in the bar -- the message, the caret,
 * "did you mean" -- with a 400 and no table, and the page's other
 * controls still there to fix it with.
 *
 * Breaks if a bad ?q= on a generic list goes back to a bare refusal, or
 * draws an empty table that reads as "no such records".
 */
static void
test_list_problem(
	Fixture		*f,
	gconstpointer	 data
){
	g_autofree gchar *q = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *body = NULL;
	g_autofree gchar *fixed = NULL;

	(void)data;

	q = escaped("nmae == \"Acme Anvils\"");
	path = g_strconcat("/e/company?q=", q, NULL);
	body = page(f, path, 400);

	g_assert_nonnull(strstr(body, "Cannot filter:"));
	g_assert_nonnull(strstr(body, "lql-caret"));
	g_assert_nonnull(strstr(body, "aria-invalid=\"true\""));
	g_assert_null(strstr(body, "data-list"));

	/* "Did you mean" writes the fix in, and the fix lists. */
	fixed = escaped("name == \"Acme Anvils\"");
	{
		g_autofree gchar *href = g_strconcat("href=\"/e/company?q=", fixed, "\"", NULL);

		g_assert_nonnull(strstr(body, href));
	}

	{
		g_autofree gchar *good = g_strconcat("/e/company?q=", fixed, NULL);
		g_autofree gchar *listed = page(f, good, 200);
		g_autofree gchar *got = shown(listed);

		g_assert_cmpstr(got, ==, "1000");
	}

	/* A `|` is a pipeline, not a filter, on any list. */
	{
		g_autofree gchar *pipe = escaped("name == \"x\" | summarize n = count()");
		g_autofree gchar *bad = g_strconcat("/e/company?q=", pipe, NULL);
		g_autofree gchar *refused = page(f, bad, 400);

		g_assert_nonnull(strstr(refused, "cannot start another stage"));
	}
}

/*
 * The same chips and ?q= on a financial list (invoices), for the finance
 * reader; ?q= on the list API too.
 *
 * Breaks if the generic bar works only on CRM types, or the API's ?q=
 * stops agreeing with the page's.
 */
static void
test_list_invoices(
	Fixture		*f,
	gconstpointer	 data
){
	g_autofree gchar *body = NULL;
	g_autofree gchar *q = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *api_path = NULL;
	g_autoptr(JsonNode) mine = NULL;
	g_autoptr(JsonNode) open = NULL;

	(void)data;

	body = page(f, "/e/invoice", 200);
	g_assert_nonnull(strstr(body, ">Open</a>"));
	g_assert_nonnull(strstr(body, ">Closed</a>"));
	g_assert_nonnull(strstr(body, ">Mine</a>"));

	/* Mine is ada's own: the invoice she collects. */
	q = escaped("owner == me()");
	api_path = g_strconcat("/api/v1/invoice?q=", q, NULL);
	mine = api(f, f->ada_secret, api_path, 200);
	g_assert_cmpuint(n_records(mine), ==, 1);

	/* Both drafts are open. */
	g_clear_pointer(&q, g_free);
	q = escaped("status not in [\"paid\", \"void\"]");
	path = g_strconcat("/e/invoice?q=", q, NULL);
	g_clear_pointer(&body, g_free);
	body = page(f, path, 200);
	g_assert_nonnull(strstr(body, "LISTS-1"));
	g_assert_nonnull(strstr(body, "LISTS-2"));

	g_clear_pointer(&api_path, g_free);
	api_path = g_strconcat("/api/v1/invoice?q=", q, NULL);
	open = api(f, f->fin_secret, api_path, 200);
	g_assert_cmpuint(n_records(open), ==, 2);
}

/* --- The module gate -------------------------------------------------------------- */

/*
 * A type whose module is off is not fetchable from anywhere a filter
 * reaches: its list page, ?q= on its list API, /lql/assist, a
 * sub-pipeline in another list's filter, or a saved view.
 *
 * Breaks if the generic bar's environment grows a source for a disabled
 * module's type, or a saved view of one is offered in the sidebar.
 */
static void
test_gate_module_off(
	Fixture		*f,
	gconstpointer	 data
){
	g_autofree gchar *q = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *body = NULL;
	guint status;

	(void)data;

	g_assert_false(venture_context_module_enabled(f->context, "claims"));

	/* The list page, the list API with ?q=, and the bar's assist. */
	q = escaped("status == \"draft\"");
	path = g_strconcat("/e/expense_claim?q=", q, NULL);
	status = fixture_request(f, "GET", path, f->cookie, NULL, NULL, NULL, NULL, NULL);
	g_assert_cmpuint(status, ==, SOUP_STATUS_NOT_FOUND);

	g_clear_pointer(&path, g_free);
	path = g_strconcat("/api/v1/expense_claim?q=", q, NULL);
	status = fixture_request(f, "GET", path, NULL, f->ada_secret, NULL, NULL, NULL, NULL);
	g_assert_cmpuint(status, ==, SOUP_STATUS_NOT_FOUND);

	status = fixture_request(f, "GET", "/lql/assist?type=expense_claim&q=sta", f->cookie, NULL,
	                         NULL, NULL, NULL, NULL);
	g_assert_cmpuint(status, ==, SOUP_STATUS_NOT_FOUND);

	/*
	 * From another list: a filter cannot name a second source at all. A
	 * sub-pipeline is only ever a command's argument (append, lookup),
	 * and a command needs the `|` a bar refuses, so the only source a
	 * bar's filter reads is its own list's type.
	 */
	g_clear_pointer(&q, g_free);
	g_clear_pointer(&path, g_free);
	q = escaped("id in (fetch expense_claim)");
	path = g_strconcat("/e/company?q=", q, NULL);
	body = page(f, path, 400);
	{
		g_autofree gchar *message = problem_message(body);

		g_assert_nonnull(message);
		g_assert_nonnull(strstr(message, "sub-pipeline can only be"));
	}
	g_assert_null(strstr(body, "data-list"));

	/* And the environment every filter runs in has no such source, with
	 * the most permissive type gate there is. */
	{
		g_autoptr(LqlEnvironment) environment = environment_for(f, f->ada_id);
		g_autoptr(LqlQuery) query = NULL;
		g_autoptr(GError) error = NULL;

		query = lql_environment_prepare(environment, "fetch expense_claim", NULL, &error);
		g_assert_null(query);
		g_assert_nonnull(error);
		g_test_message("fetch expense_claim, module off: %s", error->message);
		g_assert_nonnull(strstr(error->message, "expense_claim"));
	}

	/* A saved view of it is not saved... */
	status = fixture_request(f, "POST", "/views", f->cookie, NULL,
	                         "name=Claims&entity_type=expense_claim&query=&lql=&pinned=1",
	                         NULL, NULL, NULL);
	g_assert_cmpuint(status, ==, SOUP_STATUS_NOT_FOUND);

	/* ...and one kept from before the module was turned off is not
	 * offered: not in the sidebar, not on /views. */
	{
		g_autoptr(VentureSavedView) old = venture_saved_view_new();
		g_autofree gchar *views = NULL;
		g_autofree gchar *home = NULL;

		g_object_set(old, "name", "Old claims view", "entity-type", "expense_claim",
		             "query", "", "lql", "status == \"draft\"", "pinned", TRUE,
		             "owner-user-id", f->ada_id, NULL);
		venture_entity_set_organization_id(VENTURE_ENTITY(old), f->org);
		persist(f, VENTURE_ENTITY(old));

		views = page(f, "/views", 200);
		g_assert_null(strstr(views, "Old claims view"));
		home = page(f, "/e/company", 200);
		g_assert_null(strstr(home, "Old claims view"));
	}
}

/*
 * A type the caller's role does not list (user, which is the owner's) is
 * not fetchable through a filter either, nor saved as a view; and a
 * financial type stays the finance roles' to read: a member without one
 * filters invoices to nothing, through the page, the API and another
 * list's filter alike.
 *
 * Breaks if ?q= or a saved view becomes a way round the list's role
 * check, or LQL reads a financial record past the access policy.
 */
static void
test_gate_roles(
	Fixture		*f,
	gconstpointer	 data
){
	g_autofree gchar *q = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *ana = NULL;
	g_autoptr(JsonNode) theirs = NULL;
	g_autoptr(JsonNode) hers = NULL;
	guint status;

	(void)data;

	/* user is the owner's: not through ?q=, not from another list's
	 * filter, not as a saved view. */
	q = escaped("username == \"ada\"");
	path = g_strconcat("/api/v1/user?q=", q, NULL);
	status = fixture_request(f, "GET", path, NULL, f->ada_secret, NULL, NULL, NULL, NULL);
	g_assert_cmpuint(status, ==, SOUP_STATUS_FORBIDDEN);

	{
		g_autofree gchar *sub = escaped("owner_user_id in (fetch user)");
		g_autofree gchar *sub_path = g_strconcat("/e/company?q=", sub, NULL);
		g_autofree gchar *body = page(f, sub_path, 400);
		g_autofree gchar *message = problem_message(body);

		g_assert_nonnull(message);
		g_assert_nonnull(strstr(message, "sub-pipeline can only be"));
	}

	status = fixture_request(f, "POST", "/views", f->cookie, NULL,
	                         "name=People&entity_type=user&query=&lql=&pinned=1",
	                         NULL, NULL, NULL);
	g_assert_cmpuint(status, ==, SOUP_STATUS_FORBIDDEN);

	/* Invoices: the finance member reads both, ana (an editor, not in
	 * finance) none, whatever the filter. */
	g_clear_pointer(&q, g_free);
	g_clear_pointer(&path, g_free);
	q = escaped("status == \"draft\"");
	path = g_strconcat("/api/v1/invoice?q=", q, NULL);
	theirs = api(f, f->fin_secret, path, 200);
	g_assert_cmpuint(n_records(theirs), ==, 2);
	hers = api(f, f->ana_secret, path, 200);
	g_assert_cmpuint(n_records(hers), ==, 0);

	/* The page as ana: no invoice, and no count of them. */
	{
		g_autofree gchar *cookie = fixture_login(f, "ana");
		g_autofree gchar *list_path = g_strconcat("/e/invoice?q=", q, NULL);
		g_autofree gchar *sub = escaped("id in (fetch invoice)");
		g_autofree gchar *sub_path = g_strconcat("/e/company?q=", sub, NULL);
		g_autofree gchar *companies_page = NULL;

		ana = page_as(f, cookie, list_path, 200);
		g_assert_null(strstr(ana, "LISTS-1"));
		g_assert_nonnull(strstr(ana, "0 records"));

		/* Another list's filter cannot read invoices at all (see
		 * test_gate_module_off()); whatever it answers, never Acme's
		 * invoices. */
		status = fixture_request(f, "GET", sub_path, cookie, NULL, NULL, &companies_page,
		                         NULL, NULL);
		{
			g_autofree gchar *message = problem_message(companies_page);

			g_test_message("ana's sub-pipeline over invoices: %u %s", status,
			               (NULL != message) ? message : "(no problem)");
		}
		g_assert_true((SOUP_STATUS_OK == status) || (SOUP_STATUS_BAD_REQUEST == status));
		g_assert_null(strstr(companies_page, "LISTS-"));
	}
}

/* --- Saved views on any type -------------------------------------------------- */

/*
 * A view saved from a filtered company list keeps the LQL apart and opens
 * filtered; /views and the sidebar list it.
 *
 * Breaks if saved views work only for tickets, or the save form on a
 * generic list loses the bar's filter.
 */
static void
test_saved_views(
	Fixture		*f,
	gconstpointer	 data
){
	g_autofree gchar *q = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *body = NULL;
	g_autofree gchar *form = NULL;
	g_autofree gchar *location = NULL;
	g_autofree gchar *views = NULL;
	g_autofree gchar *opened = NULL;
	g_autofree gchar *got = NULL;
	const gchar *lql = "owner_user_id == my_id() and customer == TRUE";

	(void)data;

	/* The save form on the filtered page carries the filter as lql. */
	q = escaped(lql);
	path = g_strconcat("/e/company?q=", q, NULL);
	body = page(f, path, 200);
	{
		g_autofree gchar *attribute = venture_html_escape(lql);
		g_autofree gchar *field = g_strconcat("name=\"lql\" value=\"", attribute, "\"", NULL);

		g_assert_nonnull(strstr(body, field));
	}

	/* Posted as that form posts it, it opens at the filtered list. */
	form = g_strconcat("name=My+customers&entity_type=company&query=&pinned=1&lql=", q, NULL);
	g_assert_cmpuint(fixture_request(f, "POST", "/views", f->cookie, NULL, form, NULL,
	                                 &location, NULL), ==, SOUP_STATUS_FOUND);
	g_assert_cmpstr(location, ==, path);

	opened = page(f, location, 200);
	got = shown(opened);
	g_assert_cmpstr(got, ==, "1000");

	/* Listed, and pinned into the sidebar. */
	views = page(f, "/views", 200);
	g_assert_nonnull(strstr(views, "My customers"));
	g_assert_nonnull(strstr(opened, "My customers"));
}

/* --- At scale ------------------------------------------------------------------------ */

/* Monotonic milliseconds since @start. */
static gint64
elapsed_ms(gint64 start)
{
	return (g_get_monotonic_time() - start) / 1000;
}

/*
 * Fifty thousand companies, ten times the row bound: a filter the source
 * pushes down -- a chip, an equality, a date window -- reads only what it
 * keeps and answers well inside the time limit, as a list page and as the
 * filter alone; a filter it cannot push down passes the row bound and is
 * refused as such, rather than scanning the table.
 *
 * Breaks if pushdown regresses (the bound is hit), if the bound stops
 * applying (the unpushed filter runs), or a page with ?q= on a large type
 * gets slow.
 */
static void
test_scale_pushdown(
	Fixture		*f,
	gconstpointer	 data
){
	static const struct
	{
		const gchar	*filter;
		guint		 expect;
	} pushed[] = {
		{ "owner_user_id == my_id()", SCALE_ROWS / SCALE_ADA_EVERY + 2 },
		{ "(created_at >= period(\"this_month\").start and created_at < period(\"this_month\").end)",
		  SCALE_ROWS / SCALE_FRESH_EVERY + G_N_ELEMENTS(companies) },
		{ "name == \"Scale 12345\"", 1 },
		{ "owner_user_id == my_id() and name == \"Scale 500\"", 1 }
	};
	g_autoptr(LqlEnvironment) environment = NULL;
	guint i;

	(void)data;

	environment = environment_for(f, f->ada_id);

	for (i = 0; i < G_N_ELEMENTS(pushed); i++)
	{
		g_autoptr(VentureLqlProblem) problem = NULL;
		g_autoptr(GArray) ids = NULL;
		g_autofree gchar *text = NULL;
		g_autofree gchar *taken = NULL;
		gint64 start;
		gint64 took;

		text = g_strdup_printf("fetch company | filter %s", pushed[i].filter);
		taken = pushdowns(environment, text);
		g_assert_null(strpbrk(taken, "HN"));

		start = g_get_monotonic_time();
		ids = venture_lql_filter_find_ids(environment, "company", pushed[i].filter, FALSE,
		                                  NULL, &problem);
		took = elapsed_ms(start);

		if (NULL == ids)
			g_error("%s: %s", pushed[i].filter, problem->message);

		g_test_message("%s: %u rows in %" G_GINT64_FORMAT " ms", pushed[i].filter,
		               ids->len, took);
		g_assert_cmpuint(ids->len, ==, pushed[i].expect);
		g_assert_cmpint(took, <, SCALE_TIMEOUT_MS);
	}

	/* The same through the list page and the list API, on a read worker. */
	{
		g_autofree gchar *q = escaped("owner_user_id == my_id()");
		g_autofree gchar *path = g_strconcat("/e/company?q=", q, NULL);
		g_autofree gchar *api_path = g_strconcat("/api/v1/company?limit=1&q=", q, NULL);
		g_autofree gchar *body = NULL;
		g_autofree gchar *count = NULL;
		g_autoptr(JsonNode) answer = NULL;
		gint64 start;

		start = g_get_monotonic_time();
		body = page(f, path, 200);
		g_assert_cmpint(elapsed_ms(start), <, SCALE_TIMEOUT_MS);
		count = g_strdup_printf("%u records", SCALE_ROWS / SCALE_ADA_EVERY + 2);
		g_assert_nonnull(strstr(body, count));

		start = g_get_monotonic_time();
		answer = api(f, f->ada_secret, api_path, 200);
		g_assert_cmpint(elapsed_ms(start), <, SCALE_TIMEOUT_MS);
		g_assert_cmpint(json_object_get_int_member(json_node_get_object(answer), "total"), ==,
		                SCALE_ROWS / SCALE_ADA_EVERY + 2);
	}

	/* Not pushed down: a computed comparison has to read every row, and
	 * fifty thousand is past the bound, so it is refused, fast. */
	{
		g_autoptr(VentureLqlProblem) problem = NULL;
		g_autoptr(GArray) ids = NULL;
		const gchar *filter = "lower(name) == \"scale 12345\"";
		g_autofree gchar *text = g_strdup_printf("fetch company | filter %s", filter);
		g_autofree gchar *taken = pushdowns(environment, text);
		gint64 start;

		g_assert_cmpstr(taken, ==, "N");
		start = g_get_monotonic_time();
		ids = venture_lql_filter_find_ids(environment, "company", filter, FALSE, NULL, &problem);
		g_assert_null(ids);
		g_assert_nonnull(problem);
		g_assert_false(problem->positioned);
		g_test_message("unpushed: %s (%" G_GINT64_FORMAT " ms)", problem->message,
		               elapsed_ms(start));
		g_assert_cmpint(elapsed_ms(start), <, SCALE_TIMEOUT_MS);
	}
}

int
main(
	int	  argc,
	char	**argv
){
	g_test_init(&argc, &argv, NULL);
	g_setenv("VENTURE_TEST_SESSION_SECRET", "lql-lists-test-secret", TRUE);

#define ADD(path, func) \
	g_test_add(path, Fixture, &plain, fixture_setup, func, fixture_teardown); \
	g_test_add(path "/read-workers", Fixture, &with_workers, fixture_setup, func, fixture_teardown)

	ADD("/lql-lists/chips/generated", test_chips_generated);
	g_test_add("/lql-lists/chips/every-type", Fixture, &plain, fixture_setup,
	           test_chips_every_type, fixture_teardown);
	ADD("/lql-lists/page/bar-and-chips", test_list_bar);
	ADD("/lql-lists/page/problem-in-the-bar", test_list_problem);
	ADD("/lql-lists/page/financial-list", test_list_invoices);
	ADD("/lql-lists/gate/module-off", test_gate_module_off);
	ADD("/lql-lists/gate/roles", test_gate_roles);
	ADD("/lql-lists/saved-views/any-type", test_saved_views);
	g_test_add("/lql-lists/scale/50k-pushdown", Fixture, &at_scale, fixture_setup,
	           test_scale_pushdown, fixture_teardown);

#undef ADD

	return g_test_run();
}
