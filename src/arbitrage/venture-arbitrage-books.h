/*
 * venture-arbitrage-books.h - An external ledger in the books
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A data source that pushes the operator's own ledger (venture-series-
 * accounts.h: sales after the venue's cut, purchases, other income and
 * expenses, per account) keeps it in its series store as history. Two
 * doors take it into the general journal, and a source chooses one of
 * them with its `books` setting, so nothing is ever booked twice:
 *
 *  - `books: daily` -- post_ledger. One summary journal per account per
 *    day through the posting service: the day's sales and other income
 *    credited, its purchases and expenses debited, the net through the
 *    account's holding (its promoted location's purse). A day posts once;
 *    a day whose rows changed since is reversed and posted again. With
 *    `post_to_books: true` the feeds run hook does it after every run.
 *  - `books: trades` -- record_flips. Each sale the store can match to
 *    earlier buys, first in first out, becomes a closed arbitrage trade
 *    with executed buy and sell legs, once.
 *
 * Both are type-level actions on the books' own types (post_ledger on
 * external_posting, record_flips on arbitrage_trade) naming the source by
 * data_source_id, so they are judged as financial work (an organization's
 * finance member, never an editor member), and each runs inside one
 * whole-operation posting boundary. docs/market-data.org ("The
 * external ledger in the books") and docs/arbitrage.org ("Flips from an
 * external ledger") have every rule.
 */

#ifndef VENTURE_ARBITRAGE_BOOKS_H
#define VENTURE_ARBITRAGE_BOOKS_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/**
 * VENTURE_ARBITRAGE_BOOKS_HOOK:
 *
 * The feeds hook's name that posts a `post_to_books` source after each
 * run; also the actor name it posts as.
 */
#define VENTURE_ARBITRAGE_BOOKS_HOOK "books"

/**
 * VENTURE_ARBITRAGE_BOOKS_RULE:
 *
 * The rule name prefix of every journal an external ledger posts; the
 * rest is the external_posting record's id ("external_ledger:12"), which
 * is what finds a day's journal again to reverse it.
 */
#define VENTURE_ARBITRAGE_BOOKS_RULE "external_ledger"

/**
 * VENTURE_ARBITRAGE_BOOKS_CAPITAL_RULE:
 *
 * The rule name of a journal that brings money the external ledger never
 * showed arriving into a purse before a recorded flip spends it.
 */
#define VENTURE_ARBITRAGE_BOOKS_CAPITAL_RULE "external_ledger_capital"

/**
 * VENTURE_ARBITRAGE_BOOKS_MAX_WRITES:
 *
 * How many journals (postings and reversals) one post_ledger pass writes
 * by default before it leaves the rest for the next; the source's
 * `books_max_writes` setting changes it, up to
 * %VENTURE_ARBITRAGE_BOOKS_WRITES_LIMIT.
 */
#define VENTURE_ARBITRAGE_BOOKS_MAX_WRITES (200)

/**
 * VENTURE_ARBITRAGE_BOOKS_WRITES_LIMIT:
 *
 * The most `books_max_writes` may say.
 */
#define VENTURE_ARBITRAGE_BOOKS_WRITES_LIMIT (5000)

/**
 * VENTURE_ARBITRAGE_BOOKS_MAX_RECORDS:
 *
 * The most posting records or recorded flips of one source a pass reads;
 * past it the pass is refused rather than run on part of the history.
 */
#define VENTURE_ARBITRAGE_BOOKS_MAX_RECORDS (100000)

/**
 * VENTURE_ARBITRAGE_FLIPS_DEFAULT_LIMIT:
 *
 * How many trades one record_flips call records unless told otherwise.
 */
#define VENTURE_ARBITRAGE_FLIPS_DEFAULT_LIMIT (100)

/**
 * VENTURE_ARBITRAGE_FLIPS_MAX_LIMIT:
 *
 * The most trades one record_flips call may record.
 */
#define VENTURE_ARBITRAGE_FLIPS_MAX_LIMIT (500)

/**
 * VentureBooksPostQuery:
 * @organization_id: the organization; <= 0 is the default one
 * @data_source_id: the data source
 * @from: (nullable): the first day new days are taken from; %NULL is the
 *   source's `books_from` setting, else its whole ledger
 * @until: (nullable): the day new days stop before (exclusive); %NULL is
 *   the start of today, UTC -- a day still going on is not over
 * @account_key: (nullable): one account's days only
 * @dry_run: plan only: say what would be posted and write nothing
 * @max_writes: journals to write at most; 0 is the source's setting
 *
 * What venture_arbitrage_books_post() is asked. Days already in the books
 * are kept current whatever the window: the window only chooses which new
 * days come in.
 */
typedef struct
{
	gint64		 organization_id;
	gint64		 data_source_id;
	GDateTime	*from;
	GDateTime	*until;
	const gchar	*account_key;
	gboolean	 dry_run;
	guint		 max_writes;
} VentureBooksPostQuery;

