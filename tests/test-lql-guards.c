/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * test-lql-guards.c - What stands between a caller and LQL: UTF-8, and
 * who may have a query worker (docs/lql.org, "Problems" and "Who may
 * have a worker")
 *
 * Copyright (C) 2026 Zach Podbielniak
 *
 * mongoose's review of the LQL query path (epic #5) found two ways in:
 *
 *   - Text that is not UTF-8 reached LQL's tolerant lexer, which steps
 *     over a multi-byte sequence by its lead byte and so walks past the
 *     end of a truncated one (q=%FC). Every surface where a caller's text
 *     enters LQL now refuses it first, 400 "The filter is not UTF-8", and
 *     nothing is evaluated: the bar and ?q= (a list page, the list API,
 *     the board, the export), a chart (its q, its picks, the URL filters
 *     it turns into LQL), a saved view's filter, POST /api/v1/query, and
 *     a query widget's text when it is saved and when it is drawn.
 *   - The query workers are shared by the whole process, so one hosted
 *     workspace's slow queries could hold every worker for every tenant,
 *     and the queue had no end. A job is now admitted against caps -- per
 *     workspace, per principal, in all -- and refused at once over one
 *     (503, Retry-After); a job that waits past its deadline, or is
 *     cancelled before a worker reaches it, is answered and never run.
 *
 * The workers are held on cue by hold(), an LQL function that waits for
 * the test to open it, so none of this depends on timing. Every test
 * ends with the admission counts back at zero.
 */

#include <venture.h>
#include <libsoup/soup.h>
#include <string.h>
#include "lql/venture-lql.h"
#include "web/venture-web-query-private.h"
#include "venture-test-util.h"

#define TEST_PASSWORD "guards-long-password"

/* The words every refusal of text that is not UTF-8 says. */
#define NOT_UTF8 "The filter is not UTF-8"

/* A query that holds its worker until the test opens hold(). */
#define QUERY_HOLD "data {} | fields z = hold()"

/* A query that counts, in marks, every time it is evaluated. */
#define QUERY_MARK "data {} | fields z = mark()"

/* How long any wait in here may take before the test fails loudly. */
#define WAIT_LIMIT_S (20)

/* --- hold() and mark(): the workers on cue -------------------------------- */

static GMutex hold_lock;
static GCond hold_cond;
static gint holding;		/* hold() calls waiting now */
static gboolean hold_open;
static gint marks;		/* atomic: mark() calls */

static LqlValue *
hold(
	LqlFunctionCall	 *call,
	gpointer	  user_data,
	GError		**error
){
	(void)call;
	(void)user_data;
	(void)error;

	g_mutex_lock(&hold_lock);
	holding++;
	g_cond_broadcast(&hold_cond);

	while (!hold_open)
		g_cond_wait(&hold_cond, &hold_lock);

	holding--;
	g_mutex_unlock(&hold_lock);
	g_main_context_wakeup(NULL);

	return lql_value_new_int(1);
}

static LqlValue *
mark(
	LqlFunctionCall	 *call,
	gpointer	  user_data,
	GError		**error
){
	(void)call;
	(void)user_data;
	(void)error;

	g_atomic_int_inc(&marks);

	return lql_value_new_int(1);
}

/* A #VentureLqlEnvironmentHook: hold() and mark() everywhere. */
static gboolean
add_functions(
	LqlEnvironment	 *environment,
	gpointer	  user_data,
	GError		**error
){
	g_autoptr(LqlFunction) held = NULL;
	g_autoptr(LqlFunction) marked = NULL;

	(void)user_data;

	held = lql_function_new_scalar("hold", LQL_VALUE_TYPE_INT, hold, NULL, NULL);
	lql_function_set_flags(held, LQL_FUNCTION_FLAG_VOLATILE);
	marked = lql_function_new_scalar("mark", LQL_VALUE_TYPE_INT, mark, NULL, NULL);
	lql_function_set_flags(marked, LQL_FUNCTION_FLAG_VOLATILE);

	return lql_environment_register_function(environment, held, error) &&
	       lql_environment_register_function(environment, marked, error);
}

/* Closes hold(): the next ones wait. */
static void
hold_close(void)
{
	g_mutex_lock(&hold_lock);
	hold_open = FALSE;
	g_mutex_unlock(&hold_lock);
}

/* Opens hold(): every one waiting returns. */
static void
hold_release(void)
{
	g_mutex_lock(&hold_lock);
	hold_open = TRUE;
	g_cond_broadcast(&hold_cond);
	g_mutex_unlock(&hold_lock);
}

/* How many hold() calls wait now. */
static gint
holding_now(void)
{
	gint n;

	g_mutex_lock(&hold_lock);
	n = holding;
	g_mutex_unlock(&hold_lock);

	return n;
}

/* The admission counts. */
typedef struct
{
	guint	admitted;
	guint	running;
	guint	holders;
	guint64	ran;
	guint64	skipped;
} Counts;

static Counts
counts(void)
{
	Counts c;

	venture_web_query_get_counts(&c.admitted, &c.running, &c.holders, &c.ran, &c.skipped);

	return c;
}

/* Drives the main loop until @n hold() calls wait on workers. */
static void
wait_holding(gint n)
{
	gint64 limit = g_get_monotonic_time() + WAIT_LIMIT_S * G_TIME_SPAN_SECOND;

	while (holding_now() < n)
	{
		if (g_get_monotonic_time() > limit)
			g_error("only %d of %d hold() calls are waiting", holding_now(), n);

		g_main_context_iteration(NULL, FALSE);
		g_usleep(1000);
	}
}

/* Drives the main loop until @admitted jobs are admitted. */
static void
wait_admitted(guint admitted)
{
	gint64 limit = g_get_monotonic_time() + WAIT_LIMIT_S * G_TIME_SPAN_SECOND;

	while (counts().admitted != admitted)
	{
		if (g_get_monotonic_time() > limit)
			g_error("%u jobs admitted, wanted %u", counts().admitted, admitted);

		g_main_context_iteration(NULL, FALSE);
		g_usleep(1000);
	}
}

/* Every count back at zero: nothing held, nothing queued, nobody. */
static void
assert_idle(void)
{
	Counts c;

	wait_admitted(0);
	c = counts();
	g_assert_cmpuint(c.admitted, ==, 0);
	g_assert_cmpuint(c.running, ==, 0);
	g_assert_cmpuint(c.holders, ==, 0);
}

/* --- HTTP ------------------------------------------------------------------- */

/* One request in flight. */
typedef struct
{
	SoupMessage	*message;
	GBytes		*body;
	GError		*error;
	gboolean	 done;
	guint		 status;
	gchar		*text;
	gchar		*retry_after;
} Exchange;

static void
exchange_done(
	GObject		*source,
	GAsyncResult	*result,
	gpointer	 data
){
	Exchange *x = data;

	x->body = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &x->error);
	x->done = TRUE;
}

