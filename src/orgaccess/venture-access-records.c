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
