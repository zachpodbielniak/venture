/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

GType
venture_tenant_state_get_type(void)
{
	static gsize type_id = 0;
	if (g_once_init_enter(&type_id)) {
		static const GEnumValue values[] = {
			{ VENTURE_TENANT_STATE_UNKNOWN, "VENTURE_TENANT_STATE_UNKNOWN", "unknown" },
			{ VENTURE_TENANT_STATE_ACTIVE, "VENTURE_TENANT_STATE_ACTIVE", "active" },
			{ VENTURE_TENANT_STATE_READ_ONLY, "VENTURE_TENANT_STATE_READ_ONLY", "read_only" },
			{ VENTURE_TENANT_STATE_SUSPENDED, "VENTURE_TENANT_STATE_SUSPENDED", "suspended" },
			{ 0, NULL, NULL }
		};
		GType id = g_enum_register_static("VentureTenantState", values);
		g_once_init_leave(&type_id, id);
	}
	return (GType)type_id;
}

GType
venture_tenant_role_get_type(void)
{
	static gsize type_id = 0;
	if (g_once_init_enter(&type_id)) {
		static const GEnumValue values[] = {
			{ VENTURE_TENANT_ROLE_UNKNOWN, "VENTURE_TENANT_ROLE_UNKNOWN", "unknown" },
			{ VENTURE_TENANT_ROLE_MEMBER, "VENTURE_TENANT_ROLE_MEMBER", "member" },
			{ VENTURE_TENANT_ROLE_ADMIN, "VENTURE_TENANT_ROLE_ADMIN", "admin" },
			{ 0, NULL, NULL }
		};
		GType id = g_enum_register_static("VentureTenantRole", values);
		g_once_init_leave(&type_id, id);
	}
	return (GType)type_id;
}

