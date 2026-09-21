/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_TENANT_SERVICE_H
#define VENTURE_TENANT_SERVICE_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
#define VENTURE_TYPE_TENANT_SERVICE (venture_tenant_service_get_type())
G_DECLARE_FINAL_TYPE(VentureTenantService, venture_tenant_service, VENTURE, TENANT_SERVICE, GObject)
/**
 * venture_tenant_service_get:
 * @database: database, weakly held
 * Returns: (transfer none): database-owned hosted lifecycle service
 */
VentureTenantService *venture_tenant_service_get(VentureDatabase *database);
/**
 * venture_tenant_service_configure:
 * @self: service
 * @config: trusted startup configuration
 * @error: (out) (optional): invalid or changed configuration
 *
 * Pins configuration in memory before migration. Reconfiguration cannot change
 * identity or disable an enabled boundary within a running process.
 * Returns: whether configuration was accepted
 */
gboolean venture_tenant_service_configure(VentureTenantService *self, VentureConfig *config, GError **error);
/**
 * venture_tenant_service_initialize:
 * @self: configured service
 * @error: (out) (optional): durable identity mismatch
 *
 * Pins the workspace after migrations, without requiring an administrator.
 * An existing hosted database cannot be opened with hosted mode disabled.
 * Returns: whether the durable boundary is ready
 */
gboolean venture_tenant_service_initialize(VentureTenantService *self, GError **error);
/**
 * venture_tenant_service_is_enabled:
 * @self: service
 * Returns: whether hosted enforcement is configured
 */
gboolean venture_tenant_service_is_enabled(VentureTenantService *self);
/**
 * venture_tenant_service_get_workspace_id:
 * @self: service
 * Returns: (nullable) (transfer none): immutable configured workspace UUID
 */
const gchar *venture_tenant_service_get_workspace_id(VentureTenantService *self);
/**
 * venture_tenant_service_get_origin:
 * @self: service
 * Returns: (nullable) (transfer none): immutable configured public origin
 */
const gchar *venture_tenant_service_get_origin(VentureTenantService *self);
/**
 * venture_tenant_service_check_operation:
 * @self: service
 * @write: whether an operation writes or starts an external side effect
 * @error: (out) (optional): lifecycle refusal
 *
 * Re-reads durable lifecycle state. Internal callers are not maintenance
 * operators merely because they lack a request actor.
 * Returns: whether the operation may proceed
 */
gboolean venture_tenant_service_check_operation(VentureTenantService *self, gboolean write, GError **error);
/**
 * venture_tenant_service_check_write:
 * @self: service
 * @entity: proposed record mutation
 * @error: (out) (optional): refusal
 *
 * Protects hosted control records through every repository writer, including
 * deletion and internal jobs. Dedicated service operations hold narrow scopes.
 * Returns: whether the record mutation is permitted
 */
gboolean venture_tenant_service_check_write(VentureTenantService *self, VentureEntity *entity, GError **error);
/**
 * venture_tenant_service_bootstrap_admin:
 * @self: initialized hosted service
 * @config: trusted password policy
 * @username: explicit local username, never an email match
 * @password: one-time password obtained privately by the operator
 * @recover: explicitly reset an existing identity and revoke its credentials
 * @reason: required bounded administrative justification
 * @error: (out) (optional): refusal
 *
 * Local operator recovery only: refuses a request principal. Creates an EDITOR
 * with separate tenant administration and organization ADMIN memberships.
 * Returns: whether the atomic administration change committed
 */
gboolean venture_tenant_service_bootstrap_admin(VentureTenantService *self, VentureConfig *config,
	const gchar *username, const gchar *password, gboolean recover, const gchar *reason, GError **error);
/**
 * venture_tenant_service_is_member:
 * @self: service
 * @user_id: explicit local identity
 * @administrator: require the independent tenant-admin role
 * Returns: whether current durable membership grants the requested authority
 */
gboolean venture_tenant_service_is_member(VentureTenantService *self, gint64 user_id, gboolean administrator);
/**
 * venture_tenant_service_check_principal:
 * @self: service
 * @actor: (nullable): authenticated identity
 * @error: (out) (optional): refusal
 * Returns: whether the principal has current tenant authority
 */
