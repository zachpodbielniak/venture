/*
 * test-accounting-acceptance.c - Independent accounting replacement pack.
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <venture.h>
#include "venture-test-util.h"
#include "venture-test-accounting.h"

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
	f->db = venture_test_accounting_database(&error);
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
	venture_test_accounting_database_cleanup(f->db);
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

static VentureEntity *
first(Fixture *f, GType type, const gchar *field, const gchar *value)
{
	g_autoptr(VentureQuery) query = venture_query_new(type);
	g_autoptr(GError) error = NULL;
	VentureEntity *record;
	venture_query_set_organization(query, f->org);
	if (field != NULL)
		g_assert_true(venture_query_add_filter_string(query, field, VENTURE_FILTER_OP_EQ, value, &error));
	g_assert_true(venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, &error));
	record = venture_database_find_one(f->db, query, &error);
	g_assert_no_error(error);
	g_assert_nonnull(record);
	return record;
}

static gint64
statement_cell(Fixture *f, const gchar *report_name, const gchar *key)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonObject) options = json_object_new();
	g_autoptr(VentureDateRange) range = venture_context_parse_period(f->context, "2026-01", &error);
	g_autoptr(VentureReportResult) report = NULL;
	guint i;
	g_assert_no_error(error);
	json_object_set_int_member(options, "organization_id", f->org);
	json_object_set_string_member(options, "currency", "USD");
	report = venture_report_generate(venture_report_registry_lookup(
		venture_context_get_report_registry(f->context), report_name), f->context, range, options, &error);
	g_assert_no_error(error);
	g_assert_nonnull(report);
	for (i = 0; i < venture_report_result_get_row_count(report); i++)
	{
		const GValue *name = venture_report_result_get_cell(report, i, "key");
		if (name != NULL && g_strcmp0(g_value_get_string(name), key) == 0)
		{
			const GValue *value = venture_report_result_get_cell(report, i, "current");
			g_assert_nonnull(value);
			g_assert_true(G_VALUE_HOLDS(value, VENTURE_TYPE_MONEY));
			return venture_money_get_amount(g_value_get_boxed(value));
		}
	}
	g_error("Missing %s row %s", report_name, key);
	return 0;
}

static void
finish_cycle(Fixture *f, const VentureActor *actor)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) account = first(f, VENTURE_TYPE_ACCOUNT, "code", "1000");
	g_autoptr(VentureEntity) bank = g_object_new(VENTURE_TYPE_BANK_ACCOUNT,
		"organization-id", f->org, "name", "Acceptance checking",
		"account-id", venture_entity_get_id(account), "currency", "USD",
		"date-column", "Date", "amount-column", "Amount", "description-column", "Memo",
		"reference-column", "Ref", "external-id-column", "ID", "date-format", "%Y-%m-%d",
		"sign-convention", "normal", NULL);
	g_autoptr(JsonObject) args = json_object_new();
	g_autoptr(VentureEntity) statement = NULL;
	g_autoptr(VentureEntity) result = NULL;
	g_autoptr(VentureMoney) difference = NULL;
	g_autoptr(VentureEntity) period = NULL;
	g_autoptr(VentureEntity) workspace = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) tasks = NULL;
	g_autoptr(VentureUser) reviewer = venture_user_new();
	g_autoptr(JsonNode) pack = NULL;
	g_autoptr(VenturePayment) forbidden = venture_payment_new();
	VentureActor reviewing = *actor;
	gint64 source_org = f->org;
	guint i;
	gboolean tied = FALSE;
	save(f, bank);
	json_object_set_string_member(args, "period_start", "2026-01-01");
	json_object_set_string_member(args, "period_end", "2026-01-31");
	json_object_set_string_member(args, "opening_balance", "0 USD");
	json_object_set_string_member(args, "closing_balance", "-10 USD");
	json_object_set_string_member(args, "format", "csv");
	json_object_set_string_member(args, "data",
		"Date,Amount,Memo,Ref,ID\n2026-01-20,40,Customer receipt,ACC-1,accept-receipt\n"
		"2026-01-22,-50,Supplier payment,B-1,accept-payment\n");
	statement = venture_bank_match_service_execute(venture_database_get_bank_match_service(f->db),
		"import", venture_entity_get_id(bank), args, actor, &error);
	g_assert_no_error(error);
	g_assert_nonnull(statement);
	result = venture_bank_match_service_execute(venture_database_get_bank_match_service(f->db),
		"reconcile", venture_entity_get_id(statement), args, actor, &error);
	g_assert_null(result);
	g_assert_nonnull(error);
	g_clear_error(&error);
	for (i = 0; i < 2; i++)
	{
		g_autoptr(VentureEntity) transaction = first(f, VENTURE_TYPE_BANK_TRANSACTION,
			"external-id", i == 0 ? "accept-receipt" : "accept-payment");
		g_autoptr(VentureEntity) source = first(f, i == 0 ? VENTURE_TYPE_PAYMENT : VENTURE_TYPE_BILL_PAYMENT,
			NULL, NULL);
		g_autoptr(JsonObject) match_args = json_object_new();
		JsonArray *parts = json_array_new();
		JsonObject *part = json_object_new();
		json_object_set_string_member(part, "type", i == 0 ? "payment" : "bill_payment");
		json_object_set_int_member(part, "id", venture_entity_get_id(source));
		json_array_add_object_element(parts, part);
		json_object_set_array_member(match_args, "parts", parts);
		result = venture_bank_match_service_execute(venture_database_get_bank_match_service(f->db),
			"match", venture_entity_get_id(transaction), match_args, actor, &error);
		g_assert_no_error(error);
		g_assert_nonnull(result);
		g_clear_object(&result);
	}
	result = venture_bank_match_service_execute(venture_database_get_bank_match_service(f->db),
		"reconcile", venture_entity_get_id(statement), args, actor, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	g_object_get(result, "difference", &difference, NULL);
	g_assert_nonnull(difference);
	g_assert_cmpint(venture_money_get_amount(difference), ==, 0);
	g_assert_cmpint(statement_cell(f, "income_statement", "income"), ==, 10000);
	g_assert_cmpint(statement_cell(f, "income_statement", "expenses"), ==, 5000);
	g_assert_cmpint(statement_cell(f, "income_statement", "net_income"), ==, 5000);
	g_assert_cmpint(statement_cell(f, "balance_sheet", "assets"), ==, 5500);
	g_assert_cmpint(statement_cell(f, "balance_sheet", "difference"), ==, 0);
	period = first(f, VENTURE_TYPE_FISCAL_PERIOD, NULL, NULL);
	workspace = venture_close_service_open(venture_close_service_get(f->db),
		venture_entity_get_id(period), "USD", actor, &error);
	g_assert_no_error(error);
	g_assert_true(venture_close_service_run_checks(venture_close_service_get(f->db), workspace, actor, &error));
	g_assert_no_error(error);
	g_object_get(workspace, "subledger-tied", &tied, NULL);
	g_assert_true(tied);
	query = venture_query_new(VENTURE_TYPE_CLOSE_TASK);
	venture_query_set_organization(query, f->org);
	venture_query_set_limit(query, 0);
	tasks = venture_database_find(f->db, query, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(tasks->len, >, 0);
	for (i = 0; i < tasks->len; i++)
		g_assert_true(venture_close_service_complete_task(venture_close_service_get(f->db),
			g_ptr_array_index(tasks, i), FALSE, "Synthetic balances reconciled", actor, &error));
	g_assert_no_error(error);
	g_object_set(reviewer, "username", "acceptance-reviewer", "role", VENTURE_USER_ROLE_OWNER, "active", TRUE, NULL);
	save(f, VENTURE_ENTITY(reviewer));
	reviewing.name = "acceptance-reviewer";
	g_assert_true(venture_close_service_sign(venture_close_service_get(f->db), workspace, "preparer", actor, &error));
	g_assert_true(venture_close_service_sign(venture_close_service_get(f->db), workspace, "reviewer", &reviewing, &error));
	g_assert_true(venture_close_service_complete(venture_close_service_get(f->db), workspace, actor, &error));
	g_assert_no_error(error);
	pack = venture_close_service_pack(venture_close_service_get(f->db), workspace, &error);
	g_assert_no_error(error);
	g_assert_true(json_object_has_member(json_node_get_object(pack), "trial_balance"));
	/* Closing must protect the same posting boundary used by the receipts
	 * above, not merely change the status displayed on a period. */
	g_object_set(forbidden, "organization-id", f->org, "customer-id", f->customer, "method", "transfer", NULL);
	money(VENTURE_ENTITY(forbidden), "date", "2026-01-30");
	money(VENTURE_ENTITY(forbidden), "amount", "1 USD");
	g_assert_false(venture_database_save(f->db, VENTURE_ENTITY(forbidden), actor, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
	g_clear_error(&error);
	/* A controller corrects expense classification by an explicit balanced
	 * journal after reopening, preserving the original bill and its payment. */
	g_assert_true(venture_close_service_reopen(venture_close_service_get(f->db), workspace, &reviewing, &error));
	g_assert_no_error(error);
	{
		g_autoptr(VentureEntity) debit = first(f, VENTURE_TYPE_ACCOUNT, "code", "6400");
		g_autoptr(VentureEntity) credit = first(f, VENTURE_TYPE_ACCOUNT, "code", "6900");
		g_autoptr(VentureJournal) header = venture_journal_new();
		g_autoptr(VentureJournal) posted = NULL;
		g_autoptr(GPtrArray) lines = g_ptr_array_new_with_free_func(g_object_unref);
		g_autoptr(GDateTime) date = venture_time_from_string("2026-01-31", &error);
		g_autoptr(VentureMoney) amount = venture_money_new_for_currency(500, "USD");
		gint64 before = balance(f, "6900", "2026-01-31T23:59:59Z");
		VentureJournalLine *entry;
		g_object_set(header, "organization-id", f->org, "source-type", "organization",
			"source-id", f->org, "occurred-at", date, "currency", "USD",
			"memo", "Reclassify five dollars of supplier expense", NULL);
		entry = venture_journal_line_new();
		g_object_set(entry, "account-id", venture_entity_get_id(debit),
			"side", VENTURE_LEDGER_SIDE_DEBIT, "amount", amount, NULL);
		g_ptr_array_add(lines, entry);
		entry = venture_journal_line_new();
		g_object_set(entry, "account-id", venture_entity_get_id(credit),
			"side", VENTURE_LEDGER_SIDE_CREDIT, "amount", amount, NULL);
		g_ptr_array_add(lines, entry);
		posted = venture_posting_service_post(venture_database_get_posting_service(f->db),
			header, lines, NULL, actor, &error);
		g_assert_no_error(error);
		g_assert_nonnull(posted);
		g_assert_cmpint(balance(f, "6900", "2026-01-31T23:59:59Z"), ==, before - 500);
		g_assert_cmpint(balance(f, "6400", "2026-01-31T23:59:59Z"), ==, 500);
	}
	g_assert_true(venture_close_service_run_checks(venture_close_service_get(f->db), workspace, actor, &error));
	g_assert_no_error(error);
	g_assert_false(venture_close_service_complete(venture_close_service_get(f->db), workspace, actor, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	g_assert_true(venture_close_service_sign(venture_close_service_get(f->db), workspace, "preparer", actor, &error));
	g_assert_true(venture_close_service_sign(venture_close_service_get(f->db), workspace, "reviewer", &reviewing, &error));
	g_assert_true(venture_close_service_complete(venture_close_service_get(f->db), workspace, actor, &error));
	g_assert_no_error(error);
	f->org = venture_test_accounting_roundtrip(f->db, source_org);
	g_assert_cmpint(statement_cell(f, "income_statement", "net_income"), ==, 5000);
	g_assert_cmpint(statement_cell(f, "balance_sheet", "difference"), ==, 0);
	f->org = source_org;
	g_assert_cmpint(balance(f, "1000", "2026-01-31T23:59:59Z"), ==, -1000);
	g_assert_cmpint(balance(f, "1100", "2026-01-31T23:59:59Z"), ==, 6500);
}

/* One set of source documents must agree through posting, bank matching,
 * statements, close and restore; subsystem tests alone cannot prove that. */
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
	g_autoptr(GDateTime) cutoff = venture_time_from_string("2026-01-31", NULL);
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
		cutoff, &actor, &error));
	g_assert_no_error(error);
	finish_cycle(f, &actor);
}

