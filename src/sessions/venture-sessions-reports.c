/*
 * venture-sessions-reports.c - What each kind of run yielded per hour
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * One report, session_performance: sessions grouped by what kind of run
 * they were (or their category, location or venture), with the hours
 * they took, the goods and money they yielded, what they cost, and the
 * two numbers a person running them actually wants -- what an hour of
 * this is worth, gross and net. Money is keyed by currency before
 * anything is added, for the reason every report here does: a run that
 * yields gold and costs dollars reads as two rows, never one number in
 * neither currency.
 */

#include "venture.h"

#include <string.h>

/* ==========================================================================
 * Shared
 * ========================================================================== */

typedef enum
{
	SESSIONS_GROUP_ACTIVITY = 0,
	SESSIONS_GROUP_CATEGORY,
	SESSIONS_GROUP_LOCATION,
	SESSIONS_GROUP_VENTURE
} SessionsGroup;

/* The organisation the report reads, checked to exist, as the market and
 * production reports do: "no sessions" about an organisation that is not
 * there would be a different and wrong answer. */
static gboolean
sessions_report_organization(
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

/* Adds @value into *@total, taking a copy the first time. The caller has
 * keyed by currency already; this adds like to like. */
static gboolean
sessions_money_add(
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

/* @left - @right, either possibly absent; a zero in the other's currency
 * stands in for the missing one. NULL only when both are. */
static VentureMoney *
sessions_money_less(
	const VentureMoney	 *left,
	const VentureMoney	 *right,
	GError			**error
){
	g_autoptr(VentureMoney) zero = NULL;

	if ((NULL == left) && (NULL == right))
		return NULL;

	if (NULL == right)
		return venture_money_copy(left);

	if (NULL == left)
	{
		zero = venture_money_new(0, venture_money_get_currency(right),
		                         venture_money_get_exponent(right));
		left = zero;
	}

	return venture_money_subtract(left, right, error);
}

/* A record by id, read once per report however many rows name it. */
static VentureEntity *
sessions_cached(
	VentureDatabase	*database,
	GHashTable	*cache,
	GType		 type,
	gint64		 id
){
	VentureEntity *record;

	record = g_hash_table_lookup(cache, &id);

	if (NULL == record)
	{
		gint64 *key;

		record = venture_database_get(database, type, id, NULL);

		if (NULL == record)
			return NULL;

		key = g_new(gint64, 1);
		*key = id;
		g_hash_table_insert(cache, key, record);
	}

	return record;
}

/* Appends @name to a comma-separated list once. */
static void
sessions_list_add(
	GString		*list,
	GHashTable	*seen,
	const gchar	*name
){
	if (g_hash_table_contains(seen, name))
		return;

	g_hash_table_add(seen, g_strdup(name));

	if (list->len > 0)
		g_string_append(list, ", ");

	g_string_append(list, name);
}

/* ==========================================================================
 * The rows
 * ========================================================================== */

/* One currency's money within a group. */
typedef struct
{
	gchar		*currency;

	/* Every session in the group. */
	VentureMoney	*value;
	VentureMoney	*amount;
	VentureMoney	*cost;

	/* Finished sessions only: the per-hour figures divide these by
	 * finished hours, so an open session's yields cannot inflate an
	 * hour it has not finished spending. */
	VentureMoney	*closed_gross;
	VentureMoney	*closed_cost;
} SessionsMoney;

static void
sessions_money_free(gpointer data)
{
	SessionsMoney *money;

	money = data;
	g_free(money->currency);
	g_clear_pointer(&money->value, venture_money_free);
	g_clear_pointer(&money->amount, venture_money_free);
	g_clear_pointer(&money->cost, venture_money_free);
	g_clear_pointer(&money->closed_gross, venture_money_free);
	g_clear_pointer(&money->closed_cost, venture_money_free);
	g_free(money);
}

/* One group. Counts and hours belong to the group, not to a currency, and
 * are repeated on each of its currency rows. */
typedef struct
{
	gchar		*label;

	gint64		 sessions;
	gint64		 open;
	gint64		 closed_minutes;
	gint64		 units;

	GHashTable	*money;		/* currency -> SessionsMoney */

	GString		*priced_by;
	GHashTable	*priced_seen;
	GString		*unpriced;
	GHashTable	*unpriced_seen;
} SessionsRow;

static void
sessions_row_free(gpointer data)
{
	SessionsRow *row;

	row = data;
	g_free(row->label);
	g_hash_table_unref(row->money);
	g_string_free(row->priced_by, TRUE);
	g_hash_table_unref(row->priced_seen);
	g_string_free(row->unpriced, TRUE);
	g_hash_table_unref(row->unpriced_seen);
	g_free(row);
}

static SessionsRow *
sessions_row_new(gchar *label)
{
	SessionsRow *row;

	row = g_new0(SessionsRow, 1);
	row->label = label;
	row->money = g_hash_table_new_full(g_str_hash, g_str_equal, NULL,
	                                   sessions_money_free);
	row->priced_by = g_string_new(NULL);
	row->priced_seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	row->unpriced = g_string_new(NULL);
	row->unpriced_seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

	return row;
}

/* The row's money in @currency, made on first use. */
static SessionsMoney *
sessions_row_money(
	SessionsRow	*row,
	const gchar	*currency
){
	SessionsMoney *money;

	money = g_hash_table_lookup(row->money, currency);

	if (NULL == money)
	{
		money = g_new0(SessionsMoney, 1);
		money->currency = g_strdup(currency);
		g_hash_table_insert(row->money, money->currency, money);
	}

	return money;
}

static gint
sessions_row_compare(
	gconstpointer	a,
	gconstpointer	b
){
	return g_utf8_collate((*(SessionsRow *const *)a)->label,
	                      (*(SessionsRow *const *)b)->label);
}

static gint
sessions_money_compare(
	gconstpointer	a,
	gconstpointer	b
){
	return g_strcmp0((*(SessionsMoney *const *)a)->currency,
	                 (*(SessionsMoney *const *)b)->currency);
}

/* ==========================================================================
 * Grouping
 * ========================================================================== */

/*
 * The group a session falls in, as a key (stable, unique) and a label
 * (what a person reads). Trees are keyed by node id and labelled by their
 * computed path, rolled up to @depth when one is asked for; ventures by
 * id, so two ventures called the same are two rows.
 */
static gboolean
sessions_group_of(
	VentureDatabase	 *database,
	SessionsGroup	  group,
	gint		  depth,
	VentureEntity	 *session,
	GHashTable	 *labels,
	gchar		**out_key,
	gchar		**out_label,
	GError		**error
){
	const gchar *cached;
	const gchar *property;
	const gchar *none;
	GType tree;
	gint64 id;

	if (SESSIONS_GROUP_ACTIVITY == group)
	{
		g_autofree gchar *activity = NULL;

		g_object_get(session, "activity", &activity, NULL);

		if (venture_string_is_empty(activity))
		{
			*out_key = g_strdup("-");
			*out_label = g_strdup("No activity");
		}
		else
		{
			*out_key = g_strconcat("a:", activity, NULL);
			*out_label = g_strdup(activity);
		}

		return TRUE;
	}

	switch (group)
	{
	case SESSIONS_GROUP_CATEGORY:
		property = "category-id";
		none = "Uncategorised";
		tree = VENTURE_TYPE_CATEGORY;
		break;
	case SESSIONS_GROUP_LOCATION:
		property = "location-id";
		none = "No location";
		tree = VENTURE_TYPE_LOCATION;
		break;
	case SESSIONS_GROUP_VENTURE:
	case SESSIONS_GROUP_ACTIVITY:
	default:
		property = "venture-id";
		none = "No venture";
		tree = G_TYPE_INVALID;
		break;
	}

	id = 0;
	g_object_get(session, property, &id, NULL);

	if (id <= 0)
	{
		*out_key = g_strdup("-");
		*out_label = g_strdup(none);
		return TRUE;
	}

	/* Rolled up to its ancestor at the depth asked for; a shallower node
	 * is its own answer (see the category tree). */
	if ((G_TYPE_INVALID != tree) && (depth >= 0))
	{
		id = venture_category_ancestor_at_depth(database, tree, id, (guint)depth,
		                                        error);

		if (0 == id)
			return FALSE;
	}

	*out_key = g_strdup_printf("k:%" G_GINT64_FORMAT, id);
	cached = g_hash_table_lookup(labels, *out_key);

	if (NULL == cached)
	{
		gchar *computed;

		if (G_TYPE_INVALID != tree)
		{
			/* Computed, never stored: renaming a parent renames every
			 * row beneath it on the next run. */
			computed = venture_category_path(database, tree, id, error);

			if (NULL == computed)
			{
				g_clear_pointer(out_key, g_free);
				return FALSE;
			}
		}
		else
		{
			g_autoptr(VentureEntity) venture = NULL;

			venture = venture_database_get(database, VENTURE_TYPE_VENTURE, id, NULL);
			computed = (NULL != venture)
				? venture_entity_get_display_name(venture)
				: g_strdup_printf("Venture #%" G_GINT64_FORMAT, id);
		}

		g_hash_table_insert(labels, g_strdup(*out_key), computed);
		cached = computed;
	}

	*out_label = g_strdup(cached);

	return TRUE;
}

/* ==========================================================================
 * Valuing a goods yield
 * ========================================================================== */

typedef struct
{
	VentureDatabase	*database;
	gint64		 organization_id;
	gboolean	 market;
	const gchar	*source;
	/* The currency to value in: only prices in it when @strict (the
	 * currency option), else the book currency where seen in it. */
	const gchar	*currency;
	gboolean	 strict;
	GDateTime	*as_of;
	GHashTable	*products;
} SessionsPricing;

/*
 * What one unit of a goods yield was worth, and how that was decided:
 *
 *  1. its own `unit-value`, the value somebody recorded with it;
 *  2. with the market module on, the latest price observed from the
 *     source asked for, at @at -- and nothing else, as in recipe_margin:
 *     falling back to a list price would mix two kinds of number in one
 *     total without saying which;
 *  3. with the market module off, the product's list price.
 *
 * Nothing found is *out_price NULL, never zero: an unpriced herb is not
 * worthless, it is unknown.
 */
static gboolean
sessions_unit_value(
	SessionsPricing	 *pricing,
	VentureEntity	 *yield,
	GDateTime	 *at,
	VentureMoney	**out_price,
	const gchar	**out_method,
	GError		**error
){
	gint64 product_id;

	*out_price = NULL;
	*out_method = NULL;

	g_object_get(yield, "unit-value", out_price, "product-id", &product_id, NULL);

	if (NULL != *out_price)
	{
		*out_method = "unit value";
		return TRUE;
	}

	if (pricing->market)
	{
		if (pricing->strict
		    ? !venture_market_latest_price(pricing->database, pricing->organization_id,
		                                   product_id, pricing->source, pricing->currency,
		                                   at, out_price, NULL, error)
		    : !venture_market_price_preferring(pricing->database,
		                                       pricing->organization_id, product_id,
		                                       pricing->source, pricing->currency, at,
		                                       out_price, NULL, error))
			return FALSE;

		*out_method = venture_string_is_empty(pricing->source)
			? "prices seen, any source" : pricing->source;
		return TRUE;
	}

	{
		VentureEntity *product;

		product = sessions_cached(pricing->database, pricing->products,
		                          VENTURE_TYPE_PRODUCT, product_id);

		if (NULL != product)
			g_object_get(product, "list-price", out_price, NULL);

		*out_method = "list price";
	}

	return TRUE;
}

/* Adds one session and its yields to @row. */
static gboolean
sessions_row_add(
	SessionsRow	 *row,
	SessionsPricing	 *pricing,
	VentureEntity	 *session,
	GError		**error
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) yields = NULL;
	g_autoptr(GDateTime) ended = NULL;
	g_autoptr(VentureMoney) cost = NULL;
	GDateTime *at;
	gboolean closed;
	gint64 minutes;
	guint i;

	g_object_get(session, "ended-at", &ended, "minutes", &minutes, "cost", &cost, NULL);
	closed = (NULL != ended);

	row->sessions++;

	if (closed)
		row->closed_minutes += minutes;
	else
		row->open++;

	/* What it cost. */
	if (NULL != cost)
	{
		SessionsMoney *money;

		money = sessions_row_money(row, venture_money_get_currency(cost));

		if (!sessions_money_add(&money->cost, cost, error) ||
		    (closed && !sessions_money_add(&money->closed_cost, cost, error)))
			return FALSE;
	}

	/* What it yielded. Valued as of the cutoff when one is asked for --
	 * "what is everything I gathered worth today" -- else as of the end of
	 * the run, which is what it was worth when it was made. An open run
	 * has no end yet and is valued now. */
	at = (NULL != pricing->as_of) ? pricing->as_of : ended;

	query = venture_query_new(VENTURE_TYPE_SESSION_YIELD);
	venture_query_set_limit(query, 0);

	if (!venture_query_add_filter_int(query, "session-id", VENTURE_FILTER_OP_EQ,
	                                  venture_entity_get_id(session), error) ||
	    !venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, error))
		return FALSE;

	yields = venture_database_find(pricing->database, query, error);

	if (NULL == yields)
		return FALSE;

	for (i = 0; i < yields->len; i++)
	{
		VentureEntity *yield;
		g_autoptr(VentureMoney) amount = NULL;
		g_autoptr(VentureMoney) price = NULL;
		g_autoptr(VentureMoney) value = NULL;
		const gchar *method;
		SessionsMoney *money;
		gint64 product_id;
		gint64 quantity;

		yield = g_ptr_array_index(yields, i);
		g_object_get(yield, "amount", &amount, "product-id", &product_id,
		             "quantity", &quantity, NULL);

		/* Money: counted in its own currency, as it is. */
		if (NULL != amount)
		{
			money = sessions_row_money(row, venture_money_get_currency(amount));

			if (!sessions_money_add(&money->amount, amount, error) ||
			    (closed && !sessions_money_add(&money->closed_gross, amount, error)))
				return FALSE;

			continue;
		}

		if ((product_id <= 0) || (quantity <= 0))
			continue;

		row->units += quantity;

		/* Goods: valued, or named as unvalued. */
		if (!sessions_unit_value(pricing, yield, at, &price, &method, error))
			return FALSE;

		if (NULL == price)
		{
			VentureEntity *product;
			g_autofree gchar *name = NULL;

			product = sessions_cached(pricing->database, pricing->products,
			                          VENTURE_TYPE_PRODUCT, product_id);
			name = (NULL != product)
				? venture_entity_get_display_name(product)
				: g_strdup_printf("Product #%" G_GINT64_FORMAT, product_id);
			sessions_list_add(row->unpriced, row->unpriced_seen, name);
			continue;
		}

		sessions_list_add(row->priced_by, row->priced_seen, method);
		value = venture_money_multiply_int(price, quantity, error);

		if (NULL == value)
			return FALSE;

		money = sessions_row_money(row, venture_money_get_currency(value));

		if (!sessions_money_add(&money->value, value, error) ||
		    (closed && !sessions_money_add(&money->closed_gross, value, error)))
			return FALSE;
	}

	return TRUE;
}

