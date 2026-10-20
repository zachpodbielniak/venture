/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Field-table coverage for src/banking/venture-bank-records.c */
#include <venture.h>

static void
test_match_window_field(void)
{
	g_autoptr(VentureBankAccount) account = venture_bank_account_new();
	GParamSpec *spec = g_object_class_find_property(G_OBJECT_GET_CLASS(account), "match-window-days");
	g_assert_nonnull(spec);
}

/* The same enum drives forms, JSON and stored household account kinds. */
static void
test_household_kinds(void)
{
	static const gchar *const kinds[] = { "unspecified", "checking", "savings", "card", "cash", "loan", "investment" };
	g_autoptr(VentureBankAccount) account = venture_bank_account_new();
	GParamSpec *spec = g_object_class_find_property(G_OBJECT_GET_CLASS(account), "kind");
	GEnumClass *values;
	guint i;
	g_assert_nonnull(spec);
	g_assert_true(G_IS_PARAM_SPEC_ENUM(spec));
	values = g_type_class_ref(G_PARAM_SPEC_VALUE_TYPE(spec));
	for (i = 0; i < G_N_ELEMENTS(kinds); i++)
		g_assert_nonnull(g_enum_get_value_by_nick(values, kinds[i]));
	g_type_class_unref(values);
}

/* Imported evidence and transfers are created from an account's actions,
 * not through generic forms which the save boundary correctly refuses. */
static void
test_evidence_creation_paths(void)
{
	g_assert_cmpstr(venture_entity_type_get_create_path(VENTURE_TYPE_BANK_STATEMENT), ==, "/e/bank_account");
	g_assert_cmpstr(venture_entity_type_get_create_path(VENTURE_TYPE_BANK_TRANSACTION), ==, "/e/bank_account");
	g_assert_cmpstr(venture_entity_type_get_create_path(VENTURE_TYPE_BANK_TRANSFER), ==, "/e/bank_account");
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/bank-records/match-window", test_match_window_field);
	g_test_add_func("/bank-records/household-kinds", test_household_kinds);
	g_test_add_func("/bank-records/evidence-creation-paths", test_evidence_creation_paths);
	return g_test_run();
}
