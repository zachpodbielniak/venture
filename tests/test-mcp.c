/*
 * test-mcp.c - `venturectl mcp`, the stdio MCP server
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Every response here comes from a fixture. The whole point of the injected
 * transport is that this file needs no VENTURE server, no network and no
 * subprocess: a test that needs a running server is a test that does not
 * run, and the MCP surface is exactly the thing that must not be broken
 * quietly.
 *
 * What each group is defending:
 *
 *   catalog  -- the tool surface is a function of the schema. A record type
 *               registered by a plugin has to appear without a code change,
 *               or the surface is a second copy of the data model that goes
 *               stale.
 *   naming   -- singular and plural are one resource. Offering both would
 *               present one thing as two and invite a duplicate.
 *   errors   -- a 401 must never read as a missing record. An agent told a
 *               record does not exist creates it again.
 *   report   -- ?format=csv returns a different content type, and CSV read
 *               as JSON is nonsense that parses.
 *   staging  -- a held write must say it was not applied and that nothing is
 *               pending, because VENTURE has no server-side staging for a
 *               token write.
 */

#include <venture.h>

#include <string.h>

/* --- A transport driven by fixtures -------------------------------------- */

/*
 * What the next request should be answered with, and what the last request
 * actually was. Both directions are recorded: several of these tests are
 * about the request that goes out, not the answer that comes back.
 */
typedef struct
{
	gchar	*body;
	gchar	*content_type;
	guint	 status;

	/* Answered for /api/v1/health only, so a test can say whether the
	 * server on the other end can stage a write. Left NULL, health gets
	 * whatever @body is -- which is how an older server that never heard
	 * of staging is simulated. */
	gchar	*health;

	gchar	*last_method;
	gchar	*last_path;
	gchar	*last_body;
	guint	 calls;
} MockTransport;

static MockTransport *
mock_transport_new(void)
{
	MockTransport *mock;

	mock = g_new0(MockTransport, 1);
	mock->status = 200;
	mock->content_type = g_strdup("application/json");

	return mock;
}

static void
mock_transport_free(gpointer data)
{
	MockTransport *mock;

	mock = data;

	g_free(mock->body);
	g_free(mock->health);
	g_free(mock->content_type);
	g_free(mock->last_method);
	g_free(mock->last_path);
	g_free(mock->last_body);
	g_free(mock);
}

static void
mock_transport_answer(
	MockTransport	*mock,
	guint		 status,
	const gchar	*content_type,
	const gchar	*body
){
	g_free(mock->body);
	g_free(mock->content_type);

	mock->status = status;
	mock->content_type = g_strdup(content_type);
	mock->body = g_strdup(body);
}

static gchar *
mock_transport_call(
	const gchar	 *method,
	const gchar	 *path,
	JsonNode	 *body,
	guint		 *out_status,
	gchar		**out_content_type,
	gpointer	  user_data,
	GError		**error
){
	MockTransport *mock;

	mock = user_data;

	/* The capability probe is not one of the calls a test counts: it is
	 * setup, and counting it would make "nothing crossed the wire" mean
	 * something different depending on whether staging was on. */
	if ((NULL != mock->health) && (0 == g_strcmp0(path, "/api/v1/health")))
	{
		*out_status = 200;

		if (NULL != out_content_type)
			*out_content_type = g_strdup("application/json");

		return g_strdup(mock->health);
	}

	mock->calls++;

	g_free(mock->last_method);
	g_free(mock->last_path);
	g_free(mock->last_body);

	mock->last_method = g_strdup(method);
	mock->last_path = g_strdup(path);
	mock->last_body = (NULL != body) ? venture_json_to_string(body, FALSE)
	                                 : NULL;

	*out_status = mock->status;

	if (NULL != out_content_type)
		*out_content_type = g_strdup(mock->content_type);

	return g_strdup((NULL != mock->body) ? mock->body : "{}");
}

/* --- Fixtures ------------------------------------------------------------ */

static gchar *
fixture_read(const gchar *name)
{
	g_autofree gchar *path = NULL;
	g_autoptr(GError) error = NULL;
	gchar *contents;

	path = g_build_filename(VENTURE_TEST_FIXTURES, name, NULL);
	contents = NULL;

	g_assert_true(g_file_get_contents(path, &contents, NULL, &error));
	g_assert_no_error(error);

	return contents;
}

static JsonNode *
fixture_schema(const gchar *name)
{
	g_autofree gchar *text = NULL;
	JsonNode *node;

	text = fixture_read(name);
	node = venture_json_parse(text, NULL);

	g_assert_nonnull(node);

	return node;
}

/*
 * A server wired to a mock transport, with the catalog already loaded from
 * a schema fixture. Every protocol test starts here.
 */
static VentureMcpServer *
server_with_fixture_staging(
	const gchar	 *fixture,
	gboolean	  server_stages,
	MockTransport	**out_mock
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *schema = NULL;
	VentureMcpServer *server;
	MockTransport *mock;

	server = venture_mcp_server_new("http://example.invalid", "test-token",
	                                &error);
	g_assert_no_error(error);
	g_assert_nonnull(server);

	mock = mock_transport_new();
	venture_mcp_server_set_transport(server, mock_transport_call, mock,
	                                 mock_transport_free);

	if (server_stages)
		mock->health = g_strdup("{\"status\": \"ok\", "
		                        "\"staged_writes\": true}");

	schema = fixture_read(fixture);
	mock_transport_answer(mock, 200, "application/json", schema);

	g_assert_true(venture_mcp_server_load_catalog(server, &error));
	g_assert_no_error(error);

	if (NULL != out_mock)
		*out_mock = mock;

	return server;
}

/*
 * The common case: a server that does not advertise staging, which is what
 * every test written before staging existed assumed.
 */
static VentureMcpServer *
server_with_fixture(
	const gchar	 *fixture,
	MockTransport	**out_mock
){
	return server_with_fixture_staging(fixture, FALSE, out_mock);
}

/* --- Small helpers over the protocol ------------------------------------- */

static JsonNode *
rpc_request(
	const gchar	*method,
	JsonNode	*params
){
	g_autoptr(JsonBuilder) builder = NULL;

	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "jsonrpc");
	json_builder_add_string_value(builder, "2.0");
	json_builder_set_member_name(builder, "id");
	json_builder_add_int_value(builder, 7);
	json_builder_set_member_name(builder, "method");
	json_builder_add_string_value(builder, method);

	if (NULL != params)
	{
		json_builder_set_member_name(builder, "params");
		json_builder_add_value(builder, params);
	}

	json_builder_end_object(builder);

	return json_builder_get_root(builder);
}

/*
 * Builds a tools/call params object from an alternating name/value list of
 * strings, terminated by NULL. Values are strings, which is all these tests
 * need and is what a model sends most of the time anyway.
 */
static JsonNode *
call_params(
	const gchar	*tool,
	...
){
	g_autoptr(JsonBuilder) builder = NULL;
	va_list args;
	const gchar *name;

	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "name");
	json_builder_add_string_value(builder, tool);
	json_builder_set_member_name(builder, "arguments");
	json_builder_begin_object(builder);

	va_start(args, tool);

	while (NULL != (name = va_arg(args, const gchar *)))
	{
		const gchar *value;

		value = va_arg(args, const gchar *);
		json_builder_set_member_name(builder, name);
		json_builder_add_string_value(builder, value);
	}

	va_end(args);

	json_builder_end_object(builder);
	json_builder_end_object(builder);

	return json_builder_get_root(builder);
}