/* ==========================================================================
 * Writing the answer
 * ========================================================================== */

/* @total per hour over @minutes, one exact division rounded half to even. */
static gboolean
sessions_per_hour(
	VentureReportResult	 *result,
	const gchar		 *column,
	const VentureMoney	 *total,
	gint64			  minutes,
	GError			**error
){
	g_autoptr(VentureMoney) rate = NULL;

	if ((NULL == total) || (minutes <= 0))
		return TRUE;

	rate = venture_money_multiply_rational(total, 60, minutes, error);

	if (NULL == rate)
		return FALSE;

	venture_report_result_set_money(result, column, rate);

	return TRUE;
}

/* The group-wide cells every currency row of a group repeats. */
static void
sessions_row_counts(
	VentureReportResult	*result,
	SessionsRow		*row
){
	venture_report_result_set_text(result, "group", row->label);
	venture_report_result_set_number(result, "sessions", (gdouble)row->sessions);
	venture_report_result_set_number(result, "open", (gdouble)row->open);
	venture_report_result_set_number(result, "hours", (gdouble)row->closed_minutes / 60.0);
	venture_report_result_set_number(result, "units", (gdouble)row->units);

	if (row->priced_by->len > 0)
		venture_report_result_set_text(result, "priced_by", row->priced_by->str);

	if (row->unpriced->len > 0)
	{
		g_autofree gchar *note = NULL;

		note = g_strdup_printf("no value for %s; yield value, net and the "
		                       "per-hour figures are left blank", row->unpriced->str);
		venture_report_result_set_text(result, "note", note);
	}
}