/*
 * Starts @method @base@path without waiting: @cookie or @bearer the
 * credential, @host the Host header (hosted), @content_type and @body a
 * request body.
 */
static void
exchange_start(
	SoupSession	*session,
	Exchange	*x,
	const gchar	*base,
	const gchar	*host,
	const gchar	*method,
	const gchar	*path,
	const gchar	*cookie,
	const gchar	*bearer,
	const gchar	*content_type,
	const gchar	*body
){
	g_autofree gchar *root = g_strdup(base);
	g_autofree gchar *url = NULL;
	SoupMessageHeaders *headers;

	if (g_str_has_suffix(root, "/"))
		root[strlen(root) - 1] = '\0';

	memset(x, 0, sizeof *x);
	url = g_strconcat(root, path, NULL);
	x->message = soup_message_new(method, url);
	g_assert_nonnull(x->message);
	soup_message_set_flags(x->message, SOUP_MESSAGE_NO_REDIRECT);
	headers = soup_message_get_request_headers(x->message);

	if (NULL != host)
		soup_message_headers_replace(headers, "Host", host);

	if (NULL != cookie)
		soup_message_headers_append(headers, "Cookie", cookie);

	if (NULL != bearer)
	{
		g_autofree gchar *value = g_strconcat("Bearer ", bearer, NULL);

		soup_message_headers_replace(headers, "Authorization", value);
	}

	if (NULL != body)
	{
		g_autoptr(GBytes) bytes = g_bytes_new(body, strlen(body));

		soup_message_set_request_body_from_bytes(x->message, content_type, bytes);
	}

	soup_session_send_and_read_async(session, x->message, G_PRIORITY_DEFAULT, NULL,
	                                 exchange_done, x);
}

/* Drives the loop until @x is answered; fills its status and text. */
static void
exchange_finish(Exchange *x)
{
	gint64 limit = g_get_monotonic_time() + WAIT_LIMIT_S * G_TIME_SPAN_SECOND;

	while (!x->done)
	{
		if (g_get_monotonic_time() > limit)
			g_error("%s %s was never answered", soup_message_get_method(x->message),
			        g_uri_get_path(soup_message_get_uri(x->message)));

		g_main_context_iteration(NULL, TRUE);
	}

	g_assert_no_error(x->error);
	x->status = soup_message_get_status(x->message);
	x->text = g_strndup(g_bytes_get_data(x->body, NULL), g_bytes_get_size(x->body));
	x->retry_after = g_strdup(soup_message_headers_get_one(
		soup_message_get_response_headers(x->message), "Retry-After"));
	g_test_message("%s %s -> %u", soup_message_get_method(x->message),
	               g_uri_get_path(soup_message_get_uri(x->message)), x->status);
}

static void
exchange_clear(Exchange *x)
{
	g_clear_object(&x->message);
	g_clear_pointer(&x->body, g_bytes_unref);
	g_clear_error(&x->error);
	g_clear_pointer(&x->text, g_free);
	g_clear_pointer(&x->retry_after, g_free);
}

/* {"query": @text} as JSON. */
static gchar *
query_body(const gchar *text)
{
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(JsonNode) request = NULL;

	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "query");
	json_builder_add_string_value(builder, text);
	json_builder_end_object(builder);
	request = json_builder_get_root(builder);

	return venture_json_to_string(request, FALSE);
}

/* The "message" of a JSON error body. */
static const gchar *
message_of(
	JsonNode	*node,
	const gchar	*member
){
	g_assert_nonnull(node);
	g_assert_true(JSON_NODE_HOLDS_OBJECT(node));

	return json_object_get_string_member(json_node_get_object(node), member);
}

/* --- One install, not hosted ----------------------------------------------- */

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
	gchar			*vee_secret;
	gchar			*ana_secret;
} Fixture;

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
	g_assert_true(venture_user_set_password(user, TEST_PASSWORD, 2000, NULL));
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
	VentureDatabase	*db,
	const gchar	*name,
	gint64		 user_id,
	VentureUserRole	 role
){
	g_autoptr(VentureApiToken) token = venture_api_token_new();
	gchar *secret;

	g_object_set(token, "name", name, "user-id", user_id, "role", role, NULL);
	secret = venture_api_token_generate(token);
	g_assert_true(venture_database_save(db, VENTURE_ENTITY(token), NULL, NULL));

	return secret;
}

