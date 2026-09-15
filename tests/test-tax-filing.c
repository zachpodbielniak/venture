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
	gint64 period;
	gint64 customer;
} Fixture;

static void
save(Fixture *f, VentureEntity *e)
{
	g_autoptr(GError) error = NULL;
	g_assert_true(venture_database_save(f->db, e, NULL, &error));
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
	g_autoptr(VentureEntity) year = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureEntity) period = NULL;
	g_autoptr(VentureEntity) customer = NULL;
	(void)unused;
	f->config = venture_config_new();
	f->db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->db);
	f->org = venture_context_get_default_organization_id(f->context);
	year = g_object_new(VENTURE_TYPE_FISCAL_YEAR, "organization-id", f->org, "name", "2026", NULL);
	g_assert_true(venture_entity_set_field_from_string(year, "start-at", "2026-01-01", &error));
	save(f, year);
	query = venture_query_new(VENTURE_TYPE_FISCAL_PERIOD);
	venture_query_set_organization(query, f->org);
	venture_query_add_order(query, "start-at", VENTURE_SORT_ASCENDING, NULL);
	period = venture_database_find_one(f->db, query, &error);
	g_assert_no_error(error);
	f->period = venture_entity_get_id(period);
	customer = g_object_new(VENTURE_TYPE_COMPANY, "organization-id", f->org, "name", "Customer", NULL);
	save(f, customer);
	f->customer = venture_entity_get_id(customer);
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
	g_assert_cmpuint(venture_entity_registry_lookup(
		venture_entity_registry_get_default(), "tax_filing"), !=, G_TYPE_INVALID);
	g_assert_cmpstr(venture_entity_registry_get_table_name(
		venture_entity_registry_get_default(), "tax_filing"), ==, "tax_filings");
}

static void
issue_taxed_invoice(Fixture *f, VentureEntity *code, const gchar *number, const gchar *price)
{
	g_autoptr(VentureEntity) invoice = g_object_new(VENTURE_TYPE_INVOICE,
		"organization-id", f->org, "number", number, "company-id", f->customer, NULL);
	g_autoptr(VentureEntity) line = NULL;
	g_autoptr(GError) error = NULL;
	g_assert_true(venture_entity_set_field_from_string(invoice, "issued-at", "2026-01-15", &error));
	save(f, invoice);
	line = g_object_new(VENTURE_TYPE_INVOICE_LINE, "organization-id", f->org,
		"invoice-id", venture_entity_get_id(invoice), "description", "Work",
		"quantity", 1.0, "tax-code-id", venture_entity_get_id(code), NULL);
	g_assert_true(venture_entity_set_field_from_string(line, "unit-price", price, &error));
	save(f, line);
	g_object_set(invoice, "status", VENTURE_INVOICE_STATUS_SENT, NULL);
	save(f, invoice);
}

static VentureEntity *
tax_code(Fixture *f, const gchar *code, const gchar *jurisdiction)
{
	VentureEntity *record = g_object_new(VENTURE_TYPE_TAX_CODE, "organization-id", f->org,
		"code", code, "name", code, "jurisdiction", jurisdiction,
		"rate-numerator", (gint64)8875, "rate-denominator", (gint64)100000,
		"recoverable", FALSE, "active", TRUE, NULL);
	save(f, record);
	return record;
}

static void
test_registry_and_fake_adapter(Fixture *f, gconstpointer unused)
{
	VentureTaxFilingService *service = venture_tax_filing_service_get(f->db);
	VentureTaxFilingAdapterRegistry *registry = venture_tax_filing_service_get_adapters(service);
	g_autoptr(GPtrArray) listed = NULL;
	(void)unused;
	g_assert_nonnull(venture_tax_filing_adapter_registry_lookup(registry, "US"));
	g_assert_cmpstr(venture_tax_filing_adapter_get_rule_id(
		venture_tax_filing_adapter_registry_lookup(registry, "US")), ==, "us-sales-tax:1");
	listed = venture_tax_filing_adapter_registry_list(registry);
	g_assert_cmpuint(listed->len, >=, 1);
}

