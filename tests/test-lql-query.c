/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * test-lql-query.c - POST /api/v1/query, venturectl query, venture_query
 * and the query widget (docs/lql.org, "Running a query")
 *
 * Copyright (C) 2026 Zach Podbielniak
 *
 * What the query surfaces promise, and what breaks if one regresses:
 *
 *   - A caller queries as themselves: a type the list API refuses them is
 *     refused with the list API's own answer (here and inside a lookup);
 *     a member sees no other organization's rows; a token minted for one
 *     organization sees only it, whatever its user may read.
 *   - query.max_rows is an error, never a short answer; query.timeout_ms
 *     is a timeout that says so.
 *   - CSV is the document's rows, defused against spreadsheet formulas.
 *   - Money totals are one per currency (sum_money()), in the rows, the
 *     CSV, the table and every chart: never one summed figure.
 *   - A query widget runs as whoever is looking: a viewer who cannot read
 *     what the author can sees a refusal or nothing, never the author's
 *     figures, and with nobody looking it refuses.
 *   - The API, venturectl query and the MCP tool give identical rows.
 *   - No file, program, SQL, git, journal or Kubernetes command is
 *     reachable through the API.
 */

#include <venture.h>
#include <libsoup/soup.h>
#include <string.h>
#include "lql/venture-lql.h"
#include "venture-test-util.h"
#include "venture-test-member.h"

/* "open tickets by assignee", the first acceptance query. */
#define QUERY_OPEN_BY_ASSIGNEE \
	"fetch ticket | filter status not in [\"done\", \"cancelled\"] " \
	"| summarize n = count(), by: assignee | sort assignee"

/* "invoices this quarter by status", the second: per-currency totals of
 * the lines' amounts, frozen on each line when its invoice is issued. */
#define QUERY_INVOICES_BY_STATUS \
	"fetch invoice_line " \
	"| lookup (fetch invoice), on: invoice_id == id, prefix: \"invoice_\" " \
	"| filter invoice_issued_at >= period(\"this_quarter\").start " \
	"and invoice_issued_at < period(\"this_quarter\").end " \
	"| summarize invoices = count_distinct(invoice_id), total = sum_money(income_amount), " \
	"by: {status = invoice_status} | sort status"

/* --- The fixture -------------------------------------------------------- */

typedef struct
{
	gchar			*directory;
	VentureConfig		*config;
	VentureDatabase		*db;
	VentureContext		*context;
	VentureWebServer	*server;
	SoupSession		*session;
	gint64			 org;		/* the default business, A */
	gint64			 org_b;		/* a second business, B */
	gint64			 admin_id;
	gint64			 viewer_id;
	gint64			 pinned_id;
	gchar			*admin_secret;
	gchar			*viewer_secret;
	gchar			*pinned_secret;
	GArray			*tickets_a;	/* gint64 */
	GArray			*tickets_b;	/* gint64 */
} Fixture;

static gint64
fixture_user(
	Fixture		*f,
	const gchar	*username,
	VentureUserRole	 role
){
	g_autoptr(VentureEntity) user = NULL;
	g_autoptr(GError) error = NULL;

	user = g_object_new(VENTURE_TYPE_USER, "username", username, "role", role,
	                    "active", TRUE, NULL);
	if (!venture_database_save(f->db, user, NULL, &error))
		g_error("saving: %s", error->message);

	return venture_entity_get_id(user);
}

static void
fixture_membership(
	Fixture		*f,
	gint64		 organization,
	gint64		 user_id,
	gint		 role
){
	g_autoptr(VentureEntity) member = NULL;
	g_autoptr(GError) error = NULL;

	member = g_object_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP, "organization-id", organization,
		"user-id", user_id, "active", TRUE, "role", role, NULL);
	if (!venture_database_save(f->db, member, NULL, &error))
		g_error("saving: %s", error->message);
}

/* An API token for @user_id; @snapshot (nullable) pins its organizations. */
static gchar *
fixture_token(
	Fixture		*f,
	const gchar	*name,
	gint64		 user_id,
	VentureUserRole	 role,
	const gchar	*snapshot
){
	g_autoptr(VentureApiToken) token = venture_api_token_new();
	g_autoptr(GError) error = NULL;
	gchar *secret;

	g_object_set(token, "name", name, "user-id", user_id, "role", role, NULL);

	if (NULL != snapshot)
		g_object_set(token, "membership-snapshot", snapshot, NULL);

	secret = venture_api_token_generate(token);
	if (!venture_database_save(f->db, VENTURE_ENTITY(token), NULL, &error))
		g_error("saving: %s", error->message);

	return secret;
}

static gint64
fixture_ticket(
	Fixture			*f,
	gint64			 organization,
	const gchar		*title,
	VentureTicketStatus	 status,
	const gchar		*assignee
){
	g_autoptr(VentureEntity) ticket = NULL;
	g_autoptr(GError) error = NULL;

	ticket = g_object_new(VENTURE_TYPE_TICKET, "title", title, "organization-id", organization,
		"status", status, "assignee", assignee, NULL);
	if (!venture_database_save(f->db, ticket, NULL, &error))
		g_error("saving: %s", error->message);

	return venture_entity_get_id(ticket);
}

/* The accounts issuing an invoice posts to, for a business that has none. */
static void
fixture_accounts(
	Fixture	*f,
	gint64	 organization
){
	static const struct { const gchar *code; const gchar *name; VentureAccountKind kind; } accounts[] = {
		{ "1000", "Cash", VENTURE_ACCOUNT_KIND_ASSET },
		{ "1100", "Receivables", VENTURE_ACCOUNT_KIND_ASSET },
		{ "2100", "Sales tax", VENTURE_ACCOUNT_KIND_LIABILITY },
		{ "4000", "Income", VENTURE_ACCOUNT_KIND_INCOME }
	};
	g_autoptr(GError) error = NULL;
	guint i;

	for (i = 0; i < G_N_ELEMENTS(accounts); i++)
	{
		g_autoptr(VentureEntity) account = NULL;

		account = g_object_new(VENTURE_TYPE_ACCOUNT, "code", accounts[i].code,
			"name", accounts[i].name, "kind", accounts[i].kind, "active", TRUE,
			"organization-id", organization, NULL);
		if (!venture_database_save(f->db, account, NULL, &error))
			g_error("saving account %s: %s", accounts[i].code, error->message);
	}
}

/*
 * An invoice in @organization with one line per amount, issued now
 * through the settlement service (which freezes each line's
 * income_amount) unless @issue is FALSE, which leaves it a draft.
 */
static void
fixture_invoice(
	Fixture			*f,
	gint64			 organization,
	const gchar		*number,
	gboolean		 issue,
	const gchar *const	*amounts
){
	g_autoptr(VentureEntity) customer = NULL;
	g_autoptr(VentureInvoice) invoice = venture_invoice_new();
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(GError) error = NULL;
	guint i;

	customer = g_object_new(VENTURE_TYPE_COMPANY, "name", number, "organization-id", organization, NULL);
	if (!venture_database_save(f->db, customer, NULL, &error))
		g_error("saving: %s", error->message);

	venture_entity_set_organization_id(VENTURE_ENTITY(invoice), organization);
	g_object_set(invoice, "number", number, "company-id", venture_entity_get_id(customer), NULL);
	if (!venture_database_save(f->db, VENTURE_ENTITY(invoice), NULL, &error))
		g_error("saving: %s", error->message);

	for (i = 0; NULL != amounts[i]; i++)
	{
		g_autoptr(VentureInvoiceLine) line = venture_invoice_line_new();

		venture_entity_set_organization_id(VENTURE_ENTITY(line), organization);
		g_object_set(line, "invoice-id", venture_entity_get_id(VENTURE_ENTITY(invoice)),
			"description", "Work", "quantity", 1.0, NULL);
		g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(line), "unit-price",
		                                                   amounts[i], NULL));
		if (!venture_database_save(f->db, VENTURE_ENTITY(line), NULL, &error))
			g_error("saving: %s", error->message);
	}

	if (issue && !venture_settlement_service_transition(venture_settlement_service_get(f->db),
	                                                    invoice, "sent", now, NULL, &error))
		g_error("issuing %s: %s", number, error->message);
}

