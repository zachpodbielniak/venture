/*
 * venture-marketdata-reports.c - market_deals, venue_index and watchlist
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Three reports over the same answers the /market pages draw
 * (venture-marketdata-browse.c), so a deal on the page and a deal in the
 * CLI, the MCP tool or the assistant are one deal. Each reads the state of
 * the stores now: the period every report is handed is not a question
 * these ask, and a note says so rather than letting "last_month" look
 * like it narrowed anything.
 *
 * Options arrive through five doors that each carry their own list (the
 * web API, the report page, the CLI, MCP and the assistant). Money travels
 * as "10.00 GOLD" and a ratio as a string, so no door turns either into a
 * double on the way.
 */

#include "venture.h"

#include <math.h>
#include <string.h>

/* --- Options ------------------------------------------------------------- */

/* The organization: options' organization_id, else the default. */
static gboolean
mdr_organization(
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

/* A string option, or NULL when absent or empty. */
static const gchar *
mdr_string(
	JsonObject	*options,
	const gchar	*name
){
	JsonNode *node;

	if ((NULL == options) || !json_object_has_member(options, name))
		return NULL;

	node = json_object_get_member(options, name);

	if (!JSON_NODE_HOLDS_VALUE(node) || (G_TYPE_STRING != json_node_get_value_type(node)))
		return NULL;

	return venture_string_is_empty(json_node_get_string(node)) ? NULL : json_node_get_string(node);
}

/*
 * A non-negative integer option, as a number or the digits of one (the
 * CLI and the MCP tool can send either). Absent is @fallback; anything
 * else is refused by name rather than read as zero.
 */
static gboolean
mdr_integer(
	JsonObject	 *options,
	const gchar	 *name,
	gint64		  fallback,
	gint64		 *out,
	GError		**error
){
	JsonNode *node;

	*out = fallback;

	if ((NULL == options) || !json_object_has_member(options, name))
		return TRUE;

	node = json_object_get_member(options, name);

	if (JSON_NODE_HOLDS_VALUE(node) && (G_TYPE_INT64 == json_node_get_value_type(node)) &&
	    (json_node_get_int(node) >= 0))
	{
		*out = json_node_get_int(node);
		return TRUE;
	}

	/* A model writes 3 as 3.0 now and then; a whole double is that number. */
	if (JSON_NODE_HOLDS_VALUE(node) && (G_TYPE_DOUBLE == json_node_get_value_type(node)) &&
	    (json_node_get_double(node) >= 0.0) && (json_node_get_double(node) < 9.0e15) &&
	    (json_node_get_double(node) == floor(json_node_get_double(node))))
	{
		*out = (gint64)json_node_get_double(node);
		return TRUE;
	}

	if (JSON_NODE_HOLDS_VALUE(node) && (G_TYPE_STRING == json_node_get_value_type(node)) &&
	    g_ascii_string_to_signed(json_node_get_string(node), 10, 0, G_MAXINT64, out, NULL))
		return TRUE;

	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
	            "%s must be a whole number, zero or more", name);

	return FALSE;
}

/* --- Rendering helpers ------------------------------------------------------ */

/* A money member of an answer row, or nothing when it is null. */
static void
mdr_set_money(
	VentureReportResult	*result,
	const gchar		*column,
	JsonObject		*row,
	const gchar		*member
){
	g_autoptr(VentureMoney) money = NULL;
	JsonNode *node;

	node = json_object_get_member(row, member);

	if ((NULL == node) || !JSON_NODE_HOLDS_OBJECT(node))
		return;

	money = venture_money_from_json(node, NULL, NULL);

	if (NULL != money)
		venture_report_result_set_money(result, column, money);
}

static void
mdr_set_number(
	VentureReportResult	*result,
	const gchar		*column,
	JsonObject		*row,
	const gchar		*member,
	gdouble			 scale
){
	JsonNode *node;

	node = json_object_get_member(row, member);

	if ((NULL == node) || !JSON_NODE_HOLDS_VALUE(node))
		return;

	venture_report_result_set_number(result, column, json_node_get_double(node) * scale);
}

static const gchar *
mdr_text(
	JsonObject	*row,
	const gchar	*member
){
	JsonNode *node;

	node = json_object_get_member(row, member);

	if ((NULL == node) || !JSON_NODE_HOLDS_VALUE(node) ||
	    (G_TYPE_STRING != json_node_get_value_type(node)))
		return NULL;

	return json_node_get_string(node);
}

