/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Included by the route owner so generic actions use its authentication and
 * error policy, while keeping generated action rendering in one place. */
static gboolean venture_web_url_is_web(const gchar *url);

static void
venture_web_append_record_actions(VentureWebServer *self, GString *html,
	VentureEntity *entity, VentureAuthPrincipal *principal)
{
	VentureActionRegistry *registry = venture_database_get_action_registry(venture_context_get_database(self->context));
	g_autoptr(GPtrArray) actions = venture_action_registry_list_for_type(registry, venture_entity_get_entity_name(entity));
	VentureActor actor;
	guint i, j;
	venture_auth_to_actor(principal, &actor);
	for (i = 0; i < actions->len; i++)
	{
		VentureAction *action = g_ptr_array_index(actions, i);
		g_autofree gchar *name = NULL;
		g_autofree gchar *label = NULL;
		g_autoptr(GPtrArray) parameters = NULL;
		VentureUserRole role;
		gboolean type_level;
		g_object_get(action, "type-level", &type_level, NULL);
		if (type_level != (venture_entity_get_id(entity) == 0)) continue;
		g_object_get(action, "name", &name, "label", &label, "parameters", &parameters, "roles", &role, NULL);
		if (!venture_web_require_for_type(self, principal, G_OBJECT_TYPE(entity), role, NULL) ||
			!venture_action_registry_allowed(registry, action, entity, &actor, principal->role, NULL)) continue;
		g_string_append_printf(html, "<form method=\"post\" action=\"/api/v1/%s/%" G_GINT64_FORMAT "/actions/%s\" data-record-action=\"%s\">",
			venture_entity_get_entity_name(entity), venture_entity_get_id(entity), name, name);
		for (j = 0; j < parameters->len; j++)
		{
			VentureFieldSpec *spec = g_ptr_array_index(parameters, j);
			g_autoptr(VentureFieldSpec) field = venture_field_spec_copy(spec);
			if (VENTURE_FIELD_KIND_BOOLEAN == field->kind)
			{
				/* A select has one submitted value; duplicate hidden/checkbox
				 * names are collapsed by the generic form parser. */
				g_string_append(html, "<label class=\"field\"><span class=\"field-label\">");
				venture_html_escape_append(html, field->label);
				g_string_append_printf(html, "</span><select name=\"%s\"><option value=\"false\">No</option><option value=\"true\">Yes</option></select></label>", field->name);
			}
			else
			{
				if (type_level && g_str_equal(field->name, "organization_id") && venture_entity_get_organization_id(entity) > 0)
				{
					g_free(field->default_text);
					field->default_text = g_strdup_printf("%" G_GINT64_FORMAT, venture_entity_get_organization_id(entity));
				}
				/* Action timestamps retain their explicit ISO time and zone. */
				if (field->kind == VENTURE_FIELD_KIND_DATETIME) field->kind = VENTURE_FIELD_KIND_STRING;
				venture_web_append_form_field_scoped(self, html, field, NULL, venture_entity_get_organization_id(entity));
			}
		}
		g_string_append(html, "<button class=\"btn\" type=\"submit\">");
		venture_html_escape_append(html, label);
		g_string_append(html, "</button></form>");
	}
}

/* Transient action outputs are intentionally absent from ordinary record
 * serialization. Return them once using their declared metadata and secrecy. */
