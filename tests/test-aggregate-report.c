/*
 * test-aggregate-report.c - The generic aggregation report
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * One report that sums, averages and counts any record type is one report
 * that can be wrong about every record type at once. These tests hold it
 * to the rules the books already obey: money is added per currency and
 * never across, an average of money is one exact division rounded half to
 * even, a month is a UTC month in every zone, and what the caller may not
 * see -- another organisation's rows, a module that is off, personal data,
 * a sensitive field -- stays out of the totals.
 */

#include <venture.h>

#include <libsoup/soup.h>
#include <string.h>
#include <unistd.h>

#include "venture-test-util.h"

typedef struct
{
	VentureConfig	*config;
	VentureDatabase	*database;
	VentureContext	*context;
	gint64		 organization_id;
	gint64		 venture_id;
} Fixture;

#define ID(record) (venture_entity_get_id(VENTURE_ENTITY(record)))

/* Saves, expecting success; the refusal's reason is the failure. */
static void
save(
	Fixture		*fixture,
	gpointer	 record
){
	g_autoptr(GError) error = NULL;

	if (!venture_database_save(fixture->database, VENTURE_ENTITY(record), NULL, &error))
		g_error("save refused: %s", error->message);
}

static void
fixture_set_up(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureVenture) venture = NULL;

	(void)user_data;

	fixture->config = venture_config_new();
	fixture->database = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	fixture->context = venture_context_new(fixture->config, fixture->database);
	g_assert_true(venture_database_migrate(fixture->database,
		venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);

	fixture->organization_id =
		venture_context_get_default_organization_id(fixture->context);

	/* A user-defined currency: four minor digits, like gold, silver and
	 * copper. It must never be added to dollars. */
	g_assert_true(venture_currency_register("GOLD", 4, NULL, FALSE, NULL, &error));
	g_assert_no_error(error);

	venture = venture_venture_new();
	g_object_set(venture, "name", "Shop", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(venture), fixture->organization_id);
	save(fixture, venture);
	fixture->venture_id = ID(venture);
}

static void
fixture_tear_down(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureConfig) everything = NULL;
	g_autoptr(VentureModuleRegistry) registry = NULL;

	(void)user_data;

	g_clear_object(&fixture->context);
	g_clear_object(&fixture->database);
	g_clear_object(&fixture->config);
	venture_currency_clear_registered();

	/* The entity registry is process-wide; a test that switched a module
	 * off must not leave it off for the next. */
	everything = venture_config_new();
	registry = venture_module_registry_new();
	venture_module_registry_register_builtins(registry);
	venture_module_registry_configure(registry, everything, NULL);
	venture_module_registry_apply(registry, venture_entity_registry_get_default());
}

/* A saved product, optionally filed under a category. */
static VentureEntity *
product(
	Fixture		*fixture,
	const gchar	*name,
	gint64		 category_id
){
	VentureEntity *record;

	record = VENTURE_ENTITY(venture_product_new());
	g_object_set(record, "name", name, "venture-id", fixture->venture_id,
	             "category-id", category_id, NULL);
	venture_entity_set_organization_id(record, fixture->organization_id);
	save(fixture, record);

	return record;
}

/* A saved category under @parent_id (0 for a top level). */
static VentureEntity *
category(
	Fixture		*fixture,
	const gchar	*name,
	gint64		 parent_id
){
	VentureEntity *node;

	node = VENTURE_ENTITY(venture_category_new());
	g_object_set(node, "name", name, "parent-id", parent_id,
	             "applies-to", "product", NULL);
	venture_entity_set_organization_id(node, fixture->organization_id);
	save(fixture, node);

	return node;
}

/*
 * A saved sale: @gross as "12.50 USD", @when as an ISO timestamp or a
 * date, in @organization_id (0 for the default).
 */
static gint64
sale_full(
	Fixture		*fixture,
	gint64		 organization_id,
	gint64		 venture_id,
	gint64		 product_id,
	const gchar	*channel,
	const gchar	*gross,
	const gchar	*when,
	gint64		 quantity
){
	g_autoptr(VentureEntity) record = NULL;
	g_autoptr(VentureMoney) money = NULL;
	g_autoptr(GDateTime) at = NULL;

	record = VENTURE_ENTITY(venture_sale_new());
	money = (NULL != gross) ? venture_money_from_string(gross, NULL, NULL) : NULL;
	at = venture_time_from_string(when, NULL);
	g_assert_nonnull(at);

	if (NULL != gross)
		g_assert_nonnull(money);

	g_object_set(record, "venture-id", venture_id, "product-id", product_id,
	             "channel", channel, "gross", money, "occurred-at", at,
	             "quantity", quantity, NULL);
	venture_entity_set_organization_id(record, (0 != organization_id)
		? organization_id : fixture->organization_id);
	save(fixture, record);

	return ID(record);
}

static gint64
sale(
	Fixture		*fixture,
	gint64		 product_id,
	const gchar	*channel,
	const gchar	*gross,
	const gchar	*when
){
	return sale_full(fixture, 0, fixture->venture_id, product_id, channel,
	                 gross, when, 1);
}

/*
 * Runs the report with @first_key/value pairs as string options (a value
 * that parses as an integer is sent as one, as the web layer does).
 */
static VentureReportResult *
run(
	Fixture		 *fixture,
	const gchar	 *period,
	GError		**error,
	...
){
	g_autoptr(JsonObject) options = NULL;
	g_autoptr(VentureDateRange) range = NULL;
	VentureReport *report;
	const gchar *key;
	va_list args;

	options = json_object_new();
	va_start(args, error);

	while (NULL != (key = va_arg(args, const gchar *)))
	{
		const gchar *value;

		value = va_arg(args, const gchar *);

		if ((0 == g_strcmp0(key, "category_depth")) ||
		    (0 == g_strcmp0(key, "organization_id")))
			json_object_set_int_member(options, key, g_ascii_strtoll(value, NULL, 10));
		else
			json_object_set_string_member(options, key, value);
	}

	va_end(args);

	report = venture_report_registry_lookup(
		venture_context_get_report_registry(fixture->context), "aggregate");
	g_assert_nonnull(report);
	range = venture_context_parse_period(fixture->context, period, NULL);
	g_assert_nonnull(range);

	return venture_report_generate(report, fixture->context, range, options, error);
}

/* Runs, expecting an answer. */
#define RUN(fixture, period, ...) ({ \
	g_autoptr(GError) run_error = NULL; \
	VentureReportResult *run_result = run(fixture, period, &run_error, __VA_ARGS__, NULL); \
	if (NULL == run_result) \
		g_error("aggregate refused: %s", run_error->message); \
	run_result; })