gboolean venture_tenant_service_check_principal(VentureTenantService *self, const VentureAuthPrincipal *actor, GError **error);
#define VENTURE_TYPE_TENANT_MAINTENANCE (venture_tenant_maintenance_get_type())
G_DECLARE_FINAL_TYPE(VentureTenantMaintenance, venture_tenant_maintenance, VENTURE, TENANT_MAINTENANCE, GObject)
/**
 * venture_tenant_service_enter_maintenance:
 * @self: verified service
 * @reason: bounded required operator justification
 * @error: (out) (optional): refusal or failed audit
 *
 * Local, synchronous operator scope. Refuses a request principal, records a
 * durable begin, and temporarily permits maintenance while suspended. Never
 * hold this scope across a main-loop dispatch or external asynchronous call.
 * Returns: (transfer full) (nullable): scope; explicitly finish before releasing
 */
VentureTenantMaintenance *venture_tenant_service_enter_maintenance(VentureTenantService *self,
	const gchar *reason, GError **error);
/**
 * venture_tenant_maintenance_finish:
 * @self: operator scope
 * @error: (out) (optional): audit failure
 *
 * Records completion and restores the prior scope. Releasing without finishing
 * records abandonment; it does not turn partial work into a success.
 * Returns: whether completion was recorded
 */
gboolean venture_tenant_maintenance_finish(VentureTenantMaintenance *self, GError **error);
/**
 * venture_tenant_service_check_resource:
 * @self: service
 * @resource: explicitly classified report, action or provider resource
 * @write: starts writes or external effects
 * @error: (out) (optional): refusal
 * Returns: whether lifecycle and explicit resource authority permit the operation
 */
gboolean venture_tenant_service_check_resource(VentureTenantService *self, GObject *resource,
	gboolean write, GError **error);
/**
 * venture_tenant_service_set_state:
 * @self: service
 * @state: active, read_only or suspended
 * @reason: bounded required justification
 * @error: (out) (optional): refusal
 * Returns: whether the interactive tenant administrator's transition committed
 */
gboolean venture_tenant_service_set_state(VentureTenantService *self, const gchar *state,
	const gchar *reason, GError **error);
/**
 * venture_tenant_service_set_membership:
 * @self: service
 * @user_id: existing workspace member
 * @role: admin or member
 * @active: whether membership is active
 * @reason: bounded required justification
 * @error: (out) (optional): refusal
 *
 * Rechecks interactive administration, preserves the last active administrator,
 * and invalidates sessions and API credentials atomically with role changes.
 * Returns: whether the membership transition committed
 */
gboolean venture_tenant_service_set_membership(VentureTenantService *self, gint64 user_id,
	const gchar *role, gboolean active, const gchar *reason, GError **error);
/**
 * venture_tenant_service_invite:
 * @self: service
 * @role: admin or member
 * @organization_id: initial legal organization
 * @organization_role: explicit initial organization role
 * @lifetime_seconds: 60 through 604800 seconds
 * @reason: required bounded justification
 * @error: (out) (optional): refusal
 * Returns: (transfer full) (nullable): persisted invitation with one-time transient capability
 */
VentureTenantInvitation *venture_tenant_service_invite(VentureTenantService *self, const gchar *role,
	gint64 organization_id, VentureOrganizationRole organization_role, guint lifetime_seconds,
	const gchar *reason, GError **error);
/**
 * venture_tenant_service_invite_recovery:
 * @self: service
 * @user_id: explicitly reviewed retained ordinary member identity
 * @lifetime_seconds: 60 through 604800 seconds
 * @reason: required bounded justification
 * @error: (out) (optional): refusal
 *
 * Issues one-time recovery only for an inactive ordinary member whose password
 * was cleared by restore quarantine. Preserves identity, roles and references;
 * changes to the reviewed membership revoke the capability. Available to an
 * interactive administrator during suspension without activating business work.
 * Returns: (transfer full) (nullable): invitation with transient recovery capability
 */
VentureTenantInvitation *venture_tenant_service_invite_recovery(VentureTenantService *self,
	gint64 user_id, guint lifetime_seconds, const gchar *reason, GError **error);
