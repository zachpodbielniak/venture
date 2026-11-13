/*
 * test-mcp-http.c - POST /mcp, the Model Context Protocol over HTTP(S)
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Every test here stands up a real server on a kernel-chosen port and talks
 * to it the way a remote MCP client does -- grok, a claude.ai connector --
 * with a URL and an Authorization header. The tool calls loop back into the
 * same server's REST API as the caller, so these are the tests that would
 * notice the loopback deadlocking the main loop, losing the token, or
 * applying a write that should have been staged.
 */

#include <venture.h>
#include <libsoup/soup.h>
#include <string.h>

#include "venture-test-util.h"

typedef struct
{
	VentureConfig		*config;
	VentureDatabase		*database;
	VentureContext		*context;
	VentureWebServer	*server;
	SoupSession		*session;
	gchar			*state;
	gchar			*secret;
	gint64			 organization;
} Fixture;

typedef struct
{
	gboolean	 done;
	GBytes		*body;
	GError		*error;
} Reply;

/* The test client trusts the fixture's self-signed certificate. */
static gboolean
accept_any(SoupMessage *message, GTlsCertificate *peer, GTlsCertificateFlags errors, gpointer data)
{
	(void)message; (void)peer; (void)errors; (void)data;
	return TRUE;
}

static void
received(GObject *source, GAsyncResult *result, gpointer data)
{
	Reply *reply = data;

	reply->body = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &reply->error);
	reply->done = TRUE;
}

/*
 * One request, driving the server's main loop while it is answered. The
 * client is asynchronous on purpose: the server and the client share this
 * thread, and a synchronous send would block the loop that has to answer.
 */
static guint
send_request(
	Fixture		 *f,
	const gchar	 *method,
	const gchar	 *authorization,
	const gchar	 *content_type,
	const gchar	 *body,
	gchar		**out_body,
	gchar		**out_header
){
	g_autofree gchar *base = g_strdup(venture_web_server_get_base_url(f->server));
	g_autofree gchar *url = NULL;
	g_autoptr(SoupMessage) message = NULL;
	Reply reply = { FALSE, NULL, NULL };
	gint64 deadline = g_get_monotonic_time() + 30 * G_USEC_PER_SEC;
	SoupMessageHeaders *headers;

	/* A TLS listener's base URL is libsoup's, with a trailing slash. */
	if (g_str_has_suffix(base, "/"))
		base[strlen(base) - 1] = '\0';

	url = g_strconcat(base, "/mcp", NULL);
	message = soup_message_new(method, url);
	headers = soup_message_get_request_headers(message);

	g_signal_connect(message, "accept-certificate", G_CALLBACK(accept_any), NULL);
	soup_message_headers_replace(headers, "Accept", "application/json, text/event-stream");

	if (NULL != authorization)
		soup_message_headers_replace(headers, "Authorization", authorization);

	if (NULL != body)
	{
		g_autoptr(GBytes) bytes = g_bytes_new(body, strlen(body));
		soup_message_set_request_body_from_bytes(message, content_type, bytes);
	}

	soup_session_send_and_read_async(f->session, message, G_PRIORITY_DEFAULT, NULL, received, &reply);

	while (!reply.done && g_get_monotonic_time() < deadline)
	{
		while (g_main_context_iteration(NULL, FALSE)) { }
		if (!reply.done)
			g_usleep(1000);
	}

	g_assert_true(reply.done);
	g_assert_no_error(reply.error);

	if (NULL != out_body)
		*out_body = g_strndup(g_bytes_get_data(reply.body, NULL), g_bytes_get_size(reply.body));

	if (NULL != out_header)
		*out_header = g_strdup(soup_message_headers_get_one(
			soup_message_get_response_headers(message), "WWW-Authenticate"));

	g_bytes_unref(reply.body);

	return soup_message_get_status(message);
}

/* A POST with the fixture's token, answered as JSON. */
static JsonNode *
rpc(Fixture *f, const gchar *body, guint expected)
{
	g_autofree gchar *authorization = g_strconcat("Bearer ", f->secret, NULL);
	g_autofree gchar *text = NULL;
	g_autoptr(GError) error = NULL;
	JsonNode *node;

	g_assert_cmpuint(send_request(f, "POST", authorization, "application/json", body, &text, NULL), ==, expected);
	node = venture_json_parse(text, &error);
	g_assert_no_error(error);

	return node;
}

