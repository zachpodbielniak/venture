/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Reuse the provider fixture to prove the hosted boundary, including the
 * absence of payment during Setup and idempotent signed settlement. */
static void assert_hosted_notice(Fixture *f, gint64 business, const gchar *kind)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) answer = venture_lightsite_billing_notifications(f->database, f->organization_id, business, &error);
	JsonArray *events;
	guint i;
	gboolean found = FALSE;
	g_assert_no_error(error); g_assert_nonnull(answer);
	events = json_object_get_array_member(json_node_get_object(answer), "events");
	for (i = 0; i < json_array_get_length(events); i++)
		if (!g_strcmp0(json_object_get_string_member(json_array_get_object_element(events, i), "kind"), kind)) found = TRUE;
	g_assert_true(found);
}

/* The money-back promise: inside thirty days of going live, the business is
 * cancelled now and every settled hosted payment is refunded in full through
 * the provider, once, however often the request is repeated. */
static void
money_back_after_payment(Fixture *f, FakeTransport *transport, gint64 business, const gchar *kind)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) refunded = NULL, replayed = NULL;
	g_autoptr(GPtrArray) refunds = NULL;
	JsonObject *object;
	JsonArray *list;
	gint state = 0;
	transport->refund_amount = 1000;
	if (!g_strcmp0(kind, "money-back-late")) {
		g_autoptr(GDateTime) now = venture_time_now(), past = g_date_time_add_days(now, -31);
		g_autofree gchar *date = venture_time_to_string(past);
		g_autofree gchar *sql = g_strdup_printf("UPDATE lightsite_billing_receipts SET result='{\"go_live_at\":\"%s\"}' WHERE business_id=%" G_GINT64_FORMAT " AND outcome='publish'", date, business);
		{
			g_autoptr(VentureAccessScope) internal = venture_access_policy_enter(venture_database_get_access_policy(f->database), NULL);
			g_assert_true(venture_database_execute(f->database, sql, NULL, &error)); g_assert_no_error(error);
		}
		g_assert_null(venture_lightsite_billing_money_back(f->context, f->organization_id, business, &error));
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
		g_assert_cmpuint(transport->refund_creates, ==, 0);
		g_test_message("Money back after thirty days: refused, no provider call");
		return;
	}
	if (!g_strcmp0(kind, "money-back-lost")) {
		/* The provider makes the refund but the answer is lost: the request
		 * fails, and the retry finds that refund instead of making another. */
		transport->drop_refund = TRUE;
		g_assert_null(venture_lightsite_billing_money_back(f->context, f->organization_id, business, &error));
		g_assert_nonnull(error); g_clear_error(&error);
		g_assert_cmpuint(transport->refund_creates, ==, 1);
	}
	refunded = venture_lightsite_billing_money_back(f->context, f->organization_id, business, &error);
	g_assert_no_error(error); g_assert_nonnull(refunded);
	object = json_node_get_object(refunded);
	g_assert_cmpint(json_object_get_int_member(object, "organization_id"), ==, business);
	g_assert_cmpstr(json_object_get_string_member(object, "state"), ==, "refunded");
	list = json_object_get_array_member(object, "refunds");
	g_assert_cmpuint(json_array_get_length(list), ==, 1);
	g_assert_true(g_str_has_prefix(json_object_get_string_member(json_array_get_object_element(list, 0), "refund"), "re_"));
	g_assert_cmpstr(json_object_get_string_member(json_object_get_object_member(json_array_get_object_element(list, 0), "amount"), "amount"), ==, "10.00");
	g_assert_false(json_object_get_boolean_member(json_array_get_object_element(list, 0), "by_hand"));
	g_assert_cmpuint(transport->refund_creates, ==, 1);
	g_assert_nonnull(strstr(transport->refund_key, "money-back"));
	replayed = venture_lightsite_billing_money_back(f->context, f->organization_id, business, &error);
	g_assert_no_error(error);
	g_assert_true(json_node_equal(refunded, replayed));
	g_assert_cmpuint(transport->refund_creates, ==, 1);
	{
		g_autoptr(VentureAccessScope) internal = venture_access_policy_enter(venture_database_get_access_policy(f->database), NULL);
		g_autoptr(VentureEntity) sub = venture_database_get(f->database, VENTURE_TYPE_CUSTOMER_SUBSCRIPTION,
			json_object_get_int_member(object, "subscription_id"), &error);
		g_assert_no_error(error); g_object_get(sub, "status", &state, NULL); g_assert_cmpint(state, ==, 4);
		refunds = rows(f, "refund");
		g_assert_cmpuint(refunds->len, ==, 1);
	}
	assert_hosted_notice(f, business, "refund");
	g_test_message("Money back: cancelled now, one full provider refund booked once; replay identical%s",
		!g_strcmp0(kind, "money-back-lost") ? "; a lost refund answer recovered from the provider" : "");
}