/*
 * The text of a tools/call result, and whether it was reported as an error.
 */
static const gchar *
result_text(
	JsonNode	*response,
	gboolean	*out_is_error
){
	JsonObject *root;
	JsonObject *result;
	JsonObject *block;
	JsonArray *content;

	g_assert_nonnull(response);
	g_assert_true(JSON_NODE_HOLDS_OBJECT(response));

	root = json_node_get_object(response);

	/* A tool failure is a successful JSON-RPC response carrying isError,
	 * never a JSON-RPC error: the model has to be able to read it. */
	g_assert_false(json_object_has_member(root, "error"));
	g_assert_true(json_object_has_member(root, "result"));

	result = json_object_get_object_member(root, "result");
	content = json_object_get_array_member(result, "content");

	g_assert_cmpuint(json_array_get_length(content), ==, 1);

	block = json_array_get_object_element(content, 0);

	if (NULL != out_is_error)
		*out_is_error = json_object_get_boolean_member(result, "isError");

	return json_object_get_string_member(block, "text");
}

/*
 * The `enum` of a tool's `type` parameter -- the one thing in the tool list
 * that is generated rather than written down.
 */
static JsonArray *
tool_type_enum(
	JsonNode	*tools,
	const gchar	*tool_name
){
	JsonArray *array;
	guint i;

	array = json_node_get_array(tools);

	for (i = 0; i < json_array_get_length(array); i++)
	{
		JsonObject *tool;
		JsonObject *schema;
		JsonObject *properties;
		JsonObject *type_property;

		tool = json_array_get_object_element(array, i);

		if (0 != g_strcmp0(json_object_get_string_member(tool, "name"),
		                   tool_name))
			continue;

		schema = json_object_get_object_member(tool, "inputSchema");
		properties = json_object_get_object_member(schema, "properties");

		if (!json_object_has_member(properties, "type"))
			return NULL;

		type_property = json_object_get_object_member(properties, "type");

		return json_object_get_array_member(type_property, "enum");
	}

	return NULL;
}

static gboolean
array_contains(
	JsonArray	*array,
	const gchar	*value
){
	guint i;

	if (NULL == array)
		return FALSE;

	for (i = 0; i < json_array_get_length(array); i++)
	{
		if (0 == g_strcmp0(json_array_get_string_element(array, i), value))
			return TRUE;
	}

	return FALSE;
}

/* --- The catalog is generated from the schema ---------------------------- */

/*
 * The tool list comes out of a schema response and nothing else.
 *
 * If this ever needs a record type named in C, the surface has become a
 * second copy of VENTURE's data model and will drift from it.
 */
static void
test_catalog_from_schema(void)
{
	g_autoptr(JsonNode) schema = NULL;
	g_autoptr(VentureMcpCatalog) catalog = NULL;
	g_autoptr(JsonNode) tools = NULL;
	g_autoptr(GError) error = NULL;
	g_auto(GStrv) names = NULL;
	JsonArray *enumeration;

	schema = fixture_schema("schema.json");
	catalog = venture_mcp_catalog_new_from_schema(schema, &error);

	g_assert_no_error(error);
	g_assert_nonnull(catalog);
	g_assert_cmpuint(venture_mcp_catalog_get_n_types(catalog), ==, 3);

	names = venture_mcp_catalog_list_types(catalog);
	g_assert_cmpuint(g_strv_length(names), ==, 3);

	tools = venture_mcp_catalog_get_tools(catalog);
	g_assert_nonnull(tools);
	g_assert_true(JSON_NODE_HOLDS_ARRAY(tools));
	g_assert_cmpuint(json_array_get_length(json_node_get_array(tools)), ==, 16);

	enumeration = tool_type_enum(tools, "venture_list");
	g_assert_nonnull(enumeration);

	/*
	 * Driven by the fixture rather than by a list written here: every
	 * type the schema described must be offered, whatever it is. Adding
	 * one to the fixture extends this assertion by itself.
	 */
	{
		gsize i;

		g_assert_cmpuint(json_array_get_length(enumeration), ==,
		                 g_strv_length(names));

		for (i = 0; NULL != names[i]; i++)
			g_assert_true(array_contains(enumeration, names[i]));
	}
}

/*
 * A record type a plugin registered appears with no code change.
 *
 * The two fixtures differ by one entry and nothing else. This is the whole
 * argument for generating the surface: `docs/api.org` says a plugin's record
 * type becomes a resource the moment it registers, with no routing to add,
 * and the tool surface has to hold to the same promise.
 */
static void
test_catalog_new_type_needs_no_code(void)
{
	g_autoptr(JsonNode) base_schema = NULL;
	g_autoptr(JsonNode) plus_schema = NULL;
	g_autoptr(VentureMcpCatalog) base = NULL;
	g_autoptr(VentureMcpCatalog) plus = NULL;
	g_autoptr(JsonNode) base_tools = NULL;
	g_autoptr(JsonNode) plus_tools = NULL;
	g_autoptr(GError) error = NULL;
	JsonArray *before;
	JsonArray *after;

	base_schema = fixture_schema("schema.json");
	plus_schema = fixture_schema("schema-with-plugin-type.json");

	base = venture_mcp_catalog_new_from_schema(base_schema, &error);
	g_assert_no_error(error);
	plus = venture_mcp_catalog_new_from_schema(plus_schema, &error);
	g_assert_no_error(error);

	g_assert_cmpuint(venture_mcp_catalog_get_n_types(plus), ==,
	                 venture_mcp_catalog_get_n_types(base) + 1);

	base_tools = venture_mcp_catalog_get_tools(base);
	plus_tools = venture_mcp_catalog_get_tools(plus);

	before = tool_type_enum(base_tools, "venture_create");
	after = tool_type_enum(plus_tools, "venture_create");

	g_assert_false(array_contains(before, "subscription"));
	g_assert_true(array_contains(after, "subscription"));

	/* And it is addressable, not merely listed. */
	g_assert_cmpstr(venture_mcp_catalog_resolve_type(plus, "subscription"),
	                ==, "subscription");
	g_assert_cmpstr(venture_mcp_catalog_resolve_type(plus, "subscriptions"),
	                ==, "subscription");
}

/*
 * Singular and plural are one resource, so they are one entry.
 *
 * /api/v1/sale and /api/v1/sales are the same route. Offering both spellings
 * in the enum would present one record type as two, and an agent choosing
 * "sales" for a list and "sale" for a create would believe it was working
 * with two different things.
 */
