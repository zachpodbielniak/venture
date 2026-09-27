/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <string.h>
#include <libsoup/soup.h>
#include "venture-test-util.h"

typedef struct {
	VentureDatabase *db;
	VentureContext *context;
	VentureConfig *config;
	gint64 org;
	gint64 company;
} Fixture;

static GType
type(const gchar *name)
{
	GType result = venture_entity_registry_lookup(venture_entity_registry_get_default(), name);
	g_assert_cmpuint(result, !=, G_TYPE_INVALID);
	return result;
}

static VentureEntity *
record(Fixture *f, const gchar *name)
{
	VentureEntity *r = g_object_new(type(name), NULL);
	venture_entity_set_organization_id(r, f->org);
	return r;
}

static void
save(Fixture *f, VentureEntity *r)
{
	g_autoptr(GError) error = NULL;
	gboolean ok = venture_database_save(f->db, r, NULL, &error);
	g_assert_no_error(error);
	g_assert_true(ok);
}

static void
field(VentureEntity *r, const gchar *key, const gchar *value)
{
	g_autoptr(GError) error = NULL;
	g_assert_true(venture_entity_set_field_from_string(r, key, value, &error));
	g_assert_no_error(error);
}

static void
setup(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) company = NULL;
	f->config = venture_config_new();
	f->db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->db);
	f->org = venture_context_get_default_organization_id(f->context);
	company = record(f, "company");
	g_object_set(company, "name", "Buyer", NULL);
	save(f, company);
	f->company = venture_entity_get_id(company);
}

static void
teardown(Fixture *f, gconstpointer data)
{
	g_clear_object(&f->context);
	g_clear_object(&f->db);
	g_clear_object(&f->config);
}

static VentureEntity *
quote(Fixture *f, const gchar *number)
{
	VentureEntity *r = record(f, "quote");
	g_object_set(r, "number", number, "company-id", f->company, "currency", "USD", NULL);
	save(f, r);
	return r;
}

static VentureEntity *
line(Fixture *f, VentureEntity *q)
{
	VentureEntity *r = record(f, "quote_line");
	g_object_set(r, "quote-id", venture_entity_get_id(q), "description", "Consulting", NULL);
	field(r, "quantity", "3");
	field(r, "unit-price", "19.99 USD");
	field(r, "discount-percent", "10");
	field(r, "tax-percent", "5");
	save(f, r);
	return r;
}

static VentureEntity *
fresh(Fixture *f, const gchar *name, gint64 id)
{
	g_autoptr(GError) error = NULL;
	VentureEntity *r = venture_database_get(f->db, type(name), id, &error);
	g_assert_no_error(error);
	g_assert_nonnull(r);
	return r;
}

static VentureEntity *
request(Fixture *f, VentureEntity *q, const gchar *action)
{
	g_autoptr(VentureEntity) current = fresh(f, "quote", venture_entity_get_id(q));
	VentureEntity *r = record(f, "quote_action");
	g_object_set(r, "quote-id", venture_entity_get_id(q), "action", action,
		"expected-version", venture_entity_get_version(current), "accepted-by", "Alex Buyer", "reason", "Budget", NULL);
	return r;
}

static void
action(Fixture *f, VentureEntity *q, const gchar *verb)
{
	g_autoptr(VentureEntity) r = request(f, q, verb);
	save(f, r);
}

static gint64
amount(VentureEntity *r, const gchar *key)
{
	g_autoptr(VentureMoney) money = NULL;
	g_object_get(r, key, &money, NULL);
	g_assert_nonnull(money);
	return venture_money_get_amount(money);
}

static gint64
integer(VentureEntity *r, const gchar *key)
{
	gint64 value = 0;
	g_object_get(r, key, &value, NULL);
	return value;
}

static void
status(Fixture *f, VentureEntity *q, const gchar *expected)
{
	g_autoptr(VentureEntity) r = fresh(f, "quote", venture_entity_get_id(q));
	gint state;
	const gchar *value;
	GParamSpec *spec = g_object_class_find_property(G_OBJECT_GET_CLASS(r), "status");
	g_object_get(r, "status", &state, NULL);
	value = venture_enum_to_nick(G_PARAM_SPEC_VALUE_TYPE(spec), state);
	g_assert_cmpstr(value, ==, expected);
}

static GPtrArray *
rows(Fixture *f, const gchar *name)
{
	g_autoptr(VentureQuery) query = venture_query_new(type(name));
	g_autoptr(GError) error = NULL;
	GPtrArray *result;
	venture_query_set_organization(query, f->org);
	venture_query_set_limit(query, 0);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	result = venture_database_find(f->db, query, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	return result;
}

static void
test_records(Fixture *f, gconstpointer data)
{
	const gchar *names[] = { "price_list", "price_list_item", "quote", "quote_line", "quote_event", "quote_delivery", "quote_action", NULL };
	guint i;
	for (i = 0; names[i] != NULL; i++)
	{
		g_autoptr(VentureEntity) r = record(f, names[i]);
		g_assert_cmpstr(venture_entity_get_entity_name(r), ==, names[i]);
	}
}

/* 59.97 - 6.00 + 2.70 = 56.67; discount rounds before tax. */
static void
test_totals(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) q = quote(f, "Q-1");
	g_autoptr(VentureEntity) l = line(f, q);
	g_autoptr(VentureEntity) r = fresh(f, "quote", venture_entity_get_id(q));
	g_assert_cmpint(amount(r, "subtotal"), ==, 5997);
	g_assert_cmpint(amount(r, "discount"), ==, 600);
	g_assert_cmpint(amount(r, "tax"), ==, 270);
	g_assert_cmpint(amount(r, "total"), ==, 5667);
	field(l, "quantity", "1");
	field(l, "unit-price", "0.05 USD");
	field(l, "discount-percent", "50");
	field(l, "tax-percent", "50");
	save(f, l);
	g_clear_object(&r);
	r = fresh(f, "quote", venture_entity_get_id(q));
	g_assert_cmpint(amount(r, "discount"), ==, 2);
	g_assert_cmpint(amount(r, "tax"), ==, 2);
	g_assert_cmpint(amount(r, "total"), ==, 5);
}