static gboolean
sessions_row_write(
	VentureReportResult	 *result,
	SessionsRow		 *row,
	GError			**error
){
	g_autoptr(GPtrArray) currencies = NULL;
	GHashTableIter iter;
	gpointer value;
	gboolean whole;
	guint i;

	/* A group whose goods could not all be valued has no honest value
	 * total in any currency: the missing thing might have been worth
	 * anything, in any of them. Counts, money yielded and cost still
	 * stand. */
	whole = (0 == row->unpriced->len);

	currencies = g_ptr_array_new();
	g_hash_table_iter_init(&iter, row->money);

	while (g_hash_table_iter_next(&iter, NULL, &value))
		g_ptr_array_add(currencies, value);

	g_ptr_array_sort(currencies, sessions_money_compare);

	/* A group with no money at all still happened: one row, no currency. */
	if (0 == currencies->len)
	{
		venture_report_result_begin_row(result);
		sessions_row_counts(result, row);
		return TRUE;
	}

	for (i = 0; i < currencies->len; i++)
	{
		SessionsMoney *money;

		money = g_ptr_array_index(currencies, i);

		venture_report_result_begin_row(result);
		sessions_row_counts(result, row);
		venture_report_result_set_text(result, "currency", money->currency);

		if (NULL != money->amount)
			venture_report_result_set_money(result, "amount", money->amount);

		if (NULL != money->cost)
			venture_report_result_set_money(result, "cost", money->cost);

		if (!whole)
			continue;

		{
			g_autoptr(VentureMoney) gross = NULL;
			g_autoptr(VentureMoney) net = NULL;
			g_autoptr(VentureMoney) closed_net = NULL;

			if (NULL != money->value)
				venture_report_result_set_money(result, "value", money->value);

			/* Net = goods value + money yielded - cost, in this currency
			 * only. */
			if (NULL != money->value)
				gross = venture_money_copy(money->value);

			if (!sessions_money_add(&gross, money->amount, error))
				return FALSE;

			net = sessions_money_less(gross, money->cost, error);

			if ((NULL == net) && ((NULL != gross) || (NULL != money->cost)))
				return FALSE;

			if (NULL != net)
				venture_report_result_set_money(result, "net", net);

			closed_net = sessions_money_less(money->closed_gross, money->closed_cost, error);

			if ((NULL == closed_net) &&
			    ((NULL != money->closed_gross) || (NULL != money->closed_cost)))
				return FALSE;

			if (!sessions_per_hour(result, "value_per_hour", money->closed_gross,
			                       row->closed_minutes, error) ||
			    !sessions_per_hour(result, "net_per_hour", closed_net,
			                       row->closed_minutes, error))
				return FALSE;
		}
	}

	return TRUE;
}

