/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * test-lql-query-hosted.c - LQL in hosted mode: two workspaces, one
 * process (docs/lql.org, "Where a query runs")
 *
 * Copyright (C) 2026 Zach Podbielniak
 *
 * A hosted workspace is a database of its own, served by a server of its
 * own. Two of them in one process share everything that is process-wide:
 * the entity registry, the fibers' thread, the query workers and their
 * read dispatch. A query is evaluated on a worker and its fetches are
 * handed back to the request's fiber, so a fetch read on the wrong
 * fiber -- or as the wrong caller, or outside the request's access scope
 * -- would put one workspace's rows in the other's answer. What this
 * pins, for POST /api/v1/query and for the query widget alike:
 *
 *   - a member of workspace A sees A's tickets and none of B's, and the
 *     other way round, including while both queries are on workers at
 *     the same time, interleaving their fetches;
 *   - A's credential is nobody in B: B refuses it.
 */

#include <venture.h>
#include <libsoup/soup.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>
#include "lql/venture-lql.h"
#include "venture-test-util.h"

/* VENTURE #44's query: nested concats of 16 MiB strings, 8 GiB at the end. */
#define QUERY_44 \
	"data {} | fields_add a = repeat(\"x\", 16777216), b = concat(a,a,a,a,a,a,a,a), " \
	"c = concat(b,b,b,b,b,b,b,b), d = concat(c,c,c,c,c,c,c,c) | fields n = length(d)"

/* A 2 MiB string: inside every per-result cap, over a 1 MiB budget. */
#define QUERY_2_MIB "data {} | fields_add a = repeat(\"x\", 2097152) | fields n = length(a)"

#define WORKSPACE_A "3d6f4c1e-2a7b-4c3d-9e8f-0a1b2c3d4e5f"
#define WORKSPACE_B "7b2e9d40-5c61-4f8a-8b9c-1d2e3f405162"
#define ORIGIN_A "https://alpha.example.test:8443"
#define ORIGIN_B "https://beta.example.test:8443"
#define AUTHORITY_A "alpha.example.test:8443"
#define AUTHORITY_B "beta.example.test:8443"

/* Every ticket title, sorted: whose rows a query answered with. */
#define QUERY_TITLES "fetch ticket | fields title | sort title"

/* The same, each row taking a moment on the worker, so two queries are
 * on workers at once and their fetches interleave. */
#define QUERY_TITLES_SLOW "fetch ticket | fields title, z = nap() | sort title"

/* One hosted workspace: its own directory, database, server and member. */
typedef struct
{
	gchar			*directory;
	VentureConfig		*config;
	VentureDatabase		*db;
	VentureContext		*context;
	VentureWebServer	*server;
	VentureTenantService	*service;
	gint64			 org;
	gchar			*member_secret;
	const gchar		*authority;
} Workspace;

typedef struct
{
	Workspace	 a;
	Workspace	 b;
	SoupSession	*session;
} Fixture;

/* --- nap(): a function that takes time, on the worker ------------------- */

static LqlValue *
nap(
	LqlFunctionCall	 *call,
	gpointer	  user_data,
	GError		**error
){
	(void)call;
	(void)user_data;
	(void)error;

	g_usleep(30000);

	return lql_value_new_int(1);
}

/* A #VentureLqlEnvironmentHook: nap() in every environment. */
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

/* --- A workspace -------------------------------------------------------- */

/* An API token for @user_id, minted after its memberships exist. */
static gchar *
mint(
	VentureDatabase	*db,
	gint64		 user_id
){
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureApiToken) token = venture_api_token_new();
	gchar *secret;

	g_object_set(token, "name", "query member", "user-id", user_id,
	             "role", VENTURE_USER_ROLE_EDITOR, NULL);
	secret = venture_api_token_generate(token);
	g_assert_true(venture_database_save(db, VENTURE_ENTITY(token), NULL, &error));
	g_assert_no_error(error);

	return secret;
}

/* A hosted member of the workspace's business, as an invitation leaves one. */
static gint64
member(
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

	return venture_entity_get_id(user);
}