/*
 * A quote line taxed by a rate record is taxed exactly, like an invoice
 * line: 8.875% of 53.97 is 4.79, which no whole percent can say. The
 * invoice made on acceptance carries the same rate record, so it asks the
 * customer for exactly what the quote promised. If this regresses, a New
 * York quote is rounded to 9% and the invoice disagrees with it.
 */
static void
test_tax_code(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) q = quote(f, "Q-TAX");
	g_autoptr(VentureEntity) rate = record(f, "tax_code");
	g_autoptr(VentureEntity) l = record(f, "quote_line");
	g_autoptr(VentureEntity) r = NULL;
	g_autoptr(GPtrArray) invoices = NULL;
	g_autoptr(GPtrArray) lines = NULL;
	g_autoptr(VentureMoney) balance = NULL;
	g_autoptr(GError) error = NULL;
	gint64 invoice_id;
	guint i, found = 0;
	g_object_set(rate, "code", "NY", "name", "New York sales tax",
		"rate-numerator", (gint64)8875, "rate-denominator", (gint64)100000, "active", TRUE, NULL);
	save(f, rate);
	g_object_set(l, "quote-id", venture_entity_get_id(q), "description", "Consulting",
		"tax-code-id", venture_entity_get_id(rate), NULL);
	field(l, "quantity", "3");
	field(l, "unit-price", "19.99 USD");
	field(l, "discount-percent", "10");
	save(f, l);
	r = fresh(f, "quote", venture_entity_get_id(q));
	g_assert_cmpint(amount(r, "subtotal"), ==, 5997);
	g_assert_cmpint(amount(r, "discount"), ==, 600);
	g_assert_cmpint(amount(r, "tax"), ==, 479);
	g_assert_cmpint(amount(r, "total"), ==, 5876);

	action(f, q, "send");
	action(f, q, "accept");
	invoices = rows(f, "invoice");
	g_assert_cmpuint(invoices->len, ==, 1);
	invoice_id = venture_entity_get_id(g_ptr_array_index(invoices, 0));
	lines = rows(f, "invoice_line");
	for (i = 0; i < lines->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(lines, i);
		gint64 owner = 0, code = 0;
		g_object_get(row, "invoice-id", &owner, "tax-code-id", &code, NULL);
		if (owner != invoice_id) continue;
		g_assert_cmpint(code, ==, venture_entity_get_id(rate));
		found++;
	}
	g_assert_cmpuint(found, ==, 1);
	balance = venture_settlement_service_invoice_balance(venture_settlement_service_get(f->db),
		invoice_id, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpint(venture_money_get_amount(balance), ==, 5876);
}

static void
test_lifecycle(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) q = quote(f, "Q-1");
	g_autoptr(VentureEntity) l = line(f, q);
	g_autoptr(VentureEntity) deal = record(f, "deal");
	g_autoptr(VentureEntity) current = fresh(f, "quote", venture_entity_get_id(q));
	g_autoptr(GPtrArray) invoices = NULL;
	g_autoptr(GPtrArray) events = NULL;
	g_autoptr(GPtrArray) deliveries = NULL;
	g_autoptr(VentureMoney) balance = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) contact = record(f, "contact");
	g_autoptr(VentureEntity) sequence = record(f, "sequence");
	g_autoptr(VentureEntity) step = record(f, "sequence_step");
	g_autoptr(VentureEntity) enrollment = record(f, "sequence_enrollment");
	g_object_set(deal, "name", "Services", "company-id", f->company, NULL);
	save(f, deal);
	/* Acceptance must also stop follow-ups through the same won transition. */
	g_object_set(contact, "name", "Buyer", "email", "buyer@example.com", NULL);
	save(f, contact);
	g_object_set(deal, "contact-id", venture_entity_get_id(contact), NULL);
	save(f, deal);
	g_object_set(sequence, "name", "Proposal follow-up", "active", TRUE, "timezone", "UTC",
		"send-window-start", (gint64)9, "send-window-end", (gint64)17,
		"weekdays", "1,2,3,4,5", "exit-on-deal-won", TRUE, NULL);
	save(f, sequence);
	g_object_set(step, "sequence-id", venture_entity_get_id(sequence), "position", (gint64)1,
		"active", TRUE, "subject", "Following up", "body", "Your proposal", NULL);
	save(f, step);
	g_object_set(enrollment, "sequence-id", venture_entity_get_id(sequence),
		"contact-id", venture_entity_get_id(contact), "deal-id", venture_entity_get_id(deal),
		"enrollment-reason", "Requested proposal", NULL);
	save(f, enrollment);
	g_object_set(current, "deal-id", venture_entity_get_id(deal), NULL);
	save(f, current);
	action(f, q, "send");
	status(f, q, "sent");
	deliveries = rows(f, "quote_delivery");
	g_assert_cmpuint(deliveries->len, ==, 1);
	action(f, q, "accept");
	status(f, q, "accepted");
	{
		g_autoptr(VentureEntity) exited = fresh(f, "sequence_enrollment", venture_entity_get_id(enrollment));
		g_autofree gchar *reason = NULL;
		gint enrollment_status;
		g_object_get(exited, "status", &enrollment_status, "exit-reason", &reason, NULL);
		g_assert_cmpint(enrollment_status, ==, 3);
		g_assert_cmpstr(reason, ==, "deal_won");
	}
	invoices = rows(f, "invoice");
	g_assert_cmpuint(invoices->len, ==, 1);
	balance = venture_settlement_service_invoice_balance(venture_settlement_service_get(f->db),
		venture_entity_get_id(g_ptr_array_index(invoices, 0)), NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpint(venture_money_get_amount(balance), ==, 5667);
	events = rows(f, "quote_event");
	g_assert_cmpuint(events->len, ==, 2);
	g_assert_cmpint(amount(g_ptr_array_index(events, 0), "total"), ==, 5667);
	g_clear_object(&current);
	current = fresh(f, "deal", venture_entity_get_id(deal));
	{
		gint stage;
		g_object_get(current, "stage", &stage, NULL);
		g_assert_cmpint(stage, ==, VENTURE_DEAL_STAGE_WON);
		{
			g_autoptr(GPtrArray) history = rows(f, "deal_stage_entry");
			g_assert_cmpuint(history->len, ==, 2);
		}
	}
}

