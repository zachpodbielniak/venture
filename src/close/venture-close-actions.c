/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>

static gboolean
close_action_allowed(VentureAction *action, VentureEntity *entity,
	const VentureActor *actor, GError **error)
{
	static const gint roles[] = { VENTURE_ORGANIZATION_ROLE_OWNER,
		VENTURE_ORGANIZATION_ROLE_ADMIN, VENTURE_ORGANIZATION_ROLE_FINANCE };
	VentureDatabase *database = venture_action_get_data(action);
	const VentureAuthPrincipal *principal = venture_access_policy_get_actor(venture_database_get_access_policy(database));
	(void)actor;
	if (!venture_action_require_organization(action, entity, database, error)) return FALSE;
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "close_workspace") == G_TYPE_INVALID) {
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG, "The close module is disabled");
		return FALSE;
	}
	if (principal && !venture_access_policy_has_organization_role(venture_database_get_access_policy(database),
		principal, venture_entity_get_organization_id(entity), roles, G_N_ELEMENTS(roles))) {
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED, "Organization finance authorization is required");
		return FALSE;
	}
	return TRUE;
}

static const gchar *
close_action_text(GHashTable *parameters, const gchar *name)
{
	JsonNode *node = g_hash_table_lookup(parameters, name);
	return node && !JSON_NODE_HOLDS_NULL(node) ? json_node_get_string(node) : NULL;
}

static VentureEntity *
close_action_invoke(VentureAction *action, VentureEntity *entity, GHashTable *parameters,
	const VentureActor *actor, GError **error)
{
	VentureDatabase *database = venture_action_get_data(action);
	VentureCloseService *service = venture_close_service_get(database);
	g_autofree gchar *name = NULL;
	gboolean success = FALSE;
	g_object_get(action, "name", &name, NULL);
	if (g_str_equal(name, "open_close"))
		return venture_close_service_open(service, venture_entity_get_id(entity), close_action_text(parameters, "currency"), actor, error);
	if (VENTURE_IS_CLOSE_TASK(entity))
		success = venture_close_service_complete_task(service, entity, g_str_equal(name, "waive"), close_action_text(parameters, "notes"), actor, error);
	else if (VENTURE_IS_CLOSE_DISCREPANCY(entity))
		success = venture_close_service_explain(service, entity, close_action_text(parameters, "explanation"), NULL, 0, actor, error);
	else if (g_str_equal(name, "run_checks"))
		success = venture_close_service_run_checks(service, entity, actor, error);
	else if (g_str_equal(name, "sign"))
		success = venture_close_service_sign(service, entity, close_action_text(parameters, "role"), actor, error);
	else if (g_str_equal(name, "complete"))
		success = venture_close_service_complete(service, entity, actor, error);
	else if (g_str_equal(name, "reopen"))
		success = venture_close_service_reopen(service, entity, actor, error);
	return success ? venture_database_get(database, G_OBJECT_TYPE(entity), venture_entity_get_id(entity), error) : NULL;
}

void
venture_close_actions_register(VentureDatabase *database)
{
	static const struct {
		const gchar *type, *name, *label, *description, *parameter;
		VentureFieldKind kind;
		gboolean required;
	} declarations[] = {
		{ "fiscal_period", "open_close", "Open close workspace", "Create this period's checked close checklist", "currency", VENTURE_FIELD_KIND_STRING, FALSE },
		{ "close_workspace", "run_checks", "Run close checks", "Reconcile current control balances and retain discrepancies", NULL, VENTURE_FIELD_KIND_STRING, FALSE },
		{ "close_workspace", "sign", "Sign close", "Sign as preparer or a distinct reviewer", "role", VENTURE_FIELD_KIND_ENUM, TRUE },
		{ "close_workspace", "complete", "Complete close", "Rerun checks and close the signed fiscal period", NULL, VENTURE_FIELD_KIND_STRING, FALSE },
		{ "close_workspace", "reopen", "Reopen close", "Reopen the period; retain historical signatures and require a new review", NULL, VENTURE_FIELD_KIND_STRING, FALSE },
		{ "close_task", "complete", "Complete check", "Record the reviewed checklist finding", "notes", VENTURE_FIELD_KIND_TEXT, TRUE },
		{ "close_task", "waive", "Waive check", "Record an explicit checklist waiver and its reason", "notes", VENTURE_FIELD_KIND_TEXT, TRUE },
		{ "close_discrepancy", "explain", "Explain discrepancy", "Retain the explanation for a reviewed difference", "explanation", VENTURE_FIELD_KIND_TEXT, TRUE }
	};
	guint i;
	for (i = 0; i < G_N_ELEMENTS(declarations); i++) {
		g_autoptr(GPtrArray) parameters = g_ptr_array_new_with_free_func((GDestroyNotify)venture_field_spec_free);
		g_autoptr(VentureAction) action = NULL;
		g_autoptr(GError) error = NULL;
		if (declarations[i].parameter) {
			VentureFieldSpec *field = venture_field_spec_new(declarations[i].parameter, declarations[i].parameter, declarations[i].kind);
			field->required = declarations[i].required;
			if (declarations[i].kind == VENTURE_FIELD_KIND_ENUM) field->choices = g_strsplit("preparer,reviewer", ",", -1);
			g_ptr_array_add(parameters, field);
		}
		action = g_object_new(VENTURE_TYPE_ACTION, "data-class", VENTURE_DATA_CLASS_TENANT,
			"type-name", declarations[i].type, "name", declarations[i].name, "label", declarations[i].label,
			"description", declarations[i].description, "parameters", parameters,
			"stageable", FALSE, "service-transaction", TRUE, "roles", VENTURE_USER_ROLE_EDITOR, NULL);
		if (!venture_action_registry_register(venture_database_get_action_registry(database), action,
			close_action_allowed, close_action_invoke, database, NULL, &error))
			g_error("Close action registration: %s", error->message);
	}
}
