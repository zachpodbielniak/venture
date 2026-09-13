/* SPDX-License-Identifier: AGPL-3.0-or-later */
static gchar *
venture_ai_tool_action(AiToolUse *tool_use, GCancellable *cancellable, GError **error, gpointer data)
{
	VentureAiService *self = data;
	VentureDatabase *db = venture_context_get_database(self->context);
	VentureActionRegistry *registry = venture_database_get_action_registry(db);
	g_auto(GStrv) types = venture_entity_registry_list_names(venture_context_get_entity_registry(self->context));
	JsonObject *input = venture_ai_tool_input(tool_use);
	guint i, j;
	if (!input) return venture_ai_tool_error("Action parameters must be an object");
	for (i = 0; types[i]; i++)
	{
		g_autoptr(GPtrArray) actions = venture_action_registry_list_for_type(registry, types[i]);
		for (j = 0; j < actions->len; j++)
		{
			VentureAction *action = g_ptr_array_index(actions, j);
			g_autofree gchar *name = NULL;
			g_autofree gchar *tool = NULL;
			g_object_get(action, "name", &name, NULL);
			tool = g_strdup_printf("venture_%s_%s", types[i], name);
			if (0 == g_strcmp0(tool, ai_tool_use_get_name(tool_use)))
			{
				g_autoptr(VentureEntity) entity = NULL;
				g_autoptr(JsonNode) values = json_node_new(JSON_NODE_OBJECT);
				g_autoptr(JsonNode) response = NULL;
				g_autoptr(GHashTable) params = NULL;
				g_autoptr(GError) local_error = NULL;
				VentureConfirmation *confirmation;
				VentureActor actor;
				JsonObject *object;
				gboolean type_level;
				gint64 id;
				GType type = venture_entity_registry_lookup(venture_context_get_entity_registry(self->context), types[i]);
				g_object_get(action, "type-level", &type_level, NULL);
				if ((!json_object_has_member(input, "id") && !type_level) ||
					(json_object_has_member(input, "id") && G_TYPE_INT64 != json_node_get_value_type(json_object_get_member(input, "id"))))
					return venture_ai_tool_error("An action requires an integer id");
				id = json_object_get_int_member_with_default(input, "id", 0);
				json_node_set_object(values, input);
				params = venture_action_parameters_from_json(values, &local_error);
				g_hash_table_remove(params, "id");
				if (!venture_ai_type_is_writable(type)) return venture_ai_tool_error("This type is unavailable to the assistant");
				entity = !id && type_level ? g_object_new(type, NULL) : venture_database_get(db, type, id, &local_error);
				if (!entity) return venture_ai_tool_error("%s", local_error->message);
				actor.kind = VENTURE_ACTOR_KIND_AI;
				actor.name = self->current_principal ? self->current_principal->name : "ai";
				actor.prompt = self->current_prompt;
				actor.request_id = NULL;
				actor.approved_by = NULL;
				confirmation = venture_confirmation_store_stage_action(venture_context_get_confirmations(self->context),
					action, entity, params, &actor, self->current_principal ? self->current_principal->role : VENTURE_USER_ROLE_VIEWER,
					"assistant", &local_error);
				if (!confirmation) return venture_ai_tool_error("%s", local_error->message);
				object = json_object_new();
				json_object_set_string_member(object, "status", "awaiting_approval");
				json_object_set_string_member(object, "confirmation_id", venture_confirmation_get_id(confirmation));
				json_object_set_string_member(object, "summary", venture_confirmation_get_summary(confirmation));
				response = json_node_new(JSON_NODE_OBJECT);
				json_node_take_object(response, object);
				return venture_json_to_string(response, FALSE);
			}
		}
	}
	return venture_ai_tool_error("Unknown or unavailable record action");
}

static void
venture_ai_register_actions(VentureAiService *self)
{
	VentureActionRegistry *registry = venture_database_get_action_registry(venture_context_get_database(self->context));
	g_auto(GStrv) types = venture_entity_registry_list_names(venture_context_get_entity_registry(self->context));
	guint i, j, k;
	if (self->streaming) return;
	for (i = 0; i < self->action_tools->len; i++)
		ai_tool_executor_unregister(self->executor, g_ptr_array_index(self->action_tools, i));
	g_ptr_array_set_size(self->action_tools, 0);
	if (VENTURE_AI_POLICY_READ_ONLY == self->policy) return;
	for (i = 0; types[i]; i++)
	{
		g_autoptr(GPtrArray) actions = venture_action_registry_list_for_type(registry, types[i]);
		if (!venture_ai_type_is_writable(venture_entity_registry_lookup(venture_context_get_entity_registry(self->context), types[i]))) continue;
		for (j = 0; j < actions->len; j++)
		{
			VentureAction *action = g_ptr_array_index(actions, j);
			g_autofree gchar *name = NULL;
			g_autofree gchar *description = NULL;
			g_autofree gchar *tool_name = NULL;
			g_autofree gchar *staged_description = NULL;
			g_autoptr(GPtrArray) parameters = NULL;
			g_autoptr(AiTool) tool = NULL;
			gboolean stageable, type_level;
			g_object_get(action, "name", &name, "description", &description, "parameters", &parameters, "stageable", &stageable, "type-level", &type_level, NULL);
			if (!stageable) continue;
			tool_name = g_strdup_printf("venture_%s_%s", types[i], name);
			staged_description = g_strconcat(description, ". Always staged for approval; nothing changes until approved.", NULL);
			tool = ai_tool_new(tool_name, staged_description);
			ai_tool_add_parameter(tool, "id", "integer", "Record identifier (zero for a type-level action)", !type_level);
			for (k = 0; k < parameters->len; k++)
			{
				VentureFieldSpec *spec = g_ptr_array_index(parameters, k);
				g_autoptr(JsonNode) schema = venture_field_spec_to_json_schema(spec);
				ai_tool_add_parameter(tool, spec->name,
					venture_json_object_get_string(json_node_get_object(schema), "type", "string"),
					venture_field_spec_get_label(spec), spec->required);
			}
			ai_tool_executor_register_callback(self->executor, tool, venture_ai_tool_action, self, NULL);
			g_ptr_array_add(self->action_tools, g_strdup(tool_name));
		}
	}
}
