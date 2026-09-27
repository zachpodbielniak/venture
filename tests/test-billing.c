/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <string.h>
#include "venture-test-util.h"
#include "venture-test-accounting.h"

/* Billing must coexist with the example plugin's recurring-cost record. */
static void
test_catalog(void)
{
	static const gchar *const names[] = {
		"plan", "plan_price", "customer_subscription", "subscription_event",
		"dunning_step", "billing_notice", "customer_payment_method"
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
	g_assert_cmpint(count(f, "invoice"), ==, 2);
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
	g_assert_cmpint(count(f, "invoice"), ==, 2);
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

/*
 * A quarterly price renews three months on, charges the quarter's price,
 * and a change part-way through prorates over the quarter's days. If this
 * regresses, a quarterly plan bills monthly at the quarterly price.
 */
static void
test_quarterly(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) p = venture_database_get(f->db, venture_entity_registry_lookup(venture_entity_registry_get_default(), "plan_price"), f->price, NULL);
	g_autoptr(VentureEntity) a = NULL;
	g_autoptr(VentureEntity) s = NULL;
	g_autoptr(VentureMoney) amount = NULL;
	g_autoptr(VentureMoney) balance = NULL;
	g_autoptr(GDateTime) end = NULL;
	g_autofree gchar *text = NULL, *name = NULL;
	gint64 id;

	(void)data;
	field(p, "interval", "quarter");
	field(p, "amount", "90 USD");
	save(f, p);
	g_assert_cmpint(venture_billing_interval_months(2), ==, 3);
	name = venture_entity_get_display_name(p);
	g_assert_cmpstr(name, ==, "$90.00 a quarter per seat");

	id = start(f);
	s = subscription(f, id);
	g_object_get(s, "current-period-end", &end, NULL);
	text = g_date_time_format(end, "%F");
	g_assert_cmpstr(text, ==, "2026-04-01");

	/* One more seat halfway through a 90-day quarter: half of $90. */
	a = request(f, "change-seats", id, "2026-02-15");
	g_object_set(a, "seats", (gint64)3, NULL);
	save(f, a);
	g_object_get(a, "proration-amount", &amount, NULL);
	g_assert_cmpint(venture_money_get_amount(amount), ==, 4500);

	g_clear_object(&a);
	a = request(f, "renew", id, "2026-04-01");
	save(f, a);
	balance = venture_settlement_service_invoice_balance(venture_settlement_service_get(f->db),
		integer(a, "invoice-id"), NULL, NULL);
	g_assert_nonnull(balance);
	/* Three seats at $90 plus the $45 carried from the change. */
	g_assert_cmpint(venture_money_get_amount(balance), ==, 27000 + 4500);
	g_clear_object(&s);
	g_clear_pointer(&end, g_date_time_unref);
	g_clear_pointer(&text, g_free);
	s = subscription(f, id);
	g_object_get(s, "current-period-end", &end, NULL);
	text = g_date_time_format(end, "%F");
	g_assert_cmpstr(text, ==, "2026-07-01");
}

/*
 * A plan that belongs to a venture is sold only to that venture's
 * customers; a plan with no venture is shared. Refused by the service, so
 * the form, the API, the CLI and the assistant all hold to it. If this
 * regresses, every customer can be put on every venture's plans.
 */
static void
test_venture_plans(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) press = record(f, "venture");
	g_autoptr(VentureEntity) studio = record(f, "venture");
	g_autoptr(VentureEntity) plan = NULL;
	g_autoptr(VentureEntity) customer = NULL;
	g_autoptr(VentureEntity) a = NULL;
	g_autoptr(GError) error = NULL;

	(void)data;
	g_object_set(press, "name", "Press", "slug", "press", NULL);
	g_object_set(studio, "name", "Studio", "slug", "studio", NULL);
	save(f, press);
	save(f, studio);

	plan = venture_database_get(f->db, VENTURE_TYPE_PLAN, 1, NULL);
	g_assert_nonnull(plan);
	g_object_set(plan, "venture-id", venture_entity_get_id(studio), NULL);
	save(f, plan);

	customer = record(f, "company");
	g_object_set(customer, "name", "A press customer", "venture-id", venture_entity_get_id(press), NULL);
	save(f, customer);

	a = request(f, "start", 0, "2026-01-01");
	g_object_set(a, "company-id", venture_entity_get_id(customer), "plan-price-id", f->price, NULL);
	g_assert_false(venture_database_save(f->db, a, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_nonnull(strstr(error->message, "venture"));
	g_clear_error(&error);
	g_clear_object(&a);

	/* The studio's own customer, and a customer with no venture, may. */
	g_object_set(customer, "venture-id", venture_entity_get_id(studio), NULL);
	save(f, customer);
	a = request(f, "start", 0, "2026-01-01");
	g_object_set(a, "company-id", venture_entity_get_id(customer), "plan-price-id", f->price, NULL);
	save(f, a);
	g_clear_object(&a);
	a = request(f, "start", 0, "2026-01-01");
	g_object_set(a, "company-id", f->company, "plan-price-id", f->price, NULL);
	save(f, a);
}

/* A discount on the fixture's plan. */
static gint64
discount(Fixture *f, gint64 plan, const gchar *name, gint64 percent, const gchar *amount, gint64 periods)
{
	g_autoptr(VentureEntity) d = record(f, "plan_discount");
	g_object_set(d, "plan-id", plan, "name", name, "percent-off", percent, "periods", periods,
		"active", TRUE, NULL);
	if (amount != NULL)
		field(d, "amount-off", amount);
	save(f, d);
	return venture_entity_get_id(d);
}

static gint64
invoice_total(Fixture *f, VentureEntity *action)
{
	g_autoptr(VentureMoney) balance = venture_settlement_service_invoice_balance(
		venture_settlement_service_get(f->db), integer(action, "invoice-id"), NULL, NULL);
	g_assert_nonnull(balance);
	return venture_money_get_amount(balance);
}

/*
 * A plan's discount comes off the invoices it covers -- the first N
 * periods, or every one -- and then stops; a discount from another plan
 * is refused. If this regresses, an introductory offer is either never
 * applied or applied for ever.
 */
static void
test_discount(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) price = venture_database_get(f->db, VENTURE_TYPE_PLAN_PRICE, f->price, NULL);
	g_autoptr(VentureEntity) other = record(f, "plan");
	g_autoptr(VentureEntity) a = NULL;
	g_autoptr(GError) error = NULL;
	gint64 plan = integer(price, "plan-id"), launch, flat, foreign, id;

	(void)data;
	launch = discount(f, plan, "Launch offer", 20, NULL, 2);
	flat = discount(f, plan, "Loyalty", 0, "10 USD", 0);
	g_object_set(other, "name", "Other", "code", "other", "active", TRUE, NULL);
	save(f, other);
	foreign = discount(f, venture_entity_get_id(other), "Not ours", 50, NULL, 0);

	/* 20% off $60 for two periods, then the full price. */
	a = request(f, "start", 0, "2026-01-01");
	g_object_set(a, "company-id", f->company, "plan-price-id", f->price, "seats", (gint64)2,
		"discount-id", launch, NULL);
	save(f, a);
	id = integer(a, "subscription-id");
	g_assert_cmpint(invoice_total(f, a), ==, 4800);
	g_clear_object(&a);
	a = request(f, "renew", id, "2026-02-01");
	save(f, a);
	g_assert_cmpint(invoice_total(f, a), ==, 4800);
	g_clear_object(&a);
	a = request(f, "renew", id, "2026-03-01");
	save(f, a);
	g_assert_cmpint(invoice_total(f, a), ==, 6000);
	g_clear_object(&a);

	/* $10 off every period. */
	a = request(f, "start", 0, "2026-01-01");
	g_object_set(a, "company-id", f->company, "plan-price-id", f->price, "seats", (gint64)2,
		"discount-id", flat, NULL);
	save(f, a);
	g_assert_cmpint(invoice_total(f, a), ==, 5000);
	g_clear_object(&a);

	a = request(f, "start", 0, "2026-01-01");
	g_object_set(a, "company-id", f->company, "plan-price-id", f->price, "discount-id", foreign, NULL);
	g_assert_false(venture_database_save(f->db, a, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
}

/* The customer mail queued in the outbox, oldest first. */
static GPtrArray *
mail(Fixture *f)
{
	g_autoptr(VentureQuery) q = venture_query_new(VENTURE_TYPE_MAIL_MESSAGE);
	g_autoptr(GError) error = NULL;
	GPtrArray *rows;
	venture_query_set_limit(q, 0);
	venture_query_add_order(q, "id", VENTURE_SORT_ASCENDING, NULL);
	rows = venture_database_find(f->db, q, &error);
	g_assert_no_error(error);
	return rows;
}

static gchar *
text(VentureEntity *e, const gchar *name)
{
	gchar *value = NULL;
	g_object_get(e, name, &value, NULL);
	return value;
}

static void
customer_email(Fixture *f, const gchar *email)
{
	g_autoptr(VentureEntity) company = venture_database_get(f->db, VENTURE_TYPE_COMPANY, f->company, NULL);
	g_object_set(company, "email", email, NULL);
	save(f, company);
}

/*
 * A trial about to end tells the customer when, and what the first
 * invoice will be, once: from the renewal sweep, a few days ahead, keyed
 * by the subscription. The billing contact hears rather than the company
 * when there is one, and the amount is the discounted one the invoice will
 * carry. If this regresses, a customer's first charge arrives unannounced,
 * or every daily sweep mails them again.
 */
static void
test_trial_reminder(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) p = venture_database_get(f->db, VENTURE_TYPE_PLAN_PRICE, f->price, NULL);
	g_autoptr(VentureEntity) contact = record(f, "contact");
	g_autoptr(VentureEntity) a = NULL;
	g_autoptr(VentureEntity) sub = NULL;
	g_autoptr(VentureEntity) reminder = NULL;
	g_autoptr(GPtrArray) sent = NULL;
	g_autofree gchar *to = NULL, *subject = NULL, *body = NULL, *key = NULL, *expected_key = NULL;
	gint64 id, half;
	(void)data;
	g_object_set(p, "trial-days", (gint64)14, NULL);
	save(f, p);
	customer_email(f, "accounts@customer.test");
	g_object_set(contact, "name", "Pat Payer", "email", "pat@customer.test",
		"company-id", f->company, NULL);
	save(f, contact);
	half = discount(f, integer(p, "plan-id"), "Half off", 50, NULL, 1);
	a = request(f, "start", 0, "2026-01-01");
	g_object_set(a, "company-id", f->company, "contact-id", venture_entity_get_id(contact),
		"plan-price-id", f->price, "seats", (gint64)2, "discount-id", half, NULL);
	save(f, a);
	id = integer(a, "subscription-id");
	g_clear_object(&a);

	/* Five days out is too early for a three-day reminder. */
	a = request(f, "renew-sweep", 0, "2026-01-10");
	save(f, a);
	g_clear_object(&a);
	sent = mail(f);
	g_assert_cmpuint(sent->len, ==, 0);
	g_clear_pointer(&sent, g_ptr_array_unref);

	/* Three days out, the first sweep queues it; later sweeps do not. */
	a = request(f, "renew-sweep", 0, "2026-01-12");
	save(f, a);
	g_clear_object(&a);
	a = request(f, "renew-sweep", 0, "2026-01-13");
	save(f, a);
	g_clear_object(&a);
	sent = mail(f);
	g_assert_cmpuint(sent->len, ==, 1);
	to = text(g_ptr_array_index(sent, 0), "to");
	subject = text(g_ptr_array_index(sent, 0), "subject");
	body = text(g_ptr_array_index(sent, 0), "text-body");
	key = text(g_ptr_array_index(sent, 0), "idempotency-key");
	g_assert_cmpstr(to, ==, "pat@customer.test");
	g_assert_cmpstr(subject, ==, "Your trial ends on 2026-01-15");
	g_assert_nonnull(strstr(body, "Your trial ends on 2026-01-15; your first invoice will be $30.00."));
	sub = subscription(f, id);
	expected_key = g_strdup_printf("trial-reminder:%s", venture_entity_get_uuid(sub));
	g_assert_cmpstr(key, ==, expected_key);
	reminder = venture_billing_service_trial_reminder(venture_billing_service_get(f->db), VENTURE_CUSTOMER_SUBSCRIPTION(sub));
	g_assert_nonnull(reminder);

	/* The trial ends and is invoiced as the reminder said; no second one. */
	a = request(f, "renew-sweep", 0, "2026-01-15");
	save(f, a);
	g_clear_pointer(&sent, g_ptr_array_unref);
	sent = mail(f);
	g_assert_cmpuint(sent->len, ==, 1);
	g_assert_cmpint(count(f, "invoice"), ==, 1);
}

/* A reminder setting of zero sends none, and a trial ending at renewal
 * is never invoiced, so it is not told about an invoice. */
static void
test_trial_reminder_off(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) p = venture_database_get(f->db, VENTURE_TYPE_PLAN_PRICE, f->price, NULL);
	g_autoptr(VentureEntity) a = NULL;
	g_autoptr(GPtrArray) sent = NULL;
	gint64 first, second;
	(void)data;
	g_object_set(p, "trial-days", (gint64)14, NULL);
	save(f, p);
	customer_email(f, "accounts@customer.test");
	first = start(f);
	second = start(f);
	a = request(f, "cancel", second, "2026-01-02");
	g_object_set(a, "at-period-end", TRUE, NULL);
	save(f, a);
	g_clear_object(&a);
	g_object_set(venture_billing_service_get(f->db), "trial-reminder-days", (gint64)0, NULL);
	a = request(f, "renew-sweep", 0, "2026-01-13");
	save(f, a);
	g_clear_object(&a);
	sent = mail(f);
	g_assert_cmpuint(sent->len, ==, 0);
	g_clear_pointer(&sent, g_ptr_array_unref);
	g_object_set(venture_billing_service_get(f->db), "trial-reminder-days", (gint64)3, NULL);
	a = request(f, "renew-sweep", 0, "2026-01-13");
	save(f, a);
	sent = mail(f);
	g_assert_cmpuint(sent->len, ==, 1);
	{
		g_autofree gchar *related = NULL;
		gint64 related_id = 0;
		g_object_get(g_ptr_array_index(sent, 0), "related-type", &related, "related-id", &related_id, NULL);
		g_assert_cmpstr(related, ==, "customer_subscription");
		g_assert_cmpint(related_id, ==, first);
	}
}

/* The subscription's change events, oldest first. */
static GPtrArray *
changes(Fixture *f, gint64 id)
{
	g_autoptr(VentureQuery) q = venture_query_new(VENTURE_TYPE_SUBSCRIPTION_EVENT);
	g_autoptr(GPtrArray) all = NULL;
	GPtrArray *found = g_ptr_array_new_with_free_func(g_object_unref);
	guint i;
	venture_query_set_limit(q, 0);
	venture_query_add_filter_int(q, "subscription-id", VENTURE_FILTER_OP_EQ, id, NULL);
	venture_query_add_order(q, "id", VENTURE_SORT_ASCENDING, NULL);
	all = venture_database_find(f->db, q, NULL);
	for (i = 0; i < all->len; i++)
	{
		gint kind = 0;
		g_object_get(g_ptr_array_index(all, i), "kind", &kind, NULL);
		if (kind >= 2 && kind <= 4)
			g_ptr_array_add(found, g_object_ref(g_ptr_array_index(all, i)));
	}
	return found;
}

/*
 * Changing what a customer pays -- seats now, another plan at renewal --
 * mails them what changed, when, and what the next invoice will be, once
 * per change, keyed by the change's event. The renewal that carries a
 * scheduled change out is not a second change. And the switch turns it
 * off. If this regresses, a customer finds a different amount on an
 * invoice with nothing to say why, or hears about one change twice.
 */
static void
test_price_change_notice(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) price = venture_database_get(f->db, VENTURE_TYPE_PLAN_PRICE, f->price, NULL);
	g_autoptr(VentureEntity) pro = record(f, "plan");
	g_autoptr(VentureEntity) pro_price = record(f, "plan_price");
	g_autoptr(VentureEntity) a = NULL;
	g_autoptr(GPtrArray) sent = NULL;
	g_autoptr(GPtrArray) events = NULL;
	gint64 id;
	guint i;
	(void)data;
	(void)price;
	customer_email(f, "accounts@customer.test");
	g_object_set(pro, "name", "Lightsite Pro", "code", "pro", "active", TRUE, NULL);
	save(f, pro);
	g_object_set(pro_price, "plan-id", venture_entity_get_id(pro), "currency", "USD", "active", TRUE, "per-seat", TRUE, NULL);
	field(pro_price, "amount", "60 USD");
	save(f, pro_price);
	id = start(f);
	sent = mail(f);
	g_assert_cmpuint(sent->len, ==, 0);
	g_clear_pointer(&sent, g_ptr_array_unref);

	/* A third seat on 17 January: $14.48 for the rest of January. */
	a = request(f, "change-seats", id, "2026-01-17");
	g_object_set(a, "seats", (gint64)3, NULL);
	save(f, a);
	g_clear_object(&a);
	/* Pro from renewal. */
	a = request(f, "change", id, "2026-01-20");
	g_object_set(a, "plan-price-id", venture_entity_get_id(pro_price), "at-period-end", TRUE, NULL);
	save(f, a);
	g_clear_object(&a);

	sent = mail(f);
	events = changes(f, id);
	g_assert_cmpuint(events->len, ==, 2);
	g_assert_cmpuint(sent->len, ==, 2);
	for (i = 0; i < 2; i++)
	{
		g_autofree gchar *key = text(g_ptr_array_index(sent, i), "idempotency-key");
		g_autofree gchar *expected = g_strdup_printf("price-change:%s", venture_entity_get_uuid(g_ptr_array_index(events, i)));
		g_autofree gchar *to = text(g_ptr_array_index(sent, i), "to");
		g_assert_cmpstr(key, ==, expected);
		g_assert_cmpstr(to, ==, "accounts@customer.test");
	}
	{
		g_autofree gchar *seats = text(g_ptr_array_index(sent, 0), "text-body");
		g_autofree gchar *plan = text(g_ptr_array_index(sent, 1), "text-body");
		g_autofree gchar *subject = text(g_ptr_array_index(sent, 1), "subject");
		g_assert_nonnull(strstr(seats, "from Lightsite Starter at $30.00 a month per seat, 2 seats to "
			"Lightsite Starter at $30.00 a month per seat, 3 seats"));
		g_assert_nonnull(strstr(seats, "This took effect on 2026-01-17."));
		g_assert_nonnull(strstr(seats, "Your next invoice, on 2026-02-01, will be $104.48, including $14.48"));
		g_assert_nonnull(strstr(plan, "to Lightsite Pro at $60.00 a month per seat, 3 seats"));
		g_assert_nonnull(strstr(plan, "This takes effect on 2026-02-01, when it renews"));
		/* Pro for three seats, plus the seat carried from January. */
		g_assert_nonnull(strstr(plan, "will be $194.48"));
		g_assert_cmpstr(subject, ==, "Your subscription changes at renewal");
	}
	g_clear_pointer(&sent, g_ptr_array_unref);

	/* Carrying the scheduled change out is not another change. */
	a = request(f, "renew-sweep", 0, "2026-02-01");
	save(f, a);
	g_clear_object(&a);
	sent = mail(f);
	g_assert_cmpuint(sent->len, ==, 2);
	g_clear_pointer(&sent, g_ptr_array_unref);

	/* Off is off. */
	g_object_set(venture_billing_service_get(f->db), "price-change-notices", FALSE, NULL);
	a = request(f, "change-seats", id, "2026-02-10");
	g_object_set(a, "seats", (gint64)4, NULL);
	save(f, a);
	sent = mail(f);
	g_assert_cmpuint(sent->len, ==, 2);
}

