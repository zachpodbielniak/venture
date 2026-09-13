/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_ACCESS_POLICY_H
#define VENTURE_ACCESS_POLICY_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
#define VENTURE_TYPE_ACCESS_POLICY (venture_access_policy_get_type())
G_DECLARE_FINAL_TYPE(VentureAccessPolicy, venture_access_policy, VENTURE, ACCESS_POLICY, GObject)
#define VENTURE_TYPE_ACCESS_SCOPE (venture_access_scope_get_type())
G_DECLARE_FINAL_TYPE(VentureAccessScope, venture_access_scope, VENTURE, ACCESS_SCOPE, GObject)
/**
 * venture_database_get_access_policy:
 * @database: the repository
 * Returns: (transfer none): the repository's canonical access policy
 */
VentureAccessPolicy *venture_database_get_access_policy(VentureDatabase *database);
/**
 * venture_access_policy_new:
 * @database: the repository, held weakly
 * Returns: (transfer full): a policy
 */
VentureAccessPolicy *venture_access_policy_new(VentureDatabase *database);
/**
 * venture_access_policy_can:
 * @self: the policy
 * @actor: authenticated principal
 * @action: read, write, delete or export
 * @entity: the record or proposed record
 * @error: return location for refusal
 * Returns: whether the operation is authorized
 */
gboolean venture_access_policy_can(VentureAccessPolicy *self, const VentureAuthPrincipal *actor, const gchar *action, VentureEntity *entity, GError **error);
/**
 * venture_access_policy_requires_approval:
 * @self: the policy
 * @actor: authenticated principal
 * @action: write or post
 * @entity: proposed record
 * @error: return location for refusal
 * Returns: whether this operation may be proposed but not applied
 */
gboolean venture_access_policy_requires_approval(VentureAccessPolicy *self, const VentureAuthPrincipal *actor, const gchar *action, VentureEntity *entity, GError **error);
/**
 * venture_access_policy_enter:
 * @self: the policy
 * @actor: (nullable): principal to copy; NULL is trusted internal work
 * Returns: (transfer full): synchronous scope; release in reverse entry order
 */
VentureAccessScope *venture_access_policy_enter(VentureAccessPolicy *self, const VentureAuthPrincipal *actor);
/**
 * venture_access_policy_get_actor:
 * @self: the policy
 * Returns: (transfer none) (nullable): principal of the current synchronous scope
 */
const VentureAuthPrincipal *venture_access_policy_get_actor(VentureAccessPolicy *self);
/**
 * venture_access_policy_has_membership:
 * @self: the policy
 * @actor: the authenticated user
 * Returns: whether at least one active membership exists, or global administration applies
 */
gboolean venture_access_policy_has_membership(VentureAccessPolicy *self, const VentureAuthPrincipal *actor);
/**
 * venture_access_policy_find:
 * @self: the policy
 * @query: query, whose filters and pagination are preserved
 * @error: return location for failure
 * Returns: (transfer full) (element-type VentureEntity): authorized rows
 */
GPtrArray *venture_access_policy_find(VentureAccessPolicy *self, VentureQuery *query, GError **error);
/**
 * venture_access_policy_check_write:
 * @self: the policy
 * @entity: proposed record
 * @action: write or delete
 * @error: return location for refusal
 * Returns: whether both old and new organization/ownership permit the write
 */
gboolean venture_access_policy_check_write(VentureAccessPolicy *self, VentureEntity *entity, const gchar *action, GError **error);
/**
 * venture_access_policy_check_read:
 * @self: policy
 * @entity: stored row
 * @error: refusal
 * Returns: whether the current scope can read the row
 */
gboolean venture_access_policy_check_read(VentureAccessPolicy *self, VentureEntity *entity, GError **error);
/**
 * venture_access_policy_count:
 * @self: policy
 * @query: query
 * @error: failure
 * Returns: authorized count, ignoring pagination, or -1
 */
gint64 venture_access_policy_count(VentureAccessPolicy *self, VentureQuery *query, GError **error);
/**
 * venture_orgaccess_web_dispatch:
 * @auth: existing authenticator
 * @context: application wiring
 * @http: request context
 * @next: next middleware
 * @next_data: middleware data
 *
 * Carries a copied principal for the synchronous request dispatch.
 */
void venture_orgaccess_web_dispatch(VentureAuth *auth, VentureContext *context, HtmxContext *http, HtmxMiddlewareNext next, gpointer next_data);
/**
 * venture_orgaccess_bootstrap_owner:
 * @database: repository
 * @user: unsaved initial owner
 * @error: failure
 * Returns: whether the user and all owner memberships committed together
 */
gboolean venture_orgaccess_bootstrap_owner(VentureDatabase *database, VentureUser *user, GError **error);
/**
 * venture_orgaccess_prepare:
 * @database: repository
 * @entity: proposed record
 * @error: failure
 * Returns: whether membership and ownership references are consistent
 */
gboolean venture_orgaccess_prepare(VentureDatabase *database, VentureEntity *entity, GError **error);
/**
 * venture_orgaccess_enter_ai:
 * @context: application wiring
 * @principal: (nullable): caller; absence denies access
 * Returns: (transfer full): synchronous tool scope
 */
VentureAccessScope *venture_orgaccess_enter_ai(VentureContext *context, const VentureAuthPrincipal *principal);
/**
 * venture_orgaccess_check_proposal:
 * @database: repository
 * @staged: proposed record
 * @action: audit operation
 * @via: trusted surface name
 * @error: refusal
 * Returns: whether the caller may stage this proposal
 */
gboolean venture_orgaccess_check_proposal(VentureDatabase *database, VentureEntity *staged, VentureAuditAction action, const gchar *via, GError **error);
/**
 * venture_orgaccess_post_journal:
 * @context: wiring
 * @principal: authenticated actor
 * @id: draft journal
 * @staged_out: whether a confirmation was created
 * @error: refusal
 * Returns: (transfer full) (nullable): a confirmation or posted record
 */
JsonNode *venture_orgaccess_post_journal(VentureContext *context, const VentureAuthPrincipal *principal, gint64 id, gboolean *staged_out, GError **error);
/**
 * venture_orgaccess_apply_post:
 * @database: repository
 * @original: captured draft header
 * @via: captured line fingerprint
 * @actor: audit origin and approver
 * @error: refusal
 * Returns: whether posting committed with unchanged evidence
 */
gboolean venture_orgaccess_apply_post(VentureDatabase *database, VentureEntity *original, const gchar *via, const VentureActor *actor, GError **error);
/**
 * venture_orgaccess_web_post:
 * @auth: authenticator
 * @context: wiring
 * @request: HTTP request
 * @params: route parameters
 * Returns: (transfer full): journal action response
 */
HtmxResponse *venture_orgaccess_web_post(VentureAuth *auth, VentureContext *context, HtmxRequest *request, GHashTable *params);
/**
 * venture_orgaccess_confirmation_visible:
 * @database: repository
 * @staged: proposal
 * @proposer: authenticated originating user id, or zero for internal work
 * @via: trusted surface name
 * Returns: whether the current principal may see the confirmation
 */
gboolean venture_orgaccess_confirmation_visible(VentureDatabase *database, VentureEntity *staged, gint64 proposer, const gchar *via);
/**
 * venture_orgaccess_limit_token:
 * @auth: existing role ordering
 * @database: unscoped authentication repository
 * @principal: token principal whose role may be reduced
 * Returns: whether its minting user remains active
 */
gboolean venture_orgaccess_limit_token(VentureAuth *auth, VentureDatabase *database, VentureAuthPrincipal *principal);
G_END_DECLS
#endif
