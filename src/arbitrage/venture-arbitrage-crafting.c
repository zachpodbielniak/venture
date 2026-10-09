/*
 * venture-arbitrage-crafting.c - What every recipe makes, and the flip
 *                                planner's shopping list
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Neither answer computes a price, a fee or a profit of its own. Crafting
 * asks the `transform` strategy (venture-arbitrage-strategies.c) every
 * recipe at once with losses kept, and adds only what a person needs to
 * read it: the recipe's category path and the pickers' choices. The
 * planner reads the trades "Record attempt" and "Add to plan" wrote --
 * planned arbitrage_trade records, whose legs are the plan the strategy
 * made -- and regroups their legs as a shopping list; re-pricing asks
 * each trade's own question again (kept in its `expected`) and compares.
 * A plan is not a second record type beside the trade: a planned trade
 * *is* a line of the plan, and executing its legs is how it is carried
 * out.
 */

#include "venture.h"
#include "arbitrage/venture-arbitrage-engine-private.h"

#include <math.h>
#include <string.h>

/* ==========================================================================
 * Crafting
 * ========================================================================== */

static const gchar *const craft_options[] = {
	"data_source_id", "recipe_category_id", "recipe_id", "buy_realm", "sell_realm", "venue_group",
	"units", "sell_basis", "max_age_hours", "share", NULL
};

/* The options the scan reads as they are given; the realms are read here. */
static const gchar *const craft_passed[] = {
	"data_source_id", "recipe_category_id", "recipe_id", "venue_group", "units", "sell_basis",
	"max_age_hours", "share", NULL
};

const gchar *const *
venture_arbitrage_crafting_option_names(void)
{
	return craft_options;
}

/* A picked realm as a comma-separated `buy_venues`/`sell_venues`. */
static gboolean
craft_realm(
	VentureContext	 *context,
	gint64		  organization_id,
	const gchar	 *option,
	const gchar	 *wanted,
	JsonObject	 *scan_options,
	const gchar	 *scan_option,
	JsonObject	 *question,
	GError		**error
){
	g_auto(GStrv) keys = NULL;
	g_autofree gchar *label = NULL;
	g_autofree gchar *joined = NULL;

	if (venture_string_is_empty(wanted))
		return TRUE;

	if (!venture_marketdata_realm_venue_keys(context, organization_id, wanted, &keys, &label, error))
	{
		g_prefix_error(error, "%s: ", option);
		return FALSE;
	}

	joined = g_strjoinv(",", keys);
	json_object_set_string_member(scan_options, scan_option, joined);
	json_object_set_string_member(question, option, label);

	return TRUE;
}

/* The organization's recipe categories, {id, path}, by path. */
static gint
craft_compare_paths(
	gconstpointer	a,
	gconstpointer	b
){
	JsonObject *x = *(JsonObject *const *)a;
	JsonObject *y = *(JsonObject *const *)b;

	return g_utf8_collate(json_object_get_string_member(x, "path"), json_object_get_string_member(y, "path"));
}

static JsonArray *
craft_categories(
	VentureDatabase	*database,
	gint64		 organization_id,
	GHashTable	*paths
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) found = NULL;
	g_autoptr(GPtrArray) sorted = NULL;
	JsonArray *array;
	guint i;

	array = json_array_new();
	query = venture_query_new(VENTURE_TYPE_CATEGORY);
	venture_query_set_organization(query, organization_id);
	venture_query_set_limit(query, 2000);

	if (!venture_query_add_filter_string(query, "applies-to", VENTURE_FILTER_OP_EQ, "recipe", NULL))
		return array;

	found = venture_database_find(database, query, NULL);
	sorted = g_ptr_array_new_with_free_func((GDestroyNotify)json_object_unref);

	for (i = 0; (NULL != found) && (i < found->len); i++)
	{
		VentureEntity *category = g_ptr_array_index(found, i);
		gint64 id = venture_entity_get_id(category);
		g_autofree gchar *path = venture_category_path(database, VENTURE_TYPE_CATEGORY, id, NULL);
		JsonObject *object;

		if (NULL == path)
			continue;

		object = json_object_new();
		json_object_set_int_member(object, "id", id);
		json_object_set_string_member(object, "path", path);
		g_ptr_array_add(sorted, object);
		g_hash_table_insert(paths, g_memdup2(&id, sizeof(id)), g_strdup(path));
	}

	g_ptr_array_sort(sorted, craft_compare_paths);

	for (i = 0; i < sorted->len; i++)
		json_array_add_object_element(array, json_object_ref(g_ptr_array_index(sorted, i)));

	return array;
}

