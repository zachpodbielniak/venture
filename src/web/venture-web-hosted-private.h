/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Included after VentureWebServer is defined. Declaration matching uses the
 * same router and ordering as execution; plugins cannot acquire a declaration
 * merely by registering a callback directly with the underlying router. */
static HtmxResponse *
venture_web_classification_response(HtmxRequest *request, GHashTable *params, gpointer data)
{
	HtmxResponse *response = htmx_response_new();
	guint declaration = GPOINTER_TO_UINT(data);
	(void)request;
	(void)params;
	venture_data_class_declare_resource(G_OBJECT(response), declaration & 0xff);
	g_object_set_data(G_OBJECT(response), "venture-hosted-flags", GUINT_TO_POINTER(declaration >> 8));
	return response;
}

void
venture_web_server_add_classified_route(VentureWebServer *self, HtmxMethod method,
	const gchar *pattern, VentureDataClass classification, VentureHostedRouteFlags flags,
	HtmxRouteCallback callback, gpointer user_data)
{
	g_return_if_fail(VENTURE_IS_WEB_SERVER(self));
	g_return_if_fail(classification > VENTURE_DATA_CLASS_UNKNOWN && classification <= VENTURE_DATA_CLASS_TENANT_ADMIN);
	htmx_router_add_route(htmx_server_get_router(self->server), method, pattern, callback, user_data);
	htmx_router_add_route(self->classified_routes, method, pattern, venture_web_classification_response,
		GUINT_TO_POINTER((guint)classification | ((guint)flags << 8)));
}

static gboolean
venture_web_host_matches(VentureTenantService *service, HtmxRequest *request)
{
	SoupServerMessage *message = htmx_request_get_message(request);
	SoupMessageHeaders *headers;
	g_autoptr(GUri) expected = NULL, presented = NULL;
	g_autofree gchar *url = NULL;
	const gchar *host, *origin;
	gint expected_port, presented_port;
	if (!message) return FALSE;
	headers = soup_server_message_get_request_headers(message);
	host = soup_message_headers_get_one(headers, "Host");
	if (!host || strchr(host, '/') || strchr(host, '@') || strchr(host, '#') || strchr(host, '?')) return FALSE;
	expected = g_uri_parse(venture_tenant_service_get_origin(service), G_URI_FLAGS_NONE, NULL);
	url = g_strdup_printf("%s://%s", g_uri_get_scheme(expected), host);
	presented = g_uri_parse(url, G_URI_FLAGS_NONE, NULL);
	if (!presented || !g_uri_get_host(presented) || g_uri_get_userinfo(presented)) return FALSE;
	expected_port = g_uri_get_port(expected);
	presented_port = g_uri_get_port(presented);
	if (expected_port < 0) expected_port = g_str_equal(g_uri_get_scheme(expected), "https") ? 443 : 80;
	if (presented_port < 0) presented_port = g_str_equal(g_uri_get_scheme(expected), "https") ? 443 : 80;
	if (g_ascii_strcasecmp(g_uri_get_host(expected), g_uri_get_host(presented)) != 0 || expected_port != presented_port) return FALSE;
	origin = soup_message_headers_get_one(headers, "Origin");
	return !origin || g_strcmp0(origin, venture_tenant_service_get_origin(service)) == 0;
}

static HtmxResponse *
venture_web_hosted_refusal(HtmxRequest *request)
{
	HtmxResponse *response;
	if (g_str_has_prefix(htmx_request_get_path(request), "/api/")) {
		g_autoptr(GError) error = NULL;
		g_autofree gchar *body = NULL;
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED, "Workspace access is unavailable");
		body = venture_json_error_to_string(error, FALSE);
		response = htmx_response_new_with_content(body);
		htmx_response_set_content_type(response, "application/json; charset=utf-8");
	} else {
		response = htmx_response_new_with_content("<!doctype html><html lang=\"en\"><meta charset=\"utf-8\"><title>Workspace access</title><h1>Workspace access is unavailable</h1><p>Sign in with an authorized account or contact your workspace administrator.</p><p><a href=\"/login\">Sign in</a> · <a href=\"/e/tenant_workspace\">Workspace administration</a></p></html>");
		htmx_response_set_content_type(response, "text/html; charset=utf-8");
	}
	htmx_response_set_status(response, 403);
	htmx_response_add_header(response, "Cache-Control", "no-store");
	return response;
}

