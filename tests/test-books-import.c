/*
 * test-books-import.c - A whole Zoho Books history, replayed and reconciled.
 *
 * The fixture is an invented company's backup in Zoho's own CSV shapes:
 * a day-one bank balance against Zoho's opening adjustments account, paid
 * and open invoices (one with a line discount and a late fee), a draft and
 * a void, payments with bank charges and a clearing deposit account, bills
 * with purchase tax and a rounding adjustment, expenses with inclusive tax,
 * an owner deposit and a transfer.
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <venture.h>
#include <string.h>
#include "venture-test-util.h"
#include "venture-test-accounting.h"

typedef struct
{
	VentureDatabase *db;
	VentureConfig *config;
	VentureContext *context;
	gint64 org;
} Fixture;

static const gchar *const CHART =
	"Account ID,Account Name,Account Code,Description,Account Type,Account Status,Currency\n"
	"9100001,Checking,,,Bank,Active,USD\n"
	"9100002,Stripe Clearing,,,Payment Clearing,Active,USD\n"
	"9100003,Accounts Receivable,,,Accounts Receivable,Active,USD\n"
	"9100004,Accounts Payable,,,Accounts Payable,Active,USD\n"
	"9100005,Opening Balance Adjustments,,,Other Current Liability,Active,USD\n"
	"9100006,Owner's Equity,,,Equity,Active,USD\n"
	"9100007,Sales,,,Income,Active,USD\n"
	"9100008,Late Fee Income,,,Income,Active,USD\n"
	"9100009,Discount,,,Income,Active,USD\n"
	"9100010,Software,,,Expense,Active,USD\n"
	"9100011,Tax Paid Expense,,,Expense,Active,USD\n"
	"9100012,Other Expenses,,,Expense,Active,USD\n"
	"9100013,Card Fees,,,Expense,Active,USD\n";

static const gchar *const CONTACTS =
	"Contact ID,Display Name,Company Name,EmailID,Phone,Opening Balance,Payment Terms Label\n"
	"9200001,Acme Widgets,Acme Widgets,ap@acme.invalid,555-0100,,Net 15\n"
	"9200002,Bluebird Bakery,Bluebird Bakery,owner@bluebird.invalid,,,Due on receipt\n";

static const gchar *const VENDORS =
	"Contact ID,Display Name,Company Name,EmailID,Phone,Opening Balance,Payment Terms Label\n"
	"9300001,Toolsmith Supply,Toolsmith Supply,sales@toolsmith.invalid,,,\n"
	"9300002,Hosting Hut,Hosting Hut,,,,\n";

static const gchar *const INVOICES =
	"Invoice Date,Invoice ID,Invoice Number,Invoice Status,Customer ID,Customer Name,Due Date,Currency Code,"
	"SubTotal,Total,Balance,Adjustment,Adjustment Account,Shipping Charge,Entity Discount Amount,Item Name,Item Desc,"
	"Quantity,Item Price,Item Total,Item Tax Amount,Account,Payment Terms Label,Notes\n"
	"2025-01-10,9400001,INV-101,Closed,9200001,Acme Widgets,2025-01-25,USD,300.00,300.00,0.00,0.00,,0.00,0.0,"
	"Consulting,January work,2.00,150.00,300.00,0.00,Sales,Net 15,\n"
	"2025-02-03,9400002,INV-102,Overdue,9200002,Bluebird Bakery,2025-02-03,USD,805.00,805.00,405.00,0.00,,0.00,0.0,"
	"Design,Menu design,1.00,1000.00,800.00,0.00,Sales,Due on receipt,\n"
	"2025-02-03,9400002,INV-102,Overdue,9200002,Bluebird Bakery,2025-02-03,USD,805.00,805.00,405.00,0.00,,0.00,0.0,"
	"Late fee,Late fee,1.00,5.00,5.00,0.00,Late Fee Income,Due on receipt,\n"
	"2025-02-20,9400003,INV-103,Draft,9200001,Acme Widgets,2025-03-07,USD,75.00,75.00,75.00,0.00,,0.00,0.0,"
	"Consulting,Draft work,1.00,75.00,75.00,0.00,Sales,Net 15,\n"
	"2025-02-21,9400004,INV-104,Void,9200002,Bluebird Bakery,2025-02-21,USD,60.00,60.00,0.00,0.00,,0.00,0.0,"
	"Design,Cancelled,1.00,60.00,60.00,0.00,Sales,Due on receipt,\n";

static const gchar *const PAYMENTS =
	"Payment Number,CustomerPayment ID,Mode,CustomerID,Amount,Unused Amount,Bank Charges,Reference Number,"
	"Currency Code,Customer Name,Date,Deposit To,Amount Applied to Invoice,Invoice Payment Applied Date,Invoice Number\n"
	"1,9500001,Card,9200001,300.00,0.00,9.00,ch_1,USD,Acme Widgets,2025-01-20,Checking,300.00,2025-01-20,INV-101\n"
	"2,9500002,Card,9200002,400.00,0.00,0.00,ch_2,USD,Bluebird Bakery,2025-03-01,Stripe Clearing,400.00,2025-03-01,INV-102\n";

static const gchar *const BILLS =
	"Bill Date,Due Date,Bill ID,Vendor Name,Bill Number,Currency Code,SubTotal,Total,Balance,Adjustment,"
	"Adjustment Account,Bill Status,Item Name,Account,Description,Quantity,Rate,Item Total,Tax Amount,Vendor Notes\n"
	"2025-01-15,2025-01-15,9600001,Toolsmith Supply,TS-77,USD,120.00,128.10,0.00,0.00,,Paid,Editor,Software,"
	"Editor licence,1.00,120.00,120.00,8.10,\n"
	"2025-02-15,2025-03-15,9600002,Hosting Hut,,USD,40.00,42.69,42.69,-0.01,Other Expenses,Overdue,Hosting,Software,"
	"February hosting,1.00,40.00,40.00,2.70,\n";

static const gchar *const VENDOR_PAYMENTS =
	"Payment Number,VendorPayment ID,Mode,Amount,Unused Amount,Reference Number,Currency Code,Bank Charges,Date,"
	"Vendor Name,Paid Through,Bill ID,Bill Amount,Bill Number\n"
	"1,9700001,Transfer,128.10,0.00,tr-1,USD,0.00,2025-01-25,Toolsmith Supply,Checking,9600001,128.10,TS-77\n";

static const gchar *const EXPENSES =
	"Expense Date,Expense Description,Expense Account,Paid Through,Vendor,Entry Number,Currency Code,Is Inclusive Tax,"
	"Tax Amount,Expense Amount,Total,Reference#\n"
	"2025-02-05,Font licence,Software,Checking,Typeface Co,E-1,USD,false,0.00,50.00,50.00,\n"
	"2025-03-05,Shelving,Other Expenses,Checking,Hardware Hall,E-2,USD,true,4.20,64.20,64.20,\n";

static const gchar *const DEPOSITS =
	"Transaction Date,Transaction Type,From Account,To Account,Reference,Description,Currency Code,Total\n"
	"2025-01-05,Capital Introduced,Owner's Equity,Checking,,Owner money,USD,1000.00\n";

static const gchar *const TRANSFERS =
	"Transaction Date,Transaction Type,From Account,To Account,Reference,Description,Currency Code,Total\n"
	"2025-03-10,Funds Transfer,Stripe Clearing,Checking,,Payout,USD,400.00\n";

static const gchar *const QUOTES =
	"Quote Date,Quote ID,Quote Number,Quote Status,Customer Name,Total\n"
	"2025-01-02,9800001,QT-1,draft,Acme Widgets,99.00\n";

static void
save(Fixture *f, VentureEntity *record)
{
	g_autoptr(GError) error = NULL;
	gboolean ok = venture_database_save(f->db, record, NULL, &error);
	g_assert_no_error(error);
	g_assert_true(ok);
}

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

static JsonObject *
manifest_with(const gchar *invoices)
{
	JsonObject *manifest = json_object_new();
	JsonObject *files = json_object_new();
	JsonArray *opening = json_array_new();
	JsonObject *row;
	json_object_set_string_member(manifest, "source", "zoho_books");
	json_object_set_string_member(manifest, "start", "2025-01-01");
	json_object_set_string_member(manifest, "export_date", "2025-03-31");
	json_object_set_string_member(manifest, "currency", "USD");
	json_object_set_string_member(manifest, "bank_charges_account", "Card Fees");
	json_object_set_string_member(files, "Chart_of_Accounts.csv", CHART);
	json_object_set_string_member(files, "Contacts.csv", CONTACTS);
	json_object_set_string_member(files, "Vendors.csv", VENDORS);
	json_object_set_string_member(files, "Invoice.csv", invoices);
	json_object_set_string_member(files, "Customer_Payment.csv", PAYMENTS);
	json_object_set_string_member(files, "Bill.csv", BILLS);
	json_object_set_string_member(files, "Vendor_Payment.csv", VENDOR_PAYMENTS);
	json_object_set_string_member(files, "Expense.csv", EXPENSES);
	json_object_set_string_member(files, "Deposit.csv", DEPOSITS);
	json_object_set_string_member(files, "Transfer_Fund.csv", TRANSFERS);
	json_object_set_string_member(files, "Quote.csv", QUOTES);
	json_object_set_object_member(manifest, "files", files);
	row = json_object_new();
	json_object_set_string_member(row, "account", "Checking");
	json_object_set_string_member(row, "debit", "250.00");
	json_array_add_object_element(opening, row);
	row = json_object_new();
	json_object_set_string_member(row, "account", "Opening Balance Adjustments");
	json_object_set_string_member(row, "credit", "250.00");
	json_array_add_object_element(opening, row);
	json_object_set_array_member(manifest, "opening", opening);
	return manifest;
}

static JsonObject *
manifest(void)
{
	return manifest_with(INVOICES);
}

static VentureBooksImportService *
service(Fixture *f)
{
	return venture_books_import_service_get(f->db);
}

static VentureBooksImport *
preview(Fixture *f)
{
	g_autoptr(JsonObject) m = manifest();
	g_autoptr(GError) error = NULL;
	VentureEntity *batch = venture_books_import_service_preview(service(f), f->org, m, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(batch);
	return VENTURE_BOOKS_IMPORT(batch);
}

static gchar *
report(VentureBooksImport *batch)
{
	gchar *text = NULL;
	g_object_get(batch, "report", &text, NULL);
	return text;
}

static gchar *
state(Fixture *f, VentureBooksImport *batch)
{
	g_autoptr(VentureEntity) stored = venture_database_get(f->db, VENTURE_TYPE_BOOKS_IMPORT,
		venture_entity_get_id(VENTURE_ENTITY(batch)), NULL);
	gchar *text = NULL;
	g_assert_nonnull(stored);
	g_object_get(stored, "state", &text, NULL);
	return text;
}

static void
import(Fixture *f, VentureBooksImport *batch)
{
	g_autoptr(GError) error = NULL;
	gboolean ok = venture_books_import_service_import(service(f), batch, NULL, &error);
	g_assert_no_error(error);
	g_assert_true(ok);
}

/* The importer matches the starter chart ignoring case ("Owner's equity"). */
static gint64
account_named(Fixture *f, const gchar *name)
{
	g_autoptr(VentureQuery) q = venture_query_new(VENTURE_TYPE_ACCOUNT);
	g_autoptr(GPtrArray) found = NULL;
	g_autofree gchar *wanted = g_utf8_casefold(name, -1);
	gint64 id = 0;
	guint i, matches = 0;
	venture_query_set_organization(q, f->org);
	venture_query_set_limit(q, 0);
	found = venture_database_find(f->db, q, NULL);
	g_assert_nonnull(found);
	for (i = 0; i < found->len; i++)
	{
		g_autofree gchar *candidate = NULL;
		g_autofree gchar *folded = NULL;
		g_object_get(g_ptr_array_index(found, i), "name", &candidate, NULL);
		folded = g_utf8_casefold(candidate != NULL ? candidate : "", -1);
		if (g_strcmp0(folded, wanted) == 0)
		{
			id = venture_entity_get_id(g_ptr_array_index(found, i));
			matches++;
		}
	}
	g_assert_cmpuint(matches, ==, 1);
	return id;
}