JsonNode *
venture_arbitrage_crafting(
	VentureContext	 *context,
	gint64		  organization_id,
	JsonObject	 *asked,
	GError		**error
){
	g_autoptr(JsonObject) scan_options = NULL;
	g_autoptr(JsonObject) question = NULL;
	g_autoptr(JsonNode) answer = NULL;
	g_autoptr(JsonNode) choices = NULL;
	g_autoptr(JsonNode) groups = NULL;
	g_autoptr(GHashTable) paths = NULL;
	g_autoptr(GHashTable) unpriced = NULL;
	g_autoptr(GList) members = NULL;
	VentureDatabase *database;
	JsonObject *root;
	JsonArray *rows;
	JsonArray *realms;
	JsonArray *kept;
	GList *member;
	guint i;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);

	if (organization_id <= 0)
		organization_id = venture_context_get_default_organization_id(context);

	database = venture_context_get_database(context);
	scan_options = json_object_new();
	question = json_object_new();
	members = (NULL != asked) ? json_object_get_members(asked) : NULL;

	/* The question: a name nobody knows is refused, as the scan refuses
	 * one -- a misspelt filter is a filter not applied. */
	for (member = members; NULL != member; member = member->next)
	{
		g_autofree gchar *text = NULL;

		if (!g_strv_contains(craft_options, member->data))
		{
			g_autofree gchar *known = g_strjoinv(", ", (gchar **)craft_options);

			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			            "Crafting takes %s; %s is not one of them", known, (const gchar *)member->data);
			return NULL;
		}

		text = venture_arbitrage_node_text(json_object_get_member(asked, member->data));

		if (NULL != text)
			g_strstrip(text);

		if (venture_string_is_empty(text))
			continue;

		json_object_set_string_member(question, member->data, text);

		if (g_strv_contains(craft_passed, member->data))
			json_object_set_string_member(scan_options, member->data, text);
	}

	/* Every recipe priced, at most a page of the scan's top: the losses
	 * too, since a craft that loses money is an answer as well. */
	json_object_set_string_member(scan_options, "strategy", "transform");
	json_object_set_int_member(scan_options, "top", VENTURE_ARBITRAGE_SCAN_TOP_MAX);

	if (!craft_realm(context, organization_id, "buy_realm",
	                 venture_json_object_get_string(question, "buy_realm", NULL), scan_options, "buy_venues",
	                 question, error) ||
	    !craft_realm(context, organization_id, "sell_realm",
	                 venture_json_object_get_string(question, "sell_realm", NULL), scan_options, "sell_venues",
	                 question, error))
		return NULL;

	answer = venture_arbitrage_scan_run_full(context, organization_id, scan_options, TRUE, error);

	if (NULL == answer)
		return NULL;

	root = json_node_get_object(answer);
	rows = json_object_get_array_member(root, "rows");
	paths = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, g_free);
	unpriced = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	json_object_set_array_member(root, "categories", craft_categories(database, organization_id, paths));

	/* Each row as the strategy wrote it, its recipe's category path
	 * beside it for the filter column. */
	for (i = 0; (NULL != rows) && (i < json_array_get_length(rows)); i++)
	{
		JsonObject *row = json_array_get_object_element(rows, i);
		gint64 category_id = venture_json_object_get_int(row, "recipe_category_id", 0);
		JsonArray *warnings;
		guint j;

		if (category_id > 0)
		{
			const gchar *path = g_hash_table_lookup(paths, &category_id);
			g_autofree gchar *read = NULL;

			if (NULL == path)
			{
				read = venture_category_path(database, VENTURE_TYPE_CATEGORY, category_id, NULL);
				path = read;
			}

			if (NULL != path)
				json_object_set_string_member(row, "recipe_category", path);
		}

		warnings = json_object_has_member(row, "warnings") ? json_object_get_array_member(row, "warnings")
		                                                    : NULL;

		for (j = 0; (NULL != warnings) && (j < json_array_get_length(warnings)); j++)
		{
			const gchar *warning = json_array_get_string_element(warnings, j);
			JsonNode *sell = json_object_get_member(row, "sell");

			if ((NULL != warning) && (NULL != strstr(warning, "no fees counted")) && (NULL != sell) &&
			    JSON_NODE_HOLDS_OBJECT(sell))
				g_hash_table_add(unpriced, g_strdup(venture_json_object_get_string(json_node_get_object(sell),
				                                                                   "venue_name", "")));
		}
	}

	if (g_hash_table_size(unpriced) > 0)
	{
		g_autofree gchar *note = NULL;
		guint count = g_hash_table_size(unpriced);

		note = g_strdup_printf("%u venue%s sold at here %s no fee model, so the auction house's cut and "
		                       "deposit are not counted there and those crafts read better than they are. "
		                       "Give each venue record its fee model (wow_auction for a World of "
		                       "Warcraft realm).", count, (1 == count) ? "" : "s",
		                       (1 == count) ? "has" : "have");
		json_array_add_string_element(json_object_get_array_member(root, "notes"), note);
	}

	/* The pickers: a realm per connected realm (the region's commodity
	 * market is open from every one of them, so it is no choice), the
	 * venue groups, and the realms the answer used. */
	if (!venture_marketdata_realm_choices(context, organization_id,
	                                      venture_json_object_get_string(question, "venue_group", NULL),
	                                      &choices, error))
		return NULL;

	realms = json_node_get_array(choices);
	kept = json_array_new();

	for (i = 0; i < json_array_get_length(realms); i++)
	{
		JsonObject *choice = json_array_get_object_element(realms, i);

		if (!venture_json_object_get_bool(choice, "region_wide", FALSE))
			json_array_add_object_element(kept, json_object_ref(choice));
	}

	json_object_set_array_member(root, "realm_choices", kept);
	groups = venture_marketdata_venue_group_choices(context, organization_id);
	json_object_set_array_member(root, "venue_groups", json_array_ref(json_node_get_array(groups)));
	json_object_set_object_member(root, "question", json_object_ref(question));
	json_object_set_int_member(root, "stale_after_seconds", venture_marketdata_stale_seconds(context));

	return g_steal_pointer(&answer);
}

/* ==========================================================================
 * The flip planner: regrouping
 * ========================================================================== */

/* Money per currency: one slot per figure the totals add up. */
typedef struct
{
	gchar		*currency;
	VentureMoney	*outlay;
	VentureMoney	*gross;
	VentureMoney	*cut;
	VentureMoney	*deposit;
	VentureMoney	*listing_loss;
	guint		 trades;
} PlanTotals;

static void
plan_totals_free(gpointer data)
{
	PlanTotals *totals = data;

	g_free(totals->currency);
	g_clear_pointer(&totals->outlay, venture_money_free);
	g_clear_pointer(&totals->gross, venture_money_free);
	g_clear_pointer(&totals->cut, venture_money_free);
	g_clear_pointer(&totals->deposit, venture_money_free);
	g_clear_pointer(&totals->listing_loss, venture_money_free);
	g_free(totals);
}

/* One line of a venue's list: an instrument (at a price, when selling). */
typedef struct
{
	gint64		 instrument_id;
	gchar		*instrument_name;
	gchar		*instrument_key;
	gint64		 data_source_id;
	gint64		 quantity;
	VentureMoney	*unit_price;	/* buy: the dearest planned; sell: the price */
	VentureMoney	*amount;
	VentureMoney	*fees;
	GArray		*trades;	/* gint64 */
} PlanLine;

static void
plan_line_free(gpointer data)
{
	PlanLine *line = data;

	g_free(line->instrument_name);
	g_free(line->instrument_key);
	g_clear_pointer(&line->unit_price, venture_money_free);
	g_clear_pointer(&line->amount, venture_money_free);
	g_clear_pointer(&line->fees, venture_money_free);
	g_array_unref(line->trades);
	g_free(line);
}

/* One venue's part of the list. */
typedef struct
{
	gint64		 venue_id;
	gchar		*venue_name;
	gchar		*venue_key;
	gint64		 data_source_id;
	GPtrArray	*lines;		/* PlanLine, in the order first planned */
	GHashTable	*by_key;	/* line key -> PlanLine */
	GHashTable	*totals;	/* currency -> VentureMoney (amount and fees) */
} PlanVenue;

static void
plan_venue_free(gpointer data)
{
	PlanVenue *venue = data;

	g_free(venue->venue_name);
	g_free(venue->venue_key);
	g_ptr_array_unref(venue->lines);
	g_hash_table_unref(venue->by_key);
	g_hash_table_unref(venue->totals);
	g_free(venue);
}

/* *@slot += @part, starting from nothing; a mismatch or an overflow is an
 * error, never a figure left short. */
static gboolean
plan_add(
	VentureMoney		**slot,
	const VentureMoney	 *part,
	GError			**error
){
	VentureMoney *sum;

	if (NULL == part)
		return TRUE;

	if (NULL == *slot)
	{
		*slot = venture_money_copy(part);
		return TRUE;
	}

	sum = venture_money_add(*slot, part, error);

	if (NULL == sum)
		return FALSE;

	venture_money_free(*slot);
	*slot = sum;

	return TRUE;
}

/* A money member written as text ("12.50 GOLD"); NULL when absent. A text
 * that is not money is an error: a plan read short is worse than none. */
static gboolean
plan_money(
	JsonObject	 *object,
	const gchar	 *member,
	VentureMoney	**out,
	GError		**error
){
	const gchar *text;

	*out = NULL;
	text = venture_json_object_get_string(object, member, NULL);

	if (venture_string_is_empty(text))
		return TRUE;

	*out = venture_money_from_string(text, NULL, error);

	if (NULL == *out)
	{
		g_prefix_error(error, "%s: ", member);
		return FALSE;
	}

	return TRUE;
}