/* One request on the fixture's server, answered; the caller clears @x. */
static void
fixture_send(
	Fixture		*f,
	Exchange	*x,
	const gchar	*method,
	const gchar	*path,
	const gchar	*cookie,
	const gchar	*bearer,
	const gchar	*content_type,
	const gchar	*body
){
	exchange_start(f->session, x, venture_web_server_get_base_url(f->server), NULL, method,
	               path, cookie, bearer, content_type, body);
	exchange_finish(x);
}

/* POST /api/v1/query {"query": @text} as @secret, started. */
static void
fixture_query_start(
	Fixture		*f,
	Exchange	*x,
	const gchar	*secret,
	const gchar	*text
){
	g_autofree gchar *body = query_body(text);

	exchange_start(f->session, x, venture_web_server_get_base_url(f->server), NULL, "POST",
	               "/api/v1/query", NULL, secret, "application/json", body);
}

static void
fixture_setup(
	Fixture		*f,
	gconstpointer	 data
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *uri = NULL;
	g_autofree gchar *login = NULL;
	Exchange x;
	const gchar *set_cookie;
	gchar *semicolon;
	guint i;

	(void)data;

	hold_close();
	g_atomic_int_set(&marks, 0);
	venture_lql_set_environment_hook(add_functions, NULL);

	f->directory = g_dir_make_tmp("venture-lql-guards-XXXXXX", NULL);
	f->config = venture_config_new();
	g_object_set(f->config, "state-dir", f->directory, "server-bind-address", "127.0.0.1",
	             "server-port", (gint64)0, "security-require-auth", TRUE,
	             "security-password-iterations", (gint64)2000,
	             "security-session-secret-env", "VENTURE_TEST_SESSION_SECRET",
	             "query-timeout-ms", (gint64)60000, NULL);

	uri = g_strconcat("sqlite://", f->directory, "/venture.db", NULL);
	f->db = venture_database_new(uri, &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->db);
	f->org = venture_context_get_default_organization_id(f->context);

	/* Three people: the caps are per person, so filling all four workers
	 * takes two of them. */
	f->ada_id = fixture_user(f, "ada", VENTURE_USER_ROLE_ADMIN, VENTURE_ORGANIZATION_ROLE_OWNER);
	f->ada_secret = fixture_token(f->db, "ada-token", f->ada_id, VENTURE_USER_ROLE_ADMIN);
	f->vee_secret = fixture_token(f->db, "vee-token",
		fixture_user(f, "vee", VENTURE_USER_ROLE_ADMIN, VENTURE_ORGANIZATION_ROLE_OWNER),
		VENTURE_USER_ROLE_ADMIN);
	f->ana_secret = fixture_token(f->db, "ana-token",
		fixture_user(f, "ana", VENTURE_USER_ROLE_ADMIN, VENTURE_ORGANIZATION_ROLE_OWNER),
		VENTURE_USER_ROLE_ADMIN);

	for (i = 0; i < 3; i++)
	{
		g_autoptr(VentureEntity) ticket = NULL;
		g_autofree gchar *title = g_strdup_printf("Ticket %u", i + 1);

		ticket = g_object_new(VENTURE_TYPE_TICKET, "title", title, "organization-id", f->org,
		                      "status", VENTURE_TICKET_STATUS_TODO,
		                      "kind", VENTURE_TICKET_KIND_INTERNAL, NULL);
		g_assert_true(venture_database_save(f->db, ticket, NULL, &error));
		g_assert_no_error(error);
	}

	f->server = venture_web_server_new(f->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(f->server, &error));
	g_assert_no_error(error);

	/* Enough connections for every worker, the queue and one more. */
	f->session = soup_session_new_with_options("timeout", 60, "max-conns-per-host", 32,
	                                           "max-conns", 64, NULL);

	login = g_strdup_printf("username=ada&password=%s", TEST_PASSWORD);
	fixture_send(f, &x, "POST", "/login", NULL, NULL, "application/x-www-form-urlencoded", login);
	g_assert_cmpuint(x.status, ==, SOUP_STATUS_FOUND);
	set_cookie = soup_message_headers_get_one(soup_message_get_response_headers(x.message),
	                                          "Set-Cookie");
	g_assert_nonnull(set_cookie);
	f->cookie = g_strdup(set_cookie);
	semicolon = strchr(f->cookie, ';');

	if (NULL != semicolon)
		*semicolon = '\0';

	exchange_clear(&x);
}

static void
fixture_teardown(
	Fixture		*f,
	gconstpointer	 data
){
	(void)data;

	/* Nothing left holding a worker for the next test. */
	hold_release();
	assert_idle();

	venture_web_server_stop(f->server);
	g_clear_object(&f->session);
	g_clear_object(&f->server);
	g_clear_object(&f->context);
	g_clear_object(&f->db);
	g_clear_object(&f->config);
	g_clear_pointer(&f->cookie, g_free);
	g_clear_pointer(&f->ada_secret, g_free);
	g_clear_pointer(&f->vee_secret, g_free);
	g_clear_pointer(&f->ana_secret, g_free);
	venture_test_remove_tree(f->directory);
	g_clear_pointer(&f->directory, g_free);
	venture_lql_set_environment_hook(NULL, NULL);
}

