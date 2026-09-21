/* SPDX-License-Identifier: AGPL-3.0-or-later */
static HtmxResponse *
venture_cutover_web_preview(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(JsonNode) body = NULL;
	g_autoptr(VentureEntity) result = NULL;
	g_autoptr(GError) error = NULL;
	VentureActor actor;
	HtmxResponse *gate;
	const gchar *form = htmx_request_get_form_value(request, "payload");
	(void)params;
	gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_EDITOR);
	if (gate != NULL) return gate;
	gate = venture_web_require_module_api(self, "cutover");
	if (gate != NULL) return gate;
	principal = venture_auth_authenticate(self->auth, request);
	venture_auth_to_actor(principal, &actor);
	if (form != NULL)
	{
		if (!json_parser_load_from_data(parser, form, -1, &error))
			return venture_web_error_response(error);
		body = json_node_copy(json_parser_get_root(parser));
	}
	else
		body = htmx_request_get_json(request, &error);
	if (body == NULL || !JSON_NODE_HOLDS_OBJECT(body))
	{
		g_clear_error(&error);
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Cutover preview requires a JSON object");
		return venture_web_error_response(error);
	}
	/* The batch belongs to the legal entity the operator is working in, not
	 * the install's default: previewing a subsidiary's books into the parent
	 * would post them into the wrong ledger. */
	result = venture_cutover_service_preview(venture_cutover_service_get(venture_context_get_database(self->context)),
		venture_web_active_organization(self, request), json_node_get_object(body), &actor, &error);
	if (result == NULL) return venture_web_error_response(error);
	if (form != NULL)
	{
		g_autofree gchar *path = g_strdup_printf("/e/accounting_cutover/%" G_GINT64_FORMAT, venture_entity_get_id(result));
		return venture_web_redirect_to(path);
	}
	return venture_web_json_response(venture_serializable_to_json(VENTURE_SERIALIZABLE(result), FALSE), 200);
}

static HtmxResponse *
venture_cutover_web_action(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) result = NULL;
	g_autoptr(GError) error = NULL;
	VentureActor actor;
	HtmxResponse *gate;
	const gchar *action = g_hash_table_lookup(params, "action");
	const gchar *id = g_hash_table_lookup(params, "id");
	const gchar *form_action = htmx_request_get_form_value(request, "action");
	GHashTable *empty;
	gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_EDITOR);
	if (gate != NULL) return gate;
	gate = venture_web_require_module_api(self, "cutover");
	if (gate != NULL) return gate;
	principal = venture_auth_authenticate(self->auth, request);
	venture_auth_to_actor(principal, &actor);
	if (form_action != NULL) action = form_action;
	empty = g_hash_table_new(g_str_hash, g_str_equal);
	result = venture_action_registry_perform(venture_database_get_action_registry(venture_context_get_database(self->context)),
		"accounting_cutover", id != NULL ? g_ascii_strtoll(id, NULL, 10) : 0, action, empty, &actor,
		VENTURE_USER_ROLE_EDITOR, &error);
	g_hash_table_unref(empty);
	if (result == NULL) return venture_web_error_response(error);
	if (form_action != NULL)
	{
		g_autofree gchar *path = g_strdup_printf("/e/accounting_cutover/%" G_GINT64_FORMAT, venture_entity_get_id(result));
		return venture_web_redirect_to(path);
	}
	return venture_web_json_response(venture_serializable_to_json(VENTURE_SERIALIZABLE(result), FALSE), 200);
}

