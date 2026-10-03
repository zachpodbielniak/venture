/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Authentication and validation failures share one boundary diagnostic;
 * token names, credentials and hosted capability URLs never enter it. */
static HtmxResponse *
account_identity_finish(HtmxResponse *response)
{
	const GError *error = g_object_get_data(G_OBJECT(response), "venture-error");
	if (error != NULL)
		g_message("Account identity request failed: cause=%s domain=%s code=%d",
			error->domain == VENTURE_ERROR ? venture_error_get_slug(error->code) : "internal_failure",
			g_quark_to_string(error->domain), error->code);
	htmx_response_add_header(response, "Cache-Control", "no-store");
	return response;
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
