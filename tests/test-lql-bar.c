/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * test-lql-bar.c - The ticket query bar, ?q= on lists, saved views in LQL
 *
 * Copyright (C) 2026 Zach Podbielniak
 *
 * What the filter bar (docs/lql.org, "Filter bars") promises, and what
 * breaks if it regresses:
 *
 *   - The ticket story's example queries filter the board and the list,
 *     whose lanes stay grouped by status; "Not done" is one click, and a
 *     chip pressed again comes off.
 *   - A bad query is a caret error drawn by the server (no scripting),
 *     with "did you mean", and never an empty board -- a misspelt field,
 *     which LQL would accept and match nothing with, included.
 *   - Today's URL filters (status__not_in=...) filter the board, and the
 *     bar shows them as the LQL they mean; that LQL selects the same
 *     tickets the URL filter does.
 *   - A saved view round-trips: one saved before LQL opens exactly as it
 *     did, one saved from a filtered page keeps the LQL apart and opens
 *     filtered.
 *   - GET /api/v1/ticket?q= filters the list API under the same access
 *     policy, and refuses a bad filter naming the column.
 *   - GET /lql/assist completes field names and enumeration values, and
 *     counts in UTF-16 units, as a browser does.
 *   - query.max_bytes holds for a bar's filter as for POST /api/v1/query:
 *     a filter that blows the budget is the bar's problem on the page and
 *     a 422 on the list API, and the server carries on.
 *   - A filter is evaluated off the main loop whether or not the server
 *     has read workers: while a slow one runs, other requests are
 *     answered.
 */

#include <venture.h>
#include <libsoup/soup.h>
#include <string.h>
#include "lql/venture-lql.h"
#include "venture-test-util.h"

#define TEST_PASSWORD "bar-long-password"

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
	gint64			 vee_id;
	gint64			 team_id;
	gint64			 repo_id;
	gchar			*cookie;
	gchar			*ada_secret;
	gchar			*vee_secret;
} Fixture;

/* The tickets, by title, and what the example queries make of them. */
typedef struct
{
	const gchar		*title;
	VentureTicketStatus	 status;
	VenturePriority		 priority;
	VentureIssueType	 issue_type;
	const gchar		*assignee;
	gboolean		 on_team;
	gboolean		 stale;		/* last updated in 2020 */
} TicketSeed;

static const TicketSeed seeds[] = {
	{ "Crash on save",     VENTURE_TICKET_STATUS_TODO,        VENTURE_PRIORITY_HIGH,   VENTURE_ISSUE_TYPE_BUG,   "ada", FALSE, FALSE },
	{ "Old outage",        VENTURE_TICKET_STATUS_IN_PROGRESS, VENTURE_PRIORITY_URGENT, VENTURE_ISSUE_TYPE_BUG,   "ana", FALSE, TRUE  },
	{ "Low bug",           VENTURE_TICKET_STATUS_TRIAGE,      VENTURE_PRIORITY_LOW,    VENTURE_ISSUE_TYPE_BUG,   NULL,  FALSE, FALSE },
	{ "Add an export",     VENTURE_TICKET_STATUS_DONE,        VENTURE_PRIORITY_NORMAL, VENTURE_ISSUE_TYPE_STORY, "ada", FALSE, FALSE },
	{ "Dropped idea",      VENTURE_TICKET_STATUS_CANCELLED,   VENTURE_PRIORITY_NORMAL, VENTURE_ISSUE_TYPE_STORY, "vee", FALSE, FALSE },
	{ "Team chore",        VENTURE_TICKET_STATUS_BLOCKED,     VENTURE_PRIORITY_NORMAL, VENTURE_ISSUE_TYPE_TASK,  "",    TRUE,  FALSE },
	{ "Urgent fresh bug",  VENTURE_TICKET_STATUS_REVIEW,      VENTURE_PRIORITY_URGENT, VENTURE_ISSUE_TYPE_BUG,   "ana", FALSE, FALSE }
};

/* The fixture data that asks for read workers. */
static const gchar with_workers[] = "workers";

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

