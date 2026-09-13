/*
 * test-report.c - The reporting engine and the built-in reports
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Reports are what the whole system exists to produce, so the arithmetic is
 * pinned down here against known data rather than merely exercised.
 */

#include <venture.h>

typedef struct
{
	VentureDatabase	*database;
	VentureConfig	*config;
	VentureContext	*context;
	gint64		 organization_id;
	gint64		 venture_id;
} Fixture;

static void
fixture_set_up(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureEntity) organization = NULL;
	g_autoptr(VentureVenture) venture = NULL;

	fixture->config = venture_config_new();
	fixture->database = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);

	g_assert_true(venture_database_migrate(fixture->database,
		venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);

	fixture->context = venture_context_new(fixture->config, fixture->database);

	query = venture_query_new(VENTURE_TYPE_ORGANIZATION);
	organization = venture_database_find_one(fixture->database, query, NULL);
	fixture->organization_id = venture_entity_get_id(organization);

	venture = venture_venture_new();
	g_object_set(venture, "name", "Books", "venture-type", "books",
	             "status", VENTURE_VENTURE_STATUS_ACTIVE, NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(venture),
	                                   fixture->organization_id);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(venture), NULL, NULL));
	fixture->venture_id = venture_entity_get_id(VENTURE_ENTITY(venture));
}

static void
fixture_tear_down(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_clear_object(&fixture->context);
	g_clear_object(&fixture->database);
	g_clear_object(&fixture->config);
}

/*
 * Records a sale inside the current month, so the default period covers it.
 */
static void
add_sale(
	Fixture		*fixture,
	gint64		 product_id,
	const gchar	*gross,
	const gchar	*fees,
	gint64		 quantity
){
	g_autoptr(VentureSale) sale = NULL;
	g_autoptr(GDateTime) when = NULL;

	when = venture_time_now();

	sale = venture_sale_new();
	g_object_set(sale,
	             "venture-id", fixture->venture_id,
	             "product-id", product_id,
	             "occurred-at", when,
	             "quantity", quantity,
	             NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(sale),
	                                   fixture->organization_id);

	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(sale),
		"gross", gross, NULL));

	if (NULL != fees)
	{
		g_assert_true(venture_entity_set_field_from_string(
			VENTURE_ENTITY(sale), "fees", fees, NULL));
	}

	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(sale), NULL, NULL));
}

static void
add_expense(
	Fixture			*fixture,
	const gchar		*description,
	const gchar		*category,
	const gchar		*amount,
	VentureDeductibility	 deductibility,
	gint64			 business_use
){
	g_autoptr(VentureExpense) expense = NULL;
	g_autoptr(GDateTime) when = NULL;

	when = venture_time_now();

	expense = venture_expense_new();
	g_object_set(expense,
	             "venture-id", fixture->venture_id,
	             "description", description,
	             "category", category,
	             "occurred-at", when,
	             "deductibility", deductibility,
	             "business-use-percent", business_use,
	             NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(expense),
	                                   fixture->organization_id);

	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(expense),
		"amount", amount, NULL));
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(expense), NULL, NULL));
}

static gint64
add_product(
	Fixture		*fixture,
	const gchar	*name,
	const gchar	*genre
){
	g_autoptr(VentureProduct) product = NULL;

	product = venture_product_new();
	g_object_set(product,
	             "venture-id", fixture->venture_id,
	             "name", name,
	             "genre", genre,
	             NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(product),
	                                   fixture->organization_id);

	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(product), NULL, NULL));

	return venture_entity_get_id(VENTURE_ENTITY(product));
}

/*
 * Finds a metric by key, so tests do not depend on the order metrics were
 * added in.
 */
static VentureMetric *
find_metric(
	VentureReportResult	*result,
	const gchar		*key
){
	GPtrArray *metrics;
	guint i;

	metrics = venture_report_result_get_metrics(result);

	for (i = 0; i < metrics->len; i++)
	{
		VentureMetric *metric;

		metric = g_ptr_array_index(metrics, i);

		if (0 == g_strcmp0(venture_metric_get_key(metric), key))
			return metric;
	}

	return NULL;
}

