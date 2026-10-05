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

/* --- accounts, holdings, external_pnl ---------------------------------------------- */

/*
 * The account operations reports read the same answers the /accounts
 * pages draw (venture-marketdata-accounts.c). Money metrics carry the
 * first currency of the answer under the plain key -- the source's own,
 * where the dashboard's metric widget reads it -- and nothing converts.
 */

/* A money metric from the first object of an answer's money array. */
static void
mdr_money_metric(
	VentureReportResult	*result,
	const gchar		*key,
	const gchar		*label,
	JsonObject		*object,
	const gchar		*member
){
	g_autoptr(VentureMoney) money = NULL;
	JsonArray *amounts;

	amounts = ((NULL != object) && json_object_has_member(object, member))
		? json_object_get_array_member(object, member) : NULL;

	if ((NULL != amounts) && (json_array_get_length(amounts) > 0))
		money = venture_money_from_json(json_array_get_element(amounts, 0), NULL, NULL);

	if (NULL != money)
		venture_report_result_add_metric(result, venture_metric_new_money(key, label, money));
}

/* A money member's object directly. */
static void
mdr_money_object_metric(
	VentureReportResult	*result,
	const gchar		*key,
	const gchar		*label,
	JsonObject		*object,
	const gchar		*member
){
	g_autoptr(VentureMoney) money = NULL;
	JsonNode *node;

	node = (NULL != object) ? json_object_get_member(object, member) : NULL;

	if ((NULL != node) && JSON_NODE_HOLDS_OBJECT(node))
		money = venture_money_from_json(node, NULL, NULL);

	if (NULL != money)
		venture_report_result_add_metric(result, venture_metric_new_money(key, label, money));
}

/* The first money of an array member, as a cell. */
static void
mdr_set_first_money(
	VentureReportResult	*result,
	const gchar		*column,
	JsonObject		*row,
	const gchar		*member
){
	g_autoptr(VentureMoney) money = NULL;
	JsonArray *amounts;

	amounts = json_object_has_member(row, member) ? json_object_get_array_member(row, member) : NULL;

	if ((NULL != amounts) && (json_array_get_length(amounts) > 0))
		money = venture_money_from_json(json_array_get_element(amounts, 0), NULL, NULL);

	if (NULL != money)
		venture_report_result_set_money(result, column, money);
}

/* A threshold option: absent is 0 (the answer's default). */
static gboolean
mdr_threshold(
	JsonObject	 *options,
	const gchar	 *name,
	gint64		 *out,
	GError		**error
){
	return mdr_integer(options, name, 0, out, error);
}