/* GET @path in ada's browser session; the page. */
static gchar *
page(
	Fixture		*f,
	const gchar	*path,
	guint		 expect
){
	gchar *body = NULL;
	guint status;

	status = fixture_request(f, "GET", path, f->cookie, NULL, NULL, &body, NULL, NULL);

	if (status != expect)
		g_error("GET %s: %u, wanted %u\n%s", path, status, expect, body);

	return body;
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

static void
fixture_setup(
	Fixture		*f,
	gconstpointer	 data
){
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) team = NULL;
	g_autoptr(VentureEntity) membership = NULL;
	g_autoptr(VentureEntity) repo = NULL;
	g_autofree gchar *set_cookie = NULL;
	g_autofree gchar *login = NULL;
	gchar *semicolon;
	guint i;

	f->directory = g_dir_make_tmp("venture-lql-bar-XXXXXX", NULL);
	f->config = venture_config_new();
	g_object_set(f->config, "state-dir", f->directory, "server-bind-address", "127.0.0.1",
	             "server-port", (gint64)0, "security-require-auth", TRUE,
	             "security-session-secret-env", "VENTURE_TEST_SESSION_SECRET", NULL);

	/*
	 * With read workers, a query runs on one of them while its fiber
	 * waits; without (an in-memory database has none), in the fiber.
	 * The same tests run both ways.
	 */
	if (data == (gconstpointer)with_workers)
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

	/* ada owns the business; vee is a viewer, who sees assigned work
	 * only; ana is somebody tickets are given to. */
	f->ada_id = fixture_user(f, "ada", VENTURE_USER_ROLE_ADMIN, VENTURE_ORGANIZATION_ROLE_OWNER);
	f->vee_id = fixture_user(f, "vee", VENTURE_USER_ROLE_VIEWER, VENTURE_ORGANIZATION_ROLE_VIEWER);
	(void)fixture_user(f, "ana", VENTURE_USER_ROLE_EDITOR, VENTURE_ORGANIZATION_ROLE_EDITOR);
	f->ada_secret = fixture_token(f, "ada-token", f->ada_id, VENTURE_USER_ROLE_ADMIN);
	f->vee_secret = fixture_token(f, "vee-token", f->vee_id, VENTURE_USER_ROLE_VIEWER);

	team = g_object_new(VENTURE_TYPE_TEAM, "name", "Support", "organization-id", f->org, NULL);
	g_assert_true(venture_database_save(f->db, team, NULL, NULL));
	f->team_id = venture_entity_get_id(team);
	membership = g_object_new(VENTURE_TYPE_TEAM_MEMBERSHIP, "organization-id", f->org,
		"team-id", f->team_id, "user-id", f->ada_id, "active", TRUE, NULL);
	g_assert_true(venture_database_save(f->db, membership, NULL, NULL));

	repo = g_object_new(VENTURE_TYPE_FORGE_REPO, "name", "venture", "organization-id", f->org, NULL);
	g_assert_true(venture_database_save(f->db, repo, NULL, &error));
	g_assert_no_error(error);
	f->repo_id = venture_entity_get_id(repo);

	for (i = 0; i < G_N_ELEMENTS(seeds); i++)
	{
		g_autoptr(VentureEntity) ticket = NULL;

		ticket = g_object_new(VENTURE_TYPE_TICKET, "title", seeds[i].title,
			"organization-id", f->org, "status", seeds[i].status,
			"priority", seeds[i].priority, "issue-type", seeds[i].issue_type,
			"assignee", seeds[i].assignee, "kind", VENTURE_TICKET_KIND_INTERNAL, NULL);

		if (seeds[i].on_team)
			g_object_set(ticket, "team-id", f->team_id, NULL);

		if (0 == i)
			g_object_set(ticket, "repo-id", f->repo_id, NULL);

		g_assert_true(venture_database_save(f->db, ticket, NULL, &error));
		g_assert_no_error(error);

		/* Aged by hand: a save always stamps now. */
		if (seeds[i].stale)
		{
			g_autofree gchar *sql = g_strdup_printf(
				"UPDATE tickets SET updated_at = '2020-01-01T00:00:00Z' WHERE id = %"
				G_GINT64_FORMAT, venture_entity_get_id(ticket));

			g_assert_true(venture_database_execute(f->db, sql, NULL, &error));
			g_assert_no_error(error);
		}
	}

	f->server = venture_web_server_new(f->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(f->server, &error));
	g_assert_no_error(error);
	f->session = soup_session_new();

	login = g_strdup_printf("username=ada&password=%s", TEST_PASSWORD);
	g_assert_cmpuint(fixture_request(f, "POST", "/login", NULL, NULL, login, NULL, NULL,
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
	g_clear_pointer(&f->vee_secret, g_free);
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
environment_for_ada(Fixture *f)
{
	VentureAuthPrincipal ada;
	g_autoptr(GError) error = NULL;
	LqlEnvironment *environment;

	memset(&ada, 0, sizeof ada);
	ada.user_id = f->ada_id;
	ada.role = VENTURE_USER_ROLE_ADMIN;
	ada.authenticated = TRUE;

	environment = venture_lql_environment_new(f->context, &ada, &f->org, 1,
		allow_every_type, NULL, &error);
	g_assert_no_error(error);

	return environment;
}

/* Which of the seeded titles appear on @html, as "1010000" in seed order. */
static gchar *
shown(const gchar *html)
{
	GString *out = g_string_new(NULL);
	guint i;

	for (i = 0; i < G_N_ELEMENTS(seeds); i++)
		g_string_append_c(out, (NULL != strstr(html, seeds[i].title)) ? '1' : '0');

	return g_string_free(out, FALSE);
}

/* The same, of the titles of a JSON list API answer. */
static gchar *
listed(JsonNode *answer)
{
	JsonArray *records;
	GString *out = g_string_new(NULL);
	guint i;
	guint j;

	records = json_object_get_array_member(json_node_get_object(answer), "records");

	for (i = 0; i < G_N_ELEMENTS(seeds); i++)
	{
		gboolean found = FALSE;

		for (j = 0; j < json_array_get_length(records); j++)
			found = found || (0 == g_strcmp0(seeds[i].title,
				json_object_get_string_member(json_array_get_object_element(records, j), "title")));

		g_string_append_c(out, found ? '1' : '0');
	}

	return g_string_free(out, FALSE);
}

/* --- The board and the list ------------------------------------------------------- */

/*
 * The story's example queries, on the board and on the list, select
 * exactly the tickets they say; the board's lanes stay, grouped by
 * status, with only the matching cards in them.
 */
static void
test_example_queries(
	Fixture		*f,
	gconstpointer	 data
){
	static const struct
	{
		const gchar	*query;
		const gchar	*expect;
	} cases[] = {
		{ "status not in [\"done\", \"cancelled\"]", "1110011" },
		/* As written in the story: assignee in my_teams() compares a name
		 * with team ids, so only the first half ever matches... */
		{ "assignee == me() or assignee in my_teams()", "1001000" },
		/* ...and what it means: mine, or my team's. */
		{ "assignee == me() or team_id in my_teams()", "1001010" },
		{ "issue_type == \"bug\" and priority in [\"high\",\"urgent\"] and updated_at > ago(7d)",
		  "1000001" },
		{ "not (issue_type == \"bug\")", "0001110" },
		{ "filter status == \"todo\"", "1000000" },
		{ "repo_label == \"venture\"", "1000000" },
		{ "status == \"done\" and title == \"nothing like it\"", "0000000" }
	};
	guint i;

	(void)data;

	for (i = 0; i < G_N_ELEMENTS(cases); i++)
	{
		g_autofree gchar *q = escaped(cases[i].query);
		g_autofree gchar *board_path = g_strconcat("/tickets?view=board&q=", q, NULL);
		g_autofree gchar *list_path = g_strconcat("/tickets?view=list&q=", q, NULL);
		g_autofree gchar *board = page(f, board_path, 200);
		g_autofree gchar *list = page(f, list_path, 200);
		g_autofree gchar *on_board = shown(board);
		g_autofree gchar *on_list = shown(list);
		g_autofree gchar *board_expect = g_strdup(cases[i].expect);

		/* The board has no cancelled lane (an outcome, not a stage):
		 * "Dropped idea" is only ever on the list. */
		board_expect[4] = '0';

		g_test_message("%s: board %s, list %s", cases[i].query, on_board, on_list);
		g_assert_cmpstr(on_board, ==, board_expect);
		g_assert_cmpstr(on_list, ==, cases[i].expect);

		/* Every lane is still there, matching or not. */
		g_assert_nonnull(strstr(board, "data-status=\"triage\""));
		g_assert_nonnull(strstr(board, "data-status=\"done\""));
		g_assert_null(strstr(board, "lql-problem"));

		/* The bar holds the query, highlighted underneath. */
		g_assert_nonnull(strstr(board, "data-lql-bar"));
		g_assert_nonnull(strstr(board, "<span class=\"lql-field\">"));
	}
}

/*
 * "Not done" is one click: the chip is a link to the filtered board, it
 * shows as on there, and pressing it again takes it off again while the
 * rest of the filter stays.
 */
static void
test_chips(
	Fixture		*f,
	gconstpointer	 data
){
	g_autofree gchar *start = NULL;
	g_autofree gchar *not_done_href = NULL;
	g_autofree gchar *not_done = NULL;
	g_autofree gchar *both = NULL;
	g_autofree gchar *q = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *expect = NULL;
	const gchar *chip;
	const gchar *href;
	const gchar *end;

	(void)data;

	start = page(f, "/tickets?view=board", 200);
	chip = strstr(start, "data-lql-chip=\"status not in [&quot;done&quot;, &quot;cancelled&quot;]\"");
	g_assert_nonnull(chip);
	href = strstr(chip, "href=\"");
	g_assert_nonnull(href);
	href += strlen("href=\"");
	end = strchr(href, '"');
	not_done_href = g_strndup(href, (gsize)(end - href));

	/* The link carries the chip's LQL, and keeps the view. */
	q = escaped("status not in [\"done\", \"cancelled\"]");
	expect = g_strconcat("/tickets?view=board&amp;q=", q, NULL);
	g_assert_cmpstr(not_done_href, ==, expect);

	path = g_strconcat("/tickets?view=board&q=", q, NULL);
	not_done = page(f, path, 200);
	{
		g_autofree gchar *on = shown(not_done);

		g_assert_cmpstr(on, ==, "1110011");
	}
	g_assert_nonnull(strstr(not_done, "class=\"query-chip active\" data-lql-chip=\"status not in"));

	/* Not done and Bugs together, then Not done pressed again. */
	{
		g_autoptr(LqlEnvironment) environment = environment_for_ada(f);
		g_autofree gchar *with_bugs = venture_lql_filter_toggle(environment, "ticket",
			"status not in [\"done\", \"cancelled\"]", "issue_type == \"bug\"");
		g_autofree gchar *without = venture_lql_filter_toggle(environment, "ticket", with_bugs,
			"status not in [\"done\",\"cancelled\"]");
		g_autofree gchar *around_or = venture_lql_filter_toggle(environment, "ticket",
			"priority == \"low\" or priority == \"high\"", "issue_type == \"bug\"");
		g_autofree gchar *unassigned_off = venture_lql_filter_toggle(environment, "ticket",
			"(is_null(assignee) or assignee == \"\") and issue_type == \"bug\"",
			"(is_null(assignee) or assignee == \"\")");

		g_assert_cmpstr(with_bugs, ==, "status not in [\"done\", \"cancelled\"] and issue_type == \"bug\"");
		g_assert_cmpstr(without, ==, "issue_type == \"bug\"");
		g_assert_cmpstr(around_or, ==, "(priority == \"low\" or priority == \"high\") and issue_type == \"bug\"");
		g_assert_cmpstr(unassigned_off, ==, "issue_type == \"bug\"");
		g_assert_true(venture_lql_filter_has_conjunct(environment, "ticket", with_bugs, "issue_type==\"bug\""));
		g_assert_false(venture_lql_filter_has_conjunct(environment, "ticket",
			"issue_type == \"bug\" or x == 1", "issue_type == \"bug\""));

		g_free(q);
		q = escaped(with_bugs);
		g_free(path);
		path = g_strconcat("/tickets?view=list&q=", q, NULL);
		both = page(f, path, 200);
	}
	{
		g_autofree gchar *on = shown(both);

		g_assert_cmpstr(on, ==, "1110001");
	}
	g_assert_nonnull(strstr(both, "class=\"query-chip active\" data-lql-chip=\"issue_type"));

	/* Unassigned: never given to anybody, or given and taken back. */
	{
		g_autofree gchar *uq = escaped("(is_null(assignee) or assignee == \"\")");
		g_autofree gchar *upath = g_strconcat("/tickets?view=list&q=", uq, NULL);
		g_autofree gchar *unassigned = page(f, upath, 200);
		g_autofree gchar *on = shown(unassigned);

		g_assert_cmpstr(on, ==, "0010010");
	}
}

/*
 * A bad query is drawn by the server: the message, the text with the
 * faulty part marked, a caret under it, "did you mean" links that write
 * the fix in -- and no board. A misspelt field included, which LQL itself
 * would run and match nothing with.
 */
static void
test_parse_errors(
	Fixture		*f,
	gconstpointer	 data
){
	static const struct
	{
		const gchar	*query;
		const gchar	*message;
		gint		 column;	/* 0-based caret column; -1: not checked */
		gint		 carets;	/* how many ^ */
		const gchar	*suggestion;	/* a "did you mean" link's q, or NULL */
	} cases[] = {
		{ "staus == \"todo\"", "unknown field", 0, 5, "status == \"todo\"" },
		{ "status == \"todo\" and lowr(title) == \"x\"", "unknown function", 21, 4,
		  "status == \"todo\" and lower(title) == \"x\"" },
		{ "status == \"todo", "unterminated string", 10, 5, NULL },
		{ "status == \"todo\" | sort priority", "cannot start another stage", 17, 1, NULL },
		{ "status == $who", "$who", -1, 0, NULL },
		{ "status ==", NULL, -1, 0, NULL }
	};
	guint i;

	(void)data;

	for (i = 0; i < G_N_ELEMENTS(cases); i++)
	{
		g_autofree gchar *q = escaped(cases[i].query);
		g_autofree gchar *path = g_strconcat("/tickets?view=board&q=", q, NULL);
		g_autofree gchar *body = page(f, path, 400);
		g_autofree gchar *on = shown(body);

		g_test_message("%s", cases[i].query);
		g_assert_nonnull(strstr(body, "class=\"lql-problem\" role=\"alert\""));
		g_assert_nonnull(strstr(body, "aria-invalid=\"true\""));
		g_assert_null(strstr(body, "data-board"));
		g_assert_cmpstr(on, ==, "0000000");

		if (NULL != cases[i].message)
			g_assert_nonnull(strstr(body, cases[i].message));

		if (cases[i].column >= 0)
		{
			g_autofree gchar *spaces = g_strnfill((gsize)cases[i].column, ' ');
			g_autofree gchar *carets = g_strnfill((gsize)cases[i].carets, '^');
			g_autofree gchar *line = g_strdup_printf("<span class=\"lql-caret-line\">%s%s</span>",
			                                         spaces, carets);

			if (NULL == strstr(body, line))
				g_error("no caret line \"%s%s\" in:\n%s", spaces, carets,
				        strstr(body, "lql-problem"));
		}

		if (NULL != cases[i].suggestion)
		{
			g_autofree gchar *fixed = escaped(cases[i].suggestion);
			g_autofree gchar *link = g_strconcat("href=\"/tickets?view=board&amp;q=", fixed, "\"", NULL);

			g_assert_nonnull(strstr(body, "Did you mean"));
			if (NULL == strstr(body, link))
				g_error("no link %s in:\n%s", link, strstr(body, "lql-problem"));
		}
	}

	/* The mark: the misspelt field itself, inside the highlighted text. */
	{
		g_autofree gchar *q = escaped("staus == \"todo\"");
		g_autofree gchar *path = g_strconcat("/tickets?q=", q, NULL);
		g_autofree gchar *body = page(f, path, 400);

		g_assert_nonnull(strstr(body,
			"<mark class=\"lql-mark\"><span class=\"lql-field\">staus</span></mark>"));
	}
}

/*
 * Today's URL filters keep working -- now on the board too -- and the bar
 * shows them as the LQL they mean. That LQL, run as the bar's filter,
 * selects the very tickets the URL filter does.
 */
static void
test_url_filters(
	Fixture		*f,
	gconstpointer	 data
){
	static const struct
	{
		const gchar	*query;
		const gchar	*lql;
		const gchar	*expect;
	} cases[] = {
		{ "status__not_in=done,cancelled", "status not in [\"done\", \"cancelled\"]", "1110011" },
		{ "issue_type=bug&kind=internal", "issue_type == \"bug\" and kind == \"internal\"", "1110001" },
		{ "issue_type=all&kind=all&priority__in=high,urgent",
		  "priority in [\"high\", \"urgent\"]", "1100001" },
		{ "assignee=ada", "assignee == \"ada\"", "1001000" },
		{ "priority=urgent&updated_at__lt=2021-01-01", "priority == \"urgent\" and updated_at < timestamp(\"2021-01-01\")", "0100000" },
		{ "status__is_null=1", "is_null(status)", "0000000" },
		{ "title__ilike=%25bug%25", "like(title, \"%bug%\", TRUE)", "0010001" },
		{ "utm_source=mail&status=todo", "status == \"todo\"", "1000000" }
	};
	guint i;

	(void)data;

	for (i = 0; i < G_N_ELEMENTS(cases); i++)
	{
		g_autofree gchar *path = g_strconcat("/tickets?view=board&", cases[i].query, NULL);
		g_autofree gchar *body = page(f, path, 200);
		g_autofree gchar *on = shown(body);
		g_autofree gchar *value = venture_html_escape(cases[i].lql);
		g_autofree gchar *attribute = g_strconcat("value=\"", value, "\"", NULL);

		g_test_message("%s -> %s: %s", cases[i].query, cases[i].lql, on);
		g_assert_cmpstr(on, ==, cases[i].expect);

		if (NULL == strstr(body, attribute))
			g_error("the bar does not hold %s", attribute);

		/* The LQL alone gives the same board. */
		{
			g_autofree gchar *q = escaped(cases[i].lql);
			g_autofree gchar *lql_path = g_strconcat("/tickets?view=board&q=", q, NULL);
			g_autofree gchar *lql_body = page(f, lql_path, 200);
			g_autofree gchar *lql_on = shown(lql_body);

			g_assert_cmpstr(lql_on, ==, cases[i].expect);
		}
	}

	/* A URL filter and the bar together: both apply, and the bar says
	 * both, the bar's own part bracketed when it has an `or`. */
	{
		g_autofree gchar *q = escaped("priority == \"low\" or priority == \"high\"");
		g_autofree gchar *path = g_strconcat("/tickets?kind=internal&issue_type=bug&q=", q, NULL);
		g_autofree gchar *body = page(f, path, 200);
		g_autofree gchar *on = shown(body);
		g_autofree gchar *value = venture_html_escape(
			"issue_type == \"bug\" and kind == \"internal\" and "
			"(priority == \"low\" or priority == \"high\")");

		g_assert_cmpstr(on, ==, "1010000");
		g_assert_nonnull(strstr(body, value));

		/* And the kind links keep the bar's filter. */
		g_assert_nonnull(strstr(body, "/tickets?view=board&kind=external&issue_type=bug&q="));
	}

	/* The translation itself, without the web. */
	{
		g_autoptr(GHashTable) params = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
		g_autofree gchar *lql = NULL;

		g_hash_table_insert(params, g_strdup("sla_breached"), g_strdup("true"));
		g_hash_table_insert(params, g_strdup("estimate_hours__gte"), g_strdup("2"));
		g_hash_table_insert(params, g_strdup("id__between"), g_strdup("3,9"));
		g_hash_table_insert(params, g_strdup("external_ref"), g_strdup(""));
		g_hash_table_insert(params, g_strdup("order"), g_strdup("-id"));
		g_hash_table_insert(params, g_strdup("limit"), g_strdup("5"));
		g_hash_table_insert(params, g_strdup("no_such_field"), g_strdup("x"));
		g_hash_table_insert(params, g_strdup("title"), g_strdup("say \"hi\""));
		lql = venture_lql_filter_from_query_string(VENTURE_TYPE_TICKET, params);
		g_assert_cmpstr(lql, ==,
			"estimate_hours >= 2.0 and (is_null(external_ref) or external_ref == \"\") and "
			"between(id, 3, 9) and sla_breached == TRUE and title == \"say \\\"hi\\\"\"");

		g_hash_table_remove_all(params);
		g_hash_table_insert(params, g_strdup("limit"), g_strdup("5"));
		g_assert_null(venture_lql_filter_from_query_string(VENTURE_TYPE_TICKET, params));
	}
}

/* --- Saved views ------------------------------------------------------------------- */

/* The id at the end of a "/views/N" or a saved view's row, from a list. */
static VentureEntity *
saved_view_named(
	Fixture		*f,
	const gchar	*name
){
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_SAVED_VIEW);
	g_autoptr(GPtrArray) found = NULL;

	g_assert_true(venture_query_add_filter_string(query, "name", VENTURE_FILTER_OP_EQ, name, NULL));
	found = venture_database_find(f->db, query, NULL);
	g_assert_nonnull(found);
	g_assert_cmpuint(found->len, ==, 1);

	return g_object_ref(g_ptr_array_index(found, 0));
}

/*
 * A saved view round-trips. One saved before views held LQL -- a query
 * string and nothing else -- opens at exactly the address it always did.
 * One saved from a filtered board keeps the LQL in saved_view.lql, apart
 * from the rest of the query string, and opens filtered.
 */
static void
test_saved_views(
	Fixture		*f,
	gconstpointer	 data
){
	g_autoptr(VentureSavedView) old_board = NULL;
	g_autoptr(VentureSavedView) old_list = NULL;
	g_autoptr(VentureEntity) saved = NULL;
	g_autofree gchar *location = NULL;
	g_autofree gchar *form = NULL;
	g_autofree gchar *q = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *filtered = NULL;
	g_autofree gchar *stored_query = NULL;
	g_autofree gchar *stored_lql = NULL;
	g_autofree gchar *views = NULL;
	const gchar *lql = "issue_type == \"bug\" and status not in [\"done\", \"cancelled\"]";

	(void)data;

	/* Saved before LQL: a board and a generic list. */
	old_board = venture_saved_view_new();
	g_object_set(old_board, "name", "Old board", "entity-type", "ticket",
	             "query", "view=board&kind=internal&issue_type=bug", "board", TRUE,
	             "owner-user-id", f->ada_id, "organization-id", f->org, NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(old_board), NULL, NULL));
	old_list = venture_saved_view_new();
	g_object_set(old_list, "name", "Old list", "entity-type", "ticket",
	             "query", "status__not_in=done,cancelled", "board", FALSE,
	             "owner-user-id", f->ada_id, "organization-id", f->org, NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(old_list), NULL, NULL));

	path = g_strdup_printf("/views/%" G_GINT64_FORMAT,
	                       venture_entity_get_id(VENTURE_ENTITY(old_board)));
	g_assert_cmpuint(fixture_request(f, "GET", path, f->cookie, NULL, NULL, NULL, &location, NULL),
	                 ==, SOUP_STATUS_FOUND);
	g_assert_cmpstr(location, ==, "/tickets?view=board&kind=internal&issue_type=bug");
	{
		g_autofree gchar *body = page(f, location, 200);
		g_autofree gchar *on = shown(body);

		g_assert_cmpstr(on, ==, "1110001");
	}
	g_clear_pointer(&location, g_free);
	g_clear_pointer(&path, g_free);

	path = g_strdup_printf("/views/%" G_GINT64_FORMAT,
	                       venture_entity_get_id(VENTURE_ENTITY(old_list)));
	g_assert_cmpuint(fixture_request(f, "GET", path, f->cookie, NULL, NULL, NULL, &location, NULL),
	                 ==, SOUP_STATUS_FOUND);
	g_assert_cmpstr(location, ==, "/e/ticket?status__not_in=done,cancelled");
	g_clear_pointer(&location, g_free);
	g_clear_pointer(&path, g_free);

	/* The save form on a filtered board: q kept apart, as lql. */
	q = escaped(lql);
	path = g_strconcat("/tickets?view=board&q=", q, NULL);
	filtered = page(f, path, 200);
	g_assert_nonnull(strstr(filtered, "<input type=\"hidden\" name=\"query\" value=\"view=board\">"));
	{
		g_autofree gchar *value = venture_html_escape(lql);
		g_autofree gchar *field = g_strconcat("<input type=\"hidden\" name=\"lql\" value=\"",
		                                      value, "\">", NULL);

		g_assert_nonnull(strstr(filtered, field));
	}

	/* Posted as that form posts it. */
	form = g_strconcat("name=Open+bugs&entity_type=ticket&query=view%3Dboard&board=1&pinned=1&lql=",
	                   q, NULL);
	g_assert_cmpuint(fixture_request(f, "POST", "/views", f->cookie, NULL, form, NULL,
	                                 &location, NULL), ==, SOUP_STATUS_FOUND);
	{
		g_autofree gchar *expect = g_strconcat("/tickets?view=board&q=", q, NULL);

		g_assert_cmpstr(location, ==, expect);
	}

	saved = saved_view_named(f, "Open bugs");
	g_object_get(saved, "query", &stored_query, "lql", &stored_lql, NULL);
	g_assert_cmpstr(stored_query, ==, "view=board");
	g_assert_cmpstr(stored_lql, ==, lql);

	/* Opened: filtered. */
	{
		g_autofree gchar *body = page(f, location, 200);
		g_autofree gchar *on = shown(body);

		g_assert_cmpstr(on, ==, "1110001");
	}

	/* The list of views says the filter in LQL; the sidebar links it. */
	views = page(f, "/views", 200);
	{
		g_autofree gchar *code = venture_html_escape(lql);

		g_assert_nonnull(strstr(views, code));
	}

	/* A view whose LQL is all it has opens at ?q= alone; through the
	 * list API a view is just a record with one more field. */
	{
		g_autoptr(VentureSavedView) lql_only = venture_saved_view_new();
		g_autoptr(JsonNode) answer = NULL;
		g_autofree gchar *open = NULL;
		g_autofree gchar *expect = NULL;
		JsonObject *record;

		g_object_set(lql_only, "name", "Only LQL", "entity-type", "ticket",
		             "lql", "assignee == me()", "owner-user-id", f->ada_id, "organization-id", f->org, NULL);
		g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(lql_only), NULL, NULL));
		open = g_strdup_printf("/views/%" G_GINT64_FORMAT,
		                       venture_entity_get_id(VENTURE_ENTITY(lql_only)));
		g_clear_pointer(&location, g_free);
		g_assert_cmpuint(fixture_request(f, "GET", open, f->cookie, NULL, NULL, NULL,
		                                 &location, NULL), ==, SOUP_STATUS_FOUND);
		expect = g_strconcat("/e/ticket?q=", "assignee%20%3D%3D%20me%28%29", NULL);
		g_assert_cmpstr(location, ==, expect);

		answer = api(f, f->ada_secret, "/api/v1/saved_view?name=Only%20LQL", 200);
		record = json_array_get_object_element(json_object_get_array_member(
			json_node_get_object(answer), "records"), 0);
		g_assert_cmpstr(json_object_get_string_member(record, "lql"), ==, "assignee == me()");
	}
}

