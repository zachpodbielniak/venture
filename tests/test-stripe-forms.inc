/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "../src/forms/venture-forms-private.h"

static void paid_form_saved(VentureDatabase *database, VentureEntity *entity, gboolean created, gpointer data)
{
	(void)database; (void)created;
	if (VENTURE_IS_FORM_SUBMISSION(entity)) (*(guint *)data)++;
}

/* A public price-looking answer cannot influence Checkout. The ordinary
 * response signal is the automation boundary, and must follow settlement. */
static void test_paid_form(Fixture *f, gconstpointer data)
{
	const gchar *mode = data;
	gboolean booking = g_str_has_prefix(mode, "booking-");
	gboolean reordered = g_strcmp0(mode, "expired-paid") == 0;
	gboolean deadline = g_strcmp0(mode, "deadline") == 0;
	gboolean erased = g_strcmp0(mode, "erased") == 0, uncertain = g_strcmp0(mode, "uncertain") == 0 || deadline;
	guint attempts = uncertain ? 2 : 1;
	gboolean expired = strstr(mode, "expired") != NULL, failed = strstr(mode, "failed") != NULL, late = strstr(mode, "late") != NULL;
	g_autoptr(VentureEntity) page = NULL;
	g_autoptr(VentureBookingService) booking_service = NULL;
	g_autofree gchar *slot = NULL;
	g_autoptr(VentureEntity) form = record_new(f, "form"), price = record_new(f, "form_price"), version = NULL, response = NULL;
	g_autoptr(GObject) transport = g_object_new(fake_transport_get_type(), NULL);
	FakeTransport *fake = (FakeTransport *)transport;
	g_autoptr(VentureStripeService) service = NULL;
	g_autoptr(VentureMoney) unit = venture_money_new_for_currency(10000, "USD");
	g_autoptr(GDateTime) now = venture_time_now(), earlier = g_date_time_add_seconds(now, -10);
	g_autoptr(GHashTable) answers = NULL;
	g_autoptr(JsonObject) errors = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *nonce = NULL, *ticket = NULL, *body = NULL, *sig = NULL;
	g_autoptr(GBytes) bytes = NULL;
	g_autoptr(GPtrArray) responses = NULL, payments = NULL, checkouts = NULL;
	VentureFormsOutcome outcome;
	guint saved = 0, i;
	gboolean ok;
	gulong handler;
	g_object_set(form, "name", "Paid intake", "title", "Admission", "state", VENTURE_FORM_LIVE,
		"payment-enabled", TRUE, "public-origin", "https://forms.example.test", NULL); save(f, form);
	for (i = 0; i < 2; i++)
	{
		g_autoptr(VentureEntity) field = record_new(f, "form_field");
		g_object_set(field, "form-id", venture_entity_get_id(form), "key", i == 0 ? "name" : "email",
			"label", i == 0 ? "Name" : "Email", "kind", i == 0 ? VENTURE_FORM_FIELD_SHORT_TEXT : VENTURE_FORM_FIELD_EMAIL,
			"required", TRUE, "position", (gint64)i, NULL); save(f, field);
	}
	{
		g_autoptr(VentureEntity) secret = record_new(f, "form_field");
		g_object_set(secret, "form-id", venture_entity_get_id(form), "key", "private_answer", "label", "Private", "kind", VENTURE_FORM_FIELD_SHORT_TEXT, "sensitive", TRUE, "position", (gint64)3, NULL); save(f, secret);
	}
	if (booking)
	{
		g_autoptr(VentureEntity) field = record_new(f, "form_field");
		g_autoptr(JsonNode) slots = NULL;
		page = record_new(f, "booking_page");
		g_object_set(page, "title", "Paid meeting", "slug", "paid-meeting", "owner", "owner", "duration-minutes", (gint64)30, "timezone", "UTC", "horizon-days", (gint64)3, "active", TRUE,
			"availability", "{\"mon\":\"00:00-23:30\",\"tue\":\"00:00-23:30\",\"wed\":\"00:00-23:30\",\"thu\":\"00:00-23:30\",\"fri\":\"00:00-23:30\",\"sat\":\"00:00-23:30\",\"sun\":\"00:00-23:30\"}", NULL); save(f, page);
		g_object_set(field, "form-id", venture_entity_get_id(form), "key", "time", "label", "Meeting time", "kind", VENTURE_FORM_FIELD_BOOKING, "required", TRUE, "position", (gint64)4, "booking-page-id", venture_entity_get_id(page), NULL); save(f, field);
		booking_service = venture_booking_service_new(f->database);
		slots = venture_booking_service_slots(booking_service, page, now, &error); g_assert_no_error(error);
		g_assert_cmpuint(json_array_get_length(json_node_get_array(slots)), >, 3);
		slot = g_strdup(json_object_get_string_member(json_array_get_object_element(json_node_get_array(slots), 3), "start"));
	}
	g_object_set(price, "form-id", venture_entity_get_id(form), "key", "admission", "name", "Admission",
		"product-id", f->product_id, "unit-price", unit, NULL); save(f, price);
	version = venture_forms_publish(f->database, form, NULL, &error); g_assert_no_error(error); g_assert_nonnull(version);
	g_clear_object(&form); form = venture_database_get(f->database, VENTURE_TYPE_FORM, venture_forms_get_int(version, "form-id"), &error); g_assert_no_error(error);
	service = venture_stripe_service_new(f->database, f->organization_id, STRIPE_TRANSPORT(transport), &error); g_assert_no_error(error);
	venture_context_set_stripe_service(f->context, service);
	fake->reservation_database = f->database;
	nonce = venture_forms_payment_nonce(form); ticket = venture_forms_ticket_new_for_version(form, 1, earlier);
	body = g_strdup_printf("name=Guest&email=guest%%40example.test&%s=%s&%s=%s", VENTURE_FORMS_TICKET, ticket, VENTURE_FORMS_PAYMENT_NONCE, nonce);
	answers = venture_forms_answers_from_urlencoded(body, strlen(body), &error); g_assert_no_error(error);
	venture_forms_answers_add(answers, "private_answer", "PAYMENT-SECRET");
	if (booking) venture_forms_answers_add(answers, "time", slot);
	handler = g_signal_connect(f->database, "entity-saved", G_CALLBACK(paid_form_saved), &saved);
	venture_forms_answers_add(answers, "amount", "1");
	ok = venture_forms_submit(f->database, form, answers, NULL, now, &outcome, &response, &errors, &error); g_assert_no_error(error); g_assert_true(ok);
	g_assert_cmpint(outcome, ==, VENTURE_FORMS_INVALID); g_assert_cmpuint(fake->calls, ==, 0); g_hash_table_remove(answers, "amount");
	venture_forms_answers_add(answers, "currency", "JPY");
	g_clear_pointer(&errors, json_object_unref);
	ok = venture_forms_submit(f->database, form, answers, NULL, now, &outcome, &response, &errors, &error);
	g_assert_no_error(error); g_assert_true(ok); g_assert_cmpint(outcome, ==, VENTURE_FORMS_INVALID);
	g_assert_cmpuint(fake->calls, ==, 0); g_hash_table_remove(answers, "currency");
	if (uncertain)
	{
		g_autoptr(GPtrArray) invoices = NULL;
		fake->checkout_drop = !deadline; fake->checkout_timeout = deadline;
		g_clear_pointer(&errors, json_object_unref);
		/* A retained retry identity is useful only if an operator can also
		 * distinguish provider uncertainty from invalid customer answers. */
		g_test_expect_message("Venture", G_LOG_LEVEL_MESSAGE,
			deadline ? "Form payment checkout failed: invoice=* cause=provider deadline exhausted*" :
			"Form payment checkout failed: invoice=* cause=provider transport failure*");
		ok = venture_forms_submit(f->database, form, answers, NULL, now, &outcome, &response, &errors, &error);
		g_test_assert_expected_messages();
		g_assert_no_error(error); g_assert_true(ok); g_assert_cmpint(outcome, ==, VENTURE_FORMS_INVALID);
		g_assert_nonnull(json_object_get_member(errors, "_form")); g_assert_cmpuint(saved, ==, 0);
		invoices = rows(f, "invoice"); g_assert_cmpuint(invoices->len, ==, 1);
		fake->checkout_drop = FALSE; fake->checkout_timeout = FALSE;
	}
	g_clear_pointer(&errors, json_object_unref);
	ok = venture_forms_submit(f->database, form, answers, NULL, now, &outcome, &response, &errors, &error); g_assert_no_error(error); g_assert_true(ok);
	g_assert_cmpint(outcome, ==, VENTURE_FORMS_PAYMENT); g_assert_cmpuint(saved, ==, 0);
	g_assert_cmpstr(g_object_get_data(G_OBJECT(response), VENTURE_FORMS_PAYMENT_REDIRECT), ==, uncertain ? "https://checkout.stripe.com/second" : "https://checkout.stripe.com/offline");
	responses = rows(f, "form_submission"); g_assert_cmpuint(responses->len, ==, 0);
	checkouts = rows(f, "stripe_checkout"); g_assert_cmpuint(checkouts->len, ==, 1);
	/* A caller naming a checkout from another organization gets no evidence. */
	g_assert_false(venture_forms_payment_reconcile(f->database, f->organization_id + 1,
		venture_entity_get_id(g_ptr_array_index(checkouts, 0)), now, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND); g_clear_error(&error);
	g_assert_cmpuint(saved, ==, 0);
	{
		g_autoptr(VentureMoney) expected = NULL;
		g_object_get(g_ptr_array_index(checkouts, 0), "expected", &expected, NULL);
		g_assert_cmpint(venture_money_get_amount(expected), ==, 10000); g_assert_cmpstr(venture_money_get_currency(expected), ==, "USD");
	}
	{
		g_autoptr(GPtrArray) retained = rows(f, "form_payment");
		VentureEntity *pending_payment = g_ptr_array_index(retained, 0);
		g_autoptr(JsonNode) wire = venture_serializable_to_json(VENTURE_SERIALIZABLE(pending_payment), FALSE);
		g_autofree gchar *public_text = json_to_string(wire, FALSE);
		g_assert_null(strstr(public_text, "PAYMENT-SECRET")); g_assert_null(strstr(public_text, "guest@example.test"));
		g_assert_false(venture_database_delete(f->database, pending_payment, NULL, &error)); g_assert_nonnull(error); g_clear_error(&error);
		g_assert_false(venture_database_purge(f->database, pending_payment, NULL, &error)); g_assert_nonnull(error); g_clear_error(&error);
	}
	/* A transport retry cannot make a second invoice or provider session. */
	g_clear_object(&response);
	ok = venture_forms_submit(f->database, form, answers, NULL, now, &outcome, &response, &errors, &error); g_assert_no_error(error); g_assert_true(ok);
	g_assert_cmpint(outcome, ==, VENTURE_FORMS_PAYMENT); g_assert_cmpuint(fake->checkouts, ==, attempts);
	{
		g_autoptr(JsonNode) exported = venture_forms_export_person(f->database, f->organization_id, "guest@example.test", &error);
		g_autofree gchar *text = exported != NULL ? json_to_string(exported, FALSE) : NULL;
		g_assert_no_error(error); g_assert_nonnull(text); g_assert_nonnull(strstr(text, "PAYMENT-SECRET")); g_assert_null(strstr(text, nonce));
	}
	if (erased)
	{
		g_autoptr(JsonNode) result = venture_forms_erase_person(f->database, f->organization_id, "guest@example.test", NULL, &error);
		g_assert_no_error(error); g_assert_nonnull(result);
	}
	if (g_strcmp0(mode, "limits") == 0)
	{
		g_autofree gchar *second_nonce = venture_forms_payment_nonce(form);
		g_autoptr(VentureEntity) other = NULL;
		g_object_set(form, "unique-email-field", "email", NULL); save(f, form);
		g_hash_table_remove(answers, VENTURE_FORMS_PAYMENT_NONCE); venture_forms_answers_add(answers, VENTURE_FORMS_PAYMENT_NONCE, second_nonce);
		ok = venture_forms_submit(f->database, form, answers, NULL, now, &outcome, &other, &errors, &error);
		g_assert_no_error(error); g_assert_true(ok); g_assert_cmpint(outcome, ==, VENTURE_FORMS_INVALID);
		g_clear_pointer(&errors, json_object_unref);
		g_object_set(form, "unique-email-field", "", "response-limit", (gint64)1, NULL); save(f, form);
		ok = venture_forms_submit(f->database, form, answers, NULL, now, &outcome, &other, &errors, &error);
		g_assert_false(ok); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND); g_clear_error(&error);
		g_assert_cmpuint(fake->checkouts, ==, attempts);
	}
	if (booking)
	{
		g_autoptr(GPtrArray) meetings = rows(f, "activity");
		g_assert_cmpuint(meetings->len, ==, 0); g_assert_false(fake->checkout_ach);
		if (late)
		{
			g_autoptr(GPtrArray) holds = rows(f, "booking_reservation");
			g_assert_cmpuint(holds->len, ==, 1);
			g_assert_true(venture_booking_service_release(booking_service, g_ptr_array_index(holds, 0), &error)); g_assert_no_error(error);
		}
	}
	g_clear_pointer(&body, g_free);
	body = g_strdup_printf("{\"id\":\"evt_form\",\"type\":\"checkout.session.%s\",\"data\":{\"object\":{\"id\":\"%s\",\"amount_total\":10000,\"currency\":\"usd\",\"payment_status\":\"%s\",\"payment_intent\":\"pi_form\"}}}",
		expired ? "expired" : failed ? "async_payment_failed" : "completed", uncertain ? "cs_second" : "cs_offline", g_strcmp0(mode, "delayed") == 0 ? "unpaid" : "paid");
	sig = signature(body); bytes = g_bytes_new(body, strlen(body));
	ok = venture_stripe_service_handle_webhook(service, bytes, sig, &error); g_assert_no_error(error); g_assert_true(ok);
	if (late)
	{
		g_autoptr(GHashTable) params = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)json_node_unref);
		g_autoptr(VentureEntity) recovered = NULL;
		JsonNode *id = json_node_new(JSON_NODE_VALUE), *start = json_node_new(JSON_NODE_VALUE);
		g_assert_cmpuint(saved, ==, 0);
		json_node_set_int(id, venture_forms_get_int(g_ptr_array_index(checkouts, 0), "invoice-id")); json_node_set_string(start, slot);
		g_hash_table_insert(params, g_strdup("invoice_id"), id); g_hash_table_insert(params, g_strdup("booking_start"), start);
		recovered = venture_action_registry_perform(venture_database_get_action_registry(f->database), "form", venture_entity_get_id(form), "reconcile_payment", params, NULL, VENTURE_USER_ROLE_OWNER, &error);
		g_assert_no_error(error); g_assert_nonnull(recovered);
		g_assert_cmpuint(fake->checkouts, ==, attempts);
	}
	if (g_strcmp0(mode, "delayed") == 0 || reordered)
	{
		g_assert_cmpuint(saved, ==, 0);
		if (reordered) { g_object_set(form, "state", VENTURE_FORM_CLOSED, NULL); save(f, form); }
		g_clear_pointer(&body, g_free); g_clear_pointer(&bytes, g_bytes_unref); g_clear_pointer(&sig, g_free);
		body = g_strdup("{\"id\":\"evt_form_paid\",\"type\":\"checkout.session.async_payment_succeeded\",\"data\":{\"object\":{\"id\":\"cs_offline\",\"amount_total\":10000,\"currency\":\"usd\",\"payment_status\":\"paid\",\"payment_intent\":\"pi_form\"}}}");
		sig = signature(body); bytes = g_bytes_new(body, strlen(body));
		ok = venture_stripe_service_handle_webhook(service, bytes, sig, &error); g_assert_no_error(error); g_assert_true(ok);
	}
	if (reordered)
	{
		g_autoptr(GPtrArray) retained = rows(f, "form_payment");
		g_autofree gchar *state = venture_forms_get_string(g_ptr_array_index(retained, 0), "state");
		g_autoptr(GHashTable) params = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)json_node_unref);
		g_autoptr(VentureEntity) recovered = NULL;
		JsonNode *id = json_node_new(JSON_NODE_VALUE);
		g_assert_cmpstr(state, ==, "paid_needs_capacity"); g_assert_cmpuint(saved, ==, 0);
		g_object_set(form, "state", VENTURE_FORM_LIVE, NULL); save(f, form);
		json_node_set_int(id, venture_forms_get_int(g_ptr_array_index(checkouts, 0), "invoice-id")); g_hash_table_insert(params, g_strdup("invoice_id"), id);
		recovered = venture_action_registry_perform(venture_database_get_action_registry(f->database), "form", venture_entity_get_id(form), "reconcile_payment", params, NULL, VENTURE_USER_ROLE_OWNER, &error);
		g_assert_no_error(error); g_assert_nonnull(recovered); expired = FALSE;
	}

	g_assert_cmpuint(saved, ==, expired || failed || erased ? 0 : 1);
	ok = venture_stripe_service_handle_webhook(service, bytes, sig, &error); g_assert_no_error(error); g_assert_true(ok);
	g_assert_cmpuint(saved, ==, expired || failed || erased ? 0 : 1);
	payments = rows(f, "form_payment"); g_assert_cmpuint(payments->len, ==, 1);
	{
		g_autofree gchar *state = venture_forms_get_string(g_ptr_array_index(payments, 0), "state");
		g_autofree gchar *payload = venture_forms_get_string(g_ptr_array_index(payments, 0), "payload");
		g_assert_cmpstr(state, ==, erased ? "erased" : expired ? "expired" : failed ? "failed" : "completed");
		if (expired || failed) g_assert_false(venture_string_is_empty(payload));
		else g_assert_true(venture_string_is_empty(payload));
	}
	if (booking && (expired || failed))
	{
		g_autoptr(VentureEntity) released = venture_booking_service_reserve(booking_service, page, slot, 30, now, &error);
		g_assert_no_error(error); g_assert_nonnull(released);
	}
	if (!expired && !failed && !erased)
	{
		g_autoptr(GPtrArray) stored = rows(f, "form_submission");
		g_autofree gchar *secret = venture_forms_get_string(g_ptr_array_index(stored, 0), "sensitive-answers");
		g_autoptr(JsonNode) wire = venture_serializable_to_json(VENTURE_SERIALIZABLE(g_ptr_array_index(stored, 0)), FALSE);
		g_autofree gchar *text = json_to_string(wire, FALSE);
		g_assert_nonnull(strstr(secret, "PAYMENT-SECRET")); g_assert_null(strstr(text, "PAYMENT-SECRET"));
		if (booking) g_assert_cmpint(venture_forms_get_int(g_ptr_array_index(stored, 0), "booking-id"), >, 0);
	}
	g_signal_handler_disconnect(f->database, handler);
}
