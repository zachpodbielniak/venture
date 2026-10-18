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
/* A reminder of a sent quote to its customer: the delivery's private link
 * again, "Reminder" in the subject, keyed by day (YYYY-MM-DD) so a second
 * request that day sends nothing more. Needs a public HTTPS base_url. */
gboolean venture_quote_mail_remind(VentureDatabase *database, VentureEntity *quote, VentureEntity *delivery,
	const gchar *base_url, const gchar *day, const VentureActor *actor, GError **error);
#endif
