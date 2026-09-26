/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <string.h>
#include "venture-test-util.h"

/*
 * Usage-based pricing: a metered plan price charges for the units a
 * subscription used beyond those it includes, on the renewal invoice that
 * also carries the base charge. The arithmetic is money times a whole
 * count, rounded once to the currency; a report is counted in exactly one
 * period, [start, end).
 */

typedef struct
{
	VentureDatabase *db;
	gint64 org;
	gint64 company;
	gint64 plan;
	gint64 metered;
	gint64 flat;
} Fixture;

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

static gint64
integer(VentureEntity *e, const gchar *name)
{
	gint64 value;
	g_object_get(e, name, &value, NULL);
	return value;
}

static gint64
price(Fixture *f, const gchar *amount, const gchar *unit, const gchar *rate, gint64 included, gint64 trial)
{
	g_autoptr(VentureEntity) p = record(f, "plan_price");
	g_object_set(p, "plan-id", f->plan, "currency", "USD", "active", TRUE, "trial-days", trial,
		"usage-unit", unit, "included-units", included, NULL);
	field(p, "amount", amount);
	if (rate != NULL)
		field(p, "unit-amount", rate);
	save(f, p);
	return venture_entity_get_id(p);
}

static void
setup(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) company = NULL;
	g_autoptr(VentureEntity) plan = NULL;
	(void)data;
	f->db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->org = 1;
	company = record(f, "company");
	g_object_set(company, "name", "Metered customer", NULL);
	save(f, company);
	f->company = venture_entity_get_id(company);
	plan = record(f, "plan");
	g_object_set(plan, "name", "API", "code", "api", "active", TRUE, NULL);
	save(f, plan);
	f->plan = venture_entity_get_id(plan);
	f->metered = price(f, "30 USD", "API calls", "0.01 USD", 1000, 0);
	f->flat = price(f, "30 USD", NULL, NULL, 0, 0);
}

static void
teardown(Fixture *f, gconstpointer data)
{
	(void)data;
	g_clear_object(&f->db);
}

static VentureEntity *
request(Fixture *f, const gchar *verb, gint64 id, const gchar *date)
{
	VentureEntity *e = record(f, "billing_request");
	g_object_set(e, "action", verb, "subscription-id", id, NULL);
	field(e, "at", date);
	return e;
}

static gint64
start(Fixture *f, gint64 plan_price, const gchar *date)
{
	g_autoptr(VentureEntity) a = request(f, "start", 0, date);
	g_object_set(a, "company-id", f->company, "plan-price-id", plan_price, NULL);
	save(f, a);
	return integer(a, "subscription-id");
}

/* Returns the renewal's invoice. */
static gint64
renew(Fixture *f, gint64 id, const gchar *date)
{
	g_autoptr(VentureEntity) a = request(f, "renew", id, date);
	save(f, a);
	return integer(a, "invoice-id");
}

static gboolean
use(Fixture *f, gint64 id, gint64 quantity, const gchar *at, const gchar *key, GError **error)
{
	g_autoptr(VentureEntity) u = record(f, "usage_record");
	g_object_set(u, "subscription-id", id, "quantity", quantity, "idempotency-key", key, NULL);
	if (at != NULL)
		field(u, "occurred-at", at);
	return venture_database_save(f->db, u, NULL, error);
}

static void
used(Fixture *f, gint64 id, gint64 quantity, const gchar *at, const gchar *key)
{
	g_autoptr(GError) error = NULL;
	g_assert_true(use(f, id, quantity, at, key, &error));
	g_assert_no_error(error);
}

static gint64
owed(Fixture *f, gint64 invoice)
{
	g_autoptr(VentureMoney) balance = venture_settlement_service_invoice_balance(
		venture_settlement_service_get(f->db), invoice, NULL, NULL);
	g_assert_nonnull(balance);
	return venture_money_get_amount(balance);
}

/* The invoice's lines' descriptions, in order, joined by newlines. */
static gchar *
lines(Fixture *f, gint64 invoice)
{
	g_autoptr(VentureQuery) q = venture_query_new(VENTURE_TYPE_INVOICE_LINE);
	g_autoptr(GPtrArray) rows = NULL;
	GString *text = g_string_new(NULL);
	guint i;
	venture_query_set_limit(q, 0);
	g_assert_true(venture_query_add_filter_int(q, "invoice-id", VENTURE_FILTER_OP_EQ, invoice, NULL));
	g_assert_true(venture_query_add_order(q, "id", VENTURE_SORT_ASCENDING, NULL));
	rows = venture_database_find(f->db, q, NULL);
	g_assert_nonnull(rows);
	for (i = 0; i < rows->len; i++)
	{
		g_autofree gchar *description = NULL;
		g_object_get(g_ptr_array_index(rows, i), "description", &description, NULL);
		g_string_append_printf(text, "%s%s", i > 0 ? "\n" : "", description);
	}
	return g_string_free(text, FALSE);
}