/*
 * A start may name a discount by the code the customer quoted, in any
 * case, and only the chosen plan's live offers answer to it; an unknown,
 * retired or expired code is refused saying which. If this regresses, a
 * code field on the form does nothing, or a code from another plan -- or
 * last season's -- is honoured.
 */
static void
test_discount_code(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) price = venture_database_get(f->db, VENTURE_TYPE_PLAN_PRICE, f->price, NULL);
	g_autoptr(VentureEntity) other = record(f, "plan");
	g_autoptr(VentureEntity) a = NULL;
	g_autoptr(VentureEntity) sub = NULL;
	gint64 plan = integer(price, "plan-id"), spring, retired, past, foreign;
	static const struct { const gchar *code; const gchar *refusal; } refused[] = {
		{ "NOPE", "the code NOPE is not one this plan offers" },
		{ "elsewhere", "the code elsewhere is not one this plan offers" },
		{ "old", "the code old is no longer offered" },
		{ "Past", "the code Past expired on 2025-12-01" },
	};
	guint i;
	(void)data;
	spring = discount(f, plan, "Spring", 25, NULL, 0);
	retired = discount(f, plan, "Old", 10, NULL, 0);
	past = discount(f, plan, "Past", 10, NULL, 0);
	g_object_set(other, "name", "Other", "code", "other", "active", TRUE, NULL);
	save(f, other);
	foreign = discount(f, venture_entity_get_id(other), "Elsewhere", 50, NULL, 0);
	{
		static const struct { const gchar *code; gboolean active; const gchar *ends; } codes[] = {
			{ "Spring25", TRUE, NULL }, { "OLD", FALSE, NULL }, { "PAST", TRUE, "2025-12-01" }, { "ELSEWHERE", TRUE, NULL },
		};
		gint64 ids[4];
		ids[0] = spring; ids[1] = retired; ids[2] = past; ids[3] = foreign;
		for (i = 0; i < G_N_ELEMENTS(codes); i++)
		{
			g_autoptr(VentureEntity) d = venture_database_get(f->db, VENTURE_TYPE_PLAN_DISCOUNT, ids[i], NULL);
			g_object_set(d, "code", codes[i].code, "active", codes[i].active, NULL);
			if (codes[i].ends != NULL)
				field(d, "ends-at", codes[i].ends);
			save(f, d);
		}
	}

	/* 25% off two $30 seats. */
	a = request(f, "start", 0, "2026-01-01");
	g_object_set(a, "company-id", f->company, "plan-price-id", f->price, "seats", (gint64)2,
		"discount-code", "  spring25 ", NULL);
	save(f, a);
	g_assert_cmpint(invoice_total(f, a), ==, 4500);
	sub = subscription(f, integer(a, "subscription-id"));
	g_assert_cmpint(integer(sub, "discount-id"), ==, spring);
	g_clear_object(&a);

	for (i = 0; i < G_N_ELEMENTS(refused); i++)
	{
		g_autoptr(GError) error = NULL;
		a = request(f, "start", 0, "2026-01-01");
		g_object_set(a, "company-id", f->company, "plan-price-id", f->price, "discount-code", refused[i].code, NULL);
		g_assert_false(venture_database_save(f->db, a, NULL, &error));
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
		g_assert_nonnull(strstr(error->message, refused[i].refusal));
		g_clear_object(&a);
	}
	{
		g_autoptr(GError) error = NULL;
		a = request(f, "start", 0, "2026-01-01");
		g_object_set(a, "company-id", f->company, "plan-price-id", f->price, "discount-code", "spring25",
			"discount-id", spring, NULL);
		g_assert_false(venture_database_save(f->db, a, NULL, &error));
		g_assert_nonnull(strstr(error->message, "not both"));
	}
	g_assert_cmpint(count(f, "customer_subscription"), ==, 1);
}