/* --- The list API -------------------------------------------------------------------- */

/*
 * GET /api/v1/ticket?q= filters the list API: alone, beside the other
 * URL filters, paged and counted as ever, under the same access policy
 * (vee, a viewer, still sees only what is assigned to them); and a bad
 * filter is a 400 naming the column.
 */
static void
test_api_q(
	Fixture		*f,
	gconstpointer	 data
){
	g_autoptr(JsonNode) not_done = NULL;
	g_autoptr(JsonNode) with_status = NULL;
	g_autoptr(JsonNode) paged = NULL;
	g_autoptr(JsonNode) none = NULL;
	g_autoptr(JsonNode) bad = NULL;
	g_autoptr(JsonNode) viewer = NULL;
	g_autoptr(JsonNode) viewer_all = NULL;
	g_autofree gchar *q = NULL;
	g_autofree gchar *path = NULL;
	const gchar *message;

	(void)data;

	q = escaped("status not in [\"done\", \"cancelled\"]");
	path = g_strconcat("/api/v1/ticket?q=", q, NULL);
	{
		guint64 before = venture_parallel_get_jobs_run();

		not_done = api(f, f->ada_secret, path, 200);

		/* Either way the filter ran on a query worker
		 * (venture_web_query_off_loop()), never a read worker: its
		 * fetches are read back on the main thread. Off the main loop
		 * is what /lql-bar/runs/main-loop-stays-responsive pins. */
		g_assert_cmpuint(venture_parallel_get_jobs_run(), ==, before);
	}
	{
		g_autofree gchar *on = listed(not_done);

		g_assert_cmpstr(on, ==, "1110011");
	}
	g_assert_cmpint(json_object_get_int_member(json_node_get_object(not_done), "total"), ==, 5);

	/* Beside a URL filter: both apply. */
	g_free(path);
	path = g_strconcat("/api/v1/ticket?issue_type=bug&q=", q, NULL);
	with_status = api(f, f->ada_secret, path, 200);
	{
		g_autofree gchar *on = listed(with_status);

		g_assert_cmpstr(on, ==, "1110001");
	}

	/* Paged and counted as ever: the total is the filter's. */
	g_free(path);
	path = g_strconcat("/api/v1/ticket?limit=2&order=id&q=", q, NULL);
	paged = api(f, f->ada_secret, path, 200);
	g_assert_cmpint(json_object_get_int_member(json_node_get_object(paged), "count"), ==, 2);
	g_assert_cmpint(json_object_get_int_member(json_node_get_object(paged), "total"), ==, 5);

	/* Nothing kept is an empty list, not everything. */
	g_free(path);
	g_free(q);
	q = escaped("title == \"no such ticket\"");
	path = g_strconcat("/api/v1/ticket?q=", q, NULL);
	none = api(f, f->ada_secret, path, 200);
	g_assert_cmpint(json_object_get_int_member(json_node_get_object(none), "total"), ==, 0);

	/* A bad filter: 400, with the column. */
	g_free(path);
	g_free(q);
	q = escaped("status == \"todo\" and staus == 1");
	path = g_strconcat("/api/v1/ticket?q=", q, NULL);
	bad = api(f, f->ada_secret, path, 400);
	message = json_object_get_string_member(json_node_get_object(bad), "message");
	g_test_message("%s", message);
	g_assert_nonnull(strstr(message, "q, column 22: unknown field 'staus'"));

	/* The viewer: the filter narrows what they could list, never widens it. */
	viewer_all = api(f, f->vee_secret, "/api/v1/ticket", 200);
	g_free(path);
	g_free(q);
	q = escaped("status != \"nothing\"");
	path = g_strconcat("/api/v1/ticket?q=", q, NULL);
	viewer = api(f, f->vee_secret, path, 200);
	{
		g_autofree gchar *all = listed(viewer_all);
		g_autofree gchar *filtered = listed(viewer);

		g_assert_cmpstr(all, ==, filtered);
		g_assert_cmpstr(all, ==, "0000100");
	}
}