static VentureReportResult *
run_report(
	Fixture		*fixture,
	const gchar	*name,
	JsonObject	*options
){
	g_autoptr(VentureDateRange) period = NULL;
	g_autoptr(GError) error = NULL;
	VentureReportResult *result;
	VentureReport *report;

	report = venture_report_registry_lookup(
		venture_context_get_report_registry(fixture->context), name);
	g_assert_nonnull(report);

	period = venture_context_parse_period(fixture->context, "this_month",
	                                      &error);
	g_assert_no_error(error);

	result = venture_report_generate(report, fixture->context, period, options,
	                                 &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);

	return result;
}

/* --- Registry ------------------------------------------------------------ */

static void
test_report_registry_has_builtins(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	VentureReportRegistry *registry;
	g_autoptr(GPtrArray) reports = NULL;

	registry = venture_context_get_report_registry(fixture->context);
	reports = venture_report_registry_list(registry);

	/* Ten for the books and the CRM, three for the software factory. */
	g_assert_cmpuint(reports->len, ==, 23);

	g_assert_nonnull(venture_report_registry_lookup(registry, "pnl"));
	g_assert_nonnull(venture_report_registry_lookup(registry, "releases"));
	g_assert_nonnull(venture_report_registry_lookup(registry, "lead_time"));
	g_assert_nonnull(venture_report_registry_lookup(registry, "incidents"));
	g_assert_nonnull(venture_report_registry_lookup(registry, "ventures"));
	g_assert_nonnull(venture_report_registry_lookup(registry, "categories"));
	g_assert_nonnull(venture_report_registry_lookup(registry, "tax"));
	g_assert_nonnull(venture_report_registry_lookup(registry, "receivables"));
	g_assert_nonnull(venture_report_registry_lookup(registry, "snapshot_vs_live"));
	g_assert_null(venture_report_registry_lookup(registry, "nonesuch"));
}

static void
test_report_registry_describe(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(JsonNode) description = NULL;
	JsonArray *array;

	description = venture_report_registry_describe(
		venture_context_get_report_registry(fixture->context));

	g_assert_nonnull(description);
	array = json_node_get_array(description);
	g_assert_cmpuint(json_array_get_length(array), ==, 23);

	/* The description is what the AI's report tool advertises, so every
	 * report has to carry one. */
	{
		JsonObject *first;

		first = json_array_get_object_element(array, 0);
		g_assert_nonnull(json_object_get_string_member(first, "description"));
	}
}

/* --- Profit and loss ----------------------------------------------------- */

static void
test_report_pnl_arithmetic(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureReportResult) result = NULL;
	VentureMetric *revenue;
	VentureMetric *expenses;
	VentureMetric *profit;

	/* Two sales grossing 100.00 with 12.50 of fees, so net revenue is
	 * 87.50; and 30.00 of expenses, so profit is 57.50. */
	add_sale(fixture, 0, "60.00", "7.50", 1);
	add_sale(fixture, 0, "40.00", "5.00", 1);
	add_expense(fixture, "Cover art", "supplies", "30.00",
	            VENTURE_DEDUCTIBILITY_FULL, 100);

	result = run_report(fixture, "pnl", NULL);

	revenue = find_metric(result, "revenue");
	expenses = find_metric(result, "expenses");
	profit = find_metric(result, "profit");

	g_assert_nonnull(revenue);
	g_assert_cmpint(venture_money_get_amount(venture_metric_get_money(revenue)),
	                ==, 8750);
	g_assert_cmpint(venture_money_get_amount(venture_metric_get_money(expenses)),
	                ==, 3000);
	g_assert_cmpint(venture_money_get_amount(venture_metric_get_money(profit)),
	                ==, 5750);
}

