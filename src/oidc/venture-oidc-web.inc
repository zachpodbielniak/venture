/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* These are authentication settings, not entity editors. Verified identity
 * creation remains an operation of the service, never generic record CRUD. */
static gchar *venture_oidc_web_browser_cookie(VentureWebServer *self, const gchar *value)
{
	g_autoptr(HtmxCookie) cookie = htmx_cookie_new("venture_oidc", value ? value : "");
	gboolean secure = FALSE;
	g_object_get(venture_context_get_config(self->context), "security-cookie-secure", &secure, NULL);
	htmx_cookie_set_path(cookie, "/auth/oidc"); htmx_cookie_set_http_only(cookie, TRUE);
	htmx_cookie_set_same_site(cookie, HTMX_COOKIE_SAME_SITE_LAX); htmx_cookie_set_secure(cookie, secure);
	htmx_cookie_set_max_age(cookie, value ? 300 : 0);
	return htmx_cookie_to_set_cookie(cookie);
}
static HtmxResponse *venture_oidc_web_redirect(VentureWebServer *self, const gchar *url, const gchar *browser)
{
	g_autofree gchar *cookie = venture_oidc_web_browser_cookie(self, browser);
	HtmxResponse *response = venture_web_redirect_to(url);
	htmx_response_add_header(response, "Set-Cookie", cookie);
	htmx_response_add_header(response, "Cache-Control", "no-store");
	htmx_response_add_header(response, "Referrer-Policy", "no-referrer");
	return response;
}
static HtmxResponse *venture_oidc_web_start(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	VentureOidcService *service = venture_oidc_service_get(venture_context_get_database(self->context));
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *browser = NULL, *url = NULL, *remote = venture_auth_remote_address(request);
	gboolean linking = htmx_request_get_method(request) == HTMX_METHOD_POST;
	HtmxResponse *gate;
	const gchar *uuid;
	(void)params;
	/* Public sign-in is guarded by one-use state, PKCE and the bound browser;
	 * linking additionally requires a live local session and its password. */
	if (linking) { gate = venture_web_ui_require_session(self, request); if (gate) return gate; }
	gate = venture_web_require_module_ui(self, request, "oidc"); if (gate) return gate;
	principal = venture_auth_authenticate(self->auth, request);
	if (linking && (!principal->authenticated || principal->user_id <= 0)) return venture_web_redirect_to("/login");
	if (!linking && principal->authenticated && principal->user_id > 0) return venture_web_redirect_to("/account/oidc");
	uuid = linking ? htmx_request_get_form_value(request, "provider") : htmx_request_get_query_param(request, "provider");
	url = venture_oidc_service_begin(service, uuid, linking ? principal->user_id : 0,
		linking ? htmx_request_get_form_value(request, "password") : NULL, remote, &browser, &error);
	if (!url) return venture_web_error_response(error);
	return venture_oidc_web_redirect(self, url, browser);
}
static HtmxResponse *venture_oidc_web_callback(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	g_autoptr(VentureAuthPrincipal) principal = venture_auth_authenticate(self->auth, request);
	g_autoptr(VentureOidcIdentity) identity = NULL;
	g_autoptr(GHashTable) cookies = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *cookie = NULL;
	SoupServerMessage *message = htmx_request_get_message(request);
	const gchar *header = soup_message_headers_get_list(soup_server_message_get_request_headers(message), "Cookie");
	HtmxResponse *response;
	gboolean mfa_pending = FALSE;
	(void)params;
	/* The callback has no local-session prerequisite. The service authenticates
	 * this response using state, browser binding, nonce and the ID signature. */
	cookies = header ? htmx_cookie_parse_request(header) : NULL;
	identity = venture_oidc_service_finish(venture_oidc_service_get(venture_context_get_database(self->context)),
		htmx_request_get_query_param(request, "state"), htmx_request_get_query_param(request, "code"),
		cookies ? g_hash_table_lookup(cookies, "venture_oidc") : NULL,
		principal->authenticated ? principal->user_id : 0, &error);
	if (!identity) {
		response = venture_web_html_response(venture_web_auth_page(self, request,
			"<h2>Organization sign-in was not verified</h2><p>Start again using your organization's sign-in link, or use your local password and recovery codes.</p><a href=\"/login\">Local sign-in</a>"), 401);
		cookie = venture_oidc_web_browser_cookie(self, NULL);
		htmx_response_add_header(response, "Set-Cookie", cookie);
		htmx_response_add_header(response, "Cache-Control", "no-store");
		htmx_response_add_header(response, "Referrer-Policy", "no-referrer");
		return response;
	}
	if (principal->authenticated && principal->user_id > 0)
		return venture_oidc_web_redirect(self, "/account/oidc", NULL);
	if (!venture_auth_login_identity(self->auth, venture_entity_get_id(VENTURE_ENTITY(identity)), &cookie, &mfa_pending, &error))
		return venture_web_error_response(error);
	response = venture_oidc_web_redirect(self, mfa_pending ? "/login/mfa" : "/", NULL);
	htmx_response_append_header(response, "Set-Cookie", cookie); return response;
}
static HtmxResponse *venture_oidc_web_settings(HtmxRequest *request, GHashTable *params, gpointer data)
{
	static const gint roles[] = { VENTURE_ORGANIZATION_ROLE_OWNER, VENTURE_ORGANIZATION_ROLE_ADMIN };
	VentureWebServer *self = data;
	VentureDatabase *db = venture_context_get_database(self->context);
	VentureOidcService *service = venture_oidc_service_get(db);
	VentureIntegrationService *integrations = venture_integration_service_get(db);
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureAccessScope) access = NULL;
	g_autoptr(VentureIntegrationConnection) binding = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GString) body = g_string_new("<h1>Organization sign-in</h1>");
	g_autofree gchar *callback = NULL, *uuid = NULL;
	gint64 org = 0, version = 0, id = 0;
	VentureActor actor;
	HtmxResponse *gate = venture_web_ui_require_session(self, request);
	if (gate) return gate;
	gate = venture_web_require_module_ui(self, request, "oidc"); if (gate) return gate;
	principal = venture_auth_authenticate(self->auth, request);
	if (!g_ascii_string_to_signed(g_hash_table_lookup(params, "id"), 10, 1, G_MAXINT64, &org, NULL) ||
		!venture_access_policy_has_organization_role(venture_database_get_access_policy(db), principal, org, roles, G_N_ELEMENTS(roles))) {
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED, "Organization integration administration is required"); return venture_web_error_response(error);
	}
	access = venture_access_policy_enter(venture_database_get_access_policy(db), principal); venture_auth_to_actor(principal, &actor);
	if (htmx_request_get_method(request) == HTMX_METHOD_POST) {
		const gchar *operation = htmx_request_get_form_value(request, "operation");
		const gchar *text = htmx_request_get_form_value(request, "version");
		if (!text || !g_ascii_string_to_signed(text, 10, 0, G_MAXINT64, &version, NULL))
			g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Use the current settings form");
		else if (!g_strcmp0(operation, "configure")) binding = venture_oidc_service_connect(service, org,
			htmx_request_get_form_value(request, "issuer"), htmx_request_get_form_value(request, "client_id"),
			htmx_request_get_form_value(request, "client_secret"), version, &actor, &error);
		else if (!g_strcmp0(operation, "disconnect")) {
			text = htmx_request_get_form_value(request, "connection_id");
			if (!text || !g_ascii_string_to_signed(text, 10, 1, G_MAXINT64, &id, NULL))
				g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Use the current provider connection");
			else venture_integration_service_disable(integrations, org, id, version, &actor, &error);
		} else g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Choose a provider settings operation");
		if (error) { g_string_append(body, "<p role=\"alert\">"); venture_html_escape_append(body, error->message); g_string_append(body, "</p>"); }
		g_clear_object(&binding);
	}
	binding = venture_integration_service_find(integrations, org, "oidc", NULL);
	callback = venture_oidc_service_callback_uri(service, NULL);
	g_string_append(body, "<p>Register a confidential authorization-code client with PKCE S256 and this exact callback:</p><code>");
	venture_html_escape_append(body, callback ? callback : "Configure the platform base URL and secure cookies first"); g_string_append(body, "</code>");
	if (binding) {
		g_object_get(binding, "uuid", &uuid, NULL); id = venture_entity_get_id(VENTURE_ENTITY(binding)); version = venture_entity_get_version(VENTURE_ENTITY(binding));
		g_string_append(body, "<p>Organization sign-in link: <a href=\"/auth/oidc/start?provider="); venture_html_escape_append(body, uuid);
		g_string_append(body, "\">Sign in with this provider</a>. Each existing member first links it from their local account.</p>");
	} else { version = 0; id = 0; g_string_append(body, "<p>No provider is connected.</p>"); }
	g_string_append(body, "<p>Credentials are write-only. Supply all three values on rotation. The issuer must be on the platform allowlist. Disconnect before replacing the issuer or client.</p><form method=\"post\">");
	g_string_append_printf(body, "<input type=\"hidden\" name=\"version\" value=\"%" G_GINT64_FORMAT "\"><input type=\"hidden\" name=\"connection_id\" value=\"%" G_GINT64_FORMAT "\">", version, id);
	g_string_append(body, "<label>Issuer<input name=\"issuer\" type=\"url\" autocomplete=\"off\"></label><label>Client ID<input name=\"client_id\" autocomplete=\"off\"></label><label>Client secret<input name=\"client_secret\" type=\"password\" autocomplete=\"new-password\"></label><button name=\"operation\" value=\"configure\">Connect or rotate</button>");
	if (binding) g_string_append(body, "<button name=\"operation\" value=\"disconnect\">Disconnect and revoke provider sessions</button>");
	g_string_append(body, "</form><p>Local passwords, administrator recovery and local second factors remain available.</p>");
	return venture_web_html_response(venture_web_page(self, request, "/organizations", "Organization sign-in", body->str), error ? 400 : 200);
}
static HtmxResponse *venture_oidc_web_account(HtmxRequest *request, GHashTable *params, gpointer data)
{
	static const gint roles[] = { VENTURE_ORGANIZATION_ROLE_VIEWER, VENTURE_ORGANIZATION_ROLE_OWNER, VENTURE_ORGANIZATION_ROLE_ADMIN,
		VENTURE_ORGANIZATION_ROLE_EDITOR, VENTURE_ORGANIZATION_ROLE_FINANCE, VENTURE_ORGANIZATION_ROLE_SALES,
		VENTURE_ORGANIZATION_ROLE_SUPPORT, VENTURE_ORGANIZATION_ROLE_ACCOUNTANT };
	VentureWebServer *self = data;
	VentureDatabase *db = venture_context_get_database(self->context);
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GString) body = g_string_new("<h1>Your organization sign-in links</h1><p>Sign in locally first, then explicitly link a provider. Keep your local password and second-factor recovery codes available.</p>");
	g_autoptr(GError) error = NULL;
	HtmxResponse *gate = venture_web_ui_require_session(self, request);
	guint i;
	(void)params;
	if (gate) return gate;
	gate = venture_web_require_module_ui(self, request, "oidc"); if (gate) return gate;
	principal = venture_auth_authenticate(self->auth, request);
	if (principal->user_id <= 0) return venture_web_redirect_to("/account");
	if (htmx_request_get_method(request) == HTMX_METHOD_POST) {
		gint64 id = 0;
		VentureActor actor;
		const gchar *text = htmx_request_get_form_value(request, "identity_id");
		venture_auth_to_actor(principal, &actor);
		if (!text || !g_ascii_string_to_signed(text, 10, 1, G_MAXINT64, &id, NULL))
			g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Choose your own identity link");
		else if (venture_oidc_service_unlink(venture_oidc_service_get(db), id, principal->user_id,
			htmx_request_get_form_value(request, "password"), &actor, &error)) return venture_web_redirect_to("/login");
		if (error) { g_string_append(body, "<p role=\"alert\">"); venture_html_escape_append(body, error->message); g_string_append(body, "</p>"); }
	}
	internal = venture_access_policy_enter(venture_database_get_access_policy(db), NULL);
	query = venture_query_new(VENTURE_TYPE_OIDC_IDENTITY); venture_query_add_filter_int(query, "user-id", VENTURE_FILTER_OP_EQ, principal->user_id, NULL);
	rows = venture_database_find(db, query, NULL);
	for (i = 0; rows && i < rows->len; i++) {
		VentureEntity *row = g_ptr_array_index(rows, i);
		g_autofree gchar *issuer = NULL;
		gboolean active = FALSE;
		g_object_get(row, "active", &active, "issuer", &issuer, NULL); if (!active) continue;
		g_string_append(body, "<h2>"); venture_html_escape_append(body, issuer); g_string_append(body, "</h2><form method=\"post\">");
		g_string_append_printf(body, "<input type=\"hidden\" name=\"identity_id\" value=\"%" G_GINT64_FORMAT "\">", venture_entity_get_id(row));
		g_string_append(body, "<label>Current local password<input name=\"password\" type=\"password\" autocomplete=\"current-password\" required></label><button>Unlink and end existing sessions</button></form>");
	}
	g_clear_pointer(&rows, g_ptr_array_unref); g_clear_object(&query);
	query = venture_query_new(VENTURE_TYPE_INTEGRATION_CONNECTION); venture_query_add_filter_string(query, "provider", VENTURE_FILTER_OP_EQ, "oidc", NULL);
	rows = venture_database_find(db, query, NULL);
	for (i = 0; rows && i < rows->len; i++) {
		VentureEntity *row = g_ptr_array_index(rows, i);
		g_autoptr(VentureEntity) org = NULL;
		g_autofree gchar *uuid = NULL, *name = NULL;
		gboolean enabled = FALSE;
		gint64 org_id = venture_entity_get_organization_id(row);
		g_object_get(row, "enabled", &enabled, NULL);
		if (!enabled || !venture_access_policy_has_organization_role(venture_database_get_access_policy(db), principal, org_id, roles, G_N_ELEMENTS(roles))) continue;
		org = venture_database_get(db, VENTURE_TYPE_ORGANIZATION, org_id, NULL); if (!org) continue;
		g_object_get(org, "name", &name, NULL); g_object_get(row, "uuid", &uuid, NULL);
		g_string_append(body, "<h2>Link "); venture_html_escape_append(body, name); g_string_append(body, "</h2><form method=\"post\" action=\"/account/oidc/link\"><input type=\"hidden\" name=\"provider\" value=\"");
		venture_html_escape_append(body, uuid); g_string_append(body, "\"><label>Current local password<input name=\"password\" type=\"password\" autocomplete=\"current-password\" required></label><button>Verify and link this provider</button></form>");
	}
	return venture_web_html_response(venture_web_page(self, request, "/account", "Organization sign-in", body->str), error ? 400 : 200);
}
