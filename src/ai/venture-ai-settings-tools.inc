/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * venture-ai-settings-tools.inc - settings_list, settings_get, settings_set
 * (docs/settings.org)
 *
 * Included by venture-ai-service.c. The assistant reads and changes the
 * workflow settings through the one settings API. A change is always
 * staged -- under every policy -- so it reaches the books only through the
 * confirmation queue, where an owner or administrator approves exactly the
 * record change a person would make on the settings screen, and the
 * approval is its one audit entry.
 */

/* The organization a settings tool means: the one named, else the
 * assistant's. A service bound to one organization answers for no other. */
static gboolean
venture_ai_settings_organization(VentureAiService *self, JsonObject *input, gint64 *org, gchar **refusal)
{
	gint64 named = venture_json_object_get_int(input, "organization_id", 0);
	*org = named > 0 ? named : venture_ai_service_organization(self);
	if (self->organization_id > 0 && *org != self->organization_id) {
		*refusal = venture_ai_tool_error("This assistant answers for its own organization only");
		return FALSE;
	}
	return TRUE;
}

static gchar *
venture_ai_tool_settings_list(AiToolUse *tool_use, GCancellable *cancellable, GError **error, gpointer user_data)
{
	VentureAiService *self = user_data;
	g_autoptr(VentureAccessScope) scope = venture_ai_service_enter_scope(self);
	g_autoptr(GError) local_error = NULL;
	g_autoptr(GPtrArray) values = NULL;
	g_autoptr(JsonArray) items = json_array_new();
	g_autoptr(JsonObject) object = json_object_new();
	g_autoptr(JsonNode) root = json_node_new(JSON_NODE_OBJECT);
	JsonObject *input = venture_ai_tool_input(tool_use);
	const gchar *workflow = input != NULL ? venture_json_object_get_string(input, "workflow", NULL) : NULL;
	gchar *refusal = NULL;
	gint64 org = 0;
	guint i;
	(void)cancellable; (void)error;
	if (input == NULL)
		org = venture_ai_service_organization(self);
	else if (!venture_ai_settings_organization(self, input, &org, &refusal))
		return refusal;
	values = venture_settings_list(self->context, org, &local_error);
	if (values == NULL)
		return venture_ai_tool_error("%s", local_error->message);
	for (i = 0; i < values->len; i++) {
		VentureSettingValue *value = g_ptr_array_index(values, i);
		if (workflow != NULL && g_ascii_strcasecmp(workflow, value->info->workflow) != 0)
			continue;
		json_array_add_element(items, venture_setting_value_to_json(value));
	}
	json_object_set_int_member(object, "organization_id", org);
	json_object_set_array_member(object, "settings", g_steal_pointer(&items));
	json_node_set_object(root, object);
	return venture_ai_tool_result(root);
}

static gchar *
venture_ai_tool_settings_get(AiToolUse *tool_use, GCancellable *cancellable, GError **error, gpointer user_data)
{
	VentureAiService *self = user_data;
	g_autoptr(VentureAccessScope) scope = venture_ai_service_enter_scope(self);
	g_autoptr(GError) local_error = NULL;
	g_autoptr(VentureSettingValue) value = NULL;
	g_autoptr(JsonNode) node = NULL;
	JsonObject *input = venture_ai_tool_input(tool_use);
	gchar *refusal = NULL;
	gint64 org = 0;
	(void)cancellable; (void)error;
	if (input == NULL || venture_json_object_get_string(input, "key", NULL) == NULL)
		return venture_ai_tool_error("A \"key\" is required; settings_list names them");
	if (!venture_ai_settings_organization(self, input, &org, &refusal))
		return refusal;
	value = venture_settings_get(self->context, org, venture_json_object_get_string(input, "key", NULL), &local_error);
	if (value == NULL)
		return venture_ai_tool_error("%s", local_error->message);
	node = venture_setting_value_to_json(value);
	return venture_ai_tool_result(node);
}