/*
 * Starting a subscription on a price with a free trial can skip the
 * trial and bill now: the first invoice is issued on the day. If this
 * regresses, a customer who wanted to pay is given a trial anyway.
 */
static void
test_skip_trial(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) p = venture_database_get(f->db, VENTURE_TYPE_PLAN_PRICE, f->price, NULL);
	g_autoptr(VentureEntity) a = NULL;

	(void)data;
	g_object_set(p, "trial-days", (gint64)14, NULL);
	save(f, p);
	a = request(f, "start", 0, "2026-01-01");
	g_object_set(a, "company-id", f->company, "plan-price-id", f->price, "seats", (gint64)1,
		"skip-trial", TRUE, NULL);
	save(f, a);
	status_is(f, integer(a, "subscription-id"), "active");
	g_assert_cmpint(count(f, "invoice"), ==, 1);
	g_assert_cmpint(invoice_total(f, a), ==, 3000);
}

/*
 * The next invoice quoted to a customer -- in the trial reminder, the
 * price-change notice and the portal -- includes the price's tax, as the
 * invoice will, and leaves it off for an exempt customer. If this
 * regresses, the email promises $30 and the invoice asks for $33.
 */
static void
test_next_invoice_tax(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) code = record(f, "tax_code");
	g_autoptr(VentureEntity) p = venture_database_get(f->db, VENTURE_TYPE_PLAN_PRICE, f->price, NULL);
	g_autoptr(VentureEntity) customer = NULL;
	g_autoptr(VentureEntity) s = NULL;
	g_autoptr(VentureMoney) next = NULL;
	g_autoptr(GError) error = NULL;
	gint64 id;

	(void)data;
	g_object_set(code, "code", "STD", "name", "Standard", "rate-numerator", (gint64)10,
		"rate-denominator", (gint64)100, "active", TRUE, NULL);
	save(f, code);
	g_object_set(p, "tax-code-id", venture_entity_get_id(code), NULL);
	save(f, p);

	id = start(f);
	s = subscription(f, id);
	next = venture_billing_service_next_invoice(venture_billing_service_get(f->db),
		VENTURE_CUSTOMER_SUBSCRIPTION(s), &error);
	g_assert_no_error(error);
	/* Two seats at $30 plus 10%. */
	g_assert_cmpint(venture_money_get_amount(next), ==, 6600);

	customer = venture_database_get(f->db, VENTURE_TYPE_COMPANY, f->company, NULL);
	g_object_set(customer, "tax-exempt", TRUE, NULL);
	save(f, customer);
	g_clear_pointer(&next, venture_money_free);
	next = venture_billing_service_next_invoice(venture_billing_service_get(f->db),
		VENTURE_CUSTOMER_SUBSCRIPTION(s), &error);
	g_assert_no_error(error);
	g_assert_cmpint(venture_money_get_amount(next), ==, 6000);
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
	{
		gint64 restored_org = venture_test_accounting_roundtrip(f->db, f->org);
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_BILLING_NOTICE);
		g_autoptr(VentureEntity) notice = NULL;
		g_autoptr(GError) error = NULL;
		g_autoptr(GDateTime) past_due = NULL;
		g_autofree gchar *stamp = NULL, *expected = NULL, *key = NULL;
		gint64 subscription_id = 0, step_id = 0;
		venture_query_set_organization(query, restored_org);
		notice = venture_database_find_one(f->db, query, &error);
		g_assert_no_error(error);
		g_assert_nonnull(notice);
		g_object_get(notice, "subscription-id", &subscription_id, "dunning-step-id", &step_id,
			"past-due-at", &past_due, "delivery-key", &key, NULL);
		stamp = g_date_time_format_iso8601(past_due);
		expected = g_strdup_printf("%" G_GINT64_FORMAT ":%" G_GINT64_FORMAT ":%s", subscription_id, step_id, stamp);
		g_assert_cmpstr(key, ==, expected);
	}
}

