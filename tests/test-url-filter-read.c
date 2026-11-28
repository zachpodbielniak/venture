/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * test-url-filter-read.c - ?field= list filters compare the value as read
 *
 * Copyright (C) 2026 Zach Podbielniak
 *
 * A column a later migration added is NULL in every older row, while the
 * record reads a fixed value there: ticket.priority reads "low" (its
 * enum's zero member), deal.committed FALSE and deal.probability 0. What
 * VENTURE #50 promises, and what breaks if it regresses:
 *
 *   - Every user-facing ?field= filter -- the record list /e/<type>, its
 *     CSV export, the ticket board /tickets and the list API
 *     /api/v1/<type> -- keeps exactly the records whose GET reads a
 *     passing value, and so agrees with the LQL bar's filter for the same
 *     question (?q=), row for row.
 *   - ?f=<the value a NULL reads as> keeps the NULL row; ?f=<another
 *     value> leaves it out; the negations (__ne, __not_in) keep it
 *     unless they exclude what it reads; __is_null matches nothing.
 *   - A filter the database cannot answer as read (like, here) still
 *     compares the stored column rather than refusing the request.
 *
 * Against fd0a7292's src the cases that move the NULL row fail on every
 * URL surface: the filters there compared the stored column.
 */

#include <venture.h>
#include <libsoup/soup.h>
#include <string.h>
#include "venture-test-util.h"

#define TEST_PASSWORD "url-filter-long-password"

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
	gchar			*cookie;
	gchar			*ada_secret;
	gint64			 ticket_ids[5];
	gint64			 deal_ids[4];
} Fixture;

/*
 * The tickets, by title (none a substring of another, or of anything a
 * list page prints). Bravo is the legacy row: written "normal", then its
 * priority column set NULL, so it reads "low".
 */
static const gchar * const ticket_titles[] = {
	"Alpha ufr ticket", "Bravo ufr ticket", "Charlie ufr ticket",
	"Delta ufr ticket", "Echo ufr ticket"
};
static const VenturePriority ticket_priorities[] = {
	VENTURE_PRIORITY_LOW, VENTURE_PRIORITY_NORMAL, VENTURE_PRIORITY_HIGH,
	VENTURE_PRIORITY_NORMAL, VENTURE_PRIORITY_URGENT
};
#define LEGACY_TICKET (1)

/*
 * The deals: committed and probability as stored. Golf is the legacy
 * row, with both columns NULL, so it reads FALSE and 0.
 */
static const gchar * const deal_names[] = {
	"Foxtrot ufr deal", "Golf ufr deal", "Hotel ufr deal", "India ufr deal"
};
static const gchar * const deal_columns[] = {
	"\"committed\" = 1, \"probability\" = 50",
	"\"committed\" = NULL, \"probability\" = NULL",
	"\"committed\" = 0, \"probability\" = 10",
	"\"committed\" = 1, \"probability\" = 0"
};
#define LEGACY_DEAL (1)

/* A user with a password, an owner of the business. */
static gint64
fixture_user(
	Fixture		*f,
	const gchar	*username
){
	g_autoptr(VentureUser) user = NULL;
	g_autoptr(VentureEntity) member = NULL;
	gint64 id;

	user = venture_user_new();
	g_object_set(user, "username", username, "role", VENTURE_USER_ROLE_ADMIN,
	             "active", TRUE, NULL);
	g_assert_true(venture_user_set_password(user, TEST_PASSWORD, 100000, NULL));
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(user), NULL, NULL));
	id = venture_entity_get_id(VENTURE_ENTITY(user));

	member = g_object_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP, "organization-id", f->org,
		"user-id", id, "active", TRUE, "role", VENTURE_ORGANIZATION_ROLE_OWNER, NULL);
	g_assert_true(venture_database_save(f->db, member, NULL, NULL));

	return id;
}

