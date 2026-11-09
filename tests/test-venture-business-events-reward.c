/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include "venture-test-util.h"

/*
 * A refund inside the program window records referral_reward.revoked.
 * The window starts at the first payment record.
 */

typedef struct {
	VentureConfig *config;
	VentureDatabase *db;
	VentureContext *context;
	gint64 org;
} Fixture;

static void
setup(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	(void)data;
	f->config = venture_config_new();
	f->db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->db);
	f->org = venture_context_get_default_organization_id(f->context);
}

static void
teardown(Fixture *f, gconstpointer data)
{
	(void)data;
	g_clear_object(&f->context);
	g_clear_object(&f->db);
	g_clear_object(&f->config);
}

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

static gint64
event_count(Fixture *f, const gchar *name)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_BUSINESS_EVENT);
	g_autoptr(GError) error = NULL;
	gint64 count;
	venture_query_set_organization(query, f->org);
	g_assert_true(venture_query_add_filter_string(query, "event", VENTURE_FILTER_OP_EQ, name, NULL));
	count = venture_database_count(f->db, query, &error);
	g_assert_no_error(error);
	return count;
}

static void
refund_on(Fixture *f, VentureEntity *customer, const gchar *paid_on, const gchar *refunded_on)
{
	g_autoptr(VentureEntity) invoice = g_object_new(VENTURE_TYPE_INVOICE, "organization-id", f->org,
		"number", "INV-EVENT", "company-id", venture_entity_get_id(customer), NULL);
	g_autoptr(VentureEntity) line = g_object_new(VENTURE_TYPE_INVOICE_LINE, "organization-id", f->org,
		"description", "Plan", "quantity", 1.0, NULL);
	g_autoptr(VentureEntity) payment = g_object_new(VENTURE_TYPE_PAYMENT, "organization-id", f->org,
		"method", "manual", NULL);
	g_autoptr(VentureEntity) refund = g_object_new(VENTURE_TYPE_REFUND, "organization-id", f->org, NULL);
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_PAYMENT_ALLOCATION);
	g_autoptr(VentureEntity) allocation = NULL;
	field(invoice, "issued-at", paid_on);
	field(invoice, "due-at", paid_on);
	save(f, invoice);
	g_object_set(line, "invoice-id", venture_entity_get_id(invoice), NULL);
	field(line, "unit-price", "100.00 USD");
	save(f, line);
	g_object_set(invoice, "status", VENTURE_INVOICE_STATUS_SENT, NULL);
	save(f, invoice);
	g_object_set(payment, "customer-id", venture_entity_get_id(customer), "invoice-id", venture_entity_get_id(invoice), NULL);
	field(payment, "amount", "100.00 USD");
	field(payment, "date", paid_on);
	save(f, payment);
	allocation = venture_database_find_one(f->db, query, NULL);
	g_assert_nonnull(allocation);
	g_object_set(refund, "customer-id", venture_entity_get_id(customer), "allocation-id", venture_entity_get_id(allocation), NULL);
	field(refund, "amount", "100.00 USD");
	field(refund, "date", refunded_on);
	save(f, refund);
}

static void
test_revoked_event(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) program = g_object_new(VENTURE_TYPE_REFERRAL_PROGRAM, "organization-id", f->org,
		"name", "Friends", "active", TRUE, "reward-kind", VENTURE_REFERRAL_REWARD_CREDIT,
		"refund-window-days", (gint64)30, NULL);
	g_autoptr(VentureEntity) referrer = NULL;
	g_autoptr(VentureEntity) referred = NULL;
	g_autoptr(VentureEntity) referral = NULL;
	(void)data;
	field(program, "reward-amount", "25 USD");
	save(f, program);
	referrer = g_object_new(VENTURE_TYPE_COMPANY, "organization-id", f->org, "name", "Referrer", NULL);
	save(f, referrer);
	referred = g_object_new(VENTURE_TYPE_COMPANY, "organization-id", f->org, "name", "Referred", NULL);
	save(f, referred);
	referral = g_object_new(VENTURE_TYPE_REFERRAL, "organization-id", f->org,
		"program-id", venture_entity_get_id(program), "referrer-company-id", venture_entity_get_id(referrer),
		"company-id", venture_entity_get_id(referred), "status", VENTURE_REFERRAL_WON, NULL);
	save(f, referral);
	g_assert_cmpint(event_count(f, "referral_reward.revoked"), ==, 0);
	refund_on(f, referred, "2026-01-01", "2026-01-20");
	g_assert_cmpint(event_count(f, "referral_reward.revoked"), ==, 1);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	venture_entity_registry_register_builtins(venture_entity_registry_get_default());
	g_test_add("/business-events/referral-reward-revoked", Fixture, NULL, setup, test_revoked_event, teardown);
	return g_test_run();
}
