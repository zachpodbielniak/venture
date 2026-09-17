/*
 * venture-headline-home.h - The five headline cards and their settings
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The four headline reports -- cac, churn, ltv, ltv_cac -- are ordinary
 * registry reports. This is the layer above them: the per-organisation
 * tuning row, and the five cards the home page shows, computed once here
 * so the page and the API cannot disagree.
 */

#ifndef VENTURE_HEADLINE_HOME_H
#define VENTURE_HEADLINE_HOME_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/**
 * venture_headline_setting_find:
 * @database: the repository
 * @organization_id: the organisation
 *
 * Reads the organisation's headline settings row.
 *
 * Returns: (transfer full) (nullable): the row, or %NULL when every default
 *   applies
 */
VentureEntity *
venture_headline_setting_find(
	VentureDatabase	*database,
	gint64		 organization_id
);

/**
 * venture_headline_home_enabled:
 * @database: the repository
 * @organization_id: the organisation
 *
 * Whether the five cards are the organisation's home page. They are unless
 * the settings row says `classic_home`.
 *
 * Returns: %TRUE when / shows the headline cards
 */
gboolean
venture_headline_home_enabled(
	VentureDatabase	*database,
	gint64		 organization_id
);

/**
 * venture_headline_home_cards:
 * @context: the wiring
 * @organization_id: the organisation, or 0 for the default
 * @period: the period each card covers; the trend compares it with the
 *   period before
 * @error: (out) (optional): return location for a #GError
 *
 * Computes the five cards: P&L with bank cash, CAC, churn, LTV:CAC and
 * support load. Each is an object with `key`, `label`, `value` (text),
 * `trend` (-1, 0 or 1 against the previous period), `higher_is_better`,
 * `link` (the report page) and `lines` (an array of `label`/`value` pairs).
 *
 * Returns: (transfer full) (nullable): a JSON array of five cards
 */
JsonNode *
venture_headline_home_cards(
	VentureContext		 *context,
	gint64			  organization_id,
	VentureDateRange	 *period,
	GError			**error
);

G_END_DECLS

#endif /* VENTURE_HEADLINE_HOME_H */
