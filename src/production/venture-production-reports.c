/*
 * venture-production-reports.c - What a recipe costs and makes
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * One report, recipe_margin: for every active recipe, what one batch's
 * consumed components cost and what its output is worth, at the prices
 * the operator chose -- the latest observation from a named source when
 * the market module is on, the recorded costs when it is off -- and how
 * many batches the stock on hand allows. Money stays in its currency: a
 * recipe priced in two gets a note and no margin, never a sum of both.
 */

#include "venture.h"

#include <string.h>

/* ==========================================================================
 * Shared
 * ========================================================================== */

/* The organisation the report reads, checked to exist, as the market
 * reports do: "no recipes" about an organisation that is not there would
 * be a different and wrong answer. */
static gboolean
production_organization(
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

/* Every live inventory item of @product_id in the organisation. */
static GPtrArray *
production_items(
	VentureDatabase	 *database,
	gint64		  organization_id,
	gint64		  product_id,
	GError		**error
){
	g_autoptr(VentureQuery) query = NULL;

	query = venture_query_new(VENTURE_TYPE_INVENTORY_ITEM);
	venture_query_set_organization(query, organization_id);
	venture_query_set_limit(query, 0);

	if (!venture_query_add_filter_int(query, "product-id", VENTURE_FILTER_OP_EQ,
	                                  product_id, error) ||
	    !venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, error))
		return NULL;

	return venture_database_find(database, query, error);
}

/*
 * Units of @product_id on hand at @as_of, across every location. The
 * craft itself takes from one location; the report answers "could I make
 * this" before "where", and a note says so.
 */
static gboolean
production_on_hand(
	VentureDatabase	 *database,
	gint64		  organization_id,
	gint64		  product_id,
	GDateTime	 *as_of,
	gint64		 *out_units,
	GError		**error
){
	VentureInventoryService *inventory;
	g_autoptr(GPtrArray) items = NULL;
	guint i;

	*out_units = 0;
	inventory = venture_inventory_service_get(database);
	items = production_items(database, organization_id, product_id, error);

	if (NULL == items)
		return FALSE;

	for (i = 0; i < items->len; i++)
	{
		g_autoptr(GError) local_error = NULL;
		gint64 units;

		units = venture_inventory_service_on_hand(inventory,
			venture_entity_get_id(g_ptr_array_index(items, i)), as_of, &local_error);

		if (NULL != local_error)
		{
			g_propagate_error(error, g_steal_pointer(&local_error));
			return FALSE;
		}

		*out_units += units;
	}

	return TRUE;
}

/*
 * What one unit of @product_id is worth for this report.
 *
 * With a `series:` price source (@series set), the price oracle's figure
 * for the product's instruments on that basis and nothing else -- never
 * an observation in its place, for the reason below.
 *
 * With the market module on, the latest observation from @source at or
 * before @as_of and nothing else: falling back to a recorded cost when
 * the source has never priced a thing would mix two kinds of number in
 * one margin without saying which. With it off, the recorded figures:
 * as an input (@as_output FALSE), the first inventory item's carrying
 * cost, else the product's own unit cost; as an output, the product's
 * list price, which is what it sells for. Nothing found is *out_price
 * NULL, never zero.
 */
