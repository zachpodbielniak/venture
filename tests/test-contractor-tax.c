/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <string.h>
#include "venture-test-util.h"

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
	g_assert_true(venture_database_save(f->db, e, NULL, &error));
	g_assert_no_error(error);
}

static void
field(VentureEntity *e, const gchar *name, const gchar *value)
{
	g_autoptr(GError) error = NULL;
	g_assert_true(venture_entity_set_field_from_string(e, name, value, &error));
	g_assert_no_error(error);
}

static VentureActor
actor_named(const gchar *name)
{
	VentureActor actor;
	actor.kind = VENTURE_ACTOR_KIND_USER;
	actor.name = name;
	actor.prompt = NULL;
	actor.request_id = NULL;
	actor.approved_by = NULL;
	return actor;
}

static void
setup(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) vendor = NULL;
	(void)unused;
	f->config = venture_config_new();
	f->db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->db);
	f->org = venture_context_get_default_organization_id(f->context);
	vendor = g_object_new(VENTURE_TYPE_COMPANY, "organization-id", f->org,
		"name", "Contractor Co", "kind", VENTURE_COMPANY_KIND_SUPPLIER, NULL);
	save(f, vendor);
	f->vendor = venture_entity_get_id(vendor);
}

static void
teardown(Fixture *f, gconstpointer unused)
{
	(void)unused;
	g_clear_object(&f->context);
	g_clear_object(&f->db);
	g_clear_object(&f->config);
}

static void
test_records(void)
{
	VentureEntity *prototype;
	g_autoptr(GPtrArray) specs = NULL;
	guint i;
	gboolean tin_sensitive = FALSE;
	g_assert_cmpuint(venture_entity_registry_lookup(
		venture_entity_registry_get_default(), "contractor_tax_form"), !=, G_TYPE_INVALID);
	g_assert_cmpuint(venture_entity_registry_lookup(
		venture_entity_registry_get_default(), "contractor_tax_pack"), !=, G_TYPE_INVALID);
	prototype = venture_entity_registry_get_prototype(venture_entity_registry_get_default(),
		"contractor_tax_form");
	specs = venture_entity_get_field_specs(prototype);
	for (i = 0; i < specs->len; i++)
	{
		VentureFieldSpec *spec = g_ptr_array_index(specs, i);
		if (g_strcmp0(venture_field_spec_get_name(spec), "tin") != 0)
			continue;
		g_assert_cmpuint(venture_field_spec_get_flags(spec) & VENTURE_COLUMN_FLAG_SENSITIVE, !=, 0);
		tin_sensitive = TRUE;
	}
	g_assert_true(tin_sensitive);
}

static VentureEntity *
form(Fixture *f)
{
	VentureEntity *record = g_object_new(VENTURE_TYPE_CONTRACTOR_TAX_FORM,
		"organization-id", f->org, "vendor-id", f->vendor,
		"tin", "99-9999999", "tin-type", "EIN", "legal-name", "Contractor Co",
		"active", TRUE, NULL);
	save(f, record);
	return record;
}

static void
pay_bill(Fixture *f, const gchar *number, const gchar *amount, const gchar *date)
{
	g_autoptr(VentureEntity) bill = g_object_new(VENTURE_TYPE_VENDOR_BILL,
		"organization-id", f->org, "number", number, "company-id", f->vendor,
		"currency", "USD", "status", "draft", NULL);
	g_autoptr(VentureEntity) line = NULL;
	g_autoptr(VentureEntity) event = NULL;
	g_autoptr(VentureEntity) payment = NULL;
	field(bill, "bill-date", date);
	save(f, bill);
	line = g_object_new(VENTURE_TYPE_VENDOR_BILL_LINE, "organization-id", f->org,
		"bill-id", venture_entity_get_id(bill), "description", "Work",
		"quantity", "1", "category", "services", NULL);
	field(line, "unit-price", amount);
	save(f, line);
	event = g_object_new(VENTURE_TYPE_VENDOR_BILL_EVENT, "organization-id", f->org,
		"bill-id", venture_entity_get_id(bill), "vendor-id", f->vendor,
		"kind", "approve", "state", "approved", NULL);
	field(event, "date", date);
	save(f, event);
	payment = g_object_new(VENTURE_TYPE_BILL_PAYMENT, "organization-id", f->org,
		"vendor-id", f->vendor, "bill-id", venture_entity_get_id(bill),
		"method", "transfer", NULL);
	field(payment, "amount", amount);
	field(payment, "date", date);
	save(f, payment);
}

static void
test_tin_never_in_generic_export(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) record = form(f);
	g_autofree gchar *json = NULL;
	g_autoptr(GPtrArray) specs = NULL;
	guint i;
	(void)unused;
	json = venture_serializable_to_json_string(VENTURE_SERIALIZABLE(record), FALSE, FALSE);
	g_assert_null(strstr(json, "99-9999999"));
	specs = venture_entity_get_field_specs(record);
	for (i = 0; i < specs->len; i++)
	{
		VentureFieldSpec *spec = g_ptr_array_index(specs, i);
		if (g_strcmp0(venture_field_spec_get_name(spec), "tin") != 0)
			continue;
		g_assert_cmpuint(venture_field_spec_get_flags(spec) & VENTURE_COLUMN_FLAG_SENSITIVE, !=, 0);
	}
}

