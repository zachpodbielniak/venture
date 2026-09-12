/*
 * test-periods.c - Fiscal boundaries and reproducible financial records
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <venture.h>
#include <libsoup/soup.h>
#include <unistd.h>
#include "venture-test-util.h"

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

static VentureReportResult *
run_report(Fixture *fixture, const gchar *name, const gchar *as_of)
{
	g_autoptr(VentureDateRange) range = venture_date_range_new_month(2024, 1, NULL);
	g_autoptr(JsonObject) options = json_object_new();
	g_autoptr(GError) error = NULL;
	VentureReport *report = venture_report_registry_lookup(venture_context_get_report_registry(fixture->context), name);
	VentureReportResult *result;
	g_assert_nonnull(report);
	json_object_set_int_member(options, "organization_id", fixture->organization_id);
	if (NULL != as_of)
		json_object_set_string_member(options, "as_of", as_of);
	result = venture_report_generate(report, fixture->context, range, options, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	return result;
}

static JsonNode *
report_metrics(VentureReportResult *result)
{
	/* Some financial reports express their totals as table rows. */
	return venture_report_result_to_json(result);
}

static void
test_historical_reports(Fixture *fixture, gconstpointer data)
{
	g_autoptr(VentureProduct) product = venture_product_new();
	g_autoptr(VentureEntity) sale = dated_record(fixture, "sale", "2024-01-15");
	g_autoptr(VentureEntity) expense = dated_record(fixture, "expense", "2024-01-16");
	g_autoptr(VentureReportResult) before = NULL;
	g_autoptr(VentureReportResult) after = NULL;
	g_autoptr(VentureReportResult) live = NULL;
	g_autoptr(JsonNode) a = NULL;
	g_autoptr(JsonNode) b = NULL;
	g_autoptr(JsonNode) c = NULL;
	g_autoptr(GError) error = NULL;

	g_object_set(product, "name", "Notebook", "genre", "Stationery", "venture-id", fixture->venture_id,
		"organization-id", fixture->organization_id, NULL);
	g_assert_true(venture_database_save(fixture->database, VENTURE_ENTITY(product), NULL, &error));
	g_assert_no_error(error);
	g_object_set(sale, "product-id", venture_entity_get_id(VENTURE_ENTITY(product)), NULL);
	g_assert_true(venture_entity_set_field_from_string(sale, "gross", "100.00 USD", NULL));
	g_assert_true(venture_entity_set_field_from_string(expense, "amount", "30.00 USD", NULL));
	g_object_set(expense, "deductibility", VENTURE_DEDUCTIBILITY_FULL, "category", "Paper", "venture-id", fixture->venture_id, NULL);
	g_assert_true(venture_database_save(fixture->database, sale, NULL, &error));
	g_assert_no_error(error);
	g_assert_true(venture_database_save(fixture->database, expense, NULL, &error));
	g_assert_no_error(error);
	before = run_report(fixture, data, "2024-01-31");
	g_assert_true(venture_database_delete(fixture->database, sale, NULL, &error));
	g_assert_no_error(error);
	g_assert_true(venture_database_delete(fixture->database, expense, NULL, &error));
	g_assert_no_error(error);
	after = run_report(fixture, data, "2024-01-31");
	live = run_report(fixture, data, NULL);
	a = report_metrics(before);
	b = report_metrics(after);
	c = report_metrics(live);
	g_assert_true(json_node_equal(a, b));
	g_assert_false(json_node_equal(a, c));
}