/* The text of a tools/call answer. */
static const gchar *
tool_text(JsonNode *answer)
{
	JsonObject *result = json_object_get_object_member(json_node_get_object(answer), "result");
	JsonArray *content = json_object_get_array_member(result, "content");

	return json_object_get_string_member(json_array_get_object_element(content, 0), "text");
}

static void
setup(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) user = NULL;
	g_autoptr(VentureApiToken) token = venture_api_token_new();
	gboolean tls = GPOINTER_TO_INT(data);

	f->state = g_dir_make_tmp("venture-mcp-http-XXXXXX", &error);
	g_assert_no_error(error);

	f->config = venture_config_new();
	g_object_set(f->config, "state-dir", f->state, "server-bind-address", "127.0.0.1",
		"server-port", (gint64)0, "security-password-iterations", (gint64)2000, NULL);

	if (tls)
	{
		g_autofree gchar *cert = g_canonicalize_filename("tests/fixtures/federation/tls-cert.pem", NULL);
		g_autofree gchar *key = g_canonicalize_filename("tests/fixtures/federation/tls-key.pem", NULL);
		g_object_set(f->config, "server-tls-certificate", cert, "server-tls-private-key", key, NULL);
	}

	/* A file, not :memory:, so the read workers can serve the loopback's
	 * list and get as they would in production. */
	{
		g_autofree gchar *uri = g_strconcat("sqlite://", f->state, "/venture.db", NULL);
		f->database = venture_database_new(uri, &error);
		g_assert_no_error(error);
	}
	g_assert_true(venture_database_migrate(f->database, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);

	f->context = venture_context_new(f->config, f->database);
	f->organization = venture_context_get_default_organization_id(f->context);

	user = g_object_new(VENTURE_TYPE_USER, "username", "agent-owner", "active", TRUE,
		"role", VENTURE_USER_ROLE_OWNER, NULL);
	g_assert_true(venture_database_save(f->database, user, NULL, &error));
	g_assert_no_error(error);

	g_object_set(token, "name", "grok", "user-id", venture_entity_get_id(user),
		"role", VENTURE_USER_ROLE_OWNER, NULL);
	f->secret = venture_api_token_generate(token);
	g_assert_true(venture_database_save(f->database, VENTURE_ENTITY(token), NULL, &error));
	g_assert_no_error(error);

	f->server = venture_web_server_new(f->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(f->server, &error));
	g_assert_no_error(error);

	f->session = soup_session_new_with_options("timeout", 30, NULL);
}

static void
teardown(Fixture *f, gconstpointer data)
{
	(void)data;

	venture_web_server_stop(f->server);
	g_clear_object(&f->server);
	g_clear_object(&f->session);
	g_clear_object(&f->context);
	g_clear_object(&f->database);
	g_clear_object(&f->config);
	g_clear_pointer(&f->secret, g_free);
	venture_test_remove_tree(f->state);
	g_clear_pointer(&f->state, g_free);
}

/*
 * The handshake names this door "venture", not "venturectl": a client that
 * has both configured must be able to tell them apart.
 */
static void
test_initialize(Fixture *f, gconstpointer data)
{
	g_autoptr(JsonNode) answer = NULL;
	JsonObject *result;

	(void)data;

	answer = rpc(f, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":"
		"{\"protocolVersion\":\"2025-06-18\",\"capabilities\":{},"
		"\"clientInfo\":{\"name\":\"grok\",\"version\":\"1\"}}}", 200);
	result = json_object_get_object_member(json_node_get_object(answer), "result");
	g_assert_nonnull(result);
	g_assert_cmpstr(json_object_get_string_member(
		json_object_get_object_member(result, "serverInfo"), "name"), ==, "venture");
	g_assert_true(json_object_has_member(result, "protocolVersion"));
	/* The id keeps its type: a number answered as a string is a reply the
	 * client cannot match, and looks like a hang. */
	g_assert_cmpint(json_object_get_int_member(json_node_get_object(answer), "id"), ==, 1);
}

