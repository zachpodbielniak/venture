#include <venture.h>

VENTURE_DECLARE_ENTITY(VentureMatchFixture, venture_match_fixture, MATCH_FIXTURE)
static const VentureFieldDecl fixture_fields[] = {
	VENTURE_FIELD("date", "Date", NULL, VENTURE_FIELD_KIND_DATE, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_MONEY("amount", "Amount", NULL),
	VENTURE_FIELD_TEXT("description", "Description", NULL),
	VENTURE_FIELD("reference", "Reference", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VentureMatchFixture, venture_match_fixture, fixture_fields)

static VentureEntity *
record(gint64 id, gint64 amount, const gchar *currency, gint day, const gchar *description)
{
	g_autoptr(VentureMoney) money = venture_money_new(amount, currency, 2);
	g_autoptr(GDateTime) date = g_date_time_new_utc(2026, 9, day, 0, 0, 0);
	return g_object_new(venture_match_fixture_get_type(), "id", id,
		"organization-id", (gint64)1, "amount", money, "date", date,
		"description", description, NULL);
}
static void
check_score(gint day, gint64 amount, const gchar *currency, const gchar *description,
	gint expected, gboolean duplicate)
{
	g_autoptr(VentureDatabase) db = venture_database_new("sqlite://:memory:", NULL);
	g_autoptr(VentureExactMatcher) matcher = venture_exact_matcher_new();
	g_autoptr(VentureEntity) transaction = record(1, 10000, "USD", 5, "Coffee SUPPLIER");
	g_autoptr(GPtrArray) candidates = g_ptr_array_new_with_free_func(g_object_unref);
	g_autoptr(GPtrArray) result = NULL;
	g_autoptr(GError) error = NULL;
	g_ptr_array_add(candidates, record(2, amount, currency, day, description));
	if (duplicate)
		g_ptr_array_add(candidates, record(3, amount, currency, day, description));
	result = venture_reconciliation_matcher_suggest(VENTURE_RECONCILIATION_MATCHER(matcher), db, transaction, candidates, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	g_assert_cmpuint(result->len, ==, expected ? (duplicate ? 2 : 1) : 0);
	if (expected)
	{
		gint score;
		g_object_get(g_ptr_array_index(result, 0), "confidence", &score, NULL);
		g_assert_cmpint(score, ==, expected);
	}
}
static void test_exact_unique(void) { check_score(8, 10000, "USD", "", 100, FALSE); }
static void test_exact_ambiguous(void) { check_score(8, 10000, "USD", "", 70, TRUE); }
static void test_exact_ten_days(void) { check_score(15, 10000, "USD", "", 70, FALSE); }
static void test_exact_outside_date(void) { check_score(16, 10000, "USD", "", 0, FALSE); }
static void test_exact_partial(void) { check_score(20, 10200, "USD", "coffee beans", 40, FALSE); }
static void test_exact_outside_amount(void) { check_score(20, 10201, "USD", "coffee beans", 0, FALSE); }
static void test_exact_no_overlap(void) { check_score(20, 10200, "USD", "unrelated", 0, FALSE); }
static void test_exact_currency(void) { check_score(5, 10000, "EUR", "Coffee", 0, FALSE); }
static void
test_exact_missing(void)
{
	g_autoptr(VentureDatabase) db = venture_database_new("sqlite://:memory:", NULL);
	g_autoptr(VentureExactMatcher) matcher = venture_exact_matcher_new();
	g_autoptr(VentureEntity) transaction = VENTURE_ENTITY(venture_contact_new());
	g_autoptr(GPtrArray) candidates = g_ptr_array_new_with_free_func(g_object_unref);
	g_autoptr(GPtrArray) result = NULL;
	g_ptr_array_add(candidates, record(2, 10000, "USD", 5, "Coffee"));
	result = venture_reconciliation_matcher_suggest(VENTURE_RECONCILIATION_MATCHER(matcher), db, transaction, candidates, NULL, NULL);
	g_assert_cmpuint(result->len, ==, 0);
}

static void
test_registry(void)
{
	g_autoptr(VentureDatabase) db = venture_database_new("sqlite://:memory:", NULL);
	g_autoptr(VentureReconciliationRegistry) registry = venture_reconciliation_registry_new();
	g_autoptr(VentureEntity) transaction = record(1, 10000, "USD", 5, "coffee");
	g_autoptr(GPtrArray) candidates = g_ptr_array_new_with_free_func(g_object_unref);
	g_autoptr(GPtrArray) result = NULL;
	g_autoptr(GPtrArray) list = NULL;
	gint score;
	venture_reconciliation_registry_add(registry, VENTURE_RECONCILIATION_MATCHER(venture_exact_matcher_new()));
	venture_reconciliation_registry_add(registry, VENTURE_RECONCILIATION_MATCHER(venture_exact_matcher_new()));
	list = venture_reconciliation_registry_list(registry);
	g_assert_cmpuint(list->len, ==, 1);
	g_ptr_array_add(candidates, record(3, 10200, "USD", 20, "coffee"));
	g_ptr_array_add(candidates, record(2, 10000, "USD", 5, ""));
	g_ptr_array_add(candidates, record(3, 10200, "USD", 20, "coffee"));
	result = venture_reconciliation_registry_suggest_all(registry, db, transaction, candidates, NULL, NULL);
	g_assert_cmpuint(result->len, ==, 2);
	g_object_get(g_ptr_array_index(result, 0), "confidence", &score, NULL);
	g_assert_cmpint(score, ==, 100);
	g_assert_true(venture_reconciliation_registry_remove(registry, "exact"));
	g_assert_null(venture_reconciliation_registry_lookup(registry, "exact"));
}
static void
async_done(GObject *source, GAsyncResult *result, gpointer data)
{
	GPtrArray **output = data;
	*output = venture_reconciliation_matcher_suggest_finish(VENTURE_RECONCILIATION_MATCHER(source), result, NULL);
}
static void
test_async(void)
{
	g_autoptr(VentureDatabase) db = venture_database_new("sqlite://:memory:", NULL);
	g_autoptr(VentureExactMatcher) matcher = venture_exact_matcher_new();
	g_autoptr(VentureEntity) transaction = record(1, 10000, "USD", 5, "");
	g_autoptr(GPtrArray) candidates = g_ptr_array_new_with_free_func(g_object_unref);
	g_autoptr(GPtrArray) result = NULL;
	g_autoptr(GCancellable) cancel = g_cancellable_new();
	g_autoptr(GError) error = NULL;
	g_ptr_array_add(candidates, record(2, 10000, "USD", 5, ""));
	venture_reconciliation_matcher_suggest_async(VENTURE_RECONCILIATION_MATCHER(matcher), db, transaction, candidates, NULL, async_done, &result);
	g_assert_null(result);
	while (result == NULL) g_main_context_iteration(NULL, TRUE);
	g_assert_cmpuint(result->len, ==, 1);
	g_clear_pointer(&result, g_ptr_array_unref);
	g_cancellable_cancel(cancel);
	result = venture_reconciliation_matcher_suggest(VENTURE_RECONCILIATION_MATCHER(matcher), db, transaction, candidates, cancel, &error);
	g_assert_null(result);
	g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
}

static void
test_matcher_contract(void)
{
	g_type_ensure(VENTURE_TYPE_RECONCILIATION_MATCHER);
	g_assert_cmpuint(g_type_from_name("VentureReconciliationMatcher"), !=, G_TYPE_INVALID);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/reconciliation/contract", test_matcher_contract);
	g_assert_true(venture_entity_registry_register(venture_entity_registry_get_default(), venture_match_fixture_get_type(), NULL));
	g_test_add_func("/reconciliation/exact/unique", test_exact_unique);
	g_test_add_func("/reconciliation/exact/ambiguous", test_exact_ambiguous);
	g_test_add_func("/reconciliation/exact/ten-days", test_exact_ten_days);
	g_test_add_func("/reconciliation/exact/outside-date", test_exact_outside_date);
	g_test_add_func("/reconciliation/exact/partial", test_exact_partial);
	g_test_add_func("/reconciliation/exact/outside-amount", test_exact_outside_amount);
	g_test_add_func("/reconciliation/exact/no-overlap", test_exact_no_overlap);
	g_test_add_func("/reconciliation/exact/currency", test_exact_currency);
	g_test_add_func("/reconciliation/exact/missing", test_exact_missing);

	g_test_add_func("/reconciliation/registry", test_registry);
	g_test_add_func("/reconciliation/async", test_async);
	return g_test_run();
}
