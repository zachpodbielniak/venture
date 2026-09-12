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

static void
test_period_records(Fixture *fixture, gconstpointer data)
{
	static const gchar *const names[] = { "fiscal_year", "fiscal_period", "report_snapshot" };
	guint i;

	for (i = 0; i < G_N_ELEMENTS(names); i++)
	{
		VentureModule *module;
		GType type = venture_entity_registry_lookup(venture_entity_registry_get_default(), names[i]);
		g_assert_cmpuint(type, !=, G_TYPE_INVALID);
		module = venture_module_registry_get_module_for_type(
			venture_context_get_modules(fixture->context), names[i]);
		g_assert_nonnull(module);
		g_assert_cmpstr(venture_module_get_name(module), ==, "periods");
	}
}

static VentureEntity *
new_year(Fixture *fixture, const gchar *start_text, gint64 organization_id, gint length)
{
	g_autoptr(GDateTime) start = venture_time_from_string(start_text, NULL);
	g_autoptr(GDateTime) end = g_date_time_add_years(start, 1);
	return g_object_new(VENTURE_TYPE_FISCAL_YEAR, "name", start_text,
		"organization-id", organization_id, "start-at", start, "end-at", end,
		"period-length", length, NULL);
}

static GPtrArray *
periods_for(Fixture *fixture, gint64 organization_id)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_FISCAL_PERIOD);
	g_autoptr(GError) error = NULL;
	GPtrArray *rows;
	venture_query_set_organization(query, organization_id);
	venture_query_add_order(query, "start-at", VENTURE_SORT_ASCENDING, NULL);
	rows = venture_database_find(fixture->database, query, &error);
	g_assert_no_error(error);
	return rows;
}

/* Boundaries are derived from the original start, so a January 31 start
 * does not drift to March 28 after a short February. */
static void
test_calendar_generation(Fixture *fixture, gconstpointer data)
{
	g_autoptr(VentureEntity) year = NULL;
	g_autoptr(GPtrArray) periods = NULL;
	g_autoptr(GDateTime) start = NULL;
	g_autoptr(GDateTime) end = NULL;
	g_autoptr(GError) error = NULL;
	guint months = (0 == GPOINTER_TO_INT(data)) ? 1 : 3;
	guint i;

	year = new_year(fixture, "2024-01-31", fixture->organization_id, GPOINTER_TO_INT(data));
	g_assert_true(venture_database_save(fixture->database, year, NULL, &error));
	g_assert_no_error(error);
	periods = periods_for(fixture, fixture->organization_id);
	g_assert_cmpuint(periods->len, ==, 12 / months);
	g_object_get(year, "start-at", &start, "end-at", &end, NULL);
	for (i = 0; i < periods->len; i++)
	{
		g_autoptr(GDateTime) a = NULL;
		g_autoptr(GDateTime) b = NULL;
		g_autoptr(GDateTime) expected_a = g_date_time_add_months(start, i * months);
		g_autoptr(GDateTime) expected_b = g_date_time_add_months(start, (i + 1) * months);
		gint state;
		g_object_get(g_ptr_array_index(periods, i), "start-at", &a, "end-at", &b, "state", &state, NULL);
		g_assert_cmpint(g_date_time_compare(a, expected_a), ==, 0);
		g_assert_cmpint(g_date_time_compare(b, expected_b), ==, 0);
		g_assert_cmpint(state, ==, VENTURE_PERIOD_OPEN);
	}
}