static void
test_historical_receivables(Fixture *fixture, gconstpointer data)
{
	g_autoptr(VentureEntity) invoice = dated_record(fixture, "invoice", "2024-01-15");
	g_autoptr(VentureInvoiceLine) line = venture_invoice_line_new();
	g_autoptr(VentureReportResult) before = NULL;
	g_autoptr(VentureReportResult) after = NULL;
	g_autoptr(JsonNode) a = NULL;
	g_autoptr(JsonNode) b = NULL;
	g_autoptr(GError) error = NULL;
	g_object_set(invoice, "status", VENTURE_INVOICE_STATUS_SENT, NULL);
	g_assert_true(venture_database_save(fixture->database, invoice, NULL, &error));
	g_assert_no_error(error);
	g_object_set(line, "invoice-id", venture_entity_get_id(invoice), "description", "Paper", "quantity", 1.0, NULL);
	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(line), "unit-price", "100.00 USD", NULL));
	g_assert_true(venture_database_save(fixture->database, VENTURE_ENTITY(line), NULL, &error));
	g_assert_no_error(error);
	before = run_report(fixture, "receivables", "2024-01-31");
	if (GPOINTER_TO_INT(data))
	{
		g_object_set(invoice, "status", VENTURE_INVOICE_STATUS_PAID, NULL);
		g_assert_true(venture_entity_set_field_from_string(invoice, "paid-at", "2024-02-10", NULL));
		g_assert_true(venture_database_save(fixture->database, invoice, NULL, &error));
	}
	else
	{
		g_assert_true(venture_database_delete(fixture->database, VENTURE_ENTITY(line), NULL, &error));
		g_assert_no_error(error);
		g_assert_true(venture_database_delete(fixture->database, invoice, NULL, &error));
	}
	g_assert_no_error(error);
	after = run_report(fixture, "receivables", "2024-01-31");
	a = report_metrics(before);
	b = report_metrics(after);
	g_assert_true(json_node_equal(a, b));
}

