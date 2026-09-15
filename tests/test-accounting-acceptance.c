/*
 * test-accounting-acceptance.c - Independent accounting replacement pack.
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
	gint64 customer;
	gint64 vendor;
} Fixture;

static void
save(Fixture *f, VentureEntity *record)
{
	g_autoptr(GError) error = NULL;
	if (!venture_database_save(f->db, record, NULL, &error))
		g_error("save %s: %s", venture_entity_get_entity_name(record),
			error != NULL ? error->message : "no error");
}

static void
setup(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureCompany) customer = venture_company_new();
	g_autoptr(VentureCompany) vendor = venture_company_new();
	(void)data;
	f->config = venture_config_new();
	f->db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	f->context = venture_context_new(f->config, f->db);
	f->org = venture_context_get_default_organization_id(f->context);
	g_object_set(customer, "name", "Acceptance customer", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(customer), f->org);
	save(f, VENTURE_ENTITY(customer));
	f->customer = venture_entity_get_id(VENTURE_ENTITY(customer));
	g_object_set(vendor, "name", "Acceptance vendor", NULL);
	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(vendor), "kind", "supplier", NULL));
	venture_entity_set_organization_id(VENTURE_ENTITY(vendor), f->org);
	save(f, VENTURE_ENTITY(vendor));
	f->vendor = venture_entity_get_id(VENTURE_ENTITY(vendor));
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
balance(Fixture *f, const gchar *code, const gchar *cutoff)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ACCOUNT);
	g_autoptr(VentureEntity) account = NULL;
	g_autoptr(GDateTime) date = venture_time_from_string(cutoff, NULL);
	g_autoptr(VentureMoney) amount = NULL;
	g_autoptr(GError) error = NULL;
	venture_query_set_organization(query, f->org);
	g_assert_true(venture_query_add_filter_string(query, "code", VENTURE_FILTER_OP_EQ, code, NULL));
	account = venture_database_find_one(f->db, query, &error);
	g_assert_nonnull(account);
	amount = venture_posting_service_account_balance(venture_database_get_posting_service(f->db),
		venture_entity_get_id(account), f->org, "USD", date, &error);
	g_assert_no_error(error);
	return venture_money_get_amount(amount);
}

static void
money(VentureEntity *record, const gchar *field, const gchar *value)
{
	g_autoptr(GError) error = NULL;
	g_assert_true(venture_entity_set_field_from_string(record, field, value, &error));
}

/* Independently calculated: invoice 100+5, partial receipt 40, bill 50 paid,
 * outstanding check 10, year-end close still ties. */
static void
test_full_cycle(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureInvoice) invoice = venture_invoice_new();
	g_autoptr(VentureInvoiceLine) line = venture_invoice_line_new();
	g_autoptr(VenturePayment) payment = venture_payment_new();
	g_autoptr(VentureVendorBill) bill = venture_vendor_bill_new();
	g_autoptr(VentureVendorBillLine) bill_line = venture_vendor_bill_line_new();
	g_autoptr(VentureBillPayment) bill_pay = venture_bill_payment_new();
	g_autoptr(GDateTime) start = venture_time_from_string("2026-01-01", NULL);
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureFiscalYear) year = NULL;
	VentureActor actor;
	(void)data;
	actor.kind = VENTURE_ACTOR_KIND_USER;
	actor.name = "controller";
	actor.prompt = NULL;
	actor.request_id = NULL;
	actor.approved_by = NULL;
	year = venture_period_service_generate(venture_period_service_get(f->db),
		f->org, "FY2026", start, VENTURE_PERIOD_MONTHLY, &actor, &error);
	g_assert_nonnull(year);
	g_object_set(invoice, "number", "ACC-1", "company-id", f->customer, NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(invoice), f->org);
	money(VENTURE_ENTITY(invoice), "issued-at", "2026-01-10");
	save(f, VENTURE_ENTITY(invoice));
	g_object_set(line, "invoice-id", venture_entity_get_id(VENTURE_ENTITY(invoice)),
		"description", "Work", "quantity", 1.0, "tax-percent", (gint64)5, NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(line), f->org);
	money(VENTURE_ENTITY(line), "unit-price", "100 USD");
	save(f, VENTURE_ENTITY(line));
	g_object_set(invoice, "status", VENTURE_INVOICE_STATUS_SENT, NULL);
	save(f, VENTURE_ENTITY(invoice));
	g_assert_cmpint(balance(f, "1100", "2026-01-10T23:59:59Z"), ==, 10500);
	g_assert_cmpint(balance(f, "4000", "2026-01-10T23:59:59Z"), ==, -10000);
	g_assert_cmpint(balance(f, "2100", "2026-01-10T23:59:59Z"), ==, -500);
	g_object_set(payment, "customer-id", f->customer,
		"invoice-id", venture_entity_get_id(VENTURE_ENTITY(invoice)),
		"method", "transfer", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(payment), f->org);
	money(VENTURE_ENTITY(payment), "amount", "40 USD");
	money(VENTURE_ENTITY(payment), "date", "2026-01-20");
	save(f, VENTURE_ENTITY(payment));
	g_assert_cmpint(balance(f, "1000", "2026-01-20T23:59:59Z"), ==, 4000);
	g_assert_cmpint(balance(f, "1100", "2026-01-20T23:59:59Z"), ==, 6500);
	g_object_set(bill, "number", "B-1", "company-id", f->vendor, "status", "draft", "currency", "USD", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(bill), f->org);
	money(VENTURE_ENTITY(bill), "bill-date", "2026-01-12");
	save(f, VENTURE_ENTITY(bill));
	g_object_set(bill_line, "bill-id", venture_entity_get_id(VENTURE_ENTITY(bill)),
		"description", "Parts", "quantity", "1", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(bill_line), f->org);
	money(VENTURE_ENTITY(bill_line), "unit-price", "50 USD");
	save(f, VENTURE_ENTITY(bill_line));
	{
		g_autoptr(VentureVendorBillEvent) event = venture_vendor_bill_event_new();
		g_object_set(event, "bill-id", venture_entity_get_id(VENTURE_ENTITY(bill)),
			"vendor-id", f->vendor, "kind", "approve", "state", "approved", NULL);
		venture_entity_set_organization_id(VENTURE_ENTITY(event), f->org);
		money(VENTURE_ENTITY(event), "date", "2026-01-12");
		save(f, VENTURE_ENTITY(event));
	}
	g_object_set(bill_pay, "vendor-id", f->vendor,
		"bill-id", venture_entity_get_id(VENTURE_ENTITY(bill)),
		"method", "transfer", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(bill_pay), f->org);
	money(VENTURE_ENTITY(bill_pay), "amount", "50 USD");
	money(VENTURE_ENTITY(bill_pay), "date", "2026-01-22");
	save(f, VENTURE_ENTITY(bill_pay));
	g_assert_cmpint(balance(f, "2000", "2026-01-22T23:59:59Z"), ==, 0);
	g_assert_cmpint(balance(f, "1100", "2026-01-31T23:59:59Z"), ==, 6500);
	g_assert_true(venture_settlement_service_correct_tax_allocation(
		venture_settlement_service_get(f->db), f->org,
		venture_time_from_string("2026-01-31", NULL), &actor, &error));
}

