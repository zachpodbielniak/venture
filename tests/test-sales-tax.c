/*
 * test-sales-tax.c - Jurisdiction rates picked by address, frozen at issue,
 * and a return a CPA can file from.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include <venture.h>
#include <string.h>
#include <libsoup/soup.h>
#include "venture-test-util.h"

typedef struct
{
	VentureDatabase *db;
	VentureContext *context;
	VentureConfig *config;
	gint64 org;
	gint64 customer;
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

static VentureEntity *
record(Fixture *f, const gchar *name)
{
	GType type = venture_entity_registry_lookup(venture_entity_registry_get_default(), name);
	VentureEntity *e;
	g_assert_cmpuint(type, !=, G_TYPE_INVALID);
	e = g_object_new(type, NULL);
	venture_entity_set_organization_id(e, f->org);
	return e;
}

static GPtrArray *
rows(Fixture *f, const gchar *name)
{
	g_autoptr(VentureQuery) query = venture_query_new(
		venture_entity_registry_lookup(venture_entity_registry_get_default(), name));
	g_autoptr(GError) error = NULL;
	GPtrArray *found;
	venture_query_set_limit(query, 0);
	venture_query_set_include_deleted(query, TRUE);
	g_assert_true(venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, &error));
	found = venture_database_find(f->db, query, &error);
	g_assert_no_error(error);
	g_assert_nonnull(found);
	return found;
}

static gint64
money_of(VentureEntity *e, const gchar *name)
{
	g_autoptr(VentureMoney) m = NULL;
	g_object_get(e, name, &m, NULL);
	g_assert_nonnull(m);
	return venture_money_get_amount(m);
}

static gint64
int_of(VentureEntity *e, const gchar *name)
{
	gint64 v = 0;
	g_object_get(e, name, &v, NULL);
	return v;
}

static gint64
customer_at(Fixture *f, const gchar *name, const gchar *state, const gchar *county, const gchar *city)
{
	g_autoptr(VentureEntity) company = record(f, "company");
	g_object_set(company, "name", name, "address-state", state,
		"address-county", county, "address-city", city, NULL);
	save(f, company);
	return venture_entity_get_id(company);
}

static void
setup(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	(void)unused;
	f->config = venture_config_new();
	f->db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->db);
	f->org = venture_context_get_default_organization_id(f->context);
	f->customer = customer_at(f, "Downtown Books", "NY", "NEW YORK", "NEW YORK");
}

static void
teardown(Fixture *f, gconstpointer unused)
{
	(void)unused;
	g_clear_object(&f->context);
	g_clear_object(&f->db);
	g_clear_object(&f->config);
}

/* rate is percent times 10000: 88750 is 8.875%. */
static gint64
jurisdiction(Fixture *f, const gchar *code, const gchar *name, gint64 rate,
	const gchar *from, const gchar *to)
{
	g_autoptr(VentureEntity) j = record(f, "tax_jurisdiction");
	g_object_set(j, "code", code, "name", name, "kind", "sales", "rate-scaled", rate, NULL);
	field(j, "effective-from", from);
	if (to != NULL)
		field(j, "effective-to", to);
	save(f, j);
	return venture_entity_get_id(j);
}

static gint64
rule(Fixture *f, gint64 jurisdiction_id, const gchar *state, const gchar *county, const gchar *city)
{
	g_autoptr(VentureEntity) r = record(f, "tax_rule");
	g_object_set(r, "jurisdiction-id", jurisdiction_id, "state", state,
		"county", county, "city", city, NULL);
	save(f, r);
	return venture_entity_get_id(r);
}

static gint64
nyc(Fixture *f)
{
	gint64 j = jurisdiction(f, "US-NY-NYC", "New York City", 88750, "2026-01-01", NULL);
	rule(f, j, "NY", "NEW YORK", "NEW YORK");
	return j;
}

static VentureEntity *
draft(Fixture *f, const gchar *number, gint64 customer, const gchar *issued)
{
	VentureEntity *invoice = record(f, "invoice");
	g_object_set(invoice, "number", number, "company-id", customer, NULL);
	field(invoice, "issued-at", issued);
	field(invoice, "due-at", issued);
	save(f, invoice);
	return invoice;
}

static gint64
line(Fixture *f, VentureEntity *invoice, const gchar *price, gint64 product, gint64 tax_percent)
{
	g_autoptr(VentureEntity) l = record(f, "invoice_line");
	g_object_set(l, "invoice-id", venture_entity_get_id(invoice), "description", "Work",
		"quantity", 1.0, "product-id", product, "tax-percent", tax_percent, NULL);
	field(l, "unit-price", price);
	save(f, l);
	return venture_entity_get_id(l);
}

static void
issue(Fixture *f, VentureEntity *invoice, const gchar *date)
{
	g_autoptr(GDateTime) at = venture_time_from_string(date, NULL);
	g_autoptr(GError) error = NULL;
	g_assert_nonnull(at);
	g_assert_true(venture_settlement_service_transition(venture_settlement_service_get(f->db),
		VENTURE_INVOICE(invoice), "sent", at, NULL, &error));
	g_assert_no_error(error);
}

static VentureEntity *
line_by_id(Fixture *f, gint64 id)
{
	g_autoptr(GError) error = NULL;
	VentureEntity *l = venture_database_get(f->db, VENTURE_TYPE_INVOICE_LINE, id, &error);
	g_assert_no_error(error);
	g_assert_nonnull(l);
	return l;
}

static gboolean
has_field(const gchar *type, const gchar *name)
{
	VentureEntity *prototype = venture_entity_registry_get_prototype(
		venture_entity_registry_get_default(), type);
	g_autoptr(GPtrArray) specs = venture_entity_get_field_specs(prototype);
	guint i;
	for (i = 0; i < specs->len; i++)
		if (g_strcmp0(venture_field_spec_get_name(g_ptr_array_index(specs, i)), name) == 0)
			return TRUE;
	return FALSE;
}