static VentureReportResult *
mdr_accounts(
	VentureContext		 *context,
	VentureDateRange	 *period,
	JsonObject		 *options,
	GError			**error
){
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(JsonNode) answer = NULL;
	VentureMarketdataAccountsQuery query;
	JsonObject *root;
	JsonObject *summary;
	JsonArray *rows;
	JsonArray *attention;
	const gchar *sort;
	const gchar *group_by;
	guint i;

	venture_marketdata_accounts_query_init(&query);

	if (!mdr_organization(context, options, &query.organization_id, error) ||
	    !mdr_integer(options, "data_source_id", 0, &query.data_source_id, error) ||
	    !mdr_threshold(options, "expiring_hours", &query.expiring_hours, error) ||
	    !mdr_threshold(options, "mail_days", &query.mail_days, error) ||
	    !mdr_threshold(options, "stale_days", &query.stale_days, error))
		return NULL;

	query.basis = mdr_string(options, "basis");
	query.group_key = mdr_string(options, "group_key");
	query.login = mdr_string(options, "login");
	group_by = mdr_string(options, "group_by");
	sort = mdr_string(options, "sort");
	query.sort = sort;

	/* One way to group: a row per login instead of per account. */
	if ((NULL != group_by) && (0 != g_strcmp0(group_by, "login")))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "group_by for accounts is login, or nothing for a row per account");
		return NULL;
	}

	/* A figure is asked for largest first; a name or a deadline in its
	 * natural order. */
	query.descending = (0 == g_strcmp0(sort, "gold")) || (0 == g_strcmp0(sort, "positions")) ||
	                   (0 == g_strcmp0(sort, "inbound"));
	answer = venture_marketdata_accounts(context, &query, error);

	if (NULL == answer)
		return NULL;

	root = json_node_get_object(answer);
	summary = json_object_get_object_member(root, "summary");
	rows = json_object_get_array_member(root, "accounts");
	attention = json_object_get_array_member(root, "attention");

	result = venture_report_result_new("Accounts", period);
	mdr_money_metric(result, "gold", "Money on hand", summary, "balances");
	mdr_money_metric(result, "inventory_value", "Inventory value", summary, "inventory_value");
	mdr_money_metric(result, "listed_value", "Listed, at buyout", summary, "positions_value");
	mdr_money_metric(result, "mail_money", "Waiting in the mail", summary, "inbound_money");
	mdr_money_metric(result, "net_30d", "Net, 30 days", summary, "net_30d");
	venture_report_result_add_metric(result, venture_metric_new_count("needs_login", "Places to log in to",
		(NULL != attention) ? json_array_get_length(attention) : 0));
	venture_report_result_add_metric(result, venture_metric_new_count("listings", "Listings",
		json_object_get_int_member_with_default(summary, "positions", 0)));
	venture_report_result_add_metric(result, venture_metric_new_count("listings_expired", "Listings expired",
		json_object_get_int_member_with_default(summary, "positions_expired", 0)));
	venture_report_result_add_metric(result, venture_metric_new_count("logins", "Logins",
		json_object_get_int_member_with_default(summary, "logins", 0)));

	/* A row per login: each one's sums, the cards the overview draws. */
	if (NULL != group_by)
	{
		JsonArray *logins = json_object_get_array_member(root, "logins");

		venture_report_result_add_column(result, "login", "Login", VENTURE_REPORT_COLUMN_TEXT);
		venture_report_result_add_column(result, "characters", "Characters", VENTURE_REPORT_COLUMN_NUMBER);
		venture_report_result_add_column(result, "realms", "Realms", VENTURE_REPORT_COLUMN_NUMBER);
		venture_report_result_add_column(result, "gold", "Money", VENTURE_REPORT_COLUMN_MONEY);
		venture_report_result_add_column(result, "inventory_value", "Inventory", VENTURE_REPORT_COLUMN_MONEY);
		venture_report_result_add_column(result, "listings", "Listings", VENTURE_REPORT_COLUMN_NUMBER);
		venture_report_result_add_column(result, "expired", "Expired", VENTURE_REPORT_COLUMN_NUMBER);
		venture_report_result_add_column(result, "listed_value", "Listed, at buyout",
		                                 VENTURE_REPORT_COLUMN_MONEY);
		venture_report_result_add_column(result, "net_30d", "Net, 30 days", VENTURE_REPORT_COLUMN_MONEY);
		venture_report_result_add_column(result, "needs_login", "To visit", VENTURE_REPORT_COLUMN_NUMBER);

		for (i = 0; (NULL != logins) && (i < json_array_get_length(logins)); i++)
		{
			JsonObject *row = json_array_get_object_element(logins, i);

			venture_report_result_begin_row(result);
			venture_report_result_set_text(result, "login", mdr_text(row, "name"));
			mdr_set_number(result, "characters", row, "characters", 1.0);
			mdr_set_number(result, "realms", row, "realms", 1.0);
			mdr_set_first_money(result, "gold", row, "balances");
			mdr_set_first_money(result, "inventory_value", row, "inventory_value");
			mdr_set_number(result, "listings", row, "positions", 1.0);
			mdr_set_number(result, "expired", row, "positions_expired", 1.0);
			mdr_set_first_money(result, "listed_value", row, "positions_value");
			mdr_set_first_money(result, "net_30d", row, "net_30d");
			mdr_set_number(result, "needs_login", row, "needs_login", 1.0);
		}

		if ((NULL == logins) || (0 == json_array_get_length(logins)))
			venture_report_result_append_note(result, "No account names a login: the source sends none.");
	}
	else
	{
		gboolean has_logins = (json_object_get_int_member_with_default(summary, "logins", 0) > 0);

		/* The login column only where there is a login to show: a
		 * source that sends none reads as it always did. */
		venture_report_result_add_column(result, "account", "Account", VENTURE_REPORT_COLUMN_TEXT);

		if (has_logins)
			venture_report_result_add_column(result, "login", "Login", VENTURE_REPORT_COLUMN_TEXT);

		venture_report_result_add_column(result, "realm", "Realm", VENTURE_REPORT_COLUMN_TEXT);
		venture_report_result_add_column(result, "kind", "Kind", VENTURE_REPORT_COLUMN_TEXT);
		venture_report_result_add_column(result, "level", "Level", VENTURE_REPORT_COLUMN_NUMBER);
		venture_report_result_add_column(result, "class", "Class", VENTURE_REPORT_COLUMN_TEXT);
		venture_report_result_add_column(result, "gold", "Money", VENTURE_REPORT_COLUMN_MONEY);
		venture_report_result_add_column(result, "listings", "Listings", VENTURE_REPORT_COLUMN_NUMBER);
		venture_report_result_add_column(result, "expired", "Expired", VENTURE_REPORT_COLUMN_NUMBER);
		venture_report_result_add_column(result, "expiring", "Expiring soon", VENTURE_REPORT_COLUMN_NUMBER);
		venture_report_result_add_column(result, "soonest_expiry", "Soonest expiry", VENTURE_REPORT_COLUMN_TEXT);
		venture_report_result_add_column(result, "mail", "Mail", VENTURE_REPORT_COLUMN_NUMBER);
		venture_report_result_add_column(result, "mail_money", "In the mail", VENTURE_REPORT_COLUMN_MONEY);
		venture_report_result_add_column(result, "last_seen", "Last seen", VENTURE_REPORT_COLUMN_TEXT);
		venture_report_result_add_column(result, "why", "Log in for", VENTURE_REPORT_COLUMN_TEXT);

		for (i = 0; (NULL != rows) && (i < json_array_get_length(rows)); i++)
		{
			JsonObject *row = json_array_get_object_element(rows, i);
			JsonArray *reasons = json_object_get_array_member(row, "reasons");
			g_autoptr(GString) why = g_string_new(NULL);
			guint j;

			for (j = 0; (NULL != reasons) && (j < json_array_get_length(reasons)); j++)
			{
				if (j > 0)
					g_string_append(why, "; ");

				g_string_append(why, json_array_get_string_element(reasons, j));
			}

			venture_report_result_begin_row(result);
			venture_report_result_set_text(result, "account", mdr_text(row, "display_name"));
			if (has_logins)
				venture_report_result_set_text(result, "login", mdr_text(row, "login_name"));

			venture_report_result_set_text(result, "realm", mdr_text(row, "realm"));
			venture_report_result_set_text(result, "kind", mdr_text(row, "kind"));
			mdr_set_number(result, "level", row, "level", 1.0);
			venture_report_result_set_text(result, "class", mdr_text(row, "class"));
			mdr_set_money(result, "gold", row, "gold");
			mdr_set_number(result, "listings", row, "positions", 1.0);
			mdr_set_number(result, "expired", row, "positions_expired", 1.0);
			mdr_set_number(result, "expiring", row, "positions_expiring", 1.0);
			venture_report_result_set_text(result, "soonest_expiry", mdr_text(row, "soonest_expiry"));
			mdr_set_number(result, "mail", row, "inbound", 1.0);
			mdr_set_first_money(result, "mail_money", row, "inbound_money");
			venture_report_result_set_text(result, "last_seen", mdr_text(row, "last_seen"));
			venture_report_result_set_text(result, "why", (why->len > 0) ? why->str : NULL);
		}
	}

	/* Where to log in, a login at a time, most urgent first, as the
	 * overview lists it. */
	for (i = 0; (NULL != attention) && (i < json_array_get_length(attention)); i++)
	{
		JsonObject *row = json_array_get_object_element(attention, i);
		g_autofree gchar *note = NULL;

		note = g_strdup_printf("%s (%s)", mdr_text(row, "title"), mdr_text(row, "severity"));
		venture_report_result_append_note(result, note);
	}

	mdr_notes(result, root);

	return g_steal_pointer(&result);
}