static void
test_catalog_singular_and_plural_are_one(void)
{
	g_autoptr(JsonNode) schema = NULL;
	g_autoptr(VentureMcpCatalog) catalog = NULL;
	g_autoptr(JsonNode) tools = NULL;
	g_autoptr(GError) error = NULL;
	JsonArray *enumeration;

	schema = fixture_schema("schema.json");
	catalog = venture_mcp_catalog_new_from_schema(schema, &error);
	g_assert_no_error(error);

	/* Either spelling, in any case, resolves to the singular. */
	g_assert_cmpstr(venture_mcp_catalog_resolve_type(catalog, "sale"), ==,
	                "sale");
	g_assert_cmpstr(venture_mcp_catalog_resolve_type(catalog, "sales"), ==,
	                "sale");
	g_assert_cmpstr(venture_mcp_catalog_resolve_type(catalog, "Sales"), ==,
	                "sale");
	g_assert_null(venture_mcp_catalog_resolve_type(catalog, "salez"));

	tools = venture_mcp_catalog_get_tools(catalog);
	enumeration = tool_type_enum(tools, "venture_list");

	g_assert_true(array_contains(enumeration, "sale"));
	g_assert_false(array_contains(enumeration, "sales"));
	g_assert_false(array_contains(enumeration, "expenses"));
	g_assert_false(array_contains(enumeration, "ventures"));
}

/*
 * A malformed schema is refused whole.
 *
 * Keeping whichever entries happened to parse would produce a catalog
 * quietly missing record types, and "there is no record type called X" is
 * indistinguishable from a server that genuinely does not have one.
 */
static void
test_catalog_malformed_schema_refused(void)
{
	static const gchar *const bad[] = {
		/* Not an array at all -- something else answering on the port. */
		"{\"error\": \"unauthenticated\"}",
		/* An array of the wrong thing. */
		"[\"sale\", \"expense\"]",
		/* An entry with no name, so nothing addressable. */
		"[{\"name\": \"sale\", \"plural\": \"sales\"}, {\"plural\": \"x\"}]",
		/* The same type twice. */
		"[{\"name\": \"sale\"}, {\"name\": \"sale\"}]",
		/* An empty registry, which a real server never has. */
		"[]",
		NULL
	};
	gsize i;

	for (i = 0; NULL != bad[i]; i++)
	{
		g_autoptr(JsonNode) node = NULL;
		g_autoptr(VentureMcpCatalog) catalog = NULL;
		g_autoptr(GError) error = NULL;

		node = venture_json_parse(bad[i], NULL);
		g_assert_nonnull(node);

		catalog = venture_mcp_catalog_new_from_schema(node, &error);

		g_assert_null(catalog);
		g_assert_nonnull(error);

		/* And it says what it was looking at, rather than "malformed". */
		g_assert_cmpuint(strlen(error->message), >, 30);
	}
}

/*
 * A field name reaches the caller in the spelling that works.
 *
 * Properties are `venture-id` in C and `venture_id` on the wire, and VENTURE
 * ignores a dashed member rather than refusing it: the record saves and the
 * value is not there. The schema endpoint reports the C spelling, so this is
 * where it has to be translated.
 */
static void
test_catalog_describes_wire_spelling(void)
{
	g_autoptr(JsonNode) schema = NULL;
	g_autoptr(VentureMcpCatalog) catalog = NULL;
	g_autoptr(JsonNode) described = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *text = NULL;

	schema = fixture_schema("schema.json");
	catalog = venture_mcp_catalog_new_from_schema(schema, &error);
	g_assert_no_error(error);

	/* Reached by its plural, to prove the description follows too. */
	described = venture_mcp_catalog_describe_type(catalog, "sales");
	g_assert_nonnull(described);

	text = venture_json_to_string(described, FALSE);

	g_assert_nonnull(g_strstr_len(text, -1, "venture_id"));
	g_assert_null(g_strstr_len(text, -1, "venture-id"));
	g_assert_nonnull(g_strstr_len(text, -1, "occurred_at"));

	/* The rest of the field's metadata survives: what a reference points
	 * at is the second thing anybody needs after the name. */
	g_assert_nonnull(g_strstr_len(text, -1, "references"));
}

/* --- Construction -------------------------------------------------------- */

/*
 * A missing token is refused before anything else, and the refusal says how
 * to mint one.
 *
 * Otherwise every call 401s identically and an agent reading a wall of them
 * concludes the records are missing rather than that it has no credential.
 */
static void
test_server_refuses_without_token(void)
{
	g_autoptr(GError) error = NULL;
	VentureMcpServer *server;

	server = venture_mcp_server_new("http://example.invalid", "", &error);

	g_assert_null(server);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_UNAUTHENTICATED);

	/* The remedy, not just the diagnosis. */
	g_assert_nonnull(g_strstr_len(error->message, -1, "VENTURE_TOKEN"));
	g_assert_nonnull(g_strstr_len(error->message, -1, "/api/v1/tokens"));
	g_assert_nonnull(g_strstr_len(error->message, -1, "once"));
}

/*
 * Staging is on unless somebody turns it off, matching VENTURE's own
 * confirm_writes default.
 */
static void
test_server_stages_by_default(void)
{
	g_autoptr(VentureMcpServer) server = NULL;
	g_autoptr(GError) error = NULL;

	server = venture_mcp_server_new("http://example.invalid", "t", &error);
	g_assert_no_error(error);

	g_assert_true(venture_mcp_server_get_stage_writes(server));

	venture_mcp_server_set_stage_writes(server, FALSE);
	g_assert_false(venture_mcp_server_get_stage_writes(server));
}

/* --- The protocol -------------------------------------------------------- */

static void
test_protocol_initialize(void)
{
	g_autoptr(VentureMcpServer) server = NULL;
	g_autoptr(JsonNode) request = NULL;
	g_autoptr(JsonNode) response = NULL;
	JsonObject *result;

	server = server_with_fixture("schema.json", NULL);
	request = rpc_request("initialize", NULL);
	response = venture_mcp_server_handle(server, request);

	g_assert_nonnull(response);

	result = json_object_get_object_member(json_node_get_object(response),
	                                       "result");

	g_assert_true(json_object_has_member(result, "protocolVersion"));
	g_assert_true(json_object_has_member(result, "capabilities"));
	g_assert_cmpstr(json_object_get_string_member(
		json_object_get_object_member(result, "serverInfo"), "name"), ==,
		"venturectl");
}

/*
 * A notification is not answered.
 *
 * Sending a response to one is a protocol violation, and the client that
 * receives it has no request to match it against.
 */
static void
test_protocol_notification_unanswered(void)
{
	g_autoptr(VentureMcpServer) server = NULL;
	g_autoptr(JsonNode) request = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	JsonNode *response;

	server = server_with_fixture("schema.json", NULL);

	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "jsonrpc");
	json_builder_add_string_value(builder, "2.0");
	json_builder_set_member_name(builder, "method");
	json_builder_add_string_value(builder, "notifications/initialized");
	json_builder_end_object(builder);
	request = json_builder_get_root(builder);

	response = venture_mcp_server_handle(server, request);

	g_assert_null(response);
}

/*
 * The JSON-RPC id keeps its type.
 *
 * A numeric id answered as a quoted string is a response the client cannot
 * match to its request, and the failure surfaces as the server having hung.
 */
static void
test_protocol_id_keeps_its_type(void)
{
	g_autoptr(VentureMcpServer) server = NULL;
	g_autoptr(JsonNode) request = NULL;
	g_autoptr(JsonNode) response = NULL;
	JsonNode *id;

	server = server_with_fixture("schema.json", NULL);
	request = rpc_request("ping", NULL);
	response = venture_mcp_server_handle(server, request);

	id = json_object_get_member(json_node_get_object(response), "id");

	g_assert_nonnull(id);
	g_assert_true(JSON_NODE_HOLDS_VALUE(id));
	g_assert_true(G_TYPE_INT64 == json_node_get_value_type(id));
	g_assert_cmpint(json_node_get_int(id), ==, 7);
}