/* The answer's notes, then a word on the period. */
static void
mdr_notes(
	VentureReportResult	*result,
	JsonObject		*answer
){
	JsonArray *notes;
	guint i;

	notes = json_object_get_array_member(answer, "notes");

	for (i = 0; (NULL != notes) && (i < json_array_get_length(notes)); i++)
		venture_report_result_append_note(result, json_array_get_string_element(notes, i));

	venture_report_result_append_note(result,
		"Read from the stores as they are now; the period does not narrow it.");
}

/* --- market_deals ------------------------------------------------------------ */

static VentureReportResult *
mdr_deals(
	VentureContext		 *context,
	VentureDateRange	 *period,
	JsonObject		 *options,
	GError			**error
){
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(VentureMoney) min_value = NULL;
	g_autoptr(JsonNode) answer = NULL;
	VentureMarketdataDealsQuery query;
	const gchar *text;
	JsonObject *root;
	JsonArray *rows;
	gint64 value;
	guint i;

	venture_marketdata_deals_query_init(&query);

	if (!mdr_organization(context, options, &query.organization_id, error) ||
	    !mdr_integer(options, "data_source_id", 0, &query.data_source_id, error) ||
	    !mdr_integer(options, "top", 50, &value, error))
		return NULL;

	if ((value < 1) || (value > VENTURE_MARKETDATA_DEALS_MAX))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "top is 1 to %d", VENTURE_MARKETDATA_DEALS_MAX);
		return NULL;
	}

	query.count = (guint)value;
	query.venue = mdr_string(options, "venue");
	query.group_key = mdr_string(options, "group_key");
	query.category = mdr_string(options, "category_path");
	text = mdr_string(options, "min_value");

	/* No default currency: a bound in one currency says nothing about
	 * another, and guessing one would quietly answer a different
	 * question. */
	if (NULL != text)
	{
		min_value = venture_marketdata_parse_amount(text, error);

		if (NULL == min_value)
		{
			g_prefix_error(error, "min_value (an amount with its currency, e.g. "
			                      "\"10.00 GOLD\"): ");
			return NULL;
		}

		query.min_value = min_value;
	}

	text = mdr_string(options, "max_pct");

	if (NULL != text)
	{
		gchar *end = NULL;

		query.max_pct = g_ascii_strtod(text, &end);

		if ((NULL == end) || ('\0' != *end) || !isfinite(query.max_pct) || (query.max_pct < 0.0))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			            "max_pct is a percent of the region median, e.g. \"80\", not \"%s\"", text);
			return NULL;
		}
	}

	answer = venture_marketdata_deals(context, &query, error);

	if (NULL == answer)
		return NULL;

	root = json_node_get_object(answer);
	rows = json_object_get_array_member(root, "rows");

	result = venture_report_result_new("Market deals", period);
	venture_report_result_add_column(result, "source", "Source", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "venue", "Venue", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "instrument", "Instrument", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "key", "Key", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "price", "Lowest price", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "deal_price", "Deal price", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "region_median", "Region median", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "pct_of_region", "Of region", VENTURE_REPORT_COLUMN_PERCENT);
	venture_report_result_add_column(result, "discount", "Under the deal price", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "quantity", "Quantity", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "sale_rate", "Sale rate", VENTURE_REPORT_COLUMN_PERCENT);

	for (i = 0; (NULL != rows) && (i < json_array_get_length(rows)); i++)
	{
		JsonObject *row = json_array_get_object_element(rows, i);
		const gchar *venue;
		const gchar *name;

		venue = mdr_text(row, "venue_name");
		name = mdr_text(row, "instrument_name");
		venture_report_result_begin_row(result);
		venture_report_result_set_text(result, "source", mdr_text(row, "source_name"));
		venture_report_result_set_text(result, "venue", (NULL != venue) ? venue : mdr_text(row, "venue_key"));
		venture_report_result_set_text(result, "instrument",
		                               (NULL != name) ? name : mdr_text(row, "instrument_key"));
		venture_report_result_set_text(result, "key", mdr_text(row, "instrument_key"));
		mdr_set_money(result, "price", row, "min_price");
		mdr_set_money(result, "deal_price", row, "deal_price");
		mdr_set_money(result, "region_median", row, "region_median");
		mdr_set_number(result, "pct_of_region", row, "pct_vs_region", 0.01);
		mdr_set_money(result, "discount", row, "discount");
		mdr_set_number(result, "quantity", row, "quantity", 1.0);
		mdr_set_number(result, "sale_rate", row, "sale_rate", 1.0);
	}

	if (json_object_get_boolean_member(root, "truncated"))
	{
		g_autofree gchar *note = NULL;

		note = g_strdup_printf("Only the best %u deals are listed; narrow by venue, group or "
		                       "category, or raise top.", query.count);
		venture_report_result_append_note(result, note);
	}

	venture_report_result_append_note(result,
		"A deal is in stock at a price at or under its group's deal price: the median of the "
		"group's lowest prices, or the 33rd percentile when 15 or more venues offer it.");
	mdr_notes(result, root);

	return g_steal_pointer(&result);
}

