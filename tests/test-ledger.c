/*
 * test-ledger.c - Journals are the only authority for account balances
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <venture.h>

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
	f->db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db,
		venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->config = venture_config_new();
	f->context = venture_context_new(f->config, f->db);
	f->org = venture_context_get_default_organization_id(f->context);
}

static void
teardown(Fixture *f, gconstpointer data)
{
	(void)data;
	g_clear_object(&f->context);
	g_clear_object(&f->config);
	g_clear_object(&f->db);
}

static void
test_module(Fixture *f, gconstpointer data)
{
	VentureModule *module;
	const gchar *const *requires;

	(void)data;
	module = venture_module_registry_lookup(
		venture_context_get_modules(f->context), "ledger");
	g_assert_nonnull(module);
	requires = venture_module_get_requires(module);
	g_assert_true(g_strv_contains(requires, "finance"));
}

static void
test_records(Fixture *f, gconstpointer data)
{
	VentureEntityRegistry *registry;

	(void)data;
	registry = venture_context_get_entity_registry(f->context);
	g_assert_cmpuint(venture_entity_registry_lookup(registry, "journal"), !=, 0);
	g_assert_cmpuint(venture_entity_registry_lookup(registry, "journal_line"), !=, 0);
}

static void
test_generic_ledger_write(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureLedgerEntry) entry = NULL;
	g_autoptr(VentureMoney) amount = NULL;
	g_autoptr(GError) error = NULL;

	(void)data;
	entry = venture_ledger_entry_new();
	amount = venture_money_new_for_currency(100, "USD");
	g_object_set(entry, "transaction-id", "bypass", "account-id", (gint64)1,
		"amount", amount, NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(entry), f->org);
	/* The generic save is shared by REST, CLI and approved AI writes. */
	g_assert_false(venture_database_save(f->db, VENTURE_ENTITY(entry), NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
#define ADD(name, fn) g_test_add("/ledger/" name, Fixture, NULL, setup, fn, teardown)
	ADD("module", test_module);
	ADD("records", test_records);
	ADD("generic-ledger-write", test_generic_ledger_write);
#undef ADD
	return g_test_run();
}