static void
test_protocol_tools_list(void)
{
	g_autoptr(VentureMcpServer) server = NULL;
	g_autoptr(JsonNode) request = NULL;
	g_autoptr(JsonNode) response = NULL;
	JsonObject *result;
	JsonArray *tools;

	server = server_with_fixture("schema.json", NULL);
	request = rpc_request("tools/list", NULL);
	response = venture_mcp_server_handle(server, request);

	result = json_object_get_object_member(json_node_get_object(response),
	                                       "result");
	tools = json_object_get_array_member(result, "tools");

	g_assert_cmpuint(json_array_get_length(tools), ==, 16);
}

/*
 * The dashboards and the factory reach an agent through the same verbs a
 * person has: list and read a dashboard, read the catalogue and the
 * templates, build one, read the loop, draft a changelog. Each maps to
 * one route, and the writes refuse -- naming the alternative -- while
 * staging is on, because a whole dashboard or a tag on the forge cannot
 * wait in the queue.
 */
static void
test_dashboards_and_factory_tools(void)
{
	g_autoptr(VentureMcpServer) server = NULL;
	MockTransport *mock;

	server = server_with_fixture_staging("schema.json", TRUE, &mock);
	mock_transport_answer(mock, 200, "application/json", "[]");

	{
		g_autoptr(JsonNode) params = NULL;
		g_autoptr(JsonNode) request = NULL;
		g_autoptr(JsonNode) response = NULL;
		gboolean is_error;

		params = call_params("venture_dashboards", NULL);
		request = rpc_request("tools/call", g_steal_pointer(&params));
		response = venture_mcp_server_handle(server, request);
		result_text(response, &is_error);
		g_assert_false(is_error);
		g_assert_cmpstr(mock->last_path, ==, "/api/v1/dashboards");

		g_clear_pointer(&request, json_node_unref);
		g_clear_pointer(&response, json_node_unref);
		params = call_params("venture_dashboards", "what", "kinds", NULL);
		request = rpc_request("tools/call", g_steal_pointer(&params));
		response = venture_mcp_server_handle(server, request);
		result_text(response, &is_error);
		g_assert_false(is_error);
		g_assert_cmpstr(mock->last_path, ==, "/api/v1/widget-kinds");

		g_clear_pointer(&request, json_node_unref);
		g_clear_pointer(&response, json_node_unref);
		params = call_params("venture_dashboards", "what", "templates", NULL);
		request = rpc_request("tools/call", g_steal_pointer(&params));
		response = venture_mcp_server_handle(server, request);
		result_text(response, &is_error);
		g_assert_false(is_error);
		g_assert_cmpstr(mock->last_path, ==, "/api/v1/dashboard-templates");
	}

	mock_transport_answer(mock, 200, "application/json", "{\"widgets\": []}");

	{
		g_autoptr(JsonNode) params = NULL;
		g_autoptr(JsonNode) request = NULL;
		g_autoptr(JsonNode) response = NULL;
		gboolean is_error;

		params = call_params("venture_dashboard", "slug", "factory", NULL);
		request = rpc_request("tools/call", g_steal_pointer(&params));
		response = venture_mcp_server_handle(server, request);
		result_text(response, &is_error);
		g_assert_false(is_error);
		g_assert_cmpstr(mock->last_path, ==, "/api/v1/dashboards/factory");

		g_clear_pointer(&request, json_node_unref);
		g_clear_pointer(&response, json_node_unref);
		params = call_params("venture_dashboard", "slug", "factory",
		                     "mode", "definition", NULL);
		request = rpc_request("tools/call", g_steal_pointer(&params));
		response = venture_mcp_server_handle(server, request);
		result_text(response, &is_error);
		g_assert_false(is_error);
		g_assert_cmpstr(mock->last_path, ==,
		                "/api/v1/dashboards/factory/export");

		g_clear_pointer(&request, json_node_unref);
		g_clear_pointer(&response, json_node_unref);
		params = call_params("venture_dashboard", "slug", "factory",
		                     "mode", "settings", NULL);
		request = rpc_request("tools/call", g_steal_pointer(&params));
		response = venture_mcp_server_handle(server, request);
		result_text(response, &is_error);
		g_assert_false(is_error);
		g_assert_cmpstr(mock->last_path, ==,
		                "/api/v1/dashboards/factory?data=0");
	}

	/* Staging is on: building refuses without sending, and says what to
	 * do instead. */
	{
		g_autoptr(JsonNode) params = NULL;
		g_autoptr(JsonNode) request = NULL;
		g_autoptr(JsonNode) response = NULL;
		const gchar *text;
		gboolean is_error;
		guint calls_before;

		calls_before = mock->calls;
		params = call_params("venture_dashboard_build", "template", "factory",
		                     NULL);
		request = rpc_request("tools/call", g_steal_pointer(&params));
		response = venture_mcp_server_handle(server, request);
		text = result_text(response, &is_error);
		g_assert_true(is_error);
		g_assert_nonnull(g_strstr_len(text, -1, "--apply-writes"));
		g_assert_nonnull(g_strstr_len(text, -1, "dashboard_widget"));
		g_assert_cmpuint(mock->calls, ==, calls_before);

		g_clear_pointer(&request, json_node_unref);
		g_clear_pointer(&response, json_node_unref);
		params = call_params("venture_factory", "action", "publish", "id", "3",
		                     NULL);
		request = rpc_request("tools/call", g_steal_pointer(&params));
		response = venture_mcp_server_handle(server, request);
		text = result_text(response, &is_error);
		g_assert_true(is_error);
		g_assert_nonnull(g_strstr_len(text, -1, "Publish"));
		g_assert_cmpuint(mock->calls, ==, calls_before);
	}

	/* Status is a read and always goes. */
	{
		g_autoptr(JsonNode) params = NULL;
		g_autoptr(JsonNode) request = NULL;
		g_autoptr(JsonNode) response = NULL;
		gboolean is_error;

		params = call_params("venture_factory", NULL);
		request = rpc_request("tools/call", g_steal_pointer(&params));
		response = venture_mcp_server_handle(server, request);
		result_text(response, &is_error);
		g_assert_false(is_error);
		g_assert_cmpstr(mock->last_method, ==, "GET");
		g_assert_cmpstr(mock->last_path, ==, "/api/v1/factory");
	}

	/* With writes applied, both go to their routes with their flag. */
	venture_mcp_server_set_stage_writes(server, FALSE);

	{
		g_autoptr(JsonNode) params = NULL;
		g_autoptr(JsonNode) request = NULL;
		g_autoptr(JsonNode) response = NULL;
		gboolean is_error;

		params = call_params("venture_dashboard_build", "template", "work",
		                     NULL);
		request = rpc_request("tools/call", g_steal_pointer(&params));
		response = venture_mcp_server_handle(server, request);
		result_text(response, &is_error);
		g_assert_false(is_error);
		g_assert_cmpstr(mock->last_method, ==, "POST");
		g_assert_cmpstr(mock->last_path, ==,
		                "/api/v1/dashboards/from-template");
		g_assert_nonnull(g_strstr_len(mock->last_body, -1, "\"work\""));

		g_clear_pointer(&request, json_node_unref);
		g_clear_pointer(&response, json_node_unref);
		params = call_params("venture_factory", "action", "changelog",
		                     "id", "3", NULL);
		request = rpc_request("tools/call", g_steal_pointer(&params));
		response = venture_mcp_server_handle(server, request);
		result_text(response, &is_error);
		g_assert_false(is_error);
		g_assert_cmpstr(mock->last_path, ==, "/api/v1/releases/3/changelog");
		g_assert_nonnull(g_strstr_len(mock->last_body, -1, "\"replace\""));
	}

	/* A link is an ordinary write: staged when staging is on. */
	venture_mcp_server_set_stage_writes(server, TRUE);
	mock_transport_answer(mock, 202, "application/json",
	                      "{\"staged\": true, \"confirmation\": {\"id\": \"c1\"}}");

	{
		g_autoptr(JsonNode) params = NULL;
		g_autoptr(JsonNode) request = NULL;
		g_autoptr(JsonNode) response = NULL;
		const gchar *text;
		gboolean is_error;

		params = call_params("venture_link", "source_type", "release",
		                     "source_id", "1", "target_type", "ticket",
		                     "target_id", "2", "kind", "produces", NULL);
		request = rpc_request("tools/call", g_steal_pointer(&params));
		response = venture_mcp_server_handle(server, request);
		text = result_text(response, &is_error);
		g_assert_false(is_error);
		g_assert_cmpstr(mock->last_path, ==, "/api/v1/links?stage=1");
		g_assert_nonnull(g_strstr_len(text, -1, "confirmation id: c1"));
	}
}

