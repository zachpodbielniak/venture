/*
 * venture-mcp-server.c - `venturectl mcp`, a stdio MCP server over the API
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */


#include "mcp/venture-mcp-server.h"

#include "venture-error.h"
#include "venture-version.h"
#include "util/venture-json-util.h"

#include <libsoup/soup.h>

#include <stdio.h>
#include <string.h>

/*
 * The protocol revision this server implements. An MCP client that asks for
 * a different one is answered with this rather than refused: the handshake
 * is a negotiation, and every method used here has been stable across
 * revisions.
 */
#define VENTURE_MCP_PROTOCOL_VERSION "2025-06-18"

/* JSON-RPC 2.0 error codes. */
#define VENTURE_JSONRPC_PARSE_ERROR	(-32700)
#define VENTURE_JSONRPC_INVALID_REQUEST	(-32600)
#define VENTURE_JSONRPC_METHOD_NOT_FOUND (-32601)
#define VENTURE_JSONRPC_INVALID_PARAMS	(-32602)
#define VENTURE_JSONRPC_INTERNAL_ERROR	(-32603)

struct _VentureMcpServer
{
	GObject			 parent_instance;

	gchar			*base_url;
	gchar			*token;
	gboolean		 stage_writes;

	/*
	 * Whether the VENTURE server on the other end can stage a write.
	 * Read from /api/v1/health at startup rather than assumed: `?stage=1`
	 * against a build that predates staging is an unknown query parameter
	 * on a write route, which is ignored, and the change is applied. A
	 * client that guessed would send exactly the write it meant to hold.
	 */
	gboolean		 server_stages;

	SoupSession		*session;
	VentureMcpCatalog	*catalog;

	VentureMcpTransportFunc	 transport;
	gpointer		 transport_data;
	GDestroyNotify		 transport_notify;
};

G_DEFINE_TYPE(VentureMcpServer, venture_mcp_server, G_TYPE_OBJECT)

/* --- GObject ------------------------------------------------------------- */

static void
venture_mcp_server_finalize(GObject *object)
{
	VentureMcpServer *self;

	self = VENTURE_MCP_SERVER(object);

	if ((NULL != self->transport_notify) && (NULL != self->transport_data))
		self->transport_notify(self->transport_data);

	g_clear_pointer(&self->base_url, g_free);

	/*
	 * The token is wiped rather than merely freed. It is the one value in
	 * this process worth reading out of a core dump.
	 */
	if (NULL != self->token)
	{
		memset(self->token, 0, strlen(self->token));
		g_clear_pointer(&self->token, g_free);
	}

	g_clear_object(&self->session);
	g_clear_object(&self->catalog);

	G_OBJECT_CLASS(venture_mcp_server_parent_class)->finalize(object);
}

static void
venture_mcp_server_class_init(VentureMcpServerClass *klass)
{
	GObjectClass *object_class;

	object_class = G_OBJECT_CLASS(klass);
	object_class->finalize = venture_mcp_server_finalize;
}

static void
venture_mcp_server_init(VentureMcpServer *self)
{
	/* On by default, matching VENTURE's own `confirm_writes` policy. A
	 * hallucinated tool call that rewrites an expense is a tax problem
	 * rather than a UI annoyance. */
	self->stage_writes = TRUE;
}

/* --- Construction -------------------------------------------------------- */

VentureMcpServer *
venture_mcp_server_new(
	const gchar	 *base_url,
	const gchar	 *token,
	GError		**error
){
	g_autoptr(VentureMcpServer) self = NULL;
	const gchar *resolved_url;
	const gchar *resolved_token;

	self = g_object_new(VENTURE_TYPE_MCP_SERVER, NULL);

	resolved_url = (NULL != base_url) ? base_url : g_getenv("VENTURE_URL");

	/*
	 * VENTURE_SERVER is what the rest of venturectl reads. Honouring it
	 * here too costs one line and removes a trap: the same binary reading
	 * a different variable for one subcommand would send somebody who has
	 * set VENTURE_SERVER to a localhost that is not their server, and
	 * answer every question about somebody else's books.
	 */
	if ((NULL == resolved_url) || ('\0' == resolved_url[0]))
		resolved_url = g_getenv("VENTURE_SERVER");

	if ((NULL == resolved_url) || ('\0' == resolved_url[0]))
		resolved_url = "http://localhost:8747";

	self->base_url = g_strdup(resolved_url);

	/* A trailing slash would produce double-slashed paths, which some
	 * routers treat as a different route entirely. */
	while (g_str_has_suffix(self->base_url, "/") &&
	       (strlen(self->base_url) > 1))
		self->base_url[strlen(self->base_url) - 1] = '\0';

	resolved_token = (NULL != token) ? token : g_getenv("VENTURE_TOKEN");

	/*
	 * Refused here, not at the first tool call.
	 *
	 * Without this the server starts, `tools/list` succeeds because the
	 * schema endpoint is the first thing to 401, and every subsequent
	 * call fails identically. An agent reading a wall of 401s concludes
	 * the records are missing and goes to create them again.
	 */
	if ((NULL == resolved_token) || ('\0' == resolved_token[0]))
	{
		g_set_error_literal(error, VENTURE_ERROR,
		                    VENTURE_ERROR_UNAUTHENTICATED,
		                    "No VENTURE_TOKEN is set, so there is nothing to "
		                    "authenticate with.\n"
		                    "Mint a token with POST /api/v1/tokens while "
		                    "holding a session -- sign in to the web UI and "
		                    "create one there. The plaintext is returned "
		                    "exactly once and only its hash is kept, so copy "
		                    "it then.\n"
		                    "Put it in the environment as VENTURE_TOKEN. It "
		                    "is deliberately not a command-line flag: an "
		                    "argv is world-readable through /proc and lands "
		                    "in the shell's history.");
		return NULL;
	}

	self->token = g_strdup(resolved_token);
	self->session = soup_session_new();

	return g_steal_pointer(&self);
}

void
venture_mcp_server_set_transport(
	VentureMcpServer	*self,
	VentureMcpTransportFunc	 func,
	gpointer		 user_data,
	GDestroyNotify		 notify
){
	g_return_if_fail(VENTURE_IS_MCP_SERVER(self));

	if ((NULL != self->transport_notify) && (NULL != self->transport_data))
		self->transport_notify(self->transport_data);

	self->transport = func;
	self->transport_data = user_data;
	self->transport_notify = notify;
}

void
venture_mcp_server_set_stage_writes(
	VentureMcpServer	*self,
	gboolean		 stage_writes
){
	g_return_if_fail(VENTURE_IS_MCP_SERVER(self));

	self->stage_writes = stage_writes;
}

gboolean
venture_mcp_server_get_stage_writes(VentureMcpServer *self)
{
	g_return_val_if_fail(VENTURE_IS_MCP_SERVER(self), FALSE);

	return self->stage_writes;
}

VentureMcpCatalog *
venture_mcp_server_get_catalog(VentureMcpServer *self)
{
	g_return_val_if_fail(VENTURE_IS_MCP_SERVER(self), NULL);

	return self->catalog;
}

/* --- HTTP ---------------------------------------------------------------- */

/*
 * The built-in transport. Used unless a caller injected another one.
 */
static gchar *
venture_mcp_server_http(
	VentureMcpServer	 *self,
	const gchar		 *method,
	const gchar		 *path,
	JsonNode		 *body,
	guint			 *out_status,
	gchar			**out_content_type,
	GError			**error
){
	g_autoptr(SoupMessage) message = NULL;
	g_autoptr(GError) local_error = NULL;
	g_autoptr(GBytes) response = NULL;
	g_autofree gchar *url = NULL;
	g_autofree gchar *authorization = NULL;

	url = g_strconcat(self->base_url, path, NULL);
	message = soup_message_new(method, url);

	if (NULL == message)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "\"%s\" is not a usable URL", url);
		return NULL;
	}

	/* In a header, never in the argv or the URL: a query string reaches
	 * the server's access log and every proxy in between. */
	authorization = g_strconcat("Bearer ", self->token, NULL);
	soup_message_headers_append(soup_message_get_request_headers(message),
	                            "Authorization", authorization);

	if (NULL != body)
	{
		g_autofree gchar *encoded = NULL;
		g_autoptr(GBytes) bytes = NULL;

		encoded = venture_json_to_string(body, FALSE);
		bytes = g_bytes_new(encoded, strlen(encoded));
		soup_message_set_request_body_from_bytes(message, "application/json",
		                                         bytes);
	}

	response = soup_session_send_and_read(self->session, message, NULL,
	                                      &local_error);

	if (NULL == response)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NETWORK,
		            "Cannot reach %s: %s", self->base_url,
		            local_error->message);
		return NULL;
	}

	*out_status = soup_message_get_status(message);

	if (NULL != out_content_type)
	{
		const gchar *content_type;

		content_type = soup_message_headers_get_one(
			soup_message_get_response_headers(message), "Content-Type");
		*out_content_type = g_strdup(content_type);
	}

	return g_strndup(g_bytes_get_data(response, NULL),
	                 g_bytes_get_size(response));
}

/*
 * Turns a failing HTTP status into a #GError, in one place.
 *
 * The 401 case is the reason this is a function rather than a check at each
 * call site. VENTURE answers a request it cannot authenticate with a 401
 * whose body reads like any other error, and the plausible handling --
 * decode the body, report its message -- turns "your token is wrong" into
 * whatever the body happens to say. An agent told a record does not exist
 * goes and creates a duplicate; an agent told its token is wrong stops.
 * So a 401 is answered from the status, before the body is consulted at all.
 */
