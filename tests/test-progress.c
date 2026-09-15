/*
 * test-progress.c - Progress invoicing, retainers and retention release.
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
	gint64 company;
} Fixture;

static void
save(Fixture *f, VentureEntity *record)
{
	g_autoptr(GError) error = NULL;
	g_assert_true(venture_database_save(f->db, record, NULL, &error));
	g_assert_no_error(error);
}

static void
actor_init(VentureActor *actor)
{
	actor->kind = VENTURE_ACTOR_KIND_USER;
	actor->name = "biller";
	actor->prompt = NULL;
	actor->request_id = NULL;
	actor->approved_by = NULL;
}

static gint64
G_GNUC_UNUSED account_id(Fixture *f, const gchar *code)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ACCOUNT);
	g_autoptr(VentureEntity) row = NULL;
	venture_query_add_filter_string(query, "code", VENTURE_FILTER_OP_EQ, code, NULL);
	row = venture_database_find_one(f->db, query, NULL);
	g_assert_nonnull(row);
	return venture_entity_get_id(row);
}

static void
setup(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureCompany) company = NULL;
	(void)data;
	f->config = venture_config_new();
	f->db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->db);
	f->org = venture_context_get_default_organization_id(f->context);
	company = venture_company_new();
	g_object_set(company, "name", "Client", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(company), f->org);
	save(f, VENTURE_ENTITY(company));
	f->company = venture_entity_get_id(VENTURE_ENTITY(company));
}

static void
teardown(Fixture *f, gconstpointer data)
{
	(void)data;
	g_clear_object(&f->context);
	g_clear_object(&f->db);
	g_clear_object(&f->config);
}

static VentureEntity *
accepted_quote(Fixture *f)
{
	g_autoptr(VentureEntity) quote = VENTURE_ENTITY(venture_quote_new());
	g_autoptr(VentureEntity) line = VENTURE_ENTITY(venture_quote_line_new());
	g_autoptr(VentureEntity) action = VENTURE_ENTITY(venture_quote_action_new());
	g_autoptr(GError) error = NULL;
	VentureActor actor;
	actor_init(&actor);
	venture_entity_set_organization_id(quote, f->org);
	g_object_set(quote, "number", "Q-1", "company-id", f->company, "currency", "USD",
		"billing-mode", "progress", NULL);
	save(f, quote);
	venture_entity_set_organization_id(line, f->org);
	g_object_set(line, "quote-id", venture_entity_get_id(quote), "description", "Contract",
		"quantity", (gint64)1, NULL);
	g_assert_true(venture_entity_set_field_from_string(line, "unit-price", "1000 USD", NULL));
	save(f, line);
	{
		gint64 qid = venture_entity_get_id(quote);
		g_object_unref(quote);
		quote = venture_database_get(f->db, VENTURE_TYPE_QUOTE, qid, NULL);
	}
	venture_entity_set_organization_id(action, f->org);
	g_object_set(action, "quote-id", venture_entity_get_id(quote), "action", "send",
		"expected-version", venture_entity_get_version(quote), NULL);
	if (!venture_quote_service_execute(venture_database_get_quote_service(f->db),
		action, "manual", NULL, &actor, &error))
		g_error("send: %s", error ? error->message : "nil");
	g_assert_no_error(error);
	g_clear_object(&action);
	quote = venture_database_get(f->db, VENTURE_TYPE_QUOTE, venture_entity_get_id(quote), NULL);
	action = VENTURE_ENTITY(venture_quote_action_new());
	venture_entity_set_organization_id(action, f->org);
	g_object_set(action, "quote-id", venture_entity_get_id(quote), "action", "accept",
		"expected-version", venture_entity_get_version(quote), "accepted-by", "Alex Buyer", NULL);
	g_assert_true(venture_quote_service_execute(venture_database_get_quote_service(f->db),
		action, "manual", NULL, &actor, &error));
	g_assert_no_error(error);
	return venture_database_get(f->db, VENTURE_TYPE_QUOTE, venture_entity_get_id(quote), NULL);
}

static void
test_progress_invoice_remaining(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) quote = accepted_quote(f);
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureMoney) remaining = NULL;
	g_autoptr(VentureEntity) first = NULL;
	g_autoptr(VentureEntity) over = NULL;
	g_autoptr(VentureMoney) total = NULL;
	VentureActor actor;
	gint64 invoice_id = 0;
	(void)data;
	actor_init(&actor);
	g_object_get(quote, "invoice-id", &invoice_id, "total", &total, NULL);
	g_assert_cmpint(invoice_id, ==, 0);
	g_assert_cmpint(venture_money_get_amount(total), ==, 100000);
	remaining = venture_progress_service_remaining(venture_progress_service_get(f->db),
		VENTURE_QUOTE(quote), &error);
	g_assert_cmpint(venture_money_get_amount(remaining), ==, 100000);
	first = venture_progress_service_invoice(venture_progress_service_get(f->db),
		VENTURE_QUOTE(quote), 40, NULL, &actor, &error);
	g_assert_no_error(error);
	g_assert_nonnull(first);
	g_clear_pointer(&remaining, venture_money_free);
	remaining = venture_progress_service_remaining(venture_progress_service_get(f->db),
		VENTURE_QUOTE(quote), &error);
	g_assert_cmpint(venture_money_get_amount(remaining), ==, 60000);
	over = venture_progress_service_invoice(venture_progress_service_get(f->db),
		VENTURE_QUOTE(quote), 70, NULL, &actor, &error);
	g_assert_null(over);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
}

static gint64
liability(Fixture *f)
{
	g_autoptr(VentureAccount) account = venture_account_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(account), f->org);
	g_object_set(account, "code", "2200", "name", "Retainers",
		"kind", VENTURE_ACCOUNT_KIND_LIABILITY, "active", TRUE, NULL);
	save(f, VENTURE_ENTITY(account));
	return venture_entity_get_id(VENTURE_ENTITY(account));
}

static void
test_retainer_then_release(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureMoney) amount = venture_money_new_for_currency(25000, "USD");
	g_autoptr(VentureEntity) retainer = NULL;
	g_autoptr(VentureMoney) remaining = NULL;
	VentureActor actor;
	gint64 liability_id = liability(f);
	(void)data;
	actor_init(&actor);
	retainer = venture_progress_service_collect_retainer(venture_progress_service_get(f->db),
		f->org, f->company, liability_id, amount, &actor, &error);
	g_assert_no_error(error);
	g_object_get(retainer, "remaining", &remaining, NULL);
	g_assert_cmpint(venture_money_get_amount(remaining), ==, 25000);
	g_assert_true(venture_progress_service_release_retainer(venture_progress_service_get(f->db),
		VENTURE_CUSTOMER_RETAINER(retainer), amount, &actor, &error));
	g_assert_no_error(error);
	g_clear_pointer(&remaining, venture_money_free);
	g_object_get(retainer, "remaining", &remaining, NULL);
	g_assert_cmpint(venture_money_get_amount(remaining), ==, 0);
}

static void
test_retention_hold_and_release(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) quote = accepted_quote(f);
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureMoney) amount = venture_money_new_for_currency(10000, "USD");
	g_autoptr(VentureEntity) retention = NULL;
	VentureActor actor;
	gint64 liability_id = liability(f);
	(void)data;
	actor_init(&actor);
	retention = venture_progress_service_hold_retention(venture_progress_service_get(f->db),
		VENTURE_QUOTE(quote), liability_id, amount, &actor, &error);
	g_assert_no_error(error);
	g_assert_true(venture_progress_service_release_retention(venture_progress_service_get(f->db),
		VENTURE_CONTRACT_RETENTION(retention), amount, &actor, &error));
	g_assert_no_error(error);
}

static void
test_generic_write_refused(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureProgressBilling) row = venture_progress_billing_new();
	g_autoptr(GError) error = NULL;
	(void)data;
	venture_entity_set_organization_id(VENTURE_ENTITY(row), f->org);
	g_object_set(row, "kind", "progress", NULL);
	g_assert_false(venture_database_save(f->db, VENTURE_ENTITY(row), NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/progress/invoice-remaining", Fixture, NULL, setup, test_progress_invoice_remaining, teardown);
	g_test_add("/progress/retainer", Fixture, NULL, setup, test_retainer_then_release, teardown);
	g_test_add("/progress/retention", Fixture, NULL, setup, test_retention_hold_and_release, teardown);
	g_test_add("/progress/generic-write", Fixture, NULL, setup, test_generic_write_refused, teardown);
	return g_test_run();
}