static void
fixture_setup(
	Fixture		*f,
	gconstpointer	 data
){
	static const gchar *const dollars[] = { "1000.00 USD", "500.00 USD", NULL };
	static const gchar *const euros[] = { "300.00 EUR", NULL };
	static const gchar *const draft[] = { "75.00 USD", NULL };
	g_autoptr(VentureEntity) second = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *uri = NULL;
	g_autofree gchar *snapshot = NULL;
	gint64 id;

	(void)data;

	f->directory = g_dir_make_tmp("venture-lql-query-XXXXXX", NULL);
	f->config = venture_config_new();
	g_object_set(f->config, "state-dir", f->directory, "server-bind-address", "127.0.0.1",
	             "server-port", (gint64)0, "security-password-iterations", (gint64)2000,
	             "security-session-secret-env", "VENTURE_TEST_SESSION_SECRET", NULL);

	/* A file, so the server's read workers and the MCP loopback read the
	 * same rows the test wrote. */
	uri = g_strconcat("sqlite://", f->directory, "/venture.db", NULL);
	f->db = venture_database_new(uri, &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->db);
	f->org = venture_context_get_default_organization_id(f->context);
	f->tickets_a = g_array_new(FALSE, FALSE, sizeof(gint64));
	f->tickets_b = g_array_new(FALSE, FALSE, sizeof(gint64));

	second = g_object_new(VENTURE_TYPE_ORGANIZATION, "name", "Beta Works", "slug", "beta", NULL);
	if (!venture_database_save(f->db, second, NULL, &error))
		g_error("saving: %s", error->message);
	f->org_b = venture_entity_get_id(second);

	/* ada administers everything; vee views business A only; tia edits
	 * in A and B, through a token minted for A alone. */
	f->admin_id = fixture_user(f, "ada", VENTURE_USER_ROLE_ADMIN);
	f->viewer_id = fixture_user(f, "vee", VENTURE_USER_ROLE_VIEWER);
	fixture_membership(f, f->org, f->viewer_id, VENTURE_ORGANIZATION_ROLE_VIEWER);
	f->pinned_id = fixture_user(f, "tia", VENTURE_USER_ROLE_EDITOR);
	fixture_membership(f, f->org, f->pinned_id, VENTURE_ORGANIZATION_ROLE_EDITOR);
	venture_test_member(f->db, f->org, "ana");
	venture_test_member(f->db, f->org_b, "bo");

	f->admin_secret = fixture_token(f, "ada", f->admin_id, VENTURE_USER_ROLE_ADMIN, NULL);
	f->viewer_secret = fixture_token(f, "vee", f->viewer_id, VENTURE_USER_ROLE_VIEWER, NULL);
	/* tia's token is minted while she belongs to A alone; her membership
	 * of B comes after, and a later grant never widens a token. */
	f->pinned_secret = fixture_token(f, "tia-a", f->pinned_id, VENTURE_USER_ROLE_EDITOR, NULL);
	fixture_membership(f, f->org_b, f->pinned_id, VENTURE_ORGANIZATION_ROLE_EDITOR);
	{
		g_autoptr(VentureQuery) tokens = venture_query_new(VENTURE_TYPE_API_TOKEN);
		g_autoptr(GPtrArray) minted = NULL;

		g_assert_true(venture_query_add_filter_int(tokens, "user-id", VENTURE_FILTER_OP_EQ,
		                                           f->pinned_id, NULL));
		minted = venture_database_find(f->db, tokens, &error);
		g_assert_no_error(error);
		g_assert_cmpuint(minted->len, ==, 1);
		g_object_get(g_ptr_array_index(minted, 0), "membership-snapshot", &snapshot, NULL);
		g_test_message("tia's token snapshot: %s", snapshot);
	}

	/* Business A: open and closed work for vee and ana, and nobody's. */
	id = fixture_ticket(f, f->org, "A1", VENTURE_TICKET_STATUS_TODO, "vee");
	g_array_append_val(f->tickets_a, id);
	id = fixture_ticket(f, f->org, "A2", VENTURE_TICKET_STATUS_IN_PROGRESS, "vee");
	g_array_append_val(f->tickets_a, id);
	id = fixture_ticket(f, f->org, "A3", VENTURE_TICKET_STATUS_TODO, "ana");
	g_array_append_val(f->tickets_a, id);
	id = fixture_ticket(f, f->org, "A4", VENTURE_TICKET_STATUS_DONE, "ana");
	g_array_append_val(f->tickets_a, id);
	id = fixture_ticket(f, f->org, "=HYPERLINK(\"http://evil.test\")", VENTURE_TICKET_STATUS_TRIAGE, NULL);
	g_array_append_val(f->tickets_a, id);

	/* Business B: work vee may never see, assigned to vee by name. */
	id = fixture_ticket(f, f->org_b, "B1", VENTURE_TICKET_STATUS_TODO, "bo");
	g_array_append_val(f->tickets_b, id);
	id = fixture_ticket(f, f->org_b, "B2", VENTURE_TICKET_STATUS_TODO, "bo");
	g_array_append_val(f->tickets_b, id);

	/* Business B's invoices, this quarter: dollars and euros sent, and a
	 * draft. vee is not a member of B and must never see them. */
	fixture_accounts(f, f->org_b);
	fixture_invoice(f, f->org_b, "INV-1", TRUE, dollars);
	fixture_invoice(f, f->org_b, "INV-2", TRUE, euros);
	fixture_invoice(f, f->org_b, "INV-3", FALSE, draft);

	f->server = venture_web_server_new(f->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(f->server, &error));
	g_assert_no_error(error);
	f->session = soup_session_new_with_options("timeout", 30, NULL);
}

static void
fixture_teardown(
	Fixture		*f,
	gconstpointer	 data
){
	(void)data;

	venture_web_server_stop(f->server);
	g_clear_object(&f->session);
	g_clear_object(&f->server);
	g_clear_pointer(&f->tickets_a, g_array_unref);
	g_clear_pointer(&f->tickets_b, g_array_unref);
	g_clear_pointer(&f->admin_secret, g_free);
	g_clear_pointer(&f->viewer_secret, g_free);
	g_clear_pointer(&f->pinned_secret, g_free);
	g_clear_object(&f->context);
	g_clear_object(&f->db);
	g_clear_object(&f->config);
	venture_test_remove_tree(f->directory);
	g_clear_pointer(&f->directory, g_free);
}

/* --- HTTP --------------------------------------------------------------- */

typedef struct
{
	GBytes		*bytes;
	GError		*error;
	gboolean	 done;
} HttpResult;

static void
http_done(
	GObject		*source,
	GAsyncResult	*result,
	gpointer	 data
){
	HttpResult *r = data;

	r->bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &r->error);
	r->done = TRUE;
}

/*
 * One request with @secret as the bearer, driving the server's loop while
 * it is answered. The body as text; the status and content type out.
 */
static gchar *
http_send(
	Fixture		 *f,
	const gchar	 *method,
	const gchar	 *path,
	const gchar	 *secret,
	const gchar	 *body,
	guint		 *out_status,
	gchar		**out_content_type
){
	g_autofree gchar *base = g_strdup(venture_web_server_get_base_url(f->server));
	g_autofree gchar *url = NULL;
	g_autofree gchar *bearer = g_strconcat("Bearer ", secret, NULL);
	g_autoptr(SoupMessage) message = NULL;
	HttpResult result = { NULL, NULL, FALSE };
	gchar *text;

	if (g_str_has_suffix(base, "/"))
		base[strlen(base) - 1] = '\0';

	url = g_strconcat(base, path, NULL);
	message = soup_message_new(method, url);
	soup_message_headers_replace(soup_message_get_request_headers(message), "Authorization", bearer);
	soup_message_headers_replace(soup_message_get_request_headers(message), "Accept",
	                             "application/json, text/event-stream");

	if (NULL != body)
	{
		g_autoptr(GBytes) bytes = g_bytes_new(body, strlen(body));

		soup_message_set_request_body_from_bytes(message, "application/json", bytes);
	}

	soup_session_send_and_read_async(f->session, message, G_PRIORITY_DEFAULT, NULL, http_done, &result);

	while (!result.done)
		g_main_context_iteration(NULL, TRUE);

	g_assert_no_error(result.error);
	*out_status = soup_message_get_status(message);

	if (NULL != out_content_type)
		*out_content_type = g_strdup(soup_message_headers_get_content_type(
			soup_message_get_response_headers(message), NULL));

	text = g_strndup(g_bytes_get_data(result.bytes, NULL), g_bytes_get_size(result.bytes));
	g_bytes_unref(result.bytes);

	return text;
}

/*
 * POST /api/v1/query {"query": @text}, as @secret, scoped to @organization
 * (0: the request's own, which for a token is the default business); the
 * parsed answer.
 */
static JsonNode *
api_query_in(
	Fixture		*f,
	const gchar	*secret,
	gint64		 organization,
	const gchar	*text,
	guint		*out_status
){
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(JsonNode) request = NULL;
	g_autofree gchar *body = NULL;
	g_autofree gchar *answer = NULL;

	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "query");
	json_builder_add_string_value(builder, text);
	json_builder_end_object(builder);
	request = json_builder_get_root(builder);
	body = venture_json_to_string(request, FALSE);
	{
		g_autofree gchar *path = (organization > 0)
			? g_strdup_printf("/api/v1/query?organization_id=%" G_GINT64_FORMAT, organization)
			: g_strdup("/api/v1/query");

		answer = http_send(f, "POST", path, secret, body, out_status, NULL);
	}

	return venture_json_parse(answer, NULL);
}

/* api_query_in() with the request's own scope. */
static JsonNode *
api_query(
	Fixture		*f,
	const gchar	*secret,
	const gchar	*text,
	guint		*out_status
){
	return api_query_in(f, secret, 0, text, out_status);
}

/* How many times @needle is in @haystack. */
static guint
count_substring(
	const gchar	*haystack,
	const gchar	*needle
){
	const gchar *cursor = haystack;
	guint n = 0;

	while (NULL != (cursor = strstr(cursor, needle)))
	{
		n++;
		cursor += strlen(needle);
	}

	return n;
}

/* GET @path as @secret; the parsed answer. */
static JsonNode *
api_get(
	Fixture		*f,
	const gchar	*secret,
	const gchar	*path,
	guint		*out_status
){
	g_autofree gchar *answer = http_send(f, "GET", path, secret, NULL, out_status, NULL);

	return venture_json_parse(answer, NULL);
}

