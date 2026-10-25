/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_MAIL_CONSUMERS_H
#define VENTURE_MAIL_CONSUMERS_H
/* Ticket replies and their outbox rows commit as one business command. */
gboolean venture_mail_support_save_comment(VentureDatabase *database,
	VentureEntity *comment, const VentureActor *actor,
	gboolean (*save_next)(VentureDatabase *, VentureEntity *,
	                      const VentureActor *, GError **), GError **error);
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
/**
 * venture_mail_confirm_money:
 * @database: financial source database
 * @record: accepted quote
 * @actor: (nullable): audit actor
 * @error: (out) (optional): enqueue failure
 *
 * Internal service hook. The caller owns the action transaction; the message
 * rolls back with it. Missing customer addresses do not refuse acceptance.
 * Returns: whether the confirmation was queued or legitimately omitted
 */
gboolean venture_mail_confirm_money(VentureDatabase *database, VentureEntity *record,
    const VentureActor *actor, GError **error);
/**
 * venture_mail_invoice_message:
 * @context: application context
 * @invoice: the invoice, as it will be when sent
 * @error: (out) (optional): no customer email, a void invoice, or a rendering failure
 *
 * Builds the invoice mail the Send button queues, writing nothing: the
 * organization's =invoice= template (or the default, unsaved), the line
 * table and the PDF. venture_mail_send_invoice() and the preview both call it.
 *
 * Returns: (transfer full) (nullable): the unsaved message
 */
VentureMailMessage *venture_mail_invoice_message(VentureContext *context, VentureEntity *invoice, GError **error);
/**
 * venture_mail_user_message:
 * @user: a user with an email address
 * @reset: the password-change notice rather than the welcome
 * Returns: (transfer full): the unsaved account notice venture_mail_save_user() queues
 */
VentureMailMessage *venture_mail_user_message(VentureEntity *user, gboolean reset);
/**
 * venture_mail_consumers_preview_register:
 *
 * Registers the invoice and account mail preview kinds.
 */
void venture_mail_consumers_preview_register(void);
/**
 * venture_mail_delivery_sweep:
 * @context: application context
 * @error: (out) (optional): a sweep that could not run at all
 *
 * Delivers the due outbox mail of every active business that can send. A
 * business that has not set up a mail account is skipped, so its mail stays
 * queued until it has one, rather than being given up on.
 * Returns: messages attempted, or -1 with @error set
 */
gint venture_mail_delivery_sweep(VentureContext *context, GError **error);
/**
 * venture_mail_delivery_start:
 * @context: application context
 *
 * Runs venture_mail_delivery_sweep() on a timer in a hosted workspace that
 * has no automation engine attached. Hosted mode refuses the engine, so no
 * mail_deliver rule can run there. Elsewhere the rules own delivery and the
 * timer does nothing. Calling it again is harmless.
 */
void venture_mail_delivery_start(VentureContext *context);
/**
 * venture_mail_delivery_stop:
 * @context: application context
 */
void venture_mail_delivery_stop(VentureContext *context);
#endif
