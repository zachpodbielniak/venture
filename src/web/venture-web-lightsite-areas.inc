/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* GET /api/v1/organizations/:organization_id/lightsite-areas.
 * One answer naming the Lightsite area types this caller may open.
 * A type whose list would be 403 or 404 is left out. The route ships on
 * the staging fork with the billing-overview branch, not as its own PR. */
static gboolean
lightsite_area_open(VentureWebServer *self, VentureAuthPrincipal *principal, const gchar *type, GError **error)
{
	GType entity = venture_entity_registry_lookup(venture_context_get_entity_registry(self->context), type);
	g_autoptr(GError) local = NULL;
	if (entity == G_TYPE_INVALID) return FALSE;
	if (venture_web_require_for_type(self, principal, entity, VENTURE_USER_ROLE_VIEWER, &local)) return TRUE;
	if (local && (local->code == VENTURE_ERROR_PERMISSION_DENIED || local->code == VENTURE_ERROR_NOT_FOUND)) return FALSE;
	g_propagate_error(error, g_steal_pointer(&local));
	return FALSE;
}

static HtmxResponse *
venture_web_lightsite_areas(HtmxRequest *request, GHashTable *params, gpointer data)
{
	static const gchar *const types[] = {
		"company", "contact", "lead", "deal", "quote", "pipeline", "pipeline_stage",
		"job", "client_project", "activity", "booking_page", "ticket", "event",
		"invoice", "payment", "customer_credit", "dunning_event", "dunning_policy",
		"marketing_send", "marketing_list", "marketing_member", "marketing_consent",
		"marketing_recipient", "marketing_event", "sequence", "sequence_step",
		"sequence_enrollment", "sequence_delivery", "user"
	};
	VentureWebServer *self = data;
	VentureDatabase *database = venture_context_get_database(self->context);
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureAccessScope) scope = NULL;
	g_autoptr(VentureEntity) organization = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) node = NULL;
	HtmxResponse *response = venture_web_api_require(self, request, VENTURE_USER_ROLE_VIEWER);
	gboolean invoice = FALSE, event = FALSE;
	gint64 id;
	guint i;
	if (response) return account_request_finish(response, "Lightsite areas");
	if (!g_ascii_string_to_signed(g_hash_table_lookup(params, "organization_id"), 10, 1, G_MAXINT64, &id, NULL)) {
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "A positive organization ID is required");
		return account_request_finish(venture_web_error_response(error), "Lightsite areas");
	}
	principal = venture_auth_authenticate(self->auth, request);
	scope = venture_access_policy_enter(venture_database_get_access_policy(database), principal);
	organization = venture_database_get(database, VENTURE_TYPE_ORGANIZATION, id, &error);
	if (!organization) {
		if (!error) g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "That organization is not available.");
		return account_request_finish(venture_web_error_response(error), "Lightsite areas");
	}
	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "types");
	json_builder_begin_array(builder);
	for (i = 0; i < G_N_ELEMENTS(types); i++) {
		if (!lightsite_area_open(self, principal, types[i], &error)) {
			if (error) return account_request_finish(venture_web_error_response(error), "Lightsite areas");
			continue;
		}
		if (!strcmp(types[i], "invoice")) invoice = TRUE;
		if (!strcmp(types[i], "event")) event = TRUE;
		json_builder_add_string_value(builder, types[i]);
	}
	if (invoice && venture_report_registry_lookup(venture_context_get_report_registry(self->context), "receivables"))
		json_builder_add_string_value(builder, "receivables");
	if (invoice && venture_report_registry_lookup(venture_context_get_report_registry(self->context), "dunning_worklist"))
		json_builder_add_string_value(builder, "dunning_worklist");
	if (invoice) json_builder_add_string_value(builder, "unpaid");
	if (event) json_builder_add_string_value(builder, "calendar");
	json_builder_end_array(builder);
	json_builder_end_object(builder);
	node = json_builder_get_root(builder);
	return account_request_finish(venture_web_json_response(node, 200), "Lightsite areas");
}