/* One ticket titled @title in the workspace's business. */
static void
ticket(
	Workspace	*w,
	const gchar	*title
){
	g_autoptr(VentureEntity) entity = NULL;
	g_autoptr(GError) error = NULL;

	entity = g_object_new(VENTURE_TYPE_TICKET, "title", title,
	                      "status", VENTURE_TICKET_STATUS_TODO, NULL);
	venture_entity_set_organization_id(entity, w->org);
	g_assert_true(venture_database_save(w->db, entity, NULL, &error));
	g_assert_no_error(error);
}

/* A dashboard "titles" with one query widget "Titles" in the business. */
static void
dashboard(Workspace *w)
{
	g_autoptr(VentureEntity) board = NULL;
	g_autoptr(VentureDashboardWidget) widget = NULL;
	g_autoptr(GError) error = NULL;

	board = VENTURE_ENTITY(venture_dashboard_new());
	g_object_set(board, "name", "Titles", "slug", "titles", NULL);
	venture_entity_set_organization_id(board, w->org);
	g_assert_true(venture_database_save(w->db, board, NULL, &error));
	g_assert_no_error(error);

	widget = venture_dashboard_widget_new();
	g_object_set(widget, "dashboard-id", venture_entity_get_id(board), "kind", "query",
	             "title", "Titles", "body", QUERY_TITLES_SLOW,
	             "options", "{\"chart\": \"table\"}", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(widget), w->org);
	g_assert_true(venture_database_save(w->db, VENTURE_ENTITY(widget), NULL, &error));
	g_assert_no_error(error);
}

/*
 * A hosted workspace @id at @origin, reached as @authority, with tickets
 * titled @prefix-1 and @prefix-2, a member with a token, and the dashboard.
 */
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
	g_autoptr(GError) error = NULL;
	g_autofree gchar *uri = NULL;
	g_autofree gchar *first = g_strconcat(prefix, "-1", NULL);
	g_autofree gchar *second = g_strconcat(prefix, "-2", NULL);
	g_autofree gchar *username = g_strconcat(prefix, "-member", NULL);
	gint64 user_id;

	w->authority = authority;
	w->directory = g_dir_make_tmp("venture-lql-hosted-XXXXXX", NULL);
	w->config = venture_config_new();
	g_object_set(w->config, "hosted-enabled", TRUE, "hosted-workspace-id", id,
	             "hosted-origin", origin, "security-password-iterations", (gint64)2000,
	             "state-dir", w->directory, "server-bind-address", loopback,
	             "server-port", (gint64)0, NULL);

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
		"private-test-password", FALSE, "Query fixture", &error));
	g_assert_no_error(error);

	user_id = member(w, username);
	w->member_secret = mint(w->db, user_id);
	ticket(w, first);
	ticket(w, second);
	dashboard(w);

	w->context = venture_context_new(w->config, w->db);
	w->server = venture_web_server_new(w->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(w->server, &error));
	g_assert_no_error(error);
}

static void
workspace_teardown(Workspace *w)
{
	venture_web_server_stop(w->server);
	g_clear_object(&w->server);
	g_clear_object(&w->context);
	g_clear_pointer(&w->member_secret, g_free);
	g_clear_object(&w->db);
	g_clear_object(&w->config);
	venture_test_remove_tree(w->directory);
	g_clear_pointer(&w->directory, g_free);
}

static void
fixture_setup(
	Fixture		*f,
	gconstpointer	 data
){
	(void)data;

	venture_lql_set_environment_hook(add_nap, NULL);
	workspace_setup(&f->a, WORKSPACE_A, ORIGIN_A, AUTHORITY_A, "alpha");
	workspace_setup(&f->b, WORKSPACE_B, ORIGIN_B, AUTHORITY_B, "beta");
	f->session = soup_session_new_with_options("timeout", 30, NULL);
}

static void
fixture_teardown(
	Fixture		*f,
	gconstpointer	 data
){
	(void)data;

	g_clear_object(&f->session);
	workspace_teardown(&f->a);
	workspace_teardown(&f->b);
	venture_lql_set_environment_hook(NULL, NULL);
}

/* --- HTTP --------------------------------------------------------------- */

