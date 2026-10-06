/*
 * venture-marketdata-accounts.h - What the account operations pages show
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The operator's side of a market, as the /accounts pages, their
 * /api/v1/accounts twins, the reports and the dashboard widgets read it:
 * an overview with what needs a login, one account in full, the
 * inventory across every account valued, and the external ledger's
 * profit and loss with its flips. One implementation per question,
 * answering plain JSON, so a figure cannot differ between doors -- the
 * same rule as the Trading pages (venture-marketdata-browse.h).
 *
 * Everything is read on the main thread through read handles on the
 * sources' stores (src/series/venture-series-accounts.h). Valuation and
 * sorting of holdings happen in the store's SQL; what is done here in C
 * is bounded by the account count (at most VENTURE_SERIES_MAX_ACCOUNTS)
 * or by the store's own row caps. Another organization's source is
 * NOT_FOUND, never FORBIDDEN. With feeds off, or in a build without
 * SQLite, every answer is still an answer: "available" is false and the
 * notes say why.
 *
 * Money is a money object (amount, currency, exponent, formatted) or an
 * array of them where several currencies may meet; series figures behind
 * a chart are integer minor units beside their currency. Times are ISO
 * 8601 in UTC.
 */

#ifndef VENTURE_MARKETDATA_ACCOUNTS_H
#define VENTURE_MARKETDATA_ACCOUNTS_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/**
 * VENTURE_MARKETDATA_ACCOUNTS_EXPIRING_HOURS:
 *
 * By default a listing "expires soon" within this many hours.
 */
#define VENTURE_MARKETDATA_ACCOUNTS_EXPIRING_HOURS (12)

/**
 * VENTURE_MARKETDATA_ACCOUNTS_MAIL_DAYS:
 *
 * By default a mail "expires soon" within this many days.
 */
#define VENTURE_MARKETDATA_ACCOUNTS_MAIL_DAYS (3)

/**
 * VENTURE_MARKETDATA_ACCOUNTS_STALE_DAYS:
 *
 * By default an account not seen in use for this many days is stale.
 */
#define VENTURE_MARKETDATA_ACCOUNTS_STALE_DAYS (14)

/**
 * VENTURE_MARKETDATA_ACCOUNTS_MAX_EXPIRING_HOURS:
 *
 * The largest "expiring soon" window for listings a question may ask, in
 * hours. Every door that takes the threshold (the pages, the dashboard
 * cards, the report) holds it to this, so none accepts what the core
 * would then refuse.
 */
#define VENTURE_MARKETDATA_ACCOUNTS_MAX_EXPIRING_HOURS (720)

/**
 * VENTURE_MARKETDATA_ACCOUNTS_MAX_MAIL_DAYS:
 *
 * The largest "expiring soon" window for mail, in days.
 */
#define VENTURE_MARKETDATA_ACCOUNTS_MAX_MAIL_DAYS (60)

/**
 * VENTURE_MARKETDATA_ACCOUNTS_MAX_STALE_DAYS:
 *
 * The longest an account may go unseen before a question calls it
 * stale, in days.
 */
#define VENTURE_MARKETDATA_ACCOUNTS_MAX_STALE_DAYS (3650)

/**
 * VENTURE_MARKETDATA_ACCOUNTS_TREND_DAYS:
 *
 * The days of closing balances behind each account's sparkline.
 */
#define VENTURE_MARKETDATA_ACCOUNTS_TREND_DAYS (30)

/**
 * VENTURE_MARKETDATA_ACCOUNTS_MAX_PAGE:
 *
 * The most rows one inventory page, one P&L table or one list of flips
 * shows.
 */
#define VENTURE_MARKETDATA_ACCOUNTS_MAX_PAGE (500)

/**
 * VENTURE_MARKETDATA_ACCOUNTS_DEAD_DAYS:
 *
 * By default stock is "dead" when none of it sold for this many days.
 */
#define VENTURE_MARKETDATA_ACCOUNTS_DEAD_DAYS (30)

