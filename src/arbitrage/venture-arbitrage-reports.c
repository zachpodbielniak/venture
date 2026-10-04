/*
 * venture-arbitrage-reports.c - arbitrage_performance
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * How finished trades did. A trade is finished when it is closed or
 * abandoned; the period bounds when it finished. Every figure is a trade's
 * per-currency figures from venture_arbitrage_compute_figures(), the same
 * arithmetic the trade page shows, so a row and a page cannot disagree.
 *
 * One row per group and currency, the book currency first in each group,
 * and nothing is ever added across two currencies. Counts that belong to a
 * group (the average hold) repeat on each of its currency rows.
 */

#include "venture.h"
#include "arbitrage/venture-arbitrage-private.h"
#include "arbitrage/venture-arbitrage-engine-private.h"

#include <string.h>

typedef enum
{
	ARB_GROUP_STRATEGY = 0,
	ARB_GROUP_VENUE_PAIR,
	ARB_GROUP_INSTRUMENT,
	ARB_GROUP_MONTH
} ArbGroup;

/* One currency of one group. */
typedef struct
{
	gchar		*currency;
	guint		 trades;
	guint		 wins;
	VentureMoney	*realised;
	VentureMoney	*capital;
	VentureMoney	*fees;
	/* Only over trades whose snapshot named an expected profit in this
	 * currency: slippage compares like with like. */
	guint		 expected_trades;
	VentureMoney	*expected;
	VentureMoney	*realised_expected;
} ArbCell;

typedef struct
{
	gchar		*label;
	GPtrArray	*cells;
	guint		 trades;
	gint64		 hold_seconds;
	guint		 holds;
} ArbRow;

static void
arb_cell_free(gpointer data)
{
	ArbCell *cell;

	cell = data;
	g_free(cell->currency);
	g_clear_pointer(&cell->realised, venture_money_free);
	g_clear_pointer(&cell->capital, venture_money_free);
	g_clear_pointer(&cell->fees, venture_money_free);
	g_clear_pointer(&cell->expected, venture_money_free);
	g_clear_pointer(&cell->realised_expected, venture_money_free);
	g_free(cell);
}

static void
arb_row_free(gpointer data)
{
	ArbRow *row;

	row = data;
	g_free(row->label);
	g_ptr_array_unref(row->cells);
	g_free(row);
}

static gboolean
arb_add(
	VentureMoney		**sum,
	const VentureMoney	 *amount,
	GError			**error
){
	VentureMoney *total;

	if (NULL == amount)
		return TRUE;

	if (NULL == *sum)
	{
		*sum = venture_money_copy(amount);
		return TRUE;
	}

	total = venture_money_add(*sum, amount, error);

	if (NULL == total)
		return FALSE;

	venture_money_free(*sum);
	*sum = total;

	return TRUE;
}

static ArbCell *
arb_cell(
	ArbRow		*row,
	const gchar	*currency
){
	ArbCell *cell;
	guint i;

	for (i = 0; i < row->cells->len; i++)
	{
		cell = g_ptr_array_index(row->cells, i);

		if (0 == g_strcmp0(cell->currency, currency))
			return cell;
	}

	cell = g_new0(ArbCell, 1);
	cell->currency = g_strdup(currency);
	g_ptr_array_add(row->cells, cell);

	return cell;
}

/* The organization: options' organization_id, else the default. */
static gboolean
arb_report_organization(
	VentureContext	 *context,
	JsonObject	 *options,
	gint64		 *out_id,
	GError		**error
){
	g_autoptr(VentureEntity) organization = NULL;
	gint64 id;

	id = venture_context_get_default_organization_id(context);

	if ((NULL != options) && json_object_has_member(options, "organization_id"))
		id = venture_json_object_get_int(options, "organization_id", id);

	organization = venture_database_get(venture_context_get_database(context),
	                                    VENTURE_TYPE_ORGANIZATION, id, NULL);

	if ((NULL == organization) || venture_entity_is_deleted(organization))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		            "Organization #%" G_GINT64_FORMAT " not found", id);
		return FALSE;
	}

	*out_id = id;

	return TRUE;
}

/* A record's display name, cached by type and id for the run. */
static const gchar *
arb_cached_name(
	VentureDatabase	*database,
	GHashTable	*names,
	GType		 type,
	const gchar	*noun,
	gint64		 id
){
	g_autofree gchar *key = NULL;
	const gchar *found;
	gchar *name;

	key = g_strdup_printf("%s:%" G_GINT64_FORMAT, noun, id);
	found = g_hash_table_lookup(names, key);

	if (NULL != found)
		return found;

	{
		g_autoptr(VentureEntity) record = NULL;

		record = venture_database_get(database, type, id, NULL);
		name = (NULL != record) ? venture_entity_get_display_name(record)
		                        : g_strdup_printf("%s #%" G_GINT64_FORMAT, noun, id);
	}

	g_hash_table_insert(names, g_steal_pointer(&key), name);

	return name;
}

