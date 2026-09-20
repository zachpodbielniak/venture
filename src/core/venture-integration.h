/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_INTEGRATION_H
#define VENTURE_INTEGRATION_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
#define VENTURE_TYPE_INTEGRATION_SERVICE (venture_integration_service_get_type())
G_DECLARE_FINAL_TYPE(VentureIntegrationService, venture_integration_service, VENTURE, INTEGRATION_SERVICE, GObject)
/**
 * venture_integration_service_get:
 * @database: repository, weakly held
 * Returns: (transfer none): repository-owned integration service
 */
VentureIntegrationService *venture_integration_service_get(VentureDatabase *database);
/**
 * venture_integration_service_set_key:
 * @self: service
 * @key: exactly 32 cryptographically random bytes, provided by the platform
 * @error: (out) (optional): failure
 *
 * Installs the process key at startup. Never call this to rotate an existing
 * install: restoring a database requires its original key. A different key
 * cannot replace an already installed key. Key material is never serialized.
 * Returns: whether the key was accepted
 */
gboolean venture_integration_service_set_key(VentureIntegrationService *self, GBytes *key, GError **error);
/**
 * venture_integration_service_configure:
 * @self: service
 * @organization_id: verified business organization
 * @provider: stable provider identifier
 * @account_id: safe remote account identity
 * @environment: test or live
 * @settings: credential object; copied into authenticated ciphertext
 * @expected_version: 0 to create; current row version to rotate the same binding
 * @actor: (nullable): audit actor; authority comes from the current access scope
 * @error: (out) (optional): redacted refusal
 *
 * Organization owners/admins can connect or rotate their own account. Account
 * replacement requires disabling the old binding first. No ambient defaults
 * are read. Provider adapters validate their settings before calling this.
 * Returns: (transfer full) (nullable): persisted binding, never clear credentials
 */
VentureIntegrationConnection *venture_integration_service_configure(VentureIntegrationService *self,
	gint64 organization_id, const gchar *provider, const gchar *account_id, const gchar *environment,
	JsonNode *settings, gint64 expected_version, const VentureActor *actor, GError **error);
/**
 * venture_integration_service_find:
 * @self: service
 * @organization_id: verified organization
 * @provider: provider identifier
 * @error: (out) (optional): unavailable or storage failure
 * Returns: (transfer full) (nullable): active binding; absent configuration fails closed
 */
VentureIntegrationConnection *venture_integration_service_find(VentureIntegrationService *self,
	gint64 organization_id, const gchar *provider, GError **error);
/**
 * venture_integration_service_resolve:
 * @self: service
 * @organization_id: expected organization
 * @connection_id: exact historical account binding
 * @allow_disabled: whether a verified callback may use a disabled historical binding
 * @error: (out) (optional): redacted refusal
 *
 * Trusted provider-adapter boundary. Never expose this result on HTTP, CLI,
 * tools, logs or audit. Fetches the current row on every call so rotation and
 * disconnect take effect immediately. A caller cannot move ciphertext to a
 * different organization, account, environment or record.
 * Returns: (transfer full) (nullable): decrypted credential object
 */
JsonNode *venture_integration_service_resolve(VentureIntegrationService *self,
	gint64 organization_id, gint64 connection_id, gboolean allow_disabled, GError **error);
/**
 * venture_integration_service_disable:
 * @self: service
 * @organization_id: verified organization
 * @connection_id: binding to disconnect
 * @expected_version: version displayed in settings
 * @actor: (nullable): audit actor
 * @error: (out) (optional): refusal
 * Returns: whether the binding was disabled; historical identity is retained
 */
gboolean venture_integration_service_disable(VentureIntegrationService *self, gint64 organization_id,
	gint64 connection_id, gint64 expected_version, const VentureActor *actor, GError **error);
/**
 * venture_integration_check_write:
 * @database: repository
 * @entity: proposed record
 * @removal: deletion, restoration or purge
 * @error: (out) (optional): refusal
 * Returns: whether this lifecycle write is service-authorized
 */
gboolean venture_integration_check_write(VentureDatabase *database, VentureEntity *entity,
	gboolean removal, GError **error);
G_END_DECLS
#endif