/* ==========================================================================
 * session_performance
 * ========================================================================== */

VentureReportResult *
venture_sessions_performance(
	VentureContext		 *context,
	VentureDateRange	 *period,
	JsonObject		 *options,
	GError			**error
){
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) sessions = NULL;
	g_autoptr(GPtrArray) ordered = NULL;
	g_autoptr(GHashTable) rows = NULL;
	g_autoptr(GHashTable) labels = NULL;
	g_autoptr(GHashTable) products = NULL;
	g_autoptr(GDateTime) as_of = NULL;
	g_autoptr(GError) as_of_error = NULL;
	g_autofree gchar *currency = NULL;
	VentureDatabase *database;
	SessionsPricing pricing;
	SessionsGroup group;
	const gchar *group_by;
	const gchar *group_label;
	const gchar *source;
	gint category_depth;
	gint64 organization_id;
	gint64 venture_id;
	gboolean market;
	gboolean strict;
	GHashTableIter iter;
	gpointer value;
	guint i;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);

	database = venture_context_get_database(context);

	if (!sessions_report_organization(context, options, &organization_id, error))
		return NULL;

	/* --- The question --- */

	group_by = (NULL != options)
		? venture_json_object_get_string(options, "group_by", NULL) : NULL;

	if (venture_string_is_empty(group_by) || (0 == g_strcmp0(group_by, "activity")))
	{
		group = SESSIONS_GROUP_ACTIVITY;
		group_label = "Activity";
	}
	else if (0 == g_strcmp0(group_by, "category"))
	{
		group = SESSIONS_GROUP_CATEGORY;
		group_label = "Category";
	}
	else if (0 == g_strcmp0(group_by, "location"))
	{
		group = SESSIONS_GROUP_LOCATION;
		group_label = "Location";
	}
	else if (0 == g_strcmp0(group_by, "venture"))
	{
		group = SESSIONS_GROUP_VENTURE;
		group_label = "Venture";
	}
	else
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "session_performance groups by activity, category, location "
		            "or venture, not \"%s\"", group_by);
		return NULL;
	}

	/* Locations are the sales module's; grouping by one it cannot read
	 * is refused rather than answered as "No location" for everything. */
	if ((SESSIONS_GROUP_LOCATION == group) &&
	    (G_TYPE_INVALID == venture_entity_registry_lookup(
		venture_entity_registry_get_default(), "location")))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "group_by=location reads locations, which belong to the "
		                    "sales module, and it is off");
		return NULL;
	}

	category_depth = -1;

	if ((NULL != options) && json_object_has_member(options, "category_depth"))
	{
		gint64 depth;

		depth = venture_json_object_get_int(options, "category_depth", -1);

		if ((SESSIONS_GROUP_CATEGORY != group) && (SESSIONS_GROUP_LOCATION != group))
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			                    "category_depth rolls up group_by=category or "
			                    "group_by=location, and this report groups by "
			                    "something else");
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

	market = venture_context_module_enabled(context, "market");
	source = (NULL != options)
		? venture_json_object_get_string(options, "price_source", NULL) : NULL;

	/* Asked for a source nobody can read: refused, not answered from the
	 * list prices as though the question had been a different one. */
	if (!venture_string_is_empty(source) && !market)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "price_source reads the market module's price "
		                    "observations, and the market module is off; leave "
		                    "it out to value goods at their list prices");
		return NULL;
	}

	as_of = venture_period_report_as_of(options, &as_of_error);

	if (NULL != as_of_error)
	{
		g_propagate_error(error, g_steal_pointer(&as_of_error));
		return NULL;
	}

	if (!venture_market_valuing_currency(database, organization_id, options,
	                                     &currency, &strict, error))
		return NULL;

	venture_id = (NULL != options)
		? venture_json_object_get_int(options, "venture_id", 0) : 0;

	/* --- The sessions: started in the period --- */

	query = venture_query_new(VENTURE_TYPE_SESSION);
	venture_query_set_organization(query, organization_id);
	venture_query_set_limit(query, (guint)venture_aggregate_get_max_rows() + 1);

	if ((NULL != period) &&
	    !venture_query_set_date_range(query, "started-at", period, error))
		return NULL;

	if ((0 != venture_id) &&
	    !venture_query_add_filter_int(query, "venture-id", VENTURE_FILTER_OP_EQ,
	                                  venture_id, error))
		return NULL;

	if (!venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, error))
		return NULL;

	sessions = venture_database_find(database, query, error);

	if (NULL == sessions)
		return NULL;

	/* Refused rather than truncated: fewer sessions presented as all of
	 * them is the failure this avoids. */
	if (sessions->len > (guint)venture_aggregate_get_max_rows())
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "More than %d sessions match; narrow the period or the venture",
		            venture_aggregate_get_max_rows());
		return NULL;
	}

	/* --- Grouped --- */

	rows = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, sessions_row_free);
	labels = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	products = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free,
	                                 g_object_unref);

	pricing.database = database;
	pricing.organization_id = organization_id;
	pricing.market = market;
	pricing.source = source;
	pricing.currency = currency;
	pricing.strict = strict;
	pricing.as_of = as_of;
	pricing.products = products;

	for (i = 0; i < sessions->len; i++)
	{
		VentureEntity *session;
		g_autofree gchar *key = NULL;
		g_autofree gchar *label = NULL;
		SessionsRow *row;

		session = g_ptr_array_index(sessions, i);

		if (!sessions_group_of(database, group, category_depth, session, labels,
		                       &key, &label, error))
			return NULL;

		row = g_hash_table_lookup(rows, key);

		if (NULL == row)
		{
			row = sessions_row_new(g_steal_pointer(&label));
			g_hash_table_insert(rows, g_steal_pointer(&key), row);
		}

		if (!sessions_row_add(row, &pricing, session, error))
			return NULL;
	}

	/* --- The answer --- */

	result = venture_report_result_new("Session performance", period);
	venture_report_result_add_column(result, "group", group_label, VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "currency", "Currency", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "sessions", "Sessions", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "open", "Still open", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "hours", "Hours", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "units", "Units yielded", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "value", "Goods value", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "amount", "Money yielded", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "cost", "Cost", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "net", "Net", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "value_per_hour", "Yield per hour", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "net_per_hour", "Net per hour", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "priced_by", "Valued by", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "note", "Note", VENTURE_REPORT_COLUMN_TEXT);

	ordered = g_ptr_array_new();
	g_hash_table_iter_init(&iter, rows);

	while (g_hash_table_iter_next(&iter, NULL, &value))
		g_ptr_array_add(ordered, value);

	g_ptr_array_sort(ordered, sessions_row_compare);

	for (i = 0; i < ordered->len; i++)
	{
		if (!sessions_row_write(result, g_ptr_array_index(ordered, i), error))
			return NULL;
	}

	venture_report_result_append_note(result,
		"One row per group and currency; money is never added across "
		"currencies. Sessions, still open, hours and units yielded belong to "
		"the group and repeat on each of its currency rows -- do not add them "
		"down the column.");
	venture_report_result_append_note(result,
		"Hours count finished sessions only. Yield per hour is goods value "
		"plus money yielded, and net per hour that less cost, both over "
		"finished sessions and their hours; an open session's yields and cost "
		"are in the totals but not in the rates.");

	if (market)
		venture_report_result_append_note(result,
			"Goods are valued at the unit value recorded with them, else the "
			"latest price seen from the named source (any source when none is "
			"named) at the end of the session, or at as_of when one is given. "
			"Goods with neither are named in the note, and the figures they "
			"would change are left blank rather than read as zero.");
	else
		venture_report_result_append_note(result,
			"The market module is off: goods are valued at the unit value "
			"recorded with them, else the product's list price. Goods with "
			"neither are named in the note, and the figures they would change "
			"are left blank rather than read as zero.");

	return g_steal_pointer(&result);
}

