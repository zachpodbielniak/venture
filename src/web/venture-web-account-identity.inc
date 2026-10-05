/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Authentication and validation failures share one boundary diagnostic;
 * token names, credentials and hosted capability URLs never enter it. */
static HtmxResponse *
account_request_finish(HtmxResponse *response, const gchar *what)
{
	const GError *error = g_object_get_data(G_OBJECT(response), "venture-error");
	if (error != NULL)
		g_message("%s request failed: cause=%s domain=%s code=%d", what,
			error->domain == VENTURE_ERROR ? venture_error_get_slug(error->code) : "internal_failure",
			g_quark_to_string(error->domain), error->code);
	htmx_response_add_header(response, "Cache-Control", "no-store");
	return response;
}

static HtmxResponse *
account_identity_finish(HtmxResponse *response)
{
	return account_request_finish(response, "Account identity");
}

static HtmxResponse *
signup_finish(HtmxResponse *response)
{
	return account_request_finish(response, "Lightsite sign-up");
}

static HtmxResponse *
venture_web_account_identity(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	VentureDatabase *database = venture_context_get_database(self->context);
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureAccessScope) scope = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) identity = NULL;
	HtmxResponse *response = venture_web_api_require(self, request, VENTURE_USER_ROLE_VIEWER);
	gint64 id;
	if (response) return account_identity_finish(response);
	if (!g_ascii_string_to_signed(g_hash_table_lookup(params, "id"), 10, 1, G_MAXINT64, &id, NULL)) {
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "A positive organization ID is required");
		response = venture_web_error_response(error);
	} else {
		principal = venture_auth_authenticate(self->auth, request);
		scope = venture_access_policy_enter(venture_database_get_access_policy(database), principal);
		identity = venture_tenant_service_account_identity(venture_tenant_service_get(database), id, &error);
		response = identity ? venture_web_json_response(identity, 200) : venture_web_error_response(error);
	}
	return account_identity_finish(response);
}

/* POST /api/v1/lightsite/signups. A closed object of five strings, at most
 * 4096 bytes; only a bearer token reaches the service, which decides the
 * authority. A browser session is refused as no service credential. */
static HtmxResponse *
venture_web_lightsite_signup(HtmxRequest *request, GHashTable *params, gpointer data)
{
	static const gchar *const members[] = { "idempotency_key", "email", "issuer", "subject", "business_name" };
	VentureWebServer *self = data;
	VentureDatabase *database = venture_context_get_database(self->context);
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureAccessScope) scope = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(JsonNode) result = NULL;
	const gchar *values[G_N_ELEMENTS(members)];
	GBytes *body = htmx_request_get_body_bytes(request);
	const gchar *bytes = NULL;
	JsonNode *root;
	gsize length = 0;
	gboolean created = FALSE;
	guint i;
	HtmxResponse *response = venture_web_api_require(self, request, VENTURE_USER_ROLE_VIEWER);
	(void)params;
	if (response) return signup_finish(response);
	principal = venture_auth_authenticate(self->auth, request);
	if (principal->token_id <= 0) {
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_UNAUTHENTICATED, "A service bearer token is required");
		return signup_finish(venture_web_error_response(error));
	}
	if (body) bytes = g_bytes_get_data(body, &length);
	if (length > 4096) {
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Sign-up body exceeds 4096 bytes");
		response = venture_web_error_response(error);
		htmx_response_set_status(response, 413);
		return signup_finish(response);
	}
	/* An escaped NUL would silently truncate a member once it is a C string. */
	if (!bytes || !length || memchr(bytes, '\0', length) || g_strstr_len(bytes, (gssize)length, "\\u0000") ||
	    !json_parser_load_from_data(parser, bytes, (gssize)length, NULL)) goto malformed;
	root = json_parser_get_root(parser);
	if (!root || !JSON_NODE_HOLDS_OBJECT(root) || json_object_get_size(json_node_get_object(root)) != G_N_ELEMENTS(members))
		goto malformed;
	for (i = 0; i < G_N_ELEMENTS(members); i++) {
		JsonNode *member = json_object_get_member(json_node_get_object(root), members[i]);
		if (!member || !JSON_NODE_HOLDS_VALUE(member) || json_node_get_value_type(member) != G_TYPE_STRING) goto malformed;
		values[i] = json_node_get_string(member);
	}
	scope = venture_access_policy_enter(venture_database_get_access_policy(database), principal);
	result = venture_tenant_service_lightsite_signup(venture_tenant_service_get(database),
		values[0], values[1], values[2], values[3], values[4], &created, &error);
	return signup_finish(result ? venture_web_json_response(result, created ? 201 : 200) : venture_web_error_response(error));
malformed:
	g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		"Expected a JSON object of exactly idempotency_key, email, issuer, subject and business_name strings");
	return signup_finish(venture_web_error_response(error));
}

static HtmxResponse *
identity_link_finish(HtmxResponse *response)
{
	return account_request_finish(response, "Identity link");
}

/* POST /api/v1/account/identity-link: {"access_token": "<the person's own
 * provider access token>"}, sent by the person signed in to Venture itself.
 * The token is verified and never echoed, stored or logged. */
static HtmxResponse *
venture_web_identity_link(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	VentureDatabase *database = venture_context_get_database(self->context);
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureAccessScope) scope = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(JsonNode) result = NULL;
	GBytes *body = htmx_request_get_body_bytes(request);
	const gchar *bytes = NULL;
	JsonNode *root, *member;
	gsize length = 0;
	HtmxResponse *response = venture_web_api_require(self, request, VENTURE_USER_ROLE_VIEWER);
	(void)params;
	if (response) return identity_link_finish(response);
	principal = venture_auth_authenticate(self->auth, request);
	if (body) bytes = g_bytes_get_data(body, &length);
	if (!bytes || !length || length > 16384 || memchr(bytes, '\0', length) ||
	    !json_parser_load_from_data(parser, bytes, (gssize)length, NULL)) goto malformed;
	root = json_parser_get_root(parser);
	if (!root || !JSON_NODE_HOLDS_OBJECT(root) || json_object_get_size(json_node_get_object(root)) != 1) goto malformed;
	member = json_object_get_member(json_node_get_object(root), "access_token");
	if (!member || !JSON_NODE_HOLDS_VALUE(member) || json_node_get_value_type(member) != G_TYPE_STRING) goto malformed;
	scope = venture_access_policy_enter(venture_database_get_access_policy(database), principal);
	result = venture_tenant_service_link_identity(venture_tenant_service_get(database), json_node_get_string(member), &error);
	return identity_link_finish(result ? venture_web_json_response(result, 200) : venture_web_error_response(error));
malformed:
	g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Expected a JSON object with one access_token string");
	return identity_link_finish(venture_web_error_response(error));
}

static HtmxResponse *
venture_web_account_authority(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	VentureDatabase *database = venture_context_get_database(self->context);
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureAccessScope) scope = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) authority = NULL;
	HtmxResponse *response = venture_web_api_require(self, request, VENTURE_USER_ROLE_VIEWER);
	(void)params;
	if (response != NULL) return account_identity_finish(response);
	principal = venture_auth_authenticate(self->auth, request);
	scope = venture_access_policy_enter(venture_database_get_access_policy(database), principal);
	authority = venture_tenant_service_account_authority(venture_tenant_service_get(database), &error);
	return account_identity_finish(authority != NULL ? venture_web_json_response(authority, 200) : venture_web_error_response(error));
}