/* The group a finished trade falls in, as a label. */
static gchar *
arb_group_of(
	VentureDatabase	*database,
	GHashTable	*names,
	ArbGroup	 group,
	VentureEntity	*trade,
	GPtrArray	*legs
){
	guint i;

	switch (group)
	{
	case ARB_GROUP_VENUE_PAIR:
	{
		gint64 from;
		gint64 to;

		from = 0;
		to = 0;

		/* Where the money went out first, and where it came back first. */
		for (i = 0; i < legs->len; i++)
		{
			VentureEntity *leg;
			g_autoptr(VentureMoney) amount = NULL;
			gint64 venue;
			gint kind;
			gint status;
			gint direction;

			leg = g_ptr_array_index(legs, i);

			status = 0;
			g_object_get(leg, "status", &status, "venue-id", &venue, "kind", &kind,
			             "amount", &amount, NULL);

			if (VENTURE_ARBITRAGE_LEG_STATUS_EXECUTED != status)
				continue;

			direction = venture_arbitrage_leg_direction((VentureArbitrageLegKind)kind, amount);

			if ((direction > 0) && (0 == from))
				from = venue;
			else if ((direction < 0) && (0 == to))
				to = venue;
		}

		if ((from <= 0) && (to <= 0))
			return g_strdup("No venue");

		if ((from > 0) && (to > 0) && (from != to))
			return g_strdup_printf("%s \xe2\x86\x92 %s",
				arb_cached_name(database, names, VENTURE_TYPE_VENUE, "Venue", from),
				arb_cached_name(database, names, VENTURE_TYPE_VENUE, "Venue", to));

		return g_strdup(arb_cached_name(database, names, VENTURE_TYPE_VENUE, "Venue",
		                                (from > 0) ? from : to));
	}

	case ARB_GROUP_INSTRUMENT:
		for (i = 0; i < legs->len; i++)
		{
			gint64 instrument;

			instrument = 0;
			g_object_get(g_ptr_array_index(legs, i), "instrument-id", &instrument, NULL);

			if (instrument > 0)
				return g_strdup(arb_cached_name(database, names, VENTURE_TYPE_INSTRUMENT,
				                                "Instrument", instrument));
		}

		return g_strdup("No instrument");

	case ARB_GROUP_MONTH:
	{
		g_autoptr(GDateTime) closed = NULL;
		g_autoptr(GDateTime) utc = NULL;

		g_object_get(trade, "closed-at", &closed, NULL);

		if (NULL == closed)
			return g_strdup("Not closed");

		/* Months are UTC, like every period boundary. */
		utc = g_date_time_to_utc(closed);

		return g_date_time_format(utc, "%Y-%m");
	}

	case ARB_GROUP_STRATEGY:
	default:
	{
		g_autofree gchar *strategy = NULL;

		g_object_get(trade, "strategy", &strategy, NULL);

		return venture_string_is_empty(strategy) ? g_strdup("No strategy")
		                                         : g_steal_pointer(&strategy);
	}
	}
}

static gint
arb_row_compare(
	gconstpointer	a,
	gconstpointer	b
){
	const ArbRow *left = *(ArbRow *const *)a;
	const ArbRow *right = *(ArbRow *const *)b;

	return g_utf8_collate(left->label, right->label);
}

/* Book currency first (@book), then by code. */
static gint
arb_cell_compare(
	gconstpointer	a,
	gconstpointer	b,
	gpointer	book
){
	const ArbCell *left = *(ArbCell *const *)a;
	const ArbCell *right = *(ArbCell *const *)b;
	gboolean left_book;
	gboolean right_book;

	left_book = (0 == g_strcmp0(left->currency, book));
	right_book = (0 == g_strcmp0(right->currency, book));

	if (left_book != right_book)
		return left_book ? -1 : 1;

	return g_strcmp0(left->currency, right->currency);
}

/* Adds one finished trade to @row. */
static gboolean
arb_row_add(
	ArbRow		 *row,
	VentureEntity	 *trade,
	GPtrArray	 *legs,
	GError		**error
){
	g_autoptr(GPtrArray) figures = NULL;
	g_autoptr(GPtrArray) expected = NULL;
	g_autoptr(GDateTime) opened = NULL;
	g_autoptr(GDateTime) closed = NULL;
	g_autofree gchar *snapshot = NULL;
	guint i;

	figures = venture_arbitrage_compute_figures(legs, error);

	if (NULL == figures)
		return FALSE;

	g_object_get(trade, "opened-at", &opened, "closed-at", &closed, "expected", &snapshot, NULL);
	expected = venture_arbitrage_expected_profit(snapshot);

	row->trades++;

	if ((NULL != opened) && (NULL != closed) && (g_date_time_compare(closed, opened) >= 0))
	{
		row->hold_seconds += g_date_time_difference(closed, opened) / G_TIME_SPAN_SECOND;
		row->holds++;
	}

	for (i = 0; i < figures->len; i++)
	{
		const VentureArbitrageFigures *figure;
		const VentureMoney *forecast;
		ArbCell *cell;

		figure = g_ptr_array_index(figures, i);
		cell = arb_cell(row, figure->currency);
		cell->trades++;

		if (!venture_money_is_zero(figure->realised) && !venture_money_is_negative(figure->realised))
			cell->wins++;

		if (!arb_add(&cell->realised, figure->realised, error) ||
		    !arb_add(&cell->capital, figure->capital, error) ||
		    !arb_add(&cell->fees, figure->fees, error))
			return FALSE;

		forecast = venture_money_totals_lookup(expected, figure->currency);

		if (NULL != forecast)
		{
			cell->expected_trades++;

			if (!arb_add(&cell->expected, forecast, error) ||
			    !arb_add(&cell->realised_expected, figure->realised, error))
				return FALSE;
		}
	}

	return TRUE;
}

