/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_CONNECTOR_H
#define VENTURE_CONNECTOR_H
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include "venture-types.h"
#include "model/venture-integration-connection.h"
G_BEGIN_DECLS
#define VENTURE_TYPE_CONNECTOR_SESSION (venture_connector_session_get_type())
G_DECLARE_FINAL_TYPE(VentureConnectorSession, venture_connector_session, VENTURE, CONNECTOR_SESSION, GObject)
/**
 * venture_connector_settings_schema:
 *
 * Returns: (transfer full): write-only password settings for the existing
 *   IMAP and CalDAV transports; endpoint identity stays on their account
 */
JsonNode *venture_connector_settings_schema(void);
/**
 * venture_connector_binding_key:
 * @account: a persisted mail_account or calendar_account
 *
 * Returns: (transfer full) (nullable): provider namespace derived from the
 *   account UUID, or NULL for an unsupported account
 */
gchar *venture_connector_binding_key(VentureEntity *account);
/**
 * venture_connector_can_manage:
 * @database: repository whose active principal is checked
 * @account: selected account
 * @error: (out) (optional): refusal
 *
 * An assigned private owner may manage their account. Shared accounts require
 * organization administration. Internal maintenance uses a trusted scope.
 * Returns: whether the active principal may manage these credentials
 */
gboolean venture_connector_can_manage(VentureDatabase *database, VentureEntity *account, GError **error);
/**
 * venture_connector_install:
 * @database: repository
 *
 * Installs account identity and delegation validators for every writer.
 */
void venture_connector_install(VentureDatabase *database);
/**
 * venture_connector_configure:
 * @database: repository owning the account
 * @config: operator endpoint policy
 * @account: saved account, re-read before use
 * @settings: password object; copied and encrypted
 * @expected_binding: current integration ID, or zero when unconfigured
 * @expected_version: current integration version, or zero when unconfigured
 * @actor: (nullable): audit attribution
 * @error: (out) (optional): configuration or authorization failure
 *
 * Validates locally without connecting. Existing account identity must match
 * the retained binding; a different source requires a new account.
 * Returns: (transfer full) (nullable): retained integration evidence
 */
VentureIntegrationConnection *venture_connector_configure(VentureDatabase *database,
	VentureConfig *config, VentureEntity *account, JsonObject *settings,
	gint64 expected_binding, gint64 expected_version, const VentureActor *actor, GError **error);
/**
 * venture_connector_disconnect:
 * @database: repository owning the account
 * @account: saved account
 * @expected_binding: exact current integration ID
 * @expected_version: exact current integration version
 * @actor: (nullable): audit attribution
 * @error: (out) (optional): failure
 *
 * Returns: whether the account's current binding was disabled
 */
gboolean venture_connector_disconnect(VentureDatabase *database, VentureEntity *account,
	gint64 expected_binding, gint64 expected_version, const VentureActor *actor, GError **error);
/**
 * venture_connector_open:
 * @database: repository owning the account
 * @config: operator endpoint policy
 * @account: saved account, re-read before use
 * @error: (out) (optional): failure
 *
 * Creates an immutable credential/account snapshot on the database-owning
 * thread. This does not open a network connection. Missing configuration
 * never resolves a process environment password.
 * Returns: (transfer full) (nullable): account-bound snapshot
 */
VentureConnectorSession *venture_connector_open(VentureDatabase *database,
	VentureConfig *config, VentureEntity *account, GError **error);
/**
 * venture_connector_session_get_account:
 * @self: snapshot
 *
 * Identity and selection must not be modified. Consumers may stamp derived
 * sync status on this snapshot before saving it back to the repository.
 * Returns: (transfer none): selected account snapshot
 */
VentureEntity *venture_connector_session_get_account(VentureConnectorSession *self);
/**
 * venture_connector_session_get_password:
 * @self: snapshot
 *
 * The borrowed secret must never be logged, rendered or retained past @self.
 * Returns: (transfer none): credential for the selected transport
 */
const gchar *venture_connector_session_get_password(VentureConnectorSession *self);
/**
 * venture_connector_session_validate:
 * @self: snapshot
 * @error: (out) (optional): revoked or changed configuration
 *
 * Re-read identity, endpoint policy and credential version on the owning
 * thread before external writes and before applying downloaded results.
 * Returns: whether the snapshot remains authorized
 */
gboolean venture_connector_session_validate(VentureConnectorSession *self, GError **error);
G_END_DECLS
#endif