static void
test_protocol_unknown_tool(void)
{
	g_autoptr(VentureMcpServer) server = NULL;
	g_autoptr(JsonNode) params = NULL;
	g_autoptr(JsonNode) request = NULL;
	g_autoptr(JsonNode) response = NULL;
	JsonObject *root;

	server = server_with_fixture("schema.json", NULL);
	params = call_params("venture_drop_database", NULL);
	request = rpc_request("tools/call", g_steal_pointer(&params));
	response = venture_mcp_server_handle(server, request);

	root = json_node_get_object(response);

	g_assert_true(json_object_has_member(root, "error"));
}

/* --- Errors -------------------------------------------------------------- */

/*
 * A 401 is a bad token, never a missing record.
 *
 * This is the one that matters most. VENTURE answers an unauthenticated
 * request with an error body like any other, and the plausible handling --
 * decode it and report its message -- turns "your token is wrong" into
 * whatever the body says. An agent told a record does not exist goes and
 * creates a duplicate; an agent told its credential is wrong stops.
 */
static void
test_error_401_is_a_bad_token(void)
{
	g_autoptr(VentureMcpServer) server = NULL;
	g_autoptr(JsonNode) params = NULL;
	g_autoptr(JsonNode) request = NULL;
	g_autoptr(JsonNode) response = NULL;
	MockTransport *mock;
	const gchar *text;
	gboolean is_error;

	server = server_with_fixture("schema.json", &mock);

	/*
	 * A 401 whose body reads exactly like a missing record. VENTURE does
	 * not send this one, but a proxy in front of it can, and the point is
	 * that the body must not be what decides.
	 */
	mock_transport_answer(mock, 401, "application/json",
	                      "{\"error\": \"not_found\", "
	                      "\"message\": \"No such record: 42\", "
	                      "\"code\": 2}");

	params = call_params("venture_get", "type", "sale", "id", "42", NULL);
	request = rpc_request("tools/call", g_steal_pointer(&params));
	response = venture_mcp_server_handle(server, request);

	text = result_text(response, &is_error);

	g_assert_true(is_error);
	g_assert_nonnull(g_strstr_len(text, -1, "token"));
	g_assert_nonnull(g_strstr_len(text, -1, "401"));

	/* And it does not repeat the body's claim, which is the whole bug. */
	g_assert_null(g_strstr_len(text, -1, "No such record"));
}

/*
 * A real 404 still reads as a missing record, so the fix above did not just
 * relabel every failure as an authentication problem.
 */
static void
test_error_404_is_a_missing_record(void)
{
	g_autoptr(VentureMcpServer) server = NULL;
	g_autoptr(JsonNode) params = NULL;
	g_autoptr(JsonNode) request = NULL;
	g_autoptr(JsonNode) response = NULL;
	MockTransport *mock;
	const gchar *text;
	gboolean is_error;

	server = server_with_fixture("schema.json", &mock);
	mock_transport_answer(mock, 404, "application/json",
	                      "{\"error\": \"not_found\", "
	                      "\"message\": \"No such record: 42\", "
	                      "\"code\": 2}");

	params = call_params("venture_get", "type", "sale", "id", "42", NULL);
	request = rpc_request("tools/call", g_steal_pointer(&params));
	response = venture_mcp_server_handle(server, request);

	text = result_text(response, &is_error);

	g_assert_true(is_error);
	g_assert_nonnull(g_strstr_len(text, -1, "No such record"));
	g_assert_null(g_strstr_len(text, -1, "token"));
}

/*
 * A record type the schema never described is refused here, with the list of
 * what there is -- rather than being passed through to the REST path, where
 * it comes back as a 404 that looks like a missing record.
 */
static void
test_error_unknown_type_lists_the_real_ones(void)
{
	g_autoptr(VentureMcpServer) server = NULL;
	g_autoptr(JsonNode) params = NULL;
	g_autoptr(JsonNode) request = NULL;
	g_autoptr(JsonNode) response = NULL;
	MockTransport *mock;
	const gchar *text;
	gboolean is_error;
	guint calls_before;

	server = server_with_fixture("schema.json", &mock);
	calls_before = mock->calls;

	params = call_params("venture_list", "type", "invoice", NULL);
	request = rpc_request("tools/call", g_steal_pointer(&params));
	response = venture_mcp_server_handle(server, request);

	text = result_text(response, &is_error);

	g_assert_true(is_error);
	g_assert_nonnull(g_strstr_len(text, -1, "invoice"));
	g_assert_nonnull(g_strstr_len(text, -1, "sale"));

	/* And the server was never asked, so this cannot be mistaken for a
	 * record that is missing. */
	g_assert_cmpuint(mock->calls, ==, calls_before);
}

/*
 * A field name in the C spelling is refused rather than sent.
 *
 * VENTURE ignores a dashed member: the record saves and the value is not
 * there. That failure is invisible at every layer below this one.
 */
static void
test_error_dashed_field_name_refused(void)
{
	g_autoptr(VentureMcpServer) server = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) request = NULL;
	g_autoptr(JsonNode) response = NULL;
	MockTransport *mock;
	const gchar *text;
	gboolean is_error;
	guint calls_before;

	server = server_with_fixture("schema.json", &mock);
	venture_mcp_server_set_stage_writes(server, FALSE);
	calls_before = mock->calls;

	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "name");
	json_builder_add_string_value(builder, "venture_create");
	json_builder_set_member_name(builder, "arguments");
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "type");
	json_builder_add_string_value(builder, "sale");
	json_builder_set_member_name(builder, "values");
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "venture-id");
	json_builder_add_string_value(builder, "3");
	json_builder_end_object(builder);
	json_builder_end_object(builder);
	json_builder_end_object(builder);

	request = rpc_request("tools/call", json_builder_get_root(builder));
	response = venture_mcp_server_handle(server, request);

	text = result_text(response, &is_error);

	g_assert_true(is_error);
	g_assert_nonnull(g_strstr_len(text, -1, "venture_id"));
	g_assert_cmpuint(mock->calls, ==, calls_before);
}