static void
venture_mcp_error_from_response(
	guint		  status,
	const gchar	 *body,
	GError		**error
){
	g_autoptr(JsonNode) node = NULL;
	JsonObject *object = NULL;
	const gchar *slug;
	const gchar *message;
	VentureError code;

	if (401 == status)
	{
		g_set_error_literal(error, VENTURE_ERROR,
		                    VENTURE_ERROR_UNAUTHENTICATED,
		                    "VENTURE rejected the bearer token (HTTP 401). "
		                    "The token is wrong, revoked or expired -- this "
		                    "is not a record that does not exist, and "
		                    "nothing should be created in response to it. "
		                    "Mint a new one with POST /api/v1/tokens while "
		                    "holding a session and set VENTURE_TOKEN to it.");
		return;
	}

	if (403 == status)
	{
		g_set_error_literal(error, VENTURE_ERROR,
		                    VENTURE_ERROR_PERMISSION_DENIED,
		                    "VENTURE accepted the token but the account it "
		                    "belongs to may not do this (HTTP 403). Some "
		                    "record types -- users and API tokens among them "
		                    "-- are owner-only.");
		return;
	}

	node = (NULL != body) ? venture_json_parse(body, NULL) : NULL;

	if ((NULL != node) && JSON_NODE_HOLDS_OBJECT(node))
		object = json_node_get_object(node);

	slug = (NULL != object)
		? venture_json_object_get_string(object, "error", NULL) : NULL;
	message = (NULL != object)
		? venture_json_object_get_string(object, "message", NULL) : NULL;

	if ((NULL == slug) || !venture_error_from_slug(slug, &code))
		code = VENTURE_ERROR_FAILED;

	if (NULL != message)
	{
		g_set_error(error, VENTURE_ERROR, code, "%s (HTTP %u)", message,
		            status);
		return;
	}

	g_set_error(error, VENTURE_ERROR, code,
	            "The VENTURE server answered HTTP %u with nothing this "
	            "client could read.", status);
}

/*
 * One request, through whichever transport is in force, with the status
 * mapping applied. Returns the body only on success.
 */
static gchar *
venture_mcp_server_request_text(
	VentureMcpServer	 *self,
	const gchar		 *method,
	const gchar		 *path,
	JsonNode		 *body,
	gchar			**out_content_type,
	GError			**error
){
	g_autofree gchar *text = NULL;
	g_autofree gchar *content_type = NULL;
	guint status;

	status = 0;

	if (NULL != self->transport)
	{
		text = self->transport(method, path, body, &status, &content_type,
		                       self->transport_data, error);
	}
	else
	{
		text = venture_mcp_server_http(self, method, path, body, &status,
		                               &content_type, error);
	}

	if (NULL == text)
		return NULL;

	if (status >= 400)
	{
		venture_mcp_error_from_response(status, text, error);
		return NULL;
	}

	if (NULL != out_content_type)
		*out_content_type = g_steal_pointer(&content_type);

	return g_steal_pointer(&text);
}

/*
 * The same, decoded as JSON.
 */
static JsonNode *
venture_mcp_server_request(
	VentureMcpServer	 *self,
	const gchar		 *method,
	const gchar		 *path,
	JsonNode		 *body,
	GError			**error
){
	g_autofree gchar *text = NULL;
	JsonNode *node;

	text = venture_mcp_server_request_text(self, method, path, body, NULL,
	                                       error);

	if (NULL == text)
		return NULL;

	node = venture_json_parse(text, NULL);

	if (NULL == node)
	{
		g_set_error_literal(error, VENTURE_ERROR,
		                    VENTURE_ERROR_SERIALIZATION,
		                    "The VENTURE server returned something that is "
		                    "not JSON.");
		return NULL;
	}

	return node;
}

/* --- The catalog --------------------------------------------------------- */

gboolean
venture_mcp_server_load_catalog(
	VentureMcpServer	 *self,
	GError			**error
){
	g_autoptr(JsonNode) schema = NULL;
	g_autoptr(JsonNode) health = NULL;
	g_autoptr(VentureMcpCatalog) catalog = NULL;

	g_return_val_if_fail(VENTURE_IS_MCP_SERVER(self), FALSE);

	schema = venture_mcp_server_request(self, "GET", "/api/v1/schema", NULL,
	                                    error);

	if (NULL == schema)
		return FALSE;

	/*
	 * Ask once whether this server stages, rather than per write.
	 *
	 * A failure here is not fatal -- it means an older server, and the
	 * write tools fall back to describing the change without sending it,
	 * which is what they did before staging existed. What must not happen
	 * is claiming a change was queued on a server that never heard of the
	 * parameter and applied the write instead.
	 */
	health = venture_mcp_server_request(self, "GET", "/api/v1/health", NULL,
	                                    NULL);

	self->server_stages = (NULL != health) && JSON_NODE_HOLDS_OBJECT(health) &&
		json_object_get_boolean_member_with_default(
			json_node_get_object(health), "staged_writes", FALSE);

	catalog = venture_mcp_catalog_new_from_schema(schema, error);

	if (NULL == catalog)
		return FALSE;

	g_clear_object(&self->catalog);
	self->catalog = g_steal_pointer(&catalog);

	return TRUE;
}

/* --- Tool arguments ------------------------------------------------------ */

/*
 * Resolves the `type` argument to a canonical record type name.
 *
 * A name the schema never described is refused with the list of what there
 * is, because the alternative -- letting it through to the REST path -- gets
 * a 404 back, and a 404 is what a missing *record* looks like too.
 */
static const gchar *
venture_mcp_resolve_argument_type(
	VentureMcpServer	 *self,
	JsonObject		 *arguments,
	GError			**error
){
	g_auto(GStrv) names = NULL;
	g_autofree gchar *joined = NULL;
	const gchar *requested;
	const gchar *canonical;

	requested = (NULL != arguments)
		? venture_json_object_get_string(arguments, "type", NULL) : NULL;

	if ((NULL == requested) || ('\0' == requested[0]))
	{
		g_set_error_literal(error, VENTURE_ERROR,
		                    VENTURE_ERROR_INVALID_ARGUMENT,
		                    "Which record type? venture_schema lists them.");
		return NULL;
	}

	canonical = venture_mcp_catalog_resolve_type(self->catalog, requested);

	if (NULL != canonical)
		return canonical;

	names = venture_mcp_catalog_list_types(self->catalog);
	joined = g_strjoinv(", ", names);

	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
	            "There is no record type called \"%s\". This server holds: "
	            "%s.", requested, joined);

	return NULL;
}

/*
 * Reads the required numeric `id`.
 *
 * Accepts it as a number or as a string of digits: a model that has just
 * read an id out of a JSON body will sometimes hand it back quoted, and
 * refusing that teaches nothing.
 */
static gboolean
venture_mcp_resolve_argument_id(
	JsonObject	 *arguments,
	gint64		 *out_id,
	GError		**error
){
	JsonNode *node;

	node = (NULL != arguments) ? json_object_get_member(arguments, "id")
	                           : NULL;

	if ((NULL != node) && JSON_NODE_HOLDS_VALUE(node))
	{
		GType value_type;

		value_type = json_node_get_value_type(node);

		if (G_TYPE_INT64 == value_type)
		{
			*out_id = json_node_get_int(node);
			return TRUE;
		}

		if (G_TYPE_STRING == value_type)
		{
			const gchar *text;
			gchar *end;
			gint64 parsed;

			text = json_node_get_string(node);
			end = NULL;
			parsed = g_ascii_strtoll(text, &end, 10);

			if ((NULL != end) && ('\0' == *end) && (text != end))
			{
				*out_id = parsed;
				return TRUE;
			}
		}
	}

	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
	                    "Which record? Pass its numeric id.");

	return FALSE;
}

/*
 * Refuses a field name in the C spelling.
 *
 * Properties are `forge-id` in C and `forge_id` on the wire, and the server
 * ignores a dashed member rather than refusing it: the record saves and the
 * value is simply not there. That failure is invisible at every layer, so it
 * is caught here where the name is still attached to the caller that wrote
 * it.
 */
static gboolean
venture_mcp_check_wire_name(
	const gchar	 *name,
	GError		**error
){
	g_autofree gchar *suggestion = NULL;

	if (NULL == g_strstr_len(name, -1, "-"))
		return TRUE;

	suggestion = g_strdelimit(g_strdup(name), "-", '_');

	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
	            "\"%s\" is the C spelling of that field. The wire spelling "
	            "uses underscores: \"%s\". VENTURE ignores a dashed member "
	            "rather than refusing it, so the record would save without "
	            "the value.", name, suggestion);

	return FALSE;
}

/*
 * Turns the `values` argument into a request body.
 */
static JsonNode *
venture_mcp_values_body(
	JsonObject	 *arguments,
	GError		**error
){
	g_autoptr(JsonBuilder) builder = NULL;
	JsonObject *values;
	JsonNode *node;
	GList *members;
	GList *iter;

	node = (NULL != arguments) ? json_object_get_member(arguments, "values")
	                           : NULL;

	if ((NULL == node) || !JSON_NODE_HOLDS_OBJECT(node) ||
	    (NULL == json_node_get_object(node)))
	{
		g_set_error_literal(error, VENTURE_ERROR,
		                    VENTURE_ERROR_INVALID_ARGUMENT,
		                    "Pass the fields to write as `values`, an object "
		                    "of field name to value. venture_schema prints "
		                    "the field names.");
		return NULL;
	}

	values = json_node_get_object(node);
	members = json_object_get_members(values);

	if (NULL == members)
	{
		g_set_error_literal(error, VENTURE_ERROR,
		                    VENTURE_ERROR_INVALID_ARGUMENT,
		                    "`values` is empty, so there is nothing to "
		                    "write.");
		return NULL;
	}

	builder = json_builder_new();
	json_builder_begin_object(builder);

	for (iter = members; NULL != iter; iter = iter->next)
	{
		if (!venture_mcp_check_wire_name(iter->data, error))
		{
			g_list_free(members);
			return NULL;
		}

		json_builder_set_member_name(builder, iter->data);
		json_builder_add_value(builder,
			json_node_ref(json_object_get_member(values, iter->data)));
	}

	g_list_free(members);
	json_builder_end_object(builder);

	return json_builder_get_root(builder);
}

/*
 * Renders one JSON value as a query-string argument. A filter's value may
 * arrive as a number or a boolean; the server parses text either way.
 */
static gchar *
venture_mcp_query_value(JsonNode *node)
{
	if ((NULL == node) || JSON_NODE_HOLDS_NULL(node))
		return g_strdup("");

	if (JSON_NODE_HOLDS_VALUE(node) &&
	    (G_TYPE_STRING == json_node_get_value_type(node)))
		return g_strdup(json_node_get_string(node));

	return venture_json_to_string(node, FALSE);
}

/* --- Tools --------------------------------------------------------------- */