static void
venture_web_action_transient_result(VentureEntity *result, JsonObject *json, GString *html)
{
	g_autofree GParamSpec **properties = NULL;
	guint count, i;
	properties = g_object_class_list_properties(G_OBJECT_GET_CLASS(result), &count);
	for (i = 0; i < count; i++)
	{
		GParamSpec *property = properties[i];
		VentureColumnFlags flags = venture_entity_class_get_column_flags(VENTURE_ENTITY_GET_CLASS(result), property->name);
		g_auto(GValue) value = G_VALUE_INIT;
		g_autoptr(JsonNode) node = NULL;
		g_autofree gchar *text = NULL, *member = NULL;
		if (!(flags & VENTURE_COLUMN_FLAG_TRANSIENT) || (flags & VENTURE_COLUMN_FLAG_SENSITIVE) ||
			!(property->flags & G_PARAM_READABLE)) continue;
		g_value_init(&value, property->value_type);
		g_object_get_property(G_OBJECT(result), property->name, &value);
		node = venture_json_node_from_value(&value);
		if (!node || JSON_NODE_HOLDS_NULL(node)) continue;
		text = G_VALUE_HOLDS_STRING(&value) ? g_value_dup_string(&value) : venture_json_to_string(node, FALSE);
		if (!text || !*text) continue;
		member = venture_entity_property_to_column(property->name);
		json_object_set_member(json, member, json_node_copy(node));
		if (!html) continue;
		g_string_append(html, "<p><strong>");
		venture_html_escape_append(html, g_param_spec_get_nick(property));
		g_string_append(html, "</strong></p><p>");
		if (G_VALUE_HOLDS_STRING(&value) && venture_web_url_is_web(text))
		{
			g_string_append(html, "<a rel=\"noreferrer\" href=\"");
			venture_html_escape_append(html, text);
			g_string_append(html, "\">");
			venture_html_escape_append(html, text);
			g_string_append(html, "</a>");
		}
		else venture_html_escape_append(html, text);
		g_string_append(html, "</p>");
	}
}