static void
test_snapshots(Fixture *fixture, gconstpointer data)
{
	g_autoptr(VentureProduct) product = venture_product_new();
	g_autoptr(GPtrArray) periods = calendar(fixture);
	g_autoptr(VentureEntity) sale = dated_record(fixture, "sale", "2024-01-15");
	g_autoptr(VentureUser) owner = venture_user_new();
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_REPORT_SNAPSHOT);
	g_autoptr(GPtrArray) snapshots = NULL;
	g_autoptr(GError) error = NULL;
	g_object_set(product, "name", "Notebook", "genre", "Stationery", "venture-id", fixture->venture_id,
		"organization-id", fixture->organization_id, NULL);
	g_assert_true(venture_database_save(fixture->database, VENTURE_ENTITY(product), NULL, &error));
	g_assert_no_error(error);
	g_object_set(sale, "product-id", venture_entity_get_id(VENTURE_ENTITY(product)), NULL);
	g_assert_true(venture_entity_set_field_from_string(sale, "gross", "100.00 USD", NULL));
	g_assert_true(venture_database_save(fixture->database, sale, NULL, &error));
	g_assert_no_error(error);
	g_assert_true(change_state(fixture, g_ptr_array_index(periods, 0), VENTURE_PERIOD_CLOSED, "closer", &error));
	g_assert_no_error(error);
	snapshots = venture_database_find(fixture->database, query, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(snapshots->len, >=, 6);
	{
		g_autoptr(VentureReportResult) report = run_report(fixture, "snapshot_vs_live", NULL);
		g_assert_cmpuint(venture_report_result_get_row_count(report), ==, 0);
	}
	/* Reopening permits a correction, but cannot rewrite the evidence of
	 * what the previous close reported. */
	g_object_set(owner, "username", "owner", "role", VENTURE_USER_ROLE_OWNER, "active", TRUE, NULL);
	g_assert_true(venture_database_save(fixture->database, VENTURE_ENTITY(owner), NULL, &error));
	g_assert_no_error(error);
	g_assert_true(change_state(fixture, g_ptr_array_index(periods, 0), VENTURE_PERIOD_OPEN, "owner", &error));
	g_assert_no_error(error);
	g_assert_true(venture_database_delete(fixture->database, sale, NULL, &error));
	g_assert_no_error(error);
	{
		g_autoptr(VentureReportResult) report = run_report(fixture, "snapshot_vs_live", NULL);
		/* This sale has no product: P&L, ventures and monthly change. */
		g_assert_cmpuint(venture_report_result_get_row_count(report), >, 0);
	}
	g_object_set(g_ptr_array_index(snapshots, 0), "report", "forged", NULL);
	g_assert_false(venture_database_save(fixture->database, g_ptr_array_index(snapshots, 0), NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
}

static void
test_historical_issuance(Fixture *fixture, gconstpointer data)
{
	g_autoptr(VentureEntity) invoice = dated_record(fixture, "invoice", "2024-02-01");
	g_autoptr(VentureReportResult) before = run_report(fixture, "receivables", "2024-01-31");
	g_autoptr(VentureReportResult) after = NULL;
	g_autoptr(JsonNode) a = report_metrics(before);
	g_autoptr(JsonNode) b = NULL;
	g_object_set(invoice, "status", VENTURE_INVOICE_STATUS_SENT, NULL);
	g_assert_true(venture_database_save(fixture->database, invoice, NULL, NULL));
	after = run_report(fixture, "receivables", "2024-01-31");
	b = report_metrics(after);
	g_assert_true(json_node_equal(a, b));
}

static VentureReportResult *
failed_close_report(VentureContext *context, VentureDateRange *range,
	JsonObject *options, GError **error)
{
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Financial plugin unavailable");
	return NULL;
}

static void
test_snapshot_atomic(Fixture *fixture, gconstpointer data)
{
	g_autoptr(GPtrArray) periods = calendar(fixture);
	g_autoptr(VentureEntity) stored = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_REPORT_SNAPSHOT);
	g_autoptr(GError) error = NULL;
	VentureFuncReport *report = venture_func_report_new("failing_financial", "Failing report", NULL, failed_close_report);
	gint state;
	g_object_set(report, "financial", TRUE, NULL);
	venture_report_registry_add(venture_context_get_report_registry(fixture->context), VENTURE_REPORT(report));
	g_assert_false(change_state(fixture, g_ptr_array_index(periods, 0), VENTURE_PERIOD_CLOSED, "closer", &error));
	g_assert_nonnull(error);
	g_assert_nonnull(strstr(error->message, "Financial plugin"));
	g_clear_error(&error);
	stored = venture_database_get(fixture->database, VENTURE_TYPE_FISCAL_PERIOD,
		venture_entity_get_id(g_ptr_array_index(periods, 0)), &error);
	g_assert_no_error(error);
	g_object_get(stored, "state", &state, NULL);
	g_assert_cmpint(state, ==, VENTURE_PERIOD_OPEN);
	g_assert_cmpint(venture_database_count(fixture->database, query, &error), ==, 0);
	g_assert_no_error(error);
}

static void
test_drift_report_registered(Fixture *fixture, gconstpointer data)
{
	g_assert_nonnull(venture_report_registry_lookup(
		venture_context_get_report_registry(fixture->context), "snapshot_vs_live"));
}

typedef struct
{
	VentureWebServer *server;
	SoupSession *session;
	gchar *directory;
	gchar *url;
} TestHttp;

static void
test_http_clear(TestHttp *http)
{
	if (NULL != http->server)
		venture_web_server_stop(http->server);
	g_clear_object(&http->session);
	g_clear_object(&http->server);
	if (NULL != http->directory)
		venture_test_remove_tree(http->directory);
	g_clear_pointer(&http->directory, g_free);
	g_clear_pointer(&http->url, g_free);
}
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(TestHttp, test_http_clear)

static void
test_http_start(Fixture *fixture, TestHttp *http)
{
	g_autoptr(GError) error = NULL;
	guint port = 20000 + getpid() % 20000;
	http->directory = g_dir_make_tmp("venture-period-http-XXXXXX", &error);
	g_assert_no_error(error);
	g_object_set(fixture->config, "state-dir", http->directory,
		"server-bind-address", "127.0.0.1", "server-port", (gint64)port,
		"security-require-auth", FALSE, NULL);
	http->server = venture_web_server_new(fixture->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(http->server, &error));
	g_assert_no_error(error);
	http->session = soup_session_new();
	http->url = g_strdup_printf("http://127.0.0.1:%u", port);
}

typedef struct
{
	gboolean done;
	GBytes *body;
	GError *error;
	gchar *output;
	gchar *diagnostic;
} TestAsync;

static void
test_request_done(GObject *object, GAsyncResult *result, gpointer data)
{
	TestAsync *state = data;
	state->body = soup_session_send_and_read_finish(SOUP_SESSION(object), result, &state->error);
	state->done = TRUE;
}

static gchar *
test_request(TestHttp *http, const gchar *method, const gchar *path,
	const gchar *content_type, const gchar *body, guint *status)
{
	g_autofree gchar *url = g_strconcat(http->url, path, NULL);
	g_autoptr(SoupMessage) message = soup_message_new(method, url);
	TestAsync state = { FALSE, NULL, NULL, NULL, NULL };
	gchar *response;
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	if (NULL != body)
	{
		g_autoptr(GBytes) bytes = g_bytes_new(body, strlen(body));
		soup_message_set_request_body_from_bytes(message, content_type, bytes);
	}
	soup_session_send_and_read_async(http->session, message, G_PRIORITY_DEFAULT, NULL, test_request_done, &state);
	while (!state.done)
		g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(state.error);
	*status = soup_message_get_status(message);
	response = g_strndup(g_bytes_get_data(state.body, NULL), g_bytes_get_size(state.body));
	g_bytes_unref(state.body);
	return response;
}

static void
test_cli_done(GObject *object, GAsyncResult *result, gpointer data)
{
	TestAsync *state = data;
	g_subprocess_communicate_utf8_finish(G_SUBPROCESS(object), result,
		&state->output, &state->diagnostic, &state->error);
	state->done = TRUE;
}

static gchar *
test_cli(const gchar *const *argv, gint *status)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GSubprocess) child = g_subprocess_newv(argv,
		G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE, &error);
	TestAsync state = { FALSE, NULL, NULL, NULL, NULL };
	gchar *combined;
	g_assert_no_error(error);
	g_subprocess_communicate_utf8_async(child, NULL, NULL, test_cli_done, &state);
	while (!state.done)
		g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(state.error);
	*status = g_subprocess_get_exit_status(child);
	combined = g_strconcat(state.output, state.diagnostic, NULL);
	g_free(state.output);
	g_free(state.diagnostic);
	return combined;
}