/* The card on file as the billing view shows it: brand, last four and
 * expiry, and nothing else. */
static void
assert_view_card(Fixture *f, gint64 business, const gchar *brand, const gchar *last4, gint64 month, gint64 year)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) view = venture_lightsite_billing_view(f->database, f->organization_id, business, &error);
	JsonObject *card;
	g_assert_no_error(error); g_assert_nonnull(view);
	card = json_object_get_object_member(json_node_get_object(view), "card");
	g_assert_nonnull(card);
	g_assert_cmpuint(json_object_get_size(card), ==, 4);
	g_assert_cmpstr(json_object_get_string_member(card, "brand"), ==, brand);
	g_assert_cmpstr(json_object_get_string_member(card, "last4"), ==, last4);
	g_assert_cmpint(json_object_get_int_member(card, "exp_month"), ==, month);
	g_assert_cmpint(json_object_get_int_member(card, "exp_year"), ==, year);
}

static guint
hosted_notice_count(Fixture *f, gint64 business, const gchar *kind)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) answer = venture_lightsite_billing_notifications(f->database, f->organization_id, business, &error);
	JsonArray *events;
	guint i, count = 0;
	g_assert_no_error(error); g_assert_nonnull(answer);
	events = json_object_get_array_member(json_node_get_object(answer), "events");
	for (i = 0; i < json_array_get_length(events); i++)
		if (!g_strcmp0(json_object_get_string_member(json_array_get_object_element(events, i), "kind"), kind)) count++;
	return count;
}

static VentureEntity *
authorization_at(Fixture *f, guint index, gchar **status, gboolean *enabled)
{
	g_autoptr(VentureAccessScope) internal = venture_access_policy_enter(venture_database_get_access_policy(f->database), NULL);
	g_autoptr(GPtrArray) found = rows(f, "stripe_authorization");
	VentureEntity *row;
	g_assert_cmpuint(found->len, >, index);
	row = g_object_ref(g_ptr_array_index(found, index));
	g_object_get(row, "status", status, "enabled", enabled, NULL);
	return row;
}

/* The owner changes the card on file. The card in use stays in use until
 * Stripe confirms the new one; then the new one is charged and shown, and
 * the old one is kept as evidence but never charged again. If this breaks,
 * an owner either cannot replace an expired card or is left with no card
 * at all while the hosted page is open. */