static gboolean
reject_credit(VentureDatabase *db, VentureEntity *record, VentureEntity *previous,
	gpointer data, GError **error)
{
	(void)db; (void)record; (void)previous;
	if (!*(gboolean *)data)
		return TRUE;
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
	gboolean *reject = g_new(gboolean, 1);
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
	*reject = TRUE;
	venture_database_add_save_validator(f->db, VENTURE_TYPE_CUSTOMER_CREDIT, reject_credit, reject, g_free);
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
	g_clear_error(&error);
	*reject = FALSE;
	/* A retry reconstructs its command, not the failed mutable record. */
	g_clear_object(&payment);
	payment = venture_payment_new();
	g_object_set(payment, "customer-id", f->customer,
		"invoice-id", venture_entity_get_id(VENTURE_ENTITY(invoice)),
		"method", "transfer", "organization-id", f->org, NULL);
	money(VENTURE_ENTITY(payment), "amount", "40 USD");
	money(VENTURE_ENTITY(payment), "date", "2026-01-15");
	save(f, VENTURE_ENTITY(payment));
	g_assert_cmpuint(count_type(f, VENTURE_TYPE_PAYMENT), ==, 1);
	/* Receipt, customer credit and allocation post three linked journals. */
	g_assert_cmpuint(count_type(f, VENTURE_TYPE_JOURNAL), ==, journals + 3);
	g_assert_cmpint(balance(f, "1100", "2026-01-31T23:59:59Z"), ==, 0);
	g_assert_cmpint(balance(f, "1000", "2026-01-31T23:59:59Z"), ==, 4000);
}

