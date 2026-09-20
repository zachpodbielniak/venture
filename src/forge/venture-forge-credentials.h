/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_FORGE_CREDENTIALS_H
#define VENTURE_FORGE_CREDENTIALS_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
#define VENTURE_TYPE_FORGE_CREDENTIALS (venture_forge_credentials_get_type())
G_DECLARE_FINAL_TYPE(VentureForgeCredentials, venture_forge_credentials, VENTURE, FORGE_CREDENTIALS, GObject)
/**
 * venture_forge_credentials_acquire:
 * @database: repository, accessed only by the calling main thread
 * @forge_id: explicit persisted forge
 * @error: (out) (optional): refusal
 *
 * Resolves one enabled encrypted binding. The returned immutable snapshot may
 * cross threads; workers inspect only its cancellable, never the repository.
 * Main-thread changes conservatively revoke snapshots even if rolled back.
 * Returns: (transfer full) (nullable): revocable credential snapshot
 */
VentureForgeCredentials *venture_forge_credentials_acquire(VentureDatabase *database, gint64 forge_id, GError **error);
/**
 * venture_forge_credentials_check:
 * @self: snapshot
 * @error: (out) (optional): revoked credential refusal
 * Returns: whether this snapshot is still usable; safe on a worker
 */
gboolean venture_forge_credentials_check(VentureForgeCredentials *self, GError **error);
/**
 * venture_forge_credentials_revalidate:
 * @self: snapshot
 * @database: original repository; main thread only
 * @error: (out) (optional): refusal
 * Returns: whether retained record versions still match before applying results
 */
gboolean venture_forge_credentials_revalidate(VentureForgeCredentials *self, VentureDatabase *database, GError **error);
/**
 * venture_forge_credentials_get_cancellable:
 * @self: snapshot
 * Returns: (transfer none): thread-safe revocation signal; never reset it
 */
GCancellable *venture_forge_credentials_get_cancellable(VentureForgeCredentials *self);
/**
 * venture_forge_credentials_get_token:
 * @self: snapshot
 * Returns: (transfer none): trusted adapter-only secret; never serialize or log
 */
const gchar *venture_forge_credentials_get_token(VentureForgeCredentials *self);
/**
 * venture_forge_credentials_get_webhook_secret:
 * @self: snapshot
 * Returns: (transfer none): trusted adapter-only secret; never serialize or log
 */
const gchar *venture_forge_credentials_get_webhook_secret(VentureForgeCredentials *self);
/**
 * venture_forge_credentials_get_base_url:
 * @self: snapshot
 * Returns: (transfer none): immutable approved API origin and base path
 */
const gchar *venture_forge_credentials_get_base_url(VentureForgeCredentials *self);
/**
 * venture_forge_credentials_check_clone:
 * @self: snapshot
 * @url: resolved clone URL
 * @error: (out) (optional): refusal
 *
 * HTTPS credentials are restricted to the approved API authority. SSH URLs
 * and scp-like addresses use the operator's key agent and receive no token.
 * Returns: whether this transport is allowed
 */
gboolean venture_forge_credentials_check_clone(VentureForgeCredentials *self, const gchar *url, GError **error);
/**
 * venture_forge_settings_schema:
 * Returns: (transfer full): shared write-only settings schema
 */
JsonNode *venture_forge_settings_schema(void);
/**
 * venture_forge_settings_configure:
 * @database: repository
 * @forge_id: explicit forge
 * @settings: schema-validated credentials, copied into encrypted storage
 * @expected_version: zero for create, current binding version for rotation
 * @expected_connection: zero for create, exact binding to rotate
 * @actor: (nullable): audit identity
 * @error: (out) (optional): failure
 *
 * Verifies the token's account before retaining credentials. Rotation cannot
 * change the account or origin. Administrative authority is required.
 * Returns: (transfer full) (nullable): encrypted binding
 */
