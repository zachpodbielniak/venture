/*
 * venture-market-reports.c - Listing performance and price history
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Two reports over the market module's records. Both fetch rows through
 * the ordinary query -- organisation, access scope, module mask -- and
 * total them here in C, money keyed by currency before anything is added,
 * for the reason the aggregate report does: a SUM over a money column
 * adds cents to gold pieces. A row is a group *and* a currency, so a shop
 * selling in two currencies reads two rows, never one number in neither.
 */

#include "venture.h"

#include <string.h>

/* ==========================================================================
 * Shared
 * ========================================================================== */

/*
 * The organisation a report reads, checked to exist: a report about an
 * organisation that is not there would answer "nothing listed", which is
 * a different and wrong answer.
 */
static gboolean
market_organization(
	VentureContext	 *context,
	JsonObject	 *options,
	gint64		 *out_id,
	GError		**error
){
	g_autoptr(VentureEntity) organization = NULL;
	gint64 id;

	id = venture_context_get_default_organization_id(context);

	if (NULL != options)
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

/*
 * Runs @query, refusing a set larger than the aggregate report's bound
 * rather than totalling a truncated one: a smaller number presented as
 * the same one is the failure this avoids. @advice says how to get under
 * it, and every narrowing it names must already be in @query -- a filter
 * applied after this has not made the set any smaller.
 */
static GPtrArray *
market_fetch(
	VentureDatabase	 *database,
	VentureQuery	 *query,
	const gchar	 *what,
	const gchar	 *advice,
	GError		**error
){
	g_autoptr(GPtrArray) rows = NULL;

	venture_query_set_limit(query, (guint)venture_aggregate_get_max_rows() + 1);
	rows = venture_database_find(database, query, error);

	if (NULL == rows)
		return NULL;

	if (rows->len > (guint)venture_aggregate_get_max_rows())
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "More than %d %s match; %s",
		            venture_aggregate_get_max_rows(), what, advice);
		return NULL;
	}

	return g_steal_pointer(&rows);
}

/* Adds @value into *@total, taking a copy the first time. */
static gboolean
market_money_add(
	VentureMoney		**total,
	const VentureMoney	 *value,
	GError			**error
){
	VentureMoney *next;

	if (NULL == value)
		return TRUE;

	if (NULL == *total)
	{
		*total = venture_money_copy(value);
		return TRUE;
	}

	next = venture_money_add(*total, value, error);

	if (NULL == next)
		return FALSE;

	venture_money_free(*total);
	*total = next;

	return TRUE;
}

/* A product by id, read once per report however many rows name it. */
static VentureEntity *
market_product(
	VentureDatabase	*database,
	GHashTable	*cache,
	gint64		 id
){
	VentureEntity *product;

	product = g_hash_table_lookup(cache, &id);

	if (NULL == product)
	{
		gint64 *key;

		product = venture_database_get(database, VENTURE_TYPE_PRODUCT, id, NULL);

		if (NULL == product)
			return NULL;

		key = g_new(gint64, 1);
		*key = id;
		g_hash_table_insert(cache, key, product);
	}

	return product;
}

/* ==========================================================================
 * Listing performance
 * ========================================================================== */

typedef enum
{
	MARKET_GROUP_PRODUCT = 0,
	MARKET_GROUP_CATEGORY,
	MARKET_GROUP_CHANNEL
} MarketGroup;

typedef struct
{
	gchar		*label;
	gchar		*currency;

	gint64		 listings;
	gint64		 units_listed;
	gint64		 units_sold;

	/* The sale rate's two halves: units on listings that have ended,
	 * and how many of those sold. */
	gint64		 units_closed;
	gint64		 units_sold_closed;

	gint64		 sold;
	gint64		 partial;
	gint64		 expired;
	gint64		 cancelled;
	gint64		 open;

	/* Time to sell, over listings that sold out. */
	gint64		 sell_seconds;
	gint64		 sell_count;

	VentureMoney	*sold_value;
	VentureMoney	*deposits_lost;
	VentureMoney	*fees;
} ListingRow;

static void
listing_row_free(gpointer data)
{
	ListingRow *row;

	row = data;
	g_free(row->label);
	g_free(row->currency);
	g_clear_pointer(&row->sold_value, venture_money_free);
	g_clear_pointer(&row->deposits_lost, venture_money_free);
	g_clear_pointer(&row->fees, venture_money_free);
	g_free(row);
}

