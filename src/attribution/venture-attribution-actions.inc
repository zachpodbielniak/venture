/* SPDX-License-Identifier: AGPL-3.0-or-later */
static gboolean retention_allowed(VentureAction *action, VentureEntity *entity, const VentureActor *actor, GError **error)
{
	VentureAttributionService *self = venture_action_get_data(action);
	return venture_action_require_organization(action, entity, self->database, error) &&
		administrative(self, venture_entity_get_organization_id(entity), error);
}
static VentureEntity *retention_invoke(VentureAction *action, VentureEntity *entity, GHashTable *parameters,
	const VentureActor *actor, GError **error)
{
	VentureAttributionService *self = venture_action_get_data(action);
	JsonNode *value = g_hash_table_lookup(parameters, "limit");
	g_autoptr(GDateTime) now = venture_time_now();
	gint64 limit = value && !JSON_NODE_HOLDS_NULL(value) ? json_node_get_int(value) : 100;
	if (!retention_allowed(action, entity, actor, error) ||
		venture_attribution_service_sweep(self, venture_entity_get_organization_id(entity), limit, now, actor, error) < 0) return NULL;
	return g_object_ref(entity);
}
void venture_attribution_actions_register(VentureDatabase *database)
{
	g_autoptr(GPtrArray) parameters = g_ptr_array_new_with_free_func((GDestroyNotify)venture_field_spec_free);
	g_autoptr(VentureAction) action = NULL;
	g_autoptr(GError) error = NULL;
	VentureFieldSpec *field = venture_field_spec_new("organization_id", "Organization", VENTURE_FIELD_KIND_INTEGER);
	field->required = TRUE; field->has_min = TRUE; field->min_value = 1; g_ptr_array_add(parameters, field);
	field = venture_field_spec_new("limit", "Maximum expired visitors", VENTURE_FIELD_KIND_INTEGER);
	field->has_min = field->has_max = TRUE; field->min_value = 1; field->max_value = 100; g_ptr_array_add(parameters, field);
	action = g_object_new(VENTURE_TYPE_ACTION, "data-class", VENTURE_DATA_CLASS_TENANT, "type-name", "attribution_visitor", "name", "retention_sweep",
		"label", "Redact expired analytics", "description", "Forget at most 100 expired visitor capabilities and private navigation details; retain business acquisition evidence",
		"parameters", parameters, "type-level", TRUE, "stageable", FALSE, "service-transaction", TRUE, "roles", VENTURE_USER_ROLE_EDITOR, NULL);
	if (!venture_action_registry_register(venture_database_get_action_registry(database), action, retention_allowed,
		retention_invoke, venture_attribution_service_get(database), NULL, &error)) g_error("Attribution action registration: %s", error->message);
}