/* The rows of a venture.query/1 document as compact JSON. */
static gchar *
rows_text(JsonNode *document)
{
	JsonNode *rows;

	g_assert_true(JSON_NODE_HOLDS_OBJECT(document));
	g_assert_cmpstr(json_object_get_string_member(json_node_get_object(document), "schema"), ==,
	                "venture.query/1");
	rows = json_object_get_member(json_node_get_object(document), "rows");
	g_assert_nonnull(rows);

	return venture_json_to_string(rows, FALSE);
}

/* GCompareFunc over gint64. */
static gint
compare_int64(
	gconstpointer	a,
	gconstpointer	b
){
	gint64 x = *(const gint64 *)a;
	gint64 y = *(const gint64 *)b;

	return (x > y) - (x < y);
}

/* The "id" of every object in @array, sorted, as "1,2,3". */
static gchar *
sorted_ids(JsonArray *array)
{
	g_autoptr(GArray) ids = g_array_new(FALSE, FALSE, sizeof(gint64));
	g_autoptr(GString) out = g_string_new(NULL);
	guint i;

	for (i = 0; i < json_array_get_length(array); i++)
	{
		gint64 id = json_object_get_int_member(json_array_get_object_element(array, i), "id");

		g_array_append_val(ids, id);
	}

	g_array_sort(ids, compare_int64);

	for (i = 0; i < ids->len; i++)
		g_string_append_printf(out, "%s%" G_GINT64_FORMAT, (i > 0) ? "," : "",
		                       g_array_index(ids, gint64, i));

	return g_string_free(g_steal_pointer(&out), FALSE);
}

/* Whether @ids (sorted "1,2,3") holds any id of @set. */
static gboolean
ids_contain_any(
	const gchar	*ids,
	GArray		*set
){
	g_auto(GStrv) parts = g_strsplit(ids, ",", -1);
	guint i;
	guint j;

	for (i = 0; NULL != parts[i]; i++)
		for (j = 0; j < set->len; j++)
			if (g_ascii_strtoll(parts[i], NULL, 10) == g_array_index(set, gint64, j))
				return TRUE;

	return FALSE;
}

/* --- The document ---------------------------------------------------------- */

/*
 * The answer is venture.query/1: the columns with their LQL types, one
 * object per row in column order, the count, no plan; explain gives the
 * plan and no rows; the request document is strict.
 */
static void
test_document(
	Fixture		*f,
	gconstpointer	 data
){
	g_autoptr(JsonNode) answer = NULL;
	g_autoptr(JsonNode) plan = NULL;
	g_autofree gchar *refused = NULL;
	JsonObject *root;
	JsonArray *columns;
	JsonArray *rows;
	guint status = 0;

	(void)data;

	answer = api_query(f, f->admin_secret, QUERY_OPEN_BY_ASSIGNEE, &status);
	g_assert_cmpuint(status, ==, 200);
	root = json_node_get_object(answer);
	g_assert_cmpstr(json_object_get_string_member(root, "schema"), ==, "venture.query/1");
	g_assert_cmpstr(json_object_get_string_member(root, "query"), ==, QUERY_OPEN_BY_ASSIGNEE);
	g_assert_true(json_object_get_null_member(root, "explain"));

	columns = json_object_get_array_member(root, "columns");
	g_assert_cmpuint(json_array_get_length(columns), ==, 2);
	g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(columns, 0), "name"), ==,
	                "assignee");
	g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(columns, 1), "name"), ==,
	                "n");
	g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(columns, 1), "type"), ==,
	                "Int");

	/* A token reads the default business: open work is ana's, vee's and
	 * nobody's triage; business B's (bo's) only when asked for. */
	rows = json_object_get_array_member(root, "rows");
	g_assert_cmpint(json_object_get_int_member(root, "row_count"), ==, json_array_get_length(rows));
	g_assert_cmpuint(json_array_get_length(rows), ==, 3);
	{
		g_autoptr(JsonNode) in_b = api_query_in(f, f->admin_secret, f->org_b,
		                                        QUERY_OPEN_BY_ASSIGNEE, &status);
		JsonArray *b_rows = json_object_get_array_member(json_node_get_object(in_b), "rows");

		g_assert_cmpuint(status, ==, 200);
		g_assert_cmpuint(json_array_get_length(b_rows), ==, 1);
		g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(b_rows, 0),
		                                              "assignee"), ==, "bo");
	}

	plan = NULL;
	{
		g_autofree gchar *body = NULL;
		g_autofree gchar *text = NULL;

		body = g_strdup("{\"query\": \"fetch ticket | filter status == \\\"todo\\\"\", \"explain\": true}");
		text = http_send(f, "POST", "/api/v1/query", f->admin_secret, body, &status, NULL);
		g_assert_cmpuint(status, ==, 200);
		plan = venture_json_parse(text, NULL);
		g_assert_nonnull(json_object_get_string_member(json_node_get_object(plan), "explain"));
		g_assert_cmpuint(json_array_get_length(json_object_get_array_member(
			json_node_get_object(plan), "rows")), ==, 0);
	}

	/* A member nobody reads is refused, not ignored; so is no query. */
	refused = http_send(f, "POST", "/api/v1/query", f->admin_secret,
	                    "{\"query\": \"fetch ticket\", \"limit\": 5}", &status, NULL);
	g_assert_cmpuint(status, ==, 400);
	g_assert_nonnull(strstr(refused, "no member"));
	g_clear_pointer(&refused, g_free);
	refused = http_send(f, "POST", "/api/v1/query", f->admin_secret, "{}", &status, NULL);
	g_assert_cmpuint(status, ==, 400);
	g_clear_pointer(&refused, g_free);

	/* A query that does not parse says where. */
	refused = http_send(f, "POST", "/api/v1/query", f->admin_secret,
	                    "{\"query\": \"fetch ticket | filter (\"}", &status, NULL);
	g_assert_cmpuint(status, ==, 400);
	{
		g_autoptr(JsonNode) node = venture_json_parse(refused, NULL);
		JsonObject *error = json_node_get_object(node);

		g_assert_cmpstr(json_object_get_string_member(error, "error"), ==, "invalid_argument");
		g_assert_cmpint(json_object_get_int_member(error, "line"), ==, 1);
		g_assert_nonnull(json_object_get_string_member(error, "detail"));
	}
}

/* --- Access --------------------------------------------------------------- */

/*
 * A viewer of business A queries as themselves: `fetch user` (owner-only)
 * gets GET /api/v1/user's own refusal, also from inside a lookup; their
 * tickets are their list's, and none of business B's, even asked for by
 * organization_id.
 */
static void
test_access_member(
	Fixture		*f,
	gconstpointer	 data
){
	g_autoptr(JsonNode) listed_users = NULL;
	g_autoptr(JsonNode) users = NULL;
	g_autoptr(JsonNode) looked_up = NULL;
	g_autoptr(JsonNode) listed = NULL;
	g_autoptr(JsonNode) fetched = NULL;
	g_autoptr(JsonNode) other = NULL;
	g_autofree gchar *listed_ids = NULL;
	g_autofree gchar *fetched_ids = NULL;
	g_autofree gchar *by_organization = NULL;
	guint status = 0;

	(void)data;

	/* The type: the list API's status and message, word for word. */
	listed_users = api_get(f, f->viewer_secret, "/api/v1/user", &status);
	g_assert_cmpuint(status, ==, 403);
	users = api_query(f, f->viewer_secret, "fetch user | limit 1", &status);
	g_assert_cmpuint(status, ==, 403);
	g_assert_cmpstr(json_object_get_string_member(json_node_get_object(users), "error"), ==,
	                json_object_get_string_member(json_node_get_object(listed_users), "error"));
	g_assert_cmpstr(json_object_get_string_member(json_node_get_object(users), "message"), ==,
	                json_object_get_string_member(json_node_get_object(listed_users), "message"));

	looked_up = api_query(f, f->viewer_secret,
		"fetch ticket | lookup (fetch user), on: assignee == username, prefix: \"u_\"", &status);
	g_assert_cmpuint(status, ==, 403);

	/* The rows: GET /api/v1/ticket's, and nothing of business B. */
	listed = api_get(f, f->viewer_secret, "/api/v1/ticket", &status);
	g_assert_cmpuint(status, ==, 200);
	fetched = api_query(f, f->viewer_secret, "fetch ticket | fields id, title", &status);
	g_assert_cmpuint(status, ==, 200);
	listed_ids = sorted_ids(json_object_get_array_member(json_node_get_object(listed), "records"));
	fetched_ids = sorted_ids(json_object_get_array_member(json_node_get_object(fetched), "rows"));
	g_test_message("list API: %s; fetch ticket: %s", listed_ids, fetched_ids);
	g_assert_cmpstr(listed_ids, ==, fetched_ids);
	g_assert_cmpstr(fetched_ids, !=, "");
	g_assert_false(ids_contain_any(fetched_ids, f->tickets_b));

	by_organization = g_strdup_printf("fetch ticket | filter organization_id == %" G_GINT64_FORMAT,
	                                  f->org_b);
	other = api_query(f, f->viewer_secret, by_organization, &status);
	g_assert_cmpuint(status, ==, 200);
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(
		json_node_get_object(other), "rows")), ==, 0);
}