static gint
listing_row_compare(
	gconstpointer	a,
	gconstpointer	b
){
	const ListingRow *left;
	const ListingRow *right;
	gint order;

	left = *(ListingRow *const *)a;
	right = *(ListingRow *const *)b;
	order = g_utf8_collate(left->label, right->label);

	if (0 != order)
		return order;

	return g_strcmp0(left->currency, right->currency);
}

/*
 * The group a listing falls in, as a key (stable, unique) and a label
 * (what a person reads). Products are keyed by id, not name: two products
 * called the same are two rows.
 */
static gboolean
listing_group_of(
	VentureDatabase	 *database,
	MarketGroup	  group,
	gint		  category_depth,
	VentureEntity	 *listing,
	VentureEntity	 *product,
	GHashTable	 *paths,
	gchar		**out_key,
	gchar		**out_label,
	GError		**error
){
	switch (group)
	{
	case MARKET_GROUP_CHANNEL:
	{
		g_autofree gchar *channel = NULL;

		g_object_get(listing, "channel", &channel, NULL);

		if (venture_string_is_empty(channel))
		{
			*out_key = g_strdup("-");
			*out_label = g_strdup("No channel");
		}
		else
		{
			*out_key = g_strconcat("c:", channel, NULL);
			*out_label = g_strdup(channel);
		}

		return TRUE;
	}
	case MARKET_GROUP_CATEGORY:
	{
		const gchar *path;
		gint64 category_id;

		category_id = 0;

		if (NULL != product)
			g_object_get(product, "category-id", &category_id, NULL);

		if (0 == category_id)
		{
			*out_key = g_strdup("-");
			*out_label = g_strdup("Uncategorised");
			return TRUE;
		}

		/* Rolled up to its ancestor at the depth asked for; a
		 * shallower node is its own answer (see the category tree). */
		if (category_depth >= 0)
		{
			category_id = venture_category_ancestor_at_depth(database,
				VENTURE_TYPE_CATEGORY, category_id, (guint)category_depth,
				error);

			if (0 == category_id)
				return FALSE;
		}

		*out_key = g_strdup_printf("k:%" G_GINT64_FORMAT, category_id);
		path = g_hash_table_lookup(paths, *out_key);

		if (NULL == path)
		{
			gchar *computed;

			/* Computed, never stored: renaming a parent renames
			 * every row beneath it on the next run. */
			computed = venture_category_path(database, VENTURE_TYPE_CATEGORY,
			                                 category_id, error);

			if (NULL == computed)
			{
				g_clear_pointer(out_key, g_free);
				return FALSE;
			}

			g_hash_table_insert(paths, g_strdup(*out_key), computed);
			path = computed;
		}

		*out_label = g_strdup(path);
		return TRUE;
	}
	case MARKET_GROUP_PRODUCT:
	default:
	{
		gint64 product_id;

		g_object_get(listing, "product-id", &product_id, NULL);
		*out_key = g_strdup_printf("p:%" G_GINT64_FORMAT, product_id);
		*out_label = (NULL != product)
			? venture_entity_get_display_name(product)
			: g_strdup_printf("Product #%" G_GINT64_FORMAT, product_id);
		return TRUE;
	}
	}
}