/* --- venue_index -------------------------------------------------------------- */

static VentureReportResult *
mdr_venue_index(
	VentureContext		 *context,
	VentureDateRange	 *period,
	JsonObject		 *options,
	GError			**error
){
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(JsonNode) answer = NULL;
	JsonObject *root;
	JsonArray *venues;
	gint64 organization_id;
	gint64 source_id;
	guint i;

	if (!mdr_organization(context, options, &organization_id, error) ||
	    !mdr_integer(options, "data_source_id", 0, &source_id, error))
		return NULL;

	answer = venture_marketdata_venue_index(context, organization_id, source_id,
	                                        mdr_string(options, "group_key"), 0, error);

	if (NULL == answer)
		return NULL;

	root = json_node_get_object(answer);
	venues = json_object_get_array_member(root, "venues");

	result = venture_report_result_new("Venue index", period);
	venture_report_result_add_column(result, "source", "Source", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "venue", "Venue", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "group", "Group", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "instruments", "Compared", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "cheaper", "Cheaper", VENTURE_REPORT_COLUMN_PERCENT);
	venture_report_result_add_column(result, "equal", "Equal", VENTURE_REPORT_COLUMN_PERCENT);
	venture_report_result_add_column(result, "dearer", "Dearer", VENTURE_REPORT_COLUMN_PERCENT);
	venture_report_result_add_column(result, "avg_ratio", "Price to region", VENTURE_REPORT_COLUMN_PERCENT);
	venture_report_result_add_column(result, "listings", "Listings", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "last_update", "Last snapshot", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "interval_minutes", "Updates every (min)",
	                                 VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "age_minutes", "Data age (min)", VENTURE_REPORT_COLUMN_NUMBER);

	for (i = 0; (NULL != venues) && (i < json_array_get_length(venues)); i++)
	{
		JsonObject *venue = json_array_get_object_element(venues, i);
		const gchar *name;

		name = mdr_text(venue, "venue_name");
		venture_report_result_begin_row(result);
		venture_report_result_set_text(result, "source", mdr_text(venue, "source_name"));
		venture_report_result_set_text(result, "venue", (NULL != name) ? name : mdr_text(venue, "venue_key"));
		venture_report_result_set_text(result, "group", mdr_text(venue, "group_key"));
		mdr_set_number(result, "instruments", venue, "instruments", 1.0);
		mdr_set_number(result, "cheaper", venue, "pct_cheaper", 0.01);
		mdr_set_number(result, "equal", venue, "pct_equal", 0.01);
		mdr_set_number(result, "dearer", venue, "pct_dearer", 0.01);
		mdr_set_number(result, "avg_ratio", venue, "avg_ratio", 1.0);
		mdr_set_number(result, "listings", venue, "listings", 1.0);
		venture_report_result_set_text(result, "last_update", mdr_text(venue, "last_taken_at"));
		mdr_set_number(result, "interval_minutes", venue, "interval_seconds", 1.0 / 60.0);
		mdr_set_number(result, "age_minutes", venue, "age_seconds", 1.0 / 60.0);
	}

	venture_report_result_append_note(result,
		"Cheaper, equal and dearer are shares of the instruments in stock at the venue that "
		"have a region median; price to region is the mean of lowest price over region median.");
	mdr_notes(result, root);

	return g_steal_pointer(&result);
}