/* --- 1. UTF-8 ----------------------------------------------------------------- */

/* A page @path in ada's session answers 400 with the refusal in its bar. */
static void
assert_page_refuses(
	Fixture		*f,
	const gchar	*path
){
	Exchange x;

	fixture_send(f, &x, "GET", path, f->cookie, NULL, NULL, NULL);

	if ((400 != x.status) || (NULL == strstr(x.text, NOT_UTF8)))
		g_error("GET %s: %u, wanted 400 with \"%s\"\n%.2000s", path, x.status, NOT_UTF8, x.text);

	exchange_clear(&x);
}

/* An API @method @path (with @body) as ada answers 400 "not UTF-8". */
static void
assert_api_refuses(
	Fixture		*f,
	const gchar	*method,
	const gchar	*path,
	const gchar	*content_type,
	const gchar	*body
){
	g_autoptr(JsonNode) answer = NULL;
	Exchange x;

	fixture_send(f, &x, method, path, NULL, f->ada_secret, content_type, body);
	answer = venture_json_parse(x.text, NULL);

	if ((400 != x.status) || (NULL == answer) || (0 != g_strcmp0(message_of(answer, "message"), NOT_UTF8)))
		g_error("%s %s: %u, wanted 400 \"%s\"\n%.2000s", method, path, x.status, NOT_UTF8, x.text);

	exchange_clear(&x);
}

/*
 * Text that is not UTF-8 -- a lone lead byte (q=%FC), and the same at the
 * end of a string (q=title == "%FC), which the lexer reads as an open
 * string running to the end of the buffer -- is refused on every surface
 * that hands caller text to LQL, 400 "The filter is not UTF-8", and no
 * LQL work reaches a worker for any of it.
 */
static void
test_utf8_refused_everywhere(
	Fixture		*f,
	gconstpointer	 data
){
	static const gchar *const texts[] = { "%FC", "title%20%3D%3D%20%22%FC" };
	Counts before;
	Counts after;
	guint i;

	(void)data;

	before = counts();

	for (i = 0; i < G_N_ELEMENTS(texts); i++)
	{
		g_autofree gchar *list = g_strconcat("/e/ticket?q=", texts[i], NULL);
		g_autofree gchar *board = g_strconcat("/tickets?q=", texts[i], NULL);
		g_autofree gchar *chart = g_strconcat("/e/ticket?view=chart&q=", texts[i], NULL);
		g_autofree gchar *board_chart = g_strconcat("/tickets?view=chart&q=", texts[i], NULL);
		g_autofree gchar *api = g_strconcat("/api/v1/ticket?q=", texts[i], NULL);
		g_autofree gchar *export = g_strconcat("/e/ticket/export?q=", texts[i], NULL);
		g_autofree gchar *assist = g_strconcat("/lql/assist?type=ticket&q=", texts[i], NULL);

		/* The bar: a list page, the board, and both as charts. */
		assert_page_refuses(f, list);
		assert_page_refuses(f, board);
		assert_page_refuses(f, chart);
		assert_page_refuses(f, board_chart);

		/* ?q= on the list API, the export, and the bar's assist. */
		assert_api_refuses(f, "GET", api, NULL, NULL);
		assert_api_refuses(f, "GET", export, NULL, NULL);
		assert_api_refuses(f, "GET", assist, NULL, NULL);
	}

	/* A chart's picks and the URL filter values it quotes into LQL. */
	assert_page_refuses(f, "/e/ticket?view=chart&chart_by=%FC");
	assert_page_refuses(f, "/e/ticket?view=chart&chart_value=sum%3A%FC");
	assert_page_refuses(f, "/e/ticket?view=chart&chart_kind=%FC");
	assert_page_refuses(f, "/e/ticket?view=chart&title=%FC");

	/*
	 * POST /api/v1/query. A JSON body cannot carry text that is not
	 * UTF-8: json-glib refuses raw bytes and a lone surrogate escape
	 * alike, before the route reads "query" (400, nothing run). The
	 * route checks the text all the same.
	 */
	{
		static const gchar *const bodies[] = {
			"{\"query\": \"fetch ticket | filter title == \\\"\xfc\"}",
			"{\"query\": \"fetch ticket | filter title == \\\"\\udcfc\"}"
		};
		guint b;

		for (b = 0; b < G_N_ELEMENTS(bodies); b++)
		{
			Exchange x;

			fixture_send(f, &x, "POST", "/api/v1/query", NULL, f->ada_secret,
			             "application/json", bodies[b]);
			g_test_message("query body %u: %s", b, x.text);
			g_assert_cmpuint(x.status, ==, 400);
			exchange_clear(&x);
		}
	}

	/* A saved view's filter: refused, and nothing saved. */
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_SAVED_VIEW);
		g_autoptr(GPtrArray) found = NULL;
		Exchange x;

		fixture_send(f, &x, "POST", "/views", f->cookie, NULL,
		             "application/x-www-form-urlencoded",
		             "name=Broken&entity_type=ticket&query=&lql=title%20%3D%3D%20%22%FC");
		g_test_message("POST /views: %u %.300s", x.status, x.text);
		g_assert_cmpuint(x.status, ==, 400);
		g_assert_nonnull(strstr(x.text, NOT_UTF8));
		exchange_clear(&x);

		found = venture_database_find(f->db, query, NULL);
		g_assert_nonnull(found);
		g_assert_cmpuint(found->len, ==, 0);
	}

	after = counts();
	g_assert_cmpuint(after.ran, ==, before.ran);
	g_assert_cmpuint(after.skipped, ==, before.skipped);
	g_assert_cmpint(g_atomic_int_get(&marks), ==, 0);
	assert_idle();
}