/* --- Editor help ------------------------------------------------------------------------ */

/* The labels of an assist answer's completions, comma-joined. */
static gchar *
labels(JsonNode *answer)
{
	JsonArray *completions;
	GString *out = g_string_new(NULL);
	guint i;

	completions = json_object_get_array_member(json_node_get_object(answer), "completions");

	for (i = 0; i < json_array_get_length(completions); i++)
		g_string_append_printf(out, "%s%s", (i > 0) ? "," : "",
			json_object_get_string_member(json_array_get_object_element(completions, i), "label"));

	return g_string_free(out, FALSE);
}

/* GET /lql/assist for @text with the cursor at UTF-16 unit @at. */
static JsonNode *
assist(
	Fixture		*f,
	const gchar	*text,
	gint		 at
){
	g_autofree gchar *q = escaped(text);
	g_autofree gchar *path = NULL;
	g_autofree gchar *body = NULL;
	guint status;

	path = (at < 0) ? g_strconcat("/lql/assist?type=ticket&q=", q, NULL)
	                : g_strdup_printf("/lql/assist?type=ticket&q=%s&at=%d&base=%%2Ftickets%%3Fq%%3D",
	                                  q, at);
	status = fixture_request(f, "GET", path, f->cookie, NULL, NULL, &body, NULL, NULL);
	g_assert_cmpuint(status, ==, 200);

	return venture_json_parse(body, NULL);
}