gboolean
venture_arbitrage_realised_totals(
	VentureContext		 *context,
	gint64			  organization_id,
	gint64			  venture_id,
	VentureDateRange	 *period,
	GDateTime		 *as_of,
	GPtrArray		**out_gains,
	GPtrArray		**out_fees,
	GPtrArray		**out_realised,
	guint			 *out_trades,
	GError			**error
){
	g_autoptr(GPtrArray) gains = NULL;
	g_autoptr(GPtrArray) fees = NULL;
	g_autoptr(GPtrArray) realised = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) statuses = NULL;
	g_autoptr(GPtrArray) trades = NULL;
	VentureDatabase *database;
	guint i;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), FALSE);
	g_return_val_if_fail((NULL != out_gains) && (NULL != out_fees) && (NULL != out_realised), FALSE);

	gains = venture_money_totals_new();
	fees = venture_money_totals_new();
	realised = venture_money_totals_new();

	if (NULL != out_trades)
		*out_trades = 0;

	/* Off is nothing to add, and the table may never have been made. */
	if (!venture_context_module_enabled(context, "arbitrage"))
	{
		*out_gains = g_steal_pointer(&gains);
		*out_fees = g_steal_pointer(&fees);
		*out_realised = g_steal_pointer(&realised);
		return TRUE;
	}

	database = venture_context_get_database(context);

	if (organization_id <= 0)
		organization_id = venture_context_get_default_organization_id(context);

	/* The performance report's question: finished trades, by when they
	 * finished, narrowed in the query so the bound counts what was asked. */
	query = venture_query_new(VENTURE_TYPE_ARBITRAGE_TRADE);
	venture_query_set_organization(query, organization_id);
	venture_query_set_limit(query, (guint)venture_aggregate_get_max_rows() + 1);

	/* The report's as_of, as its sales and expenses get it: a P&L asked
	 * "as of" a date counts what was visible then. */
	if (NULL != as_of)
		venture_query_set_as_of(query, as_of);

	statuses = g_ptr_array_new_with_free_func(g_free);
	g_ptr_array_add(statuses, g_strdup("closed"));
	g_ptr_array_add(statuses, g_strdup("abandoned"));

	if (!venture_query_add_filter(query, "status", VENTURE_FILTER_OP_IN, statuses, error) ||
	    ((NULL != period) && !venture_query_set_date_range(query, "closed-at", period, error)) ||
	    ((0 != venture_id) &&
	     !venture_query_add_filter_int(query, "venture-id", VENTURE_FILTER_OP_EQ, venture_id, error)))
		return FALSE;

	trades = venture_database_find(database, query, error);

	if (NULL == trades)
		return FALSE;

	if (trades->len > (guint)venture_aggregate_get_max_rows())
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "More than %d finished arbitrage trades fall in the period; narrow the period "
		            "or the venture", venture_aggregate_get_max_rows());
		return FALSE;
	}

	for (i = 0; i < trades->len; i++)
	{
		g_autoptr(GPtrArray) legs = NULL;
		g_autoptr(GPtrArray) figures = NULL;
		guint j;

		legs = venture_arbitrage_trade_legs(database, venture_entity_get_id(g_ptr_array_index(trades, i)),
		                                    TRUE, error);
		figures = (NULL != legs) ? venture_arbitrage_compute_figures(legs, error) : NULL;

		if (NULL == figures)
			return FALSE;

		for (j = 0; j < figures->len; j++)
		{
			const VentureArbitrageFigures *row = g_ptr_array_index(figures, j);
			g_autoptr(VentureMoney) before_fees = NULL;

			/* Before fees is what the close journal moved to the gains
			 * account; the fees went to their own account as each leg
			 * was executed. */
			before_fees = venture_money_add(row->realised, row->fees, error);

			if ((NULL == before_fees) ||
			    !venture_money_totals_add(gains, before_fees, error) ||
			    !venture_money_totals_add(fees, row->fees, error) ||
			    !venture_money_totals_add(realised, row->realised, error))
				return FALSE;
		}
	}

	if (NULL != out_trades)
		*out_trades = trades->len;

	*out_gains = g_steal_pointer(&gains);
	*out_fees = g_steal_pointer(&fees);
	*out_realised = g_steal_pointer(&realised);

	return TRUE;
}