/*
 * A query widget's text: refused when saved, and -- written past the
 * save, straight into the table -- refused again when the page draws
 * it, never handed to LQL.
 */
static void
test_utf8_widget(
	Fixture		*f,
	gconstpointer	 data
){
	g_autoptr(VentureEntity) dashboard = NULL;
	g_autoptr(VentureDashboardWidget) broken = NULL;
	g_autoptr(VentureDashboardWidget) widget = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *sql = NULL;
	Counts before;
	Exchange x;

	(void)data;

	dashboard = VENTURE_ENTITY(venture_dashboard_new());
	g_object_set(dashboard, "name", "Guards", "slug", "guards", NULL);
	venture_entity_set_organization_id(dashboard, f->org);
	g_assert_true(venture_database_save(f->db, dashboard, NULL, &error));
	g_assert_no_error(error);

	/* Saved: refused. */
	broken = venture_dashboard_widget_new();
	g_object_set(broken, "dashboard-id", venture_entity_get_id(dashboard), "kind", "query",
	             "title", "Broken", "body", "fetch ticket | filter title == \"\xfc",
	             "options", "{\"chart\": \"table\"}", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(broken), f->org);
	g_assert_false(venture_database_save(f->db, VENTURE_ENTITY(broken), NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_assert_cmpstr(error->message, ==, NOT_UTF8);
	g_clear_error(&error);

	/* Stored all the same, past the save: drawn as a refusal. */
	widget = venture_dashboard_widget_new();
	g_object_set(widget, "dashboard-id", venture_entity_get_id(dashboard), "kind", "query",
	             "title", "Stored", "body", "fetch ticket | fields title",
	             "options", "{\"chart\": \"table\"}", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(widget), f->org);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(widget), NULL, &error));
	g_assert_no_error(error);
	sql = g_strdup_printf("UPDATE %s SET body = CAST(X'66657463682074696B6574207C2066696C746572207469746C65203D3D2022FC' AS TEXT) "
	                      "WHERE id = %" G_GINT64_FORMAT,
	                      venture_entity_get_table_name(VENTURE_ENTITY(widget)),
	                      venture_entity_get_id(VENTURE_ENTITY(widget)));
	g_assert_true(venture_database_execute(f->db, sql, NULL, &error));
	g_assert_no_error(error);

	before = counts();
	fixture_send(f, &x, "GET", "/dashboards/guards", f->cookie, NULL, NULL, NULL);
	g_assert_cmpuint(x.status, ==, 200);
	g_assert_nonnull(strstr(x.text, NOT_UTF8));
	exchange_clear(&x);

	g_assert_cmpuint(counts().ran, ==, before.ran);
	assert_idle();
}

/* --- 2. Who may have a worker ------------------------------------------------- */

/*
 * One person may have two queries running or waiting: a third, from the
 * API or a page's bar, is refused at once -- 503 with Retry-After, and
 * the bar's problem on the page -- while somebody else is still served.
 * When the two finish, everything held is given back.
 */
static void
test_principal_cap(
	Fixture		*f,
	gconstpointer	 data
){
	g_autoptr(JsonNode) refused = NULL;
	Exchange first;
	Exchange second;
	Exchange x;

	(void)data;

	fixture_query_start(f, &first, f->ada_secret, QUERY_HOLD);
	fixture_query_start(f, &second, f->ada_secret, QUERY_HOLD);
	wait_holding(2);
	g_assert_cmpuint(counts().admitted, ==, 2);

	/* The API: 503 busy, Retry-After. */
	{
		g_autofree gchar *body = query_body(QUERY_MARK);

		fixture_send(f, &x, "POST", "/api/v1/query", NULL, f->ada_secret, "application/json", body);
	}
	refused = venture_json_parse(x.text, NULL);
	g_test_message("third query: %s", x.text);
	g_assert_cmpuint(x.status, ==, 503);
	g_assert_cmpstr(x.retry_after, ==, "2");
	g_assert_cmpstr(message_of(refused, "error"), ==, "busy");
	g_assert_nonnull(strstr(message_of(refused, "message"), "the most one person may"));
	exchange_clear(&x);

	/* The list API's ?q=: the same. */
	fixture_send(f, &x, "GET", "/api/v1/ticket?q=title%20!%3D%20%22%22", NULL, f->ada_secret,
	             NULL, NULL);
	g_assert_cmpuint(x.status, ==, 503);
	g_assert_cmpstr(x.retry_after, ==, "2");
	exchange_clear(&x);

	/* A page: the bar's problem, 503. ada's session is ada. */
	fixture_send(f, &x, "GET", "/e/ticket?q=title%20!%3D%20%22%22", f->cookie, NULL, NULL, NULL);
	g_assert_cmpuint(x.status, ==, 503);
	g_assert_cmpstr(x.retry_after, ==, "2");
	g_assert_nonnull(strstr(x.text, "lql-problem"));
	g_assert_nonnull(strstr(x.text, "the most one person may"));
	exchange_clear(&x);

	/* Nothing of those three ran. Somebody else is served meanwhile. */
	g_assert_cmpint(g_atomic_int_get(&marks), ==, 0);
	{
		g_autofree gchar *body = query_body(QUERY_MARK);

		fixture_send(f, &x, "POST", "/api/v1/query", NULL, f->vee_secret, "application/json", body);
	}
	g_assert_cmpuint(x.status, ==, 200);
	g_assert_cmpint(g_atomic_int_get(&marks), ==, 1);
	exchange_clear(&x);

	hold_release();
	exchange_finish(&first);
	exchange_finish(&second);
	g_assert_cmpuint(first.status, ==, 200);
	g_assert_cmpuint(second.status, ==, 200);
	exchange_clear(&first);
	exchange_clear(&second);
	assert_idle();
}