/* Runs, expecting a refusal with @code whose message holds @fragment. */
static void
assert_refused(
	GError		*error,
	gint		 code,
	const gchar	*fragment
){
	g_assert_nonnull(error);
	g_assert_error(error, VENTURE_ERROR, code);

	if (NULL == strstr(error->message, fragment))
		g_error("expected \"%s\" in: %s", fragment, error->message);
}

static const gchar *
text(
	VentureReportResult	*result,
	guint			 row,
	const gchar		*key
){
	const GValue *value;

	value = venture_report_result_get_cell(result, row, key);
	g_assert_nonnull(value);
	g_assert_true(G_VALUE_HOLDS_STRING(value));

	return g_value_get_string(value);
}

static gdouble
number(
	VentureReportResult	*result,
	guint			 row,
	const gchar		*key
){
	const GValue *value;

	value = venture_report_result_get_cell(result, row, key);
	g_assert_nonnull(value);
	g_assert_true(G_VALUE_HOLDS_DOUBLE(value));

	return g_value_get_double(value);
}

/* A money cell as its round-trip text, "15.50 USD". */
static gchar *
money(
	VentureReportResult	*result,
	guint			 row,
	const gchar		*key
){
	const GValue *value;

	value = venture_report_result_get_cell(result, row, key);
	g_assert_nonnull(value);
	g_assert_true(G_VALUE_HOLDS(value, VENTURE_TYPE_MONEY));

	return venture_money_to_string(g_value_get_boxed(value));
}

#define ASSERT_MONEY(result, row, key, expected) G_STMT_START { \
	g_autofree gchar *assert_money_text = money(result, row, key); \
	g_assert_cmpstr(assert_money_text, ==, expected); \
} G_STMT_END

/*
 * Gold and dollars are two rows. What breaks if this regresses: a sum
 * over a shop that sells in two currencies is a number in neither.
 */
static void
test_sum_per_currency(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(VentureEntity) herb = NULL;

	(void)user_data;

	herb = product(fixture, "Herb", 0);
	sale(fixture, ID(herb), "auction", "10.00 USD", "2026-03-02");
	sale(fixture, ID(herb), "auction", "5.50 USD", "2026-03-03");
	sale(fixture, ID(herb), "auction", "12.0000 GOLD", "2026-03-04");
	sale(fixture, ID(herb), "auction", "3.0000 GOLD", "2026-03-05");
	/* Outside the period: bounded by date_field. */
	sale(fixture, ID(herb), "auction", "99.00 USD", "2026-04-01");

	result = RUN(fixture, "2026-03", "type", "sale", "measure", "gross",
	             "date_field", "occurred_at");

	g_assert_cmpuint(venture_report_result_get_row_count(result), ==, 2);
	g_assert_cmpstr(text(result, 0, "currency"), ==, "GOLD");
	ASSERT_MONEY(result, 0, "value", "15.0000 GOLD");
	g_assert_cmpfloat(number(result, 0, "records"), ==, 2);
	g_assert_cmpstr(text(result, 1, "currency"), ==, "USD");
	ASSERT_MONEY(result, 1, "value", "15.50 USD");

	/* The engine the dashboard uses agrees, total for total. */
	{
		g_autoptr(VentureQuery) query = NULL;
		g_autoptr(GPtrArray) rows = NULL;
		g_autoptr(GPtrArray) totals = NULL;
		g_autoptr(VentureFieldSpec) spec = NULL;
		g_autoptr(GError) error = NULL;
		gboolean custom;

		query = venture_query_new(VENTURE_TYPE_SALE);
		venture_query_set_limit(query, 0);
		rows = venture_database_find(fixture->database, query, &error);
		g_assert_no_error(error);
		spec = venture_aggregate_find_field(fixture->database,
			fixture->organization_id, "sale", "gross", &custom, &error);
		g_assert_no_error(error);
		totals = venture_aggregate_sum(rows, spec, custom, &error);
		g_assert_no_error(error);
		g_assert_cmpuint(totals->len, ==, 2);
		g_assert_cmpstr(((VentureAggregateTotal *)g_ptr_array_index(totals, 0))->currency,
		                ==, "GOLD");
		g_assert_cmpstr(((VentureAggregateTotal *)g_ptr_array_index(totals, 1))->currency,
		                ==, "USD");
		g_assert_cmpint(venture_money_get_amount(
			((VentureAggregateTotal *)g_ptr_array_index(totals, 1))->money), ==, 11450);
	}
}

/*
 * An average of money is one exact division rounded half to even. 0.01
 * and 0.04 average 0.025, which is 0.02 -- not the 0.03 that rounding
 * half up gives, nor whatever a double makes of 0.025. Minimum and maximum
 * ride the same cells.
 */
static void
test_avg_money_half_even(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureReportResult) avg = NULL;
	g_autoptr(VentureReportResult) min = NULL;
	g_autoptr(VentureReportResult) max = NULL;
	g_autoptr(VentureEntity) item = NULL;

	(void)user_data;

	item = product(fixture, "Pin", 0);
	sale(fixture, ID(item), "a", "0.01 USD", "2026-03-02");
	sale(fixture, ID(item), "a", "0.04 USD", "2026-03-02");
	sale(fixture, ID(item), "b", "0.01 USD", "2026-03-02");
	sale(fixture, ID(item), "b", "0.02 USD", "2026-03-02");

	avg = RUN(fixture, "2026-03", "type", "sale", "measure", "gross",
	          "aggregate", "avg", "group_by", "channel", "date_field", "occurred_at");
	g_assert_cmpuint(venture_report_result_get_row_count(avg), ==, 2);
	g_assert_cmpstr(text(avg, 0, "channel"), ==, "a");
	ASSERT_MONEY(avg, 0, "value", "0.02 USD");
	/* 0.015 is also a tie, and 2 is the even neighbour. */
	g_assert_cmpstr(text(avg, 1, "channel"), ==, "b");
	ASSERT_MONEY(avg, 1, "value", "0.02 USD");

	min = RUN(fixture, "2026-03", "type", "sale", "measure", "gross",
	          "aggregate", "min", "group_by", "channel", "date_field", "occurred_at");
	ASSERT_MONEY(min, 0, "value", "0.01 USD");
	max = RUN(fixture, "2026-03", "type", "sale", "measure", "gross",
	          "aggregate", "max", "group_by", "channel", "date_field", "occurred_at");
	ASSERT_MONEY(max, 0, "value", "0.04 USD");
	ASSERT_MONEY(max, 1, "value", "0.02 USD");
}

