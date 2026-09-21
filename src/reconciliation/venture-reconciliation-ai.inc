/* SPDX-License-Identifier: AGPL-3.0-or-later */
static gchar *
venture_ai_tool_reconcile_suggest(AiToolUse *tool_use, GCancellable *cancellable, GError **error, gpointer data)
{
	VentureAiService *self = data;
	/* Model callbacks must carry the caller through every scoped read and
	 * proposed action; NULL represents anonymous access, never internal work. */
	g_autoptr(VentureAccessScope) scope = venture_ai_service_enter_scope(self);
	JsonObject *input = venture_ai_tool_input(tool_use);
	g_autoptr(GError) local_error = NULL;
	g_autoptr(JsonNode) result = NULL;
	VentureActor actor;
	GType type;
	(void)cancellable;
	(void)error;
	if (input == NULL) return venture_ai_tool_error("Arguments must be an object");
	if (!venture_context_module_enabled(self->context, "reconciliation") || self->policy == VENTURE_AI_POLICY_READ_ONLY)
		return venture_ai_tool_error("Reconciliation staging is unavailable under the current module or policy settings");
	type = venture_entity_registry_lookup(venture_context_get_entity_registry(self->context), venture_json_object_get_string(input, "type", ""));
	if (type != G_TYPE_INVALID && !venture_ai_type_is_readable(type))
		return venture_ai_tool_error("The assistant cannot read that record type");
	actor.kind = VENTURE_ACTOR_KIND_AI;
	actor.name = self->current_principal != NULL ? self->current_principal->name : "ai";
	actor.prompt = self->current_prompt;
	actor.request_id = NULL;
	actor.approved_by = NULL;
	result = venture_reconciliation_service_request(venture_context_get_reconciliation_service(self->context), input, &actor, "assistant", &local_error);
	if (result == NULL) return venture_ai_tool_error("%s", local_error != NULL ? local_error->message : "The matcher failed without an explanation");
	return venture_ai_tool_result(result);
}
static void
venture_ai_reconciliation_register(VentureAiService *self)
{
	g_autoptr(AiTool) tool = NULL;
	GList *iter;
	gboolean enabled = venture_context_module_enabled(self->context, "reconciliation") && self->policy != VENTURE_AI_POLICY_READ_ONLY;
	for (iter = ai_tool_executor_get_tools(self->executor); iter != NULL; iter = iter->next)
	{
		if (g_str_equal(ai_tool_get_name(iter->data), "venture_reconcile_suggest"))
		{
			if (!enabled) ai_tool_executor_unregister(self->executor, "venture_reconcile_suggest");
			return;
		}
	}
	if (!enabled) return;
	tool = ai_tool_new("venture_reconcile_suggest", "Rank book matches for a bank line and stage suggestions for human approval. Never applies a match.");
	ai_tool_add_parameter(tool, "type", "string", "Registered transaction type", TRUE);
	ai_tool_add_parameter(tool, "id", "integer", "Transaction ID", TRUE);
	ai_tool_add_parameter(tool, "matcher", "string", "Optional matcher name", FALSE);
	ai_tool_add_parameter(tool, "threshold", "integer", "Stage scores above this threshold (0-100, default 80)", FALSE);
	ai_tool_executor_register_callback(self->executor, tool, venture_ai_tool_reconcile_suggest, self, NULL);
}