static VentureReportResult *
mdr_holdings(
	VentureContext		 *context,
	VentureDateRange	 *period,
	JsonObject		 *options,
	GError			**error
){
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(JsonNode) answer = NULL;
	VentureMarketdataInventoryQuery query;
	JsonObject *root;
	JsonObject *totals;
	JsonArray *rows;
	gint64 value;
	guint i;

	venture_marketdata_inventory_query_init(&query);

	if (!mdr_organization(context, options, &query.organization_id, error) ||
	    !mdr_integer(options, "data_source_id", 0, &query.data_source_id, error) ||
	    !mdr_integer(options, "dead_days", 0, &query.dead_days, error) ||
	    !mdr_integer(options, "top", 50, &value, error))
		return NULL;

	if ((value < 1) || (value > VENTURE_MARKETDATA_ACCOUNTS_MAX_PAGE))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "top is 1 to %d", VENTURE_MARKETDATA_ACCOUNTS_MAX_PAGE);
		return NULL;
	}

	query.per_page = (guint)value;
	query.dead = (query.dead_days > 0);
	query.basis = mdr_string(options, "basis");
	query.account = mdr_string(options, "account_key");
	query.login = mdr_string(options, "login");
	query.group_by = mdr_string(options, "group_by");
	query.place = mdr_string(options, "place");
	query.category = mdr_string(options, "category_path");
	query.min_value = mdr_string(options, "min_value");
	query.sort = mdr_string(options, "sort");
	query.descending = (0 != g_strcmp0(query.sort, "name"));
	answer = venture_marketdata_inventory(context, &query, error);

	if (NULL == answer)
		return NULL;

	root = json_node_get_object(answer);
	totals = json_object_get_object_member(root, "totals");
	rows = json_object_get_array_member(root, "rows");

	result = venture_report_result_new("Account holdings", period);
	mdr_money_object_metric(result, "value", "Value", totals, "value");
	mdr_money_object_metric(result, "portfolio_value", "Whole inventory", root, "portfolio_value");
	venture_report_result_add_metric(result, venture_metric_new_count("items", "Items",
		json_object_get_int_member_with_default(totals, "instruments", 0)));
	venture_report_result_add_metric(result, venture_metric_new_count("units", "Units",
		json_object_get_int_member_with_default(totals, "units", 0)));

	/* By login: a row per login of the same holdings, instead of a row
	 * per item. The core refused any other grouping. */
	if (NULL != query.group_by)
	{
		JsonArray *groups = json_object_get_array_member(root, "by_login");

		venture_report_result_add_column(result, "login", "Login", VENTURE_REPORT_COLUMN_TEXT);
		venture_report_result_add_column(result, "lines", "Lines", VENTURE_REPORT_COLUMN_NUMBER);
		venture_report_result_add_column(result, "units", "Units", VENTURE_REPORT_COLUMN_NUMBER);
		venture_report_result_add_column(result, "value", "Value", VENTURE_REPORT_COLUMN_MONEY);
		venture_report_result_add_column(result, "share", "Of these", VENTURE_REPORT_COLUMN_PERCENT);

		for (i = 0; (NULL != groups) && (i < json_array_get_length(groups)); i++)
		{
			JsonObject *row = json_array_get_object_element(groups, i);

			venture_report_result_begin_row(result);
			venture_report_result_set_text(result, "login", mdr_text(row, "name"));
			mdr_set_number(result, "lines", row, "lines", 1.0);
			mdr_set_number(result, "units", row, "units", 1.0);
			mdr_set_money(result, "value", row, "value");
			mdr_set_number(result, "share", row, "share", 1.0);
		}

		mdr_notes(result, root);

		return g_steal_pointer(&result);
	}

	venture_report_result_add_column(result, "item", "Item", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "key", "Key", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "category", "Category", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "units", "Units", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "unit_value", "Each", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "value", "Value", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "share", "Of inventory", VENTURE_REPORT_COLUMN_PERCENT);
	venture_report_result_add_column(result, "days_of_supply", "Days to sell", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "accounts", "Held by", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "last_sale", "Last sold", VENTURE_REPORT_COLUMN_TEXT);

	for (i = 0; (NULL != rows) && (i < json_array_get_length(rows)); i++)
	{
		JsonObject *row = json_array_get_object_element(rows, i);
		const gchar *name = mdr_text(row, "instrument_name");

		venture_report_result_begin_row(result);
		venture_report_result_set_text(result, "item", (NULL != name) ? name : mdr_text(row, "instrument_key"));
		venture_report_result_set_text(result, "key", mdr_text(row, "instrument_key"));
		venture_report_result_set_text(result, "category", mdr_text(row, "category"));
		mdr_set_number(result, "units", row, "quantity", 1.0);
		mdr_set_money(result, "unit_value", row, "unit_value");
		mdr_set_money(result, "value", row, "value");
		mdr_set_number(result, "share", row, "share", 1.0);
		mdr_set_number(result, "days_of_supply", row, "days_of_supply", 1.0);
		mdr_set_number(result, "accounts", row, "accounts", 1.0);
		venture_report_result_set_text(result, "last_sale", mdr_text(row, "last_sale"));
	}

	if (json_object_get_int_member_with_default(root, "total", 0) > (gint64)query.per_page)
	{
		g_autofree gchar *note = NULL;

		note = g_strdup_printf("The top %u of %" G_GINT64_FORMAT " items; raise top or narrow to see more.",
		                       query.per_page, json_object_get_int_member(root, "total"));
		venture_report_result_append_note(result, note);
	}

	venture_report_result_append_note(result,
		"Valued on the basis named (conservative by default: the lower of the region's sale "
		"average and the market value where the account trades). Nothing converts a currency.");
	mdr_notes(result, root);

	return g_steal_pointer(&result);
}

