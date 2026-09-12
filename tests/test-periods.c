/*
 * test-periods.c - Fiscal boundaries and reproducible financial records
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <venture.h>

typedef struct
{
	VentureDatabase *database;
	VentureConfig *config;
	VentureContext *context;
	gint64 organization_id;
	gint64 other_id;
	gint64 venture_id;
} Fixture;

static void
fixture_set_up(Fixture *fixture, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureOrganization) other = NULL;
	g_autoptr(VentureVenture) venture = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureEntity) organization = NULL;

	fixture->config = venture_config_new();
	fixture->database = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	fixture->context = venture_context_new(fixture->config, fixture->database);
	g_assert_true(venture_database_migrate(fixture->database,
		venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	query = venture_query_new(VENTURE_TYPE_ORGANIZATION);
	organization = venture_database_find_one(fixture->database, query, &error);
	g_assert_no_error(error);
	fixture->organization_id = venture_entity_get_id(organization);
	other = venture_organization_new();
	g_object_set(other, "name", "Second company", NULL);
	g_assert_true(venture_database_save(fixture->database,
		VENTURE_ENTITY(other), NULL, &error));
	g_assert_no_error(error);
	fixture->other_id = venture_entity_get_id(VENTURE_ENTITY(other));
	venture = venture_venture_new();
	g_object_set(venture, "name", "Books", "venture-type", "books",
		"organization-id", fixture->organization_id, NULL);
	g_assert_true(venture_database_save(fixture->database,
		VENTURE_ENTITY(venture), NULL, &error));
	g_assert_no_error(error);
	fixture->venture_id = venture_entity_get_id(VENTURE_ENTITY(venture));
}

static void
fixture_tear_down(Fixture *fixture, gconstpointer data)
{
	g_clear_object(&fixture->context);
	g_clear_object(&fixture->database);
	g_clear_object(&fixture->config);
}

static VentureEntity *
financial_record(Fixture *fixture, GType type, gint64 organization_id)
{
	VentureEntity *entity;

	entity = g_object_new(type, "organization-id", organization_id, NULL);
	if (VENTURE_TYPE_SALE == type)
		g_object_set(entity, "venture-id", fixture->venture_id,
			"external-id", "import-42", NULL);
	else if (VENTURE_TYPE_EXPENSE == type)
		g_object_set(entity, "description", "Paper", "external-id", "import-42", NULL);
	else if (VENTURE_TYPE_ACCOUNT == type)
		g_object_set(entity, "code", "1000", "name", "Cash", NULL);
	else
		g_object_set(entity, "number", "1", NULL);
	return entity;
}

/* An importer can retry a row, but must never double the books. Empty IDs
 * are not import identities, and a different company owns its own IDs. */
static void
test_external_id(Fixture *fixture, gconstpointer data)
{
	g_autoptr(VentureEntity) first = NULL;
	g_autoptr(VentureEntity) duplicate = NULL;
	g_autoptr(VentureEntity) other = NULL;
	g_autoptr(GError) error = NULL;
	GType type;
	VentureActor actor;
	guint i;

	type = (0 == GPOINTER_TO_INT(data)) ? VENTURE_TYPE_SALE : VENTURE_TYPE_EXPENSE;
	actor.kind = VENTURE_ACTOR_KIND_IMPORT;
	actor.name = "marketplace";
	actor.prompt = NULL;
	actor.request_id = NULL;
	actor.approved_by = NULL;
	first = financial_record(fixture, type, fixture->organization_id);
	duplicate = financial_record(fixture, type, fixture->organization_id);
	other = financial_record(fixture, type, fixture->other_id);
	g_assert_true(venture_database_save(fixture->database, first, &actor, &error));
	g_assert_no_error(error);
	g_assert_false(venture_database_save(fixture->database, duplicate, &actor, &error));
	g_assert_nonnull(error);
	g_clear_error(&error);
	g_assert_true(venture_database_save(fixture->database, other, &actor, &error));
	g_assert_no_error(error);

	/* Soft deletion does not make an imported identity available again. */
	g_assert_true(venture_database_delete(fixture->database, first, &actor, &error));
	g_assert_no_error(error);
	g_assert_false(venture_database_save(fixture->database, duplicate, &actor, &error));
	g_assert_nonnull(error);
	g_clear_error(&error);
	for (i = 0; i < 2; i++)
	{
		g_autoptr(VentureEntity) manual = NULL;
		manual = financial_record(fixture, type, fixture->organization_id);
		g_object_set(manual, "external-id", "", NULL);
		g_assert_true(venture_database_save(fixture->database, manual, NULL, &error));
		g_assert_no_error(error);
	}
}

/* Independent legal entities need independent charts and invoice sequences. */
static void
test_numbering(Fixture *fixture, gconstpointer data)
{
	g_autoptr(VentureEntity) first = NULL;
	g_autoptr(VentureEntity) other = NULL;
	g_autoptr(VentureEntity) duplicate = NULL;
	g_autoptr(GError) error = NULL;
	GType type;

	type = (0 == GPOINTER_TO_INT(data)) ? VENTURE_TYPE_ACCOUNT : VENTURE_TYPE_INVOICE;
	/* Account 1000 already belongs to the seeded default company. */
	if (VENTURE_TYPE_INVOICE == type)
	{
		first = financial_record(fixture, type, fixture->organization_id);
		g_assert_true(venture_database_save(fixture->database, first, NULL, &error));
		g_assert_no_error(error);
	}
	other = financial_record(fixture, type, fixture->other_id);
	g_assert_true(venture_database_save(fixture->database, other, NULL, &error));
	g_assert_no_error(error);
	duplicate = financial_record(fixture, type, fixture->other_id);
	g_assert_false(venture_database_save(fixture->database, duplicate, NULL, &error));
	g_assert_nonnull(error);
}

