/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <string.h>
#include "venture-test-accounting.h"

typedef struct {
	VentureConfig *config;
	VentureDatabase *database;
	VentureContext *context;
	gint64 organization;
} Fixture;

static void
setup(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	(void)data;
	f->config = venture_config_new();
	f->database = venture_test_accounting_database(&error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->database, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->database);
	f->organization = venture_context_get_default_organization_id(f->context);
}

static void
teardown(Fixture *f, gconstpointer data)
{
	(void)data;
	g_clear_object(&f->context);
	venture_test_accounting_database_cleanup(f->database);
	g_clear_object(&f->database);
	g_clear_object(&f->config);
}

static void
save(Fixture *f, gpointer record)
{
	g_autoptr(GError) error = NULL;
	gboolean ok = venture_database_save(f->database, VENTURE_ENTITY(record), NULL, &error);
	g_assert_no_error(error);
	g_assert_true(ok);
}

static void
field(gpointer record, const gchar *name, const gchar *text)
{
	g_autoptr(GError) error = NULL;
	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(record), name, text, &error));
	g_assert_no_error(error);
}

static void
assert_events(Fixture *f, const gchar *name, gint64 expected)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_BUSINESS_EVENT);
	g_autoptr(GError) error = NULL;
	venture_query_set_organization(query, f->organization);
	venture_query_add_filter_string(query, "event", VENTURE_FILTER_OP_EQ, name, NULL);
	g_assert_cmpint(venture_database_count(f->database, query, &error), ==, expected);
	g_assert_no_error(error);
}

static void
quote_action(Fixture *f, VentureQuote *quote, const gchar *action)
{
	g_autoptr(VentureEntity) fresh = venture_database_get(f->database, VENTURE_TYPE_QUOTE, venture_entity_get_id(VENTURE_ENTITY(quote)), NULL);
	g_autoptr(VentureQuoteAction) request = g_object_new(VENTURE_TYPE_QUOTE_ACTION,
		"organization-id", f->organization, "quote-id", venture_entity_get_id(fresh),
		"expected-version", venture_entity_get_version(fresh), "action", action, "accepted-by", "Example person", NULL);
	save(f, request);
}

static void
test_commercial_outcomes(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureNewsletter) newsletter = g_object_new(VENTURE_TYPE_NEWSLETTER, "organization-id", f->organization, "name", "Example newsletter", NULL);
	g_autoptr(VentureSubscriber) subscriber = NULL;
	g_autoptr(VentureDeal) deal = g_object_new(VENTURE_TYPE_DEAL, "organization-id", f->organization, "name", "Example opportunity", NULL);
	g_autoptr(VentureCompany) company = g_object_new(VENTURE_TYPE_COMPANY, "organization-id", f->organization, "name", "Example Co", "email", "buyer@example.test", NULL);
	g_autoptr(VentureQuote) quote = NULL;
	g_autoptr(VentureQuoteLine) line = NULL;
	(void)data;
	save(f, newsletter);
	subscriber = g_object_new(VENTURE_TYPE_SUBSCRIBER, "organization-id", f->organization,
		"newsletter-id", venture_entity_get_id(VENTURE_ENTITY(newsletter)), "email", "reader@example.test", NULL);
	save(f, subscriber);
	assert_events(f, "subscriber.created", 1);
	g_object_set(subscriber, "name", "Updated reader", NULL); save(f, subscriber);
	assert_events(f, "subscriber.created", 1);
	save(f, deal);
	assert_events(f, "deal.won", 0);
	{
		g_autoptr(VentureQuery) stages = venture_query_new(VENTURE_TYPE_PIPELINE_STAGE);
		g_autoptr(VentureEntity) won = NULL;
		g_autoptr(VentureDeal) moved = NULL;
		g_autoptr(GError) error = NULL;
		venture_query_set_organization(stages, f->organization);
		venture_query_add_filter_string(stages, "kind", VENTURE_FILTER_OP_EQ, "won", NULL);
		won = venture_database_find_one(f->database, stages, &error);
		g_assert_no_error(error); g_assert_nonnull(won);
		moved = venture_deal_service_move_stage(venture_database_get_deal_service(f->database), deal,
			venture_entity_get_id(won), "Accepted", NULL, &error);
		g_assert_no_error(error); g_assert_nonnull(moved);
		g_set_object(&deal, moved);
	}
	assert_events(f, "deal.won", 1);
	g_object_set(deal, "name", "Updated opportunity", NULL); save(f, deal);
	assert_events(f, "deal.won", 1);
	save(f, company);
	quote = g_object_new(VENTURE_TYPE_QUOTE, "organization-id", f->organization, "number", "Q-EXAMPLE",
		"company-id", venture_entity_get_id(VENTURE_ENTITY(company)), "currency", "USD", NULL);
	save(f, quote);
	line = g_object_new(VENTURE_TYPE_QUOTE_LINE, "organization-id", f->organization,
		"quote-id", venture_entity_get_id(VENTURE_ENTITY(quote)), "description", "Example service", "quantity", (gint64)1, NULL);
	field(line, "unit-price", "100.00 USD"); save(f, line);
	quote_action(f, quote, "send");
	assert_events(f, "quote.sent", 1);
	quote_action(f, quote, "accept");
	assert_events(f, "quote.accepted", 1);
	assert_events(f, "quote.sent", 1);
}