typedef struct
{
	GObject parent_instance;
} FakeAdapter;
typedef struct
{
	GObjectClass parent_class;
} FakeAdapterClass;
GType fake_adapter_get_type(void);
static void fake_iface(VentureTaxFilingAdapterInterface *iface);
G_DEFINE_TYPE_WITH_CODE(FakeAdapter, fake_adapter, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(VENTURE_TYPE_TAX_FILING_ADAPTER, fake_iface))
static void fake_adapter_init(FakeAdapter *self) { (void)self; }
static void fake_adapter_class_init(FakeAdapterClass *klass) { (void)klass; }
static const gchar *fake_name(VentureTaxFilingAdapter *self) { (void)self; return "XX"; }
static const gchar *fake_rule(VentureTaxFilingAdapter *self) { (void)self; return "fake:1"; }
static const gchar *fake_country(VentureTaxFilingAdapter *self) { (void)self; return "XX"; }
static gboolean
fake_prepare(VentureTaxFilingAdapter *self, VentureDatabase *database,
	VentureEntity *filing, GError **error)
{
	(void)self;
	(void)database;
	(void)error;
	g_object_set(filing, "json-pack", "{\"rule_id\":\"fake:1\"}",
		"csv-pack", "code,tax\n", "rule-id", "fake:1", NULL);
	return TRUE;
}
static void
fake_iface(VentureTaxFilingAdapterInterface *iface)
{
	iface->get_name = fake_name;
	iface->get_rule_id = fake_rule;
	iface->get_country = fake_country;
	iface->prepare = fake_prepare;
}

static void
test_fake_adapter_prepare(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) filing = NULL;
	g_autofree gchar *json = NULL;
	g_autofree gchar *status = NULL;
	VentureActor actor = actor_named("clerk");
	FakeAdapter *fake = g_object_new(fake_adapter_get_type(), NULL);
	(void)unused;
	venture_tax_filing_adapter_registry_add(
		venture_tax_filing_service_get_adapters(venture_tax_filing_service_get(f->db)),
		VENTURE_TAX_FILING_ADAPTER(fake));
	filing = venture_tax_filing_service_prepare(venture_tax_filing_service_get(f->db),
		f->org, "XX", "XX", f->period, NULL, NULL, &actor, &error);
	g_assert_no_error(error);
	g_assert_nonnull(filing);
	g_object_get(filing, "json-pack", &json, "status", &status, NULL);
	g_assert_cmpstr(json, ==, "{\"rule_id\":\"fake:1\"}");
	g_assert_cmpstr(status, ==, "draft");
}

static void
test_us_sales_tax_pack(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) code = tax_code(f, "NY-8875", "US-NY");
	g_autoptr(VentureEntity) filing = NULL;
	g_autofree gchar *json = NULL;
	g_autofree gchar *csv = NULL;
	g_autofree gchar *rule = NULL;
	g_autofree gchar *status = NULL;
	VentureActor actor = actor_named("clerk");
	(void)unused;
	issue_taxed_invoice(f, code, "TAX-1", "80 USD");
	filing = venture_tax_filing_service_prepare(venture_tax_filing_service_get(f->db),
		f->org, "US", "US-NY", f->period, NULL, NULL, &actor, &error);
	g_assert_no_error(error);
	g_assert_nonnull(filing);
	g_object_get(filing, "json-pack", &json, "csv-pack", &csv, "rule-id", &rule,
		"status", &status, NULL);
	g_assert_cmpstr(status, ==, "draft");
	g_assert_cmpstr(rule, ==, "us-sales-tax:1");
	g_assert_nonnull(strstr(json, "us-sales-tax:1"));
	g_assert_nonnull(strstr(json, "NY-8875"));
	g_assert_nonnull(strstr(csv, "NY-8875"));
	g_assert_null(strstr(json, "production"));
}