/**
 * venture_marketdata_accounts_bases:
 *
 * The valuation bases holdings may be valued on: conservative (the
 * default: the lower of the region's sale average and the account's
 * venue's market value), market, min, historical, region_market and
 * region_sale_avg. See #VentureSeriesValueBasis.
 *
 * Returns: (transfer none) (array zero-terminated=1): their names
 */
const gchar *const *
venture_marketdata_accounts_bases(void);

/**
 * VentureMarketdataAccountsQuery:
 * @organization_id: whose sources
 * @data_source_id: one source, or 0 for every source of the organization
 * @basis: (nullable): the valuation basis; %NULL for conservative. An
 *   unknown one is refused.
 * @group_key: (nullable): only accounts in this group (a realm)
 * @expiring_hours: listings expiring within this many hours need a login;
 *   0 for %VENTURE_MARKETDATA_ACCOUNTS_EXPIRING_HOURS
 * @mail_days: mail expiring within this many days does; 0 for
 *   %VENTURE_MARKETDATA_ACCOUNTS_MAIL_DAYS
 * @stale_days: an account unseen this long is stale; 0 for
 *   %VENTURE_MARKETDATA_ACCOUNTS_STALE_DAYS
 * @sort: (nullable): how the account table is ordered, one of
 *   venture_marketdata_accounts_sorts(); %NULL for "attention"
 * @descending: largest first
 * @now: the moment everything is judged at; 0 for the clock
 * @realm_first: order by realm before @sort, for a table grouped by realm
 *   (a realm's rows must be contiguous); @sort orders within each realm
 * @login: (nullable): only accounts reached through this login, by key
 * @login_first: order by login before the realm and @sort, for a table
 *   grouped by login then realm; implies @realm_first within a login
 * @show_ignored: count the accounts the organization ignores (see
 *   account_ignore) as well, each row marked; by default they are left out
 *
 * What the overview asks.
 */
typedef struct
{
	gint64		 organization_id;
	gint64		 data_source_id;
	const gchar	*basis;
	const gchar	*group_key;
	gint64		 expiring_hours;
	gint64		 mail_days;
	gint64		 stale_days;
	const gchar	*sort;
	gboolean	 descending;
	gint64		 now;
	gboolean	 realm_first;
	const gchar	*login;
	gboolean	 login_first;
	gboolean	 show_ignored;
} VentureMarketdataAccountsQuery;

/**
 * venture_marketdata_accounts_query_init:
 * @query: (out caller-allocates): a query
 *
 * Every source, conservative values, the default thresholds.
 */
void
venture_marketdata_accounts_query_init(VentureMarketdataAccountsQuery *query);

/**
 * venture_marketdata_accounts_sorts:
 *
 * Returns: (transfer none) (array zero-terminated=1): the account table's
 *   orders: attention, name, realm, gold, positions, expiry, inbound,
 *   last_seen, freshness
 */
const gchar *const *
venture_marketdata_accounts_sorts(void);

/**
 * venture_marketdata_accounts:
 * @context: a #VentureContext
 * @query: what to show
 * @error: (out) (optional): return location for a #GError
 *
 * The overview: {available, notes, attribution, sources, data_source_id,
 * now, query, bases, sorts, summary, attention, accounts}. `summary`
 * holds the headline figures (gold, inventory value, open listings and
 * their buyout, mail, the thirty days' net), `attention` one row per
 * place to log in to, most urgent first, and `accounts` one row per
 * account.
 *
 * Returns: (transfer full) (nullable): the answer, or %NULL on error
 *   (NOT_FOUND for another organization's source, INVALID_ARGUMENT for
 *   an unknown basis or sort)
 */
JsonNode *
venture_marketdata_accounts(
	VentureContext				 *context,
	const VentureMarketdataAccountsQuery	 *query,
	GError					**error
);

