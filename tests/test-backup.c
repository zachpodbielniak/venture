/*
 * test-backup.c - Operator export/restore of an organization accounting pack.
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <venture.h>
#include <string.h>
#include <libsoup/soup.h>
#include "venture-test-util.h"
#include "venture-test-accounting.h"

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
	f->config = venture_config_new();
	f->db = venture_test_accounting_database(&error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->db);
	f->org = venture_context_get_default_organization_id(f->context);
}

static void
teardown(Fixture *f, gconstpointer data)
{
	(void)data;
	g_clear_object(&f->context);
	venture_test_accounting_database_cleanup(f->db);
	g_clear_object(&f->db);
	g_clear_object(&f->config);
}

static gint64
account_id(Fixture *f, const gchar *code)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ACCOUNT);
	g_autoptr(VentureEntity) row = NULL;
	venture_query_add_filter_string(query, "code", VENTURE_FILTER_OP_EQ, code, NULL);
	row = venture_database_find_one(f->db, query, NULL);
	return venture_entity_get_id(row);
}

static void
seed_books(Fixture *f, gboolean documents)
{
	g_autoptr(VentureJournal) journal = venture_journal_new();
	g_autoptr(VentureJournalLine) debit = venture_journal_line_new();
	g_autoptr(VentureJournalLine) credit = venture_journal_line_new();
	g_autoptr(GPtrArray) lines = g_ptr_array_new_with_free_func(g_object_unref);
	g_autoptr(GDateTime) when = g_date_time_new_from_iso8601("2026-08-10T00:00:00Z", NULL);
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureCompany) customer = venture_company_new();
	g_autoptr(VentureInvoice) invoice = venture_invoice_new();
	g_autoptr(VentureInvoiceLine) iline = venture_invoice_line_new();
	g_object_set(journal, "source-type", "organization", "source-id", f->org,
		"occurred-at", when, "currency", "USD", "organization-id", f->org, NULL);
	g_object_set(debit, "account-id", account_id(f, "1000"), "side", VENTURE_LEDGER_SIDE_DEBIT,
		"organization-id", f->org, NULL);
	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(debit), "amount", "50 USD", NULL));
	g_object_set(credit, "account-id", account_id(f, "4000"), "side", VENTURE_LEDGER_SIDE_CREDIT,
		"organization-id", f->org, NULL);
	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(credit), "amount", "50 USD", NULL));
	g_ptr_array_add(lines, g_object_ref(debit));
	g_ptr_array_add(lines, g_object_ref(credit));
	g_assert_nonnull(venture_posting_service_post(venture_database_get_posting_service(f->db),
		journal, lines, NULL, NULL, &error));
	if (!documents)
		return;
	g_object_set(customer, "name", "Acme", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(customer), f->org);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(customer), NULL, &error));
	venture_entity_set_organization_id(VENTURE_ENTITY(invoice), f->org);
	g_object_set(invoice, "number", "INV-BAK", "company-id",
		venture_entity_get_id(VENTURE_ENTITY(customer)), NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(invoice), NULL, &error));
	venture_entity_set_organization_id(VENTURE_ENTITY(iline), f->org);
	g_object_set(iline, "invoice-id", venture_entity_get_id(VENTURE_ENTITY(invoice)),
		"description", "Work", "quantity", 1.0, NULL);
	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(iline), "unit-price", "50 USD", NULL));
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(iline), NULL, &error));
	{
		g_autoptr(GDateTime) now = venture_time_now();
		g_autoptr(VenturePayment) payment = venture_payment_new();
		g_assert_true(venture_settlement_service_transition(venture_settlement_service_get(f->db),
			invoice, "sent", now, NULL, &error));
		g_assert_no_error(error);
		venture_entity_set_organization_id(VENTURE_ENTITY(payment), f->org);
		g_object_set(payment, "customer-id", venture_entity_get_id(VENTURE_ENTITY(customer)),
			"date", now, "invoice-id", venture_entity_get_id(VENTURE_ENTITY(invoice)),
			"method", "transfer", "reference", "wire-1", NULL);
		g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(payment), "amount", "50 USD", NULL));
		g_assert_true(venture_settlement_service_apply_payment(venture_settlement_service_get(f->db),
			payment, NULL, NULL, &error));
		g_assert_no_error(error);
	}
}

static gint64
count_type(Fixture *f, gint64 org, GType type)
{
	g_autoptr(VentureQuery) query = venture_query_new(type);
	venture_query_set_organization(query, org);
	venture_query_set_limit(query, 0);
	return venture_database_count(f->db, query, NULL);
}

static void
test_export_restore_empty_org(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) backup = NULL;
	g_autoptr(VentureOrganization) empty = venture_organization_new();
	g_autofree gchar *payload = NULL;
	gint64 dest;
	VentureActor actor;
	(void)data;
	actor.kind = VENTURE_ACTOR_KIND_USER;
	actor.name = "operator";
	actor.prompt = NULL;
	actor.request_id = NULL;
	actor.approved_by = NULL;
	seed_books(f, TRUE);
	backup = venture_backup_service_export(venture_backup_service_get(f->db),
		f->org, "json", &actor, &error);
	g_assert_no_error(error);
	g_object_get(backup, "payload", &payload, NULL);
	g_assert_nonnull(strstr(payload, "\"journal\""));
	g_assert_nonnull(strstr(payload, "\"invoice\""));
	g_object_set(empty, "name", "Empty books", "legal-name", "Empty books",
		"default-currency", "USD", NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(empty), NULL, &error));
	dest = venture_entity_get_id(VENTURE_ENTITY(empty));
	/* Historical restore preserves the posted evidence once; it must not
	 * issue the invoice or collect its payment a second time. */
	g_assert_true(venture_backup_service_restore(venture_backup_service_get(f->db),
		dest, payload, &actor, &error));
	g_assert_no_error(error);
	g_assert_cmpint(count_type(f, dest, VENTURE_TYPE_JOURNAL), ==, count_type(f, f->org, VENTURE_TYPE_JOURNAL));
	g_assert_cmpint(count_type(f, dest, VENTURE_TYPE_INVOICE), ==, 1);
	g_assert_cmpint(count_type(f, dest, VENTURE_TYPE_PAYMENT), ==, 1);
	g_assert_cmpint(count_type(f, dest, VENTURE_TYPE_ACCOUNT), ==, count_type(f, f->org, VENTURE_TYPE_ACCOUNT));

}