static void
test_missing_country_refuses_named_jurisdiction(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) filing = NULL;
	VentureActor actor = actor_named("clerk");
	(void)unused;
	filing = venture_tax_filing_service_prepare(venture_tax_filing_service_get(f->db),
		f->org, NULL, "US-NY", f->period, NULL, NULL, &actor, &error);
	g_assert_null(filing);
	g_assert_nonnull(error);
	g_assert_nonnull(strstr(error->message, "US-NY"));
	g_assert_nonnull(strstr(error->message, "country"));
}

static void
test_unknown_country_refuses_named_jurisdiction(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) filing = NULL;
	VentureActor actor = actor_named("clerk");
	(void)unused;
	filing = venture_tax_filing_service_prepare(venture_tax_filing_service_get(f->db),
		f->org, "DE", "DE-BE", f->period, NULL, NULL, &actor, &error);
	g_assert_null(filing);
	g_assert_nonnull(error);
	g_assert_nonnull(strstr(error->message, "DE"));
}

static void
test_tax_code_without_country_refuses(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) code = tax_code(f, "BARE", "");
	g_autoptr(VentureEntity) filing = NULL;
	VentureActor actor = actor_named("clerk");
	(void)unused;
	issue_taxed_invoice(f, code, "BARE-1", "80 USD");
	filing = venture_tax_filing_service_prepare(venture_tax_filing_service_get(f->db),
		f->org, "US", "US", f->period, NULL, NULL, &actor, &error);
	g_assert_null(filing);
	g_assert_nonnull(error);
	g_assert_nonnull(strstr(error->message, "BARE"));
}

static void
test_states_and_preserved_bytes(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) code = tax_code(f, "NY-8875", "US-NY");
	g_autoptr(VentureEntity) filing = NULL;
	g_autoptr(VentureEntity) amendment = NULL;
	g_autofree gchar *json = NULL;
	g_autofree gchar *csv = NULL;
	g_autofree gchar *after_json = NULL;
	g_autofree gchar *ack = NULL;
	g_autofree gchar *prior_status = NULL;
	g_autofree gchar *new_status = NULL;
	gint64 prior_id;
	VentureActor actor = actor_named("clerk");
	(void)unused;
	issue_taxed_invoice(f, code, "ST-1", "80 USD");
	filing = venture_tax_filing_service_prepare(venture_tax_filing_service_get(f->db),
		f->org, "US", "US-NY", f->period, NULL, NULL, &actor, &error);
	g_assert_true(venture_tax_filing_service_review(venture_tax_filing_service_get(f->db),
		filing, &actor, &error));
	g_assert_no_error(error);
	g_object_get(filing, "json-pack", &json, "csv-pack", &csv, NULL);
	g_assert_true(venture_tax_filing_service_submit(venture_tax_filing_service_get(f->db),
		filing, &actor, &error));
	g_assert_no_error(error);
	g_object_get(filing, "json-pack", &after_json, NULL);
	g_assert_cmpstr(after_json, ==, json);
	g_assert_true(venture_tax_filing_service_acknowledge(venture_tax_filing_service_get(f->db),
		filing, "ACK-9", &actor, &error));
	g_assert_no_error(error);
	g_object_get(filing, "acknowledgment-id", &ack, NULL);
	g_assert_cmpstr(ack, ==, "ACK-9");
	prior_id = venture_entity_get_id(filing);
	amendment = venture_tax_filing_service_amend(venture_tax_filing_service_get(f->db),
		filing, &actor, &error);
	g_assert_no_error(error);
	g_assert_nonnull(amendment);
	g_assert_cmpint(venture_entity_get_id(amendment), !=, prior_id);
	{
		g_autoptr(VentureEntity) stored = venture_database_get(f->db,
			VENTURE_TYPE_TAX_FILING, prior_id, NULL);
		g_object_get(stored, "status", &prior_status, NULL);
		g_assert_cmpstr(prior_status, ==, "acknowledged");
	}
	g_object_get(amendment, "status", &new_status, NULL);
	g_assert_cmpstr(new_status, ==, "draft");
}