/*
 * Counting records, and counting distinct values. What breaks: a count
 * that counts a product once per sale when the question was how many
 * products sold.
 */
static void
test_count_and_distinct(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureReportResult) count = NULL;
	g_autoptr(VentureReportResult) distinct = NULL;
	g_autoptr(VentureReportResult) integers = NULL;
	g_autoptr(VentureEntity) a = NULL;
	g_autoptr(VentureEntity) b = NULL;

	(void)user_data;

	a = product(fixture, "Alpha", 0);
	b = product(fixture, "Beta", 0);
	sale(fixture, ID(a), "etsy", "1.00 USD", "2026-03-02");
	sale(fixture, ID(a), "etsy", "1.00 USD", "2026-03-03");
	sale(fixture, ID(b), "etsy", "1.00 USD", "2026-03-04");
	sale(fixture, ID(b), "shop", "1.00 USD", "2026-03-05");

	count = RUN(fixture, "2026-03", "type", "sale", "group_by", "channel",
	            "date_field", "occurred_at");
	g_assert_cmpuint(venture_report_result_get_row_count(count), ==, 2);
	g_assert_cmpstr(text(count, 0, "channel"), ==, "etsy");
	g_assert_cmpfloat(number(count, 0, "records"), ==, 3);
	g_assert_cmpfloat(number(count, 1, "records"), ==, 1);

	distinct = RUN(fixture, "2026-03", "type", "sale", "measure", "product_id",
	               "aggregate", "count_distinct", "group_by", "channel",
	               "date_field", "occurred_at");
	g_assert_cmpfloat(number(distinct, 0, "value"), ==, 2);
	g_assert_cmpfloat(number(distinct, 1, "value"), ==, 1);

	/* An integer measure sums exactly and needs no currency column. */
	integers = RUN(fixture, "2026-03", "type", "sale", "measure", "quantity",
	               "date_field", "occurred_at");
	g_assert_cmpuint(venture_report_result_get_row_count(integers), ==, 1);
	g_assert_cmpfloat(number(integers, 0, "value"), ==, 4);
	g_assert_null(venture_report_result_get_cell(integers, 0, "currency"));
}

/*
 * Groups by an enum (shown by its label), a reference (shown by the
 * record's name), a custom field (by its name) and a category through a
 * reference, rolled up by depth. What breaks: a table of nicks and ids
 * nobody can read, or a category tree that cannot be totalled a level up.
 */
static void
test_group_by_kinds(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) materials = NULL;
	g_autoptr(VentureEntity) herbs = NULL;
	g_autoptr(VentureEntity) rare = NULL;
	g_autoptr(VentureEntity) ore = NULL;
	g_autoptr(VentureEntity) bloom = NULL;
	g_autoptr(VentureEntity) leaf = NULL;
	g_autoptr(VentureEntity) iron = NULL;
	g_autoptr(VentureEntity) field = NULL;
	g_autoptr(GError) error = NULL;
	gint64 graded;

	(void)user_data;

	materials = category(fixture, "Materials", 0);
	herbs = category(fixture, "Herbs", ID(materials));
	rare = category(fixture, "Rare", ID(herbs));
	ore = category(fixture, "Ore", ID(materials));
	bloom = product(fixture, "Bloom", ID(rare));
	leaf = product(fixture, "Leaf", ID(herbs));
	iron = product(fixture, "Iron", ID(ore));

	sale(fixture, ID(bloom), "auction", "4.00 USD", "2026-03-02");
	sale(fixture, ID(leaf), "auction", "2.00 USD", "2026-03-02");
	sale(fixture, ID(iron), "auction", "1.00 USD", "2026-03-02");
	sale(fixture, 0, "auction", "0.50 USD", "2026-03-02");

	/* A reference, by name; no product is "(none)". */
	{
		g_autoptr(VentureReportResult) result = NULL;

		result = RUN(fixture, "2026-03", "type", "sale", "measure", "gross",
		             "group_by", "product_id", "date_field", "occurred_at");
		g_assert_cmpuint(venture_report_result_get_row_count(result), ==, 4);
		g_assert_cmpstr(text(result, 0, "product_id"), ==, "(none)");
		g_assert_cmpstr(text(result, 1, "product_id"), ==, "Bloom");
		ASSERT_MONEY(result, 1, "value", "4.00 USD");
	}

	/* A category through the product, by path, then rolled up. */
	{
		g_autoptr(VentureReportResult) leaves = NULL;
		g_autoptr(VentureReportResult) depth1 = NULL;
		g_autoptr(VentureReportResult) depth0 = NULL;

		leaves = RUN(fixture, "2026-03", "type", "sale", "measure", "gross",
		             "group_by", "product_id.category_id", "date_field", "occurred_at");
		g_assert_cmpuint(venture_report_result_get_row_count(leaves), ==, 4);
		g_assert_cmpstr(text(leaves, 1, "product_id.category_id"), ==, "Materials / Herbs");
		g_assert_cmpstr(text(leaves, 2, "product_id.category_id"), ==, "Materials / Herbs / Rare");
		g_assert_cmpstr(text(leaves, 3, "product_id.category_id"), ==, "Materials / Ore");

		depth1 = RUN(fixture, "2026-03", "type", "sale", "measure", "gross",
		             "group_by", "product_id.category_id", "category_depth", "1",
		             "date_field", "occurred_at");
		g_assert_cmpuint(venture_report_result_get_row_count(depth1), ==, 3);
		g_assert_cmpstr(text(depth1, 1, "product_id.category_id"), ==, "Materials / Herbs");
		ASSERT_MONEY(depth1, 1, "value", "6.00 USD");
		g_assert_cmpstr(text(depth1, 2, "product_id.category_id"), ==, "Materials / Ore");

		depth0 = RUN(fixture, "2026-03", "type", "sale", "measure", "gross",
		             "group_by", "product_id.category_id", "category_depth", "0",
		             "date_field", "occurred_at");
		g_assert_cmpuint(venture_report_result_get_row_count(depth0), ==, 2);
		g_assert_cmpstr(text(depth0, 1, "product_id.category_id"), ==, "Materials");
		ASSERT_MONEY(depth0, 1, "value", "7.00 USD");

		/* Renaming a parent renames the group: the path is computed. */
		g_object_set(materials, "name", "Goods", NULL);
		save(fixture, materials);
		g_clear_object(&depth0);
		depth0 = RUN(fixture, "2026-03", "type", "sale", "measure", "gross",
		             "group_by", "product_id.category_id", "category_depth", "0",
		             "date_field", "occurred_at");
		g_assert_cmpstr(text(depth0, 1, "product_id.category_id"), ==, "Goods");
	}

	/* A custom field, by the name it was defined with. */
	field = venture_custom_fields_service_define(
		venture_custom_fields_service_get(fixture->database),
		fixture->organization_id, "sale", "grade", "string", FALSE, NULL, NULL, &error);
	g_assert_no_error(error);
	graded = sale(fixture, ID(bloom), "auction", "8.00 USD", "2026-03-09");
	{
		g_autoptr(VentureEntity) fresh = NULL;
		g_autoptr(VentureReportResult) result = NULL;

		fresh = venture_database_get(fixture->database, VENTURE_TYPE_SALE, graded, &error);
		g_assert_no_error(error);
		venture_entity_set_attribute(fresh, "grade", "prime");
		save(fixture, fresh);

		result = RUN(fixture, "2026-03", "type", "sale", "measure", "gross",
		             "group_by", "grade", "date_field", "occurred_at");
		g_assert_cmpuint(venture_report_result_get_row_count(result), ==, 2);
		g_assert_cmpstr(text(result, 0, "grade"), ==, "(none)");
		g_assert_cmpstr(text(result, 1, "grade"), ==, "prime");
		ASSERT_MONEY(result, 1, "value", "8.00 USD");
	}

	/* An enum, by label, and three levels at once. */
	{
		g_autoptr(VentureReportResult) result = NULL;
		g_autoptr(VentureTicket) one = NULL;
		g_autoptr(VentureTicket) two = NULL;
		g_autoptr(VentureTicket) three = NULL;

		one = venture_ticket_new();
		g_object_set(one, "title", "One", "status", VENTURE_TICKET_STATUS_IN_PROGRESS, NULL);
		venture_entity_set_organization_id(VENTURE_ENTITY(one), fixture->organization_id);
		save(fixture, one);
		two = venture_ticket_new();
		g_object_set(two, "title", "Two", "status", VENTURE_TICKET_STATUS_IN_PROGRESS, NULL);
		venture_entity_set_organization_id(VENTURE_ENTITY(two), fixture->organization_id);
		save(fixture, two);
		three = venture_ticket_new();
		g_object_set(three, "title", "Three", "status", VENTURE_TICKET_STATUS_DONE, NULL);
		venture_entity_set_organization_id(VENTURE_ENTITY(three), fixture->organization_id);
		save(fixture, three);

		result = RUN(fixture, "all", "type", "ticket", "group_by", "status");
		g_assert_cmpuint(venture_report_result_get_row_count(result), ==, 2);
		g_assert_cmpstr(text(result, 0, "status"), ==, "Done");
		g_assert_cmpfloat(number(result, 0, "records"), ==, 1);
		g_assert_cmpstr(text(result, 1, "status"), ==, "In progress");
		g_assert_cmpfloat(number(result, 1, "records"), ==, 2);

		/* The spine's timestamps bound and bucket like any date field. */
		{
			g_autoptr(VentureReportResult) created = NULL;
			g_autoptr(GDateTime) now = g_date_time_new_now_utc();
			g_autofree gchar *year = g_strdup_printf("%d", g_date_time_get_year(now));

			created = RUN(fixture, "all", "type", "ticket", "date_field",
			              "created_at", "bucket", "year");
			g_assert_cmpuint(venture_report_result_get_row_count(created), >=, 1);
			g_assert_cmpfloat(number(created, 0, "records"), ==, 3);
			g_assert_cmpstr(text(created, 0, "bucket"), ==, year);
		}
	}
}