static void
hosted_change_card(Fixture *f, VentureStripeService *provider, FakeTransport *transport, gint64 business, gboolean on_return)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) change = NULL, again = NULL, refused = NULL;
	g_autoptr(VentureEntity) first = NULL, second = NULL;
	g_autofree gchar *url = NULL, *status = NULL, *body = NULL, *sig = NULL, *detached = NULL, *detached_sig = NULL;
	g_autoptr(GBytes) raw = NULL, raw_detached = NULL;
	g_autoptr(GPtrArray) events = NULL;
	gboolean enabled = FALSE;
	assert_view_card(f, business, "visa", "4242", 8, 2028);
	change = venture_lightsite_billing_change_card(f->context, f->organization_id, business, &error);
	g_assert_no_error(error); g_assert_nonnull(change);
	g_assert_cmpuint(json_object_get_size(json_node_get_object(change)), ==, 2);
	g_assert_cmpstr(json_object_get_string_member(json_node_get_object(change), "state"), ==, "awaiting_card");
	url = g_strdup(json_object_get_string_member(json_node_get_object(change), "url"));
	g_assert_true(g_str_has_prefix(url, "https://checkout.stripe.com/"));
	/* Asking again while the page is open is the same page, not a second. */
	again = venture_lightsite_billing_change_card(f->context, f->organization_id, business, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(json_object_get_string_member(json_node_get_object(again), "url"), ==, url);
	first = authorization_at(f, 0, &status, &enabled);
	g_assert_cmpstr(status, ==, "active"); g_assert_true(enabled);
	g_clear_pointer(&status, g_free);
	second = authorization_at(f, 1, &status, &enabled);
	g_assert_cmpstr(status, ==, "replacing"); g_assert_false(enabled);
	g_clear_pointer(&status, g_free);
	{
		g_autoptr(VentureAccessScope) internal = venture_access_policy_enter(venture_database_get_access_policy(f->database), NULL);
		g_autoptr(GPtrArray) all = rows(f, "stripe_authorization");
		g_assert_cmpuint(all->len, ==, 2);
	}
	/* Until Stripe confirms, the old card is the card. */
	assert_view_card(f, business, "visa", "4242", 8, 2028);
	/* The owner comes back with the page unfinished: Setup asks Stripe,
	 * answers card_ready (there is a card) and changes nothing. */
	transport->setup_open = TRUE;
	{
		g_autoptr(JsonNode) back = venture_lightsite_billing_setup(f->context, f->organization_id, business, &error);
		g_assert_no_error(error);
		g_assert_cmpstr(json_object_get_string_member(json_node_get_object(back), "state"), ==, "card_ready");
	}
	transport->setup_open = FALSE;
	assert_view_card(f, business, "visa", "4242", 8, 2028);
	if (on_return) {
		/* Back with the page finished, before Stripe's callback: the
		 * answer comes from Stripe's record of the Setup, so the new card
		 * is the card the moment the owner is back. */
		g_autoptr(JsonNode) back = venture_lightsite_billing_setup(f->context, f->organization_id, business, &error);
		g_assert_no_error(error);
		g_assert_cmpstr(json_object_get_string_member(json_node_get_object(back), "state"), ==, "card_ready");
		assert_view_card(f, business, "mastercard", "4444", 12, 2030);
		g_test_message("Change card confirmed on return, with no callback");
		return;
	}
	/* Stripe confirms with a signed event. It is answered before any
	 * provider read; the Stripe timer's own step then reads the Setup. */
	body = g_strdup_printf("{\"id\":\"evt_card_change\",\"type\":\"checkout.session.completed\",\"livemode\":false,\"data\":{\"object\":{\"id\":\"cs_setup2\",\"mode\":\"setup\",\"client_reference_id\":\"%s\",\"setup_intent\":\"seti_offline2\"}}}",
		venture_entity_get_uuid(second));
	sig = binding_signature(body, "whsec_original");
	raw = g_bytes_new(body, strlen(body));
	g_assert_true(venture_stripe_service_handle_webhook(provider, raw, sig, &error)); g_assert_no_error(error);
	assert_view_card(f, business, "visa", "4242", 8, 2028);
	{
		g_autoptr(VentureAccessScope) internal = venture_access_policy_enter(venture_database_get_access_policy(f->database), NULL);
		events = rows(f, "stripe_event");
		g_assert_cmpuint(events->len, >=, 1);
		g_assert_true(venture_stripe_service_finish_event(provider,
			venture_entity_get_id(g_ptr_array_index(events, events->len - 1)), NULL, &error));
		g_assert_no_error(error);
	}
	assert_view_card(f, business, "mastercard", "4444", 12, 2030);
	g_clear_object(&first); g_clear_object(&second);
	first = authorization_at(f, 0, &status, &enabled);
	g_assert_cmpstr(status, ==, "replaced"); g_assert_false(enabled);
	g_clear_pointer(&status, g_free);
	second = authorization_at(f, 1, &status, &enabled);
	g_assert_cmpstr(status, ==, "active"); g_assert_true(enabled);
	g_clear_pointer(&status, g_free);
	/* The owner's mail is "card changed", never a second "card saved"
	 * (which promises the first payment). */
	g_assert_cmpuint(hosted_notice_count(f, business, "card_saved"), ==, 1);
	g_assert_cmpuint(hosted_notice_count(f, business, "card_changed"), ==, 1);
	/* Setup still answers card_ready: the business has a card. */
	{
		g_autoptr(JsonNode) ready = venture_lightsite_billing_setup(f->context, f->organization_id, business, &error);
		g_assert_no_error(error);
		g_assert_cmpstr(json_object_get_string_member(json_node_get_object(ready), "state"), ==, "card_ready");
	}
	/* With no card on file there is nothing to change: Setup saves the first. */
	detached = g_strdup("{\"id\":\"evt_card_detached\",\"type\":\"payment_method.detached\",\"livemode\":false,\"data\":{\"object\":{\"id\":\"pm_offline2\"}}}");
	detached_sig = binding_signature(detached, "whsec_original");
	raw_detached = g_bytes_new(detached, strlen(detached));
	g_assert_true(venture_stripe_service_handle_webhook(provider, raw_detached, detached_sig, &error)); g_assert_no_error(error);
	{
		g_autoptr(JsonNode) view = venture_lightsite_billing_view(f->database, f->organization_id, business, &error);
		g_assert_no_error(error);
		g_assert_true(json_node_is_null(json_object_get_member(json_node_get_object(view), "card")));
	}
	refused = venture_lightsite_billing_change_card(f->context, f->organization_id, business, &error);
	g_assert_null(refused);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
	(void)transport;
	g_test_message("Change card: old card in use until Stripe confirms; then the new card is shown and charged, one card_changed notice");
}