static gboolean
venture_web_hosted_preflight(VentureWebServer *self, HtmxContext *http, VentureTenantSupportScope **support_scope)
{
	VentureTenantService *service = venture_tenant_service_get(venture_context_get_database(self->context));
	HtmxRequest *request = htmx_context_get_request(http);
	g_autoptr(HtmxResponse) declaration = NULL;
	g_autoptr(GHashTable) params = NULL, cookies = NULL;
	g_autoptr(VentureAuthPrincipal) actor = NULL;
	g_autoptr(VentureTenantSupportScope) verified = NULL;
	VentureDataClass classification;
	guint flags;
	gboolean control, write;
	HtmxMethod method;
	HtmxResponse *refused;
	const gchar *capability = NULL;
	SoupServerMessage *message;
	if (!venture_tenant_service_is_enabled(service)) return TRUE;
	*support_scope = venture_tenant_service_enter_request(service, NULL, NULL, htmx_request_get_path(request), NULL);
	declaration = htmx_router_match(self->classified_routes, request, &params);
	classification = declaration ? venture_data_class_for_resource(G_OBJECT(declaration)) : VENTURE_DATA_CLASS_UNKNOWN;
	flags = declaration ? GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(declaration), "venture-hosted-flags")) : 0;
	control = (flags & VENTURE_HOSTED_ROUTE_CONTROL) != 0;
	method = htmx_request_get_method(request);
	write = method != HTMX_METHOD_GET && method != HTMX_METHOD_HEAD;
	if (params && g_hash_table_lookup(params, "type")) {
		GType type = venture_entity_registry_lookup(venture_context_get_entity_registry(self->context), g_hash_table_lookup(params, "type"));
		if (type != G_TYPE_INVALID && venture_data_class_for_type(type) == VENTURE_DATA_CLASS_TENANT_ADMIN) {
			control = TRUE;
			classification = VENTURE_DATA_CLASS_TENANT_ADMIN;
		}
	}
	venture_tenant_support_scope_set_control(*support_scope, control && classification == VENTURE_DATA_CLASS_PERSONAL);
	if (classification == VENTURE_DATA_CLASS_REFERENCE) {
		venture_data_class_declare_resource(G_OBJECT(request), classification);
		return TRUE;
	}
	if (classification == VENTURE_DATA_CLASS_UNKNOWN || classification == VENTURE_DATA_CLASS_PLATFORM ||
	    !venture_web_host_matches(service, request)) goto deny;
	actor = venture_auth_authenticate(self->auth, request);
	g_object_set_data(G_OBJECT(request), "venture-hosted-authenticated", actor->authenticated ? GINT_TO_POINTER(1) : NULL);
	if (classification == VENTURE_DATA_CLASS_TENANT_ADMIN && actor->authenticated &&
	    (actor->token_id != 0 || !venture_tenant_service_check_principal(service, actor, NULL) ||
	     !venture_tenant_service_is_member(service, actor->user_id, TRUE))) goto deny;
	message = htmx_request_get_message(request);
	if (!control && message) {
		SoupMessageHeaders *headers = soup_server_message_get_request_headers(message);
		capability = soup_message_headers_get_one(headers, "X-Venture-Support");
		if (!capability) {
			const gchar *header = soup_message_headers_get_one(headers, "Cookie");
			if (header) {
				cookies = htmx_cookie_parse_request(header);
				capability = g_hash_table_lookup(cookies, "venture_support");
			}
		}
	}
	if (capability && *capability) {
		if (!(flags & VENTURE_HOSTED_ROUTE_SUPPORT)) goto deny;
		verified = venture_tenant_service_enter_request(service, actor, capability, htmx_request_get_path(request), NULL);
		if (!verified || !venture_tenant_service_check_support_request(service, actor, write, NULL)) goto deny;
		g_clear_object(support_scope);
		*support_scope = g_steal_pointer(&verified);
	} else if (!control && !venture_tenant_service_check_operation(service, write, NULL)) goto deny;
	if (!control && actor->authenticated && !venture_tenant_service_check_principal(service, actor, NULL)) goto deny;
	venture_data_class_declare_resource(G_OBJECT(request), classification);
	g_object_set_data(G_OBJECT(request), "venture-hosted-control", control ? GINT_TO_POINTER(1) : NULL);
	return TRUE;