/**
 * VentureMarketdataAccountQuery:
 * @organization_id: whose source
 * @data_source_id: the source
 * @key: the account's key in that source
 * @basis: (nullable): the valuation basis; %NULL for conservative
 * @expiring_hours: what "expires soon" means; 0 for the default
 * @ledger_count: the newest ledger rows to list; 0 for 25, at most
 *   %VENTURE_MARKETDATA_ACCOUNTS_MAX_PAGE
 * @now: the moment; 0 for the clock
 *
 * What an account's page asks.
 */
typedef struct
{
	gint64		 organization_id;
	gint64		 data_source_id;
	const gchar	*key;
	const gchar	*basis;
	gint64		 expiring_hours;
	guint		 ledger_count;
	gint64		 now;
} VentureMarketdataAccountQuery;

/**
 * venture_marketdata_account:
 * @context: a #VentureContext
 * @query: which account
 * @error: (out) (optional): return location for a #GError
 *
 * One account in full: {available, notes, attribution, data_source_id,
 * source_name, currency, now, basis, bases, account, location,
 * listings_url, balance_history, places, holdings, holdings_value,
 * positions, positions_value, inbound, ledger, ledger_30d}.
 *
 * Returns: (transfer full) (nullable): the answer, or %NULL on error
 *   (NOT_FOUND for another organization's source or an account the store
 *   has never seen)
 */
JsonNode *
venture_marketdata_account(
	VentureContext				 *context,
	const VentureMarketdataAccountQuery	 *query,
	GError					**error
);

/**
 * VentureMarketdataInventoryQuery:
 * @organization_id: whose sources
 * @data_source_id: the source, or 0 for the organization's first source
 *   (by name) that has accounts
 * @basis: (nullable): the valuation basis; %NULL for conservative
 * @account: (nullable): one account's holdings, by key
 * @place: (nullable): one place's (bag, bank, warbank, mail, ...)
 * @category: (nullable): this category path and beneath
 * @search: (nullable): instruments whose name or key contains this
 * @min_value: (nullable): only instruments worth at least this in total;
 *   it must name its currency
 * @dead: only instruments with no sale in the ledger for @dead_days
 * @dead_days: what "dead" means; 0 for
 *   %VENTURE_MARKETDATA_ACCOUNTS_DEAD_DAYS
 * @sort: (nullable): one of the store's value sorts (value, quantity,
 *   name, unit_value, accounts, days_of_supply); %NULL for value
 * @descending: largest first
 * @page: the page, from 1; 0 means 1
 * @per_page: rows a page, 0 for 50, at most
 *   %VENTURE_MARKETDATA_ACCOUNTS_MAX_PAGE
 * @now: the moment; 0 for the clock
 * @login: (nullable): the holdings of the accounts reached through one
 *   login, by key
 * @group_by: (nullable): "login" for the holdings' value login by login
 *   beside the page; %NULL for none
 * @show_ignored: count the accounts the organization ignores (see
 *   account_ignore) as well, each row marked; by default they are left out
 *
 * What the inventory page asks.
 */
typedef struct
{
	gint64		 organization_id;
	gint64		 data_source_id;
	const gchar	*basis;
	const gchar	*account;
	const gchar	*place;
	const gchar	*category;
	const gchar	*search;
	const gchar	*min_value;
	gboolean	 dead;
	gint64		 dead_days;
	const gchar	*sort;
	gboolean	 descending;
	guint		 page;
	guint		 per_page;
	gint64		 now;
	const gchar	*login;
	const gchar	*group_by;
	gboolean	 show_ignored;
} VentureMarketdataInventoryQuery;

/**
 * venture_marketdata_inventory_query_init:
 * @query: (out caller-allocates): a query
 *
 * Everything held, the most valuable first, the first page.
 */
void
venture_marketdata_inventory_query_init(VentureMarketdataInventoryQuery *query);

/**
 * venture_marketdata_inventory_sorts:
 *
 * Returns: (transfer none) (array zero-terminated=1): the inventory's
 *   orders, each computed in the store's SQL
 */
const gchar *const *
venture_marketdata_inventory_sorts(void);