/*
 * Grouped by a money field, a denominated currency's group is labelled as
 * a person reads it while its key stays the parseable decimal. What
 * breaks: a table of "7.0720 GEMS" beside a price field that shows
 * "7g 7s 20c".
 */
static void
test_group_by_denominated_money(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureReportResult) result = NULL;

	(void)user_data;

	g_assert_true(venture_currency_register("GEMS", 4, NULL, FALSE,
		"[{\"suffix\":\"g\",\"units\":10000},"
		"{\"suffix\":\"s\",\"units\":100},"
		"{\"suffix\":\"c\",\"units\":1}]", &error));
	g_assert_no_error(error);

	sale(fixture, 0, "auction", "7.0720 GEMS", "2026-03-02");

	result = RUN(fixture, "2026-03", "type", "sale", "group_by", "gross",
	             "date_field", "occurred_at");
	g_assert_cmpuint(venture_report_result_get_row_count(result), ==, 1);
	g_assert_cmpstr(text(result, 0, "gross"), ==, "7g 7s 20c");
}

/*
 * Buckets are UTC calendar months, whatever the configured zone. A sale
 * stored on 1 March (midnight UTC) is in March, and in the "2026-03"
 * period, in Los Angeles too. What breaks: every first of the month
 * reported in the month before, west of Greenwich.
 */
static void
test_date_buckets_west_of_utc(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureReportResult) months = NULL;
	g_autoptr(VentureReportResult) quarters = NULL;
	g_autoptr(VentureReportResult) march = NULL;
	g_autoptr(VentureReportResult) weeks = NULL;
	g_autoptr(VentureEntity) item = NULL;

	(void)user_data;

	g_object_set(fixture->config, "locale-timezone", "America/Los_Angeles", NULL);
	item = product(fixture, "Thing", 0);
	sale(fixture, ID(item), "shop", "1.00 USD", "2026-01-15");
	sale(fixture, ID(item), "shop", "2.00 USD", "2026-02-01");
	sale(fixture, ID(item), "shop", "3.00 USD", "2026-02-28T23:30:00Z");
	sale(fixture, ID(item), "shop", "4.00 USD", "2026-03-01");

	months = RUN(fixture, "2026-01-01..2026-03-31", "type", "sale",
	             "measure", "gross", "date_field", "occurred_at", "bucket", "month");
	g_assert_cmpuint(venture_report_result_get_row_count(months), ==, 3);
	g_assert_cmpstr(text(months, 0, "bucket"), ==, "2026-01");
	g_assert_cmpstr(text(months, 1, "bucket"), ==, "2026-02");
	ASSERT_MONEY(months, 1, "value", "5.00 USD");
	g_assert_cmpstr(text(months, 2, "bucket"), ==, "2026-03");
	ASSERT_MONEY(months, 2, "value", "4.00 USD");

	quarters = RUN(fixture, "2026-01-01..2026-03-31", "type", "sale",
	               "measure", "gross", "date_field", "occurred_at", "bucket", "quarter");
	g_assert_cmpuint(venture_report_result_get_row_count(quarters), ==, 1);
	g_assert_cmpstr(text(quarters, 0, "bucket"), ==, "2026-Q1");
	ASSERT_MONEY(quarters, 0, "value", "10.00 USD");

	march = RUN(fixture, "2026-03", "type", "sale", "measure", "gross",
	            "date_field", "occurred_at");
	g_assert_cmpuint(venture_report_result_get_row_count(march), ==, 1);
	ASSERT_MONEY(march, 0, "value", "4.00 USD");

	/* ISO weeks start on Monday: 28 Feb and 1 Mar 2026 share a week. */
	weeks = RUN(fixture, "2026-02-23..2026-03-01", "type", "sale",
	            "measure", "gross", "date_field", "occurred_at", "bucket", "week");
	g_assert_cmpuint(venture_report_result_get_row_count(weeks), ==, 1);
	g_assert_cmpstr(text(weeks, 0, "bucket"), ==, "2026-W09");
	ASSERT_MONEY(weeks, 0, "value", "7.00 USD");
}