/*
 * A token minted for business A sees only A, though its user edits in B
 * too, exactly as its list does; the administrator's token sees both.
 */
static void
test_access_token(
	Fixture		*f,
	gconstpointer	 data
){
	g_autoptr(JsonNode) listed = NULL;
	g_autoptr(JsonNode) pinned = NULL;
	g_autoptr(JsonNode) everything = NULL;
	g_autofree gchar *listed_ids = NULL;
	g_autofree gchar *pinned_ids = NULL;
	g_autofree gchar *all_ids = NULL;
	guint status = 0;

	(void)data;

	listed = api_get(f, f->pinned_secret, "/api/v1/ticket", &status);
	g_assert_cmpuint(status, ==, 200);
	pinned = api_query(f, f->pinned_secret, "fetch ticket | fields id", &status);
	g_assert_cmpuint(status, ==, 200);
	listed_ids = sorted_ids(json_object_get_array_member(json_node_get_object(listed), "records"));
	pinned_ids = sorted_ids(json_object_get_array_member(json_node_get_object(pinned), "rows"));
	g_test_message("token list: %s; token query: %s", listed_ids, pinned_ids);
	g_assert_cmpstr(listed_ids, ==, pinned_ids);
	g_assert_true(ids_contain_any(pinned_ids, f->tickets_a));
	g_assert_false(ids_contain_any(pinned_ids, f->tickets_b));

	/* Asked for business B by name, or scoped to it, the pinned token
	 * still reads nothing there; the administrator does. */
	{
		g_autofree gchar *in_b = g_strdup_printf("fetch ticket | filter organization_id == %"
		                                         G_GINT64_FORMAT " | fields id", f->org_b);
		g_autoptr(JsonNode) named = api_query(f, f->pinned_secret, in_b, &status);
		g_autoptr(JsonNode) scoped = api_query_in(f, f->pinned_secret, f->org_b,
		                                          "fetch ticket | fields id", &status);

		g_assert_cmpuint(json_array_get_length(json_object_get_array_member(
			json_node_get_object(named), "rows")), ==, 0);
		g_assert_cmpuint(json_array_get_length(json_object_get_array_member(
			json_node_get_object(scoped), "rows")), ==, 0);

		everything = api_query(f, f->admin_secret, in_b, &status);
		g_assert_cmpuint(status, ==, 200);
	}

	all_ids = sorted_ids(json_object_get_array_member(json_node_get_object(everything), "rows"));
	g_assert_true(ids_contain_any(all_ids, f->tickets_b));
}

/* --- Bounds ---------------------------------------------------------------- */

/*
 * Over query.max_rows the answer is a 422 that names the bound, never a
 * shorter list; | limit stays under it.
 */
static void
test_max_rows(
	Fixture		*f,
	gconstpointer	 data
){
	g_autoptr(JsonNode) refused = NULL;
	g_autoptr(JsonNode) limited = NULL;
	guint status = 0;

	(void)data;

	g_object_set(f->config, "query-max-rows", (gint64)3, NULL);

	refused = api_query(f, f->admin_secret, "fetch ticket", &status);
	g_assert_cmpuint(status, ==, 422);
	g_assert_cmpstr(json_object_get_string_member(json_node_get_object(refused), "error"), ==,
	                "validation");
	g_assert_nonnull(strstr(json_object_get_string_member(json_node_get_object(refused), "message"),
	                        "query.max_rows"));
	g_assert_false(json_object_has_member(json_node_get_object(refused), "rows"));

	limited = api_query(f, f->admin_secret, "fetch ticket | limit 2", &status);
	g_assert_cmpuint(status, ==, 200);
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(
		json_node_get_object(limited), "rows")), ==, 2);
}

/* nap() -> Int: sleeps 5 ms, so a query over a few rows outlasts 20 ms. */
static LqlValue *
nap(
	LqlFunctionCall	 *call,
	gpointer	  user_data,
	GError		**error
){
	(void)call;
	(void)user_data;
	(void)error;

	g_usleep(5000);

	return lql_value_new_int(1);
}

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

/*
 * Past query.timeout_ms the runner answers a timeout (504) that names the
 * setting, not a half-finished table.
 */
static void
test_timeout(
	Fixture		*f,
	gconstpointer	 data
){
	g_autoptr(LqlEnvironment) environment = NULL;
	g_autoptr(LqlFunction) slow = NULL;
	g_autoptr(JsonNode) document = NULL;
	g_autoptr(GError) error = NULL;
	VentureAuthPrincipal admin;

	(void)data;

	memset(&admin, 0, sizeof admin);
	admin.user_id = f->admin_id;
	admin.role = VENTURE_USER_ROLE_ADMIN;
	admin.authenticated = TRUE;

	g_object_set(f->config, "query-timeout-ms", (gint64)20, NULL);
	environment = venture_lql_environment_new(f->context, &admin, NULL, 0,
		allow_every_type, NULL, &error);
	g_assert_no_error(error);

	slow = lql_function_new_scalar("nap", LQL_VALUE_TYPE_INT, nap, NULL, NULL);
	lql_function_set_flags(slow, LQL_FUNCTION_FLAG_VOLATILE);
	g_assert_true(lql_environment_register_function(environment, slow, &error));

	document = venture_lql_query_run(environment, "fetch ticket | fields id, z = nap() | sort id",
	                                 FALSE, NULL, &error);
	g_assert_null(document);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_TIMEOUT);
	g_assert_nonnull(strstr(error->message, "query.timeout_ms"));
	g_assert_cmpuint(venture_error_to_http_status(VENTURE_ERROR_TIMEOUT), ==, 504);
}

/* --- query.max_bytes (VENTURE #44) ----------------------------------------- */

/*
 * The memory budget is a setting now, read where the environment is built
 * and refused outside its range, though LQL does not enforce it yet: the
 * value reaches the one place it is to be handed to LQL.
 */