/*
 * venture_schema: the record types, or one of them in detail.
 *
 * Answered from the catalog rather than by asking the server again. The
 * catalog was built from the schema at startup, so this costs no request and
 * cannot disagree with the type list the tools were generated from.
 */
static gchar *
venture_mcp_tool_schema(
	VentureMcpServer	 *self,
	JsonObject		 *arguments,
	GError			**error
){
	g_autoptr(JsonNode) node = NULL;
	const gchar *requested;

	requested = (NULL != arguments)
		? venture_json_object_get_string(arguments, "type", NULL) : NULL;

	if ((NULL == requested) || ('\0' == requested[0]))
	{
		g_auto(GStrv) names = NULL;
		g_autoptr(GString) text = NULL;
		gsize i;

		names = venture_mcp_catalog_list_types(self->catalog);
		text = g_string_new("Record types on this VENTURE server. Both the "
		                    "singular and the plural address the same "
		                    "resource.\n\n");

		for (i = 0; NULL != names[i]; i++)
			g_string_append_printf(text, "  %s\n", names[i]);

		g_string_append(text,
			"\nCall venture_schema with a type to see its fields, what each "
			"reference points at and what each enumeration accepts.\n");

		return g_string_free(g_steal_pointer(&text), FALSE);
	}

	node = venture_mcp_catalog_describe_type(self->catalog, requested);

	if (NULL == node)
	{
		g_auto(GStrv) names = NULL;
		g_autofree gchar *joined = NULL;

		names = venture_mcp_catalog_list_types(self->catalog);
		joined = g_strjoinv(", ", names);

		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		            "There is no record type called \"%s\". This server "
		            "holds: %s.", requested, joined);
		return NULL;
	}

	return venture_json_to_string(node, TRUE);
}

static gchar *
venture_mcp_tool_list(
	VentureMcpServer	 *self,
	JsonObject		 *arguments,
	GError			**error
){
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GString) path = NULL;
	const gchar *type;
	gboolean first;

	type = venture_mcp_resolve_argument_type(self, arguments, error);

	if (NULL == type)
		return NULL;

	path = g_string_new("/api/v1/");
	g_string_append_uri_escaped(path, type, NULL, FALSE);
	first = TRUE;

	if (json_object_has_member(arguments, "filters"))
	{
		JsonNode *filters_node;

		filters_node = json_object_get_member(arguments, "filters");

		if (JSON_NODE_HOLDS_OBJECT(filters_node) &&
		    (NULL != json_node_get_object(filters_node)))
		{
			JsonObject *filters;
			GList *members;
			GList *iter;

			filters = json_node_get_object(filters_node);
			members = json_object_get_members(filters);

			for (iter = members; NULL != iter; iter = iter->next)
			{
				g_autofree gchar *value = NULL;

				if (!venture_mcp_check_wire_name(iter->data, error))
				{
					g_list_free(members);
					return NULL;
				}

				value = venture_mcp_query_value(
					json_object_get_member(filters, iter->data));

				g_string_append_c(path, first ? '?' : '&');
				first = FALSE;
				g_string_append_uri_escaped(path, iter->data, NULL, FALSE);
				g_string_append_c(path, '=');
				g_string_append_uri_escaped(path, value, NULL, FALSE);
			}

			g_list_free(members);
		}
	}

	{
		/*
		 * The reserved query-string names, spelled once.
		 *
		 * `search`, `order`, `limit`, `offset` and `page` are reserved
		 * in venture_query_apply_query_string(); anything else is
		 * parsed as a field filter and refused if the field does not
		 * exist. `period` and `period_field` are handled beside them.
		 */
		static const gchar *const reserved[] = {
			"search", "order", "limit", "offset", "period",
			"period_field", NULL
		};
		gsize i;

		for (i = 0; NULL != reserved[i]; i++)
		{
			g_autofree gchar *value = NULL;

			if (!json_object_has_member(arguments, reserved[i]))
				continue;

			value = venture_mcp_query_value(
				json_object_get_member(arguments, reserved[i]));

			if ('\0' == value[0])
				continue;

			g_string_append_c(path, first ? '?' : '&');
			first = FALSE;
			g_string_append(path, reserved[i]);
			g_string_append_c(path, '=');
			g_string_append_uri_escaped(path, value, NULL, FALSE);
		}
	}

	node = venture_mcp_server_request(self, "GET", path->str, NULL, error);

	if (NULL == node)
		return NULL;

	return venture_json_to_string(node, TRUE);
}

static gchar *
venture_mcp_tool_get(
	VentureMcpServer	 *self,
	JsonObject		 *arguments,
	GError			**error
){
	g_autoptr(JsonNode) node = NULL;
	g_autofree gchar *path = NULL;
	const gchar *type;
	gint64 id;

	type = venture_mcp_resolve_argument_type(self, arguments, error);

	if (NULL == type)
		return NULL;

	if (!venture_mcp_resolve_argument_id(arguments, &id, error))
		return NULL;

	path = g_strdup_printf("/api/v1/%s/%" G_GINT64_FORMAT, type, id);
	node = venture_mcp_server_request(self, "GET", path, NULL, error);

	if (NULL == node)
		return NULL;

	return venture_json_to_string(node, TRUE);
}

/*
 * The path a write goes to, with `?stage=1` appended when the change is
 * being proposed rather than made.
 *
 * One function so the three write tools cannot disagree about the spelling.
 */
static gchar *
venture_mcp_write_path(
	VentureMcpServer	*self,
	const gchar		*path
){
	if (self->stage_writes && self->server_stages)
		return g_strconcat(path, "?stage=1", NULL);

	return g_strdup(path);
}

/*
 * The answer a write tool gives when the server staged the change.
 *
 * It names the confirmation id, because that is what somebody has to quote
 * to approve or reject it, and it says where the change is waiting. Unlike
 * the hold this replaced, "staged for approval" is now true: the change is
 * in the server's queue and venture_confirmations lists it.
 */
static gchar *
venture_mcp_staged_text(JsonNode *reply)
{
	g_autoptr(GString) text = NULL;
	g_autofree gchar *encoded = NULL;
	JsonObject *object;
	JsonObject *confirmation;
	const gchar *id;

	object = JSON_NODE_HOLDS_OBJECT(reply) ? json_node_get_object(reply)
	                                       : NULL;
	confirmation = ((NULL != object) &&
	                json_object_has_member(object, "confirmation"))
		? json_object_get_object_member(object, "confirmation") : NULL;
	id = (NULL != confirmation)
		? venture_json_object_get_string(confirmation, "id", NULL) : NULL;

	text = g_string_new("Not applied yet. The change was staged on the "
	                    "VENTURE server and is waiting for a person to "
	                    "approve it.\n\n");

	if (NULL != id)
	{
		g_string_append_printf(text,
			"  confirmation id: %s\n"
			"  approve: POST /api/v1/confirmations/%s/approve\n"
			"  reject:  POST /api/v1/confirmations/%s/reject\n\n",
			id, id, id);
	}

	encoded = venture_json_to_string(reply, TRUE);
	g_string_append_printf(text, "%s\n", encoded);

	g_string_append(text,
		"\nIt is listed by venture_confirmations and by GET "
		"/api/v1/confirmations until it is decided, and it expires on its "
		"own if nobody answers. Tell the person you are working for what it "
		"will do and that it needs approving; do not report it as done. To "
		"write directly instead, the server has to be started as "
		"`venturectl mcp --apply-writes`.\n");

	return g_string_free(g_steal_pointer(&text), FALSE);
}

/*
 * The answer a write tool gives when staging was asked for and the server
 * cannot do it.
 *
 * This is the old client-side hold, kept for exactly this case. It states
 * that nothing was sent and that nothing is queued anywhere, because saying
 * "staged for approval" would send whoever read it to look in a queue that
 * will never hold the change.
 */
static gchar *
venture_mcp_held_text(
	const gchar	*method,
	const gchar	*path,
	JsonNode	*body
){
	g_autoptr(GString) text = NULL;

	text = g_string_new("Not applied. Write staging is on and this VENTURE "
	                    "server is too old to stage a change itself, so the "
	                    "change was described and not sent.\n\n");

	g_string_append_printf(text, "  %s %s\n", method, path);

	if (NULL != body)
	{
		g_autofree gchar *encoded = NULL;

		encoded = venture_json_to_string(body, TRUE);
		g_string_append_printf(text, "\n%s\n", encoded);
	}

	g_string_append(text,
		"\nNothing is queued on the VENTURE server: it has no pending "
		"confirmation for this, and venture_confirmations will not show it. "
		"Report the change above to the person you are working for and let "
		"them decide. A server that reports staged_writes at /api/v1/health "
		"would have queued it for them instead.\n");

	return g_string_free(g_steal_pointer(&text), FALSE);
}

/*
 * Sends a write, staged or not, and renders whichever answer came back.
 *
 * The staged and direct paths differ by a query parameter and nothing else,
 * which is the point: the server builds the record the same way either way,
 * so a change an agent proposes is the change that gets applied.
 */
static gchar *
venture_mcp_write(
	VentureMcpServer	 *self,
	const gchar		 *method,
	const gchar		 *path,
	JsonNode		 *values,
	GError			**error
){
	g_autofree gchar *target = NULL;
	g_autoptr(JsonNode) node = NULL;

	if (self->stage_writes && !self->server_stages)
		return venture_mcp_held_text(method, path, values);

	target = venture_mcp_write_path(self, path);
	node = venture_mcp_server_request(self, method, target, values, error);

	if (NULL == node)
		return NULL;

	/*
	 * Classified from the reply, never from the request. A server that
	 * ignored the parameter answers with the record it just wrote, and
	 * reporting that as staged would be the one lie that matters here.
	 */
	if (self->stage_writes && JSON_NODE_HOLDS_OBJECT(node) &&
	    json_object_get_boolean_member_with_default(json_node_get_object(node),
	                                                "staged", FALSE))
		return venture_mcp_staged_text(node);

	return venture_json_to_string(node, TRUE);
}

static gchar *
venture_mcp_tool_create(
	VentureMcpServer	 *self,
	JsonObject		 *arguments,
	GError			**error
){
	g_autoptr(JsonNode) values = NULL;
	g_autofree gchar *path = NULL;
	const gchar *type;

	type = venture_mcp_resolve_argument_type(self, arguments, error);

	if (NULL == type)
		return NULL;

	values = venture_mcp_values_body(arguments, error);

	if (NULL == values)
		return NULL;

	path = g_strdup_printf("/api/v1/%s", type);

	return venture_mcp_write(self, "POST", path, values, error);
}