static void
test_generic_status_refused(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) filing = NULL;
	VentureActor actor = actor_named("clerk");
	(void)unused;
	filing = venture_tax_filing_service_prepare(venture_tax_filing_service_get(f->db),
		f->org, "US", "US", f->period, NULL, NULL, &actor, &error);
	g_object_set(filing, "status", "submitted", NULL);
	g_assert_false(venture_database_save(f->db, filing, &actor, &error));
	g_assert_nonnull(error);
	g_assert_nonnull(strstr(error->message, "VentureTaxFilingService"));
}

static void
test_actions_review_submit(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) filing = NULL;
	g_autoptr(VentureEntity) reviewed = NULL;
	VentureActionRegistry *actions;
	VentureActor actor = actor_named("clerk");
	(void)unused;
	filing = venture_tax_filing_service_prepare(venture_tax_filing_service_get(f->db),
		f->org, "US", "US", f->period, NULL, NULL, &actor, &error);
	actions = venture_database_get_action_registry(f->db);
	{
		g_autoptr(GHashTable) params = g_hash_table_new(g_str_hash, g_str_equal);
		reviewed = venture_action_registry_perform(actions, "tax_filing",
			venture_entity_get_id(filing), "review", params, &actor,
			VENTURE_USER_ROLE_EDITOR, &error);
	}
	g_assert_no_error(error);
	g_assert_nonnull(reviewed);
}

static void
test_module_off_keeps_tax_code(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) code = NULL;
	g_autoptr(VentureEntity) filing = NULL;
	VentureActor actor = actor_named("clerk");
	(void)unused;
	venture_config_set_module_enabled(f->config, "tax_filing", FALSE);
	g_assert_cmpuint(venture_entity_registry_lookup(
		venture_entity_registry_get_default(), "tax_filing"), ==, G_TYPE_INVALID);
	g_assert_cmpuint(venture_entity_registry_lookup(
		venture_entity_registry_get_default(), "tax_code"), !=, G_TYPE_INVALID);
	code = tax_code(f, "STILL", "US-NY");
	g_assert_cmpint(venture_entity_get_id(code), >, 0);
	filing = venture_tax_filing_service_prepare(venture_tax_filing_service_get(f->db),
		f->org, "US", "US-NY", f->period, NULL, NULL, &actor, &error);
	g_assert_null(filing);
	g_assert_nonnull(error);
	venture_config_set_module_enabled(f->config, "tax_filing", TRUE);
}

gint
main(gint argc, gchar **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/tax-filing/records", test_records);
	g_test_add("/tax-filing/registry", Fixture, NULL, setup, test_registry_and_fake_adapter, teardown);
	g_test_add("/tax-filing/fake-adapter", Fixture, NULL, setup, test_fake_adapter_prepare, teardown);
	g_test_add("/tax-filing/us-pack", Fixture, NULL, setup, test_us_sales_tax_pack, teardown);
	g_test_add("/tax-filing/missing-country", Fixture, NULL, setup,
		test_missing_country_refuses_named_jurisdiction, teardown);
	g_test_add("/tax-filing/unknown-country", Fixture, NULL, setup,
		test_unknown_country_refuses_named_jurisdiction, teardown);
	g_test_add("/tax-filing/bare-jurisdiction", Fixture, NULL, setup,
		test_tax_code_without_country_refuses, teardown);
	g_test_add("/tax-filing/states", Fixture, NULL, setup, test_states_and_preserved_bytes, teardown);
	g_test_add("/tax-filing/generic-refused", Fixture, NULL, setup, test_generic_status_refused, teardown);
	g_test_add("/tax-filing/actions", Fixture, NULL, setup, test_actions_review_submit, teardown);
	g_test_add("/tax-filing/module-off", Fixture, NULL, setup, test_module_off_keeps_tax_code, teardown);
	return g_test_run();
}