static gboolean
production_unit_price(
	VentureDatabase			 *database,
	VentureMarketdataOracle		 *series,
	VentureMarketdataBasis		  basis,
	const gchar			 *where,
	gint64		  organization_id,
	gint64		  product_id,
	gboolean	  market,
	const gchar	 *source,
	const gchar	 *currency,
	gboolean	  strict,
	GDateTime	 *as_of,
	gboolean	  as_output,
	VentureMoney	**out_price,
	GError		**error
){
	g_autoptr(VentureEntity) product = NULL;

	*out_price = NULL;

	if (NULL != series)
		return venture_marketdata_oracle_source_price(series, organization_id, product_id,
		                                              basis, where, currency, strict, as_of,
		                                              out_price, error);

	/* Asked for a currency, only prices seen in it count; otherwise the
	 * book currency's price wins wherever there is one, so a margin is
	 * not computed in whichever currency was observed last. */
	if (market && strict)
		return venture_market_latest_price(database, organization_id, product_id,
		                                   source, currency, as_of, out_price, NULL, error);

	if (market)
		return venture_market_price_preferring(database, organization_id, product_id,
		                                       source, currency, as_of, out_price, NULL,
		                                       error);

	if (!as_output)
	{
		g_autoptr(GPtrArray) items = NULL;
		guint i;

		items = production_items(database, organization_id, product_id, error);

		if (NULL == items)
			return FALSE;

		for (i = 0; (i < items->len) && (NULL == *out_price); i++)
			g_object_get(g_ptr_array_index(items, i), "unit-cost", out_price, NULL);

		if (NULL != *out_price)
			return TRUE;
	}

	product = venture_database_get(database, VENTURE_TYPE_PRODUCT, product_id, NULL);

	if (NULL != product)
		g_object_get(product, as_output ? "list-price" : "cost", out_price, NULL);

	return TRUE;
}

/* Adds @value into *@total, taking a copy the first time. The caller has
 * already refused mixed currencies; this adds like to like. */