static PlanTotals *
plan_totals_for(
	GHashTable	*totals,
	GPtrArray	*order,
	const gchar	*currency
){
	PlanTotals *found = g_hash_table_lookup(totals, currency);

	if (NULL == found)
	{
		found = g_new0(PlanTotals, 1);
		found->currency = g_strdup(currency);
		g_hash_table_insert(totals, g_strdup(currency), found);
		g_ptr_array_add(order, found);
	}

	return found;
}

static PlanVenue *
plan_venue_for(
	GHashTable	*venues,
	GPtrArray	*order,
	JsonObject	*leg
){
	g_autofree gchar *key = NULL;
	PlanVenue *venue;
	gint64 venue_id;

	venue_id = venture_json_object_get_int(leg, "venue_id", 0);
	key = (venue_id > 0)
		? g_strdup_printf("#%" G_GINT64_FORMAT, venue_id)
		: g_strdup_printf("%" G_GINT64_FORMAT "\037%s", venture_json_object_get_int(leg, "data_source_id", 0),
		                  venture_json_object_get_string(leg, "venue_key", ""));
	venue = g_hash_table_lookup(venues, key);

	if (NULL != venue)
		return venue;

	venue = g_new0(PlanVenue, 1);
	venue->venue_id = venue_id;
	venue->venue_name = g_strdup(venture_json_object_get_string(leg, "venue_name",
	                             venture_json_object_get_string(leg, "venue_key", "Unknown venue")));
	venue->venue_key = g_strdup(venture_json_object_get_string(leg, "venue_key", NULL));
	venue->data_source_id = venture_json_object_get_int(leg, "data_source_id", 0);
	venue->lines = g_ptr_array_new_with_free_func(plan_line_free);
	venue->by_key = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	venue->totals = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                      (GDestroyNotify)venture_money_free);
	g_hash_table_insert(venues, g_steal_pointer(&key), venue);
	g_ptr_array_add(order, venue);

	return venue;
}

/*
 * Adds one leg to its venue's list. Buys merge by instrument, keeping the
 * dearest unit price as the most a unit may cost; sells merge by
 * instrument and price, since what to post is a price as well as a count.
 */
static gboolean
plan_add_leg(
	PlanVenue	 *venue,
	JsonObject	 *leg,
	gboolean	  selling,
	gint64		  trade_id,
	VentureMoney	 *unit_price,
	VentureMoney	 *amount,
	VentureMoney	 *fees,
	GError		**error
){
	g_autofree gchar *key = NULL;
	g_autofree gchar *price = NULL;
	PlanLine *line;
	VentureMoney *before;
	gint64 instrument_id;
	gint64 quantity;

	instrument_id = venture_json_object_get_int(leg, "instrument_id", 0);
	quantity = venture_json_object_get_int(leg, "quantity", 0);
	price = (selling && (NULL != unit_price)) ? venture_money_to_string(unit_price) : g_strdup("");
	key = (instrument_id > 0)
		? g_strdup_printf("#%" G_GINT64_FORMAT "\037%s", instrument_id, price)
		: g_strdup_printf("%s\037%s", venture_json_object_get_string(leg, "instrument_key",
		                  venture_json_object_get_string(leg, "notes", "")), price);
	line = g_hash_table_lookup(venue->by_key, key);

	if (NULL == line)
	{
		line = g_new0(PlanLine, 1);
		line->instrument_id = instrument_id;
		line->instrument_name = g_strdup(venture_json_object_get_string(leg, "instrument_name",
		                                 venture_json_object_get_string(leg, "notes", NULL)));
		line->instrument_key = g_strdup(venture_json_object_get_string(leg, "instrument_key", NULL));
		line->data_source_id = venture_json_object_get_int(leg, "data_source_id", venue->data_source_id);
		line->trades = g_array_new(FALSE, FALSE, sizeof(gint64));
		g_hash_table_insert(venue->by_key, g_steal_pointer(&key), line);
		g_ptr_array_add(venue->lines, line);
	}

	line->quantity += MAX((gint64)0, quantity);
	g_array_append_val(line->trades, trade_id);

	if (NULL != unit_price)
	{
		if (NULL == line->unit_price)
			line->unit_price = venture_money_copy(unit_price);
		else if (!selling && (0 == g_strcmp0(venture_money_get_currency(unit_price),
		                                     venture_money_get_currency(line->unit_price))) &&
		         (venture_money_compare(unit_price, line->unit_price) > 0))
		{
			venture_money_free(line->unit_price);
			line->unit_price = venture_money_copy(unit_price);
		}
	}

	if (!plan_add(&line->amount, amount, error) || !plan_add(&line->fees, fees, error))
		return FALSE;

	/* The venue's own total, per currency: what it takes (a buy) or
	 * brings (a sell), its fees with it. */
	if (NULL != amount)
	{
		const gchar *currency = venture_money_get_currency(amount);
		g_autoptr(VentureMoney) sum = NULL;

		before = g_hash_table_lookup(venue->totals, currency);
		sum = (NULL != before) ? venture_money_copy(before) : NULL;

		if (!plan_add(&sum, amount, error))
			return FALSE;

		if ((NULL != fees) && (0 == g_strcmp0(venture_money_get_currency(fees), currency)))
		{
			g_autoptr(VentureMoney) adjusted = NULL;

			adjusted = selling ? venture_money_subtract(sum, fees, error) : venture_money_add(sum, fees, error);

			if (NULL == adjusted)
				return FALSE;

			g_clear_pointer(&sum, venture_money_free);
			sum = g_steal_pointer(&adjusted);
		}

		g_hash_table_insert(venue->totals, g_strdup(currency), g_steal_pointer(&sum));
	}

	return TRUE;
}

static gint
plan_compare_venues(
	gconstpointer	a,
	gconstpointer	b
){
	const PlanVenue *x = *(const PlanVenue *const *)a;
	const PlanVenue *y = *(const PlanVenue *const *)b;

	return g_utf8_collate(x->venue_name, y->venue_name);
}

static gint
plan_compare_lines(
	gconstpointer	a,
	gconstpointer	b
){
	const PlanLine *x = *(const PlanLine *const *)a;
	const PlanLine *y = *(const PlanLine *const *)b;

	return g_utf8_collate((NULL != x->instrument_name) ? x->instrument_name : "",
	                      (NULL != y->instrument_name) ? y->instrument_name : "");
}