static gchar *
venture_ai_tool_settings_set(AiToolUse *tool_use, GCancellable *cancellable, GError **error, gpointer user_data)
{
	VentureAiService *self = user_data;
	g_autoptr(VentureAccessScope) scope = venture_ai_service_enter_scope(self);
	g_autoptr(GError) local_error = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autofree gchar *value = NULL;
	JsonObject *input = venture_ai_tool_input(tool_use);
	VentureConfirmation *confirmation;
	VentureActor origin;
	const gchar *key;
	gboolean reset;
	gchar *refusal = NULL;
	gint64 org = 0;
	(void)cancellable; (void)error;
	if (input == NULL || (key = venture_json_object_get_string(input, "key", NULL)) == NULL)
		return venture_ai_tool_error("A \"key\" is required; settings_list names them");
	if (!venture_ai_settings_organization(self, input, &org, &refusal))
		return refusal;
	reset = venture_json_object_get_bool(input, "reset", FALSE);
	if (!reset) {
		value = json_object_has_member(input, "value") ? venture_settings_value_from_json(json_object_get_member(input, "value")) : NULL;
		if (value == NULL)
			return venture_ai_tool_error("Give a \"value\" (true or false, a number or text), or \"reset\": true");
	}
	origin.kind = VENTURE_ACTOR_KIND_AI;
	origin.name = (NULL != self->current_principal) ? self->current_principal->name : "ai";
	origin.prompt = self->current_prompt;
	origin.request_id = NULL;
	origin.approved_by = NULL;
	confirmation = venture_settings_stage(self->context, org, key, value, &origin, &local_error);
	if (confirmation == NULL)
		return venture_ai_tool_error("%s", local_error->message);
	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "status");
	json_builder_add_string_value(builder, "awaiting_approval");
	json_builder_set_member_name(builder, "confirmation_id");
	json_builder_add_string_value(builder, venture_confirmation_get_id(confirmation));
	json_builder_set_member_name(builder, "summary");
	json_builder_add_string_value(builder, venture_confirmation_get_summary(confirmation));
	if (venture_confirmation_get_diff(confirmation) != NULL) {
		json_builder_set_member_name(builder, "diff");
		json_builder_add_value(builder, json_node_ref(venture_confirmation_get_diff(confirmation)));
	}
	json_builder_set_member_name(builder, "note");
	json_builder_add_string_value(builder,
		"Nothing has been changed yet. A settings change always waits for an owner or administrator to approve it. "
		"Tell them what it will do and that it needs their approval; do not claim it is done.");
	json_builder_end_object(builder);
	node = json_builder_get_root(builder);
	return venture_ai_tool_result(node);
}

static void
venture_ai_settings_tools_register(VentureAiService *self, gboolean writes)
{
	g_autoptr(AiTool) list = venture_ai_make_tool(self, "settings_list",
		"List this organization's workflow settings (email, campaigns, sales, phone and text, web chat, AI on messages): "
		"each key, its value now, its default, its limits and what it does.");
	g_autoptr(AiTool) get = venture_ai_make_tool(self, "settings_get",
		"Read one workflow setting by key, for example telephony.quiet-hours.");
	ai_tool_add_parameter(list, "workflow", "string", "Only this group, for example \"Phone and text\"", FALSE);
	ai_tool_add_parameter(list, "organization_id", "integer", "The organization; the assistant's own when left out", FALSE);
	ai_tool_add_parameter(get, "key", "string", "The setting's key from settings_list", TRUE);
	ai_tool_add_parameter(get, "organization_id", "integer", "The organization; the assistant's own when left out", FALSE);
	ai_tool_executor_register_callback(self->executor, list, venture_ai_tool_settings_list, self, NULL);
	ai_tool_executor_register_callback(self->executor, get, venture_ai_tool_settings_get, self, NULL);
	if (writes) {
		g_autoptr(AiTool) set = venture_ai_make_tool(self, "settings_set",
			"Change one workflow setting, or put it back to its default. This ALWAYS stages the change for an owner or "
			"administrator to approve and does NOT apply it; say so.");
		ai_tool_add_parameter(set, "key", "string", "The setting's key from settings_list", TRUE);
		ai_tool_add_parameter(set, "value", "string", "The new value: true or false, a number, or text as settings_get shows it", FALSE);
		ai_tool_add_parameter(set, "reset", "boolean", "Put the default back instead of giving a value", FALSE);
		ai_tool_add_parameter(set, "organization_id", "integer", "The organization; the assistant's own when left out", FALSE);
		ai_tool_executor_register_callback(self->executor, set, venture_ai_tool_settings_set, self, NULL);
	}
}