/* Adds one listing to its row. */
static gboolean
listing_row_add(
	ListingRow	 *row,
	VentureEntity	 *listing,
	GError		**error
){
	g_autoptr(VentureMoney) unit_price = NULL;
	g_autoptr(VentureMoney) deposit = NULL;
	g_autoptr(VentureMoney) fees = NULL;
	g_autoptr(GDateTime) listed_at = NULL;
	g_autoptr(GDateTime) closed_at = NULL;
	VentureListingOutcome outcome;
	gint64 quantity;
	gint64 sold;

	g_object_get(listing, "unit-price", &unit_price, "deposit", &deposit,
	             "fees", &fees, "listed-at", &listed_at, "closed-at", &closed_at,
	             "outcome", &outcome, "quantity", &quantity,
	             "quantity-sold", &sold, NULL);

	row->listings++;
	row->units_listed += quantity;
	row->units_sold += sold;

	/* The sale rate reads only listings that have ended. An open one has
	 * not had its chance yet: counting its unsold units as unsold would
	 * make every busy week look like a bad one. A partial listing's sold
	 * units are sold and its remainder is not. */
	if (venture_market_listing_outcome_is_closed(outcome))
	{
		row->units_closed += quantity;
		row->units_sold_closed += sold;
	}

	switch (outcome)
	{
	case VENTURE_LISTING_OUTCOME_SOLD:
		row->sold++;

		if ((NULL != listed_at) && (NULL != closed_at))
		{
			row->sell_seconds += g_date_time_difference(closed_at, listed_at) /
			                     G_TIME_SPAN_SECOND;
			row->sell_count++;
		}
		break;
	case VENTURE_LISTING_OUTCOME_PARTIAL:
		row->partial++;
		break;
	case VENTURE_LISTING_OUTCOME_EXPIRED:
		row->expired++;
		break;
	case VENTURE_LISTING_OUTCOME_CANCELLED:
		row->cancelled++;
		break;
	case VENTURE_LISTING_OUTCOME_OPEN:
	default:
		row->open++;
		break;
	}

	/* The deposit is lost when nothing sold: an expired or cancelled
	 * listing. A partial one sold, so its deposit came back with the
	 * sale, as an auction house returns it. */
	if ((VENTURE_LISTING_OUTCOME_EXPIRED == outcome) ||
	    (VENTURE_LISTING_OUTCOME_CANCELLED == outcome))
	{
		if (!market_money_add(&row->deposits_lost, deposit, error))
			return FALSE;
	}

	if (!market_money_add(&row->fees, fees, error))
		return FALSE;

	if ((sold > 0) && (NULL != unit_price))
	{
		g_autoptr(VentureMoney) value = NULL;

		value = venture_money_multiply_int(unit_price, sold, error);

		if ((NULL == value) || !market_money_add(&row->sold_value, value, error))
			return FALSE;
	}

	return TRUE;
}

static gboolean
listing_row_write(
	VentureReportResult	 *result,
	ListingRow		 *row,
	GError			**error
){
	venture_report_result_begin_row(result);
	venture_report_result_set_text(result, "group", row->label);
	venture_report_result_set_text(result, "currency", row->currency);
	venture_report_result_set_number(result, "listings", (gdouble)row->listings);
	venture_report_result_set_number(result, "units_listed", (gdouble)row->units_listed);
	venture_report_result_set_number(result, "units_sold", (gdouble)row->units_sold);
	venture_report_result_set_number(result, "units_closed", (gdouble)row->units_closed);
	venture_report_result_set_number(result, "sold", (gdouble)row->sold);
	venture_report_result_set_number(result, "partial", (gdouble)row->partial);
	venture_report_result_set_number(result, "expired", (gdouble)row->expired);
	venture_report_result_set_number(result, "cancelled", (gdouble)row->cancelled);
	venture_report_result_set_number(result, "open", (gdouble)row->open);

	/* Ratios and day counts are presentation only; every money figure
	 * below is exact integer arithmetic. No closed units, no rate: 0%
	 * would claim nothing sold. */
	if (row->units_closed > 0)
		venture_report_result_set_number(result, "sale_rate",
			(gdouble)row->units_sold_closed / (gdouble)row->units_closed);

	if (row->sell_count > 0)
		venture_report_result_set_number(result, "avg_days_to_sell",
			((gdouble)row->sell_seconds / 86400.0) / (gdouble)row->sell_count);

	if ((NULL != row->sold_value) && (row->units_sold > 0))
	{
		g_autoptr(VentureMoney) average = NULL;

		/* One division of the exact total, rounded half to even once. */
		average = venture_money_multiply_rational(row->sold_value, 1,
		                                          row->units_sold, error);

		if (NULL == average)
			return FALSE;

		venture_report_result_set_money(result, "avg_unit_price", average);
		venture_report_result_set_money(result, "sold_value", row->sold_value);
	}

	if (NULL != row->deposits_lost)
		venture_report_result_set_money(result, "deposits_lost", row->deposits_lost);

	if (NULL != row->fees)
		venture_report_result_set_money(result, "fees", row->fees);

	return TRUE;
}