/*
 * The filter is the list page's query string. Paging it would total a
 * page, and a sensitive field is not a thing to filter by one guess at a
 * time. What breaks: `limit=1` in a filter quietly totalling one record.
 */
static void
test_filter(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(VentureEntity) item = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureReportResult) refused = NULL;

	(void)user_data;

	item = product(fixture, "Thing", 0);
	sale(fixture, ID(item), "etsy", "1.00 USD", "2026-03-02");
	sale(fixture, ID(item), "etsy", "2.00 USD", "2026-03-03");
	sale(fixture, ID(item), "shop", "4.00 USD", "2026-03-04");

	result = RUN(fixture, "2026-03", "type", "sale", "measure", "gross",
	             "filter", "channel=etsy", "date_field", "occurred_at");
	g_assert_cmpuint(venture_report_result_get_row_count(result), ==, 1);
	ASSERT_MONEY(result, 0, "value", "3.00 USD");

	g_clear_object(&result);
	result = RUN(fixture, "2026-03", "type", "sale", "measure", "gross",
	             "filter", "channel__in=etsy,shop&quantity__gte=1",
	             "date_field", "occurred_at");
	ASSERT_MONEY(result, 0, "value", "7.00 USD");

	refused = run(fixture, "2026-03", &error, "type", "sale", "measure", "gross",
	              "filter", "channel=etsy&limit=1", NULL);
	g_assert_null(refused);
	assert_refused(error, VENTURE_ERROR_INVALID_ARGUMENT, "limit");
	g_clear_error(&error);

	refused = run(fixture, "2026-03", &error, "type", "sale",
	              "filter", "moonshine=1", NULL);
	g_assert_null(refused);
	g_assert_nonnull(error);
	g_clear_error(&error);

	refused = run(fixture, "all", &error, "type", "activity",
	              "filter", "call_request_hash=abc", NULL);
	g_assert_null(refused);
	assert_refused(error, VENTURE_ERROR_PERMISSION_DENIED, "sensitive");
}

/*
 * A rate divides by the time elapsed in the row's window. February 2026
 * has 28 days, so 28.00 of sales is 1.00 a day; per hour it is 28.00 over
 * 672 hours, 0.041666..., which rounds half to even to 0.04 -- in money,
 * never through a double. What breaks: "per day" over a month quietly
 * divided by thirty.
 */
static void
test_per_day_rate(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureReportResult) daily = NULL;
	g_autoptr(VentureReportResult) hourly = NULL;
	g_autoptr(VentureReportResult) bucketed = NULL;
	g_autoptr(VentureReportResult) counted = NULL;
	g_autoptr(VentureEntity) item = NULL;

	(void)user_data;

	item = product(fixture, "Thing", 0);
	sale(fixture, ID(item), "shop", "20.00 USD", "2026-02-03");
	sale(fixture, ID(item), "shop", "8.00 USD", "2026-02-20");
	sale(fixture, ID(item), "shop", "31.00 USD", "2026-03-10");

	daily = RUN(fixture, "2026-02", "type", "sale", "measure", "gross",
	            "date_field", "occurred_at", "per", "day");
	ASSERT_MONEY(daily, 0, "rate", "1.00 USD");

	hourly = RUN(fixture, "2026-02", "type", "sale", "measure", "gross",
	             "date_field", "occurred_at", "per", "hour");
	ASSERT_MONEY(hourly, 0, "rate", "0.04 USD");

	/* With buckets, each bucket is its own window: March has 31 days. */
	bucketed = RUN(fixture, "2026-02-01..2026-03-31", "type", "sale",
	               "measure", "gross", "date_field", "occurred_at",
	               "bucket", "month", "per", "day");
	g_assert_cmpuint(venture_report_result_get_row_count(bucketed), ==, 2);
	ASSERT_MONEY(bucketed, 0, "rate", "1.00 USD");
	ASSERT_MONEY(bucketed, 1, "rate", "1.00 USD");

	/* A count has a rate too. */
	counted = RUN(fixture, "2026-02", "type", "sale", "date_field",
	              "occurred_at", "per", "day");
	g_assert_cmpfloat_with_epsilon(number(counted, 0, "rate"), 2.0 / 28.0, 1e-9);
}

/*
 * Every malformed question is refused before a row is read, with a
 * message that says what to change.
 */