/* --- watchlist ------------------------------------------------------------- */

static VentureReportResult *
mdr_watchlist(
	VentureContext		 *context,
	VentureDateRange	 *period,
	JsonObject		 *options,
	GError			**error
){
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(JsonNode) answer = NULL;
	g_autofree gchar *title = NULL;
	JsonObject *root;
	JsonArray *entries;
	gint64 organization_id;
	gint64 watchlist_id;
	guint i;

	if (!mdr_organization(context, options, &organization_id, error) ||
	    !mdr_integer(options, "watchlist_id", 0, &watchlist_id, error))
		return NULL;

	if (watchlist_id <= 0)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "The watchlist report needs watchlist_id: which list to price");
		return NULL;
	}

	answer = venture_marketdata_watchlist_view(context, organization_id, watchlist_id, 0, error);

	if (NULL == answer)
		return NULL;

	root = json_node_get_object(answer);
	entries = json_object_get_array_member(root, "entries");
	title = g_strdup_printf("Watchlist: %s",
		json_object_get_string_member(json_object_get_object_member(root, "watchlist"), "name"));

	result = venture_report_result_new(title, period);
	venture_report_result_add_column(result, "instrument", "Instrument", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "venue", "Cheapest at", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "price", "Lowest price", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "quantity", "Quantity", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "target_buy", "Buy at", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "buy_delta", "Against buy", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "target_sell", "Sell at", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "sell_delta", "Against sell", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "signal", "Now", VENTURE_REPORT_COLUMN_TEXT);

	for (i = 0; (NULL != entries) && (i < json_array_get_length(entries)); i++)
	{
		JsonObject *entry = json_array_get_object_element(entries, i);
		JsonNode *best;
		const gchar *name;

		name = mdr_text(entry, "instrument_name");
		venture_report_result_begin_row(result);
		venture_report_result_set_text(result, "instrument", (NULL != name) ? name : mdr_text(entry, "key"));
		best = json_object_get_member(entry, "best");

		if ((NULL != best) && JSON_NODE_HOLDS_OBJECT(best))
		{
			JsonObject *summary = json_node_get_object(best);
			const gchar *venue = mdr_text(summary, "venue_name");

			venture_report_result_set_text(result, "venue",
			                               (NULL != venue) ? venue : mdr_text(summary, "venue_key"));
			mdr_set_money(result, "price", summary, "min_price");
			mdr_set_number(result, "quantity", summary, "quantity", 1.0);
		}
		else
			venture_report_result_set_text(result, "venue", mdr_text(entry, "problem"));

		mdr_set_money(result, "target_buy", entry, "target_buy");
		mdr_set_money(result, "buy_delta", entry, "buy_delta");
		mdr_set_money(result, "target_sell", entry, "target_sell");
		mdr_set_money(result, "sell_delta", entry, "sell_delta");
		venture_report_result_set_text(result, "signal",
			json_object_get_boolean_member(entry, "buy_now") ? "buy"
			: (json_object_get_boolean_member(entry, "sell_now") ? "sell" : NULL));
	}

	venture_report_result_append_note(result,
		"The cheapest in-stock venue of the list's group (every venue when the list has none). "
		"A target in another currency is not compared: nothing here converts.");
	mdr_notes(result, root);

	return g_steal_pointer(&result);
}

/* --- Registration ------------------------------------------------------------ */

/*
 * A small report class rather than a function report, so each carries its
 * parameter schema: the schema is what the assistant's report tool, the
 * MCP catalogue and the dashboard widget validator read, and an option
 * they cannot see is one never sent. The class declares its data class
 * like a classified function report does.
 */

#define VENTURE_TYPE_MARKETDATA_REPORT (venture_marketdata_report_get_type())

G_DECLARE_FINAL_TYPE(VentureMarketdataReport, venture_marketdata_report,
                     VENTURE, MARKETDATA_REPORT, VentureReport)

struct _VentureMarketdataReport
{
	VentureReport		 parent_instance;

	VentureReportFunc	 func;
	const gchar		*schema;
};

G_DEFINE_FINAL_TYPE(VentureMarketdataReport, venture_marketdata_report, VENTURE_TYPE_REPORT)