/**
 * venture_arbitrage_books_post:
 * @context: the wiring
 * @query: what to post
 * @actor: (nullable): who; the operation's consent is theirs
 * @out_report: (out) (transfer full) (optional): what it did, as JSON:
 *   counts, the days with their figures and status, notes
 * @error: (out) (optional): return location for a #GError
 *
 * Posts a data source's external ledger as daily summary journals (see
 * the file's comment). Refused (CONFIG) unless the source books `daily`
 * -- a dry run is answered in any mode -- or when the arbitrage, ledger,
 * market data or feeds modules are off or there is no SQLite; NOT_FOUND
 * for a source not the organization's. Everything it writes is one
 * transaction inside one whole-operation posting boundary: a refusal
 * (the holding floor, when another document spent the purse) leaves
 * nothing written. A day in a closed period is kept as posted, or left
 * unposted, with a note.
 *
 * Returns: %TRUE on success
 */
gboolean
venture_arbitrage_books_post(
	VentureContext			 *context,
	const VentureBooksPostQuery	 *query,
	const VentureActor		 *actor,
	JsonNode			**out_report,
	GError				**error
);

/**
 * VentureBooksFlipsQuery:
 * @organization_id: the organization; <= 0 is the default one
 * @data_source_id: the data source
 * @from: (nullable): only sales at or after this; %NULL is the source's
 *   `books_from` setting, else every sale
 * @until: (nullable): only sales before this; %NULL is now
 * @instrument_key: (nullable): one instrument's flips only
 * @min_profit: (nullable): only flips that made at least this, in the
 *   source's currency
 * @limit: trades to record at most; 0 is
 *   %VENTURE_ARBITRAGE_FLIPS_DEFAULT_LIMIT
 * @dry_run: say what would be recorded and write nothing
 *
 * What venture_arbitrage_books_record_flips() is asked.
 */
typedef struct
{
	gint64			 organization_id;
	gint64			 data_source_id;
	GDateTime		*from;
	GDateTime		*until;
	const gchar		*instrument_key;
	const VentureMoney	*min_profit;
	guint			 limit;
	gboolean		 dry_run;
} VentureBooksFlipsQuery;

/**
 * venture_arbitrage_books_record_flips:
 * @context: the wiring
 * @query: what to record
 * @actor: (nullable): who
 * @out_report: (out) (transfer full) (optional): what it did, as JSON
 * @error: (out) (optional): return location for a #GError
 *
 * Records the flips of a data source's external ledger as closed
 * arbitrage trades, one per sale: the buys the sale drew on, first in
 * first out from the start of the source's books (`books_from`), as buy
 * legs at the buyer's purse, and the sale as one sell leg at the seller's,
 * closed at the sale. Each trade carries `external_ref`
 * "<source uuid>:<sale key>:<n>", and what each ledger row gave to a
 * trade is kept in the trade's `expected`, so recording again never
 * records a unit twice. Refused (CONFIG) unless the source books
 * `trades` (a dry run is answered in any mode). One transaction.
 *
 * Returns: %TRUE on success
 */
gboolean
venture_arbitrage_books_record_flips(
	VentureContext			 *context,
	const VentureBooksFlipsQuery	 *query,
	const VentureActor		 *actor,
	JsonNode			**out_report,
	GError				**error
);

/**
 * venture_arbitrage_books_report:
 * @context: the wiring
 * @period: the days asked about
 * @options: data_source_id (required), account_key, organization_id
 * @error: (out) (optional): return location for a #GError
 *
 * The `external_books` report: every day of the period that has money in
 * a source's external ledger, per account, and where it stands in the
 * books -- posted, changed since it was posted, not posted, left out
 * because flips of that day are recorded as trades, or kept as posted
 * because retention has removed its rows. It is a dry run of
 * venture_arbitrage_books_post() over the period, so the report and the
 * action cannot disagree.
 *
 * Returns: (transfer full) (nullable): the result
 */
VentureReportResult *
venture_arbitrage_books_report(
	VentureContext		 *context,
	VentureDateRange	 *period,
	JsonObject		 *options,
	GError			**error
);

/**
 * venture_arbitrage_books_check_write:
 * @database: the database
 * @entity: a record being written or removed
 * @removal: %TRUE for a delete, restore or purge
 * @error: (out) (optional): return location for a #GError
 *
 * The database's subsystem guard for `external_posting`: a removal is
 * refused, because the record is how the books know a day is posted --
 * without it the next pass would post the day again beside its journal.
 *
 * Returns: %TRUE when the write may go ahead
 */
gboolean
venture_arbitrage_books_check_write(
	VentureDatabase	 *database,
	VentureEntity	 *entity,
	gboolean	  removal,
	GError		**error
);

/**
 * venture_arbitrage_books_install:
 * @context: the context
 *
 * The data source settings validator, the external_posting validator, the
 * post_ledger and record_flips type-level actions (once per database) and, with
 * SQLite, the feeds hook that posts `post_to_books` sources after a run.
 * After the mirror's install, so a run's accounts are places first.
 */
void
venture_arbitrage_books_install(VentureContext *context);

G_END_DECLS

#endif /* VENTURE_ARBITRAGE_BOOKS_H */