/* Holds all four workers: two queries each from ada and vee. */
static void
fill_workers(
	Fixture		*f,
	Exchange	 holders[4]
){
	fixture_query_start(f, &holders[0], f->ada_secret, QUERY_HOLD);
	fixture_query_start(f, &holders[1], f->ada_secret, QUERY_HOLD);
	fixture_query_start(f, &holders[2], f->vee_secret, QUERY_HOLD);
	fixture_query_start(f, &holders[3], f->vee_secret, QUERY_HOLD);
	wait_holding(4);
	g_assert_cmpuint(counts().running, ==, 4);
}

/* Lets the four go and takes their answers. */
static void
drain_workers(Exchange holders[4])
{
	guint i;

	hold_release();

	for (i = 0; i < 4; i++)
	{
		exchange_finish(&holders[i]);
		exchange_clear(&holders[i]);
	}
}

/*
 * A query that is still waiting for a worker at its deadline stops
 * waiting and is answered, 504, without ever running; the worker that
 * reaches it later skips it, and what it held is given back.
 */
static void
test_deadline_unstarted(
	Fixture		*f,
	gconstpointer	 data
){
	g_autoptr(JsonNode) answer = NULL;
	Exchange holders[4];
	Exchange x;
	Counts before;
	gint64 started;
	gint64 elapsed_ms;

	(void)data;

	fill_workers(f, holders);
	before = counts();

	/* ana's query waits in the queue; its deadline is 50 ms and the
	 * second's grace. */
	g_object_set(f->config, "query-timeout-ms", (gint64)50, NULL);
	started = g_get_monotonic_time();
	{
		g_autofree gchar *body = query_body(QUERY_MARK);

		fixture_send(f, &x, "POST", "/api/v1/query", NULL, f->ana_secret, "application/json", body);
	}
	elapsed_ms = (g_get_monotonic_time() - started) / 1000;
	answer = venture_json_parse(x.text, NULL);
	g_test_message("queued past its deadline: %u after %" G_GINT64_FORMAT " ms: %s",
	               x.status, elapsed_ms, x.text);
	g_assert_cmpuint(x.status, ==, 504);
	g_assert_cmpstr(message_of(answer, "error"), ==, "timeout");
	g_assert_nonnull(strstr(message_of(answer, "message"), "No query worker was free"));
	g_assert_cmpint(elapsed_ms, <, 10000);
	exchange_clear(&x);
	g_object_set(f->config, "query-timeout-ms", (gint64)60000, NULL);

	/* Answered while the workers are still held: it never started, and
	 * still holds its place until a worker gets to it. */
	g_assert_cmpint(holding_now(), ==, 4);
	g_assert_cmpuint(counts().admitted, ==, 5);

	drain_workers(holders);
	assert_idle();
	g_assert_cmpuint(counts().skipped, ==, before.skipped + 1);
	/* The four were counted when they started, before @before; the
	 * fifth never started. */
	g_assert_cmpuint(counts().ran, ==, before.ran);
	g_assert_cmpint(g_atomic_int_get(&marks), ==, 0);
}

/* Work that says it ran. */
static void
mark_work(gpointer data)
{
	g_atomic_int_set((gint *)data, 1);
}

/* Cancels @data (a GCancellable) a little later, from another thread. */
static gpointer
cancel_later(gpointer data)
{
	g_usleep(100000);
	g_cancellable_cancel(G_CANCELLABLE(data));

	return NULL;
}

/*
 * A job cancelled while it waits for a worker -- before it was queued,
 * and while it waits, from another thread -- is answered at once, not at
 * its deadline, and the worker that reaches it skips it: its work never
 * runs, and what it held is given back.
 */
static void
test_cancelled_unstarted(
	Fixture		*f,
	gconstpointer	 data
){
	Exchange holders[4];
	Counts before;
	gint ran = 0;
	guint pass;

	(void)data;

	fill_workers(f, holders);
	before = counts();

	for (pass = 0; pass < 2; pass++)
	{
		g_autoptr(GCancellable) cancellable = g_cancellable_new();
		g_autoptr(GError) error = NULL;
		GThread *thread = NULL;
		gint64 started;

		if (0 == pass)
			g_cancellable_cancel(cancellable);
		else
			thread = g_thread_new("cancel-later", cancel_later, cancellable);

		started = g_get_monotonic_time();
		g_assert_false(venture_web_server_query_off_loop(f->server, mark_work, &ran,
		                                                 cancellable, &error));
		g_test_message("pass %u: %s after %" G_GINT64_FORMAT " ms", pass, error->message,
		               (g_get_monotonic_time() - started) / 1000);
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_TIMEOUT);
		g_assert_nonnull(strstr(error->message, "cancelled before a worker started it"));

		/* Well before the 61 s deadline. */
		g_assert_cmpint(g_get_monotonic_time() - started, <, 10 * G_TIME_SPAN_SECOND);

		if (NULL != thread)
			g_thread_join(thread);
	}

	/* Both still queued behind the four, holding their places. */
	g_assert_cmpuint(counts().admitted, ==, 6);

	drain_workers(holders);
	assert_idle();
	g_assert_cmpuint(counts().skipped, ==, before.skipped + 2);
	g_assert_cmpuint(counts().ran, ==, before.ran);
	g_assert_cmpint(ran, ==, 0);
}