VentureReportResult *
venture_market_listing_performance(
	VentureContext		 *context,
	VentureDateRange	 *period,
	JsonObject		 *options,
	GError			**error
){
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) listings = NULL;
	g_autoptr(GPtrArray) ordered = NULL;
	g_autoptr(GHashTable) rows = NULL;
	g_autoptr(GHashTable) products = NULL;
	g_autoptr(GHashTable) paths = NULL;
	VentureDatabase *database;
	MarketGroup group;
	const gchar *group_by;
	const gchar *group_label;
	gint category_depth;
	gint64 organization_id;
	gint64 venture_id;
	GHashTableIter iter;
	gpointer value;
	guint i;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);

	database = venture_context_get_database(context);

	if (!market_organization(context, options, &organization_id, error))
		return NULL;

	/* --- The question --- */

	group_by = (NULL != options)
		? venture_json_object_get_string(options, "group_by", NULL) : NULL;

	if (venture_string_is_empty(group_by) || (0 == g_strcmp0(group_by, "product")))
	{
		group = MARKET_GROUP_PRODUCT;
		group_label = "Product";
	}
	else if (0 == g_strcmp0(group_by, "category"))
	{
		group = MARKET_GROUP_CATEGORY;
		group_label = "Category";
	}
	else if (0 == g_strcmp0(group_by, "channel"))
	{
		group = MARKET_GROUP_CHANNEL;
		group_label = "Channel";
	}
	else
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "listing_performance groups by product, category or "
		            "channel, not \"%s\"", group_by);
		return NULL;
	}

	category_depth = -1;

	if ((NULL != options) && json_object_has_member(options, "category_depth"))
	{
		gint64 depth;

		depth = venture_json_object_get_int(options, "category_depth", -1);

		if (MARKET_GROUP_CATEGORY != group)
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			                    "category_depth rolls up group_by=category, "
			                    "and this report groups by something else");
			return NULL;
		}

		if ((depth < 0) || (depth >= VENTURE_CATEGORY_MAX_DEPTH))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			            "category_depth must be 0 (the top of the tree) to %d",
			            VENTURE_CATEGORY_MAX_DEPTH - 1);
			return NULL;
		}

		category_depth = (gint)depth;
	}

	venture_id = (NULL != options)
		? venture_json_object_get_int(options, "venture_id", 0) : 0;

	/* --- The listings: opened in the period --- */

	query = venture_query_new(VENTURE_TYPE_LISTING);
	venture_query_set_organization(query, organization_id);

	if ((NULL != period) &&
	    !venture_query_set_date_range(query, "listed-at", period, error))
		return NULL;

	/*
	 * A venture is the product's: a listing belongs to whatever venture
	 * sells the thing it offers. The venture's products are read first
	 * and the listings narrowed to them in the query, so the bound counts
	 * the listings asked about -- filtered after the fetch, a narrowed
	 * question was refused whenever the whole organization was past it.
	 */
	if (0 != venture_id)
	{
		g_autoptr(VentureQuery) owned = NULL;
		g_autoptr(GPtrArray) products_sold = NULL;
		g_autoptr(GPtrArray) ids = NULL;

		owned = venture_query_new(VENTURE_TYPE_PRODUCT);
		venture_query_set_organization(owned, organization_id);

		if (!venture_query_add_filter_int(owned, "venture-id", VENTURE_FILTER_OP_EQ,
		                                  venture_id, error) ||
		    !venture_query_add_order(owned, "id", VENTURE_SORT_ASCENDING, error))
			return NULL;

		products_sold = market_fetch(database, owned, "products of the venture",
		                             "read the listings by period without venture_id",
		                             error);

		if (NULL == products_sold)
			return NULL;

		ids = g_ptr_array_new_with_free_func(g_free);

		for (i = 0; i < products_sold->len; i++)
			g_ptr_array_add(ids, g_strdup_printf("%" G_GINT64_FORMAT,
				venture_entity_get_id(g_ptr_array_index(products_sold, i))));

		/* IN needs a value; a venture that sells nothing has no
		 * listings, and none can match product 0. */
		if (0 == ids->len)
			g_ptr_array_add(ids, g_strdup("0"));

		if (!venture_query_add_filter(query, "product-id", VENTURE_FILTER_OP_IN,
		                              ids, error))
			return NULL;
	}

	if (!venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, error))
		return NULL;

	listings = market_fetch(database, query, "listings",
	                        "narrow the period or the venture", error);

	if (NULL == listings)
		return NULL;

	/* --- Grouped per group and currency --- */

	rows = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, listing_row_free);
	products = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free,
	                                 g_object_unref);
	paths = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

	for (i = 0; i < listings->len; i++)
	{
		VentureEntity *listing;
		VentureEntity *product;
		g_autoptr(VentureMoney) unit_price = NULL;
		g_autofree gchar *group_key = NULL;
		g_autofree gchar *label = NULL;
		g_autofree gchar *key = NULL;
		ListingRow *row;
		gint64 product_id;

		listing = g_ptr_array_index(listings, i);
		g_object_get(listing, "product-id", &product_id,
		             "unit-price", &unit_price, NULL);
		product = market_product(database, products, product_id);

		if (!listing_group_of(database, group, category_depth, listing, product,
		                      paths, &group_key, &label, error))
			return NULL;

		/* Keyed by currency before anything is added: no code below
		 * this line can add gold to dollars. */
		key = g_strdup_printf("%s\x1f%s", group_key,
		                      (NULL != unit_price)
		                       ? venture_money_get_currency(unit_price) : "");
		row = g_hash_table_lookup(rows, key);

		if (NULL == row)
		{
			row = g_new0(ListingRow, 1);
			row->label = g_steal_pointer(&label);
			row->currency = g_strdup((NULL != unit_price)
				? venture_money_get_currency(unit_price) : "");
			g_hash_table_insert(rows, g_steal_pointer(&key), row);
		}

		if (!listing_row_add(row, listing, error))
			return NULL;
	}

	/* --- The answer --- */

	result = venture_report_result_new("Listing performance", period);
	venture_report_result_add_column(result, "group", group_label, VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "currency", "Currency", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "listings", "Listings", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "units_listed", "Units listed", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "units_sold", "Units sold", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "units_closed", "Units on closed listings", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "sold", "Sold", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "partial", "Partial", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "expired", "Expired", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "cancelled", "Cancelled", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "open", "Open", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "sale_rate", "Sale rate", VENTURE_REPORT_COLUMN_PERCENT);
	venture_report_result_add_column(result, "avg_days_to_sell", "Average days to sell", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "avg_unit_price", "Average sold unit price", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "sold_value", "Sold value", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "deposits_lost", "Deposits lost", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "fees", "Fees", VENTURE_REPORT_COLUMN_MONEY);

	ordered = g_ptr_array_new();
	g_hash_table_iter_init(&iter, rows);

	while (g_hash_table_iter_next(&iter, NULL, &value))
		g_ptr_array_add(ordered, value);

	g_ptr_array_sort(ordered, listing_row_compare);

	for (i = 0; i < ordered->len; i++)
	{
		if (!listing_row_write(result, g_ptr_array_index(ordered, i), error))
			return NULL;
	}

	venture_report_result_append_note(result,
		"Sale rate is units sold on closed listings divided by units on "
		"closed listings. Open listings are left out until they end; a "
		"partial listing's sold units count as sold and the rest as unsold; "
		"a cancelled listing counts as unsold.");
	venture_report_result_append_note(result,
		"Listings are those opened in the period. Average days to sell reads "
		"listings that sold out; deposits lost are those of expired and "
		"cancelled listings.");

	return g_steal_pointer(&result);
}

