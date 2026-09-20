/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_OIDC_SERVICE_H
#define VENTURE_OIDC_SERVICE_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
#define VENTURE_TYPE_OIDC_SERVICE (venture_oidc_service_get_type())
G_DECLARE_FINAL_TYPE(VentureOidcService, venture_oidc_service, VENTURE, OIDC_SERVICE, GObject)
/**
 * venture_oidc_service_get:
 * @database: repository, weakly held
 * Returns: (transfer none): repository-owned sign-in service
 */
VentureOidcService *venture_oidc_service_get(VentureDatabase *database);
/**
 * venture_oidc_service_set_config:
 * @self: service
 * @config: live platform policy, including exact allowed issuers and base URL
 */
void venture_oidc_service_set_config(VentureOidcService *self, VentureConfig *config);
/**
 * venture_oidc_service_connect:
 * @self: service
 * @organization_id: verified local organization
 * @issuer: exact platform-approved OIDC issuer
 * @client_id: registered confidential client
 * @client_secret: write-only client credential
 * @expected_version: 0 for creation, current connection version for rotation
 * @actor: (nullable): audit attribution; current access scope authorizes the write
 * @error: (out) (optional): redacted configuration failure
 * Returns: (transfer full) (nullable): encrypted organization provider binding
 */
VentureIntegrationConnection *venture_oidc_service_connect(VentureOidcService *self, gint64 organization_id,
	const gchar *issuer, const gchar *client_id, const gchar *client_secret, gint64 expected_version,
	const VentureActor *actor, GError **error);
/**
 * venture_oidc_service_begin:
 * @self: service
 * @provider_uuid: exact connection UUID from the organization's sign-in link
 * @local_user_id: authenticated local user when linking, or 0 for sign-in
 * @password: (nullable): current local password required only for linking
 * @remote_address: actual peer address, used only for a bounded attempt budget
 * @out_browser_token: (out) (transfer full): browser binding for a secure HttpOnly cookie
 * @error: (out) (optional): redacted refusal
 *
 * Creates a five-minute, one-use state/nonce/S256 attempt. It creates no user
 * or identity. Restart discards pending attempts; the browser starts again.
 * Returns: (transfer full) (nullable): pinned provider authorization URL
 */
gchar *venture_oidc_service_begin(VentureOidcService *self, const gchar *provider_uuid,
	gint64 local_user_id, const gchar *password, const gchar *remote_address,
	gchar **out_browser_token, GError **error);
/**
 * venture_oidc_service_finish:
 * @self: service
 * @state: authorization response state
 * @code: authorization code, never persisted or logged
 * @browser_token: bound browser cookie
 * @local_user_id: currently authenticated local user, or 0
 * @error: (out) (optional): generic identity refusal
 *
 * Consumes the attempt, exchanges the code with PKCE, verifies the ID token,
 * and either resolves an existing link or explicitly links the same local
 * account that authenticated at begin. Claims never grant roles or membership.
 * Returns: (transfer full) (nullable): verified active identity for local authentication
 */
VentureOidcIdentity *venture_oidc_service_finish(VentureOidcService *self, const gchar *state,
	const gchar *code, const gchar *browser_token, gint64 local_user_id, GError **error);
/**
 * venture_oidc_service_identity_user:
 * @self: service
 * @identity_id: identity from a verified signed session/challenge or finish result
 * @error: (out) (optional): revoked identity/provider/membership/account
 *
 * Trusted authentication boundary. Reloads local authority on every call;
 * a caller-controlled identity ID is never sufficient to authenticate.
 * Returns: (transfer full) (nullable): current local user
 */
VentureUser *venture_oidc_service_identity_user(VentureOidcService *self, gint64 identity_id, GError **error);
/**
 * venture_oidc_service_dup_session_binding:
 * @self: service
 * @identity_id: linked identity
 * @error: (out) (optional): missing or revoked binding
 *
 * Supplies current identity and provider revisions for the session MAC.
 * Rotation, unlink/relink and disconnect/reconnect cannot revive old cookies.
 * This value is not a credential and grants no authority by itself.
 * Returns: (transfer full) (nullable): current revision binding
 */
gchar *venture_oidc_service_dup_session_binding(VentureOidcService *self, gint64 identity_id, GError **error);
/**
 * venture_oidc_service_unlink:
 * @self: service
 * @identity_id: identity to unlink
 * @local_user_id: authenticated local user; only their own link is allowed
 * @password: current local password, preserving independent recovery
 * @actor: (nullable): audit attribution
 * @error: (out) (optional): refusal
 * Returns: whether the link and existing user sessions were revoked atomically
 */
gboolean venture_oidc_service_unlink(VentureOidcService *self, gint64 identity_id, gint64 local_user_id,
	const gchar *password, const VentureActor *actor, GError **error);
/**
 * venture_oidc_service_callback_uri:
 * @self: service
 * @error: (out) (optional): unsafe or missing platform base URL
 * Returns: (transfer full) (nullable): exact registered redirect URI
 */
gchar *venture_oidc_service_callback_uri(VentureOidcService *self, GError **error);
/**
 * venture_oidc_check_write:
 * @database: repository
 * @entity: proposed record
 * @removal: deletion, restoration or purge
 * @error: (out) (optional): refusal
 * Returns: whether the lifecycle write is service-authorized
 */
gboolean venture_oidc_check_write(VentureDatabase *database, VentureEntity *entity, gboolean removal, GError **error);
G_END_DECLS
#endif