VentureIntegrationConnection *venture_forge_settings_configure(VentureDatabase *database, gint64 forge_id, JsonNode *settings, gint64 expected_version, gint64 expected_connection, const VentureActor *actor, GError **error);
/**
 * venture_forge_settings_find:
 * @database: repository
 * @forge_id: explicit forge
 * @error: (out) (optional): missing or invalid configuration
 * Returns: (transfer full) (nullable): current encrypted binding metadata
 */
VentureIntegrationConnection *venture_forge_settings_find(VentureDatabase *database, gint64 forge_id, GError **error);
/**
 * venture_forge_settings_disconnect:
 * @database: repository
 * @forge_id: explicit forge
 * @expected_version: current binding version
 * @expected_connection: exact binding to disconnect
 * @actor: (nullable): audit identity
 * @error: (out) (optional): refusal
 * Returns: whether that exact binding was disabled
 */
gboolean venture_forge_settings_disconnect(VentureDatabase *database, gint64 forge_id, gint64 expected_version, gint64 expected_connection, const VentureActor *actor, GError **error);
/**
 * venture_forge_settings_import_legacy:
 * @database: repository
 * @forge_id: explicit forge with legacy plaintext
 * @actor: (nullable): audit identity
 * @error: (out) (optional): refusal
 *
 * Explicitly verifies, encrypts and clears legacy plaintext in one independent
 * transaction. Old backups still contain plaintext and need operator retention.
 * Returns: whether import committed
 */
gboolean venture_forge_settings_import_legacy(VentureDatabase *database, gint64 forge_id, const VentureActor *actor, GError **error);
/**
 * venture_forge_client_for_database:
 * @database: repository
 * @forge_id: explicit persisted forge
 * @timeout_seconds: request timeout
 * @error: (out) (optional): failure
 * Returns: (transfer full) (nullable): client bound to an encrypted configuration
 */
VentureForgeClient *venture_forge_client_for_database(VentureDatabase *database, gint64 forge_id, gint timeout_seconds, GError **error);
/**
 * venture_forge_client_get_credentials:
 * @client: client
 * Returns: (transfer none) (nullable): bound snapshot, absent on explicit-token test clients
 */
VentureForgeCredentials *venture_forge_client_get_credentials(VentureForgeClient *client);
/**
 * venture_forge_client_check_credentials:
 * @client: client
 * @error: (out) (optional): refusal
 * Returns: whether bound credentials remain live, or explicit test injection applies
 */
gboolean venture_forge_client_check_credentials(VentureForgeClient *client, GError **error);
/**
 * venture_forge_credentials_get_account:
 * @self: snapshot
 * Returns: (transfer none): verified immutable provider login
 */
const gchar *venture_forge_credentials_get_account(VentureForgeCredentials *self);
/**
 * venture_forge_settings_apply:
 * @database: repository
 * @forge_id: explicit persisted forge
 * @request: operation, exact connection_id/version, and optional settings object
 * @actor: (nullable): audit identity
 * @error: (out) (optional): refusal
 *
 * Shared UI/API operation dispatcher. Plaintext never appears in the result.
 * Returns: (transfer full) (nullable): connection metadata on success
 */
JsonNode *venture_forge_settings_apply(VentureDatabase *database, gint64 forge_id,
	JsonNode *request, const VentureActor *actor, GError **error);
/**
 * venture_forge_check_write:
 * @database: repository
 * @entity: proposed record
 * @removal: deletion, restore or purge
 * @error: (out) (optional): refusal
 * Returns: whether the write preserves encrypted forge identity and legacy evidence
 */
gboolean venture_forge_check_write(VentureDatabase *database, VentureEntity *entity, gboolean removal, GError **error);
/**
 * venture_forge_credentials_get_connection_id:
 * @self: snapshot
 * Returns: exact encrypted binding identity for retained run evidence
 */
gint64 venture_forge_credentials_get_connection_id(VentureForgeCredentials *self);
/**
 * venture_forge_credentials_get_connection_version:
 * @self: snapshot
 * Returns: exact credential version for retained run evidence
 */
gint64 venture_forge_credentials_get_connection_version(VentureForgeCredentials *self);
G_END_DECLS
#endif