/* An admin API token for @user_id; its secret. */
static gchar *
fixture_token(
	Fixture		*f,
	gint64		 user_id
){
	g_autoptr(VentureApiToken) token = venture_api_token_new();
	gchar *secret;

	g_object_set(token, "name", "ufr-token", "user-id", user_id,
	             "role", VENTURE_USER_ROLE_ADMIN, NULL);
	secret = venture_api_token_generate(token);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(token), NULL, NULL));

	return secret;
}

/* Runs @statement on the fixture's database, which must succeed. */
static void
fixture_sql(
	Fixture		*f,
	const gchar	*statement
){
	g_autoptr(GError) error = NULL;

	g_assert_true(venture_database_execute(f->db, statement, NULL, &error));
	g_assert_no_error(error);
}

/* The table @type is stored in. */
static gchar *
fixture_table(GType type)
{
	g_autoptr(VentureEntity) prototype = g_object_new(type, NULL);

	return g_strdup(venture_entity_get_table_name(prototype));
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

/* GET @path in ada's browser session, which must answer 200; the body. */
static gchar *
page(
	Fixture		*f,
	const gchar	*path
){
	gchar *body = NULL;
	guint status;

	status = fixture_request(f, "GET", path, f->cookie, NULL, NULL, &body, NULL);

	if (SOUP_STATUS_OK != status)
		g_error("GET %s: %u\n%s", path, status, body);

	return body;
}

/* GET @path with ada's token, which must answer 200; the JSON, parsed. */
static JsonNode *
api(
	Fixture		*f,
	const gchar	*path
){
	g_autofree gchar *body = NULL;
	JsonNode *node;
	guint status;

	status = fixture_request(f, "GET", path, NULL, f->ada_secret, NULL, &body, NULL);

	if (SOUP_STATUS_OK != status)
		g_error("GET %s: %u\n%s", path, status, body);

	node = venture_json_parse(body, NULL);
	g_assert_nonnull(node);

	return node;
}

static void
fixture_setup(
	Fixture		*f,
	gconstpointer	 data
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *set_cookie = NULL;
	g_autofree gchar *login = NULL;
	g_autofree gchar *tickets = fixture_table(VENTURE_TYPE_TICKET);
	g_autofree gchar *deals = fixture_table(VENTURE_TYPE_DEAL);
	gchar *semicolon;
	guint i;

	(void)data;

	f->directory = g_dir_make_tmp("venture-url-filter-read-XXXXXX", NULL);
	f->config = venture_config_new();
	g_object_set(f->config, "state-dir", f->directory, "server-bind-address", "127.0.0.1",
	             "server-port", (gint64)0, "security-require-auth", TRUE,
	             "security-session-secret-env", "VENTURE_TEST_SESSION_SECRET", NULL);
	f->db = venture_database_new("sqlite://:memory:", NULL);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), NULL));
	f->context = venture_context_new(f->config, f->db);
	f->org = venture_context_get_default_organization_id(f->context);

	f->ada_id = fixture_user(f, "ada");
	f->ada_secret = fixture_token(f, f->ada_id);

	for (i = 0; i < G_N_ELEMENTS(ticket_titles); i++)
	{
		g_autoptr(VentureEntity) ticket = NULL;

		ticket = g_object_new(VENTURE_TYPE_TICKET, "title", ticket_titles[i],
			"organization-id", f->org, "priority", ticket_priorities[i],
			"status", VENTURE_TICKET_STATUS_TODO,
			"kind", VENTURE_TICKET_KIND_INTERNAL, NULL);
		g_assert_true(venture_database_save(f->db, ticket, NULL, &error));
		g_assert_no_error(error);
		f->ticket_ids[i] = venture_entity_get_id(ticket);
	}

	/* Bravo as a row older than the priority column. */
	{
		g_autofree gchar *sql = g_strdup_printf(
			"UPDATE \"%s\" SET \"priority\" = NULL WHERE \"id\" = %" G_GINT64_FORMAT,
			tickets, f->ticket_ids[LEGACY_TICKET]);

		fixture_sql(f, sql);
	}

	/* The deals' columns are written by hand, so a save's own idea of a
	 * stage's probability does not get in the way. */
	for (i = 0; i < G_N_ELEMENTS(deal_names); i++)
	{
		g_autoptr(VentureEntity) deal = NULL;
		g_autofree gchar *sql = NULL;

		deal = g_object_new(VENTURE_TYPE_DEAL, "name", deal_names[i],
		                    "organization-id", f->org, NULL);
		g_assert_true(venture_database_save(f->db, deal, NULL, &error));
		g_assert_no_error(error);
		f->deal_ids[i] = venture_entity_get_id(deal);

		sql = g_strdup_printf("UPDATE \"%s\" SET %s WHERE \"id\" = %" G_GINT64_FORMAT,
		                      deals, deal_columns[i], f->deal_ids[i]);
		fixture_sql(f, sql);
	}

	f->server = venture_web_server_new(f->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(f->server, &error));
	g_assert_no_error(error);
	f->session = soup_session_new();

	login = g_strdup_printf("username=ada&password=%s", TEST_PASSWORD);
	g_assert_cmpuint(fixture_request(f, "POST", "/login", NULL, NULL, login, NULL,
	                                 &set_cookie), ==, SOUP_STATUS_FOUND);
	g_assert_nonnull(set_cookie);
	semicolon = strchr(set_cookie, ';');

	if (NULL != semicolon)
		*semicolon = '\0';

	f->cookie = g_steal_pointer(&set_cookie);
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

