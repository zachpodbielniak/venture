/*
 * venture-headline-home.h - The five headline cards and their settings
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The headline reports -- cac, customer_churn, ltv, ltv_cac and
 * customer_cohorts -- are ordinary registry reports. This is the layer above
 * them: the per-organisation tuning row, and the cards the home page shows,
 * written as JSON, HTML and CSV in one pass so the page, the API and an
 * export cannot disagree.
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
 * venture_headline_home_render:
 * @context: the wiring
 * @options: (nullable): the scope: organization_id (0 or absent for the
 *   default), venture_id and as_of
 * @period: the period each card covers
 * @period_text: (nullable): the period as it was asked for, carried into
 *   each card's report link; %NULL means this_month
 * @now: (nullable): the instant treated as now when choosing the comparison
 *   period, or %NULL for the clock
 * @may_see_totals: whether the viewer may see organisation totals; when
 *   %FALSE every card is written in its restricted state and nothing is
 *   computed
 * @out_cards: (out) (optional) (transfer full): the cards as a JSON array
 * @out_html: (out) (optional) (transfer full): the cards as HTML
 * @out_csv: (out) (optional) (transfer full): the cards as CSV, one row per
 *   figure and one per line under it
 * @error: (out) (optional): an invalid scope
 *
 * Computes the headline cards and writes each one to JSON, HTML and CSV in
 * the same pass: P&L with bank cash, recurring revenue (only when billing is
 * on and the organisation has subscription history), CAC, churn, LTV:CAC
 * and support load. Each card is an object with `key`, `label`, `value`
 * (text), `trend` (-1, 0 or 1 against the comparison period chosen by
 * venture_date_range_comparison_period()), `higher_is_better`, `link` (the
 * report page with the same period and scope), `definition`, `state` (ok,
 * error or restricted), `error` and `lines` (an array of `label`/`value`
 * pairs). A card that fails is an error card; the others still render.
 *
 * Returns: %TRUE unless the scope itself was invalid
 */
gboolean
venture_headline_home_render(
	VentureContext		 *context,
	JsonObject		 *options,
	VentureDateRange	 *period,
	const gchar		 *period_text,
	GDateTime		 *now,
	gboolean		  may_see_totals,
	JsonNode		**out_cards,
	gchar			**out_html,
	gchar			**out_csv,
	GError			**error
);

/**
 * venture_headline_home_cards:
 * @context: the wiring
 * @organization_id: the organisation, or 0 for the default
 * @period: the period each card covers
 * @error: (out) (optional): return location for a #GError
 *
 * The cards of venture_headline_home_render() as JSON, for a caller already
 * entitled to organisation totals.
 *
 * Returns: (transfer full) (nullable): a JSON array of cards
 */
JsonNode *
venture_headline_home_cards(
	VentureContext       *context,
	gint64            organization_id,
	VentureDateRange     *period,
	GError          **error
);

G_END_DECLS

#endif /* VENTURE_HEADLINE_HOME_H */