static gint64
account_coded(Fixture *f, const gchar *code)
{
	g_autoptr(VentureQuery) q = venture_query_new(VENTURE_TYPE_ACCOUNT);
	g_autoptr(GPtrArray) found = NULL;
	venture_query_set_organization(q, f->org);
	g_assert_true(venture_query_add_filter_string(q, "code", VENTURE_FILTER_OP_EQ, code, NULL));
	found = venture_database_find(f->db, q, NULL);
	g_assert_nonnull(found);
	g_assert_cmpuint(found->len, ==, 1);
	return venture_entity_get_id(g_ptr_array_index(found, 0));
}

static gint64
balance_at(Fixture *f, gint64 account, const gchar *when)
{
	g_autoptr(GDateTime) as_of = g_date_time_new_from_iso8601(when, NULL);
	g_autoptr(VentureMoney) value = NULL;
	g_autoptr(GError) error = NULL;
	value = venture_posting_service_account_balance(venture_database_get_posting_service(f->db),
		account, f->org, "USD", as_of, &error);
	g_assert_no_error(error);
	g_assert_nonnull(value);
	return venture_money_get_amount(value);
}

static gint64
balance(Fixture *f, const gchar *name)
{
	return balance_at(f, account_named(f, name), "2025-03-31T23:59:59Z");
}