/* ==========================================================================
 * Registration
 *
 * A small report class rather than a function report, like the market's
 * and production's, so the report carries its parameter schema: the
 * schema is what the assistant's report tool and the options form read,
 * and an option they cannot see is one never sent.
 * ========================================================================== */

#define VENTURE_TYPE_SESSIONS_REPORT (venture_sessions_report_get_type())

G_DECLARE_FINAL_TYPE(VentureSessionsReport, venture_sessions_report,
                     VENTURE, SESSIONS_REPORT, VentureReport)

struct _VentureSessionsReport
{
	VentureReport		 parent_instance;

	VentureReportFunc	 func;
	const gchar		*schema;
};

G_DEFINE_FINAL_TYPE(VentureSessionsReport, venture_sessions_report, VENTURE_TYPE_REPORT)

static VentureReportResult *
venture_sessions_report_generate(
	VentureReport		 *self,
	VentureContext		 *context,
	VentureDateRange	 *period,
	JsonObject		 *options,
	GError			**error
){
	return VENTURE_SESSIONS_REPORT(self)->func(context, period, options, error);
}

static JsonNode *
venture_sessions_report_parameters(VentureReport *self)
{
	return venture_json_parse(VENTURE_SESSIONS_REPORT(self)->schema, NULL);
}