/* Real web and REST requests, a real venturectl process, and the actual
 * AI confirmation executor must all refuse every financial record type. */
static void
test_write_surface(Fixture *fixture, gconstpointer data)
{
	static const gchar *const types[] = { "sale", "expense", "invoice", "ledger_entry" };
	g_autoptr(GPtrArray) periods = calendar(fixture);
	g_auto(TestHttp) http = { NULL, NULL, NULL, NULL };
	g_autoptr(GError) error = NULL;
	gint surface = GPOINTER_TO_INT(data);
	guint state;
	guint i;
	if (3 != surface)
		test_http_start(fixture, &http);
	for (state = VENTURE_PERIOD_CLOSED; state <= VENTURE_PERIOD_LOCKED; state++)
	{
		const gchar *state_name = (VENTURE_PERIOD_CLOSED == state) ? "closed" : "locked";
		g_assert_true(change_state(fixture, g_ptr_array_index(periods, 0), state, "closer", &error));
		g_assert_no_error(error);
		for (i = 0; i < G_N_ELEMENTS(types); i++)
		{
			g_autoptr(VentureEntity) entity = dated_record(fixture, types[i], "2024-01-15");
			g_autofree gchar *response = NULL;
			if (3 == surface)
			{
				VentureActor actor = actor_named("assistant");
				VentureConfirmationStore *store = venture_context_get_confirmations(fixture->context);
				VentureConfirmation *confirmation;
				g_autofree gchar *id = NULL;
				actor.kind = VENTURE_ACTOR_KIND_AI;
				actor.prompt = "Record this purchase";
				confirmation = venture_confirmation_store_stage(store, VENTURE_AUDIT_ACTION_CREATE,
					entity, NULL, &actor, "assistant", &error);
				g_assert_no_error(error);
				g_assert_nonnull(confirmation);
				id = g_strdup(venture_confirmation_get_id(confirmation));
				g_assert_false(venture_confirmation_store_approve(store, id, "closer", &error));
				g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
				response = g_strdup(error->message);
				g_clear_error(&error);
			}
			else if (2 == surface)
			{
				g_autofree gchar *organization = g_strdup_printf("organization_id=%" G_GINT64_FORMAT, fixture->organization_id);
				g_autofree gchar *venture = g_strdup_printf("venture_id=%" G_GINT64_FORMAT, fixture->venture_id);
				const gchar *argv[] = { "build/debug/venturectl", "--server", http.url,
					"create", types[i], organization, "occurred_at=2024-01-15", NULL, NULL, NULL, NULL };
				gint status;
				if (0 == i) argv[7] = venture;
				else if (1 == i) argv[7] = "description=Paper";
				else if (2 == i) { argv[6] = "issued_at=2024-01-15"; argv[7] = "number=1"; }
				else { argv[7] = "transaction_id=batch-1"; argv[8] = "account_id=1"; argv[9] = "amount=10.00 USD"; }
				response = test_cli(argv, &status);
				g_assert_cmpint(status, ==, 4);
			}
			else
			{
				g_autofree gchar *path = g_strdup_printf((0 == surface) ? "/e/%s" : "/api/v1/%s", types[i]);
				g_autofree gchar *body = NULL;
				guint status;
				if (0 == surface)
					body = g_strdup_printf("organization_id=%" G_GINT64_FORMAT "&venture-id=%" G_GINT64_FORMAT
						"&occurred-at=2024-01-15&issued-at=2024-01-15&description=Paper&number=1&transaction-id=batch-1&account-id=1&amount=10.00+USD",
						fixture->organization_id, fixture->venture_id);
				else
				{
					g_autoptr(JsonNode) node = venture_serializable_to_json(VENTURE_SERIALIZABLE(entity), FALSE);
					body = venture_json_to_string(node, FALSE);
				}
				response = test_request(&http, "POST", path,
					(0 == surface) ? "application/x-www-form-urlencoded" : "application/json", body, &status);
				g_assert_cmpuint(status, ==, 409);
			}
			g_assert_nonnull(strstr(response, "P01"));
			g_assert_nonnull(strstr(response, state_name));
		}
	}
}

