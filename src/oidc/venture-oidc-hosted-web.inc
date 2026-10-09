/* SPDX-License-Identifier: AGPL-3.0-or-later */
static gchar *venture_hosted_browser_cookie(const gchar *value)
{
	g_autoptr(HtmxCookie) cookie = htmx_cookie_new("venture_hosted", value ? value : "");
	htmx_cookie_set_path(cookie, "/auth/hosted");
	htmx_cookie_set_http_only(cookie, TRUE);
	htmx_cookie_set_same_site(cookie, HTMX_COOKIE_SAME_SITE_LAX);
	htmx_cookie_set_secure(cookie, TRUE);
	htmx_cookie_set_max_age(cookie, value ? 300 : 0);
	return htmx_cookie_to_set_cookie(cookie);
}
static void venture_hosted_browser_headers(HtmxResponse *response, const gchar *value)
{
	g_autofree gchar *cookie = venture_hosted_browser_cookie(value);
	htmx_response_append_header(response, "Set-Cookie", cookie);
	htmx_response_add_header(response, "Cache-Control", "no-store");
	htmx_response_add_header(response, "Referrer-Policy", "no-referrer");
}
static HtmxResponse *venture_hosted_browser_start(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	g_autofree gchar *browser = NULL, *url = NULL, *remote = venture_auth_remote_address(request);
	g_autoptr(GError) error = NULL;
	HtmxResponse *response;
	(void)params;
	url = venture_oidc_service_begin_hosted(venture_oidc_service_get(venture_context_get_database(self->context)),
		htmx_request_get_query_param(request, "return_to"), remote, &browser, &error);
	if (!url) return venture_web_error_response(error);
	response = venture_web_redirect_to(url);
	venture_hosted_browser_headers(response, browser);
	return response;
}
/* The installation's default business is not necessarily this person's.
 * This unsigned preference grants no access: resolve the target under the
 * verified user's policy, and select only one of their explicit memberships.
 * A portfolio without a specific record stays on the existing "all" view. */
static gchar *venture_hosted_browser_business(VentureWebServer *self, gint64 identity, const gchar *target)
{
	VentureDatabase *db = venture_context_get_database(self->context);
	g_autoptr(VentureUser) user = venture_oidc_service_identity_user(venture_oidc_service_get(db), -identity, NULL);
	g_autoptr(VentureAccessScope) scope = NULL;
	g_autoptr(JsonNode) authority = NULL;
	g_autoptr(VentureEntity) record = NULL;
	g_autoptr(HtmxCookie) cookie = NULL;
	g_autofree gchar *value = NULL, *path = NULL;
	g_auto(GStrv) parts = NULL;
	VentureAuthPrincipal principal = { 0 };
	JsonArray *organizations;
	gint64 selected = 0, requested = 0, id;
	guint i;
	if (!user) return NULL;
	principal.authenticated = TRUE;
	principal.user_id = venture_entity_get_id(VENTURE_ENTITY(user));
	g_object_get(user, "role", &principal.role, NULL);
	scope = venture_access_policy_enter(venture_database_get_access_policy(db), &principal);
	authority = venture_tenant_service_account_authority(venture_tenant_service_get(db), NULL);
	if (authority) {
		organizations = json_object_get_array_member(json_node_get_object(authority), "organizations");
		path = g_strndup(target, strcspn(target, "?#")); parts = g_strsplit(path, "/", 0);
		if (g_strv_length(parts) >= 4 && !strcmp(parts[1], "e") &&
			g_ascii_string_to_signed(parts[3], 10, 1, G_MAXINT64, &id, NULL)) {
			GType type = venture_entity_registry_lookup(venture_entity_registry_get_default(), parts[2]);
			if (type != G_TYPE_INVALID) record = venture_database_get(db, type, id, NULL);
			if (record) requested = VENTURE_IS_ORGANIZATION(record) ? venture_entity_get_id(record) : venture_entity_get_organization_id(record);
		}
		for (i = 0; i < json_array_get_length(organizations); i++) {
			id = json_object_get_int_member(json_array_get_object_element(organizations, i), "organization_id");
			if (id == requested || json_array_get_length(organizations) == 1) selected = id;
		}
	}
	value = selected > 0 ? g_strdup_printf("%" G_GINT64_FORMAT, selected) : g_strdup("all");
	cookie = htmx_cookie_new(VENTURE_WEB_ENTITY_COOKIE, value);
	htmx_cookie_set_path(cookie, "/"); htmx_cookie_set_http_only(cookie, TRUE);
	htmx_cookie_set_secure(cookie, TRUE); htmx_cookie_set_same_site(cookie, HTMX_COOKIE_SAME_SITE_LAX);
	htmx_cookie_set_max_age(cookie, 60 * 60 * 24 * 365);
	return htmx_cookie_to_set_cookie(cookie);
}
static HtmxResponse *venture_hosted_browser_callback(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	SoupServerMessage *message = htmx_request_get_message(request);
	const gchar *header = soup_message_headers_get_list(soup_server_message_get_request_headers(message), "Cookie");
	g_autoptr(GHashTable) cookies = header ? htmx_cookie_parse_request(header) : NULL;
	g_autoptr(VentureTenantIdentity) identity = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *target = NULL, *session = NULL;
	gboolean mfa = FALSE;
	HtmxResponse *response;
	(void)params;
	identity = venture_oidc_service_finish_hosted(venture_oidc_service_get(venture_context_get_database(self->context)),
		htmx_request_get_query_param(request, "state"), htmx_request_get_query_param(request, "code"),
		cookies ? g_hash_table_lookup(cookies, "venture_hosted") : NULL, &target, &error);
	if (!identity || !venture_auth_login_identity(self->auth, -venture_entity_get_id(VENTURE_ENTITY(identity)), &session, &mfa, &error)) {
		response = venture_web_html_response(venture_web_auth_page(self, request,
			"<h2>We couldn't sign you in</h2><p>Please start again from Lightsite.</p><a href=\"/auth/hosted/start\">Try again</a>"), 401);
	} else {
		g_autofree gchar *business = venture_hosted_browser_business(self, venture_entity_get_id(VENTURE_ENTITY(identity)), target);
		response = venture_web_redirect_to(mfa ? "/login/mfa" : target);
		htmx_response_append_header(response, "Set-Cookie", session);
		if (business) htmx_response_append_header(response, "Set-Cookie", business);
	}
	venture_hosted_browser_headers(response, NULL);
	return response;
}