/* ==========================================================================
 * Price history
 * ========================================================================== */

typedef enum
{
	MARKET_BUCKET_DAY = 0,
	MARKET_BUCKET_WEEK,
	MARKET_BUCKET_MONTH
} MarketBucket;

typedef struct
{
	gchar		*start;
	gchar		*label;
	gchar		*source;
	gchar		*currency;
	VentureMoney	*min;
	VentureMoney	*max;
	VentureMoney	*sum;
	gint64		 count;
	gint64		 volume;
} PriceRow;

static void
price_row_free(gpointer data)
{
	PriceRow *row;

	row = data;
	g_free(row->start);
	g_free(row->label);
	g_free(row->source);
	g_free(row->currency);
	g_clear_pointer(&row->min, venture_money_free);
	g_clear_pointer(&row->max, venture_money_free);
	g_clear_pointer(&row->sum, venture_money_free);
	g_free(row);
}

static gint
price_row_compare(
	gconstpointer	a,
	gconstpointer	b
){
	const PriceRow *left;
	const PriceRow *right;
	gint order;

	left = *(PriceRow *const *)a;
	right = *(PriceRow *const *)b;
	order = g_strcmp0(left->start, right->start);

	if (0 == order)
		order = g_utf8_collate(left->source, right->source);

	if (0 == order)
		order = g_strcmp0(left->currency, right->currency);

	return order;
}

