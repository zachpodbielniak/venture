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
	result = aging(f, "2026-07");
	g_assert_cmpint(metric_amount(result, "outstanding"), ==, 10000);
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
#undef ADD
	return g_test_run();
}