static gboolean
production_money_add(
	VentureMoney		**total,
	const VentureMoney	 *value,
	GError			**error
){
	VentureMoney *next;

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

/* Appends @name to a comma-separated list. */
static void
production_list_add(
	GString		*list,
	const gchar	*name
){
	if (list->len > 0)
		g_string_append(list, ", ");

	g_string_append(list, name);
}

/* ==========================================================================
 * recipe_margin
 * ========================================================================== */

/* One recipe's row. */
static gboolean
production_margin_row(
	VentureDatabase		 *database,
	VentureMarketdataOracle	 *series,
	VentureMarketdataBasis	  basis,
	const gchar		 *where,
	VentureReportResult	 *result,
	VentureEntity		 *recipe,
	gint64			  organization_id,
	gboolean		  market,
	const gchar		 *source,
	const gchar		 *value_in,
	gboolean		  strict,
	GDateTime		 *as_of,
	GError			**error
){
	g_autoptr(GPtrArray) components = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureMoney) cost = NULL;
	g_autoptr(VentureMoney) value = NULL;
	g_autoptr(VentureMoney) output_price = NULL;
	g_autoptr(GString) missing = NULL;
	g_autoptr(GString) currencies = NULL;
	g_autoptr(GString) note = NULL;
	g_autofree gchar *recipe_name = NULL;
	g_autofree gchar *output_name = NULL;
	const gchar *currency;
	gint64 output_id;
	gint64 output_quantity;
	gint64 craftable;
	gboolean mixed;
	gboolean inputs_missing;
	guint i;

	g_object_get(recipe, "output-product-id", &output_id,
	             "output-quantity", &output_quantity, NULL);
	recipe_name = venture_entity_get_display_name(recipe);

	{
		g_autoptr(VentureEntity) output = NULL;

		output = venture_database_get(database, VENTURE_TYPE_PRODUCT, output_id, NULL);
		output_name = (NULL != output)
			? venture_entity_get_display_name(output)
			: g_strdup_printf("Product #%" G_GINT64_FORMAT, output_id);
	}

	query = venture_query_new(VENTURE_TYPE_RECIPE_COMPONENT);
	venture_query_set_limit(query, 0);

	if (!venture_query_add_filter_int(query, "recipe-id", VENTURE_FILTER_OP_EQ,
	                                  venture_entity_get_id(recipe), error) ||
	    !venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, error))
		return FALSE;

	components = venture_database_find(database, query, error);

	if (NULL == components)
		return FALSE;

	missing = g_string_new(NULL);
	currencies = g_string_new(NULL);
	note = g_string_new(NULL);
	currency = NULL;
	mixed = FALSE;
	inputs_missing = FALSE;
	craftable = -1;

	/* --- The inputs: priced, and counted against the shelf --- */

	for (i = 0; i < components->len; i++)
	{
		VentureEntity *component;
		g_autoptr(VentureMoney) price = NULL;
		gint64 product_id;
		gint64 quantity;
		gint64 on_hand;
		gboolean reusable;

		component = g_ptr_array_index(components, i);
		g_object_get(component, "product-id", &product_id, "quantity", &quantity,
		             "reusable", &reusable, NULL);

		if (quantity < 1)
			continue;

		if (!production_on_hand(database, organization_id, product_id, as_of,
		                        &on_hand, error))
			return FALSE;

		/* A tool gates the craft without limiting how many: one on the
		 * bench makes as many batches as the materials allow. */
		if (reusable)
		{
			if (on_hand < quantity)
				craftable = 0;

			continue;
		}

		{
			gint64 batches;

			batches = (on_hand > 0) ? (on_hand / quantity) : 0;

			if ((craftable < 0) || (batches < craftable))
				craftable = batches;
		}

		if (!production_unit_price(database, series, basis, where, organization_id,
		                           product_id, market, source, value_in, strict, as_of,
		                           FALSE, &price, error))
			return FALSE;

		if (NULL == price)
		{
			g_autoptr(VentureEntity) product = NULL;
			g_autofree gchar *name = NULL;

			product = venture_database_get(database, VENTURE_TYPE_PRODUCT, product_id, NULL);
			name = (NULL != product) ? venture_entity_get_display_name(product)
			                         : g_strdup_printf("Product #%" G_GINT64_FORMAT, product_id);
			production_list_add(missing, name);
			inputs_missing = TRUE;
			continue;
		}

		if (NULL == currency)
		{
			currency = g_intern_string(venture_money_get_currency(price));
			production_list_add(currencies, currency);
		}
		else if (0 != g_strcmp0(currency, venture_money_get_currency(price)))
		{
			if (NULL == strstr(currencies->str, venture_money_get_currency(price)))
				production_list_add(currencies, venture_money_get_currency(price));

			mixed = TRUE;
			continue;
		}

		{
			g_autoptr(VentureMoney) line = NULL;

			line = venture_money_multiply_int(price, quantity, error);

			if ((NULL == line) || !production_money_add(&cost, line, error))
				return FALSE;
		}
	}

	/* --- The output: valued the same way --- */

	if (!production_unit_price(database, series, basis, where, organization_id,
	                           output_id, market, source, value_in, strict, as_of,
	                           TRUE, &output_price, error))
		return FALSE;

	if (NULL == output_price)
		production_list_add(missing, output_name);
	else
	{
		if (NULL == currency)
		{
			currency = g_intern_string(venture_money_get_currency(output_price));
			production_list_add(currencies, currency);
		}
		else if (0 != g_strcmp0(currency, venture_money_get_currency(output_price)))
		{
			if (NULL == strstr(currencies->str, venture_money_get_currency(output_price)))
				production_list_add(currencies, venture_money_get_currency(output_price));

			mixed = TRUE;
		}

		value = venture_money_multiply_int(output_price, output_quantity, error);

		if (NULL == value)
			return FALSE;
	}

	/* --- The row --- */

	venture_report_result_begin_row(result);
	venture_report_result_set_text(result, "recipe", recipe_name);
	venture_report_result_set_text(result, "makes", output_name);
	venture_report_result_set_number(result, "batch", (gdouble)output_quantity);
	venture_report_result_set_text(result, "priced_by",
		market ? (venture_string_is_empty(source) ? "prices seen, any source" : source)
		       : "recorded costs");

	if (components->len == 0)
		production_list_add(note, "no components");
	else
		venture_report_result_set_number(result, "craftable_now",
		                                 (gdouble)((craftable < 0) ? 0 : craftable));

	if (missing->len > 0)
	{
		g_string_append_printf(note, "%sno price for %s",
		                       (note->len > 0) ? "; " : "", missing->str);
	}

	/* Two currencies: no figure at all, rather than a margin that adds
	 * gold to dollars or quietly leaves one side out. */
	if (mixed)
	{
		g_string_append_printf(note, "%spriced in %s; no margin across currencies",
		                       (note->len > 0) ? "; " : "", currencies->str);
		venture_report_result_set_text(result, "note", note->str);
		return TRUE;
	}

	if (NULL != currency)
		venture_report_result_set_text(result, "currency", currency);

	/* A missing price leaves its figure blank, never zero: a component
	 * nobody has priced is not free, and an output nobody has priced is
	 * not worthless. What can be said still is -- the cost when only the
	 * output is unpriced, the value when only an input is. */
	{
		gboolean inputs_whole;

		inputs_whole = !inputs_missing;

		if (inputs_whole && (NULL == cost) && (NULL != currency))
			cost = venture_money_new_zero(currency);

		if (inputs_whole && (NULL != cost))
		{
			g_autoptr(VentureMoney) per_unit = NULL;

			venture_report_result_set_money(result, "cost", cost);

			/* One division of the exact total, rounded half to even
			 * once. */
			per_unit = venture_money_multiply_rational(cost, 1, output_quantity, error);

			if (NULL == per_unit)
				return FALSE;

			venture_report_result_set_money(result, "cost_per_unit", per_unit);
		}

		if (NULL != value)
			venture_report_result_set_money(result, "value", value);

		if (inputs_whole && (NULL != cost) && (NULL != value))
		{
			g_autoptr(VentureMoney) profit = NULL;

			profit = venture_money_subtract(value, cost, error);

			if (NULL == profit)
				return FALSE;

			venture_report_result_set_money(result, "profit", profit);

			/* A ratio is presentation only; the money above is exact. */
			if (venture_money_get_amount(value) > 0)
				venture_report_result_set_number(result, "margin",
					(gdouble)venture_money_get_amount(profit) /
					(gdouble)venture_money_get_amount(value));
		}
	}

	if (note->len > 0)
		venture_report_result_set_text(result, "note", note->str);

	return TRUE;
}