static void
test_dunning_sequence(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) cancel = record(f, "dunning_step");
	g_autoptr(VentureEntity) pause = record(f, "dunning_step");
	g_autoptr(VentureEntity) notice = record(f, "dunning_step");
	g_autoptr(VentureEntity) a = NULL;
	gint64 id = start(f);
	gint64 late;
	/* Configuration insertion order must not change escalation order. */
	g_object_set(cancel, "day-offset", (gint64)7, "active", TRUE, NULL);
	field(cancel, "action", "cancel");
	save(f, cancel);
	g_object_set(pause, "day-offset", (gint64)3, "active", TRUE, NULL);
	field(pause, "action", "pause");
	save(f, pause);
	g_object_set(notice, "day-offset", (gint64)1, "active", TRUE, NULL);
	save(f, notice);
	a = request(f, "mark-payment-failed", id, "2026-01-02");
	save(f, a);
	g_clear_object(&a);
	a = request(f, "dunning-sweep", 0, "2026-01-05");
	save(f, a);
	status_is(f, id, "paused");
	g_assert_cmpint(count(f, "billing_notice"), ==, 2);
	g_clear_object(&a);
	a = request(f, "dunning-sweep", 0, "2026-01-09");
	save(f, a);
	status_is(f, id, "cancelled");
	g_assert_cmpint(count(f, "billing_notice"), ==, 3);
	late = start(f);
	g_clear_object(&a);
	a = request(f, "mark-payment-failed", late, "2026-01-02");
	save(f, a);
	g_clear_object(&a);
	a = request(f, "dunning-sweep", 0, "2026-01-09");
	save(f, a);
	status_is(f, late, "cancelled");
	g_assert_cmpint(count(f, "billing_notice"), ==, 6);
	g_clear_object(&a);
	a = request(f, "dunning-sweep", 0, "2026-01-09");
	save(f, a);
	g_assert_cmpint(count(f, "billing_notice"), ==, 6);
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
	g_assert_cmpint(count(f, "invoice"), ==, 1);
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

static void
test_late_sweep(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) a = NULL;
	guint i;
	start(f);
	for (i = 0; i < 2; i++)
	{
		g_clear_object(&a);
		a = request(f, "renew-sweep", 0, "2026-04-01");
		save(f, a);
		g_assert_cmpint(count(f, "invoice"), ==, 4);
	}
}

static void
test_dry_run_no_writes(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) a = NULL;
	gint64 audits;
	gint64 requests;
	start(f);
	audits = count(f, "audit_entry");
	requests = count(f, "billing_request");
	a = request(f, "renew-sweep", 0, "2026-02-01");
	g_object_set(a, "dry-run", TRUE, NULL);
	save(f, a);
	g_assert_cmpint(integer(a, "processed"), ==, 1);
	g_assert_cmpint(count(f, "audit_entry"), ==, audits);
	g_assert_cmpint(count(f, "billing_request"), ==, requests);
}

static void
test_price_history(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) p = NULL;
	g_autoptr(GError) error = NULL;
	start(f);
	p = venture_database_get(f->db, VENTURE_TYPE_PLAN_PRICE, f->price, NULL);
	field(p, "amount", "100 USD");
	g_assert_false(venture_database_save(f->db, p, NULL, &error));
	g_assert_nonnull(g_strstr_len(error->message, -1, "VentureBillingService"));
}

static void
test_contact_scope(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) other = record(f, "company");
	g_autoptr(VentureEntity) contact = record(f, "contact");
	g_autoptr(VentureEntity) a = request(f, "start", 0, "2026-01-01");
	g_autoptr(GError) error = NULL;
	g_object_set(other, "name", "Different customer", NULL);
	save(f, other);
	g_object_set(contact, "name", "Other customer's contact", "company-id", venture_entity_get_id(other), NULL);
	save(f, contact);
	g_object_set(a, "company-id", f->company, "contact-id", venture_entity_get_id(contact),
		"plan-price-id", f->price, NULL);
	g_assert_false(venture_database_save(f->db, a, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_cmpint(count(f, "customer_subscription"), ==, 0);
}

static void
test_churn_rates(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) a = NULL;
	g_autoptr(VentureReportResult) r = NULL;
	GPtrArray *metrics;
	guint i;
	guint found = 0;
	gint64 id = start(f);
	a = request(f, "cancel", id, "2026-02-15");
	save(f, a);
	r = report(f, "churn", "2026-02");
	metrics = venture_report_result_get_metrics(r);
	for (i = 0; i < metrics->len; i++)
	{
		VentureMetric *m = g_ptr_array_index(metrics, i);
		if (g_strcmp0(venture_metric_get_key(m), "logo_churn_bps") == 0 ||
			g_strcmp0(venture_metric_get_key(m), "revenue_churn_bps") == 0)
		{
			g_assert_cmpfloat(venture_metric_get_number(m), ==, 10000.0);
			found++;
		}
	}
	g_assert_cmpuint(found, ==, 2);
}

static void
test_initial_invoice(Fixture *f, gconstpointer data)
{
	start(f);
	g_assert_cmpint(count(f, "invoice"), ==, 1);
}

static void
test_migration(Fixture *f, gconstpointer data)
{
	g_autoptr(OrmResult) result = venture_database_query_raw(f->db,
		"SELECT CAST(COUNT(*) AS BIGINT) FROM schema_migrations WHERE version = 170", NULL, NULL);
	g_assert_true(orm_result_next(result));
	g_assert_cmpint(orm_row_get_integer(orm_result_get_row(result), 0), ==, 1);
}

static void
test_proration_billed(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) a = NULL;
	g_autoptr(VentureMoney) balance = NULL;
	gint64 id = start(f);
	a = request(f, "change-seats", id, "2026-01-17");
	g_object_set(a, "seats", (gint64)3, NULL);
	save(f, a);
	g_clear_object(&a);
	a = request(f, "renew", id, "2026-02-01");
	save(f, a);
	balance = venture_settlement_service_invoice_balance(venture_settlement_service_get(f->db), integer(a, "invoice-id"), NULL, NULL);
	g_assert_cmpint(venture_money_get_amount(balance), ==, 10448);
}

static void
test_proration_credit(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) a = NULL;
	g_autoptr(VentureMoney) balance = NULL;
	gint64 id = start(f);
	a = request(f, "change-seats", id, "2026-01-17");
	g_object_set(a, "seats", (gint64)1, NULL);
	save(f, a);
	g_clear_object(&a);
	a = request(f, "renew", id, "2026-02-01");
	save(f, a);
	balance = venture_settlement_service_invoice_balance(venture_settlement_service_get(f->db), integer(a, "invoice-id"), NULL, NULL);
	g_assert_cmpint(venture_money_get_amount(balance), ==, 1552);
	g_assert_cmpint(count(f, "customer_credit"), ==, 1);
}

/* A downgrade credit keeps the higher precision of the old price; comparing
 * raw coefficients would overallocate the new invoice and reject renewal. */
