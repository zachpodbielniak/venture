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
	signup = venture_tenant_service_lightsite_signup(tenant, "hosted-example", "owner@example.test", "https://id.example.test", "example-owner", "Example business", &created, &error);
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
		g_assert_true(venture_stripe_service_verify_authorization(provider, venture_entity_get_id(g_ptr_array_index(authorizations, 0)), NULL, &error));
		g_assert_no_error(error);
	}
	g_clear_pointer(&setup, json_node_unref);
	setup = venture_lightsite_billing_setup(f->context, f->organization_id, business, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(json_object_get_string_member(json_node_get_object(setup), "state"), ==, "card_ready");
	assert_hosted_notice(f, business, "card_saved");
	if (data) {
		g_autoptr(JsonNode) waiting = venture_lightsite_billing_first_charge(f->context, f->organization_id, business, FALSE, &error);
		g_autoptr(GDateTime) now = venture_time_now(), past = g_date_time_add_days(now, -1);
		g_autofree gchar *date = venture_time_to_string(past), *receipt = NULL, *sql = NULL;
		g_autoptr(VentureAccessScope) internal = venture_access_policy_enter(venture_database_get_access_policy(f->database), NULL);
		g_assert_no_error(error);
		g_assert_cmpstr(json_object_get_string_member(json_node_get_object(waiting), "state"), ==, "building");
		g_assert_cmpint(json_object_get_int_member(json_node_get_object(waiting), "invoice_id"), ==, 0);
		json_object_set_string_member(json_node_get_object(enrolled), "charge_due_at", date);
		receipt = venture_json_to_string(enrolled, FALSE);
		sql = g_strdup_printf("UPDATE lightsite_billing_receipts SET result='%s' WHERE business_id=%" G_GINT64_FORMAT " AND outcome='enroll'", receipt, business);
		g_assert_true(venture_database_execute(f->database, sql, NULL, &error)); g_assert_no_error(error);
		g_clear_pointer(&sql, g_free);
		sql = g_strdup_printf("UPDATE customer_subscriptions SET current_period_start='%s',current_period_end='%s',trial_end='%s',billing_anchor='%s',last_event_at='%s' WHERE id=%" G_GINT64_FORMAT,
			date, date, date, date, date, json_object_get_int_member(json_node_get_object(enrolled), "subscription_id"));
		g_assert_true(venture_database_execute(f->database, sql, NULL, &error)); g_assert_no_error(error);
	}
	charged = venture_lightsite_billing_first_charge(f->context, f->organization_id, business, data == NULL, &error);
	g_assert_no_error(error); g_assert_nonnull(charged);
	invoice = json_object_get_int_member(json_node_get_object(charged), "invoice_id"); g_assert_cmpint(invoice, >, 0);
	repeated = venture_lightsite_billing_first_charge(f->context, f->organization_id, business, data == NULL, &error);
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

	g_test_message("Hosted enrollment + verified Setup: zero invoices; publish replay: one invoice and one collection; signed fixture settlement: paid");
}
