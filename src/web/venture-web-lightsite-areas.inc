/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* GET /api/v1/organizations/:organization_id/lightsite-areas?types=
 * One answer for the area types the caller named. A type whose list would
 * be 403 or 404 is left out. A type that was not asked is absent. There is
 * no list of Lightsite areas in this file: the caller sends the names. */
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

/* slot is -1 until the first check, then 1 or 0. A later ask reuses it. */
static gboolean
lightsite_slot_open(VentureWebServer *self, VentureAuthPrincipal *principal, const gchar *entity, gint *slot, GError **error)
{
	if (*slot >= 0) return TRUE;
	if (!lightsite_area_open(self, principal, entity, error)) {
		if (error && *error) return FALSE;
		*slot = 0;
		return TRUE;
	}
	*slot = 1;
	return TRUE;
}

static gboolean
lightsite_area_token(const gchar *type)
{
	gsize i, n;
	if (!type || !type[0]) return FALSE;
	n = strlen(type);
	if (n > 64 || !g_ascii_islower(type[0])) return FALSE;
	for (i = 1; i < n; i++)
		if (!g_ascii_islower(type[i]) && !g_ascii_isdigit(type[i]) && type[i] != '_') return FALSE;
	return TRUE;
}

static HtmxResponse *
venture_web_lightsite_areas(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	VentureDatabase *database = venture_context_get_database(self->context);
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureAccessScope) scope = NULL;
	g_autoptr(VentureEntity) organization = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GHashTable) seen = NULL;
	g_auto(GStrv) asked = NULL;
	HtmxResponse *response = venture_web_api_require(self, request, VENTURE_USER_ROLE_VIEWER);
	const gchar *raw;
	gint invoice = -1, event = -1;
	gint64 id;
	guint i, n;
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
	raw = htmx_request_get_query_param(request, "types");
	if (!raw || !raw[0]) {
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Name the area types in types.");
		return account_request_finish(venture_web_error_response(error), "Lightsite areas");
	}
	if (strlen(raw) > 4096) {
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "The area type list is too long.");
		return account_request_finish(venture_web_error_response(error), "Lightsite areas");
	}
	asked = g_strsplit(raw, ",", -1);
	n = g_strv_length(asked);
	if (n < 1 || n > 64) {
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Name at most 64 area types.");
		return account_request_finish(venture_web_error_response(error), "Lightsite areas");
	}
	for (i = 0; i < n; i++) if (!lightsite_area_token(asked[i])) {
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "An area type is lowercase letters, digits and underscores.");
		return account_request_finish(venture_web_error_response(error), "Lightsite areas");
	}
	seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "types");
	json_builder_begin_array(builder);
	for (i = 0; i < n; i++) {
		const gchar *type = asked[i];
		gboolean open = FALSE;
		if (g_hash_table_contains(seen, type)) continue;
		g_hash_table_insert(seen, g_strdup(type), GINT_TO_POINTER(1));
		if (!strcmp(type, "calendar")) {
			if (!lightsite_slot_open(self, principal, "event", &event, &error))
				return account_request_finish(venture_web_error_response(error), "Lightsite areas");
			open = event == 1;
		} else if (!strcmp(type, "unpaid")) {
			if (!lightsite_slot_open(self, principal, "invoice", &invoice, &error))
				return account_request_finish(venture_web_error_response(error), "Lightsite areas");
			open = invoice == 1;
		} else if (!strcmp(type, "receivables") || !strcmp(type, "dunning_worklist")) {
			if (!lightsite_slot_open(self, principal, "invoice", &invoice, &error))
				return account_request_finish(venture_web_error_response(error), "Lightsite areas");
			open = invoice == 1 && venture_report_registry_lookup(venture_context_get_report_registry(self->context), type);
		} else if (!lightsite_area_open(self, principal, type, &error)) {
			if (error) return account_request_finish(venture_web_error_response(error), "Lightsite areas");
			continue;
		} else open = TRUE;
		if (open) json_builder_add_string_value(builder, type);
	}
	json_builder_end_array(builder);
	json_builder_end_object(builder);
	node = json_builder_get_root(builder);
	return account_request_finish(venture_web_json_response(node, 200), "Lightsite areas");
}