static gint64
plan_price(Fixture *f, gboolean per_seat)
{
	g_autoptr(VentureEntity) plan = record(f, "plan");
	g_autoptr(VentureEntity) price = record(f, "plan_price");
	g_object_set(plan, "name", per_seat ? "Team" : "Solo", "code", per_seat ? "team" : "solo", "active", TRUE, NULL);
	save(f, plan);
	g_object_set(price, "plan-id", venture_entity_get_id(plan), "currency", "USD", "active", TRUE,
		"per-seat", per_seat, NULL);
	field(price, "amount", "30 USD");
	save(f, price);
	return venture_entity_get_id(price);
}

static gboolean
plan_line(Fixture *f, VentureEntity *q, gint64 price, const gchar *seats, const gchar *tax, GError **error)
{
	g_autoptr(VentureEntity) r = record(f, "quote_line");
	g_object_set(r, "quote-id", venture_entity_get_id(q), "description", "Team plan", "plan-price-id", price, NULL);
	field(r, "quantity", seats);
	if (tax != NULL)
		field(r, "tax-percent", tax);
	return venture_database_save(f->db, r, NULL, error);
}

/*
 * A quote whose line names a plan price starts that subscription in one
 * step once accepted: the price and seats come from the line, the
 * subscription and the quote point at each other, and the acceptance
 * invoice leaves the plan line to the subscription's own first invoice.
 * A second start is refused, from the quote or behind its back. If this
 * regresses, an accepted subscription quote is billed twice for its first
 * period, retyped by hand, or starts two subscriptions.
 */
