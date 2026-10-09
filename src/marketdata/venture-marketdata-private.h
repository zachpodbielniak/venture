/*
 * venture-marketdata-private.h - Checks the marketdata files share
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Include only from src/marketdata. The validators for venues and
 * instruments (venture-marketdata.c) and for alert rules
 * (venture-marketdata-alerts.c) hold text and references to the same
 * rules, and one definition of each is what keeps them the same.
 */

#ifndef VENTURE_MARKETDATA_PRIVATE_H
#define VENTURE_MARKETDATA_PRIVATE_H

#include <glib-object.h>

G_BEGIN_DECLS

/*
 * venture_marketdata_check_text:
 * A namespace, key, group or pattern: at most the store's key length,
 * valid UTF-8 and no control characters. Empty is fine.
 */
gboolean
venture_marketdata_check_text(
	const gchar	 *value,
	const gchar	 *label,
	GError		**error
);

/*
 * venture_marketdata_check_same_organization:
 * Refuses a reference being written to a record in another organization;
 * a value kept from @previous is left alone.
 */
gboolean
venture_marketdata_check_same_organization(
	VentureDatabase	 *database,
	VentureEntity	 *entity,
	VentureEntity	 *previous,
	const gchar	 *property,
	GType		  target_type,
	const gchar	 *label,
	GError		**error
);

#ifdef VENTURE_HAVE_SQLITE

/*
 * venture_marketdata_source_in:
 * The data source @data_source_id, which must be @organization_id's and
 * not deleted; NOT_FOUND otherwise.
 */
VentureEntity *
venture_marketdata_source_in(
	VentureDatabase	 *database,
	gint64		  organization_id,
	gint64		  data_source_id,
	GError		**error
);

/*
 * venture_marketdata_deal_sell_side:
 * Where @buy is best sold, as Deals reckons it: the dearest other venue
 * with it in stock in @group_keys (else the buy venue's group), in the
 * same currency, where it sells (at least @min_sold_per_day a day by the
 * store's estimate, when that is not NAN), at no troll's price, and --
 * with @fresh_after above 0 -- taken no earlier than that. Sets @out_net
 * (nullable: its lowest price less @cut_pct, rounded the seller's way),
 * @out_profit (that less the buy) and @out_roi (profit over the buy
 * price, percent). Reads only the
 * store, so the feeds worker may call it. NULL when nowhere qualifies.
 */
VentureSeriesRow *
venture_marketdata_deal_sell_side(
	VentureSeriesStore	*reader,
	const VentureSeriesRow	*buy,
	const gchar *const	*group_keys,
	gdouble			 cut_pct,
	gdouble			 min_sold_per_day,
	gint64			 fresh_after,
	gint64			*out_net,
	gint64			*out_profit,
	gdouble			*out_roi
);

/*
 * venture_marketdata_reader:
 * A main-thread read handle on the source's store; CONFIG with the feeds
 * module off, NOT_FOUND before the source has stored anything.
 */
VentureSeriesStore *
venture_marketdata_reader(
	VentureContext	 *context,
	gint64		  data_source_id,
	GError		**error
);

/*
 * VentureMarketdataBudget:
 * How many records a promotion may still write -- create or restore --
 * for a caller that writes under a bound (the position mirror). Every
 * record written takes one from @left; a record that would be written
 * with nothing left is not, @cut is set, and the call answers TRUE with
 * *out NULL. An instrument's parents count one each, so a chain cut half
 * way leaves its parents made and the next pass, finding them, goes on
 * from there.
 */
typedef struct
{
	guint		 left;
	gboolean	 cut;
} VentureMarketdataBudget;

/*
 * venture_marketdata_promote_instrument_in:
 * Promotion of one instrument the store @store knows, as the public call
 * does, except that with @restore FALSE a deleted record is handed back
 * deleted rather than restored -- what the position mirror asks for, so
 * it never brings back what a person removed -- and that with a @budget
 * (nullable: no bound) it writes no more records than the budget has.
 * *@out is set on success, and is NULL when the budget cut it.
 */
gboolean
venture_marketdata_promote_instrument_in(
	VentureContext			 *context,
	VentureSeriesStore		 *store,
	VentureEntity			 *source,
	const gchar			 *key,
	gboolean			  restore,
	VentureMarketdataBudget		 *budget,
	const VentureActor		 *actor,
	VentureEntity			**out,
	GError				**error
);

/*
 * venture_marketdata_promote_venue_in:
 * The same for a venue, from the store's row for it.
 */
gboolean
venture_marketdata_promote_venue_in(
	VentureContext			 *context,
	VentureEntity			 *source,
	const VentureSeriesVenueRow	 *row,
	gboolean			  restore,
	VentureMarketdataBudget		 *budget,
	const VentureActor		 *actor,
	VentureEntity			**out,
	GError				**error
);

/*
 * VentureMarketdataIgnores:
 * The accounts and realms an organization ignores (account_ignore
 * records), loaded once per answer. An account is ignored when its key is
 * named as a character, or its realm -- its group or its venue, ignoring
 * case -- is named as a realm.
 */
typedef struct _VentureMarketdataIgnores VentureMarketdataIgnores;

VentureMarketdataIgnores *
venture_marketdata_ignores_load(
	VentureContext	*context,
	gint64		 organization_id
);

void
venture_marketdata_ignores_free(VentureMarketdataIgnores *ignores);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(VentureMarketdataIgnores, venture_marketdata_ignores_free)

/*
 * venture_marketdata_ignores_json:
 * The records, as [{id, kind, key, name}] by name, for a page that lists
 * them to turn off. (transfer full)
 */
JsonArray *
venture_marketdata_ignores_json(const VentureMarketdataIgnores *ignores);

/*
 * venture_marketdata_ignores_match:
 * The id of the record that ignores @account -- its own before its
 * realm's -- or 0. *@out_kind (nullable) is "character" or "realm".
 */
gint64
venture_marketdata_ignores_match(
	const VentureMarketdataIgnores	 *ignores,
	const VentureSeriesAccountRow	 *account,
	const gchar			**out_kind
);

/*
 * venture_marketdata_ignores_character:
 * The id of the record ignoring the account @account_key itself, or 0.
 */
gint64
venture_marketdata_ignores_character(
	const VentureMarketdataIgnores	*ignores,
	const gchar			*account_key
);

/*
 * venture_marketdata_ignores_realm:
 * The id of the record ignoring the realm @group_key or @venue_key
 * (either nullable), or 0.
 */
gint64
venture_marketdata_ignores_realm(
	const VentureMarketdataIgnores	*ignores,
	const gchar			*group_key,
	const gchar			*venue_key
);

/*
 * venture_marketdata_ignores_split:
 * Takes the ignored accounts out of @accounts (VentureSeriesAccountRow),
 * in order, and answers their keys as a NULL-terminated array (never
 * NULL; empty when none is ignored) for a filter's exclude_account_keys.
 * The keys are copies, so they outlive the rows. With @keep the rows
 * stay and only the keys are answered.
 */
gchar **
venture_marketdata_ignores_split(
	const VentureMarketdataIgnores	*ignores,
	GPtrArray			*accounts,
	gboolean			 keep
);

#endif /* VENTURE_HAVE_SQLITE */

G_END_DECLS

#endif /* VENTURE_MARKETDATA_PRIVATE_H */