/*
 * No token, a wrong token, a browser-only credential: each is a 401 with a
 * WWW-Authenticate challenge, and never a tool answer that reads like a
 * missing record.
 */
static void
test_requires_bearer(Fixture *f, gconstpointer data)
{
	const gchar *body = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}";
	g_autofree gchar *challenge = NULL;
	g_autofree gchar *wrong_challenge = NULL;

	(void)data;

	g_assert_cmpuint(send_request(f, "POST", NULL, "application/json", body, NULL, &challenge), ==, 401);
	g_assert_nonnull(challenge);
	g_assert_true(g_str_has_prefix(challenge, "Bearer"));

	g_assert_cmpuint(send_request(f, "POST", "Bearer vk_not-a-real-token", "application/json", body,
		NULL, &wrong_challenge), ==, 401);
	g_assert_nonnull(strstr(wrong_challenge, "invalid_token"));

	g_assert_cmpuint(send_request(f, "POST", "Basic YWdlbnQ6cGFzcw==", "application/json", body,
		NULL, NULL), ==, 401);
}

/*
 * tools/list comes from the live schema through the loopback -- the proof
 * that the worker reached the API as the caller and the main loop kept
 * serving while it waited.
 */
static void
test_tools_list(Fixture *f, gconstpointer data)
{
	g_autoptr(JsonNode) answer = NULL;
	JsonArray *tools;
	gboolean found = FALSE;
	guint i;

	(void)data;

	answer = rpc(f, "{\"jsonrpc\":\"2.0\",\"id\":\"a\",\"method\":\"tools/list\"}", 200);
	tools = json_object_get_array_member(
		json_object_get_object_member(json_node_get_object(answer), "result"), "tools");
	g_assert_nonnull(tools);

	for (i = 0; i < json_array_get_length(tools); i++)
	{
		if (0 == g_strcmp0(json_object_get_string_member(
			json_array_get_object_element(tools, i), "name"), "venture_list"))
			found = TRUE;
	}

	g_assert_true(found);
	g_assert_cmpstr(json_object_get_string_member(json_node_get_object(answer), "id"), ==, "a");
}

/*
 * A read answers with the record a direct API request would; a write is
 * staged, not applied, under the default policy -- and nothing is in the
 * books until a person approves it.
 */
static void
test_read_and_staged_write(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) lead = NULL;
	g_autoptr(JsonNode) listed = NULL;
	g_autoptr(JsonNode) created = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) leads = NULL;
	g_autofree gchar *create = NULL;

	(void)data;

	lead = g_object_new(VENTURE_TYPE_LEAD, "organization-id", f->organization, "name", "Bellhaven inquiry", NULL);
	g_assert_true(venture_database_save(f->database, lead, NULL, &error));
	g_assert_no_error(error);

	listed = rpc(f, "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":"
		"{\"name\":\"venture_list\",\"arguments\":{\"type\":\"lead\"}}}", 200);
	g_assert_nonnull(strstr(tool_text(listed), "Bellhaven inquiry"));

	create = g_strdup_printf("{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":"
		"{\"name\":\"venture_create\",\"arguments\":{\"type\":\"lead\",\"values\":"
		"{\"name\":\"Proposed by an agent\",\"organization_id\":%" G_GINT64_FORMAT "}}}}", f->organization);
	created = rpc(f, create, 200);
	g_assert_nonnull(strstr(tool_text(created), "confirmation id"));

	query = venture_query_new(VENTURE_TYPE_LEAD);
	g_assert_true(venture_query_add_filter_string(query, "name", VENTURE_FILTER_OP_EQ, "Proposed by an agent", &error));
	g_assert_no_error(error);
	leads = venture_database_find(f->database, query, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(leads->len, ==, 0);
}

/*
 * mcp.apply_writes turns staging off: the same call now writes, audited as
 * the token's account. Read per request, so no restart is involved.
 */
