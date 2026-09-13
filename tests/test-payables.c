/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <string.h>

static void
test_records(void)
{
	static const gchar *const names[] = {
		"vendor_bill", "vendor_bill_line", "bill_payment",
		"bill_payment_allocation", "vendor_credit", "vendor_bill_event"
	};
	guint i;

	/* Every generated surface must discover the same financial records. */
	for (i = 0; i < G_N_ELEMENTS(names); i++)
		g_assert_cmpuint(venture_entity_registry_lookup(
			venture_entity_registry_get_default(), names[i]), !=, G_TYPE_INVALID);
}


typedef struct
{
	VentureDatabase *db;
	VentureContext *context;
	VentureConfig *config;
	gint64 org;
	gint64 vendor;
} Fixture;

static void
save(Fixture *f, VentureEntity *e)
{
	g_autoptr(GError) error = NULL;
	gboolean ok = venture_database_save(f->db, e, NULL, &error);
	g_assert_no_error(error);
	g_assert_true(ok);
}

static void
field(VentureEntity *e, const gchar *name, const gchar *value)
{
	g_autoptr(GError) error = NULL;
	g_assert_true(venture_entity_set_field_from_string(e, name, value, &error));
	g_assert_no_error(error);
}

static VentureEntity *
record(Fixture *f, const gchar *name)
{
	VentureEntity *e = venture_entity_registry_create(venture_entity_registry_get_default(), name, NULL);
	g_assert_nonnull(e);
	venture_entity_set_organization_id(e, f->org);
	return e;
}

static void
setup(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) vendor = NULL;
	f->config = venture_config_new();
	f->db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->db);
	f->org = venture_context_get_default_organization_id(f->context);
	vendor = record(f, "company");
	field(vendor, "name", "Supplier");
	field(vendor, "kind", "supplier");
	save(f, vendor);
	f->vendor = venture_entity_get_id(vendor);
}

static void
teardown(Fixture *f, gconstpointer unused)
{
	g_clear_object(&f->context);
	g_clear_object(&f->db);
	g_clear_object(&f->config);
}

static VentureEntity *
bill(Fixture *f, const gchar *number)
{
	VentureEntity *e = record(f, "vendor_bill");
	g_autoptr(VentureEntity) line = NULL;
	g_object_set(e, "number", number, "company-id", f->vendor,
		"currency", "USD", "status", "draft", NULL);
	field(e, "bill-date", "2026-01-01");
	field(e, "due-date", "2026-01-31");
	save(f, e);
	line = record(f, "vendor_bill_line");
	g_object_set(line, "bill-id", venture_entity_get_id(e),
		"description", "Supplies", "quantity", "2.5", "category", "supplies", NULL);
	field(line, "unit-price", "36 USD");
	field(line, "tax-amount", "10 USD");
	save(f, line);
	return e;
}

static VentureEntity *
event(Fixture *f, VentureEntity *b, const gchar *kind, const gchar *date)
{
	VentureEntity *e = record(f, "vendor_bill_event");
	g_object_set(e, "bill-id", venture_entity_get_id(b), "vendor-id", f->vendor,
		"kind", kind, "state", "approved", NULL);
	field(e, "date", date);
	return e;
}

static void
approve(Fixture *f, VentureEntity *b)
{
	g_autoptr(VentureEntity) e = event(f, b, "approve", "2026-01-01");
	save(f, e);
}

static void
status(Fixture *f, VentureEntity *b, const gchar *expected)
{
	g_autoptr(VentureEntity) stored = venture_database_get(f->db, G_OBJECT_TYPE(b), venture_entity_get_id(b), NULL);
	g_autofree gchar *actual = NULL;
	g_assert_nonnull(stored);
	g_object_get(stored, "status", &actual, NULL);
	g_assert_cmpstr(actual, ==, expected);
}

static gint64
count(Fixture *f, const gchar *name)
{
	g_autoptr(VentureQuery) q = venture_query_new(venture_entity_registry_lookup(venture_entity_registry_get_default(), name));
	venture_query_set_organization(q, f->org);
	return venture_database_count(f->db, q, NULL);
}

static VentureEntity *
payment(Fixture *f, VentureEntity *b, const gchar *amount, const gchar *date)
{
	VentureEntity *e = record(f, "bill_payment");
	g_object_set(e, "vendor-id", f->vendor, "bill-id", venture_entity_get_id(b), "method", "transfer", NULL);
	field(e, "amount", amount);
	field(e, "date", date);
	return e;
}

static void
test_state_guard(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) b = bill(f, "GUARD");
	g_autoptr(GError) error = NULL;
	g_object_set(b, "status", "paid", NULL);
	g_assert_false(venture_database_save(f->db, b, NULL, &error));
	g_assert_nonnull(error);
	g_assert_nonnull(strstr(error->message, "VenturePayablesService"));
	status(f, b, "draft");
}

static void
test_approval(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) b = bill(f, "APPROVE");
	g_autoptr(VentureEntity) stored = NULL;
	g_autoptr(VentureMoney) amount = NULL;
	g_autoptr(VentureQuery) q = NULL;
	g_autoptr(GError) error = NULL;
	approve(f, b);
	status(f, b, "approved");
	g_assert_cmpint(count(f, "journal"), ==, 1);
	q = venture_query_new(VENTURE_TYPE_VENDOR_BILL_EVENT);
	stored = venture_database_find_one(f->db, q, &error);
	g_assert_no_error(error);
	g_object_get(stored, "amount", &amount, NULL);
	g_assert_nonnull(amount);
	g_assert_cmpint(venture_money_get_amount(amount), ==, 10000);
	g_clear_object(&stored);
	stored = venture_database_get(f->db, G_OBJECT_TYPE(b), venture_entity_get_id(b), NULL);
	field(stored, "due-date", "2026-02-28");
	g_assert_false(venture_database_save(f->db, stored, NULL, &error));
	g_assert_nonnull(error);
}

