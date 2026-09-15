/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>

static void
test_seeded_tax_accounts(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureDatabase) database = venture_database_new("sqlite://:memory:", &error);
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureEntity) recoverable = NULL;

	g_assert_true(venture_database_migrate(database, venture_entity_registry_get_default(), &error));
	query = venture_query_new(VENTURE_TYPE_ACCOUNT);
	g_assert_true(venture_query_add_filter_string(query, "code", VENTURE_FILTER_OP_EQ, "1300", NULL));
	recoverable = venture_database_find_one(database, query, &error);
	g_assert_no_error(error);
	g_assert_nonnull(recoverable);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/venture-database/seeded-tax-accounts", test_seeded_tax_accounts);
	return g_test_run();
}
