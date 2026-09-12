/*
 * test-receivables.c - Settlement is the same transaction at every door.
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <venture.h>

#include <string.h>

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
	g_autoptr(GError) error = NULL;
	VentureReport *report;
	VentureReportResult *result;

	period = venture_date_range_parse(period_text, NULL, 1, &error);
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
	g_autoptr(GPtrArray) entries = NULL;
	g_autoptr(GHashTable) balances = NULL;
	GHashTableIter iter;
	gpointer balance;
	guint i;

	invoice = invoice_new(f, "LEDGER", "2026-07-01", "100 USD");
	payment = payment_new(f, venture_entity_get_id(invoice), "100 USD", "2026-07-10");
	save(f, payment);
	entries = rows(f, "ledger_entry");
	g_assert_cmpuint(entries->len, >=, 4);
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

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
#define ADD(name, function) g_test_add("/receivables/" name, Fixture, NULL, set_up, function, tear_down)
	ADD("records", test_records);
	ADD("direct-paid-refused", test_direct_paid_refused);
	ADD("partial-full", test_partial_full);
	ADD("atomic-failure", test_atomic_failure);
	ADD("issue-cutoff", test_issue_cutoff);
	ADD("historical-payment", test_historical_payment);
	ADD("deposit-allocations-refund", test_deposit_allocations_refund);
	ADD("credit-note", test_credit_note);
	ADD("duplicate", test_duplicate);
	ADD("ledger-batches", test_ledger_batches);
	ADD("transition-veto", test_transition_veto);
	ADD("batch", test_batch);
	g_test_add("/receivables/batch-rollback", Fixture, "fail", set_up, test_batch, tear_down);
	ADD("plugin-state", test_plugin_state);
	ADD("backdated-allocation", test_backdated_allocation);
	ADD("delete-cannot-set-paid", test_delete_cannot_set_paid);
	ADD("transition-cannot-edit-issued", test_transition_cannot_edit_issued);
	ADD("statement", test_statement);
	ADD("docs", test_docs);
	g_test_add("/receivables/zero", Fixture, "0 USD", set_up, test_invalid_amount, tear_down);
	g_test_add("/receivables/negative", Fixture, "-10 USD", set_up, test_invalid_amount, tear_down);
	g_test_add("/receivables/wrong-currency", Fixture, "100 EUR", set_up, test_invalid_amount, tear_down);
#undef ADD
	return g_test_run();
}