/* --- Two hosted workspaces, one process --------------------------------------- */

#define WORKSPACE_A "3d6f4c1e-2a7b-4c3d-9e8f-0a1b2c3d4e5f"
#define WORKSPACE_B "7b2e9d40-5c61-4f8a-8b9c-1d2e3f405162"
#define ORIGIN_A "https://alpha.example.test:8443"
#define ORIGIN_B "https://beta.example.test:8443"
#define AUTHORITY_A "alpha.example.test:8443"
#define AUTHORITY_B "beta.example.test:8443"

typedef struct
{
	gchar			*directory;
	VentureConfig		*config;
	VentureDatabase		*db;
	VentureContext		*context;
	VentureWebServer	*server;
	VentureTenantService	*service;
	gint64			 org;
	gchar			*secrets[3];
	const gchar		*authority;
} Workspace;

typedef struct
{
	Workspace	 a;
	Workspace	 b;
	SoupSession	*session;
} Hosted;

/* A hosted member of the workspace's business, with a token. */
static gchar *
hosted_member(
	Workspace	*w,
	const gchar	*username
){
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureTenantMaintenance) maintenance = NULL;
	g_autoptr(VentureEntity) user = NULL;
	g_autoptr(VentureEntity) tenant = NULL;
	g_autoptr(VentureEntity) membership = NULL;

	maintenance = venture_tenant_service_enter_maintenance(w->service, "Provision a member", &error);
	g_assert_no_error(error);
	user = g_object_new(VENTURE_TYPE_USER, "username", username, "organization-id", w->org,
	                    "role", VENTURE_USER_ROLE_EDITOR, "active", TRUE, NULL);
	g_assert_true(venture_database_save(w->db, user, NULL, &error));
	g_assert_no_error(error);
	tenant = g_object_new(VENTURE_TYPE_TENANT_MEMBERSHIP, "name", username,
	                      "user-id", venture_entity_get_id(user),
	                      "role", VENTURE_TENANT_ROLE_MEMBER, "active", TRUE, NULL);
	g_assert_true(venture_database_save(w->db, tenant, NULL, &error));
	g_assert_no_error(error);
	membership = g_object_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP, "organization-id", w->org,
	                          "user-id", venture_entity_get_id(user),
	                          "role", VENTURE_ORGANIZATION_ROLE_EDITOR, "active", TRUE, NULL);
	g_assert_true(venture_database_save(w->db, membership, NULL, &error));
	g_assert_no_error(error);
	g_assert_true(venture_tenant_maintenance_finish(maintenance, &error));
	g_assert_no_error(error);

	return fixture_token(w->db, username, venture_entity_get_id(user), VENTURE_USER_ROLE_EDITOR);
}