/*
 * The UTC bucket @when falls in: its start as a sortable key, and a label.
 * UTC like every period boundary, so a day is the same day in every zone.
 */
static void
price_bucket_of(
	MarketBucket	  bucket,
	GDateTime	 *when,
	gchar		**out_start,
	gchar		**out_label
){
	g_autoptr(GDateTime) utc = NULL;
	g_autoptr(GDateTime) start = NULL;
	gint year;
	gint month;
	gint day;

	utc = g_date_time_to_utc(when);
	g_date_time_get_ymd(utc, &year, &month, &day);

	switch (bucket)
	{
	case MARKET_BUCKET_WEEK:
	{
		g_autoptr(GDateTime) midnight = NULL;

		/* ISO weeks start on Monday. */
		midnight = g_date_time_new_utc(year, month, day, 0, 0, 0);
		start = g_date_time_add_days(midnight,
		                             1 - g_date_time_get_day_of_week(midnight));
		*out_label = g_date_time_format(start, "%G-W%V");
		break;
	}
	case MARKET_BUCKET_MONTH:
		start = g_date_time_new_utc(year, month, 1, 0, 0, 0);
		*out_label = g_date_time_format(start, "%Y-%m");
		break;
	case MARKET_BUCKET_DAY:
	default:
		start = g_date_time_new_utc(year, month, day, 0, 0, 0);
		*out_label = g_date_time_format(start, "%Y-%m-%d");
		break;
	}

	*out_start = g_date_time_format(start, "%Y-%m-%d");
}