/* --- What each surface keeps ------------------------------------------------ */

/*
 * One question, asked of every surface.
 *
 * @url: the ?field= filter, as a query string fragment
 * @lql: the same question for the LQL bar (?q=)
 * @field: the field the expectation reads from each record's GET
 * @op: "eq", "ne", "in", "not_in", "is_null", "not_null" or "lt"
 * @operands: comma separated, as the URL spells them
 */
typedef struct
{
	const gchar	*url;
	const gchar	*lql;
	const gchar	*field;
	const gchar	*op;
	const gchar	*operands;
} FilterCase;

/* A JSON member as text: a string itself, a boolean "true" or "false",
 * an integer in decimal; NULL for a null or missing member. */
static gchar *
member_text(
	JsonObject	*object,
	const gchar	*member
){
	JsonNode *node;

	if (!json_object_has_member(object, member))
		return NULL;

	node = json_object_get_member(object, member);

	if (JSON_NODE_HOLDS_NULL(node) || !JSON_NODE_HOLDS_VALUE(node))
		return NULL;

	switch (json_node_get_value_type(node))
	{
	case G_TYPE_STRING:
		return g_strdup(json_node_get_string(node));

	case G_TYPE_BOOLEAN:
		return g_strdup(json_node_get_boolean(node) ? "true" : "false");

	case G_TYPE_INT64:
		return g_strdup_printf("%" G_GINT64_FORMAT, json_node_get_int(node));

	default:
		return NULL;
	}
}

/*
 * Whether the value @read (a record's GET, as member_text() spells it)
 * passes @c, by LQL's rules: a NULL passes nothing but is_null.
 */
static gboolean
case_passes(
	const FilterCase	*c,
	const gchar		*read
){
	g_auto(GStrv) operands = g_strsplit(c->operands, ",", -1);
	gboolean found = FALSE;
	guint i;

	if (0 == g_strcmp0(c->op, "is_null"))
		return NULL == read;

	if (NULL == read)
		return FALSE;

	if (0 == g_strcmp0(c->op, "not_null"))
		return TRUE;

	if (0 == g_strcmp0(c->op, "lt"))
		return g_ascii_strtoll(read, NULL, 10) < g_ascii_strtoll(operands[0], NULL, 10);

	for (i = 0; NULL != operands[i]; i++)
		found = found || (0 == g_strcmp0(read, operands[i]));

	if ((0 == g_strcmp0(c->op, "eq")) || (0 == g_strcmp0(c->op, "in")))
		return found;

	return !found;
}