/* Opening AR/AP are balances brought forward, not this period's sales or
 * expenses. Re-importing the same source must not duplicate either book. */
static void
test_opening_import(Fixture *f, gconstpointer data)
{
	static const gchar payload[] =
		"{\"source\":\"quickbooks\",\"cutoff\":\"2026-01-01\",\"currency\":\"USD\","
		"\"customers\":[{\"source_id\":\"accept-customer\",\"name\":\"Opening customer\"}],"
		"\"vendors\":[{\"source_id\":\"accept-vendor\",\"name\":\"Opening supplier\"}],"
		"\"open_ar\":[{\"source_id\":\"accept-invoice\",\"customer_source_id\":\"accept-customer\","
		"\"number\":\"OPEN-AR\",\"date\":\"2025-12-15\",\"net\":\"100 USD\",\"tax\":\"5 USD\"}],"
		"\"open_ap\":[{\"source_id\":\"accept-bill\",\"vendor_source_id\":\"accept-vendor\","
		"\"number\":\"OPEN-AP\",\"date\":\"2025-12-16\","
		"\"lines\":[{\"description\":\"Prior period materials\",\"amount\":\"50\",\"tax\":\"0\"}]}],"
		"\"bank_balances\":[{\"source_id\":\"accept-bank\",\"name\":\"Opening checking\","
		"\"account_code\":\"1000\",\"amount\":\"500 USD\"}],"
		"\"trial_balance\":[{\"account_code\":\"1000\",\"debit\":\"500 USD\"},"
		"{\"account_code\":\"1100\",\"debit\":\"105 USD\"},"
		"{\"account_code\":\"2000\",\"credit\":\"50 USD\"},"
		"{\"account_code\":\"3000\",\"credit\":\"555 USD\"}]}";
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) parsed = venture_json_parse(payload, &error);
	g_autoptr(VentureEntity) cutover = NULL;
	g_autoptr(VentureEntity) repeat = NULL;
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(VentureEntity) bill = NULL;
	g_autoptr(VenturePayment) receipt = venture_payment_new();
	g_autoptr(VentureBillPayment) payment = venture_bill_payment_new();
	g_autofree gchar *evidence = NULL;
	gint64 customer, vendor, source_org = f->org;
	(void)data;
	g_assert_no_error(error);
	cutover = venture_cutover_service_preview(venture_cutover_service_get(f->db),
		f->org, json_node_get_object(parsed), NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(cutover);
	g_assert_true(venture_cutover_service_import(venture_cutover_service_get(f->db),
		VENTURE_ACCOUNTING_CUTOVER(cutover), NULL, &error));
	g_assert_no_error(error);
	g_assert_true(venture_cutover_service_reconcile(venture_cutover_service_get(f->db),
		VENTURE_ACCOUNTING_CUTOVER(cutover), NULL, &error));
	g_assert_no_error(error);
	g_object_get(cutover, "reconciliation-report", &evidence, NULL);
	g_assert_nonnull(strstr(evidence, "Trial balance ties"));
	g_assert_cmpint(balance(f, "1000", "2026-01-01T23:59:59Z"), ==, 50000);
	g_assert_cmpint(balance(f, "1100", "2026-01-01T23:59:59Z"), ==, 10500);
	g_assert_cmpint(balance(f, "2000", "2026-01-01T23:59:59Z"), ==, -5000);
	g_assert_cmpint(balance(f, "3000", "2026-01-01T23:59:59Z"), ==, -55500);
	g_assert_cmpint(balance(f, "3900", "2026-01-01T23:59:59Z"), ==, 0);
	g_assert_cmpint(balance(f, "4000", "2026-01-01T23:59:59Z"), ==, 0);
	g_assert_cmpint(balance(f, "2100", "2026-01-01T23:59:59Z"), ==, 0);
	repeat = venture_cutover_service_preview(venture_cutover_service_get(f->db),
		f->org, json_node_get_object(parsed), NULL, &error);
	g_assert_no_error(error);
	g_assert_true(venture_cutover_service_import(venture_cutover_service_get(f->db),
		VENTURE_ACCOUNTING_CUTOVER(repeat), NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(count_type(f, VENTURE_TYPE_INVOICE), ==, 1);
	g_assert_cmpuint(count_type(f, VENTURE_TYPE_VENDOR_BILL), ==, 1);
	invoice = first(f, VENTURE_TYPE_INVOICE, "number", "OPEN-AR");
	bill = first(f, VENTURE_TYPE_VENDOR_BILL, "number", "OPEN-AP");
	g_object_get(invoice, "company-id", &customer, NULL);
	g_object_get(bill, "company-id", &vendor, NULL);
	g_object_set(receipt, "organization-id", f->org, "customer-id", customer,
		"invoice-id", venture_entity_get_id(invoice), "method", "transfer", NULL);
	money(VENTURE_ENTITY(receipt), "amount", "105 USD");
	money(VENTURE_ENTITY(receipt), "date", "2026-01-05");
	save(f, VENTURE_ENTITY(receipt));
	g_object_set(payment, "organization-id", f->org, "vendor-id", vendor,
		"bill-id", venture_entity_get_id(bill), "method", "transfer", NULL);
	money(VENTURE_ENTITY(payment), "amount", "50 USD");
	money(VENTURE_ENTITY(payment), "date", "2026-01-06");
	save(f, VENTURE_ENTITY(payment));
	g_assert_cmpint(balance(f, "1100", "2026-01-31T23:59:59Z"), ==, 0);
	g_assert_cmpint(balance(f, "2000", "2026-01-31T23:59:59Z"), ==, 0);
	g_assert_cmpint(balance(f, "1000", "2026-01-31T23:59:59Z"), ==, 55500);
	g_assert_cmpint(statement_cell(f, "income_statement", "net_income"), ==, 0);
	g_assert_cmpint(statement_cell(f, "balance_sheet", "difference"), ==, 0);
	f->org = venture_test_accounting_roundtrip(f->db, source_org);
	g_assert_cmpint(statement_cell(f, "income_statement", "net_income"), ==, 0);
	g_assert_cmpint(statement_cell(f, "balance_sheet", "difference"), ==, 0);
	f->org = source_org;
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	venture_entity_registry_register_builtins(venture_entity_registry_get_default());
	g_test_add("/accounting-acceptance/opening-import", Fixture, NULL, setup, test_opening_import, teardown);
	g_test_add("/accounting-acceptance/full-cycle", Fixture, NULL, setup, test_full_cycle, teardown);
	g_test_add("/accounting-acceptance/rollback-retry", Fixture, NULL, setup, test_rollback_retry, teardown);
	return g_test_run();
}