/* The records and the flags come from field tables, nowhere else. */
static void
test_records(void)
{
	g_assert_cmpuint(venture_entity_registry_lookup(venture_entity_registry_get_default(),
		"tax_jurisdiction"), !=, G_TYPE_INVALID);
	g_assert_cmpuint(venture_entity_registry_lookup(venture_entity_registry_get_default(),
		"tax_rule"), !=, G_TYPE_INVALID);
	g_assert_true(has_field("tax_jurisdiction", "rate-scaled"));
	g_assert_true(has_field("tax_jurisdiction", "effective-from"));
	g_assert_true(has_field("tax_jurisdiction", "effective-to"));
	g_assert_true(has_field("tax_jurisdiction", "kind"));
	g_assert_true(has_field("tax_rule", "jurisdiction-id"));
	g_assert_true(has_field("tax_rule", "state"));
	g_assert_true(has_field("company", "tax-exempt"));
	g_assert_true(has_field("company", "tax-exemption-number"));
	g_assert_true(has_field("company", "address-state"));
	g_assert_true(has_field("product", "tax-exempt"));
	g_assert_true(has_field("invoice_line", "tax-jurisdiction-id"));
	g_assert_true(has_field("invoice_line", "tax-rate-scaled"));
	g_assert_true(has_field("invoice_line", "tax-exempt"));
	g_assert_true(has_field("customer_credit", "tax-jurisdiction-id"));
}

