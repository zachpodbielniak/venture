/* Conversion is proposed through the same confirmation queue as REST. */
static gchar *
venture_ai_tool_lead_convert(AiToolUse *tool_use, GCancellable *cancellable,
	GError **error, gpointer data)
{
	VentureAiService *self = data;
	g_autoptr(VentureAccessScope) scope = venture_orgaccess_enter_ai(self->context, self->current_principal);
	JsonObject *input = venture_ai_tool_input(tool_use);
	g_autoptr(VentureEntity) lead = NULL;
	g_autoptr(VentureEntity) converted = NULL;
	g_autoptr(JsonNode) result = NULL;
	g_autoptr(GError) local_error = NULL;
	VentureLeadService *service;
	VentureActor actor;
	VentureConfirmation *confirmation;
	(void)cancellable; (void)error;
	if (self->policy == VENTURE_AI_POLICY_READ_ONLY || !venture_context_module_enabled(self->context, "leads"))
		return venture_ai_tool_error("Lead conversion is unavailable");
	if (input == NULL) return venture_ai_tool_error("A lead id is required");
	lead = venture_database_get(venture_context_get_database(self->context), VENTURE_TYPE_LEAD,
		venture_json_object_get_int(input, "id", 0), &local_error);
	if (lead == NULL) return venture_ai_tool_error("%s", NULL != local_error ? local_error->message : "Record not found or action refused");
	actor.kind = VENTURE_ACTOR_KIND_AI;
	actor.name = self->current_principal != NULL ? self->current_principal->name : "ai";
	actor.prompt = self->current_prompt;
	actor.request_id = NULL;
	actor.approved_by = NULL;
	service = venture_database_get_lead_service(venture_context_get_database(self->context));
	if (self->policy == VENTURE_AI_POLICY_AUTONOMOUS)
	{
		converted = venture_lead_service_convert(service, lead, input, &actor, &local_error);
		if (converted == NULL) return venture_ai_tool_error("%s", NULL != local_error ? local_error->message : "Record not found or action refused");
		result = venture_serializable_to_json(VENTURE_SERIALIZABLE(converted), FALSE);
	}
	else
	{
		confirmation = venture_lead_service_stage_convert(service, venture_context_get_confirmations(self->context),
			lead, input, &actor, &local_error);
		if (confirmation == NULL) return venture_ai_tool_error("%s", NULL != local_error ? local_error->message : "Record not found or action refused");
		result = venture_confirmation_to_json(confirmation);
		json_object_set_string_member(json_node_get_object(result), "status", "awaiting_approval");
		json_object_set_string_member(json_node_get_object(result), "note", "Nothing has changed; conversion awaits approval.");
	}
	return venture_ai_tool_result(g_steal_pointer(&result));
}

static void
venture_ai_register_lead_tool(VentureAiService *self)
{
	g_autoptr(AiTool) tool = NULL;
	if (self->policy == VENTURE_AI_POLICY_READ_ONLY || !venture_context_module_enabled(self->context, "leads")) return;
	tool = venture_ai_make_tool(self, "venture_lead_convert",
		"Convert a qualified lead atomically to company, contact and optionally deal. Staged for approval unless autonomous.");
	ai_tool_add_parameter(tool, "id", "integer", "Saved qualified lead id", TRUE);
	ai_tool_add_parameter(tool, "deal", "boolean", "Create a deal; default true", FALSE);
	ai_tool_add_parameter(tool, "company_id", "integer", "Link an existing company in the same organization", FALSE);
	ai_tool_add_parameter(tool, "contact_id", "integer", "Link an existing contact in that company", FALSE);
	ai_tool_executor_register_callback(self->executor, tool, venture_ai_tool_lead_convert, self, NULL);
}