static gint64
count(Fixture *f, const gchar *type)
{
	g_autoptr(VentureQuery) q = NULL;
	GType gtype = venture_entity_registry_lookup(venture_entity_registry_get_default(), type);
	if (gtype == G_TYPE_INVALID)
		return 0;
	q = venture_query_new(gtype);
	venture_query_set_organization(q, f->org);
	return venture_database_count(f->db, q, NULL);
}

static VentureEntity *
invoice_numbered(Fixture *f, const gchar *number)
{
	g_autoptr(VentureQuery) q = venture_query_new(VENTURE_TYPE_INVOICE);
	g_autoptr(GPtrArray) found = NULL;
	venture_query_set_organization(q, f->org);
	g_assert_true(venture_query_add_filter_string(q, "number", VENTURE_FILTER_OP_EQ, number, NULL));
	found = venture_database_find(f->db, q, NULL);
	g_assert_nonnull(found);
	if (found->len == 0)
		return NULL;
	return g_object_ref(g_ptr_array_index(found, 0));
}

/* Preview counts what the backup holds, lists what has no home and writes
 * no document. */
static void
test_preview(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureBooksImport) batch = NULL;
	g_autofree gchar *text = NULL;
	g_autofree gchar *now = NULL;
	(void)data;
	batch = preview(f);
	now = state(f, batch);
	g_assert_cmpstr(now, ==, "preview");
	text = report(batch);
	g_assert_nonnull(strstr(text, "invoices: 4 (2 posting, 2 draft or void)"));
	g_assert_nonnull(strstr(text, "customer payments: 2"));
	g_assert_nonnull(strstr(text, "bills: 2"));
	g_assert_nonnull(strstr(text, "expenses: 2"));
	g_assert_nonnull(strstr(text, "deposits and transfers: 2"));
	g_assert_nonnull(strstr(text, "NOT IMPORTED Quote.csv: 1 rows"));
	g_assert_null(strstr(text, "PROBLEM"));
	g_assert_nonnull(strstr(text, "ready to import"));
	g_assert_cmpint(count(f, "invoice"), ==, 0);
	g_assert_cmpint(count(f, "company"), ==, 0);
}