VentureReportResult *
venture_production_recipe_margin(
	VentureContext		 *context,
	VentureDateRange	 *period,
	JsonObject		 *options,
	GError			**error
){
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) recipes = NULL;
	g_autoptr(GArray) categories = NULL;
	g_autoptr(GDateTime) as_of = NULL;
	g_autoptr(GError) as_of_error = NULL;
	g_autofree gchar *currency = NULL;
	g_autofree gchar *where = NULL;
	g_autoptr(VentureMarketdataOracle) series = NULL;
	VentureMarketdataBasis basis;
	VentureDatabase *database;
	const gchar *source;
	gint64 organization_id;
	gint64 venture_id;
	gint64 category_id;
	gboolean market;
	gboolean strict;
	gboolean is_series;
	guint i;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);

	database = venture_context_get_database(context);

	if (!production_organization(context, options, &organization_id, error))
		return NULL;

	/* --- The question --- */

	market = venture_context_module_enabled(context, "market");
	source = (NULL != options)
		? venture_json_object_get_string(options, "price_source", NULL) : NULL;

	/* The grammar is decided before any exact-match lookup: a series:
	 * source is a question for the price oracle, never an observation
	 * source of that name. */
	if (!venture_marketdata_parse_price_source(source, &is_series, &basis, &where, error))
		return NULL;

	if (is_series)
	{
		if (!venture_marketdata_series_available(context, error))
			return NULL;

		series = venture_marketdata_oracle_new(context);
	}

	/* Asked for a source nobody can read: refused, not answered from the
	 * recorded costs as though the question had been a different one. */
	if (!is_series && !venture_string_is_empty(source) && !market)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "price_source reads the market module's price "
		                    "observations, and the market module is off; leave "
		                    "it out to price at recorded costs");
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
	category_id = (NULL != options)
		? venture_json_object_get_int(options, "category_id", 0) : 0;

	/* A category and everything filed beneath it, as a person means
	 * "potions" when the tree has Potions / Healing under it. */
	if (0 != category_id)
	{
		categories = venture_category_descendants(database, VENTURE_TYPE_CATEGORY,
		                                          category_id, TRUE, error);

		if (NULL == categories)
			return NULL;
	}

	/* --- The recipes --- */

	/*
	 * Every narrowing is part of the query, so the bound below counts the
	 * recipes the question is about. Filtering in C after the fetch
	 * counted every recipe in the organization instead: past the bound a
	 * narrowed question was refused, and told to narrow.
	 */
	query = venture_query_new(VENTURE_TYPE_RECIPE);
	venture_query_set_organization(query, organization_id);
	venture_query_set_limit(query, (guint)venture_aggregate_get_max_rows() + 1);

	if (!venture_query_add_filter_string(query, "active", VENTURE_FILTER_OP_EQ,
	                                     "true", error))
		return NULL;

	if ((0 != venture_id) &&
	    !venture_query_add_filter_int(query, "venture-id", VENTURE_FILTER_OP_EQ,
	                                  venture_id, error))
		return NULL;

	if (NULL != categories)
	{
		g_autoptr(GPtrArray) filed = NULL;

		filed = g_ptr_array_new_with_free_func(g_free);

		for (i = 0; i < categories->len; i++)
			g_ptr_array_add(filed, g_strdup_printf("%" G_GINT64_FORMAT,
				g_array_index(categories, gint64, i)));

		if (!venture_query_add_filter(query, "category-id", VENTURE_FILTER_OP_IN,
		                              filed, error))
			return NULL;
	}

	if (!venture_query_add_order(query, "name", VENTURE_SORT_ASCENDING, error) ||
	    !venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, error))
		return NULL;

	recipes = venture_database_find(database, query, error);

	if (NULL == recipes)
		return NULL;

	/* Refused rather than truncated: fewer rows presented as all of them
	 * is the failure this avoids. */
	if (recipes->len > (guint)venture_aggregate_get_max_rows())
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "More than %d active recipes match; narrow by venture_id "
		            "or category_id", venture_aggregate_get_max_rows());
		return NULL;
	}

	result = venture_report_result_new("Recipe margin", period);
	venture_report_result_add_column(result, "recipe", "Recipe", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "makes", "Makes", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "batch", "Batch size", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "currency", "Currency", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "cost", "Cost per batch", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "value", "Value per batch", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "profit", "Profit per batch", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "margin", "Margin", VENTURE_REPORT_COLUMN_PERCENT);
	venture_report_result_add_column(result, "cost_per_unit", "Cost per unit", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "craftable_now", "Batches on hand", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "priced_by", "Priced by", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "note", "Note", VENTURE_REPORT_COLUMN_TEXT);

	for (i = 0; i < recipes->len; i++)
	{
		VentureEntity *recipe;

		recipe = g_ptr_array_index(recipes, i);

		if (!production_margin_row(database, series, basis, where, result, recipe,
		                           organization_id,
		                           market, source, currency, strict, as_of, error))
			return NULL;
	}

	if (is_series)
		venture_report_result_append_note(result,
			"Components and output are priced from market data feeds: the price "
			"oracle's figure on the price source's basis for the instruments that "
			"name each product, at the named venue or group, as it stood at the "
			"cutoff, in the currency option's currency when one is given and "
			"otherwise in the book currency wherever an instrument is priced in it. "
			"Never an observation instead: a product the feeds have not priced is "
			"named in the note and its figures are left blank.");
	else if (market)
		venture_report_result_append_note(result,
			"Components and output are priced at the latest price seen at or "
			"before the cutoff, from the named source (any source when none is "
			"named), in the currency option's currency when one is given and "
			"otherwise in the book currency wherever the product was seen priced "
			"in it. A product never seen priced is named in the note and its "
			"figures are left blank, not read as zero.");
	else
		venture_report_result_append_note(result,
			"The market module is off: components are priced at their inventory "
			"item's unit cost (else the product's unit cost) and the output at its "
			"list price.");

	venture_report_result_append_note(result,
		"Cost counts consumed components only; a reusable one (a tool) must be "
		"on hand but is not used up. Batches on hand is the fewest whole batches "
		"any consumed component allows, across every location, and 0 when a tool "
		"is missing; a craft itself takes from one location.");

	return g_steal_pointer(&result);
}