/**
 * venture_marketdata_inventory:
 * @context: a #VentureContext
 * @query: what to list
 * @error: (out) (optional): return location for a #GError
 *
 * One page of what the operator holds, per instrument across every
 * account: {available, notes, attribution, sources, data_source_id,
 * source_name, currency, basis, bases, sorts, query, accounts, places,
 * page, per_page, pages, totals, portfolio_value, rows}. Each row has its
 * place-by-place breakdown, its share of the portfolio and its days of
 * supply where units sold a day are known.
 *
 * Returns: (transfer full) (nullable): the answer, or %NULL on error
 */
JsonNode *
venture_marketdata_inventory(
	VentureContext				 *context,
	const VentureMarketdataInventoryQuery	 *query,
	GError					**error
);

/**
 * VentureMarketdataPnlQuery:
 * @organization_id: whose sources
 * @data_source_id: the source, or 0 for the organization's first source
 *   that has accounts
 * @group_by: (nullable): day, week, month, account, venue, instrument,
 *   source or login; %NULL for day
 * @since: the window's start, inclusive, Unix seconds; 0 for the start of
 *   the thirty whole days ending on @now's day (midnight UTC, the window
 *   period=last_30_days names), negative for no start
 * @until: its end, exclusive; 0 for the end of @now's day (midnight UTC),
 *   negative for no end
 * @account: (nullable): one account's rows
 * @venue: (nullable): one venue's
 * @instrument: (nullable): one instrument's
 * @source: (nullable): one of the source's own labels ("Auction",
 *   "Vendor")
 * @top: how many top items and flips to list; 0 for 20, at most
 *   %VENTURE_MARKETDATA_ACCOUNTS_MAX_PAGE
 * @now: the moment; 0 for the clock
 * @login: (nullable): the rows of the accounts reached through one login,
 *   by key
 * @show_ignored: count the accounts the organization ignores (see
 *   account_ignore) as well, each row marked; by default they are left out
 *
 * What the profit and loss page asks.
 */
typedef struct
{
	gint64		 organization_id;
	gint64		 data_source_id;
	const gchar	*group_by;
	gint64		 since;
	gint64		 until;
	const gchar	*account;
	const gchar	*venue;
	const gchar	*instrument;
	const gchar	*source;
	guint		 top;
	gint64		 now;
	const gchar	*login;
	gboolean	 show_ignored;
} VentureMarketdataPnlQuery;

/**
 * venture_marketdata_pnl_query_init:
 * @query: (out caller-allocates): a query
 *
 * The last thirty days by day, everything.
 */
void
venture_marketdata_pnl_query_init(VentureMarketdataPnlQuery *query);

/**
 * venture_marketdata_external_pnl:
 * @context: a #VentureContext
 * @query: what to sum
 * @error: (out) (optional): return location for a #GError
 *
 * The source's external ledger summed for a profit and loss, the way a
 * trading addon's own report reads it (sales after the venue's cut,
 * purchases, other income and expenses, net): {available, notes,
 * attribution, sources, data_source_id, source_name, currency, since,
 * until, group_by, groups, totals, buckets, trend, top_items, flips}.
 * Every figure is an integer sum; nothing converts a currency.
 *
 * Returns: (transfer full) (nullable): the answer, or %NULL on error
 *   (INVALID_ARGUMENT for an unknown grouping or a window too large to
 *   answer whole)
 */
JsonNode *
venture_marketdata_external_pnl(
	VentureContext				 *context,
	const VentureMarketdataPnlQuery		 *query,
	GError					**error
);

/**
 * venture_marketdata_account_path:
 * @data_source_id: the source
 * @key: the account's key
 *
 * Returns: (transfer full): the account page's path,
 *   "/accounts/S/<key escaped, '/' included>"
 */
gchar *
venture_marketdata_account_path(
	gint64		 data_source_id,
	const gchar	*key
);

G_END_DECLS

#endif /* VENTURE_MARKETDATA_ACCOUNTS_H */