static JsonArray *
plan_venues_json(GPtrArray *venues)
{
	JsonArray *array;
	guint i;
	guint j;

	array = json_array_new();
	g_ptr_array_sort(venues, plan_compare_venues);

	for (i = 0; i < venues->len; i++)
	{
		PlanVenue *venue = g_ptr_array_index(venues, i);
		JsonObject *object = json_object_new();
		JsonArray *lines = json_array_new();
		JsonArray *totals = json_array_new();
		g_autoptr(GList) currencies = NULL;
		GList *currency;

		if (venue->venue_id > 0)
			json_object_set_int_member(object, "venue_id", venue->venue_id);

		json_object_set_string_member(object, "venue_name", venue->venue_name);

		if (NULL != venue->venue_key)
			json_object_set_string_member(object, "venue_key", venue->venue_key);

		if (venue->data_source_id > 0)
			json_object_set_int_member(object, "data_source_id", venue->data_source_id);

		g_ptr_array_sort(venue->lines, plan_compare_lines);

		for (j = 0; j < venue->lines->len; j++)
		{
			PlanLine *line = g_ptr_array_index(venue->lines, j);
			JsonObject *one = json_object_new();
			JsonArray *trades = json_array_new();
			guint k;

			if (line->instrument_id > 0)
				json_object_set_int_member(one, "instrument_id", line->instrument_id);

			json_object_set_string_member(one, "instrument_name",
			                              (NULL != line->instrument_name) ? line->instrument_name : "");

			if (NULL != line->instrument_key)
				json_object_set_string_member(one, "instrument_key", line->instrument_key);

			if (line->data_source_id > 0)
				json_object_set_int_member(one, "data_source_id", line->data_source_id);

			json_object_set_int_member(one, "quantity", line->quantity);
			venture_arbitrage_set_money(one, "unit_price", line->unit_price);
			venture_arbitrage_set_money(one, "amount", line->amount);
			venture_arbitrage_set_money(one, "fees", line->fees);

			for (k = 0; k < line->trades->len; k++)
				json_array_add_int_element(trades, g_array_index(line->trades, gint64, k));

			json_object_set_array_member(one, "trades", trades);
			json_array_add_object_element(lines, one);
		}

		currencies = g_hash_table_get_keys(venue->totals);
		currencies = g_list_sort(currencies, (GCompareFunc)g_strcmp0);

		for (currency = currencies; NULL != currency; currency = currency->next)
		{
			JsonObject *total = json_object_new();

			json_object_set_string_member(total, "currency", currency->data);
			venture_arbitrage_set_money(total, "amount", g_hash_table_lookup(venue->totals, currency->data));
			json_array_add_object_element(totals, total);
		}

		json_object_set_array_member(object, "lines", lines);
		json_object_set_array_member(object, "totals", totals);
		json_array_add_object_element(array, object);
	}

	return array;
}

/* A trade's expected deposit or listing loss: a money string, or an array
 * of them (one per currency), as `profit` may be. */
static gboolean
plan_expected(
	JsonObject	 *expected,
	const gchar	 *member,
	GHashTable	 *totals,
	GPtrArray	 *order,
	gboolean	  listing_loss,
	GError		**error
){
	g_autoptr(VentureMoney) money = NULL;
	PlanTotals *slot;

	if ((NULL == expected) || !json_object_has_member(expected, member))
		return TRUE;

	if (!plan_money(expected, member, &money, error))
		return FALSE;

	if (NULL == money)
		return TRUE;

	slot = plan_totals_for(totals, order, venture_money_get_currency(money));

	return plan_add(listing_loss ? &slot->listing_loss : &slot->deposit, money, error);
}

/* What a total is: @a less @b and @c, each optional. */
static VentureMoney *
plan_less(
	const VentureMoney	 *a,
	const VentureMoney	 *b,
	const VentureMoney	 *c,
	const gchar		 *currency,
	GError			**error
){
	g_autoptr(VentureMoney) out = NULL;

	out = (NULL != a) ? venture_money_copy(a) : venture_money_new_for_currency(0, currency);

	if (NULL != b)
	{
		VentureMoney *next = venture_money_subtract(out, b, error);

		if (NULL == next)
			return NULL;

		venture_money_free(out);
		out = next;
	}

	if (NULL != c)
	{
		VentureMoney *next = venture_money_subtract(out, c, error);

		if (NULL == next)
			return NULL;

		venture_money_free(out);
		out = next;
	}

	return g_steal_pointer(&out);
}

static gint
plan_compare_totals(
	gconstpointer	a,
	gconstpointer	b
){
	return g_strcmp0((*(PlanTotals *const *)a)->currency, (*(PlanTotals *const *)b)->currency);
}

