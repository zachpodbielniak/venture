/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>

static void
test_missing_rate_refused(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureDatabase) database = venture_database_new("sqlite://:memory:", &error);
	g_autoptr(VentureExchangePolicy) policy = NULL;
	g_autoptr(VentureMoney) euros = venture_money_new_for_currency(10000, "EUR");
	g_autoptr(VentureMoney) valued = NULL;
	g_autoptr(GDateTime) when = g_date_time_new_from_iso8601("2026-01-10T00:00:00Z", NULL);
	gint64 org;

	g_assert_true(venture_database_migrate(database, venture_entity_registry_get_default(), &error));
	org = 1;
	policy = venture_rate_table_policy_new(database, org);
	valued = venture_exchange_policy_convert(policy, euros, "USD", when, &error);
	g_assert_null(valued);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/venture-rate-table-policy/missing-rate", test_missing_rate_refused);
	return g_test_run();
}