static void
test_max_bytes(void)
{
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureDatabase) db = NULL;
	g_autoptr(VentureContext) context = NULL;
	g_autoptr(LqlEnvironment) environment = NULL;
	g_autoptr(GError) error = NULL;
	VentureAuthPrincipal admin;
	gint64 max_bytes = 0;

	/* The default, documented in data/default-config.yaml. */
	g_object_get(config, "query-max-bytes", &max_bytes, NULL);
	g_assert_cmpint(max_bytes, ==, VENTURE_LQL_DEFAULT_MAX_BYTES);

	/* It cannot be switched off, nor set past a gibibyte. */
	g_object_set(config, "query-max-bytes", (gint64)0, NULL);
	g_assert_false(venture_config_validate(config, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
	g_assert_nonnull(strstr(error->message, "query.max_bytes"));
	g_clear_error(&error);
	g_object_set(config, "query-max-bytes", (gint64)2 * 1073741824, NULL);
	g_assert_false(venture_config_validate(config, &error));
	g_assert_nonnull(strstr(error->message, "query.max_bytes"));
	g_clear_error(&error);

	/* A value in range reaches the environment. */
	g_object_set(config, "query-max-bytes", (gint64)2097152, NULL);
	g_assert_true(venture_config_validate(config, &error));
	g_assert_no_error(error);

	db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	context = venture_context_new(config, db);

	memset(&admin, 0, sizeof admin);
	admin.user_id = 0;
	admin.role = VENTURE_USER_ROLE_ADMIN;
	admin.authenticated = TRUE;
	environment = venture_lql_environment_new(context, &admin, NULL, 0,
		allow_every_type, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpint(venture_lql_environment_get_max_bytes(environment), ==, 2097152);
}

/* --- Where a query runs ------------------------------------------------------ */

/*
 * A query that takes a while row by row: each of business A's five
 * tickets expanded into 1601 rows (8005, under query.max_rows), each
 * napping. LQL checks its cancellable and its deadline every 4096
 * evaluation steps and between stages, so it takes thousands of rows --
 * not a few slow ones -- for a stop to land part-way through a stage.
 */
#define QUERY_MANY_NAPS \
	"fetch ticket | fields id, xs = split(repeat(\"x,\", 1600), \",\") " \
	"| expand xs | fields id, z = nap()"

/* How long nap() sleeps, on whichever thread evaluates the query. */
static gulong nap_microseconds = 5000;

/* nap(), slower: the time it takes is nap_microseconds. */
static LqlValue *
long_nap(
	LqlFunctionCall	 *call,
	gpointer	  user_data,
	GError		**error
){
	(void)call;
	(void)user_data;
	(void)error;

	g_usleep(nap_microseconds);

	return lql_value_new_int(1);
}

/* A #VentureLqlEnvironmentHook: nap() in every environment the server builds. */
static gboolean
add_long_nap(
	LqlEnvironment	 *environment,
	gpointer	  user_data,
	GError		**error
){
	g_autoptr(LqlFunction) slow = NULL;

	(void)user_data;

	slow = lql_function_new_scalar("nap", LQL_VALUE_TYPE_INT, long_nap, NULL, NULL);
	lql_function_set_flags(slow, LQL_FUNCTION_FLAG_VOLATILE);

	return lql_environment_register_function(environment, slow, error);
}

/* One POST /api/v1/query in flight. */
typedef struct
{
	SoupMessage	*message;
	HttpResult	 result;
} Pending;

/* Starts POST /api/v1/query {"query": @text} as @secret, without waiting. */
static void
pending_query(
	Fixture		*f,
	Pending		*p,
	const gchar	*secret,
	const gchar	*text
){
	g_autofree gchar *base = g_strdup(venture_web_server_get_base_url(f->server));
	g_autofree gchar *url = NULL;
	g_autofree gchar *bearer = g_strconcat("Bearer ", secret, NULL);
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(JsonNode) request = NULL;
	g_autoptr(GBytes) bytes = NULL;
	g_autofree gchar *body = NULL;

	if (g_str_has_suffix(base, "/"))
		base[strlen(base) - 1] = '\0';

	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "query");
	json_builder_add_string_value(builder, text);
	json_builder_end_object(builder);
	request = json_builder_get_root(builder);
	body = venture_json_to_string(request, FALSE);
	bytes = g_bytes_new(body, strlen(body));

	memset(p, 0, sizeof *p);
	url = g_strconcat(base, "/api/v1/query", NULL);
	p->message = soup_message_new("POST", url);
	soup_message_headers_replace(soup_message_get_request_headers(p->message),
	                             "Authorization", bearer);
	soup_message_set_request_body_from_bytes(p->message, "application/json", bytes);
	soup_session_send_and_read_async(f->session, p->message, G_PRIORITY_DEFAULT, NULL,
	                                 http_done, &p->result);
}

/* Waits for @p; its status. */
static guint
pending_finish(Pending *p)
{
	guint status;

	while (!p->result.done)
		g_main_context_iteration(NULL, TRUE);

	g_assert_no_error(p->result.error);
	status = soup_message_get_status(p->message);
	g_clear_pointer(&p->result.bytes, g_bytes_unref);
	g_clear_object(&p->message);

	return status;
}

/*
 * While one query is evaluated -- half a second of nap() -- the server
 * goes on answering: a second query and a record list are both answered
 * before the first is. Evaluated on the main loop, as it was before the
 * query workers, neither could be.
 */
static void
test_main_loop_responsive(
	Fixture		*f,
	gconstpointer	 data
){
	Pending slow;
	Pending quick;
	g_autoptr(JsonNode) listed = NULL;
	gint64 until;
	guint status = 0;

	(void)data;

	nap_microseconds = 100000;
	venture_lql_set_environment_hook(add_long_nap, NULL);
	g_object_set(f->config, "query-timeout-ms", (gint64)60000, NULL);

	/* Five tickets of business A, a tenth of a second each. */
	pending_query(f, &slow, f->admin_secret, "fetch ticket | fields id, z = nap() | sort id");

	/* Long enough for it to be on a worker. */
	until = g_get_monotonic_time() + 100 * G_TIME_SPAN_MILLISECOND;

	while (g_get_monotonic_time() < until)
		g_main_context_iteration(NULL, FALSE);

	g_assert_false(slow.result.done);

	pending_query(f, &quick, f->admin_secret, "fetch ticket | summarize n = count()");
	g_assert_cmpuint(pending_finish(&quick), ==, 200);
	listed = api_get(f, f->admin_secret, "/api/v1/tickets?limit=1", &status);
	g_assert_cmpuint(status, ==, 200);

	/* The slow query is still on its worker. */
	g_assert_false(slow.result.done);
	g_assert_cmpuint(pending_finish(&slow), ==, 200);

	venture_lql_set_environment_hook(NULL, NULL);
}

/*
 * A query past query.timeout_ms is stopped part-way through, not run to
 * the end: over a second of nap() (QUERY_MANY_NAPS) under a quarter-second
 * limit answers 504 long before the second is up.
 */
static void
test_deadline_stops_the_query(
	Fixture		*f,
	gconstpointer	 data
){
	g_autoptr(JsonNode) answer = NULL;
	gint64 started;
	gint64 elapsed_ms;
	guint status = 0;

	(void)data;

	nap_microseconds = 150;
	venture_lql_set_environment_hook(add_long_nap, NULL);
	g_object_set(f->config, "query-timeout-ms", (gint64)250, NULL);

	started = g_get_monotonic_time();
	answer = api_query(f, f->admin_secret, QUERY_MANY_NAPS, &status);
	elapsed_ms = (g_get_monotonic_time() - started) / 1000;
	g_test_message("answered %u after %" G_GINT64_FORMAT " ms", status, elapsed_ms);

	g_assert_cmpuint(status, ==, 504);
	g_assert_nonnull(strstr(json_object_get_string_member(json_node_get_object(answer), "message"),
	                        "query.timeout_ms"));
	g_assert_cmpint(elapsed_ms, <, 800);

	venture_lql_set_environment_hook(NULL, NULL);
}

/* A worker of the test's own: the query, its fetches queued for main. */
typedef struct
{
	VentureLqlPrepared	*prepared;
	GCancellable		*cancellable;
	GAsyncQueue		*reads;		/* VentureLqlRead */
	JsonNode		*document;
	GError			*error;
	gint			 finished;	/* atomic */
} CancelJob;

/* A #VentureLqlReadDispatch: the fetch goes to the main thread's queue. */
static void
cancel_job_dispatch(
	VentureLqlRead	*read,
	gpointer	 user_data
){
	CancelJob *job = user_data;

	g_async_queue_push(job->reads, read);
	g_main_context_wakeup(NULL);
}

static gpointer
cancel_job_run(gpointer data)
{
	CancelJob *job = data;

	venture_lql_set_read_dispatch(cancel_job_dispatch, job);
	job->document = venture_lql_query_execute(job->prepared, job->cancellable, &job->error);
	venture_lql_set_read_dispatch(NULL, NULL);
	g_atomic_int_set(&job->finished, 1);
	g_main_context_wakeup(NULL);

	return NULL;
}

/*
 * The GCancellable reaches into the evaluation: a query on another thread
 * is cancelled part-way through its rows (QUERY_MANY_NAPS, over a second
 * of naps) and stops there, with the time limit's error, long before its
 * naps would have ended. And a fetch that
 * was handed back to be read, but not yet read when the query was
 * cancelled, is never read: no statement reaches the database for it.
 */
static void
test_cancel_mid_query(
	Fixture		*f,
	gconstpointer	 data
){
	g_autoptr(LqlEnvironment) environment = NULL;
	g_autoptr(GError) error = NULL;
	VentureAuthPrincipal admin;
	CancelJob job;
	GThread *thread;
	gint64 started;
	gint64 elapsed_ms;
	gboolean read_one = FALSE;

	(void)data;

	memset(&admin, 0, sizeof admin);
	admin.user_id = f->admin_id;
	admin.role = VENTURE_USER_ROLE_ADMIN;
	admin.authenticated = TRUE;

	g_object_set(f->config, "query-timeout-ms", (gint64)60000, NULL);
	nap_microseconds = 150;
	/* Business A's five tickets, as the API's own scope would read them. */
	environment = venture_lql_environment_new(f->context, &admin, &f->org, 1,
		allow_every_type, NULL, &error);
	g_assert_no_error(error);
	g_assert_true(add_long_nap(environment, NULL, &error));
	g_assert_no_error(error);

	/* --- cancelled during the evaluation ------------------------------ */
	memset(&job, 0, sizeof job);
	job.prepared = venture_lql_query_prepare(environment, QUERY_MANY_NAPS, FALSE, &error);
	g_assert_no_error(error);
	job.cancellable = g_cancellable_new();
	job.reads = g_async_queue_new();

	started = g_get_monotonic_time();
	thread = g_thread_new("test-query", cancel_job_run, &job);

	while (!g_atomic_int_get(&job.finished))
	{
		VentureLqlRead *read = g_async_queue_try_pop(job.reads);

		/* The fetch, read here, then the naps begin: cancel during them. */
		if (NULL != read)
		{
			venture_lql_read_perform(read);
			read_one = TRUE;
			g_usleep(150000);
			g_cancellable_cancel(job.cancellable);
			continue;
		}

		g_main_context_iteration(NULL, FALSE);
		g_usleep(1000);
	}

	g_thread_join(thread);
	elapsed_ms = (g_get_monotonic_time() - started) / 1000;
	g_test_message("cancelled query answered after %" G_GINT64_FORMAT " ms", elapsed_ms);

	g_assert_true(read_one);
	g_assert_null(job.document);
	g_assert_error(job.error, VENTURE_ERROR, VENTURE_ERROR_TIMEOUT);
	g_assert_cmpint(elapsed_ms, <, 800);
	g_clear_error(&job.error);
	g_clear_pointer(&job.prepared, venture_lql_prepared_free);
	g_clear_object(&job.cancellable);

	/* --- cancelled with its fetch waiting to be read ------------------ */
	memset(&job.finished, 0, sizeof job.finished);
	job.prepared = venture_lql_query_prepare(environment, "fetch ticket | fields id",
	                                         FALSE, &error);
	g_assert_no_error(error);
	job.cancellable = g_cancellable_new();
	thread = g_thread_new("test-query", cancel_job_run, &job);

	{
		VentureLqlRead *read;
		guint64 statements;

		read = g_async_queue_timeout_pop(job.reads, 5 * G_TIME_SPAN_SECOND);
		g_assert_nonnull(read);
		g_cancellable_cancel(job.cancellable);
		statements = venture_database_get_statement_count(f->db);
		venture_lql_read_perform(read);
		g_assert_cmpuint(venture_database_get_statement_count(f->db), ==, statements);
	}

	g_thread_join(thread);
	g_assert_null(job.document);
	g_assert_error(job.error, VENTURE_ERROR, VENTURE_ERROR_TIMEOUT);
	g_clear_error(&job.error);
	g_clear_pointer(&job.prepared, venture_lql_prepared_free);
	g_clear_object(&job.cancellable);
	g_async_queue_unref(job.reads);
}

/* --- CSV -------------------------------------------------------------------- */

/*
 * format: "csv" answers text/csv: a header of the columns, one CRLF line
 * per row, a formula defused; ?format=csv the same.
 */
static void
test_csv(
	Fixture		*f,
	gconstpointer	 data
){
	g_autofree gchar *csv = NULL;
	g_autofree gchar *again = NULL;
	g_autofree gchar *content_type = NULL;
	g_auto(GStrv) lines = NULL;
	guint status = 0;

	(void)data;

	csv = http_send(f, "POST", "/api/v1/query", f->admin_secret,
		"{\"query\": \"fetch ticket | fields title, assignee | sort title\", "
		"\"format\": \"csv\"}",
		&status, &content_type);
	g_assert_cmpuint(status, ==, 200);
	g_assert_cmpstr(content_type, ==, "text/csv");
	g_test_message("%s", csv);

	lines = g_strsplit(csv, "\r\n", -1);
	g_assert_cmpstr(lines[0], ==, "title,assignee");
	/* Business A's five tickets, then the empty string after the last CRLF. */
	g_assert_cmpuint(g_strv_length(lines), ==, 1 + 5 + 1);
	g_assert_cmpstr(lines[6], ==, "");
	g_assert_cmpstr(lines[1], ==, "\"'=HYPERLINK(\"\"http://evil.test\"\")\",");
	g_assert_cmpstr(lines[2], ==, "A1,vee");

	again = http_send(f, "POST", "/api/v1/query?format=csv", f->admin_secret,
		"{\"query\": \"fetch ticket | fields title, assignee | sort title\"}",
		&status, NULL);
	g_assert_cmpuint(status, ==, 200);
	g_assert_cmpstr(again, ==, csv);
}

/* --- Money ------------------------------------------------------------------ */

/*
 * sum_money() keeps currencies apart: business B's sent invoices total
 * 1500.00 USD and 300.00 EUR, two figures, in the rows, the CSV, the
 * table and the bars; no figure anywhere is the 1800 they are not.
 */
static void
test_per_currency(
	Fixture		*f,
	gconstpointer	 data
){
	g_autoptr(JsonNode) answer = NULL;
	g_autoptr(JsonNode) drawn = NULL;
	g_autoptr(JsonNode) refused = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *csv = NULL;
	g_autofree gchar *table = NULL;
	g_autofree gchar *bars = NULL;
	g_autofree gchar *body = NULL;
	g_autofree gchar *everything = NULL;
	JsonArray *rows;
	JsonObject *sent = NULL;
	JsonArray *totals;
	JsonArray *groups;
	guint status = 0;
	guint i;

	(void)data;

	answer = api_query_in(f, f->admin_secret, f->org_b, QUERY_INVOICES_BY_STATUS, &status);
	g_assert_cmpuint(status, ==, 200);
	rows = json_object_get_array_member(json_node_get_object(answer), "rows");

	/* The draft was never issued: no issued_at, so not this quarter's. */
	g_assert_cmpuint(json_array_get_length(rows), ==, 1);

	for (i = 0; i < json_array_get_length(rows); i++)
	{
		JsonObject *row = json_array_get_object_element(rows, i);

		if (0 == g_strcmp0(json_object_get_string_member(row, "status"), "sent"))
			sent = row;
	}

	g_assert_nonnull(sent);
	g_assert_cmpint(json_object_get_int_member(sent, "invoices"), ==, 2);
	totals = json_object_get_array_member(sent, "total");
	g_assert_cmpuint(json_array_get_length(totals), ==, 2);
	g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(totals, 0), "currency"),
	                ==, "EUR");
	g_assert_cmpint(json_object_get_int_member(json_array_get_object_element(totals, 0), "amount"),
	                ==, 30000);
	g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(totals, 1), "currency"),
	                ==, "USD");
	g_assert_cmpint(json_object_get_int_member(json_array_get_object_element(totals, 1), "amount"),
	                ==, 150000);

	everything = venture_json_to_string(answer, FALSE);
	g_assert_null(strstr(everything, "180000"));

	/* CSV: each total, joined, never added. */
	body = g_strdup_printf("{\"query\": \"%s\", \"format\": \"csv\"}", "fetch invoice_line "
		"| lookup (fetch invoice), on: invoice_id == id, prefix: \\\"invoice_\\\" "
		"| filter invoice_status == \\\"sent\\\" "
		"| summarize total = sum_money(income_amount)");
	{
		g_autofree gchar *path = g_strdup_printf("/api/v1/query?organization_id=%" G_GINT64_FORMAT,
		                                         f->org_b);

		csv = http_send(f, "POST", path, f->admin_secret, body, &status, NULL);
	}
	g_assert_cmpuint(status, ==, 200);
	g_assert_cmpstr(csv, ==, "total\r\n300.00 EUR; 1500.00 USD\r\n");

	/* The table: a line per currency in the one cell. */
	table = venture_web_query_chart(answer, "table", NULL, NULL, NULL, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(strstr(table, "data-currency=\"USD\""));
	g_assert_nonnull(strstr(table, "data-currency=\"EUR\""));
	g_assert_nonnull(strstr(table, "$1,500.00"));
	g_assert_null(strstr(table, "1,800"));

	/* Bars: one chart per currency, each its own series. */
	bars = venture_web_query_chart(answer, "bar", "status", NULL, "total", &drawn, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(count_substring(bars, "<figure class=\"chart chart-bar\">"), ==, 2);
	g_assert_nonnull(strstr(bars, "total by status \xe2\x80\x94 EUR"));
	g_assert_nonnull(strstr(bars, "total by status \xe2\x80\x94 USD"));
	groups = json_object_get_array_member(json_node_get_object(drawn), "groups");
	g_assert_cmpuint(json_array_get_length(groups), ==, 2);

	for (i = 0; i < json_array_get_length(groups); i++)
		g_assert_nonnull(json_object_get_string_member(json_array_get_object_element(groups, i),
		                                               "currency"));

	/* sum_money() of a number is refused rather than read as money. */
	refused = api_query(f, f->admin_secret, "fetch ticket | summarize s = sum_money(id)", &status);
	g_assert_cmpuint(status, ==, 400);
	g_assert_nonnull(strstr(json_object_get_string_member(json_node_get_object(refused), "message"),
	                        "sum_money()"));
}

/* --- The widget ------------------------------------------------------------- */

static VentureDashboard *
widget_dashboard(Fixture *f)
{
	g_autoptr(VentureEntity) dashboard = NULL;
	g_autoptr(GError) error = NULL;

	dashboard = VENTURE_ENTITY(venture_dashboard_new());
	g_object_set(dashboard, "name", "Money", "slug", "money", NULL);
	venture_entity_set_organization_id(dashboard, f->org);
	if (!venture_database_save(f->db, dashboard, NULL, &error))
		g_error("saving: %s", error->message);

	return VENTURE_DASHBOARD(g_steal_pointer(&dashboard));
}

static VentureDashboardWidget *
query_widget(
	Fixture			*f,
	VentureDashboard	*dashboard,
	const gchar		*title,
	const gchar		*body,
	const gchar		*options,
	GError		       **error
){
	g_autoptr(VentureDashboardWidget) widget = venture_dashboard_widget_new();

	g_object_set(widget, "dashboard-id", venture_entity_get_id(VENTURE_ENTITY(dashboard)),
	             "kind", "query", "title", title, "body", body, "options", options, NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(widget), f->org);

	if (!venture_database_save(f->db, VENTURE_ENTITY(widget), NULL, error))
		return NULL;

	return g_steal_pointer(&widget);
}

/* The widget titled @title in GET /api/v1/dashboards/:slug's answer. */
static JsonObject *
described_widget(
	JsonNode	*described,
	const gchar	*title
){
	JsonArray *widgets = json_object_get_array_member(json_node_get_object(described), "widgets");
	guint i;

	for (i = 0; i < json_array_get_length(widgets); i++)
	{
		JsonObject *widget = json_array_get_object_element(widgets, i);

		if (0 == g_strcmp0(json_object_get_string_member(widget, "title"), title))
			return widget;
	}

	g_error("no widget titled %s", title);
	return NULL;
}

/*
 * A shared dashboard's query widget runs as whoever looks: the author's
 * administrator token sees business B's totals; tia, whose token reads
 * nothing of B, gets an empty answer, never those figures, and the
 * owner-only type the list API refuses her; with nobody looking (the
 * assistant's scope) the widget refuses.
 */
static void
test_widget_scope(
	Fixture		*f,
	gconstpointer	 data
){
	g_autoptr(VentureDashboard) dashboard = NULL;
	g_autoptr(VentureDashboardWidget) totals = NULL;
	g_autoptr(VentureDashboardWidget) people = NULL;
	g_autoptr(VentureWidgetResult) nobody = NULL;
	g_autoptr(JsonNode) as_admin = NULL;
	g_autoptr(JsonNode) as_viewer = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *viewer_text = NULL;
	JsonObject *widget;
	JsonObject *answer;
	guint status = 0;

	(void)data;

	dashboard = widget_dashboard(f);
	totals = query_widget(f, dashboard, "Invoices", QUERY_INVOICES_BY_STATUS,
	                      "{\"chart\": \"table\"}", &error);
	g_assert_no_error(error);
	people = query_widget(f, dashboard, "Plugins", "fetch plugin_config | fields id",
	                      "{\"chart\": \"table\"}", &error);
	g_assert_no_error(error);

	as_admin = api_get(f, f->admin_secret, "/api/v1/dashboards/money", &status);
	g_assert_cmpuint(status, ==, 200);
	widget = described_widget(as_admin, "Invoices");
	answer = json_object_get_object_member(widget, "data");
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(answer, "rows")), ==, 1);

	/* tia may open the dashboard (she edits in A, where it is filed) but
	 * her token reads nothing of B, where the invoices are. */
	as_viewer = api_get(f, f->pinned_secret, "/api/v1/dashboards/money", &status);
	g_assert_cmpuint(status, ==, 200);
	viewer_text = venture_json_to_string(as_viewer, FALSE);
	g_assert_null(strstr(viewer_text, "150000"));
	g_assert_null(strstr(viewer_text, "30000"));
	g_assert_null(strstr(viewer_text, "\"ada\""));

	/* Invoices: nothing of B, so no rows (or a refusal, never figures). */
	widget = described_widget(as_viewer, "Invoices");

	if (JSON_NODE_HOLDS_OBJECT(json_object_get_member(widget, "data")))
		g_assert_cmpuint(json_array_get_length(json_object_get_array_member(
			json_object_get_object_member(widget, "data"), "rows")), ==, 0);
	else
		g_assert_false(json_object_get_null_member(widget, "error"));

	/* Plugins: an administrator's type, refused to tia as the list API
	 * refuses it, answered for the administrator. */
	{
		g_autoptr(JsonNode) listed = api_get(f, f->pinned_secret, "/api/v1/plugin_config", &status);

		g_assert_cmpuint(status, ==, 403);
		widget = described_widget(as_viewer, "Plugins");
		g_test_message("tia's plugins widget: %s", json_object_get_string_member(widget, "error"));
		g_assert_cmpstr(json_object_get_string_member(widget, "error"), ==,
		                json_object_get_string_member(json_node_get_object(listed), "message"));
	}
	widget = described_widget(as_admin, "Plugins");
	g_assert_true(json_object_get_null_member(widget, "error"));

	/* Nobody looking: no rows as anybody. */
	nobody = venture_dashboard_render_widget(f->context, totals, NULL);
	g_assert_nonnull(nobody->error);
	g_assert_nonnull(strstr(nobody->error, "nobody here to run it as"));
	g_assert_null(nobody->html);
}