JsonObject *
venture_arbitrage_planner_build(
	JsonArray	 *trades,
	GError		**error
){
	g_autoptr(GHashTable) buy_venues = NULL;
	g_autoptr(GHashTable) sell_venues = NULL;
	g_autoptr(GHashTable) fee_venues = NULL;
	g_autoptr(GPtrArray) buy_order = NULL;
	g_autoptr(GPtrArray) sell_order = NULL;
	g_autoptr(GPtrArray) fee_order = NULL;
	g_autoptr(GHashTable) totals = NULL;
	g_autoptr(GPtrArray) totals_order = NULL;
	g_autoptr(JsonObject) out = NULL;
	JsonArray *totals_json;
	guint i;
	guint j;

	buy_venues = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	sell_venues = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	fee_venues = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	buy_order = g_ptr_array_new_with_free_func(plan_venue_free);
	sell_order = g_ptr_array_new_with_free_func(plan_venue_free);
	fee_order = g_ptr_array_new_with_free_func(plan_venue_free);
	totals = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	totals_order = g_ptr_array_new_with_free_func(plan_totals_free);

	for (i = 0; (NULL != trades) && (i < json_array_get_length(trades)); i++)
	{
		JsonObject *trade = json_array_get_object_element(trades, i);
		gint64 trade_id = venture_json_object_get_int(trade, "id", 0);
		JsonNode *expected_node = json_object_get_member(trade, "expected");
		JsonObject *expected = ((NULL != expected_node) && JSON_NODE_HOLDS_OBJECT(expected_node))
			? json_node_get_object(expected_node) : NULL;
		JsonArray *legs = json_object_has_member(trade, "legs") ? json_object_get_array_member(trade, "legs")
		                                                        : NULL;
		g_autoptr(GHashTable) counted = g_hash_table_new(g_str_hash, g_str_equal);

		for (j = 0; (NULL != legs) && (j < json_array_get_length(legs)); j++)
		{
			JsonObject *leg = json_array_get_object_element(legs, j);
			const gchar *kind = venture_json_object_get_string(leg, "kind", "buy");
			const gchar *status = venture_json_object_get_string(leg, "status", "planned");
			g_autoptr(VentureMoney) unit_price = NULL;
			g_autoptr(VentureMoney) amount = NULL;
			g_autoptr(VentureMoney) fees = NULL;
			gboolean buying = (0 == g_strcmp0(kind, "buy")) || (0 == g_strcmp0(kind, "stake"));
			gboolean selling = (0 == g_strcmp0(kind, "sell")) || (0 == g_strcmp0(kind, "payout"));
			gboolean fee = (0 == g_strcmp0(kind, "fee")) || (0 == g_strcmp0(kind, "transfer"));
			PlanTotals *slot;

			/* Only what is still to do: an executed leg is the books'
			 * now, a cancelled or failed one is not happening. */
			if (0 != g_strcmp0(status, "planned"))
				continue;

			if (!buying && !selling && !fee)
				continue;

			if (!plan_money(leg, "unit_price", &unit_price, error) ||
			    !plan_money(leg, "amount", &amount, error) ||
			    !plan_money(leg, "fees", &fees, error))
			{
				g_prefix_error(error, "Trade #%" G_GINT64_FORMAT ": ", trade_id);
				return NULL;
			}

			if (NULL == amount)
				continue;

			slot = plan_totals_for(totals, totals_order, venture_money_get_currency(amount));
			g_hash_table_add(counted, (gpointer)slot->currency);

			if (selling)
			{
				if (!plan_add(&slot->gross, amount, error) || !plan_add(&slot->cut, fees, error) ||
				    !plan_add_leg(plan_venue_for(sell_venues, sell_order, leg), leg, TRUE, trade_id,
				                  unit_price, amount, fees, error))
					return NULL;
			}
			else
			{
				if (!plan_add(&slot->outlay, amount, error) || !plan_add(&slot->outlay, fees, error) ||
				    !plan_add_leg(plan_venue_for(buying ? buy_venues : fee_venues,
				                                 buying ? buy_order : fee_order, leg),
				                  leg, FALSE, trade_id, unit_price, amount, fees, error))
					return NULL;
			}
		}

		if (!plan_expected(expected, "deposit", totals, totals_order, FALSE, error) ||
		    !plan_expected(expected, "listing_loss", totals, totals_order, TRUE, error))
		{
			g_prefix_error(error, "Trade #%" G_GINT64_FORMAT ": ", trade_id);
			return NULL;
		}

		/* A trade counts once in each currency it spends or brings. */
		{
			GHashTableIter iter;
			gpointer currency;

			g_hash_table_iter_init(&iter, counted);

			while (g_hash_table_iter_next(&iter, &currency, NULL))
				((PlanTotals *)g_hash_table_lookup(totals, currency))->trades++;
		}
	}

	out = json_object_new();
	json_object_set_array_member(out, "buy", plan_venues_json(buy_order));
	json_object_set_array_member(out, "sell", plan_venues_json(sell_order));
	json_object_set_array_member(out, "fees", plan_venues_json(fee_order));
	totals_json = json_array_new();
	g_ptr_array_sort(totals_order, plan_compare_totals);

	/* One block per currency, never a sum of two: revenue is the sales
	 * less the cut less the deposits relisting is expected to lose,
	 * profit is that less every coin spent getting there. */
	for (i = 0; i < totals_order->len; i++)
	{
		PlanTotals *slot = g_ptr_array_index(totals_order, i);
		g_autoptr(VentureMoney) revenue = NULL;
		g_autoptr(VentureMoney) profit = NULL;
		JsonObject *total = json_object_new();

		revenue = plan_less(slot->gross, slot->cut, slot->listing_loss, slot->currency, error);
		profit = (NULL != revenue) ? plan_less(revenue, slot->outlay, NULL, slot->currency, error) : NULL;

		if (NULL == profit)
		{
			json_object_unref(total);
			json_array_unref(totals_json);
			g_prefix_error(error, "Totals in %s: ", slot->currency);
			return NULL;
		}

		json_object_set_string_member(total, "currency", slot->currency);
		json_object_set_int_member(total, "trades", slot->trades);
		venture_arbitrage_set_money(total, "outlay", slot->outlay);
		venture_arbitrage_set_money(total, "gross", slot->gross);
		venture_arbitrage_set_money(total, "cut", slot->cut);
		venture_arbitrage_set_money(total, "deposit", slot->deposit);
		venture_arbitrage_set_money(total, "listing_loss", slot->listing_loss);
		venture_arbitrage_set_money(total, "revenue", revenue);
		venture_arbitrage_set_money(total, "profit", profit);
		json_array_add_object_element(totals_json, total);
	}

	json_object_set_array_member(out, "totals", totals_json);

	return g_steal_pointer(&out);
}

/* ==========================================================================
 * The flip planner: reading the plan
 * ========================================================================== */

/* A record's name, read once per answer. */
static gchar *
plan_name_of(
	VentureDatabase	*database,
	GHashTable	*cache,
	GType		 type,
	gint64		 id,
	gchar		**out_key,
	gint64		*out_source
){
	g_autofree gchar *cache_key = NULL;
	gchar **cached;

	*out_key = NULL;
	*out_source = 0;

	if (id <= 0)
		return NULL;

	cache_key = g_strdup_printf("%s\037%" G_GINT64_FORMAT, g_type_name(type), id);
	cached = g_hash_table_lookup(cache, cache_key);

	if (NULL == cached)
	{
		g_autoptr(VentureEntity) record = venture_database_get(database, type, id, NULL);
		gint64 source = 0;

		cached = g_new0(gchar *, 4);

		if (NULL != record)
		{
			cached[0] = venture_entity_get_display_name(record);
			g_object_get(record, "key", &cached[1], "data-source-id", &source, NULL);
			cached[2] = g_strdup_printf("%" G_GINT64_FORMAT, source);
		}

		g_hash_table_insert(cache, g_steal_pointer(&cache_key), cached);
	}

	*out_key = g_strdup(cached[1]);
	*out_source = (NULL != cached[2]) ? g_ascii_strtoll(cached[2], NULL, 10) : 0;

	return g_strdup(cached[0]);
}

static void
plan_money_member(
	JsonObject	*object,
	const gchar	*member,
	VentureEntity	*entity,
	const gchar	*property
){
	VentureMoney *money = NULL;
	g_autofree gchar *text = NULL;

	g_object_get(entity, property, &money, NULL);

	if (NULL == money)
		return;

	text = venture_money_to_string(money);
	json_object_set_string_member(object, member, text);
	venture_money_free(money);
}

/* The planned trades and their legs, as venture_arbitrage_planner_build()
 * reads them. */
