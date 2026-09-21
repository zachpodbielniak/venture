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
