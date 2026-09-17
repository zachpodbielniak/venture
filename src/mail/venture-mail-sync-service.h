/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_MAIL_SYNC_SERVICE_H
#define VENTURE_MAIL_SYNC_SERVICE_H
#include "mail/venture-imap-client.h"
#include "mail/venture-mail-sync-records.h"
#include "mail/venture-mail-records.h"
#include "db/venture-database.h"
G_BEGIN_DECLS
#define VENTURE_TYPE_MAIL_SYNC_SERVICE (venture_mail_sync_service_get_type())
G_DECLARE_FINAL_TYPE(VentureMailSyncService, venture_mail_sync_service, VENTURE, MAIL_SYNC_SERVICE, GObject)
/**
 * venture_mail_sync_service_new:
 * @database: owning database; used only on its main thread
 * @client: IMAP transport; the socket client in production, the fake in tests
 * Returns: (transfer full): the inbound mail service
 */
VentureMailSyncService *venture_mail_sync_service_new(VentureDatabase *database, VentureImapClient *client);
/**
 * venture_mail_sync_service_sync:
 * @self: the service
 * @account: a saved mail_account
 * @actor: (nullable): audit actor
 * @error: (out) (optional): configuration, transport or persistence failure
 *
 * Fetches every UID above the stored per-folder high-water mark, files the
 * raw message as a document, records matched contacts' interactions,
 * unmatched senders and capture items, each message in its own transaction.
 * Returns: newly recorded messages, or -1
 */
gint venture_mail_sync_service_sync(VentureMailSyncService *self, VentureEntity *account, const VentureActor *actor, GError **error);
/**
 * venture_mail_sync_service_sweep:
 * @self: the service
 * @organization_id: exact owning organization
 * @limit: maximum accounts to sync
 * @actor: (nullable): audit actor
 * @error: (out) (optional): query failure; per-account failures are reported in the result
 * Returns: (transfer full) (nullable): {"accounts", "messages", "errors": [...]}
 */
JsonNode *venture_mail_sync_service_sweep(VentureMailSyncService *self, gint64 organization_id, guint limit, const VentureActor *actor, GError **error);
/**
 * venture_mail_sync_service_create_contact:
 * @self: the service
 * @sender: an unmatched sender row
 * @actor: (nullable): audit actor
 * @error: (out) (optional): failure
 * Returns: (transfer full) (nullable): the new contact; the sender row is removed in the same transaction
 */
VentureEntity *venture_mail_sync_service_create_contact(VentureMailSyncService *self, VentureEntity *sender, const VentureActor *actor, GError **error);
/**
 * venture_mail_sync_record_outbound:
 * @database: the database
 * @message: a mail_message that the relay just accepted
 * @error: (out) (optional): persistence failure
 *
 * Called by the outbox after a successful submission so the recipient
 * contact's timeline shows the outbound side. Unknown recipients record nothing.
 * Returns: whether recording succeeded or was not needed
 */
gboolean venture_mail_sync_record_outbound(VentureDatabase *database, VentureMailMessage *message, GError **error);
G_END_DECLS
#endif
