/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_TENANT_RECORDS_H
#define VENTURE_TENANT_RECORDS_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
/**
 * VentureTenantState:
 * @VENTURE_TENANT_STATE_UNKNOWN: invalid or missing state, always refused
 * @VENTURE_TENANT_STATE_ACTIVE: normal workspace operations
 * @VENTURE_TENANT_STATE_READ_ONLY: reads without writes or external effects
 * @VENTURE_TENANT_STATE_SUSPENDED: only explicit control and maintenance
 */
typedef enum {
	VENTURE_TENANT_STATE_UNKNOWN,
	VENTURE_TENANT_STATE_ACTIVE,
	VENTURE_TENANT_STATE_READ_ONLY,
	VENTURE_TENANT_STATE_SUSPENDED
} VentureTenantState;
#define VENTURE_TYPE_TENANT_STATE (venture_tenant_state_get_type())
/**
 * venture_tenant_state_get_type:
 * Returns: registered workspace lifecycle enumeration
 */
GType venture_tenant_state_get_type(void) G_GNUC_CONST;
/**
 * VentureTenantRole:
 * @VENTURE_TENANT_ROLE_UNKNOWN: invalid or missing membership, always refused
 * @VENTURE_TENANT_ROLE_MEMBER: organization-scoped workspace member
 * @VENTURE_TENANT_ROLE_ADMIN: workspace administration without platform authority
 */
typedef enum {
	VENTURE_TENANT_ROLE_UNKNOWN,
	VENTURE_TENANT_ROLE_MEMBER,
	VENTURE_TENANT_ROLE_ADMIN
} VentureTenantRole;
#define VENTURE_TYPE_TENANT_ROLE (venture_tenant_role_get_type())
/**
 * venture_tenant_role_get_type:
 * Returns: registered tenant membership enumeration
 */
GType venture_tenant_role_get_type(void) G_GNUC_CONST;

#define VENTURE_TYPE_TENANT_WORKSPACE (venture_tenant_workspace_get_type())
G_DECLARE_FINAL_TYPE(VentureTenantWorkspace, venture_tenant_workspace, VENTURE, TENANT_WORKSPACE, VentureEntity)
/**
 * venture_tenant_workspace_new:
 * Returns: (transfer full): a new service-managed hosted workspace record
 */
VentureTenantWorkspace *venture_tenant_workspace_new(void);
#define VENTURE_TYPE_TENANT_MEMBERSHIP (venture_tenant_membership_get_type())
G_DECLARE_FINAL_TYPE(VentureTenantMembership, venture_tenant_membership, VENTURE, TENANT_MEMBERSHIP, VentureEntity)
/**
 * venture_tenant_membership_new:
 * Returns: (transfer full): a new service-managed hosted workspace record
 */
VentureTenantMembership *venture_tenant_membership_new(void);
#define VENTURE_TYPE_TENANT_INVITATION (venture_tenant_invitation_get_type())
G_DECLARE_FINAL_TYPE(VentureTenantInvitation, venture_tenant_invitation, VENTURE, TENANT_INVITATION, VentureEntity)
/**
 * venture_tenant_invitation_new:
 * Returns: (transfer full): a new service-managed hosted workspace record
 */
VentureTenantInvitation *venture_tenant_invitation_new(void);
#define VENTURE_TYPE_TENANT_SUPPORT_GRANT (venture_tenant_support_grant_get_type())
G_DECLARE_FINAL_TYPE(VentureTenantSupportGrant, venture_tenant_support_grant, VENTURE, TENANT_SUPPORT_GRANT, VentureEntity)
/**
 * venture_tenant_support_grant_new:
 * Returns: (transfer full): a new service-managed hosted workspace record
 */
VentureTenantSupportGrant *venture_tenant_support_grant_new(void);
#define VENTURE_TYPE_TENANT_EVENT (venture_tenant_event_get_type())
G_DECLARE_FINAL_TYPE(VentureTenantEvent, venture_tenant_event, VENTURE, TENANT_EVENT, VentureEntity)
/**
 * venture_tenant_event_new:
 * Returns: (transfer full): a new service-managed hosted workspace record
 */
VentureTenantEvent *venture_tenant_event_new(void);
G_END_DECLS
#endif