/* GET template: the header row of one CSV section, ready to fill in. */
static HtmxResponse *
venture_cutover_web_template(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	VentureWebServer *self = user_data;
	const gchar *section = g_hash_table_lookup(params, "section");
	g_autofree gchar *csv = NULL;
	g_autofree gchar *disposition = NULL;
	g_autoptr(GError) error = NULL;
	HtmxResponse *gate;
	HtmxResponse *response;
	gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_EDITOR);
	if (gate != NULL) return gate;
	gate = venture_web_require_module_api(self, "cutover");
	if (gate != NULL) return gate;
	csv = venture_cutover_csv_template(section);
	if (csv == NULL)
	{
		g_set_error(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "No cutover CSV section named %s", section);
		return venture_web_error_response(error);
	}
	response = htmx_response_new_with_content(csv);
	htmx_response_set_content_type(response, "text/csv; charset=utf-8");
	htmx_response_set_status(response, 200);
	disposition = g_strdup_printf("attachment; filename=\"cutover-%s.csv\"", section);
	htmx_response_add_header(response, "Content-Disposition", disposition);
	return response;
}

static gboolean
venture_cutover_web_add_table(GHashTable *tables, const gchar *section, const gchar *data, gsize length, GError **error)
{
	if (length == 0)
		return TRUE;
	if (!g_utf8_validate(data, (gssize)length, NULL))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "%s.csv is not UTF-8 text", section);
		return FALSE;
	}
	g_hash_table_insert(tables, g_strdup(section), venture_web_csv_parse(data, length));
	return TRUE;
}

/*
 * POST csv: CSV files to a previewed batch. The form posts one file per
 * section plus the settings; the API takes JSON with the files' text under
 * "files". build_only returns the payload without creating a batch.
 */
static HtmxResponse *
venture_cutover_web_csv(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	static const gchar *const settings_names[] = {
		"source", "cutoff", "currency", "decimal_separator", "thousands_separator", "clearing_account_code"
	};
	VentureWebServer *self = user_data;
	VentureCutoverService *service = venture_cutover_service_get(venture_context_get_database(self->context));
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(JsonObject) settings = json_object_new();
	g_autoptr(JsonObject) payload = NULL;
	g_autoptr(GHashTable) tables = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_ptr_array_unref);
	g_autoptr(VentureEntity) result = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *content_type = htmx_request_get_content_type(request);
	gboolean form = content_type != NULL && g_str_has_prefix(content_type, "multipart/form-data");
	gboolean build_only = FALSE;
	VentureActor actor;
	HtmxResponse *gate;
	guint i;
	(void)params;
	gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_EDITOR);
	if (gate != NULL) return gate;
	gate = venture_web_require_module_api(self, "cutover");
	if (gate != NULL) return gate;
	principal = venture_auth_authenticate(self->auth, request);
	venture_auth_to_actor(principal, &actor);
	if (form)
	{
		GHashTable *fields = NULL;
		g_autoptr(GPtrArray) files = htmx_uploaded_file_parse_multipart(content_type,
			htmx_request_get_body_bytes(request), &fields, &error);
		if (files == NULL)
		{
			if (fields != NULL)
				g_hash_table_unref(fields);
			return venture_web_error_response(error);
		}
		for (i = 0; fields != NULL && i < G_N_ELEMENTS(settings_names); i++)
		{
			const gchar *value = g_hash_table_lookup(fields, settings_names[i]);
			if (value != NULL)
				json_object_set_string_member(settings, settings_names[i], value);
		}
		if (fields != NULL)
			g_hash_table_unref(fields);
		for (i = 0; i < files->len; i++)
		{
			const HtmxUploadedFile *file = g_ptr_array_index(files, i);
			gsize length = 0;
			const gchar *data = g_bytes_get_data(htmx_uploaded_file_get_data(file), &length);
			if (!venture_cutover_web_add_table(tables, htmx_uploaded_file_get_field_name(file), data, length, &error))
				return venture_web_error_response(error);
		}
	}
	else
	{
		g_autoptr(JsonNode) body = htmx_request_get_json(request, &error);
		JsonObject *object;
		JsonNode *files;
		if (body == NULL || !JSON_NODE_HOLDS_OBJECT(body))
		{
			g_clear_error(&error);
			g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
				"Cutover CSV import requires a JSON object with settings and files");
			return venture_web_error_response(error);
		}
		object = json_node_get_object(body);
		for (i = 0; i < G_N_ELEMENTS(settings_names); i++)
			if (json_object_has_member(object, settings_names[i]) &&
				JSON_NODE_HOLDS_VALUE(json_object_get_member(object, settings_names[i])) &&
				json_node_get_value_type(json_object_get_member(object, settings_names[i])) == G_TYPE_STRING)
				json_object_set_string_member(settings, settings_names[i], json_object_get_string_member(object, settings_names[i]));
		if (json_object_has_member(object, "build_only") &&
			JSON_NODE_HOLDS_VALUE(json_object_get_member(object, "build_only")) &&
			json_node_get_value_type(json_object_get_member(object, "build_only")) == G_TYPE_BOOLEAN)
			build_only = json_object_get_boolean_member(object, "build_only");
		files = json_object_has_member(object, "files") ? json_object_get_member(object, "files") : NULL;
		if (files != NULL && JSON_NODE_HOLDS_OBJECT(files))
		{
			g_autoptr(GList) names = json_object_get_members(json_node_get_object(files));
			GList *l;
			for (l = names; l != NULL; l = l->next)
			{
				JsonNode *text = json_object_get_member(json_node_get_object(files), l->data);
				if (!JSON_NODE_HOLDS_VALUE(text) || json_node_get_value_type(text) != G_TYPE_STRING)
				{
					g_set_error(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "files.%s must be the CSV text", (const gchar *)l->data);
					return venture_web_error_response(error);
				}
				if (!venture_cutover_web_add_table(tables, l->data, json_node_get_string(text), strlen(json_node_get_string(text)), &error))
					return venture_web_error_response(error);
			}
		}
	}
	payload = venture_cutover_service_payload_from_csv(service, settings, tables, &error);
	if (payload == NULL)
		return venture_web_error_response(error);
	if (build_only)
	{
		JsonNode *node = json_node_new(JSON_NODE_OBJECT);
		json_node_set_object(node, payload);
		return venture_web_json_response(node, 200);
	}
	result = venture_cutover_service_preview(service, venture_web_active_organization(self, request), payload, &actor, &error);
	if (result == NULL)
		return venture_web_error_response(error);
	if (form)
	{
		g_autofree gchar *path = g_strdup_printf("/e/accounting_cutover/%" G_GINT64_FORMAT, venture_entity_get_id(result));
		return venture_web_redirect_to(path);
	}
	return venture_web_json_response(venture_serializable_to_json(VENTURE_SERIALIZABLE(result), FALSE), 200);
}