VentureReportResult *
venture_arbitrage_performance(
	VentureContext		 *context,
	VentureDateRange	 *period,
	JsonObject		 *options,
	GError			**error
){
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) trades = NULL;
	g_autoptr(GPtrArray) statuses = NULL;
	g_autoptr(GPtrArray) ordered = NULL;
	g_autoptr(GHashTable) rows = NULL;
	g_autoptr(GHashTable) names = NULL;
	g_autofree gchar *book = NULL;
	VentureDatabase *database;
	const gchar *group_by;
	const gchar *group_label;
	const gchar *strategy;
	ArbGroup group;
	gint64 organization_id;
	gint64 venture_id;
	GHashTableIter iter;
	gpointer value;
	guint i;
	guint j;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);

	database = venture_context_get_database(context);

	if (!arb_report_organization(context, options, &organization_id, error))
		return NULL;

	/* --- The question --- */

	group_by = (NULL != options) ? venture_json_object_get_string(options, "group_by", NULL) : NULL;

	if (venture_string_is_empty(group_by) || (0 == g_strcmp0(group_by, "strategy")))
	{
		group = ARB_GROUP_STRATEGY;
		group_label = "Strategy";
	}
	else if (0 == g_strcmp0(group_by, "venue_pair"))
	{
		group = ARB_GROUP_VENUE_PAIR;
		group_label = "Venues";
	}
	else if (0 == g_strcmp0(group_by, "instrument"))
	{
		group = ARB_GROUP_INSTRUMENT;
		group_label = "Instrument";
	}
	else if (0 == g_strcmp0(group_by, "month"))
	{
		group = ARB_GROUP_MONTH;
		group_label = "Month";
	}
	else
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "arbitrage_performance groups by strategy, venue_pair, instrument "
		            "or month, not \"%s\"", group_by);
		return NULL;
	}

	strategy = (NULL != options) ? venture_json_object_get_string(options, "strategy", NULL) : NULL;
	venture_id = (NULL != options) ? venture_json_object_get_int(options, "venture_id", 0) : 0;

	/* --- Finished trades, by when they finished --- */

	query = venture_query_new(VENTURE_TYPE_ARBITRAGE_TRADE);
	venture_query_set_organization(query, organization_id);
	venture_query_set_limit(query, (guint)venture_aggregate_get_max_rows() + 1);
	statuses = g_ptr_array_new_with_free_func(g_free);
	g_ptr_array_add(statuses, g_strdup("closed"));
	g_ptr_array_add(statuses, g_strdup("abandoned"));

	if (!venture_query_add_filter(query, "status", VENTURE_FILTER_OP_IN, statuses, error))
		return NULL;

	if ((NULL != period) && !venture_query_set_date_range(query, "closed-at", period, error))
		return NULL;

	/* Narrowed in the query, before the bound counts: a filter applied
	 * after the fetch would refuse an organization past the bound however
	 * narrowly it asked. */
	if (!venture_string_is_empty(strategy) &&
	    !venture_query_add_filter_string(query, "strategy", VENTURE_FILTER_OP_EQ, strategy, error))
		return NULL;

	if ((0 != venture_id) &&
	    !venture_query_add_filter_int(query, "venture-id", VENTURE_FILTER_OP_EQ, venture_id, error))
		return NULL;

	if (!venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, error))
		return NULL;

	trades = venture_database_find(database, query, error);

	if (NULL == trades)
		return NULL;

	if (trades->len > (guint)venture_aggregate_get_max_rows())
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "More than %d finished trades match; narrow the period, the "
		            "strategy or the venture", venture_aggregate_get_max_rows());
		return NULL;
	}

	/* --- Grouped --- */

	rows = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, arb_row_free);
	names = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

	for (i = 0; i < trades->len; i++)
	{
		VentureEntity *trade;
		g_autoptr(GPtrArray) legs = NULL;
		g_autofree gchar *label = NULL;
		ArbRow *row;

		trade = g_ptr_array_index(trades, i);
		legs = venture_arbitrage_trade_legs(database, venture_entity_get_id(trade), TRUE, error);

		if (NULL == legs)
			return NULL;

		label = arb_group_of(database, names, group, trade, legs);
		row = g_hash_table_lookup(rows, label);

		if (NULL == row)
		{
			row = g_new0(ArbRow, 1);
			row->label = g_strdup(label);
			row->cells = g_ptr_array_new_with_free_func(arb_cell_free);
			g_hash_table_insert(rows, g_steal_pointer(&label), row);
		}

		if (!arb_row_add(row, trade, legs, error))
			return NULL;
	}

	/* --- The answer --- */

	result = venture_report_result_new("Arbitrage performance", period);
	venture_report_result_add_column(result, "group", group_label, VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "currency", "Currency", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "trades", "Trades", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "wins", "Profitable", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "hit_rate", "Hit rate", VENTURE_REPORT_COLUMN_PERCENT);
	venture_report_result_add_column(result, "realised", "Realised", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "capital", "Capital deployed", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "roi", "ROI", VENTURE_REPORT_COLUMN_PERCENT);
	venture_report_result_add_column(result, "fees", "Fees", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "avg_hold_days", "Average hold (days)", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "expected", "Expected", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "slippage", "Realised less expected", VENTURE_REPORT_COLUMN_MONEY);

	book = venture_database_get_book_currency(database, organization_id);
	ordered = g_ptr_array_new();
	g_hash_table_iter_init(&iter, rows);

	while (g_hash_table_iter_next(&iter, NULL, &value))
		g_ptr_array_add(ordered, value);

	g_ptr_array_sort(ordered, arb_row_compare);

	for (i = 0; i < ordered->len; i++)
	{
		ArbRow *row;

		row = g_ptr_array_index(ordered, i);
		g_ptr_array_sort_with_data(row->cells, arb_cell_compare, book);

		/* A finished trade that moved no money at all still finished. */
		if (0 == row->cells->len)
		{
			venture_report_result_begin_row(result);
			venture_report_result_set_text(result, "group", row->label);
			venture_report_result_set_number(result, "trades", (gdouble)row->trades);
			continue;
		}

		for (j = 0; j < row->cells->len; j++)
		{
			ArbCell *cell;

			cell = g_ptr_array_index(row->cells, j);

			venture_report_result_begin_row(result);
			venture_report_result_set_text(result, "group", row->label);
			venture_report_result_set_text(result, "currency", cell->currency);
			venture_report_result_set_number(result, "trades", (gdouble)cell->trades);
			venture_report_result_set_number(result, "wins", (gdouble)cell->wins);

			if (cell->trades > 0)
				venture_report_result_set_number(result, "hit_rate",
					(gdouble)cell->wins / (gdouble)cell->trades);

			if (NULL != cell->realised)
				venture_report_result_set_money(result, "realised", cell->realised);

			if (NULL != cell->capital)
				venture_report_result_set_money(result, "capital", cell->capital);

			if (NULL != cell->fees)
				venture_report_result_set_money(result, "fees", cell->fees);

			/* A ratio of two amounts in one currency; nothing deployed is
			 * no ROI, not an infinite one. */
			if ((NULL != cell->capital) && (NULL != cell->realised) &&
			    !venture_money_is_zero(cell->capital))
				venture_report_result_set_number(result, "roi",
					venture_money_to_double(cell->realised) /
					venture_money_to_double(cell->capital));

			if (row->holds > 0)
				venture_report_result_set_number(result, "avg_hold_days",
					((gdouble)row->hold_seconds / (gdouble)row->holds) / 86400.0);

			if (NULL != cell->expected)
			{
				g_autoptr(VentureMoney) slippage = NULL;

				venture_report_result_set_money(result, "expected", cell->expected);
				slippage = venture_money_subtract(cell->realised_expected, cell->expected, error);

				if (NULL == slippage)
					return NULL;

				venture_report_result_set_money(result, "slippage", slippage);
			}
		}
	}

	venture_report_result_append_note(result,
		"Finished trades only -- closed or abandoned -- in the period they finished. "
		"One row per group and currency; money is never added across currencies, "
		"the book currency comes first, and the average hold belongs to the group and "
		"repeats on each of its rows.");
	venture_report_result_append_note(result,
		"Realised is what came back less what went out, less the cost of stock that "
		"left (sold or written off) and less fees; money that became stock still held "
		"is not counted as spent. ROI is realised over capital deployed (money out, "
		"stock bought included). The hit rate is the share of a currency's trades that "
		"realised more than nothing.");
	venture_report_result_append_note(result,
		"Expected and realised less expected count only trades whose snapshot named an "
		"expected profit in that currency, so the comparison is like for like.");

	return g_steal_pointer(&result);
}

