/* SPDX-License-Identifier: AGPL-3.0-or-later */
static void ai_web_fields(VentureWebServer *self, GString *html)
{
	g_auto(GStrv) providers = venture_ai_provider_service_list_providers(venture_ai_provider_service_get(venture_context_get_database(self->context)));
	guint i;
	g_string_append(html, "<label class=\"field\">Provider<select name=\"provider\">");
	for (i = 0; providers && providers[i]; i++) {
		g_string_append(html, "<option>"); venture_html_escape_append(html, providers[i]); g_string_append(html, "</option>");
	}
	g_string_append(html, "</select></label>"
		"<label class=\"field\">Model<input name=\"model\" autocomplete=\"off\"></label>"
		"<label class=\"field\">Exact API base URL<input name=\"base_url\" type=\"url\" autocomplete=\"off\"></label>"
		"<label class=\"field\">API key<input name=\"api_key\" type=\"password\" autocomplete=\"new-password\"></label>");
}
static JsonNode *ai_web_settings(HtmxRequest *request)
{
	static const gchar *const names[] = { "provider", "model", "base_url", "api_key" };
	JsonNode *node = json_node_new(JSON_NODE_OBJECT);
	JsonObject *object = json_object_new();
	guint i;
	json_node_take_object(node, object);
	for (i = 0; i < G_N_ELEMENTS(names); i++) {
		const gchar *value = htmx_request_get_form_value(request, names[i]);
		json_object_set_string_member(object, names[i], value ? value : "");
	}
	return node;
}
static gint64 ai_web_integer(HtmxRequest *request, const gchar *name)
{
	const gchar *value = htmx_request_get_form_value(request, name);
	gint64 integer = -1;
	if (value) g_ascii_string_to_signed(value, 10, 0, G_MAXINT64, &integer, NULL);
	return integer;
}
static HtmxResponse *venture_web_ai_settings(HtmxRequest *request, GHashTable *params, gpointer data)
{
	static const gchar *const purposes[] = { "chat", "embedding", "coding" };
	static const gint roles[] = { VENTURE_ORGANIZATION_ROLE_OWNER, VENTURE_ORGANIZATION_ROLE_ADMIN };
	VentureWebServer *self = data;
	VentureDatabase *database = venture_context_get_database(self->context);
	VentureAiProviderService *service = venture_ai_provider_service_get(database);
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureAccessScope) scope = NULL;
	g_autoptr(VentureEntity) organization = NULL;
	g_autoptr(GString) html = g_string_new("<h1>Organization AI settings</h1><p>Each purpose has independent credentials and models. No ambient credentials or fallback are used. Secrets are write-only; supply all fields when connecting or rotating.</p>");
	g_autoptr(GError) error = NULL;
	gint64 org = g_ascii_strtoll(g_hash_table_lookup(params, "id"), NULL, 10);
	VentureActor actor;
	guint i;
	HtmxResponse *gate = venture_web_ui_require_session(self, request);
	if (gate) return gate;
	gate = venture_web_require_module_ui(self, request, "ai_providers"); if (gate) return gate;
	principal = venture_auth_authenticate(self->auth, request);
	if (!venture_access_policy_has_organization_role(venture_database_get_access_policy(database), principal, org, roles, G_N_ELEMENTS(roles))) {
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED, "Organization integration administration is required"); return venture_web_error_response(error);
	}
	organization = venture_database_get(database, VENTURE_TYPE_ORGANIZATION, org, &error);
	if (!organization) return venture_web_error_response(error);
	scope = venture_access_policy_enter(venture_database_get_access_policy(database), principal);
	venture_auth_to_actor(principal, &actor);
	if (venture_access_policy_is_administrator(principal)) g_string_append(html, "<p><a href=\"/settings/ai/platform\">Manage platform AI offers and grants</a></p>");
	if (htmx_request_get_method(request) == HTMX_METHOD_POST) {
		const gchar *operation = htmx_request_get_form_value(request, "operation");
		const gchar *purpose = htmx_request_get_form_value(request, "purpose");
		g_autoptr(VentureAiConfiguration) result = NULL;
		if (!g_strcmp0(operation, "configure")) {
			g_autoptr(JsonNode) settings = ai_web_settings(request);
			result = venture_ai_provider_service_configure(service, org, purpose, settings, ai_web_integer(request, "monthly_requests"),
				ai_web_integer(request, "concurrency_limit"), ai_web_integer(request, "version"), &actor, &error);
		} else if (!g_strcmp0(operation, "select") || !g_strcmp0(operation, "disable")) {
			result = venture_ai_provider_service_select(service, org, purpose, !g_strcmp0(operation, "disable") ? 0 : ai_web_integer(request, "grant_id"),
				ai_web_integer(request, "version"), &actor, &error);
		} else if (!g_strcmp0(operation, "test")) {
			if (venture_ai_provider_service_test(service, org, purpose, &error)) g_string_append(html, "<p>Selected provider answered the synthetic connection test.</p>");
		} else g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Choose an AI settings operation");
		if (error) { g_string_append(html, "<p role=\"alert\">"); venture_html_escape_append(html, error->message); g_string_append(html, "</p>"); }
		else {
			g_autofree gchar *location = !g_strcmp0(operation, "test") ? g_strdup_printf("/organizations/%" G_GINT64_FORMAT "/settings/ai?tested=%s", org, purpose) :
				g_strdup_printf("/organizations/%" G_GINT64_FORMAT "/settings/ai", org);
			return venture_web_redirect_to(location);
		}
	}
	for (i = 0; i < G_N_ELEMENTS(purposes); i++) {
		g_autoptr(JsonNode) effective = venture_ai_provider_service_effective(service, org, purposes[i], NULL);
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_AI_GRANT);
		g_autoptr(GPtrArray) grants = NULL;
		JsonObject *state = effective ? json_node_get_object(effective) : NULL;
		guint j;
		if (htmx_request_get_method(request) == HTMX_METHOD_GET && state &&
			json_object_get_boolean_member(state, "available") &&
			!g_strcmp0(htmx_request_get_query_param(request, "tested"), purposes[i]) &&
			!g_strcmp0(json_object_get_string_member(state, "last_request_state"), "completed"))
			g_string_append(html, "<p>Selected provider answered the synthetic connection test.</p>");
		g_string_append_printf(html, "<h2>%s</h2><p>Effective source: ", purposes[i]);
		venture_html_escape_append(html, state ? json_object_get_string_member(state, "mode") : "unavailable");
		g_string_append(html, state && json_object_get_boolean_member(state, "available") ? " · configured" : " · unavailable");
		g_string_append(html, " · last request "); venture_html_escape_append(html, state ? json_object_get_string_member(state, "last_request_state") : "unavailable");
		if (state && json_object_get_boolean_member(state, "available")) {
			g_string_append(html, " · model "); venture_html_escape_append(html, json_object_get_string_member(state, "model"));
			g_string_append_printf(html, " · payer organization %" G_GINT64_FORMAT, json_object_get_int_member(state, "payer_organization_id"));
		}
		g_string_append_printf(html, "</p><form class=\"card card-body\" method=\"post\"><input type=\"hidden\" name=\"purpose\" value=\"%s\"><input type=\"hidden\" name=\"version\" value=\"%" G_GINT64_FORMAT "\">", purposes[i], state ? json_object_get_int_member(state, "configuration_version") : 0);
		ai_web_fields(self, html);
		g_string_append(html, "<label class=\"field\">Monthly provider requests<input type=\"number\" name=\"monthly_requests\" min=\"1\" max=\"10000000\" value=\"1000\"></label><label class=\"field\">Concurrent provider requests<input type=\"number\" name=\"concurrency_limit\" min=\"1\" max=\"32\" value=\"2\"></label><button class=\"btn\" name=\"operation\" value=\"configure\">Connect or rotate own provider</button>");
		venture_query_set_organization(query, org); venture_query_set_limit(query, 0);
		grants = venture_database_find(database, query, NULL);
		g_string_append(html, "<label class=\"field\">Explicit platform eligibility<select name=\"grant_id\"><option value=\"0\">Disabled</option>");
		for (j = 0; grants && j < grants->len; j++) {
			VentureEntity *grant = g_ptr_array_index(grants, j);
			g_autofree gchar *title = venture_entity_get_display_name(grant);
			g_string_append_printf(html, "<option value=\"%" G_GINT64_FORMAT "\">", venture_entity_get_id(grant));
			venture_html_escape_append(html, title); g_string_append(html, "</option>");
		}
		g_string_append(html, "</select></label><button class=\"btn\" name=\"operation\" value=\"select\">Select platform grant</button><button class=\"btn\" name=\"operation\" value=\"test\">Test selected provider</button><button class=\"btn\" name=\"operation\" value=\"disable\">Disable and disconnect</button></form>");
	}
	g_string_append(html, "<p>Tests send only synthetic input and consume request quota. Every provider round counts, including failures. UTC monthly quotas survive rotation and restart. Embedding model changes require explicit corpus reindexing.</p>");
	return venture_web_html_response(venture_web_page(self, request, "/organizations", "AI settings", html->str), error ? 400 : 200);
}
static HtmxResponse *venture_web_ai_platform_settings(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	VentureDatabase *database = venture_context_get_database(self->context);
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureAccessScope) scope = NULL;
	g_autoptr(GString) html = g_string_new("<h1>Platform AI offers</h1><p>Only the platform operator controls these credentials, including when a business organization pays the bill. Creating an offer grants nobody access. Use the offer's Grant action, then the recipient's AI settings to select it.</p>");
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_AI_PLATFORM_OFFER);
	g_autoptr(GPtrArray) offers = NULL, organizations = NULL;
	VentureActor actor;
	guint i;
	HtmxResponse *gate = venture_web_ui_require_session(self, request);
	(void)params;
	if (gate) return gate;
	gate = venture_web_require_module_ui(self, request, "ai_providers"); if (gate) return gate;
	principal = venture_auth_authenticate(self->auth, request);
	if (!venture_access_policy_is_administrator(principal)) {
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED, "Platform administration is required"); return venture_web_error_response(error);
	}
	scope = venture_access_policy_enter(venture_database_get_access_policy(database), principal);
	venture_auth_to_actor(principal, &actor);
	if (htmx_request_get_method(request) == HTMX_METHOD_POST) {
		g_autoptr(JsonNode) settings = ai_web_settings(request);
		g_autoptr(VentureAiPlatformOffer) offer = venture_ai_provider_service_offer(venture_ai_provider_service_get(database),
			ai_web_integer(request, "billing_organization_id"), htmx_request_get_form_value(request, "purpose"),
			htmx_request_get_form_value(request, "title"), settings, ai_web_integer(request, "offer_id"), ai_web_integer(request, "version"), &actor, &error);
		if (offer) return venture_web_redirect_to("/settings/ai/platform");
		else { g_string_append(html, "<p role=\"alert\">"); venture_html_escape_append(html, error->message); g_string_append(html, "</p>"); }
	}
	venture_query_set_limit(query, 0); offers = venture_database_find(database, query, NULL);
	organizations = venture_web_ai_organizations(self);
	for (i = 0; i <= (offers ? offers->len : 0); i++) {
		VentureEntity *offer = offers && i < offers->len ? g_ptr_array_index(offers, i) : NULL;
		g_autofree gchar *title = NULL, *purpose = NULL;
		gint64 payer = 0;
		guint j;
		if (offer) g_object_get(offer, "title", &title, "purpose", &purpose, "billing-organization-id", &payer, NULL);
		g_string_append(html, "<form class=\"card card-body\" method=\"post\"><h2>"); venture_html_escape_append(html, title ? title : "Create an offer"); g_string_append(html, "</h2>");
		if (offer) g_string_append_printf(html, "<p><a href=\"/e/ai_platform_offer/%" G_GINT64_FORMAT "\">Eligibility and disable actions</a></p>", venture_entity_get_id(offer));
		g_string_append_printf(html, "<input type=\"hidden\" name=\"offer_id\" value=\"%" G_GINT64_FORMAT "\"><input type=\"hidden\" name=\"version\" value=\"%" G_GINT64_FORMAT "\">", offer ? venture_entity_get_id(offer) : 0, offer ? venture_entity_get_version(offer) : 0);
		g_string_append(html, "<label class=\"field\">Safe offer label<input name=\"title\" value=\""); venture_html_escape_append(html, title ? title : ""); g_string_append(html, "\"></label><label class=\"field\">Purpose<select name=\"purpose\">");
		{ static const gchar *const purposes[] = { "chat", "embedding", "coding" }; for (j = 0; j < G_N_ELEMENTS(purposes); j++) g_string_append_printf(html, "<option%s>%s</option>", !g_strcmp0(purpose, purposes[j]) ? " selected" : "", purposes[j]); }
		g_string_append(html, "</select></label><label class=\"field\">Platform billing organization<select name=\"billing_organization_id\"><option value=\"0\">Select a payer</option>");
		for (j = 0; organizations && j < organizations->len; j++) {
			VentureEntity *org = g_ptr_array_index(organizations, j); g_autofree gchar *name = venture_entity_get_display_name(org);
			g_string_append_printf(html, "<option value=\"%" G_GINT64_FORMAT "\"%s>", venture_entity_get_id(org), payer == venture_entity_get_id(org) ? " selected" : "");
			venture_html_escape_append(html, name); g_string_append(html, "</option>");
		}
		g_string_append(html, "</select></label>"); ai_web_fields(self, html); g_string_append(html, "<button class=\"btn\">Create or rotate platform credentials</button></form>");
	}
	return venture_web_html_response(venture_web_page(self, request, "/settings", "Platform AI", html->str), error ? 400 : 200);
}
