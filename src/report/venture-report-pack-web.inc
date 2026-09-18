/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Included by the web server so this thin action adapter shares its guards.
 * POST /api/v1/report_pack/:id/deliver re-sends a pack's last retained
 * output through the mail outbox; the same editor gate as running a pack. */
static HtmxResponse *venture_web_report_pack_deliver(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) pack = NULL;
	g_autoptr(VentureMailMessage) queued = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GError) error = NULL;
	VentureDatabase *database = venture_context_get_database(self->context);
	VentureActor actor;
	HtmxResponse *gate;
	const gchar *id_text = g_hash_table_lookup(params, "id");
	gchar *end = NULL;
	gint64 id = id_text ? g_ascii_strtoll(id_text, &end, 10) : 0;
	gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_EDITOR);
	if (gate) return gate;
	gate = venture_web_require_module_api(self, "statements");
	if (gate) return gate;
	if (htmx_request_get_query_param(request, "stage")) {
		venture_set_error_validation(&error, "stage", "Report pack delivery cannot be staged");
		return venture_web_error_response(error);
	}
	principal = venture_auth_authenticate(self->auth, request);
	venture_auth_to_actor(principal, &actor);
	if (id > 0 && end && !*end) pack = venture_database_get(database, VENTURE_TYPE_REPORT_PACK, id, &error);
	if (!pack || venture_entity_is_deleted(pack)) {
		if (!error) g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Report pack not found");
		return venture_web_error_response(error);
	}
	queued = venture_report_pack_service_deliver(venture_report_pack_service_get(database), self->context,
		VENTURE_REPORT_PACK(pack), NULL, &actor, NULL, &error);
	if (!queued) return venture_web_error_response(error);
	node = venture_serializable_to_json(VENTURE_SERIALIZABLE(queued), FALSE);
	return venture_web_json_response(node, 202);
}
