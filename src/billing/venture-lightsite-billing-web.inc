/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * /api/v1/lightsite/billing: Lightsite binding a business to a plan and
 * reading what it owes. Included after venture-web-account-identity.inc,
 * whose boundary diagnostic and no-store header every answer shares. Like
 * the sign-up, only a bearer token reaches the service, which judges the
 * trusted-service authority; a browser session is no service credential.
 */

static HtmxResponse *
lightsite_billing_finish(HtmxResponse *response)
{
	return account_request_finish(response, "Lightsite billing");
}

static gint64
lightsite_billing_organization(VentureWebServer *self)
{
	gint64 id = 0;
	g_object_get(venture_context_get_config(self->context), "lightsite-billing-organization-id", &id, NULL);
	return id;
}

/* NULL to proceed, with @principal set; otherwise the refusal. */
static HtmxResponse *
lightsite_billing_gate(VentureWebServer *self, HtmxRequest *request, VentureAuthPrincipal **principal)
{
	g_autoptr(GError) error = NULL;
	HtmxResponse *response = venture_web_api_require(self, request, VENTURE_USER_ROLE_VIEWER);
	if (response) return response;
	*principal = venture_auth_authenticate(self->auth, request);
	if ((*principal)->token_id <= 0) {
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_UNAUTHENTICATED, "A service bearer token is required");
		return venture_web_error_response(error);
	}
	return NULL;
}

/* POST /api/v1/lightsite/billing/subscriptions. A closed object of exactly
 * organization_id (an integer), plan_code and idempotency_key (strings),
 * at most 4096 bytes. */
static HtmxResponse *
venture_web_lightsite_billing_subscribe(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	VentureDatabase *database = venture_context_get_database(self->context);
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureAccessScope) scope = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(JsonNode) result = NULL;
	GBytes *body = htmx_request_get_body_bytes(request);
	const gchar *bytes = NULL, *plan_code, *key;
	JsonObject *object;
	JsonNode *root, *organization, *plan, *idempotency;
	gsize length = 0;
	gboolean created = FALSE;
	HtmxResponse *response = lightsite_billing_gate(self, request, &principal);
	(void)params;
	if (response) return lightsite_billing_finish(response);
	if (body) bytes = g_bytes_get_data(body, &length);
	if (length > 4096) {
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Billing body exceeds 4096 bytes");
		response = venture_web_error_response(error);
		htmx_response_set_status(response, 413);
		return lightsite_billing_finish(response);
	}
	/* An escaped NUL would silently truncate a member once it is a C string. */
	if (!bytes || !length || memchr(bytes, '\0', length) || g_strstr_len(bytes, (gssize)length, "\\u0000") ||
	    !json_parser_load_from_data(parser, bytes, (gssize)length, NULL)) goto malformed;
	root = json_parser_get_root(parser);
	if (!root || !JSON_NODE_HOLDS_OBJECT(root)) goto malformed;
	object = json_node_get_object(root);
	organization = json_object_get_member(object, "organization_id");
	plan = json_object_get_member(object, "plan_code");
	idempotency = json_object_get_member(object, "idempotency_key");
	if (json_object_get_size(object) != 3 ||
	    !organization || !JSON_NODE_HOLDS_VALUE(organization) || json_node_get_value_type(organization) != G_TYPE_INT64 ||
	    !plan || !JSON_NODE_HOLDS_VALUE(plan) || json_node_get_value_type(plan) != G_TYPE_STRING ||
	    !idempotency || !JSON_NODE_HOLDS_VALUE(idempotency) || json_node_get_value_type(idempotency) != G_TYPE_STRING)
		goto malformed;
	/* JSON-GLib clamps an integer too large for 64 bits to the maximum:
	 * that is not the ID the caller sent, so it is not answered as one. */
	if (json_node_get_int(organization) == G_MAXINT64 || json_node_get_int(organization) == G_MININT64)
		goto malformed;
	plan_code = json_node_get_string(plan);
	key = json_node_get_string(idempotency);
	scope = venture_access_policy_enter(venture_database_get_access_policy(database), principal);
	result = venture_lightsite_billing_subscribe(database, lightsite_billing_organization(self),
		json_node_get_int(organization), plan_code, key, &created, &error);
	return lightsite_billing_finish(result ? venture_web_json_response(result, created ? 201 : 200) : venture_web_error_response(error));
malformed:
	g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		"Expected a JSON object of exactly organization_id (integer), plan_code and idempotency_key (strings)");
	return lightsite_billing_finish(venture_web_error_response(error));
}

/* GET /api/v1/lightsite/billing/:organization_id */
static HtmxResponse *
venture_web_lightsite_billing_view(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	VentureDatabase *database = venture_context_get_database(self->context);
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureAccessScope) scope = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) result = NULL;
	gint64 id = 0;
	HtmxResponse *response = lightsite_billing_gate(self, request, &principal);
	if (response) return lightsite_billing_finish(response);
	if (!g_ascii_string_to_signed(g_hash_table_lookup(params, "organization_id"), 10, 1, G_MAXINT64, &id, NULL)) {
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "A positive organization ID is required");
		return lightsite_billing_finish(venture_web_error_response(error));
	}
	scope = venture_access_policy_enter(venture_database_get_access_policy(database), principal);
	result = venture_lightsite_billing_view(database, lightsite_billing_organization(self), id, &error);
	return lightsite_billing_finish(result ? venture_web_json_response(result, 200) : venture_web_error_response(error));
}

/* GET /api/v1/lightsite/billing: the staff overview. */
static HtmxResponse *
venture_web_lightsite_billing_overview(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	VentureDatabase *database = venture_context_get_database(self->context);
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureAccessScope) scope = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) result = NULL;
	HtmxResponse *response = lightsite_billing_gate(self, request, &principal);
	(void)params;
	if (response) return lightsite_billing_finish(response);
	scope = venture_access_policy_enter(venture_database_get_access_policy(database), principal);
	result = venture_lightsite_billing_overview(database, lightsite_billing_organization(self), &error);
	return lightsite_billing_finish(result ? venture_web_json_response(result, 200) : venture_web_error_response(error));
}

static void
venture_lightsite_billing_web_register(HtmxRouter *router, VentureWebServer *self)
{
	(void)router;
	venture_web_server_add_classified_route(self, HTMX_METHOD_POST, "/api/v1/lightsite/billing/subscriptions",
		VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, venture_web_lightsite_billing_subscribe, self);
	venture_web_server_add_classified_route(self, HTMX_METHOD_GET, "/api/v1/lightsite/billing/:organization_id",
		VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, venture_web_lightsite_billing_view, self);
	venture_web_server_add_classified_route(self, HTMX_METHOD_GET, "/api/v1/lightsite/billing",
		VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, venture_web_lightsite_billing_overview, self);
}
