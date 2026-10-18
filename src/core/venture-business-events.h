/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_BUSINESS_EVENTS_H
#define VENTURE_BUSINESS_EVENTS_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
/**
 * venture_business_events_read:
 * @database: repository under the caller's access scope
 * @organization: one positive organization identity
 * @after: exclusive cursor, zero to start at the beginning
 * @limit: page size between 1 and 1000
 * @error: (out) (optional): refusal
 *
 * Reads retained events in cursor order without acknowledging or removing any.
 * The caller retains next_cursor and deduplicates event UUIDs. Empty pages retain
 * the supplied cursor. A cursor belongs to this repository and organization.
 * Returns: (transfer full) (nullable): an object with events and next_cursor
 */
JsonNode *venture_business_events_read(VentureDatabase *database, gint64 organization,
	gint64 after, guint limit, GError **error);
/**
 * venture_business_events_install:
 * @context: application wiring
 *
 * Installs the bounded main-context delivery timer. No network runs inside a
 * business transaction. Pending deliveries remain durable when delivery is off.
 */
void venture_business_events_install(VentureContext *context);
/**
 * venture_business_events_dispatch:
 * @context: application wiring
 * @now: (nullable): delivery clock, or NULL for the current time
 * @error: (out) (optional): refusal
 *
 * Starts up to sixteen due deliveries asynchronously. Refuses an open database
 * transaction. Failures retain the same delivery identity with capped backoff.
 * Returns: whether the sweep could inspect pending work
 */
gboolean venture_business_events_dispatch(VentureContext *context, GDateTime *now, GError **error);
G_END_DECLS
#endif
