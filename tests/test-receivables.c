/*
 * test-receivables.c - Settlement is the same transaction at every door.
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <venture.h>

#include <string.h>
#include <unistd.h>
#include <libsoup/soup.h>

#include "venture-test-util.h"

typedef struct
{
	VentureDatabase *database;
	VentureConfig *config;
	VentureContext *context;
	gint64 organization_id;
	gint64 customer_id;
} Fixture;

static void
save(Fixture *f, VentureEntity *record)
{
	g_autoptr(GError) error = NULL;
	gboolean ok;

	ok = venture_database_save(f->database, record, NULL, &error);
	g_assert_no_error(error);
	g_assert_true(ok);
}

static void
set_up(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureCompany) customer = NULL;

	f->config = venture_config_new();
	f->database = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->database,
		venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->database);
	f->organization_id = venture_context_get_default_organization_id(f->context);
	customer = venture_company_new();
	g_object_set(customer, "name", "Customer", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(customer), f->organization_id);
	save(f, VENTURE_ENTITY(customer));
	f->customer_id = venture_entity_get_id(VENTURE_ENTITY(customer));
}

static void
tear_down(Fixture *f, gconstpointer data)
{
	g_clear_object(&f->context);
	g_clear_object(&f->database);
	g_clear_object(&f->config);
}

static GType
record_type(const gchar *name)
{
	GType type;

	type = venture_entity_registry_lookup(venture_entity_registry_get_default(), name);
	g_assert_cmpuint(type, !=, G_TYPE_INVALID);
	return type;
}

static VentureEntity *
record_new(Fixture *f, const gchar *name)
{
	VentureEntity *record;

	record = g_object_new(record_type(name), NULL);
	venture_entity_set_organization_id(record, f->organization_id);
	return record;
}

static GPtrArray *
rows(Fixture *f, const gchar *name)
{
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GError) error = NULL;
	GPtrArray *found;

	query = venture_query_new(record_type(name));
	venture_query_set_limit(query, 0);
	venture_query_set_include_deleted(query, TRUE);
	g_assert_true(venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, &error));
	found = venture_database_find(f->database, query, &error);
	g_assert_no_error(error);
	g_assert_nonnull(found);
	return found;
}

static void
money_field(VentureEntity *record, const gchar *field, const gchar *amount)
{
	g_autoptr(GError) error = NULL;

	g_assert_true(venture_entity_set_field_from_string(record, field, amount, &error));
	g_assert_no_error(error);
}

static gint64
amount_field(VentureEntity *record, const gchar *field)
{
	g_autoptr(VentureMoney) amount = NULL;

	g_object_get(record, field, &amount, NULL);
	g_assert_nonnull(amount);
	return venture_money_get_amount(amount);
}

static VentureEntity *
invoice_new(Fixture *f, const gchar *number, const gchar *issued, const gchar *amount)
{
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(VentureEntity) line = NULL;

	invoice = record_new(f, "invoice");
	g_object_set(invoice, "number", number, "company-id", f->customer_id, NULL);
	money_field(invoice, "issued-at", issued);
	money_field(invoice, "due-at", issued);
	save(f, invoice);
	line = record_new(f, "invoice_line");
	g_object_set(line, "invoice-id", venture_entity_get_id(invoice),
		"description", "Work", "quantity", 1.0, NULL);
	money_field(line, "unit-price", amount);
	save(f, line);
	g_object_set(invoice, "status", VENTURE_INVOICE_STATUS_SENT, NULL);
	save(f, invoice);
	return g_steal_pointer(&invoice);
}

static VentureEntity *
payment_new(Fixture *f, gint64 invoice_id, const gchar *amount, const gchar *date)
{
	VentureEntity *payment;

	payment = record_new(f, "payment");
	g_object_set(payment, "customer-id", f->customer_id,
		"invoice-id", invoice_id, "method", "manual", NULL);
	money_field(payment, "amount", amount);
	money_field(payment, "date", date);
	return payment;
}

static void
assert_status(Fixture *f, VentureEntity *invoice, const gchar *expected)
{
	g_autoptr(VentureEntity) stored = NULL;
	gint status;

	stored = venture_database_get(f->database, VENTURE_TYPE_INVOICE,
		venture_entity_get_id(invoice), NULL);
	g_assert_nonnull(stored);
	g_object_get(stored, "status", &status, NULL);
	g_assert_cmpstr(venture_enum_to_nick(VENTURE_TYPE_INVOICE_STATUS, status), ==, expected);
}

/* Include the audit trail: rolling back just the business rows is not atomic. */
static gchar *
fingerprint(Fixture *f)
{
	g_autoptr(GChecksum) checksum = NULL;
	g_auto(GStrv) names = NULL;
	guint i;

	checksum = g_checksum_new(G_CHECKSUM_SHA256);
	names = venture_entity_registry_list_names(venture_entity_registry_get_default());
	for (i = 0; names[i] != NULL; i++)
	{
		g_autoptr(GPtrArray) all = NULL;
		guint j;

		all = rows(f, names[i]);
		for (j = 0; j < all->len; j++)
		{
			g_autoptr(JsonNode) node = NULL;
			g_autofree gchar *json = NULL;

			node = venture_serializable_to_json(VENTURE_SERIALIZABLE(
				g_ptr_array_index(all, j)), TRUE);
			json = venture_json_to_string(node, FALSE);
			g_checksum_update(checksum, (const guchar *)json, strlen(json));
		}
	}
	return g_strdup(g_checksum_get_string(checksum));
}

static void
test_records(Fixture *f, gconstpointer data)
{
	static const gchar *const names[] = {
		"payment", "payment_allocation", "customer_credit", "refund", NULL
	};
	guint i;

	for (i = 0; names[i] != NULL; i++)
	{
		g_autoptr(VentureEntity) record = NULL;
		GParamSpec *spec;

		record = record_new(f, names[i]);
		spec = g_object_class_find_property(G_OBJECT_GET_CLASS(record), "amount");
		g_assert_nonnull(spec);
		g_assert_cmpuint(G_PARAM_SPEC_VALUE_TYPE(spec), ==, VENTURE_TYPE_MONEY);
	}
}