static void
test_csv_export(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) backup = NULL;
	g_autofree gchar *payload = NULL;
	VentureActor actor;
	(void)data;
	actor.kind = VENTURE_ACTOR_KIND_USER;
	actor.name = "operator";
	actor.prompt = NULL;
	actor.request_id = NULL;
	actor.approved_by = NULL;
	seed_books(f, TRUE);
	backup = venture_backup_service_export(venture_backup_service_get(f->db),
		f->org, "csv", &actor, &error);
	g_assert_no_error(error);
	g_object_get(backup, "payload", &payload, NULL);
	g_assert_nonnull(strstr(payload, "journal,"));
	g_assert_nonnull(strstr(payload, "invoice,"));
}

static void
test_manual_import(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) backup = NULL;
	g_autoptr(VentureOrganization) empty = venture_organization_new();
	g_autofree gchar *payload = NULL;
	gint64 dest;
	(void)data;
	seed_books(f, FALSE);
	backup = venture_backup_service_export(venture_backup_service_get(f->db), f->org, "json", NULL, &error);
	g_assert_no_error(error);
	g_object_get(backup, "payload", &payload, NULL);
	g_object_set(empty, "name", "Opening books", "legal-name", "Opening books", "default-currency", "USD", NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(empty), NULL, &error));
	dest = venture_entity_get_id(VENTURE_ENTITY(empty));
	g_assert_true(venture_backup_service_restore(venture_backup_service_get(f->db), dest, payload, NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpint(count_type(f, dest, VENTURE_TYPE_JOURNAL), ==, 1);
	g_assert_cmpint(count_type(f, dest, VENTURE_TYPE_ACCOUNT), ==, count_type(f, f->org, VENTURE_TYPE_ACCOUNT));
}

static gboolean
reject_restore_evidence(VentureDatabase *database, VentureEntity *record, VentureEntity *previous,
	gpointer data, GError **error)
{
	g_autofree gchar *state = NULL;
	(void)database; (void)previous; (void)data;
	g_object_get(record, "state", &state, NULL);
	if (g_strcmp0(state, "restored") == 0)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Injected restore evidence failure");
		return FALSE;
	}
	return TRUE;
}

