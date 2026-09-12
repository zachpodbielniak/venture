/*
 * venture-routing.h - Who a new ticket goes to
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A queue nobody owns is a queue nobody works. A routing rule matches a
 * ticket on its kind, priority and tags and puts a name on it the moment
 * it arrives -- in turn around a rota, to whoever is least busy, or
 * always to the same person. Applied only when the ticket arrives with
 * nobody on it, so raising a ticket for somebody specific still works.
 */

#ifndef VENTURE_ROUTING_H
#define VENTURE_ROUTING_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>

G_BEGIN_DECLS

/**
 * venture_routing_install:
 * @context: the wiring
 *
 * Installs the save validator that assigns a new ticket. Called once by
 * the context.
 */
void
venture_routing_install(VentureContext *context);

/**
 * venture_routing_find_rule:
 * @context: the wiring
 * @kind: the ticket's kind
 * @priority: the ticket's priority
 * @tags: (nullable): the ticket's tags, comma separated
 *
 * The active rule that covers a ticket: the most specific match wins --
 * a rule naming a tag beats one naming a kind and a priority, which
 * beats a catch-all -- and ties go to the lowest id, so the answer is
 * stable.
 *
 * Returns: (transfer full) (nullable): the rule, or %NULL if none covers it
 */
VentureEntity *
venture_routing_find_rule(
	VentureContext		*context,
	VentureTicketKind	 kind,
	VenturePriority		 priority,
	const gchar		*tags
);

/**
 * venture_routing_choose:
 * @context: the wiring
 * @rule: the rule, whose rota cursor is advanced for a round robin
 * @error: (out) (optional): return location for a #GError
 *
 * Picks the username a ticket under @rule goes to, and moves the rule's
 * cursor when the strategy is a rota. A rule naming nobody picks nobody.
 *
 * Returns: (transfer full) (nullable): the username, or %NULL
 */
gchar *
venture_routing_choose(
	VentureContext	 *context,
	VentureEntity	 *rule,
	GError		**error
);

G_END_DECLS

#endif /* VENTURE_ROUTING_H */