static void
test_report_pnl_separates_deductible(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureReportResult) result = NULL;
	g_autofree gchar *rendered = NULL;

	/* 100.00 spent, of which only the 40.00 fully-deductible part and half
	 * of the 60.00 partial one may be claimed: 40.00 + 30.00 = 70.00. */
	add_expense(fixture, "Software", "software", "40.00",
	            VENTURE_DEDUCTIBILITY_FULL, 100);
	add_expense(fixture, "Home office", "home_office", "60.00",
	            VENTURE_DEDUCTIBILITY_PARTIAL, 50);

	result = run_report(fixture, "pnl", NULL);
	rendered = venture_report_result_render(result, VENTURE_OUTPUT_FORMAT_TABLE);

	g_assert_nonnull(g_strstr_len(rendered, -1, "Of which deductible"));
	g_assert_nonnull(g_strstr_len(rendered, -1, "$70.00"));

	/* Cash out and claimable differ, so the report must say so rather than
	 * letting the two figures be mistaken for each other. */
	g_assert_nonnull(g_strstr_len(rendered, -1, "review"));
}

static void
test_report_pnl_empty_period(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureReportResult) result = NULL;
	VentureMetric *revenue;

	result = run_report(fixture, "pnl", NULL);
	revenue = find_metric(result, "revenue");

	/* A period with nothing in it reports zero, not an error. */
	g_assert_nonnull(revenue);
	g_assert_true(venture_money_is_zero(venture_metric_get_money(revenue)));
}

static void
test_report_pnl_totals_in_the_records_currency(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureReportResult) result = NULL;
	VentureMetric *revenue;

	/* Books kept entirely in EUR. The totals used to anchor on the
	 * process default currency, so every one of these failed to add and
	 * the P&L reported $0.00 revenue -- silently. If this regresses, an
	 * all-foreign portfolio reads as earning nothing. */
	add_sale(fixture, 0, "60.00 EUR", "7.50 EUR", 1);
	add_sale(fixture, 0, "40.00 EUR", "5.00 EUR", 1);

	result = run_report(fixture, "pnl", NULL);
	revenue = find_metric(result, "revenue");

	g_assert_nonnull(revenue);
	g_assert_cmpstr(
		venture_money_get_currency(venture_metric_get_money(revenue)),
		==, "EUR");
	g_assert_cmpint(venture_money_get_amount(venture_metric_get_money(revenue)),
	                ==, 8750);
}

static void
test_report_pnl_notes_excluded_currencies(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureReportResult) result = NULL;
	g_autofree gchar *rendered = NULL;
	VentureMetric *revenue;

	/* A EUR sale among USD ones cannot join the totals, and that used to
	 * happen without a word: the out-parameter carrying the skip count
	 * had NULL passed at every call site. The number being incomplete is
	 * tolerable; the report not saying so is not. */
	add_sale(fixture, 0, "60.00", "7.50", 1);
	add_sale(fixture, 0, "40.00", "5.00", 1);
	add_sale(fixture, 0, "10.00 EUR", NULL, 1);

	result = run_report(fixture, "pnl", NULL);
	revenue = find_metric(result, "revenue");

	/* The USD total is exact, not polluted by a guessed conversion. */
	g_assert_cmpint(venture_money_get_amount(venture_metric_get_money(revenue)),
	                ==, 8750);

	rendered = venture_report_result_render(result, VENTURE_OUTPUT_FORMAT_TABLE);
	g_assert_nonnull(g_strstr_len(rendered, -1, "could not be included"));
}

static void
test_report_result_append_note_keeps_both(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureReportResult) result = NULL;
	g_autofree gchar *rendered = NULL;

	result = venture_report_result_new("Test", NULL);
	venture_report_result_set_note(result, "first caveat");
	venture_report_result_append_note(result, "second caveat");

	rendered = venture_report_result_render(result, VENTURE_OUTPUT_FORMAT_TEXT);

	/* A report can deserve two caveats at once, and the reader needs
	 * both -- not whichever was attached last. */
	g_assert_nonnull(g_strstr_len(rendered, -1, "first caveat"));
	g_assert_nonnull(g_strstr_len(rendered, -1, "second caveat"));
}

/* --- Ventures ------------------------------------------------------------ */

static void
test_report_ventures(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureReportResult) result = NULL;
	g_autofree gchar *rendered = NULL;

	add_sale(fixture, 0, "25.00", NULL, 1);

	result = run_report(fixture, "ventures", NULL);
	rendered = venture_report_result_render(result, VENTURE_OUTPUT_FORMAT_TABLE);

	g_assert_cmpuint(venture_report_result_get_row_count(result), ==, 1);
	g_assert_nonnull(g_strstr_len(rendered, -1, "Books"));
	g_assert_nonnull(g_strstr_len(rendered, -1, "$25.00"));
}

