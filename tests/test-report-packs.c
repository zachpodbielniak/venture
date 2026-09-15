/*
 * test-report-packs.c - Saved reports, journal dimensions and scheduled packs.
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <venture.h>
#include <string.h>
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

static void
test_save_and_run(Fixture *f, gconstpointer data)
{
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(JsonObject) options = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) saved = NULL;
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(VentureEntity) pack = NULL;
	g_autoptr(GPtrArray) results = NULL;
	(void)data;
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "organization_id");
	json_builder_add_int_value(builder, f->org);
	json_builder_set_member_name(builder, "currency");
	json_builder_add_string_value(builder, "USD");
	json_builder_end_object(builder);
	options = json_object_ref(json_node_get_object(json_builder_get_root(builder)));
	saved = venture_report_pack_service_save(venture_report_pack_service_get(f->db),
		f->org, "August cash flow", "cash_flow", "2026-08", options, "dept-ops", NULL, &error);
	g_assert_no_error(error);
	result = venture_report_pack_service_run(venture_report_pack_service_get(f->db),
		f->context, VENTURE_SAVED_REPORT(saved), &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	pack = venture_report_pack_service_schedule(venture_report_pack_service_get(f->db),
		f->org, "Month-end pack", "0 8 1 * *",
		venture_entity_get_id(saved), NULL, &error);
	g_assert_no_error(error);
	results = venture_report_pack_service_run_pack(venture_report_pack_service_get(f->db),
		f->context, VENTURE_REPORT_PACK(pack), &error);
	g_assert_no_error(error);
	g_assert_cmpuint(results->len, ==, 1);
}

static void
test_journal_dimension(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureJournal) journal = venture_journal_new();
	g_autoptr(GPtrArray) lines = g_ptr_array_new_with_free_func(g_object_unref);
	g_autoptr(VentureJournalLine) debit = venture_journal_line_new();
	g_autoptr(VentureJournalLine) credit = venture_journal_line_new();
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) when = g_date_time_new_from_iso8601("2026-08-10T00:00:00Z", NULL);
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ACCOUNT);
	g_autoptr(VentureEntity) cash = NULL;
	g_autoptr(VentureEntity) sales = NULL;
	g_autofree gchar *dimension = NULL;
	g_autoptr(VentureJournal) posted = NULL;
	(void)data;
	venture_query_add_filter_string(query, "code", VENTURE_FILTER_OP_EQ, "1000", NULL);
	cash = venture_database_find_one(f->db, query, NULL);
	g_object_unref(query);
	query = venture_query_new(VENTURE_TYPE_ACCOUNT);
	venture_query_add_filter_string(query, "code", VENTURE_FILTER_OP_EQ, "4000", NULL);
	sales = venture_database_find_one(f->db, query, NULL);
	g_object_set(journal, "source-type", "organization", "source-id", f->org,
		"occurred-at", when, "currency", "USD", "organization-id", f->org, NULL);
	g_object_set(debit, "account-id", venture_entity_get_id(cash), "side", VENTURE_LEDGER_SIDE_DEBIT,
		"organization-id", f->org, "dimension", "dept-ops", NULL);
	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(debit), "amount", "10 USD", NULL));
	g_object_set(credit, "account-id", venture_entity_get_id(sales), "side", VENTURE_LEDGER_SIDE_CREDIT,
		"organization-id", f->org, "dimension", "dept-ops", NULL);
	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(credit), "amount", "10 USD", NULL));
	g_ptr_array_add(lines, g_steal_pointer(&debit));
	g_ptr_array_add(lines, g_steal_pointer(&credit));
	posted = venture_posting_service_post(venture_database_get_posting_service(f->db),
		journal, lines, NULL, NULL, &error);
	g_assert_no_error(error);
	{
		g_autoptr(VentureQuery) q = venture_query_new(VENTURE_TYPE_JOURNAL_LINE);
		g_autoptr(GPtrArray) rows = NULL;
		venture_query_add_filter_int(q, "journal-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(VENTURE_ENTITY(posted)), NULL);
		rows = venture_database_find(f->db, q, NULL);
		g_object_get(g_ptr_array_index(rows, 0), "dimension", &dimension, NULL);
		g_assert_cmpstr(dimension, ==, "dept-ops");
	}
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/report-packs/save-run", Fixture, NULL, setup, test_save_and_run, teardown);
	g_test_add("/report-packs/journal-dimension", Fixture, NULL, setup, test_journal_dimension, teardown);
	return g_test_run();
}
