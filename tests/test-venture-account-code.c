/*
 * test-venture-account-code.c - a taken account code says who has it
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <venture.h>
#include <string.h>

static VentureEntity *
account(gint64 org, const gchar *code, const gchar *name)
{
	return g_object_new(VENTURE_TYPE_ACCOUNT, "organization-id", org, "code", code, "name", name,
		"kind", VENTURE_ACCOUNT_KIND_EXPENSE, "active", TRUE, NULL);
}

/* A taken code is refused naming the account that has it, never as a
 * database constraint error; a free code saves. */
static void
test_account_codes(void)
{
	g_autoptr(VentureDatabase) db = venture_database_new("sqlite://:memory:", NULL);
	g_autoptr(VentureEntity) groceries = NULL, clash = NULL;
	g_autoptr(GError) error = NULL;
	gint64 org;
	g_assert_true(venture_database_migrate(db, venture_entity_registry_get_default(), &error));
	org = venture_entity_get_id(venture_database_find_one(db, venture_query_new(VENTURE_TYPE_ORGANIZATION), NULL));
	groceries = account(org, "H-GROCERIES", "Groceries");
	g_assert_true(venture_database_save(db, groceries, NULL, &error));
	g_assert_no_error(error);
	g_object_set(groceries, "name", "Groceries and household", NULL);
	g_assert_true(venture_database_save(db, groceries, NULL, &error));
	g_assert_no_error(error);
	clash = account(org, "H-GROCERIES", "Groceries twice");
	g_assert_false(venture_database_save(db, clash, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_nonnull(strstr(error->message, "Groceries and household"));
	g_assert_null(strstr(error->message, "SQL"));
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/account-code/taken", test_account_codes);
	return g_test_run();
}