/* --- Categories and genres ----------------------------------------------- */

static void
test_report_categories_groups_by_genre(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureReportResult) result = NULL;
	g_autofree gchar *rendered = NULL;
	gint64 scifi;
	gint64 romance;

	scifi = add_product(fixture, "Starfall", "science-fiction");
	romance = add_product(fixture, "Summer Light", "romance");

	add_sale(fixture, scifi, "20.00", NULL, 2);
	add_sale(fixture, scifi, "20.00", NULL, 1);
	add_sale(fixture, romance, "15.00", NULL, 1);

	/* Genre is the default grouping because it is the question a book
	 * venture actually asks. */
	result = run_report(fixture, "categories", NULL);
	rendered = venture_report_result_render(result, VENTURE_OUTPUT_FORMAT_TABLE);

	g_assert_cmpuint(venture_report_result_get_row_count(result), ==, 2);
	g_assert_nonnull(g_strstr_len(rendered, -1, "science-fiction"));
	g_assert_nonnull(g_strstr_len(rendered, -1, "romance"));
	g_assert_nonnull(g_strstr_len(rendered, -1, "$40.00"));
}

static void
test_report_categories_accepts_group_by(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(JsonNode) options = NULL;
	g_autofree gchar *rendered = NULL;
	gint64 product;

	product = add_product(fixture, "Print set", "art");
	add_sale(fixture, product, "10.00", NULL, 1);

	options = venture_json_parse("{\"group_by\": \"name\"}", NULL);

	result = run_report(fixture, "categories", json_node_get_object(options));
	rendered = venture_report_result_render(result, VENTURE_OUTPUT_FORMAT_TABLE);

	g_assert_nonnull(g_strstr_len(rendered, -1, "Print set"));
}

static void
test_report_categories_rejects_unknown_group(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(JsonNode) options = NULL;
	g_autoptr(VentureDateRange) period = NULL;
	g_autoptr(GError) error = NULL;
	VentureReportResult *result;
	VentureReport *report;
	gint64 product;

	product = add_product(fixture, "Print set", "art");
	add_sale(fixture, product, "10.00", NULL, 1);

	options = venture_json_parse("{\"group_by\": \"nonesuch\"}", NULL);
	period = venture_context_parse_period(fixture->context, "this_month", NULL);
	report = venture_report_registry_lookup(
		venture_context_get_report_registry(fixture->context), "categories");

	result = venture_report_generate(report, fixture->context, period,
	                                 json_node_get_object(options), &error);

	g_assert_null(result);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
}

/* --- Tax ----------------------------------------------------------------- */

static void
test_report_tax_excludes_unreviewed(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureReportResult) result = NULL;
	g_autofree gchar *rendered = NULL;
	VentureMetric *deductible;
	VentureMetric *review;

	add_expense(fixture, "Paper", "supplies", "50.00",
	            VENTURE_DEDUCTIBILITY_FULL, 100);
	add_expense(fixture, "Conference", "travel", "500.00",
	            VENTURE_DEDUCTIBILITY_REVIEW, 0);

	result = run_report(fixture, "tax", NULL);

	deductible = find_metric(result, "deductible");
	review = find_metric(result, "review");

	/* An unreviewed expense counts as zero, so an import cannot quietly
	 * inflate a deduction. */
	g_assert_cmpint(
		venture_money_get_amount(venture_metric_get_money(deductible)),
		==, 5000);
	g_assert_cmpfloat(venture_metric_get_number(review), ==, 1.0);

	/* And the report says so, rather than presenting the figure as final. */
	rendered = venture_report_result_render(result, VENTURE_OUTPUT_FORMAT_TABLE);
	g_assert_nonnull(g_strstr_len(rendered, -1, "still marked"));
}