/* ==========================================================================
 * Registration
 *
 * A small report class rather than a function report, like the market's,
 * so the report carries its parameter schema: the schema is what the
 * assistant's report tool and the options form read, and an option they
 * cannot see is one never sent.
 * ========================================================================== */

#define VENTURE_TYPE_PRODUCTION_REPORT (venture_production_report_get_type())

G_DECLARE_FINAL_TYPE(VentureProductionReport, venture_production_report,
                     VENTURE, PRODUCTION_REPORT, VentureReport)

struct _VentureProductionReport
{
	VentureReport		 parent_instance;

	VentureReportFunc	 func;
	const gchar		*schema;
};

G_DEFINE_FINAL_TYPE(VentureProductionReport, venture_production_report, VENTURE_TYPE_REPORT)

static VentureReportResult *
venture_production_report_generate(
	VentureReport		 *self,
	VentureContext		 *context,
	VentureDateRange	 *period,
	JsonObject		 *options,
	GError			**error
){
	return VENTURE_PRODUCTION_REPORT(self)->func(context, period, options, error);
}

static JsonNode *
venture_production_report_parameters(VentureReport *self)
{
	return venture_json_parse(VENTURE_PRODUCTION_REPORT(self)->schema, NULL);
}

static void
venture_production_report_class_init(VentureProductionReportClass *klass)
{
	VentureReportClass *report_class;

	report_class = VENTURE_REPORT_CLASS(klass);
	report_class->generate = venture_production_report_generate;
	report_class->describe_parameters = venture_production_report_parameters;
}

