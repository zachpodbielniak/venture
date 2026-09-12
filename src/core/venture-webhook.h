/*
 * venture-webhook.h - Telling something outside that a record changed
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The other direction from the forge's inbound hook. A webhook is a URL,
 * a list of events and a signing secret; every audited change matching
 * one is POSTed to it, signed the same way VENTURE verifies the
 * deliveries it receives, and what happened is kept as a record. Sending
 * is asynchronous on the main loop -- no thread, because the only
 * background thread in this program belongs to coding runs and may not
 * touch the database.
 */

#ifndef VENTURE_WEBHOOK_H
#define VENTURE_WEBHOOK_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/**
 * VENTURE_WEBHOOK_FAILURE_LIMIT:
 *
 * Consecutive failures after which a webhook switches itself off. An
 * endpoint that has refused ten deliveries in a row is gone, and
 * posting to it on every write for the rest of the week costs a request
 * per change and tells nobody anything.
 */
#define VENTURE_WEBHOOK_FAILURE_LIMIT (10)

/**
 * venture_webhook_install:
 * @context: the wiring
 *
 * Connects the sender to the database's audit signal. Called once, by
 * the context, after the database is open.
 */
void
venture_webhook_install(VentureContext *context);

/**
 * venture_webhook_event_name:
 * @target_type: the record type, e.g. `ticket`
 * @action: the audit action
 *
 * The event a change is published as: `ticket.created`,
 * `invoice.updated`, `sale.deleted`.
 *
 * Returns: (transfer full) (nullable): the event name, or %NULL for an
 *   action that is not published
 */
gchar *
venture_webhook_event_name(
	const gchar		*target_type,
	VentureAuditAction	 action
);

/**
 * venture_webhook_matches:
 * @events: (nullable): a webhook's comma-separated event patterns
 * @event: the event name
 *
 * Whether a webhook subscribed to @events wants @event. `*` matches
 * everything, `ticket.*` every action on a ticket, and an exact name
 * itself. An empty list means everything, because a webhook with no
 * events named is one somebody has not narrowed yet rather than one
 * that should never fire.
 *
 * Returns: %TRUE if it matches
 */
gboolean
venture_webhook_matches(
	const gchar	*events,
	const gchar	*event
);

/**
 * venture_webhook_set_secret:
 * @context: the wiring
 * @webhook: the webhook, updated in place and saved
 * @secret: (nullable): the secret, or %NULL to generate one
 * @actor: (nullable): who is setting it
 * @error: (out) (optional): return location for a #GError
 *
 * Sets a webhook's signing secret and stamps when. Generating is the
 * normal path: a secret somebody invented is a secret somebody can
 * guess.
 *
 * Returns: (transfer full) (nullable): the secret, to be shown exactly
 *   once, or %NULL on error
 */
gchar *
venture_webhook_set_secret(
	VentureContext		 *context,
	VentureEntity		 *webhook,
	const gchar		 *secret,
	const VentureActor	 *actor,
	GError			**error
);

/**
 * venture_webhook_test:
 * @context: the wiring
 * @webhook: the webhook
 * @error: (out) (optional): return location for a #GError
 *
 * Sends a `webhook.test` delivery and waits for the answer, so pressing
 * Test says whether the far end is there rather than promising to find
 * out. Recorded like any other delivery. Blocking, because it is an
 * explicit request by a person who is waiting for the answer.
 *
 * Returns: (transfer full) (nullable): the delivery, or %NULL on error
 */
VentureEntity *
venture_webhook_test(
	VentureContext	 *context,
	VentureEntity	 *webhook,
	GError		**error
);

/**
 * venture_webhook_describe:
 * @context: the wiring
 * @error: (out) (optional): return location for a #GError
 *
 * Every webhook with how it is doing: whether it is active, what it
 * subscribes to, its consecutive failures, when it last delivered, and
 * the last few deliveries. What `GET /api/v1/webhooks` and the Webhooks
 * page show.
 *
 * Returns: (transfer full) (nullable): a JSON array, or %NULL on error
 */
JsonNode *
venture_webhook_describe(
	VentureContext	 *context,
	GError		**error
);

G_END_DECLS

#endif /* VENTURE_WEBHOOK_H */