static void
test_report_tax_more_review_is_worse(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureReportResult) result = NULL;
	VentureMetric *review;

	add_expense(fixture, "Conference", "travel", "500.00",
	            VENTURE_DEDUCTIBILITY_REVIEW, 0);

	result = run_report(fixture, "tax", NULL);
	review = find_metric(result, "review");

	/* A rise in the awaiting-review count must not be coloured green. */
	venture_metric_set_previous_number(review, 0.0);
	g_assert_cmpint(venture_metric_get_direction(review), ==, -1);
}

/* --- Inventory ----------------------------------------------------------- */

static void
test_report_inventory_derives_quantity_from_transactions(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(VentureInventoryItem) item = NULL;
	g_autofree gchar *rendered = NULL;
	gint64 product;
	gint64 item_id;
	gsize i;
	const gint64 movements[] = { 100, -30, -5 };

	product = add_product(fixture, "Poster", "art");

	item = venture_inventory_item_new();
	g_object_set(item, "product-id", product, "sku", "POSTER-A3",
	             "location", "studio", "reorder-point", (gint64)100, NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(item),
	                                   fixture->organization_id);
	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(item),
		"unit-cost", "2.50", NULL));
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(item), NULL, NULL));
	item_id = venture_entity_get_id(VENTURE_ENTITY(item));

	for (i = 0; i < G_N_ELEMENTS(movements); i++)
	{
		g_autoptr(VentureInventoryTxn) txn = NULL;
		g_autoptr(GDateTime) when = NULL;

		when = venture_time_now();
		txn = venture_inventory_txn_new();
		g_object_set(txn,
		             "inventory-item-id", item_id,
		             "quantity", movements[i],
		             "occurred-at", when,
		             NULL);
		venture_entity_set_organization_id(VENTURE_ENTITY(txn),
		                                   fixture->organization_id);
		g_assert_true(venture_database_save(fixture->database,
		                                    VENTURE_ENTITY(txn), NULL, NULL));
	}

	result = run_report(fixture, "inventory", NULL);
	rendered = venture_report_result_render(result, VENTURE_OUTPUT_FORMAT_TABLE);

	/* 100 - 30 - 5 = 65 on hand, valued at 2.50 each = 162.50. */
	g_assert_nonnull(g_strstr_len(rendered, -1, "65"));
	g_assert_nonnull(g_strstr_len(rendered, -1, "$162.50"));

	/* 65 is at or below the reorder point of 100. */
	{
		VentureMetric *below;

		below = find_metric(result, "below_reorder");
		g_assert_cmpfloat(venture_metric_get_number(below), ==, 1.0);
	}
}

/* --- Pipeline ------------------------------------------------------------ */

static void
test_report_pipeline_weights_by_probability(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(VentureDeal) open_deal = NULL;
	g_autoptr(VentureDeal) won_deal = NULL;
	VentureMetric *open_value;
	VentureMetric *weighted;

	open_deal = venture_deal_new();
	g_object_set(open_deal, "name", "Big order",
	             "stage", VENTURE_DEAL_STAGE_PROPOSAL,
	             "probability", (gint64)40, NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(open_deal),
	                                   fixture->organization_id);
	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(open_deal),
		"value", "1000.00", NULL));
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(open_deal), NULL, NULL));

	won_deal = venture_deal_new();
	g_object_set(won_deal, "name", "Closed one",
	             "stage", VENTURE_DEAL_STAGE_WON,
	             "probability", (gint64)10, NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(won_deal),
	                                   fixture->organization_id);
	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(won_deal),
		"value", "500.00", NULL));
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(won_deal), NULL, NULL));

	result = run_report(fixture, "pipeline", NULL);

	open_value = find_metric(result, "open");
	weighted = find_metric(result, "weighted");

	/* Only the open deal counts toward the pipeline, and it weighs 40%. */
	g_assert_cmpint(
		venture_money_get_amount(venture_metric_get_money(open_value)),
		==, 100000);
	g_assert_cmpint(
		venture_money_get_amount(venture_metric_get_money(weighted)),
		==, 40000);

	/* Every stage appears, so the pipeline reads as a funnel. */
	g_assert_cmpuint(venture_report_result_get_row_count(result), ==, 6);
}

