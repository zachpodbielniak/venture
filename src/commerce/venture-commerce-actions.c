/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

static gboolean
commerce_adopt_allowed(VentureAction *action, VentureEntity *entity, const VentureActor *actor, GError **error)
{
	static const gint roles[] = { VENTURE_ORGANIZATION_ROLE_OWNER, VENTURE_ORGANIZATION_ROLE_ADMIN };
	VentureDatabase *database = venture_action_get_data(action);
	VentureAccessPolicy *policy = venture_database_get_access_policy(database);
	const VentureAuthPrincipal *principal = venture_access_policy_get_actor(policy);
	g_autofree gchar *provider = NULL;
	gboolean enabled;
	(void)actor;
	g_object_get(entity, "provider", &provider, "enabled", &enabled, NULL);
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "commerce_import_link") == G_TYPE_INVALID ||
		!enabled || !g_str_has_prefix(provider ? provider : "", "commerce.") ||
		(principal && !venture_access_policy_has_organization_role(policy, principal,
			venture_entity_get_organization_id(entity), roles, G_N_ELEMENTS(roles))))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED, "An active commerce account and organization integration administration are required");
		return FALSE;
	}
	return TRUE;
}
static VentureEntity *
commerce_adopt_invoke(VentureAction *action, VentureEntity *entity, GHashTable *params,
	const VentureActor *actor, GError **error)
{
	VentureDatabase *database = venture_action_get_data(action);
	gint64 invoice_id = json_node_get_int(g_hash_table_lookup(params, "invoice_id"));
	g_autoptr(VentureCommerceService) service = venture_commerce_service_new(database,
		venture_entity_get_organization_id(entity), NULL, error);
	if (service == NULL || !venture_commerce_service_adopt_invoice(service, venture_entity_get_organization_id(entity),
		venture_entity_get_id(entity), venture_entity_get_version(entity), invoice_id,
		json_node_get_string(g_hash_table_lookup(params, "reason")), actor, error)) return NULL;
	return venture_database_get(database, VENTURE_TYPE_INVOICE, invoice_id, error);
}
void
venture_commerce_actions_register(VentureDatabase *database)
{
	g_autoptr(GPtrArray) parameters = g_ptr_array_new_with_free_func((GDestroyNotify)venture_field_spec_free);
	g_autoptr(VentureAction) action = NULL;
	g_autoptr(GError) error = NULL;
	VentureFieldSpec *field = venture_field_spec_new("invoice_id", "Legacy invoice", VENTURE_FIELD_KIND_REFERENCE);
	field->reference_type = g_strdup("invoice");
	field->required = TRUE;
	g_ptr_array_add(parameters, field);
	field = venture_field_spec_new("reason", "Account adoption reason", VENTURE_FIELD_KIND_TEXT);
	field->required = TRUE;
	field->max_length = 4096;
	g_ptr_array_add(parameters, field);
	action = g_object_new(VENTURE_TYPE_ACTION, "data-class", VENTURE_DATA_CLASS_TENANT,
		"type-name", "integration_connection", "name", "adopt_commerce_invoice", "label", "Adopt legacy commerce invoice",
		"description", "Associate retained provider identity with this account without rewriting accounting evidence",
		"parameters", parameters, "stageable", FALSE, "service-transaction", TRUE, "roles", VENTURE_USER_ROLE_EDITOR, NULL);
	if (!venture_action_registry_register(venture_database_get_action_registry(database), action,
		commerce_adopt_allowed, commerce_adopt_invoke, database, NULL, &error))
		g_error("Commerce action registration: %s", error->message);
}
