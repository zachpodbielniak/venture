/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* GET /api/v1/lightsite/departure/:org and POST
 * /api/v1/lightsite/departure/:org/:action (leave, close, return, purge;
 * no body): a business's leaving state, read and moved by Lightsite's
 * daemon. Only the trusted service token reaches the departure service,
 * which judges it and holds the rule; a browser session is refused as no
 * service credential. 410 is the one answer the error mapping does not
 * give: a return that can no longer happen. */
static HtmxResponse *
lightsite_departure_finish(HtmxResponse *response)
{
	return account_request_finish(response, "Lightsite departure");
}

static HtmxResponse *
venture_web_lightsite_departure(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	VentureDatabase *database = venture_context_get_database(self->context);
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureAccessScope) scope = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) result = NULL;
	const gchar *name = g_hash_table_lookup(params, "action");
	VentureDepartureAction action = VENTURE_DEPARTURE_ACTION_LEAVE;
	gboolean gone = FALSE;
	gint64 organization_id = 0;
	VentureActor actor;
	HtmxResponse *response = venture_web_api_require(self, request, VENTURE_USER_ROLE_VIEWER);
	if (response) return lightsite_departure_finish(response);
	principal = venture_auth_authenticate(self->auth, request);
	if (principal->token_id <= 0) {
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_UNAUTHENTICATED, "A service bearer token is required");
		return lightsite_departure_finish(venture_web_error_response(error));
	}
	if (!g_ascii_string_to_signed(g_hash_table_lookup(params, "org"), 10, 1, G_MAXINT64, &organization_id, NULL)) {
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "A positive organization ID is required");
		return lightsite_departure_finish(venture_web_error_response(error));
	}
	if (name && !venture_departure_action_from_string(name, &action)) {
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "The action is leave, close, return or purge");
		return lightsite_departure_finish(venture_web_error_response(error));
	}
	scope = venture_access_policy_enter(venture_database_get_access_policy(database), principal);
	venture_auth_to_actor(principal, &actor);
	result = name ? venture_lightsite_departure_act(database, organization_id, action, &actor, &gone, &error)
		: venture_lightsite_departure_read(database, organization_id, &error);
	if (result) return lightsite_departure_finish(venture_web_json_response(result, 200));
	response = venture_web_error_response(error);
	if (gone) htmx_response_set_status(response, 410);
	return lightsite_departure_finish(response);
}