/* Reproduce the shipped inline UNIQUE constraints before migrating a
 * populated database. Existing IDs, references and audit stamps must survive. */
static void
test_numbering_migration(void)
{
	g_autoptr(VentureEntityClass) account_class = NULL;
	g_autoptr(VentureEntityClass) invoice_class = NULL;
	g_autoptr(VentureDatabase) database = NULL;
	g_autoptr(VentureInvoice) invoice = NULL;
	g_autoptr(VentureInvoiceLine) line = NULL;
	g_autoptr(VentureEntity) stored = NULL;
	g_autoptr(VentureOrganization) other = NULL;
	g_autoptr(VentureAccount) account = NULL;
	g_autoptr(VentureInvoice) second = NULL;
	g_autoptr(GError) error = NULL;
	VentureColumnFlags account_flags;
	VentureColumnFlags invoice_flags;
	gint64 invoice_id;
	gint64 line_id;
	gint64 version;
	g_autofree gchar *uuid = NULL;

	venture_entity_registry_get_default();
	account_class = g_type_class_ref(VENTURE_TYPE_ACCOUNT);
	invoice_class = g_type_class_ref(VENTURE_TYPE_INVOICE);
	account_flags = venture_entity_class_get_column_flags(account_class, "code");
	invoice_flags = venture_entity_class_get_column_flags(invoice_class, "number");
	venture_entity_class_set_column_flags(account_class, "code",
		VENTURE_COLUMN_FLAG_UNIQUE | VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED);
	venture_entity_class_set_column_flags(invoice_class, "number",
		VENTURE_COLUMN_FLAG_UNIQUE | VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED);
	database = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(database,
		venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	invoice = venture_invoice_new();
	g_object_set(invoice, "number", "1", "organization-id", (gint64)1, NULL);
	g_assert_true(venture_database_save(database, VENTURE_ENTITY(invoice), NULL, &error));
	g_assert_no_error(error);
	invoice_id = venture_entity_get_id(VENTURE_ENTITY(invoice));
	version = venture_entity_get_version(VENTURE_ENTITY(invoice));
	uuid = g_strdup(venture_entity_get_uuid(VENTURE_ENTITY(invoice)));
	line = venture_invoice_line_new();
	g_object_set(line, "invoice-id", invoice_id, "description", "Original line", NULL);
	g_assert_true(venture_database_save(database, VENTURE_ENTITY(line), NULL, &error));
	g_assert_no_error(error);
	line_id = venture_entity_get_id(VENTURE_ENTITY(line));
	venture_entity_class_set_column_flags(account_class, "code", account_flags);
	venture_entity_class_set_column_flags(invoice_class, "number", invoice_flags);
	g_assert_true(venture_database_migrate(database,
		venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	stored = venture_database_get(database, VENTURE_TYPE_INVOICE, invoice_id, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(venture_entity_get_uuid(stored), ==, uuid);
	g_assert_cmpint(venture_entity_get_version(stored), ==, version);
	g_clear_object(&stored);
	stored = venture_database_get(database, VENTURE_TYPE_INVOICE_LINE, line_id, &error);
	g_assert_no_error(error);
	{
		gint64 reference;
		g_object_get(stored, "invoice-id", &reference, NULL);
		g_assert_cmpint(reference, ==, invoice_id);
	}
	other = venture_organization_new();
	g_object_set(other, "name", "Other company", NULL);
	g_assert_true(venture_database_save(database, VENTURE_ENTITY(other), NULL, &error));
	g_assert_no_error(error);
	account = venture_account_new();
	g_object_set(account, "code", "1000", "name", "Cash",
		"organization-id", venture_entity_get_id(VENTURE_ENTITY(other)), NULL);
	g_assert_true(venture_database_save(database, VENTURE_ENTITY(account), NULL, &error));
	g_assert_no_error(error);
	second = venture_invoice_new();
	g_object_set(second, "number", "1",
		"organization-id", venture_entity_get_id(VENTURE_ENTITY(other)), NULL);
	g_assert_true(venture_database_save(database, VENTURE_ENTITY(second), NULL, &error));
	g_assert_no_error(error);
	/* Startup migrations are repeatable. */
	g_assert_true(venture_database_migrate(database,
		venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/periods/unique/sale", Fixture, GINT_TO_POINTER(0),
		fixture_set_up, test_external_id, fixture_tear_down);
	g_test_add("/periods/unique/expense", Fixture, GINT_TO_POINTER(1),
		fixture_set_up, test_external_id, fixture_tear_down);
	g_test_add("/periods/numbering/account", Fixture, GINT_TO_POINTER(0),
		fixture_set_up, test_numbering, fixture_tear_down);
	g_test_add("/periods/numbering/invoice", Fixture, GINT_TO_POINTER(1),
		fixture_set_up, test_numbering, fixture_tear_down);
	g_test_add_func("/periods/numbering/migration", test_numbering_migration);
	return g_test_run();
}