static void
test_refusals(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	static const struct
	{
		const gchar	*period;
		gint		 code;
		const gchar	*fragment;
		const gchar	*options[12];
	} cases[] = {
		{ "all", VENTURE_ERROR_INVALID_ARGUMENT, "needs a type", { NULL } },
		{ "all", VENTURE_ERROR_NOT_FOUND, "unicorn", { "type", "unicorn", NULL } },
		{ "all", VENTURE_ERROR_INVALID_ARGUMENT, "moonshine",
		  { "type", "sale", "measure", "moonshine", NULL } },
		{ "all", VENTURE_ERROR_INVALID_ARGUMENT, "needs a measure that is money",
		  { "type", "sale", "measure", "channel", "aggregate", "sum", NULL } },
		{ "all", VENTURE_ERROR_INVALID_ARGUMENT, "needs a measure that is money",
		  { "type", "sale", "aggregate", "avg", NULL } },
		{ "all", VENTURE_ERROR_INVALID_ARGUMENT, "not one of sum",
		  { "type", "sale", "aggregate", "median", NULL } },
		{ "all", VENTURE_ERROR_INVALID_ARGUMENT, "count_distinct needs a measure",
		  { "type", "sale", "aggregate", "count_distinct", NULL } },
		{ "all", VENTURE_ERROR_INVALID_ARGUMENT, "at most 3",
		  { "type", "sale", "group_by", "channel,country,product_id,venture_id", NULL } },
		{ "all", VENTURE_ERROR_INVALID_ARGUMENT, "not one of day",
		  { "type", "sale", "date_field", "occurred_at", "bucket", "fortnight", NULL } },
		{ "all", VENTURE_ERROR_INVALID_ARGUMENT, "bucket needs a date_field",
		  { "type", "sale", "bucket", "month", NULL } },
		{ "all", VENTURE_ERROR_INVALID_ARGUMENT, "not a declared date",
		  { "type", "sale", "date_field", "channel", NULL } },
		{ "all", VENTURE_ERROR_INVALID_ARGUMENT, "not hour or day",
		  { "type", "sale", "per", "week", NULL } },
		{ "all", VENTURE_ERROR_INVALID_ARGUMENT, "all time has no length",
		  { "type", "sale", "per", "day", NULL } },
		{ "2026-03", VENTURE_ERROR_INVALID_ARGUMENT, "rate of a sum or a count",
		  { "type", "sale", "measure", "gross", "aggregate", "avg", "per", "day", NULL } },
		{ "all", VENTURE_ERROR_INVALID_ARGUMENT, "category_depth rolls up",
		  { "type", "sale", "group_by", "channel", "category_depth", "1", NULL } },
		{ "all", VENTURE_ERROR_INVALID_ARGUMENT, "is not a reference",
		  { "type", "sale", "group_by", "channel.name", NULL } },
		{ "all", VENTURE_ERROR_INVALID_ARGUMENT, "long text",
		  { "type", "sale", "group_by", "notes", NULL } },
		{ "all", VENTURE_ERROR_PERMISSION_DENIED, "not business data",
		  { "type", "user", NULL } },
		{ "all", VENTURE_ERROR_PERMISSION_DENIED, "not business data",
		  { "type", "api_token", NULL } },
		{ "all", VENTURE_ERROR_PERMISSION_DENIED, "sensitive",
		  { "type", "activity", "group_by", "call_request_hash", NULL } }
	};
	VentureReport *report;
	gsize i;

	(void)user_data;

	report = venture_report_registry_lookup(
		venture_context_get_report_registry(fixture->context), "aggregate");
	g_assert_nonnull(report);

	for (i = 0; i < G_N_ELEMENTS(cases); i++)
	{
		g_autoptr(JsonObject) options = NULL;
		g_autoptr(VentureDateRange) range = NULL;
		g_autoptr(VentureReportResult) result = NULL;
		g_autoptr(GError) error = NULL;
		gsize j;

		options = json_object_new();

		for (j = 0; NULL != cases[i].options[j]; j += 2)
		{
			if (0 == g_strcmp0(cases[i].options[j], "category_depth"))
				json_object_set_int_member(options, cases[i].options[j],
					g_ascii_strtoll(cases[i].options[j + 1], NULL, 10));
			else
				json_object_set_string_member(options, cases[i].options[j],
				                              cases[i].options[j + 1]);
		}

		range = venture_context_parse_period(fixture->context, cases[i].period, NULL);
		result = venture_report_generate(report, fixture->context, range,
		                                 options, &error);
		g_assert_null(result);
		assert_refused(error, cases[i].code, cases[i].fragment);
	}
}

/*
 * Another organisation's rows are not in this one's totals, and naming
 * that organisation reads only its own. What breaks: one legal entity's
 * revenue inside another's report.
 */
static void
test_organization_scope(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureOrganization) other = NULL;
	g_autoptr(VentureVenture) venture = NULL;
	g_autoptr(VentureReportResult) mine = NULL;
	g_autoptr(VentureReportResult) theirs = NULL;
	g_autoptr(VentureEntity) item = NULL;
	g_autofree gchar *other_id = NULL;

	(void)user_data;

	item = product(fixture, "Thing", 0);
	sale(fixture, ID(item), "shop", "5.00 USD", "2026-03-02");

	other = venture_organization_new();
	g_object_set(other, "name", "Elsewhere", "slug", "elsewhere", NULL);
	save(fixture, other);
	venture = venture_venture_new();
	g_object_set(venture, "name", "Their shop", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(venture), ID(other));
	save(fixture, venture);
	sale_full(fixture, ID(other), ID(venture), 0, "shop", "70.00 USD",
	          "2026-03-02", 1);

	mine = RUN(fixture, "2026-03", "type", "sale", "measure", "gross",
	           "date_field", "occurred_at");
	g_assert_cmpuint(venture_report_result_get_row_count(mine), ==, 1);
	ASSERT_MONEY(mine, 0, "value", "5.00 USD");

	other_id = g_strdup_printf("%" G_GINT64_FORMAT, ID(other));
	theirs = RUN(fixture, "2026-03", "type", "sale", "measure", "gross",
	             "date_field", "occurred_at", "organization_id", other_id);
	g_assert_cmpuint(venture_report_result_get_row_count(theirs), ==, 1);
	ASSERT_MONEY(theirs, 0, "value", "70.00 USD");
}

/*
 * The report belongs to core and is never hidden; a type whose module is
 * off is refused with the switch named. What breaks: sales switched off
 * and still totalled through the back door, or the report vanishing with
 * whichever module happened to own it.
 */
static void
test_module_off(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(GError) error = NULL;
	VentureModule *core;

	(void)user_data;

	core = venture_module_registry_lookup(venture_context_get_modules(fixture->context),
	                                      "core");
	g_assert_nonnull(core);
	g_assert_true(g_strv_contains(venture_module_get_reports(core), "aggregate"));

	venture_config_set_module_enabled(fixture->config, "sales", FALSE);
	g_assert_false(venture_context_module_enabled(fixture->context, "sales"));
	g_assert_nonnull(venture_report_registry_lookup(
		venture_context_get_report_registry(fixture->context), "aggregate"));

	result = run(fixture, "all", &error, "type", "sale", NULL);
	g_assert_null(result);
	g_assert_nonnull(error);
	g_assert_nonnull(strstr(error->message, "sales"));
}

/*
 * The notes say what the figures leave out: a period that bounded
 * nothing, and records with no amount.
 */