static HtmxResponse *
venture_web_api_action(HtmxRequest *request, GHashTable *path, gpointer data)
{
	VentureWebServer *self = data;
	VentureDatabase *db = venture_context_get_database(self->context);
	VentureActionRegistry *registry = venture_database_get_action_registry(db);
	g_autoptr(VentureAuthPrincipal) principal = venture_auth_authenticate(self->auth, request);
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) entity = NULL;
	g_autoptr(VentureEntity) result = NULL;
	g_autoptr(JsonNode) body = NULL;
	g_autoptr(JsonNode) response = NULL;
	g_autoptr(GHashTable) params = NULL;
	const gchar *type_name = g_hash_table_lookup(path, "type");
	const gchar *name = g_hash_table_lookup(path, "action");
	const gchar *id_text = g_hash_table_lookup(path, "id");
	VentureAction *action;
	VentureUserRole role;
	VentureActor actor;
	GType type;
	gboolean stage, form;
	gboolean journal_alias = NULL == type_name;
	gint64 id;
	gchar *end = NULL;
	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_VIEWER, &error)) return venture_web_error_response(error);
	if (journal_alias)
	{
		type_name = "journal";
		name = "create_and_post";
		id_text = "0";
		type = venture_entity_registry_lookup(venture_context_get_entity_registry(self->context), type_name);
	}
	else if (!venture_web_resolve_type(self, path, &type, &error)) return venture_web_error_response(error);
	action = venture_action_registry_lookup(registry, type_name, name);
	if (!action)
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Unknown record action");
		return venture_web_error_response(error);
	}
	g_object_get(action, "roles", &role, NULL);
	if (!venture_web_require_for_type(self, principal, type, role, &error)) return venture_web_error_response(error);
	if (!venture_confirmation_parse_stage_flag(htmx_request_get_query_param(request, "stage"), &stage, &error)) return venture_web_error_response(error);
	id = g_ascii_strtoll(id_text ? id_text : "", &end, 10);
	if (id < 0 || !end || *end || end == id_text)
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT, "A positive record ID is required");
		return venture_web_error_response(error);
	}
	form = g_str_has_prefix(htmx_request_get_content_type(request) ? htmx_request_get_content_type(request) : "", "application/x-www-form-urlencoded");
	if (form)
	{
		g_autoptr(GPtrArray) specs = NULL;
		JsonObject *object = json_object_new();
		GHashTableIter iter;
		gpointer key, value;
		body = json_node_new(JSON_NODE_OBJECT);
		json_node_take_object(body, object);
		g_object_get(action, "parameters", &specs, NULL);
		g_hash_table_iter_init(&iter, htmx_request_get_form_data(request));
		while (g_hash_table_iter_next(&iter, &key, &value))
		{
			guint i;
			VentureFieldSpec *spec = NULL;
			for (i = 0; i < specs->len; i++)
			{
				VentureFieldSpec *candidate = g_ptr_array_index(specs, i);
				if (0 == g_strcmp0(key, candidate->name)) { spec = candidate; break; }
			}
			if (spec && !spec->required && venture_string_is_empty(value)) continue;
			if (spec && (VENTURE_FIELD_KIND_INTEGER == spec->kind || VENTURE_FIELD_KIND_REFERENCE == spec->kind ||
				VENTURE_FIELD_KIND_BOOLEAN == spec->kind || VENTURE_FIELD_KIND_DOUBLE == spec->kind || VENTURE_FIELD_KIND_JSON == spec->kind))
			{
				JsonNode *parsed = venture_json_parse(value, &error);
				if (!parsed) return venture_web_error_response(error);
				json_object_set_member(object, key, parsed);
			}
			else json_object_set_string_member(object, key, value);
		}
	}
	else body = htmx_request_get_json(request, &error);
	if (!body) return venture_web_error_response(error);
	if (journal_alias)
	{
		JsonObject *object = json_object_new();
		json_object_set_member(object, "journal", g_steal_pointer(&body));
		body = json_node_new(JSON_NODE_OBJECT);
		json_node_take_object(body, object);
	}
	params = venture_action_parameters_from_json(body, &error);
	if (!params) return venture_web_error_response(error);
	venture_auth_to_actor(principal, &actor);
	/* Posting has a dedicated proposal path because organization editors
	 * cannot read financial rows and require a finance approver. Reuse its
	 * line fingerprint and authorization for the generated action too. */
	if (!g_strcmp0(type_name, "journal") && !g_strcmp0(name, "post"))
	{
		gboolean proposed = FALSE;
		if (!venture_action_validate_parameters(action, params, &error)) return venture_web_error_response(error);
		response = venture_orgaccess_post_journal(self->context, principal, id, stage, &proposed, &error);
		if (!response) return venture_web_error_response(error);
		if (proposed)
		{
			JsonObject *object = json_object_new();
			json_object_set_string_member(object, "status", "awaiting_approval");
			json_object_set_boolean_member(object, "staged", TRUE);
			json_object_set_member(object, "confirmation", g_steal_pointer(&response));
			response = json_node_new(JSON_NODE_OBJECT);
			json_node_take_object(response, object);
		}
		return venture_web_json_response(response, proposed ? 202 : 200);
	}
	if (stage)
	{
		VentureConfirmation *confirmation;
		JsonObject *object;
		entity = id ? venture_database_get(db, type, id, &error) : g_object_new(type, NULL);
		if (!entity) return venture_web_error_response(error);
		confirmation = venture_confirmation_store_stage_action(venture_context_get_confirmations(self->context),
			action, entity, params, &actor, principal->role, "rest-api", &error);
		if (!confirmation) return venture_web_error_response(error);
		object = json_object_new();
		json_object_set_string_member(object, "status", "awaiting_approval");
		json_object_set_boolean_member(object, "staged", TRUE);
		json_object_set_member(object, "confirmation", venture_confirmation_to_json(confirmation));
		response = json_node_new(JSON_NODE_OBJECT);
		json_node_take_object(response, object);
		return venture_web_json_response(response, 202);
	}
	result = venture_action_registry_perform(registry, type_name, id, name, params, &actor, principal->role, &error);
	if (!result) return venture_web_error_response(error);
	response = venture_serializable_to_json(VENTURE_SERIALIZABLE(result), FALSE);
	if (form)
	{
		g_autofree gchar *location = g_strdup_printf("/e/%s/%" G_GINT64_FORMAT,
			venture_entity_get_entity_name(result), venture_entity_get_id(result));
		g_autoptr(GString) transient = g_string_new("");
		HtmxResponse *redirect;
		venture_web_action_transient_result(result, json_node_get_object(response), transient);
		if (transient->len)
		{
			HtmxResponse *shown;
			g_string_append_printf(transient, "<p><a href=\"%s\">View record</a></p>", location);
			shown = venture_web_html_response(venture_web_page(self, request, NULL, "Action result", transient->str), 200);
			htmx_response_add_header(shown, "Cache-Control", "no-store");
			htmx_response_add_header(shown, "Referrer-Policy", "no-referrer");
			return shown;
		}
		redirect = htmx_response_new();
		htmx_response_set_status(redirect, 303);
		htmx_response_add_header(redirect, "Location", location);
		return redirect;
	}
	venture_web_action_transient_result(result, json_node_get_object(response), NULL);
	{
		HtmxResponse *reply = venture_web_json_response(response, journal_alias ? 201 : 200);
		htmx_response_add_header(reply, "Cache-Control", "no-store");
		return reply;
	}
}
