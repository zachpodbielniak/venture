/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_ACCESS_RECORDS_H
#define VENTURE_ACCESS_RECORDS_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
/**
 * VentureOrganizationRole:
 * @VENTURE_ORGANIZATION_ROLE_VIEWER: reads assigned records; proposes creation
 * @VENTURE_ORGANIZATION_ROLE_OWNER: administers this organization
 * @VENTURE_ORGANIZATION_ROLE_ADMIN: administers this organization's data
 * @VENTURE_ORGANIZATION_ROLE_EDITOR: edits ordinary business records
 * @VENTURE_ORGANIZATION_ROLE_FINANCE: edits financial records
 * @VENTURE_ORGANIZATION_ROLE_SALES: edits assigned sales records
 * @VENTURE_ORGANIZATION_ROLE_SUPPORT: edits assigned support records
 *
 * Organization authority is independent of the global user role.
 */
typedef enum
{
	VENTURE_ORGANIZATION_ROLE_VIEWER,
	VENTURE_ORGANIZATION_ROLE_OWNER,
	VENTURE_ORGANIZATION_ROLE_ADMIN,
	VENTURE_ORGANIZATION_ROLE_EDITOR,
	VENTURE_ORGANIZATION_ROLE_FINANCE,
	VENTURE_ORGANIZATION_ROLE_SALES,
	VENTURE_ORGANIZATION_ROLE_SUPPORT
} VentureOrganizationRole;
/**
 * venture_organization_role_get_type:
 * Returns: the organization role enumeration
 */
GType venture_organization_role_get_type(void) G_GNUC_CONST;
#define VENTURE_TYPE_ORGANIZATION_ROLE (venture_organization_role_get_type())
#define VENTURE_TYPE_ORGANIZATION_MEMBERSHIP (venture_organization_membership_get_type())
VENTURE_DECLARE_ENTITY(VentureOrganizationMembership, venture_organization_membership, ORGANIZATION_MEMBERSHIP)
#define VENTURE_TYPE_TEAM (venture_team_get_type())
VENTURE_DECLARE_ENTITY(VentureTeam, venture_team, TEAM)
#define VENTURE_TYPE_TEAM_MEMBERSHIP (venture_team_membership_get_type())
VENTURE_DECLARE_ENTITY(VentureTeamMembership, venture_team_membership, TEAM_MEMBERSHIP)
G_END_DECLS
#endif