static gchar *
venture_mcp_tool_update(
	VentureMcpServer	 *self,
	JsonObject		 *arguments,
	GError			**error
){
	g_autoptr(JsonNode) values = NULL;
	g_autofree gchar *path = NULL;
	const gchar *type;
	gint64 id;

	type = venture_mcp_resolve_argument_type(self, arguments, error);

	if (NULL == type)
		return NULL;

	if (!venture_mcp_resolve_argument_id(arguments, &id, error))
		return NULL;

	values = venture_mcp_values_body(arguments, error);

	if (NULL == values)
		return NULL;

	path = g_strdup_printf("/api/v1/%s/%" G_GINT64_FORMAT, type, id);

	return venture_mcp_write(self, "PATCH", path, values, error);
}

static gchar *
venture_mcp_tool_delete(
	VentureMcpServer	 *self,
	JsonObject		 *arguments,
	GError			**error
){
	g_autofree gchar *path = NULL;
	const gchar *type;
	gint64 id;

	type = venture_mcp_resolve_argument_type(self, arguments, error);

	if (NULL == type)
		return NULL;

	if (!venture_mcp_resolve_argument_id(arguments, &id, error))
		return NULL;

	path = g_strdup_printf("/api/v1/%s/%" G_GINT64_FORMAT, type, id);

	return venture_mcp_write(self, "DELETE", path, NULL, error);
}

static gchar *
venture_mcp_tool_reports(
	VentureMcpServer	 *self,
	JsonObject		 *arguments,
	GError			**error
){
	g_autoptr(JsonNode) node = NULL;

	node = venture_mcp_server_request(self, "GET", "/api/v1/reports", NULL,
	                                  error);

	if (NULL == node)
		return NULL;

	return venture_json_to_string(node, TRUE);
}

/*
 * venture_report: run one report, and say what came back.
 *
 * `?format=csv` returns a different content type, and CSV read as JSON is
 * nonsense that parses -- a header row becomes a key, a comma becomes a
 * delimiter that is not one. So the answer names the content type the server
 * actually sent rather than the format that was asked for: if the request
 * asked for CSV and JSON came back, that is what it says.
 */
static gchar *
venture_mcp_tool_report(
	VentureMcpServer	 *self,
	JsonObject		 *arguments,
	GError			**error
){
	g_autoptr(GString) path = NULL;
	g_autoptr(GString) text = NULL;
	g_autofree gchar *body = NULL;
	g_autofree gchar *content_type = NULL;
	const gchar *name;
	const gchar *period;
	const gchar *format;
	const gchar *kind;

	name = (NULL != arguments)
		? venture_json_object_get_string(arguments, "name", NULL) : NULL;

	if ((NULL == name) || ('\0' == name[0]))
	{
		g_set_error_literal(error, VENTURE_ERROR,
		                    VENTURE_ERROR_INVALID_ARGUMENT,
		                    "Which report? venture_reports lists them.");
		return NULL;
	}

	period = venture_json_object_get_string(arguments, "period", NULL);
	format = venture_json_object_get_string(arguments, "format", NULL);

	if ((NULL != format) && (0 != g_strcmp0(format, "json")) &&
	    (0 != g_strcmp0(format, "csv")))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "\"%s\" is not a report format. Use json or csv.",
		            format);
		return NULL;
	}

	path = g_string_new("/api/v1/reports/");
	g_string_append_uri_escaped(path, name, NULL, FALSE);

	if ((NULL != period) && ('\0' != period[0]))
	{
		g_string_append(path, "?period=");
		g_string_append_uri_escaped(path, period, NULL, FALSE);
	}

	{
		static const gchar *const options[] = { "customer_id", "organization_id", "currency", "as_of", "vendor_id", NULL };
		guint i;
		for (i = 0; options[i] != NULL; i++)
		{
			g_autofree gchar *value = NULL;
			if (!json_object_has_member(arguments, options[i]))
				continue;
			value = venture_mcp_query_value(json_object_get_member(arguments, options[i]));
			g_string_append_c(path, strchr(path->str, '?') != NULL ? '&' : '?');
			g_string_append_printf(path, "%s=", options[i]);
			g_string_append_uri_escaped(path, value, NULL, FALSE);
		}
	}

	if (0 == g_strcmp0(format, "csv"))
	{
		g_string_append_c(path,
			(NULL != g_strstr_len(path->str, -1, "?")) ? '&' : '?');
		g_string_append(path, "format=csv");
	}

	body = venture_mcp_server_request_text(self, "GET", path->str, NULL,
	                                       &content_type, error);

	if (NULL == body)
		return NULL;

	/*
	 * Classified from the response's own Content-Type, not from what was
	 * requested. A transport that reports no content type says so rather
	 * than being assumed to have sent JSON.
	 */
	if (NULL == content_type)
		kind = "unstated";
	else if (NULL != g_strstr_len(content_type, -1, "csv"))
		kind = "CSV";
	else if (NULL != g_strstr_len(content_type, -1, "json"))
		kind = "JSON";
	else
		kind = "neither CSV nor JSON";

	text = g_string_new(NULL);
	g_string_append_printf(text,
		"Report: %s\nContent-Type: %s\nThis body is %s.\n\n",
		name, (NULL != content_type) ? content_type : "(none sent)", kind);
	g_string_append(text, body);

	if ('\n' != body[strlen(body) > 0 ? strlen(body) - 1 : 0])
		g_string_append_c(text, '\n');

	return g_string_free(g_steal_pointer(&text), FALSE);
}

static gchar *
venture_mcp_tool_confirmations(
	VentureMcpServer	 *self,
	JsonObject		 *arguments,
	GError			**error
){
	g_autoptr(JsonNode) node = NULL;
	g_autofree gchar *path = NULL;
	const gchar *action;
	const gchar *id;

	action = (NULL != arguments)
		? venture_json_object_get_string(arguments, "action", NULL) : NULL;

	if ((NULL == action) || ('\0' == action[0]))
		action = "list";

	if (0 == g_strcmp0(action, "list"))
	{
		node = venture_mcp_server_request(self, "GET",
		                                  "/api/v1/confirmations", NULL,
		                                  error);

		if (NULL == node)
			return NULL;

		return venture_json_to_string(node, TRUE);
	}

	if ((0 != g_strcmp0(action, "approve")) &&
	    (0 != g_strcmp0(action, "reject")))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "\"%s\" is not a confirmation action. Use list, approve "
		            "or reject.", action);
		return NULL;
	}

	id = venture_json_object_get_string(arguments, "id", NULL);

	if ((NULL == id) || ('\0' == id[0]))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "Which staged change should be %sed? Pass its id from "
		            "the list.", action);
		return NULL;
	}

	{
		g_autoptr(GString) built = NULL;

		built = g_string_new("/api/v1/confirmations/");
		g_string_append_uri_escaped(built, id, NULL, FALSE);
		g_string_append_c(built, '/');
		g_string_append(built, action);
		path = g_string_free(g_steal_pointer(&built), FALSE);
	}

	node = venture_mcp_server_request(self, "POST", path, NULL, error);

	if (NULL == node)
		return NULL;

	return venture_json_to_string(node, TRUE);
}

/* --- JSON-RPC ------------------------------------------------------------ */

/*
 * Builds a JSON-RPC envelope carrying @result.
 *
 * The id is copied node for node rather than read as a string. JSON-RPC ids
 * keep their type, and a numeric id answered as a quoted one is a response
 * the client cannot match to its request.
 */
static JsonNode *
venture_mcp_response_new(
	JsonNode	*id,
	JsonNode	*result
){
	g_autoptr(JsonBuilder) builder = NULL;

	builder = json_builder_new();
	json_builder_begin_object(builder);

	json_builder_set_member_name(builder, "jsonrpc");
	json_builder_add_string_value(builder, "2.0");

	json_builder_set_member_name(builder, "id");

	if (NULL != id)
		json_builder_add_value(builder, json_node_copy(id));
	else
		json_builder_add_null_value(builder);

	json_builder_set_member_name(builder, "result");
	json_builder_add_value(builder, result);

	json_builder_end_object(builder);

	return json_builder_get_root(builder);
}

static JsonNode *
venture_mcp_error_new(
	JsonNode	*id,
	gint		 code,
	const gchar	*message
){
	g_autoptr(JsonBuilder) builder = NULL;

	builder = json_builder_new();
	json_builder_begin_object(builder);

	json_builder_set_member_name(builder, "jsonrpc");
	json_builder_add_string_value(builder, "2.0");

	json_builder_set_member_name(builder, "id");

	if (NULL != id)
		json_builder_add_value(builder, json_node_copy(id));
	else
		json_builder_add_null_value(builder);

	json_builder_set_member_name(builder, "error");
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "code");
	json_builder_add_int_value(builder, code);
	json_builder_set_member_name(builder, "message");
	json_builder_add_string_value(builder, message);
	json_builder_end_object(builder);

	json_builder_end_object(builder);

	return json_builder_get_root(builder);
}

/*
 * A tools/call result: one text block, plus whether it was a failure.
 *
 * A tool failure is reported inside a successful JSON-RPC response with
 * isError set, not as a JSON-RPC error. The distinction matters: a protocol
 * error is something the client did wrong, and a tool error is something the
 * model should read and act on.
 */
static JsonNode *
venture_mcp_content_new(
	const gchar	*text,
	gboolean	 is_error
){
	g_autoptr(JsonBuilder) builder = NULL;

	builder = json_builder_new();
	json_builder_begin_object(builder);

	json_builder_set_member_name(builder, "content");
	json_builder_begin_array(builder);
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "type");
	json_builder_add_string_value(builder, "text");
	json_builder_set_member_name(builder, "text");
	json_builder_add_string_value(builder, text);
	json_builder_end_object(builder);
	json_builder_end_array(builder);

	json_builder_set_member_name(builder, "isError");
	json_builder_add_boolean_value(builder, is_error);

	json_builder_end_object(builder);

	return json_builder_get_root(builder);
}