/* One request in flight. */
typedef struct
{
	SoupMessage	*message;
	GBytes		*body;
	GError		*error;
	gboolean	 done;
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

/* Starts @method @path on @w's server as @secret, without waiting. */
static void
exchange_start(
	Fixture		*f,
	Workspace	*w,
	Exchange	*x,
	const gchar	*method,
	const gchar	*path,
	const gchar	*secret,
	const gchar	*json
){
	g_autofree gchar *base = g_strdup(venture_web_server_get_base_url(w->server));
	g_autofree gchar *url = NULL;
	g_autofree gchar *bearer = g_strconcat("Bearer ", secret, NULL);
	SoupMessageHeaders *headers;

	if (g_str_has_suffix(base, "/"))
		base[strlen(base) - 1] = '\0';

	memset(x, 0, sizeof *x);
	url = g_strconcat(base, path, NULL);
	x->message = soup_message_new(method, url);
	headers = soup_message_get_request_headers(x->message);
	soup_message_headers_replace(headers, "Host", w->authority);
	soup_message_headers_replace(headers, "Authorization", bearer);
	soup_message_headers_replace(headers, "Accept", "application/json");

	if (NULL != json)
	{
		g_autoptr(GBytes) bytes = g_bytes_new(json, strlen(json));

		soup_message_set_request_body_from_bytes(x->message, "application/json", bytes);
	}

	soup_session_send_and_read_async(f->session, x->message, G_PRIORITY_DEFAULT, NULL,
	                                 exchange_done, x);
}

/* Drives the loop until @x is answered; its status, its body parsed. */
static JsonNode *
exchange_finish(
	Exchange	*x,
	guint		*out_status
){
	g_autofree gchar *text = NULL;
	JsonNode *node;

	while (!x->done)
		g_main_context_iteration(NULL, TRUE);

	g_assert_no_error(x->error);
	*out_status = soup_message_get_status(x->message);
	text = g_strndup(g_bytes_get_data(x->body, NULL), g_bytes_get_size(x->body));
	g_test_message("%s %s -> %u %s", soup_message_get_method(x->message),
	               g_uri_get_path(soup_message_get_uri(x->message)), *out_status, text);
	node = venture_json_parse(text, NULL);
	g_clear_pointer(&x->body, g_bytes_unref);
	g_clear_object(&x->message);

	return node;
}

/* POST /api/v1/query {"query": @text} on @w as @secret, started. */
static void
query_start(
	Fixture		*f,
	Workspace	*w,
	Exchange	*x,
	const gchar	*secret,
	const gchar	*text
){
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(JsonNode) request = NULL;
	g_autofree gchar *body = NULL;

	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "query");
	json_builder_add_string_value(builder, text);
	json_builder_end_object(builder);
	request = json_builder_get_root(builder);
	body = venture_json_to_string(request, FALSE);
	exchange_start(f, w, x, "POST", "/api/v1/query", secret, body);
}

/* The titles in a document's rows, joined by commas. */
static gchar *
titles_of(JsonObject *document)
{
	g_autoptr(GString) titles = g_string_new(NULL);
	JsonArray *rows = json_object_get_array_member(document, "rows");
	guint i;

	for (i = 0; i < json_array_get_length(rows); i++)
	{
		if (i > 0)
			g_string_append_c(titles, ',');

		g_string_append(titles, json_object_get_string_member(
			json_array_get_object_element(rows, i), "title"));
	}

	return g_string_free(g_steal_pointer(&titles), FALSE);
}

/* The data of the widget "Titles" in GET /api/v1/dashboards/titles. */
static JsonObject *
widget_data(JsonNode *described)
{
	JsonArray *widgets = json_object_get_array_member(json_node_get_object(described), "widgets");
	guint i;

	for (i = 0; i < json_array_get_length(widgets); i++)
	{
		JsonObject *widget = json_array_get_object_element(widgets, i);

		if (0 == g_strcmp0(json_object_get_string_member(widget, "title"), "Titles"))
		{
			if (!JSON_NODE_HOLDS_OBJECT(json_object_get_member(widget, "data")))
				g_error("the widget answered no data: %s",
				        json_object_get_string_member(widget, "error"));

			return json_object_get_object_member(widget, "data");
		}
	}

	g_error("no widget titled Titles");
	return NULL;
}

/* --- The tests ---------------------------------------------------------- */