static void
finish_setup_event(Fixture *f, VentureStripeService *provider, const gchar *event_id,
	const gchar *session, const gchar *reference)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *intent = NULL;
	g_autofree gchar *body = NULL;
	g_autofree gchar *sig = NULL;
	g_autoptr(GBytes) raw = NULL;
	g_autoptr(GPtrArray) events = NULL;
	const gchar *suffix = (session && g_strcmp0(session, "cs_setup")) ? session + strlen("cs_setup") : "";
	intent = g_strdup_printf("seti_offline%s", suffix);
	body = g_strdup_printf("{\"id\":\"%s\",\"type\":\"checkout.session.completed\",\"livemode\":false,\"data\":{\"object\":{\"id\":\"%s\",\"mode\":\"setup\",\"client_reference_id\":\"%s\",\"setup_intent\":\"%s\"}}}",
		event_id, session, reference, intent);
	sig = binding_signature(body, "whsec_original");
	raw = g_bytes_new(body, strlen(body));
	g_assert_true(venture_stripe_service_handle_webhook(provider, raw, sig, &error));
	g_assert_no_error(error);
	{
		g_autoptr(VentureAccessScope) internal = venture_access_policy_enter(venture_database_get_access_policy(f->database), NULL);
		events = rows(f, "stripe_event");
		g_assert_cmpuint(events->len, >=, 1);
		g_assert_true(venture_stripe_service_finish_event(provider,
			venture_entity_get_id(g_ptr_array_index(events, events->len - 1)), NULL, &error));
		g_assert_no_error(error);
	}
}

static guint
enabled_permissions(Fixture *f)
{
	g_autoptr(VentureAccessScope) internal = venture_access_policy_enter(venture_database_get_access_policy(f->database), NULL);
	g_autoptr(GPtrArray) all = rows(f, "stripe_authorization");
	guint i, n = 0;
	for (i = 0; i < all->len; i++)
	{
		gboolean enabled = FALSE;
		g_object_get(g_ptr_array_index(all, i), "enabled", &enabled, NULL);
		if (enabled) n++;
	}
	return n;
}

/* A page with under half an hour left is retired, and a finished retired
 * page must not become a second enabled card. Verification of a change
 * that was left open on purpose still leaves exactly one enabled
 * permission: collection charges whichever enabled row it sees last. */