static const VentureFieldDecl workspace_fields[] = {
	VENTURE_FIELD("administration-sequence", "Administration sequence", "Serializes cross-row authority invariants", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("binding-key", "Binding", "Exactly one immutable process workspace", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_UNIQUE),
	VENTURE_FIELD("workspace-id", "Workspace UUID", "Immutable database identity", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_UNIQUE),
	VENTURE_FIELD("origin", "Public origin", "Immutable verified tenant origin", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_ENUM("state", "Lifecycle", "active, read_only or suspended", venture_tenant_state_get_type, VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("reason", "Reason", "Last lifecycle transition reason", VENTURE_FIELD_KIND_TEXT, VENTURE_COLUMN_FLAG_NONE),
};
VENTURE_DEFINE_ENTITY(VentureTenantWorkspace, venture_tenant_workspace, workspace_fields)

static const VentureFieldDecl membership_fields[] = {
	VENTURE_FIELD_NAME("name", "Member", "Explicit local username"),
	VENTURE_FIELD("user-id", "User", "Explicit local identity", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_UNIQUE),
	VENTURE_FIELD_ENUM("role", "Tenant role", "admin or member; never platform authority", venture_tenant_role_get_type, VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("active", "Active", "Revocation is checked on every request", VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
};
VENTURE_DEFINE_ENTITY(VentureTenantMembership, venture_tenant_membership, membership_fields)

static const VentureFieldDecl invitation_fields[] = {
	VENTURE_FIELD("recovery-user-id", "Recovery identity", "Explicit retained user; zero means ordinary invitation", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("recovery-membership-version", "Recovery authority version", "Reviewed member version; changes revoke recovery", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("issuer-user-id", "Inviting administrator", "Explicit issuing identity", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("issuer-session-generation", "Issuer credential generation", "Credential recovery revokes pending invitations", VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("issuer-membership-version", "Authority version", "Membership changes revoke pending invitations", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_ENUM("organization-role", "Organization role", "Role in the initial legal organization", venture_organization_role_get_type, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("token-hash", "Token hash", "Hash of single-use invitation capability", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_SENSITIVE | VENTURE_COLUMN_FLAG_UNIQUE),
	VENTURE_FIELD("capability", "Invitation capability", "Shown only by the creation action", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_TRANSIENT),
	VENTURE_FIELD_ENUM("role", "Tenant role", "admin or member", venture_tenant_role_get_type, VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("expires-at", "Expires", "Bounded invitation expiration", VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("consumed-at", "Consumed", "Successful identity acceptance time", VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("consumed-user-id", "Accepted user", "Explicit identity; never inferred from email", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("initial-organization-id", "Initial organization", "Legal organization granted on acceptance", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
};
VENTURE_DEFINE_ENTITY(VentureTenantInvitation, venture_tenant_invitation, invitation_fields)

static const VentureFieldDecl support_grant_fields[] = {
	VENTURE_FIELD("token-hash", "Token hash", "Hash of scoped operator capability", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_SENSITIVE | VENTURE_COLUMN_FLAG_UNIQUE),
	VENTURE_FIELD("capability", "Support capability", "Shown once at grant creation", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_TRANSIENT),
	VENTURE_FIELD("owner-user-id", "Operator", "Explicit authenticated support identity", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("allow-write", "Allow repairs", "Explicit scoped record writes; no host-resource authority", VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("include-private", "Include private records", "Explicit access to private records within the scoped organization", VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("reason", "Reason", "Required operator justification", VENTURE_FIELD_KIND_TEXT, VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("expires-at", "Expires", "Bounded support expiration", VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("revoked", "Revoked", "Immediate support revocation", VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("emergency", "Emergency", "Explicit audited operator recovery", VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
};
VENTURE_DEFINE_ENTITY(VentureTenantSupportGrant, venture_tenant_support_grant, support_grant_fields)

static const VentureFieldDecl event_fields[] = {
	VENTURE_FIELD("event", "Event", "Immutable administrative or support event", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("actor-user-id", "Actor", "Local identity or zero for local operator", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("grant-id", "Support grant", "Support authority if used", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("resource", "Resource", "Classified resource without sensitive payload", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("reason", "Reason", "Administrative justification", VENTURE_FIELD_KIND_TEXT, VENTURE_COLUMN_FLAG_NONE),
};
VENTURE_DEFINE_ENTITY(VentureTenantEvent, venture_tenant_event, event_fields)

/* One row per trusted-service sign-up, keyed by the caller's idempotency
 * key. The unique key is what turns a retry into a replay of this result
 * instead of a second user or business. */
static const VentureFieldDecl signup_fields[] = {
	VENTURE_FIELD("idempotency-key", "Idempotency key", "Caller's key for one sign-up", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_UNIQUE | VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("request-hash", "Request hash", "SHA-256 of the canonical request", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("user-id", "User", "Created or linked owner", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("business-id", "Business", "Organization created for the owner", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("linked", "Linked", "An existing user was linked, not created", VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("issuer", "Issuer", "Identity provider realm asserted by the caller", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("subject", "Subject", "Provider subject asserted by the caller; not a sign-in link", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("actor-user-id", "Administrator", "Workspace administrator whose token asserted it", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
};
VENTURE_DEFINE_ENTITY(VentureTenantSignup, venture_tenant_signup, signup_fields)

/* A provider identity linked to one local user, so that provider's access
 * token acts as that user (src/tenant/venture-tenant-identity.c). Written only
 * by the trusted sign-up and by the person's own signed-in link; never by a
 * claim, an email or generic CRUD. */
static const VentureFieldDecl identity_fields[] = {
	VENTURE_FIELD("user-id", "User", "Local user the provider identity acts as", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("issuer", "Issuer", "Exact trusted provider issuer", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("subject", "Subject", "Verified opaque provider subject", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_SENSITIVE),
	VENTURE_FIELD("identity-key", "Identity key", "Issuer and subject uniqueness", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_UNIQUE | VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_SENSITIVE),
	VENTURE_FIELD("active", "Active", "An inactive link authenticates nobody", VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
};
VENTURE_DEFINE_ENTITY(VentureTenantIdentity, venture_tenant_identity, identity_fields)
