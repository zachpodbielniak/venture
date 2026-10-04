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
 * Registration
 *
 * A small report class rather than a function report, like the sessions'
 * and the market's, so it carries its parameter schema: the schema is
 * what the assistant's report tool, the MCP catalogue and the dashboard
 * widget validator read, and an option they cannot see is one never sent.
 * ========================================================================== */

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
}