/* ==========================================================================
 * arbitrage_scan and craft_arbitrage
 *
 * The scan as a report, through every door: the same question
 * venture_arbitrage_scan_run() answers for /arbitrage, written as rows. A
 * scan reads the latest prices, so the period is not used and says so.
 * ========================================================================== */

static void
arb_scan_notes(
	VentureReportResult	*result,
	JsonObject		*root
){
	JsonArray *notes;
	JsonObject *excluded;
	g_autoptr(GList) members = NULL;
	g_autoptr(GString) left = NULL;
	GList *member;
	guint i;

	notes = json_object_get_array_member(root, "notes");

	for (i = 0; (NULL != notes) && (i < json_array_get_length(notes)); i++)
		venture_report_result_append_note(result, json_array_get_string_element(notes, i));

	excluded = json_object_get_object_member(root, "excluded");
	members = (NULL != excluded) ? json_object_get_members(excluded) : NULL;
	left = g_string_new(NULL);

	for (member = members; NULL != member; member = member->next)
	{
		if (left->len > 0)
			g_string_append(left, ", ");

		g_string_append_printf(left, "%s %" G_GINT64_FORMAT, (const gchar *)member->data,
		                       json_object_get_int_member(excluded, member->data));
	}

	if (left->len > 0)
	{
		g_autofree gchar *note = g_strdup_printf("Left out: %s.", left->str);

		venture_report_result_append_note(result, note);
	}

	venture_report_result_append_note(result,
		"A scan reads the latest prices; the period is not used.");
}

static void
arb_scan_set_money(
	VentureReportResult	*result,
	const gchar		*column,
	JsonObject		*object,
	const gchar		*member
){
	g_autoptr(VentureMoney) money = NULL;
	JsonNode *node;

	node = (NULL != object) ? json_object_get_member(object, member) : NULL;

	if ((NULL == node) || !JSON_NODE_HOLDS_OBJECT(node))
		return;

	money = venture_money_from_json(node, NULL, NULL);

	if (NULL != money)
		venture_report_result_set_money(result, column, money);
}

static void
arb_scan_set_ratio(
	VentureReportResult	*result,
	const gchar		*column,
	JsonObject		*object,
	const gchar		*member,
	gdouble			 scale
){
	JsonNode *node;

	node = (NULL != object) ? json_object_get_member(object, member) : NULL;

	if ((NULL == node) || !JSON_NODE_HOLDS_VALUE(node))
		return;

	venture_report_result_set_number(result, column, json_node_get_double(node) * scale);
}

static const gchar *
arb_scan_side(
	JsonObject	*row,
	const gchar	*side
){
	JsonObject *object;

	object = json_object_has_member(row, side) ? json_object_get_object_member(row, side) : NULL;

	return venture_json_object_get_string(object, "venue_name",
	                                      venture_json_object_get_string(object, "venue_key", NULL));
}

static gchar *
arb_scan_missing(JsonObject *row)
{
	g_autoptr(GString) text = NULL;
	JsonArray *missing;
	guint i;

	missing = json_object_has_member(row, "missing") ? json_object_get_array_member(row, "missing") : NULL;
	text = g_string_new(NULL);

	for (i = 0; (NULL != missing) && (i < json_array_get_length(missing)); i++)
	{
		if (i > 0)
			g_string_append(text, "; ");

		g_string_append(text, json_array_get_string_element(missing, i));
	}

	return g_string_free(g_steal_pointer(&text), FALSE);
}

