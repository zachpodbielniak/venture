/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>

/* Billing must coexist with the example plugin's recurring-cost record. */
static void
test_catalog(void)
{
	static const gchar *const names[] = {
		"plan", "plan_price", "customer_subscription", "subscription_event",
		"dunning_step", "billing_notice"
	};
	VentureEntityRegistry *registry;
	gsize i;

	registry = venture_entity_registry_get_default();
	for (i = 0; i < G_N_ELEMENTS(names); i++)
		g_assert_cmpuint(venture_entity_registry_lookup(registry, names[i]), !=, G_TYPE_INVALID);
}

typedef struct
{
	VentureDatabase *db;
	gint64 org;
	gint64 company;
	gint64 price;
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

static void
setup(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) company = NULL;
	g_autoptr(VentureEntity) plan = NULL;
	g_autoptr(VentureEntity) price = NULL;
	f->db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->org = 1;
	company = record(f, "company");
	g_object_set(company, "name", "Lightsite customer", NULL);
	save(f, company);
	f->company = venture_entity_get_id(company);
	plan = record(f, "plan");
	g_object_set(plan, "name", "Lightsite Starter", "code", "starter", "active", TRUE, NULL);
	save(f, plan);
	price = record(f, "plan_price");
	g_object_set(price, "plan-id", venture_entity_get_id(plan), "currency", "USD",
		"active", TRUE, "per-seat", TRUE, NULL);
	field(price, "amount", "30 USD");
	save(f, price);
	f->price = venture_entity_get_id(price);
}

static void
teardown(Fixture *f, gconstpointer data)
{
	g_clear_object(&f->db);
}

static gint64
integer(VentureEntity *e, const gchar *name)
{
	gint64 value;
	g_object_get(e, name, &value, NULL);
	return value;
}

static VentureEntity *
request(Fixture *f, const gchar *verb, gint64 id, const gchar *date)
{
	VentureEntity *e = record(f, "billing_request");
	g_object_set(e, "action", verb, "subscription-id", id, NULL);
	field(e, "at", date);
	return e;
}

/* One request must create a subscription and event; CRUD cannot bypass it. */
static void
test_start_and_guard(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) action = request(f, "start", 0, "2026-01-01");
	g_autoptr(VentureEntity) sub = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GError) error = NULL;
	GType type = venture_entity_registry_lookup(venture_entity_registry_get_default(), "customer_subscription");
	g_object_set(action, "company-id", f->company, "plan-price-id", f->price, "seats", (gint64)2, NULL);
	save(f, action);
	g_assert_cmpint(integer(action, "subscription-id"), >, 0);
	sub = venture_database_get(f->db, type, integer(action, "subscription-id"), &error);
	g_assert_no_error(error);
	g_assert_cmpint(integer(sub, "seats"), ==, 2);
	field(sub, "status", "cancelled");
	g_assert_false(venture_database_save(f->db, sub, NULL, &error));
	g_assert_nonnull(g_strstr_len(error->message, -1, "VentureBillingService"));
	g_clear_error(&error);
	query = venture_query_new(venture_entity_registry_lookup(venture_entity_registry_get_default(), "subscription_event"));
	g_assert_cmpint(venture_database_count(f->db, query, &error), ==, 1);
	g_assert_no_error(error);
}

static gint64
start(Fixture *f)
{
	g_autoptr(VentureEntity) a = request(f, "start", 0, "2026-01-01");
	g_object_set(a, "company-id", f->company, "plan-price-id", f->price,
		"seats", (gint64)2, NULL);
	save(f, a);
	return integer(a, "subscription-id");
}

static gint64
count(Fixture *f, const gchar *name)
{
	g_autoptr(VentureQuery) q = venture_query_new(venture_entity_registry_lookup(venture_entity_registry_get_default(), name));
	return venture_database_count(f->db, q, NULL);
}

static VentureEntity *
subscription(Fixture *f, gint64 id)
{
	return venture_database_get(f->db, venture_entity_registry_lookup(venture_entity_registry_get_default(), "customer_subscription"), id, NULL);
}

static void
status_is(Fixture *f, gint64 id, const gchar *expected)
{
	g_autoptr(VentureEntity) s = subscription(f, id);
	gint status;
	GParamSpec *spec = g_object_class_find_property(G_OBJECT_GET_CLASS(s), "status");
	g_object_get(s, "status", &status, NULL);
	g_assert_cmpstr(venture_enum_to_nick(G_PARAM_SPEC_VALUE_TYPE(spec), status), ==, expected);
}