/* Two rate rows for one code may touch but never overlap; a bad row is refused. */
static void
test_overlap_refused(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) bad = NULL;
	(void)unused;
	jurisdiction(f, "US-NY", "New York State", 40000, "2026-01-01", "2026-07-01");
	/* Adjacent: the exclusive end of one is the start of the next. */
	jurisdiction(f, "US-NY", "New York State", 45000, "2026-07-01", NULL);

	bad = record(f, "tax_jurisdiction");
	g_object_set(bad, "code", "US-NY", "name", "Overlap", "kind", "sales", "rate-scaled", (gint64)50000, NULL);
	field(bad, "effective-from", "2026-03-01");
	field(bad, "effective-to", "2026-04-01");
	g_assert_false(venture_database_save(f->db, bad, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
	g_assert_nonnull(strstr(error->message, "US-NY"));
	g_clear_error(&error);

	/* An open-ended row overlaps everything after it. */
	field(bad, "effective-from", "2026-12-01");
	g_object_set(bad, "effective-to", NULL, NULL);
	g_assert_false(venture_database_save(f->db, bad, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
	g_clear_error(&error);

	/* Another code is a different jurisdiction and may overlap freely. */
	jurisdiction(f, "US-NJ", "New Jersey", 66250, "2026-01-01", NULL);

	/* Shape rules. */
	g_clear_object(&bad);
	bad = record(f, "tax_jurisdiction");
	g_object_set(bad, "code", "US-CT", "name", "Bad", "kind", "sales", "rate-scaled", (gint64)-1, NULL);
	field(bad, "effective-from", "2026-01-01");
	g_assert_false(venture_database_save(f->db, bad, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	g_object_set(bad, "rate-scaled", (gint64)63500, "kind", "vat", NULL);
	g_assert_false(venture_database_save(f->db, bad, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	g_object_set(bad, "kind", "use", NULL);
	field(bad, "effective-to", "2025-12-31");
	g_assert_false(venture_database_save(f->db, bad, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	g_object_set(bad, "effective-to", NULL, "effective-from", NULL, NULL);
	g_assert_false(venture_database_save(f->db, bad, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);

	/* A rule needs a state code and a jurisdiction of this organization. */
	g_clear_object(&bad);
	bad = record(f, "tax_rule");
	g_object_set(bad, "jurisdiction-id", (gint64)0, "state", "NY", NULL);
	g_assert_false(venture_database_save(f->db, bad, NULL, &error));
	g_assert_nonnull(error);
	g_clear_error(&error);
	g_object_set(bad, "jurisdiction-id", (gint64)999999, NULL);
	g_assert_false(venture_database_save(f->db, bad, NULL, &error));
	g_assert_nonnull(error);
	g_clear_error(&error);
	g_object_set(bad, "jurisdiction-id", nyc(f), "state", "", NULL);
	g_assert_false(venture_database_save(f->db, bad, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
}

/* Issuing picks the jurisdiction from the customer's address and freezes
 * jurisdiction, rate and amount on the line; the event carries the total. */
static void
test_issue_applies_jurisdiction(Fixture *f, gconstpointer unused)
{
	gint64 j = nyc(f);
	g_autoptr(VentureEntity) invoice = draft(f, "INV-NYC", f->customer, "2026-02-01");
	gint64 id = line(f, invoice, "80 USD", 0, 0);
	g_autoptr(VentureEntity) frozen = NULL;
	g_autoptr(GPtrArray) events = NULL;
	gboolean exempt = TRUE;
	(void)unused;
	issue(f, invoice, "2026-02-01");
	frozen = line_by_id(f, id);
	g_assert_cmpint(int_of(frozen, "tax-jurisdiction-id"), ==, j);
	g_assert_cmpint(int_of(frozen, "tax-rate-scaled"), ==, 88750);
	g_assert_cmpint(money_of(frozen, "tax-amount"), ==, 710);
	g_assert_cmpint(money_of(frozen, "income-amount"), ==, 8000);
	g_object_get(frozen, "tax-exempt", &exempt, NULL);
	g_assert_false(exempt);
	events = rows(f, "invoice_event");
	g_assert_cmpuint(events->len, ==, 1);
	g_assert_cmpint(money_of(g_ptr_array_index(events, 0), "tax-amount"), ==, 710);
	g_assert_cmpint(money_of(g_ptr_array_index(events, 0), "amount"), ==, 8710);
}

/* No rule for the address: the line's own percent still applies, nothing is frozen. */
static void
test_no_rule_falls_back(Fixture *f, gconstpointer unused)
{
	gint64 elsewhere = customer_at(f, "Oregon Reader", "OR", "", "PORTLAND");
	g_autoptr(VentureEntity) invoice = draft(f, "INV-OR", elsewhere, "2026-02-01");
	gint64 id = line(f, invoice, "80 USD", 0, 5);
	g_autoptr(VentureEntity) frozen = NULL;
	(void)unused;
	nyc(f);
	issue(f, invoice, "2026-02-01");
	frozen = line_by_id(f, id);
	g_assert_cmpint(int_of(frozen, "tax-jurisdiction-id"), ==, 0);
	g_assert_cmpint(int_of(frozen, "tax-rate-scaled"), ==, 0);
	g_assert_cmpint(money_of(frozen, "tax-amount"), ==, 400);
}

/* An explicit tax code on a line is the operator's override; the rule is not consulted. */
static void
test_tax_code_overrides_rule(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) code = record(f, "tax_code");
	g_autoptr(VentureEntity) invoice = draft(f, "INV-CODE", f->customer, "2026-02-01");
	g_autoptr(VentureEntity) l = record(f, "invoice_line");
	g_autoptr(VentureEntity) frozen = NULL;
	(void)unused;
	nyc(f);
	g_object_set(code, "code", "NY-4", "name", "State only", "jurisdiction", "US-NY",
		"rate-numerator", (gint64)4, "rate-denominator", (gint64)100, "active", TRUE, NULL);
	save(f, code);
	g_object_set(l, "invoice-id", venture_entity_get_id(invoice), "description", "Work",
		"quantity", 1.0, "tax-code-id", venture_entity_get_id(code), NULL);
	field(l, "unit-price", "100 USD");
	save(f, l);
	issue(f, invoice, "2026-02-01");
	frozen = line_by_id(f, venture_entity_get_id(l));
	g_assert_cmpint(money_of(frozen, "tax-amount"), ==, 400);
	g_assert_cmpint(int_of(frozen, "tax-jurisdiction-id"), ==, 0);
}

/* An exempt customer freezes the jurisdiction with zero tax; so does an exempt product. */
static void
test_exempt_customer_and_product(Fixture *f, gconstpointer unused)
{
	gint64 j = nyc(f);
	g_autoptr(VentureEntity) company = venture_database_get(f->db, VENTURE_TYPE_COMPANY, f->customer, NULL);
	g_autoptr(VentureEntity) venture = record(f, "venture");
	g_autoptr(VentureEntity) taxable = record(f, "product");
	g_autoptr(VentureEntity) exempt = record(f, "product");
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(VentureEntity) frozen = NULL;
	g_autoptr(VentureEntity) frozen_exempt = NULL;
	g_autoptr(GPtrArray) events = NULL;
	gint64 taxed_id, exempt_id, both_id, unruled_id;
	gint64 nowhere;
	gboolean flag = FALSE;
	(void)unused;
	g_object_set(venture, "name", "Books", NULL);
	save(f, venture);
	g_object_set(taxable, "name", "Paperback", "venture-id", venture_entity_get_id(venture), NULL);
	save(f, taxable);
	g_object_set(exempt, "name", "Consulting", "venture-id", venture_entity_get_id(venture),
		"tax-exempt", TRUE, NULL);
	save(f, exempt);

	invoice = draft(f, "INV-MIX", f->customer, "2026-02-01");
	taxed_id = line(f, invoice, "80 USD", venture_entity_get_id(taxable), 0);
	exempt_id = line(f, invoice, "100 USD", venture_entity_get_id(exempt), 0);
	issue(f, invoice, "2026-02-01");
	frozen = line_by_id(f, taxed_id);
	g_assert_cmpint(money_of(frozen, "tax-amount"), ==, 710);
	g_assert_cmpint(int_of(frozen, "tax-jurisdiction-id"), ==, j);
	frozen_exempt = line_by_id(f, exempt_id);
	g_assert_cmpint(money_of(frozen_exempt, "tax-amount"), ==, 0);
	g_assert_cmpint(int_of(frozen_exempt, "tax-jurisdiction-id"), ==, j);
	g_assert_cmpint(int_of(frozen_exempt, "tax-rate-scaled"), ==, 0);
	g_object_get(frozen_exempt, "tax-exempt", &flag, NULL);
	g_assert_true(flag);
	events = rows(f, "invoice_event");
	g_assert_cmpint(money_of(g_ptr_array_index(events, 0), "tax-amount"), ==, 710);

	/* Exempt customer: taxable product, zero tax, exemption carried to the invoice. */
	g_object_set(company, "tax-exempt", TRUE, "tax-exempt-reason", "501(c)(3)",
		"tax-exemption-number", "EX-123456", NULL);
	save(f, company);
	g_clear_object(&invoice);
	invoice = draft(f, "INV-EXEMPT", f->customer, "2026-02-02");
	both_id = line(f, invoice, "80 USD", venture_entity_get_id(taxable), 0);
	issue(f, invoice, "2026-02-02");
	g_clear_object(&frozen);
	frozen = line_by_id(f, both_id);
	g_assert_cmpint(money_of(frozen, "tax-amount"), ==, 0);
	g_assert_cmpint(int_of(frozen, "tax-jurisdiction-id"), ==, j);
	g_object_get(frozen, "tax-exempt", &flag, NULL);
	g_assert_true(flag);
	{
		g_autoptr(VentureEntity) stored = venture_database_get(f->db, VENTURE_TYPE_INVOICE,
			venture_entity_get_id(invoice), NULL);
		g_autofree gchar *reason = NULL;
		g_object_get(stored, "tax-exempt", &flag, "tax-exempt-reason", &reason, NULL);
		g_assert_true(flag);
		g_assert_cmpstr(reason, ==, "501(c)(3)");
	}

	/* An exempt product with no matching rule levies nothing even when the
	 * line carries its own percent; no jurisdiction is recorded. */
	nowhere = customer_at(f, "Nowhere", "", "", "");
	g_clear_object(&invoice);
	invoice = draft(f, "INV-UNRULED", nowhere, "2026-02-03");
	unruled_id = line(f, invoice, "80 USD", venture_entity_get_id(exempt), 10);
	issue(f, invoice, "2026-02-03");
	g_clear_object(&frozen);
	frozen = line_by_id(f, unruled_id);
	g_assert_cmpint(money_of(frozen, "tax-amount"), ==, 0);
	g_assert_cmpint(int_of(frozen, "tax-jurisdiction-id"), ==, 0);
	g_assert_cmpint(int_of(frozen, "tax-rate-scaled"), ==, 0);
	g_object_get(frozen, "tax-exempt", &flag, NULL);
	g_assert_true(flag);
}

/* The most specific matching rule wins; a rule for another state never matches. */
static void
test_rule_specificity(Fixture *f, gconstpointer unused)
{
	gint64 state = jurisdiction(f, "US-NY", "New York State", 40000, "2026-01-01", NULL);
	gint64 city = jurisdiction(f, "US-NY-NYC", "New York City", 88750, "2026-01-01", NULL);
	gint64 nj = jurisdiction(f, "US-NJ", "New Jersey", 66250, "2026-01-01", NULL);
	gint64 upstate = customer_at(f, "Albany Books", "NY", "ALBANY", "ALBANY");
	g_autoptr(VentureEntity) a = draft(f, "INV-A", f->customer, "2026-02-01");
	g_autoptr(VentureEntity) b = draft(f, "INV-B", upstate, "2026-02-01");
	gint64 a_line = line(f, a, "100 USD", 0, 0);
	gint64 b_line = line(f, b, "100 USD", 0, 0);
	g_autoptr(VentureEntity) fa = NULL;
	g_autoptr(VentureEntity) fb = NULL;
	(void)unused;
	rule(f, nj, "NJ", NULL, NULL);
	rule(f, state, "NY", NULL, NULL);
	rule(f, city, "NY", "NEW YORK", "NEW YORK");
	issue(f, a, "2026-02-01");
	issue(f, b, "2026-02-01");
	fa = line_by_id(f, a_line);
	fb = line_by_id(f, b_line);
	g_assert_cmpint(int_of(fa, "tax-jurisdiction-id"), ==, city);
	g_assert_cmpint(money_of(fa, "tax-amount"), ==, 888);
	g_assert_cmpint(int_of(fb, "tax-jurisdiction-id"), ==, state);
	g_assert_cmpint(money_of(fb, "tax-amount"), ==, 400);
}

/* Address codes match exactly; case and whitespace are normalised, nothing else. */
static void
test_exact_match_only(Fixture *f, gconstpointer unused)
{
	gint64 j = jurisdiction(f, "US-NY-NYC", "New York City", 88750, "2026-01-01", NULL);
	gint64 lower = customer_at(f, "Lower", "ny", "new york ", "New York");
	gint64 typo = customer_at(f, "Typo", "NY", "NEW YORK", "NEW YORK CITY");
	g_autoptr(VentureEntity) a = draft(f, "INV-L", lower, "2026-02-01");
	g_autoptr(VentureEntity) b = draft(f, "INV-T", typo, "2026-02-01");
	gint64 a_line = line(f, a, "100 USD", 0, 0);
	gint64 b_line = line(f, b, "100 USD", 0, 0);
	g_autoptr(VentureEntity) fa = NULL;
	g_autoptr(VentureEntity) fb = NULL;
	(void)unused;
	rule(f, j, "NY", "NEW YORK", "NEW YORK");
	issue(f, a, "2026-02-01");
	issue(f, b, "2026-02-01");
	fa = line_by_id(f, a_line);
	fb = line_by_id(f, b_line);
	g_assert_cmpint(int_of(fa, "tax-jurisdiction-id"), ==, j);
	g_assert_cmpint(int_of(fb, "tax-jurisdiction-id"), ==, 0);
	g_assert_cmpint(money_of(fb, "tax-amount"), ==, 0);
}

/* The rate effective on the issue date applies; an issued invoice never moves
 * when the rate changes later, and a new invoice takes the new rate. */
static void
test_rate_change_leaves_issued_invoice(Fixture *f, gconstpointer unused)
{
	gint64 j = nyc(f);
	g_autoptr(VentureEntity) first = draft(f, "INV-1", f->customer, "2026-02-01");
	gint64 first_line = line(f, first, "80 USD", 0, 0);
	g_autoptr(VentureEntity) row = NULL;
	g_autoptr(VentureEntity) frozen = NULL;
	g_autoptr(VentureEntity) second = NULL;
	g_autoptr(VentureEntity) later = NULL;
	g_autoptr(VentureDateRange) period = NULL;
	g_autoptr(VentureReportResult) before = NULL;
	g_autoptr(VentureReportResult) after = NULL;
	g_autoptr(GTimeZone) utc = g_time_zone_new_utc();
	g_autoptr(GError) error = NULL;
	VentureReport *report;
	gint64 second_line;
	(void)unused;
	issue(f, first, "2026-02-01");
	period = venture_date_range_parse("2026-02", utc, 1, &error);
	g_assert_no_error(error);
	report = venture_report_registry_lookup(venture_context_get_report_registry(f->context), "sales_tax_return");
	g_assert_nonnull(report);
	before = venture_report_generate(report, f->context, period, NULL, &error);
	g_assert_no_error(error);

	/* Close the old rate and open a new one. */
	row = venture_database_get(f->db, VENTURE_TYPE_TAX_JURISDICTION, j, NULL);
	field(row, "effective-to", "2026-03-01");
	save(f, row);
	jurisdiction(f, "US-NY-NYC", "New York City", 100000, "2026-03-01", NULL);
	/* Even editing the frozen row's rate in place cannot reach the issued line. */
	g_object_set(row, "rate-scaled", (gint64)90000, NULL);
	save(f, row);

	frozen = line_by_id(f, first_line);
	g_assert_cmpint(int_of(frozen, "tax-rate-scaled"), ==, 88750);
	g_assert_cmpint(money_of(frozen, "tax-amount"), ==, 710);
	after = venture_report_generate(report, f->context, period, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(venture_report_result_get_row_count(after), ==, 1);
	g_assert_cmpint(((const VentureMoney *)g_value_get_boxed(
		venture_report_result_get_cell(after, 0, "tax_collected")))->amount, ==, 710);
	g_assert_cmpint(((const VentureMoney *)g_value_get_boxed(
		venture_report_result_get_cell(before, 0, "tax_collected")))->amount, ==, 710);

	/* Re-saving the issued invoice does not recompute its lines either. */
	g_object_set(first, "notes", "touched", NULL);
	{
		g_autoptr(VentureEntity) stored = venture_database_get(f->db, VENTURE_TYPE_INVOICE,
			venture_entity_get_id(first), NULL);
		g_object_set(stored, "notes", "touched", NULL);
		save(f, stored);
	}
	g_clear_object(&frozen);
	frozen = line_by_id(f, first_line);
	g_assert_cmpint(money_of(frozen, "tax-amount"), ==, 710);

	second = draft(f, "INV-2", f->customer, "2026-03-15");
	second_line = line(f, second, "80 USD", 0, 0);
	issue(f, second, "2026-03-15");
	g_clear_object(&frozen);
	frozen = line_by_id(f, second_line);
	g_assert_cmpint(int_of(frozen, "tax-rate-scaled"), ==, 100000);
	g_assert_cmpint(money_of(frozen, "tax-amount"), ==, 800);

	/* Before any rate is effective there is no jurisdiction rate to apply. */
	later = draft(f, "INV-0", f->customer, "2025-12-01");
	second_line = line(f, later, "80 USD", 0, 0);
	issue(f, later, "2025-12-01");
	g_clear_object(&frozen);
	frozen = line_by_id(f, second_line);
	g_assert_cmpint(int_of(frozen, "tax-jurisdiction-id"), ==, 0);
	g_assert_cmpint(money_of(frozen, "tax-amount"), ==, 0);
}

static const VentureMoney *
cell_money(VentureReportResult *result, guint row, const gchar *key)
{
	const GValue *value = venture_report_result_get_cell(result, row, key);
	g_assert_nonnull(value);
	return g_value_get_boxed(value);
}

static const gchar *
cell_text(VentureReportResult *result, guint row, const gchar *key)
{
	const GValue *value = venture_report_result_get_cell(result, row, key);
	g_assert_nonnull(value);
	return g_value_get_string(value);
}

static gint
row_for(VentureReportResult *result, const gchar *jurisdiction_code)
{
	guint i;
	for (i = 0; i < venture_report_result_get_row_count(result); i++)
		if (g_strcmp0(cell_text(result, i, "jurisdiction"), jurisdiction_code) == 0)
			return (gint)i;
	return -1;
}

/* One month with: a taxed NYC invoice, an exempt NYC invoice, a taxed NJ
 * invoice with a same-month void, a tax-coded invoice, a percent-only
 * invoice, an NYC credit note and a credit note with no jurisdiction. The
 * return breaks the month down per jurisdiction and its net due ties to
 * tax_liability's output side to the cent. */
static void
test_return_ties_to_liability(Fixture *f, gconstpointer unused)
{
	gint64 nyc_id = nyc(f);
	gint64 nj = jurisdiction(f, "US-NJ", "New Jersey", 66250, "2026-01-01", NULL);
	gint64 jersey = customer_at(f, "Hoboken Books", "NJ", "HUDSON", "HOBOKEN");
	gint64 nowhere = customer_at(f, "Nowhere", "", "", "");
	gint64 exempt_customer = customer_at(f, "Library", "NY", "NEW YORK", "NEW YORK");
	g_autoptr(VentureEntity) library = venture_database_get(f->db, VENTURE_TYPE_COMPANY, exempt_customer, NULL);
	g_autoptr(VentureEntity) code = record(f, "tax_code");
	g_autoptr(VentureEntity) a = draft(f, "A", f->customer, "2026-02-03");
	g_autoptr(VentureEntity) b = draft(f, "B", exempt_customer, "2026-02-04");
	g_autoptr(VentureEntity) c = draft(f, "C", jersey, "2026-02-05");
	g_autoptr(VentureEntity) d = draft(f, "D", nowhere, "2026-02-06");
	g_autoptr(VentureEntity) e = draft(f, "E", nowhere, "2026-02-07");
	g_autoptr(VentureEntity) coded = record(f, "invoice_line");
	g_autoptr(VentureEntity) credit = record(f, "customer_credit");
	g_autoptr(VentureEntity) loose = record(f, "customer_credit");
	g_autoptr(VentureDateRange) period = NULL;
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(VentureReportResult) liability = NULL;
	g_autoptr(VentureReportResult) march = NULL;
	g_autoptr(GTimeZone) utc = g_time_zone_new_utc();
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) void_at = venture_time_from_string("2026-02-20", NULL);
	gint64 net_due_sum = 0, output_sum = 0;
	gint row;
	guint i;
	(void)unused;
	rule(f, nj, "NJ", NULL, NULL);
	g_object_set(library, "tax-exempt", TRUE, "tax-exemption-number", "EX-1", NULL);
	save(f, library);
	g_object_set(code, "code", "NY-4", "name", "State only", "jurisdiction", "US-NY",
		"rate-numerator", (gint64)4, "rate-denominator", (gint64)100, "active", TRUE, NULL);
	save(f, code);

	line(f, a, "80 USD", 0, 0);            /* NYC: 7.10 tax */
	line(f, b, "50 USD", 0, 0);            /* NYC exempt customer: 0 tax */
	line(f, c, "100 USD", 0, 0);           /* NJ: 6.63 tax, voided later this month */
	g_object_set(coded, "invoice-id", venture_entity_get_id(d), "description", "Work",
		"quantity", 1.0, "tax-code-id", venture_entity_get_id(code), NULL);
	field(coded, "unit-price", "100 USD"); /* tax code US-NY: 4.00 tax */
	save(f, coded);
	line(f, e, "10 USD", 0, 10);           /* percent only: 1.00 tax, no jurisdiction */
	issue(f, a, "2026-02-03");
	issue(f, b, "2026-02-04");
	issue(f, c, "2026-02-05");
	issue(f, d, "2026-02-06");
	issue(f, e, "2026-02-07");
	g_assert_true(venture_settlement_service_transition(venture_settlement_service_get(f->db),
		VENTURE_INVOICE(c), "void", void_at, NULL, &error));
	g_assert_no_error(error);

	g_object_set(credit, "customer-id", f->customer, "kind", "credit_note",
		"tax-jurisdiction-id", nyc_id, NULL);
	field(credit, "date", "2026-02-10");
	field(credit, "amount", "21.78 USD");
	field(credit, "tax-amount", "1.78 USD");
	save(f, credit);
	g_object_set(loose, "customer-id", f->customer, "kind", "credit_note", NULL);
	field(loose, "date", "2026-02-11");
	field(loose, "amount", "5.25 USD");
	field(loose, "tax-amount", "0.25 USD");
	save(f, loose);

	period = venture_date_range_parse("2026-02", utc, 1, &error);
	g_assert_no_error(error);
	result = venture_report_generate(venture_report_registry_lookup(
		venture_context_get_report_registry(f->context), "sales_tax_return"),
		f->context, period, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);

	row = row_for(result, "US-NY-NYC");
	g_assert_cmpint(row, >=, 0);
	g_assert_cmpstr(cell_text(result, row, "name"), ==, "New York City");
	g_assert_cmpint(cell_money(result, row, "gross_sales")->amount, ==, 13000);
	g_assert_cmpint(cell_money(result, row, "exempt_sales")->amount, ==, 5000);
	g_assert_cmpint(cell_money(result, row, "taxable_sales")->amount, ==, 8000);
	g_assert_cmpint(cell_money(result, row, "tax_collected")->amount, ==, 710);
	g_assert_cmpint(cell_money(result, row, "tax_credited")->amount, ==, 178);
	g_assert_cmpint(cell_money(result, row, "net_due")->amount, ==, 532);

	/* Issue and void in the same month net to nothing for New Jersey. */
	row = row_for(result, "US-NJ");
	g_assert_cmpint(row, >=, 0);
	g_assert_cmpint(cell_money(result, row, "gross_sales")->amount, ==, 0);
	g_assert_cmpint(cell_money(result, row, "tax_collected")->amount, ==, 0);
	g_assert_cmpint(cell_money(result, row, "net_due")->amount, ==, 0);

	/* A tax code's jurisdiction string is its own row. */
	row = row_for(result, "US-NY");
	g_assert_cmpint(row, >=, 0);
	g_assert_cmpint(cell_money(result, row, "taxable_sales")->amount, ==, 10000);
	g_assert_cmpint(cell_money(result, row, "tax_collected")->amount, ==, 400);

	/* Percent-only tax and the unattributed credit note stay visible. */
	row = row_for(result, "unassigned");
	g_assert_cmpint(row, >=, 0);
	g_assert_cmpint(cell_money(result, row, "gross_sales")->amount, ==, 1000);
	g_assert_cmpint(cell_money(result, row, "tax_collected")->amount, ==, 100);
	g_assert_cmpint(cell_money(result, row, "tax_credited")->amount, ==, 25);
	g_assert_cmpint(cell_money(result, row, "net_due")->amount, ==, 75);

	for (i = 0; i < venture_report_result_get_row_count(result); i++)
		net_due_sum += cell_money(result, i, "net_due")->amount;
	liability = venture_report_generate(venture_report_registry_lookup(
		venture_context_get_report_registry(f->context), "tax_liability"),
		f->context, period, NULL, &error);
	g_assert_no_error(error);
	for (i = 0; i < venture_report_result_get_row_count(liability); i++)
		if (g_strcmp0(cell_text(liability, i, "direction"), "output") == 0)
			output_sum += cell_money(liability, i, "liability")->amount;
	g_assert_cmpint(output_sum, ==, 710 + 400 + 100 - 178 - 25);
	g_assert_cmpint(net_due_sum, ==, output_sum);

	/* The next month has nothing: the period boundary is respected. */
	g_clear_pointer(&period, venture_date_range_free);
	period = venture_date_range_parse("2026-03", utc, 1, &error);
	g_assert_no_error(error);
	march = venture_report_generate(venture_report_registry_lookup(
		venture_context_get_report_registry(f->context), "sales_tax_return"),
		f->context, period, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(venture_report_result_get_row_count(march), ==, 0);
}

/* The CSV has the 1099 pack's shape: a header of keys and every cell quoted. */
static void
test_csv_export(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) a = draft(f, "A", f->customer, "2026-02-03");
	g_autoptr(VentureDateRange) period = NULL;
	g_autoptr(GTimeZone) utc = g_time_zone_new_utc();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *csv = NULL;
	g_autofree gchar *filtered = NULL;
	g_autoptr(JsonObject) options = json_object_new();
	gchar **lines;
	(void)unused;
	nyc(f);
	line(f, a, "80 USD", 0, 0);
	issue(f, a, "2026-02-03");
	period = venture_date_range_parse("2026-02", utc, 1, &error);
	g_assert_no_error(error);
	csv = venture_sales_tax_service_export_csv(venture_sales_tax_service_get(f->db),
		f->context, period, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(csv);
	lines = g_strsplit(csv, "\n", -1);
	g_assert_cmpstr(lines[0], ==,
		"report,period_start,period_end,jurisdiction,name,kind,rate_percent,gross_sales,exempt_sales,taxable_sales,tax_collected,tax_credited,net_due");
	g_assert_cmpstr(lines[1], ==,
		"\"sales_tax_return\",\"2026-02-01\",\"2026-03-01\",\"US-NY-NYC\",\"New York City\",\"sales\",\"8.8750\",\"80.00 USD\",\"0.00 USD\",\"80.00 USD\",\"7.10 USD\",\"0.00 USD\",\"7.10 USD\"");
	g_assert_cmpstr(lines[2], ==, "");
	g_strfreev(lines);

	/* A jurisdiction filter keeps only that code; an unknown code is refused. */
	json_object_set_string_member(options, "jurisdiction", "US-NJ");
	filtered = venture_sales_tax_service_export_csv(venture_sales_tax_service_get(f->db),
		f->context, period, options, &error);
	g_assert_null(filtered);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
	g_clear_error(&error);
	json_object_set_string_member(options, "jurisdiction", "US-NY-NYC");
	filtered = venture_sales_tax_service_export_csv(venture_sales_tax_service_get(f->db),
		f->context, period, options, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(filtered, ==, csv);
}

/* With the module off the records vanish, the report is gone, and issuing
 * falls back to the line's own percent. */
static void
test_module_off(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(VentureEntity) frozen = NULL;
	gint64 id;
	(void)unused;
	nyc(f);
	venture_config_set_module_enabled(f->config, "sales_tax", FALSE);
	g_assert_cmpuint(venture_entity_registry_lookup(venture_entity_registry_get_default(),
		"tax_jurisdiction"), ==, G_TYPE_INVALID);
	g_assert_cmpuint(venture_entity_registry_lookup(venture_entity_registry_get_default(),
		"tax_rule"), ==, G_TYPE_INVALID);
	g_assert_null(venture_report_registry_lookup(venture_context_get_report_registry(f->context),
		"sales_tax_return"));
	invoice = draft(f, "INV-OFF", f->customer, "2026-02-01");
	id = line(f, invoice, "80 USD", 0, 5);
	issue(f, invoice, "2026-02-01");
	frozen = line_by_id(f, id);
	g_assert_cmpint(money_of(frozen, "tax-amount"), ==, 400);
	g_assert_cmpint(int_of(frozen, "tax-jurisdiction-id"), ==, 0);
	venture_config_set_module_enabled(f->config, "sales_tax", TRUE);
}

/* The export route and the CLI verb reach the same service. */
typedef struct { gboolean done; GError *error; GBytes *bytes; gchar *out, *err; } Result;
typedef struct { Fixture base; VentureWebServer *server; gchar *directory; } ServerFixture;

static void
server_setup(ServerFixture *s, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GSocketListener) listener = g_socket_listener_new();
	guint16 port = g_socket_listener_add_any_inet_port(listener, NULL, &error);
	g_assert_no_error(error);
	g_socket_listener_close(listener);
	s->directory = g_dir_make_tmp("venture-sales-tax-XXXXXX", &error);
	g_assert_no_error(error);
	setup(&s->base, unused);
	g_object_set(s->base.config, "state-dir", s->directory, "server-bind-address", "127.0.0.1",
		"server-port", (gint64)port, "security-require-auth", FALSE, NULL);
	s->server = venture_web_server_new(s->base.context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(s->server, &error));
	g_assert_no_error(error);
}

static void
server_teardown(ServerFixture *s, gconstpointer unused)
{
	venture_web_server_stop(s->server);
	g_clear_object(&s->server);
	teardown(&s->base, unused);
	venture_test_remove_tree(s->directory);
	g_free(s->directory);
}

static void
http_done(GObject *source, GAsyncResult *result, gpointer data)
{
	Result *r = data;
	r->bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &r->error);
	r->done = TRUE;
}

static guint
request(ServerFixture *s, const gchar *path, gchar **out)
{
	g_autoptr(SoupSession) session = soup_session_new_with_options("timeout", 15, NULL);
	g_autofree gchar *url = g_strconcat(venture_web_server_get_base_url(s->server), path, NULL);
	g_autoptr(SoupMessage) message = soup_message_new("GET", url);
	Result result;
	memset(&result, 0, sizeof(result));
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	soup_session_send_and_read_async(session, message, G_PRIORITY_DEFAULT, NULL, http_done, &result);
	while (!result.done)
		g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(result.error);
	if (out)
		*out = g_strndup(g_bytes_get_data(result.bytes, NULL), g_bytes_get_size(result.bytes));
	g_bytes_unref(result.bytes);
	return soup_message_get_status(message);
}

static void
cli_done(GObject *source, GAsyncResult *result, gpointer data)
{
	Result *r = data;
	g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, &r->out, &r->err, &r->error);
	r->done = TRUE;
}

static gboolean
cli_timeout(gpointer process)
{
	g_subprocess_force_exit(process);
	return G_SOURCE_CONTINUE;
}

static gchar *
cli(ServerFixture *s, const gchar *const *args, gboolean expect_success)
{
	g_autoptr(GSubprocessLauncher) launcher = g_subprocess_launcher_new(
		G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE);
	g_autoptr(GSubprocess) process = NULL;
	g_autoptr(GPtrArray) argv = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GError) error = NULL;
	Result result;
	guint i, timeout;
	memset(&result, 0, sizeof(result));
	g_ptr_array_add(argv, g_canonicalize_filename("build/debug/venturectl", NULL));
	g_ptr_array_add(argv, g_strdup("--server"));
	g_ptr_array_add(argv, g_strdup(venture_web_server_get_base_url(s->server)));
	for (i = 0; args[i]; i++)
		g_ptr_array_add(argv, g_strdup(args[i]));
	g_ptr_array_add(argv, NULL);
	g_subprocess_launcher_setenv(launcher, "VENTURE_TOKEN", "sales-tax-fixture", TRUE);
	process = g_subprocess_launcher_spawnv(launcher, (const gchar *const *)argv->pdata, &error);
	g_assert_no_error(error);
	timeout = g_timeout_add_seconds(30, cli_timeout, process);
	g_subprocess_communicate_utf8_async(process, NULL, NULL, cli_done, &result);
	while (!result.done)
		g_main_context_iteration(NULL, TRUE);
	g_source_remove(timeout);
	g_assert_no_error(result.error);
	if (g_subprocess_get_successful(process) != expect_success)
		g_test_message("CLI: %s%s", result.out, result.err);
	g_assert_true(g_subprocess_get_successful(process) == expect_success);
	g_free(result.err);
	return result.out;
}

static void
test_surfaces(ServerFixture *s, gconstpointer unused)
{
	Fixture *f = &s->base;
	g_autoptr(VentureEntity) a = draft(f, "A", f->customer, "2026-02-03");
	const gchar *export_args[] = { "sales-tax", "export", "period=2026-02", NULL };
	const gchar *filtered_args[] = { "sales-tax", "export", "period=2026-02", "jurisdiction=US-NY-NYC", NULL };
	const gchar *bad_args[] = { "sales-tax", "nope", NULL };
	const gchar *report_args[] = { "-f", "csv", "report", "sales_tax_return", "2026-02", NULL };
	g_autofree gchar *body = NULL, *out = NULL, *filtered = NULL, *bad = NULL, *generic = NULL;
	(void)unused;
	nyc(f);
	line(f, a, "80 USD", 0, 0);
	issue(f, a, "2026-02-03");
	g_assert_cmpuint(request(s, "/api/v1/sales-tax/export?period=2026-02", &body), ==, 200);
	g_assert_nonnull(strstr(body, "\"US-NY-NYC\",\"New York City\",\"sales\",\"8.8750\",\"80.00 USD\""));
	g_assert_cmpuint(request(s, "/api/v1/sales-tax/export?period=2026-02&jurisdiction=US-TX", NULL), ==, 404);
	g_assert_cmpuint(request(s, "/api/v1/sales-tax/export?period=not-a-period", NULL), >=, 400);
	out = cli(s, export_args, TRUE);
	g_assert_cmpstr(out, ==, body);
	filtered = cli(s, filtered_args, TRUE);
	g_assert_cmpstr(filtered, ==, body);
	bad = cli(s, bad_args, FALSE);
	/* The generic report surface renders the same figures. */
	generic = cli(s, report_args, TRUE);
	g_assert_nonnull(strstr(generic, "US-NY-NYC"));
	g_assert_nonnull(strstr(generic, "7.10"));
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/sales-tax/records", test_records);
	g_test_add("/sales-tax/overlap-refused", Fixture, NULL, setup, test_overlap_refused, teardown);
	g_test_add("/sales-tax/issue-applies-jurisdiction", Fixture, NULL, setup, test_issue_applies_jurisdiction, teardown);
	g_test_add("/sales-tax/no-rule-falls-back", Fixture, NULL, setup, test_no_rule_falls_back, teardown);
	g_test_add("/sales-tax/tax-code-overrides-rule", Fixture, NULL, setup, test_tax_code_overrides_rule, teardown);
	g_test_add("/sales-tax/exempt-customer-and-product", Fixture, NULL, setup, test_exempt_customer_and_product, teardown);
	g_test_add("/sales-tax/rule-specificity", Fixture, NULL, setup, test_rule_specificity, teardown);
	g_test_add("/sales-tax/exact-match-only", Fixture, NULL, setup, test_exact_match_only, teardown);
	g_test_add("/sales-tax/rate-change-leaves-issued", Fixture, NULL, setup, test_rate_change_leaves_issued_invoice, teardown);
	g_test_add("/sales-tax/return-ties-to-liability", Fixture, NULL, setup, test_return_ties_to_liability, teardown);
	g_test_add("/sales-tax/csv-export", Fixture, NULL, setup, test_csv_export, teardown);
	g_test_add("/sales-tax/module-off", Fixture, NULL, setup, test_module_off, teardown);
	g_test_add("/sales-tax/surfaces", ServerFixture, NULL, server_setup, test_surfaces, server_teardown);
	return g_test_run();
}
