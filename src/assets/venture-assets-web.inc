/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Included in the web translation unit to reuse its authentication policy. */
static HtmxResponse *
venture_web_asset_action(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	VentureDatabase *db = venture_context_get_database(self->context);
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) asset = NULL;
	g_autoptr(VentureEntity) original = NULL;
	g_autoptr(JsonNode) body = NULL;
	g_autoptr(JsonNode) result = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *destination = NULL;
	const gchar *operation = g_hash_table_lookup(params, "operation");
	gboolean api = g_str_has_prefix(htmx_request_get_path(request), "/api/");
	gboolean stage = FALSE;
	gint64 id = g_ascii_strtoll(g_hash_table_lookup(params, "id"), NULL, 10);
	HtmxResponse *gate;
	VentureActor actor;
	gate = api ? venture_web_api_require(self, request, VENTURE_USER_ROLE_EDITOR) : venture_web_ui_require_session(self, request);
	if (gate != NULL)
		return gate;
	gate = api ? venture_web_require_module_api(self, "assets") : venture_web_require_module_ui(self, request, "assets");
	if (gate != NULL)
		return gate;
	principal = venture_auth_authenticate(self->auth, request);
	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_EDITOR, &error))
		return venture_web_error_response(error);
	if (g_strcmp0(operation, "place-in-service") == 0)
		operation = "place";
	if (g_strcmp0(operation, "place") != 0 && g_strcmp0(operation, "dispose") != 0 && g_strcmp0(operation, "write-off") != 0)
	{
		venture_set_error_validation(&error, "operation", "Use place-in-service, dispose or write-off");
		return venture_web_error_response(error);
	}
	asset = venture_database_get(db, VENTURE_TYPE_FIXED_ASSET, id, &error);
	if (asset == NULL || venture_entity_is_deleted(asset))
	{
		if (error == NULL)
			g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Fixed asset was not found");
		return venture_web_error_response(error);
	}
	original = venture_database_get(db, VENTURE_TYPE_FIXED_ASSET, id, &error);
	if (original == NULL)
	{
		if (error == NULL)
			g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Fixed asset was not found");
		return venture_web_error_response(error);
	}
	if (api)
	{
		body = htmx_request_get_json(request, &error);
		if (body == NULL || !JSON_NODE_HOLDS_OBJECT(body))
		{
			if (error == NULL)
				venture_set_error_validation(&error, "body", "Expected a JSON object");
			return venture_web_error_response(error);
		}
		{
			g_autoptr(JsonNode) fields = venture_json_parse("{}", NULL);
			const gchar *names[] = { "in_service_at", "disposed_at", "disposal_proceeds" };
			guint i;
			for (i = 0; i < G_N_ELEMENTS(names); i++)
				if (json_object_has_member(json_node_get_object(body), names[i]))
					json_object_set_member(json_node_get_object(fields), names[i],
						json_node_copy(json_object_get_member(json_node_get_object(body), names[i])));
			if (!venture_serializable_from_json(VENTURE_SERIALIZABLE(asset), fields, &error))
				return venture_web_error_response(error);
		}
	}
	else
	{
		const gchar *proceeds = htmx_request_get_form_value(request, "disposal_proceeds");
		if (!venture_string_is_empty(proceeds) && !venture_entity_set_field_from_string(asset, "disposal-proceeds", proceeds, &error))
			return venture_web_error_response(error);
	}
	g_object_set(asset, "operation", operation, NULL);
	if (!venture_confirmation_parse_stage_flag(htmx_request_get_query_param(request, "stage"), &stage, &error))
		return venture_web_error_response(error);
	venture_auth_to_actor(principal, &actor);
	if (stage)
	{
		VentureConfirmation *confirmation = venture_confirmation_store_stage(venture_context_get_confirmations(self->context),
			VENTURE_AUDIT_ACTION_UPDATE, asset, original, &actor, "rest-api", &error);
		if (confirmation == NULL)
			return venture_web_error_response(error);
		result = venture_confirmation_to_json(confirmation);
		return venture_web_json_response(result, 202);
	}
	if (!venture_database_save(db, asset, &actor, &error))
		return venture_web_error_response(error);
	g_clear_object(&asset);
	asset = venture_database_get(db, VENTURE_TYPE_FIXED_ASSET, id, &error);
	if (asset == NULL || venture_entity_is_deleted(asset))
	{
		if (error == NULL)
			g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Fixed asset was not found");
		return venture_web_error_response(error);
	}
	if (api)
	{
		result = venture_serializable_to_json(VENTURE_SERIALIZABLE(asset), FALSE);
		return venture_web_json_response(result, 200);
	}
	destination = g_strdup_printf("/e/fixed_asset/%" G_GINT64_FORMAT, id);
	return venture_web_redirect_to(destination);
}