static void
test_partial_payment(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) b = bill(f, "PARTIAL");
	g_autoptr(VentureEntity) p = NULL;
	approve(f, b);
	p = payment(f, b, "40 USD", "2026-01-15");
	save(f, p);
	status(f, b, "partially_paid");
	g_assert_cmpint(count(f, "bill_payment_allocation"), ==, 1);
	g_clear_object(&p);
	p = payment(f, b, "70 USD", "2026-02-15");
	save(f, p);
	status(f, b, "paid");
	g_assert_cmpint(count(f, "bill_payment_allocation"), ==, 2);
}

static void
test_currency(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) b = bill(f, "CURRENCY");
	g_autoptr(VentureEntity) p = NULL;
	g_autoptr(GError) error = NULL;
	approve(f, b);
	p = payment(f, b, "40 EUR", "2026-01-15");
	g_assert_false(venture_database_save(f->db, p, NULL, &error));
	g_assert_nonnull(error);
	g_assert_cmpint(count(f, "bill_payment"), ==, 0);
	status(f, b, "approved");
}

static gboolean
reject_credit(VentureDatabase *db, VentureEntity *e, VentureEntity *old, gpointer data, GError **error)
{
	Fixture *f = data;
	/* The first payment write really happened inside the transaction. */
	g_assert_cmpint(count(f, "bill_payment"), ==, 1);
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_DATABASE, "Injected after the payment write");
	return FALSE;
}

static void
test_atomic(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) b = bill(f, "ATOMIC");
	g_autoptr(VentureEntity) p = NULL;
	g_autoptr(GError) error = NULL;
	gint64 audits;
	gint64 journals;
	approve(f, b);
	audits = count(f, "audit_entry");
	journals = count(f, "journal");
	p = payment(f, b, "100 USD", "2026-02-01");
	venture_database_add_save_validator(f->db, VENTURE_TYPE_VENDOR_CREDIT, reject_credit, f, NULL);
	g_assert_false(venture_database_save(f->db, p, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_DATABASE);
	g_assert_false(venture_entity_is_persisted(p));
	g_assert_cmpint(count(f, "bill_payment"), ==, 0);
	g_assert_cmpint(count(f, "vendor_credit"), ==, 0);
	g_assert_cmpint(count(f, "bill_payment_allocation"), ==, 0);
	g_assert_cmpint(count(f, "journal"), ==, journals);
	g_assert_cmpint(count(f, "audit_entry"), ==, audits);
	status(f, b, "approved");
}

static gint64
metric(VentureReportResult *result, const gchar *key)
{
	GPtrArray *metrics = venture_report_result_get_metrics(result);
	guint i;
	for (i = 0; i < metrics->len; i++)
	{
		VentureMetric *m = g_ptr_array_index(metrics, i);
		if (g_strcmp0(key, venture_metric_get_key(m)) == 0)
			return venture_money_get_amount(venture_metric_get_money(m));
	}
	g_assert_not_reached();
}

static void
test_reports(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) b = bill(f, "REPORT");
	g_autoptr(VentureEntity) p = NULL;
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(GTimeZone) tz = g_time_zone_new_utc();
	g_autoptr(VentureDateRange) period = venture_date_range_parse("2026-01", tz, 1, NULL);
	g_autoptr(JsonObject) options = json_object_new();
	g_autoptr(GError) error = NULL;
	VentureReport *report;
	approve(f, b);
	p = payment(f, b, "40 USD", "2026-01-15");
	save(f, p);
	g_clear_object(&p);
	p = payment(f, b, "60 USD", "2026-02-15");
	save(f, p);
	report = venture_report_registry_lookup(venture_context_get_report_registry(f->context), "payables");
	g_assert_nonnull(report);
	result = venture_report_generate(report, f->context, period, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	g_assert_cmpint(metric(result, "outstanding"), ==, 6000);
	g_clear_object(&result);
	report = venture_report_registry_lookup(venture_context_get_report_registry(f->context), "vendor_statement");
	g_assert_nonnull(report);
	json_object_set_int_member(options, "vendor_id", f->vendor);
	result = venture_report_generate(report, f->context, period, options, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	g_assert_cmpint(metric(result, "balance"), ==, 6000);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	venture_entity_registry_register_builtins(venture_entity_registry_get_default());
	g_test_add_func("/payables/records", test_records);
	g_test_add("/payables/state-guard", Fixture, NULL, setup, test_state_guard, teardown);
	g_test_add("/payables/approval", Fixture, NULL, setup, test_approval, teardown);
	g_test_add("/payables/partial-payment", Fixture, NULL, setup, test_partial_payment, teardown);
	g_test_add("/payables/currency", Fixture, NULL, setup, test_currency, teardown);
	g_test_add("/payables/atomic", Fixture, NULL, setup, test_atomic, teardown);
	g_test_add("/payables/reports", Fixture, NULL, setup, test_reports, teardown);
	return g_test_run();
}