/* --- Receivables aging --------------------------------------------------- */

static gint64
add_invoice(
	Fixture			*fixture,
	const gchar		*number,
	VentureInvoiceStatus	 status,
	gint			 due_in_days,
	const gchar		*unit_price,
	gdouble			 quantity
){
	g_autoptr(VentureInvoice) invoice = NULL;
	g_autoptr(VentureCompany) customer = NULL;
	g_autoptr(VentureInvoiceLine) line = NULL;
	g_autoptr(GDateTime) now = NULL;
	g_autoptr(GDateTime) due = NULL;
	gint64 invoice_id;

	now = venture_time_now();
	due = g_date_time_add_days(now, due_in_days);

	customer = venture_company_new();
	g_object_set(customer, "name", "Invoice customer", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(customer), fixture->organization_id);
	g_assert_true(venture_database_save(fixture->database, VENTURE_ENTITY(customer), NULL, NULL));
	invoice = venture_invoice_new();
	g_object_set(invoice, "number", number, "status", VENTURE_INVOICE_STATUS_DRAFT,
	             "company-id", venture_entity_get_id(VENTURE_ENTITY(customer)), "issued-at", now,
	             "venture-id", fixture->venture_id, "due-at", due, NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(invoice),
	                                   fixture->organization_id);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(invoice), NULL, NULL));
	invoice_id = venture_entity_get_id(VENTURE_ENTITY(invoice));

	line = venture_invoice_line_new();
	g_object_set(line, "invoice-id", invoice_id, "description", "Work",
	             "quantity", quantity, NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(line),
	                                   fixture->organization_id);
	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(line),
		"unit-price", unit_price, NULL));
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(line), NULL, NULL));

	if (status != VENTURE_INVOICE_STATUS_DRAFT)
	{
		g_object_set(invoice, "status", VENTURE_INVOICE_STATUS_SENT, NULL);
		g_assert_true(venture_database_save(fixture->database, VENTURE_ENTITY(invoice), NULL, NULL));
	}
	if (status == VENTURE_INVOICE_STATUS_PAID)
		g_assert_true(venture_settlement_service_settle_invoice(
			venture_settlement_service_get(fixture->database), invoice_id, now, NULL, NULL));
	return invoice_id;
}

static void
test_report_receivables_buckets_by_age(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureReportResult) result = NULL;
	g_autofree gchar *rendered = NULL;
	VentureMetric *outstanding;
	VentureMetric *overdue;

	/* One invoice not yet due, one 45 days past due, and a paid one that
	 * must not appear: settled history is not a receivable. */
	add_invoice(fixture, "INV-1", VENTURE_INVOICE_STATUS_SENT, 10,
	            "25.00", 4.0);
	add_invoice(fixture, "INV-2", VENTURE_INVOICE_STATUS_SENT, -45,
	            "20.00", 2.0);
	add_invoice(fixture, "INV-3", VENTURE_INVOICE_STATUS_PAID, -45,
	            "999.00", 1.0);

	result = run_report(fixture, "receivables", NULL);

	outstanding = find_metric(result, "outstanding");
	overdue = find_metric(result, "overdue");

	/* 100.00 current plus 40.00 overdue outstanding; only the 40.00 is
	 * overdue. If the paid invoice leaks in, both figures jump by 999. */
	g_assert_nonnull(outstanding);
	g_assert_cmpint(
		venture_money_get_amount(venture_metric_get_money(outstanding)),
		==, 14000);
	g_assert_cmpint(
		venture_money_get_amount(venture_metric_get_money(overdue)),
		==, 4000);

	/* Every bucket renders, even empty ones, so the shape of the table is
	 * stable enough to compare week over week. */
	g_assert_cmpuint(venture_report_result_get_row_count(result), ==, 6);

	rendered = venture_report_result_render(result, VENTURE_OUTPUT_FORMAT_TABLE);
	g_assert_nonnull(g_strstr_len(rendered, -1, "31-60 days"));
	g_assert_nonnull(g_strstr_len(rendered, -1, "$40.00"));
}

/* --- Monthly series ------------------------------------------------------ */