static void
test_start_subscription(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) q = quote(f, "Q-SUB");
	g_autoptr(VentureEntity) consulting = line(f, q);
	g_autoptr(VentureEntity) current = NULL;
	g_autoptr(VentureEntity) sub = NULL;
	g_autoptr(VentureEntity) again = NULL;
	g_autoptr(VentureEntity) direct = record(f, "billing_request");
	g_autoptr(GPtrArray) invoices = NULL;
	g_autoptr(GPtrArray) plan_lines = NULL;
	g_autoptr(VentureMoney) balance = NULL;
	g_autoptr(GError) error = NULL;
	gint64 team = plan_price(f, TRUE), solo = plan_price(f, FALSE), subscription_id = 0, quote_id = 0;
	(void)data;
	/* A plan line may not carry a tax or a discount the subscription would
	 * not charge, a flat price takes one seat, and a quote starts one. */
	g_assert_false(plan_line(f, q, team, "3", "5", &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	g_assert_false(plan_line(f, q, solo, "2", NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	/* Nor a tax rate other than the plan price's own. */
	{
		g_autoptr(VentureEntity) rate = record(f, "tax_code");
		g_autoptr(VentureEntity) taxed = record(f, "quote_line");
		g_object_set(rate, "code", "SUB", "name", "Sales tax", "rate-numerator", (gint64)8875,
			"rate-denominator", (gint64)100000, "active", TRUE, NULL);
		save(f, rate);
		g_object_set(taxed, "quote-id", venture_entity_get_id(q), "description", "Team plan", "plan-price-id", team,
			"tax-code-id", venture_entity_get_id(rate), NULL);
		field(taxed, "quantity", "3");
		g_assert_false(venture_database_save(f->db, taxed, NULL, &error));
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
		g_clear_error(&error);
	}
	g_assert_true(plan_line(f, q, team, "3", NULL, &error));
	g_assert_no_error(error);
	g_assert_false(plan_line(f, q, solo, "1", NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	plan_lines = rows(f, "quote_line");
	g_assert_cmpuint(plan_lines->len, ==, 2);
	/* The line is priced from the plan: three seats at $30. */
	g_assert_cmpint(amount(g_ptr_array_index(plan_lines, 1), "unit-price"), ==, 3000);
	current = fresh(f, "quote", venture_entity_get_id(q));
	g_assert_cmpint(amount(current, "total"), ==, 5667 + 9000);

	action(f, q, "send");
	g_clear_object(&again);
	again = request(f, q, "start-subscription");
	g_assert_false(venture_database_save(f->db, again, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	action(f, q, "accept");
	invoices = rows(f, "invoice");
	g_assert_cmpuint(invoices->len, ==, 1);
	balance = venture_settlement_service_invoice_balance(venture_settlement_service_get(f->db),
		venture_entity_get_id(g_ptr_array_index(invoices, 0)), NULL, &error);
	g_assert_no_error(error);
	/* Consulting only; the plan is billed by the subscription. */
	g_assert_cmpint(venture_money_get_amount(balance), ==, 5667);

	g_clear_object(&again);
	again = request(f, q, "start-subscription");
	save(f, again);
	g_object_get(again, "result-subscription-id", &subscription_id, NULL);
	g_assert_cmpint(subscription_id, >, 0);
	g_clear_object(&current);
	current = fresh(f, "quote", venture_entity_get_id(q));
	g_assert_cmpint(integer(current, "subscription-id"), ==, subscription_id);
	sub = fresh(f, "customer_subscription", subscription_id);
	g_object_get(sub, "quote-id", &quote_id, NULL);
	g_assert_cmpint(quote_id, ==, venture_entity_get_id(q));
	g_assert_cmpint(integer(sub, "seats"), ==, 3);
	g_assert_cmpint(integer(sub, "plan-price-id"), ==, team);
	g_assert_cmpint(integer(sub, "company-id"), ==, f->company);
	g_clear_pointer(&invoices, g_ptr_array_unref);
	invoices = rows(f, "invoice");
	g_assert_cmpuint(invoices->len, ==, 2);
	g_clear_pointer(&balance, venture_money_free);
	balance = venture_settlement_service_invoice_balance(venture_settlement_service_get(f->db),
		venture_entity_get_id(g_ptr_array_index(invoices, 1)), NULL, &error);
	g_assert_cmpint(venture_money_get_amount(balance), ==, 9000);

	/* Twice from the quote, and once around it through billing. */
	g_clear_object(&again);
	again = request(f, q, "start-subscription");
	g_assert_false(venture_database_save(f->db, again, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	g_object_set(direct, "action", "start", "company-id", f->company, "plan-price-id", team,
		"quote-id", venture_entity_get_id(q), NULL);
	field(direct, "at", "2026-01-01");
	g_assert_false(venture_database_save(f->db, direct, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	g_clear_pointer(&invoices, g_ptr_array_unref);
	invoices = rows(f, "customer_subscription");
	g_assert_cmpuint(invoices->len, ==, 1);
	(void)consulting;
}

/*
 * A subscription line quotes what the subscription's first invoice will
 * charge, tax included: the price's own rate when it has one, else the
 * customer's address rate, and nothing for an exempt customer. The
 * started subscription's invoice then asks for exactly the quote's total.
 * If this regresses, the customer signs for $90.00 and is invoiced $99.00.
 */
static void
test_plan_line_tax(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) rate = record(f, "tax_code");
	g_autoptr(VentureEntity) jurisdiction = record(f, "tax_jurisdiction");
	g_autoptr(VentureEntity) rule = record(f, "tax_rule");
	g_autoptr(VentureEntity) price = NULL, q = NULL, current = NULL, start = NULL, company = NULL;
	g_autoptr(GPtrArray) lines = NULL, invoices = NULL;
	g_autoptr(VentureMoney) balance = NULL;
	g_autoptr(GError) error = NULL;
	gint64 team = plan_price(f, TRUE), solo = plan_price(f, FALSE), code = 0;
	(void)data;
	g_object_set(rate, "code", "TEN", "name", "Ten percent", "rate-numerator", (gint64)10,
		"rate-denominator", (gint64)100, "active", TRUE, NULL);
	save(f, rate);
	price = fresh(f, "plan_price", team);
	g_object_set(price, "tax-code-id", venture_entity_get_id(rate), NULL);
	save(f, price);

	/* The price's code: three seats at $30 plus 10%. */
	q = quote(f, "Q-TAXED");
	g_assert_true(plan_line(f, q, team, "3", NULL, &error));
	g_assert_no_error(error);
	lines = rows(f, "quote_line");
	g_object_get(g_ptr_array_index(lines, lines->len - 1), "tax-code-id", &code, NULL);
	g_assert_cmpint(code, ==, venture_entity_get_id(rate));
	current = fresh(f, "quote", venture_entity_get_id(q));
	g_assert_cmpint(amount(current, "tax"), ==, 900);
	g_assert_cmpint(amount(current, "total"), ==, 9900);
	action(f, q, "send");
	action(f, q, "accept");
	start = request(f, q, "start-subscription");
	save(f, start);
	invoices = rows(f, "invoice");
	g_assert_cmpuint(invoices->len, ==, 1);
	balance = venture_settlement_service_invoice_balance(venture_settlement_service_get(f->db),
		venture_entity_get_id(g_ptr_array_index(invoices, 0)), NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpint(venture_money_get_amount(balance), ==, 9900);
	g_clear_object(&q);
	g_clear_object(&current);

	/* No code: the customer's address rate, 8.875% on $30, half to even. */
	g_object_set(jurisdiction, "code", "QUOTE", "name", "Quote rate", "kind", "sales",
		"rate-scaled", (gint64)88750, NULL);
	field(jurisdiction, "effective-from", "2020-01-01");
	save(f, jurisdiction);
	g_object_set(rule, "jurisdiction-id", venture_entity_get_id(jurisdiction), "state", "NY", "active", TRUE, NULL);
	save(f, rule);
	company = fresh(f, "company", f->company);
	g_object_set(company, "address-state", "NY", NULL);
	save(f, company);
	q = quote(f, "Q-ADDRESS");
	g_assert_true(plan_line(f, q, solo, "1", NULL, &error));
	g_assert_no_error(error);
	current = fresh(f, "quote", venture_entity_get_id(q));
	g_assert_cmpint(amount(current, "total"), ==, 3266);
	g_clear_object(&q);
	g_clear_object(&current);

	/* An exempt customer is quoted no tax, with a code or without. */
	g_object_set(company, "tax-exempt", TRUE, "tax-exempt-reason", "Resale", NULL);
	save(f, company);
	q = quote(f, "Q-EXEMPT");
	g_assert_true(plan_line(f, q, solo, "1", NULL, &error));
	g_assert_no_error(error);
	current = fresh(f, "quote", venture_entity_get_id(q));
	g_assert_cmpint(amount(current, "total"), ==, 3000);
}

/* A process refusing its won stage must roll back the issued invoice too. */
static void
test_pipeline_refusal(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) q = quote(f, "Required-field");
	g_autoptr(VentureEntity) l = line(f, q);
	g_autoptr(VentureEntity) deal = record(f, "deal");
	g_autoptr(VentureEntity) current = fresh(f, "quote", venture_entity_get_id(q));
	g_autoptr(VentureEntity) accept = NULL;
	g_autoptr(VentureEntity) won = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_PIPELINE_STAGE);
	g_autoptr(GPtrArray) invoices = NULL;
	g_autoptr(GPtrArray) entries = NULL;
	g_autoptr(GError) error = NULL;
	(void)data;
	g_object_set(deal, "name", "Needs approval", NULL);
	save(f, deal);
	g_object_set(current, "deal-id", venture_entity_get_id(deal), NULL);
	save(f, current);
	venture_query_add_filter_string(query, "kind", VENTURE_FILTER_OP_EQ, "won", NULL);
	won = venture_database_find_one(f->db, query, &error);
	g_assert_nonnull(won);
	g_object_set(won, "required-fields", "next_step", NULL);
	save(f, won);
	action(f, q, "send");
	accept = request(f, q, "accept");
	g_assert_false(venture_database_save(f->db, accept, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	status(f, q, "sent");
	invoices = rows(f, "invoice");
	entries = rows(f, "deal_stage_entry");
	g_assert_cmpuint(invoices->len, ==, 0);
	g_assert_cmpuint(entries->len, ==, 1);
}

static gboolean
fail_invoice(VentureDatabase *db, VentureEntity *r, VentureEntity *previous, gpointer data, GError **error)
{
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Injected invoice failure");
	return FALSE;
}

static void
test_rollback(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) q = quote(f, "Q-1");
	g_autoptr(VentureEntity) l = line(f, q);
	g_autoptr(VentureEntity) a = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) events = NULL;
	g_autoptr(GPtrArray) invoices = NULL;
	action(f, q, "send");
	venture_database_add_save_validator(f->db, VENTURE_TYPE_INVOICE, fail_invoice, NULL, NULL);
	a = request(f, q, "accept");
	g_assert_false(venture_database_save(f->db, a, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	status(f, q, "sent");
	events = rows(f, "quote_event");
	invoices = rows(f, "invoice");
	g_assert_cmpuint(events->len, ==, 1);
	g_assert_cmpuint(invoices->len, ==, 0);
}

static void
test_freeze(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) q = quote(f, "Q-1");
	g_autoptr(VentureEntity) l = line(f, q);
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) current = fresh(f, "quote", venture_entity_get_id(q));
	field(current, "status", "accepted");
	g_assert_false(venture_database_save(f->db, current, NULL, &error));
	g_assert_nonnull(strstr(error->message, "VentureQuoteService"));
	g_clear_error(&error);
	action(f, q, "send");
	field(l, "unit-price", "1 USD");
	g_assert_false(venture_database_save(f->db, l, NULL, &error));
	g_clear_error(&error);
	g_assert_false(venture_database_delete(f->db, l, NULL, &error));
	g_clear_error(&error);
	action(f, q, "revise");
	status(f, q, "superseded");
	{
		g_autoptr(GPtrArray) quotes = rows(f, "quote");
		g_autoptr(GPtrArray) lines = rows(f, "quote_line");
		gint64 revision;
		g_assert_cmpuint(quotes->len, ==, 2);
		g_assert_cmpuint(lines->len, ==, 2);
		g_object_get(g_ptr_array_index(quotes, 1), "revision", &revision, NULL);
		g_assert_cmpint(revision, ==, 2);
	}
}

static void
test_prices(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) q = quote(f, "Priced");
	g_autoptr(VentureEntity) product = record(f, "product");
	g_autoptr(VentureEntity) list = record(f, "price_list");
	g_autoptr(VentureEntity) item = record(f, "price_list_item");
	g_autoptr(VentureEntity) l = record(f, "quote_line");
	g_object_set(product, "name", "Service", NULL);
	field(product, "list-price", "50 USD");
	save(f, product);
	g_object_set(list, "name", "Default", "currency", "USD", "is-default", TRUE, NULL);
	save(f, list);
	g_object_set(item, "price-list-id", venture_entity_get_id(list), "product-id", venture_entity_get_id(product), NULL);
	field(item, "unit-price", "42 USD");
	field(item, "min-quantity", "2");
	save(f, item);
	g_object_set(l, "quote-id", venture_entity_get_id(q), "product-id", venture_entity_get_id(product), "description", "Service", NULL);
	field(l, "quantity", "3");
	save(f, l);
	g_assert_cmpint(amount(l, "unit-price"), ==, 4200);
	{
		g_autoptr(VentureEntity) customer = fresh(f, "company", f->company);
		g_autoptr(VentureEntity) special = record(f, "price_list");
		g_autoptr(VentureEntity) tier = record(f, "price_list_item");
		g_autoptr(VentureEntity) next = record(f, "quote_line");
		g_autofree gchar *id = NULL;
		g_object_set(special, "name", "Customer terms", "currency", "USD", NULL);
		save(f, special);
		g_object_set(tier, "price-list-id", venture_entity_get_id(special), "product-id", venture_entity_get_id(product), NULL);
		field(tier, "unit-price", "35 USD");
		save(f, tier);
		id = g_strdup_printf("%" G_GINT64_FORMAT, venture_entity_get_id(special));
		field(customer, "default-price-list-id", id);
		save(f, customer);
		g_object_set(next, "quote-id", venture_entity_get_id(q), "product-id", venture_entity_get_id(product), "description", "Customer rate", NULL);
		field(next, "quantity", "3");
		save(f, next);
		g_assert_cmpint(amount(next, "unit-price"), ==, 3500);
	}

}

static void
test_scope(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) q = quote(f, "Q-1");
	g_autoptr(VentureEntity) duplicate = record(f, "quote");
	g_autoptr(GError) error = NULL;
	g_object_set(duplicate, "number", "Q-1", "currency", "USD", NULL);
	g_assert_false(venture_database_save(f->db, duplicate, NULL, &error));
	g_clear_error(&error);
	{
		g_autoptr(VentureEntity) org = record(f, "organization");
		g_autoptr(VentureEntity) q2 = record(f, "quote");
		g_object_set(org, "name", "Other", NULL);
		save(f, org);
		venture_entity_set_organization_id(q2, venture_entity_get_id(org));
		g_object_set(q2, "number", "Q-1", "currency", "USD", NULL);
		save(f, q2);
		g_object_set(q2, "company-id", f->company, NULL);
		g_assert_false(venture_database_save(f->db, q2, NULL, &error));
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
	}
}

static void
test_stale(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) q = quote(f, "Q-1");
	g_autoptr(VentureEntity) l = line(f, q);
	g_autoptr(VentureEntity) a = request(f, q, "send");
	g_autoptr(GError) error = NULL;
	field(l, "unit-price", "30 USD");
	save(f, l);
	g_assert_false(venture_database_save(f->db, a, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
	status(f, q, "draft");
}

typedef struct { gboolean done; GBytes *body; GError *error; } HttpResult;

static void
http_done(GObject *source, GAsyncResult *result, gpointer data)
{
	HttpResult *r = data;
	r->body = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &r->error);
	r->done = TRUE;
}

static guint
http(SoupSession *session, const gchar *base, const gchar *method, const gchar *path, const gchar *body, gchar **response)
{
	g_autofree gchar *url = g_strconcat(base, path, NULL);
	g_autoptr(SoupMessage) message = soup_message_new(method, url);
	HttpResult r = { FALSE, NULL, NULL };
	guint code;
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	if (body != NULL)
	{
		g_autoptr(GBytes) bytes = g_bytes_new(body, strlen(body));
		soup_message_set_request_body_from_bytes(message, "application/json", bytes);
	}
	soup_session_send_and_read_async(session, message, G_PRIORITY_DEFAULT, NULL, http_done, &r);
	while (!r.done) g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(r.error);
	*response = g_strndup(g_bytes_get_data(r.body, NULL), g_bytes_get_size(r.body));
	code = soup_message_get_status(message);
	g_bytes_unref(r.body);
	return code;
}

typedef struct { gboolean done; gchar *out; gchar *err; GError *error; } CliResult;
static void
cli_done(GObject *source, GAsyncResult *result, gpointer data)
{
	CliResult *r = data;
	g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, &r->out, &r->err, &r->error);
	r->done = TRUE;
}

static void
test_http(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) q = quote(f, "Public-proposal");
	g_autoptr(VentureEntity) l = line(f, q);
	g_autoptr(VentureEntity) current = NULL;
	g_autoptr(VentureWebServer) server = NULL;
	g_autoptr(SoupSession) session = soup_session_new();
	g_autoptr(GSocketListener) probe = g_socket_listener_new();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *dir = g_dir_make_tmp("venture-quotes-http-XXXXXX", NULL);
	g_autofree gchar *base = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *body = NULL;
	g_autofree gchar *token = NULL;
	guint port = g_socket_listener_add_any_inet_port(probe, NULL, &error);
	g_assert_no_error(error);
	g_clear_object(&probe);
	base = g_strdup_printf("http://127.0.0.1:%u", port);
	g_object_set(f->config, "server-bind-address", "127.0.0.1", "server-port", (gint64)port,
		"security-require-auth", FALSE, "state-dir", dir, NULL);
	server = venture_web_server_new(f->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(server, &error));
	g_assert_no_error(error);
	/* An invalid staging flag must never publish a draft immediately. */
	path = g_strdup_printf("/api/v1/quotes/%" G_GINT64_FORMAT "/send?stage=typo", venture_entity_get_id(q));
	g_assert_cmpuint(http(session, base, "POST", path, "{}", &body), ==, 400);
	status(f, q, "draft");
	g_clear_pointer(&body, g_free);
	g_clear_pointer(&path, g_free);
	path = g_strdup_printf("/api/v1/quotes/%" G_GINT64_FORMAT "/send", venture_entity_get_id(q));
	g_assert_cmpuint(http(session, base, "POST", path, "{}", &body), ==, 200);
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(http(session, base, "GET", "/q/wrong-token", NULL, &body), ==, 404);
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(http(session, base, "POST", "/api/v1/quotes/999999/send", "{}", &body), ==, 404);
	g_clear_pointer(&body, g_free);
	current = fresh(f, "quote", venture_entity_get_id(q));
	g_object_get(current, "acceptance-token", &token, NULL);
	g_assert_cmpuint(strlen(token), ==, 64);
	g_clear_pointer(&path, g_free);
	path = g_strconcat("/q/", token, NULL);
	g_assert_cmpuint(http(session, base, "GET", path, NULL, &body), ==, 200);
	g_assert_nonnull(strstr(body, "Public-proposal"));
	g_assert_nonnull(strstr(body, "56.67"));
	g_assert_null(strstr(body, "company_id"));
	g_assert_null(strstr(body, "/e/"));
	g_clear_pointer(&body, g_free);
	g_clear_pointer(&path, g_free);
	path = g_strconcat("/q/", token, "/accept", NULL);
	g_assert_cmpuint(http(session, base, "POST", path, "{\"accepted_by\":\"Public Buyer\"}", &body), ==, 200);
	status(f, q, "accepted");
	{
		g_autoptr(GPtrArray) events = rows(f, "quote_event");
		g_autofree gchar *method = NULL;
		g_object_get(g_ptr_array_index(events, 1), "method", &method, NULL);
		g_assert_cmpstr(method, ==, "web");
	}
	{
		g_autoptr(VentureEntity) q2 = quote(f, "CLI");
		g_autoptr(VentureEntity) l2 = line(f, q2);
		g_autoptr(GSubprocessLauncher) launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE);
		g_autoptr(GSubprocess) child = NULL;
		g_autofree gchar *id = g_strdup_printf("%" G_GINT64_FORMAT, venture_entity_get_id(q2));
		const gchar *args[] = { "build/debug/venturectl", "--server", base, "quote", "send", id, NULL };
		CliResult r = { FALSE, NULL, NULL, NULL };
		g_subprocess_launcher_unsetenv(launcher, "VENTURE_TOKEN");
		child = g_subprocess_launcher_spawnv(launcher, args, &error);
		g_assert_no_error(error);
		g_subprocess_communicate_utf8_async(child, NULL, NULL, cli_done, &r);
		while (!r.done) g_main_context_iteration(NULL, TRUE);
		g_assert_no_error(r.error);
		g_assert_true(g_subprocess_get_successful(child));
		status(f, q2, "sent");
		g_free(r.out);
		g_free(r.err);
	}
	venture_web_server_stop(server);
	g_clear_object(&server);
	g_object_set(f->config, "security-require-auth", TRUE, NULL);
	server = venture_web_server_new(f->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(server, &error));
	g_assert_no_error(error);
	g_clear_pointer(&body, g_free);
	g_clear_pointer(&path, g_free);
	path = g_strconcat("/q/", token, NULL);
	g_assert_cmpuint(http(session, base, "GET", path, NULL, &body), ==, 200);
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(http(session, base, "GET", "/quotes/1/print", NULL, &body), ==, 302);
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(http(session, base, "POST", "/api/v1/quotes/1/send", "{}", &body), ==, 401);
	{
		guint i;
		guint code = 0;
		for (i = 0; i < 32; i++)
		{
			g_clear_pointer(&body, g_free);
			code = http(session, base, "GET", "/q/wrong-token", NULL, &body);
		}
		g_assert_cmpuint(code, ==, 429);
	}
	venture_config_set_module_enabled(f->config, "quotes", FALSE);
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(http(session, base, "GET", path, NULL, &body), ==, 404);
	g_assert_cmpuint(venture_entity_registry_lookup(venture_entity_registry_get_default(), "quote"), ==, G_TYPE_INVALID);
	g_assert_null(venture_report_registry_lookup(venture_context_get_report_registry(f->context), "quotes"));
	venture_config_set_module_enabled(f->config, "quotes", TRUE);
	venture_web_server_stop(server);
	g_clear_object(&server);
	venture_test_remove_tree(dir);
}

static void
test_report(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) q = quote(f, "Report");
	g_autoptr(VentureEntity) l = line(f, q);
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autofree gchar *json = NULL;
	g_autoptr(VentureDateRange) period = venture_date_range_new_all_time();
	VentureReport *report = venture_report_registry_lookup(venture_context_get_report_registry(f->context), "quotes");
	g_assert_nonnull(report);
	action(f, q, "send");
	action(f, q, "accept");
	result = venture_report_generate(report, f->context, period, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	node = venture_report_result_to_json(result);
	json = venture_json_to_string(node, FALSE);
	g_assert_nonnull(strstr(json, "acceptance_rate"));
	{
		GPtrArray *metrics = venture_report_result_get_metrics(result);
		guint i;
		gboolean found = FALSE;
		for (i = 0; i < metrics->len; i++)
		{
			VentureMetric *metric = g_ptr_array_index(metrics, i);
			if (g_strcmp0(venture_metric_get_key(metric), "accepted_count") == 0)
			{
				g_assert_cmpfloat(metric->number, ==, 1.0);
				found = TRUE;
			}
		}
		g_assert_true(found);
	}
}

static void
test_staged(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) q = quote(f, "Staged");
	g_autoptr(VentureEntity) l = line(f, q);
	g_autoptr(VentureEntity) a = NULL;
	g_autoptr(GError) error = NULL;
	VentureConfirmationStore *store = venture_context_get_confirmations(f->context);
	VentureConfirmation *confirmation;
	VentureActor origin;
	origin.kind = VENTURE_ACTOR_KIND_AI;
	origin.name = "assistant";
	origin.prompt = "Accept the quote for Alex Buyer";
	origin.request_id = NULL;
	origin.approved_by = NULL;
	action(f, q, "send");
	a = request(f, q, "accept");
	confirmation = venture_confirmation_store_stage(store, VENTURE_AUDIT_ACTION_CREATE, a, NULL, &origin, "assistant", &error);
	g_assert_no_error(error);
	g_assert_nonnull(confirmation);
	status(f, q, "sent");
	g_assert_true(venture_confirmation_store_approve(store, venture_confirmation_get_id(confirmation), "owner", &error));
	g_assert_no_error(error);
	status(f, q, "accepted");
}

static void
test_expiry(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) q = quote(f, "Expiring");
	g_autoptr(VentureEntity) l = line(f, q);
	g_autoptr(VentureEntity) current = fresh(f, "quote", venture_entity_get_id(q));
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(GDateTime) until = g_date_time_add_seconds(now, 1);
	g_autoptr(GError) error = NULL;
	g_object_set(current, "valid-until", until, NULL);
	save(f, current);
	action(f, q, "send");
	g_usleep(1200000);
	g_clear_pointer(&now, g_date_time_unref);
	now = venture_time_now();
	g_assert_cmpint(venture_quote_service_sweep(venture_database_get_quote_service(f->db), f->org, now, &error), ==, 1);
	g_assert_no_error(error);
	status(f, q, "expired");
	g_assert_cmpint(venture_quote_service_sweep(venture_database_get_quote_service(f->db), f->org, now, &error), ==, 0);
	g_assert_no_error(error);
}

static void
test_decline(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) q = quote(f, "Declined");
	g_autoptr(VentureEntity) l = line(f, q);
	g_autoptr(VentureEntity) a = NULL;
	g_autoptr(GError) error = NULL;
	action(f, q, "send");
	a = request(f, q, "decline");
	g_object_set(a, "reason", "", NULL);
	g_assert_false(venture_database_save(f->db, a, NULL, &error));
	g_clear_error(&error);
	action(f, q, "decline");
	status(f, q, "declined");
	g_clear_object(&a);
	a = request(f, q, "accept");
	g_assert_false(venture_database_save(f->db, a, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
}

static void
test_upgrade(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) org = NULL;
	/* Model the prior schema as a history prefix when later modules exist. */
	g_assert_true(venture_database_execute(f->db, "UPDATE organizations SET quote_valid_days = NULL; DELETE FROM schema_migrations WHERE version >= 150", NULL, &error));
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	org = fresh(f, "organization", f->org);
	{
		gint64 days;
		g_object_get(org, "quote-valid-days", &days, NULL);
		g_assert_cmpint(days, ==, 30);
	}
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
}

static GError *
veto_action(VentureQuoteService *service, VentureEntity *q, const gchar *action_name, gpointer data)
{
	return g_error_new_literal(VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED, "Approval required by quote policy");
}

static void
test_veto(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) q = quote(f, "Policy");
	g_autoptr(VentureEntity) l = line(f, q);
	g_autoptr(VentureEntity) a = request(f, q, "send");
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) events = NULL;
	g_signal_connect(venture_database_get_quote_service(f->db), "before-action", G_CALLBACK(veto_action), NULL);
	g_assert_false(venture_database_save(f->db, a, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	status(f, q, "draft");
	events = rows(f, "quote_event");
	g_assert_cmpuint(events->len, ==, 0);
}

static void
test_draft_removal(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) q = quote(f, "Editable");
	g_autoptr(VentureEntity) l = line(f, q);
	g_autoptr(VentureEntity) current = NULL;
	g_autoptr(GError) error = NULL;
	g_assert_true(venture_database_delete(f->db, l, NULL, &error));
	g_assert_no_error(error);
	current = fresh(f, "quote", venture_entity_get_id(q));
	g_assert_cmpint(amount(current, "total"), ==, 0);
	g_assert_true(venture_database_restore(f->db, l, NULL, &error));
	g_assert_no_error(error);
	g_clear_object(&current);
	current = fresh(f, "quote", venture_entity_get_id(q));
	g_assert_cmpint(amount(current, "total"), ==, 5667);
}

static gboolean
forge_state(VentureDatabase *db, VentureEntity *r, VentureEntity *previous, gpointer data, GError **error)
{
	g_object_set(r, "status", VENTURE_QUOTE_ACCEPTED, NULL);
	return TRUE;
}

static void
test_validator_boundary(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) q = quote(f, "Protected");
	g_autoptr(GError) error = NULL;
	venture_database_add_save_validator(f->db, VENTURE_TYPE_QUOTE, forge_state, NULL, NULL);
	g_object_set(q, "terms", "Changed", NULL);
	g_assert_false(venture_database_save(f->db, q, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	status(f, q, "draft");
}

/* Tax is computed on the discounted net, never on the pre-discount subtotal. */
static void
test_percentage_parts(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureMoney) subtotal = venture_money_new_for_currency(10000, "USD");
	g_autoptr(VentureMoney) discount = NULL;
	g_autoptr(VentureMoney) net = NULL;
	g_autoptr(VentureMoney) tax = NULL;
	g_autoptr(VentureMoney) total = NULL;
	g_autoptr(GError) error = NULL;
	(void)f;
	(void)data;
	g_assert_true(venture_quote_percentage_parts(subtotal, 10, 5, &discount, &net, &tax, &total, &error));
	g_assert_no_error(error);
	g_assert_cmpint(venture_money_get_amount(discount), ==, 1000);
	g_assert_cmpint(venture_money_get_amount(net), ==, 9000);
	g_assert_cmpint(venture_money_get_amount(tax), ==, 450);
	g_assert_cmpint(venture_money_get_amount(total), ==, 9450);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	venture_entity_registry_get_default();
	g_test_add("/quotes/percentage-parts", Fixture, NULL, setup, test_percentage_parts, teardown);
	g_test_add("/quotes/records", Fixture, NULL, setup, test_records, teardown);
	g_test_add("/quotes/tax-code", Fixture, NULL, setup, test_tax_code, teardown);
	g_test_add("/quotes/totals", Fixture, NULL, setup, test_totals, teardown);
	g_test_add("/quotes/pipeline-refusal", Fixture, NULL, setup, test_pipeline_refusal, teardown);
	g_test_add("/quotes/lifecycle", Fixture, NULL, setup, test_lifecycle, teardown);
	g_test_add("/quotes/start-subscription", Fixture, NULL, setup, test_start_subscription, teardown);
	g_test_add("/quotes/plan-line-tax", Fixture, NULL, setup, test_plan_line_tax, teardown);
	g_test_add("/quotes/rollback", Fixture, NULL, setup, test_rollback, teardown);
	g_test_add("/quotes/freeze-revision", Fixture, NULL, setup, test_freeze, teardown);
	g_test_add("/quotes/prices", Fixture, NULL, setup, test_prices, teardown);
	g_test_add("/quotes/scope", Fixture, NULL, setup, test_scope, teardown);
	g_test_add("/quotes/stale", Fixture, NULL, setup, test_stale, teardown);
	g_test_add("/quotes/http", Fixture, NULL, setup, test_http, teardown);
	g_test_add("/quotes/report", Fixture, NULL, setup, test_report, teardown);
	g_test_add("/quotes/staged", Fixture, NULL, setup, test_staged, teardown);
	g_test_add("/quotes/expiry", Fixture, NULL, setup, test_expiry, teardown);
	g_test_add("/quotes/decline", Fixture, NULL, setup, test_decline, teardown);
	g_test_add("/quotes/upgrade", Fixture, NULL, setup, test_upgrade, teardown);
	g_test_add("/quotes/veto", Fixture, NULL, setup, test_veto, teardown);
	g_test_add("/quotes/draft-removal", Fixture, NULL, setup, test_draft_removal, teardown);
	g_test_add("/quotes/validator-boundary", Fixture, NULL, setup, test_validator_boundary, teardown);
	return g_test_run();
}