static JsonArray *
plan_read(
	VentureDatabase	 *database,
	gint64		  organization_id,
	GError		**error
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureQuery) leg_query = NULL;
	g_autoptr(GPtrArray) trades = NULL;
	g_autoptr(GPtrArray) legs = NULL;
	g_autoptr(GPtrArray) ids = NULL;
	g_autoptr(GHashTable) by_id = NULL;
	g_autoptr(GHashTable) names = NULL;
	JsonArray *array;
	guint i;

	query = venture_query_new(VENTURE_TYPE_ARBITRAGE_TRADE);
	venture_query_set_organization(query, organization_id);
	venture_query_set_limit(query, VENTURE_ARBITRAGE_PLANNER_MAX_TRADES + 1);

	if (!venture_query_add_filter_string(query, "status", VENTURE_FILTER_OP_EQ, "planned", error) ||
	    !venture_query_add_order(query, "id", VENTURE_SORT_DESCENDING, error))
		return NULL;

	trades = venture_database_find(database, query, error);

	if (NULL == trades)
		return NULL;

	/* All of the plan or a refusal: a shopping list that leaves trades
	 * out buys short. */
	if (trades->len > VENTURE_ARBITRAGE_PLANNER_MAX_TRADES)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT,
		            "More than %d trades are planned; finish, abandon or remove some first",
		            VENTURE_ARBITRAGE_PLANNER_MAX_TRADES);
		return NULL;
	}

	array = json_array_new();
	by_id = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	names = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_strfreev);
	ids = g_ptr_array_new_with_free_func(g_free);

	for (i = 0; i < trades->len; i++)
	{
		VentureEntity *trade = g_ptr_array_index(trades, i);
		JsonObject *object = json_object_new();
		g_autofree gchar *name = NULL;
		g_autofree gchar *strategy = NULL;
		g_autofree gchar *expected = NULL;
		g_autoptr(GDateTime) created = NULL;
		gint64 id;

		g_object_get(trade, "name", &name, "strategy", &strategy, "expected", &expected, "created-at",
		             &created, NULL);
		json_object_set_int_member(object, "id", venture_entity_get_id(trade));
		json_object_set_string_member(object, "name", (NULL != name) ? name : "");
		json_object_set_string_member(object, "strategy", (NULL != strategy) ? strategy : "");

		if (NULL != created)
		{
			g_autofree gchar *text = venture_time_to_string(created);

			json_object_set_string_member(object, "created_at", text);
		}

		if (!venture_string_is_empty(expected))
		{
			g_autoptr(JsonNode) node = json_from_string(expected, NULL);

			if ((NULL != node) && JSON_NODE_HOLDS_OBJECT(node))
				json_object_set_member(object, "expected", g_steal_pointer(&node));
		}

		json_object_set_array_member(object, "legs", json_array_new());
		json_array_add_object_element(array, object);
		id = venture_entity_get_id(trade);
		g_hash_table_insert(by_id, g_memdup2(&id, sizeof(id)), object);
		g_ptr_array_add(ids, g_strdup_printf("%" G_GINT64_FORMAT, venture_entity_get_id(trade)));
	}

	if (0 == ids->len)
		return array;

	leg_query = venture_query_new(VENTURE_TYPE_ARBITRAGE_LEG);
	venture_query_set_organization(leg_query, organization_id);
	venture_query_set_limit(leg_query, 0);

	if (!venture_query_add_filter(leg_query, "trade-id", VENTURE_FILTER_OP_IN, ids, error) ||
	    !venture_query_add_order(leg_query, "id", VENTURE_SORT_ASCENDING, error))
	{
		json_array_unref(array);
		return NULL;
	}

	legs = venture_database_find(database, leg_query, error);

	if (NULL == legs)
	{
		json_array_unref(array);
		return NULL;
	}

	for (i = 0; i < legs->len; i++)
	{
		VentureEntity *leg = g_ptr_array_index(legs, i);
		JsonObject *object;
		JsonObject *trade;
		g_autofree gchar *venue_name = NULL;
		g_autofree gchar *venue_key = NULL;
		g_autofree gchar *instrument_name = NULL;
		g_autofree gchar *instrument_key = NULL;
		gint64 trade_id;
		gint64 venue_id;
		gint64 instrument_id;
		gint64 quantity;
		gint64 venue_source;
		gint64 instrument_source;
		gint kind;
		gint status;

		g_object_get(leg, "trade-id", &trade_id, "venue-id", &venue_id, "instrument-id", &instrument_id,
		             "quantity", &quantity, "kind", &kind, "status", &status, NULL);
		trade = g_hash_table_lookup(by_id, &trade_id);

		if (NULL == trade)
			continue;

		object = json_object_new();
		json_object_set_int_member(object, "id", venture_entity_get_id(leg));
		json_object_set_string_member(object, "kind", venture_enum_to_nick(VENTURE_TYPE_ARBITRAGE_LEG_KIND, kind));
		json_object_set_string_member(object, "status",
		                              venture_enum_to_nick(VENTURE_TYPE_ARBITRAGE_LEG_STATUS, status));
		venue_name = plan_name_of(database, names, VENTURE_TYPE_VENUE, venue_id, &venue_key, &venue_source);
		instrument_name = plan_name_of(database, names, VENTURE_TYPE_INSTRUMENT, instrument_id, &instrument_key,
		                               &instrument_source);

		if (venue_id > 0)
			json_object_set_int_member(object, "venue_id", venue_id);

		if (NULL != venue_name)
			json_object_set_string_member(object, "venue_name", venue_name);

		if (NULL != venue_key)
			json_object_set_string_member(object, "venue_key", venue_key);

		if (instrument_id > 0)
			json_object_set_int_member(object, "instrument_id", instrument_id);

		if (NULL != instrument_name)
			json_object_set_string_member(object, "instrument_name", instrument_name);

		if (NULL != instrument_key)
			json_object_set_string_member(object, "instrument_key", instrument_key);

		if ((instrument_source > 0) || (venue_source > 0))
			json_object_set_int_member(object, "data_source_id",
			                           (instrument_source > 0) ? instrument_source : venue_source);

		json_object_set_int_member(object, "quantity", quantity);
		plan_money_member(object, "unit_price", leg, "unit-price");
		plan_money_member(object, "amount", leg, "amount");
		plan_money_member(object, "fees", leg, "fees");

		{
			g_autofree gchar *notes = NULL;

			g_object_get(leg, "notes", &notes, NULL);

			if (!venture_string_is_empty(notes))
				json_object_set_string_member(object, "notes", notes);
		}

		json_array_add_object_element(json_object_get_array_member(trade, "legs"), object);
	}

	return array;
}

/* Whether a row's buy side (or sell side) is stale: a flip says so on its
 * side, a craft on the row. */
static gboolean
plan_side_stale(
	JsonObject	*row,
	const gchar	*side
){
	g_autofree gchar *flat = g_strconcat(side, "_stale", NULL);
	JsonNode *node = json_object_get_member(row, side);

	if ((NULL != node) && JSON_NODE_HOLDS_OBJECT(node) &&
	    json_object_has_member(json_node_get_object(node), "stale"))
		return venture_json_object_get_bool(json_node_get_object(node), "stale", FALSE);

	return venture_json_object_get_bool(row, flat, FALSE);
}

/*
 * Asks one trade's question again and says how it stands. @answers keeps
 * each question's answer, so trades from one question ask once.
 */