/* The record of a GET /api/v1/<type>/<id> answer, bare or wrapped. */
static JsonObject *
record_of(JsonNode *answer)
{
	JsonObject *object = json_node_get_object(answer);

	if (json_object_has_member(object, "record"))
		return json_object_get_object_member(object, "record");

	return object;
}

/* Which of @names (n of them) a JSON list answer's records carry in
 * @member, as a string of 0 and 1. */
static gchar *
api_listed(
	JsonNode		*answer,
	const gchar		*member,
	const gchar * const	*names,
	guint			 n
){
	JsonArray *records;
	GString *out = g_string_new(NULL);
	guint i;
	guint j;

	records = json_object_get_array_member(json_node_get_object(answer), "records");

	for (i = 0; i < n; i++)
	{
		gboolean found = FALSE;

		for (j = 0; j < json_array_get_length(records); j++)
			found = found || (0 == g_strcmp0(names[i], json_object_get_string_member(
				json_array_get_object_element(records, j), member)));

		g_string_append_c(out, found ? '1' : '0');
	}

	return g_string_free(out, FALSE);
}

/* Which of @names a page or file mentions, as a string of 0 and 1. */
static gchar *
text_listed(
	const gchar		*text,
	const gchar * const	*names,
	guint			 n
){
	GString *out = g_string_new(NULL);
	guint i;

	for (i = 0; i < n; i++)
		g_string_append_c(out, (NULL != strstr(text, names[i])) ? '1' : '0');

	return g_string_free(out, FALSE);
}

/*
 * Every case of @cases on @type: what each record's GET reads decides
 * the expected rows, and the list API's ?field=, its ?q= (the LQL bar),
 * the record list page, its CSV export and, for tickets, the board must
 * each keep exactly those. Every case is run before failing, so a
 * regression names every surface it breaks.
 */
static void
check_cases(
	Fixture			*f,
	const gchar		*type,
	const gchar		*name_member,
	const gchar * const	*names,
	const gint64		*ids,
	guint			 n,
	const FilterCase	*cases,
	guint			 n_cases
){
	g_autoptr(GString) failures = g_string_new(NULL);
	gboolean board = (0 == g_strcmp0(type, "ticket"));
	guint i;

	for (i = 0; i < n_cases; i++)
	{
		g_autoptr(GString) expected = g_string_new(NULL);
		g_autofree gchar *q = g_uri_escape_string(cases[i].lql, NULL, FALSE);
		g_autofree gchar *api_path = g_strdup_printf("/api/v1/%s?%s&limit=500", type, cases[i].url);
		g_autofree gchar *bar_path = g_strdup_printf("/api/v1/%s?q=%s&limit=500", type, q);
		g_autofree gchar *list_path = g_strdup_printf("/e/%s?%s", type, cases[i].url);
		g_autofree gchar *export_path = g_strdup_printf("/e/%s/export?%s", type, cases[i].url);
		g_autofree gchar *board_path = g_strdup_printf("/tickets?%s", cases[i].url);
		g_autoptr(JsonNode) listed = api(f, api_path);
		g_autoptr(JsonNode) barred = api(f, bar_path);
		g_autofree gchar *list_page = page(f, list_path);
		g_autofree gchar *export_file = page(f, export_path);
		g_autofree gchar *api_rows = api_listed(listed, name_member, names, n);
		g_autofree gchar *bar_rows = api_listed(barred, name_member, names, n);
		g_autofree gchar *list_rows = text_listed(list_page, names, n);
		g_autofree gchar *export_rows = text_listed(export_file, names, n);
		g_autofree gchar *board_rows = NULL;
		guint k;

		/* What each record reads, as GET /api/v1/<type>/<id> says. */
		for (k = 0; k < n; k++)
		{
			g_autofree gchar *path = g_strdup_printf("/api/v1/%s/%" G_GINT64_FORMAT, type, ids[k]);
			g_autoptr(JsonNode) got = api(f, path);
			g_autofree gchar *read = member_text(record_of(got), cases[i].field);

			g_string_append_c(expected, case_passes(&cases[i], read) ? '1' : '0');
		}

		if (board)
		{
			g_autofree gchar *board_page = page(f, board_path);

			board_rows = text_listed(board_page, names, n);
		}

		g_test_message("%s?%s: GET %s, api %s, bar %s, list %s, export %s, board %s",
		               type, cases[i].url, expected->str, api_rows, bar_rows, list_rows,
		               export_rows, board ? board_rows : "-");

#define CHECK_SURFACE(label, rows) \
		if (0 != g_strcmp0((rows), expected->str)) \
			g_string_append_printf(failures, "\n  %s?%s: %s [%s], GET reads [%s]", \
			                       type, cases[i].url, (label), (rows), expected->str)

		CHECK_SURFACE("list API", api_rows);
		CHECK_SURFACE("LQL bar (API ?q=)", bar_rows);
		CHECK_SURFACE("/e list", list_rows);
		CHECK_SURFACE("CSV export", export_rows);
		if (board)
			CHECK_SURFACE("ticket board", board_rows);
#undef CHECK_SURFACE
	}

	g_assert_cmpstr(failures->str, ==, "");
}

