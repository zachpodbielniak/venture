/*
 * royalties.c - A worked example of a crispy plugin
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is never built. Drop it in a plugin directory and the server
 * compiles it on demand at startup -- crispy invokes gcc, caches the result
 * by content hash, and loads the object with GModule. Editing the file
 * recompiles it on the next start; leaving it alone costs nothing.
 *
 * The entry points are identical to a native plugin's. The difference is
 * only that there is no Makefile, no link line and no build step: for a
 * report you want to tweak while looking at the numbers, that is the whole
 * point.
 *
 * This one adds a report rather than a record type, which is the more
 * common reason to reach for a script. It answers a question the built-in
 * reports do not: of what I actually sold, how much is a genre pulling its
 * weight, and what is each unit really earning after the platform's cut?
 */

#include <venture/venture.h>

/*
 * The share of gross that reaches you. Configurable from the /plugins page:
 * set `rate_percent: 65` there and the very next report run uses it -- no
 * restart, no recompile. The default matches a common ebook royalty.
 *
 * Stored as an exact rational (percent times a hundred, over ten thousand)
 * rather than a double, because the money layer multiplies rationally and
 * rounds once; 65.5% of $3.99 must not drift.
 */
static gint64 royalties_rate_numerator   = 7000;
static gint64 royalties_rate_denominator = 10000;

/*
 * Reads the rate from this plugin's stored configuration. Called once at
 * registration and again whenever the /plugins page saves -- which is what
 * the config-changed signal is for.
 */
static void
royalties_read_config(VenturePluginManager *manager)
{
	g_autoptr(JsonNode) config = NULL;
	gdouble percent;

	if (NULL == manager)
		return;

	config = venture_plugin_manager_get_config(manager, "royalties.c");
	percent = venture_plugin_config_get_double(config, "rate_percent", 70.0);

	/* A rate outside (0, 100] is a typo, not a business model. */
	if ((percent <= 0.0) || (percent > 100.0))
	{
		g_warning("royalties: rate_percent %.2f is outside (0, 100]; "
		          "keeping the previous rate", percent);
		return;
	}

	royalties_rate_numerator = (gint64)(percent * 100.0 + 0.5);
	royalties_rate_denominator = 10000;
}

static void
royalties_on_config_changed(
	VenturePluginManager	*manager,
	const gchar		*plugin_name,
	gpointer		 user_data
){
	/* One signal serves every plugin; only our own name is ours. */
	if (0 == g_strcmp0(plugin_name, "royalties.c"))
		royalties_read_config(manager);
}

/*
 * One genre's running totals. Kept in a hash table keyed by genre so the
 * report is a single pass over the sales rather than a query per genre.
 */
typedef struct
{
	VentureMoney	*gross;
	VentureMoney	*royalties;
	gint64		 units;
} GenreTotals;

static void
genre_totals_free(gpointer data)
{
	GenreTotals *totals;

	totals = data;

	g_clear_pointer(&totals->gross, venture_money_free);
	g_clear_pointer(&totals->royalties, venture_money_free);
	g_free(totals);
}

/*
 * Adds @addend to *@target in place, leaving the total alone if the
 * addition is refused -- which happens when two sales are in different
 * currencies. Silently converting them would invent an exchange rate.
 */
static void
money_accumulate(
	VentureMoney		**target,
	const VentureMoney	 *addend
){
	g_autoptr(GError) error = NULL;
	VentureMoney *sum;

	if (NULL == addend)
		return;

	sum = venture_money_add(*target, addend, &error);

	if (NULL == sum)
	{
		g_warning("royalties: skipping a sale: %s", error->message);
		return;
	}

	g_clear_pointer(target, venture_money_free);
	*target = sum;
}