static void
plan_reprice(
	VentureContext	*context,
	gint64		 organization_id,
	JsonObject	*trade,
	GHashTable	*answers
){
	g_autoptr(JsonNode) question_node = NULL;
	g_autofree gchar *question_text = NULL;
	JsonObject *reprice;
	JsonObject *expected;
	JsonObject *question;
	JsonNode *answer;
	JsonArray *rows;
	JsonObject *found;
	const gchar *key;
	const gchar *state;
	g_autofree gchar *message = NULL;
	guint i;

	reprice = json_object_new();
	json_object_set_object_member(trade, "reprice", reprice);
	expected = json_object_has_member(trade, "expected") ? json_object_get_object_member(trade, "expected") : NULL;
	question = ((NULL != expected) && json_object_has_member(expected, "question") &&
	            JSON_NODE_HOLDS_OBJECT(json_object_get_member(expected, "question")))
		? json_object_get_object_member(expected, "question") : NULL;
	key = (NULL != expected) ? venture_json_object_get_string(expected, "key", NULL) : NULL;

	/* What it promised, for the comparison. */
	if ((NULL != expected) && json_object_has_member(expected, "profit") &&
	    JSON_NODE_HOLDS_ARRAY(json_object_get_member(expected, "profit")) &&
	    (json_array_get_length(json_object_get_array_member(expected, "profit")) > 0))
	{
		g_autoptr(VentureMoney) planned = venture_money_from_string(
			json_array_get_string_element(json_object_get_array_member(expected, "profit"), 0), NULL, NULL);

		venture_arbitrage_set_money(reprice, "planned_net", planned);
	}

	if ((NULL == question) || venture_string_is_empty(key))
	{
		json_object_set_string_member(reprice, "state", "unknown");
		json_object_set_string_member(reprice, "message", "Recorded without the question that found it, so it "
		                                                  "cannot be asked again; check it by hand.");
		return;
	}

	question_node = json_node_new(JSON_NODE_OBJECT);
	json_node_set_object(question_node, question);
	question_text = json_to_string(question_node, FALSE);
	answer = g_hash_table_lookup(answers, question_text);

	if (NULL == answer)
	{
		g_autoptr(JsonObject) asked = json_object_new();
		g_autoptr(GError) error = NULL;
		g_autoptr(GList) members = json_object_get_members(question);
		GList *member;

		for (member = members; NULL != member; member = member->next)
			json_object_set_member(asked, member->data,
			                       json_node_copy(json_object_get_member(question, member->data)));

		/* Every row, losses kept: a trade that now loses money is the
		 * answer "no longer profitable", not "gone". */
		json_object_set_int_member(asked, "top", VENTURE_ARBITRAGE_SCAN_TOP_MAX);
		answer = venture_arbitrage_scan_run_full(context, organization_id, asked, TRUE, &error);

		if (NULL == answer)
		{
			JsonObject *failed = json_object_new();

			json_object_set_string_member(failed, "error", (NULL != error) ? error->message : "failed");
			answer = json_node_new(JSON_NODE_OBJECT);
			json_node_take_object(answer, failed);
		}

		g_hash_table_insert(answers, g_strdup(question_text), answer);
	}

	if (json_object_has_member(json_node_get_object(answer), "error"))
	{
		json_object_set_string_member(reprice, "state", "gone");
		message = g_strdup_printf("Its question can no longer be asked: %s",
		                          json_object_get_string_member(json_node_get_object(answer), "error"));
		json_object_set_string_member(reprice, "message", message);
		return;
	}

	rows = json_object_get_array_member(json_node_get_object(answer), "rows");
	found = NULL;

	for (i = 0; (NULL != rows) && (i < json_array_get_length(rows)) && (NULL == found); i++)
	{
		JsonObject *row = json_array_get_object_element(rows, i);

		if (0 == g_strcmp0(venture_json_object_get_string(row, "key", NULL), key))
			found = row;
	}

	if (NULL == found)
	{
		json_object_set_string_member(reprice, "state", "gone");
		json_object_set_string_member(reprice, "message", "No longer there: the listings moved, or a price "
		                                                  "is older than its question allows.");
		return;
	}

	{
		g_autoptr(VentureMoney) net = venture_arbitrage_get_money(found, "net");
		gboolean buy_stale = plan_side_stale(found, "buy");
		gboolean sell_stale = plan_side_stale(found, "sell");

		venture_arbitrage_set_money(reprice, "net", net);
		venture_arbitrage_set_ratio(reprice, "roi", venture_arbitrage_get_ratio(found, "roi"));
		json_object_set_boolean_member(reprice, "buy_stale", buy_stale);
		json_object_set_boolean_member(reprice, "sell_stale", sell_stale);

		if (json_object_has_member(found, "missing") &&
		    (json_array_get_length(json_object_get_array_member(found, "missing")) > 0))
		{
			JsonArray *missing = json_object_get_array_member(found, "missing");
			g_autoptr(GString) text = g_string_new("Unquoted now: ");

			for (i = 0; i < json_array_get_length(missing); i++)
				g_string_append_printf(text, "%s%s", (i > 0) ? "; " : "",
				                       json_array_get_string_element(missing, i));

			state = "unquoted";
			message = g_string_free(g_steal_pointer(&text), FALSE);
		}
		else if ((NULL == net) || (venture_money_get_amount(net) <= 0))
		{
			state = "unprofitable";
			message = g_strdup("No longer profitable on today's prices.");
		}
		else if (buy_stale || sell_stale)
		{
			state = "stale";
			message = g_strdup_printf("Still pays, but the %s price%s old: check the realm before acting.",
			                          (buy_stale && sell_stale) ? "buy and sell" : buy_stale ? "buy" : "sell",
			                          (buy_stale && sell_stale) ? "s are" : " is");
		}
		else
		{
			state = "ok";
			message = g_strdup("Still pays on today's prices.");
		}

		json_object_set_string_member(reprice, "state", state);
		json_object_set_string_member(reprice, "message", message);
	}
}

JsonNode *
venture_arbitrage_planner(
	VentureContext	 *context,
	gint64		  organization_id,
	gboolean	  reprice,
	GError		**error
){
	g_autoptr(JsonArray) trades = NULL;
	g_autoptr(JsonObject) built = NULL;
	g_autoptr(GHashTable) answers = NULL;
	g_autoptr(GList) members = NULL;
	JsonObject *root;
	JsonArray *notes;
	JsonNode *node;
	GList *member;
	guint repriced;
	guint i;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);

	if (!venture_context_module_enabled(context, "arbitrage"))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG, "The arbitrage module is off");
		return NULL;
	}

	if (organization_id <= 0)
		organization_id = venture_context_get_default_organization_id(context);

	trades = plan_read(venture_context_get_database(context), organization_id, error);

	if (NULL == trades)
		return NULL;

	built = venture_arbitrage_planner_build(trades, error);

	if (NULL == built)
		return NULL;

	notes = json_array_new();
	repriced = 0;

	if (reprice)
	{
		answers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)json_node_unref);

		for (i = 0; i < json_array_get_length(trades); i++)
		{
			if (repriced >= VENTURE_ARBITRAGE_PLANNER_MAX_REPRICE)
				break;

			plan_reprice(context, organization_id, json_array_get_object_element(trades, i), answers);
			repriced++;
		}

		if (repriced < json_array_get_length(trades))
		{
			g_autofree gchar *note = g_strdup_printf("Only the newest %d planned trades were priced again.",
			                                         VENTURE_ARBITRAGE_PLANNER_MAX_REPRICE);

			json_array_add_string_element(notes, note);
		}
	}

	if (0 == json_array_get_length(trades))
		json_array_add_string_element(notes, "Nothing is planned. Add a deal, a flip or a craft to the plan "
		                                     "from Deals, Arbitrage or Crafting.");

	root = json_object_new();
	json_object_set_boolean_member(root, "available", TRUE);
	json_object_set_boolean_member(root, "repriced", reprice);
	json_object_set_array_member(root, "trades", json_array_ref(trades));
	members = json_object_get_members(built);

	for (member = members; NULL != member; member = member->next)
		json_object_set_member(root, member->data, json_node_copy(json_object_get_member(built, member->data)));

	json_object_set_array_member(root, "notes", notes);
	node = json_node_new(JSON_NODE_OBJECT);
	json_node_take_object(node, root);

	return node;
}

/* ==========================================================================
 * The flip planner: writing it out, and taking a trade off it
 * ========================================================================== */

static void
plan_csv_money(
	GString		*out,
	JsonObject	*object,
	const gchar	*member
){
	g_autoptr(VentureMoney) money = venture_arbitrage_get_money(object, member);
	g_autofree gchar *text = (NULL != money) ? venture_money_to_string(money) : NULL;

	g_string_append_c(out, ',');
	venture_arbitrage_csv_field(out, text);
}