/*
 * Completion offers field names, and where a field is being compared, the
 * values it takes -- enumeration names, the repositories a reference
 * names -- each replacing what was typed of it; positions are UTF-16.
 */
static void
test_assist(
	Fixture		*f,
	gconstpointer	 data
){
	g_autoptr(JsonNode) fields = NULL;
	g_autoptr(JsonNode) values = NULL;
	g_autoptr(JsonNode) in_string = NULL;
	g_autoptr(JsonNode) bare = NULL;
	g_autoptr(JsonNode) in_list = NULL;
	g_autoptr(JsonNode) repos = NULL;
	g_autoptr(JsonNode) repo_names = NULL;
	g_autoptr(JsonNode) problem = NULL;
	g_autoptr(JsonNode) fine = NULL;
	g_autofree gchar *field_labels = NULL;
	g_autofree gchar *value_labels = NULL;
	g_autofree gchar *list_labels = NULL;
	JsonObject *first;
	JsonObject *wrong;

	(void)data;

	/* Field names. */
	fields = assist(f, "sta", 3);
	field_labels = labels(fields);
	g_test_message("sta: %s", field_labels);
	g_assert_true(g_str_has_prefix(field_labels, "status"));

	/* An enumeration's values, after the operator... */
	values = assist(f, "status == ", 10);
	value_labels = labels(values);
	g_test_message("status == : %s", value_labels);
	g_assert_true(g_str_has_prefix(value_labels, "triage,todo,in_progress,blocked,review,done,cancelled"));
	first = json_array_get_object_element(json_object_get_array_member(
		json_node_get_object(values), "completions"), 0);
	g_assert_cmpstr(json_object_get_string_member(first, "insert"), ==, "\"triage\"");
	g_assert_cmpstr(json_object_get_string_member(first, "kind"), ==, "value");
	g_assert_cmpint(json_object_get_int_member(first, "start"), ==, 10);
	g_assert_cmpint(json_object_get_int_member(first, "end"), ==, 10);

	/* ...inside a string, replacing all of it... */
	in_string = assist(f, "priority == \"hi\" and x", 15);
	first = json_array_get_object_element(json_object_get_array_member(
		json_node_get_object(in_string), "completions"), 0);
	g_assert_cmpstr(json_object_get_string_member(first, "label"), ==, "high");
	g_assert_cmpint(json_object_get_int_member(first, "start"), ==, 12);
	g_assert_cmpint(json_object_get_int_member(first, "end"), ==, 16);

	/* ...as a bare word, which becomes a string... */
	bare = assist(f, "issue_type == bu", 16);
	first = json_array_get_object_element(json_object_get_array_member(
		json_node_get_object(bare), "completions"), 0);
	g_assert_cmpstr(json_object_get_string_member(first, "insert"), ==, "\"bug\"");
	g_assert_cmpint(json_object_get_int_member(first, "start"), ==, 14);

	/* ...and in a list, after `not in`. */
	in_list = assist(f, "status not in [\"done\", ", 23);
	list_labels = labels(in_list);
	g_assert_true(g_str_has_prefix(list_labels, "triage,todo"));

	/* A repository: its id, named; or its name. */
	repos = assist(f, "repo_id == ", 11);
	first = json_array_get_object_element(json_object_get_array_member(
		json_node_get_object(repos), "completions"), 0);
	{
		g_autofree gchar *id = g_strdup_printf("%" G_GINT64_FORMAT, f->repo_id);

		g_assert_cmpstr(json_object_get_string_member(first, "insert"), ==, id);
		g_assert_cmpstr(json_object_get_string_member(first, "detail"), ==, "venture");
	}
	repo_names = assist(f, "repo_label == \"ven", 18);
	first = json_array_get_object_element(json_object_get_array_member(
		json_node_get_object(repo_names), "completions"), 0);
	g_assert_cmpstr(json_object_get_string_member(first, "insert"), ==, "\"venture\"");

	/* A problem, placed in UTF-16 units: "café" is four characters in
	 * five bytes, so the misspelt field starts at byte 21, unit 20. */
	problem = assist(f, "title == \"caf\xc3\xa9\" and staus == 1", -1);
	wrong = json_object_get_object_member(json_node_get_object(problem), "problem");
	g_assert_nonnull(wrong);
	g_assert_cmpint(json_object_get_int_member(wrong, "start"), ==, 20);
	g_assert_cmpint(json_object_get_int_member(wrong, "end"), ==, 25);
	g_assert_nonnull(strstr(json_object_get_string_member(json_node_get_object(problem), "status"),
	                        "lql-caret-line"));

	/* A fine filter: no problem, and the status line highlights it. */
	fine = assist(f, "status == \"todo\"", -1);
	g_assert_true(JSON_NODE_HOLDS_NULL(json_object_get_member(json_node_get_object(fine), "problem")));
	g_assert_nonnull(strstr(json_object_get_string_member(json_node_get_object(fine), "status"),
	                        "<span class=\"lql-string\">&quot;todo&quot;</span>"));

	/* Without a session: no answer. */
	g_assert_cmpuint(fixture_request(f, "GET", "/lql/assist?type=ticket&q=x", NULL, NULL, NULL,
	                                 NULL, NULL, NULL), !=, 200);
}

