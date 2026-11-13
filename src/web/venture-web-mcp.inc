/*
 * venture-web-mcp.inc - POST /mcp, the Model Context Protocol over HTTPS
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Included from venture-web-server.c after the response helpers.
 *
 * `venturectl mcp` puts VENTURE in front of an agent that can spawn a
 * process. A hosted agent cannot: grok, a claude.ai connector, anything
 * configured as "Remote HTTPS" takes a URL and some headers and nothing
 * else. This is that URL. It speaks the Streamable HTTP transport of the
 * 2025-03-26 and later revisions in its simplest conforming shape: every
 * JSON-RPC message is one POST, every answer is one `application/json`
 * body, and there are no sessions to keep.
 *
 * The protocol itself is not written twice. The request is handed to the
 * same #VentureMcpServer that `venturectl mcp` runs, with the same nineteen
 * tools generated from the same schema, and its transport is pointed back
 * at this server's own REST API with the caller's own bearer token. So a
 * tool call is an ordinary API request made by the caller: the same
 * validators, the same access policy, the same staging and the same audit
 * line naming the same token. Nothing here can do what the token could not
 * do with curl, which is the property that makes this door safe to open.
 *
 * Why loop back over HTTP rather than call the handlers in-process: every
 * handler authenticates from the SoupServerMessage's headers, and a request
 * without a message cannot carry them (htmx_request_new_for_path() says
 * so). The loopback is also why the work runs on a thread: a handler that
 * blocked the main loop on a request to the main loop would wait forever.
 */

/*
 * How long one tool call may take end to end. The factory's tools can wait
 * on a forge; anything past this is a hung connection holding a GIO worker.
 */
#define VENTURE_WEB_MCP_TIMEOUT_SECONDS (600)

/*
 * Everything a worker needs, copied off the request on the main thread.
 * The worker never touches the request, the server or the database.
 */
typedef struct
{
	gchar		*loopback_url;
	gchar		*host;
	gchar		*forwarded_for;
	gchar		*token;
	GTlsCertificate	*certificate;
	gboolean	 stage_writes;
	JsonNode	*message;
	SoupSession	*session;
} VentureWebMcpJob;

static void
venture_web_mcp_job_free(gpointer data)
{
	VentureWebMcpJob *job = data;

	if (NULL == job)
		return;

	g_free(job->loopback_url);
	g_free(job->host);
	g_free(job->forwarded_for);

	/* Wiped, as VentureMcpServer wipes its own copy: it is the one value
	 * here worth reading out of a core dump. */
	if (NULL != job->token)
	{
		memset(job->token, 0, strlen(job->token));
		g_free(job->token);
	}

	g_clear_object(&job->certificate);
	g_clear_object(&job->session);

	if (NULL != job->message)
		json_node_unref(job->message);

	g_free(job);
}

/*
 * Accepts the server's own certificate and nothing else.
 *
 * A TLS listener is usually a self-signed or private-CA certificate, and
 * the loopback connects to an IP address the certificate does not name, so
 * ordinary validation refuses it. Pinning to the exact certificate the
 * listener presents is stricter than validation, not looser: no other
 * certificate is accepted, whoever signed it.
 */
static gboolean
venture_web_mcp_accept_certificate(
	SoupMessage		*message,
	GTlsCertificate		*peer,
	GTlsCertificateFlags	 errors,
	gpointer		 user_data
){
	VentureWebMcpJob *job = user_data;

	(void)message;
	(void)errors;

	return (NULL != job->certificate) && (NULL != peer) &&
	       g_tls_certificate_is_same(job->certificate, peer);
}

/*
 * The #VentureMcpTransportFunc: one request against this server's own API,
 * as the caller.
 *
 * Host is forwarded so a hosted workspace routes the loopback exactly as it
 * routed the original. X-Forwarded-For names the real client, which keeps
 * the loopback from looking like a local request to anything that trusts
 * one (`metrics.access: loopback` refuses any forwarding header).
 */
