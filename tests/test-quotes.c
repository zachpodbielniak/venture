/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <string.h>

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
	g_object_set(deal, "name", "Services", "company-id", f->company, NULL);
	save(f, deal);
	g_object_set(current, "deal-id", venture_entity_get_id(deal), NULL);
	save(f, current);
	action(f, q, "send");
	status(f, q, "sent");
	deliveries = rows(f, "quote_delivery");
	g_assert_cmpuint(deliveries->len, ==, 1);
	action(f, q, "accept");
	status(f, q, "accepted");
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
	}
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

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	venture_entity_registry_get_default();
	g_test_add("/quotes/records", Fixture, NULL, setup, test_records, teardown);
	g_test_add("/quotes/totals", Fixture, NULL, setup, test_totals, teardown);
	g_test_add("/quotes/lifecycle", Fixture, NULL, setup, test_lifecycle, teardown);
	g_test_add("/quotes/rollback", Fixture, NULL, setup, test_rollback, teardown);
	g_test_add("/quotes/freeze-revision", Fixture, NULL, setup, test_freeze, teardown);
	return g_test_run();
}