static void
test_report_surface(Fixture *fixture, gconstpointer data)
{
	g_auto(TestHttp) http = { NULL, NULL, NULL, NULL };
	g_autoptr(VentureEntity) sale = dated_record(fixture, "sale", "2024-01-15");
	g_autofree gchar *response = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GError) error = NULL;
	JsonArray *metrics;
	guint i;
	gboolean found = FALSE;
	g_assert_true(venture_entity_set_field_from_string(sale, "gross", "100.00 USD", NULL));
	g_assert_true(venture_database_save(fixture->database, sale, NULL, &error));
	g_assert_no_error(error);
	g_assert_true(venture_database_delete(fixture->database, sale, NULL, &error));
	g_assert_no_error(error);
	test_http_start(fixture, &http);
	if (GPOINTER_TO_INT(data))
	{
		const gchar *argv[] = { "build/debug/venturectl", "--server", http.url,
			"report", "pnl", "2024-01", "as_of=2024-01-31", NULL };
		gint status;
		response = test_cli(argv, &status);
		g_assert_cmpint(status, ==, 0);
	}
	else
	{
		guint status;
		response = test_request(&http, "GET", "/api/v1/reports/pnl?period=2024-01&as_of=2024-01-31", NULL, NULL, &status);
		g_assert_cmpuint(status, ==, 200);
	}
	node = venture_json_parse(response, &error);
	g_assert_no_error(error);
	metrics = json_object_get_array_member(json_node_get_object(node), "metrics");
	for (i = 0; i < json_array_get_length(metrics); i++)
	{
		JsonObject *metric = json_array_get_object_element(metrics, i);
		if (0 == strcmp(json_object_get_string_member(metric, "key"), "revenue"))
		{
			JsonObject *money = json_object_get_object_member(metric, "value");
			g_assert_cmpint(json_object_get_int_member(money, "amount"), ==, 10000);
			found = TRUE;
		}
	}
	g_assert_true(found);
}

