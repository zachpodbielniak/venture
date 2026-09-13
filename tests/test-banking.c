/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <string.h>

static void
test_records(void)
{
	const gchar *names[] = { "bank_account", "bank_statement", "bank_transaction", "bank_match", "reconciliation" };
	guint i;
	for (i = 0; i < G_N_ELEMENTS(names); i++)
	{
		GType type = venture_entity_registry_lookup(venture_entity_registry_get_default(), names[i]);
		g_assert_cmpuint(type, !=, G_TYPE_INVALID);
	}
}

/* Generic writes must never manufacture matched evidence. */
static void
test_match_guard(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureDatabase) db = venture_database_new("sqlite://:memory:", &error);
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureContext) context = NULL;
	g_autoptr(VentureBankMatch) match = venture_bank_match_new();
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(db, venture_entity_registry_get_default(), &error));
	context = venture_context_new(config, db);
	venture_entity_set_organization_id(VENTURE_ENTITY(match), venture_context_get_default_organization_id(context));
	g_object_set(match, "record-type", "expense", "record-id", (gint64)1, "kind", "exact", NULL);
	g_assert_false(venture_database_save(db, VENTURE_ENTITY(match), NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_nonnull(strstr(error->message, "VentureBankMatchService"));
}

/* The public action is also the boundary used by HTTP and CLI adapters. */

static void
test_import_match_reconcile(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureDatabase) db = venture_database_new("sqlite://:memory:", &error);
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureContext) context = NULL;
	g_autoptr(VentureBankAccount) bank = venture_bank_account_new();
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ACCOUNT);
	g_autoptr(GPtrArray) accounts = NULL;
	g_autoptr(GPtrArray) transactions = NULL;
	g_autoptr(GPtrArray) candidates = NULL;
	g_autoptr(JsonObject) args = json_object_new();
	g_autoptr(VentureEntity) statement = NULL;
	g_autoptr(VentureEntity) result = NULL;
	g_autoptr(VentureMoney) balance = NULL;
	VentureBankMatchService *service;
	gint64 org, id;
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(db, venture_entity_registry_get_default(), &error));
	context = venture_context_new(config, db);
	org = venture_context_get_default_organization_id(context);
	venture_query_set_organization(query, org);
	g_assert_true(venture_query_add_filter_string(query, "code", VENTURE_FILTER_OP_EQ, "1000", &error));
	accounts = venture_database_find(db, query, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(accounts->len, ==, 1);
	g_object_set(bank, "name", "Checking", "account-id", venture_entity_get_id(g_ptr_array_index(accounts, 0)),
		"currency", "USD", "date-column", "Date", "amount-column", "Amount",
		"description-column", "Memo", "reference-column", "Ref", "external-id-column", "ID",
		"date-format", "%Y-%m-%d", "sign-convention", "normal", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(bank), org);
	g_assert_true(venture_database_save(db, VENTURE_ENTITY(bank), NULL, &error));
	service = venture_database_get_bank_match_service(db);
	json_object_set_string_member(args, "period_start", "2026-01-01");
	json_object_set_string_member(args, "period_end", "2026-01-31");
	json_object_set_string_member(args, "opening_balance", "0 USD");
	json_object_set_string_member(args, "closing_balance", "-10 USD");
	json_object_set_string_member(args, "format", "csv");
	json_object_set_string_member(args, "data", "Date,Amount,Memo,Ref,ID\n2026-01-10,-10,Fee,January,fee-1\n");
	statement = venture_bank_match_service_execute(service, "import", venture_entity_get_id(VENTURE_ENTITY(bank)), args, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(statement);
	result = venture_bank_match_service_execute(service, "import", venture_entity_get_id(VENTURE_ENTITY(bank)), args, NULL, &error);
	g_assert_null(result);
	g_assert_nonnull(error);
	g_clear_error(&error);
	g_clear_object(&query);
	query = venture_query_new(VENTURE_TYPE_BANK_TRANSACTION);
	transactions = venture_database_find(db, query, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(transactions->len, ==, 1);
	id = venture_entity_get_id(g_ptr_array_index(transactions, 0));
	result = venture_bank_match_service_execute(service, "reconcile", venture_entity_get_id(statement), args, NULL, &error);
	g_assert_null(result);
	g_assert_nonnull(error);
	g_clear_error(&error);
	/* Creation must post through the expense service and match atomically. */
	json_object_set_string_member(args, "type", "expense");
	result = venture_bank_match_service_execute(service, "create", id, args, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	g_clear_object(&result);
	result = venture_bank_match_service_execute(service, "unmatch", id, args, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	candidates = venture_bank_transaction_candidates(db, result, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(candidates->len, ==, 1);
	g_clear_object(&result);
	result = venture_bank_match_service_execute(service, "auto", venture_entity_get_id(statement), args, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	g_clear_object(&result);
	result = venture_bank_match_service_execute(service, "reconcile", venture_entity_get_id(statement), args, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	g_object_get(result, "difference", &balance, NULL);
	g_assert_cmpint(venture_money_get_amount(balance), ==, 0);
	g_clear_object(&result);
	result = venture_bank_match_service_execute(service, "unmatch", id, args, NULL, &error);
	g_assert_null(result);
	g_assert_nonnull(error);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	venture_entity_registry_register_builtins(venture_entity_registry_get_default());
	g_test_add_func("/banking/records", test_records);
	g_test_add_func("/banking/generic-match-refused", test_match_guard);
	g_test_add_func("/banking/import-match-reconcile", test_import_match_reconcile);
	return g_test_run();
}
