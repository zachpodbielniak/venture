/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

GType
venture_organization_role_get_type(void)
{
	static gsize type_id = 0;
	if (g_once_init_enter(&type_id))
	{
		static const GEnumValue values[] = {
			{ VENTURE_ORGANIZATION_ROLE_VIEWER, "VENTURE_ORGANIZATION_ROLE_VIEWER", "viewer" },
			{ VENTURE_ORGANIZATION_ROLE_OWNER, "VENTURE_ORGANIZATION_ROLE_OWNER", "owner" },
			{ VENTURE_ORGANIZATION_ROLE_ADMIN, "VENTURE_ORGANIZATION_ROLE_ADMIN", "admin" },
			{ VENTURE_ORGANIZATION_ROLE_EDITOR, "VENTURE_ORGANIZATION_ROLE_EDITOR", "editor" },
			{ VENTURE_ORGANIZATION_ROLE_FINANCE, "VENTURE_ORGANIZATION_ROLE_FINANCE", "finance" },
			{ VENTURE_ORGANIZATION_ROLE_SALES, "VENTURE_ORGANIZATION_ROLE_SALES", "sales" },
			{ VENTURE_ORGANIZATION_ROLE_SUPPORT, "VENTURE_ORGANIZATION_ROLE_SUPPORT", "support" },
			{ VENTURE_ORGANIZATION_ROLE_ACCOUNTANT, "VENTURE_ORGANIZATION_ROLE_ACCOUNTANT", "accountant" },
			{ 0, NULL, NULL }
		};
		GType id = g_enum_register_static("VentureOrganizationRole", values);
		g_once_init_leave(&type_id, id);
	}
	return type_id;
}

static const VentureFieldDecl membership_fields[] = {
	VENTURE_FIELD_REF("user-id", "User", NULL, "user", VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_ENUM("role", "Role", NULL, venture_organization_role_get_type, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("active", "Active", NULL, VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_INDEXED)
};
VENTURE_DEFINE_ENTITY(VentureOrganizationMembership, venture_organization_membership, membership_fields)

static const VentureFieldDecl team_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", "Team within this organization")
};
VENTURE_DEFINE_ENTITY(VentureTeam, venture_team, team_fields)

static const VentureFieldDecl team_membership_fields[] = {
	VENTURE_FIELD_REF("user-id", "User", NULL, "user", VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_REF("team-id", "Team", NULL, "team", VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("active", "Active", NULL, VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_INDEXED)
};
VENTURE_DEFINE_ENTITY(VentureTeamMembership, venture_team_membership, team_membership_fields)

/* Module declarations are topologically ordered, so traversing previously
 * registered requirements is finite and also covers future finance plugins. */
static gboolean
requires_finance(VentureModuleRegistry *registry, const gchar *name)
{
	VentureModule *module;
	const gchar *const *requires;
	guint i;
	if (g_strcmp0(name, "finance") == 0)
		return TRUE;
	module = venture_module_registry_lookup(registry, name);
	if (module == NULL)
		return FALSE;
	requires = venture_module_get_requires(module);
	for (i = 0; requires != NULL && requires[i] != NULL; i++)
		if (requires_finance(registry, requires[i]))
			return TRUE;
	return FALSE;
}

void
venture_access_records_tag_module(VentureModuleRegistry *registry, const VentureModuleInfo *info)
{
	guint i;
	gboolean financial = g_strcmp0(info->name, "finance") == 0;
	for (i = 0; !financial && info->requires != NULL && info->requires[i] != NULL; i++)
		financial = requires_finance(registry, info->requires[i]);
	if (!financial)
		return;
	for (i = 0; NULL != info->entity_types && NULL != info->entity_types[i]; i++)
		venture_access_type_set_financial(info->entity_types[i](), TRUE);
}
void
venture_access_type_set_financial(GType type, gboolean financial)
{
	g_type_set_qdata(type, g_quark_from_static_string("venture-access-financial"), GINT_TO_POINTER(financial));
}