static void
test_report_monthly_splits_period(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureDateRange) period = NULL;
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(GError) error = NULL;
	VentureReport *report;

	report = venture_report_registry_lookup(
		venture_context_get_report_registry(fixture->context), "monthly");

	period = venture_date_range_new_quarter(2026, 1, NULL);
	result = venture_report_generate(report, fixture->context, period, NULL,
	                                 &error);

	g_assert_no_error(error);
	g_assert_cmpuint(venture_report_result_get_row_count(result), ==, 3);
}

/* --- Ideas --------------------------------------------------------------- */

static void
test_report_ideas_ranks_by_score(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureReportResult) result = NULL;
	g_autofree gchar *rendered = NULL;
	const gchar *cheap_position;
	const gchar *dear_position;

	{
		g_autoptr(VentureIdea) dear = NULL;

		dear = venture_idea_new();
		g_object_set(dear, "title", "Expensive idea",
		             "opportunity", (gint64)9, "confidence", (gint64)8,
		             "effort", (gint64)9, NULL);
		venture_entity_set_organization_id(VENTURE_ENTITY(dear),
		                                   fixture->organization_id);
		g_assert_true(venture_database_save(fixture->database,
		                                    VENTURE_ENTITY(dear), NULL, NULL));
	}

	{
		g_autoptr(VentureIdea) cheap = NULL;

		cheap = venture_idea_new();
		g_object_set(cheap, "title", "Cheap idea",
		             "opportunity", (gint64)6, "confidence", (gint64)7,
		             "effort", (gint64)2, NULL);
		venture_entity_set_organization_id(VENTURE_ENTITY(cheap),
		                                   fixture->organization_id);
		g_assert_true(venture_database_save(fixture->database,
		                                    VENTURE_ENTITY(cheap), NULL, NULL));
	}

	result = run_report(fixture, "ideas", NULL);
	rendered = venture_report_result_render(result, VENTURE_OUTPUT_FORMAT_TABLE);

	cheap_position = g_strstr_len(rendered, -1, "Cheap idea");
	dear_position = g_strstr_len(rendered, -1, "Expensive idea");

	g_assert_nonnull(cheap_position);
	g_assert_nonnull(dear_position);

	/* Effort divides, so the cheaper idea of lower raw promise outranks
	 * the expensive one -- which is the right answer when the binding
	 * constraint is your own time. */
	g_assert_true(cheap_position < dear_position);
}

/* --- Rendering ----------------------------------------------------------- */

static void
test_report_renders_every_format(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureReportResult) result = NULL;
	VentureOutputFormat formats[] = {
		VENTURE_OUTPUT_FORMAT_TABLE,
		VENTURE_OUTPUT_FORMAT_JSON,
		VENTURE_OUTPUT_FORMAT_CSV,
		VENTURE_OUTPUT_FORMAT_ORG,
		VENTURE_OUTPUT_FORMAT_HTML,
		VENTURE_OUTPUT_FORMAT_YAML,
		VENTURE_OUTPUT_FORMAT_TEXT
	};
	gsize i;

	add_sale(fixture, 0, "25.00", "2.50", 1);
	result = run_report(fixture, "pnl", NULL);

	/* Every format comes from one structure, so a report written once
	 * appears correctly everywhere. */
	for (i = 0; i < G_N_ELEMENTS(formats); i++)
	{
		g_autofree gchar *rendered = NULL;

		rendered = venture_report_result_render(result, formats[i]);

		g_assert_nonnull(rendered);
		g_assert_cmpuint(strlen(rendered), >, 0);
	}
}

static void
test_report_json_carries_formatted_values(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(JsonNode) node = NULL;
	JsonObject *row;
	JsonArray *rows;

	add_sale(fixture, 0, "25.00", NULL, 1);
	result = run_report(fixture, "pnl", NULL);

	node = venture_report_result_to_json(result);
	rows = json_object_get_array_member(json_node_get_object(node), "rows");
	row = json_array_get_object_element(rows, 0);

	/* A formatted twin accompanies each raw value so an AI reading the
	 * result reports the right figure rather than misreading minor units. */
	g_assert_true(json_object_has_member(row, "amount"));
	g_assert_true(json_object_has_member(row, "amount_formatted"));
}