/*
 * ticket.priority, an enum: Bravo's NULL column reads "low". Breaks if
 * any surface's ?priority= compares the stored column again.
 */
static void
test_ticket_priority(
	Fixture		*f,
	gconstpointer	 data
){
	static const FilterCase cases[] = {
		{ "priority=low",                "priority == \"low\"",               "priority", "eq",       "low" },
		{ "priority=high",               "priority == \"high\"",              "priority", "eq",       "high" },
		{ "priority__ne=low",            "priority != \"low\"",               "priority", "ne",       "low" },
		{ "priority__ne=high",           "priority != \"high\"",              "priority", "ne",       "high" },
		{ "priority__in=low,high",       "priority in [\"low\", \"high\"]",   "priority", "in",       "low,high" },
		{ "priority__in=normal,urgent",  "priority in [\"normal\", \"urgent\"]", "priority", "in",    "normal,urgent" },
		{ "priority__not_in=low,normal", "priority not in [\"low\", \"normal\"]", "priority", "not_in", "low,normal" },
		{ "priority__not_in=urgent",     "priority not in [\"urgent\"]",      "priority", "not_in",   "urgent" },
		{ "priority__is_null=true",      "is_null(priority)",                 "priority", "is_null",  "" },
		{ "priority__not_null=true",     "is_not_null(priority)",             "priority", "not_null", "" },
	};

	(void)data;

	/* The legacy row reads what the cases rely on, and is NULL stored. */
	{
		g_autofree gchar *path = g_strdup_printf("/api/v1/ticket/%" G_GINT64_FORMAT,
		                                         f->ticket_ids[LEGACY_TICKET]);
		g_autoptr(JsonNode) got = api(f, path);
		g_autofree gchar *read = member_text(record_of(got), "priority");
		g_autoptr(VentureQuery) stored = venture_query_new(VENTURE_TYPE_TICKET);

		g_assert_cmpstr(read, ==, "low");
		g_assert_true(venture_query_add_filter_string(stored, "priority",
			VENTURE_FILTER_OP_IS_NULL, NULL, NULL));
		venture_query_set_organization(stored, f->org);
		g_assert_cmpint(venture_database_count(f->db, stored, NULL), ==, 1);
	}

	check_cases(f, "ticket", "title", ticket_titles, f->ticket_ids,
	            G_N_ELEMENTS(ticket_titles), cases, G_N_ELEMENTS(cases));

	/* Not vacuous: ?priority=low is Alpha and the legacy Bravo. */
	{
		g_autoptr(JsonNode) low = api(f, "/api/v1/ticket?priority=low");
		g_autofree gchar *rows = api_listed(low, "title", ticket_titles,
		                                    G_N_ELEMENTS(ticket_titles));

		g_assert_cmpstr(rows, ==, "11000");
	}
}