static VentureReportResult *
mdr_external_pnl(
	VentureContext		 *context,
	VentureDateRange	 *period,
	JsonObject		 *options,
	GError			**error
){
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(JsonNode) answer = NULL;
	VentureMarketdataPnlQuery query;
	JsonObject *root;
	JsonArray *buckets;
	JsonArray *totals;
	GDateTime *start;
	GDateTime *end;
	const gchar *group_by;
	guint i;

	venture_marketdata_pnl_query_init(&query);

	if (!mdr_organization(context, options, &query.organization_id, error) ||
	    !mdr_integer(options, "data_source_id", 0, &query.data_source_id, error))
		return NULL;

	/* The report's period is the window: an unbounded side is no bound. */
	start = (NULL != period) ? venture_date_range_get_start(period) : NULL;
	end = (NULL != period) ? venture_date_range_get_end(period) : NULL;
	query.since = (NULL != start) ? MAX(g_date_time_to_unix(start), 1) : -1;
	query.until = (NULL != end) ? MAX(g_date_time_to_unix(end), 1) : -1;
	query.group_by = mdr_string(options, "group_by");
	query.account = mdr_string(options, "account_key");
	query.login = mdr_string(options, "login");
	query.venue = mdr_string(options, "venue");
	query.instrument = mdr_string(options, "instrument");
	query.source = mdr_string(options, "source");
	answer = venture_marketdata_external_pnl(context, &query, error);

	if (NULL == answer)
		return NULL;

	root = json_node_get_object(answer);
	buckets = json_object_get_array_member(root, "buckets");
	totals = json_object_get_array_member(root, "totals");
	group_by = mdr_text(root, "group_by");

	result = venture_report_result_new("Trading profit and loss", period);

	if ((NULL != totals) && (json_array_get_length(totals) > 0))
	{
		JsonObject *total = json_array_get_object_element(totals, 0);

		mdr_money_object_metric(result, "sales", "Sales, after the cut", total, "sales_amount");
		mdr_money_object_metric(result, "purchases", "Purchases", total, "buys_amount");
		mdr_money_object_metric(result, "income", "Other income", total, "income");
		mdr_money_object_metric(result, "expenses", "Expenses", total, "expense");
		mdr_money_object_metric(result, "net", "Net", total, "net");
		venture_report_result_add_metric(result, venture_metric_new_count("units_sold", "Units sold",
			json_object_get_int_member_with_default(total, "sold_units", 0)));
	}

	venture_report_result_add_column(result, "group", (NULL != group_by) ? group_by : "day",
	                                 VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "sales", "Sales", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "sold_units", "Units sold", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "purchases", "Purchases", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "bought_units", "Units bought", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "income", "Other income", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "expenses", "Expenses", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "net", "Net", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "expired_units", "Units expired", VENTURE_REPORT_COLUMN_NUMBER);

	for (i = 0; (NULL != buckets) && (i < json_array_get_length(buckets)); i++)
	{
		JsonObject *row = json_array_get_object_element(buckets, i);
		const gchar *label = mdr_text(row, "period_start");

		if (NULL == label)
			label = mdr_text(row, "label");

		venture_report_result_begin_row(result);
		venture_report_result_set_text(result, "group", venture_string_is_empty(label) ? "(none)" : label);
		mdr_set_money(result, "sales", row, "sales_amount");
		mdr_set_number(result, "sold_units", row, "sold_units", 1.0);
		mdr_set_money(result, "purchases", row, "buys_amount");
		mdr_set_number(result, "bought_units", row, "bought_units", 1.0);
		mdr_set_money(result, "income", row, "income");
		mdr_set_money(result, "expenses", row, "expense");
		mdr_set_money(result, "net", row, "net");
		mdr_set_number(result, "expired_units", row, "expired_units", 1.0);
	}

	venture_report_result_append_note(result,
		"The source's own ledger, as its addon records it: sales after the venue's cut, purchases, "
		"other income and expenses. Periods are UTC. Nothing converts a currency.");

	{
		JsonArray *notes = json_object_get_array_member(root, "notes");

		for (i = 0; (NULL != notes) && (i < json_array_get_length(notes)); i++)
			venture_report_result_append_note(result, json_array_get_string_element(notes, i));
	}

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
	mdr_add(registry, "accounts", "Accounts",
		"The operator's accounts from every data source that reports them: money on hand, "
		"listings up and expired, mail waiting, when each was last seen and why it needs a "
		"login, with the places to log in to, most urgent first, in the notes",
		mdr_accounts,
		"{\"type\":\"object\",\"properties\":{"
		"\"data_source_id\":{\"type\":\"integer\",\"description\":\"Only this data "
		"source; every source of the organization by default\"},"
		"\"group_key\":{\"type\":\"string\",\"description\":\"Only accounts in this "
		"group (a realm)\"},"
		"\"basis\":{\"type\":\"string\",\"enum\":[\"conservative\",\"market\",\"min\","
		"\"historical\",\"region_market\",\"region_sale_avg\"],\"description\":\"How "
		"holdings are valued; conservative by default\"},"
		"\"expiring_hours\":{\"type\":\"integer\",\"description\":\"Listings expiring "
		"within this many hours need a login; 12 by default, at most 720\"},"
		"\"mail_days\":{\"type\":\"integer\",\"description\":\"Mail expiring within "
		"this many days needs a login; 3 by default, at most 60\"},"
		"\"stale_days\":{\"type\":\"integer\",\"description\":\"An account unseen this "
		"many days needs a visit; 14 by default\"},"
		"\"sort\":{\"type\":\"string\",\"enum\":[\"attention\",\"name\",\"realm\",\"gold\","
		"\"positions\",\"expiry\",\"inbound\",\"last_seen\",\"freshness\",\"login\"],"
		"\"description\":\"The table's order; attention by default\"},"
		"\"login\":{\"type\":\"string\",\"description\":\"Only the accounts reached "
		"through this login, by its key in the source\"},"
		"\"group_by\":{\"type\":\"string\",\"enum\":[\"login\"],\"description\":\"login "
		"for a row per login (its characters, money, inventory, listings, net and places "
		"to visit) instead of a row per account\"},"
		MDR_ORGANIZATION "}}");

	mdr_add(registry, "account_holdings", "Account holdings",
		"What every account of one data source holds, per item across accounts and places, "
		"valued on a basis, with its share of the inventory and days to sell at the region's "
		"pace; most valuable first",
		mdr_holdings,
		"{\"type\":\"object\",\"properties\":{"
		"\"data_source_id\":{\"type\":\"integer\",\"description\":\"The data source; "
		"the organization's first with accounts by default\"},"
		"\"account_key\":{\"type\":\"string\",\"description\":\"Only this account's "
		"holdings, by its key in the source\"},"
		"\"place\":{\"type\":\"string\",\"description\":\"Only this place: bag, bank, "
		"reagent_bank, warbank, guild, mail, auction, void, equipped, currency or other\"},"
		"\"category_path\":{\"type\":\"string\",\"description\":\"Only items in this "
		"store category path and beneath\"},"
		"\"basis\":{\"type\":\"string\",\"enum\":[\"conservative\",\"market\",\"min\","
		"\"historical\",\"region_market\",\"region_sale_avg\"],\"description\":\"How "
		"items are valued; conservative by default\"},"
		"\"min_value\":{\"type\":\"string\",\"description\":\"Only items worth at least "
		"this in total, in the source's currency, e.g. 100.00 GOLD\"},"
		"\"dead_days\":{\"type\":\"integer\",\"description\":\"Dead stock only: items "
		"with no sale in the ledger for this many days\"},"
		"\"sort\":{\"type\":\"string\",\"enum\":[\"value\",\"quantity\",\"name\","
		"\"unit_value\",\"accounts\",\"days_of_supply\"],\"description\":\"The order; "
		"value, largest first, by default\"},"
		"\"top\":{\"type\":\"integer\",\"description\":\"How many items, 1 to 500; 50 "
		"by default\"},"
		"\"login\":{\"type\":\"string\",\"description\":\"Only the holdings of the "
		"accounts reached through this login, by its key in the source\"},"
		"\"group_by\":{\"type\":\"string\",\"enum\":[\"login\"],\"description\":\"login "
		"for a row per login of the same holdings, valued line by line, instead of a "
		"row per item\"},"
		MDR_ORGANIZATION "}}");

	mdr_add(registry, "external_pnl", "Trading profit and loss",
		"A data source's own ledger of the operator's trades summed for the period: sales "
		"after the venue's cut, purchases, other income and expenses, net, by day, week, "
		"month, account, venue, item, login or the source's own label",
		mdr_external_pnl,
		"{\"type\":\"object\",\"properties\":{"
		"\"data_source_id\":{\"type\":\"integer\",\"description\":\"The data source; "
		"the organization's first with accounts by default\"},"
		"\"group_by\":{\"type\":\"string\",\"enum\":[\"day\",\"week\",\"month\",\"account\","
		"\"venue\",\"instrument\",\"source\",\"login\"],\"description\":\"What a row is; "
		"day by default\"},"
		"\"login\":{\"type\":\"string\",\"description\":\"Only the rows of the "
		"accounts reached through this login, by its key in the source\"},"
		"\"account_key\":{\"type\":\"string\",\"description\":\"Only this account's "
		"rows, by its key\"},"
		"\"venue\":{\"type\":\"string\",\"description\":\"Only this venue's rows\"},"
		"\"instrument\":{\"type\":\"string\",\"description\":\"Only this item's rows, by "
		"its key\"},"
		"\"source\":{\"type\":\"string\",\"description\":\"Only rows the source labels "
		"so, e.g. Auction\"},"
		MDR_ORGANIZATION "}}");
}