/* --- Reports ------------------------------------------------------------- */

/*
 * venture_report says which content type came back.
 *
 * CSV read as JSON is nonsense that parses, so the answer has to name what
 * it is holding rather than leave the reader to infer it from the request.
 */
static void
test_report_states_csv(void)
{
	g_autoptr(VentureMcpServer) server = NULL;
	g_autoptr(JsonNode) params = NULL;
	g_autoptr(JsonNode) request = NULL;
	g_autoptr(JsonNode) response = NULL;
	MockTransport *mock;
	const gchar *text;
	gboolean is_error;

	server = server_with_fixture("schema.json", &mock);
	mock_transport_answer(mock, 200, "text/csv; charset=utf-8",
	                      "metric,value\ngross,1499\n");

	params = call_params("venture_report", "name", "pnl", "period",
	                     "this_quarter", "format", "csv", NULL);
	request = rpc_request("tools/call", g_steal_pointer(&params));
	response = venture_mcp_server_handle(server, request);

	text = result_text(response, &is_error);

	g_assert_false(is_error);
	g_assert_nonnull(g_strstr_len(text, -1, "CSV"));
	g_assert_nonnull(g_strstr_len(text, -1, "text/csv"));
	g_assert_nonnull(g_strstr_len(text, -1, "metric,value"));

	/* The request carried the format through, not just the label. */
	g_assert_nonnull(g_strstr_len(mock->last_path, -1, "format=csv"));
	g_assert_nonnull(g_strstr_len(mock->last_path, -1, "period=this_quarter"));
}

static void
test_report_states_json(void)
{
	g_autoptr(VentureMcpServer) server = NULL;
	g_autoptr(JsonNode) params = NULL;
	g_autoptr(JsonNode) request = NULL;
	g_autoptr(JsonNode) response = NULL;
	MockTransport *mock;
	const gchar *text;
	gboolean is_error;

	server = server_with_fixture("schema.json", &mock);
	mock_transport_answer(mock, 200, "application/json",
	                      "{\"metrics\": []}");

	params = call_params("venture_report", "name", "pnl", NULL);
	request = rpc_request("tools/call", g_steal_pointer(&params));
	response = venture_mcp_server_handle(server, request);

	text = result_text(response, &is_error);

	g_assert_false(is_error);
	g_assert_nonnull(g_strstr_len(text, -1, "JSON"));
	g_assert_null(g_strstr_len(mock->last_path, -1, "format=csv"));
}

/*
 * Asking for CSV and being handed JSON is reported as JSON.
 *
 * The classification comes from the response's own Content-Type, so a server
 * that ignored the format parameter -- an older build, a proxy that stripped
 * the query string -- cannot make this claim CSV.
 */
static void
test_report_reports_what_arrived_not_what_was_asked(void)
{
	g_autoptr(VentureMcpServer) server = NULL;
	g_autoptr(JsonNode) params = NULL;
	g_autoptr(JsonNode) request = NULL;
	g_autoptr(JsonNode) response = NULL;
	MockTransport *mock;
	const gchar *text;
	gboolean is_error;

	server = server_with_fixture("schema.json", &mock);
	mock_transport_answer(mock, 200, "application/json",
	                      "{\"metrics\": []}");

	params = call_params("venture_report", "name", "pnl", "format", "csv",
	                     NULL);
	request = rpc_request("tools/call", g_steal_pointer(&params));
	response = venture_mcp_server_handle(server, request);

	text = result_text(response, &is_error);

	g_assert_false(is_error);
	g_assert_nonnull(g_strstr_len(text, -1, "This body is JSON."));
}

/* --- Staging ------------------------------------------------------------- */

/*
 * A staged write against a server too old to stage sends nothing and says so.
 *
 * This is the fallback, and it is the behaviour every build had before the
 * server grew a staging route. It has to say plainly that nothing is queued:
 * "staged for approval" would send the reader to a queue that will never
 * hold this change, which is worse than reporting that the change was
 * described and not sent.
 */
static void
test_staged_write_sends_nothing(void)
{
	g_autoptr(VentureMcpServer) server = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) request = NULL;
	g_autoptr(JsonNode) response = NULL;
	MockTransport *mock;
	const gchar *text;
	gboolean is_error;
	guint calls_before;

	server = server_with_fixture("schema.json", &mock);
	g_assert_true(venture_mcp_server_get_stage_writes(server));
	calls_before = mock->calls;

	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "name");
	json_builder_add_string_value(builder, "venture_create");
	json_builder_set_member_name(builder, "arguments");
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "type");
	json_builder_add_string_value(builder, "expense");
	json_builder_set_member_name(builder, "values");
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "description");
	json_builder_add_string_value(builder, "Cover art");
	json_builder_set_member_name(builder, "amount");
	json_builder_add_string_value(builder, "250.00");
	json_builder_end_object(builder);
	json_builder_end_object(builder);
	json_builder_end_object(builder);

	request = rpc_request("tools/call", json_builder_get_root(builder));
	response = venture_mcp_server_handle(server, request);

	text = result_text(response, &is_error);

	/* Not an error: the tool did what it was configured to do. */
	g_assert_false(is_error);
	g_assert_nonnull(g_strstr_len(text, -1, "Not applied"));
	g_assert_nonnull(g_strstr_len(text, -1, "POST /api/v1/expense"));
	g_assert_nonnull(g_strstr_len(text, -1, "Cover art"));

	/* It must not claim anything is waiting for a decision. */
	g_assert_nonnull(g_strstr_len(text, -1, "Nothing is queued"));
	g_assert_null(g_strstr_len(text, -1, "confirmation id"));

	/* And nothing crossed the wire. */
	g_assert_cmpuint(mock->calls, ==, calls_before);
}

/*
 * Against a server that can stage, the write goes out with `?stage=1` and
 * the answer names the confirmation.
 *
 * What breaks if this regresses: the whole point of the server-side queue.
 * A hold that never leaves the process is only as good as the agent's
 * willingness to report it; a confirmation on the server is something a
 * person can find without the agent's cooperation.
 */