static void
test_direct_paid_refused(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(GError) error = NULL;

	invoice = invoice_new(f, "DIRECT", "2026-07-01", "100 USD");
	g_object_set(invoice, "status", VENTURE_INVOICE_STATUS_PAID, NULL);
	g_assert_false(venture_database_save(f->database, invoice, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_nonnull(strstr(error->message, "VentureSettlementService"));
	assert_status(f, invoice, "sent");
}

static void
test_partial_full(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(VentureEntity) first = NULL;
	g_autoptr(VentureEntity) second = NULL;
	g_autoptr(GPtrArray) allocations = NULL;
	g_autoptr(GPtrArray) sales = NULL;

	invoice = invoice_new(f, "PART", "2026-07-01", "100 USD");
	first = payment_new(f, venture_entity_get_id(invoice), "40 USD", "2026-07-10");
	save(f, first);
	assert_status(f, invoice, "partially_paid");
	second = payment_new(f, venture_entity_get_id(invoice), "60 USD", "2026-08-10");
	save(f, second);
	assert_status(f, invoice, "paid");
	allocations = rows(f, "payment_allocation");
	g_assert_cmpuint(allocations->len, ==, 2);
	g_assert_cmpint(amount_field(g_ptr_array_index(allocations, 0), "amount"), ==, 4000);
	g_assert_cmpint(amount_field(g_ptr_array_index(allocations, 1), "amount"), ==, 6000);
	sales = rows(f, "sale");
	g_assert_cmpuint(sales->len, ==, 2);
}

static gboolean
reject_write(VentureDatabase *db, VentureEntity *record, VentureEntity *previous,
	gpointer data, GError **error)
{
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_DATABASE, "Injected after payment write");
	return FALSE;
}

static void
test_atomic_failure(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(VentureEntity) payment = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;

	invoice = invoice_new(f, "ATOMIC", "2026-07-01", "100 USD");
	payment = payment_new(f, venture_entity_get_id(invoice), "100 USD", "2026-08-01");
	before = fingerprint(f);
	venture_database_add_save_validator(f->database, record_type("customer_credit"),
		reject_write, NULL, NULL);
	g_assert_false(venture_database_save(f->database, payment, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_DATABASE);
	after = fingerprint(f);
	g_assert_cmpstr(before, ==, after);
	g_assert_false(venture_entity_is_persisted(payment));
	assert_status(f, invoice, "sent");
}

static VentureReportResult *
aging(Fixture *f, const gchar *period_text)
{
	g_autoptr(VentureDateRange) period = NULL;
	g_autoptr(GTimeZone) timezone = NULL;
	g_autoptr(GError) error = NULL;
	VentureReport *report;
	VentureReportResult *result;

	/* Bare event dates are UTC; keep the report cutoff in that timezone
	 * instead of inheriting the machine running the test. */
	timezone = g_time_zone_new_utc();
	period = venture_date_range_parse(period_text, timezone, 1, &error);
	g_assert_no_error(error);
	report = venture_report_registry_lookup(venture_context_get_report_registry(f->context), "receivables");
	g_assert_nonnull(report);
	result = venture_report_generate(report, f->context, period, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	return result;
}

static gint64
metric_amount(VentureReportResult *result, const gchar *name)
{
	GPtrArray *metrics;
	guint i;

	metrics = venture_report_result_get_metrics(result);
	for (i = 0; i < metrics->len; i++)
	{
		VentureMetric *metric;

		metric = g_ptr_array_index(metrics, i);
		if (g_strcmp0(venture_metric_get_key(metric), name) == 0)
			return venture_money_get_amount(venture_metric_get_money(metric));
	}
	g_assert_not_reached();
}

static void
test_issue_cutoff(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(VentureReportResult) result = NULL;

	invoice = invoice_new(f, "LATER", "2026-08-15", "100 USD");
	result = aging(f, "2026-07");
	g_assert_cmpint(metric_amount(result, "outstanding"), ==, 0);
}

/* July's exclusive endpoint is August 1. The age is measured on July
 * 31, when a July 1 due date is thirty days old, not thirty-one. */
static void
test_aging_month_end(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(VentureReportResult) result = NULL;
	const GValue *cell;

	invoice = invoice_new(f, "MONTH-END", "2026-07-01", "100 USD");
	result = aging(f, "2026-07");
	cell = venture_report_result_get_cell(result, 1, "amount");
	g_assert_nonnull(cell);
	g_assert_cmpint(venture_money_get_amount(g_value_get_boxed(cell)), ==, 10000);
	cell = venture_report_result_get_cell(result, 2, "amount");
	g_assert_cmpint(venture_money_get_amount(g_value_get_boxed(cell)), ==, 0);
}

static void
test_historical_payment(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(VentureEntity) payment = NULL;
	g_autoptr(VentureReportResult) result = NULL;

	invoice = invoice_new(f, "HISTORY", "2026-07-01", "100 USD");
	payment = payment_new(f, venture_entity_get_id(invoice), "100 USD", "2026-08-15");
	save(f, payment);
	assert_status(f, invoice, "paid");
	result = aging(f, "2026-07");
	g_assert_cmpint(metric_amount(result, "outstanding"), ==, 10000);
}

static VentureEntity *
allocation_new(Fixture *f, gint64 payment_id, gint64 credit_id,
	gint64 invoice_id, const gchar *amount, const gchar *date)
{
	VentureEntity *allocation;

	allocation = record_new(f, "payment_allocation");
	g_object_set(allocation, "payment-id", payment_id, "credit-id", credit_id,
		"invoice-id", invoice_id, NULL);
	money_field(allocation, "amount", amount);
	money_field(allocation, "date", date);
	return allocation;
}

static void
test_deposit_allocations_refund(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) first = NULL;
	g_autoptr(VentureEntity) second = NULL;
	g_autoptr(VentureEntity) payment = NULL;
	g_autoptr(VentureEntity) allocation = NULL;
	g_autoptr(VentureEntity) refund = NULL;
	g_autoptr(GPtrArray) credits = NULL;
	gint64 credit_id;

	first = invoice_new(f, "ONE", "2026-07-01", "50 USD");
	second = invoice_new(f, "TWO", "2026-07-01", "40 USD");
	payment = payment_new(f, 0, "100 USD", "2026-07-10");
	save(f, payment);
	credits = rows(f, "customer_credit");
	g_assert_cmpuint(credits->len, ==, 1);
	credit_id = venture_entity_get_id(g_ptr_array_index(credits, 0));
	g_assert_cmpint(amount_field(g_ptr_array_index(credits, 0), "remaining"), ==, 10000);
	allocation = allocation_new(f, venture_entity_get_id(payment), 0,
		venture_entity_get_id(first), "50 USD", "2026-07-12");
	save(f, allocation);
	g_clear_object(&allocation);
	allocation = allocation_new(f, 0, credit_id, venture_entity_get_id(second),
		"40 USD", "2026-07-13");
	save(f, allocation);
	assert_status(f, first, "paid");
	assert_status(f, second, "paid");
	g_clear_pointer(&credits, g_ptr_array_unref);
	credits = rows(f, "customer_credit");
	g_assert_cmpint(amount_field(g_ptr_array_index(credits, 0), "remaining"), ==, 1000);
	refund = record_new(f, "refund");
	g_object_set(refund, "customer-id", f->customer_id,
		"allocation-id", venture_entity_get_id(allocation), NULL);
	money_field(refund, "date", "2026-08-05");
	money_field(refund, "amount", "10 USD");
	save(f, refund);
	assert_status(f, second, "partially_paid");
	g_clear_object(&refund);
	refund = record_new(f, "refund");
	g_object_set(refund, "customer-id", f->customer_id, "credit-id", credit_id, NULL);
	money_field(refund, "date", "2026-08-06");
	money_field(refund, "amount", "10 USD");
	save(f, refund);
	g_clear_pointer(&credits, g_ptr_array_unref);
	credits = rows(f, "customer_credit");
	g_assert_cmpint(amount_field(g_ptr_array_index(credits, 0), "remaining"), ==, 0);
}

static void
test_credit_note(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(VentureEntity) credit = NULL;
	g_autoptr(VentureEntity) allocation = NULL;
	g_autoptr(GPtrArray) credits = NULL;

	invoice = invoice_new(f, "CREDIT", "2026-07-01", "100 USD");
	credit = record_new(f, "customer_credit");
	g_object_set(credit, "customer-id", f->customer_id, "kind", "credit_note", NULL);
	money_field(credit, "amount", "100 USD");
	money_field(credit, "date", "2026-07-10");
	save(f, credit);
	allocation = allocation_new(f, 0, venture_entity_get_id(credit),
		venture_entity_get_id(invoice), "100 USD", "2026-07-11");
	save(f, allocation);
	assert_status(f, invoice, "paid");
	credits = rows(f, "customer_credit");
	g_assert_cmpint(amount_field(g_ptr_array_index(credits, 0), "remaining"), ==, 0);
}

static void
test_duplicate(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) payment = NULL;
	g_autoptr(VentureEntity) duplicate = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;

	payment = payment_new(f, 0, "10 USD", "2026-07-01");
	g_object_set(payment, "external-id", "bank/42", NULL);
	save(f, payment);
	duplicate = payment_new(f, 0, "10 USD", "2026-07-01");
	g_object_set(duplicate, "external-id", "bank/42", NULL);
	before = fingerprint(f);
	g_assert_false(venture_database_save(f->database, duplicate, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_ALREADY_EXISTS);
	after = fingerprint(f);
	g_assert_cmpstr(before, ==, after);
}

static void
test_invalid_amount(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(VentureEntity) payment = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;

	invoice = invoice_new(f, "INVALID", "2026-07-01", "100 USD");
	payment = payment_new(f, venture_entity_get_id(invoice), data, "2026-07-10");
	before = fingerprint(f);
	g_assert_false(venture_database_save(f->database, payment, NULL, &error));
	g_assert_nonnull(error);
	after = fingerprint(f);
	g_assert_cmpstr(before, ==, after);
}

static void
test_ledger_batches(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(VentureEntity) payment = NULL;
	g_autoptr(VentureEntity) credit = NULL;
	g_autoptr(VentureEntity) allocation = NULL;
	g_autoptr(VentureEntity) refund = NULL;
	g_autoptr(GPtrArray) allocations = NULL;
	g_autoptr(GPtrArray) entries = NULL;
	g_autoptr(GHashTable) balances = NULL;
	GHashTableIter iter;
	gpointer balance;
	guint i;

	invoice = invoice_new(f, "LEDGER", "2026-07-01", "100 USD");
	payment = payment_new(f, venture_entity_get_id(invoice), "100 USD", "2026-07-10");
	save(f, payment);
	allocations = rows(f, "payment_allocation");
	refund = record_new(f, "refund");
	g_object_set(refund, "customer-id", f->customer_id,
		"allocation-id", venture_entity_get_id(g_ptr_array_index(allocations, 0)), NULL);
	money_field(refund, "amount", "100 USD");
	money_field(refund, "date", "2026-08-10");
	save(f, refund);
	credit = record_new(f, "customer_credit");
	g_object_set(credit, "customer-id", f->customer_id, "kind", "credit_note", NULL);
	money_field(credit, "amount", "100 USD");
	money_field(credit, "date", "2026-08-11");
	save(f, credit);
	allocation = allocation_new(f, 0, venture_entity_get_id(credit),
		venture_entity_get_id(invoice), "100 USD", "2026-08-12");
	save(f, allocation);
	entries = rows(f, "ledger_entry");
	/* Issue, receipt, deposit credit, cash allocation, refund, credit
	 * note and credit allocation each produce their own balanced batch. */
	g_assert_cmpuint(entries->len, ==, 14);
	balances = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	for (i = 0; i < entries->len; i++)
	{
		VentureEntity *entry;
		g_autofree gchar *transaction = NULL;
		g_autofree gchar *source = NULL;
		gint64 *sum;
		gint side;

		entry = g_ptr_array_index(entries, i);
		g_object_get(entry, "transaction-id", &transaction, "source-type", &source, "side", &side, NULL);
		g_assert_nonnull(source);
		sum = g_hash_table_lookup(balances, transaction);
		if (sum == NULL)
		{
			sum = g_new0(gint64, 1);
			g_hash_table_insert(balances, g_strdup(transaction), sum);
		}
		*sum += (side == VENTURE_LEDGER_SIDE_DEBIT ? 1 : -1) * amount_field(entry, "amount");
	}
	g_assert_cmpuint(g_hash_table_size(balances), ==, 7);
	g_hash_table_iter_init(&iter, balances);
	while (g_hash_table_iter_next(&iter, NULL, &balance))
		g_assert_cmpint(*(gint64 *)balance, ==, 0);
}

static gboolean
veto_transition(GObject *machine, VentureEntity *invoice, const gchar *from,
	const gchar *to, gpointer data)
{
	return FALSE;
}

/* The refund edges belong to derivation, not to a writer trying to erase
 * a paid state while leaving the cash and allocations untouched. */
static void
test_direct_reopen_refused(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(VentureEntity) payment = NULL;
	g_autoptr(VentureEntity) stored = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) date = NULL;
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;

	invoice = invoice_new(f, "REOPEN", "2026-07-01", "100 USD");
	payment = payment_new(f, venture_entity_get_id(invoice), "100 USD", "2026-07-10");
	save(f, payment);
	stored = venture_database_get(f->database, VENTURE_TYPE_INVOICE,
		venture_entity_get_id(invoice), NULL);
	before = fingerprint(f);
	if (data != NULL)
	{
		date = venture_time_now();
		g_assert_false(venture_settlement_service_transition(
			venture_settlement_service_get(f->database), VENTURE_INVOICE(stored),
			"sent", date, NULL, &error));
	}
	else
	{
		g_object_set(stored, "status", VENTURE_INVOICE_STATUS_SENT, NULL);
		g_assert_false(venture_database_save(f->database, stored, NULL, &error));
	}
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	after = fingerprint(f);
	g_assert_cmpstr(before, ==, after);
	assert_status(f, invoice, "paid");
}

/* Calling the public service must preserve the issued document just as
 * the generic save does; its mutable GObject is not a write permit. */
static void
test_transition_freezes_invoice(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) date = NULL;
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;

	invoice = invoice_new(f, "FROZEN", "2026-07-01", "100 USD");
	before = fingerprint(f);
	money_field(invoice, "due-at", "2026-07-20");
	date = venture_time_now();
	g_assert_false(venture_settlement_service_transition(
		venture_settlement_service_get(f->database), VENTURE_INVOICE(invoice),
		"void", date, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	after = fingerprint(f);
	g_assert_cmpstr(before, ==, after);
}

static void
test_transition_veto(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(VentureEntity) payment = NULL;
	g_autoptr(GObject) machine = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	GObject *service;
	GSignalQuery signal;

	invoice = invoice_new(f, "VETO", "2026-07-01", "100 USD");
	service = g_object_get_data(G_OBJECT(f->database), "venture-settlement-service");
	g_assert_nonnull(service);
	g_object_get(service, "state-machine", &machine, NULL);
	g_assert_nonnull(machine);
	g_signal_query(g_signal_lookup("transition", G_OBJECT_TYPE(machine)), &signal);
	g_assert_true((signal.signal_flags & G_SIGNAL_RUN_LAST) != 0);
	g_signal_connect(machine, "transition", G_CALLBACK(veto_transition), NULL);
	payment = payment_new(f, venture_entity_get_id(invoice), "100 USD", "2026-07-10");
	before = fingerprint(f);
	g_assert_false(venture_database_save(f->database, payment, NULL, &error));
	g_assert_nonnull(error);
	after = fingerprint(f);
	g_assert_cmpstr(before, ==, after);
}

static void
test_batch(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) first = NULL;
	g_autoptr(VentureEntity) second = NULL;
	g_autoptr(VentureEntity) payment = NULL;
	g_autoptr(GPtrArray) allocations = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	VentureSettlementService *service;
	gboolean ok;

	service = venture_settlement_service_get(f->database);
	first = invoice_new(f, "BATCH-1", "2026-07-01", "60 USD");
	second = invoice_new(f, "BATCH-2", "2026-07-01", "40 USD");
	payment = payment_new(f, 0, "100 USD", "2026-07-10");
	allocations = g_ptr_array_new_with_free_func(g_object_unref);
	g_ptr_array_add(allocations, allocation_new(f, 0, 0, venture_entity_get_id(first), "60 USD", "2026-07-10"));
	g_ptr_array_add(allocations, allocation_new(f, 0, 0, venture_entity_get_id(second), data != NULL ? "50 USD" : "40 USD", "2026-07-10"));
	before = fingerprint(f);
	ok = venture_settlement_service_apply_payment(service, VENTURE_PAYMENT(payment), allocations, NULL, &error);
	if (data != NULL)
	{
		g_assert_false(ok);
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
		after = fingerprint(f);
		g_assert_cmpstr(before, ==, after);
		g_assert_false(venture_entity_is_persisted(payment));
		g_assert_false(venture_entity_is_persisted(g_ptr_array_index(allocations, 0)));
		assert_status(f, first, "sent");
	}
	else
	{
		g_assert_no_error(error);
		g_assert_true(ok);
		assert_status(f, first, "paid");
		assert_status(f, second, "paid");
	}
}

static void
test_plugin_state(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(VentureEntity) payment = NULL;
	g_autoptr(GDateTime) date = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureMoney) balance = NULL;
	VentureSettlementService *service;
	VentureInvoiceStateMachine *machine;

	service = venture_settlement_service_get(f->database);
	machine = venture_settlement_service_get_state_machine(service);
	invoice = invoice_new(f, "PLUGIN", "2026-07-01", "100 USD");
	g_assert_true(venture_invoice_state_machine_add_state(machine, "disputed", VENTURE_INVOICE_STATUS_SENT, &error));
	g_assert_no_error(error);
	g_assert_true(venture_invoice_state_machine_add_transition(machine, "sent", "disputed", &error));
	g_assert_true(venture_invoice_state_machine_add_transition(machine, "disputed", "sent", &error));
	g_assert_true(venture_invoice_state_machine_add_transition(machine, "disputed", "paid", &error));
	date = g_date_time_new_from_iso8601("2026-07-02T00:00:00Z", NULL);
	g_assert_true(venture_settlement_service_transition(service, VENTURE_INVOICE(invoice), "disputed", date, NULL, &error));
	g_assert_true(venture_settlement_service_transition(service, VENTURE_INVOICE(invoice), "sent", date, NULL, &error));
	g_assert_true(venture_settlement_service_transition(service, VENTURE_INVOICE(invoice), "disputed", date, NULL, &error));
	g_assert_no_error(error);
	balance = venture_settlement_service_invoice_balance(service, venture_entity_get_id(invoice), NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpint(venture_money_get_amount(balance), ==, 10000);
	payment = payment_new(f, venture_entity_get_id(invoice), "100 USD", "2026-07-10");
	save(f, payment);
	assert_status(f, invoice, "paid");
	g_assert_false(venture_invoice_state_machine_add_state(machine, "disputed", VENTURE_INVOICE_STATUS_SENT, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_ALREADY_EXISTS);
}

/* A future refund must never fund an allocation in an earlier statement. */
static void
test_backdated_allocation(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(VentureEntity) payment = NULL;
	g_autoptr(VentureEntity) deposit = NULL;
	g_autoptr(VentureEntity) refund = NULL;
	g_autoptr(VentureEntity) allocation = NULL;
	g_autoptr(GPtrArray) allocations = NULL;
	g_autoptr(GError) error = NULL;

	invoice = invoice_new(f, "BACKDATE", "2026-07-01", "100 USD");
	payment = payment_new(f, venture_entity_get_id(invoice), "100 USD", "2026-07-20");
	save(f, payment);
	allocations = rows(f, "payment_allocation");
	refund = record_new(f, "refund");
	g_object_set(refund, "customer-id", f->customer_id,
		"allocation-id", venture_entity_get_id(g_ptr_array_index(allocations, 0)), NULL);
	money_field(refund, "date", "2026-08-20");
	money_field(refund, "amount", "50 USD");
	save(f, refund);
	deposit = payment_new(f, 0, "50 USD", "2026-07-01");
	save(f, deposit);
	allocation = allocation_new(f, venture_entity_get_id(deposit), 0,
		venture_entity_get_id(invoice), "50 USD", "2026-07-25");
	g_assert_false(venture_database_save(f->database, allocation, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
}

/* Deletion writes all columns; an unsaved paid field must not hitch a ride. */
static void
test_delete_cannot_set_paid(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(GError) error = NULL;

	invoice = record_new(f, "invoice");
	g_object_set(invoice, "number", "DELETE-BYPASS", NULL);
	save(f, invoice);
	g_object_set(invoice, "status", VENTURE_INVOICE_STATUS_PAID, NULL);
	g_assert_false(venture_database_delete(f->database, invoice, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	assert_status(f, invoice, "draft");
}

static void
test_transition_cannot_edit_issued(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(GDateTime) date = NULL;
	g_autoptr(GError) error = NULL;

	invoice = invoice_new(f, "FROZEN", "2026-07-01", "100 USD");
	money_field(invoice, "issued-at", "2026-06-01");
	date = g_date_time_new_from_iso8601("2026-08-01T00:00:00Z", NULL);
	g_assert_false(venture_settlement_service_transition(venture_settlement_service_get(f->database),
		VENTURE_INVOICE(invoice), "void", date, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
}

static void
test_statement(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(VentureEntity) payment = NULL;
	g_autoptr(VentureDateRange) period = NULL;
	g_autoptr(VentureMoney) balance = NULL;
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(JsonObject) options = NULL;
	g_autoptr(GError) error = NULL;
	VentureReport *report;

	invoice = invoice_new(f, "STATEMENT", "2026-07-01", "100 USD");
	payment = payment_new(f, venture_entity_get_id(invoice), "120 USD", "2026-08-15");
	save(f, payment);
	period = venture_date_range_parse("2026-07", NULL, 1, &error);
	g_assert_no_error(error);
	balance = venture_settlement_service_customer_balance(venture_settlement_service_get(f->database),
		f->organization_id, f->customer_id, venture_date_range_get_end(period), "USD", &error);
	g_assert_no_error(error);
	g_assert_cmpint(venture_money_get_amount(balance), ==, 10000);
	g_clear_pointer(&balance, venture_money_free);
	balance = venture_settlement_service_customer_balance(venture_settlement_service_get(f->database),
		f->organization_id, f->customer_id, NULL, "USD", &error);
	g_assert_no_error(error);
	g_assert_cmpint(venture_money_get_amount(balance), ==, -2000);
	g_clear_pointer(&period, venture_date_range_free);
	period = venture_date_range_parse("2026-08", NULL, 1, &error);
	options = json_object_new();
	json_object_set_int_member(options, "customer_id", f->customer_id);
	report = venture_report_registry_lookup(venture_context_get_report_registry(f->context), "customer_statement");
	g_assert_nonnull(report);
	result = venture_report_generate(report, f->context, period, options, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	g_assert_cmpint(metric_amount(result, "opening"), ==, 10000);
	g_assert_cmpint(metric_amount(result, "balance"), ==, -2000);
	g_assert_cmpuint(venture_report_result_get_row_count(result), ==, 1);
}

static void
test_docs(Fixture *f, gconstpointer data)
{
	g_autofree gchar *index = NULL;
	g_autofree gchar *modules = NULL;
	g_autofree gchar *doc = NULL;

	g_assert_true(g_file_get_contents("docs/receivables.org", &doc, NULL, NULL));
	g_assert_true(g_file_get_contents("docs/index.org", &index, NULL, NULL));
	g_assert_true(g_file_get_contents("docs/modules.org", &modules, NULL, NULL));
	g_assert_nonnull(strstr(index, "file:receivables.org"));
	g_assert_nonnull(strstr(modules, "| =receivables="));
	g_assert_nonnull(strstr(doc, "venture_receivables_post_batch"));
}

typedef struct
{
	GType type;
	guint writes;
} WriteProbe;

static void
observe_write(VentureDatabase *database, VentureEntity *record, gboolean created, gpointer data)
{
	WriteProbe *probe;

	probe = data;
	if (G_OBJECT_TYPE(record) == probe->type)
		probe->writes++;
}

static void
test_operation_rollback(Fixture *f, gconstpointer data)
{
	const gchar *operation;
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(VentureEntity) payment = NULL;
	g_autoptr(VentureEntity) target = NULL;
	g_autoptr(GPtrArray) allocations = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	WriteProbe probe;
	gulong handler;

	operation = data;
	invoice = invoice_new(f, "ROLLBACK", "2026-07-01", "100 USD");
	if (g_str_equal(operation, "credit"))
	{
		target = record_new(f, "customer_credit");
		g_object_set(target, "customer-id", f->customer_id, "kind", "credit_note", NULL);
		money_field(target, "amount", "40 USD");
		money_field(target, "date", "2026-07-10");
	}
	else if (g_str_equal(operation, "allocation"))
	{
		payment = payment_new(f, 0, "100 USD", "2026-07-10");
		save(f, payment);
		target = allocation_new(f, venture_entity_get_id(payment), 0, venture_entity_get_id(invoice), "40 USD", "2026-07-20");
	}
	else if (g_str_equal(operation, "refund"))
	{
		payment = payment_new(f, venture_entity_get_id(invoice), "100 USD", "2026-07-10");
		save(f, payment);
		allocations = rows(f, "payment_allocation");
		target = record_new(f, "refund");
		g_object_set(target, "customer-id", f->customer_id,
			"allocation-id", venture_entity_get_id(g_ptr_array_index(allocations, 0)), NULL);
		money_field(target, "amount", "40 USD");
		money_field(target, "date", "2026-07-20");
	}
	else
	{
		/* Voiding is also a lifecycle event with an atomic posting. */
		target = g_object_ref(invoice);
		g_object_set(target, "status", VENTURE_INVOICE_STATUS_VOID, NULL);
	}
	before = fingerprint(f);
	probe.type = VENTURE_IS_INVOICE(target) ? VENTURE_TYPE_INVOICE_EVENT : G_OBJECT_TYPE(target);
	probe.writes = 0;
	handler = g_signal_connect(f->database, "entity-saved", G_CALLBACK(observe_write), &probe);
	venture_database_add_save_validator(f->database, VENTURE_TYPE_LEDGER_ENTRY, reject_write, NULL, NULL);
	g_assert_false(venture_database_save(f->database, target, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_DATABASE);
	g_assert_cmpuint(probe.writes, ==, 1);
	g_signal_handler_disconnect(f->database, handler);
	after = fingerprint(f);
	g_assert_cmpstr(before, ==, after);
	if (!VENTURE_IS_INVOICE(target))
		g_assert_false(venture_entity_is_persisted(target));
}

static void
test_refusals(Fixture *f, gconstpointer data)
{
	const gchar *kind;
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(VentureEntity) payment = NULL;
	g_autoptr(VentureEntity) target = NULL;
	g_autoptr(GPtrArray) credits = NULL;
	g_autoptr(GPtrArray) allocations = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;

	kind = data;
	invoice = invoice_new(f, "REFUSAL", "2026-07-01", "100 USD");
	if (g_str_equal(kind, "over-allocation") || g_str_equal(kind, "two-sources") || g_str_equal(kind, "no-source"))
	{
		payment = payment_new(f, 0, "40 USD", "2026-07-10");
		save(f, payment);
		credits = rows(f, "customer_credit");
		target = allocation_new(f, g_str_equal(kind, "no-source") ? 0 : venture_entity_get_id(payment),
			g_str_equal(kind, "two-sources") ? venture_entity_get_id(g_ptr_array_index(credits, 0)) : 0,
			venture_entity_get_id(invoice), "50 USD", "2026-07-20");
	}
	else if (g_str_equal(kind, "over-refund") || g_str_equal(kind, "refund-two-sources"))
	{
		payment = payment_new(f, venture_entity_get_id(invoice), "40 USD", "2026-07-10");
		save(f, payment);
		allocations = rows(f, "payment_allocation");
		credits = rows(f, "customer_credit");
		target = record_new(f, "refund");
		g_object_set(target, "customer-id", f->customer_id,
			"allocation-id", venture_entity_get_id(g_ptr_array_index(allocations, 0)),
			"credit-id", g_str_equal(kind, "refund-two-sources") ? venture_entity_get_id(g_ptr_array_index(credits, 0)) : 0, NULL);
		money_field(target, "date", "2026-07-20");
		money_field(target, "amount", "50 USD");
	}
	else if (g_str_equal(kind, "frozen-line"))
	{
		allocations = rows(f, "invoice_line");
		target = g_object_ref(g_ptr_array_index(allocations, 0));
		money_field(target, "unit-price", "200 USD");
	}
	else if (g_str_equal(kind, "credit-kind") || g_str_equal(kind, "zero-credit"))
	{
		target = record_new(f, "customer_credit");
		g_object_set(target, "customer-id", f->customer_id, "kind",
			g_str_equal(kind, "credit-kind") ? "deposit" : "credit_note", NULL);
		money_field(target, "date", "2026-07-10");
		money_field(target, "amount", g_str_equal(kind, "zero-credit") ? "0 USD" : "40 USD");
	}
	else if (g_str_equal(kind, "insert-paid"))
	{
		target = record_new(f, "invoice");
		g_object_set(target, "number", "DIRECT-INSERT", "status", VENTURE_INVOICE_STATUS_PAID, NULL);
	}
	else
	{
		target = payment_new(f, venture_entity_get_id(invoice), "40 USD",
			g_str_equal(kind, "early-payment") ? "2026-06-01" : "2026-07-10");
		if (g_str_equal(kind, "wrong-customer"))
		{
			g_autoptr(VentureEntity) customer = NULL;

			customer = record_new(f, "company");
			g_object_set(customer, "name", "Another customer", NULL);
			save(f, customer);
			g_object_set(target, "customer-id", venture_entity_get_id(customer), NULL);
		}
		if (g_str_equal(kind, "wrong-organization"))
		{
			g_autoptr(VentureEntity) org = NULL;

			org = record_new(f, "organization");
			g_object_set(org, "name", "Second business", "slug", "second-business", NULL);
			save(f, org);
			venture_entity_set_organization_id(target, venture_entity_get_id(org));
		}
	}
	before = fingerprint(f);
	g_assert_false(venture_database_save(f->database, target, NULL, &error));
	g_assert_nonnull(error);
	after = fingerprint(f);
	g_assert_cmpstr(before, ==, after);
}

static void
test_immutable_history(Fixture *f, gconstpointer data)
{
	static const gchar *const names[] = { "payment", "customer_credit", "payment_allocation", "refund", "invoice_event", NULL };
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(VentureEntity) payment = NULL;
	g_autoptr(VentureEntity) refund = NULL;
	g_autoptr(GPtrArray) allocations = NULL;
	guint i;

	invoice = invoice_new(f, "IMMUTABLE", "2026-07-01", "100 USD");
	payment = payment_new(f, venture_entity_get_id(invoice), "100 USD", "2026-07-10");
	save(f, payment);
	allocations = rows(f, "payment_allocation");
	refund = record_new(f, "refund");
	g_object_set(refund, "customer-id", f->customer_id,
		"allocation-id", venture_entity_get_id(g_ptr_array_index(allocations, 0)), NULL);
	money_field(refund, "amount", "10 USD");
	money_field(refund, "date", "2026-07-20");
	save(f, refund);
	for (i = 0; names[i] != NULL; i++)
	{
		g_autoptr(GPtrArray) records = NULL;
		g_autoptr(GError) error = NULL;
		g_autofree gchar *before = NULL;
		g_autofree gchar *after = NULL;
		VentureEntity *record;

		records = rows(f, names[i]);
		record = g_ptr_array_index(records, 0);
		before = fingerprint(f);
		money_field(record, "amount", "200 USD");
		g_assert_false(venture_database_save(f->database, record, NULL, &error));
		g_assert_nonnull(error);
		g_clear_error(&error);
		g_assert_false(venture_database_delete(f->database, record, NULL, &error));
		g_assert_nonnull(error);
		g_clear_error(&error);
		g_assert_false(venture_database_purge(f->database, record, NULL, &error));
		g_assert_nonnull(error);
		after = fingerprint(f);
		g_assert_cmpstr(before, ==, after);
	}
}

static void
test_external_id_per_organization(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) first = NULL;
	g_autoptr(VentureEntity) second = NULL;
	g_autoptr(VentureEntity) org = NULL;
	g_autoptr(VentureEntity) customer = NULL;
	g_autoptr(VentureEntity) cash = NULL;
	g_autoptr(VentureEntity) receivable = NULL;
	g_autofree gchar *first_key = NULL;
	g_autofree gchar *second_key = NULL;
	gint64 organization_id;

	first = payment_new(f, 0, "10 USD", "2026-07-01");
	g_object_set(first, "external-id", "same-id", NULL);
	save(f, first);
	org = record_new(f, "organization");
	g_object_set(org, "name", "Second organization", "slug", "second-org", NULL);
	save(f, org);
	organization_id = venture_entity_get_id(org);
	customer = record_new(f, "company");
	g_object_set(customer, "name", "Second customer", NULL);
	venture_entity_set_organization_id(customer, organization_id);
	save(f, customer);
	cash = record_new(f, "account");
	g_object_set(cash, "code", "second-1000", "name", "Second cash", "active", TRUE, "kind", VENTURE_ACCOUNT_KIND_ASSET, NULL);
	venture_entity_set_organization_id(cash, organization_id);
	save(f, cash);
	receivable = record_new(f, "account");
	g_object_set(receivable, "code", "second-1100", "name", "Second receivable", "active", TRUE, "kind", VENTURE_ACCOUNT_KIND_ASSET, NULL);
	venture_entity_set_organization_id(receivable, organization_id);
	save(f, receivable);
	g_object_set(venture_settlement_service_get(f->database), "cash-account-id", venture_entity_get_id(cash),
		"receivable-account-id", venture_entity_get_id(receivable), NULL);
	second = payment_new(f, 0, "10 USD", "2026-07-01");
	g_object_set(second, "external-id", "same-id", "customer-id", venture_entity_get_id(customer), NULL);
	venture_entity_set_organization_id(second, organization_id);
	save(f, second);
	g_object_get(first, "external-key", &first_key, NULL);
	g_object_get(second, "external-key", &second_key, NULL);
	g_assert_cmpstr(first_key, !=, second_key);
	g_assert_true((venture_entity_class_get_column_flags(VENTURE_ENTITY_GET_CLASS(first), "external-key") & VENTURE_COLUMN_FLAG_UNIQUE) != 0);
}

typedef struct
{
	gboolean done;
	GBytes *bytes;
	GError *error;
	gchar *out;
	gchar *err;
} SurfaceResult;

static void
http_done(GObject *source, GAsyncResult *result, gpointer data)
{
	SurfaceResult *response;

	response = data;
	response->bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &response->error);
	response->done = TRUE;
}

static guint
http_request(VentureWebServer *server, const gchar *method, const gchar *path,
	const gchar *content_type, const gchar *body, gchar **out)
{
	g_autoptr(SoupSession) session = NULL;
	g_autoptr(SoupMessage) message = NULL;
	g_autofree gchar *url = NULL;
	SurfaceResult response;
	guint status;

	memset(&response, 0, sizeof(response));
	session = soup_session_new_with_options("timeout", 15, NULL);
	url = g_strconcat(venture_web_server_get_base_url(server), path, NULL);
	message = soup_message_new(method, url);
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	if (body != NULL)
	{
		g_autoptr(GBytes) bytes = NULL;

		bytes = g_bytes_new(body, strlen(body));
		soup_message_set_request_body_from_bytes(message, content_type, bytes);
	}
	soup_session_send_and_read_async(session, message, G_PRIORITY_DEFAULT, NULL, http_done, &response);
	while (!response.done)
		g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(response.error);
	if (out != NULL)
		*out = g_strndup(g_bytes_get_data(response.bytes, NULL), g_bytes_get_size(response.bytes));
	status = soup_message_get_status(message);
	g_clear_pointer(&response.bytes, g_bytes_unref);
	return status;
}

static void
cli_done(GObject *source, GAsyncResult *result, gpointer data)
{
	SurfaceResult *response;

	response = data;
	g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, &response->out, &response->err, &response->error);
	response->done = TRUE;
}

static gboolean
cli_timeout(gpointer data)
{
	g_subprocess_force_exit(G_SUBPROCESS(data));
	return G_SOURCE_CONTINUE;
}

static gchar *
run_cli(const gchar *const *argv, const gchar *input, gboolean success)
{
	g_autoptr(GSubprocess) process = NULL;
	g_autoptr(GSubprocessLauncher) launcher = NULL;
	g_autoptr(GError) error = NULL;
	SurfaceResult response;
	guint timeout;

	memset(&response, 0, sizeof(response));
	launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE);
	/* MCP insists on a token even when the private loopback fixture has
	 * authentication disabled. Never inherit a real install's credential. */
	g_subprocess_launcher_setenv(launcher, "VENTURE_TOKEN", "receivables-test-only", TRUE);
	process = g_subprocess_launcher_spawnv(launcher, argv, &error);
	g_assert_no_error(error);
	timeout = g_timeout_add_seconds(30, cli_timeout, process);
	g_subprocess_communicate_utf8_async(process, input, NULL, cli_done, &response);
	while (!response.done)
		g_main_context_iteration(NULL, TRUE);
	g_source_remove(timeout);
	g_assert_no_error(response.error);
	if (g_subprocess_get_successful(process) != success)
		g_test_message("CLI stdout: %s; stderr: %s", response.out, response.err);
	g_assert_cmpint(g_subprocess_get_successful(process), ==, success);
	g_free(response.err);
	return response.out;
}

static VentureWebServer *
start_server(Fixture *f, gchar **state_dir)
{
	g_autoptr(GSocketListener) listener = NULL;
	g_autoptr(GError) error = NULL;
	VentureWebServer *server;
	guint16 port;

	*state_dir = g_dir_make_tmp("venture-receivables-XXXXXX", &error);
	g_assert_no_error(error);
	listener = g_socket_listener_new();
	port = g_socket_listener_add_any_inet_port(listener, NULL, &error);
	g_assert_no_error(error);
	g_socket_listener_close(listener);
	g_object_set(f->config, "state-dir", *state_dir, "server-bind-address", "127.0.0.1",
		"server-port", (gint64)port, "security-require-auth", FALSE, NULL);
	server = venture_web_server_new(f->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(server, &error));
	g_assert_no_error(error);
	return server;
}

/* IDs and business fields are compared exactly. UUIDs and write clocks are
 * nondeterministic; audit actors deliberately differ between surfaces. */
static gchar *
settlement_rows(Fixture *f)
{
	static const gchar *const types[] = {
		"invoice", "invoice_event", "payment", "customer_credit", "payment_allocation", "sale", "ledger_entry", NULL
	};
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) root = NULL;
	guint t;

	builder = json_builder_new();
	json_builder_begin_object(builder);
	for (t = 0; types[t] != NULL; t++)
	{
		g_autoptr(GPtrArray) records = NULL;
		guint i;

		records = rows(f, types[t]);
		json_builder_set_member_name(builder, types[t]);
		json_builder_begin_array(builder);
		for (i = 0; i < records->len; i++)
		{
			JsonNode *node;
			JsonObject *object;

			node = venture_serializable_to_json(VENTURE_SERIALIZABLE(g_ptr_array_index(records, i)), FALSE);
			object = json_node_get_object(node);
			if (g_str_equal(types[t], "sale"))
			{
				const gchar *external = json_object_get_string_member(object, "external_id");
				g_assert_true(g_str_has_prefix(external, "allocation:"));
				g_assert_true(g_uuid_string_is_valid(external + strlen("allocation:")));
				json_object_set_string_member(object, "external_id", "allocation:<uuid>");
			}
			json_object_remove_member(object, "uuid");
			json_object_remove_member(object, "created_at");
			json_object_remove_member(object, "updated_at");
			if (g_str_equal(types[t], "ledger_entry"))
			{
				g_autofree gchar *transaction = NULL;

				transaction = g_strdup_printf("%s:%" G_GINT64_FORMAT,
					json_object_get_string_member(object, "source_type"), json_object_get_int_member(object, "source_id"));
				json_object_set_string_member(object, "transaction_id", transaction);
			}
			json_builder_add_value(builder, node);
		}
		json_builder_end_array(builder);
	}
	json_builder_end_object(builder);
	root = json_builder_get_root(builder);
	return venture_json_to_string(root, FALSE);
}

static void
test_surfaces(Fixture *unused, gconstpointer data)
{
	g_autofree gchar *expected = NULL;
	g_autofree gchar *date_text = NULL;
	g_autofree gchar *cli_path = NULL;
	guint surface;

	cli_path = g_canonicalize_filename("build/debug/venturectl", NULL);
	for (surface = 0; surface < 5; surface++)
	{
		Fixture f;
		g_autoptr(VentureEntity) invoice = NULL;
		g_autoptr(VentureWebServer) server = NULL;
		g_autofree gchar *state_dir = NULL;
		g_autofree gchar *json = NULL;
		g_autofree gchar *actual = NULL;
		g_autofree gchar *response = NULL;

		set_up(&f, NULL);
		invoice = invoice_new(&f, "SURFACE", "2026-07-01", "100 USD");
		server = start_server(&f, &state_dir);
		if (surface == 0)
		{
			g_autoptr(GPtrArray) payments = NULL;
			g_autoptr(GDateTime) date = NULL;

			g_assert_cmpuint(http_request(server, "POST", "/invoices/1/status", "application/x-www-form-urlencoded", "to=paid", NULL), ==, 302);
			payments = rows(&f, "payment");
			g_assert_cmpuint(payments->len, ==, 1);
			g_object_get(g_ptr_array_index(payments, 0), "date", &date, NULL);
			date_text = g_date_time_format_iso8601(date);
		}
		else
		{
			json = g_strdup_printf("{\"customer_id\":1,\"invoice_id\":1,\"date\":\"%s\",\"amount\":\"100 USD\",\"method\":\"manual\"}", date_text);
			if (surface == 1)
				g_assert_cmpuint(http_request(server, "POST", "/api/v1/payment", "application/json", json, &response), ==, 201);
			else if (surface == 2)
			{
				g_autofree gchar *date_arg = NULL;
				const gchar *health_argv[] = { cli_path, "--server", venture_web_server_get_base_url(server), "health", NULL };
				const gchar *describe_argv[] = { cli_path, "--server", venture_web_server_get_base_url(server), "describe", "payment", NULL };
				const gchar *argv[] = { cli_path, "--server", venture_web_server_get_base_url(server), "-f", "json",
					"create", "payment", "customer_id=1", "invoice_id=1", "amount=100 USD", "method=manual", NULL, NULL };

				response = run_cli(health_argv, NULL, TRUE);
				g_clear_pointer(&response, g_free);
				response = run_cli(describe_argv, NULL, TRUE);
				g_assert_nonnull(strstr(response, "customer_id"));
				g_clear_pointer(&response, g_free);
				date_arg = g_strconcat("date=", date_text, NULL);
				argv[G_N_ELEMENTS(argv) - 2] = date_arg;
				response = run_cli(argv, NULL, TRUE);
			}
			else
			{
				g_autofree gchar *input = NULL;
				g_autofree gchar *before = NULL;
				const gchar *argv[] = { cli_path, "--server", venture_web_server_get_base_url(server), "mcp", surface == 3 ? "--apply-writes" : NULL, NULL };

				input = g_strdup_printf("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{},\"clientInfo\":{\"name\":\"settlement-test\",\"version\":\"1\"}}}\n"
					"{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n"
					"{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":{\"name\":\"venture_create\",\"arguments\":{\"type\":\"payment\",\"values\":%s}}}\n", json);
				before = fingerprint(&f);
				response = run_cli(argv, input, TRUE);
				g_assert_null(strstr(response, "\"isError\":true"));
				if (surface == 4)
				{
					g_autofree gchar *after = NULL;
					g_autofree gchar *path = NULL;
					g_autoptr(GPtrArray) pending = NULL;

					after = fingerprint(&f);
					g_assert_cmpstr(before, ==, after);
					pending = venture_confirmation_store_list_pending(venture_context_get_confirmations(f.context));
					g_assert_cmpuint(pending->len, ==, 1);
					path = g_strdup_printf("/api/v1/confirmations/%s/approve", venture_confirmation_get_id(g_ptr_array_index(pending, 0)));
					g_assert_cmpuint(http_request(server, "POST", path, "application/json", "{}", NULL), ==, 200);
				}
			}
		}
		assert_status(&f, invoice, "paid");
		actual = settlement_rows(&f);
		if (expected == NULL)
			expected = g_strdup(actual);
		else
			g_assert_cmpstr(actual, ==, expected);
		/* A generic paid mutation is still refused at the same boundary. */
		{
			g_autoptr(VentureEntity) draft = NULL;

			draft = record_new(&f, "invoice");
			g_object_set(draft, "number", "NO-DIRECT-PAID", NULL);
			save(&f, draft);
			g_clear_pointer(&response, g_free);
			g_assert_cmpuint(http_request(server, "PATCH", "/api/v1/invoice/2", "application/json", "{\"status\":\"paid\"}", &response), ==, 422);
			g_assert_nonnull(strstr(response, "VentureSettlementService"));
		}
		venture_web_server_stop(server);
		g_clear_object(&server);
		tear_down(&f, NULL);
		venture_test_remove_tree(state_dir);
	}
}

static void
test_module_switch(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(VentureEntity) payment = NULL;
	g_autoptr(VentureWebServer) server = NULL;
	g_autofree gchar *state_dir = NULL;
	g_autofree gchar *body = NULL;
	g_autoptr(VentureModuleRegistry) modules = NULL;
	g_autoptr(VentureConfig) config = NULL;
	g_autoptr(GError) error = NULL;

	invoice = invoice_new(f, "SWITCH", "2026-07-01", "100 USD");
	payment = payment_new(f, venture_entity_get_id(invoice), "40 USD", "2026-07-10");
	save(f, payment);
	server = start_server(f, &state_dir);
	g_assert_cmpuint(http_request(server, "GET", "/e/invoice/1", NULL, NULL, &body), ==, 200);
	g_assert_nonnull(strstr(body, "name=\"to\" value=\"paid\""));
	g_clear_pointer(&body, g_free);
	venture_config_set_module_enabled(f->config, "receivables", FALSE);
	g_assert_cmpuint(venture_entity_registry_lookup(venture_entity_registry_get_default(), "payment"), ==, G_TYPE_INVALID);
	g_assert_null(venture_report_registry_lookup(venture_context_get_report_registry(f->context), "receivables"));
	g_assert_null(venture_report_registry_lookup(venture_context_get_report_registry(f->context), "customer_statement"));
	g_assert_cmpuint(http_request(server, "GET", "/api/v1/payment", NULL, NULL, NULL), ==, 404);
	g_assert_cmpuint(http_request(server, "POST", "/invoices/1/status", "application/x-www-form-urlencoded", "to=paid", NULL), ==, 404);
	g_assert_cmpuint(http_request(server, "GET", "/api/v1/schema", NULL, NULL, &body), ==, 200);
	g_assert_null(strstr(body, "\"payment\""));
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(http_request(server, "GET", "/e/invoice/1", NULL, NULL, &body), ==, 200);
	g_assert_null(strstr(body, "name=\"to\" value=\"paid\""));
	venture_config_set_module_enabled(f->config, "receivables", TRUE);
	g_assert_cmpuint(venture_entity_registry_lookup(venture_entity_registry_get_default(), "payment"), !=, G_TYPE_INVALID);
	g_assert_nonnull(venture_report_registry_lookup(venture_context_get_report_registry(f->context), "customer_statement"));
	venture_web_server_stop(server);
	g_clear_object(&server);
	venture_test_remove_tree(state_dir);
	modules = venture_module_registry_new();
	venture_module_registry_register_builtins(modules);
	config = venture_config_new();
	venture_config_set_module_enabled(config, "invoicing", FALSE);
	g_assert_false(venture_module_registry_configure(modules, config, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
	g_assert_nonnull(strstr(error->message, "receivables"));
}

/* Invoicing remains enabled when its optional receivables dependent is off;
 * sending a draft must therefore remain usable. */
static void
test_module_off_invoice(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) invoice = NULL;

	(void)data;
	venture_config_set_module_enabled(f->config, "receivables", FALSE);
	invoice = invoice_new(f, "REVIEW-OFF", "2026-01-10", "100 USD");
	g_assert_nonnull(invoice);
	venture_config_set_module_enabled(f->config, "receivables", TRUE);
}

/* A report requiring customer_id must receive that option through its
 * public endpoint, not only through direct C calls in its unit tests. */
static void
test_statement_options(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureWebServer) server = NULL;
	g_autoptr(VentureEntity) invoice = NULL;
	g_autofree gchar *state_dir = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *body = NULL;
	g_autofree gchar *cli = g_canonicalize_filename("build/debug/venturectl", NULL);
	g_autofree gchar *customer = g_strdup_printf("customer_id=%" G_GINT64_FORMAT, f->customer_id);
	g_autofree gchar *response = NULL;
	g_autoptr(JsonNode) expected = NULL;
	g_autoptr(JsonNode) actual = NULL;
	const gchar *argv[] = { cli, "--server", NULL, "-f", "json", "report", "customer_statement", "2026-01", customer, "currency=EUR", NULL };

	(void)data;
	invoice = invoice_new(f, "STATEMENT-OPTIONS", "2026-01-10", "100 EUR");
	server = start_server(f, &state_dir);
	path = g_strdup_printf("/api/v1/reports/customer_statement?period=2026-01&%s&currency=EUR", customer);
	g_assert_cmpuint(http_request(server, "GET", path, NULL, NULL, &body), ==, 200);
	g_assert_nonnull(strstr(body, "invoice_event #1"));
	expected = venture_json_parse(body, NULL);
	argv[2] = venture_web_server_get_base_url(server);
	response = run_cli(argv, NULL, TRUE);
	actual = venture_json_parse(response, NULL);
	g_assert_nonnull(actual);
	g_assert_true(json_node_equal(expected, actual));
	g_clear_pointer(&body, g_free);
	g_clear_pointer(&path, g_free);
	path = g_strdup_printf("/reports/customer_statement?period=2026-01&%s&currency=EUR", customer);
	g_assert_cmpuint(http_request(server, "GET", path, NULL, NULL, &body), ==, 200);
	g_assert_nonnull(strstr(body, "invoice_event #1"));
	g_assert_nonnull(strstr(body, "currency=EUR"));
	venture_web_server_stop(server);
	g_clear_object(&server);
	venture_test_remove_tree(state_dir);
}

static gint64
account_id_for_code(Fixture *f, const gchar *code)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ACCOUNT);
	g_autoptr(VentureEntity) account = NULL;
	g_autoptr(GError) error = NULL;

	venture_query_set_organization(query, f->organization_id);
	g_assert_true(venture_query_add_filter_string(query, "code", VENTURE_FILTER_OP_EQ, code, &error));
	account = venture_database_find_one(f->database, query, &error);
	g_assert_no_error(error);
	g_assert_nonnull(account);
	return venture_entity_get_id(account);
}

static gint64
account_balance_amount(Fixture *f, gint64 account_id, const gchar *cutoff)
{
	g_autoptr(GDateTime) date = venture_time_from_string(cutoff, NULL);
	g_autoptr(VentureMoney) balance = NULL;
	g_autoptr(GError) error = NULL;

	balance = venture_posting_service_account_balance(venture_database_get_posting_service(f->database),
		account_id, f->organization_id, "USD", date, &error);
	g_assert_no_error(error);
	return venture_money_get_amount(balance);
}

static VentureEntity *
taxed_invoice(Fixture *f, const gchar *number, const gchar *issued, const gchar *price,
	gint64 tax_percent)
{
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(VentureEntity) line = NULL;

	invoice = record_new(f, "invoice");
	g_object_set(invoice, "number", number, "company-id", f->customer_id, NULL);
	money_field(invoice, "issued-at", issued);
	money_field(invoice, "due-at", issued);
	save(f, invoice);
	line = record_new(f, "invoice_line");
	g_object_set(line, "invoice-id", venture_entity_get_id(invoice),
		"description", "Work", "quantity", 1.0, "tax-percent", tax_percent, NULL);
	money_field(line, "unit-price", price);
	save(f, line);
	g_object_set(invoice, "status", VENTURE_INVOICE_STATUS_SENT, NULL);
	save(f, invoice);
	return g_steal_pointer(&invoice);
}

/* 100 net + 5 tax must post AR 105, income 100, tax 5. A receipt moves cash
 * and AR only. Frozen line allocations survive issuance. */
static void
test_tax_issue_receipt(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(VentureEntity) payment = NULL;
	g_autoptr(GPtrArray) events = NULL;
	g_autoptr(GPtrArray) lines = NULL;
	g_autoptr(VentureMoney) income = NULL;
	g_autoptr(VentureMoney) tax = NULL;
	g_autoptr(GError) error = NULL;

	(void)data;
	invoice = taxed_invoice(f, "TAXED", "2026-01-10", "100 USD", 5);
	events = rows(f, "invoice_event");
	g_assert_cmpuint(events->len, ==, 1);
	g_object_get(g_ptr_array_index(events, 0), "net-amount", &income, "tax-amount", &tax, NULL);
	g_assert_cmpint(venture_money_get_amount(income), ==, 10000);
	g_assert_cmpint(venture_money_get_amount(tax), ==, 500);
	g_clear_pointer(&income, venture_money_free);
	g_clear_pointer(&tax, venture_money_free);
	lines = rows(f, "invoice_line");
	g_object_get(g_ptr_array_index(lines, 0), "income-amount", &income, "tax-amount", &tax, NULL);
	g_assert_cmpint(venture_money_get_amount(income), ==, 10000);
	g_assert_cmpint(venture_money_get_amount(tax), ==, 500);
	g_assert_cmpint(account_balance_amount(f, account_id_for_code(f, "1100"), "2026-01-10T23:59:59Z"), ==, 10500);
	g_assert_cmpint(account_balance_amount(f, account_id_for_code(f, "4000"), "2026-01-10T23:59:59Z"), ==, -10000);
	g_assert_cmpint(account_balance_amount(f, account_id_for_code(f, "2100"), "2026-01-10T23:59:59Z"), ==, -500);
	payment = payment_new(f, venture_entity_get_id(invoice), "105 USD", "2026-01-15");
	save(f, payment);
	g_assert_cmpint(account_balance_amount(f, account_id_for_code(f, "1000"), "2026-01-15T23:59:59Z"), ==, 10500);
	g_assert_cmpint(account_balance_amount(f, account_id_for_code(f, "1100"), "2026-01-15T23:59:59Z"), ==, 0);
	g_assert_cmpint(account_balance_amount(f, account_id_for_code(f, "4000"), "2026-01-15T23:59:59Z"), ==, -10000);
	g_assert_cmpint(account_balance_amount(f, account_id_for_code(f, "2100"), "2026-01-15T23:59:59Z"), ==, -500);
	g_assert_no_error(error);
}

/* A void and a credit note reverse the frozen income and tax legs, not an
 * aggregate income credit of the tax-inclusive total. */
static void
test_tax_void_credit(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(VentureEntity) other = NULL;
	g_autoptr(VentureEntity) credit = NULL;
	g_autoptr(VentureEntity) allocation = NULL;
	g_autoptr(GDateTime) date = venture_time_from_string("2026-01-12", NULL);
	g_autoptr(GError) error = NULL;
	VentureActor actor;

	(void)data;
	actor.kind = VENTURE_ACTOR_KIND_USER;
	actor.name = "bookkeeper";
	actor.prompt = NULL;
	actor.request_id = NULL;
	actor.approved_by = NULL;
	invoice = taxed_invoice(f, "VOIDTAX", "2026-01-10", "100 USD", 5);
	g_assert_true(venture_settlement_service_transition(venture_settlement_service_get(f->database),
		VENTURE_INVOICE(invoice), "void", date, &actor, &error));
	g_assert_no_error(error);
	g_assert_cmpint(account_balance_amount(f, account_id_for_code(f, "1100"), "2026-01-12T23:59:59Z"), ==, 0);
	g_assert_cmpint(account_balance_amount(f, account_id_for_code(f, "4000"), "2026-01-12T23:59:59Z"), ==, 0);
	g_assert_cmpint(account_balance_amount(f, account_id_for_code(f, "2100"), "2026-01-12T23:59:59Z"), ==, 0);
	other = taxed_invoice(f, "CREDTAX", "2026-02-01", "100 USD", 5);
	credit = record_new(f, "customer_credit");
	g_object_set(credit, "customer-id", f->customer_id, "kind", "credit_note", NULL);
	money_field(credit, "amount", "105 USD");
	money_field(credit, "tax-amount", "5 USD");
	money_field(credit, "date", "2026-02-10");
	save(f, credit);
	allocation = record_new(f, "payment_allocation");
	g_object_set(allocation, "credit-id", venture_entity_get_id(credit),
		"invoice-id", venture_entity_get_id(other), NULL);
	money_field(allocation, "amount", "105 USD");
	money_field(allocation, "date", "2026-02-11");
	save(f, allocation);
	g_assert_cmpint(account_balance_amount(f, account_id_for_code(f, "1100"), "2026-02-11T23:59:59Z"), ==, 0);
	g_assert_cmpint(account_balance_amount(f, account_id_for_code(f, "4000"), "2026-02-11T23:59:59Z"), ==, 0);
	g_assert_cmpint(account_balance_amount(f, account_id_for_code(f, "2100"), "2026-02-11T23:59:59Z"), ==, 0);
}

/* Read actual posted balances, not the settlement service's own rollups:
 * a balanced duplicate journal is still a financially wrong result. */
static void
assert_book_balance(Fixture *f, const gchar *code, const gchar *cutoff, gint64 expected)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ACCOUNT);
	g_autoptr(VentureEntity) account = NULL;
	g_autoptr(GDateTime) date = venture_time_from_string(cutoff, NULL);
	g_autoptr(VentureMoney) balance = NULL;
	g_autoptr(GError) error = NULL;

	venture_query_set_organization(query, f->organization_id);
	venture_query_add_filter_string(query, "code", VENTURE_FILTER_OP_EQ, code, NULL);
	account = venture_database_find_one(f->database, query, &error);
	g_assert_no_error(error);
	g_assert_nonnull(account);
	balance = venture_posting_service_account_balance(venture_database_get_posting_service(f->database),
		venture_entity_get_id(account), f->organization_id, "USD", date, &error);
	g_assert_no_error(error);
	g_assert_cmpint(balance->amount, ==, expected);
}

/* The integrated workflow must recognize each receipt once, accept a
 * second partial payment, and refund in February without rewriting January. */
static void
test_accounting_cycle(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(VentureEntity) first = NULL;
	g_autoptr(VentureEntity) second = NULL;
	g_autoptr(VentureEntity) refund = NULL;
	g_autoptr(VentureEntity) sale = NULL;
	g_autoptr(GPtrArray) allocations = NULL;
	g_autoptr(GPtrArray) periods = NULL;
	g_autoptr(VentureFiscalYear) year = NULL;
	g_autoptr(GDateTime) start = venture_time_from_string("2026-01-01", NULL);
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_FISCAL_PERIOD);
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureMoney) gross = venture_money_new_for_currency(99999, "USD");
	VentureActor actor;

	(void)data;
	actor.kind = VENTURE_ACTOR_KIND_USER;
	actor.name = "bookkeeper";
	actor.prompt = NULL;
	actor.request_id = NULL;
	actor.approved_by = NULL;
	year = venture_period_service_generate(venture_period_service_get(f->database),
		f->organization_id, "2026", start, VENTURE_PERIOD_MONTHLY, &actor, &error);
	g_assert_no_error(error);
	g_assert_nonnull(year);
	invoice = invoice_new(f, "INTEGRATED", "2026-01-10", "100 USD");
	first = payment_new(f, venture_entity_get_id(invoice), "40 USD", "2026-01-15");
	save(f, first);
	assert_book_balance(f, "1000", "2026-01-31T23:59:59Z", 4000);
	assert_book_balance(f, "1100", "2026-01-31T23:59:59Z", 6000);
	assert_book_balance(f, "4000", "2026-01-31T23:59:59Z", -10000);
	venture_query_set_organization(query, f->organization_id);
	venture_query_add_order(query, "start-at", VENTURE_SORT_ASCENDING, NULL);
	periods = venture_database_find(f->database, query, &error);
	g_assert_no_error(error);
	g_object_set(g_ptr_array_index(periods, 0), "state", VENTURE_PERIOD_CLOSED, NULL);
	g_assert_true(venture_database_save(f->database, g_ptr_array_index(periods, 0), &actor, &error));
	g_assert_no_error(error);
	second = payment_new(f, venture_entity_get_id(invoice), "60 USD", "2026-02-01");
	save(f, second);
	assert_status(f, invoice, "paid");
	assert_book_balance(f, "1000", "2026-02-02", 10000);
	assert_book_balance(f, "1100", "2026-02-02", 0);
	assert_book_balance(f, "4000", "2026-02-02", -10000);
	allocations = rows(f, "payment_allocation");
	refund = record_new(f, "refund");
	g_object_set(refund, "customer-id", f->customer_id,
		"allocation-id", venture_entity_get_id(g_ptr_array_index(allocations, 0)), NULL);
	money_field(refund, "amount", "10 USD");
	money_field(refund, "date", "2026-02-05");
	save(f, refund);
	assert_book_balance(f, "1000", "2026-02-06", 9000);
	assert_book_balance(f, "1100", "2026-02-06", 1000);
	assert_book_balance(f, "4000", "2026-02-06", -10000);
	assert_book_balance(f, "1000", "2026-01-31T23:59:59Z", 4000);
	{
		gint64 sale_id;
		g_object_get(g_ptr_array_index(allocations, 0), "sale-id", &sale_id, NULL);
		sale = venture_database_get(f->database, VENTURE_TYPE_SALE, sale_id, &error);
		g_assert_no_error(error);
		g_assert_cmpint(amount_field(sale, "gross"), ==, 4000);
		g_object_set(sale, "gross", gross, NULL);
		g_assert_false(venture_database_save(f->database, sale, NULL, &error));
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	}
}