static HtmxResponse *
venture_web_assets_run(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(JsonNode) body = NULL;
	g_autoptr(JsonNode) result = NULL;
	g_autoptr(GError) error = NULL;
	HtmxResponse *gate;
	JsonObject *object;
	VentureActor actor;
	gint64 org;
	gint count;
	gboolean dry_run;
	(void)params;
	gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_EDITOR);
	if (gate != NULL)
		return gate;
	gate = venture_web_require_module_api(self, "assets");
	if (gate != NULL)
		return gate;
	body = htmx_request_get_json(request, &error);
	if (body == NULL || !JSON_NODE_HOLDS_OBJECT(body))
	{
		if (error == NULL)
			venture_set_error_validation(&error, "body", "Expected period, optional organization_id and dry_run");
		return venture_web_error_response(error);
	}
	object = json_node_get_object(body);
	org = venture_context_get_default_organization_id(self->context);
	dry_run = FALSE;
	if (json_object_has_member(object, "organization_id"))
	{
		GValue value = G_VALUE_INIT;
		gboolean ok = venture_json_value_from_node(json_object_get_member(object, "organization_id"), G_TYPE_INT64, &value, &error);
		if (ok)
			org = g_value_get_int64(&value);
		g_value_unset(&value);
		if (!ok)
			return venture_web_error_response(error);
	}
	if (json_object_has_member(object, "dry_run"))
	{
		GValue value = G_VALUE_INIT;
		gboolean ok = venture_json_value_from_node(json_object_get_member(object, "dry_run"), G_TYPE_BOOLEAN, &value, &error);
		if (ok)
			dry_run = g_value_get_boolean(&value);
		g_value_unset(&value);
		if (!ok)
			return venture_web_error_response(error);
	}
	{
		gboolean stage = FALSE;
		if (!venture_confirmation_parse_stage_flag(htmx_request_get_query_param(request, "stage"), &stage, &error))
			return venture_web_error_response(error);
		if (stage)
		{
			venture_set_error_validation(&error, "stage", "Stage a fixed_asset operation=run-period:YYYY-MM to approve a sweep");
			return venture_web_error_response(error);
		}
	}
	principal = venture_auth_authenticate(self->auth, request);
	venture_auth_to_actor(principal, &actor);
	count = venture_asset_service_run_period(venture_asset_service_get(venture_context_get_database(self->context)),
		venture_json_object_get_string(object, "period", ""), org, dry_run, &actor, &error);
	if (count < 0)
		return venture_web_error_response(error);
	result = venture_json_parse("{}", NULL);
	json_object_set_int_member(json_node_get_object(result), "entries", count);
	json_object_set_boolean_member(json_node_get_object(result), "dry_run", dry_run);
	return venture_web_json_response(result, 200);
}

static void
venture_web_append_asset_actions(GString *html, VentureEntity *asset)
{
	gint status;
	gint64 id = venture_entity_get_id(asset);
	g_object_get(asset, "status", &status, NULL);
	if (status == VENTURE_ASSET_STATUS_DRAFT)
		g_string_append_printf(html, "<form method=\"post\" action=\"/assets/%" G_GINT64_FORMAT "/place-in-service\"><button class=\"btn\">Place in service</button></form>", id);
	else if (status == VENTURE_ASSET_STATUS_IN_SERVICE)
	{
		g_string_append_printf(html, "<form method=\"post\" action=\"/assets/%" G_GINT64_FORMAT "/dispose\"><label>Disposal proceeds <input name=\"disposal_proceeds\" required></label><button class=\"btn\">Dispose</button></form>", id);
		g_string_append_printf(html, "<form method=\"post\" action=\"/assets/%" G_GINT64_FORMAT "/write-off\"><button class=\"btn\">Write off</button></form>", id);
	}
}