typedef struct { GObject parent_instance; } TestPeriodPlugin;
typedef struct { GObjectClass parent_class; } TestPeriodPluginClass;
GType test_period_plugin_get_type(void);
static const gchar *plugin_check_name(VenturePeriodCheck *self) { return "External reconciliation"; }
static gboolean plugin_check_run(VenturePeriodCheck *self, VentureDatabase *database,
	VentureEntity *period, GError **error)
{
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Reconciliation incomplete");
	return FALSE;
}
static gboolean plugin_guard_run(VenturePeriodGuard *self, VentureDatabase *database,
	gint64 organization_id, GDateTime *date, GError **error)
{
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT, "External posting restriction");
	return FALSE;
}
static void plugin_check_iface(VenturePeriodCheckInterface *iface)
{ iface->get_name = plugin_check_name; iface->run = plugin_check_run; }
static void plugin_guard_iface(VenturePeriodGuardInterface *iface)
{ iface->is_postable = plugin_guard_run; }
G_DEFINE_TYPE_WITH_CODE(TestPeriodPlugin, test_period_plugin, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(VENTURE_TYPE_PERIOD_CHECK, plugin_check_iface)
	G_IMPLEMENT_INTERFACE(VENTURE_TYPE_PERIOD_GUARD, plugin_guard_iface))
static void test_period_plugin_class_init(TestPeriodPluginClass *klass) { }
static void test_period_plugin_init(TestPeriodPlugin *self) { }
static void collect_plugin_check(VenturePeriodChecklist *self, GPtrArray *checks, gpointer data)
{ g_ptr_array_add(checks, g_object_new(test_period_plugin_get_type(), NULL)); }