static void
test_proration_credit_precision(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) old = venture_database_get(f->db, VENTURE_TYPE_PLAN_PRICE, f->price, NULL);
	g_autoptr(VentureEntity) price = record(f, "plan_price");
	g_autoptr(VentureEntity) action = NULL;
	g_autoptr(VentureMoney) precise = venture_money_new(31000, "USD", 3);
	g_autoptr(VentureMoney) balance = NULL;
	gint64 id;
	g_object_set(old, "amount", precise, NULL);
	save(f, old);
	g_object_set(price, "plan-id", integer(old, "plan-id"), "currency", "USD", "active", TRUE, "per-seat", TRUE, NULL);
	field(price, "amount", "20 USD");
	save(f, price);
	id = start(f);
	action = request(f, "change", id, "2026-01-17");
	g_object_set(action, "plan-price-id", venture_entity_get_id(price), NULL);
	save(f, action);
	g_clear_object(&action);
	action = request(f, "renew", id, "2026-02-01");
	save(f, action);
	balance = venture_settlement_service_invoice_balance(venture_settlement_service_get(f->db), integer(action, "invoice-id"), NULL, NULL);
	g_assert_nonnull(balance);
	g_assert_cmpint(venture_money_get_amount(balance), ==, 29360);
	g_assert_cmpint(venture_money_get_exponent(balance), ==, 3);
	g_assert_cmpint(count(f, "customer_credit"), ==, 1);
}

static void
test_month_end(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) a = request(f, "start", 0, "2026-01-31");
	g_autoptr(VentureEntity) s = NULL;
	g_autoptr(GDateTime) end = NULL;
	g_autofree gchar *text = NULL;
	gint64 id;
	g_object_set(a, "company-id", f->company, "plan-price-id", f->price, NULL);
	save(f, a);
	id = integer(a, "subscription-id");
	g_clear_object(&a);
	a = request(f, "renew", id, "2026-02-28");
	save(f, a);
	s = subscription(f, id);
	g_object_get(s, "current-period-end", &end, NULL);
	text = g_date_time_format(end, "%F");
	g_assert_cmpstr(text, ==, "2026-03-31");
}

static void
test_documentation(void)
{
	static const gchar *const files[] = { "docs/billing.org", "docs/cli.org", "docs/api.org", "skills/venturectl/SKILL.md" };
	guint i;
	for (i = 0; i < G_N_ELEMENTS(files); i++)
	{
		g_autofree gchar *text = NULL;
		g_assert_true(g_file_get_contents(files[i], &text, NULL, NULL));
		g_assert_nonnull(g_strstr_len(text, -1, "billing"));
		g_assert_nonnull(g_strstr_len(text, -1, "customer_subscription"));
	}
}

static void
test_closed_period(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureContext) context = venture_context_new(config, f->db);
	g_autoptr(GDateTime) begin = venture_time_from_string("2026-01-01", NULL);
	g_autoptr(VentureFiscalYear) year = NULL;
	g_autoptr(VentureQuery) q = venture_query_new(VENTURE_TYPE_FISCAL_PERIOD);
	g_autoptr(GPtrArray) periods = NULL;
	g_autoptr(VentureEntity) a = NULL;
	g_autoptr(GError) error = NULL;
	VentureActor actor;
	gint64 id = start(f);
	guint i;
	actor.kind = VENTURE_ACTOR_KIND_USER;
	actor.name = "owner";
	actor.prompt = NULL;
	actor.request_id = NULL;
	actor.approved_by = NULL;
	year = venture_period_service_generate(venture_period_service_get(f->db), f->org, "FY2026", begin, VENTURE_PERIOD_MONTHLY, &actor, &error);
	g_assert_no_error(error);
	g_assert_nonnull(year);
	venture_query_set_organization(q, f->org);
	g_assert_true(venture_query_add_order(q, "start-at", VENTURE_SORT_ASCENDING, &error));
	periods = venture_database_find(f->db, q, &error);
	g_assert_no_error(error);
	for (i = 0; i < 2; i++)
	{
		VentureEntity *p = g_ptr_array_index(periods, i);
		g_object_set(p, "state", VENTURE_PERIOD_CLOSED, NULL);
		g_assert_true(venture_database_save(f->db, p, &actor, &error));
		g_assert_no_error(error);
	}
	a = request(f, "renew", id, "2026-02-01");
	g_assert_false(venture_database_save(f->db, a, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
	g_assert_cmpint(count(f, "invoice"), ==, 1);
	g_assert_cmpint(count(f, "subscription_event"), ==, 1);
}

static void
test_currency(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) original = venture_database_get(f->db, VENTURE_TYPE_PLAN_PRICE, f->price, NULL);
	g_autoptr(VentureEntity) price = record(f, "plan_price");
	g_autoptr(VentureEntity) a = NULL;
	g_autoptr(VentureEntity) s = NULL;
	g_autoptr(GError) error = NULL;
	gint64 id = start(f);
	g_object_set(price, "plan-id", integer(original, "plan-id"), "currency", "EUR", "active", TRUE, NULL);
	field(price, "amount", "60 USD");
	g_assert_false(venture_database_save(f->db, price, NULL, &error));
	g_assert_nonnull(error);
	g_clear_error(&error);
	field(price, "amount", "60 EUR");
	save(f, price);
	a = request(f, "change", id, "2026-01-17");
	g_object_set(a, "plan-price-id", venture_entity_get_id(price), NULL);
	g_assert_false(venture_database_save(f->db, a, NULL, &error));
	g_assert_nonnull(error);
	s = subscription(f, id);
	g_assert_cmpint(integer(s, "plan-price-id"), ==, f->price);
	g_assert_cmpint(count(f, "invoice"), ==, 1);
}

static void
test_yearly(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) p = venture_database_get(f->db, VENTURE_TYPE_PLAN_PRICE, f->price, NULL);
	g_autoptr(VentureReportResult) r = NULL;
	field(p, "interval", "year");
	field(p, "amount", "120 USD");
	save(f, p);
	start(f);
	r = report(f, "mrr", "2026-01");
	g_assert_cmpint(metric_amount(r, "mrr"), ==, 2000);
	g_assert_cmpint(metric_amount(r, "arr"), ==, 24000);
}

static void
test_scheduled_change_cancel(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) original = venture_database_get(f->db, VENTURE_TYPE_PLAN_PRICE, f->price, NULL);
	g_autoptr(VentureEntity) p = record(f, "plan_price");
	g_autoptr(VentureEntity) a = NULL;
	g_autoptr(VentureEntity) s = NULL;
	g_autoptr(VentureReportResult) r = NULL;
	gint64 id = start(f);
	g_object_set(p, "plan-id", integer(original, "plan-id"), "currency", "USD", "active", TRUE, "per-seat", TRUE, NULL);
	field(p, "amount", "60 USD");
	save(f, p);
	a = request(f, "change", id, "2026-01-17");
	g_object_set(a, "plan-price-id", venture_entity_get_id(p), "at-period-end", TRUE, NULL);
	save(f, a);
	s = subscription(f, id);
	g_assert_cmpint(integer(s, "plan-price-id"), ==, f->price);
	g_assert_cmpint(integer(s, "pending-plan-price-id"), ==, venture_entity_get_id(p));
	g_clear_object(&a);
	a = request(f, "renew", id, "2026-02-01");
	save(f, a);
	g_clear_object(&s);
	s = subscription(f, id);
	g_assert_cmpint(integer(s, "plan-price-id"), ==, venture_entity_get_id(p));
	g_assert_cmpint(integer(s, "pending-plan-price-id"), ==, 0);
	g_clear_object(&a);
	a = request(f, "cancel", id, "2026-02-15");
	g_object_set(a, "at-period-end", TRUE, NULL);
	save(f, a);
	status_is(f, id, "active");
	g_clear_object(&a);
	a = request(f, "renew-sweep", 0, "2026-03-01");
	save(f, a);
	status_is(f, id, "cancelled");
	g_assert_cmpint(count(f, "invoice"), ==, 2);
	r = report(f, "mrr", "2026-01");
	g_assert_cmpint(metric_amount(r, "mrr"), ==, 6000);
}

static void
test_scheduled_cancel_past_due(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) a = NULL;
	g_autoptr(VentureReportResult) r = NULL;
	gint64 id = start(f);
	a = request(f, "mark-payment-failed", id, "2026-01-17");
	save(f, a);
	g_clear_object(&a);
	a = request(f, "cancel", id, "2026-01-20");
	g_object_set(a, "at-period-end", TRUE, NULL);
	save(f, a);
	g_clear_object(&a);
	a = request(f, "renew-sweep", 0, "2026-04-01");
	save(f, a);
	status_is(f, id, "cancelled");
	r = report(f, "mrr", "2026-02");
	g_assert_cmpint(metric_amount(r, "mrr"), ==, 0);
}