static void
test_staged_write_reaches_the_queue(void)
{
	g_autoptr(VentureMcpServer) server = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) request = NULL;
	g_autoptr(JsonNode) response = NULL;
	MockTransport *mock;
	const gchar *text;
	gboolean is_error;

	server = server_with_fixture_staging("schema.json", TRUE, &mock);
	g_assert_true(venture_mcp_server_get_stage_writes(server));

	mock_transport_answer(mock, 202, "application/json",
		"{\"status\": \"awaiting_approval\", \"staged\": true, "
		"\"confirmation\": {\"id\": \"a3f9c118\", \"type\": \"expense\", "
		"\"action\": \"create\"}}");

	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "name");
	json_builder_add_string_value(builder, "venture_create");
	json_builder_set_member_name(builder, "arguments");
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "type");
	json_builder_add_string_value(builder, "expense");
	json_builder_set_member_name(builder, "values");
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "description");
	json_builder_add_string_value(builder, "Cover art");
	json_builder_end_object(builder);
	json_builder_end_object(builder);
	json_builder_end_object(builder);

	request = rpc_request("tools/call", json_builder_get_root(builder));
	response = venture_mcp_server_handle(server, request);

	text = result_text(response, &is_error);
	g_assert_false(is_error);

	/* The parameter really went out. */
	g_assert_cmpstr(mock->last_method, ==, "POST");
	g_assert_cmpstr(mock->last_path, ==, "/api/v1/expense?stage=1");

	/* And the answer says where the change is waiting, by id. */
	g_assert_nonnull(g_strstr_len(text, -1, "a3f9c118"));
	g_assert_nonnull(g_strstr_len(text, -1, "confirmation id"));
	g_assert_nonnull(g_strstr_len(text, -1, "Not applied yet"));
	g_assert_nonnull(g_strstr_len(text, -1,
		"/api/v1/confirmations/a3f9c118/approve"));

	/* It must not repeat the old claim that nothing is queued: there is
	 * something queued, and telling the agent otherwise stops it saying
	 * so. */
	g_assert_null(g_strstr_len(text, -1, "Nothing is queued"));
}

/*
 * A staged write that came back applied is reported as applied.
 *
 * What breaks if this regresses: the one lie that matters here. A proxy that
 * strips the query string, or a server that advertised staging and then did
 * not do it, answers with the record it just wrote. Classifying from the
 * request rather than the reply would report a change as waiting for
 * approval when it is already in the books, and nobody would go looking.
 */
static void
test_staged_write_does_not_claim_a_queue_it_cannot_see(void)
{
	g_autoptr(VentureMcpServer) server = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) request = NULL;
	g_autoptr(JsonNode) response = NULL;
	MockTransport *mock;
	const gchar *text;
	gboolean is_error;

	server = server_with_fixture_staging("schema.json", TRUE, &mock);

	/* What a server that ignored the parameter answers: the record. */
	mock_transport_answer(mock, 201, "application/json",
		"{\"id\": 12, \"description\": \"Cover art\"}");

	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "name");
	json_builder_add_string_value(builder, "venture_create");
	json_builder_set_member_name(builder, "arguments");
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "type");
	json_builder_add_string_value(builder, "expense");
	json_builder_set_member_name(builder, "values");
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "description");
	json_builder_add_string_value(builder, "Cover art");
	json_builder_end_object(builder);
	json_builder_end_object(builder);
	json_builder_end_object(builder);

	request = rpc_request("tools/call", json_builder_get_root(builder));
	response = venture_mcp_server_handle(server, request);

	text = result_text(response, &is_error);
	g_assert_false(is_error);

	g_assert_null(g_strstr_len(text, -1, "Not applied"));
	g_assert_null(g_strstr_len(text, -1, "confirmation id"));
	g_assert_nonnull(g_strstr_len(text, -1, "\"id\""));
}

/*
 * `--apply-writes` sends no parameter at all, against a staging server as
 * much as against an old one.
 */
static void
test_applied_write_sends_no_stage_parameter(void)
{
	g_autoptr(VentureMcpServer) server = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) request = NULL;
	g_autoptr(JsonNode) response = NULL;
	MockTransport *mock;

	server = server_with_fixture_staging("schema.json", TRUE, &mock);
	venture_mcp_server_set_stage_writes(server, FALSE);
	mock_transport_answer(mock, 200, "application/json", "{\"id\": 12}");

	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "name");
	json_builder_add_string_value(builder, "venture_delete");
	json_builder_set_member_name(builder, "arguments");
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "type");
	json_builder_add_string_value(builder, "expense");
	json_builder_set_member_name(builder, "id");
	json_builder_add_int_value(builder, 12);
	json_builder_end_object(builder);
	json_builder_end_object(builder);

	request = rpc_request("tools/call", json_builder_get_root(builder));
	response = venture_mcp_server_handle(server, request);

	g_assert_nonnull(response);
	g_assert_cmpstr(mock->last_method, ==, "DELETE");
	g_assert_cmpstr(mock->last_path, ==, "/api/v1/expense/12");
}

/*
 * With staging off the same call reaches the server, at the plural-resolved
 * path and with the values it was given.
 */
static void
test_applied_write_reaches_the_server(void)
{
	g_autoptr(VentureMcpServer) server = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) request = NULL;
	g_autoptr(JsonNode) response = NULL;
	MockTransport *mock;
	gboolean is_error;

	server = server_with_fixture("schema.json", &mock);
	venture_mcp_server_set_stage_writes(server, FALSE);
	mock_transport_answer(mock, 200, "application/json", "{\"id\": 12}");

	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "name");
	json_builder_add_string_value(builder, "venture_create");
	json_builder_set_member_name(builder, "arguments");
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "type");
	/* The plural, to prove the path is built from the canonical name. */
	json_builder_add_string_value(builder, "expenses");
	json_builder_set_member_name(builder, "values");
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "description");
	json_builder_add_string_value(builder, "Cover art");
	json_builder_end_object(builder);
	json_builder_end_object(builder);
	json_builder_end_object(builder);

	request = rpc_request("tools/call", json_builder_get_root(builder));
	response = venture_mcp_server_handle(server, request);

	result_text(response, &is_error);

	g_assert_false(is_error);
	g_assert_cmpstr(mock->last_method, ==, "POST");
	g_assert_cmpstr(mock->last_path, ==, "/api/v1/expense");
	g_assert_nonnull(g_strstr_len(mock->last_body, -1, "Cover art"));
}

/* --- Listing ------------------------------------------------------------- */

/*
 * Filters and the reserved query-string names both reach the URL.
 *
 * `search`, `order`, `limit`, `offset`, `period` and `period_field` are
 * reserved in venture_query_apply_query_string(); anything else is parsed as
 * a field filter and refused if the field does not exist. A reserved name
 * sent as a filter would break the whole request with "no field named
 * limit".
 */
static void
test_list_builds_the_query(void)
{
	g_autoptr(VentureMcpServer) server = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) request = NULL;
	g_autoptr(JsonNode) response = NULL;
	MockTransport *mock;
	gboolean is_error;

	server = server_with_fixture("schema.json", &mock);
	mock_transport_answer(mock, 200, "application/json",
	                      "{\"records\": [], \"total\": 0}");

	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "name");
	json_builder_add_string_value(builder, "venture_list");
	json_builder_set_member_name(builder, "arguments");
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "type");
	json_builder_add_string_value(builder, "sales");
	json_builder_set_member_name(builder, "filters");
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "venture_id__eq");
	json_builder_add_int_value(builder, 3);
	json_builder_end_object(builder);
	json_builder_set_member_name(builder, "limit");
	json_builder_add_int_value(builder, 10);
	json_builder_set_member_name(builder, "period");
	json_builder_add_string_value(builder, "this_month");
	json_builder_end_object(builder);
	json_builder_end_object(builder);

	request = rpc_request("tools/call", json_builder_get_root(builder));
	response = venture_mcp_server_handle(server, request);

	result_text(response, &is_error);

	g_assert_false(is_error);
	g_assert_cmpstr(mock->last_method, ==, "GET");

	/* The plural resolved to the singular path. */
	g_assert_true(g_str_has_prefix(mock->last_path, "/api/v1/sale?"));
	g_assert_nonnull(g_strstr_len(mock->last_path, -1, "venture_id__eq=3"));
	g_assert_nonnull(g_strstr_len(mock->last_path, -1, "limit=10"));
	g_assert_nonnull(g_strstr_len(mock->last_path, -1, "period=this_month"));
}

