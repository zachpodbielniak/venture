/*
 * venture-market.h - What things fetch, and how offers end
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The market module keeps two kinds of evidence that a sale record cannot:
 * prices somebody saw (`price_observation`) and offers that ended without
 * becoming a sale (`listing`). The rules that keep them honest are save
 * validators installed here, so the form, the API, an approved staged
 * change and the assistant all obey the same ones. The one lookup every
 * valuing report shares -- what was this worth, from that source, on that
 * day -- is venture_market_latest_price().
 */

#ifndef VENTURE_MARKET_H
#define VENTURE_MARKET_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/**
 * venture_market_install:
 * @context: the wiring
 *
 * Installs the save validators for `price_observation` (a product, a
 * price that is not negative) and `listing` (quantities, the outcome's
 * consequences, the derived closing time, one currency). Called once by
 * the context; a second context over the same database installs nothing
 * twice.
 */
void
venture_market_install(VentureContext *context);

/**
 * venture_market_listing_outcome_is_closed:
 * @outcome: a listing outcome
 *
 * Returns: %TRUE for every outcome but open: the listing has ended
 */
gboolean
venture_market_listing_outcome_is_closed(VentureListingOutcome outcome);

/**
 * venture_market_latest_price:
 * @database: the database to read
 * @organization_id: the organization whose observations count
 * @product_id: the product priced
 * @source: (nullable): only observations from this source (matched
 *   exactly); %NULL or empty for any source
 * @currency: (nullable): only observations priced in this currency;
 *   %NULL or empty for any currency
 * @at: (nullable): the moment to value at; the newest observation at or
 *   before it wins. %NULL means now, with no bound
 * @out_price: (out) (optional) (nullable) (transfer full): the price, or
 *   %NULL when nothing was observed
 * @out_observation: (out) (optional) (nullable) (transfer full): the
 *   observation the price came from
 * @error: (out) (optional): return location for a #GError
 *
 * The price of one unit of @product_id as last seen at or before @at.
 * Soft-deleted observations and other organizations' observations are
 * never read. With no @currency the newest observation wins whatever it
 * is priced in, so a product seen in TICKET yesterday and GOLD last week
 * is worth tickets; a report comparing in GOLD asks for GOLD, or uses
 * venture_market_price_preferring(). Ties on the observation time go to the higher id -- the one
 * recorded last -- so the answer is the same on every call.
 *
 * Nothing observed is not an error: the call succeeds with @out_price set
 * to %NULL, and the caller decides what an unpriced product is worth.
 *
 * Returns: %TRUE on success, %FALSE with @error set when the read failed
 */
gboolean
venture_market_latest_price(
	VentureDatabase	 *database,
	gint64		  organization_id,
	gint64		  product_id,
	const gchar	 *source,
	const gchar	 *currency,
	GDateTime	 *at,
	VentureMoney	**out_price,
	VentureEntity	**out_observation,
	GError		**error
);

/**
 * venture_market_price_preferring:
 * @database: the database to read
 * @organization_id: the organization whose observations count
 * @product_id: the product priced
 * @source: (nullable): only observations from this source; %NULL or empty
 *   for any
 * @prefer: (nullable): the currency to value in when the product has
 *   been seen priced in it, usually the organization's book currency
 * @at: (nullable): the moment to value at; %NULL means now
 * @out_price: (out) (optional) (nullable) (transfer full): the price, or
 *   %NULL when nothing was observed
 * @out_observation: (out) (optional) (nullable) (transfer full): the
 *   observation the price came from
 * @error: (out) (optional): return location for a #GError
 *
 * The newest price in @prefer at or before @at, and only when the
 * product was never seen priced in @prefer, the newest in any currency.
 * What the valuing reports (recipe_margin, session_performance,
 * goal_materials) use when no =currency= option names one: a margin
 * whose inputs were seen in gold and in tickets is computed in gold
 * wherever gold was seen, instead of in whichever currency happened to
 * be observed last. It never converts.
 *
 * Returns: %TRUE on success, %FALSE with @error set when the read failed
 */
gboolean
venture_market_price_preferring(
	VentureDatabase	 *database,
	gint64		  organization_id,
	gint64		  product_id,
	const gchar	 *source,
	const gchar	 *prefer,
	GDateTime	 *at,
	VentureMoney	**out_price,
	VentureEntity	**out_observation,
	GError		**error
);

/**
 * venture_market_valuing_currency:
 * @database: the database to read
 * @organization_id: the organization the report is about
 * @options: (nullable): the report's options
 * @out_currency: (out) (transfer full): the currency to value in
 * @out_strict: (out): %TRUE when only prices in @out_currency count
 * @error: (out) (optional): return location for a #GError
 *
 * The one reading of a valuing report's =currency= option, shared by
 * recipe_margin, session_performance and goal_materials so the three
 * cannot disagree: named, only observations in that currency count
 * (venture_market_latest_price()); absent, the organization's book
 * currency is preferred and any other is the fallback
 * (venture_market_price_preferring()). A name that is not a currency code
 * is refused rather than matching nothing.
 *
 * Returns: %TRUE on success, %FALSE with @error set
 */
gboolean
venture_market_valuing_currency(
	VentureDatabase	 *database,
	gint64		  organization_id,
	JsonObject	 *options,
	gchar		**out_currency,
	gboolean	 *out_strict,
	GError		**error
);

/**
 * venture_market_listing_performance:
 * @context: the wiring
 * @period: (nullable): bounds `listed-at`
 * @options: (nullable): `group_by` (product, category or channel),
 *   `category_depth`, `venture_id`, `organization_id`; see
 *   docs/market.org
 * @error: (out) (optional): return location for a #GError
 *
 * How listings did: counted by outcome, units listed and sold, the sale
 * rate over closed listings, days to sell, the average sold unit price,
 * deposits lost and fees -- one row per group and currency.
 *
 * Returns: (transfer full) (nullable): the result, or %NULL with @error set
 */
VentureReportResult *
venture_market_listing_performance(
	VentureContext		 *context,
	VentureDateRange	 *period,
	JsonObject		 *options,
	GError			**error
);

/**
 * venture_market_price_history:
 * @context: the wiring
 * @period: (nullable): bounds `observed-at`
 * @options: `product_id` (required), `source`, `bucket` (day, week or
 *   month), `organization_id`
 * @error: (out) (optional): return location for a #GError
 *
 * A product's observed prices per bucket and source: minimum, average
 * (half to even), maximum, total volume and how many observations, one row
 * per currency.
 *
 * Returns: (transfer full) (nullable): the result, or %NULL with @error set
 */
VentureReportResult *
venture_market_price_history(
	VentureContext		 *context,
	VentureDateRange	 *period,
	JsonObject		 *options,
	GError			**error
);

/**
 * venture_market_register_reports:
 * @registry: the report registry
 *
 * Registers `listing_performance` and `price_history`. Both belong to the
 * market module, so switching it off hides them.
 */
void
venture_market_register_reports(VentureReportRegistry *registry);

G_END_DECLS

#endif /* VENTURE_MARKET_H */