static void
test_payment_outcomes(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureCompany) company = g_object_new(VENTURE_TYPE_COMPANY, "organization-id", f->organization, "name", "Example Co", NULL);
	g_autoptr(VentureInvoice) invoice = NULL;
	g_autoptr(VentureInvoiceLine) line = NULL;
	g_autoptr(VenturePayment) payment = NULL;
	g_autoptr(VentureRefund) refund = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_PAYMENT_ALLOCATION);
	g_autoptr(VentureEntity) allocation = NULL;
	(void)data;
	save(f, company);
	invoice = g_object_new(VENTURE_TYPE_INVOICE, "organization-id", f->organization, "number", "INV-EXAMPLE",
		"company-id", venture_entity_get_id(VENTURE_ENTITY(company)), NULL);
	field(invoice, "issued-at", "2026-07-01"); field(invoice, "due-at", "2026-07-01"); save(f, invoice);
	line = g_object_new(VENTURE_TYPE_INVOICE_LINE, "organization-id", f->organization,
		"invoice-id", venture_entity_get_id(VENTURE_ENTITY(invoice)), "description", "Example service", "quantity", 1.0, NULL);
	field(line, "unit-price", "100.00 USD"); save(f, line);
	g_object_set(invoice, "status", VENTURE_INVOICE_STATUS_SENT, NULL); save(f, invoice);
	assert_events(f, "invoice.paid", 0);
	payment = g_object_new(VENTURE_TYPE_PAYMENT, "organization-id", f->organization,
		"customer-id", venture_entity_get_id(VENTURE_ENTITY(company)), "invoice-id", venture_entity_get_id(VENTURE_ENTITY(invoice)), "method", "manual", NULL);
	field(payment, "amount", "100.00 USD"); field(payment, "date", "2026-07-02"); save(f, payment);
	assert_events(f, "payment.received", 1); assert_events(f, "invoice.paid", 1);
	allocation = venture_database_find_one(f->database, query, NULL);
	g_assert_nonnull(allocation);
	refund = g_object_new(VENTURE_TYPE_REFUND, "organization-id", f->organization,
		"customer-id", venture_entity_get_id(VENTURE_ENTITY(company)), "allocation-id", venture_entity_get_id(allocation), NULL);
	field(refund, "amount", "100.00 USD"); field(refund, "date", "2026-07-03"); save(f, refund);
	assert_events(f, "payment.refunded", 1); assert_events(f, "invoice.paid", 1);
}

static void
test_booking_outcomes(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureBookingService) service = venture_booking_service_new(f->database);
	g_autoptr(VentureBookingPage) page = g_object_new(VENTURE_TYPE_BOOKING_PAGE, "organization-id", f->organization,
		"title", "Example appointment", "slug", "example", "owner", "calendar-owner", "timezone", "UTC",
		"duration-minutes", (gint64)30, "availability", "{\"mon\":\"09:00-10:00\"}", "horizon-days", (gint64)2, "active", TRUE, NULL);
	g_autoptr(VentureUser) user = g_object_new(VENTURE_TYPE_USER, "username", "calendar-owner", "active", TRUE, NULL);
	g_autoptr(GDateTime) now = venture_time_from_string("2026-09-21T08:00:00Z", NULL);
	g_autoptr(VentureEntity) hold = NULL, booked = NULL, fresh = NULL, cancelled = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *url = NULL;
	(void)data;
	save(f, user); save(f, page);
	hold = venture_booking_service_reserve(service, VENTURE_ENTITY(page), "2026-09-21T09:00:00Z", 900, now, &error);
	g_assert_no_error(error); g_assert_nonnull(hold);
	assert_events(f, "booking.created", 0);
	booked = venture_booking_service_confirm(service, hold, "Example person", "person@example.test", NULL, NULL, now, NULL, &error);
	g_assert_no_error(error); g_assert_nonnull(booked);
	assert_events(f, "booking.created", 1);
	fresh = venture_database_get(f->database, VENTURE_TYPE_BOOKING_RESERVATION, venture_entity_get_id(hold), &error);
	g_assert_no_error(error);
	url = venture_booking_service_manage_url(service, fresh, "https://booking.example.test", &error);
	g_assert_no_error(error); g_assert_nonnull(url);
	cancelled = venture_booking_service_manage(service, VENTURE_ENTITY(page), strrchr(url, '/') + 1, NULL, now, NULL, &error);
	g_assert_no_error(error); g_assert_nonnull(cancelled);
	assert_events(f, "booking.cancelled", 1);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/business-event-outcomes/commercial", Fixture, NULL, setup, test_commercial_outcomes, teardown);
	g_test_add("/business-event-outcomes/payments", Fixture, NULL, setup, test_payment_outcomes, teardown);
	g_test_add("/business-event-outcomes/bookings", Fixture, NULL, setup, test_booking_outcomes, teardown);
	return g_test_run();
}