static void
venture_production_report_init(VentureProductionReport *self)
{
	(void)self;
}

void
venture_production_register_reports(VentureReportRegistry *registry)
{
	VentureProductionReport *report;

	g_return_if_fail(VENTURE_IS_REPORT_REGISTRY(registry));

	report = g_object_new(VENTURE_TYPE_PRODUCTION_REPORT, "name", "recipe_margin",
	                      "title", "Recipe margin",
	                      "description", "What each active recipe's batch costs and "
	                      "makes at a chosen price source: profit, margin, cost per "
	                      "unit and how many batches the stock on hand allows",
	                      NULL);
	report->func = venture_production_recipe_margin;
	report->schema =
		"{\"type\":\"object\",\"properties\":{"
		"\"price_source\":{\"type\":\"string\",\"description\":\"Price at the "
		"latest observation from this source, matched exactly, e.g. market "
		"value; any source by default. Needs the market module. "
		"series:<basis>[@<venue or group>] prices from market data feeds "
		"instead (basis min, market, market_14d, historical_60d, region_median, "
		"region_p33, region_market_avg or sale_avg); needs the marketdata and "
		"feeds modules\"},"
		"\"as_of\":{\"type\":\"string\",\"description\":\"Prices and stock as "
		"they stood at this date; now by default\"},"
		"\"currency\":{\"type\":\"string\",\"description\":\"Only prices "
		"observed in this currency count; by default the book currency's price "
		"wins wherever the product was seen in it, else the latest in any\"},"
		"\"venture_id\":{\"type\":\"integer\",\"description\":\"Only this "
		"venture's recipes\"},"
		"\"category_id\":{\"type\":\"integer\",\"description\":\"Only recipes "
		"filed in this category or beneath it\"},"
		"\"organization_id\":{\"type\":\"integer\",\"description\":\"The legal "
		"entity; defaults to the default organization\"}}}";
	venture_data_class_declare_resource(G_OBJECT(report), VENTURE_DATA_CLASS_TENANT);
	venture_report_registry_add(registry, VENTURE_REPORT(report));
}
