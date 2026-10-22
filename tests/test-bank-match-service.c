/* SPDX-License-Identifier: AGPL-3.0-or-later
 * API coverage for src/banking/venture-bank-match-service.c */
#include <venture.h>

static void
test_search_symbol(void)
{
	g_assert_nonnull(venture_bank_transaction_candidates_search);
}

/* A household names a bank account and picks its kind; it does not know
 * the ledger. A new bank account with no ledger account gets its own, of
 * the right class, in the book currency, saved with it or not at all. */
static void
test_bank_account_creates_ledger(void)
{
	g_autoptr(VentureDatabase) db = venture_database_new("sqlite://:memory:", NULL);
	g_autoptr(VentureEntity) checking = NULL, card = NULL, ledger = NULL, broken = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureQuery) count = venture_query_new(VENTURE_TYPE_ACCOUNT);
	g_autoptr(GPtrArray) before = NULL, after = NULL;
	g_autofree gchar *name = NULL, *currency = NULL;
	gint kind = -1;
	gint64 id = 0, org;
	gboolean saved;
	g_assert_true(venture_database_migrate(db, venture_entity_registry_get_default(), &error));
	org = venture_entity_get_id(venture_database_find_one(db, venture_query_new(VENTURE_TYPE_ORGANIZATION), NULL));
	venture_query_set_limit(count, 0);
	checking = g_object_new(VENTURE_TYPE_BANK_ACCOUNT, "organization-id", org, "name", "Joint checking",
		"kind", VENTURE_BANK_ACCOUNT_CHECKING, NULL);
	saved = venture_database_save(db, checking, NULL, &error);
	g_assert_no_error(error);
	g_assert_true(saved);
	g_object_get(checking, "account-id", &id, "currency", &currency, NULL);
	g_assert_cmpint(id, >, 0);
	g_assert_cmpstr(currency, ==, "USD");
	ledger = venture_database_get(db, VENTURE_TYPE_ACCOUNT, id, &error);
	g_assert_no_error(error);
	g_object_get(ledger, "name", &name, "kind", &kind, NULL);
	g_assert_cmpstr(name, ==, "Joint checking");
	g_assert_cmpint(kind, ==, VENTURE_ACCOUNT_KIND_ASSET);

	card = g_object_new(VENTURE_TYPE_BANK_ACCOUNT, "organization-id", org, "name", "Travel card",
		"kind", VENTURE_BANK_ACCOUNT_CARD, "currency", "USD", NULL);
	g_assert_true(venture_database_save(db, card, NULL, &error));
	g_assert_no_error(error);
	g_object_get(card, "account-id", &id, NULL);
	g_clear_object(&ledger);
	ledger = venture_database_get(db, VENTURE_TYPE_ACCOUNT, id, &error);
	g_object_get(ledger, "kind", &kind, NULL);
	g_assert_cmpint(kind, ==, VENTURE_ACCOUNT_KIND_LIABILITY);

	/* A refused bank account leaves no ledger account behind, and says why. */
	before = venture_database_find(db, count, NULL);
	broken = g_object_new(VENTURE_TYPE_BANK_ACCOUNT, "organization-id", org, "name", "Bad currency",
		"kind", VENTURE_BANK_ACCOUNT_SAVINGS, "currency", "1", NULL);
	g_assert_false(venture_database_save(db, broken, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	after = venture_database_find(db, count, NULL);
	g_assert_cmpuint(after->len, ==, before->len);
}

static VentureEntity *
execute(VentureDatabase *db, const gchar *action, gint64 id, const gchar *json, GError **error)
{
	g_autoptr(JsonParser) parser = json_parser_new();
	g_assert_true(json_parser_load_from_data(parser, json, -1, NULL));
	return venture_bank_match_service_execute(venture_database_get_bank_match_service(db), action, id,
		json_node_get_object(json_parser_get_root(parser)), NULL, error);
}

static gint64
line_id(VentureDatabase *db, const gchar *description)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_BANK_TRANSACTION);
	g_autoptr(VentureEntity) row = NULL;
	venture_query_add_filter_string(query, "description", VENTURE_FILTER_OP_EQ, description, NULL);
	row = venture_database_find_one(db, query, NULL);
	g_assert_nonnull(row);
	return venture_entity_get_id(row);
}