static VentureReportResult *
venture_marketdata_report_generate(
	VentureReport		 *self,
	VentureContext		 *context,
	VentureDateRange	 *period,
	JsonObject		 *options,
	GError			**error
){
	return VENTURE_MARKETDATA_REPORT(self)->func(context, period, options, error);
}

static JsonNode *
venture_marketdata_report_parameters(VentureReport *self)
{
	return venture_json_parse(VENTURE_MARKETDATA_REPORT(self)->schema, NULL);
}

static void
venture_marketdata_report_class_init(VentureMarketdataReportClass *klass)
{
	VentureReportClass *report_class;

	report_class = VENTURE_REPORT_CLASS(klass);
	report_class->generate = venture_marketdata_report_generate;
	report_class->describe_parameters = venture_marketdata_report_parameters;
}

static void
venture_marketdata_report_init(VentureMarketdataReport *self)
{
	(void)self;
}

static void
mdr_add(
	VentureReportRegistry	*registry,
	const gchar		*name,
	const gchar		*title,
	const gchar		*description,
	VentureReportFunc	 func,
	const gchar		*schema
){
	VentureMarketdataReport *report;

	report = g_object_new(VENTURE_TYPE_MARKETDATA_REPORT, "name", name,
	                      "title", title, "description", description, NULL);
	report->func = func;
	report->schema = schema;
	venture_data_class_declare_resource(G_OBJECT(report), VENTURE_DATA_CLASS_TENANT);
	venture_report_registry_add(registry, VENTURE_REPORT(report));
}

#define MDR_ORGANIZATION \
	"\"organization_id\":{\"type\":\"integer\",\"description\":\"The legal " \
	"entity whose data sources are read; defaults to the default organization\"}"

void
venture_marketdata_register_reports(VentureReportRegistry *registry)
{
	g_return_if_fail(VENTURE_IS_REPORT_REGISTRY(registry));

	mdr_add(registry, "market_deals", "Market deals",
		"Instruments in stock at a venue at or under their group's deal price (the median "
		"of the group's lowest prices, or the 33rd percentile with 15 or more venues), "
		"cheapest against the region first, from every data source or one",
		mdr_deals,
		"{\"type\":\"object\",\"properties\":{"
		"\"data_source_id\":{\"type\":\"integer\",\"description\":\"Only this data "
		"source; every source of the organization by default\"},"
		"\"venue\":{\"type\":\"string\",\"description\":\"Only this venue, by its key "
		"in the store\"},"
		"\"group_key\":{\"type\":\"string\",\"description\":\"Only venues in this "
		"group (a region)\"},"
		"\"category_path\":{\"type\":\"string\",\"description\":\"Only instruments "
		"in this store category path and beneath, e.g. Trade Goods/Herb\"},"
		"\"min_value\":{\"type\":\"string\",\"description\":\"Only instruments worth "
		"at least this, with its currency, e.g. 10.00 GOLD; compared in that "
		"currency only\"},"
		"\"max_pct\":{\"type\":\"string\",\"description\":\"Only prices at most this "
		"percent of the region median, e.g. 80 for 20% under\"},"
		"\"top\":{\"type\":\"integer\",\"description\":\"How many deals, 1 to 500; "
		"50 by default\"},"
		MDR_ORGANIZATION "}}");

	mdr_add(registry, "venue_index", "Venue index",
		"Per venue: the share of instruments cheaper, equal to and dearer than the region "
		"median, the mean price to region ratio, listings, the last snapshot, the learned "
		"update interval and the data's age",
		mdr_venue_index,
		"{\"type\":\"object\",\"properties\":{"
		"\"data_source_id\":{\"type\":\"integer\",\"description\":\"Only this data "
		"source; every source of the organization by default\"},"
		"\"group_key\":{\"type\":\"string\",\"description\":\"Only venues in this "
		"group (a region)\"},"
		MDR_ORGANIZATION "}}");

	mdr_add(registry, "watchlist", "Watchlist",
		"A watchlist priced now: each instrument's cheapest in-stock venue in the list's "
		"group, against its buy and sell targets",
		mdr_watchlist,
		"{\"type\":\"object\",\"required\":[\"watchlist_id\"],\"properties\":{"
		"\"watchlist_id\":{\"type\":\"integer\",\"description\":\"The watchlist to "
		"price\"},"
		MDR_ORGANIZATION "}}");
}
