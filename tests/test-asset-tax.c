/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include "venture-test-util.h"

typedef struct
{
	VentureDatabase *db;
	VentureConfig *config;
	VentureContext *context;
	gint64 org;
} Fixture;

static void
setup(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	(void)data;
	f->config = venture_config_new();
	f->db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->db);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->org = venture_context_get_default_organization_id(f->context);
}

static void
teardown(Fixture *f, gconstpointer data)
{
	(void)data;
	g_clear_object(&f->context);
	g_clear_object(&f->db);
	g_clear_object(&f->config);
}

static void
save(Fixture *f, VentureEntity *entity)
{
	g_autoptr(GError) error = NULL;
	g_assert_true(venture_database_save(f->db, entity, NULL, &error));
	g_assert_no_error(error);
}

static gint64
account(Fixture *f, const gchar *code, VentureAccountKind kind)
{
	g_autoptr(VentureEntity) entity = VENTURE_ENTITY(venture_account_new());
	g_object_set(entity, "organization-id", f->org, "name", code, "code", code,
		"kind", kind, "active", TRUE, NULL);
	save(f, entity);
	return venture_entity_get_id(entity);
}

static VentureEntity *
asset(Fixture *f, const gchar *tag)
{
	VentureEntity *entity = VENTURE_ENTITY(venture_fixed_asset_new());
	g_autoptr(VentureMoney) cost = venture_money_new_for_currency(120000, "USD");
	g_autoptr(VentureMoney) salvage = venture_money_new_zero("USD");
	g_autoptr(GDateTime) date = g_date_time_new_utc(2026, 1, 1, 0, 0, 0);
	g_object_set(entity, "organization-id", f->org, "name", "Van", "tag", tag,
		"cost", cost, "salvage-value", salvage, "useful-life-months", (gint64)24,
		"tax-useful-life-months", (gint64)12, "tax-method", VENTURE_ASSET_METHOD_STRAIGHT_LINE,
		"tax-convention", VENTURE_ASSET_CONVENTION_HALF_YEAR,
		"acquired-at", date, "in-service-at", date,
		"asset-account-id", account(f, tag, VENTURE_ACCOUNT_KIND_ASSET),
		"accumulated-depreciation-account-id", account(f, "AD", VENTURE_ACCOUNT_KIND_ASSET),
		"depreciation-expense-account-id", account(f, "DE", VENTURE_ACCOUNT_KIND_EXPENSE), NULL);
	save(f, entity);
	return entity;
}

static void
place(Fixture *f, VentureEntity *entity)
{
	g_autoptr(GError) error = NULL;
	g_assert_true(venture_entity_set_field_from_string(entity, "operation", "place", &error));
	g_assert_no_error(error);
	save(f, entity);
}

static GPtrArray *
rows_of(Fixture *f, GType type)
{
	g_autoptr(VentureQuery) query = venture_query_new(type);
	g_autoptr(GError) error = NULL;
	GPtrArray *rows;
	venture_query_set_organization(query, f->org);
	venture_query_set_limit(query, 0);
	rows = venture_database_find(f->db, query, &error);
	g_assert_no_error(error);
	return rows;
}

static void
test_tax_schedule_separate(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) entity = asset(f, "VAN");
	g_autoptr(GPtrArray) book = NULL;
	g_autoptr(GPtrArray) tax = NULL;
	gint64 tax_sum = 0;
	guint i;
	(void)data;
	place(f, entity);
	book = rows_of(f, VENTURE_TYPE_DEPRECIATION_ENTRY);
	tax = rows_of(f, VENTURE_TYPE_TAX_DEPRECIATION_ENTRY);
	g_assert_cmpuint(book->len, ==, 24);
	g_assert_cmpuint(tax->len, ==, 12);
	for (i = 0; i < tax->len; i++)
	{
		g_autoptr(VentureMoney) amount = NULL;
		g_object_get(g_ptr_array_index(tax, i), "amount", &amount, NULL);
		tax_sum += amount->amount;
	}
	g_assert_cmpint(tax_sum, ==, 120000);
}