/*
 * POST /api/v1/query: each workspace's member sees exactly its own
 * tickets, one query at a time and two on workers at once.
 */
static void
test_api_isolated(
	Fixture		*f,
	gconstpointer	 data
){
	g_autoptr(JsonNode) a = NULL;
	g_autoptr(JsonNode) b = NULL;
	g_autoptr(JsonNode) crossed = NULL;
	g_autofree gchar *a_titles = NULL;
	g_autofree gchar *b_titles = NULL;
	Exchange xa;
	Exchange xb;
	guint status = 0;

	(void)data;

	/* One at a time. */
	query_start(f, &f->a, &xa, f->a.member_secret, QUERY_TITLES);
	a = exchange_finish(&xa, &status);
	g_assert_cmpuint(status, ==, 200);
	a_titles = titles_of(json_node_get_object(a));
	g_assert_cmpstr(a_titles, ==, "alpha-1,alpha-2");
	g_clear_pointer(&a, json_node_unref);
	g_clear_pointer(&a_titles, g_free);

	query_start(f, &f->b, &xb, f->b.member_secret, QUERY_TITLES);
	b = exchange_finish(&xb, &status);
	g_assert_cmpuint(status, ==, 200);
	b_titles = titles_of(json_node_get_object(b));
	g_assert_cmpstr(b_titles, ==, "beta-1,beta-2");
	g_clear_pointer(&b, json_node_unref);
	g_clear_pointer(&b_titles, g_free);

	/* Both on workers at once, their fetches handed back interleaved. */
	query_start(f, &f->a, &xa, f->a.member_secret, QUERY_TITLES_SLOW);
	query_start(f, &f->b, &xb, f->b.member_secret, QUERY_TITLES_SLOW);
	a = exchange_finish(&xa, &status);
	g_assert_cmpuint(status, ==, 200);
	b = exchange_finish(&xb, &status);
	g_assert_cmpuint(status, ==, 200);
	a_titles = titles_of(json_node_get_object(a));
	b_titles = titles_of(json_node_get_object(b));
	g_assert_cmpstr(a_titles, ==, "alpha-1,alpha-2");
	g_assert_cmpstr(b_titles, ==, "beta-1,beta-2");

	/* A's credential is nobody in B. */
	query_start(f, &f->b, &xb, f->a.member_secret, QUERY_TITLES);
	crossed = exchange_finish(&xb, &status);
	g_assert_cmpuint(status, ==, 401);
	g_assert_false(json_object_has_member(json_node_get_object(crossed), "rows"));
}

/*
 * The query widget, through GET /api/v1/dashboards/:slug: the same
 * isolation, the widget's query evaluated on a worker for each request.
 */
static void
test_widget_isolated(
	Fixture		*f,
	gconstpointer	 data
){
	g_autoptr(JsonNode) a = NULL;
	g_autoptr(JsonNode) b = NULL;
	g_autofree gchar *a_titles = NULL;
	g_autofree gchar *b_titles = NULL;
	Exchange xa;
	Exchange xb;
	guint status = 0;

	(void)data;

	exchange_start(f, &f->a, &xa, "GET", "/api/v1/dashboards/titles", f->a.member_secret, NULL);
	exchange_start(f, &f->b, &xb, "GET", "/api/v1/dashboards/titles", f->b.member_secret, NULL);
	a = exchange_finish(&xa, &status);
	g_assert_cmpuint(status, ==, 200);
	b = exchange_finish(&xb, &status);
	g_assert_cmpuint(status, ==, 200);

	a_titles = titles_of(widget_data(a));
	b_titles = titles_of(widget_data(b));
	g_assert_cmpstr(a_titles, ==, "alpha-1,alpha-2");
	g_assert_cmpstr(b_titles, ==, "beta-1,beta-2");
}

/*
 * Caps this process's address space at what it maps now plus @headroom,
 * so an allocation without bound fails at once -- g_malloc() aborts --
 * instead of paging the machine to death. Called in a test's subprocess
 * only.
 */