/*
 * The highlighted text escapes what it holds and marks a problem's range;
 * the problem is placed in the bar's text, not in the query it becomes.
 */
static void
test_highlight_and_check(
	Fixture		*f,
	gconstpointer	 data
){
	g_autoptr(LqlEnvironment) environment = environment_for_ada(f);
	g_autoptr(VentureLqlProblem) problem = NULL;
	g_autoptr(VentureLqlProblem) end = NULL;
	g_autoptr(GArray) ids = NULL;
	g_autofree gchar *html = NULL;
	g_autofree gchar *marked = NULL;
	g_autofree gchar *at_end = NULL;

	(void)data;

	g_assert_null(venture_lql_filter_check(environment, "ticket", NULL));
	g_assert_null(venture_lql_filter_check(environment, "ticket", "  \t"));
	g_assert_null(venture_lql_filter_check(environment, "ticket", "filter_out status == \"done\""));

	html = venture_lql_filter_highlight(environment, "ticket", "title == \"<b>\"", NULL);
	g_assert_cmpstr(html, ==, "<span class=\"lql-field\">title</span> "
	                          "<span class=\"lql-operator\">==</span> "
	                          "<span class=\"lql-string\">&quot;&lt;b&gt;&quot;</span>");

	problem = venture_lql_filter_check(environment, "ticket", "status == 1 and priorty == 2");
	g_assert_nonnull(problem);
	g_assert_true(problem->positioned);
	g_assert_cmpuint(problem->offset, ==, 16);
	g_assert_cmpuint(problem->length, ==, 7);
	g_assert_cmpstr(problem->name, ==, "priorty");
	g_assert_cmpstr(problem->suggestions[0], ==, "priority");
	marked = venture_lql_filter_highlight(environment, "ticket", "status == 1 and priorty == 2", problem);
	g_assert_nonnull(strstr(marked, "<span class=\"lql-keyword\">and</span> "
	                                "<mark class=\"lql-mark\"><span class=\"lql-field\">priorty</span></mark>"));

	/* Unfinished: the problem is the end of the text, marked as a place. */
	end = venture_lql_filter_check(environment, "ticket", "status ==");
	g_assert_nonnull(end);
	g_assert_cmpuint(end->offset, ==, 9);
	at_end = venture_lql_filter_highlight(environment, "ticket", "status ==", end);
	g_assert_true(g_str_has_suffix(at_end, "<mark class=\"lql-mark lql-mark-empty\"></mark>"));

	/* The ids, as the caller reads them; none is an empty array. */
	ids = venture_lql_filter_find_ids(environment, "ticket", "status == \"todo\"", FALSE, NULL, NULL);
	g_assert_nonnull(ids);
	g_assert_cmpuint(ids->len, ==, 1);
	g_array_unref(g_steal_pointer(&ids));
	ids = venture_lql_filter_find_ids(environment, "ticket", "id < 0", FALSE, NULL, NULL);
	g_assert_nonnull(ids);
	g_assert_cmpuint(ids->len, ==, 0);

	g_assert_cmpstr(venture_lql_filter_and(NULL, " "), ==, "");
	{
		g_autofree gchar *both = venture_lql_filter_and("a == 1 or b == 2", "c == 3");
		g_autofree gchar *one = venture_lql_filter_and(NULL, "c == 3 or d == 4");

		g_assert_cmpstr(both, ==, "(a == 1 or b == 2) and c == 3");
		g_assert_cmpstr(one, ==, "c == 3 or d == 4");
	}
}