/**
 * venture_tenant_service_accept_invitation:
 * @self: service
 * @config: trusted password policy
 * @capability: privately delivered invitation capability
 * @new_username: (nullable): unused username, or exact retained username for targeted recovery
 * @new_password: (nullable): policy-compliant new password
 * @error: (out) (optional): refusal
 *
 * An authenticated interactive caller accepts for itself and supplies neither
 * new-identity argument. Otherwise creates a new identity; existing usernames
 * and email guesses never select an identity. A targeted recovery capability instead
 * selects only its reviewed retained identity, resets MFA and its password, and
 * can be consumed while suspended without activating business work. Consumption and memberships commit
 * together, and changes to the issuing administrator revoke pending invitations.
 * Returns: (transfer full) (nullable): explicitly accepted local identity
 */
VentureUser *venture_tenant_service_accept_invitation(VentureTenantService *self, VentureConfig *config,
	const gchar *capability, const gchar *new_username, const gchar *new_password, GError **error);
#define VENTURE_TYPE_TENANT_SUPPORT_SCOPE (venture_tenant_support_scope_get_type())
G_DECLARE_FINAL_TYPE(VentureTenantSupportScope, venture_tenant_support_scope, VENTURE, TENANT_SUPPORT_SCOPE, GObject)
/**
 * venture_tenant_service_create_support_grant:
 * @self: verified service
 * @operator_user_id: active explicit local operator identity, without tenant membership
 * @organization_id: one verified legal organization
 * @allow_write: permit explicit scoped record repairs
 * @include_private: explicitly include private records within that organization
 * @emergency: allow bounded direct record access while suspended
 * @lifetime_seconds: 60 through 3600; emergencies at most 900
 * @reason: required bounded operator justification
 * @error: (out) (optional): refusal
 * Returns: (transfer full) (nullable): grant with one-time transient capability
 */
VentureTenantSupportGrant *venture_tenant_service_create_support_grant(VentureTenantService *self,
	gint64 operator_user_id, gint64 organization_id, gboolean allow_write, gboolean include_private,
	gboolean emergency, guint lifetime_seconds, const gchar *reason, GError **error);
/**
 * venture_tenant_service_enter_request:
 * @self: service
 * @actor: (nullable): independently authenticated caller
 * @capability: (nullable): privately supplied support capability
 * @resource: bounded request path, without query or secrets
 * @error: (out) (optional): invalid support authority
 *
 * Always clears any enclosing support authority. A capability additionally
 * requires its named authenticated operator, unexpired grant and one legal
 * organization. Its request audit commits before business access starts.
 * Returns: (transfer full) (nullable): nested request scope
 */
VentureTenantSupportScope *venture_tenant_service_enter_request(VentureTenantService *self,
	const VentureAuthPrincipal *actor, const gchar *capability, const gchar *resource, GError **error);
/**
 * venture_tenant_service_get_support_organization:
 * @self: service
 * Returns: current support organization, or zero when no support request exists
 */
gint64 venture_tenant_service_get_support_organization(VentureTenantService *self);
/**
 * venture_tenant_service_support_allows:
 * @self: service
 * @actor: (nullable): current independently authenticated principal
 * @entity: record being accessed
 * @write: whether access mutates the record
 * @require_emergency: require explicit emergency authority
 * @error: (out) (optional): refusal
 * Returns: whether the live grant authorizes this exact record and operation
 */
gboolean venture_tenant_service_support_allows(VentureTenantService *self, const VentureAuthPrincipal *actor,
	VentureEntity *entity, gboolean write, gboolean require_emergency, GError **error);
/**
 * venture_tenant_service_revoke_support:
 * @self: service
 * @grant_id: existing grant
 * @reason: required bounded reason
 * @error: (out) (optional): refusal
 * Returns: whether an interactive tenant administrator revoked the grant
 */
gboolean venture_tenant_service_revoke_support(VentureTenantService *self, gint64 grant_id,
	const gchar *reason, GError **error);
/**
 * venture_tenant_service_check_support_request:
 * @self: service
 * @actor: independently authenticated operator
 * @write: request writes records
 * @error: (out) (optional): refusal
 * Returns: whether live support authority permits admission; individual records still require exact checks
 */
gboolean venture_tenant_service_check_support_request(VentureTenantService *self,
	const VentureAuthPrincipal *actor, gboolean write, GError **error);