static void
test_notes_and_description(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(JsonNode) json = NULL;
	g_autoptr(JsonNode) parameters = NULL;
	g_autofree gchar *rendered = NULL;
	g_autoptr(VentureEntity) item = NULL;
	JsonObject *properties;
	VentureReport *report;

	(void)user_data;

	item = product(fixture, "Thing", 0);
	sale(fixture, ID(item), "shop", "5.00 USD", "2026-03-02");
	sale(fixture, ID(item), "shop", NULL, "2026-03-03");

	result = RUN(fixture, "2026-03", "type", "sale", "measure", "gross");
	json = venture_report_result_to_json(result);
	rendered = venture_json_to_string(json, FALSE);
	g_assert_nonnull(strstr(rendered, "No date_field was named"));
	g_assert_nonnull(strstr(rendered, "1 record had no gross"));

	/* The schema the assistant and the CLI read names every option. */
	report = venture_report_registry_lookup(
		venture_context_get_report_registry(fixture->context), "aggregate");
	parameters = venture_report_describe_parameters(report);
	g_assert_nonnull(parameters);
	properties = json_object_get_object_member(json_node_get_object(parameters),
	                                           "properties");
	g_assert_true(json_object_has_member(properties, "type"));
	g_assert_true(json_object_has_member(properties, "measure"));
	g_assert_true(json_object_has_member(properties, "aggregate"));
	g_assert_true(json_object_has_member(properties, "group_by"));
	g_assert_true(json_object_has_member(properties, "category_depth"));
	g_assert_true(json_object_has_member(properties, "date_field"));
	g_assert_true(json_object_has_member(properties, "bucket"));
	g_assert_true(json_object_has_member(properties, "filter"));
	g_assert_true(json_object_has_member(properties, "per"));
}

/* --- Over HTTP ------------------------------------------------------------ */

typedef struct
{
	VentureConfig		*config;
	VentureDatabase		*database;
	VentureContext		*context;
	VentureWebServer	*server;
	SoupSession		*session;
	gchar			*state_dir;
	gchar			*cookie;
	guint16			 port;
} ServerFixture;

typedef struct
{
	gboolean	 done;
	GBytes		*body;
	GError		*error;
} RequestResult;

static void
request_done(
	GObject		*source,
	GAsyncResult	*result,
	gpointer	 user_data
){
	RequestResult *outcome;

	outcome = user_data;
	outcome->body = soup_session_send_and_read_finish(SOUP_SESSION(source),
	                                                  result, &outcome->error);
	outcome->done = TRUE;
}

static guint
server_request_full(
	ServerFixture	 *fixture,
	const gchar	 *method,
	const gchar	 *path,
	const gchar	 *content_type,
	const gchar	 *body,
	gboolean	  with_cookie,
	gchar		**out_body,
	gchar		**out_location
){
	g_autoptr(SoupMessage) message = NULL;
	g_autofree gchar *url = NULL;
	RequestResult outcome = { FALSE, NULL, NULL };

	url = g_strdup_printf("http://127.0.0.1:%u%s", fixture->port, path);
	message = soup_message_new(method, url);
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);

	if (with_cookie && (NULL != fixture->cookie))
		soup_message_headers_append(
			soup_message_get_request_headers(message), "Cookie",
			fixture->cookie);

	if (NULL != body)
	{
		g_autoptr(GBytes) bytes = NULL;

		bytes = g_bytes_new(body, strlen(body));
		soup_message_set_request_body_from_bytes(message, content_type, bytes);
	}

	soup_session_send_and_read_async(fixture->session, message,
	                                 G_PRIORITY_DEFAULT, NULL, request_done,
	                                 &outcome);

	while (!outcome.done)
		g_main_context_iteration(NULL, TRUE);

	if (NULL != outcome.error)
		g_error("%s %s: %s", method, path, outcome.error->message);

	if (NULL != out_body)
		*out_body = g_strndup(g_bytes_get_data(outcome.body, NULL),
		                      g_bytes_get_size(outcome.body));

	if (NULL != out_location)
		*out_location = g_strdup(soup_message_headers_get_one(
			soup_message_get_response_headers(message), "Location"));

	g_clear_pointer(&outcome.body, g_bytes_unref);
	g_clear_error(&outcome.error);

	return soup_message_get_status(message);
}

static guint
server_get(
	ServerFixture	 *fixture,
	const gchar	 *path,
	gchar		**out_body
){
	return server_request_full(fixture, "GET", path, NULL, NULL, TRUE,
	                           out_body, NULL);
}

static void
server_fixture_set_up(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureUser) user = NULL;
	g_autofree gchar *set_cookie = NULL;
	gchar *semicolon;

	(void)user_data;

	g_setenv("VENTURE_TEST_SESSION_SECRET", "aggregate-test-secret", TRUE);

	fixture->state_dir = g_dir_make_tmp("venture-aggregate-XXXXXX", NULL);
	fixture->port = (guint16)(20000 + ((getpid() + 7919) % 20000));

	fixture->config = venture_config_new();
	g_object_set(fixture->config,
	             "state-dir", fixture->state_dir,
	             "server-bind-address", "127.0.0.1",
	             "server-port", (gint64)fixture->port,
	             "security-session-secret-env", "VENTURE_TEST_SESSION_SECRET",
	             "security-password-iterations", (gint64)100000,
	             NULL);

	fixture->database = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	fixture->context = venture_context_new(fixture->config, fixture->database);

	g_assert_true(venture_database_migrate(fixture->database,
		venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);

	fixture->server = venture_web_server_new(fixture->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(fixture->server, &error));
	g_assert_no_error(error);

	fixture->session = soup_session_new();

	user = venture_user_new();
	g_object_set(user, "username", "owner", "role", VENTURE_USER_ROLE_OWNER,
	             "active", TRUE, NULL);
	g_assert_true(venture_user_set_password(user, "owner-password-1", 100000,
	                                        NULL));
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(user), NULL, NULL));

	{
		g_autoptr(SoupMessage) message = NULL;
		g_autofree gchar *url = NULL;
		g_autoptr(GBytes) bytes = NULL;
		RequestResult outcome = { FALSE, NULL, NULL };

		url = g_strdup_printf("http://127.0.0.1:%u/login", fixture->port);
		message = soup_message_new("POST", url);
		soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
		bytes = g_bytes_new_static("username=owner&password=owner-password-1",
		                           strlen("username=owner&password=owner-password-1"));
		soup_message_set_request_body_from_bytes(message,
			"application/x-www-form-urlencoded", bytes);
		soup_session_send_and_read_async(fixture->session, message,
		                                 G_PRIORITY_DEFAULT, NULL,
		                                 request_done, &outcome);

		while (!outcome.done)
			g_main_context_iteration(NULL, TRUE);

		g_clear_pointer(&outcome.body, g_bytes_unref);
		g_clear_error(&outcome.error);

		set_cookie = g_strdup(soup_message_headers_get_one(
			soup_message_get_response_headers(message), "Set-Cookie"));
	}

	g_assert_nonnull(set_cookie);
	semicolon = strchr(set_cookie, ';');

	if (NULL != semicolon)
		*semicolon = '\0';

	fixture->cookie = g_steal_pointer(&set_cookie);
}

