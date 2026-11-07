/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * Connect with Stripe over HTTP. Included after venture-stripe-web.inc.
 *
 *   GET  /api/v1/organizations/:organization_id/stripe/connect   the state
 *   POST /api/v1/organizations/:organization_id/stripe/connect   account + link
 *   POST /webhooks/stripe/connect                                 platform events
 *
 * The two API routes act as the calling person: only an owner or admin of
 * the business passes (the same check as the Stripe settings page).
 */

/* Tests put a service with an offline transport on the context; the
 * routes use its transport. Production has none and uses the network. */
static StripeTransport *
stripe_connect_transport(VentureWebServer *self)
{
	VentureStripeService *installed = venture_context_get_stripe_service(self->context);
	return installed ? venture_stripe_service_get_transport(installed) : NULL;
}

static const gchar *
stripe_connect_member(JsonObject *object, const gchar *name)
{
	JsonNode *node = json_object_get_member(object, name);
	return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING ?
		json_node_get_string(node) : NULL;
}

static HtmxResponse *
venture_web_stripe_connect(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	VentureWebServer *self = user_data;
	VentureDatabase *database = venture_context_get_database(self->context);
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureAccessScope) scope = NULL;
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(JsonNode) result = NULL;
	g_autoptr(GError) error = NULL;
	GBytes *body = htmx_request_get_body_bytes(request);
	gsize length = 0;
	const gchar *bytes = body ? g_bytes_get_data(body, &length) : NULL;
	gint64 organization_id = 0;
	VentureActor actor;
	HtmxResponse *response = venture_web_api_require(self, request, VENTURE_USER_ROLE_VIEWER);
	if (response) return response;
	if (!venture_context_module_enabled(self->context, "stripe"))
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Stripe module is disabled");
		return venture_web_error_response(error);
	}
	if (!g_ascii_string_to_signed(g_hash_table_lookup(params, "organization_id"), 10, 1, G_MAXINT64 - 1, &organization_id, NULL))
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "A positive organization ID is required");
		return venture_web_error_response(error);
	}
	principal = venture_auth_authenticate(self->auth, request);
	venture_auth_to_actor(principal, &actor);
	scope = venture_access_policy_enter(venture_database_get_access_policy(database), principal);
	if (htmx_request_get_method(request) == HTMX_METHOD_GET)
		result = venture_stripe_connect_status(database, organization_id, stripe_connect_transport(self), &actor, &error);
	else
	{
		JsonNode *root;
		JsonObject *object;
		const gchar *return_url, *refresh_url, *success_url, *cancel_url;
		/* An escaped NUL would silently cut a URL once it is a C string. */
		if (!bytes || !length || length > 4096 || memchr(bytes, '\0', length) ||
			g_strstr_len(bytes, (gssize)length, "\\u0000") || !json_parser_load_from_data(parser, bytes, (gssize)length, NULL))
			goto malformed;
		root = json_parser_get_root(parser);
		if (!root || !JSON_NODE_HOLDS_OBJECT(root)) goto malformed;
		object = json_node_get_object(root);
		return_url = stripe_connect_member(object, "return_url");
		refresh_url = stripe_connect_member(object, "refresh_url");
		success_url = stripe_connect_member(object, "success_url");
		cancel_url = stripe_connect_member(object, "cancel_url");
		if (json_object_get_size(object) != 4 || !return_url || !refresh_url || !success_url || !cancel_url)
			goto malformed;
		result = venture_stripe_connect_start(database, organization_id, return_url, refresh_url,
			success_url, cancel_url, stripe_connect_transport(self), &actor, &error);
	}
	response = result ? venture_web_json_response(result, 200) : venture_web_error_response(error);
	htmx_response_add_header(response, "Cache-Control", "no-store");
	return response;
malformed:
	g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		"Expected a JSON object of exactly return_url, refresh_url, success_url and cancel_url (HTTPS strings)");
	return venture_web_error_response(error);
}

/* 200 when handled or ignored, 400 when refused, 503 when Stripe or
 * Connect is off on this installation: Stripe retries the last two. */
static HtmxResponse *
venture_web_stripe_connect_webhook(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	VentureWebServer *self = user_data;
	g_autoptr(GError) error = NULL;
	HtmxResponse *response = htmx_response_new();
	GBytes *body = htmx_request_get_body_bytes(request);
	SoupMessageHeaders *headers = soup_server_message_get_request_headers(htmx_request_get_message(request));
	const gchar *signature = soup_message_headers_get_one(headers, "Stripe-Signature");
	(void)params;
	if (!venture_context_module_enabled(self->context, "stripe"))
		htmx_response_set_status(response, 503);
	else if (!body)
		htmx_response_set_status(response, 400);
	else if (venture_stripe_connect_webhook(venture_context_get_database(self->context), body, signature,
		stripe_connect_transport(self), &error))
		htmx_response_set_status(response, 200);
	else
		htmx_response_set_status(response, g_error_matches(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND) ||
			g_error_matches(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG) ? 503 : 400);
	return response;
}

/* Before /webhooks/stripe/:connection_id: the router takes the first match. */
static void
venture_stripe_connect_web_register(VentureWebServer *self)
{
	venture_web_server_add_classified_route(self, HTMX_METHOD_POST, "/webhooks/stripe/connect",
		VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, venture_web_stripe_connect_webhook, self);
	venture_web_server_add_classified_route(self, HTMX_METHOD_GET, "/api/v1/organizations/:organization_id/stripe/connect",
		VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, venture_web_stripe_connect, self);
	venture_web_server_add_classified_route(self, HTMX_METHOD_POST, "/api/v1/organizations/:organization_id/stripe/connect",
		VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, venture_web_stripe_connect, self);
}