static void
venture_cutover_web_register(HtmxRouter *router, VentureWebServer *self)
{
	(void)router;
	venture_web_server_add_classified_route(self, HTMX_METHOD_GET, "/api/v1/accounting_cutovers/template/:section", VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, venture_cutover_web_template, self);
	venture_web_server_add_classified_route(self, HTMX_METHOD_GET, "/cutover/template/:section", VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, venture_cutover_web_template, self);
	venture_web_server_add_classified_route(self, HTMX_METHOD_POST, "/api/v1/accounting_cutovers/csv", VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, venture_cutover_web_csv, self);
	venture_web_server_add_classified_route(self, HTMX_METHOD_POST, "/cutover/csv", VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, venture_cutover_web_csv, self);
	venture_web_server_add_classified_route(self, HTMX_METHOD_POST, "/api/v1/accounting_cutovers/preview", VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, venture_cutover_web_preview, self);
	venture_web_server_add_classified_route(self, HTMX_METHOD_POST, "/cutover/preview", VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, venture_cutover_web_preview, self);
	venture_web_server_add_classified_route(self, HTMX_METHOD_POST, "/api/v1/accounting_cutovers/:id/:action", VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, venture_cutover_web_action, self);
	venture_web_server_add_classified_route(self, HTMX_METHOD_POST, "/cutover/:id/action", VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, venture_cutover_web_action, self);
}