static void
server_fixture_tear_down(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureConfig) everything = NULL;
	g_autoptr(VentureModuleRegistry) registry = NULL;

	(void)user_data;

	if (NULL != fixture->server)
		venture_web_server_stop(fixture->server);

	g_clear_pointer(&fixture->cookie, g_free);
	g_clear_object(&fixture->session);
	g_clear_object(&fixture->server);
	g_clear_object(&fixture->context);
	g_clear_object(&fixture->database);
	g_clear_object(&fixture->config);

	if (NULL != fixture->state_dir)
	{
		venture_test_remove_tree(fixture->state_dir);
		g_clear_pointer(&fixture->state_dir, g_free);
	}

	g_unsetenv("VENTURE_TEST_SESSION_SECRET");

	everything = venture_config_new();
	registry = venture_module_registry_new();
	venture_module_registry_register_builtins(registry);
	venture_module_registry_configure(registry, everything, NULL);
	venture_module_registry_apply(registry,
	                              venture_entity_registry_get_default());
}


/*
 * Every door forwards the options. The API is where the CLI, the MCP tool
 * and the page all arrive; an option missing from its list is silently
 * absent and the report answers a different question. The page, asked
 * with no type, offers the form above the refusal rather than an error
 * page with nowhere to go.
 */
static void
test_http(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureVenture) venture = NULL;
	g_autofree gchar *body = NULL;
	g_autoptr(JsonNode) node = NULL;
	JsonArray *rows;
	gint64 organization_id;
	guint i;

	(void)user_data;

	organization_id = venture_context_get_default_organization_id(fixture->context);
	venture = venture_venture_new();
	g_object_set(venture, "name", "Shop", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(venture), organization_id);
	g_assert_true(venture_database_save(fixture->database, VENTURE_ENTITY(venture),
	                                    NULL, NULL));

	for (i = 0; i < 3; i++)
	{
		g_autoptr(VentureSale) record = NULL;
		g_autoptr(VentureMoney) gross = NULL;
		g_autoptr(GDateTime) when = NULL;

		gross = venture_money_new(100 * (i + 1), "USD", 2);
		when = g_date_time_new_utc(2026, 1 + i, 10, 0, 0, 0);
		record = venture_sale_new();
		g_object_set(record, "venture-id", venture_entity_get_id(VENTURE_ENTITY(venture)),
		             "gross", gross, "occurred-at", when, "quantity", (gint64)1,
		             "channel", (i < 2) ? "etsy" : "shop", NULL);
		venture_entity_set_organization_id(VENTURE_ENTITY(record), organization_id);
		g_assert_true(venture_database_save(fixture->database,
		                                    VENTURE_ENTITY(record), NULL, NULL));
	}

	g_assert_cmpuint(server_get(fixture,
		"/api/v1/reports/aggregate?period=2026&type=sale&measure=gross"
		"&aggregate=sum&group_by=channel&date_field=occurred_at&bucket=quarter"
		"&filter=quantity__gte%3D1&per=day", &body), ==, SOUP_STATUS_OK);
	node = venture_json_parse(body, NULL);
	g_assert_nonnull(node);
	rows = json_object_get_array_member(json_node_get_object(node), "rows");
	g_assert_cmpuint(json_array_get_length(rows), ==, 2);
	g_assert_cmpstr(json_object_get_string_member(
		json_array_get_object_element(rows, 0), "bucket"), ==, "2026-Q1");
	g_assert_cmpstr(json_object_get_string_member(
		json_array_get_object_element(rows, 0), "channel"), ==, "etsy");
	g_assert_true(json_object_has_member(json_array_get_object_element(rows, 0),
	                                     "rate"));

	/* A refusal is a 400 naming the problem, not a 500. */
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(server_get(fixture,
		"/api/v1/reports/aggregate?period=2026&type=sale&group_by=channel,country,product_id,venture_id",
		&body), ==, 400);
	g_assert_nonnull(strstr(body, "at most 3"));

	/* A personal type is forbidden. */
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(server_get(fixture,
		"/api/v1/reports/aggregate?period=all&type=user", &body), ==, 403);

	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(server_get(fixture, "/reports/aggregate", &body), ==, 400);
	g_assert_nonnull(strstr(body, "name=\"group_by\""));
	g_assert_nonnull(strstr(body, "needs a type"));

	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(server_get(fixture,
		"/reports/aggregate?period=2026&type=sale&measure=gross&date_field=occurred_at",
		&body), ==, SOUP_STATUS_OK);
	g_assert_nonnull(strstr(body, "$6.00"));
	g_assert_nonnull(strstr(body, "value=\"occurred_at\""));
}

int
main(
	int	 argc,
	char	**argv
){
	g_test_init(&argc, &argv, NULL);

#define ADD(path, func) \
	g_test_add(path, Fixture, NULL, fixture_set_up, func, fixture_tear_down)

	ADD("/aggregate/sum-per-currency", test_sum_per_currency);
	ADD("/aggregate/avg-money-half-even", test_avg_money_half_even);
	ADD("/aggregate/count-and-distinct", test_count_and_distinct);
	ADD("/aggregate/group-by-kinds", test_group_by_kinds);
	ADD("/aggregate/group-by-denominated-money", test_group_by_denominated_money);
	ADD("/aggregate/date-buckets-west-of-utc", test_date_buckets_west_of_utc);
	ADD("/aggregate/filter", test_filter);
	ADD("/aggregate/per-day-rate", test_per_day_rate);
	ADD("/aggregate/refusals", test_refusals);
	ADD("/aggregate/organization-scope", test_organization_scope);
	ADD("/aggregate/module-off", test_module_off);
	ADD("/aggregate/notes-and-description", test_notes_and_description);

#undef ADD

	g_test_add("/aggregate/http", ServerFixture, NULL, server_fixture_set_up,
	           test_http, server_fixture_tear_down);

	return g_test_run();
}