static void
test_prepare_threshold(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) pack = NULL;
	g_autoptr(VentureEntity) stored_form = form(f);
	VentureActor actor = actor_named("clerk");
	(void)unused;
	(void)stored_form;
	pay_bill(f, "LOW", "500 USD", "2026-03-01");
	pack = venture_tax_filing_service_prepare_1099(venture_tax_filing_service_get(f->db),
		f->org, f->vendor, 2026, &actor, &error);
	g_assert_null(pack);
	g_assert_nonnull(error);
	g_assert_nonnull(strstr(error->message, "600"));
}

static void
test_prepare_review_approve_export(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) stored_form = form(f);
	g_autoptr(VentureEntity) pack = NULL;
	g_autofree gchar *first = NULL;
	g_autofree gchar *second = NULL;
	g_autofree gchar *status = NULL;
	VentureActor actor = actor_named("clerk");
	(void)unused;
	(void)stored_form;
	pay_bill(f, "NEC-1", "700 USD", "2026-04-01");
	pack = venture_tax_filing_service_prepare_1099(venture_tax_filing_service_get(f->db),
		f->org, f->vendor, 2026, &actor, &error);
	g_assert_no_error(error);
	g_assert_nonnull(pack);
	first = venture_tax_filing_service_export_1099(venture_tax_filing_service_get(f->db),
		pack, &actor, &error);
	g_assert_null(first);
	g_assert_nonnull(error);
	g_clear_error(&error);
	g_assert_true(venture_tax_filing_service_review_1099(venture_tax_filing_service_get(f->db),
		pack, &actor, &error));
	g_assert_true(venture_tax_filing_service_approve_1099(venture_tax_filing_service_get(f->db),
		pack, &actor, &error));
	first = venture_tax_filing_service_export_1099(venture_tax_filing_service_get(f->db),
		pack, &actor, &error);
	g_assert_no_error(error);
	g_assert_nonnull(first);
	g_assert_nonnull(strstr(first, "1099-NEC"));
	g_assert_nonnull(strstr(first, "700"));
	g_assert_nonnull(strstr(first, "99-9999999"));
	second = venture_tax_filing_service_export_1099(venture_tax_filing_service_get(f->db),
		pack, &actor, &error);
	g_assert_cmpstr(second, ==, first);
	g_object_get(pack, "status", &status, NULL);
	g_assert_cmpstr(status, ==, "exported");
	{
		g_autoptr(VentureEntity) again = venture_tax_filing_service_prepare_1099(
			venture_tax_filing_service_get(f->db), f->org, f->vendor, 2026, &actor, &error);
		g_assert_no_error(error);
		g_assert_cmpint(venture_entity_get_id(again), ==, venture_entity_get_id(pack));
	}
}

static void
test_requires_form(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) pack = NULL;
	VentureActor actor = actor_named("clerk");
	(void)unused;
	pay_bill(f, "NOFORM", "700 USD", "2026-04-01");
	pack = venture_tax_filing_service_prepare_1099(venture_tax_filing_service_get(f->db),
		f->org, f->vendor, 2026, &actor, &error);
	g_assert_null(pack);
	g_assert_nonnull(error);
	g_assert_null(strstr(error->message, "99-9999999"));
}

static void
test_generic_pack_status_refused(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) stored_form = form(f);
	g_autoptr(VentureEntity) pack = NULL;
	VentureActor actor = actor_named("clerk");
	(void)unused;
	(void)stored_form;
	pay_bill(f, "GEN", "700 USD", "2026-04-01");
	pack = venture_tax_filing_service_prepare_1099(venture_tax_filing_service_get(f->db),
		f->org, f->vendor, 2026, &actor, &error);
	g_object_set(pack, "status", "exported", NULL);
	g_assert_false(venture_database_save(f->db, pack, &actor, &error));
	g_assert_nonnull(strstr(error->message, "VentureTaxFilingService"));
}

static void
test_other_year_ignored(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) stored_form = form(f);
	g_autoptr(VentureEntity) pack = NULL;
	VentureActor actor = actor_named("clerk");
	(void)unused;
	(void)stored_form;
	pay_bill(f, "OLD", "700 USD", "2025-12-31");
	pack = venture_tax_filing_service_prepare_1099(venture_tax_filing_service_get(f->db),
		f->org, f->vendor, 2026, &actor, &error);
	g_assert_null(pack);
	g_assert_nonnull(error);
}

gint
main(gint argc, gchar **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/contractor-tax/records", test_records);
	g_test_add("/contractor-tax/tin-redacted", Fixture, NULL, setup,
		test_tin_never_in_generic_export, teardown);
	g_test_add("/contractor-tax/threshold", Fixture, NULL, setup, test_prepare_threshold, teardown);
	g_test_add("/contractor-tax/export", Fixture, NULL, setup,
		test_prepare_review_approve_export, teardown);
	g_test_add("/contractor-tax/requires-form", Fixture, NULL, setup, test_requires_form, teardown);
	g_test_add("/contractor-tax/generic-refused", Fixture, NULL, setup,
		test_generic_pack_status_refused, teardown);
	g_test_add("/contractor-tax/year", Fixture, NULL, setup, test_other_year_ignored, teardown);
	return g_test_run();
}
