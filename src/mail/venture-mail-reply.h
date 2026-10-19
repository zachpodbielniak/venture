/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * venture-mail-reply.h - Replying to a synced message from the timeline
 *
 * A reply is an ordinary outbox message: same queue, same retries, same
 * delivery sweep, which records it on the contact's timeline as outbound
 * after the relay accepts it. What makes it a reply is that it names the
 * message it answers in In-Reply-To and the thread in References, so the
 * recipient's mail client threads it.
 */
#ifndef VENTURE_MAIL_REPLY_H
#define VENTURE_MAIL_REPLY_H
G_BEGIN_DECLS

/**
 * VENTURE_MAIL_REPLY_MAX_BODY:
 *
 * The longest reply body accepted, in bytes.
 */
#define VENTURE_MAIL_REPLY_MAX_BODY (64 * 1024)

/**
 * venture_mail_reply_source:
 * @database: repository
 * @interaction: an email interaction
 *
 * Returns: (transfer full) (nullable): the synced mail_inbound row the
 *   interaction came from, when it is an inbound message that can be answered
 */
VentureEntity *venture_mail_reply_source(VentureDatabase *database, VentureEntity *interaction);

/**
 * venture_mail_reply_enqueue:
 * @context: the wiring (its outbox delivers the reply)
 * @interaction: an inbound email interaction from the mail sync
 * @body: the reply text
 * @actor: (nullable): who wrote it
 * @error: (out) (optional): refusal
 *
 * Queues "Re: <subject>" to the message's sender with In-Reply-To and
 * References set. Nothing is sent here; the outbox sweep delivers it.
 * Returns: (transfer full) (nullable): the queued message
 */
VentureMailMessage *venture_mail_reply_enqueue(VentureContext *context, VentureEntity *interaction,
	const gchar *body, const VentureActor *actor, GError **error);

G_END_DECLS
#endif
