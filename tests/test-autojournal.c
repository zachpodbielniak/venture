/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <libsoup/soup.h>
#include "venture-test-util.h"

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
line_is(Fixture *f, VentureEntity *line, const gchar *code, VentureLedgerSide expected_side, gint64 cents)
{
	g_autoptr(VentureMoney) value = NULL;
	g_autoptr(VentureEntity) acct = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *actual_code = NULL;
	g_autofree gchar *scoped_code = g_strdup_printf("%" G_GINT64_FORMAT ":%s", f->org, code);
	gint64 id;
	VentureLedgerSide side;
	g_object_get(line, "account-id", &id, "side", &side, "amount", &value, NULL);
	acct = venture_database_get(f->db, VENTURE_TYPE_ACCOUNT, id, &error);
	g_assert_no_error(error); g_assert_nonnull(acct);
	g_object_get(acct, "code", &actual_code, NULL);
	g_assert_true(g_strcmp0(actual_code, code) == 0 || g_strcmp0(actual_code, scoped_code) == 0);
	g_assert_cmpint(side, ==, expected_side);
	g_assert_cmpint(value->amount, ==, cents);
}
static void
legs(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureSale) record = sale(f);
	g_autoptr(VentureProduct) product = venture_product_new();
	g_autoptr(GDateTime) refund_date = g_date_time_new_from_iso8601("2026-02-10T00:00:00Z", NULL);
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GError) error = NULL;
	(void)data;
	g_object_set(product, "name", "Complete sale stock", "organization-id", f->org, "venture-id", f->venture, NULL);
	money(product, "cost", 1234);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(product), NULL, &error));
	g_object_set(record, "product-id", venture_entity_get_id(VENTURE_ENTITY(product)), "quantity", (gint64)3,
		"refunded-at", refund_date, NULL);
	money(record, "refunded", 1500);
	money(record, "discount", 500); money(record, "fees", 300);
	money(record, "tax-collected", 800); money(record, "shipping-collected", 1000);
	money(record, "shipping-cost", 600); money(record, "tax-remitted", 200);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(record), NULL, &error));
	g_assert_no_error(error);
	rows = find(f, VENTURE_TYPE_JOURNAL_LINE);
	/* Remittance is its own payable/cash pair; fees never net against sales. */
	g_assert_cmpuint(rows->len, ==, 13);
	line_is(f, g_ptr_array_index(rows, 0), "6000", VENTURE_LEDGER_SIDE_DEBIT, 300);
	line_is(f, g_ptr_array_index(rows, 1), "4200", VENTURE_LEDGER_SIDE_DEBIT, 500);
	line_is(f, g_ptr_array_index(rows, 2), "6100", VENTURE_LEDGER_SIDE_DEBIT, 600);
	line_is(f, g_ptr_array_index(rows, 3), "4000", VENTURE_LEDGER_SIDE_CREDIT, 10000);
	line_is(f, g_ptr_array_index(rows, 4), "2100", VENTURE_LEDGER_SIDE_CREDIT, 800);
	line_is(f, g_ptr_array_index(rows, 5), "4100", VENTURE_LEDGER_SIDE_CREDIT, 1000);
	line_is(f, g_ptr_array_index(rows, 6), "1000", VENTURE_LEDGER_SIDE_DEBIT, 10400);
	line_is(f, g_ptr_array_index(rows, 7), "2100", VENTURE_LEDGER_SIDE_DEBIT, 200);
	line_is(f, g_ptr_array_index(rows, 8), "1000", VENTURE_LEDGER_SIDE_CREDIT, 200);
	line_is(f, g_ptr_array_index(rows, 9), "5000", VENTURE_LEDGER_SIDE_DEBIT, 3702);
	line_is(f, g_ptr_array_index(rows, 10), "1200", VENTURE_LEDGER_SIDE_CREDIT, 3702);
	line_is(f, g_ptr_array_index(rows, 11), "4300", VENTURE_LEDGER_SIDE_DEBIT, 1500);
	line_is(f, g_ptr_array_index(rows, 12), "1000", VENTURE_LEDGER_SIDE_CREDIT, 1500);
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
	VentureActor actor;
	(void)data;
	actor.kind = VENTURE_ACTOR_KIND_USER; actor.name = "refund-operator";
	actor.prompt = "Refund the buyer"; actor.request_id = "refund-request"; actor.approved_by = "reviewer";
	money(record, "refunded", 1500);
	/* Baseline has no date property, but must still fail on the missing journal. */
	if (g_object_class_find_property(G_OBJECT_GET_CLASS(record), "refunded-at") != NULL)
		g_object_set(record, "refunded-at", when, NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(record), &actor, &error));
	g_assert_no_error(error);
	journals = find(f, VENTURE_TYPE_JOURNAL);
	g_assert_cmpuint(journals->len, ==, 2);
	g_object_get(g_ptr_array_index(journals, 1), "rule-name", &rule, "occurred-at", &actual, NULL);
	g_assert_cmpstr(rule, ==, "sale_refund");
	g_assert_true(g_date_time_equal(actual, when));
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_AUDIT_ENTRY);
		g_autoptr(VentureEntity) audit = NULL;
		g_autofree gchar *name = NULL, *request = NULL, *approved = NULL;
		venture_query_set_organization(query, f->org);
		venture_query_add_filter_string(query, "target-type", VENTURE_FILTER_OP_EQ, "journal", NULL);
		venture_query_add_filter_int(query, "target-id", VENTURE_FILTER_OP_EQ,
			venture_entity_get_id(g_ptr_array_index(journals, 1)), NULL);
		audit = venture_database_find_one(f->db, query, &error);
		g_assert_no_error(error); g_assert_nonnull(audit);
		g_object_get(audit, "actor", &name, "request-id", &request, "approved-by", &approved, NULL);
		g_assert_cmpstr(name, ==, actor.name); g_assert_cmpstr(request, ==, actor.request_id);
		g_assert_cmpstr(approved, ==, actor.approved_by);
	}
	g_clear_pointer(&journals, g_ptr_array_unref);
	g_object_set(record, "notes", "Unchanged refund", NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(record), &actor, &error));
	journals = find(f, VENTURE_TYPE_JOURNAL);
	g_assert_cmpuint(journals->len, ==, 2);
	g_clear_pointer(&journals, g_ptr_array_unref);
	money(record, "refunded", 2000);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(record), &actor, &error));
	g_assert_no_error(error);
	journals = find(f, VENTURE_TYPE_JOURNAL);
	g_assert_cmpuint(journals->len, ==, 4);
	g_clear_pointer(&journals, g_ptr_array_unref);
	money(record, "refunded", 0);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(record), &actor, &error));
	g_assert_no_error(error);
	journals = find(f, VENTURE_TYPE_JOURNAL); g_assert_cmpuint(journals->len, ==, 5);
}
static gint64
balance(Fixture *f, const gchar *code)
{
	g_autofree gchar *scoped_code = g_strdup_printf("%" G_GINT64_FORMAT ":%s", f->org, code);
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ACCOUNT);
	g_autoptr(VentureEntity) account = NULL;
	g_autoptr(VentureMoney) value = NULL;
	g_autoptr(GDateTime) when = g_date_time_new_from_iso8601("2026-12-31T00:00:00Z", NULL);
	g_autoptr(GError) error = NULL;
	venture_query_set_organization(query, f->org);
	venture_query_add_filter_string(query, "code", VENTURE_FILTER_OP_EQ, code, NULL);
	account = venture_database_find_one(f->db, query, &error);
	if (account == NULL && error == NULL) {
		g_clear_object(&query);
		query = venture_query_new(VENTURE_TYPE_ACCOUNT);
		venture_query_set_organization(query, f->org);
		venture_query_add_filter_string(query, "code", VENTURE_FILTER_OP_EQ, scoped_code, NULL);
		account = venture_database_find_one(f->db, query, &error);
	}
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
	g_autoptr(VentureSale) record = sale(f);
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(GError) error = NULL;
	(void)data;
	g_assert_nonnull(report);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(record), NULL, &error));
	g_object_set(record, "notes", "Current version missing a journal", NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(record), NULL, &error));
	result = venture_report_generate(report, f->context, NULL, NULL, &error);
	g_assert_no_error(error); g_assert_nonnull(result);
	g_assert_cmpuint(venture_report_result_get_row_count(result), ==, 1);
}
static void
cogs_test(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureSale) record = sale(f);
	g_autoptr(VentureProduct) product = venture_product_new();
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GError) error = NULL;
	(void)data;
	g_object_set(product, "organization-id", f->org, "venture-id", f->venture, "name", "Stock", NULL);
	money(product, "cost", 1234);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(product), NULL, &error));
	g_object_set(record, "product-id", venture_entity_get_id(VENTURE_ENTITY(product)), "quantity", (gint64)3, NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(record), NULL, &error));
	g_assert_no_error(error);
	rows = find(f, VENTURE_TYPE_JOURNAL_LINE);
	g_assert_cmpuint(rows->len, ==, 4);
	line_is(f, g_ptr_array_index(rows, 2), "5000", VENTURE_LEDGER_SIDE_DEBIT, 3702);
	line_is(f, g_ptr_array_index(rows, 3), "1200", VENTURE_LEDGER_SIDE_CREDIT, 3702);
}
static void
refund_failure(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureSale) seed = sale(f), record = sale(f);
	g_autoptr(VentureEntity) account = NULL;
	g_autoptr(GPtrArray) profiles = NULL, journals = NULL, sales = NULL;
	g_autoptr(GError) error = NULL;
	gint64 id;
	GType type = venture_entity_registry_lookup(venture_entity_registry_get_default(), "posting_profile");
	(void)data;
	g_assert_cmpuint(type, !=, 0);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(seed), NULL, &error));
	profiles = find(f, type);
	g_object_get(g_ptr_array_index(profiles, 0), "refunds-account-id", &id, NULL);
	account = venture_database_get(f->db, VENTURE_TYPE_ACCOUNT, id, &error);
	g_assert_no_error(error);
	g_object_set(account, "active", FALSE, NULL);
	g_assert_true(venture_database_save(f->db, account, NULL, &error));
	money(record, "refunded", 1000);
	g_assert_false(venture_database_save(f->db, VENTURE_ENTITY(record), NULL, &error));
	g_assert_nonnull(error);
	g_assert_cmpint(venture_entity_get_id(VENTURE_ENTITY(record)), ==, 0);
	journals = find(f, VENTURE_TYPE_JOURNAL); sales = find(f, VENTURE_TYPE_SALE);
	g_assert_cmpuint(journals->len, ==, 1); g_assert_cmpuint(sales->len, ==, 1);
}
static void
backfill_test(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureSale) record = sale(f);
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) result = NULL;
	g_autoptr(GPtrArray) sources = NULL, journals = NULL;
	VentureAutojournalService *service = venture_database_get_autojournal_service(f->db);
	(void)data;
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(record), NULL, &error));
	g_object_set(record, "notes", "A new version without new financial legs", NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(record), NULL, &error));
	sources = venture_autojournal_service_unposted(service, f->org, &error);
	g_assert_no_error(error); g_assert_cmpuint(sources->len, ==, 1);
	result = venture_autojournal_service_backfill(service, f->org, TRUE, NULL, &error);
	g_assert_no_error(error); g_assert_nonnull(result);
	g_assert_cmpint(json_object_get_int_member(json_node_get_object(result), "posted"), ==, 1);
	journals = find(f, VENTURE_TYPE_JOURNAL); g_assert_cmpuint(journals->len, ==, 1);
	g_clear_pointer(&result, json_node_unref); g_clear_pointer(&journals, g_ptr_array_unref);
	result = venture_autojournal_service_backfill(service, f->org, FALSE, NULL, &error);
	g_assert_no_error(error); g_assert_nonnull(result);
	journals = find(f, VENTURE_TYPE_JOURNAL); g_assert_cmpuint(journals->len, ==, 3);
	g_clear_pointer(&sources, g_ptr_array_unref);
	sources = venture_autojournal_service_unposted(service, f->org, &error);
	g_assert_no_error(error); g_assert_cmpuint(sources->len, ==, 0);
	g_assert_cmpint(balance(f, "4000"), ==, -10000);
}
static void
profile_scope(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureOrganization) other = venture_organization_new();
	g_autoptr(VenturePostingProfile) first = NULL, second = NULL;
	g_autoptr(VentureEntity) duplicate = NULL;
	g_autoptr(GError) error = NULL;
	gint64 a, b;
	VentureAutojournalService *service = venture_database_get_autojournal_service(f->db);
	(void)data;
	g_object_set(other, "name", "Other books", NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(other), NULL, &error));
	first = venture_autojournal_service_profile(service, f->org, &error);
	second = venture_autojournal_service_profile(service, venture_entity_get_id(VENTURE_ENTITY(other)), &error);
	g_assert_no_error(error); g_assert_nonnull(first); g_assert_nonnull(second);
	g_object_get(first, "cash-account-id", &a, NULL); g_object_get(second, "cash-account-id", &b, NULL);
	g_assert_cmpint(a, !=, b);
	duplicate = venture_entity_duplicate(VENTURE_ENTITY(first));
	g_assert_false(venture_database_save(f->db, duplicate, NULL, &error));
	g_assert_nonnull(error);
	g_clear_error(&error);
	g_object_set(first, "sales-account-id", b, NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(first), NULL, &error));
	{
		g_autoptr(VentureSale) record = sale(f);
		g_assert_false(venture_database_save(f->db, VENTURE_ENTITY(record), NULL, &error));
		g_assert_nonnull(error);
	}
}
/* Switching on detailed posting must keep using the existing base chart. */
static void
profile_reuses_scoped_chart(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureOrganization) other = venture_organization_new();
	g_autoptr(VentureAccount) cash = venture_account_new();
	g_autoptr(VenturePostingProfile) profile_row = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *code = NULL;
	gint64 org, cash_id;
	(void)data;
	g_object_set(other, "name", "Existing scoped chart", NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(other), NULL, &error));
	org = venture_entity_get_id(VENTURE_ENTITY(other));
	code = g_strdup_printf("%" G_GINT64_FORMAT ":1000", org);
	g_object_set(cash, "organization-id", org, "code", code, "name", "Existing cash",
		"kind", VENTURE_ACCOUNT_KIND_ASSET, "active", TRUE, NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(cash), NULL, &error));
	profile_row = venture_autojournal_service_profile(venture_database_get_autojournal_service(f->db), org, &error);
	g_assert_no_error(error);
	g_assert_nonnull(profile_row);
	g_object_get(profile_row, "cash-account-id", &cash_id, NULL);
	g_assert_cmpint(cash_id, ==, venture_entity_get_id(VENTURE_ENTITY(cash)));
}
static GError *
refuse_date(VenturePostingService *posting, gint64 org, GDateTime *when, gpointer data)
{
	(void)posting; (void)org; (void)when; (void)data;
	return g_error_new_literal(VENTURE_ERROR, VENTURE_ERROR_CONFLICT, "Period is closed");
}
static void
backfill_guard(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureSale) record = sale(f);
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) result = NULL;
	g_autoptr(GPtrArray) journals = NULL;
	(void)data;
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(record), NULL, &error));
	g_object_set(record, "notes", "Unposted latest version", NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(record), NULL, &error));
	g_signal_connect(venture_database_get_posting_service(f->db), "date-postable", G_CALLBACK(refuse_date), NULL);
	result = venture_autojournal_service_backfill(venture_database_get_autojournal_service(f->db), f->org, FALSE, NULL, &error);
	g_assert_null(result); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
	journals = find(f, VENTURE_TYPE_JOURNAL); g_assert_cmpuint(journals->len, ==, 1);
}
static void
upgrade(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureSale) record = sale(f);
	g_autoptr(VentureEntity) loaded = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) refunded_at = NULL;
	g_autoptr(GPtrArray) journals = NULL;
	guint i;
	(void)data;
	money(record, "refunded", 1500);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(record), NULL, &error));
	/* A prior schema has refund totals, but no reliable separate refund dates. */
	g_assert_true(venture_database_execute(f->db,
		"ALTER TABLE sales DROP COLUMN refunded_at; DELETE FROM schema_migrations WHERE version >= 60", NULL, &error));
	g_assert_no_error(error);
	for (i = 0; i < 2; i++) {
		g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
		g_assert_no_error(error);
	}
	loaded = venture_database_get(f->db, VENTURE_TYPE_SALE, venture_entity_get_id(VENTURE_ENTITY(record)), &error);
	g_assert_no_error(error);
	g_object_get(loaded, "refunded-at", &refunded_at, NULL);
	g_assert_null(refunded_at);
	journals = find(f, VENTURE_TYPE_JOURNAL); g_assert_cmpuint(journals->len, ==, 2);
	g_assert_cmpint(balance(f, "4000"), ==, -10000);
}
static void
module_off(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureSale) record = sale(f);
	g_autoptr(GError) error = NULL;
	VentureAutojournalService *service = venture_database_get_autojournal_service(f->db);
	(void)data;
	venture_config_set_module_enabled(f->config, "autojournal", FALSE);
	g_assert_null(venture_autojournal_service_profile(service, f->org, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_clear_error(&error);
	g_assert_cmpuint(venture_entity_registry_lookup(venture_entity_registry_get_default(), "posting_profile"), ==, 0);
	g_assert_null(venture_report_registry_lookup(venture_context_get_report_registry(f->context), "unposted"));
	money(record, "fees", 300);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(record), NULL, &error));
	g_assert_no_error(error); g_assert_cmpint(balance(f, "4000"), ==, -9700);
	g_assert_true(venture_database_execute(f->db, "DROP TABLE posting_profiles; DELETE FROM schema_migrations WHERE version >= 60", NULL, &error));
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	venture_config_set_module_enabled(f->config, "autojournal", TRUE);
}
typedef struct { gboolean done; gchar *out; gchar *err; GError *error; } CliResult;
static void
cli_finished(GObject *process, GAsyncResult *result, gpointer data)
{
	CliResult *state = data;
	g_subprocess_communicate_utf8_finish(G_SUBPROCESS(process), result, &state->out, &state->err, &state->error);
	state->done = TRUE;
}
static void
cli_surface(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureSale) record = sale(f);
	g_autoptr(VentureWebServer) server = NULL;
	g_autoptr(GSocketListener) probe = g_socket_listener_new();
	g_autoptr(GSubprocess) cli = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(GPtrArray) journals = NULL;
	g_autofree gchar *directory = g_dir_make_tmp("venture-autojournal-XXXXXX", NULL);
	g_autofree gchar *url = NULL;
	guint port = g_socket_listener_add_any_inet_port(probe, NULL, &error);
	CliResult result;
	(void)data;
	g_assert_no_error(error); g_clear_object(&probe);
	url = g_strdup_printf("http://127.0.0.1:%u", port);
	g_object_set(f->config, "security-require-auth", FALSE, "server-bind-address", "127.0.0.1",
		"server-port", (gint64)port, "state-dir", directory, NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(record), NULL, &error));
	g_object_set(record, "notes", "CLI candidate", NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(record), NULL, &error));
	server = venture_web_server_new(f->context, &error);
	g_assert_no_error(error); g_assert_true(venture_web_server_start(server, &error));
	g_assert_no_error(error);
	result.done = FALSE; result.out = NULL; result.err = NULL; result.error = NULL;
	cli = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE, &error,
		"build/debug/venturectl", "--server", url, "-f", "json", "post", "backfill", "--dry-run", NULL);
	g_assert_no_error(error);
	g_subprocess_communicate_utf8_async(cli, NULL, NULL, cli_finished, &result);
	while (!result.done) g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(result.error);
	g_test_message("CLI stderr: %s", result.err);
	g_assert_true(g_subprocess_get_successful(cli));
	g_assert_true(json_parser_load_from_data(parser, result.out, -1, &error));
	g_assert_no_error(error);
	g_assert_cmpint(json_object_get_int_member(json_node_get_object(json_parser_get_root(parser)), "posted"), ==, 1);
	journals = find(f, VENTURE_TYPE_JOURNAL); g_assert_cmpuint(journals->len, ==, 1);
	g_free(result.out); g_free(result.err);
	venture_web_server_stop(server);
	g_clear_object(&server);
	venture_test_remove_tree(directory);
}
static void
profile_edit(Fixture *f, gconstpointer data)
{
	g_autoptr(VenturePostingProfile) profile = NULL;
	g_autoptr(VentureAccount) clearing = venture_account_new();
	g_autoptr(VentureSale) record = sale(f);
	g_autoptr(GError) error = NULL;
	(void)data;
	profile = venture_autojournal_service_profile(venture_database_get_autojournal_service(f->db), f->org, &error);
	g_assert_no_error(error);
	g_object_set(clearing, "name", "Marketplace clearing", "code", "1010", "organization-id", f->org,
		"kind", VENTURE_ACCOUNT_KIND_ASSET, "active", TRUE, NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(clearing), NULL, &error));
	g_object_set(profile, "cash-account-id", venture_entity_get_id(VENTURE_ENTITY(clearing)), NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(profile), NULL, &error));
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(record), NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpint(balance(f, "1010"), ==, 10000);
	g_assert_cmpint(balance(f, "1000"), ==, 0);
}
static void
expense_priority(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureExpense) record = venture_expense_new();
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_TAX_CATEGORY);
	g_autoptr(VentureEntity) tax = NULL;
	g_autoptr(GError) error = NULL;
	(void)data;
	venture_query_set_organization(query, f->org);
	venture_query_add_filter_string(query, "code", VENTURE_FILTER_OP_EQ, "SOFTWARE", NULL);
	tax = venture_database_find_one(f->db, query, &error);
	g_assert_no_error(error); g_assert_nonnull(tax);
	g_object_set(record, "organization-id", f->org, "description", "Tax classification wins", "category", "ADVERTISING",
		"tax-category-id", venture_entity_get_id(tax), NULL);
	money(record, "amount", 900);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(record), NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpint(balance(f, "6300"), ==, 900);
	g_assert_cmpint(balance(f, "6200"), ==, 0);
	g_object_set(record, "tax-category-id", (gint64)0, "category", "UNKNOWN", NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(record), NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpint(balance(f, "6900"), ==, 900);
	g_assert_cmpint(balance(f, "6300"), ==, 0);
}
static void
upgrade_disabled(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	(void)data;
	/* A switched-off historical sales table is not reconciled at startup. */
	venture_config_set_module_enabled(f->config, "sales", FALSE);
	g_assert_true(venture_database_execute(f->db,
		"DROP TABLE posting_profiles; ALTER TABLE sales DROP COLUMN refunded_at; DELETE FROM schema_migrations WHERE version >= 60", NULL, &error));
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	venture_config_set_module_enabled(f->config, "sales", TRUE);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
}
static void
rules_at_context_start(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureSale) record = sale(f);
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GError) error = NULL;
	VenturePostingRule *rule = venture_posting_rule_registry_lookup(venture_posting_service_get_rules(
		venture_database_get_posting_service(f->db)), "sale");
	(void)data;
	money(record, "fees", 300);
	rows = venture_posting_rule_build_lines(rule, f->db, VENTURE_ENTITY(record), &error);
	g_assert_no_error(error); g_assert_nonnull(rows);
	g_assert_cmpuint(rows->len, ==, 3);
}
static void
backfill_order(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureSale) late = sale(f), early = sale(f);
	g_autoptr(GDateTime) when = g_date_time_new_from_iso8601("2026-02-10T00:00:00Z", NULL);
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) sources = NULL, journals = NULL;
	g_autoptr(JsonNode) result = NULL;
	gint64 source_id;
	VentureAutojournalService *service = venture_database_get_autojournal_service(f->db);
	(void)data;
	g_object_set(late, "occurred-at", when, NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(late), NULL, &error));
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(early), NULL, &error));
	g_object_set(late, "notes", "Latest version", NULL); g_object_set(early, "notes", "Latest version", NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(late), NULL, &error));
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(early), NULL, &error));
	sources = venture_autojournal_service_unposted(service, f->org, &error);
	g_assert_no_error(error); g_assert_cmpuint(sources->len, ==, 2);
	g_assert_cmpint(venture_entity_get_id(g_ptr_array_index(sources, 0)), ==, venture_entity_get_id(VENTURE_ENTITY(early)));
	result = venture_autojournal_service_backfill(service, f->org, FALSE, NULL, &error);
	g_assert_no_error(error); g_assert_nonnull(result);
	journals = find(f, VENTURE_TYPE_JOURNAL); g_assert_cmpuint(journals->len, ==, 6);
	g_object_get(g_ptr_array_index(journals, 3), "source-id", &source_id, NULL);
	g_assert_cmpint(source_id, ==, venture_entity_get_id(VENTURE_ENTITY(early)));
	g_object_get(g_ptr_array_index(journals, 5), "source-id", &source_id, NULL);
	g_assert_cmpint(source_id, ==, venture_entity_get_id(VENTURE_ENTITY(late)));
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
	g_test_add("/autojournal/cogs", Fixture, NULL, setup, cogs_test, teardown);
	g_test_add("/autojournal/refund-failure", Fixture, NULL, setup, refund_failure, teardown);
	g_test_add("/autojournal/backfill", Fixture, NULL, setup, backfill_test, teardown);
	g_test_add("/autojournal/profile-scope", Fixture, NULL, setup, profile_scope, teardown);
	g_test_add("/autojournal/profile-reuses-scoped-chart", Fixture, NULL, setup, profile_reuses_scoped_chart, teardown);
	g_test_add("/autojournal/backfill-period-guard", Fixture, NULL, setup, backfill_guard, teardown);
	g_test_add("/autojournal/upgrade-restart", Fixture, NULL, setup, upgrade, teardown);
	g_test_add("/autojournal/module-off", Fixture, NULL, setup, module_off, teardown);
	g_test_add("/autojournal/cli-rest-dry-run", Fixture, NULL, setup, cli_surface, teardown);
	g_test_add("/autojournal/profile-edit", Fixture, NULL, setup, profile_edit, teardown);
	g_test_add("/autojournal/expense-priority", Fixture, NULL, setup, expense_priority, teardown);
	g_test_add("/autojournal/upgrade-disabled-sales", Fixture, NULL, setup, upgrade_disabled, teardown);
	g_test_add("/autojournal/rules-at-context-start", Fixture, NULL, setup, rules_at_context_start, teardown);
	g_test_add("/autojournal/backfill-date-order", Fixture, NULL, setup, backfill_order, teardown);
	return g_test_run();
}
