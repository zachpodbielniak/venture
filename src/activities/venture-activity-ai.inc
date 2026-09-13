/* SPDX-License-Identifier: AGPL-3.0-or-later */
static gchar *
venture_ai_tool_activity(AiToolUse *tool_use, GCancellable *cancellable, GError **error, gpointer data)
{
	VentureAiService *self = data;
	JsonObject *input = venture_ai_tool_input(tool_use);
	VentureDatabase *db = venture_context_get_database(self->context);
	g_autoptr(VentureEntity) row = NULL;
	g_autoptr(VentureEntity) completed = NULL;
	g_autoptr(GError) failure = NULL;
	g_autoptr(JsonNode) node = NULL;
	VentureActor actor;
	const gchar *outcome;
	if (!venture_context_module_enabled(self->context, "activities"))
		return venture_ai_tool_error("The activities module is off");
	if (self->policy == VENTURE_AI_POLICY_READ_ONLY)
		return venture_ai_tool_error("The assistant is read-only");
	if (input == NULL)
		return venture_ai_tool_error("An activity id is required");
	row = venture_database_get(db, VENTURE_TYPE_ACTIVITY, venture_json_object_get_int(input, "id", 0), &failure);
	if (row == NULL)
		return venture_ai_tool_error("%s", NULL != failure ? failure->message : "Record not found or action refused");
	outcome = venture_json_object_get_string(input, "outcome", "");
	actor.kind = VENTURE_ACTOR_KIND_AI;
	actor.name = self->current_principal ? self->current_principal->name : "ai";
	actor.prompt = self->current_prompt;
	actor.request_id = NULL;
	actor.approved_by = NULL;
	if (self->policy == VENTURE_AI_POLICY_AUTONOMOUS)
	{
		completed = venture_activity_service_complete(venture_database_get_activity_service(db), row, outcome, &actor, &failure);
		if (completed == NULL)
			return venture_ai_tool_error("%s", NULL != failure ? failure->message : "Record not found or action refused");
		node = venture_serializable_to_json(VENTURE_SERIALIZABLE(completed), FALSE);
	}
	else
	{
		VentureConfirmation *confirmation = venture_confirmation_store_stage_activity_complete(
			venture_context_get_confirmations(self->context), row, outcome, &actor, "assistant", &failure);
		if (confirmation == NULL)
			return venture_ai_tool_error("%s", NULL != failure ? failure->message : "Record not found or action refused");
		node = venture_confirmation_to_json(confirmation);
	}
	return venture_ai_tool_result(g_steal_pointer(&node));
}