static void
venture_sessions_report_class_init(VentureSessionsReportClass *klass)
{
	VentureReportClass *report_class;

	report_class = VENTURE_REPORT_CLASS(klass);
	report_class->generate = venture_sessions_report_generate;
	report_class->describe_parameters = venture_sessions_report_parameters;
}

static void
venture_sessions_report_init(VentureSessionsReport *self)
{
	(void)self;
}

void
venture_sessions_register_reports(VentureReportRegistry *registry)
{
	VentureSessionsReport *report;

	g_return_if_fail(VENTURE_IS_REPORT_REGISTRY(registry));

	report = g_object_new(VENTURE_TYPE_SESSIONS_REPORT, "name", "session_performance",
	                      "title", "Session performance",
	                      "description", "What each kind of session yielded for "
	                      "the hours it took: goods value, money, cost, net, and "
	                      "yield and net per hour, per currency",
	                      NULL);
	report->func = venture_sessions_performance;
	report->schema =
		"{\"type\":\"object\",\"properties\":{"
		"\"group_by\":{\"type\":\"string\",\"enum\":[\"activity\",\"category\","
		"\"location\",\"venture\"],\"description\":\"What to compare; activity "
		"by default\"},"
		"\"category_depth\":{\"type\":\"integer\",\"description\":\"With "
		"group_by=category or location: roll up to this level of the tree, 0 "
		"being the top\"},"
		"\"price_source\":{\"type\":\"string\",\"description\":\"Value goods "
		"with no recorded unit value at the latest observation from this "
		"source, matched exactly; any source by default. Needs the market "
		"module\"},"
		"\"as_of\":{\"type\":\"string\",\"description\":\"Value every yield "
		"at this date instead of at the end of its session\"},"
		"\"currency\":{\"type\":\"string\",\"description\":\"Only prices "
		"observed in this currency count; by default the book currency's price "
		"wins wherever the product was seen in it, else the latest in any\"},"
		"\"venture_id\":{\"type\":\"integer\",\"description\":\"Only this "
		"venture's sessions\"},"
		"\"organization_id\":{\"type\":\"integer\",\"description\":\"The legal "
		"entity; defaults to the default organization\"}}}";
	venture_data_class_declare_resource(G_OBJECT(report), VENTURE_DATA_CLASS_TENANT);
	venture_report_registry_add(registry, VENTURE_REPORT(report));
}
