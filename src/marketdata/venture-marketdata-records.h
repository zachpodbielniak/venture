/*
 * venture-marketdata-records.h - Venues, instruments and watchlists
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The marketdata module's record types. A venue is a place things are
 * priced (a realm's auction house, a bookmaker, a supplier); an
 * instrument is a thing priced there (an item, an outcome, a SKU). Both
 * can name the series store row they stand for -- a data source and a
 * key -- which is what lets the price oracle answer for a product. A
 * watchlist is a shared list of instruments with the prices somebody is
 * waiting for. An alert rule says what somebody wants to be told about
 * that data, and an alert hit is one time it fired. All are field tables
 * and nothing else: the rules that span rows are save validators in
 * venture-marketdata.c and venture-marketdata-alerts.c.
 */

#ifndef VENTURE_MARKETDATA_RECORDS_H
#define VENTURE_MARKETDATA_RECORDS_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

G_BEGIN_DECLS

#define VENTURE_TYPE_VENUE (venture_venue_get_type())
VENTURE_DECLARE_ENTITY(VentureVenue, venture_venue, VENUE)

#define VENTURE_TYPE_INSTRUMENT (venture_instrument_get_type())
VENTURE_DECLARE_ENTITY(VentureInstrument, venture_instrument, INSTRUMENT)

#define VENTURE_TYPE_WATCHLIST (venture_watchlist_get_type())
VENTURE_DECLARE_ENTITY(VentureWatchlist, venture_watchlist, WATCHLIST)

#define VENTURE_TYPE_WATCHLIST_ENTRY (venture_watchlist_entry_get_type())
VENTURE_DECLARE_ENTITY(VentureWatchlistEntry, venture_watchlist_entry, WATCHLIST_ENTRY)

#define VENTURE_TYPE_ALERT_RULE (venture_alert_rule_get_type())
VENTURE_DECLARE_ENTITY(VentureAlertRule, venture_alert_rule, ALERT_RULE)

#define VENTURE_TYPE_ALERT_HIT (venture_alert_hit_get_type())
VENTURE_DECLARE_ENTITY(VentureAlertHit, venture_alert_hit, ALERT_HIT)

G_END_DECLS

#endif /* VENTURE_MARKETDATA_RECORDS_H */