VentureReportResult *
venture_market_price_history(
	VentureContext		 *context,
	VentureDateRange	 *period,
	JsonObject		 *options,
	GError			**error
){
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureEntity) product = NULL;
	g_autoptr(GPtrArray) observations = NULL;
	g_autoptr(GPtrArray) ordered = NULL;
	g_autoptr(GHashTable) rows = NULL;
	g_autofree gchar *product_name = NULL;
	g_autofree gchar *title = NULL;
	VentureDatabase *database;
	MarketBucket bucket;
	const gchar *bucket_name;
	const gchar *source;
	gint64 organization_id;
	gint64 product_id;
	GHashTableIter iter;
	gpointer value;
	guint i;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);

	database = venture_context_get_database(context);

	if (!market_organization(context, options, &organization_id, error))
		return NULL;

	/* --- The question: one product, optionally one source --- */

	product_id = (NULL != options)
		? venture_json_object_get_int(options, "product_id", 0) : 0;

	if (product_id <= 0)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "price_history needs product_id: the product "
		                    "whose prices to show");
		return NULL;
	}

	/* Another organisation's product is not found, not forbidden: the
	 * refusal must not confirm that it exists. */
	product = venture_database_get(database, VENTURE_TYPE_PRODUCT, product_id, NULL);

	if ((NULL == product) ||
	    (venture_entity_get_organization_id(product) != organization_id))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		            "Product #%" G_GINT64_FORMAT " not found", product_id);
		return NULL;
	}

	bucket_name = (NULL != options)
		? venture_json_object_get_string(options, "bucket", NULL) : NULL;

	if (venture_string_is_empty(bucket_name) || (0 == g_strcmp0(bucket_name, "day")))
		bucket = MARKET_BUCKET_DAY;
	else if (0 == g_strcmp0(bucket_name, "week"))
		bucket = MARKET_BUCKET_WEEK;
	else if (0 == g_strcmp0(bucket_name, "month"))
		bucket = MARKET_BUCKET_MONTH;
	else
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "price_history buckets by day, week or month, not \"%s\"",
		            bucket_name);
		return NULL;
	}

	source = (NULL != options)
		? venture_json_object_get_string(options, "source", NULL) : NULL;

	/* --- The observations --- */

	query = venture_query_new(VENTURE_TYPE_PRICE_OBSERVATION);
	venture_query_set_organization(query, organization_id);

	if (!venture_query_add_filter_int(query, "product-id", VENTURE_FILTER_OP_EQ,
	                                  product_id, error))
		return NULL;

	if (!venture_string_is_empty(source) &&
	    !venture_query_add_filter_string(query, "source", VENTURE_FILTER_OP_EQ,
	                                     source, error))
		return NULL;

	if ((NULL != period) &&
	    !venture_query_set_date_range(query, "observed-at", period, error))
		return NULL;

	if (!venture_query_add_order(query, "observed-at", VENTURE_SORT_ASCENDING, error) ||
	    !venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, error))
		return NULL;

	observations = market_fetch(database, query, "price observations",
	                            "narrow the period or the source", error);

	if (NULL == observations)
		return NULL;

	/* --- Per bucket, source and currency --- */

	rows = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, price_row_free);

	for (i = 0; i < observations->len; i++)
	{
		VentureEntity *observation;
		g_autoptr(VentureMoney) price = NULL;
		g_autoptr(GDateTime) observed_at = NULL;
		g_autofree gchar *seen_by = NULL;
		g_autofree gchar *start = NULL;
		g_autofree gchar *label = NULL;
		g_autofree gchar *key = NULL;
		PriceRow *row;
		gint64 volume;

		observation = g_ptr_array_index(observations, i);
		g_object_get(observation, "price", &price, "observed-at", &observed_at,
		             "source", &seen_by, "volume", &volume, NULL);

		if ((NULL == price) || (NULL == observed_at))
			continue;

		price_bucket_of(bucket, observed_at, &start, &label);

		/* Keyed by currency before anything is compared or added. */
		key = g_strdup_printf("%s\x1f%s\x1f%s", start,
		                      (NULL != seen_by) ? seen_by : "",
		                      venture_money_get_currency(price));
		row = g_hash_table_lookup(rows, key);

		if (NULL == row)
		{
			row = g_new0(PriceRow, 1);
			row->start = g_steal_pointer(&start);
			row->label = g_steal_pointer(&label);
			row->source = g_strdup((NULL != seen_by) ? seen_by : "");
			row->currency = g_strdup(venture_money_get_currency(price));
			g_hash_table_insert(rows, g_steal_pointer(&key), row);
		}

		if ((NULL == row->min) || (venture_money_compare(price, row->min) < 0))
		{
			g_clear_pointer(&row->min, venture_money_free);
			row->min = venture_money_copy(price);
		}

		if ((NULL == row->max) || (venture_money_compare(price, row->max) > 0))
		{
			g_clear_pointer(&row->max, venture_money_free);
			row->max = venture_money_copy(price);
		}

		if (!market_money_add(&row->sum, price, error))
			return NULL;

		row->count++;

		if (volume > 0)
			row->volume += volume;
	}

	/* --- The answer --- */

	product_name = venture_entity_get_display_name(product);
	title = g_strdup_printf("Price history: %s", product_name);
	result = venture_report_result_new(title, period);
	venture_report_result_add_column(result, "bucket", "Bucket", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "starts_on", "Starts", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "source", "Source", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "currency", "Currency", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "min", "Lowest", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "avg", "Average", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "max", "Highest", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "volume", "Volume", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "observations", "Observations", VENTURE_REPORT_COLUMN_NUMBER);

	ordered = g_ptr_array_new();
	g_hash_table_iter_init(&iter, rows);

	while (g_hash_table_iter_next(&iter, NULL, &value))
		g_ptr_array_add(ordered, value);

	g_ptr_array_sort(ordered, price_row_compare);

	for (i = 0; i < ordered->len; i++)
	{
		g_autoptr(VentureMoney) average = NULL;
		PriceRow *row;

		row = g_ptr_array_index(ordered, i);

		/* One exact division of the total, rounded half to even. */
		average = venture_money_multiply_rational(row->sum, 1, row->count, error);

		if (NULL == average)
			return NULL;

		venture_report_result_begin_row(result);
		venture_report_result_set_text(result, "bucket", row->label);
		venture_report_result_set_text(result, "starts_on", row->start);
		venture_report_result_set_text(result, "source", row->source);
		venture_report_result_set_text(result, "currency", row->currency);
		venture_report_result_set_money(result, "min", row->min);
		venture_report_result_set_money(result, "avg", average);
		venture_report_result_set_money(result, "max", row->max);
		venture_report_result_set_number(result, "volume", (gdouble)row->volume);
		venture_report_result_set_number(result, "observations", (gdouble)row->count);
	}

	venture_report_result_append_note(result,
		"Buckets are UTC. The average is of the observations, not weighted "
		"by volume; one row per bucket, source and currency.");

	return g_steal_pointer(&result);
}