deny:
	refused = venture_web_hosted_refusal(request);
	htmx_context_set_response(http, refused);
	return FALSE;
}

/* A tenant administrator can satisfy a business-setting role gate only after
 * the matched route's explicit authority declaration excludes platform work.
 * The principal keeps its real role for every downstream resource check. */
static gboolean
venture_web_hosted_auth_require(VentureWebServer *self, HtmxRequest *request,
	VentureAuthPrincipal *principal, VentureUserRole minimum, GError **error)
{
	VentureTenantService *service = venture_tenant_service_get(venture_context_get_database(self->context));
	VentureDataClass classification = venture_data_class_for_resource(G_OBJECT(request));
	if (venture_tenant_service_get_support_organization(service) > 0) {
		if (!venture_tenant_service_check_support_request(service, principal,
		        htmx_request_get_method(request) != HTMX_METHOD_GET && htmx_request_get_method(request) != HTMX_METHOD_HEAD, error)) return FALSE;
		return venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_VIEWER, error);
	}
	if (venture_tenant_service_is_enabled(service) && principal->authenticated && principal->token_id == 0 &&
	    principal->role == VENTURE_USER_ROLE_EDITOR &&
	    (classification == VENTURE_DATA_CLASS_TENANT || classification == VENTURE_DATA_CLASS_TENANT_ADMIN) &&
	    venture_tenant_service_is_member(service, principal->user_id, TRUE))
		return venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_EDITOR, error);
	return venture_auth_require(self->auth, principal, minimum, error);
}

static gboolean
venture_web_hosted_finish(VentureWebServer *self, HtmxContext *http)
{
	VentureTenantService *service = venture_tenant_service_get(venture_context_get_database(self->context));
	HtmxRequest *request = htmx_context_get_request(http);
	g_autoptr(VentureAuthPrincipal) actor = NULL;
	HtmxResponse *refused;
	if (!venture_tenant_service_is_enabled(service) ||
	    venture_data_class_for_resource(G_OBJECT(request)) == VENTURE_DATA_CLASS_REFERENCE ||
	    g_object_get_data(G_OBJECT(request), "venture-hosted-control")) return TRUE;
	actor = venture_auth_authenticate(self->auth, request);
	/* Provider completion may drive nested requests. Recheck the durable
	 * lifecycle and membership before a previously prepared response escapes. */
	if (venture_tenant_service_get_support_organization(service) > 0) {
		if (venture_tenant_service_check_support_request(service, actor, FALSE, NULL)) return TRUE;
	} else if (venture_tenant_service_check_operation(service, FALSE, NULL) &&
	           ((!g_object_get_data(G_OBJECT(request), "venture-hosted-authenticated") && !actor->authenticated) ||
	            (actor->authenticated && venture_tenant_service_check_principal(service, actor, NULL)))) return TRUE;
	refused = venture_web_hosted_refusal(request);
	htmx_context_set_response(http, refused);
	return FALSE;
}