static void
test_sweep_scope(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) a = NULL;
	g_autoptr(GError) error = NULL;
	start(f);
	a = request(f, "renew-sweep", 0, "2026-02-01");
	venture_entity_set_organization_id(a, 0);
	g_assert_false(venture_database_save(f->db, a, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_cmpint(count(f, "invoice"), ==, 1);
}

static void
test_interval_changes(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) monthly = venture_database_get(f->db, VENTURE_TYPE_PLAN_PRICE, f->price, NULL);
	g_autoptr(VentureEntity) annual = record(f, "plan_price");
	g_autoptr(VentureEntity) a = NULL;
	g_autoptr(VentureMoney) balance = NULL;
	gint64 id;
	g_object_set(annual, "plan-id", integer(monthly, "plan-id"), "currency", "USD", "active", TRUE, "per-seat", TRUE, NULL);
	field(annual, "interval", "year");
	field(annual, "amount", "300 USD");
	save(f, annual);
	id = start(f);
	a = request(f, "change", id, "2026-01-17");
	g_object_set(a, "plan-price-id", venture_entity_get_id(annual), NULL);
	save(f, a);
	g_clear_object(&a);
	a = request(f, "change", id, "2026-01-18");
	g_object_set(a, "plan-price-id", f->price, NULL);
	save(f, a);
	g_clear_object(&a);
	a = request(f, "renew", id, "2026-02-01");
	save(f, a);
	balance = venture_settlement_service_invoice_balance(venture_settlement_service_get(f->db), integer(a, "invoice-id"), NULL, NULL);
	g_assert_cmpint(venture_money_get_amount(balance), ==, 5968);
}

static void
test_upgrade_disabled_restart(void)
{
	g_autofree gchar *directory = g_dir_make_tmp("venture-billing-upgrade-XXXXXX", NULL);
	g_autofree gchar *uri = g_strdup_printf("sqlite://%s/books.db", directory);
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureContext) context = NULL;
	g_autoptr(VentureDatabase) db = NULL;
	g_autoptr(VentureCompany) company = venture_company_new();
	g_autoptr(GError) error = NULL;
	gint64 id;
	guint i;
	venture_config_set_module_enabled(config, "billing", FALSE);
	db = venture_database_new(uri, &error);
	g_assert_no_error(error);
	context = venture_context_new(config, db);
	g_assert_true(venture_database_migrate(db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	g_assert_cmpuint(venture_entity_registry_lookup(venture_entity_registry_get_default(), "customer_subscription"), ==, G_TYPE_INVALID);
	g_assert_null(venture_report_registry_lookup(venture_context_get_report_registry(context), "mrr"));
	g_object_set(company, "name", "Pre-billing Lightsite customer", "organization-id", (gint64)1, NULL);
	g_assert_true(venture_database_save(db, VENTURE_ENTITY(company), NULL, &error));
	g_assert_no_error(error);
	id = venture_entity_get_id(VENTURE_ENTITY(company));
	/* Simulate the pre-feature script history while retaining real core rows. */
	g_assert_true(venture_database_execute(db, "DELETE FROM schema_migrations WHERE version >= 170", NULL, &error));
	g_assert_no_error(error);
	venture_config_set_module_enabled(config, "billing", TRUE);
	g_clear_object(&context);
	g_clear_object(&db);
	for (i = 0; i < 2; i++)
	{
		g_autoptr(VentureEntity) saved = NULL;
		g_autofree gchar *name = NULL;
		db = venture_database_new(uri, &error);
		g_assert_no_error(error);
		context = venture_context_new(config, db);
		g_assert_true(venture_database_migrate(db, venture_entity_registry_get_default(), &error));
		g_assert_no_error(error);
		saved = venture_database_get(db, VENTURE_TYPE_COMPANY, id, &error);
		g_assert_no_error(error);
		g_object_get(saved, "name", &name, NULL);
		g_assert_cmpstr(name, ==, "Pre-billing Lightsite customer");
		g_assert_nonnull(venture_report_registry_lookup(venture_context_get_report_registry(context), "mrr"));
		g_clear_object(&context);
		g_clear_object(&db);
	}
	venture_test_remove_tree(directory);
}

static void
test_uniqueness(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) duplicate = record(f, "plan");
	g_autoptr(VentureEntity) org = record(f, "organization");
	g_autoptr(VentureEntity) first = request(f, "start", 0, "2026-01-01");
	g_autoptr(VentureEntity) second = request(f, "start", 0, "2026-01-01");
	g_autoptr(GError) error = NULL;
	g_object_set(duplicate, "name", "Duplicate Starter", "code", "starter", NULL);
	g_assert_false(venture_database_save(f->db, duplicate, NULL, &error));
	g_assert_nonnull(error);
	g_clear_error(&error);
	g_object_set(org, "name", "Another organization", NULL);
	save(f, org);
	venture_entity_set_organization_id(duplicate, venture_entity_get_id(org));
	save(f, duplicate);
	g_object_set(first, "company-id", f->company, "plan-price-id", f->price, "external-id", "lightsite-42", NULL);
	save(f, first);
	g_object_set(second, "company-id", f->company, "plan-price-id", f->price, "external-id", "lightsite-42", NULL);
	g_assert_false(venture_database_save(f->db, second, NULL, &error));
	g_assert_nonnull(error);
	g_assert_cmpint(count(f, "customer_subscription"), ==, 1);
	g_assert_cmpint(count(f, "invoice"), ==, 1);
}

/* Collection uses an authorized method, then recover exits dunning. */
static void
test_collect_and_recover(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) product = record(f, "product");
	g_autoptr(VentureEntity) method = record(f, "customer_payment_method");
	g_autoptr(VentureEntity) fail = NULL;
	g_autoptr(VentureEntity) collect = NULL;
	g_autoptr(VentureEntity) price = NULL;
	g_autoptr(VentureQuery) lines = NULL;
	g_autoptr(GPtrArray) found = NULL;
	g_autoptr(VentureMoney) balance = NULL;
	gint64 id;
	gint64 product_id;
	(void)data;
	g_object_set(product, "name", "Starter seats", NULL);
	save(f, product);
	product_id = venture_entity_get_id(product);
	price = venture_database_get(f->db, VENTURE_TYPE_PLAN_PRICE, f->price, NULL);
	g_object_set(price, "product-id", product_id, NULL);
	save(f, price);
	id = start(f);
	lines = venture_query_new(VENTURE_TYPE_INVOICE_LINE);
	found = venture_database_find(f->db, lines, NULL);
	g_assert_cmpuint(found->len, ==, 1);
	g_assert_cmpint(integer(g_ptr_array_index(found, 0), "product-id"), ==, product_id);
	g_object_set(method, "company-id", f->company, "method", "manual", "authorized", TRUE, NULL);
	save(f, method);
	fail = request(f, "mark-payment-failed", id, "2026-01-05");
	save(f, fail);
	status_is(f, id, "past_due");
	collect = request(f, "collect", id, "2026-01-06");
	/* An authorized card alone must never manufacture a cash receipt. */
	g_object_set(method, "method", "card", NULL);
	save(f, method);
	{
		g_autoptr(GError) error = NULL;
		g_assert_false(venture_database_save(f->db, collect, NULL, &error));
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
		status_is(f, id, "past_due");
	}
	g_clear_object(&collect);
	collect = request(f, "collect", id, "2026-01-06");
	g_object_set(method, "method", "manual", NULL);
	save(f, method);
	save(f, collect);
	status_is(f, id, "active");
	balance = venture_settlement_service_invoice_balance(venture_settlement_service_get(f->db),
		integer(collect, "invoice-id"), NULL, NULL);
	g_assert_cmpint(venture_money_get_amount(balance), ==, 0);
}

static gint64
money_field(VentureEntity *e, const gchar *name)
{
	g_autoptr(VentureMoney) value = NULL;
	g_object_get(e, name, &value, NULL);
	return value != NULL ? venture_money_get_amount(value) : 0;
}

static VentureEntity *
last_of(Fixture *f, const gchar *name)
{
	g_autoptr(VentureQuery) q = venture_query_new(venture_entity_registry_lookup(venture_entity_registry_get_default(), name));
	g_autoptr(GPtrArray) rows = NULL;
	venture_query_add_order(q, "id", VENTURE_SORT_DESCENDING, NULL);
	venture_query_set_limit(q, 1);
	rows = venture_database_find(f->db, q, NULL);
	g_assert_nonnull(rows);
	g_assert_cmpuint(rows->len, ==, 1);
	return g_object_ref(g_ptr_array_index(rows, 0));
}

static gint64
tax_rate(Fixture *f, gint64 numerator, gint64 denominator)
{
	g_autoptr(VentureEntity) code = record(f, "tax_code");
	g_object_set(code, "code", "STD", "name", "Standard", "rate-numerator", numerator,
		"rate-denominator", denominator, "active", TRUE, NULL);
	save(f, code);
	return venture_entity_get_id(code);
}