static void
test_invalid_archive(Fixture *f, gconstpointer data)
{
	const gchar *mode = data;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) backup = NULL;
	g_autoptr(VentureOrganization) destination = venture_organization_new();
	g_autofree gchar *payload = NULL, *changed = NULL;
	g_autoptr(JsonNode) root = NULL;
	JsonArray *records;
	gint64 dest;
	guint i;
	seed_books(f, TRUE);
	backup = venture_backup_service_export(venture_backup_service_get(f->db), f->org, "json", NULL, &error);
	g_assert_no_error(error);
	g_object_get(backup, "payload", &payload, NULL);
	root = venture_json_parse(payload, &error);
	g_assert_no_error(error);
	records = json_object_get_array_member(json_node_get_object(root), "records");
	if (g_str_equal(mode, "duplicate"))
		json_array_add_element(records, json_node_copy(json_array_get_element(records, 0)));
	else if (g_str_equal(mode, "attributes"))
		json_object_set_int_member(json_object_get_object_member(json_array_get_object_element(records, 0), "record"), "attributes", 42);
	else if (g_str_equal(mode, "duplicate-uuid"))
	{
		JsonObject *first = json_object_get_object_member(json_array_get_object_element(records, 0), "record");
		JsonObject *second = json_object_get_object_member(json_array_get_object_element(records, 1), "record");
		json_object_set_string_member(second, "uuid", json_object_get_string_member(first, "uuid"));
	}
	else if (g_str_equal(mode, "missing-field"))
		json_object_remove_member(json_object_get_object_member(json_array_get_object_element(records, 0), "record"), "version");
	else if (g_str_equal(mode, "missing-customer") || g_str_equal(mode, "missing-projection"))
	{
		for (i = 0; i < json_array_get_length(records); i++)
			if (g_str_equal(json_object_get_string_member(json_array_get_object_element(records, i), "type"), g_str_equal(mode, "missing-customer") ? "company" : "ledger_entry"))
			{
				json_array_remove_element(records, i);
				break;
			}
	}
	else if (g_str_equal(mode, "unbalanced"))
	{
		for (i = 0; i < json_array_get_length(records); i++)
			if (g_str_equal(json_object_get_string_member(json_array_get_object_element(records, i), "type"), "journal_line"))
			{
				JsonObject *record = json_object_get_object_member(json_array_get_object_element(records, i), "record");
				g_autoptr(VentureJournalLine) line = venture_journal_line_new();
				g_autoptr(JsonNode) serialized = NULL;
				g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(line), "book-amount", "999 USD", &error));
				serialized = venture_serializable_to_json(VENTURE_SERIALIZABLE(line), FALSE);
				json_object_set_member(record, "book_amount", json_node_copy(json_object_get_member(json_node_get_object(serialized), "book_amount")));
				break;
			}
	}
	else
		venture_database_add_save_validator(f->db, VENTURE_TYPE_ACCOUNTING_BACKUP, reject_restore_evidence, NULL, NULL);
	changed = venture_json_to_string(root, FALSE);
	g_object_set(destination, "name", "Rollback target", "default-currency", "JPY", NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(destination), NULL, &error));
	g_assert_no_error(error);
	dest = venture_entity_get_id(VENTURE_ENTITY(destination));
	g_assert_false(venture_backup_service_restore(venture_backup_service_get(f->db), dest, changed, NULL, &error));
	g_assert_nonnull(error);
	g_clear_error(&error);
	g_assert_cmpint(count_type(f, dest, VENTURE_TYPE_ACCOUNT), ==, 0);
	g_assert_cmpint(count_type(f, dest, VENTURE_TYPE_JOURNAL), ==, 0);
	g_assert_cmpint(count_type(f, dest, VENTURE_TYPE_INVOICE), ==, 0);
	g_assert_cmpint(count_type(f, dest, VENTURE_TYPE_ACCOUNTING_BACKUP), ==, 0);
	{
		g_autoptr(VentureEntity) stored = venture_database_get(f->db, VENTURE_TYPE_ORGANIZATION, dest, &error);
		g_autofree gchar *currency = NULL;
		g_assert_no_error(error);
		g_object_get(stored, "default-currency", &currency, NULL);
		g_assert_cmpstr(currency, ==, "JPY");
		g_assert_cmpint(venture_entity_get_version(stored), ==, venture_entity_get_version(VENTURE_ENTITY(destination)));
	}
}

