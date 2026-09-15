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
	f->db = venture_database_new("sqlite://:memory:", &error);
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
seed_books(Fixture *f)
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
		g_assert_true(venture_settlement_service_transition(venture_settlement_service_get(f->db),
			invoice, "sent", now, NULL, &error));
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
	seed_books(f);
	backup = venture_backup_service_export(venture_backup_service_get(f->db),
		f->org, "json", &actor, &error);
	g_assert_no_error(error);
	g_object_get(backup, "payload", &payload, NULL);
	g_assert_nonnull(strstr(payload, "\"journals\""));
	g_assert_nonnull(strstr(payload, "\"invoices\""));
	g_object_set(empty, "name", "Empty books", "legal-name", "Empty books",
		"default-currency", "USD", NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(empty), NULL, &error));
	dest = venture_entity_get_id(VENTURE_ENTITY(empty));
	g_assert_true(venture_backup_service_restore(venture_backup_service_get(f->db),
		dest, payload, &actor, &error));
	g_assert_no_error(error);
	g_assert_cmpint(count_type(f, dest, VENTURE_TYPE_JOURNAL), >=, count_type(f, f->org, VENTURE_TYPE_JOURNAL));
	g_assert_cmpint(count_type(f, dest, VENTURE_TYPE_INVOICE), >=, 1);
	{
		g_autoptr(VentureQuery) q = venture_query_new(VENTURE_TYPE_ACCOUNT);
		g_autoptr(GPtrArray) rows = NULL;
		guint i;
		gboolean found_ap = FALSE;
		venture_query_set_organization(q, dest);
		rows = venture_database_find(f->db, q, NULL);
		for (i = 0; rows && i < rows->len; i++)
		{
			g_autofree gchar *code = NULL;
			gint kind = 0;
			g_object_get(g_ptr_array_index(rows, i), "code", &code, "kind", &kind, NULL);
			if (code && g_str_has_suffix(code, "2000"))
			{
				g_assert_cmpint(kind, ==, VENTURE_ACCOUNT_KIND_LIABILITY);
				found_ap = TRUE;
			}
		}
		g_assert_true(found_ap);
	}
	{
		g_autoptr(VentureQuery) q = venture_query_new(VENTURE_TYPE_INVOICE);
		g_autoptr(GPtrArray) rows = NULL;
		gint status = 0;
		venture_query_set_organization(q, dest);
		rows = venture_database_find(f->db, q, NULL);
		g_assert_cmpuint(rows->len, >=, 1);
		g_object_get(g_ptr_array_index(rows, 0), "status", &status, NULL);
		g_assert_cmpint(status, ==, VENTURE_INVOICE_STATUS_SENT);
	}
	g_assert_false(venture_backup_service_restore(venture_backup_service_get(f->db),
		dest, payload, &actor, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
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
	seed_books(f);
	backup = venture_backup_service_export(venture_backup_service_get(f->db),
		f->org, "csv", &actor, &error);
	g_assert_no_error(error);
	g_object_get(backup, "payload", &payload, NULL);
	g_assert_nonnull(strstr(payload, "journals"));
	g_assert_nonnull(strstr(payload, "invoices"));
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/backup/export-restore", Fixture, NULL, setup, test_export_restore_empty_org, teardown);
	g_test_add("/backup/csv", Fixture, NULL, setup, test_csv_export, teardown);
	return g_test_run();
}
