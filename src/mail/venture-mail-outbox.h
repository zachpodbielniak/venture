/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_MAIL_OUTBOX_H
#define VENTURE_MAIL_OUTBOX_H
#include "mail/venture-mailer.h"
#include "db/venture-database.h"
G_BEGIN_DECLS
#define VENTURE_TYPE_MAIL_OUTBOX (venture_mail_outbox_get_type())
/**
 * VENTURE_MAIL_LEASE_SECONDS:
 *
 * How long a claimed message's sending lease lasts. A #VentureMailOutbox::before-send
 * handler recovers the delivery clock as the claimed row's lease-until minus
 * this, so a sweep run with an explicit clock is judged by that clock.
 */
#define VENTURE_MAIL_LEASE_SECONDS (600)
G_DECLARE_FINAL_TYPE(VentureMailOutbox, venture_mail_outbox, VENTURE, MAIL_OUTBOX, GObject)
/**
 * venture_mail_outbox_new:
 * @database: owning database; used only on its main thread
 * @mailer: transport for one attempt
 * Returns: (transfer full): outbox service
 */
VentureMailOutbox *venture_mail_outbox_new(VentureDatabase *database, VentureMailer *mailer);
/**
 * venture_mail_outbox_enqueue:
 * @self: outbox
 * @message: unsaved message with organization and idempotency key
 * @actor: (nullable): audit actor
 * @error: (out) (optional): validation or persistence failure
 * Returns: (transfer full) (nullable): queued row, or existing row for the key
 */
VentureMailMessage *venture_mail_outbox_enqueue(VentureMailOutbox *self, VentureMailMessage *message, const VentureActor *actor, GError **error);
/**
 * venture_mail_outbox_claim:
 * @self: outbox
 * @organization_id: exact owning organization
 * @id: message id
 * @now: time of claim
 * @error: (out) (optional): not due, already claimed or persistence error
 * Returns: (transfer full) (nullable): message with committed lease
 */
VentureMailMessage *venture_mail_outbox_claim(VentureMailOutbox *self, gint64 organization_id, gint64 id, GDateTime *now, GError **error);
/**
 * venture_mail_outbox_deliver_due:
 * @self: outbox
 * @organization_id: exact owning organization
 * @limit: maximum attempts
 * @now: (nullable): sweep time, or current UTC time
 * @cancellable: (nullable): cancellation
 * @error: (out) (optional): sweep persistence failure
 * Returns: attempted count, or -1 on sweep failure; submission errors live on rows
 */
gint venture_mail_outbox_deliver_due(VentureMailOutbox *self, gint64 organization_id, guint limit, GDateTime *now, GCancellable *cancellable, GError **error);
/**
 * venture_mail_outbox_retry:
 * @self: outbox
 * @organization_id: exact owning organization
 * @id: message id
 * @actor: (nullable): deliberate retry actor
 * @error: (out) (optional): invalid state or persistence error
 * Returns: whether the same row and Message-ID were queued again
 */
gboolean venture_mail_outbox_retry(VentureMailOutbox *self, gint64 organization_id, gint64 id, const VentureActor *actor, GError **error);
/**
 * venture_mail_check_removal:
 * @entity: record proposed for deletion, restoration or purge
 * @error: (out) (optional): retained-outbox refusal
 * Returns: whether removal is allowed; mail identities must remain retained
 */
gboolean venture_mail_check_removal(VentureEntity *entity, GError **error);
G_END_DECLS
#endif