static JsonNode *
venture_mcp_handle_initialize(VentureMcpServer *self)
{
	g_autoptr(JsonBuilder) builder = NULL;

	builder = json_builder_new();
	json_builder_begin_object(builder);

	json_builder_set_member_name(builder, "protocolVersion");
	json_builder_add_string_value(builder, VENTURE_MCP_PROTOCOL_VERSION);

	json_builder_set_member_name(builder, "capabilities");
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "tools");
	json_builder_begin_object(builder);
	json_builder_end_object(builder);
	json_builder_end_object(builder);

	json_builder_set_member_name(builder, "serverInfo");
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "name");
	json_builder_add_string_value(builder, "venturectl");
	json_builder_set_member_name(builder, "version");
	json_builder_add_string_value(builder, venture_get_version_string());
	json_builder_end_object(builder);

	json_builder_set_member_name(builder, "instructions");
	json_builder_add_string_value(builder,
		self->stage_writes
			? "VENTURE is an ERP and CRM. Call venture_schema before "
			  "writing anything: field names are exact and a wrong one is "
			  "ignored rather than refused. Write staging is on, so "
			  "venture_create, venture_update and venture_delete propose "
			  "the change instead of making it: the server queues it and a "
			  "person approves it. Nothing you write takes effect until "
			  "somebody says so, and venture_confirmations shows what is "
			  "waiting."
			: "VENTURE is an ERP and CRM. Call venture_schema before "
			  "writing anything: field names are exact and a wrong one is "
			  "ignored rather than refused. Write staging is off, so writes "
			  "apply immediately and are audited against this token's "
			  "account.");

	json_builder_end_object(builder);

	return json_builder_get_root(builder);
}

/*
 * Dispatches one tools/call to the tool that answers it.
 */
/*
 * A write that the server cannot stage -- a whole dashboard, or an action
 * on the forge -- is refused outright when staging is on, and says what
 * to do instead. Sending it would apply it, and holding it client-side
 * would describe a queue that does not exist.
 */
static gboolean
venture_mcp_require_apply_writes(
	VentureMcpServer	 *self,
	const gchar		 *what,
	const gchar		 *instead,
	GError			**error
){
	if (!self->stage_writes)
		return TRUE;

	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED,
	            "Not applied. %s cannot be staged for approval, and this "
	            "server was started with write staging on. Either start it "
	            "as `venturectl mcp --apply-writes`, or %s. Tell the person "
	            "you are working for which you need.", what, instead);

	return FALSE;
}

static gchar *
venture_mcp_tool_modules(
	VentureMcpServer	 *self,
	JsonObject		 *arguments,
	GError			**error
){
	g_autoptr(JsonNode) node = NULL;

	(void)arguments;

	node = venture_mcp_server_request(self, "GET", "/api/v1/modules", NULL,
	                                  error);

	if (NULL == node)
		return NULL;

	return venture_json_to_string(node, TRUE);
}

static gchar *
venture_mcp_tool_links(
	VentureMcpServer	 *self,
	JsonObject		 *arguments,
	GError			**error
){
	g_autoptr(JsonNode) node = NULL;
	g_autofree gchar *path = NULL;
	const gchar *type;
	gint64 id;

	type = venture_mcp_resolve_argument_type(self, arguments, error);

	if (NULL == type)
		return NULL;

	if (!venture_mcp_resolve_argument_id(arguments, &id, error))
		return NULL;

	path = g_strdup_printf("/api/v1/links/%s/%" G_GINT64_FORMAT, type, id);
	node = venture_mcp_server_request(self, "GET", path, NULL, error);

	if (NULL == node)
		return NULL;

	return venture_json_to_string(node, TRUE);
}

/*
 * venture_link: POST /api/v1/links, which stages like any write.
 */
static gchar *
venture_mcp_tool_link(
	VentureMcpServer	 *self,
	JsonObject		 *arguments,
	GError			**error
){
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) body = NULL;
	const gchar *source_type;
	const gchar *target_type;
	const gchar *kind;
	const gchar *note;
	gint64 source_id;
	gint64 target_id;

	source_type = (NULL != arguments)
		? venture_json_object_get_string(arguments, "source_type", NULL)
		: NULL;
	target_type = (NULL != arguments)
		? venture_json_object_get_string(arguments, "target_type", NULL)
		: NULL;
	source_id = (NULL != arguments)
		? venture_json_object_get_int(arguments, "source_id", 0) : 0;
	target_id = (NULL != arguments)
		? venture_json_object_get_int(arguments, "target_id", 0) : 0;

	if ((NULL == source_type) || (NULL == target_type) || (0 == source_id) ||
	    (0 == target_id))
	{
		g_set_error_literal(error, VENTURE_ERROR,
		                    VENTURE_ERROR_INVALID_ARGUMENT,
		                    "A link needs source_type, source_id, "
		                    "target_type and target_id.");
		return NULL;
	}

	kind = venture_json_object_get_string(arguments, "kind", NULL);
	note = venture_json_object_get_string(arguments, "note", NULL);

	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "source_type");
	json_builder_add_string_value(builder, source_type);
	json_builder_set_member_name(builder, "source_id");
	json_builder_add_int_value(builder, source_id);
	json_builder_set_member_name(builder, "target_type");
	json_builder_add_string_value(builder, target_type);
	json_builder_set_member_name(builder, "target_id");
	json_builder_add_int_value(builder, target_id);

	if ((NULL != kind) && ('\0' != kind[0]))
	{
		json_builder_set_member_name(builder, "kind");
		json_builder_add_string_value(builder, kind);
	}

	if ((NULL != note) && ('\0' != note[0]))
	{
		json_builder_set_member_name(builder, "note");
		json_builder_add_string_value(builder, note);
	}

	json_builder_end_object(builder);
	body = json_builder_get_root(builder);

	return venture_mcp_write(self, "POST", "/api/v1/links", body, error);
}

static gchar *
venture_mcp_tool_dashboards(
	VentureMcpServer	 *self,
	JsonObject		 *arguments,
	GError			**error
){
	g_autoptr(JsonNode) node = NULL;
	const gchar *what;
	const gchar *path;

	what = (NULL != arguments)
		? venture_json_object_get_string(arguments, "what", NULL) : NULL;

	if ((NULL == what) || ('\0' == what[0]) || (0 == g_strcmp0(what, "list")))
		path = "/api/v1/dashboards";
	else if (0 == g_strcmp0(what, "kinds"))
		path = "/api/v1/widget-kinds";
	else if (0 == g_strcmp0(what, "templates"))
		path = "/api/v1/dashboard-templates";
	else
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "\"%s\" is not something venture_dashboards lists. "
		            "Use list, kinds or templates.", what);
		return NULL;
	}

	node = venture_mcp_server_request(self, "GET", path, NULL, error);

	if (NULL == node)
		return NULL;

	return venture_json_to_string(node, TRUE);
}

static gchar *
venture_mcp_tool_dashboard(
	VentureMcpServer	 *self,
	JsonObject		 *arguments,
	GError			**error
){
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GString) path = NULL;
	const gchar *slug;
	const gchar *mode;

	slug = (NULL != arguments)
		? venture_json_object_get_string(arguments, "slug", NULL) : NULL;

	if ((NULL == slug) || ('\0' == slug[0]))
	{
		g_set_error_literal(error, VENTURE_ERROR,
		                    VENTURE_ERROR_INVALID_ARGUMENT,
		                    "Which dashboard? venture_dashboards lists "
		                    "them by slug.");
		return NULL;
	}

	mode = venture_json_object_get_string(arguments, "mode", NULL);
	path = g_string_new("/api/v1/dashboards/");
	g_string_append_uri_escaped(path, slug, NULL, FALSE);

	if (0 == g_strcmp0(mode, "definition"))
		g_string_append(path, "/export");
	else if (0 == g_strcmp0(mode, "settings"))
		g_string_append(path, "?data=0");
	else if ((NULL != mode) && ('\0' != mode[0]) &&
	         (0 != g_strcmp0(mode, "data")))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "\"%s\" is not a mode. Use data, settings or "
		            "definition.", mode);
		return NULL;
	}

	node = venture_mcp_server_request(self, "GET", path->str, NULL, error);

	if (NULL == node)
		return NULL;

	return venture_json_to_string(node, TRUE);
}

static gchar *
venture_mcp_tool_dashboard_build(
	VentureMcpServer	 *self,
	JsonObject		 *arguments,
	GError			**error
){
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(JsonNode) body = NULL;
	const gchar *template_name;
	JsonNode *definition;

	if (!venture_mcp_require_apply_writes(self, "Building a whole dashboard",
		"create the `dashboard` record with venture_create and then each "
		"`dashboard_widget` with its dashboard_id, which stage like any "
		"record", error))
		return NULL;

	template_name = (NULL != arguments)
		? venture_json_object_get_string(arguments, "template", NULL) : NULL;
	definition = (NULL != arguments)
		? json_object_get_member(arguments, "definition") : NULL;

	if ((NULL != template_name) && ('\0' != template_name[0]))
	{
		g_autoptr(JsonBuilder) builder = NULL;

		builder = json_builder_new();
		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "template");
		json_builder_add_string_value(builder, template_name);
		json_builder_end_object(builder);
		body = json_builder_get_root(builder);
		node = venture_mcp_server_request(self, "POST",
			"/api/v1/dashboards/from-template", body, error);
	}
	else if ((NULL != definition) && JSON_NODE_HOLDS_OBJECT(definition))
	{
		node = venture_mcp_server_request(self, "POST",
			"/api/v1/dashboards/import", definition, error);
	}
	else
	{
		g_set_error_literal(error, VENTURE_ERROR,
		                    VENTURE_ERROR_INVALID_ARGUMENT,
		                    "Name a template, or pass a definition object "
		                    "with a name and a list of widgets.");
		return NULL;
	}

	if (NULL == node)
		return NULL;

	return venture_json_to_string(node, TRUE);
}

/* --- The workdesk ---------------------------------------------------------- */