int
main(int argc, char **argv)
{
	static const gchar *const refusals[] = {
		"over-allocation", "two-sources", "no-source", "over-refund", "refund-two-sources", "frozen-line",
		"credit-kind", "zero-credit", "insert-paid", "early-payment", "wrong-customer", "wrong-organization", NULL
	};
	static const gchar *const operations[] = { "credit", "allocation", "refund", "void", NULL };
	guint i;

	g_test_init(&argc, &argv, NULL);
#define ADD(name, function) g_test_add("/receivables/" name, Fixture, NULL, set_up, function, tear_down)
	ADD("records", test_records);
	ADD("tax-issue-receipt", test_tax_issue_receipt);
	ADD("tax-void-credit", test_tax_void_credit);
	ADD("accounting-cycle", test_accounting_cycle);
	ADD("module-off-invoice", test_module_off_invoice);
	ADD("statement-options", test_statement_options);
	ADD("direct-paid-refused", test_direct_paid_refused);
	ADD("partial-full", test_partial_full);
	ADD("atomic-failure", test_atomic_failure);
	ADD("issue-cutoff", test_issue_cutoff);
	ADD("aging-month-end", test_aging_month_end);
	ADD("historical-payment", test_historical_payment);
	ADD("deposit-allocations-refund", test_deposit_allocations_refund);
	ADD("credit-note", test_credit_note);
	ADD("duplicate", test_duplicate);
	ADD("ledger-batches", test_ledger_batches);
	ADD("transition-veto", test_transition_veto);
	ADD("direct-reopen-refused", test_direct_reopen_refused);
	g_test_add("/receivables/service-reopen-refused", Fixture, "service",
		set_up, test_direct_reopen_refused, tear_down);
	ADD("transition-freezes-invoice", test_transition_freezes_invoice);
	ADD("batch", test_batch);
	g_test_add("/receivables/batch-rollback", Fixture, "fail", set_up, test_batch, tear_down);
	ADD("plugin-state", test_plugin_state);
	ADD("backdated-allocation", test_backdated_allocation);
	ADD("delete-cannot-set-paid", test_delete_cannot_set_paid);
	ADD("transition-cannot-edit-issued", test_transition_cannot_edit_issued);
	ADD("statement", test_statement);
	ADD("docs", test_docs);
	ADD("surfaces", test_surfaces);
	ADD("module-switch", test_module_switch);
	ADD("immutable-history", test_immutable_history);
	ADD("external-id-per-organization", test_external_id_per_organization);
	for (i = 0; refusals[i] != NULL; i++)
	{
		g_autofree gchar *path = NULL;

		path = g_strconcat("/receivables/refuse/", refusals[i], NULL);
		g_test_add(path, Fixture, refusals[i], set_up, test_refusals, tear_down);
	}
	for (i = 0; operations[i] != NULL; i++)
	{
		g_autofree gchar *path = NULL;

		path = g_strconcat("/receivables/rollback/", operations[i], NULL);
		g_test_add(path, Fixture, operations[i], set_up, test_operation_rollback, tear_down);
	}
	g_test_add("/receivables/zero", Fixture, "0 USD", set_up, test_invalid_amount, tear_down);
	g_test_add("/receivables/negative", Fixture, "-10 USD", set_up, test_invalid_amount, tear_down);
	g_test_add("/receivables/wrong-currency", Fixture, "100 EUR", set_up, test_invalid_amount, tear_down);
#undef ADD
	return g_test_run();
}