static gchar *
venture_web_mcp_transport(
	const gchar	 *method,
	const gchar	 *path,
	JsonNode	 *body,
	guint		 *out_status,
	gchar		**out_content_type,
	gpointer	  user_data,
	GError		**error
){
	VentureWebMcpJob *job = user_data;
	g_autoptr(SoupMessage) message = NULL;
	g_autoptr(GBytes) response = NULL;
	g_autoptr(GError) local_error = NULL;
	g_autofree gchar *url = NULL;
	g_autofree gchar *authorization = NULL;
	SoupMessageHeaders *headers;

	url = g_strconcat(job->loopback_url, path, NULL);
	message = soup_message_new(method, url);

	if (NULL == message)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "\"%s\" is not a usable path", path);
		return NULL;
	}

	/* A redirect would be the API sending a browser somewhere; the
	 * caller is not a browser, and following it would answer the tool
	 * with a login page. */
	soup_message_add_flags(message, SOUP_MESSAGE_NO_REDIRECT);

	if (NULL != job->certificate)
		g_signal_connect(message, "accept-certificate",
		                 G_CALLBACK(venture_web_mcp_accept_certificate), job);

	headers = soup_message_get_request_headers(message);
	authorization = g_strconcat("Bearer ", job->token, NULL);
	soup_message_headers_replace(headers, "Authorization", authorization);

	if (NULL != job->host)
		soup_message_headers_replace(headers, "Host", job->host);

	if (NULL != job->forwarded_for)
		soup_message_headers_replace(headers, "X-Forwarded-For", job->forwarded_for);

	if (NULL != body)
	{
		g_autofree gchar *encoded = NULL;
		g_autoptr(GBytes) bytes = NULL;

		encoded = venture_json_to_string(body, FALSE);
		bytes = g_bytes_new(encoded, strlen(encoded));
		soup_message_set_request_body_from_bytes(message, "application/json", bytes);
	}

	response = soup_session_send_and_read(job->session, message, NULL, &local_error);

	/* The header copy is wiped for the same reason the job's is. */
	memset(authorization, 0, strlen(authorization));

	if (NULL == response)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NETWORK,
		            "VENTURE could not reach its own API at %s: %s",
		            job->loopback_url, local_error->message);
		return NULL;
	}

	*out_status = soup_message_get_status(message);

	if (NULL != out_content_type)
		*out_content_type = g_strdup(soup_message_headers_get_one(
			soup_message_get_response_headers(message), "Content-Type"));

	return g_strndup(g_bytes_get_data(response, NULL), g_bytes_get_size(response));
}

/*
 * Whether answering @message needs the tool catalog: tools/list and
 * tools/call do; the handshake, ping and notifications do not. Loading it
 * costs two API requests, so a client's initialize is not charged for it.
 */
static gboolean
venture_web_mcp_needs_catalog(JsonNode *message)
{
	const gchar *method;

	if (!JSON_NODE_HOLDS_OBJECT(message) || (NULL == json_node_get_object(message)))
		return FALSE;

	method = venture_json_object_get_string(json_node_get_object(message), "method", NULL);

	return (NULL != method) && g_str_has_prefix(method, "tools/");
}

/* A JSON-RPC error with no request to answer, as an HTTP body. */
static HtmxResponse *
venture_web_mcp_rpc_error(
	guint		 status,
	gint		 code,
	const gchar	*text
){
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) node = NULL;

	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "jsonrpc");
	json_builder_add_string_value(builder, "2.0");
	json_builder_set_member_name(builder, "id");
	json_builder_add_null_value(builder);
	json_builder_set_member_name(builder, "error");
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "code");
	json_builder_add_int_value(builder, code);
	json_builder_set_member_name(builder, "message");
	json_builder_add_string_value(builder, text);
	json_builder_end_object(builder);
	json_builder_end_object(builder);
	node = json_builder_get_root(builder);

	return venture_web_json_response(node, status);
}

/*
 * Answers one message on @server, loading the catalog first when the
 * message needs it. A catalog that cannot be loaded is a JSON-RPC error on
 * that request, not an empty tool list: an agent shown no tools concludes
 * VENTURE has none.
 */
static JsonNode *
venture_web_mcp_answer_one(
	VentureMcpServer	*server,
	JsonNode		*message
){
	g_autoptr(GError) error = NULL;
	JsonNode *id = NULL;

	if (venture_web_mcp_needs_catalog(message) &&
	    (NULL == venture_mcp_server_get_catalog(server)) &&
	    !venture_mcp_server_load_catalog(server, &error))
	{
		g_autoptr(JsonBuilder) builder = NULL;

		id = json_object_get_member(json_node_get_object(message), "id");

		if ((NULL == id) || JSON_NODE_HOLDS_NULL(id))
			return NULL;

		builder = json_builder_new();
		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "jsonrpc");
		json_builder_add_string_value(builder, "2.0");
		json_builder_set_member_name(builder, "id");
		json_builder_add_value(builder, json_node_copy(id));
		json_builder_set_member_name(builder, "error");
		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "code");
		json_builder_add_int_value(builder, -32603);
		json_builder_set_member_name(builder, "message");
		json_builder_add_string_value(builder, error->message);
		json_builder_end_object(builder);
		json_builder_end_object(builder);

		return json_builder_get_root(builder);
	}

	return venture_mcp_server_handle(server, message);
}

/*
 * The worker: builds a #VentureMcpServer for this caller and answers the
 * POST body, a single message or a batch.
 *
 * Runs on a GIO worker thread. Everything it reads is in the job.
 */
