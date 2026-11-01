/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Capability exchange is an authentication ceremony. Administrative entity
 * forms and actions remain generated from their shared declarations. */
static HtmxResponse *
venture_tenant_web_invitation(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	VentureDatabase *database = venture_context_get_database(self->context);
	VentureTenantService *service = venture_tenant_service_get(database);
	g_autoptr(VentureAuthPrincipal) principal = venture_auth_authenticate(self->auth, request);
	g_autoptr(VentureAccessScope) access = venture_access_policy_enter(venture_database_get_access_policy(database),
		principal->authenticated ? principal : NULL);
	g_autoptr(VentureUser) accepted = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GString) body = g_string_new("<h1>Accept workspace invitation</h1>");
	HtmxResponse *response;
	(void)params;
	if (!venture_tenant_service_is_enabled(service)) return venture_web_html_response(g_strdup("Workspace invitations are unavailable"), 404);
	if (htmx_request_get_method(request) == HTMX_METHOD_POST) {
		GBytes *bytes = htmx_request_get_body_bytes(request);
		if (!bytes || g_bytes_get_size(bytes) > 16384) return venture_web_html_response(g_strdup("Invitation input is too large"), 413);
		accepted = venture_tenant_service_accept_invitation(service, venture_context_get_config(self->context),
			htmx_request_get_form_value(request, "capability"),
			principal->authenticated ? NULL : htmx_request_get_form_value(request, "username"),
			principal->authenticated ? NULL : htmx_request_get_form_value(request, "password"), &error);
		if (accepted) g_string_append(body, "<p>Invitation accepted. Sign in with your own account to continue.</p><a href=\"/login\">Sign in</a>");
		else g_string_append(body, "<p>Invitation was not accepted. Check the capability, expiry and chosen identity with the administrator.</p>");
	}
	if (!accepted) {
		g_string_append(body, "<form method=\"post\" action=\"/account/invitation\"><label>Invitation capability<input type=\"password\" name=\"capability\" autocomplete=\"off\" required maxlength=\"256\"></label>");
		if (principal->authenticated)
			g_string_append(body, "<p>This explicitly accepts for your current signed-in identity.</p>");
		else g_string_append(body, "<p>Use a new username for an ordinary invitation. For a targeted recovery invitation, use the exact retained username. To accept an ordinary invitation for an existing account, sign in first.</p><label>Username<input name=\"username\" required maxlength=\"128\" autocomplete=\"username\"></label><label>New password<input name=\"password\" type=\"password\" required maxlength=\"4096\" autocomplete=\"new-password\"></label>");
		g_string_append(body, "<button type=\"submit\">Accept invitation</button></form>");
		/* An emailed link carries the capability after '#', which no browser
		 * sends to a server; it is moved into the form and off the address. */
		g_string_append(body, "<script>(function(){var m=/^#code=([A-Za-z0-9_-]{32,256})$/.exec(location.hash);"
			"var f=document.querySelector('input[name=capability]');if(m&&f){f.value=m[1];"
			"history.replaceState(null,'',location.pathname);}})();</script>");
	}
	response = venture_web_html_response(venture_web_auth_page(self, request, body->str), error ? 400 : 200);
	htmx_response_add_header(response, "Cache-Control", "no-store");
	/* Suppress cross-origin referrers without turning same-origin form
	 * submissions into opaque Origin: null requests in browsers. */
	htmx_response_add_header(response, "Referrer-Policy", "same-origin");
	return response;
}

static HtmxResponse *
venture_tenant_web_support(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	VentureTenantService *service = venture_tenant_service_get(venture_context_get_database(self->context));
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureTenantSupportScope) support = NULL;
	g_autoptr(GError) error = NULL;
	HtmxResponse *response = venture_web_ui_require_session(self, request);
	(void)params;
	if (response) return response;
	if (!venture_tenant_service_is_enabled(service)) return venture_web_html_response(g_strdup("Scoped support is unavailable"), 404);
	principal = venture_auth_authenticate(self->auth, request);
	if (htmx_request_get_method(request) == HTMX_METHOD_POST) {
		g_autoptr(HtmxCookie) cookie = NULL;
		g_autofree gchar *encoded = NULL;
		const gchar *capability = htmx_request_get_form_value(request, "capability");
		GBytes *bytes = htmx_request_get_body_bytes(request);
		if (!bytes || g_bytes_get_size(bytes) > 2048) return venture_web_html_response(g_strdup("Support input is too large"), 413);
		if (capability && *capability) {
			support = venture_tenant_service_enter_request(service, principal, capability, "/account/support", &error);
			if (!support) return venture_web_error_response(error);
		}
		cookie = htmx_cookie_new("venture_support", capability ? capability : "");
		htmx_cookie_set_path(cookie, "/"); htmx_cookie_set_http_only(cookie, TRUE);
		htmx_cookie_set_same_site(cookie, HTMX_COOKIE_SAME_SITE_STRICT);
		htmx_cookie_set_secure(cookie, g_str_has_prefix(venture_tenant_service_get_origin(service), "https://"));
		htmx_cookie_set_max_age(cookie, capability && *capability ? 3600 : 0);
		encoded = htmx_cookie_to_set_cookie(cookie);
		response = venture_web_redirect_to(capability && *capability ? "/e/invoice" : "/account/support");
		htmx_response_add_header(response, "Set-Cookie", encoded);
	} else response = venture_web_html_response(venture_web_auth_page(self, request,
		"<h1>Scoped operator support</h1><p>Use the separately issued capability for your own operator identity. Each request rechecks its organization, expiry and revocation. Leave empty to end browser support access.</p><form method=\"post\" action=\"/account/support\"><label>Support capability<input type=\"password\" name=\"capability\" autocomplete=\"off\" maxlength=\"256\"></label><button type=\"submit\">Set support capability</button></form>"), 200);
	htmx_response_add_header(response, "Cache-Control", "no-store");
	/* Suppress cross-origin referrers without turning same-origin form
	 * submissions into opaque Origin: null requests in browsers. */
	htmx_response_add_header(response, "Referrer-Policy", "same-origin");
	return response;
}

