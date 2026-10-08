/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_QUOTE_MAIL_H
#define VENTURE_QUOTE_MAIL_H
/**
 * venture_quote_mail_enqueue:
 * @database: quote's database, inside the send transaction
 * @quote: draft being sent
 * @delivery: unsaved delivery row whose acceptance URL is already set
 * @base_url: public HTTPS origin, or empty when this install has none
 * @actor: (nullable): audit actor
 * @error: (out) (optional): enqueue failure
 *
 * Queues one customer email in the durable outbox. Performs no network I/O.
 * An empty public origin returns success and queues nothing, so an offline
 * ledger can still record delivery intent. A non-HTTPS origin or a missing
 * recipient fails the caller's send. SMTP happens later through the outbox.
 *
 * Returns: %TRUE when mail was queued or legitimately omitted
 */
gboolean venture_quote_mail_enqueue(VentureDatabase *database, VentureEntity *quote,
	VentureEntity *delivery, const gchar *base_url, const VentureActor *actor, GError **error);
#endif
