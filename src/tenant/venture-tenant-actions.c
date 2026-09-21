/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

static gboolean
tenant_action_allowed(VentureAction *action, VentureEntity *entity, const VentureActor *actor, GError **error)
{
	VentureDatabase *database = venture_action_get_data(action);
	VentureTenantService *service = venture_tenant_service_get(database);
	const VentureAuthPrincipal *principal = venture_access_policy_get_actor(venture_database_get_access_policy(database));
	(void)entity; (void)actor;
	if (venture_tenant_service_is_enabled(service) && principal && principal->token_id == 0 &&
	    venture_tenant_service_check_principal(service, principal, NULL) &&
	    venture_tenant_service_is_member(service, principal->user_id, TRUE)) return TRUE;
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED,
		"Workspace administration requires an interactive tenant administrator");
	return FALSE;
}

static VentureEntity *
tenant_action_invoke(VentureAction *action, VentureEntity *entity, GHashTable *params,
	const VentureActor *actor, GError **error)
{
	VentureDatabase *database = venture_action_get_data(action);
	VentureTenantService *service = venture_tenant_service_get(database);
	const gchar *reason = json_node_get_string(g_hash_table_lookup(params, "reason"));
	g_autofree gchar *name = NULL;
	gboolean ok = FALSE;
	(void)actor;
	g_object_get(action, "name", &name, NULL);
	if (g_str_equal(name, "invite_recovery")) {
		gint64 user_id = 0;
		gint64 lifetime = json_node_get_int(g_hash_table_lookup(params, "lifetime_seconds"));
		g_object_get(entity, "user-id", &user_id, NULL);
		if (lifetime < 60 || lifetime > 604800) {
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Recovery lifetime must be 60 through 604800 seconds");
			return NULL;
		}
		return VENTURE_ENTITY(venture_tenant_service_invite_recovery(service, user_id, (guint)lifetime, reason, error));
	}
	if (g_str_equal(name, "invite")) {
		gint organization_role = VENTURE_ORGANIZATION_ROLE_VIEWER;
		gint64 lifetime = json_node_get_int(g_hash_table_lookup(params, "lifetime_seconds"));
		if (lifetime < 60 || lifetime > 604800) {
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Invitation lifetime must be 60 through 604800 seconds");
			return NULL;
		}
		venture_enum_from_nick(VENTURE_TYPE_ORGANIZATION_ROLE,
			json_node_get_string(g_hash_table_lookup(params, "organization_role")), &organization_role);
		return VENTURE_ENTITY(venture_tenant_service_invite(service,
			json_node_get_string(g_hash_table_lookup(params, "role")), venture_entity_get_organization_id(entity),
			organization_role, (guint)lifetime, reason, error));
	}
	if (g_str_equal(name, "set_state"))
		ok = venture_tenant_service_set_state(service, json_node_get_string(g_hash_table_lookup(params, "state")), reason, error);
	else if (g_str_equal(name, "set_membership")) {
		gint64 user_id = 0;
		g_object_get(entity, "user-id", &user_id, NULL);
		ok = venture_tenant_service_set_membership(service, user_id,
			json_node_get_string(g_hash_table_lookup(params, "role")),
			json_node_get_boolean(g_hash_table_lookup(params, "active")), reason, error);
	} else if (g_str_equal(name, "revoke"))
		ok = venture_tenant_service_revoke_support(service, venture_entity_get_id(entity), reason, error);
	if (!ok) return NULL;
	return venture_database_get(database, G_OBJECT_TYPE(entity), venture_entity_get_id(entity), error);
}

static void
tenant_parameter(GPtrArray *parameters, const gchar *name, const gchar *label,
	VentureFieldKind kind, const gchar *choices)
{
	VentureFieldSpec *field = venture_field_spec_new(name, label, kind);
	field->required = TRUE;
	if (choices) field->choices = g_strsplit(choices, ",", -1);
	if (kind == VENTURE_FIELD_KIND_REFERENCE) field->reference_type = g_strdup("organization");
	g_ptr_array_add(parameters, field);
}

void
venture_tenant_actions_register(VentureDatabase *database)
{
	static const struct { const gchar *type; const gchar *name; const gchar *label; } declarations[] = {
		{ "tenant_workspace", "set_state", "Change workspace lifecycle" },
		{ "tenant_membership", "set_membership", "Change member access" },
		{ "tenant_invitation", "invite", "Invite a workspace member" },
		{ "tenant_support_grant", "revoke", "Revoke support access" },
		{ "tenant_membership", "invite_recovery", "Recover quarantined member" }
	};
	guint i;
	for (i = 0; i < G_N_ELEMENTS(declarations); i++) {
		g_autoptr(GPtrArray) parameters = g_ptr_array_new_with_free_func((GDestroyNotify)venture_field_spec_free);
		g_autoptr(VentureAction) action = NULL;
		g_autoptr(GError) error = NULL;
		if (i == 0) tenant_parameter(parameters, "state", "Lifecycle", VENTURE_FIELD_KIND_ENUM, "active,read_only,suspended");
		if (i == 1 || i == 2) tenant_parameter(parameters, "role", "Workspace role", VENTURE_FIELD_KIND_ENUM, "member,admin");
		if (i == 1) tenant_parameter(parameters, "active", "Active membership", VENTURE_FIELD_KIND_BOOLEAN, NULL);
		if (i == 2) {
			tenant_parameter(parameters, "organization_id", "Initial organization", VENTURE_FIELD_KIND_REFERENCE, NULL);
			tenant_parameter(parameters, "organization_role", "Initial organization role", VENTURE_FIELD_KIND_ENUM,
				"admin,finance,editor,viewer,sales,support,accountant");
			tenant_parameter(parameters, "lifetime_seconds", "Lifetime in seconds (60–604800)", VENTURE_FIELD_KIND_INTEGER, NULL);
		}
		if (i == 4) tenant_parameter(parameters, "lifetime_seconds", "Recovery lifetime in seconds (60–604800)", VENTURE_FIELD_KIND_INTEGER, NULL);
		tenant_parameter(parameters, "reason", "Administrative reason", VENTURE_FIELD_KIND_TEXT, NULL);
		action = g_object_new(VENTURE_TYPE_ACTION, "data-class", VENTURE_DATA_CLASS_TENANT_ADMIN,
			"type-name", declarations[i].type, "name", declarations[i].name, "label", declarations[i].label,
			"description", "Audited workspace administration; never grants platform authority",
			"parameters", parameters, "type-level", i == 2, "stageable", FALSE,
			"service-transaction", TRUE, "roles", VENTURE_USER_ROLE_EDITOR, NULL);
		if (!venture_action_registry_register(venture_database_get_action_registry(database), action,
			tenant_action_allowed, tenant_action_invoke, database, NULL, &error))
			g_error("Tenant action registration: %s", error->message);
	}
}