static void
test_cross_database(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureDatabase) target = NULL;
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureContext) context = NULL;
	g_autoptr(GError) error = NULL;
	(void)data;
	seed_books(f, TRUE);
	/* The target already owns a bank, so its new IDs overlap these. Derived
	 * map keys must use temporary values until all references are remapped. */
	{
		guint i;
		for (i = 0; i < 2; i++)
		{
			g_autoptr(VentureBankAccount) bank = venture_bank_account_new();
			g_autoptr(VentureAccountingControlMap) map = venture_accounting_control_map_new();
			g_object_set(bank, "organization-id", f->org, "name", i == 0 ? "Operating" : "Savings",
				"currency", "USD", "account-id", account_id(f, "1000"), NULL);
			g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(bank), NULL, &error));
			g_assert_no_error(error);
			g_object_set(map, "organization-id", f->org, "classification", "cash", "subject-type", "bank_account",
				"subject-id", venture_entity_get_id(VENTURE_ENTITY(bank)), "account-id", account_id(f, "1000"), NULL);
			g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(map), NULL, &error));
			g_assert_no_error(error);
		}
	}
	/* The normal fixture may be PostgreSQL; the target is independently
	 * migrated SQLite. No IDs, accounts or defaults can be borrowed. */
	target = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(target, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	context = venture_context_new(config, target);
	{
		g_autoptr(VentureBankAccount) unrelated = venture_bank_account_new();
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ACCOUNT);
		g_autoptr(VentureEntity) cash = NULL;
		venture_query_add_filter_string(query, "code", VENTURE_FILTER_OP_EQ, "1000", NULL);
		cash = venture_database_find_one(target, query, &error);
		g_assert_no_error(error);
		g_object_set(unrelated, "organization-id", venture_context_get_default_organization_id(context),
			"name", "Existing bank in another organization", "currency", "USD",
			"account-id", venture_entity_get_id(cash), NULL);
		g_assert_true(venture_database_save(target, VENTURE_ENTITY(unrelated), NULL, &error));
		g_assert_no_error(error);
	}
	{
		gint64 restored_org = venture_test_accounting_roundtrip_into(f->db, target, f->org);
		venture_test_accounting_roundtrip_into(target, f->db, restored_org);
	}
}

/* Historical referents stay readable after soft deletion, and public custom
 * attributes retain their declarations and derived lookup indexes. */
static void
test_metadata_history(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) field = NULL, company = NULL, value = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_COMPANY);
	g_autofree gchar *text = NULL;
	gint64 target;
	(void)data;
	seed_books(f, TRUE);
	field = venture_custom_fields_service_define(venture_custom_fields_service_get(f->db), f->org,
		"company", "region", "string", FALSE, "[]", NULL, &error);
	g_assert_no_error(error);
	venture_query_set_organization(query, f->org);
	company = venture_database_find_one(f->db, query, &error);
	g_assert_no_error(error);
	venture_entity_set_attribute(company, "region", "North");
	g_assert_true(venture_database_save(f->db, company, NULL, &error));
	g_assert_no_error(error);
	g_assert_true(venture_database_delete(f->db, company, NULL, &error));
	g_assert_no_error(error);
	target = venture_test_accounting_roundtrip(f->db, f->org);
	g_clear_object(&query);
	query = venture_query_new(VENTURE_TYPE_CUSTOM_FIELD_VALUE);
	venture_query_set_organization(query, target);
	value = venture_database_find_one(f->db, query, &error);
	g_assert_no_error(error);
	g_assert_nonnull(value);
	g_object_get(value, "value", &text, NULL);
	g_assert_cmpstr(text, ==, "North");
}

/* A namesake in another schema once made optional-module tables appear to
 * exist, breaking the second PostgreSQL fixture and schema reconciliation. */
static void
test_postgresql_schema_visibility(void)
{
	g_autoptr(VentureDatabase) first = NULL, second = NULL;
	g_autoptr(GHashTable) columns = NULL;
	g_autoptr(GError) error = NULL;
	if (g_getenv("VENTURE_TEST_ACCOUNTING_POSTGRES_URI") == NULL)
	{
		g_test_skip("Set VENTURE_TEST_ACCOUNTING_POSTGRES_URI for schema isolation coverage");
		return;
	}
	first = venture_test_accounting_database(&error);
	g_assert_no_error(error);
	second = venture_test_accounting_database(&error);
	g_assert_no_error(error);
	g_assert_true(venture_database_execute(first, "CREATE TABLE accounting_schema_probe (first_only INTEGER)", NULL, &error));
	g_assert_no_error(error);
	columns = venture_schema_get_existing_columns(venture_database_get_connection(second), "accounting_schema_probe", &error);
	g_assert_no_error(error);
	g_assert_cmpuint(g_hash_table_size(columns), ==, 0);
	g_clear_pointer(&columns, g_hash_table_unref);
	g_assert_true(venture_database_execute(second, "CREATE TABLE accounting_schema_probe (second_only INTEGER)", NULL, &error));
	g_assert_no_error(error);
	columns = venture_schema_get_existing_columns(venture_database_get_connection(second), "accounting_schema_probe", &error);
	g_assert_no_error(error);
	g_assert_cmpuint(g_hash_table_size(columns), ==, 1);
	g_assert_true(g_hash_table_contains(columns, "second_only"));
	venture_test_accounting_database_cleanup(second);
	venture_test_accounting_database_cleanup(first);
}