/* A test runner: @user_data is an environment, the query runs in it. */
static JsonNode *
run_in(
	const gchar	 *query,
	gpointer	  user_data,
	GError		**error
){
	return venture_lql_query_run(user_data, query, FALSE, NULL, error);
}

/*
 * The acceptance widgets: "open tickets by assignee" as bars, and
 * "invoices this quarter by status" as a table with a figure per
 * currency; and the save refuses a widget that cannot be drawn.
 */
static void
test_widget_acceptance(
	Fixture		*f,
	gconstpointer	 data
){
	g_autoptr(VentureDashboard) dashboard = NULL;
	g_autoptr(VentureDashboardWidget) bars = NULL;
	g_autoptr(VentureDashboardWidget) table = NULL;
	g_autoptr(VentureDashboardWidget) broken = NULL;
	g_autoptr(VentureWidgetResult) drawn_bars = NULL;
	g_autoptr(VentureWidgetResult) drawn_table = NULL;
	g_autoptr(LqlEnvironment) environment = NULL;
	g_autoptr(GError) error = NULL;
	VentureWidgetScope scope;
	VentureAuthPrincipal admin;
	JsonObject *drawn;

	(void)data;

	memset(&admin, 0, sizeof admin);
	admin.user_id = f->admin_id;
	admin.role = VENTURE_USER_ROLE_ADMIN;
	admin.authenticated = TRUE;
	environment = venture_lql_environment_new(f->context, &admin, NULL, 0,
		allow_every_type, NULL, &error);
	g_assert_no_error(error);

	memset(&scope, 0, sizeof scope);
	scope.user_id = f->admin_id;
	scope.username = "ada";
	scope.query = run_in;
	scope.query_data = environment;

	dashboard = widget_dashboard(f);
	bars = query_widget(f, dashboard, "Open tickets by assignee", QUERY_OPEN_BY_ASSIGNEE,
	                    "{\"chart\": \"bar\", \"x\": \"assignee\", \"value\": \"n\"}", &error);
	g_assert_no_error(error);
	table = query_widget(f, dashboard, "Invoices this quarter", QUERY_INVOICES_BY_STATUS,
	                     "{\"chart\": \"table\"}", &error);
	g_assert_no_error(error);

	drawn_bars = venture_dashboard_render_widget(f->context, bars, &scope);
	g_assert_null(drawn_bars->error);
	g_assert_cmpuint(count_substring(drawn_bars->html,
		"<figure class=\"chart chart-bar\">"), ==, 1);
	g_assert_nonnull(strstr(drawn_bars->html, "n by assignee"));
	g_assert_nonnull(strstr(drawn_bars->html, "vee"));
	g_assert_nonnull(strstr(drawn_bars->html, "ana"));
	drawn = json_object_get_object_member(json_node_get_object(drawn_bars->data), "chart");
	g_assert_cmpstr(json_object_get_string_member(drawn, "chart"), ==, "bar");
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(drawn, "labels")), ==, 4);

	drawn_table = venture_dashboard_render_widget(f->context, table, &scope);
	g_assert_null(drawn_table->error);
	g_assert_nonnull(strstr(drawn_table->html, "<table"));
	g_assert_nonnull(strstr(drawn_table->html, "$1,500.00"));
	g_assert_nonnull(strstr(drawn_table->html, "data-currency=\"EUR\""));

	/* The save: a query that parses, options the kind reads. */
	broken = query_widget(f, dashboard, "Broken", "fetch ticket | filter (", NULL, &error);
	g_assert_null(broken);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	broken = query_widget(f, dashboard, "Broken", "fetch ticket", "{\"colour\": \"red\"}", &error);
	g_assert_null(broken);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	broken = query_widget(f, dashboard, "Broken", "fetch ticket", "{\"chart\": \"donut\"}", &error);
	g_assert_null(broken);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
}