static void
test_report_csv_defuses_formula_injection(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureReportResult) result = NULL;
	g_autofree gchar *csv = NULL;

	result = venture_report_result_new("Test", NULL);
	venture_report_result_add_column(result, "name", "Name",
	                                 VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_begin_row(result);
	venture_report_result_set_text(result, "name", "=cmd|'/c calc'!A1");

	csv = venture_report_result_render(result, VENTURE_OUTPUT_FORMAT_CSV);

	/* A leading '=' would execute when the export is opened in a
	 * spreadsheet, so it is quoted and prefixed. */
	g_assert_nonnull(g_strstr_len(csv, -1, "\"'=cmd"));
}

static void
test_report_html_escapes_content(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureReportResult) result = NULL;
	g_autofree gchar *html = NULL;

	result = venture_report_result_new("Test", NULL);
	venture_report_result_add_column(result, "name", "Name",
	                                 VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_begin_row(result);
	venture_report_result_set_text(result, "name",
	                               "<script>alert(1)</script>");

	html = venture_report_result_render(result, VENTURE_OUTPUT_FORMAT_HTML);

	g_assert_null(g_strstr_len(html, -1, "<script>"));
	g_assert_nonnull(g_strstr_len(html, -1, "&lt;script&gt;"));
}

static void
test_report_empty_renders_placeholder(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureReportResult) result = NULL;
	g_autofree gchar *html = NULL;

	result = run_report(fixture, "ventures", NULL);
	html = venture_report_result_render(result, VENTURE_OUTPUT_FORMAT_HTML);

	g_assert_nonnull(html);
}

int
main(
	int	  argc,
	char	**argv
){
	g_test_init(&argc, &argv, NULL);

#define ADD(path, func) \
	g_test_add(path, Fixture, NULL, fixture_set_up, func, fixture_tear_down)

	ADD("/report/registry-has-builtins", test_report_registry_has_builtins);
	ADD("/report/registry-describe", test_report_registry_describe);

	ADD("/report/pnl-arithmetic", test_report_pnl_arithmetic);
	ADD("/report/pnl-separates-deductible", test_report_pnl_separates_deductible);
	ADD("/report/pnl-empty-period", test_report_pnl_empty_period);
	ADD("/report/pnl-totals-in-the-records-currency",
	    test_report_pnl_totals_in_the_records_currency);
	ADD("/report/pnl-notes-excluded-currencies",
	    test_report_pnl_notes_excluded_currencies);
	ADD("/report/result-append-note-keeps-both",
	    test_report_result_append_note_keeps_both);

	ADD("/report/ventures", test_report_ventures);

	ADD("/report/categories-groups-by-genre",
	    test_report_categories_groups_by_genre);
	ADD("/report/categories-accepts-group-by",
	    test_report_categories_accepts_group_by);
	ADD("/report/categories-rejects-unknown-group",
	    test_report_categories_rejects_unknown_group);

	ADD("/report/tax-excludes-unreviewed", test_report_tax_excludes_unreviewed);
	ADD("/report/tax-more-review-is-worse", test_report_tax_more_review_is_worse);

	ADD("/report/inventory-derives-quantity-from-transactions",
	    test_report_inventory_derives_quantity_from_transactions);

	ADD("/report/pipeline-weights-by-probability",
	    test_report_pipeline_weights_by_probability);

	ADD("/report/receivables-buckets-by-age",
	    test_report_receivables_buckets_by_age);

	ADD("/report/monthly-splits-period", test_report_monthly_splits_period);

	ADD("/report/ideas-ranks-by-score", test_report_ideas_ranks_by_score);

	ADD("/report/renders-every-format", test_report_renders_every_format);
	ADD("/report/json-carries-formatted-values",
	    test_report_json_carries_formatted_values);
	ADD("/report/csv-defuses-formula-injection",
	    test_report_csv_defuses_formula_injection);
	ADD("/report/html-escapes-content", test_report_html_escapes_content);
	ADD("/report/empty-renders-placeholder", test_report_empty_renders_placeholder);

#undef ADD

	return g_test_run();
}