static HtmxResponse *
venture_web_mcp_run(
	GCancellable	*cancellable,
	gpointer	 user_data
){
	VentureWebMcpJob *job = user_data;
	g_autoptr(VentureMcpServer) server = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) answer = NULL;
	HtmxResponse *response;

	(void)cancellable;

	/* The session is made on this thread, the one that uses it. A proxy
	 * from the environment is not used: the loopback is to this process,
	 * and an http_proxy set for outbound feeds would otherwise carry
	 * every tool call and its token through somebody else. */
	job->session = soup_session_new_with_options(
		"timeout", (guint)VENTURE_WEB_MCP_TIMEOUT_SECONDS, NULL);
	soup_session_set_proxy_resolver(job->session, NULL);

	server = venture_mcp_server_new(job->loopback_url, job->token, &error);

	if (NULL == server)
		return venture_web_mcp_rpc_error(500, -32603, error->message);

	venture_mcp_server_set_transport(server, venture_web_mcp_transport, job, NULL);
	venture_mcp_server_set_stage_writes(server, job->stage_writes);
	venture_mcp_server_set_server_name(server, "venture");

	/*
	 * A batch is an array of messages answered by an array of the answers
	 * that exist. Revision 2025-06-18 dropped batching and earlier ones
	 * allow it; answering one costs nothing and refusing it would strand
	 * an older client.
	 */
	if (JSON_NODE_HOLDS_ARRAY(job->message))
	{
		JsonArray *messages = json_node_get_array(job->message);
		g_autoptr(JsonArray) answers = json_array_new();
		guint i;

		for (i = 0; i < json_array_get_length(messages); i++)
		{
			JsonNode *one = venture_web_mcp_answer_one(server,
				json_array_get_element(messages, i));

			if (NULL != one)
				json_array_add_element(answers, one);
		}

		if (json_array_get_length(answers) > 0)
		{
			answer = json_node_new(JSON_NODE_ARRAY);
			json_node_set_array(answer, answers);
		}
	}
	else
	{
		answer = venture_web_mcp_answer_one(server, job->message);
	}

	/* Only notifications: the transport says 202 with no body. */
	if (NULL == answer)
	{
		response = htmx_response_new();
		htmx_response_set_status(response, 202);
		return response;
	}

	return venture_web_json_response(answer, 200);
}

/*
 * Where the worker reaches this server: the address it is bound to, with a
 * wildcard bind replaced by the loopback of the same family.
 */
static gchar *
venture_web_mcp_loopback_url(VentureWebServer *self)
{
	g_autoptr(GUri) uri = NULL;
	const gchar *host;

	if (NULL == self->base_url)
		return NULL;

	uri = g_uri_parse(self->base_url, G_URI_FLAGS_NONE, NULL);

	if (NULL == uri)
		return NULL;

	host = g_uri_get_host(uri);

	if ((NULL == host) || ('\0' == host[0]) || (0 == g_strcmp0(host, "0.0.0.0")))
		host = "127.0.0.1";
	else if (0 == g_strcmp0(host, "::"))
		host = "::1";

	return g_uri_join(G_URI_FLAGS_NONE, g_uri_get_scheme(uri), NULL, host,
	                  g_uri_get_port(uri), "", NULL, NULL);
}

/*
 * POST /mcp - one JSON-RPC message (or batch) in, its answer out.
 *
 * The caller must present `Authorization: Bearer <token>`; a browser
 * session is refused even when it is valid. That is what makes this route
 * safe against a page in somebody's browser posting to it: a cookie rides
 * along on a cross-site request, a bearer token never does. It is also why
 * the transport's Origin check is not needed here -- DNS rebinding reaches
 * the route, but not with a token.
 */