/* --- Three surfaces, one answer -------------------------------------------- */

typedef struct
{
	gboolean	 done;
	gchar		*output;
	gchar		*diagnostic;
	GError		*error;
} CliResult;

static void
cli_done(
	GObject		*source,
	GAsyncResult	*result,
	gpointer	 data
){
	CliResult *r = data;

	g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, &r->output,
	                                     &r->diagnostic, &r->error);
	r->done = TRUE;
}

/* `venturectl -f FORMAT query @text [organization_id=N]` against the fixture's server. */
static gchar *
cli_query(
	Fixture		*f,
	const gchar	*secret,
	const gchar	*format,
	gint64		 organization,
	const gchar	*text
){
	g_autoptr(GSubprocessLauncher) launcher = NULL;
	g_autoptr(GSubprocess) child = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *base = g_strdup(venture_web_server_get_base_url(f->server));
	g_autofree gchar *scope = (organization > 0)
		? g_strdup_printf("organization_id=%" G_GINT64_FORMAT, organization) : NULL;
	CliResult result = { FALSE, NULL, NULL, NULL };

	if (g_str_has_suffix(base, "/"))
		base[strlen(base) - 1] = '\0';

	launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE |
	                                     G_SUBPROCESS_FLAGS_STDERR_PIPE);
	g_subprocess_launcher_setenv(launcher, "VENTURE_TOKEN", secret, TRUE);
	child = g_subprocess_launcher_spawn(launcher, &error, VENTURE_TEST_BIN_DIR "/venturectl",
		"--server", base, "-f", format, "query", text, scope, NULL);
	g_assert_no_error(error);
	g_subprocess_communicate_utf8_async(child, NULL, NULL, cli_done, &result);

	while (!result.done)
		g_main_context_iteration(NULL, TRUE);

	g_assert_no_error(result.error);

	if (0 != g_subprocess_get_exit_status(child))
		g_error("venturectl query failed: %s", result.diagnostic);

	g_free(result.diagnostic);

	return result.output;
}