static gchar *
venture_mcp_tool_inbox(
	VentureMcpServer	 *self,
	JsonObject		 *arguments,
	GError			**error
){
	g_autoptr(JsonNode) node = NULL;
	const gchar *action;

	action = (NULL != arguments)
		? venture_json_object_get_string(arguments, "action", NULL) : NULL;

	if (0 == g_strcmp0(action, "read"))
	{
		g_autoptr(JsonBuilder) builder = NULL;
		g_autoptr(JsonNode) body = NULL;

		builder = json_builder_new();
		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "id");
		json_builder_add_int_value(builder,
			(NULL != arguments)
				? venture_json_object_get_int(arguments, "id", 0) : 0);
		json_builder_end_object(builder);
		body = json_builder_get_root(builder);

		node = venture_mcp_server_request(self, "POST", "/api/v1/inbox/read",
		                                  body, error);
	}
	else if ((NULL == action) || ('\0' == action[0]) ||
	         (0 == g_strcmp0(action, "list")))
	{
		g_autofree gchar *path = NULL;
		gboolean unread;
		gint64 limit;

		unread = (NULL != arguments)
			? venture_json_object_get_bool(arguments, "unread", TRUE) : TRUE;
		limit = (NULL != arguments)
			? venture_json_object_get_int(arguments, "limit", 0) : 0;
		path = g_strdup_printf("/api/v1/inbox?unread=%s&limit=%"
		                       G_GINT64_FORMAT, unread ? "1" : "0", limit);
		node = venture_mcp_server_request(self, "GET", path, NULL, error);
	}
	else
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "\"%s\" is not an inbox action. Use list or read.",
		            action);
		return NULL;
	}

	if (NULL == node)
		return NULL;

	return venture_json_to_string(node, TRUE);
}

static gchar *
venture_mcp_tool_runs(
	VentureMcpServer	 *self,
	JsonObject		 *arguments,
	GError			**error
){
	g_autoptr(JsonNode) node = NULL;
	g_autofree gchar *path = NULL;
	const gchar *what;
	const gchar *state;
	gint64 limit;

	what = (NULL != arguments)
		? venture_json_object_get_string(arguments, "what", NULL) : NULL;

	if (0 == g_strcmp0(what, "budgets"))
	{
		path = g_strdup("/api/v1/budgets");
	}
	else if ((NULL == what) || ('\0' == what[0]) ||
	         (0 == g_strcmp0(what, "runs")))
	{
		g_autofree gchar *escaped = NULL;

		state = (NULL != arguments)
			? venture_json_object_get_string(arguments, "state", NULL) : NULL;
		limit = (NULL != arguments)
			? venture_json_object_get_int(arguments, "limit", 0) : 0;
		escaped = g_uri_escape_string((NULL != state) ? state : "all", NULL,
		                              FALSE);
		path = g_strdup_printf("/api/v1/runs?state=%s&limit=%" G_GINT64_FORMAT,
		                       escaped, limit);
	}
	else
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "\"%s\" is not something venture_runs shows. Use runs "
		            "or budgets.", what);
		return NULL;
	}

	node = venture_mcp_server_request(self, "GET", path, NULL, error);

	if (NULL == node)
		return NULL;

	return venture_json_to_string(node, TRUE);
}

static gchar *
venture_mcp_tool_desk(
	VentureMcpServer	 *self,
	JsonObject		 *arguments,
	GError			**error
){
	g_autoptr(JsonNode) node = NULL;
	g_autofree gchar *path = NULL;
	const gchar *action;
	gint64 id;

	action = (NULL != arguments)
		? venture_json_object_get_string(arguments, "action", NULL) : NULL;

	if ((NULL == action) || ('\0' == action[0]))
	{
		g_set_error_literal(error, VENTURE_ERROR,
		                    VENTURE_ERROR_INVALID_ARGUMENT,
		                    "venture_desk needs an action.");
		return NULL;
	}

	id = (NULL != arguments) ? venture_json_object_get_int(arguments, "id", 0)
	                         : 0;

	if (0 == g_strcmp0(action, "sprints"))
	{
		node = venture_mcp_server_request(self, "GET", "/api/v1/sprints", NULL,
		                                  error);
	}
	else if (0 == g_strcmp0(action, "sprint"))
	{
		if (!venture_mcp_resolve_argument_id(arguments, &id, error))
			return NULL;

		path = g_strdup_printf("/api/v1/sprints/%" G_GINT64_FORMAT, id);
		node = venture_mcp_server_request(self, "GET", path, NULL, error);
	}
	else if (0 == g_strcmp0(action, "sla"))
	{
		if (!venture_mcp_resolve_argument_id(arguments, &id, error))
			return NULL;

		path = g_strdup_printf("/api/v1/tickets/%" G_GINT64_FORMAT "/sla", id);
		node = venture_mcp_server_request(self, "GET", path, NULL, error);
	}
	else if (0 == g_strcmp0(action, "activity"))
	{
		const gchar *type_name;

		type_name = (NULL != arguments)
			? venture_json_object_get_string(arguments, "type", NULL) : NULL;

		if ((NULL == type_name) || ('\0' == type_name[0]) ||
		    !venture_mcp_resolve_argument_id(arguments, &id, error))
		{
			if ((NULL == error) || (NULL == *error))
				g_set_error_literal(error, VENTURE_ERROR,
				                    VENTURE_ERROR_INVALID_ARGUMENT,
				                    "activity needs a type and an id.");
			return NULL;
		}

		path = g_strdup_printf("/api/v1/activity/%s/%" G_GINT64_FORMAT
		                       "?limit=%" G_GINT64_FORMAT, type_name, id,
		                       venture_json_object_get_int(arguments, "limit",
		                                                   0));
		node = venture_mcp_server_request(self, "GET", path, NULL, error);
	}
	else if ((0 == g_strcmp0(action, "triage")) ||
	         (0 == g_strcmp0(action, "summarise")) ||
	         (0 == g_strcmp0(action, "draft")))
	{
		/* All three are reads: a triage is a proposal and a draft is a
		 * draft, so neither needs --apply-writes. */
		if (!venture_mcp_resolve_argument_id(arguments, &id, error))
			return NULL;

		if (0 == g_strcmp0(action, "summarise"))
		{
			path = g_strdup_printf("/api/v1/tickets/%" G_GINT64_FORMAT
			                       "/summary", id);
			node = venture_mcp_server_request(self, "GET", path, NULL, error);
		}
		else if (0 == g_strcmp0(action, "draft"))
		{
			g_autoptr(JsonBuilder) builder = NULL;
			g_autoptr(JsonNode) body = NULL;

			builder = json_builder_new();
			json_builder_begin_object(builder);
			json_builder_set_member_name(builder, "instruction");
			json_builder_add_string_value(builder,
				venture_json_object_get_string(arguments, "note", ""));
			json_builder_end_object(builder);
			body = json_builder_get_root(builder);

			path = g_strdup_printf("/api/v1/tickets/%" G_GINT64_FORMAT
			                       "/draft", id);
			node = venture_mcp_server_request(self, "POST", path, body, error);
		}
		else
		{
			path = g_strdup_printf("/api/v1/tickets/%" G_GINT64_FORMAT
			                       "/triage", id);
			node = venture_mcp_server_request(self, "POST", path, NULL, error);
		}
	}
	else if (0 == g_strcmp0(action, "worklog"))
	{
		g_autoptr(JsonBuilder) builder = NULL;
		g_autoptr(JsonNode) body = NULL;
		g_autofree gchar *write_path = NULL;
		const gchar *hours;

		if (!venture_mcp_resolve_argument_id(arguments, &id, error))
			return NULL;

		hours = venture_json_object_get_string(arguments, "hours", NULL);

		/* A worklog is a record, so it goes through the generic create
		 * and stages exactly as one would. */
		builder = json_builder_new();
		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "ticket_id");
		json_builder_add_int_value(builder, id);
		json_builder_set_member_name(builder, "hours");
		json_builder_add_double_value(builder,
			(NULL != hours) ? g_ascii_strtod(hours, NULL) : 0.0);
		json_builder_set_member_name(builder, "note");
		json_builder_add_string_value(builder,
			venture_json_object_get_string(arguments, "note", ""));
		json_builder_end_object(builder);
		body = json_builder_get_root(builder);

		write_path = venture_mcp_write_path(self, "/api/v1/worklog");
		node = venture_mcp_server_request(self, "POST", write_path, body,
		                                  error);
	}
	else if (0 == g_strcmp0(action, "macro"))
	{
		g_autoptr(JsonBuilder) builder = NULL;
		g_autoptr(JsonNode) body = NULL;

		if (!venture_mcp_resolve_argument_id(arguments, &id, error))
			return NULL;

		if (!venture_mcp_require_apply_writes(self, "Applying a macro",
			"propose its reply with venture_create on ticket_comment and "
			"its changes with venture_update on the ticket, which stage",
			error))
			return NULL;

		builder = json_builder_new();
		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "macro");
		json_builder_add_string_value(builder,
			venture_json_object_get_string(arguments, "macro", ""));
		json_builder_end_object(builder);
		body = json_builder_get_root(builder);

		path = g_strdup_printf("/api/v1/tickets/%" G_GINT64_FORMAT "/macro",
		                       id);
		node = venture_mcp_server_request(self, "POST", path, body, error);
	}
	else if (0 == g_strcmp0(action, "fix_ticket"))
	{
		if (!venture_mcp_resolve_argument_id(arguments, &id, error))
			return NULL;

		if (!venture_mcp_require_apply_writes(self,
			"Opening an incident's fix ticket",
			"propose the ticket with venture_create on ticket and set the "
			"incident's ticket_id with venture_update, which stage", error))
			return NULL;

		path = g_strdup_printf("/api/v1/incidents/%" G_GINT64_FORMAT "/ticket",
		                       id);
		node = venture_mcp_server_request(self, "POST", path, NULL, error);
	}
	else if (0 == g_strcmp0(action, "bulk"))
	{
		g_autoptr(JsonBuilder) builder = NULL;
		g_autoptr(JsonNode) body = NULL;
		g_auto(GStrv) parts = NULL;
		const gchar *type_name;
		const gchar *ids;
		gsize i;

		type_name = venture_json_object_get_string(arguments, "type", NULL);
		ids = venture_json_object_get_string(arguments, "ids", NULL);

		if ((NULL == type_name) || (NULL == ids))
		{
			g_set_error_literal(error, VENTURE_ERROR,
			                    VENTURE_ERROR_INVALID_ARGUMENT,
			                    "bulk needs a type and comma-separated ids.");
			return NULL;
		}

		if (!venture_mcp_require_apply_writes(self, "A bulk change",
			"change each record with venture_update, which stages each",
			error))
			return NULL;

		builder = json_builder_new();
		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "ids");
		json_builder_begin_array(builder);
		parts = g_strsplit_set(ids, ", ", -1);

		for (i = 0; NULL != parts[i]; i++)
		{
			if ('\0' != parts[i][0])
				json_builder_add_int_value(builder,
					g_ascii_strtoll(parts[i], NULL, 10));
		}

		json_builder_end_array(builder);

		if (venture_json_object_get_bool(arguments, "delete", FALSE))
		{
			json_builder_set_member_name(builder, "delete");
			json_builder_add_boolean_value(builder, TRUE);
		}
		else if (json_object_has_member(arguments, "changes"))
		{
			json_builder_set_member_name(builder, "changes");
			json_builder_add_value(builder,
				json_node_ref(json_object_get_member(arguments, "changes")));
		}

		json_builder_end_object(builder);
		body = json_builder_get_root(builder);

		path = g_strdup_printf("/api/v1/%s/bulk", type_name);
		node = venture_mcp_server_request(self, "POST", path, body, error);
	}
	else if (0 == g_strcmp0(action, "sweep"))
	{
		if (!venture_mcp_require_apply_writes(self, "A service-level sweep",
			"read the clocks with action sla; the board and the inbox "
			"sweep on their own when opened", error))
			return NULL;

		node = venture_mcp_server_request(self, "POST", "/api/v1/sla/sweep",
		                                  NULL, error);
	}
	else
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "\"%s\" is not a desk action.", action);
		return NULL;
	}

	if (NULL == node)
		return NULL;

	return venture_json_to_string(node, TRUE);
}