static VentureReportResult *
venture_arbitrage_scan_report(
	VentureContext		 *context,
	VentureDateRange	 *period,
	JsonObject		 *options,
	GError			**error
){
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(JsonNode) answer = NULL;
	JsonObject *root;
	JsonArray *rows;
	gint64 organization_id;
	guint i;

	if (!arb_report_organization(context, options, &organization_id, error))
		return NULL;

	answer = venture_arbitrage_scan_run(context, organization_id, options, error);

	if (NULL == answer)
		return NULL;

	root = json_node_get_object(answer);
	rows = json_object_get_array_member(root, "rows");
	result = venture_report_result_new("Arbitrage scan", period);
	venture_report_result_add_column(result, "strategy", "Strategy", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "title", "Opportunity", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "buy_venue", "Buy at", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "sell_venue", "Sell at", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "units", "Units", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "currency", "Currency", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "capital", "Capital", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "net", "Net", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "roi", "ROI", VENTURE_REPORT_COLUMN_PERCENT);
	venture_report_result_add_column(result, "roi_per_day", "ROI a day", VENTURE_REPORT_COLUMN_PERCENT);
	venture_report_result_add_column(result, "annualized", "Annualised", VENTURE_REPORT_COLUMN_PERCENT);
	venture_report_result_add_column(result, "confidence", "Confidence", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "sale_rate", "Sale rate", VENTURE_REPORT_COLUMN_PERCENT);
	venture_report_result_add_column(result, "age_hours", "Data age (hours)", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "missing", "Unquoted", VENTURE_REPORT_COLUMN_TEXT);

	for (i = 0; (NULL != rows) && (i < json_array_get_length(rows)); i++)
	{
		JsonObject *row = json_array_get_object_element(rows, i);
		g_autofree gchar *missing = arb_scan_missing(row);
		const gchar *buy = arb_scan_side(row, "buy");
		const gchar *sell = arb_scan_side(row, "sell");

		venture_report_result_begin_row(result);
		venture_report_result_set_text(result, "strategy", venture_json_object_get_string(row, "strategy", ""));
		venture_report_result_set_text(result, "title", venture_json_object_get_string(row, "title", ""));

		if (NULL != buy)
			venture_report_result_set_text(result, "buy_venue", buy);

		if (NULL != sell)
			venture_report_result_set_text(result, "sell_venue", sell);

		venture_report_result_set_number(result, "units",
		                                 (gdouble)venture_json_object_get_int(row, "units", 0));

		if (NULL != venture_json_object_get_string(row, "currency", NULL))
			venture_report_result_set_text(result, "currency",
			                               venture_json_object_get_string(row, "currency", NULL));

		arb_scan_set_money(result, "capital", row, "capital");
		arb_scan_set_money(result, "net", row, "net");
		arb_scan_set_ratio(result, "roi", row, "roi", 1.0);
		arb_scan_set_ratio(result, "roi_per_day", row, "roi_per_day", 1.0);
		arb_scan_set_ratio(result, "annualized", row, "annualized", 1.0);
		arb_scan_set_ratio(result, "confidence", row, "confidence", 1.0);
		arb_scan_set_ratio(result, "sale_rate", row, "sale_rate", 1.0);
		venture_report_result_set_number(result, "age_hours",
			(gdouble)venture_json_object_get_int(row, "age_seconds", 0) / 3600.0);

		if ('\0' != missing[0])
			venture_report_result_set_text(result, "missing", missing);
	}

	arb_scan_notes(result, root);

	if (json_object_get_boolean_member(root, "truncated"))
		venture_report_result_append_note(result, "More opportunities passed the filters than top "
		                                          "asks for; raise top or narrow the question.");

	return g_steal_pointer(&result);
}

static gint
arb_shop_compare(
	gconstpointer	a,
	gconstpointer	b
){
	JsonObject *x = *(JsonObject *const *)a;
	JsonObject *y = *(JsonObject *const *)b;
	gint by_venue;

	/* A shopping list reads venue by venue; unquoted lines last. */
	by_venue = g_strcmp0(venture_json_object_get_string(x, "venue_name", "\xff"),
	                     venture_json_object_get_string(y, "venue_name", "\xff"));

	if (0 != by_venue)
		return by_venue;

	return g_strcmp0(venture_json_object_get_string(x, "name", ""),
	                 venture_json_object_get_string(y, "name", ""));
}

static VentureReportResult *
venture_arbitrage_craft_report(
	VentureContext		 *context,
	VentureDateRange	 *period,
	JsonObject		 *options,
	GError			**error
){
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(JsonNode) answer = NULL;
	g_autoptr(JsonObject) asked = NULL;
	g_autoptr(GList) members = NULL;
	JsonObject *root;
	JsonArray *rows;
	GList *member;
	gint64 organization_id;
	guint i;
	guint j;

	if (!arb_report_organization(context, options, &organization_id, error))
		return NULL;

	/* The transform strategy with nothing left out for being a loss: a
	 * craft that loses money is an answer too. */
	asked = json_object_new();

	if (NULL != options)
	{
		members = json_object_get_members(options);

		for (member = members; NULL != member; member = member->next)
			json_object_set_member(asked, member->data,
			                       json_node_copy(json_object_get_member(options, member->data)));
	}

	json_object_set_string_member(asked, "strategy", "transform");
	answer = venture_arbitrage_scan_run_full(context, organization_id, asked, TRUE, error);

	if (NULL == answer)
		return NULL;

	root = json_node_get_object(answer);
	rows = json_object_get_array_member(root, "rows");
	result = venture_report_result_new("Craft arbitrage", period);
	venture_report_result_add_column(result, "recipe", "Recipe", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "line", "Line", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "item", "Item", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "venue", "Where", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "quantity", "Quantity", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "unit_price", "Unit price", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "amount", "Amount", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "roi", "ROI", VENTURE_REPORT_COLUMN_PERCENT);
	venture_report_result_add_column(result, "note", "Note", VENTURE_REPORT_COLUMN_TEXT);

	for (i = 0; (NULL != rows) && (i < json_array_get_length(rows)); i++)
	{
		JsonObject *row = json_array_get_object_element(rows, i);
		JsonArray *inputs = json_object_get_array_member(row, "inputs");
		JsonObject *sell = json_object_has_member(row, "sell") ? json_object_get_object_member(row, "sell")
		                                                        : NULL;
		g_autoptr(GPtrArray) lines = NULL;
		g_autofree gchar *missing = arb_scan_missing(row);
		const gchar *recipe = venture_json_object_get_string(row, "title", "");

		lines = g_ptr_array_new();

		for (j = 0; (NULL != inputs) && (j < json_array_get_length(inputs)); j++)
			g_ptr_array_add(lines, json_array_get_object_element(inputs, j));

		g_ptr_array_sort(lines, arb_shop_compare);

		/* The shopping list: each input at its cheapest venue. */
		for (j = 0; j < lines->len; j++)
		{
			JsonObject *line = g_ptr_array_index(lines, j);
			const gchar *problem = venture_json_object_get_string(line, "problem", NULL);

			venture_report_result_begin_row(result);
			venture_report_result_set_text(result, "recipe", recipe);
			venture_report_result_set_text(result, "line", "input");
			venture_report_result_set_text(result, "item", venture_json_object_get_string(line, "name", ""));

			if (NULL != venture_json_object_get_string(line, "venue_name", NULL))
				venture_report_result_set_text(result, "venue",
				                               venture_json_object_get_string(line, "venue_name", NULL));

			venture_report_result_set_number(result, "quantity",
			                                 (gdouble)venture_json_object_get_int(line, "quantity", 0));
			arb_scan_set_money(result, "unit_price", line, "unit_price");
			arb_scan_set_money(result, "amount", line, "cost");

			if (NULL != problem)
				venture_report_result_set_text(result, "note", problem);
			else if (venture_json_object_get_bool(line, "reusable", FALSE))
				venture_report_result_set_text(result, "note", "Reusable: bought once, whatever the batches");
		}

		venture_report_result_begin_row(result);
		venture_report_result_set_text(result, "recipe", recipe);
		venture_report_result_set_text(result, "line", "output");
		venture_report_result_set_text(result, "item",
		                               venture_json_object_get_string(row, "instrument_name", ""));

		if (NULL != sell)
		{
			venture_report_result_set_text(result, "venue",
			                               venture_json_object_get_string(sell, "venue_name", ""));
			arb_scan_set_money(result, "unit_price", sell, "unit_price");
			arb_scan_set_money(result, "amount", sell, "amount");
		}

		venture_report_result_set_number(result, "quantity",
		                                 (gdouble)venture_json_object_get_int(row, "output_units", 0));

		venture_report_result_begin_row(result);
		venture_report_result_set_text(result, "recipe", recipe);
		venture_report_result_set_text(result, "line", "profit");
		arb_scan_set_money(result, "amount", row, "net");
		arb_scan_set_ratio(result, "roi", row, "roi", 1.0);

		/* Unquoted is blank, named, and never a zero. */
		if ('\0' != missing[0])
			venture_report_result_set_text(result, "note", missing);
	}

	arb_scan_notes(result, root);

	return g_steal_pointer(&result);
}

