/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_MAIL_SYNC_SERVICE_H
#define VENTURE_MAIL_SYNC_SERVICE_H
#include "mail/venture-imap-client.h"
#include "mail/venture-mail-sync-records.h"
#include "mail/venture-mail-records.h"
#include "db/venture-database.h"
G_BEGIN_DECLS
/**
 * VENTURE_MAIL_SYNC_MAX_ATTEMPTS:
 *
 * How many syncs in a row one message may fail for a reason that is its
 * own (it will not parse, a save refuses it) before it is filed as a stub
 * with a skip reason and the folder moves past it.
 */
#define VENTURE_MAIL_SYNC_MAX_ATTEMPTS 3
/**
 * VENTURE_MAIL_SYNC_AUTH_FAILURE_LIMIT:
 *
 * Consecutive failed syncs, the last a refused login, that switch an
 * account off and tell the admins; retrying a wrong password every minute
 * is how a provider locks the mailbox.
 */
#define VENTURE_MAIL_SYNC_AUTH_FAILURE_LIMIT 5
#define VENTURE_TYPE_MAIL_SYNC_SERVICE (venture_mail_sync_service_get_type())
G_DECLARE_FINAL_TYPE(VentureMailSyncService, venture_mail_sync_service, VENTURE, MAIL_SYNC_SERVICE, GObject)
/**
 * venture_mail_sync_service_new:
 * @database: owning database; used only on its main thread
 * @client: IMAP transport; the socket client in production, the fake in tests
 *
 * Properties: "attachment-root" (where raw messages and attachments are
 * filed), "message-budget" (messages one call processes, default 200),
 * "time-budget" (seconds one call may take, default 45) and "context"
 * (optional; lets a switched-off account notify the admins).
 * Returns: (transfer full): the inbound mail service
 */
VentureMailSyncService *venture_mail_sync_service_new(VentureDatabase *database, VentureImapClient *client);
/**
 * venture_mail_sync_service_new_for_context:
 * @context: the running install
 *
 * The production wiring every surface shares: a socket client, the
 * state directory's attachments/ folder and the context for notifications.
 * Returns: (transfer full): the inbound mail service
 */
VentureMailSyncService *venture_mail_sync_service_new_for_context(VentureContext *context);
/**
 * venture_mail_sync_service_sync:
 * @self: the service
 * @account: a saved mail_account; updated in place with the outcome
 * @actor: (nullable): audit actor
 * @error: (out) (optional): configuration, transport, folder or message failure
 *
 * Syncs one account now, ignoring its backoff but not another caller's
 * lease. Each watched folder is read above its stored high-water UID, at
 * most the message budget and within the time budget; each message is
 * filed in its own transaction, so what was read stays committed and the
 * next call resumes. A refused folder is recorded and the next folder
 * still syncs.
 * Returns: messages filed, or -1 when anything failed (the count of what
 *   was filed before is then only on the account)
 */
gint venture_mail_sync_service_sync(VentureMailSyncService *self, VentureEntity *account, const VentureActor *actor, GError **error);
/**
 * venture_mail_sync_service_sync_now:
 * @self: the service
 * @account: a saved mail_account; updated in place with the outcome
 * @actor: (nullable): audit actor
 * @error: (out) (optional): refusal before the account was reached
 *
 * venture_mail_sync_service_sync() reported the way a sweep reports, for
 * the per-account route and command.
 * Returns: (transfer full) (nullable): {"accounts", "messages", "skipped", "deferred", "errors": [...]}
 */
JsonNode *venture_mail_sync_service_sync_now(VentureMailSyncService *self, VentureEntity *account, const VentureActor *actor, GError **error);
/**
 * venture_mail_sync_service_sweep:
 * @self: the service
 * @organization_id: exact owning organization
 * @limit: maximum accounts to sync
 * @actor: (nullable): audit actor
 * @error: (out) (optional): query failure; per-account failures are reported in the result
 *
 * Syncs the organization's active accounts that are not backing off, one
 * after another, sharing one message budget and one time budget.
 * Returns: (transfer full) (nullable): {"accounts", "messages", "skipped", "deferred", "errors": [...]}
 */
JsonNode *venture_mail_sync_service_sweep(VentureMailSyncService *self, gint64 organization_id, guint limit, const VentureActor *actor, GError **error);
/**
 * venture_mail_sync_service_create_contact:
 * @self: the service
 * @sender: an unmatched sender row
 * @actor: (nullable): audit actor
 * @error: (out) (optional): failure
 *
 * Creates the contact, removes the sender row and records interactions for
 * the mail already filed from that address, in one transaction.
 * Returns: (transfer full) (nullable): the new contact
 */
VentureEntity *venture_mail_sync_service_create_contact(VentureMailSyncService *self, VentureEntity *sender, const VentureActor *actor, GError **error);
/**
 * venture_mail_sync_service_dismiss:
 * @self: the service
 * @sender: an unmatched sender row
 * @actor: (nullable): audit actor
 * @error: (out) (optional): failure
 *
 * Marks the address dismissed. The row stays as an ignore-list entry, so
 * the next message from it neither lists it again nor collides with it.
 * Returns: (transfer full) (nullable): the updated row
 */
VentureEntity *venture_mail_sync_service_dismiss(VentureMailSyncService *self, VentureEntity *sender, const VentureActor *actor, GError **error);
/**
 * venture_mail_sync_record_outbound:
 * @database: the database
 * @message: a mail_message whose sent state is already committed
 * @error: (out) (optional): persistence failure
 *
 * Called by the outbox after a successful submission so the recipient
 * contact's timeline shows the outbound side, in its own transaction. The
 * Message-ID is kept on a mail_inbound row when the organization syncs
 * mail, so the copy in a watched Sent folder is not filed twice and a
 * reply threads to it. Unknown recipients record no interaction.
 * Returns: whether recording succeeded or was not needed
 */
gboolean venture_mail_sync_record_outbound(VentureDatabase *database, VentureMailMessage *message, GError **error);
/**
 * venture_mail_sync_install_validators:
 * @database: the database
 *
 * Registers the mail_account save validator that refuses a capture
 * address the account's own mail would match.
 */
void venture_mail_sync_install_validators(VentureDatabase *database);
G_END_DECLS
#endif
