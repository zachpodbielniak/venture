/*
 * venture-market-records.h - Price observations and listings
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The market module's two record types. An observation is a price
 * somebody saw; a listing is an offer to sell that ends. Both are field
 * tables and nothing else -- the rules that span rows live in
 * venture-market.c as save validators, so every writer obeys them.
 */

#ifndef VENTURE_MARKET_RECORDS_H
#define VENTURE_MARKET_RECORDS_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

G_BEGIN_DECLS

#define VENTURE_TYPE_PRICE_OBSERVATION (venture_price_observation_get_type())
VENTURE_DECLARE_ENTITY(VenturePriceObservation, venture_price_observation, PRICE_OBSERVATION)

#define VENTURE_TYPE_LISTING (venture_listing_get_type())
VENTURE_DECLARE_ENTITY(VentureListing, venture_listing, LISTING)

G_END_DECLS

#endif /* VENTURE_MARKET_RECORDS_H */