/*
 * venture_schema is answered from the catalog, without asking the server
 * again -- so it cannot disagree with the type list the tools carry.
 */
static void
test_schema_tool_costs_no_request(void)
{
	g_autoptr(VentureMcpServer) server = NULL;
	g_autoptr(JsonNode) params = NULL;
	g_autoptr(JsonNode) request = NULL;
	g_autoptr(JsonNode) response = NULL;
	MockTransport *mock;
	const gchar *text;
	gboolean is_error;
	guint calls_before;

	server = server_with_fixture("schema.json", &mock);
	calls_before = mock->calls;

	params = call_params("venture_schema", NULL);
	request = rpc_request("tools/call", g_steal_pointer(&params));
	response = venture_mcp_server_handle(server, request);

	text = result_text(response, &is_error);

	g_assert_false(is_error);
	g_assert_nonnull(g_strstr_len(text, -1, "sale"));
	g_assert_nonnull(g_strstr_len(text, -1, "expense"));
	g_assert_cmpuint(mock->calls, ==, calls_before);
}

/* --- Confirmations ------------------------------------------------------- */

static void
test_confirmations_endpoints(void)
{
	g_autoptr(VentureMcpServer) server = NULL;
	MockTransport *mock;

	server = server_with_fixture("schema.json", &mock);
	mock_transport_answer(mock, 200, "application/json", "[]");

	{
		g_autoptr(JsonNode) params = NULL;
		g_autoptr(JsonNode) request = NULL;
		g_autoptr(JsonNode) response = NULL;
		gboolean is_error;

		params = call_params("venture_confirmations", NULL);
		request = rpc_request("tools/call", g_steal_pointer(&params));
		response = venture_mcp_server_handle(server, request);

		result_text(response, &is_error);

		g_assert_false(is_error);
		g_assert_cmpstr(mock->last_method, ==, "GET");
		g_assert_cmpstr(mock->last_path, ==, "/api/v1/confirmations");
	}

	mock_transport_answer(mock, 200, "application/json", "{\"applied\": true}");

	{
		g_autoptr(JsonNode) params = NULL;
		g_autoptr(JsonNode) request = NULL;
		g_autoptr(JsonNode) response = NULL;
		gboolean is_error;

		params = call_params("venture_confirmations", "action", "approve",
		                     "id", "abc-123", NULL);
		request = rpc_request("tools/call", g_steal_pointer(&params));
		response = venture_mcp_server_handle(server, request);

		result_text(response, &is_error);

		g_assert_false(is_error);
		g_assert_cmpstr(mock->last_method, ==, "POST");
		g_assert_cmpstr(mock->last_path, ==,
		                "/api/v1/confirmations/abc-123/approve");
	}

	/* Approving without saying which is refused rather than sent. */
	{
		g_autoptr(JsonNode) params = NULL;
		g_autoptr(JsonNode) request = NULL;
		g_autoptr(JsonNode) response = NULL;
		const gchar *text;
		gboolean is_error;
		guint calls_before;

		calls_before = mock->calls;
		params = call_params("venture_confirmations", "action", "reject",
		                     NULL);
		request = rpc_request("tools/call", g_steal_pointer(&params));
		response = venture_mcp_server_handle(server, request);

		text = result_text(response, &is_error);

		g_assert_true(is_error);
		g_assert_nonnull(g_strstr_len(text, -1, "id"));
		g_assert_cmpuint(mock->calls, ==, calls_before);
	}
}

/* --- Entry point --------------------------------------------------------- */

int
main(
	int	  argc,
	char	**argv
){
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/mcp/catalog/from-schema", test_catalog_from_schema);
	g_test_add_func("/mcp/catalog/new-type-needs-no-code",
	                test_catalog_new_type_needs_no_code);
	g_test_add_func("/mcp/catalog/singular-and-plural-are-one",
	                test_catalog_singular_and_plural_are_one);
	g_test_add_func("/mcp/catalog/malformed-schema-refused",
	                test_catalog_malformed_schema_refused);
	g_test_add_func("/mcp/catalog/describes-wire-spelling",
	                test_catalog_describes_wire_spelling);

	g_test_add_func("/mcp/server/refuses-without-token",
	                test_server_refuses_without_token);
	g_test_add_func("/mcp/server/stages-by-default",
	                test_server_stages_by_default);

	g_test_add_func("/mcp/protocol/initialize", test_protocol_initialize);
	g_test_add_func("/mcp/protocol/notification-unanswered",
	                test_protocol_notification_unanswered);
	g_test_add_func("/mcp/protocol/id-keeps-its-type",
	                test_protocol_id_keeps_its_type);
	g_test_add_func("/mcp/protocol/tools-list", test_protocol_tools_list);
	g_test_add_func("/mcp/tools/dashboards-and-factory",
	                test_dashboards_and_factory_tools);
	g_test_add_func("/mcp/protocol/unknown-tool", test_protocol_unknown_tool);

	g_test_add_func("/mcp/error/401-is-a-bad-token",
	                test_error_401_is_a_bad_token);
	g_test_add_func("/mcp/error/404-is-a-missing-record",
	                test_error_404_is_a_missing_record);
	g_test_add_func("/mcp/error/unknown-type-lists-the-real-ones",
	                test_error_unknown_type_lists_the_real_ones);
	g_test_add_func("/mcp/error/dashed-field-name-refused",
	                test_error_dashed_field_name_refused);

	g_test_add_func("/mcp/report/states-csv", test_report_states_csv);
	g_test_add_func("/mcp/report/states-json", test_report_states_json);
	g_test_add_func("/mcp/report/reports-what-arrived",
	                test_report_reports_what_arrived_not_what_was_asked);

	g_test_add_func("/mcp/staging/sends-nothing",
	                test_staged_write_sends_nothing);
	g_test_add_func("/mcp/staging/reaches-the-queue",
	                test_staged_write_reaches_the_queue);
	g_test_add_func("/mcp/staging/does-not-claim-a-queue-it-cannot-see",
	                test_staged_write_does_not_claim_a_queue_it_cannot_see);
	g_test_add_func("/mcp/staging/apply-sends-no-parameter",
	                test_applied_write_sends_no_stage_parameter);
	g_test_add_func("/mcp/staging/applied-write-reaches-the-server",
	                test_applied_write_reaches_the_server);

	g_test_add_func("/mcp/list/builds-the-query", test_list_builds_the_query);
	g_test_add_func("/mcp/schema/costs-no-request",
	                test_schema_tool_costs_no_request);
	g_test_add_func("/mcp/confirmations/endpoints",
	                test_confirmations_endpoints);

	return g_test_run();
}