/*
 * A metered price names its unit and rate together, in its own currency.
 * If this regresses, a price can carry a rate with nothing to count, or a
 * rate in another currency that every renewal then refuses to add.
 */
static void
test_price_metering(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) rate_only = record(f, "plan_price");
	g_autoptr(VentureEntity) foreign = record(f, "plan_price");
	g_autoptr(VentureEntity) stored = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *name = NULL;
	(void)data;
	g_object_set(rate_only, "plan-id", f->plan, "currency", "USD", "active", TRUE, NULL);
	field(rate_only, "amount", "30 USD");
	field(rate_only, "unit-amount", "0.01 USD");
	g_assert_false(venture_database_save(f->db, rate_only, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	g_object_set(foreign, "plan-id", f->plan, "currency", "USD", "active", TRUE, "usage-unit", "API calls", NULL);
	field(foreign, "amount", "30 USD");
	field(foreign, "unit-amount", "0.01 EUR");
	g_assert_false(venture_database_save(f->db, foreign, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	stored = venture_database_get(f->db, VENTURE_TYPE_PLAN_PRICE, f->metered, NULL);
	g_assert_true(venture_plan_price_is_metered(VENTURE_PLAN_PRICE(stored)));
	name = venture_entity_get_display_name(stored);
	g_assert_cmpstr(name, ==, "$30.00 a month + API calls at $0.01");
}

/*
 * A report on the boundary belongs to the period it starts, and every
 * report is billed once: January's usage on the invoice issued on
 * 1 February, February's -- including the one at exactly 1 February
 * 00:00 -- on 1 March's. If this regresses, a report at midnight on the
 * renewal day is billed twice, or not at all.
 */
static void
test_boundary_counted_once(Fixture *f, gconstpointer data)
{
	g_autoptr(GDateTime) january = venture_time_from_string("2026-01-01", NULL);
	g_autoptr(GDateTime) boundary = venture_time_from_string("2026-02-01", NULL);
	gint64 id, february, march;
	g_autofree gchar *text = NULL;
	(void)data;
	id = start(f, f->metered, "2026-01-01");
	used(f, id, 700, "2026-01-01T00:00:00Z", "jan-first");
	used(f, id, 540, "2026-01-31T23:59:59Z", "jan-last");
	used(f, id, 1500, "2026-02-01T00:00:00Z", "feb-first");
	g_assert_cmpint(venture_billing_usage_total(f->db, f->org, id, january, boundary, NULL), ==, 1240);
	february = renew(f, id, "2026-02-01");
	/* $30 base, plus 240 calls over the 1,000 included at $0.01. */
	g_assert_cmpint(owed(f, february), ==, 3240);
	text = lines(f, february);
	g_assert_nonnull(strstr(text, "240 API calls \xc3\x97 $0.01"));
	g_assert_nonnull(strstr(text, "1,240 used, 1,000 included"));
	g_assert_nonnull(strstr(text, "2026-01-01 to 2026-02-01"));
	g_clear_pointer(&text, g_free);
	march = renew(f, id, "2026-03-01");
	/* $30, plus 500 over at $0.01: the 1 February report once, here. */
	g_assert_cmpint(owed(f, march), ==, 3500);
	text = lines(f, march);
	g_assert_nonnull(strstr(text, "500 API calls \xc3\x97 $0.01"));
}

/*
 * Usage within the included units adds no line at all, and a flat price
 * is untouched by the machinery. If this regresses, every invoice grows a
 * $0.00 usage line, or a flat plan refuses to renew.
 */
static void
test_within_included(Fixture *f, gconstpointer data)
{
	gint64 metered, flat, invoice;
	g_autofree gchar *text = NULL;
	g_autoptr(GError) error = NULL;
	(void)data;
	metered = start(f, f->metered, "2026-01-01");
	flat = start(f, f->flat, "2026-01-01");
	used(f, metered, 1000, "2026-01-10T00:00:00Z", NULL);
	invoice = renew(f, metered, "2026-02-01");
	g_assert_cmpint(owed(f, invoice), ==, 3000);
	text = lines(f, invoice);
	g_assert_null(strstr(text, "API calls \xc3\x97"));
	/* A flat price takes no usage: there is nothing to price it at. */
	g_assert_false(use(f, flat, 5, "2026-01-10T00:00:00Z", NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_cmpint(owed(f, renew(f, flat, "2026-02-01")), ==, 3000);
}

/*
 * A rate below a cent is multiplied exactly and rounded once, half to
 * even, to the currency: 1,235 calls at $0.001 is $1.235, billed $1.24.
 * If this regresses, the charge is computed in floating point or rounded
 * per unit and drifts from what the price says.
 */
static void
test_sub_cent_rate(Fixture *f, gconstpointer data)
{
	gint64 fine, id;
	(void)data;
	fine = price(f, "10 USD", "API calls", "0.001 USD", 0, 0);
	id = start(f, fine, "2026-01-01");
	used(f, id, 1235, "2026-01-05T00:00:00Z", NULL);
	g_assert_cmpint(owed(f, renew(f, id, "2026-02-01")), ==, 1124);
}

/*
 * A report repeated with the same idempotency key is refused as a
 * conflict and counted once; a report for a period already invoiced is
 * refused, and so is deleting a billed one. If this regresses, a sender's
 * retry doubles a customer's bill, or usage lands in a period that was
 * already billed and is never charged.
 */
static void
test_idempotent_and_closed(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureQuery) q = venture_query_new(VENTURE_TYPE_USAGE_RECORD);
	g_autoptr(VentureEntity) billed = NULL;
	gint64 id;
	(void)data;
	id = start(f, f->metered, "2026-01-01");
	used(f, id, 1100, "2026-01-05T00:00:00Z", "batch-1");
	g_assert_false(use(f, id, 1100, "2026-01-05T00:00:00Z", "batch-1", &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
	g_clear_error(&error);
	g_assert_false(use(f, id, 0, "2026-01-05T00:00:00Z", NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	g_assert_cmpint(owed(f, renew(f, id, "2026-02-01")), ==, 3100);
	g_assert_false(use(f, id, 10, "2026-01-20T00:00:00Z", NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	venture_query_set_limit(q, 1);
	billed = venture_database_find_one(f->db, q, NULL);
	g_assert_nonnull(billed);
	g_assert_false(venture_database_delete(f->db, billed, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	/* A report without a time is the present: the open period. */
	used(f, id, 1, NULL, NULL);
}

/*
 * Use during a free trial is recorded and not charged: the first invoice,
 * when the trial ends, is the base price alone. If this regresses, a trial
 * customer's first bill includes everything they tried.
 */
static void
test_trial_not_charged(Fixture *f, gconstpointer data)
{
	gint64 trial, id;
	(void)data;
	trial = price(f, "30 USD", "API calls", "0.01 USD", 0, 14);
	id = start(f, trial, "2026-01-01");
	used(f, id, 5000, "2026-01-05T00:00:00Z", NULL);
	g_assert_cmpint(owed(f, renew(f, id, "2026-01-15")), ==, 3000);
}

/*
 * A price changed at renewal still bills the ended period at the rate that
 * was in force in it. If this regresses, a customer moving to a dearer
 * rate is charged the new rate for the month before they moved.
 */
static void
test_rate_of_the_period(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) change = NULL;
	gint64 dearer, id;
	(void)data;
	dearer = price(f, "30 USD", "API calls", "0.05 USD", 1000, 0);
	id = start(f, f->metered, "2026-01-01");
	used(f, id, 1100, "2026-01-05T00:00:00Z", NULL);
	change = request(f, "change", id, "2026-01-10");
	g_object_set(change, "plan-price-id", dearer, "at-period-end", TRUE, NULL);
	save(f, change);
	/* 100 over at the January rate of $0.01, not $0.05. */
	g_assert_cmpint(owed(f, renew(f, id, "2026-02-01")), ==, 3100);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/billing-usage/price-metering", Fixture, NULL, setup, test_price_metering, teardown);
	g_test_add("/billing-usage/boundary-counted-once", Fixture, NULL, setup, test_boundary_counted_once, teardown);
	g_test_add("/billing-usage/within-included", Fixture, NULL, setup, test_within_included, teardown);
	g_test_add("/billing-usage/sub-cent-rate", Fixture, NULL, setup, test_sub_cent_rate, teardown);
	g_test_add("/billing-usage/idempotent-and-closed", Fixture, NULL, setup, test_idempotent_and_closed, teardown);
	g_test_add("/billing-usage/trial-not-charged", Fixture, NULL, setup, test_trial_not_charged, teardown);
	g_test_add("/billing-usage/rate-of-the-period", Fixture, NULL, setup, test_rate_of_the_period, teardown);
	return g_test_run();
}
