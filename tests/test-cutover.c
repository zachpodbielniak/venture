/*
 * test-cutover.c - Guided Zoho Books / QuickBooks opening-balance cutover.
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <venture.h>
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

static JsonObject *
sample_payload(void)
{
	g_autoptr(JsonParser) parser = json_parser_new();
	const gchar *json =
		"{\"source\":\"zoho_books\",\"cutoff\":\"2026-01-01\","
		"\"chart\":[{\"source_id\":\"c-cash\",\"code\":\"1000\",\"name\":\"Cash\",\"kind\":\"asset\"},"
		"{\"source_id\":\"c-ar\",\"code\":\"1100\",\"name\":\"AR\",\"kind\":\"asset\"},"
		"{\"source_id\":\"c-income\",\"code\":\"4000\",\"name\":\"Income\",\"kind\":\"income\"},"
		"{\"source_id\":\"c-equity\",\"code\":\"3000\",\"name\":\"Equity\",\"kind\":\"equity\"}],"
		"\"customers\":[{\"source_id\":\"cust-1\",\"name\":\"Acme\"}],"
		"\"vendors\":[{\"source_id\":\"vend-1\",\"name\":\"Supplier\"}],"
		"\"items\":[{\"source_id\":\"item-1\",\"name\":\"Work\"}],"
		"\"open_ar\":[{\"source_id\":\"inv-1\",\"customer_source_id\":\"cust-1\","
		"\"number\":\"OB-1\",\"amount\":\"105 USD\",\"net\":\"100 USD\",\"tax\":\"5 USD\",\"date\":\"2025-12-15\"}],"
		"\"open_ap\":[],\"credits\":[],\"bank_balances\":[{\"source_id\":\"bank-1\","
		"\"name\":\"Checking\",\"account_code\":\"1000\",\"amount\":\"500 USD\"}],"
		"\"assets\":[],\"unsupported\":[\"payroll_item\"]}";
	g_assert_true(json_parser_load_from_data(parser, json, -1, NULL));
	return json_object_ref(json_node_get_object(json_parser_get_root(parser)));
}

static void
test_preview_import_activate(Fixture *f, gconstpointer data)
{
	g_autoptr(JsonObject) payload = sample_payload();
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) cutover = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autofree gchar *state = NULL;
	g_autofree gchar *report = NULL;
	VentureActor actor;
	(void)data;
	actor.kind = VENTURE_ACTOR_KIND_USER;
	actor.name = "migrator";
	actor.prompt = NULL;
	actor.request_id = NULL;
	actor.approved_by = NULL;
	cutover = venture_cutover_service_preview(venture_cutover_service_get(f->db),
		f->org, payload, &actor, &error);
	g_assert_no_error(error);
	g_assert_nonnull(cutover);
	g_object_get(cutover, "state", &state, "reconciliation-report", &report, NULL);
	g_assert_cmpstr(state, ==, "preview");
	g_assert_nonnull(strstr(report, "payroll_item"));
	g_assert_true(venture_cutover_service_import(venture_cutover_service_get(f->db),
		VENTURE_ACCOUNTING_CUTOVER(cutover), &actor, &error));
	g_assert_no_error(error);
	g_clear_pointer(&state, g_free);
	g_object_get(cutover, "state", &state, NULL);
	g_assert_cmpstr(state, ==, "imported");
	query = venture_query_new(VENTURE_TYPE_ACCOUNTING_CUTOVER_ROW);
	venture_query_set_limit(query, 0);
	rows = venture_database_find(f->db, query, &error);
	g_assert_cmpuint(rows->len, >, 0);
	g_assert_true(venture_cutover_service_reconcile(venture_cutover_service_get(f->db),
		VENTURE_ACCOUNTING_CUTOVER(cutover), &actor, &error));
	g_assert_true(venture_cutover_service_activate(venture_cutover_service_get(f->db),
		VENTURE_ACCOUNTING_CUTOVER(cutover), &actor, &error));
	g_clear_pointer(&state, g_free);
	g_object_get(cutover, "state", &state, NULL);
	g_assert_cmpstr(state, ==, "active");
	g_assert_false(venture_cutover_service_rollback(venture_cutover_service_get(f->db),
		VENTURE_ACCOUNTING_CUTOVER(cutover), &actor, &error));
	g_assert_nonnull(error);
}

static void
test_idempotent_rollback(Fixture *f, gconstpointer data)
{
	g_autoptr(JsonObject) payload = sample_payload();
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) first = NULL;
	g_autoptr(VentureEntity) second = NULL;
	g_autoptr(GPtrArray) invoices = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_INVOICE);
	VentureActor actor;
	(void)data;
	actor.kind = VENTURE_ACTOR_KIND_USER;
	actor.name = "migrator";
	actor.prompt = NULL;
	actor.request_id = NULL;
	actor.approved_by = NULL;
	first = venture_cutover_service_preview(venture_cutover_service_get(f->db),
		f->org, payload, &actor, &error);
	g_assert_true(venture_cutover_service_import(venture_cutover_service_get(f->db),
		VENTURE_ACCOUNTING_CUTOVER(first), &actor, &error));
	second = venture_cutover_service_preview(venture_cutover_service_get(f->db),
		f->org, payload, &actor, &error);
	g_assert_true(venture_cutover_service_import(venture_cutover_service_get(f->db),
		VENTURE_ACCOUNTING_CUTOVER(second), &actor, &error));
	venture_query_set_limit(query, 0);
	invoices = venture_database_find(f->db, query, &error);
	g_assert_cmpuint(invoices->len, ==, 1);
	g_assert_true(venture_cutover_service_rollback(venture_cutover_service_get(f->db),
		VENTURE_ACCOUNTING_CUTOVER(first), &actor, &error));
	g_assert_no_error(error);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	venture_entity_registry_register_builtins(venture_entity_registry_get_default());
	g_test_add("/cutover/preview-import-activate", Fixture, NULL, setup, test_preview_import_activate, teardown);
	g_test_add("/cutover/idempotent-rollback", Fixture, NULL, setup, test_idempotent_rollback, teardown);
	return g_test_run();
}