/* What cannot be read at all creates no batch. */
static void
test_preview_refusals(Fixture *f, gconstpointer data)
{
	g_autoptr(JsonObject) m = manifest();
	g_autoptr(GError) error = NULL;
	VentureEntity *batch;
	(void)data;
	json_object_set_string_member(m, "source", "quickbooks");
	batch = venture_books_import_service_preview(service(f), f->org, m, NULL, &error);
	g_assert_null(batch);
	g_assert_nonnull(strstr(error->message, "zoho_books"));
	g_clear_error(&error);
	json_object_set_string_member(m, "source", "zoho_books");
	json_object_set_string_member(m, "start", "soon");
	batch = venture_books_import_service_preview(service(f), f->org, m, NULL, &error);
	g_assert_null(batch);
	g_assert_nonnull(strstr(error->message, "start"));
	g_assert_cmpint(count(f, "books_import"), ==, 0);
}

/* A row naming an account the chart lacks is located on preview, and
 * import refuses before its first write. */
static void
test_located_problem(Fixture *f, gconstpointer data)
{
	g_autofree gchar *broken = NULL;
	g_autoptr(JsonObject) m = NULL;
	g_autoptr(VentureEntity) batch = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *text = NULL;
	(void)data;
	{
		/* INV-101's line credits an account the chart does not have. */
		const gchar *at = strstr(INVOICES, ",0.00,Sales,Net 15");
		g_assert_nonnull(at);
		broken = g_strdup_printf("%.*s,0.00,Consulting Income,Net 15%s", (gint)(at - INVOICES), INVOICES,
			at + strlen(",0.00,Sales,Net 15"));
	}
	m = manifest_with(broken);
	batch = venture_books_import_service_preview(service(f), f->org, m, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(batch);
	text = report(VENTURE_BOOKS_IMPORT(batch));
	g_assert_nonnull(strstr(text, "PROBLEM Invoice.csv row 2"));
	g_assert_false(venture_books_import_service_import(service(f), VENTURE_BOOKS_IMPORT(batch), NULL, &error));
	g_assert_nonnull(strstr(error->message, "before its first write"));
	g_assert_cmpint(count(f, "invoice"), ==, 0);
	g_assert_cmpint(count(f, "company"), ==, 0);
}

/* The replay reproduces Zoho's ledger account for account, keeps drafts as
 * drafts and voids as voids, sends nothing and reconciles. */
static void
test_import_reconcile(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureBooksImport) batch = NULL;
	g_autoptr(VentureEntity) draft = NULL;
	g_autoptr(VentureEntity) voided = NULL;
	g_autoptr(VentureEntity) open = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *text = NULL;
	g_autofree gchar *now = NULL;
	gint status = -1;
	gboolean dunning = FALSE, ok;
	gint64 mail_before;
	(void)data;
	mail_before = count(f, "mail_message");
	batch = preview(f);
	import(f, batch);
	g_assert_cmpint(balance(f, "Checking"), ==, 169870);
	g_assert_cmpint(balance(f, "Stripe Clearing"), ==, 0);
	g_assert_cmpint(balance(f, "Sales"), ==, -130000);
	g_assert_cmpint(balance(f, "Discount"), ==, 20000);
	g_assert_cmpint(balance(f, "Late Fee Income"), ==, -500);
	g_assert_cmpint(balance(f, "Software"), ==, 21000);
	g_assert_cmpint(balance(f, "Tax Paid Expense"), ==, 1500);
	g_assert_cmpint(balance(f, "Other Expenses"), ==, 5999);
	g_assert_cmpint(balance(f, "Card Fees"), ==, 900);
	g_assert_cmpint(balance(f, "Owner's Equity"), ==, -100000);
	g_assert_cmpint(balance(f, "Opening Balance Adjustments"), ==, -25000);
	g_assert_cmpint(balance_at(f, account_coded(f, "1100"), "2025-03-31T23:59:59Z"), ==, 40500);
	g_assert_cmpint(balance_at(f, account_coded(f, "2000"), "2025-03-31T23:59:59Z"), ==, -4269);
	/* History, not a summary: in mid-February the bank held what it did. */
	g_assert_cmpint(balance_at(f, account_named(f, "Checking"), "2025-02-10T00:00:00Z"), ==, 25000 + 100000 + 29100 - 12810 - 5000);
	draft = invoice_numbered(f, "INV-103");
	g_object_get(draft, "status", &status, NULL);
	g_assert_cmpint(status, ==, VENTURE_INVOICE_STATUS_DRAFT);
	voided = invoice_numbered(f, "INV-104");
	g_object_get(voided, "status", &status, NULL);
	g_assert_cmpint(status, ==, VENTURE_INVOICE_STATUS_VOID);
	open = invoice_numbered(f, "INV-102");
	g_object_get(open, "dunning-disabled", &dunning, NULL);
	g_assert_true(dunning);
	g_assert_cmpint(count(f, "mail_message"), ==, mail_before);
	g_assert_cmpint(count(f, "company"), ==, 4);
	ok = venture_books_import_service_reconcile(service(f), batch, NULL, &error);
	g_assert_no_error(error);
	g_assert_true(ok);
	now = state(f, batch);
	g_assert_cmpstr(now, ==, "reconciled");
	text = report(batch);
	g_assert_nonnull(strstr(text, "Result: reconciled"));
	g_assert_nonnull(strstr(text, "PASS invoices 2025-02"));
	g_assert_null(strstr(text, "FAIL"));
}