static void
test_calendar_refusal(Fixture *fixture, gconstpointer data)
{
	g_autoptr(VentureEntity) year = NULL;
	g_autoptr(VentureEntity) invalid = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *start = data;

	year = new_year(fixture, "2024-01-01", fixture->organization_id, 0);
	g_assert_true(venture_database_save(fixture->database, year, NULL, &error));
	g_assert_no_error(error);
	invalid = new_year(fixture, start, fixture->organization_id, 0);
	g_assert_false(venture_database_save(fixture->database, invalid, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	/* The same dates in another company are independent. */
	g_object_set(invalid, "organization-id", fixture->other_id, NULL);
	g_assert_true(venture_database_save(fixture->database, invalid, NULL, &error));
	g_assert_no_error(error);
}

static VentureActor
actor_named(const gchar *name)
{
	VentureActor actor;
	actor.kind = VENTURE_ACTOR_KIND_USER;
	actor.name = name;
	actor.prompt = NULL;
	actor.request_id = NULL;
	actor.approved_by = NULL;
	return actor;
}

static GPtrArray *
calendar(Fixture *fixture)
{
	g_autoptr(VentureEntity) year = new_year(fixture, "2024-01-01", fixture->organization_id, 0);
	g_autoptr(GError) error = NULL;
	g_assert_true(venture_database_save(fixture->database, year, NULL, &error));
	g_assert_no_error(error);
	return periods_for(fixture, fixture->organization_id);
}

static gboolean
change_state(Fixture *fixture, VentureEntity *period, gint state,
	const gchar *name, GError **error)
{
	VentureActor actor = actor_named(name);
	g_object_set(period, "state", state, NULL);
	return venture_database_save(fixture->database, period, &actor, error);
}

static void
test_close_order(Fixture *fixture, gconstpointer data)
{
	g_autoptr(GPtrArray) periods = calendar(fixture);
	g_autoptr(GError) error = NULL;
	g_assert_false(change_state(fixture, g_ptr_array_index(periods, 1), VENTURE_PERIOD_CLOSED, "closer", &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
	g_assert_nonnull(strstr(error->message, "P01"));
}

static void
test_closed_stamps(Fixture *fixture, gconstpointer data)
{
	g_autoptr(GPtrArray) periods = calendar(fixture);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *closed_by = NULL;
	g_autoptr(GDateTime) closed_at = NULL;
	VentureEntity *period = g_ptr_array_index(periods, 0);

	g_assert_true(change_state(fixture, period, VENTURE_PERIOD_CLOSED, "closer", &error));
	g_assert_no_error(error);
	g_object_get(period, "closed-by", &closed_by, "closed-at", &closed_at, NULL);
	g_assert_cmpstr(closed_by, ==, "closer");
	g_assert_nonnull(closed_at);
}

static void
test_reopen_permission(Fixture *fixture, gconstpointer data)
{
	g_autoptr(GPtrArray) periods = calendar(fixture);
	g_autoptr(VentureUser) user = venture_user_new();
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_AUDIT_ENTRY);
	g_autoptr(GPtrArray) audit = NULL;
	g_autoptr(GError) error = NULL;
	VentureEntity *period = g_ptr_array_index(periods, 0);

	g_assert_true(change_state(fixture, period, VENTURE_PERIOD_CLOSED, "closer", &error));
	g_assert_no_error(error);
	g_assert_false(change_state(fixture, period, VENTURE_PERIOD_OPEN, "editor", &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_assert_nonnull(strstr(error->message, "periods.reopen"));
	g_clear_error(&error);
	g_object_set(user, "username", "owner", "role", VENTURE_USER_ROLE_OWNER, "active", TRUE, NULL);
	g_assert_true(venture_database_save(fixture->database, VENTURE_ENTITY(user), NULL, &error));
	g_assert_no_error(error);
	g_assert_true(change_state(fixture, period, VENTURE_PERIOD_OPEN, "owner", &error));
	g_assert_no_error(error);
	venture_query_add_filter_string(query, "target-type", VENTURE_FILTER_OP_EQ, "fiscal_period", NULL);
	venture_query_add_filter_string(query, "actor", VENTURE_FILTER_OP_EQ, "owner", NULL);
	audit = venture_database_find(fixture->database, query, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(audit->len, ==, 1);
	{
		g_autofree gchar *diff = NULL;
		g_object_get(g_ptr_array_index(audit, 0), "diff", &diff, NULL);
		g_assert_nonnull(strstr(diff, "closed"));
		g_assert_nonnull(strstr(diff, "open"));
	}
}

static void
test_locked(Fixture *fixture, gconstpointer data)
{
	g_autoptr(GPtrArray) periods = calendar(fixture);
	g_autoptr(GError) error = NULL;
	VentureEntity *period = g_ptr_array_index(periods, 0);
	g_assert_true(change_state(fixture, period, VENTURE_PERIOD_CLOSED, "closer", &error));
	g_assert_no_error(error);
	g_assert_true(change_state(fixture, period, VENTURE_PERIOD_LOCKED, "closer", &error));
	g_assert_no_error(error);
	g_assert_false(change_state(fixture, period, VENTURE_PERIOD_OPEN, "owner", &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
	g_assert_nonnull(strstr(error->message, "locked"));
}

static VentureEntity *
dated_record(Fixture *fixture, const gchar *type_name, const gchar *date)
{
	GType type = venture_entity_registry_lookup(venture_entity_registry_get_default(), type_name);
	VentureEntity *entity;
	entity = (VENTURE_TYPE_LEDGER_ENTRY == type)
		? g_object_new(type, "transaction-id", "batch-1", "account-id", (gint64)1,
			"organization-id", fixture->organization_id, NULL)
		: financial_record(fixture, type, fixture->organization_id);
	g_assert_true(venture_entity_set_field_from_string(entity,
		(VENTURE_TYPE_INVOICE == type) ? "issued-at" : "occurred-at", date, NULL));
	if (VENTURE_TYPE_LEDGER_ENTRY == type)
		g_assert_true(venture_entity_set_field_from_string(entity, "amount", "10.00 USD", NULL));
	return entity;
}

static void
test_guard_writes(Fixture *fixture, gconstpointer data)
{
	g_autoptr(GPtrArray) periods = calendar(fixture);
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) entity = dated_record(fixture, data, "2024-01-15");
	VentureEntity *period = g_ptr_array_index(periods, 0);

	g_assert_true(change_state(fixture, period, VENTURE_PERIOD_CLOSED, "closer", &error));
	g_assert_no_error(error);
	g_assert_false(venture_database_save(fixture->database, entity, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
	g_assert_nonnull(strstr(error->message, "P01"));
	g_assert_nonnull(strstr(error->message, "closed"));
}

static void
test_close_check(Fixture *fixture, gconstpointer data)
{
	g_autoptr(GPtrArray) periods = calendar(fixture);
	g_autoptr(VentureEntity) entity = dated_record(fixture, data, "2024-01-15");
	g_autoptr(GError) error = NULL;
	g_assert_true(venture_database_save(fixture->database, entity, NULL, &error));
	g_assert_no_error(error);
	g_assert_false(change_state(fixture, g_ptr_array_index(periods, 0), VENTURE_PERIOD_CLOSED, "closer", &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_nonnull(strstr(error->message, (0 == strcmp(data, "invoice")) ? "draft" : "unbalanced"));
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
	g_test_add("/periods/records", Fixture, NULL,
		fixture_set_up, test_period_records, fixture_tear_down);
	g_test_add("/periods/calendar/monthly", Fixture, GINT_TO_POINTER(0),
		fixture_set_up, test_calendar_generation, fixture_tear_down);
	g_test_add("/periods/calendar/quarterly", Fixture, GINT_TO_POINTER(1),
		fixture_set_up, test_calendar_generation, fixture_tear_down);
	g_test_add("/periods/calendar/overlap", Fixture, "2024-07-01",
		fixture_set_up, test_calendar_refusal, fixture_tear_down);
	g_test_add("/periods/calendar/gap", Fixture, "2025-02-01",
		fixture_set_up, test_calendar_refusal, fixture_tear_down);
	g_test_add("/periods/close/order", Fixture, NULL, fixture_set_up, test_close_order, fixture_tear_down);
	g_test_add("/periods/close/stamps", Fixture, NULL, fixture_set_up, test_closed_stamps, fixture_tear_down);
	g_test_add("/periods/close/reopen", Fixture, NULL, fixture_set_up, test_reopen_permission, fixture_tear_down);
	g_test_add("/periods/close/locked", Fixture, NULL, fixture_set_up, test_locked, fixture_tear_down);
	g_test_add("/periods/guard/sale", Fixture, "sale", fixture_set_up, test_guard_writes, fixture_tear_down);
	g_test_add("/periods/guard/expense", Fixture, "expense", fixture_set_up, test_guard_writes, fixture_tear_down);
	g_test_add("/periods/guard/invoice", Fixture, "invoice", fixture_set_up, test_guard_writes, fixture_tear_down);
	g_test_add("/periods/guard/ledger", Fixture, "ledger_entry", fixture_set_up, test_guard_writes, fixture_tear_down);
	g_test_add("/periods/checks/ledger", Fixture, "ledger_entry", fixture_set_up, test_close_check, fixture_tear_down);
	g_test_add("/periods/checks/invoice", Fixture, "invoice", fixture_set_up, test_close_check, fixture_tear_down);
	return g_test_run();
}