static void
test_apply_writes(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) created = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) leads = NULL;
	g_autofree gchar *create = NULL;

	(void)data;

	g_object_set(f->config, "mcp-apply-writes", TRUE, NULL);
	create = g_strdup_printf("{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\",\"params\":"
		"{\"name\":\"venture_create\",\"arguments\":{\"type\":\"lead\",\"values\":"
		"{\"name\":\"Applied by an agent\",\"organization_id\":%" G_GINT64_FORMAT "}}}}", f->organization);
	created = rpc(f, create, 200);
	g_assert_null(strstr(tool_text(created), "confirmation id"));

	query = venture_query_new(VENTURE_TYPE_LEAD);
	g_assert_true(venture_query_add_filter_string(query, "name", VENTURE_FILTER_OP_EQ, "Applied by an agent", &error));
	g_assert_no_error(error);
	leads = venture_database_find(f->database, query, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(leads->len, ==, 1);
}

/* A notification is answered 202 with no body; JSON-RPC forbids a reply. */
static void
test_notification(Fixture *f, gconstpointer data)
{
	g_autofree gchar *authorization = g_strconcat("Bearer ", f->secret, NULL);
	g_autofree gchar *body = NULL;

	(void)data;

	g_assert_cmpuint(send_request(f, "POST", authorization, "application/json",
		"{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}", &body, NULL), ==, 202);
	g_assert_true((NULL == body) || ('\0' == body[0]));
}

/* The transport's other verbs, a wrong body and a switched-off door. */
static void
test_refusals(Fixture *f, gconstpointer data)
{
	g_autofree gchar *authorization = g_strconcat("Bearer ", f->secret, NULL);

	(void)data;

	g_assert_cmpuint(send_request(f, "GET", authorization, NULL, NULL, NULL, NULL), ==, 405);
	g_assert_cmpuint(send_request(f, "DELETE", authorization, NULL, NULL, NULL, NULL), ==, 405);
	g_assert_cmpuint(send_request(f, "POST", authorization, "text/plain", "{}", NULL, NULL), ==, 415);
	g_assert_cmpuint(send_request(f, "POST", authorization, "application/json", "not json", NULL, NULL), ==, 400);
	g_assert_cmpuint(send_request(f, "POST", authorization, "application/json", "[]", NULL, NULL), ==, 400);

	g_object_set(f->config, "mcp-enabled", FALSE, NULL);
	g_assert_cmpuint(send_request(f, "POST", authorization, "application/json",
		"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"}", NULL, NULL), ==, 404);
}

/* A batch is answered by the array of answers that exist. */
static void
test_batch(Fixture *f, gconstpointer data)
{
	g_autoptr(JsonNode) answer = NULL;

	(void)data;

	answer = rpc(f, "[{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"},"
		"{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"},"
		"{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"ping\"}]", 200);
	g_assert_true(JSON_NODE_HOLDS_ARRAY(answer));
	g_assert_cmpuint(json_array_get_length(json_node_get_array(answer)), ==, 2);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add("/mcp-http/initialize", Fixture, GINT_TO_POINTER(0), setup, test_initialize, teardown);
	g_test_add("/mcp-http/requires-bearer", Fixture, GINT_TO_POINTER(0), setup, test_requires_bearer, teardown);
	g_test_add("/mcp-http/tools-list", Fixture, GINT_TO_POINTER(0), setup, test_tools_list, teardown);
	g_test_add("/mcp-http/read-and-staged-write", Fixture, GINT_TO_POINTER(0), setup, test_read_and_staged_write, teardown);
	g_test_add("/mcp-http/apply-writes", Fixture, GINT_TO_POINTER(0), setup, test_apply_writes, teardown);
	g_test_add("/mcp-http/notification", Fixture, GINT_TO_POINTER(0), setup, test_notification, teardown);
	g_test_add("/mcp-http/refusals", Fixture, GINT_TO_POINTER(0), setup, test_refusals, teardown);
	g_test_add("/mcp-http/batch", Fixture, GINT_TO_POINTER(0), setup, test_batch, teardown);
	/* Over TLS the loopback pins the listener's own certificate. */
	g_test_add("/mcp-http/tls-tools-list", Fixture, GINT_TO_POINTER(1), setup, test_tools_list, teardown);
	g_test_add("/mcp-http/tls-read-and-staged-write", Fixture, GINT_TO_POINTER(1), setup, test_read_and_staged_write, teardown);

	return g_test_run();
}