static VentureReportResult *
royalties_report(
	VentureContext		 *context,
	VentureDateRange	 *period,
	JsonObject		 *options,
	GError			**error
){
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) sales = NULL;
	g_autoptr(GHashTable) genres = NULL;
	g_autoptr(VentureMoney) total_royalties = NULL;
	g_autoptr(GList) keys = NULL;
	GList *iter;
	gint64 total_units;
	guint i;

	/*
	 * Sales in the period, and only in the period: a royalty statement
	 * for the quarter that quietly included last year's sales would be
	 * worse than no statement.
	 */
	query = venture_query_new(VENTURE_TYPE_SALE);
	venture_query_set_organization(query,
		venture_context_get_default_organization_id(context));
	venture_query_set_limit(query, 0);

	if (!venture_query_set_date_range(query, "occurred-at", period, error))
		return NULL;

	sales = venture_database_find(venture_context_get_database(context),
	                              query, error);

	if (NULL == sales)
		return NULL;

	genres = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                               genre_totals_free);
	total_royalties = venture_money_new_zero(NULL);
	total_units = 0;

	result = venture_report_result_new("Royalties by genre", period);

	venture_report_result_add_column(result, "genre", "Genre",
	                                 VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "units", "Units",
	                                 VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "gross", "Gross",
	                                 VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "royalties", "Royalties",
	                                 VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "per_unit", "Per unit",
	                                 VENTURE_REPORT_COLUMN_MONEY);

	/* One pass over the sales, bucketed by the product's genre. */
	for (i = 0; i < sales->len; i++)
	{
		g_autoptr(VentureMoney) gross = NULL;
		g_autoptr(VentureMoney) royalty = NULL;
		g_autoptr(VentureEntity) product = NULL;
		g_autofree gchar *genre = NULL;
		GenreTotals *totals;
		VentureEntity *sale;
		gint64 product_id;
		gint64 quantity;

		sale = g_ptr_array_index(sales, i);

		g_object_get(sale, "gross", &gross, "quantity", &quantity,
		             "product-id", &product_id, NULL);

		/*
		 * Genre lives on the product, not the sale, so it has to be
		 * fetched. A sale with no product still counts -- it just
		 * lands in "unattributed" rather than being dropped, because
		 * a total that silently omits revenue is a wrong total.
		 */
		if (0 != product_id)
		{
			product = venture_database_get(
				venture_context_get_database(context),
				VENTURE_TYPE_PRODUCT, product_id, NULL);
		}

		if (NULL != product)
			g_object_get(product, "genre", &genre, NULL);

		if (venture_string_is_empty(genre))
		{
			g_free(genre);
			genre = g_strdup("unattributed");
		}

		totals = g_hash_table_lookup(genres, genre);

		if (NULL == totals)
		{
			totals = g_new0(GenreTotals, 1);
			totals->gross = venture_money_new_zero(
				venture_money_get_currency(gross));
			totals->royalties = venture_money_new_zero(
				venture_money_get_currency(gross));

			g_hash_table_insert(genres, g_strdup(genre), totals);
		}

		/*
		 * Exact integer arithmetic on the minor units: 70% of a
		 * three-cent sale is not a number floating point can hold,
		 * and a royalty statement is a figure someone is paid on.
		 */
		royalty = venture_money_multiply_rational(gross,
		                                         royalties_rate_numerator,
		                                         royalties_rate_denominator,
		                                         error);

		if (NULL == royalty)
			return NULL;

		money_accumulate(&totals->gross, gross);
		money_accumulate(&totals->royalties, royalty);
		money_accumulate(&total_royalties, royalty);

		totals->units += quantity;
		total_units += quantity;
	}

	/* Sorted, so the same data always renders in the same order -- a
	 * statement that reshuffles between runs cannot be diffed. */
	keys = g_list_sort(g_hash_table_get_keys(genres), (GCompareFunc)g_strcmp0);

	for (iter = keys; NULL != iter; iter = iter->next)
	{
		g_autoptr(VentureMoney) per_unit = NULL;
		GenreTotals *totals;

		totals = g_hash_table_lookup(genres, iter->data);

		/* Royalties divided by units, as an exact rational rounded
		 * once -- the same path every other division of money in
		 * VENTURE takes. */
		if (0 != totals->units)
		{
			per_unit = venture_money_multiply_rational(totals->royalties,
			                                           1, totals->units,
			                                           NULL);
		}

		venture_report_result_begin_row(result);
		venture_report_result_set_text(result, "genre", iter->data);
		venture_report_result_set_number(result, "units",
		                                 (gdouble)totals->units);
		venture_report_result_set_money(result, "gross", totals->gross);
		venture_report_result_set_money(result, "royalties",
		                                totals->royalties);
		venture_report_result_set_money(result, "per_unit", per_unit);
	}

	venture_report_result_add_metric(result,
		venture_metric_new_money("royalties", "Total royalties",
		                         total_royalties));
	venture_report_result_add_metric(result,
		venture_metric_new_count("units", "Units sold", total_units));
	venture_report_result_add_metric(result,
		venture_metric_new_count("genres", "Genres selling",
		                         (gint64)g_hash_table_size(genres)));

	return g_steal_pointer(&result);
}

/* ==========================================================================
 * Entry points -- identical to a native plugin's
 * ========================================================================== */

/**
 * venture_plugin_info:
 *
 * Optional. Shown in `venturectl`'s plugin list and at /api/v1/plugins.
 *
 * Returns: (transfer none): a one-line description
 */
const gchar *
venture_plugin_info(void);

const gchar *
venture_plugin_info(void)
{
	return "Royalties by genre; rate_percent configurable at /plugins "
	       "(compiled on demand)";
}

/**
 * venture_plugin_register:
 * @context: the wiring, giving access to every registry
 * @error: (out) (optional): return location for a #GError
 *
 * Required. Called once when the plugin is loaded.
 *
 * Returns: %TRUE if the plugin registered successfully
 */
gboolean
venture_plugin_register(
	VentureContext	 *context,
	GError		**error
);

gboolean
venture_plugin_register(
	VentureContext	 *context,
	GError		**error
){
	VenturePluginManager *manager;

	venture_report_registry_add(
		venture_context_get_report_registry(context),
		VENTURE_REPORT(venture_func_report_new(
			"royalties", "Royalties by genre",
			"Per-genre royalties for the period, at the configured "
			"rate",
			royalties_report)));

	/*
	 * Runtime configuration: read the stored settings now, and follow
	 * the /plugins page from here on. The signal fires for every
	 * plugin's save; the handler ignores names that are not ours.
	 */
	manager = venture_context_get_plugin_manager(context);

	if (NULL != manager)
	{
		royalties_read_config(manager);
		g_signal_connect(manager, "config-changed",
		                 G_CALLBACK(royalties_on_config_changed), NULL);
	}

	return TRUE;
}