/**
 * venture_tenant_support_scope_set_control:
 * @self: current request scope
 * @control: verified route is an identity or workspace control surface
 *
 * Set from explicit route metadata before dispatch. Allows only identity
 * provider verification while business operations are suspended.
 */
void venture_tenant_support_scope_set_control(VentureTenantSupportScope *self, gboolean control);
/**
 * venture_tenant_service_check_provider:
 * @self: service
 * @provider: explicit provider kind
 * @error: (out) (optional): refusal
 * Returns: whether new provider work is allowed in the current lifecycle and request scope
 */
gboolean venture_tenant_service_check_provider(VentureTenantService *self, const gchar *provider, GError **error);
/**
 * venture_tenant_service_set_state_operator:
 * @self: verified hosted service
 * @state: active, read_only or suspended
 * @reason: bounded required local-operator reason
 * @error: (out) (optional): refusal
 * Returns: whether the audited offline lifecycle transition committed
 */
gboolean venture_tenant_service_set_state_operator(VentureTenantService *self, const gchar *state,
	const gchar *reason, GError **error);
/**
 * venture_tenant_service_status:
 * @self: verified hosted service
 * @error: (out) (optional): refusal
 * Returns: (transfer full) (nullable): non-secret identity and lifecycle JSON for authorized administration
 */
JsonNode *venture_tenant_service_status(VentureTenantService *self, GError **error);
/**
 * venture_tenant_service_verify_existing:
 * @self: configured service
 * @error: (out) (optional): identity mismatch or malformed existing schema
 *
 * Checks an existing binding before migrations or providers can write. An
 * absent table is permitted only as uninitialized startup awaiting migration.
 * Returns: whether existing database identity matches configured authority
 */
gboolean venture_tenant_service_verify_existing(VentureTenantService *self, GError **error);
/**
 * venture_tenant_service_enter_migration:
 * @self: configured service
 * @error: (out) (optional): refusal
 *
 * Explicit local startup scope: verifies existing identity first and permits
 * initial schema creation when no workspace table exists. Finish only after
 * initialization. The migration ledger covers failures before an audit table
 * exists; a completed initial migration also records its workspace event.
 * Returns: (transfer full) (nullable): synchronous startup migration scope
 */
VentureTenantMaintenance *venture_tenant_service_enter_migration(VentureTenantService *self, GError **error);
/**
 * venture_tenant_actions_register:
 * @database: service repository
 *
 * Registers classified lifecycle, membership, invitation and support-revocation
 * actions for the generic UI, REST API and command-line action surfaces.
 */
void venture_tenant_actions_register(VentureDatabase *database);
/**
 * venture_tenant_service_bootstrap_operator:
 * @self: verified hosted service
 * @config: trusted password policy
 * @username: explicit named operator identity
 * @password: one-time password supplied privately
 * @recover: explicitly reset existing credentials
 * @reason: bounded required local-operator justification
 * @error: (out) (optional): refusal
 *
 * Creates a VIEWER identity without tenant membership or business authority.
 * Refuses conversion of tenant members. Each support request additionally
 * requires a separately issued, scoped and expiring grant.
 * Returns: whether the audited local operation committed
 */
gboolean venture_tenant_service_bootstrap_operator(VentureTenantService *self, VentureConfig *config,
	const gchar *username, const gchar *password, gboolean recover, const gchar *reason, GError **error);
/**
 * venture_tenant_service_is_maintenance:
 * @self: service
 * Returns: whether a verified synchronous operator maintenance scope is active
 */
gboolean venture_tenant_service_is_maintenance(VentureTenantService *self);
/**
 * venture_tenant_service_revoke_credentials:
 * @self: verified hosted service
 * @reason: bounded required operator explanation
 * @error: (out) (optional): refusal or transactional failure
 *
 * Offline restore quarantine. Atomically suspends the workspace, disables all
 * user logins, clears local password verifiers, invalidates sessions and pending
 * invitations, revokes API/support capabilities and disables OIDC links. Retains
 * memberships and business data for explicit recovery review. Subsequent named
 * bootstrap recovery supplies fresh credentials; activation is a separate act.
 * Returns: whether durable quarantine committed and its maintenance audit finished
 */
gboolean venture_tenant_service_revoke_credentials(VentureTenantService *self,
	const gchar *reason, GError **error);
G_END_DECLS
#endif