static gchar *
venture_mcp_tool_factory(
	VentureMcpServer	 *self,
	JsonObject		 *arguments,
	GError			**error
){
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(JsonNode) body = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autofree gchar *path = NULL;
	const gchar *action;
	gint64 id;

	action = (NULL != arguments)
		? venture_json_object_get_string(arguments, "action", NULL) : NULL;

	if ((NULL == action) || ('\0' == action[0]) ||
	    (0 == g_strcmp0(action, "status")))
	{
		node = venture_mcp_server_request(self, "GET", "/api/v1/factory",
		                                  NULL, error);

		if (NULL == node)
			return NULL;

		return venture_json_to_string(node, TRUE);
	}

	/* What needs somebody: a read, and of the whole factory, so no id. */
	if (0 == g_strcmp0(action, "actions"))
	{
		node = venture_mcp_server_request(self, "GET",
		                                  "/api/v1/factory/actions", NULL,
		                                  error);

		if (NULL == node)
			return NULL;

		return venture_json_to_string(node, TRUE);
	}

	if ((0 != g_strcmp0(action, "readiness")) &&
	    (0 != g_strcmp0(action, "forecast")) &&
	    (0 != g_strcmp0(action, "changelog")) &&
	    (0 != g_strcmp0(action, "deploy")) &&
	    (0 != g_strcmp0(action, "rollback")) &&
	    (0 != g_strcmp0(action, "build_ticket")) &&
	    (0 != g_strcmp0(action, "publish")))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "\"%s\" is not a factory action. Use status, actions, "
		            "readiness, forecast, changelog, deploy, rollback, "
		            "build_ticket or publish.", action);
		return NULL;
	}

	if (!venture_mcp_resolve_argument_id(arguments, &id, error))
		return NULL;

	/* The two that only read. */
	if ((0 == g_strcmp0(action, "readiness")) ||
	    (0 == g_strcmp0(action, "forecast")))
	{
		path = (0 == g_strcmp0(action, "readiness"))
			? g_strdup_printf("/api/v1/releases/%" G_GINT64_FORMAT
			                  "/readiness", id)
			: g_strdup_printf("/api/v1/milestones/%" G_GINT64_FORMAT
			                  "/forecast", id);
		node = venture_mcp_server_request(self, "GET", path, NULL, error);

		if (NULL == node)
			return NULL;

		return venture_json_to_string(node, TRUE);
	}

	/* The rest write, and each says what to do instead when it may not. */
	if (0 == g_strcmp0(action, "changelog"))
	{
		if (!venture_mcp_require_apply_writes(self,
			"Drafting a changelog onto the release",
			"read the tickets with venture_list (release_id filter) and "
			"set the changelog with venture_update, which stages", error))
			return NULL;
	}
	else if (0 == g_strcmp0(action, "deploy"))
	{
		if (!venture_mcp_require_apply_writes(self,
			"Recording a deployment",
			"create the deployment with venture_create, which stages",
			error))
			return NULL;
	}
	else if (0 == g_strcmp0(action, "rollback"))
	{
		if (!venture_mcp_require_apply_writes(self,
			"Rolling an environment back",
			"ask the person you are working for to press Roll back on the "
			"environment's page", error))
			return NULL;
	}
	else if (0 == g_strcmp0(action, "build_ticket"))
	{
		if (!venture_mcp_require_apply_writes(self,
			"Opening a ticket for a failed build",
			"create the ticket with venture_create, which stages, and link "
			"it from the build with venture_link", error))
			return NULL;
	}
	else if (!venture_mcp_require_apply_writes(self,
		"Publishing a release to the forge",
		"ask the person you are working for to press Publish on the "
		"release's page", error))
	{
		return NULL;
	}

	builder = json_builder_new();
	json_builder_begin_object(builder);

	if (0 == g_strcmp0(action, "changelog"))
	{
		json_builder_set_member_name(builder, "replace");
		json_builder_add_boolean_value(builder,
			venture_json_object_get_bool(arguments, "replace", FALSE));
	}
	else if (0 == g_strcmp0(action, "publish"))
	{
		json_builder_set_member_name(builder, "prerelease");
		json_builder_add_boolean_value(builder,
			venture_json_object_get_bool(arguments, "prerelease", FALSE));
	}
	else if (0 == g_strcmp0(action, "deploy"))
	{
		json_builder_set_member_name(builder, "environment_id");
		json_builder_add_int_value(builder,
			venture_json_object_get_int(arguments, "environment_id", 0));
		json_builder_set_member_name(builder, "notes");
		json_builder_add_string_value(builder,
			venture_json_object_get_string(arguments, "notes", ""));
	}
	else if (0 == g_strcmp0(action, "rollback"))
	{
		json_builder_set_member_name(builder, "reason");
		json_builder_add_string_value(builder,
			venture_json_object_get_string(arguments, "reason", ""));
	}

	json_builder_end_object(builder);
	body = json_builder_get_root(builder);

	if (0 == g_strcmp0(action, "rollback"))
		path = g_strdup_printf("/api/v1/environments/%" G_GINT64_FORMAT
		                       "/rollback", id);
	else if (0 == g_strcmp0(action, "build_ticket"))
		path = g_strdup_printf("/api/v1/builds/%" G_GINT64_FORMAT "/ticket",
		                       id);
	else
		path = g_strdup_printf("/api/v1/releases/%" G_GINT64_FORMAT "/%s", id,
		                       action);

	node = venture_mcp_server_request(self, "POST", path, body, error);

	if (NULL == node)
		return NULL;

	return venture_json_to_string(node, TRUE);
}

static gchar *
venture_mcp_tool_action(VentureMcpServer *self, const gchar *name, JsonObject *arguments, GError **error)
{
	g_autoptr(JsonNode) tools = venture_mcp_catalog_get_tools(self->catalog);
	JsonArray *array = json_node_get_array(tools);
	guint i;
	for (i = 0; i < json_array_get_length(array); i++)
	{
		JsonObject *tool = json_array_get_object_element(array, i);
		if (json_object_has_member(tool, "action_name") && 0 == g_strcmp0(name, json_object_get_string_member(tool, "name")))
		{
			g_autofree gchar *type = g_uri_escape_string(json_object_get_string_member(tool, "action_type"), NULL, FALSE);
			g_autofree gchar *action = g_uri_escape_string(json_object_get_string_member(tool, "action_name"), NULL, FALSE);
			g_autofree gchar *path = NULL;
			g_autoptr(JsonNode) body = json_node_new(JSON_NODE_OBJECT);
			g_autoptr(JsonNode) result = NULL;
			g_autoptr(GList) members = NULL;
			GList *item;
			JsonObject *values = json_object_new();
			gint64 id;
			json_node_take_object(body, values);
			if (!arguments || (!json_object_has_member(arguments, "id") && !json_object_get_boolean_member_with_default(tool, "type_level", FALSE)) ||
				(json_object_has_member(arguments, "id") && G_TYPE_INT64 != json_node_get_value_type(json_object_get_member(arguments, "id"))))
			{
				g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT, "An action requires an integer id");
				return NULL;
			}
			id = json_object_get_int_member_with_default(arguments, "id", 0);
			members = json_object_get_members(arguments);
			for (item = members; item; item = item->next)
				if (0 != g_strcmp0(item->data, "id")) json_object_set_member(values, item->data, json_node_ref(json_object_get_member(arguments, item->data)));
			path = g_strdup_printf("/api/v1/%s/%" G_GINT64_FORMAT "/actions/%s?stage=1", type, id, action);
			result = venture_mcp_server_request(self, "POST", path, body, error);
			if (!result) return NULL;
			return venture_mcp_staged_text(result);
		}
	}
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Unknown action tool");
	return NULL;
}

