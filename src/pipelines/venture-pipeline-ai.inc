/* SPDX-License-Identifier: AGPL-3.0-or-later */
static gchar *
venture_ai_tool_deal_move(AiToolUse *use, GCancellable *cancellable, GError **error, gpointer data)
{
	VentureAiService *self = data;
	g_autoptr(VentureAccessScope) scope = venture_orgaccess_enter_ai(self->context, self->current_principal);
	JsonObject *input = venture_ai_tool_input(use);
	g_autoptr(VentureEntity) deal = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GError) local_error = NULL;
	VentureConfirmation *confirmation;
	VentureActor actor;
	gint64 stage_id;
	(void)cancellable;
	(void)error;
	if (VENTURE_AI_POLICY_READ_ONLY == self->policy || !venture_context_module_enabled(self->context, "pipelines"))
		return venture_ai_tool_error("Deal moves are unavailable under this policy or module configuration");
	deal = venture_database_get(venture_context_get_database(self->context), VENTURE_TYPE_DEAL,
		venture_json_object_get_int(input, "id", 0), &local_error);
	if (NULL == deal)
		return venture_ai_tool_error("%s", NULL != local_error ? local_error->message : "Record not found or action refused");
	stage_id = venture_json_object_get_int(input, "stage_id", 0);
	actor.kind = VENTURE_ACTOR_KIND_AI;
	actor.name = NULL != self->current_principal ? self->current_principal->name : "ai";
	actor.prompt = self->current_prompt;
	actor.request_id = NULL;
	actor.approved_by = NULL;
	confirmation = venture_confirmation_store_stage_deal_move(venture_context_get_confirmations(self->context),
		VENTURE_DEAL(deal), stage_id, venture_json_object_get_string(input, "note", NULL), &actor, "assistant", &local_error);
	if (NULL == confirmation)
		return venture_ai_tool_error("%s", NULL != local_error ? local_error->message : "Record not found or action refused");
	node = venture_confirmation_to_json(confirmation);
	json_object_set_string_member(json_node_get_object(node), "status", "awaiting_approval");
	return venture_ai_tool_result(g_steal_pointer(&node));
}
static void
venture_ai_register_deal_move(VentureAiService *self)
{
	g_autoptr(AiTool) tool = NULL;
	if (!venture_context_module_enabled(self->context, "pipelines") || VENTURE_AI_POLICY_READ_ONLY == self->policy)
		return;
	tool = venture_ai_make_tool(self, "venture_deal_move", "Propose a deal stage move. Nothing changes until a person approves it.");
	ai_tool_add_parameter(tool, "id", "integer", "Deal id", TRUE);
	ai_tool_add_parameter(tool, "stage_id", "integer", "Destination stage in the deal's pipeline", TRUE);
	ai_tool_add_parameter(tool, "note", "string", "Transition explanation", FALSE);
	ai_tool_executor_register_callback(self->executor, tool, venture_ai_tool_deal_move, self, NULL);
}
