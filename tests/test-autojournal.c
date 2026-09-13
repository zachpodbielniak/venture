/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>

typedef struct { VentureDatabase *db; VentureConfig *config; VentureContext *context; gint64 org; gint64 venture; } Fixture;
static void
setup(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureVenture) venture = venture_venture_new();
	(void)data;
	f->db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	f->config = venture_config_new();
	f->context = venture_context_new(f->config, f->db);
	f->org = venture_context_get_default_organization_id(f->context);
	g_object_set(venture, "name", "Autojournal", "venture-type", "books", "organization-id", f->org, NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(venture), NULL, &error));
	g_assert_no_error(error);
	f->venture = venture_entity_get_id(VENTURE_ENTITY(venture));
}
static void
teardown(Fixture *f, gconstpointer data)
{
	(void)data;
	g_clear_object(&f->context); g_clear_object(&f->config); g_clear_object(&f->db);
}
static void
money(gpointer object, const gchar *field, gint64 cents)
{
	g_autoptr(VentureMoney) amount = venture_money_new_for_currency(cents, "USD");
	g_object_set(object, field, amount, NULL);
}
static VentureSale *
sale(Fixture *f)
{
	VentureSale *record = venture_sale_new();
	g_autoptr(GDateTime) when = g_date_time_new_from_iso8601("2026-01-10T00:00:00Z", NULL);
	g_object_set(record, "organization-id", f->org, "venture-id", f->venture, "occurred-at", when, NULL);
	money(record, "gross", 10000);
	return record;
}
static GPtrArray *
find(Fixture *f, GType type)
{
	g_autoptr(VentureQuery) query = venture_query_new(type);
	g_autoptr(GError) error = NULL;
	GPtrArray *rows;
	venture_query_set_organization(query, f->org);
	venture_query_set_limit(query, 0);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	rows = venture_database_find(f->db, query, &error);
	g_assert_no_error(error);
	return rows;
}
static void
profile(Fixture *f, gconstpointer data)
{
	GType type = venture_entity_registry_lookup(venture_entity_registry_get_default(), "posting_profile");
	g_autoptr(VentureSale) record = sale(f);
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GError) error = NULL;
	(void)data;
	g_assert_cmpuint(type, !=, 0);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(record), NULL, &error));
	g_assert_no_error(error);
	rows = find(f, type);
	g_assert_cmpuint(rows->len, ==, 1);
	g_clear_pointer(&rows, g_ptr_array_unref);
	g_object_set(record, "notes", "Metadata only", NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(record), NULL, &error));
	rows = find(f, type);
	g_assert_cmpuint(rows->len, ==, 1);
}
static void
legs(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureSale) record = sale(f);
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GError) error = NULL;
	(void)data;
	money(record, "discount", 500); money(record, "fees", 300);
	money(record, "tax-collected", 800); money(record, "shipping-collected", 1000);
	money(record, "shipping-cost", 600); money(record, "tax-remitted", 200);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(record), NULL, &error));
	g_assert_no_error(error);
	rows = find(f, VENTURE_TYPE_JOURNAL_LINE);
	/* Remittance is its own payable/cash pair; fees never net against sales. */
	g_assert_cmpuint(rows->len, ==, 9);
}
static void
refund_test(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureSale) record = sale(f);
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) journals = NULL;
	g_autoptr(GDateTime) when = g_date_time_new_from_iso8601("2026-02-10T00:00:00Z", NULL);
	g_autoptr(GDateTime) actual = NULL;
	g_autofree gchar *rule = NULL;
	(void)data;
	money(record, "refunded", 1500);
	/* Baseline has no date property, but must still fail on the missing journal. */
	if (g_object_class_find_property(G_OBJECT_GET_CLASS(record), "refunded-at") != NULL)
		g_object_set(record, "refunded-at", when, NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(record), NULL, &error));
	g_assert_no_error(error);
	journals = find(f, VENTURE_TYPE_JOURNAL);
	g_assert_cmpuint(journals->len, ==, 2);
	g_object_get(g_ptr_array_index(journals, 1), "rule-name", &rule, "occurred-at", &actual, NULL);
	g_assert_cmpstr(rule, ==, "sale_refund");
	g_assert_true(g_date_time_equal(actual, when));
	g_clear_pointer(&journals, g_ptr_array_unref);
	g_object_set(record, "notes", "Unchanged refund", NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(record), NULL, &error));
	journals = find(f, VENTURE_TYPE_JOURNAL);
	g_assert_cmpuint(journals->len, ==, 2);
	g_clear_pointer(&journals, g_ptr_array_unref);
	money(record, "refunded", 2000);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(record), NULL, &error));
	g_assert_no_error(error);
	journals = find(f, VENTURE_TYPE_JOURNAL);
	g_assert_cmpuint(journals->len, ==, 4);
}
static gint64
balance(Fixture *f, const gchar *code)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ACCOUNT);
	g_autoptr(VentureEntity) account = NULL;
	g_autoptr(VentureMoney) value = NULL;
	g_autoptr(GDateTime) when = g_date_time_new_from_iso8601("2026-12-31T00:00:00Z", NULL);
	g_autoptr(GError) error = NULL;
	venture_query_set_organization(query, f->org);
	venture_query_add_filter_string(query, "code", VENTURE_FILTER_OP_EQ, code, NULL);
	account = venture_database_find_one(f->db, query, &error);
	g_assert_no_error(error); g_assert_nonnull(account);
	value = venture_posting_service_account_balance(venture_database_get_posting_service(f->db),
		venture_entity_get_id(account), f->org, "USD", when, &error);
	g_assert_no_error(error); g_assert_nonnull(value);
	return value->amount;
}
static void
remittance(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureSale) record = sale(f);
	g_autoptr(GError) error = NULL;
	(void)data;
	money(record, "tax-collected", 800);
	money(record, "tax-remitted", 300);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(record), NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpint(balance(f, "4000"), ==, -10000);
	g_assert_cmpint(balance(f, "2100"), ==, -500);
	g_assert_cmpint(balance(f, "1000"), ==, 10500);
}
static void
expense_test(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureExpense) record = venture_expense_new();
	g_autoptr(GError) error = NULL;
	(void)data;
	g_object_set(record, "organization-id", f->org, "description", "Advertisement", "category", "ADVERTISING", "payment-method", "credit", NULL);
	money(record, "amount", 1200);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(record), NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpint(balance(f, "6200"), ==, 1200);
	g_assert_cmpint(balance(f, "2000"), ==, -1200);
	g_assert_cmpint(balance(f, "1000"), ==, 0);
}
static void
unposted_test(Fixture *f, gconstpointer data)
{
	VentureReport *report = venture_report_registry_lookup(venture_context_get_report_registry(f->context), "unposted");
	(void)data;
	g_assert_nonnull(report);
}
int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/autojournal/profile", Fixture, NULL, setup, profile, teardown);
	g_test_add("/autojournal/legs", Fixture, NULL, setup, legs, teardown);
	g_test_add("/autojournal/refund", Fixture, NULL, setup, refund_test, teardown);
	g_test_add("/autojournal/remittance", Fixture, NULL, setup, remittance, teardown);
	g_test_add("/autojournal/expense", Fixture, NULL, setup, expense_test, teardown);
	g_test_add("/autojournal/unposted", Fixture, NULL, setup, unposted_test, teardown);
	return g_test_run();
}