static void
workspace_setup(
	Workspace	*w,
	const gchar	*id,
	const gchar	*origin,
	const gchar	*authority,
	const gchar	*prefix
){
	g_autoptr(GInetAddress) loopback_address = g_inet_address_new_loopback(G_SOCKET_FAMILY_IPV4);
	g_autofree gchar *loopback = g_inet_address_to_string(loopback_address);
	g_autoptr(VentureEntity) home = NULL;
	g_autoptr(VentureEntity) ticket = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *uri = NULL;
	guint i;

	w->authority = authority;
	w->directory = g_dir_make_tmp("venture-lql-guards-hosted-XXXXXX", NULL);
	w->config = venture_config_new();
	g_object_set(w->config, "hosted-enabled", TRUE, "hosted-workspace-id", id,
	             "hosted-origin", origin, "security-password-iterations", (gint64)2000,
	             "state-dir", w->directory, "server-bind-address", loopback,
	             "server-port", (gint64)0, "query-timeout-ms", (gint64)60000, NULL);

	uri = g_strconcat("sqlite://", w->directory, "/venture.db", NULL);
	w->db = venture_database_new(uri, &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(w->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);

	w->service = venture_tenant_service_get(w->db);
	g_assert_true(venture_tenant_service_configure(w->service, w->config, &error));
	g_assert_no_error(error);
	g_assert_true(venture_tenant_service_initialize(w->service, &error));
	g_assert_no_error(error);

	home = g_object_new(VENTURE_TYPE_ORGANIZATION, "name", prefix, "active", TRUE,
	                    "is-default", TRUE, NULL);
	g_assert_true(venture_database_save(w->db, home, NULL, &error));
	g_assert_no_error(error);
	w->org = venture_entity_get_id(home);

	g_assert_true(venture_tenant_service_bootstrap_admin(w->service, w->config, "provisioner",
		"private-test-password", FALSE, "Guards fixture", &error));
	g_assert_no_error(error);

	for (i = 0; i < G_N_ELEMENTS(w->secrets); i++)
	{
		g_autofree gchar *username = g_strdup_printf("%s-%u", prefix, i);

		w->secrets[i] = hosted_member(w, username);
	}

	ticket = g_object_new(VENTURE_TYPE_TICKET, "title", prefix, "status", VENTURE_TICKET_STATUS_TODO,
	                      NULL);
	venture_entity_set_organization_id(ticket, w->org);
	g_assert_true(venture_database_save(w->db, ticket, NULL, &error));
	g_assert_no_error(error);

	w->context = venture_context_new(w->config, w->db);
	w->server = venture_web_server_new(w->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(w->server, &error));
	g_assert_no_error(error);
}

static void
workspace_teardown(Workspace *w)
{
	guint i;

	venture_web_server_stop(w->server);
	g_clear_object(&w->server);
	g_clear_object(&w->context);

	for (i = 0; i < G_N_ELEMENTS(w->secrets); i++)
		g_clear_pointer(&w->secrets[i], g_free);

	g_clear_object(&w->db);
	g_clear_object(&w->config);
	venture_test_remove_tree(w->directory);
	g_clear_pointer(&w->directory, g_free);
}

static void
hosted_setup(
	Hosted		*h,
	gconstpointer	 data
){
	(void)data;

	hold_close();
	g_atomic_int_set(&marks, 0);
	venture_lql_set_environment_hook(add_functions, NULL);
	workspace_setup(&h->a, WORKSPACE_A, ORIGIN_A, AUTHORITY_A, "alpha");
	workspace_setup(&h->b, WORKSPACE_B, ORIGIN_B, AUTHORITY_B, "beta");
	h->session = soup_session_new_with_options("timeout", 60, "max-conns-per-host", 32,
	                                           "max-conns", 64, NULL);
}

static void
hosted_teardown(
	Hosted		*h,
	gconstpointer	 data
){
	(void)data;

	hold_release();
	assert_idle();
	g_clear_object(&h->session);
	workspace_teardown(&h->a);
	workspace_teardown(&h->b);
	venture_lql_set_environment_hook(NULL, NULL);
}

/* POST /api/v1/query {"query": @text} on @w as its member @who, started. */
static void
hosted_query_start(
	Hosted		*h,
	Workspace	*w,
	Exchange	*x,
	guint		 who,
	const gchar	*text
){
	g_autofree gchar *body = query_body(text);

	exchange_start(h->session, x, venture_web_server_get_base_url(w->server), w->authority,
	               "POST", "/api/v1/query", NULL, w->secrets[who], "application/json", body);
}

/*
 * Workspace A's flood cannot take the pool: A has three queries held, the
 * most one workspace may, and its fourth -- from a member who has none
 * running -- is refused 503 at once; workspace B is answered while A's
 * three still hold their workers, by the worker A cannot reach.
 */
static void
test_workspace_cap(
	Hosted		*h,
	gconstpointer	 data
){
	g_autoptr(JsonNode) refused = NULL;
	g_autoptr(JsonNode) answered = NULL;
	Exchange flood[3];
	Exchange x;
	guint i;

	(void)data;

	hosted_query_start(h, &h->a, &flood[0], 0, QUERY_HOLD);
	hosted_query_start(h, &h->a, &flood[1], 0, QUERY_HOLD);
	hosted_query_start(h, &h->a, &flood[2], 1, QUERY_HOLD);
	wait_holding(3);
	g_assert_cmpuint(counts().running, ==, 3);

	/* A's fourth, from its third member: the workspace's cap. */
	hosted_query_start(h, &h->a, &x, 2, QUERY_MARK);
	exchange_finish(&x);
	refused = venture_json_parse(x.text, NULL);
	g_test_message("A's fourth: %s", x.text);
	g_assert_cmpuint(x.status, ==, 503);
	g_assert_cmpstr(x.retry_after, ==, "2");
	g_assert_cmpstr(message_of(refused, "error"), ==, "busy");
	g_assert_nonnull(strstr(message_of(refused, "message"), "the most one workspace may"));
	exchange_clear(&x);

	/* B, while A's three hold: answered, with B's own rows. */
	hosted_query_start(h, &h->b, &x, 0, "fetch ticket | fields title, z = mark()");
	exchange_finish(&x);
	answered = venture_json_parse(x.text, NULL);
	g_assert_cmpuint(x.status, ==, 200);
	g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(
		json_object_get_array_member(json_node_get_object(answered), "rows"), 0), "title"),
		==, "beta");
	g_assert_cmpint(holding_now(), ==, 3);
	g_assert_cmpint(g_atomic_int_get(&marks), ==, 1);
	exchange_clear(&x);

	hold_release();

	for (i = 0; i < G_N_ELEMENTS(flood); i++)
	{
		exchange_finish(&flood[i]);
		g_assert_cmpuint(flood[i].status, ==, 200);
		exchange_clear(&flood[i]);
	}

	assert_idle();
}

int
main(
	int	  argc,
	char	**argv
){
	g_test_init(&argc, &argv, NULL);

#define ADD(path, func) \
	g_test_add(path, Fixture, NULL, fixture_setup, func, fixture_teardown)
#define ADD_HOSTED(path, func) \
	g_test_add(path, Hosted, NULL, hosted_setup, func, hosted_teardown)

	ADD("/lql/guards/utf8/refused-everywhere", test_utf8_refused_everywhere);
	ADD("/lql/guards/utf8/widget", test_utf8_widget);
	ADD("/lql/guards/admission/principal-cap", test_principal_cap);
	ADD("/lql/guards/admission/deadline-unstarted", test_deadline_unstarted);
	ADD("/lql/guards/admission/cancelled-unstarted", test_cancelled_unstarted);
	ADD_HOSTED("/lql/guards/admission/workspace-cap", test_workspace_cap);

#undef ADD
#undef ADD_HOSTED

	return g_test_run();
}