static void
hosted_retire_dying_page(Fixture *f, VentureStripeService *provider, FakeTransport *transport, gint64 business)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) change = NULL, again = NULL, refused = NULL;
	g_autoptr(VentureEntity) first = NULL, dying = NULL, opened = NULL;
	g_autofree gchar *url = NULL, *url2 = NULL, *status = NULL, *session = NULL, *reference = NULL, *was = NULL;
	gboolean enabled = FALSE;
	gint64 dying_id = 0;
	assert_view_card(f, business, "visa", "4242", 8, 2028);
	{
		g_autoptr(VentureAccessScope) internal = venture_access_policy_enter(venture_database_get_access_policy(f->database), NULL);
		g_autoptr(GPtrArray) subs = rows(f, "customer_subscription");
		g_autofree gchar *sql = NULL;
		VentureEntity *sub;
		g_assert_cmpuint(subs->len, ==, 1);
		sub = g_ptr_array_index(subs, 0);
		g_object_get(sub, "external-id", &was, NULL);
		sql = g_strdup_printf("UPDATE customer_subscriptions SET external_id='prepaid:retire' WHERE id=%" G_GINT64_FORMAT, venture_entity_get_id(sub));
		g_assert_true(venture_database_execute(f->database, sql, NULL, &error));
		g_assert_no_error(error);
	}
	refused = venture_lightsite_billing_change_card(f->context, f->organization_id, business, &error);
	g_assert_null(refused);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
	g_clear_error(&error);
	{
		g_autoptr(VentureAccessScope) internal = venture_access_policy_enter(venture_database_get_access_policy(f->database), NULL);
		g_autoptr(GPtrArray) subs = rows(f, "customer_subscription");
		g_autofree gchar *sql = g_strdup_printf("UPDATE customer_subscriptions SET external_id='%s' WHERE id=%" G_GINT64_FORMAT,
			was, venture_entity_get_id(g_ptr_array_index(subs, 0)));
		g_assert_true(venture_database_execute(f->database, sql, NULL, &error));
		g_assert_no_error(error);
	}
	change = venture_lightsite_billing_change_card(f->context, f->organization_id, business, &error);
	g_assert_no_error(error);
	url = g_strdup(json_object_get_string_member(json_node_get_object(change), "url"));
	dying = authorization_at(f, 1, &status, &enabled);
	g_assert_cmpstr(status, ==, "replacing");
	g_assert_false(enabled);
	g_clear_pointer(&status, g_free);
	dying_id = venture_entity_get_id(dying);
	g_object_get(dying, "session-id", &session, "uuid", &reference, NULL);
	{
		g_autoptr(GDateTime) now = g_date_time_new_now_utc();
		g_autoptr(GDateTime) soon = g_date_time_add_minutes(now, 10);
		g_autofree gchar *stored = venture_time_to_string(soon);
		g_autofree gchar *unix_deadline = g_strdup_printf("%" G_GINT64_FORMAT, g_date_time_to_unix(soon));
		g_autofree gchar *sql = g_strdup_printf("UPDATE stripe_authorizations SET expires_at='%s' WHERE id=%" G_GINT64_FORMAT, stored, dying_id);
		g_autoptr(VentureAccessScope) internal = venture_access_policy_enter(venture_database_get_access_policy(f->database), NULL);
		/* The service owns this row. A direct save is refused, and the
		 * next read comes from SQL, so the deadline moves there. */
		g_assert_true(venture_database_execute(f->database, sql, NULL, &error));
		g_assert_no_error(error);
		fake_setup_set_deadline(transport, session, unix_deadline);
	}
	again = venture_lightsite_billing_change_card(f->context, f->organization_id, business, &error);
	g_assert_no_error(error);
	url2 = g_strdup(json_object_get_string_member(json_node_get_object(again), "url"));
	g_assert_cmpstr(url2, !=, url);
	g_clear_object(&first);
	g_clear_object(&dying);
	first = authorization_at(f, 0, &status, &enabled);
	g_assert_cmpstr(status, ==, "active");
	g_assert_true(enabled);
	g_clear_pointer(&status, g_free);
	dying = authorization_at(f, 1, &status, &enabled);
	g_assert_cmpstr(status, ==, "revoked");
	g_assert_false(enabled);
	g_clear_pointer(&status, g_free);
	opened = authorization_at(f, 2, &status, &enabled);
	g_assert_cmpstr(status, ==, "replacing");
	g_assert_false(enabled);
	g_clear_pointer(&status, g_free);
	assert_view_card(f, business, "visa", "4242", 8, 2028);
	finish_setup_event(f, provider, "evt_card_retired", session, reference);
	assert_view_card(f, business, "visa", "4242", 8, 2028);
	g_clear_object(&dying);
	dying = authorization_at(f, 1, &status, &enabled);
	g_assert_cmpstr(status, ==, "revoked");
	g_assert_false(enabled);
	g_clear_pointer(&status, g_free);
	/* Put that retired page back to an open change, which is the state a
	 * second page used to leave behind. Verifying it, then the newer page,
	 * must still end on one enabled card. */
	{
		g_autoptr(VentureAccessScope) internal = venture_access_policy_enter(venture_database_get_access_policy(f->database), NULL);
		g_autofree gchar *sql = g_strdup_printf("UPDATE stripe_authorizations SET status='replacing' WHERE id=%" G_GINT64_FORMAT, dying_id);
		g_assert_true(venture_database_execute(f->database, sql, NULL, &error));
		g_assert_no_error(error);
	}
	finish_setup_event(f, provider, "evt_card_late", session, reference);
	g_assert_cmpuint(enabled_permissions(f), ==, 1);
	{
		g_autofree gchar *newer_session = NULL, *newer_reference = NULL;
		g_object_get(opened, "session-id", &newer_session, "uuid", &newer_reference, NULL);
		finish_setup_event(f, provider, "evt_card_newer", newer_session, newer_reference);
	}
	g_assert_cmpuint(enabled_permissions(f), ==, 1);
	g_clear_object(&first);
	g_clear_object(&dying);
	g_clear_object(&opened);
	first = authorization_at(f, 0, &status, &enabled);
	g_assert_false(enabled);
	g_clear_pointer(&status, g_free);
	dying = authorization_at(f, 1, &status, &enabled);
	g_assert_false(enabled);
	g_clear_pointer(&status, g_free);
	opened = authorization_at(f, 2, &status, &enabled);
	g_assert_cmpstr(status, ==, "active");
	g_assert_true(enabled);
	assert_view_card(f, business, "mastercard", "4444", 12, 2030);
	g_test_message("Dying change page retired; a late confirmation cannot leave two cards enabled");
}