static gchar *
venture_mcp_dispatch_tool(
	VentureMcpServer	 *self,
	const gchar		 *name,
	JsonObject		 *arguments,
	GError			**error
){
	if (0 == g_strcmp0(name, "venture_schema"))
		return venture_mcp_tool_schema(self, arguments, error);

	if (0 == g_strcmp0(name, "venture_list"))
		return venture_mcp_tool_list(self, arguments, error);

	if (0 == g_strcmp0(name, "venture_get"))
		return venture_mcp_tool_get(self, arguments, error);

	if (0 == g_strcmp0(name, "venture_create"))
		return venture_mcp_tool_create(self, arguments, error);

	if (0 == g_strcmp0(name, "venture_update"))
		return venture_mcp_tool_update(self, arguments, error);

	if (0 == g_strcmp0(name, "venture_delete"))
		return venture_mcp_tool_delete(self, arguments, error);

	if (0 == g_strcmp0(name, "venture_reports"))
		return venture_mcp_tool_reports(self, arguments, error);

	if (0 == g_strcmp0(name, "venture_report"))
		return venture_mcp_tool_report(self, arguments, error);

	if (0 == g_strcmp0(name, "venture_confirmations"))
		return venture_mcp_tool_confirmations(self, arguments, error);

	if (0 == g_strcmp0(name, "venture_modules"))
		return venture_mcp_tool_modules(self, arguments, error);

	if (0 == g_strcmp0(name, "venture_links"))
		return venture_mcp_tool_links(self, arguments, error);

	if (0 == g_strcmp0(name, "venture_link"))
		return venture_mcp_tool_link(self, arguments, error);

	if (0 == g_strcmp0(name, "venture_dashboards"))
		return venture_mcp_tool_dashboards(self, arguments, error);

	if (0 == g_strcmp0(name, "venture_dashboard"))
		return venture_mcp_tool_dashboard(self, arguments, error);

	if (0 == g_strcmp0(name, "venture_dashboard_build"))
		return venture_mcp_tool_dashboard_build(self, arguments, error);

	if (0 == g_strcmp0(name, "venture_factory"))
		return venture_mcp_tool_factory(self, arguments, error);

	if (0 == g_strcmp0(name, "venture_inbox"))
		return venture_mcp_tool_inbox(self, arguments, error);

	if (0 == g_strcmp0(name, "venture_runs"))
		return venture_mcp_tool_runs(self, arguments, error);

	if (0 == g_strcmp0(name, "venture_desk"))
		return venture_mcp_tool_desk(self, arguments, error);

	return venture_mcp_tool_action(self, name, arguments, error);

}

static JsonNode *
venture_mcp_handle_tools_call(
	VentureMcpServer	*self,
	JsonNode		*id,
	JsonObject		*params
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *text = NULL;
	JsonObject *arguments;
	JsonNode *arguments_node;
	const gchar *name;

	name = (NULL != params)
		? venture_json_object_get_string(params, "name", NULL) : NULL;

	if (NULL == name)
	{
		return venture_mcp_error_new(id, VENTURE_JSONRPC_INVALID_PARAMS,
		                             "tools/call needs the tool's name.");
	}

	if (!venture_mcp_catalog_has_tool(self->catalog, name))
	{
		g_autofree gchar *message = NULL;

		message = g_strdup_printf("There is no tool called \"%s\".", name);

		return venture_mcp_error_new(id, VENTURE_JSONRPC_METHOD_NOT_FOUND,
		                             message);
	}

	arguments_node = json_object_get_member(params, "arguments");
	arguments = ((NULL != arguments_node) &&
	             JSON_NODE_HOLDS_OBJECT(arguments_node))
		? json_node_get_object(arguments_node) : NULL;

	text = venture_mcp_dispatch_tool(self, name, arguments, &error);

	if (NULL == text)
	{
		return venture_mcp_response_new(id,
			venture_mcp_content_new(
				(NULL != error) ? error->message
				                : "The tool failed and said nothing.",
				TRUE));
	}

	return venture_mcp_response_new(id, venture_mcp_content_new(text, FALSE));
}

JsonNode *
venture_mcp_server_handle(
	VentureMcpServer	*self,
	JsonNode		*request
){
	JsonObject *object;
	JsonObject *params;
	JsonNode *params_node;
	JsonNode *id;
	const gchar *method;

	g_return_val_if_fail(VENTURE_IS_MCP_SERVER(self), NULL);
	g_return_val_if_fail(NULL != request, NULL);

	/* A JsonNode can hold the object type and no object, so the type
	 * check is not a pointer check. */
	if (!JSON_NODE_HOLDS_OBJECT(request) ||
	    (NULL == json_node_get_object(request)))
	{
		return venture_mcp_error_new(NULL, VENTURE_JSONRPC_INVALID_REQUEST,
		                             "A JSON-RPC message must be an object.");
	}

	object = json_node_get_object(request);
	method = venture_json_object_get_string(object, "method", NULL);
	id = json_object_get_member(object, "id");

	/* A null id is no id: JSON-RPC says a request without one is a
	 * notification, and a notification must not be answered. */
	if ((NULL != id) && JSON_NODE_HOLDS_NULL(id))
		id = NULL;

	if (NULL == method)
	{
		if (NULL == id)
			return NULL;

		return venture_mcp_error_new(id, VENTURE_JSONRPC_INVALID_REQUEST,
		                             "A JSON-RPC request must name a "
		                             "method.");
	}

	params_node = json_object_get_member(object, "params");
	params = ((NULL != params_node) && JSON_NODE_HOLDS_OBJECT(params_node))
		? json_node_get_object(params_node) : NULL;

	if (0 == g_strcmp0(method, "initialize"))
		return venture_mcp_response_new(id, venture_mcp_handle_initialize(self));

	/* Notifications. Answering one is a protocol violation, so they are
	 * recognised explicitly rather than falling through to the
	 * method-not-found reply. */
	if (g_str_has_prefix(method, "notifications/"))
		return NULL;

	if (0 == g_strcmp0(method, "ping"))
	{
		g_autoptr(JsonBuilder) builder = NULL;

		builder = json_builder_new();
		json_builder_begin_object(builder);
		json_builder_end_object(builder);

		return venture_mcp_response_new(id, json_builder_get_root(builder));
	}

	if (NULL == self->catalog)
	{
		return venture_mcp_error_new(id, VENTURE_JSONRPC_INTERNAL_ERROR,
		                             "The tool surface has not been loaded "
		                             "from the server's schema yet.");
	}

	if (0 == g_strcmp0(method, "tools/list"))
	{
		g_autoptr(JsonBuilder) builder = NULL;

		builder = json_builder_new();
		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "tools");
		json_builder_add_value(builder,
			venture_mcp_catalog_get_tools(self->catalog));
		json_builder_end_object(builder);

		return venture_mcp_response_new(id, json_builder_get_root(builder));
	}

	if (0 == g_strcmp0(method, "tools/call"))
		return venture_mcp_handle_tools_call(self, id, params);

	if (NULL == id)
		return NULL;

	{
		g_autofree gchar *message = NULL;

		message = g_strdup_printf("This server does not implement \"%s\". "
		                          "It offers tools and nothing else.",
		                          method);

		return venture_mcp_error_new(id, VENTURE_JSONRPC_METHOD_NOT_FOUND,
		                             message);
	}
}

/* --- The stdio loop ------------------------------------------------------ */

/*
 * Reads one line, however long, or %NULL at end of stream.
 *
 * Written against getc rather than getline so the file needs no feature-test
 * macro to compile as gnu89; a tool call is a handful of kilobytes at most
 * and stdio buffers the reads.
 */
static gchar *
venture_mcp_read_line(void)
{
	g_autoptr(GString) line = NULL;
	int c;

	line = g_string_new(NULL);

	while (EOF != (c = getc(stdin)))
	{
		if ('\n' == c)
			return g_string_free(g_steal_pointer(&line), FALSE);

		g_string_append_c(line, (gchar)c);
	}

	/* A final line with no newline is still a message. */
	if (line->len > 0)
		return g_string_free(g_steal_pointer(&line), FALSE);

	return NULL;
}

static void
venture_mcp_write_message(JsonNode *message)
{
	g_autofree gchar *text = NULL;

	text = venture_json_to_string(message, FALSE);

	/*
	 * One message per line, flushed immediately. A client is blocked on
	 * this response, and stdout to a pipe is block-buffered by default,
	 * so without the flush the exchange deadlocks at the first call.
	 */
	fputs(text, stdout);
	fputc('\n', stdout);
	fflush(stdout);
}

/*
 * Sends every log message to stderr.
 *
 * GLib's default writer sends g_message() and g_info() to STDOUT, and stdout
 * here is the protocol transport. One such line lands between two JSON-RPC
 * messages and the client's parser fails on a stream that looks perfect in a
 * terminal -- the failure appears in the agent, a process away, as the MCP
 * server having crashed.
 */
static GLogWriterOutput
venture_mcp_log_writer(
	GLogLevelFlags	  log_level,
	const GLogField	 *fields,
	gsize		  n_fields,
	gpointer	  user_data
){
	g_autofree gchar *formatted = NULL;

	/*
	 * Formatted with the real level and written to stderr directly.
	 *
	 * g_log_writer_standard_streams() picks its stream from the level, so
	 * forcing stderr through it means lying about the level -- and a
	 * message relabelled as a warning sends whoever reads the log to the
	 * wrong layer.
	 */
	formatted = g_log_writer_format_fields(log_level, fields, n_fields,
	                                       FALSE);

	fputs(formatted, stderr);
	fputc('\n', stderr);

	return G_LOG_WRITER_HANDLED;
}

/*
 * Installs it once.
 *
 * g_log_set_writer_func() is g_error() on a second call, which aborts the
 * process -- so a second server in one process would take the first one
 * down rather than merely failing to install a writer it does not need.
 */
static void
venture_mcp_install_log_writer(void)
{
	static gsize installed = 0;

	if (g_once_init_enter(&installed))
	{
		g_log_set_writer_func(venture_mcp_log_writer, NULL, NULL);
		g_once_init_leave(&installed, 1);
	}
}

gboolean
venture_mcp_server_run(
	VentureMcpServer	 *self,
	GError			**error
){
	g_return_val_if_fail(VENTURE_IS_MCP_SERVER(self), FALSE);

	venture_mcp_install_log_writer();

	for (;;)
	{
		g_autofree gchar *line = NULL;
		g_autoptr(JsonNode) request = NULL;
		g_autoptr(JsonNode) response = NULL;

		line = venture_mcp_read_line();

		if (NULL == line)
			break;

		if ('\0' == g_strstrip(line)[0])
			continue;

		request = venture_json_parse(line, NULL);

		if (NULL == request)
		{
			g_autoptr(JsonNode) failure = NULL;

			failure = venture_mcp_error_new(NULL,
				VENTURE_JSONRPC_PARSE_ERROR,
				"That line is not JSON.");
			venture_mcp_write_message(failure);
			continue;
		}

		response = venture_mcp_server_handle(self, request);

		if (NULL != response)
			venture_mcp_write_message(response);
	}

	return TRUE;
}