static void
plan_csv_section(
	GString		*out,
	JsonObject	*answer,
	const gchar	*section
){
	JsonArray *venues;
	guint i;
	guint j;

	venues = json_object_has_member(answer, section) ? json_object_get_array_member(answer, section) : NULL;

	for (i = 0; (NULL != venues) && (i < json_array_get_length(venues)); i++)
	{
		JsonObject *venue = json_array_get_object_element(venues, i);
		JsonArray *lines = json_object_get_array_member(venue, "lines");

		for (j = 0; (NULL != lines) && (j < json_array_get_length(lines)); j++)
		{
			JsonObject *line = json_array_get_object_element(lines, j);
			g_autofree gchar *quantity = g_strdup_printf("%" G_GINT64_FORMAT,
			                                             venture_json_object_get_int(line, "quantity", 0));

			venture_arbitrage_csv_field(out, section);
			g_string_append_c(out, ',');
			venture_arbitrage_csv_field(out, venture_json_object_get_string(venue, "venue_name", ""));
			g_string_append_c(out, ',');
			venture_arbitrage_csv_field(out, venture_json_object_get_string(line, "instrument_name", ""));
			g_string_append_c(out, ',');
			venture_arbitrage_csv_field(out, venture_json_object_get_string(line, "instrument_key", ""));
			g_string_append_c(out, ',');
			venture_arbitrage_csv_field(out, quantity);
			plan_csv_money(out, line, "unit_price");
			plan_csv_money(out, line, "amount");
			plan_csv_money(out, line, "fees");
			g_string_append(out, "\r\n");
		}
	}
}

GBytes *
venture_arbitrage_planner_csv(JsonObject *answer)
{
	g_autoptr(GString) out = NULL;
	JsonArray *totals;
	guint i;

	out = g_string_new("section,venue,instrument,key,quantity,unit_price,amount,fees\r\n");

	if (NULL == answer)
		return g_string_free_to_bytes(g_steal_pointer(&out));

	plan_csv_section(out, answer, "buy");
	plan_csv_section(out, answer, "fees");
	plan_csv_section(out, answer, "sell");
	totals = json_object_has_member(answer, "totals") ? json_object_get_array_member(answer, "totals") : NULL;

	/* A total per currency, its figures in the amount column's place and
	 * named in the instrument's: outlay, revenue after the cut and the
	 * expected lost deposits, profit. */
	for (i = 0; (NULL != totals) && (i < json_array_get_length(totals)); i++)
	{
		JsonObject *total = json_array_get_object_element(totals, i);
		static const gchar *const figures[] = { "outlay", "gross", "cut", "deposit", "listing_loss", "revenue",
		                                        "profit", NULL };
		guint j;

		for (j = 0; NULL != figures[j]; j++)
		{
			g_string_append(out, "total,");
			venture_arbitrage_csv_field(out, venture_json_object_get_string(total, "currency", ""));
			g_string_append_c(out, ',');
			venture_arbitrage_csv_field(out, figures[j]);
			g_string_append(out, ",,");
			plan_csv_money(out, total, figures[j]);
			g_string_append(out, ",\r\n");
		}
	}

	return g_string_free_to_bytes(g_steal_pointer(&out));
}

JsonArray *
venture_arbitrage_planner_export_rows(JsonObject *answer)
{
	g_autoptr(GHashTable) seen = NULL;
	JsonArray *rows;
	JsonArray *venues;
	guint i;
	guint j;

	rows = json_array_new();
	seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	venues = ((NULL != answer) && json_object_has_member(answer, "buy"))
		? json_object_get_array_member(answer, "buy") : NULL;

	for (i = 0; (NULL != venues) && (i < json_array_get_length(venues)); i++)
	{
		JsonArray *lines = json_object_get_array_member(json_array_get_object_element(venues, i), "lines");

		for (j = 0; (NULL != lines) && (j < json_array_get_length(lines)); j++)
		{
			JsonObject *line = json_array_get_object_element(lines, j);
			const gchar *key = venture_json_object_get_string(line, "instrument_key", NULL);
			JsonObject *row;

			if ((NULL == key) || !g_hash_table_add(seen, g_strdup(key)))
				continue;

			row = json_object_new();
			json_object_set_string_member(row, "instrument_key", key);
			json_object_set_string_member(row, "instrument_name",
			                              venture_json_object_get_string(line, "instrument_name", ""));

			if (json_object_has_member(line, "data_source_id"))
				json_object_set_int_member(row, "data_source_id",
				                           venture_json_object_get_int(line, "data_source_id", 0));

			json_array_add_object_element(rows, row);
		}
	}

	return rows;
}

gboolean
venture_arbitrage_planner_remove(
	VentureContext		 *context,
	gint64			  organization_id,
	gint64			  trade_id,
	const VentureActor	 *actor,
	GError			**error
){
	g_autoptr(VentureEntity) trade = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) legs = NULL;
	VentureDatabase *database;
	gint status;
	guint i;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), FALSE);

	if (organization_id <= 0)
		organization_id = venture_context_get_default_organization_id(context);

	database = venture_context_get_database(context);
	trade = (trade_id > 0) ? venture_database_get(database, VENTURE_TYPE_ARBITRAGE_TRADE, trade_id, NULL) : NULL;

	if ((NULL == trade) || venture_entity_is_deleted(trade) ||
	    (venture_entity_get_organization_id(trade) != organization_id))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		            "No planned trade #%" G_GINT64_FORMAT " in this organization", trade_id);
		return FALSE;
	}

	g_object_get(trade, "status", &status, NULL);

	if (VENTURE_ARBITRAGE_TRADE_STATUS_PLANNED != status)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT,
		            "Trade #%" G_GINT64_FORMAT " is under way or finished: close or abandon it instead",
		            trade_id);
		return FALSE;
	}

	query = venture_query_new(VENTURE_TYPE_ARBITRAGE_LEG);
	venture_query_set_organization(query, organization_id);
	venture_query_set_limit(query, 0);

	if (!venture_query_add_filter_int(query, "trade-id", VENTURE_FILTER_OP_EQ, trade_id, error))
		return FALSE;

	legs = venture_database_find(database, query, error);

	if (NULL == legs)
		return FALSE;

	for (i = 0; i < legs->len; i++)
	{
		gint leg_status;

		g_object_get(g_ptr_array_index(legs, i), "status", &leg_status, NULL);

		if (VENTURE_ARBITRAGE_LEG_STATUS_EXECUTED == leg_status)
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT,
			            "Trade #%" G_GINT64_FORMAT " has an executed leg: close or abandon it instead",
			            trade_id);
			return FALSE;
		}
	}

	if (!venture_database_begin(database, error))
		return FALSE;

	for (i = 0; i < legs->len; i++)
	{
		if (!venture_database_delete(database, g_ptr_array_index(legs, i), actor, error))
		{
			venture_database_rollback(database);
			return FALSE;
		}
	}

	if (!venture_database_delete(database, trade, actor, error))
	{
		venture_database_rollback(database);
		return FALSE;
	}

	return venture_database_commit(database, error);
}