/*
 * Cancelling now credits the unused days of the invoiced period, spread
 * over the period's days exactly as a downgrade is: $60 for January,
 * cancelled on the 17th, is 1 day at $1.94 and 14 at $1.93 = $28.96
 * back, applied to the unpaid invoice. The preview the page shows is the
 * same figure. A trial and a cancellation at renewal give nothing. If this
 * regresses, a customer who leaves mid-month pays for days they never get,
 * or the button promises a different amount than the books record.
 */
static void
test_cancel_credit(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) a = NULL;
	g_autoptr(VentureEntity) s = NULL;
	g_autoptr(VentureEntity) event = NULL;
	g_autoptr(VentureEntity) p = NULL;
	g_autoptr(VentureMoney) preview = NULL;
	g_autoptr(VentureMoney) balance = NULL;
	g_autoptr(GDateTime) when = g_date_time_new_utc(2026, 1, 17, 0, 0, 0);
	g_autoptr(GError) error = NULL;
	gint64 id = start(f), first_invoice, days = 0;

	(void)data;
	s = subscription(f, id);
	preview = venture_billing_service_cancel_credit(venture_billing_service_get(f->db),
		VENTURE_CUSTOMER_SUBSCRIPTION(s), when, &days, &error);
	g_assert_no_error(error);
	g_assert_cmpint(venture_money_get_amount(preview), ==, 2896);
	g_assert_cmpint(days, ==, 15);
	event = last_of(f, "subscription_event");
	first_invoice = integer(event, "invoice-id");
	g_clear_object(&event);

	a = request(f, "cancel", id, "2026-01-17");
	save(f, a);
	status_is(f, id, "cancelled");
	g_assert_cmpint(money_field(a, "proration-amount"), ==, -2896);
	event = last_of(f, "subscription_event");
	g_assert_cmpint(money_field(event, "proration-amount"), ==, -2896);
	g_assert_cmpint(count(f, "customer_credit"), ==, 1);
	balance = venture_settlement_service_invoice_balance(venture_settlement_service_get(f->db), first_invoice, NULL, NULL);
	g_assert_cmpint(venture_money_get_amount(balance), ==, 6000 - 2896);
	g_clear_object(&a);

	/* Scheduled for renewal: nothing more billed, nothing given back. */
	id = start(f);
	a = request(f, "cancel", id, "2026-01-17");
	g_object_set(a, "at-period-end", TRUE, NULL);
	save(f, a);
	g_assert_cmpint(money_field(a, "proration-amount"), ==, 0);
	g_assert_cmpint(count(f, "customer_credit"), ==, 1);
	g_clear_object(&a);

	/* A trial was never invoiced, so there is nothing to credit. */
	g_clear_object(&s);
	s = venture_database_get(f->db, VENTURE_TYPE_PLAN_PRICE, f->price, NULL);
	p = record(f, "plan_price");
	g_object_set(p, "plan-id", integer(s, "plan-id"), "currency", "USD", "active", TRUE, "trial-days", (gint64)14, NULL);
	field(p, "amount", "30 USD");
	save(f, p);
	g_clear_object(&a);
	a = request(f, "start", 0, "2026-01-01");
	g_object_set(a, "company-id", f->company, "plan-price-id", venture_entity_get_id(p), NULL);
	save(f, a);
	id = integer(a, "subscription-id");
	g_clear_object(&a);
	status_is(f, id, "trialing");
	a = request(f, "cancel", id, "2026-01-05");
	save(f, a);
	g_assert_cmpint(money_field(a, "proration-amount"), ==, 0);
	g_assert_cmpint(count(f, "customer_credit"), ==, 1);
}

/*
 * A price's tax rate reaches the invoice line, so settlement taxes it
 * exactly -- 10% on $60 is $66 owed -- and an exempt customer stays
 * exempt. Cancelling credits the tax on the unused days too. If this
 * regresses, subscription revenue is invoiced untaxed and the return is
 * short, or a charity is charged tax.
 */
static void
test_price_tax(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) p = venture_database_get(f->db, VENTURE_TYPE_PLAN_PRICE, f->price, NULL);
	g_autoptr(VentureEntity) exempt = record(f, "company");
	g_autoptr(VentureEntity) a = NULL;
	g_autoptr(VentureEntity) line = NULL;
	g_autoptr(VentureEntity) credit = NULL;
	gint64 code = tax_rate(f, 10, 100), id;

	(void)data;
	{
		g_autoptr(OrmResult) result = venture_database_query_raw(f->db,
			"SELECT CAST(COUNT(*) AS BIGINT) FROM schema_migrations WHERE version = 650", NULL, NULL);
		g_assert_true(orm_result_next(result));
		g_assert_cmpint(orm_row_get_integer(orm_result_get_row(result), 0), ==, 1);
	}
	g_object_set(p, "tax-code-id", code, NULL);
	save(f, p);
	id = start(f);
	line = last_of(f, "invoice_line");
	g_assert_cmpint(integer(line, "tax-code-id"), ==, code);
	g_assert_cmpint(money_field(line, "tax-amount"), ==, 600);
	a = last_of(f, "subscription_event");
	g_assert_cmpint(invoice_total(f, a), ==, 6600);
	g_clear_object(&a);

	a = request(f, "cancel", id, "2026-01-17");
	save(f, a);
	/* $28.96 unused plus 10% of it, half-even: $2.90. */
	g_assert_cmpint(money_field(a, "proration-amount"), ==, -3186);
	credit = last_of(f, "customer_credit");
	g_assert_cmpint(money_field(credit, "tax-amount"), ==, 290);
	g_clear_object(&a);

	g_object_set(exempt, "name", "Charity", "tax-exempt", TRUE, "tax-exempt-reason", "501(c)(3)", NULL);
	save(f, exempt);
	a = request(f, "start", 0, "2026-01-01");
	g_object_set(a, "company-id", venture_entity_get_id(exempt), "plan-price-id", f->price, "seats", (gint64)2, NULL);
	save(f, a);
	g_assert_cmpint(invoice_total(f, a), ==, 6000);
}

/*
 * MRR is what the period is billed at: 50% off the first period makes
 * January's MRR $30, and it returns to $60 at the renewal the discount no
 * longer covers -- an expansion, not a new customer. An amount off a
 * yearly price is taken per year, then divided by twelve. If this
 * regresses, MRR overstates revenue for every discounted customer.
 */
static void
test_mrr_discount(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) p = venture_database_get(f->db, VENTURE_TYPE_PLAN_PRICE, f->price, NULL);
	g_autoptr(VentureEntity) yearly = record(f, "plan_price");
	g_autoptr(VentureEntity) a = NULL;
	g_autoptr(VentureReportResult) r = NULL;
	gint64 plan = integer(p, "plan-id"), half, flat, id;

	(void)data;
	half = discount(f, plan, "Half off", 50, NULL, 1);
	a = request(f, "start", 0, "2026-01-01");
	g_object_set(a, "company-id", f->company, "plan-price-id", f->price, "seats", (gint64)2, "discount-id", half, NULL);
	save(f, a);
	id = integer(a, "subscription-id");
	g_clear_object(&a);
	a = last_of(f, "subscription_event");
	g_assert_cmpint(money_field(a, "to-mrr"), ==, 3000);
	g_clear_object(&a);
	a = request(f, "renew", id, "2026-02-01");
	save(f, a);
	g_assert_cmpint(invoice_total(f, a), ==, 6000);
	g_clear_object(&a);
	a = last_of(f, "subscription_event");
	g_assert_cmpint(money_field(a, "from-mrr"), ==, 3000);
	g_assert_cmpint(money_field(a, "to-mrr"), ==, 6000);
	g_clear_object(&a);

	g_object_set(yearly, "plan-id", plan, "currency", "USD", "active", TRUE, NULL);
	field(yearly, "interval", "year");
	field(yearly, "amount", "600 USD");
	save(f, yearly);
	flat = discount(f, plan, "Loyal", 0, "120 USD", 0);
	a = request(f, "start", 0, "2026-03-01");
	g_object_set(a, "company-id", f->company, "plan-price-id", venture_entity_get_id(yearly), "discount-id", flat, NULL);
	save(f, a);
	g_clear_object(&a);
	a = last_of(f, "subscription_event");
	g_assert_cmpint(money_field(a, "to-mrr"), ==, 4000);

	/* Reports last: each builds a context whose audit listener outlives it. */
	r = report(f, "mrr", "2026-01");
	g_assert_cmpint(metric_amount(r, "mrr"), ==, 3000);
	{
		g_autoptr(JsonNode) json = venture_report_result_to_json(r);
		const gchar *note = json_object_get_string_member(json_node_get_object(json), "note");
		g_assert_nonnull(g_strstr_len(note, -1, "discounts are included"));
	}
	g_clear_object(&r);
	r = report(f, "mrr", "2026-02");
	g_assert_cmpint(metric_amount(r, "mrr"), ==, 6000);
	g_assert_cmpint(metric_amount(r, "expansion"), ==, 3000);
}