/* tools/call venture_query over POST /mcp; the tool's text. */
static gchar *
mcp_query(
	Fixture		*f,
	gint64		 organization,
	const gchar	*text
){
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(JsonNode) request = NULL;
	g_autoptr(JsonNode) answer = NULL;
	g_autofree gchar *body = NULL;
	g_autofree gchar *reply = NULL;
	JsonObject *result;
	guint status = 0;

	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "jsonrpc");
	json_builder_add_string_value(builder, "2.0");
	json_builder_set_member_name(builder, "id");
	json_builder_add_int_value(builder, 7);
	json_builder_set_member_name(builder, "method");
	json_builder_add_string_value(builder, "tools/call");
	json_builder_set_member_name(builder, "params");
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "name");
	json_builder_add_string_value(builder, "venture_query");
	json_builder_set_member_name(builder, "arguments");
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "query");
	json_builder_add_string_value(builder, text);

	if (organization > 0)
	{
		json_builder_set_member_name(builder, "organization_id");
		json_builder_add_int_value(builder, organization);
	}

	json_builder_end_object(builder);
	json_builder_end_object(builder);
	json_builder_end_object(builder);
	request = json_builder_get_root(builder);
	body = venture_json_to_string(request, FALSE);

	reply = http_send(f, "POST", "/mcp", f->admin_secret, body, &status, NULL);
	g_assert_cmpuint(status, ==, 200);
	answer = venture_json_parse(reply, NULL);
	g_assert_nonnull(answer);

	if (json_object_has_member(json_node_get_object(answer), "error"))
		g_error("venture_query: %s", reply);

	result = json_object_get_object_member(json_node_get_object(answer), "result");
	g_assert_false(json_object_get_boolean_member_with_default(result, "isError", FALSE));

	return g_strdup(json_object_get_string_member(json_array_get_object_element(
		json_object_get_array_member(result, "content"), 0), "text"));
}

/*
 * Both acceptance queries give identical rows through POST /api/v1/query,
 * `venturectl -f json query` and the MCP tool venture_query; their CSV
 * is identical too; and the tool is listed as read-only.
 */
static void
test_surfaces_identical(
	Fixture		*f,
	gconstpointer	 data
){
	static const gchar *const queries[] = { QUERY_OPEN_BY_ASSIGNEE, QUERY_INVOICES_BY_STATUS };
	/* The tickets in the token's own business, the invoices in B's. */
	gint64 scopes[2];
	g_autoptr(JsonNode) tools = NULL;
	g_autofree gchar *listed = NULL;
	gboolean found = FALSE;
	guint status = 0;
	guint i;

	(void)data;

	scopes[0] = 0;
	scopes[1] = f->org_b;

	for (i = 0; i < G_N_ELEMENTS(queries); i++)
	{
		g_autoptr(JsonNode) api = NULL;
		g_autoptr(JsonNode) cli = NULL;
		g_autoptr(JsonNode) mcp = NULL;
		g_autofree gchar *cli_text = NULL;
		g_autofree gchar *mcp_text = NULL;
		g_autofree gchar *api_rows = NULL;
		g_autofree gchar *cli_rows = NULL;
		g_autofree gchar *mcp_rows = NULL;
		g_autofree gchar *api_csv = NULL;
		g_autofree gchar *cli_csv = NULL;
		g_autofree gchar *body = NULL;

		api = api_query_in(f, f->admin_secret, scopes[i], queries[i], &status);
		g_assert_cmpuint(status, ==, 200);
		cli_text = cli_query(f, f->admin_secret, "json", scopes[i], queries[i]);
		cli = venture_json_parse(cli_text, NULL);
		g_assert_nonnull(cli);
		mcp_text = mcp_query(f, scopes[i], queries[i]);
		mcp = venture_json_parse(mcp_text, NULL);
		g_assert_nonnull(mcp);

		api_rows = rows_text(api);
		cli_rows = rows_text(cli);
		mcp_rows = rows_text(mcp);
		g_test_message("%s\n  api: %s\n  cli: %s\n  mcp: %s", queries[i], api_rows, cli_rows, mcp_rows);
		g_assert_cmpstr(api_rows, !=, "[]");
		g_assert_cmpstr(api_rows, ==, cli_rows);
		g_assert_cmpstr(api_rows, ==, mcp_rows);

		/* The same columns too. */
		{
			g_autofree gchar *a = venture_json_to_string(
				json_object_get_member(json_node_get_object(api), "columns"), FALSE);
			g_autofree gchar *b = venture_json_to_string(
				json_object_get_member(json_node_get_object(cli), "columns"), FALSE);
			g_autofree gchar *c = venture_json_to_string(
				json_object_get_member(json_node_get_object(mcp), "columns"), FALSE);

			g_assert_cmpstr(a, ==, b);
			g_assert_cmpstr(a, ==, c);
		}

		/* CSV: the CLI prints the server's own, byte for byte. */
		{
			g_autoptr(JsonBuilder) builder = json_builder_new();
			g_autoptr(JsonNode) request = NULL;

			json_builder_begin_object(builder);
			json_builder_set_member_name(builder, "query");
			json_builder_add_string_value(builder, queries[i]);
			json_builder_set_member_name(builder, "format");
			json_builder_add_string_value(builder, "csv");
			json_builder_end_object(builder);
			request = json_builder_get_root(builder);
			body = venture_json_to_string(request, FALSE);
		}

		{
			g_autofree gchar *path = (scopes[i] > 0)
				? g_strdup_printf("/api/v1/query?organization_id=%" G_GINT64_FORMAT, scopes[i])
				: g_strdup("/api/v1/query");

			api_csv = http_send(f, "POST", path, f->admin_secret, body, &status, NULL);
		}
		g_assert_cmpuint(status, ==, 200);
		cli_csv = cli_query(f, f->admin_secret, "csv", scopes[i], queries[i]);
		g_assert_cmpstr(api_csv, ==, cli_csv);
	}

	/* tools/list: venture_query, annotated read-only. */
	listed = http_send(f, "POST", "/mcp", f->admin_secret,
	                   "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}", &status, NULL);
	g_assert_cmpuint(status, ==, 200);
	tools = venture_json_parse(listed, NULL);

	{
		JsonArray *array = json_object_get_array_member(
			json_object_get_object_member(json_node_get_object(tools), "result"), "tools");

		for (i = 0; i < json_array_get_length(array); i++)
		{
			JsonObject *tool = json_array_get_object_element(array, i);

			if (0 != g_strcmp0(json_object_get_string_member(tool, "name"), "venture_query"))
				continue;

			found = TRUE;
			g_assert_true(json_object_get_boolean_member(
				json_object_get_object_member(tool, "annotations"), "readOnlyHint"));
			g_assert_false(json_object_get_boolean_member(
				json_object_get_object_member(tool, "annotations"), "destructiveHint"));
		}
	}

	g_assert_true(found);
}

/* --- What is not there ------------------------------------------------------- */

/*
 * LQL's file, program, SQL, git, journal and Kubernetes commands are not
 * in a VENTURE environment, so through the API every one is an unknown
 * command (400) and nothing runs, even for an administrator.
 */
static void
test_excluded_providers(
	Fixture		*f,
	gconstpointer	 data
){
	static const gchar *const queries[] = {
		"read \"/etc/passwd\"",
		"read \"/etc/passwd\" | limit 1",
		"sql \"SELECT * FROM users\"",
		"sqlite \"/tmp/x.db\", query: \"SELECT 1\"",
		"postgres \"postgresql://localhost/venture\", query: \"SELECT 1\"",
		"exec \"id\"",
		"git_log \".\"",
		"k8s \"pods\"",
		"journal",
		"fetch file",
		"fetch sql"
	};
	guint i;

	(void)data;

	for (i = 0; i < G_N_ELEMENTS(queries); i++)
	{
		g_autoptr(JsonNode) refused = NULL;
		const gchar *message;
		guint status = 0;

		refused = api_query(f, f->admin_secret, queries[i], &status);
		message = json_object_get_string_member(json_node_get_object(refused), "message");
		g_test_message("%s -> %u %s", queries[i], status, message);
		g_assert_cmpuint(status, ==, 400);
		g_assert_false(json_object_has_member(json_node_get_object(refused), "rows"));
		g_assert_true((NULL != strstr(message, "unknown command")) ||
		              (NULL != strstr(message, "no source named")));
		g_assert_null(strstr(message, "root:"));
	}
}

/* --- main ---------------------------------------------------------------------- */

int
main(
	int	  argc,
	char	**argv
){
	g_test_init(&argc, &argv, NULL);
	g_setenv("VENTURE_TEST_SESSION_SECRET", "lql-query-test-secret", TRUE);

#define ADD(path, func) \
	g_test_add(path, Fixture, NULL, fixture_setup, func, fixture_teardown)

	ADD("/lql/query/api/document", test_document);
	ADD("/lql/query/access/member-type-and-organization", test_access_member);
	ADD("/lql/query/access/token-sees-its-organization", test_access_token);
	ADD("/lql/query/bounds/max-rows-is-an-error", test_max_rows);
	ADD("/lql/query/bounds/timeout", test_timeout);
	ADD("/lql/query/runs/main-loop-stays-responsive", test_main_loop_responsive);
	ADD("/lql/query/runs/deadline-stops-the-query", test_deadline_stops_the_query);
	ADD("/lql/query/runs/cancel-mid-query", test_cancel_mid_query);
	g_test_add_func("/lql/query/bounds/max-bytes-setting", test_max_bytes);
	ADD("/lql/query/csv", test_csv);
	ADD("/lql/query/money/one-figure-per-currency", test_per_currency);
	ADD("/lql/query/widget/runs-as-the-viewer", test_widget_scope);
	ADD("/lql/query/widget/acceptance", test_widget_acceptance);
	ADD("/lql/query/surfaces/api-cli-mcp-identical", test_surfaces_identical);
	ADD("/lql/query/providers/excluded", test_excluded_providers);

#undef ADD

	return g_test_run();
}