/* A due period is invoiced only once, including repeated sweep requests. */
static void
test_renewal(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) a = NULL;
	g_autoptr(VentureEntity) s = NULL;
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(VentureMoney) balance = NULL;
	g_autoptr(GDateTime) end = NULL;
	g_autofree gchar *text = NULL;
	gint64 id = start(f);
	a = request(f, "renew", id, "2026-02-01");
	save(f, a);
	g_assert_cmpint(count(f, "invoice"), ==, 1);
	balance = venture_settlement_service_invoice_balance(venture_settlement_service_get(f->db), integer(a, "invoice-id"), NULL, NULL);
	g_assert_nonnull(balance);
	g_assert_cmpint(venture_money_get_amount(balance), ==, 6000);
	s = subscription(f, id);
	g_object_get(s, "current-period-end", &end, NULL);
	text = g_date_time_format(end, "%F");
	g_assert_cmpstr(text, ==, "2026-03-01");
	g_clear_object(&a);
	a = request(f, "renew-sweep", 0, "2026-02-01");
	save(f, a);
	g_clear_object(&a);
	a = request(f, "renew-sweep", 0, "2026-02-01");
	save(f, a);
	g_assert_cmpint(count(f, "invoice"), ==, 1);
}

/* Trial activation must not bill before the trial expires. */
static void
test_trial(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) p = venture_database_get(f->db, venture_entity_registry_lookup(venture_entity_registry_get_default(), "plan_price"), f->price, NULL);
	g_autoptr(VentureEntity) a = NULL;
	gint64 id;
	g_object_set(p, "trial-days", (gint64)14, NULL);
	save(f, p);
	id = start(f);
	status_is(f, id, "trialing");
	a = request(f, "renew", id, "2026-01-14");
	save(f, a);
	g_assert_cmpint(count(f, "invoice"), ==, 0);
	g_clear_object(&a);
	a = request(f, "renew", id, "2026-01-15");
	save(f, a);
	status_is(f, id, "active");
	g_assert_cmpint(count(f, "invoice"), ==, 1);
}

static void
test_lifecycle(Fixture *f, gconstpointer data)
{
	static const gchar *const verbs[] = { "pause", "resume", "mark-payment-failed", "recover", "cancel" };
	static const gchar *const states[] = { "paused", "active", "past_due", "active", "cancelled" };
	gint64 id = start(f);
	guint i;
	for (i = 0; i < G_N_ELEMENTS(verbs); i++)
	{
		g_autoptr(VentureEntity) a = request(f, verbs[i], id, "2026-01-10");
		save(f, a);
		status_is(f, id, states[i]);
	}
	g_assert_cmpint(count(f, "subscription_event"), ==, 6);
}

/* Remaining days get their original allocation, including remainder cents. */
static void
test_seats_proration(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) a = NULL;
	g_autoptr(VentureEntity) s = NULL;
	g_autoptr(VentureMoney) amount = NULL;
	gint64 id = start(f);
	a = request(f, "change-seats", id, "2026-01-17");
	g_object_set(a, "seats", (gint64)3, NULL);
	save(f, a);
	g_object_get(a, "proration-amount", &amount, NULL);
	/* $30 / 31 => 97 cents for 24 days, then 96; remaining 15 = $14.48. */
	g_assert_cmpint(venture_money_get_amount(amount), ==, 1448);
	s = subscription(f, id);
	g_assert_cmpint(integer(s, "seats"), ==, 3);
}

static void
test_dunning(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) step = record(f, "dunning_step");
	g_autoptr(VentureEntity) a = NULL;
	gint64 id = start(f);
	g_object_set(step, "day-offset", (gint64)3, "active", TRUE, NULL);
	field(step, "action", "pause");
	save(f, step);
	a = request(f, "mark-payment-failed", id, "2026-01-02");
	save(f, a);
	g_clear_object(&a);
	a = request(f, "dunning-sweep", 0, "2026-01-05");
	g_object_set(a, "dry-run", TRUE, NULL);
	save(f, a);
	g_assert_cmpint(count(f, "billing_notice"), ==, 0);
	status_is(f, id, "past_due");
	g_clear_object(&a);
	a = request(f, "dunning-sweep", 0, "2026-01-05");
	save(f, a);
	g_clear_object(&a);
	a = request(f, "dunning-sweep", 0, "2026-01-05");
	save(f, a);
	g_assert_cmpint(count(f, "billing_notice"), ==, 1);
	status_is(f, id, "paused");
}

/* A veto after the invoice's draft write must roll every row back. */
static gboolean
reject_line(VentureDatabase *db, VentureEntity *e, VentureEntity *previous, gpointer data, GError **error)
{
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Injected line failure");
	return FALSE;
}