static HtmxResponse *
venture_web_mcp_post(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) message = NULL;
	g_autofree gchar *peer = NULL;
	g_autofree gchar *bearer = NULL;
	SoupServerMessage *soup_message;
	SoupMessageHeaders *headers;
	VentureWebMcpJob *job;
	const gchar *authorization;
	const gchar *content_type;
	const gchar *forwarded;
	const gchar *body;
	gboolean enabled = TRUE;
	gboolean apply_writes = FALSE;

	(void)params;

	g_object_get(venture_context_get_config(self->context),
	             "mcp-enabled", &enabled, "mcp-apply-writes", &apply_writes, NULL);

	if (!enabled)
		return htmx_response_not_found();

	soup_message = htmx_request_get_message(request);

	if (NULL == soup_message)
		return venture_web_mcp_rpc_error(400, -32600, "There is no HTTP request to answer.");

	headers = soup_server_message_get_request_headers(soup_message);

	/* --- Who is asking: a bearer token and nothing else --- */

	authorization = soup_message_headers_get_one(headers, "Authorization");

	if ((NULL != authorization) && (0 == g_ascii_strncasecmp(authorization, "Bearer ", 7)))
		bearer = g_strstrip(g_strdup(authorization + 7));

	if ((NULL == bearer) || ('\0' == bearer[0]))
	{
		HtmxResponse *refused;

		refused = venture_web_mcp_rpc_error(401, -32001,
			"POST /mcp needs an API token in an \"Authorization: Bearer <token>\" header. "
			"Mint one at /account/tokens; it is shown once. A browser session is not accepted here.");
		htmx_response_add_header(refused, "WWW-Authenticate", "Bearer realm=\"venture\"");
		return refused;
	}

	principal = venture_auth_authenticate(self->auth, request);

	if (!venture_web_hosted_auth_require(self, request, principal, VENTURE_USER_ROLE_VIEWER, &error))
	{
		HtmxResponse *refused;

		refused = venture_web_mcp_rpc_error(
			venture_error_to_http_status((VentureError)error->code), -32001,
			"VENTURE rejected the bearer token: it is wrong, revoked or expired, or its "
			"account may not use the API. This is not a missing record.");
		htmx_response_add_header(refused, "WWW-Authenticate",
			"Bearer realm=\"venture\", error=\"invalid_token\"");
		return refused;
	}

	/* A valid cookie beside a bad bearer would authenticate as the cookie.
	 * With authentication on, the credential that counted must be the
	 * token -- or the identity provider's token a hosted install trusts. */
	if (venture_auth_is_required(self->auth) &&
	    (principal->token_id <= 0) && !principal->provider_token)
	{
		HtmxResponse *refused;

		refused = venture_web_mcp_rpc_error(401, -32001,
			"POST /mcp authenticates by bearer token only, and the one sent was not accepted.");
		htmx_response_add_header(refused, "WWW-Authenticate",
			"Bearer realm=\"venture\", error=\"invalid_token\"");
		return refused;
	}

	/* --- What is asked --- */

	content_type = soup_message_headers_get_one(headers, "Content-Type");

	if ((NULL == content_type) || (0 != g_ascii_strncasecmp(content_type, "application/json", 16)))
		return venture_web_mcp_rpc_error(415, -32600,
			"POST /mcp takes a JSON-RPC message as application/json.");

	body = htmx_request_get_body(request);
	message = ((NULL != body) && ('\0' != body[0])) ? venture_json_parse(body, NULL) : NULL;

	if (NULL == message)
		return venture_web_mcp_rpc_error(400, -32700, "The body is not JSON.");

	if (!JSON_NODE_HOLDS_OBJECT(message) &&
	    !(JSON_NODE_HOLDS_ARRAY(message) && (json_array_get_length(json_node_get_array(message)) > 0)))
		return venture_web_mcp_rpc_error(400, -32600,
			"A JSON-RPC message is an object, or a non-empty array of them.");

	/* --- Hand it to a worker --- */

	job = g_new0(VentureWebMcpJob, 1);
	job->loopback_url = venture_web_mcp_loopback_url(self);

	if (NULL == job->loopback_url)
	{
		venture_web_mcp_job_free(job);
		return venture_web_mcp_rpc_error(503, -32603, "This server has not finished starting.");
	}

	job->token = g_steal_pointer(&bearer);
	job->host = g_strdup(soup_message_headers_get_one(headers, "Host"));
	job->stage_writes = !apply_writes;
	job->message = g_steal_pointer(&message);

	/* Appended, not replaced: behind a reverse proxy the chain already
	 * names the client and the proxy, and both stay true. */
	peer = venture_auth_remote_address(request);
	forwarded = soup_message_headers_get_list(headers, "X-Forwarded-For");

	if ((NULL != forwarded) && (NULL != peer))
		job->forwarded_for = g_strdup_printf("%s, %s", forwarded, peer);
	else if (NULL != peer)
		job->forwarded_for = g_strdup(peer);
	else
		job->forwarded_for = g_strdup(forwarded);

	if (NULL != soup_server_get_tls_certificate(htmx_server_get_soup_server(self->server)))
		job->certificate = g_object_ref(
			soup_server_get_tls_certificate(htmx_server_get_soup_server(self->server)));

	return htmx_response_new_threaded(venture_web_mcp_run, job, venture_web_mcp_job_free);
}

/*
 * GET and DELETE /mcp. The transport lets a server decline both: GET opens
 * a stream for messages the server starts, and this server starts none;
 * DELETE ends a session, and this server keeps none.
 */
static HtmxResponse *
venture_web_mcp_not_allowed(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	HtmxResponse *response;
	gboolean enabled = TRUE;

	(void)request;
	(void)params;

	g_object_get(venture_context_get_config(self->context), "mcp-enabled", &enabled, NULL);

	if (!enabled)
		return htmx_response_not_found();

	response = venture_web_mcp_rpc_error(405, -32600,
		"POST a JSON-RPC message to /mcp. This server opens no event stream and keeps no session.");
	htmx_response_add_header(response, "Allow", "POST");

	return response;
}