/* --- main ----------------------------------------------------------------------------- */

/* --- Bounds and where a filter runs ------------------------------------------- */

/*
 * A filter that allocates per row: every ticket's title with 600 KB of
 * padding, so a 1 MiB budget is gone by the second ticket. Not a constant,
 * so nothing can fold it away or push it to the database.
 */
#define FILTER_OVER_BUDGET "length(concat(title, repeat(\"x\", 600000))) > 0"

/*
 * query.max_bytes (VENTURE #44) bounds a bar's filter as it bounds
 * POST /api/v1/query: the page draws the problem in the bar and lists
 * nothing (400), the list API refuses it as a bound (422 validation,
 * naming the setting), and the next request is answered as usual.
 */
static void
test_max_bytes(
	Fixture		*f,
	gconstpointer	 data
){
	g_autofree gchar *q = escaped(FILTER_OVER_BUDGET);
	g_autofree gchar *path = NULL;
	g_autofree gchar *html = NULL;
	g_autofree gchar *after = NULL;
	g_autoptr(JsonNode) refused = NULL;
	g_autoptr(JsonNode) small = NULL;
	const gchar *message;
	const gchar *code;

	(void)data;

	g_object_set(f->config, "query-max-bytes", (gint64)1048576, NULL);

	/* The page: the problem in the bar, no table. */
	path = g_strconcat("/e/ticket?q=", q, NULL);
	html = page(f, path, 400);
	g_assert_nonnull(strstr(html, "lql-problem"));
	g_assert_nonnull(strstr(html, "(max_bytes)"));
	g_assert_nonnull(strstr(html, "query.max_bytes"));
	{
		g_autofree gchar *on = shown(html);

		g_assert_cmpstr(on, ==, "0000000");
	}

	/* The list API: a bound, as POST /api/v1/query calls it. */
	g_free(path);
	path = g_strconcat("/api/v1/ticket?q=", q, NULL);
	refused = api(f, f->ada_secret, path, 422);
	message = json_object_get_string_member(json_node_get_object(refused), "message");
	code = json_object_get_string_member(json_node_get_object(refused), "error");
	g_test_message("over budget -> %s: %s", code, message);
	g_assert_cmpstr(code, ==, "validation");
	g_assert_nonnull(strstr(message, "(max_bytes)"));
	g_assert_nonnull(strstr(message, "query.max_bytes (1048576)"));

	/* The server is still there, and a small filter runs under the
	 * same budget. */
	after = page(f, "/e/ticket", 200);
	small = api(f, f->ada_secret, "/api/v1/ticket?q=status%20%3D%3D%20%22todo%22", 200);
	{
		g_autofree gchar *on = listed(small);

		g_assert_cmpstr(on, ==, "1000000");
	}
}

/*
 * The message of the problem @html draws in the bar, as plain text still
 * escaped, in a new string; NULL when the page draws none.
 */
static gchar *
problem_message(const gchar *html)
{
	static const gchar open[] = "<p class=\"lql-problem-message\"><strong>Cannot filter:</strong> ";
	const gchar *start;
	const gchar *end;

	start = strstr(html, open);

	if (NULL == start)
		return NULL;

	start += strlen(open);
	end = strstr(start, "</p>");
	g_assert_nonnull(end);

	return g_strndup(start, (gsize)(end - start));
}

/*
 * A bar's text with something wrong only at run time is placed in what
 * was typed or nowhere (VENTURE #52): never at the line and column of
 * the query VENTURE builds around it, which the person did not write.
 */
