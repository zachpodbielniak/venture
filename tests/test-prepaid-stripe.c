/* SPDX-License-Identifier: AGPL-3.0-or-later */
int existing_stripe_test_main(int argc, char **argv);
#define main existing_stripe_test_main
#include "test-stripe.c"
#undef main
#include "test-prepaid-provider.inc"

static void prepay_due(Fixture *f, gboolean second)
{
	g_autoptr(GDateTime) now = venture_time_now(), past = g_date_time_add_days(now, second ? -31 : -1);
	g_autofree gchar *date = venture_time_to_string(past), *sql = NULL;
	g_autoptr(GError) error = NULL;
	sql = g_strdup_printf("UPDATE stripe_prepayments SET next_attempt_at='%s'%s", date,
		second ? ", published_at=created_at" : "");
	g_assert_true(venture_database_execute(f->database, sql, NULL, &error)); g_assert_no_error(error);
	if (second) {
		g_clear_pointer(&sql, g_free); sql = g_strdup_printf("UPDATE stripe_prepayments SET published_at='%s'", date);
		g_assert_true(venture_database_execute(f->database, sql, NULL, &error)); g_assert_no_error(error);
	}
}

static void prepaid_setup(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GObject) transport = g_object_new(prepaid_transport_get_type(), NULL);
	PrepaidTransport *fixture = (PrepaidTransport *)transport;
	g_autoptr(JsonNode) settings = binding_settings("whsec_fixture");
	g_autoptr(GBytes) key = g_bytes_new_take(g_malloc0(32), 32);
	g_autoptr(VentureIntegrationConnection) connection = NULL;
	g_autoptr(VentureStripeService) provider = NULL;
	g_autoptr(VentureEntity) plan = record_new(f, "plan"), price = record_new(f, "plan_price");
	g_autoptr(VentureEntity) instruction = record_new(f, "billing_request");
	g_autoptr(JsonNode) first = NULL, again = NULL;
	g_autoptr(GPtrArray) invoices = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	gint64 subscription = 0;
	const gchar *scenario = data ? data : "installments";
	const gchar *path = g_str_has_prefix(scenario, "installments") ? "installments" :
		g_str_has_prefix(scenario, "split") || !strcmp(scenario, "wrong-capture") ? "split" :
		!strcmp(scenario, "lost-confirmation") || !strcmp(scenario, "setup-expired") || g_str_has_prefix(scenario, "wrong-") ? "single" : scenario;
	g_autoptr(JsonNode) published = NULL, replay = NULL;
	g_autoptr(JsonArray) shares = NULL;
	g_autoptr(VentureMoney) balance = NULL;
	gint64 invoice;
	fixture->bank = !strcmp(path, "ach");
	fixture->capture_failure = !strcmp(scenario, "split-capture-failure") || !strcmp(scenario, "split-lost-refund") ? 2 : 0;
	fixture->decline = !strcmp(scenario, "split-auth-failure") ? 2 : 0;
	fixture->drop_confirmation = !strcmp(scenario, "lost-confirmation");
	fixture->drop_refund = !strcmp(scenario, "split-lost-refund");
	fixture->mismatch = !strcmp(scenario, "wrong-method") ? 1 : !strcmp(scenario, "wrong-amount") ? 2 :
		!strcmp(scenario, "wrong-customer") ? 3 : !strcmp(scenario, "wrong-capture") ? 4 : 0;
	if (!strcmp(scenario, "split-three")) {
		shares = json_array_new();
		json_array_add_int_element(shares, 500); json_array_add_int_element(shares, 700); json_array_add_int_element(shares, 800);
	}
	json_object_set_string_member(json_node_get_object(settings), "api_version", "2024-06-20");
	json_object_set_boolean_member(json_node_get_object(settings), "ach_enabled", TRUE);
	g_assert_true(venture_integration_service_set_key(venture_integration_service_get(f->database), key, &error));
	connection = venture_stripe_settings_configure(f->database, f->organization_id, settings, 0, 0, STRIPE_TRANSPORT(transport), NULL, &error);
	g_assert_no_error(error);
	provider = venture_stripe_service_for_organization(f->database, f->organization_id, STRIPE_TRANSPORT(transport), &error);
	g_assert_no_error(error);
	venture_context_set_stripe_service(f->context, provider);
	g_object_set(plan, "name", "Example plan", "active", TRUE, NULL); save(f, plan);
	g_object_set(price, "plan-id", venture_entity_get_id(plan), "currency", "USD", "active", TRUE, "product-id", f->product_id, NULL);
	g_assert_true(venture_entity_set_field_from_string(price, "interval", "year", &error));
	money_field(price, "amount", "10 USD"); save(f, price);
	g_object_set(instruction, "action", "start", "company-id", f->customer_id,
		"plan-price-id", venture_entity_get_id(price), "defer-days", (gint64)14,
		"external-id", "prepaid:example", "at", now, NULL); save(f, instruction);
	g_object_get(instruction, "subscription-id", &subscription, NULL);
	first = venture_stripe_service_prepay(provider, subscription, path, shares, FALSE, NULL, &error);
	g_assert_no_error(error); g_assert_nonnull(first);
	fixture->expire_setup = !strcmp(scenario, "setup-expired");
	again = venture_stripe_service_prepay(provider, subscription, path, NULL, FALSE, NULL, &error);
	g_assert_no_error(error); g_assert_nonnull(again);
	g_assert_cmpint(json_object_get_int_member(json_node_get_object(first), "prepayment_id"), ==,
		json_object_get_int_member(json_node_get_object(again), "prepayment_id"));
	invoices = rows(f, "invoice"); g_assert_cmpuint(invoices->len, ==, 0);
	if (!strcmp(scenario, "setup-expired")) { g_assert_cmpuint(fixture->sessions, ==, 2); return; }
	if (!data) {
		gint64 group_id = json_object_get_int_member(json_node_get_object(first), "prepayment_id");
		g_autoptr(VentureEntity) forged = venture_database_get(f->database, VENTURE_TYPE_STRIPE_PREPAYMENT, group_id, &error);
		g_autofree gchar *state = NULL;
		g_assert_no_error(error); g_object_set(forged, "state", "paid", NULL);
		g_assert_false(venture_database_save(f->database, forged, NULL, &error));
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION); g_clear_error(&error);
		g_clear_object(&forged);
		forged = venture_database_get(f->database, VENTURE_TYPE_STRIPE_PREPAYMENT, group_id, &error);
		g_assert_no_error(error); g_object_get(forged, "state", &state, NULL);
		g_assert_cmpstr(state, ==, "setup");
		return;
	}
	published = venture_stripe_service_prepay(provider, subscription, path, NULL, now, NULL, &error);
	if (fixture->mismatch) {
		g_assert_null(published); g_assert_nonnull(error);
		g_assert_cmpuint(fixture->confirms, ==, 0);
		return;
	}
	if (!strcmp(scenario, "lost-confirmation") || !strcmp(scenario, "split-lost-refund")) {
		g_assert_null(published); g_assert_nonnull(error); g_clear_error(&error);
		/* Provider idempotency retention can expire while the process is down. */
		g_hash_table_remove_all(fixture->keys);
		prepay_due(f, FALSE);
		published = venture_stripe_service_prepay(provider, subscription, path, NULL, now, NULL, &error);
	}
	g_assert_no_error(error); g_assert_nonnull(published);
	invoice = json_object_get_int_member(json_node_get_object(published), "invoice_id"); g_assert_cmpint(invoice, >, 0);
	{
		g_autoptr(VentureEntity) enrolled = venture_database_get(f->database, VENTURE_TYPE_CUSTOMER_SUBSCRIPTION, subscription, &error);
		g_autoptr(GDateTime) end = NULL, expected_end = g_date_time_add_months(now, 24);
		g_assert_no_error(error); g_object_get(enrolled, "current-period-end", &end, NULL);
		g_assert_cmpint(g_date_time_compare(end, expected_end), ==, 0);
	}
	if (!strcmp(path, "installments")) {
		g_autoptr(VentureEntity) invoice_row = venture_database_get(f->database, VENTURE_TYPE_INVOICE, invoice, &error);
		g_autoptr(GDateTime) due_at = NULL, expected_due = g_date_time_add_days(now, 30);
		g_assert_no_error(error); g_object_get(invoice_row, "due-at", &due_at, NULL);
		g_assert_cmpint(g_date_time_compare(due_at, expected_due), ==, 0);
		g_assert_false(venture_stripe_service_can_checkout(provider, invoice, &error));
		g_assert_nonnull(error); g_clear_error(&error);
		balance = venture_settlement_service_invoice_balance(venture_settlement_service_get(f->database), invoice, NULL, &error);
		g_assert_no_error(error); g_assert_cmpint(venture_money_get_amount(balance), ==, 1000);
		g_assert_cmpuint(fixture->confirms, ==, 1);
		if (!strcmp(scenario, "installments-manual")) {
			g_autoptr(VenturePayment) manual = venture_payment_new();
			g_autoptr(GDateTime) paid_at = venture_time_now();
			venture_entity_set_organization_id(VENTURE_ENTITY(manual), f->organization_id);
			g_object_set(manual, "customer-id", f->customer_id, "invoice-id", invoice,
				"amount", balance, "date", paid_at, "method", "bank", NULL);
			venture_settlement_service_apply_payment(venture_settlement_service_get(f->database), manual, NULL, NULL, &error);
			g_assert_no_error(error);
			prepay_due(f, TRUE);
			g_clear_pointer(&replay, json_node_unref);
			replay = venture_stripe_service_prepay(provider, subscription, path, NULL, NULL, NULL, &error);
			g_assert_null(replay); g_assert_nonnull(error);
			g_assert_cmpuint(fixture->confirms, ==, 1);
			return;
		}
		fixture->decline = 2; prepay_due(f, TRUE);
		g_clear_pointer(&published, json_node_unref);
		published = venture_stripe_service_prepay(provider, subscription, path, NULL, FALSE, NULL, &error);
		g_assert_no_error(error); g_assert_cmpstr(json_object_get_string_member(json_node_get_object(published), "state"), ==, "waiting");
		fixture->decline = 0; prepay_due(f, FALSE);
		/* Reconstruct the service to prove the retry is persisted in the database. */
		g_clear_object(&provider);
		provider = venture_stripe_service_for_organization(f->database, f->organization_id, STRIPE_TRANSPORT(transport), &error);
		g_assert_no_error(error);
		venture_context_set_stripe_service(f->context, provider);
		{
			guint tick;
			venture_stripe_collection_start(f->context);
			for (tick = 0; tick < 120; tick++) {
				g_autoptr(VentureMoney) remaining = NULL;
				while (g_main_context_iteration(NULL, FALSE));
				remaining = venture_settlement_service_invoice_balance(venture_settlement_service_get(f->database), invoice, NULL, &error);
				g_assert_no_error(error);
				if (!venture_money_get_amount(remaining)) break;
				g_usleep(G_TIME_SPAN_SECOND / 10);
			}
			venture_stripe_collection_stop(f->context);
			g_assert_cmpuint(tick, <, 120);
		}
	}
	if (!strcmp(path, "ach") || !strcmp(path, "wire")) {
		g_assert_cmpstr(json_object_get_string_member(json_node_get_object(published), "state"), ==, "waiting");
		if (!strcmp(path, "wire")) g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_node_get_object(published), "payment_links")), ==, 1);
		fixture->settled = TRUE; prepay_due(f, FALSE);
	}
	replay = venture_stripe_service_prepay(provider, subscription, path, NULL, now, NULL, &error);
	g_assert_no_error(error); g_assert_nonnull(replay);
	g_assert_cmpint(json_object_get_int_member(json_node_get_object(replay), "invoice_id"), ==, invoice);
	g_clear_pointer(&balance, venture_money_free);
	balance = venture_settlement_service_invoice_balance(venture_settlement_service_get(f->database), invoice, NULL, &error);
	g_assert_no_error(error);
	if (fixture->decline || fixture->capture_failure) {
		g_assert_cmpstr(json_object_get_string_member(json_node_get_object(replay), "state"), ==, "failed");
		g_assert_cmpuint(fixture->refunds, ==, fixture->capture_failure ? 1 : 0);
		g_assert_cmpint(venture_money_get_amount(balance), ==, 2000);
	} else {
		g_assert_cmpstr(json_object_get_string_member(json_node_get_object(replay), "state"), ==, "paid");
		g_assert_cmpint(venture_money_get_amount(balance), ==, 0);
		g_assert_cmpuint(fixture->intents, ==, shares ? 3 : !strcmp(path, "split") || !strcmp(path, "installments") ? 2 : 1);
	}
	g_clear_pointer(&invoices, g_ptr_array_unref); invoices = rows(f, "invoice"); g_assert_cmpuint(invoices->len, ==, 1);
	g_test_message("%s: one invoice; %u intents, %u captures, %u refunds; balance=%" G_GINT64_FORMAT "; state=%s",
		scenario, fixture->intents, fixture->captures, fixture->refunds, venture_money_get_amount(balance),
		json_object_get_string_member(json_node_get_object(replay), "state"));
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/prepaid/setup-without-charge", Fixture, NULL, set_up, prepaid_setup, tear_down);
	g_test_add("/prepaid/setup-expired", Fixture, "setup-expired", set_up, prepaid_setup, tear_down);
	g_test_add("/prepaid/single", Fixture, "single", set_up, prepaid_setup, tear_down);
	g_test_add("/prepaid/installments-retry", Fixture, "installments", set_up, prepaid_setup, tear_down);
	g_test_add("/prepaid/installments-manual", Fixture, "installments-manual", set_up, prepaid_setup, tear_down);
	g_test_add("/prepaid/ach", Fixture, "ach", set_up, prepaid_setup, tear_down);
	g_test_add("/prepaid/wire", Fixture, "wire", set_up, prepaid_setup, tear_down);
	g_test_add("/prepaid/split", Fixture, "split", set_up, prepaid_setup, tear_down);
	g_test_add("/prepaid/split-three", Fixture, "split-three", set_up, prepaid_setup, tear_down);
	g_test_add("/prepaid/split-auth-failure", Fixture, "split-auth-failure", set_up, prepaid_setup, tear_down);
	g_test_add("/prepaid/split-capture-failure", Fixture, "split-capture-failure", set_up, prepaid_setup, tear_down);
	g_test_add("/prepaid/split-lost-refund", Fixture, "split-lost-refund", set_up, prepaid_setup, tear_down);
	g_test_add("/prepaid/lost-confirmation", Fixture, "lost-confirmation", set_up, prepaid_setup, tear_down);
	g_test_add("/prepaid/wrong-method", Fixture, "wrong-method", set_up, prepaid_setup, tear_down);
	g_test_add("/prepaid/wrong-amount", Fixture, "wrong-amount", set_up, prepaid_setup, tear_down);
	g_test_add("/prepaid/wrong-customer", Fixture, "wrong-customer", set_up, prepaid_setup, tear_down);
	g_test_add("/prepaid/wrong-capture", Fixture, "wrong-capture", set_up, prepaid_setup, tear_down);
	return g_test_run();
}