/* A second import of an imported batch changes nothing. */
static void
test_import_idempotent(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureBooksImport) batch = NULL;
	gint64 invoices;
	(void)data;
	batch = preview(f);
	import(f, batch);
	invoices = count(f, "invoice");
	import(f, batch);
	g_assert_cmpint(count(f, "invoice"), ==, invoices);
	g_assert_cmpint(balance(f, "Checking"), ==, 169870);
}

/* A ledger that moved after the import no longer reconciles. */
static void
test_reconcile_drift(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureBooksImport) batch = NULL;
	g_autoptr(VentureEntity) expense = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *text = NULL;
	g_autofree gchar *now = NULL;
	(void)data;
	batch = preview(f);
	import(f, batch);
	expense = venture_entity_registry_create(venture_entity_registry_get_default(), "expense", NULL);
	venture_entity_set_organization_id(expense, f->org);
	g_object_set(expense, "description", "Stray", "vendor", "Somebody", "expense-account-id", account_named(f, "Software"),
		"cash-account-id", account_named(f, "Checking"), NULL);
	g_assert_true(venture_entity_set_field_from_string(expense, "amount", "1.00 USD", NULL));
	g_assert_true(venture_entity_set_field_from_string(expense, "occurred-at", "2025-03-20", NULL));
	save(f, expense);
	g_assert_false(venture_books_import_service_reconcile(service(f), batch, NULL, &error));
	g_assert_nonnull(error);
	g_assert_nonnull(strstr(error->message, "Software"));
	now = state(f, batch);
	g_assert_cmpstr(now, ==, "imported");
	text = report(batch);
	g_assert_nonnull(strstr(text, "FAIL account Software"));
}