/* ==========================================================================
 * Registration
 *
 * A small report class rather than a function report, like the sessions'
 * and the market's, so it carries its parameter schema: the schema is
 * what the assistant's report tool, the MCP catalogue and the dashboard
 * widget validator read, and an option they cannot see is one never sent.
 * ========================================================================== */

/* arbitrage_scan's parameters: every common scan option. Ratios travel as
 * strings ("15" percent), money as "10.00 GOLD"; nothing is `limit`. */
#define ARB_SCAN_SCHEMA_BEGIN \
	"{\"type\":\"object\",\"properties\":{" \
	"\"strategy\":{\"type\":\"string\",\"description\":\"spread, transform, deal, cover, " \
	"back_lay or a plugin's; spread by default\"}," \
	"\"preset_id\":{\"type\":\"integer\",\"description\":\"An arbitrage_strategy preset to load; " \
	"options given beside it win\"}," \
	"\"data_source_id\":{\"type\":\"integer\",\"description\":\"Only this source's stores\"}," \
	"\"buy_venues\":{\"type\":\"string\",\"description\":\"Venue keys to buy at, comma separated\"}," \
	"\"sell_venues\":{\"type\":\"string\",\"description\":\"Venue keys to sell at, comma separated\"}," \
	"\"group_key\":{\"type\":\"string\",\"description\":\"Only venues in this group\"}," \
	"\"category_path\":{\"type\":\"string\",\"description\":\"Only instruments under this store " \
	"category path\"}," \
	"\"kind\":{\"type\":\"string\",\"description\":\"Only instruments of this kind\"}," \
	"\"instrument\":{\"type\":\"string\",\"description\":\"One instrument (or event) key\"}," \
	"\"recipe_id\":{\"type\":\"integer\",\"description\":\"transform: one recipe\"}," \
	"\"units\":{\"type\":\"integer\",\"description\":\"Units to price each opportunity for (batches " \
	"for transform); 1 by default\"}," \
	"\"buy_sources\":{\"type\":\"integer\",\"description\":\"spread: the N cheapest buy venues per " \
	"item (drop shipping); 1 by default\"}," \
	"\"sell_basis\":{\"type\":\"string\",\"enum\":[\"min\",\"market\",\"sale_avg\",\"region_median\"," \
	"\"bid\"],\"description\":\"What a unit sells for; sale_avg is the region's average sale price\"}," \
	"\"total_stake\":{\"type\":\"string\",\"description\":\"cover and back_lay: the stake, e.g. " \
	"\\\"100.00 USD\\\"\"}," \
	"\"min_profit\":{\"type\":\"string\",\"description\":\"Least net profit, e.g. \\\"10.00 GOLD\\\"\"}," \
	"\"min_roi\":{\"type\":\"string\",\"description\":\"Least ROI, percent, e.g. \\\"15\\\"\"}," \
	"\"min_sale_rate\":{\"type\":\"string\",\"description\":\"Least sale rate, percent\"}," \
	"\"max_capital\":{\"type\":\"string\",\"description\":\"Most capital one opportunity may tie " \
	"up, e.g. \\\"500.00 GOLD\\\"\"}," \
	"\"max_buy_pct\":{\"type\":\"string\",\"description\":\"Most a unit may cost as a percent of " \
	"the region's average sale price\"}," \
	"\"min_confidence\":{\"type\":\"string\",\"description\":\"Least confidence, 0 to 1\"}," \
	"\"max_age_hours\":{\"type\":\"integer\",\"description\":\"Leave out prices older than this\"}," \
	"\"share\":{\"type\":\"string\",\"description\":\"The percent of a market's sales a lot can " \
	"expect; 100 by default\"}," \
	"\"sort\":{\"type\":\"string\",\"enum\":[\"profit\",\"roi\",\"roi_per_day\",\"annualized\"," \
	"\"ev\",\"confidence\"],\"description\":\"Order; profit by default\"}," \
	"\"top\":{\"type\":\"integer\",\"description\":\"How many, 1 to 500; 50 by default\"}," \
	"\"organization_id\":{\"type\":\"integer\",\"description\":\"The legal entity; defaults to the " \
	"default organization\"}"