static void
cap_address_space(guint64 headroom)
{
	g_autofree gchar *statm = NULL;
	struct rlimit limit;
	guint64 pages;

	g_assert_true(g_file_get_contents("/proc/self/statm", &statm, NULL, NULL));
	pages = g_ascii_strtoull(statm, NULL, 10);
	limit.rlim_cur = (rlim_t)(pages * (guint64)sysconf(_SC_PAGESIZE) + headroom);
	limit.rlim_max = limit.rlim_cur;
	g_assert_cmpint(setrlimit(RLIMIT_AS, &limit), ==, 0);
}

/*
 * Hosted mode has no read workers, so a query's fetches are read on the
 * main thread between its steps on a query worker; the memory budget
 * holds there too. VENTURE #44's query in workspace A is a refused query
 * (422), A and B both answer after it, and A's 1 MiB budget refuses a
 * 2 MiB string while B's default budget lets it through. In a capped
 * subprocess, as in test-lql-query.c.
 */
static void
test_max_bytes_refuses(
	Fixture		*f,
	gconstpointer	 data
){
	g_autoptr(JsonNode) refused = NULL;
	g_autoptr(JsonNode) a = NULL;
	g_autoptr(JsonNode) b = NULL;
	g_autoptr(JsonNode) budget = NULL;
	g_autoptr(JsonNode) roomy = NULL;
	g_autofree gchar *a_titles = NULL;
	g_autofree gchar *b_titles = NULL;
	const gchar *message;
	Exchange x;
	guint status = 0;

	(void)data;

	if (!g_test_subprocess())
	{
		g_test_trap_subprocess(NULL, 120 * G_USEC_PER_SEC, G_TEST_SUBPROCESS_DEFAULT);
		g_test_trap_assert_passed();
		return;
	}

	cap_address_space((guint64)3 * 1073741824);

	query_start(f, &f->a, &x, f->a.member_secret, QUERY_44);
	refused = exchange_finish(&x, &status);
	message = json_object_get_string_member(json_node_get_object(refused), "message");
	g_test_message("#44 in A -> %u %s", status, message);
	g_assert_cmpuint(status, ==, 422);
	g_assert_true((NULL != strstr(message, "would exceed 16 MiB")) ||
	              (NULL != strstr(message, "(max_bytes)")));

	/* Both workspaces still answer. */
	query_start(f, &f->a, &x, f->a.member_secret, QUERY_TITLES);
	a = exchange_finish(&x, &status);
	g_assert_cmpuint(status, ==, 200);
	a_titles = titles_of(json_node_get_object(a));
	g_assert_cmpstr(a_titles, ==, "alpha-1,alpha-2");
	query_start(f, &f->b, &x, f->b.member_secret, QUERY_TITLES);
	b = exchange_finish(&x, &status);
	g_assert_cmpuint(status, ==, 200);
	b_titles = titles_of(json_node_get_object(b));
	g_assert_cmpstr(b_titles, ==, "beta-1,beta-2");

	/* Each workspace's own budget. */
	g_object_set(f->a.config, "query-max-bytes", (gint64)1048576, NULL);
	query_start(f, &f->a, &x, f->a.member_secret, QUERY_2_MIB);
	budget = exchange_finish(&x, &status);
	message = json_object_get_string_member(json_node_get_object(budget), "message");
	g_test_message("2 MiB in A under 1 MiB -> %u %s", status, message);
	g_assert_cmpuint(status, ==, 422);
	g_assert_nonnull(strstr(message, "query.max_bytes (1048576)"));

	query_start(f, &f->b, &x, f->b.member_secret, QUERY_2_MIB);
	roomy = exchange_finish(&x, &status);
	g_assert_cmpuint(status, ==, 200);
	g_assert_cmpint(json_object_get_int_member(json_array_get_object_element(
		json_object_get_array_member(json_node_get_object(roomy), "rows"), 0), "n"), ==, 2097152);
}

int
main(
	int	  argc,
	char	**argv
){
	g_test_init(&argc, &argv, NULL);

#define ADD(path, func) \
	g_test_add(path, Fixture, NULL, fixture_setup, func, fixture_teardown)

	ADD("/lql/query-hosted/api/two-workspaces", test_api_isolated);
	ADD("/lql/query-hosted/widget/two-workspaces", test_widget_isolated);
	ADD("/lql/query-hosted/bounds/max-bytes-refuses", test_max_bytes_refuses);

#undef ADD

	return g_test_run();
}