/* /api/v1/organizations/:organization_id/invitations: the business owner's
 * own invitations. The owner's credential (a session, their token or their
 * provider token); the tenant service judges the ownership. Bodies are
 * closed JSON objects of at most 4096 bytes. */
static JsonObject *
owner_invitation_body(HtmxRequest *request, JsonParser *parser)
{
	GBytes *body = htmx_request_get_body_bytes(request);
	gsize length = 0;
	const gchar *bytes = body ? g_bytes_get_data(body, &length) : NULL;
	JsonNode *root;
	/* An escaped NUL would silently truncate a member once it is a C string. */
	if (!bytes || !length || length > 4096 || memchr(bytes, '\0', length) ||
	    g_strstr_len(bytes, (gssize)length, "\\u0000") || !json_parser_load_from_data(parser, bytes, (gssize)length, NULL))
		return NULL;
	root = json_parser_get_root(parser);
	return root && JSON_NODE_HOLDS_OBJECT(root) ? json_node_get_object(root) : NULL;
}

static const gchar *
owner_invitation_string(JsonObject *object, const gchar *name)
{
	JsonNode *node = json_object_get_member(object, name);
	return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING ?
		json_node_get_string(node) : NULL;
}

static HtmxResponse *
venture_tenant_web_owner_invitations(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	VentureDatabase *database = venture_context_get_database(self->context);
	VentureTenantService *service = venture_tenant_service_get(database);
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureAccessScope) scope = NULL;
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(JsonNode) result = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *path = htmx_request_get_path(request), *invitation_text = g_hash_table_lookup(params, "invitation_id");
	gint64 organization_id = 0, invitation_id = 0;
	gboolean created = FALSE;
	JsonObject *object = NULL;
	HtmxResponse *response = venture_web_api_require(self, request, VENTURE_USER_ROLE_VIEWER);
	if (response) return account_request_finish(response, "Owner invitation");
	if (!g_ascii_string_to_signed(g_hash_table_lookup(params, "organization_id"), 10, 1, G_MAXINT64 - 1, &organization_id, NULL) ||
	    (invitation_text && !g_ascii_string_to_signed(invitation_text, 10, 1, G_MAXINT64 - 1, &invitation_id, NULL))) {
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Positive organization and invitation IDs are required");
		return account_request_finish(venture_web_error_response(error), "Owner invitation");
	}
	if (htmx_request_get_method(request) == HTMX_METHOD_POST) {
		object = owner_invitation_body(request, parser);
		if (!object) goto malformed;
	}
	principal = venture_auth_authenticate(self->auth, request);
	scope = venture_access_policy_enter(venture_database_get_access_policy(database), principal);
	if (htmx_request_get_method(request) == HTMX_METHOD_GET)
		result = venture_tenant_service_owner_invitations(service, organization_id, &error);
	else if (g_str_has_suffix(path, "/resend")) {
		const gchar *key = owner_invitation_string(object, "idempotency_key");
		if (json_object_get_size(object) != 1 || !key) goto malformed;
		result = venture_tenant_service_owner_resend(service, organization_id, invitation_id, key, &created, &error);
	} else if (g_str_has_suffix(path, "/revoke")) {
		if (json_object_get_size(object) != 0) goto malformed;
		result = venture_tenant_service_owner_revoke(service, organization_id, invitation_id, &error);
	} else {
		const gchar *email = owner_invitation_string(object, "email"), *role = owner_invitation_string(object, "role");
		const gchar *key = owner_invitation_string(object, "idempotency_key"), *name = owner_invitation_string(object, "name");
		JsonNode *confirmed = json_object_get_member(object, "confirmed");
		/* Sending mail to someone on the business's behalf is agreed to,
		 * never implied: confirmed must be literally true. */
		if (!email || !role || !key || (json_object_has_member(object, "name") && !name) ||
		    json_object_get_size(object) != (name ? 5u : 4u) || !confirmed || !JSON_NODE_HOLDS_VALUE(confirmed) ||
		    json_node_get_value_type(confirmed) != G_TYPE_BOOLEAN || !json_node_get_boolean(confirmed)) goto malformed;
		result = venture_tenant_service_owner_invite(service, organization_id, email, name, role, key, &created, &error);
	}
	return account_request_finish(result ? venture_web_json_response(result, created ? 201 : 200) :
		venture_web_error_response(error), "Owner invitation");
malformed:
	g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, g_str_has_suffix(path, "/resend") ?
		"Expected exactly idempotency_key" : g_str_has_suffix(path, "/revoke") ? "Expected an empty JSON object" :
		"Expected email, role, idempotency_key, confirmed: true and an optional name");
	return account_request_finish(venture_web_error_response(error), "Owner invitation");
}