#define VENTURE_TYPE_ARBITRAGE_REPORT (venture_arbitrage_report_get_type())

G_DECLARE_FINAL_TYPE(VentureArbitrageReport, venture_arbitrage_report,
                     VENTURE, ARBITRAGE_REPORT, VentureReport)

struct _VentureArbitrageReport
{
	VentureReport		 parent_instance;

	VentureReportFunc	 func;
	const gchar		*schema;
};

G_DEFINE_FINAL_TYPE(VentureArbitrageReport, venture_arbitrage_report, VENTURE_TYPE_REPORT)

static VentureReportResult *
venture_arbitrage_report_generate(
	VentureReport		 *self,
	VentureContext		 *context,
	VentureDateRange	 *period,
	JsonObject		 *options,
	GError			**error
){
	return VENTURE_ARBITRAGE_REPORT(self)->func(context, period, options, error);
}

static JsonNode *
venture_arbitrage_report_parameters(VentureReport *self)
{
	return venture_json_parse(VENTURE_ARBITRAGE_REPORT(self)->schema, NULL);
}

static void
venture_arbitrage_report_class_init(VentureArbitrageReportClass *klass)
{
	VentureReportClass *report_class;

	report_class = VENTURE_REPORT_CLASS(klass);
	report_class->generate = venture_arbitrage_report_generate;
	report_class->describe_parameters = venture_arbitrage_report_parameters;
}

static void
venture_arbitrage_report_init(VentureArbitrageReport *self)
{
	(void)self;
}

void
venture_arbitrage_register_reports(VentureReportRegistry *registry)
{
	VentureArbitrageReport *report;

	g_return_if_fail(VENTURE_IS_REPORT_REGISTRY(registry));

	report = g_object_new(VENTURE_TYPE_ARBITRAGE_REPORT, "name", "arbitrage_performance",
	                      "title", "Arbitrage performance",
	                      "description", "How finished arbitrage trades did, per group and "
	                      "currency: realised profit, capital deployed, ROI, fees, hit rate, "
	                      "average hold and expected against realised",
	                      NULL);
	report->func = venture_arbitrage_performance;
	report->schema =
		"{\"type\":\"object\",\"properties\":{"
		"\"group_by\":{\"type\":\"string\",\"enum\":[\"strategy\",\"venue_pair\","
		"\"instrument\",\"month\"],\"description\":\"What to compare; strategy by "
		"default\"},"
		"\"strategy\":{\"type\":\"string\",\"description\":\"Only trades of this "
		"strategy, matched exactly\"},"
		"\"venture_id\":{\"type\":\"integer\",\"description\":\"Only this venture's "
		"trades\"},"
		"\"organization_id\":{\"type\":\"integer\",\"description\":\"The legal "
		"entity; defaults to the default organization\"}}}";
	venture_data_class_declare_resource(G_OBJECT(report), VENTURE_DATA_CLASS_TENANT);
	venture_report_registry_add(registry, VENTURE_REPORT(report));

	/* The scan's options, in the names every door forwards. */
	report = g_object_new(VENTURE_TYPE_ARBITRAGE_REPORT, "name", "arbitrage_scan",
	                      "title", "Arbitrage scan",
	                      "description", "Opportunities found now by one strategy (spread, "
	                      "transform, deal, cover, back_lay or a plugin's): net, capital, ROI, "
	                      "ROI a day, annualised, confidence, and what is unquoted",
	                      NULL);
	report->func = venture_arbitrage_scan_report;
	report->schema = ARB_SCAN_SCHEMA_BEGIN "}}";
	venture_data_class_declare_resource(G_OBJECT(report), VENTURE_DATA_CLASS_TENANT);
	venture_report_registry_add(registry, VENTURE_REPORT(report));

	report = g_object_new(VENTURE_TYPE_ARBITRAGE_REPORT, "name", "craft_arbitrage",
	                      "title", "Craft arbitrage",
	                      "description", "Each recipe's inputs at their cheapest venue -- a "
	                      "shopping list -- against the output's best venue, and the profit",
	                      NULL);
	report->func = venture_arbitrage_craft_report;
	report->schema =
		"{\"type\":\"object\",\"properties\":{"
		"\"recipe_id\":{\"type\":\"integer\",\"description\":\"One recipe; every active "
		"recipe when omitted\"},"
		"\"units\":{\"type\":\"integer\",\"description\":\"Batches to make; 1 by default\"},"
		"\"data_source_id\":{\"type\":\"integer\",\"description\":\"Only this source's "
		"stores\"},"
		"\"buy_venues\":{\"type\":\"string\",\"description\":\"Venue keys to buy inputs at, "
		"comma separated\"},"
		"\"sell_venues\":{\"type\":\"string\",\"description\":\"Venue keys to sell the output "
		"at, comma separated\"},"
		"\"group_key\":{\"type\":\"string\",\"description\":\"Only venues in this group\"},"
		"\"sell_basis\":{\"type\":\"string\",\"enum\":[\"min\",\"market\",\"sale_avg\","
		"\"region_median\",\"bid\"],\"description\":\"What the output sells for; min by "
		"default\"},"
		"\"max_age_hours\":{\"type\":\"integer\",\"description\":\"Leave out prices older "
		"than this\"},"
		"\"organization_id\":{\"type\":\"integer\",\"description\":\"The legal entity; "
		"defaults to the default organization\"}}}";
	venture_data_class_declare_resource(G_OBJECT(report), VENTURE_DATA_CLASS_TENANT);
	venture_report_registry_add(registry, VENTURE_REPORT(report));
}