static gboolean
reject_credit(VentureDatabase *db, VentureEntity *record, VentureEntity *previous,
	gpointer data, GError **error)
{
	(void)db; (void)record; (void)previous; (void)data;
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_DATABASE, "injected credit failure");
	return FALSE;
}

static guint
count_type(Fixture *f, GType type)
{
	g_autoptr(VentureQuery) query = venture_query_new(type);
	g_autoptr(GPtrArray) rows = NULL;
	venture_query_set_limit(query, 0);
	rows = venture_database_find(f->db, query, NULL);
	return rows != NULL ? rows->len : 0;
}

static void
test_rollback_retry(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureInvoice) invoice = venture_invoice_new();
	g_autoptr(VentureInvoiceLine) line = venture_invoice_line_new();
	g_autoptr(VenturePayment) payment = venture_payment_new();
	g_autoptr(GError) error = NULL;
	guint journals;
	(void)data;
	g_object_set(invoice, "number", "ACC-RB", "company-id", f->customer, NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(invoice), f->org);
	money(VENTURE_ENTITY(invoice), "issued-at", "2026-01-10");
	save(f, VENTURE_ENTITY(invoice));
	g_object_set(line, "invoice-id", venture_entity_get_id(VENTURE_ENTITY(invoice)),
		"description", "Work", "quantity", 1.0, NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(line), f->org);
	money(VENTURE_ENTITY(line), "unit-price", "40 USD");
	save(f, VENTURE_ENTITY(line));
	g_object_set(invoice, "status", VENTURE_INVOICE_STATUS_SENT, NULL);
	save(f, VENTURE_ENTITY(invoice));
	journals = count_type(f, VENTURE_TYPE_JOURNAL);
	venture_database_add_save_validator(f->db, VENTURE_TYPE_CUSTOMER_CREDIT, reject_credit, NULL, NULL);
	g_object_set(payment, "customer-id", f->customer,
		"invoice-id", venture_entity_get_id(VENTURE_ENTITY(invoice)),
		"method", "transfer", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(payment), f->org);
	money(VENTURE_ENTITY(payment), "amount", "40 USD");
	money(VENTURE_ENTITY(payment), "date", "2026-01-15");
	g_assert_false(venture_database_save(f->db, VENTURE_ENTITY(payment), NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_DATABASE);
	g_assert_cmpuint(count_type(f, VENTURE_TYPE_JOURNAL), ==, journals);
	g_assert_cmpuint(count_type(f, VENTURE_TYPE_PAYMENT), ==, 0);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	venture_entity_registry_register_builtins(venture_entity_registry_get_default());
	g_test_add("/accounting-acceptance/full-cycle", Fixture, NULL, setup, test_full_cycle, teardown);
	g_test_add("/accounting-acceptance/rollback-retry", Fixture, NULL, setup, test_rollback_retry, teardown);
	return g_test_run();
}