/* Rollback leaves every account at zero and frees every number, so the
 * same backup imports again; activation closes that door. */
static void
test_rollback_and_reimport(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureBooksImport) batch = NULL;
	g_autoptr(VentureBooksImport) again = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *now = NULL;
	g_autofree gchar *later = NULL;
	g_autoptr(GDateTime) today = venture_time_now();
	g_autoptr(GDateTime) tomorrow = g_date_time_add_days(today, 1);
	g_autofree gchar *when = g_date_time_format_iso8601(tomorrow);
	gboolean ok;
	(void)data;
	batch = preview(f);
	import(f, batch);
	g_assert_true(venture_books_import_service_rollback(service(f), batch, NULL, &error));
	g_assert_no_error(error);
	now = state(f, batch);
	g_assert_cmpstr(now, ==, "rolled_back");
	g_assert_cmpint(balance_at(f, account_named(f, "Checking"), when), ==, 0);
	g_assert_cmpint(balance_at(f, account_named(f, "Sales"), when), ==, 0);
	g_assert_cmpint(balance_at(f, account_named(f, "Software"), when), ==, 0);
	g_assert_cmpint(balance_at(f, account_coded(f, "1100"), when), ==, 0);
	g_assert_cmpint(balance_at(f, account_coded(f, "2000"), when), ==, 0);
	again = preview(f);
	import(f, again);
	ok = venture_books_import_service_reconcile(service(f), again, NULL, &error);
	g_assert_no_error(error);
	g_assert_true(ok);
	g_assert_true(venture_books_import_service_activate(service(f), again, NULL, &error));
	g_assert_no_error(error);
	later = state(f, again);
	g_assert_cmpstr(later, ==, "active");
	g_assert_false(venture_books_import_service_rollback(service(f), again, NULL, &error));
	g_assert_nonnull(error);
}

/* The same history cannot be imported twice. */
static void
test_owned_elsewhere(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureBooksImport) first = NULL;
	g_autoptr(VentureBooksImport) second = NULL;
	g_autofree gchar *text = NULL;
	(void)data;
	first = preview(f);
	import(f, first);
	second = preview(f);
	text = report(second);
	g_assert_nonnull(strstr(text, "already imported by books import"));
}

/* Books import records are the service's own. */
static void
test_generic_write_refused(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureBooksImport) batch = NULL;
	g_autoptr(GError) error = NULL;
	(void)data;
	batch = preview(f);
	g_object_set(batch, "state", "active", NULL);
	g_assert_false(venture_database_save(f->db, VENTURE_ENTITY(batch), NULL, &error));
	g_assert_nonnull(strstr(error->message, "VentureBooksImportService"));
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	venture_entity_registry_register_builtins(venture_entity_registry_get_default());
	g_test_add("/books-import/preview", Fixture, NULL, setup, test_preview, teardown);
	g_test_add("/books-import/preview-refusals", Fixture, NULL, setup, test_preview_refusals, teardown);
	g_test_add("/books-import/located-problem", Fixture, NULL, setup, test_located_problem, teardown);
	g_test_add("/books-import/import-reconcile", Fixture, NULL, setup, test_import_reconcile, teardown);
	g_test_add("/books-import/import-idempotent", Fixture, NULL, setup, test_import_idempotent, teardown);
	g_test_add("/books-import/reconcile-drift", Fixture, NULL, setup, test_reconcile_drift, teardown);
	g_test_add("/books-import/rollback-and-reimport", Fixture, NULL, setup, test_rollback_and_reimport, teardown);
	g_test_add("/books-import/owned-elsewhere", Fixture, NULL, setup, test_owned_elsewhere, teardown);
	g_test_add("/books-import/generic-write-refused", Fixture, NULL, setup, test_generic_write_refused, teardown);
	return g_test_run();
}
