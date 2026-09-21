/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Included by the web server so authentication and response policy stay shared. */
static HtmxResponse *
venture_bank_web_action(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(JsonNode) body = NULL, node = NULL;
	g_autoptr(VentureEntity) result = NULL;
	g_autoptr(GError) error = NULL;
	VentureActor actor;
	HtmxResponse *gate;
	const gchar *action = g_hash_table_lookup(params, "action");
	const gchar *id = g_hash_table_lookup(params, "id");
	const gchar *form = htmx_request_get_form_value(request, "parameters");
	const gchar *form_action = htmx_request_get_form_value(request, "action");
	gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_EDITOR);
	if (gate != NULL) return gate;
	gate = venture_web_require_module_api(self, "banking");
	if (gate != NULL) return gate;
	principal = venture_auth_authenticate(self->auth, request);
	venture_auth_to_actor(principal, &actor);
	if (form != NULL)
	{
		g_autoptr(JsonParser) parser = json_parser_new();
		if (!json_parser_load_from_data(parser, form, -1, &error)) return venture_web_error_response(error);
		body = json_node_copy(json_parser_get_root(parser));
	}
	else if (form_action != NULL)
	{
		static const gchar *const fields[] = { "format", "data", "period_start", "period_end", "opening_balance", "closing_balance", "reason", "type", "state",
			"date_column", "amount_column", "description_column", "reference_column", "external_id_column",
			"debit_column", "credit_column", "locale", "date_format", "category", "amount", "date", "memo", "mode", NULL };
		JsonObject *object = json_object_new();
		JsonArray *parts = json_array_new();
		const gchar *customer = htmx_request_get_form_value(request, "customer_id");
		guint i;
		for (i = 0; fields[i] != NULL; i++)
		{
			const gchar *value = htmx_request_get_form_value(request, fields[i]);
			if (value != NULL && *value) json_object_set_string_member(object, fields[i], value);
		}
		if (customer != NULL && *customer) json_object_set_int_member(object, "customer_id", g_ascii_strtoll(customer, NULL, 10));
		{
			const gchar *counterparty = htmx_request_get_form_value(request, "counterparty_bank_account_id");
			const gchar *apply = htmx_request_get_form_value(request, "apply");
			if (counterparty != NULL && *counterparty)
				json_object_set_int_member(object, "counterparty_bank_account_id", g_ascii_strtoll(counterparty, NULL, 10));
			if (apply != NULL && *apply)
				json_object_set_boolean_member(object, "apply", TRUE);
		}
		for (i = 0; i < 3; i++)
		for (i = 0; i < 50; i++)
		{
			g_autofree gchar *id_key = g_strdup_printf("record_id_%u", i);
			g_autofree gchar *type_key = g_strdup_printf("record_type_%u", i);
			g_autofree gchar *amount_key = g_strdup_printf("match_amount_%u", i);
			const gchar *record_id = htmx_request_get_form_value(request, id_key);
			const gchar *record_type = htmx_request_get_form_value(request, type_key);
			const gchar *amount = htmx_request_get_form_value(request, amount_key);
			JsonObject *part;
			if (record_type == NULL || *record_type == '\0') continue;
			if (g_strcmp0(record_type, "adjustment") && (record_id == NULL || *record_id == '\0')) continue;
			part = json_object_new();
			json_object_set_string_member(part, "type", record_type);
			if (record_id != NULL && *record_id)
				json_object_set_int_member(part, g_strcmp0(record_type, "adjustment") ? "id" : "account_id",
					g_ascii_strtoll(record_id, NULL, 10));
			if (amount != NULL && *amount) json_object_set_string_member(part, "amount", amount);
			json_array_add_object_element(parts, part);
		}
		json_object_set_array_member(object, "parts", parts);
		body = json_node_new(JSON_NODE_OBJECT);
		json_node_take_object(body, object);
	}
	else body = htmx_request_get_json(request, &error);
	if (body == NULL || !JSON_NODE_HOLDS_OBJECT(body))
	{
		g_clear_error(&error);
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Bank action parameters must be a JSON object");
		return venture_web_error_response(error);
	}
	if (form_action != NULL) action = form_action;
	result = venture_bank_match_service_execute(venture_database_get_bank_match_service(venture_context_get_database(self->context)),
		action, id != NULL ? g_ascii_strtoll(id, NULL, 10) : 0, json_node_get_object(body), &actor, &error);
	if (result == NULL) return venture_web_error_response(error);
	if (form_action != NULL)
	{
		g_autofree gchar *path = g_strdup_printf("/e/%s/%" G_GINT64_FORMAT,
			venture_entity_get_entity_name(result), venture_entity_get_id(result));
		return venture_web_redirect_to(path);
	}
	node = venture_serializable_to_json(VENTURE_SERIALIZABLE(result), FALSE);
	return venture_web_json_response(node, 200);
}

static void
venture_bank_web_register(HtmxRouter *router, VentureWebServer *self)
{
	(void)router;
	venture_web_server_add_classified_route(self, HTMX_METHOD_POST, "/api/v1/bank_transactions/:id/:action", VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, venture_bank_web_action, self);
	venture_web_server_add_classified_route(self, HTMX_METHOD_POST, "/api/v1/bank_statements/:id/:action", VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, venture_bank_web_action, self);
	venture_web_server_add_classified_route(self, HTMX_METHOD_POST, "/api/v1/bank_accounts/:id/:action", VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, venture_bank_web_action, self);
	venture_web_server_add_classified_route(self, HTMX_METHOD_POST, "/api/v1/bank_rules/:id/:action", VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, venture_bank_web_action, self);
	venture_web_server_add_classified_route(self, HTMX_METHOD_POST, "/banking/:id/action", VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, venture_bank_web_action, self);
}
