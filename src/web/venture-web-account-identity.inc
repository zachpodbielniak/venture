/* SPDX-License-Identifier: AGPL-3.0-or-later */
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
	if (response) { htmx_response_add_header(response, "Cache-Control", "no-store"); return response; }
	if (!g_ascii_string_to_signed(g_hash_table_lookup(params, "id"), 10, 1, G_MAXINT64, &id, NULL)) {
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "A positive organization ID is required");
		response = venture_web_error_response(error);
	} else {
		principal = venture_auth_authenticate(self->auth, request);
		scope = venture_access_policy_enter(venture_database_get_access_policy(database), principal);
		identity = venture_tenant_service_account_identity(venture_tenant_service_get(database), id, &error);
		response = identity ? venture_web_json_response(identity, 200) : venture_web_error_response(error);
	}
	htmx_response_add_header(response, "Cache-Control", "no-store");
	return response;
}