/*
 * Moving a retired price's customers is one change per subscription in
 * one transaction: a refusal for one customer moves nobody and names who,
 * and a move at renewal schedules every one of them. If this regresses,
 * half a customer base ends up on the new price after an error, with no
 * record of which half.
 */
static void
test_move_customers(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) old = venture_database_get(f->db, VENTURE_TYPE_PLAN_PRICE, f->price, NULL);
	g_autoptr(VentureEntity) next = record(f, "plan_price");
	g_autoptr(VentureEntity) other = record(f, "company");
	g_autoptr(VentureEntity) a = NULL;
	g_autoptr(VentureEntity) s = NULL;
	g_autoptr(GDateTime) at = g_date_time_new_utc(2026, 1, 17, 0, 0, 0);
	g_autoptr(GDateTime) later = g_date_time_new_utc(2026, 1, 25, 0, 0, 0);
	g_autoptr(GError) error = NULL;
	VentureBillingService *service = venture_billing_service_get(f->db);
	gint64 first, second, requests;
	guint moved = 0;

	(void)data;
	g_object_set(next, "plan-id", integer(old, "plan-id"), "currency", "USD", "active", TRUE, "per-seat", TRUE, NULL);
	field(next, "amount", "40 USD");
	save(f, next);
	g_object_set(other, "name", "Second customer", NULL);
	save(f, other);
	first = start(f);
	a = request(f, "start", 0, "2026-01-01");
	g_object_set(a, "company-id", venture_entity_get_id(other), "plan-price-id", f->price, NULL);
	save(f, a);
	second = integer(a, "subscription-id");
	g_clear_object(&a);
	/* The second customer changed seats on the 20th, so a move dated the
	 * 17th is out of order for them, and must undo the first's move too. */
	a = request(f, "change-seats", second, "2026-01-20");
	g_object_set(a, "seats", (gint64)2, NULL);
	save(f, a);
	g_clear_object(&a);
	g_object_set(old, "active", FALSE, NULL);
	save(f, old);
	requests = count(f, "billing_request");
	g_assert_false(venture_billing_service_move_customers(service, VENTURE_PLAN_PRICE(old), VENTURE_PLAN_PRICE(next),
		FALSE, at, NULL, &moved, &error));
	g_assert_nonnull(error);
	g_assert_nonnull(g_strstr_len(error->message, -1, "Second customer"));
	g_clear_error(&error);
	g_assert_cmpuint(moved, ==, 0);
	g_assert_cmpint(count(f, "billing_request"), ==, requests);
	s = subscription(f, first);
	g_assert_cmpint(integer(s, "plan-price-id"), ==, f->price);
	g_clear_object(&s);

	/* At renewal: both keep today's price until then. */
	g_assert_true(venture_billing_service_move_customers(service, VENTURE_PLAN_PRICE(old), VENTURE_PLAN_PRICE(next),
		TRUE, later, NULL, &moved, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(moved, ==, 2);
	g_assert_cmpint(count(f, "billing_request"), ==, requests + 2);
	s = subscription(f, first);
	g_assert_cmpint(integer(s, "plan-price-id"), ==, f->price);
	g_assert_cmpint(integer(s, "pending-plan-price-id"), ==, venture_entity_get_id(next));
	g_clear_object(&s);

	/* Right now, prorated: the price changes today. */
	g_date_time_unref(later);
	later = g_date_time_new_utc(2026, 1, 26, 0, 0, 0);
	g_assert_true(venture_billing_service_move_customers(service, VENTURE_PLAN_PRICE(old), VENTURE_PLAN_PRICE(next),
		FALSE, later, NULL, &moved, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(moved, ==, 2);
	s = subscription(f, second);
	g_assert_cmpint(integer(s, "plan-price-id"), ==, venture_entity_get_id(next));
	g_assert_cmpint(money_field(s, "pending-adjustment"), >, 0);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/billing/catalog", test_catalog);
	g_test_add("/billing/start-and-guard", Fixture, NULL, setup, test_start_and_guard, teardown);
	g_test_add("/billing/quarterly", Fixture, NULL, setup, test_quarterly, teardown);
	g_test_add("/billing/venture-plans", Fixture, NULL, setup, test_venture_plans, teardown);
	g_test_add("/billing/discount", Fixture, NULL, setup, test_discount, teardown);
	g_test_add("/billing/next-invoice-tax", Fixture, NULL, setup, test_next_invoice_tax, teardown);
	g_test_add("/billing/skip-trial", Fixture, NULL, setup, test_skip_trial, teardown);
	g_test_add("/billing/trial-reminder", Fixture, NULL, setup, test_trial_reminder, teardown);
	g_test_add("/billing/trial-reminder-off", Fixture, NULL, setup, test_trial_reminder_off, teardown);
	g_test_add("/billing/price-change-notice", Fixture, NULL, setup, test_price_change_notice, teardown);
	g_test_add("/billing/discount-code", Fixture, NULL, setup, test_discount_code, teardown);
	g_test_add("/billing/renewal", Fixture, NULL, setup, test_renewal, teardown);
	g_test_add("/billing/trial", Fixture, NULL, setup, test_trial, teardown);
	g_test_add("/billing/lifecycle", Fixture, NULL, setup, test_lifecycle, teardown);
	g_test_add("/billing/seats-proration", Fixture, NULL, setup, test_seats_proration, teardown);
	g_test_add("/billing/dunning-sequence", Fixture, NULL, setup, test_dunning_sequence, teardown);
	g_test_add("/billing/dunning", Fixture, NULL, setup, test_dunning, teardown);
	g_test_add("/billing/rollback", Fixture, NULL, setup, test_rollback, teardown);
	g_test_add("/billing/mrr", Fixture, NULL, setup, test_mrr, teardown);
	g_test_add("/billing/churn", Fixture, NULL, setup, test_churn, teardown);
	g_test_add("/billing/due-report", Fixture, NULL, setup, test_due_report, teardown);
	g_test_add("/billing/late-sweep", Fixture, NULL, setup, test_late_sweep, teardown);
	g_test_add("/billing/dry-run-no-writes", Fixture, NULL, setup, test_dry_run_no_writes, teardown);
	g_test_add("/billing/price-history", Fixture, NULL, setup, test_price_history, teardown);
	g_test_add("/billing/contact-scope", Fixture, NULL, setup, test_contact_scope, teardown);
	g_test_add("/billing/churn-rates", Fixture, NULL, setup, test_churn_rates, teardown);
	g_test_add("/billing/initial-invoice", Fixture, NULL, setup, test_initial_invoice, teardown);
	g_test_add("/billing/migration", Fixture, NULL, setup, test_migration, teardown);
	g_test_add("/billing/proration-billed", Fixture, NULL, setup, test_proration_billed, teardown);
	g_test_add("/billing/proration-credit", Fixture, NULL, setup, test_proration_credit, teardown);
	g_test_add("/billing/proration-credit-precision", Fixture, NULL, setup, test_proration_credit_precision, teardown);
	g_test_add("/billing/month-end", Fixture, NULL, setup, test_month_end, teardown);
	g_test_add_func("/billing/documentation", test_documentation);
	g_test_add("/billing/closed-period", Fixture, NULL, setup, test_closed_period, teardown);
	g_test_add("/billing/currency", Fixture, NULL, setup, test_currency, teardown);
	g_test_add("/billing/yearly", Fixture, NULL, setup, test_yearly, teardown);
	g_test_add("/billing/scheduled-change-cancel", Fixture, NULL, setup, test_scheduled_change_cancel, teardown);
	g_test_add("/billing/scheduled-cancel-past-due", Fixture, NULL, setup, test_scheduled_cancel_past_due, teardown);
	g_test_add("/billing/sweep-scope", Fixture, NULL, setup, test_sweep_scope, teardown);
	g_test_add("/billing/interval-changes", Fixture, NULL, setup, test_interval_changes, teardown);
	g_test_add("/billing/uniqueness", Fixture, NULL, setup, test_uniqueness, teardown);
	g_test_add("/billing/collect-and-recover", Fixture, NULL, setup, test_collect_and_recover, teardown);
	g_test_add_func("/billing/upgrade-disabled-restart", test_upgrade_disabled_restart);
	g_test_add("/billing/cancel-credit", Fixture, NULL, setup, test_cancel_credit, teardown);
	g_test_add("/billing/price-tax", Fixture, NULL, setup, test_price_tax, teardown);
	g_test_add("/billing/mrr-discount", Fixture, NULL, setup, test_mrr_discount, teardown);
	g_test_add("/billing/move-customers", Fixture, NULL, setup, test_move_customers, teardown);
	return g_test_run();
}