/* Legacy archives are operator input too: malformed JSON must return a
 * validation error without a GLib critical or any partially imported chart. */
static void
test_legacy_validation(Fixture *f, gconstpointer data)
{
	static const gchar *const valid = "{\"version\":3,\"accounts\":[{\"code\":\"1000\",\"kind\":0},{\"code\":\"4000\",\"kind\":3}],"
		"\"journals\":[{\"source_type\":\"organization\",\"occurred_at\":\"2026-01-01T00:00:00Z\",\"currency\":\"USD\","
		"\"lines\":[{\"account_code\":\"1000\",\"side\":\"debit\",\"amount\":\"10 USD\"},"
		"{\"account_code\":\"4000\",\"side\":\"credit\",\"amount\":\"10 USD\"}]}],"
		"\"invoices\":[],\"vendor_bills\":[],\"bank_accounts\":[],\"payments\":[],\"allocations\":[],"
		"\"invoice_events\":[],\"refund\":[],\"customer_credit\":[],\"company\":[]}";
	const gchar *mode = data;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) node = venture_json_parse(valid, &error);
	g_autoptr(VentureOrganization) destination = venture_organization_new();
	g_autofree gchar *payload = NULL;
	JsonObject *root = json_node_get_object(node);
	JsonObject *journal = json_array_get_object_element(json_object_get_array_member(root, "journals"), 0);
	gint64 org;
	g_assert_no_error(error);
	if (g_str_equal(mode, "account"))
		json_array_add_int_element(json_object_get_array_member(root, "accounts"), 7);
	else if (g_str_equal(mode, "date"))
		json_object_set_string_member(journal, "occurred_at", "not-a-date");
	else if (g_str_equal(mode, "lines"))
		json_object_set_int_member(journal, "lines", 7);
	else if (g_str_equal(mode, "side"))
		json_object_set_string_member(json_array_get_object_element(json_object_get_array_member(journal, "lines"), 1), "side", "typo");
	payload = venture_json_to_string(node, FALSE);
	g_object_set(destination, "name", "Legacy target", "default-currency", "USD", NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(destination), NULL, &error));
	org = venture_entity_get_id(VENTURE_ENTITY(destination));
	if (g_str_equal(mode, "valid"))
	{
		g_assert_true(venture_backup_service_restore(venture_backup_service_get(f->db), org, payload, NULL, &error));
		g_assert_no_error(error);
		g_assert_cmpint(count_type(f, org, VENTURE_TYPE_JOURNAL), ==, 1);
	}
	else
	{
		g_assert_false(venture_backup_service_restore(venture_backup_service_get(f->db), org, payload, NULL, &error));
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
		g_assert_cmpint(count_type(f, org, VENTURE_TYPE_ACCOUNT), ==, 0);
		g_assert_cmpint(count_type(f, org, VENTURE_TYPE_JOURNAL), ==, 0);
	}
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/backup/postgresql-schema-visibility", test_postgresql_schema_visibility);
	g_test_add("/backup/export-restore", Fixture, NULL, setup, test_export_restore_empty_org, teardown);
	g_test_add("/backup/metadata-history", Fixture, NULL, setup, test_metadata_history, teardown);
	g_test_add("/backup/csv", Fixture, NULL, setup, test_csv_export, teardown);
	g_test_add("/backup/manual-import", Fixture, NULL, setup, test_manual_import, teardown);
	g_test_add("/backup/cross-database", Fixture, NULL, setup, test_cross_database, teardown);
	{
		static const gchar *const modes[] = { "valid", "account", "date", "lines", "side" };
		guint i;
		for (i = 0; i < G_N_ELEMENTS(modes); i++)
		{
			g_autofree gchar *name = g_strconcat("/backup/legacy/", modes[i], NULL);
			g_test_add(name, Fixture, modes[i], setup, test_legacy_validation, teardown);
		}
	}
	{
		static const gchar *const modes[] = { "duplicate", "duplicate-uuid", "attributes", "missing-field", "missing-customer", "missing-projection", "unbalanced", "late-failure" };
		guint i;
		for (i = 0; i < G_N_ELEMENTS(modes); i++)
		{
			g_autofree gchar *name = g_strconcat("/backup/invalid/", modes[i], NULL);
			g_test_add(name, Fixture, modes[i], setup, test_invalid_archive, teardown);
		}
	}
	return g_test_run();
}