static void
test_check_extension(Fixture *fixture, gconstpointer data)
{
	g_autoptr(GPtrArray) periods = calendar(fixture);
	g_autoptr(VentureEntity) invoice = dated_record(fixture, "invoice", "2024-01-15");
	g_autoptr(VentureEntity) entry = dated_record(fixture, "ledger_entry", "2024-01-15");
	g_autoptr(GError) error = NULL;
	VenturePeriodChecklist *checklist = venture_period_service_get_checklist(venture_period_service_get(fixture->database));
	g_signal_connect(checklist, "collect-checks", G_CALLBACK(collect_plugin_check), NULL);
	g_assert_true(venture_database_save(fixture->database, invoice, NULL, &error));
	g_assert_no_error(error);
	g_assert_true(venture_database_save(fixture->database, entry, NULL, &error));
	g_assert_no_error(error);
	g_assert_false(change_state(fixture, g_ptr_array_index(periods, 0), VENTURE_PERIOD_CLOSED, "closer", &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_nonnull(strstr(error->message, "External reconciliation"));
	g_assert_nonnull(strstr(error->message, "unbalanced"));
	g_assert_nonnull(strstr(error->message, "draft"));
}

static void
test_guard_extension_and_switch(Fixture *fixture, gconstpointer data)
{
	g_autoptr(GPtrArray) periods = calendar(fixture);
	g_autoptr(GDateTime) date = venture_time_from_string("2024-01-15", NULL);
	g_autoptr(GError) error = NULL;
	VenturePeriodGuardRegistry *registry = venture_database_get_period_guard(fixture->database);
	g_assert_true(change_state(fixture, g_ptr_array_index(periods, 0), VENTURE_PERIOD_CLOSED, "closer", &error));
	g_assert_no_error(error);
	g_assert_false(venture_period_guard_is_postable(VENTURE_PERIOD_GUARD(registry), fixture->database,
		fixture->organization_id, date, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
	g_clear_error(&error);
	venture_config_set_module_enabled(fixture->config, "periods", FALSE);
	g_assert_cmpuint(venture_entity_registry_lookup(venture_entity_registry_get_default(), "fiscal_period"), ==, G_TYPE_INVALID);
	g_assert_null(venture_report_registry_lookup(venture_context_get_report_registry(fixture->context), "snapshot_vs_live"));
	g_assert_true(venture_period_guard_is_postable(VENTURE_PERIOD_GUARD(registry), fixture->database,
		fixture->organization_id, date, &error));
	g_assert_no_error(error);
	venture_period_guard_registry_add(registry, g_object_new(test_period_plugin_get_type(), NULL));
	g_assert_false(venture_period_guard_is_postable(VENTURE_PERIOD_GUARD(registry), fixture->database,
		fixture->organization_id, date, &error));
	g_assert_nonnull(strstr(error->message, "External posting"));
	venture_config_set_module_enabled(fixture->config, "periods", TRUE);
}

static void
test_guard_mutations(Fixture *fixture, gconstpointer data)
{
	g_autoptr(GPtrArray) periods = calendar(fixture);
	g_autoptr(VentureEntity) sale = dated_record(fixture, "sale", "2024-01-15");
	g_autoptr(GError) error = NULL;
	g_assert_true(venture_database_save(fixture->database, sale, NULL, &error));
	g_assert_no_error(error);
	if (GPOINTER_TO_INT(data))
	{
		g_assert_true(venture_database_delete(fixture->database, sale, NULL, &error));
		g_assert_no_error(error);
	}
	g_assert_true(change_state(fixture, g_ptr_array_index(periods, 0), VENTURE_PERIOD_CLOSED, "closer", &error));
	g_assert_no_error(error);
	if (GPOINTER_TO_INT(data))
		g_assert_false(venture_database_restore(fixture->database, sale, NULL, &error));
	else
		g_assert_false(venture_database_delete(fixture->database, sale, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
	g_clear_error(&error);
	g_assert_true(venture_entity_set_field_from_string(sale, "occurred-at", "2024-02-15", NULL));
	g_object_set(sale, "organization-id", fixture->other_id, NULL);
	g_assert_false(venture_database_save(fixture->database, sale, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
}

static void
test_duplicate_csv(Fixture *fixture, gconstpointer data)
{
	g_auto(TestHttp) http = { NULL, NULL, NULL, NULL };
	g_autoptr(VentureEntity) record = dated_record(fixture, data, "2024-01-15");
	g_autoptr(VentureQuery) query = venture_query_new(G_OBJECT_TYPE(record));
	g_autofree gchar *path = g_strdup_printf("/e/%s/import", (gchar *)data);
	g_autofree gchar *body = NULL;
	g_autofree gchar *response = NULL;
	g_autoptr(GError) error = NULL;
	guint status;
	g_assert_true(venture_database_save(fixture->database, record, NULL, &error));
	g_assert_no_error(error);
	test_http_start(fixture, &http);
	body = g_strdup_printf("--PERIODCSV\r\nContent-Disposition: form-data; name=\"file\"; filename=\"import.csv\"\r\n"
		"Content-Type: text/csv\r\n\r\norganization_id,venture_id,description,external_id\r\n"
		"%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT ",Paper,new-id\r\n"
		"%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT ",Paper,import-42\r\n\r\n--PERIODCSV--\r\n",
		fixture->organization_id, fixture->venture_id, fixture->organization_id, fixture->venture_id);
	response = test_request(&http, "POST", path, "multipart/form-data; boundary=PERIODCSV", body, &status);
	g_assert_cmpuint(status, >=, 400);
	g_assert_nonnull(strstr(response, "external_id"));
	g_assert_cmpint(venture_database_count(fixture->database, query, &error), ==, 1);
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
	g_test_add("/periods/history/pnl", Fixture, "pnl", fixture_set_up, test_historical_reports, fixture_tear_down);
	g_test_add("/periods/history/tax", Fixture, "tax", fixture_set_up, test_historical_reports, fixture_tear_down);
	g_test_add("/periods/history/monthly", Fixture, "monthly", fixture_set_up, test_historical_reports, fixture_tear_down);
	g_test_add("/periods/history/ventures", Fixture, "ventures", fixture_set_up, test_historical_reports, fixture_tear_down);
	g_test_add("/periods/history/categories", Fixture, "categories", fixture_set_up, test_historical_reports, fixture_tear_down);
	g_test_add("/periods/history/receivables-deleted", Fixture, GINT_TO_POINTER(0), fixture_set_up, test_historical_receivables, fixture_tear_down);
	g_test_add("/periods/history/receivables-paid", Fixture, GINT_TO_POINTER(1), fixture_set_up, test_historical_receivables, fixture_tear_down);
	g_test_add("/periods/history/receivables-issuance", Fixture, NULL, fixture_set_up, test_historical_issuance, fixture_tear_down);
	g_test_add("/periods/snapshots/store", Fixture, NULL, fixture_set_up, test_snapshots, fixture_tear_down);
	g_test_add("/periods/snapshots/atomic", Fixture, NULL, fixture_set_up, test_snapshot_atomic, fixture_tear_down);
	g_test_add("/periods/snapshots/report", Fixture, NULL, fixture_set_up, test_drift_report_registered, fixture_tear_down);
	g_test_add("/periods/surfaces/web", Fixture, GINT_TO_POINTER(0), fixture_set_up, test_write_surface, fixture_tear_down);
	g_test_add("/periods/surfaces/rest", Fixture, GINT_TO_POINTER(1), fixture_set_up, test_write_surface, fixture_tear_down);
	g_test_add("/periods/surfaces/cli", Fixture, GINT_TO_POINTER(2), fixture_set_up, test_write_surface, fixture_tear_down);
	g_test_add("/periods/surfaces/ai", Fixture, GINT_TO_POINTER(3), fixture_set_up, test_write_surface, fixture_tear_down);
	g_test_add("/periods/report-surfaces/rest", Fixture, GINT_TO_POINTER(0), fixture_set_up, test_report_surface, fixture_tear_down);
	g_test_add("/periods/report-surfaces/cli", Fixture, GINT_TO_POINTER(1), fixture_set_up, test_report_surface, fixture_tear_down);
	g_test_add("/periods/checks/extension", Fixture, NULL, fixture_set_up, test_check_extension, fixture_tear_down);
	g_test_add("/periods/guard/extension-switch", Fixture, NULL, fixture_set_up, test_guard_extension_and_switch, fixture_tear_down);
	g_test_add("/periods/guard/delete", Fixture, GINT_TO_POINTER(0), fixture_set_up, test_guard_mutations, fixture_tear_down);
	g_test_add("/periods/guard/restore", Fixture, GINT_TO_POINTER(1), fixture_set_up, test_guard_mutations, fixture_tear_down);
	g_test_add("/periods/import/sale", Fixture, "sale", fixture_set_up, test_duplicate_csv, fixture_tear_down);
	g_test_add("/periods/import/expense", Fixture, "expense", fixture_set_up, test_duplicate_csv, fixture_tear_down);
	return g_test_run();
}