static void
test_tax_does_not_post_to_book(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) entity = asset(f, "BOOK");
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_JOURNAL);
	g_autoptr(GPtrArray) journals = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureReportResult) income = NULL;
	g_autoptr(JsonObject) options = json_object_new();
	g_autoptr(VentureDateRange) period = NULL;
	VentureReport *report;
	gboolean tax_book = FALSE;
	(void)data;
	place(f, entity);
	g_assert_cmpint(venture_asset_service_run_period(venture_asset_service_get(f->db),
		"2026-01", f->org, FALSE, NULL, &error), ==, 1);
	g_assert_no_error(error);
	venture_query_set_organization(query, f->org);
	journals = venture_database_find(f->db, query, &error);
	g_assert_cmpuint(journals->len, ==, 1);
	g_object_get(g_ptr_array_index(journals, 0), "tax-book", &tax_book, NULL);
	g_assert_false(tax_book);
	g_assert_cmpint(venture_asset_service_run_tax_period(venture_asset_service_get(f->db),
		"2026-01", f->org, FALSE, NULL, &error), ==, 1);
	g_assert_no_error(error);
	g_clear_pointer(&journals, g_ptr_array_unref);
	journals = venture_database_find(f->db, query, &error);
	g_assert_cmpuint(journals->len, ==, 2);
	{
		guint i;
		guint tax_books = 0;
		for (i = 0; i < journals->len; i++)
		{
			gboolean flagged = FALSE;
			g_autofree gchar *source = NULL;
			g_object_get(g_ptr_array_index(journals, i), "tax-book", &flagged, "source-type", &source, NULL);
			if (g_strcmp0(source, "tax_depreciation_entry") == 0)
			{
				g_assert_true(flagged);
				tax_books++;
			}
			else
				g_assert_false(flagged);
		}
		g_assert_cmpuint(tax_books, ==, 1);
	}
	report = venture_report_registry_lookup(venture_context_get_report_registry(f->context), "income_statement");
	period = venture_context_parse_period(f->context, "2026-01", &error);
	json_object_set_int_member(options, "organization_id", f->org);
	json_object_set_string_member(options, "currency", "USD");
	income = venture_report_generate(report, f->context, period, options, &error);
	g_assert_no_error(error);
	{
		guint i;
		gint64 expenses = 0;
		for (i = 0; i < venture_report_result_get_row_count(income); i++)
		{
			const GValue *key = venture_report_result_get_cell(income, i, "key");
			if (key != NULL && g_strcmp0(g_value_get_string(key), "expenses") == 0)
			{
				const GValue *value = venture_report_result_get_cell(income, i, "current");
				expenses = ((const VentureMoney *)g_value_get_boxed(value))->amount;
			}
		}
		g_assert_cmpint(expenses, ==, 5000);
	}
}

static void
test_declining_tax(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) entity = asset(f, "DBL");
	g_autoptr(GPtrArray) tax = NULL;
	gint64 sum = 0;
	guint i;
	(void)data;
	g_object_set(entity, "tax-method", VENTURE_ASSET_METHOD_DECLINING_BALANCE,
		"tax-convention", VENTURE_ASSET_CONVENTION_FULL_MONTH, NULL);
	save(f, entity);
	place(f, entity);
	tax = rows_of(f, VENTURE_TYPE_TAX_DEPRECIATION_ENTRY);
	g_assert_cmpuint(tax->len, ==, 12);
	for (i = 0; i < tax->len; i++)
	{
		g_autoptr(VentureMoney) amount = NULL;
		g_object_get(g_ptr_array_index(tax, i), "amount", &amount, NULL);
		sum += amount->amount;
	}
	g_assert_cmpint(sum, ==, 120000);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/assets/tax-schedule", Fixture, NULL, setup, test_tax_schedule_separate, teardown);
	g_test_add("/assets/tax-book-flag", Fixture, NULL, setup, test_tax_does_not_post_to_book, teardown);
	g_test_add("/assets/tax-declining", Fixture, NULL, setup, test_declining_tax, teardown);
	return g_test_run();
}