static void
test_hosted_saved_card(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GObject) transport = g_object_new(fake_transport_get_type(), NULL);
	g_autoptr(JsonNode) settings = binding_settings("whsec_original");
	g_autoptr(GBytes) key = g_bytes_new_take(g_malloc0(32), 32);
	g_autoptr(VentureIntegrationConnection) connection = NULL;
	g_autoptr(VentureStripeService) provider = NULL;
	g_autoptr(VentureEntity) plan = record_new(f, "plan"), price = record_new(f, "plan_price");
	g_autoptr(VentureQuery) users_query = venture_query_new(VENTURE_TYPE_USER);
	g_autoptr(GPtrArray) users = NULL, invoices = NULL, authorizations = NULL, attempts = NULL;
	g_autoptr(VentureApiToken) token = venture_api_token_new();
	g_autoptr(VentureAccessScope) scope = NULL;
	g_autoptr(JsonNode) signup = NULL, enrolled = NULL, setup = NULL, charged = NULL, repeated = NULL, paid = NULL;
	g_autofree gchar *secret = NULL;
	VentureTenantService *tenant = venture_tenant_service_get(f->database);
	VentureAuthPrincipal principal = { 0 };
	gboolean created = FALSE;
	gint64 business, invoice;
	json_object_set_string_member(json_node_get_object(settings), "api_version", "2024-06-20");
	g_assert_true(venture_integration_service_set_key(venture_integration_service_get(f->database), key, &error));
	connection = venture_stripe_settings_configure(f->database, f->organization_id, settings, 0, 0, STRIPE_TRANSPORT(transport), NULL, &error);
	g_assert_no_error(error);
	provider = venture_stripe_service_for_organization(f->database, f->organization_id, STRIPE_TRANSPORT(transport), &error);
	g_assert_no_error(error); venture_context_set_stripe_service(f->context, provider);
	g_object_set(plan, "name", "Example plan", "active", TRUE, NULL); save(f, plan);
	g_object_set(price, "plan-id", venture_entity_get_id(plan), "currency", "USD", "active", TRUE, "product-id", f->product_id, NULL);
	money_field(price, "amount", "10 USD"); save(f, price);
	g_object_set(f->config, "hosted-enabled", TRUE, "hosted-workspace-id", "8f062b79-1d2b-4d7f-99e5-bd3bf588e05a",
		"hosted-origin", "https://example.test", "security-password-iterations", (gint64)2000, NULL);
	g_assert_true(venture_tenant_service_configure(tenant, f->config, &error)); g_assert_no_error(error);
	g_assert_true(venture_tenant_service_initialize(tenant, &error)); g_assert_no_error(error);
	g_assert_true(venture_tenant_service_bootstrap_admin(tenant, f->config, "operator", "fixture-password", FALSE, "Fixture", &error)); g_assert_no_error(error);
	users = venture_database_find(f->database, users_query, &error); g_assert_no_error(error);
	principal.user_id = venture_entity_get_id(g_ptr_array_index(users, 0));
	g_object_set(token, "name", "Example service", "user-id", principal.user_id, "role", VENTURE_USER_ROLE_EDITOR, NULL);
	secret = venture_api_token_generate(token); save(f, VENTURE_ENTITY(token));
	principal.token_id = venture_entity_get_id(VENTURE_ENTITY(token));
	principal.role = VENTURE_USER_ROLE_EDITOR; principal.authenticated = TRUE; principal.name = (gchar *)"Example service";
	scope = venture_access_policy_enter(venture_database_get_access_policy(f->database), &principal);
	signup = venture_tenant_service_lightsite_signup(tenant, "hosted-example", "owner@example.test", "https://id.example.test", "example-owner", "Example business", "Alex", "Example", &created, &error);
	g_assert_no_error(error); g_assert_nonnull(signup);
	business = json_object_get_int_member(json_node_get_object(signup), "organization_id");
	enrolled = venture_lightsite_billing_enroll(f->database, f->organization_id, business, venture_entity_get_id(price), &created, &error);
	g_assert_no_error(error); g_assert_true(created);
	setup = venture_lightsite_billing_setup(f->context, f->organization_id, business, &error);
	g_assert_no_error(error); g_assert_nonnull(setup);
	g_assert_cmpstr(json_object_get_string_member(json_node_get_object(setup), "state"), ==, "awaiting_card");
	g_assert_true(g_str_has_prefix(json_object_get_string_member(json_node_get_object(setup), "url"), "https://checkout.stripe.com/"));
	/* Inspect service-owned records under maintenance, as the hosted API does. */
	{
		g_autoptr(VentureAccessScope) internal = venture_access_policy_enter(venture_database_get_access_policy(f->database), NULL);
		invoices = rows(f, "invoice"); authorizations = rows(f, "stripe_authorization");
		g_assert_cmpuint(invoices->len, ==, 0); g_assert_cmpuint(authorizations->len, ==, 1);
		if (!data || g_strcmp0(data, "confirm-on-return")) {
			g_assert_true(venture_stripe_service_verify_authorization(provider, venture_entity_get_id(g_ptr_array_index(authorizations, 0)), NULL, &error));
			g_assert_no_error(error);
		}
	}
	if (data && !g_strcmp0(data, "confirm-on-return")) {
		/* The owner is back from the hosted page before Stripe's callback
		 * has been handled. The answer comes from the provider's own
		 * record of the Setup, not from whether the webhook got here
		 * first: an unfinished Setup still waits for the card... */
		((FakeTransport *)transport)->setup_open = TRUE;
		g_clear_pointer(&setup, json_node_unref);
		setup = venture_lightsite_billing_setup(f->context, f->organization_id, business, &error);
		g_assert_no_error(error);
		g_assert_cmpstr(json_object_get_string_member(json_node_get_object(setup), "state"), ==, "awaiting_card");
		((FakeTransport *)transport)->setup_open = FALSE;
		/* ...and a finished one is a saved card, with no callback at all. */
	}
	g_clear_pointer(&setup, json_node_unref);
	setup = venture_lightsite_billing_setup(f->context, f->organization_id, business, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(json_object_get_string_member(json_node_get_object(setup), "state"), ==, "card_ready");
	assert_hosted_notice(f, business, "card_saved");
	if (data && !g_strcmp0(data, "change-card-retire")) {
		hosted_retire_dying_page(f, provider, (FakeTransport *)transport, business);
		return;
	}
	if (data && g_str_has_prefix(data, "change-card")) {
		hosted_change_card(f, provider, (FakeTransport *)transport, business, !g_strcmp0(data, "change-card-return"));
		return;
	}
	gboolean expired = data && !g_strcmp0(data, "expired");
	if (expired) {
		/* A saved card is not consent to charge before go-live. On day 31,
		 * past expires_at, the sweep, the provider collector and the
		 * unpublished first charge issue nothing and call the provider
		 * for nothing; publishing then activates, invoices and collects. */
		g_autoptr(JsonNode) waiting = venture_lightsite_billing_first_charge(f->context, f->organization_id, business, FALSE, &error);
		g_autoptr(JsonNode) late = NULL;
		g_autoptr(GDateTime) now = venture_time_now(), enrolled_at = g_date_time_add_days(now, -31), end = g_date_time_add_days(enrolled_at, 30);
		g_autofree gchar *start = venture_time_to_string(enrolled_at), *date = venture_time_to_string(end), *receipt = NULL, *sql = NULL;
		g_autoptr(VentureAccessScope) internal = NULL;
		g_autoptr(VentureEntity) sweep = record_new(f, "billing_request");
		guint calls;
		g_assert_no_error(error);
		internal = venture_access_policy_enter(venture_database_get_access_policy(f->database), NULL);
		g_assert_cmpstr(json_object_get_string_member(json_node_get_object(waiting), "state"), ==, "building");
		g_assert_cmpint(json_object_get_int_member(json_node_get_object(waiting), "invoice_id"), ==, 0);
		json_object_set_string_member(json_node_get_object(enrolled), "enrolled_at", start);
		json_object_set_string_member(json_node_get_object(enrolled), "expires_at", date);
		receipt = venture_json_to_string(enrolled, FALSE);
		sql = g_strdup_printf("UPDATE lightsite_billing_receipts SET result='%s' WHERE business_id=%" G_GINT64_FORMAT " AND outcome='enroll'", receipt, business);
		g_assert_true(venture_database_execute(f->database, sql, NULL, &error)); g_assert_no_error(error);
		g_clear_pointer(&sql, g_free);
		sql = g_strdup_printf("UPDATE customer_subscriptions SET current_period_start='%s',current_period_end='%s',trial_end='%s',billing_anchor='%s',last_event_at='%s' WHERE id=%" G_GINT64_FORMAT,
			start, date, date, date, start, json_object_get_int_member(json_node_get_object(enrolled), "subscription_id"));
		g_assert_true(venture_database_execute(f->database, sql, NULL, &error)); g_assert_no_error(error);
		calls = ((FakeTransport *)transport)->calls;
		g_object_set(sweep, "action", "renew-sweep", "at", now, NULL); save(f, sweep);
		g_assert_cmpint(venture_stripe_service_collect_due(provider, now, 10, NULL, &error), >=, 0); g_assert_no_error(error);
		/* The hosted operation runs as the service token, as the API does. */
		g_clear_object(&internal);
		late = venture_lightsite_billing_first_charge(f->context, f->organization_id, business, FALSE, &error);
		g_assert_no_error(error);
		g_assert_cmpstr(json_object_get_string_member(json_node_get_object(late), "state"), ==, "expired");
		g_assert_cmpint(json_object_get_int_member(json_node_get_object(late), "invoice_id"), ==, 0);
		internal = venture_access_policy_enter(venture_database_get_access_policy(f->database), NULL);
		g_clear_pointer(&invoices, g_ptr_array_unref); invoices = rows(f, "invoice");
		g_assert_cmpuint(invoices->len, ==, 0);
		g_assert_cmpuint(((FakeTransport *)transport)->calls, ==, calls);
		g_assert_cmpuint(((FakeTransport *)transport)->payment_calls, ==, 0);
		g_test_message("Day 31 unpublished with a saved card: zero invoices, zero provider requests, state expired");
		g_clear_object(&internal);
	}
	charged = venture_lightsite_billing_first_charge(f->context, f->organization_id, business, TRUE, &error);
	g_assert_no_error(error); g_assert_nonnull(charged);
	invoice = json_object_get_int_member(json_node_get_object(charged), "invoice_id"); g_assert_cmpint(invoice, >, 0);
	if (expired) {
		g_autoptr(VentureAccessScope) internal = venture_access_policy_enter(venture_database_get_access_policy(f->database), NULL);
		g_autoptr(VentureEntity) sub = venture_database_get(f->database, VENTURE_TYPE_CUSTOMER_SUBSCRIPTION,
			json_object_get_int_member(json_node_get_object(enrolled), "subscription_id"), &error);
		gint state = -1;
		g_assert_no_error(error); g_object_get(sub, "status", &state, NULL);
		g_assert_cmpint(state, ==, 1);
		g_test_message("Published after the window: activated, one invoice, collection started");
	}
	repeated = venture_lightsite_billing_first_charge(f->context, f->organization_id, business, TRUE, &error);
	g_assert_no_error(error);
	g_assert_cmpint(json_object_get_int_member(json_node_get_object(repeated), "invoice_id"), ==, invoice);
	{
		g_autoptr(VentureAccessScope) internal = venture_access_policy_enter(venture_database_get_access_policy(f->database), NULL);
		g_clear_pointer(&invoices, g_ptr_array_unref); invoices = rows(f, "invoice"); attempts = rows(f, "stripe_checkout");
		g_assert_cmpuint(invoices->len, ==, 1); g_assert_cmpuint(attempts->len, ==, 1);
		g_assert_true(automatic_fixture_event(provider, (FakeTransport *)transport, "evt_hosted_paid", TRUE, &error)); g_assert_no_error(error);
	}
	paid = venture_lightsite_billing_first_charge(f->context, f->organization_id, business, TRUE, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(json_object_get_string_member(json_node_get_object(paid), "state"), ==, "paid");
	assert_hosted_notice(f, business, "receipt");
	{
		g_autoptr(VentureAccessScope) internal = venture_access_policy_enter(venture_database_get_access_policy(f->database), NULL);
		g_autoptr(VentureEntity) retained = venture_database_get(f->database, VENTURE_TYPE_STRIPE_AUTHORIZATION, venture_entity_get_id(g_ptr_array_index(authorizations, 0)), &error);
		g_assert_no_error(error);
		g_object_set(retained, "enabled", FALSE, "status", "revoked", NULL);
		g_assert_true(venture_stripe_save_owned(f->database, retained, NULL, &error)); g_assert_no_error(error);
	}
	assert_hosted_notice(f, business, "card_saved");
	if (data && g_str_has_prefix(data, "money-back")) {
		money_back_after_payment(f, (FakeTransport *)transport, business, data);
		return;
	}

	g_test_message("Hosted enrollment + verified Setup: zero invoices; publish replay: one invoice and one collection; signed fixture settlement: paid");
}