/*
 * deal.committed (a boolean, NULL reads FALSE) and deal.probability (an
 * integer, NULL reads 0), on another type and through the generic
 * record list rather than the ticket views.
 */
static void
test_deal_boolean_and_integer(
	Fixture		*f,
	gconstpointer	 data
){
	static const FilterCase cases[] = {
		{ "committed=false",          "committed == FALSE",        "committed",   "eq",      "false" },
		{ "committed=true",           "committed == TRUE",         "committed",   "eq",      "true" },
		{ "committed__ne=true",       "committed != TRUE",         "committed",   "ne",      "true" },
		{ "committed__is_null=true",  "is_null(committed)",        "committed",   "is_null", "" },
		{ "probability=0",            "probability == 0",          "probability", "eq",      "0" },
		{ "probability=50",           "probability == 50",         "probability", "eq",      "50" },
		{ "probability__lt=20",       "probability < 20",          "probability", "lt",      "20" },
		{ "probability__in=0,50",     "probability in [0, 50]",    "probability", "in",      "0,50" },
		{ "probability__not_in=0",    "probability not in [0]",    "probability", "not_in",  "0" },
	};

	(void)data;

	{
		g_autofree gchar *path = g_strdup_printf("/api/v1/deal/%" G_GINT64_FORMAT,
		                                         f->deal_ids[LEGACY_DEAL]);
		g_autoptr(JsonNode) got = api(f, path);
		g_autofree gchar *committed = member_text(record_of(got), "committed");
		g_autofree gchar *probability = member_text(record_of(got), "probability");

		g_assert_cmpstr(committed, ==, "false");
		g_assert_cmpstr(probability, ==, "0");
	}

	check_cases(f, "deal", "name", deal_names, f->deal_ids,
	            G_N_ELEMENTS(deal_names), cases, G_N_ELEMENTS(cases));
}

/*
 * A filter the database cannot answer as read still answers as it always
 * did: like on a field whose NULL reads "low" compares the stored column
 * (the legacy row is left out) rather than refusing the page.
 */
static void
test_undecidable_keeps_stored(
	Fixture		*f,
	gconstpointer	 data
){
	g_autoptr(JsonNode) like = NULL;
	g_autofree gchar *rows = NULL;
	g_autofree gchar *list_page = NULL;

	(void)data;

	like = api(f, "/api/v1/ticket?priority__like=lo%25");
	rows = api_listed(like, "title", ticket_titles, G_N_ELEMENTS(ticket_titles));
	g_assert_cmpstr(rows, ==, "10000");

	list_page = page(f, "/e/ticket?priority__like=lo%25");
	g_assert_nonnull(strstr(list_page, ticket_titles[0]));
}

int
main(
	int	 argc,
	char	*argv[]
){
	g_setenv("VENTURE_TEST_SESSION_SECRET", "url-filter-read-session-secret-0123456789", TRUE);
	g_test_init(&argc, &argv, NULL);

#define ADD(path, func) \
	g_test_add(path, Fixture, NULL, fixture_setup, func, fixture_teardown)

	ADD("/url-filter-read/ticket-priority", test_ticket_priority);
	ADD("/url-filter-read/deal-boolean-and-integer", test_deal_boolean_and_integer);
	ADD("/url-filter-read/undecidable-keeps-stored", test_undecidable_keeps_stored);

#undef ADD

	return g_test_run();
}