/* ==========================================================================
 * Registration
 *
 * A small report class rather than a function report, so each carries its
 * parameter schema: the schema is what the assistant's report tool and the
 * options form read, and an option they cannot see is one never sent.
 * ========================================================================== */

#define VENTURE_TYPE_MARKET_REPORT (venture_market_report_get_type())

G_DECLARE_FINAL_TYPE(VentureMarketReport, venture_market_report,
                     VENTURE, MARKET_REPORT, VentureReport)

struct _VentureMarketReport
{
	VentureReport		 parent_instance;

	VentureReportFunc	 func;
	const gchar		*schema;
};

G_DEFINE_FINAL_TYPE(VentureMarketReport, venture_market_report, VENTURE_TYPE_REPORT)

static VentureReportResult *
venture_market_report_generate(
	VentureReport		 *self,
	VentureContext		 *context,
	VentureDateRange	 *period,
	JsonObject		 *options,
	GError			**error
){
	return VENTURE_MARKET_REPORT(self)->func(context, period, options, error);
}

static JsonNode *
venture_market_report_parameters(VentureReport *self)
{
	return venture_json_parse(VENTURE_MARKET_REPORT(self)->schema, NULL);
}

static void
venture_market_report_class_init(VentureMarketReportClass *klass)
{
	VentureReportClass *report_class;

	report_class = VENTURE_REPORT_CLASS(klass);
	report_class->generate = venture_market_report_generate;
	report_class->describe_parameters = venture_market_report_parameters;
}

static void
venture_market_report_init(VentureMarketReport *self)
{
	(void)self;
}

static void
venture_market_report_add(
	VentureReportRegistry	*registry,
	const gchar		*name,
	const gchar		*title,
	const gchar		*description,
	VentureReportFunc	 func,
	const gchar		*schema
){
	VentureMarketReport *report;

	report = g_object_new(VENTURE_TYPE_MARKET_REPORT, "name", name,
	                      "title", title, "description", description, NULL);
	report->func = func;
	report->schema = schema;
	venture_data_class_declare_resource(G_OBJECT(report), VENTURE_DATA_CLASS_TENANT);
	venture_report_registry_add(registry, VENTURE_REPORT(report));
}

void
venture_market_register_reports(VentureReportRegistry *registry)
{
	g_return_if_fail(VENTURE_IS_REPORT_REGISTRY(registry));

	venture_market_report_add(registry, "listing_performance",
		"Listing performance",
		"How listings ended, per product, category or channel and currency: "
		"sale rate over closed listings, days to sell, average sold unit "
		"price, deposits lost and fees",
		venture_market_listing_performance,
		"{\"type\":\"object\",\"properties\":{"
		"\"group_by\":{\"type\":\"string\",\"enum\":[\"product\",\"category\","
		"\"channel\"],\"description\":\"What a row is; product by default. "
		"category uses the product's category, by path\"},"
		"\"category_depth\":{\"type\":\"integer\",\"description\":\"With "
		"group_by=category, roll categories up to this level; 0 is the top\"},"
		"\"period\":{\"type\":\"string\",\"description\":\"Listings opened in "
		"this period, e.g. this_month, 2026-03, all\"},"
		"\"venture_id\":{\"type\":\"integer\",\"description\":\"Only listings "
		"of this venture's products\"},"
		"\"organization_id\":{\"type\":\"integer\",\"description\":\"The legal "
		"entity; defaults to the default organization\"}}}");

	venture_market_report_add(registry, "price_history",
		"Price history",
		"A product's observed prices per day, week or month and source: "
		"lowest, average, highest, volume and observations, per currency",
		venture_market_price_history,
		"{\"type\":\"object\",\"required\":[\"product_id\"],\"properties\":{"
		"\"product_id\":{\"type\":\"integer\",\"description\":\"The product "
		"whose prices to show\"},"
		"\"source\":{\"type\":\"string\",\"description\":\"Only this source, "
		"matched exactly, e.g. market value; every source by default\"},"
		"\"bucket\":{\"type\":\"string\",\"enum\":[\"day\",\"week\",\"month\"],"
		"\"description\":\"UTC bucket size; day by default\"},"
		"\"period\":{\"type\":\"string\",\"description\":\"Observations in "
		"this period, e.g. last_30_days, 2026-03, all\"},"
		"\"organization_id\":{\"type\":\"integer\",\"description\":\"The legal "
		"entity; defaults to the default organization\"}}}");
}
