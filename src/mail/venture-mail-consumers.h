/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_MAIL_CONSUMERS_H
#define VENTURE_MAIL_CONSUMERS_H
/**
 * venture_mail_send_invoice:
 * @context: application context
 * @organization_id: exact organization
 * @invoice_id: invoice to issue and enqueue
 * @actor: (nullable): audit actor
 * @error: (out) (optional): validation or persistence failure
 * Returns: (transfer full) (nullable): durable message; repeated calls return the same row
 */
VentureMailMessage *venture_mail_send_invoice(VentureContext *context, gint64 organization_id, gint64 invoice_id, const VentureActor *actor, GError **error);
/**
 * venture_mail_save_user:
 * @outbox: canonical outbox
 * @user: proposed user write
 * @actor: (nullable): audit actor
 * @handled: (out): whether this function performed the save
 * @error: (out) (optional): transaction failure
 * Returns: success; user and welcome/reset notification commit together
 */
gboolean venture_mail_save_user(VentureMailOutbox *outbox, VentureEntity *user, const VentureActor *actor, gboolean *handled, GError **error);
#endif