static void
test_rollback(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) a = NULL;
	g_autoptr(GError) error = NULL;
	gint64 id = start(f);
	gint64 audits = count(f, "audit_entry");
	venture_database_add_save_validator(f->db, VENTURE_TYPE_INVOICE_LINE, reject_line, NULL, NULL);
	a = request(f, "renew", id, "2026-02-01");
	g_assert_false(venture_database_save(f->db, a, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_cmpint(count(f, "invoice"), ==, 0);
	g_assert_cmpint(count(f, "subscription_event"), ==, 1);
	g_assert_cmpint(count(f, "audit_entry"), ==, audits);
}

static VentureReportResult *
report(Fixture *f, const gchar *name, const gchar *period_text)
{
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureContext) context = venture_context_new(config, f->db);
	g_autoptr(VentureDateRange) period = NULL;
	g_autoptr(GTimeZone) timezone = g_time_zone_new_utc();
	g_autoptr(GError) error = NULL;
	VentureReport *r = venture_report_registry_lookup(venture_context_get_report_registry(context), name);
	VentureReportResult *result;
	g_assert_nonnull(r);
	period = venture_date_range_parse(period_text, timezone, 1, &error);
	g_assert_no_error(error);
	result = venture_report_generate(r, context, period, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	return result;
}

static gint64
metric_amount(VentureReportResult *r, const gchar *name)
{
	GPtrArray *metrics = venture_report_result_get_metrics(r);
	guint i;
	for (i = 0; i < metrics->len; i++)
	{
		VentureMetric *m = g_ptr_array_index(metrics, i);
		if (g_strcmp0(venture_metric_get_key(m), name) == 0)
			return venture_money_get_amount(venture_metric_get_money(m));
	}
	g_assert_not_reached();
	return -1;
}

static void
test_mrr(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) a = NULL;
	g_autoptr(VentureReportResult) r = NULL;
	gint64 id = start(f);
	a = request(f, "change-seats", id, "2026-01-17");
	g_object_set(a, "seats", (gint64)3, NULL);
	save(f, a);
	g_clear_object(&a);
	a = request(f, "cancel", id, "2026-02-01");
	save(f, a);
	r = report(f, "mrr", "2026-01");
	g_assert_cmpint(metric_amount(r, "mrr"), ==, 9000);
	g_assert_cmpint(metric_amount(r, "arr"), ==, 108000);
	g_assert_cmpint(metric_amount(r, "new"), ==, 6000);
	g_assert_cmpint(metric_amount(r, "expansion"), ==, 3000);
	g_clear_object(&r);
	r = report(f, "mrr", "2026-02");
	g_assert_cmpint(metric_amount(r, "mrr"), ==, 0);
	g_assert_cmpint(metric_amount(r, "churn"), ==, 9000);
}

static void
test_churn(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) a = NULL;
	g_autoptr(VentureReportResult) r = NULL;
	gint64 id = start(f);
	a = request(f, "cancel", id, "2026-02-15");
	save(f, a);
	r = report(f, "churn", "2026-02");
	g_assert_cmpint(metric_amount(r, "opening_mrr"), ==, 6000);
	g_assert_cmpint(metric_amount(r, "lost_mrr"), ==, 6000);
}

static void
test_due_report(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureReportResult) r = NULL;
	start(f);
	r = report(f, "subscriptions_due", "2026-01");
	g_assert_cmpuint(venture_report_result_get_row_count(r), ==, 1);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/billing/catalog", test_catalog);
	g_test_add("/billing/start-and-guard", Fixture, NULL, setup, test_start_and_guard, teardown);
	g_test_add("/billing/renewal", Fixture, NULL, setup, test_renewal, teardown);
	g_test_add("/billing/trial", Fixture, NULL, setup, test_trial, teardown);
	g_test_add("/billing/lifecycle", Fixture, NULL, setup, test_lifecycle, teardown);
	g_test_add("/billing/seats-proration", Fixture, NULL, setup, test_seats_proration, teardown);
	g_test_add("/billing/dunning", Fixture, NULL, setup, test_dunning, teardown);
	g_test_add("/billing/rollback", Fixture, NULL, setup, test_rollback, teardown);
	g_test_add("/billing/mrr", Fixture, NULL, setup, test_mrr, teardown);
	g_test_add("/billing/churn", Fixture, NULL, setup, test_churn, teardown);
	g_test_add("/billing/due-report", Fixture, NULL, setup, test_due_report, teardown);
	return g_test_run();
}