static void
test_run_error_placement(
	Fixture		*f,
	gconstpointer	 data
){
	g_autofree gchar *q = escaped(FILTER_OVER_BUDGET);
	g_autofree gchar *path = NULL;
	g_autofree gchar *html = NULL;
	g_autofree gchar *message = NULL;
	g_autoptr(JsonNode) refused = NULL;
	const gchar *api_message;

	(void)data;

	/*
	 * Over budget while the filter is evaluated: LQL says "2:17:", a
	 * place in `fetch ticket | filter ...`. The bar marks concat(), where
	 * it is in the text, and shows the message without that prefix.
	 */
	g_object_set(f->config, "query-max-bytes", (gint64)1048576, NULL);

	path = g_strconcat("/e/ticket?q=", q, NULL);
	html = page(f, path, 400);
	message = problem_message(html);
	g_test_message("over budget -> %s", message);
	g_assert_nonnull(message);
	g_assert_nonnull(strstr(html,
		"<mark class=\"lql-mark\"><span class=\"lql-function\">concat</span></mark>"));
	g_assert_nonnull(strstr(html, "<span class=\"lql-caret-line\">       ^^^^^^</span>"));
	g_assert_nonnull(strstr(html, "At column 8."));
	g_assert_true(g_str_has_prefix(message, "concat(): "));
	g_assert_nonnull(strstr(message, "query.max_bytes (1048576)"));
	g_assert_null(strstr(html, "2:17"));
	g_assert_false(g_ascii_isdigit(message[0]));

	/* The list API: the column in what was typed, the same message. */
	g_free(path);
	path = g_strconcat("/api/v1/ticket?q=", q, NULL);
	refused = api(f, f->ada_secret, path, 422);
	api_message = json_object_get_string_member(json_node_get_object(refused), "message");
	g_test_message("over budget (api) -> %s", api_message);
	g_assert_true(g_str_has_prefix(api_message, "q, column 8: concat(): "));
	g_assert_null(strstr(api_message, "2:17"));

	/*
	 * On the board the bar also holds the URL's filters ahead of q, so
	 * the place is found again in that longer text.
	 */
	g_free(path);
	g_free(html);
	g_free(message);
	path = g_strconcat("/tickets?view=board&status=todo&q=", q, NULL);
	html = page(f, path, 400);
	message = problem_message(html);
	g_test_message("over budget (board) -> %s", message);
	g_assert_nonnull(message);
	g_assert_nonnull(strstr(html,
		"<mark class=\"lql-mark\"><span class=\"lql-function\">concat</span></mark>"));
	g_assert_true(g_str_has_prefix(message, "concat(): "));
	g_assert_null(strstr(html, "2:17"));

	/*
	 * Over the row bound while it is read: the fetch is VENTURE's, not
	 * the text's. No place in the text: the message alone, nothing marked,
	 * and no line and column.
	 */
	g_object_set(f->config, "query-max-rows", (gint64)3, NULL);
	g_free(message);
	g_free(html);
	g_free(path);
	g_clear_pointer(&refused, json_node_unref);
	g_free(q);
	q = escaped("length(title) > 0");
	path = g_strconcat("/e/ticket?q=", q, NULL);
	html = page(f, path, 400);
	message = problem_message(html);
	g_test_message("over rows -> %s", message);
	g_assert_nonnull(message);
	g_assert_nonnull(strstr(message, "query.max_rows"));
	g_assert_false(g_ascii_isdigit(message[0]));
	g_assert_null(strstr(html, "<mark class=\"lql-mark\">"));
	g_assert_null(strstr(html, "lql-caret-line"));

	g_free(path);
	path = g_strconcat("/api/v1/ticket?q=", q, NULL);
	refused = api(f, f->ada_secret, path, 422);
	api_message = json_object_get_string_member(json_node_get_object(refused), "message");
	g_test_message("over rows (api) -> %s", api_message);
	g_assert_true(g_str_has_prefix(api_message, "q: "));
	g_assert_nonnull(strstr(api_message, "query.max_rows"));
}

/* How long nap() sleeps, on whichever thread evaluates the filter. */
#define NAP_MICROSECONDS (100000)

/* nap() -> Int: sleeps NAP_MICROSECONDS, then answers 1. */
static LqlValue *
nap(
	LqlFunctionCall	 *call,
	gpointer	  user_data,
	GError		**error
){
	(void)call;
	(void)user_data;
	(void)error;

	g_usleep(NAP_MICROSECONDS);

	return lql_value_new_int(1);
}

/* A #VentureLqlEnvironmentHook: nap() in every environment the server
 * builds. Volatile, so it is evaluated once per row and never folded. */
static gboolean
add_nap(
	LqlEnvironment	 *environment,
	gpointer	  user_data,
	GError		**error
){
	g_autoptr(LqlFunction) slow = NULL;

	(void)user_data;

	slow = lql_function_new_scalar("nap", LQL_VALUE_TYPE_INT, nap, NULL, NULL);
	lql_function_set_flags(slow, LQL_FUNCTION_FLAG_VOLATILE);

	return lql_environment_register_function(environment, slow, error);
}

/*
 * While one list's filter is evaluated -- seven tickets, a tenth of a
 * second of nap() each -- the server goes on answering: the list API and
 * a second filtered page are both answered before the first page is.
 * Registered both ways; without read workers (an in-memory database, as a
 * hosted install has none) is the case that used to run the filter in
 * the fiber and block the main loop for all of it.
 */
static void
test_main_loop_responsive(
	Fixture		*f,
	gconstpointer	 data
){
	g_autoptr(SoupMessage) message = NULL;
	g_autofree gchar *url = NULL;
	g_autofree gchar *q = escaped("nap() > 0");
	g_autofree gchar *quick = NULL;
	g_autofree gchar *slow_page = NULL;
	g_autoptr(JsonNode) listed_quick = NULL;
	Outcome slow = { FALSE, NULL, NULL };
	gint64 until;

	(void)data;

	venture_lql_set_environment_hook(add_nap, NULL);
	g_object_set(f->config, "query-timeout-ms", (gint64)60000, NULL);

	/* The slow page, not waited for. */
	url = g_strconcat(venture_web_server_get_base_url(f->server), "/e/ticket?q=", q, NULL);
	message = soup_message_new("GET", url);
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	soup_message_headers_append(soup_message_get_request_headers(message), "Cookie", f->cookie);
	soup_session_send_and_read_async(f->session, message, G_PRIORITY_DEFAULT, NULL,
	                                 request_done, &slow);

	/* Long enough for its filter to be on a worker. */
	until = g_get_monotonic_time() + 100 * G_TIME_SPAN_MILLISECOND;

	while (g_get_monotonic_time() < until)
		g_main_context_iteration(NULL, FALSE);

	g_assert_false(slow.done);

	/* Answered meanwhile: the list API, and another filtered page. */
	listed_quick = api(f, f->ada_secret, "/api/v1/ticket?limit=1", 200);
	quick = page(f, "/e/ticket?q=status%20%3D%3D%20%22todo%22", 200);
	g_assert_nonnull(strstr(quick, "Crash on save"));

	/* The slow one is still being evaluated. */
	g_assert_false(slow.done);

	while (!slow.done)
		g_main_context_iteration(NULL, TRUE);

	g_assert_no_error(slow.error);
	g_assert_cmpuint(soup_message_get_status(message), ==, 200);
	slow_page = g_strndup(g_bytes_get_data(slow.body, NULL), g_bytes_get_size(slow.body));
	g_bytes_unref(slow.body);
	{
		g_autofree gchar *on = shown(slow_page);

		g_assert_cmpstr(on, ==, "1111111");
	}

	venture_lql_set_environment_hook(NULL, NULL);
}

int
main(
	int	  argc,
	char	**argv
){
	g_test_init(&argc, &argv, NULL);
	g_setenv("VENTURE_TEST_SESSION_SECRET", "lql-bar-test-secret", TRUE);

#define ADD(path, func) \
	g_test_add(path, Fixture, NULL, fixture_setup, func, fixture_teardown); \
	g_test_add(path "/read-workers", Fixture, with_workers, fixture_setup, func, fixture_teardown)

	ADD("/lql-bar/board/example-queries", test_example_queries);
	ADD("/lql-bar/board/chips", test_chips);
	ADD("/lql-bar/board/parse-errors-rendered", test_parse_errors);
	ADD("/lql-bar/board/url-filters-as-lql", test_url_filters);
	ADD("/lql-bar/saved-views/round-trip", test_saved_views);
	ADD("/lql-bar/api/q", test_api_q);
	ADD("/lql-bar/assist/complete-and-place", test_assist);
	ADD("/lql-bar/filter/highlight-and-check", test_highlight_and_check);
	ADD("/lql-bar/bounds/max-bytes", test_max_bytes);
	ADD("/lql-bar/bounds/run-error-placement", test_run_error_placement);
	ADD("/lql-bar/runs/main-loop-stays-responsive", test_main_loop_responsive);

#undef ADD

	return g_test_run();
}