/* A household's month: the columns typed beside Import are used even when
 * nothing was mapped before, ISO dates need no format, pay in becomes
 * income with no customer, and a categorised purchase books to the expense
 * account of that name. */
static void
test_household_month(void)
{
	g_autoptr(VentureDatabase) db = venture_database_new("sqlite://:memory:", NULL);
	g_autoptr(VentureEntity) bank = NULL, groceries = NULL, statement = NULL, reloaded = NULL, line = NULL;
	g_autoptr(VentureQuery) sales = venture_query_new(VENTURE_TYPE_SALE);
	g_autoptr(VentureQuery) legs = venture_query_new(VENTURE_TYPE_JOURNAL_LINE);
	g_autoptr(GPtrArray) found = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *saved_column = NULL, *state = NULL;
	gint64 org;
	const gchar *month =
		"{\"format\":\"csv\",\"period_start\":\"2026-09-01\",\"period_end\":\"2026-09-30\","
		"\"opening_balance\":\"500.00\",\"closing_balance\":\"3604.60\","
		"\"date_column\":\"Date\",\"amount_column\":\"Amount\",\"description_column\":\"Description\","
		"\"external_id_column\":\"Ref\","
		"\"data\":\"Date,Description,Amount,Ref\\n2026-09-01,Salary,3200.00,S1\\n2026-09-04,Grocer,-95.40,G1\\n\"}";
	g_assert_true(venture_database_migrate(db, venture_entity_registry_get_default(), &error));
	org = venture_entity_get_id(venture_database_find_one(db, venture_query_new(VENTURE_TYPE_ORGANIZATION), NULL));
	bank = g_object_new(VENTURE_TYPE_BANK_ACCOUNT, "organization-id", org, "name", "Joint checking",
		"kind", VENTURE_BANK_ACCOUNT_CHECKING, NULL);
	g_assert_true(venture_database_save(db, bank, NULL, &error));
	g_assert_no_error(error);
	{
		g_autoptr(VentureEntity) home = g_object_new(VENTURE_TYPE_VENTURE, "organization-id", org, "name", "Our home",
			"venture-type", "household", NULL);
		g_assert_true(venture_database_save(db, home, NULL, &error));
		g_assert_no_error(error);
	}
	groceries = g_object_new(VENTURE_TYPE_ACCOUNT, "organization-id", org, "code", "6100-G", "name", "Groceries",
		"kind", VENTURE_ACCOUNT_KIND_EXPENSE, "active", TRUE, NULL);
	g_assert_true(venture_database_save(db, groceries, NULL, &error));

	statement = execute(db, "import", venture_entity_get_id(bank), month, &error);
	g_assert_no_error(error);
	g_assert_nonnull(statement);
	reloaded = venture_database_get(db, VENTURE_TYPE_BANK_ACCOUNT, venture_entity_get_id(bank), NULL);
	g_object_get(reloaded, "date-column", &saved_column, NULL);
	g_assert_true(saved_column == NULL || *saved_column == '\0');

	g_clear_object(&line);
	line = execute(db, "create", line_id(db, "Salary"), "{\"type\":\"income\"}", &error);
	g_assert_no_error(error);
	found = venture_database_find(db, sales, NULL);
	g_assert_cmpuint(found->len, ==, 1);
	g_clear_object(&line);
	line = venture_database_get(db, VENTURE_TYPE_BANK_TRANSACTION, line_id(db, "Salary"), NULL);
	g_object_get(line, "state", &state, NULL);
	g_assert_cmpstr(state, !=, "unmatched");

	g_clear_object(&line);
	line = execute(db, "create", line_id(db, "Grocer"), "{\"type\":\"expense\",\"category\":\"groceries\"}", &error);
	g_assert_no_error(error);
	venture_query_add_filter_int(legs, "account-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(groceries), NULL);
	g_clear_pointer(&found, g_ptr_array_unref);
	found = venture_database_find(db, legs, NULL);
	g_assert_cmpuint(found->len, ==, 1);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/bank-match-service/search-symbol", test_search_symbol);
	g_test_add_func("/bank-match-service/bank-account-creates-ledger", test_bank_account_creates_ledger);
	g_test_add_func("/bank-match-service/household-month", test_household_month);
	return g_test_run();
}
