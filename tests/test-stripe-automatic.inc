/* SPDX-License-Identifier: AGPL-3.0-or-later */
static gboolean
automatic_fixture_event(VentureStripeService *service, FakeTransport *transport,
	const gchar *event_id, gboolean paid, GError **error)
{
	FakeInvoice *invoice = fake_invoice_latest(transport);
	g_autofree gchar *body = g_strdup_printf("{\"id\":\"%s\",\"type\":\"invoice.%s\",\"api_version\":\"2024-06-20\",\"livemode\":false,\"created\":%" G_GINT64_FORMAT ",\"data\":{\"object\":{\"id\":\"%s\",\"customer\":\"%s\",\"currency\":\"usd\",\"amount_paid\":%" G_GINT64_FORMAT ",\"payment_intent\":\"pi_%s\"}}}",
		event_id, paid ? "payment_succeeded" : "payment_failed", g_get_real_time() / G_USEC_PER_SEC,
		invoice->id, invoice->customer, paid ? invoice->amount : 0, invoice->id + 3);
	g_autofree gchar *signature = binding_signature(body, "whsec_original");
	g_autoptr(GBytes) raw = g_bytes_new(body, strlen(body));
	invoice->processing = FALSE; invoice->paid = paid;
	return venture_stripe_service_handle_webhook(service, raw, signature, error);
}

/* A saved payment-method ID is not authority to charge. Permission must come
 * from hosted setup and stay bound to the subscription's frozen terms. */
static void
test_automatic_authorization(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GObject) transport = g_object_new(fake_transport_get_type(), NULL);
	g_autoptr(JsonNode) settings = binding_settings("whsec_original");
	g_autoptr(GBytes) key = g_bytes_new_static("01234567890123456789012345678901", 32);
	g_autoptr(VentureIntegrationConnection) connection = NULL;
	g_autoptr(VentureStripeService) service = NULL;
	g_autoptr(VentureEntity) plan = record_new(f, "plan");
	g_autoptr(VentureEntity) price = record_new(f, "plan_price");
	g_autoptr(VentureEntity) action = record_new(f, "billing_request");
	g_autoptr(VentureStripeAuthorization) authorization = NULL;
	g_autoptr(VentureMoney) limit = venture_money_new(10000, "USD", 2);
	g_autofree gchar *state = NULL, *url = NULL;
	gint64 subscription_id;
	(void)data;
	json_object_set_string_member(json_node_get_object(settings), "api_version", "2024-06-20");
	if (data && !g_strcmp0(data, "bank"))
	{
		json_object_set_boolean_member(json_node_get_object(settings), "ach_enabled", TRUE);
		((FakeTransport *)transport)->setup_bank = TRUE;
	}
	g_assert_true(venture_integration_service_set_key(venture_integration_service_get(f->database), key, &error));
	connection = venture_stripe_settings_configure(f->database, f->organization_id, settings, 0, 0, STRIPE_TRANSPORT(transport), NULL, &error);
	g_assert_no_error(error);
	service = venture_stripe_service_for_organization(f->database, f->organization_id, STRIPE_TRANSPORT(transport), &error);
	g_assert_no_error(error);
	g_object_set(plan, "name", "Managed plan", "active", TRUE, NULL); save(f, plan);
	g_object_set(price, "plan-id", venture_entity_get_id(plan), "currency", "USD", "active", TRUE,
		"product-id", f->product_id, NULL);
	money_field(price, "amount", "50 USD"); save(f, price);
	g_object_set(action, "action", "start", "company-id", f->customer_id,
		"plan-price-id", venture_entity_get_id(price), "seats", (gint64)1, NULL);
	money_field(action, "at", data && !g_strcmp0(data, "sweep") ? "2026-07-01" : "2026-09-01"); save(f, action);
	g_object_get(action, "subscription-id", &subscription_id, NULL);
	if (data && !g_strcmp0(data, "action"))
	{
		g_autoptr(VentureWebServer) server = NULL;
		g_autoptr(GPtrArray) records = NULL;
		g_autofree gchar *directory = NULL, *body = NULL;
		g_autofree gchar *path = g_strdup_printf("/api/v1/customer_subscription/%" G_GINT64_FORMAT "/actions/authorize_payment", subscription_id);
		venture_context_set_stripe_service(f->context, service);
		server = start_server(f, &directory);
		g_assert_cmpuint(http_request(server, "POST", path, "application/x-www-form-urlencoded", "limit=100+USD", &body), ==, 200);
		g_assert_nonnull(strstr(body, "Copy authorization link"));
		g_assert_nonnull(strstr(body, "checkout.stripe.com")); g_assert_null(strstr(body, "payment_method_id"));
		{
			g_autofree gchar *binary = g_build_filename("build", "debug", "venturectl", NULL);
			const gchar *argv[] = { binary, "--server", venture_web_server_get_base_url(server), "describe", "stripe_authorization", NULL };
			g_autofree gchar *description = run_cli(argv, NULL, TRUE);
			g_assert_nonnull(strstr(description, "next_check_at")); g_assert_nonnull(strstr(description, "limit"));
			g_test_message("venturectl describe stripe_authorization: exposes limit and next_check_at using generated wire names");
		}
		venture_web_server_stop(server); venture_test_remove_tree(directory);
		records = rows(f, "stripe_authorization"); g_assert_cmpuint(records->len, ==, 1);
		g_test_message("POST %s -> 200; generic HTML returns copy-once hosted Setup URL; persisted authorization rows=%u", path, records->len);
		authorization = g_object_ref(g_ptr_array_index(records, 0));
		/* The returned URL is one-time output, so retrieve a pending repeat
		 * through the same service to finish the common lifecycle assertions. */
		g_clear_object(&authorization);
		authorization = venture_stripe_service_authorize_subscription(service, subscription_id, limit, NULL, &error);
	}
	else authorization = venture_stripe_service_authorize_subscription(service, subscription_id, limit, NULL, &error);
	g_assert_no_error(error); g_assert_nonnull(authorization);
	g_object_get(authorization, "status", &state, "url", &url, NULL);
	g_assert_cmpstr(state, ==, "pending"); g_assert_nonnull(url);
	g_object_set(authorization, "status", "active", "payment-method-id", "pm_forged", NULL);
	g_assert_false(venture_database_save(f->database, VENTURE_ENTITY(authorization), NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	if (!data)
	{
		g_assert_false(venture_database_delete(f->database, VENTURE_ENTITY(authorization), NULL, &error));
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION); g_clear_error(&error);
		g_assert_false(venture_database_purge(f->database, VENTURE_ENTITY(authorization), NULL, &error));
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION); g_clear_error(&error);
		g_assert_false(venture_database_restore(f->database, VENTURE_ENTITY(authorization), NULL, &error));
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION); g_clear_error(&error);
	}
	if (data && !g_strcmp0(data, "expiry-collision"))
	{
		g_autofree gchar *body = g_strdup_printf("{\"id\":\"evt_setup_wrong_expiry\",\"type\":\"checkout.session.expired\",\"livemode\":false,\"data\":{\"object\":{\"id\":\"cs_unrelated\",\"mode\":\"setup\",\"client_reference_id\":\"%s\"}}}", venture_entity_get_uuid(VENTURE_ENTITY(authorization)));
		g_autofree gchar *signature = binding_signature(body, "whsec_original");
		g_autoptr(GBytes) raw = g_bytes_new(body, strlen(body));
		/* A signed session carrying our opaque reference still cannot replace
		 * the immutable session identity retained by the create response. */
		g_assert_false(venture_stripe_service_handle_webhook(service, raw, signature, &error));
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION); g_clear_error(&error);
	}
	if (data && !g_strcmp0(data, "catalog"))
	{
		g_autoptr(VentureStripeAuthorization) repeated = NULL;
		g_object_set(price, "active", FALSE, NULL); save(f, price);
		repeated = venture_stripe_service_authorize_subscription(service, subscription_id, limit, NULL, &error);
		g_assert_no_error(error); g_assert_nonnull(repeated);
		g_assert_cmpint(venture_entity_get_id(VENTURE_ENTITY(repeated)), ==, venture_entity_get_id(VENTURE_ENTITY(authorization)));
	}
	((FakeTransport *)transport)->setup_customer = "cus_unrelated";
	g_assert_false(venture_stripe_service_verify_authorization(service,
		venture_entity_get_id(VENTURE_ENTITY(authorization)), NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	((FakeTransport *)transport)->setup_customer = NULL;
	if (data && !g_strcmp0(data, "environment"))
	{
		((FakeTransport *)transport)->setup_live = TRUE;
		g_assert_false(venture_stripe_service_verify_authorization(service, venture_entity_get_id(VENTURE_ENTITY(authorization)), NULL, &error));
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION); g_clear_error(&error);
		((FakeTransport *)transport)->setup_live = FALSE;
	}
	if (data && !g_strcmp0(data, "bank"))
	{
		((FakeTransport *)transport)->bad_mandate = TRUE;
		g_assert_false(venture_stripe_service_verify_authorization(service, venture_entity_get_id(VENTURE_ENTITY(authorization)), NULL, &error));
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION); g_clear_error(&error);
		((FakeTransport *)transport)->bad_mandate = FALSE;
	}
	if (data && !g_strcmp0(data, "callback"))
	{
		g_autofree gchar *body = g_strdup_printf("{\"id\":\"evt_setup\",\"type\":\"checkout.session.completed\",\"livemode\":false,\"data\":{\"object\":{\"id\":\"cs_setup\",\"mode\":\"setup\",\"client_reference_id\":\"%s\",\"setup_intent\":\"seti_offline\"}}}", venture_entity_get_uuid(VENTURE_ENTITY(authorization)));
		g_autofree gchar *sig = binding_signature(body, "whsec_original");
		g_autoptr(GBytes) raw = g_bytes_new(body, strlen(body));
		g_assert_true(venture_stripe_service_handle_webhook(service, raw, sig, &error));
	}
	else
	{
		g_assert_true(venture_stripe_service_verify_authorization(service,
			venture_entity_get_id(VENTURE_ENTITY(authorization)), NULL, &error));
	}

	g_assert_no_error(error);
	{
		g_autoptr(VentureEntity) stored = venture_database_get(f->database, VENTURE_TYPE_STRIPE_AUTHORIZATION,
			venture_entity_get_id(VENTURE_ENTITY(authorization)), &error);
		g_autofree gchar *method = NULL, *status = NULL, *evidence = NULL;
		g_object_get(stored, "status", &status, "payment-method-id", &method, "evidence", &evidence, NULL);
		g_assert_cmpstr(status, ==, "active"); g_assert_cmpstr(method, ==, "pm_offline");
		g_assert_nonnull(evidence); g_assert_null(strstr(evidence, "client_secret"));
		g_test_message("Hosted permission verified: status=%s; retained minimal proof excludes client secrets", status);
		{
			gint64 version = venture_entity_get_version(stored);
			g_assert_true(venture_stripe_service_verify_authorization(service, venture_entity_get_id(stored), NULL, &error));
			g_clear_object(&stored);
			stored = venture_database_get(f->database, VENTURE_TYPE_STRIPE_AUTHORIZATION,
				venture_entity_get_id(VENTURE_ENTITY(authorization)), &error);
			g_assert_cmpint(venture_entity_get_version(stored), ==, version);
		}
		if (data && !g_strcmp0(data, "catalog"))
		{
			/* Retiring a price from new sales changes only its active flag;
			 * existing immutable commercial terms remain authorized. */
			g_object_set(price, "active", FALSE, NULL); save(f, price);
			g_assert_true(venture_stripe_service_verify_authorization(service, venture_entity_get_id(stored), NULL, &error));
			g_assert_no_error(error);
		}
		if (data && !g_strcmp0(data, "tenant-lifecycle"))
		{
			VentureTenantService *tenant = venture_tenant_service_get(f->database);
			const gchar *states[] = { "read_only", "suspended" };
			g_autoptr(GDateTime) now = venture_time_now();
			guint i;
			g_object_set(f->config, "hosted-enabled", TRUE,
				"hosted-workspace-id", "681123dc-a427-4e32-906d-679980293075",
				"hosted-origin", "https://collection.example.test", NULL);
			g_assert_true(venture_tenant_service_configure(tenant, f->config, &error));
			g_assert_true(venture_tenant_service_initialize(tenant, &error));
			g_assert_no_error(error);
			venture_context_set_stripe_service(f->context, service);
			venture_stripe_collection_start(f->context);
			for (i = 0; i < G_N_ELEMENTS(states); i++)
			{
				gint64 deadline;
				g_assert_true(venture_tenant_service_set_state_operator(tenant, states[i], "Pause collections", &error));
				g_assert_no_error(error);
				g_assert_cmpint(venture_stripe_service_collect_due(service, now, 1, NULL, &error), ==, -1);
				g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
				g_clear_error(&error);
				/* A background tick must also leave scheduling rows untouched;
				 * otherwise a suspended tenant accrues retry bookkeeping. */
				deadline = g_get_monotonic_time() + 6 * G_TIME_SPAN_SECOND;
				while (g_get_monotonic_time() < deadline)
				{
					while (g_main_context_iteration(NULL, FALSE)) { }
					g_usleep(10000);
				}
				g_assert_cmpuint(((FakeTransport *)transport)->payment_calls, ==, 0);
			}
			venture_stripe_collection_stop(f->context);
			g_assert_true(venture_tenant_service_set_state_operator(tenant, "active", "Resume authorized collection", &error));
			g_assert_no_error(error);
			{
				g_autoptr(VentureEntity) unchanged = venture_database_get(f->database,
					VENTURE_TYPE_STRIPE_AUTHORIZATION, venture_entity_get_id(stored), &error);
				g_assert_no_error(error); g_assert_nonnull(unchanged);
				g_assert_cmpint(venture_entity_get_version(unchanged), ==, venture_entity_get_version(stored));
			}
			g_assert_cmpint(venture_stripe_service_collect_due(service, now, 1, NULL, &error), ==, 1);
			g_assert_no_error(error);
			g_assert_cmpuint(((FakeTransport *)transport)->payment_calls, ==, 1);
			return;
		}
		if (data && !g_strcmp0(data, "finance"))
		{
			g_autoptr(VentureUser) user = venture_user_new();
			g_autoptr(VentureOrganizationMembership) membership = venture_organization_membership_new();
			g_autoptr(VentureAccessScope) scope = NULL;
			g_autoptr(GHashTable) params = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)json_node_free);
			g_autoptr(VentureEntity) result = NULL;
			VentureAuthPrincipal principal;
			VentureActor actor;
			g_object_set(user, "username", "collection-finance", "role", VENTURE_USER_ROLE_EDITOR, "active", TRUE, NULL); save(f, VENTURE_ENTITY(user));
			venture_entity_set_organization_id(VENTURE_ENTITY(membership), f->organization_id);
			g_object_set(membership, "user-id", venture_entity_get_id(VENTURE_ENTITY(user)), "role", VENTURE_ORGANIZATION_ROLE_FINANCE, "active", TRUE, NULL); save(f, VENTURE_ENTITY(membership));
			principal.user_id = venture_entity_get_id(VENTURE_ENTITY(user)); principal.token_id = 0;
			principal.role = VENTURE_USER_ROLE_EDITOR; principal.name = (gchar *)"collection-finance"; principal.authenticated = TRUE;
			venture_auth_to_actor(&principal, &actor); venture_context_set_stripe_service(f->context, service);
			scope = venture_access_policy_enter(venture_database_get_access_policy(f->database), &principal);
			result = venture_action_registry_perform(venture_database_get_action_registry(f->database), "stripe_authorization",
				venture_entity_get_id(stored), "verify", params, &actor, principal.role, &error);
			g_assert_no_error(error); g_assert_nonnull(result);
			g_clear_object(&result); g_clear_object(&scope);
			g_object_set(membership, "role", VENTURE_ORGANIZATION_ROLE_VIEWER, NULL); save(f, VENTURE_ENTITY(membership));
			scope = venture_access_policy_enter(venture_database_get_access_policy(f->database), &principal);
			result = venture_action_registry_perform(venture_database_get_action_registry(f->database), "stripe_authorization",
				venture_entity_get_id(stored), "verify", params, &actor, principal.role, &error);
			g_assert_null(result); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND); g_clear_error(&error);
		}
		if (data && !g_strcmp0(data, "scheduler"))
		{
			gint64 deadline;
			venture_context_set_stripe_service(f->context, service);
			venture_config_set_module_enabled(f->config, "billing", FALSE);
			venture_stripe_collection_start(f->context);
			deadline = g_get_monotonic_time() + 6 * G_TIME_SPAN_SECOND;
			while (g_get_monotonic_time() < deadline)
			{
				while (g_main_context_iteration(NULL, FALSE)) { }
				g_usleep(10000);
			}
			g_assert_cmpuint(((FakeTransport *)transport)->payment_calls, ==, 0);
			g_test_message("Main-context timer with billing disabled: provider payment requests=%u", ((FakeTransport *)transport)->payment_calls);
			venture_config_set_module_enabled(f->config, "billing", TRUE);
			deadline = g_get_monotonic_time() + 7 * G_TIME_SPAN_SECOND;
			while (!((FakeTransport *)transport)->payment_calls && g_get_monotonic_time() < deadline)
			{
				while (g_main_context_iteration(NULL, FALSE)) { }
				g_usleep(10000);
			}
			g_assert_cmpuint(((FakeTransport *)transport)->payment_calls, ==, 1);
			g_test_message("Billing enabled live: provider payment requests=%u; stopping timer", ((FakeTransport *)transport)->payment_calls);
			venture_stripe_collection_stop(f->context);
			g_assert_null(g_object_get_data(G_OBJECT(f->context), "stripe-collection-source"));
		}
		if (data && !g_strcmp0(data, "reconcile"))
		{
			g_autoptr(GPtrArray) events = rows(f, "subscription_event"), attempts = NULL;
			g_autoptr(VentureStripeCheckout) attempt = NULL, linked = NULL;
			FakeInvoice *remote;
			g_autofree gchar *reference = NULL;
			gint64 invoice_id, attempt_id;
			g_object_get(g_ptr_array_index(events, 0), "invoice-id", &invoice_id, NULL);
			((FakeTransport *)transport)->hide_invoice_response = TRUE;
			attempt = venture_stripe_service_collect_invoice(service, invoice_id, NULL, &error);
			g_assert_null(attempt); g_assert_nonnull(error); g_clear_error(&error);
			((FakeTransport *)transport)->hide_invoice_response = FALSE;
			attempts = rows(f, "stripe_checkout"); g_assert_cmpuint(attempts->len, ==, 1);
			attempt_id = venture_entity_get_id(g_ptr_array_index(attempts, 0));
			remote = fake_invoice_latest((FakeTransport *)transport);
			reference = g_strdup(remote->reference); g_free(remote->reference); remote->reference = g_uuid_string_random();
			linked = venture_stripe_service_reconcile_collection(service, attempt_id, remote->id, NULL, &error);
			g_assert_null(linked); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION); g_clear_error(&error);
			g_free(remote->reference); remote->reference = g_strdup(reference);
			linked = venture_stripe_service_reconcile_collection(service, attempt_id, remote->id, NULL, &error);
			g_assert_no_error(error); g_assert_nonnull(linked);
			g_assert_true(venture_stripe_service_cancel_collection(service, attempt_id, NULL, &error));
			g_assert_no_error(error); g_assert_true(remote->deleted);
			g_test_message("Lost create response: wrong correlation refused; original invoice=%s linked and confirmed deleted=%d", remote->id, remote->deleted);
		}
		if (data && !g_strcmp0(data, "sweep"))
		{
			g_autoptr(GDateTime) now = venture_time_now();
			g_autoptr(GPtrArray) attempts = NULL;
			g_assert_cmpint(venture_stripe_service_collect_due(service, now, 1, NULL, &error), ==, 1);
			g_assert_no_error(error);
			attempts = rows(f, "stripe_checkout"); g_assert_cmpuint(attempts->len, ==, 1);
			g_assert_cmpint(venture_stripe_service_collect_due(service, now, 1, NULL, &error), ==, 0);
			g_assert_no_error(error); g_assert_cmpuint(((FakeTransport *)transport)->payment_calls, ==, 1);
			{
				g_autoptr(GPtrArray) invoices = rows(f, "invoice");
				gint64 original_id, collected_id;
				g_object_get(action, "invoice-id", &original_id, NULL);
				g_object_get(g_ptr_array_index(attempts, 0), "invoice-id", &collected_id, NULL);
				g_assert_cmpuint(invoices->len, ==, 2);
				/* Backlog renewal must leave collection on the oldest unsettled
				 * period, rather than jumping to the newly generated invoice. */
				g_assert_cmpint(collected_id, ==, original_id);
				g_test_message("Due renewal reused billing lifecycle: invoices=%u; collected oldest invoice=%" G_GINT64_FORMAT, invoices->len, collected_id);
			}
		}
		if (data && (!g_strcmp0(data, "collection") || !g_strcmp0(data, "settlement") || !g_strcmp0(data, "failure-recovery") || !g_strcmp0(data, "paused-recovery") || !g_strcmp0(data, "cancel") || !g_strcmp0(data, "retry") || !g_strcmp0(data, "manual-conflict") || !g_strcmp0(data, "manual-recovery") || !g_strcmp0(data, "pay-retry")))
		{
			g_autoptr(GPtrArray) events = rows(f, "subscription_event");
			g_autoptr(GPtrArray) payments = NULL;
			g_autoptr(VentureStripeCheckout) checkout = NULL, again = NULL;
			g_autofree gchar *checkout_status = NULL, *channel = NULL;
			gint64 invoice_id;
			g_object_get(g_ptr_array_index(events, 0), "invoice-id", &invoice_id, NULL);
			g_assert_cmpint(invoice_id, >, 0);
			((FakeTransport *)transport)->reservation_database = f->database;
			if (!g_strcmp0(data, "pay-retry"))
			{
				g_autoptr(GPtrArray) retained = NULL;
				g_autofree gchar *phase = NULL;
				((FakeTransport *)transport)->hide_pay_response = TRUE;
				checkout = venture_stripe_service_collect_invoice(service, invoice_id, NULL, &error);
				g_assert_null(checkout); g_assert_nonnull(error); g_clear_error(&error);
				retained = rows(f, "stripe_checkout"); g_assert_cmpuint(retained->len, ==, 1);
				g_object_get(g_ptr_array_index(retained, 0), "collection-phase", &phase, NULL);
				g_assert_cmpstr(phase, ==, "paying");
				((FakeTransport *)transport)->hide_pay_response = FALSE;
				g_clear_object(&service);
				service = venture_stripe_service_for_organization(f->database, f->organization_id, STRIPE_TRANSPORT(transport), &error);
				g_assert_no_error(error); g_assert_nonnull(service);
			}
			checkout = venture_stripe_service_collect_invoice(service, invoice_id, NULL, &error);
			g_assert_no_error(error); g_assert_nonnull(checkout);
			if (!g_strcmp0(data, "pay-retry"))
			{
				g_assert_cmpuint(((FakeTransport *)transport)->payment_calls, ==, 1);
				g_test_message("Truncated pay response retained paying phase; reconstructed client reused durable key; provider payment requests=%u", ((FakeTransport *)transport)->payment_calls);
			}
			g_object_get(checkout, "status", &checkout_status, "channel", &channel, NULL);
			g_assert_cmpstr(checkout_status, ==, "processing"); g_assert_cmpstr(channel, ==, "automatic");
			g_test_message("Reserved collection: channel=%s, status=%s; pay request has not posted a receipt", channel, checkout_status);
			again = venture_stripe_service_collect_invoice(service, invoice_id, NULL, &error);
			g_assert_no_error(error); g_assert_cmpint(venture_entity_get_id(VENTURE_ENTITY(again)), ==, venture_entity_get_id(VENTURE_ENTITY(checkout)));
			payments = rows(f, "payment"); g_assert_cmpuint(payments->len, ==, 0);
			if (!g_strcmp0(data, "manual-conflict") || !g_strcmp0(data, "manual-recovery"))
			{
				g_autoptr(VenturePayment) manual = venture_payment_new();
				g_autoptr(VentureMoney) received = venture_money_new(!g_strcmp0(data, "manual-conflict") ? 2500 : 5000, "USD", 2);
				g_autoptr(GDateTime) paid_at = venture_time_now();
				venture_entity_set_organization_id(VENTURE_ENTITY(manual), f->organization_id);
				g_object_set(manual, "customer-id", f->customer_id, "invoice-id", invoice_id, "amount", received, "date", paid_at, "method", "manual", NULL);
				g_assert_true(venture_settlement_service_apply_payment(venture_settlement_service_get(f->database), manual, NULL, NULL, &error));
				g_assert_no_error(error);
				if (!g_strcmp0(data, "manual-conflict"))
				{
					g_assert_false(automatic_fixture_event(service, (FakeTransport *)transport, "evt_paid_after_manual", TRUE, &error));
					g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION); g_clear_error(&error);
					g_clear_pointer(&payments, g_ptr_array_unref); payments = rows(f, "payment");
					g_assert_cmpuint(payments->len, ==, 1);
					g_test_message("Verified provider payment after partial manual receipt: reconciliation required; retained payment rows=%u", payments->len);
				}
				else
				{
					g_autoptr(VentureEntity) sub = NULL;
					gint sub_status;
					g_assert_true(automatic_fixture_event(service, (FakeTransport *)transport, "evt_failed_after_manual", FALSE, &error));
					g_assert_no_error(error);
					g_assert_true(venture_stripe_service_cancel_collection(service, venture_entity_get_id(VENTURE_ENTITY(checkout)), NULL, &error));
					g_assert_no_error(error);
					sub = venture_database_get(f->database, VENTURE_TYPE_CUSTOMER_SUBSCRIPTION, subscription_id, &error);
					g_object_get(sub, "status", &sub_status, NULL); g_assert_cmpint(sub_status, ==, 1);
					g_test_message("Manual full receipt plus authenticated provider void: subscription status=%d (active); no replacement collection", sub_status);
				}
			}
			if (!g_strcmp0(data, "cancel"))
			{
				g_assert_false(venture_stripe_service_cancel_collection(service, venture_entity_get_id(VENTURE_ENTITY(checkout)), NULL, &error));
				g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION); g_clear_error(&error);
			}
			if (!g_strcmp0(data, "failure-recovery") || !g_strcmp0(data, "paused-recovery") || !g_strcmp0(data, "cancel") || !g_strcmp0(data, "retry"))
			{
				g_autofree gchar *body = g_strdup_printf("{\"id\":\"evt_auto_failed\",\"type\":\"invoice.payment_failed\",\"api_version\":\"2024-06-20\",\"livemode\":false,\"created\":%" G_GINT64_FORMAT ",\"data\":{\"object\":{\"id\":\"in_auto\",\"customer\":\"cus_offline\",\"currency\":\"usd\",\"amount_paid\":0,\"payment_intent\":\"pi_auto\"}}}", g_get_real_time() / G_USEC_PER_SEC);
				g_autofree gchar *sig = binding_signature(body, "whsec_original");
				g_autoptr(GBytes) raw = g_bytes_new(body, strlen(body));
				g_autoptr(VentureEntity) sub = NULL;
				gint sub_status;
				fake_invoice_latest((FakeTransport *)transport)->processing = FALSE;
				g_assert_true(venture_stripe_service_handle_webhook(service, raw, sig, &error));
				g_assert_no_error(error);
				sub = venture_database_get(f->database, VENTURE_TYPE_CUSTOMER_SUBSCRIPTION, subscription_id, &error);
				g_object_get(sub, "status", &sub_status, NULL); g_assert_cmpint(sub_status, ==, 2);
				if (!g_strcmp0(data, "paused-recovery"))
				{
					g_autoptr(VentureEntity) pause = record_new(f, "billing_request");
					g_autoptr(GDateTime) at = venture_time_now();
					g_object_set(pause, "action", "pause", "subscription-id", subscription_id, "at", at, NULL);
					save(f, pause);
				}
			}
			if (!g_strcmp0(data, "cancel"))
			{
				g_autoptr(VentureStripeCheckout) hosted = NULL;
				g_assert_true(venture_stripe_service_cancel_collection(service, venture_entity_get_id(VENTURE_ENTITY(checkout)), NULL, &error));
				g_assert_no_error(error);
				g_clear_object(&again);
				again = venture_stripe_service_collect_invoice(service, invoice_id, NULL, &error);
				g_assert_null(again); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION); g_clear_error(&error);
				hosted = venture_stripe_service_checkout(service, invoice_id, NULL, &error);
				g_assert_no_error(error); g_assert_nonnull(hosted);
				g_clear_pointer(&channel, g_free); g_object_get(hosted, "channel", &channel, NULL);
				g_assert_cmpstr(channel, ==, "hosted");
			}
			if (!g_strcmp0(data, "retry"))
			{
				g_autoptr(GDateTime) now = venture_time_now(), later = g_date_time_add_days(now, 2);
				g_autoptr(VentureStripeCheckout) replacement = NULL;
				g_autofree gchar *provider = NULL;
				gint64 number;
				/* A failed invoice cannot be charged again before the delay, and
				 * each retry must close its old provider invoice before reserving a new one. */
				replacement = venture_stripe_service_retry_collection(service, venture_entity_get_id(VENTURE_ENTITY(checkout)), now, NULL, &error);
				g_assert_null(replacement); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION); g_clear_error(&error);
				replacement = venture_stripe_service_retry_collection(service, venture_entity_get_id(VENTURE_ENTITY(checkout)), later, NULL, &error);
				g_assert_no_error(error); g_assert_nonnull(replacement);
				g_assert_cmpint(venture_entity_get_id(VENTURE_ENTITY(replacement)), !=, venture_entity_get_id(VENTURE_ENTITY(checkout)));
				g_object_get(replacement, "provider-invoice-id", &provider, "pay-attempt", &number, NULL);
				g_assert_cmpstr(provider, ==, "in_auto_2"); g_assert_cmpint(number, ==, 2);
				g_test_message("Retry after recorded delay: old invoice voided, replacement=%s, attempt=%" G_GINT64_FORMAT, provider, number);
				g_assert_true(((FakeInvoice *)g_hash_table_lookup(((FakeTransport *)transport)->invoices, "in_auto"))->voided);
				g_assert_cmpuint(((FakeTransport *)transport)->payment_calls, ==, 2);
				g_assert_true(automatic_fixture_event(service, (FakeTransport *)transport, "evt_failed_second", FALSE, &error));
				g_assert_no_error(error);
				g_clear_object(&checkout); checkout = g_steal_pointer(&replacement);
				replacement = venture_stripe_service_retry_collection(service, venture_entity_get_id(VENTURE_ENTITY(checkout)), later, NULL, &error);
				g_assert_no_error(error); g_assert_nonnull(replacement);
				g_assert_cmpuint(((FakeTransport *)transport)->payment_calls, ==, 3);
				g_assert_true(automatic_fixture_event(service, (FakeTransport *)transport, "evt_failed_third", FALSE, &error));
				g_assert_no_error(error);
				g_clear_object(&checkout); checkout = g_steal_pointer(&replacement);
				replacement = venture_stripe_service_retry_collection(service, venture_entity_get_id(VENTURE_ENTITY(checkout)), later, NULL, &error);
				g_assert_null(replacement); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION); g_clear_error(&error);
				g_assert_cmpuint(((FakeTransport *)transport)->payment_calls, ==, 3);
				g_test_message("Fourth automatic attempt refused; actual provider payment requests=%u", ((FakeTransport *)transport)->payment_calls);
			}
			if (g_strcmp0(data, "collection") && g_strcmp0(data, "cancel") && g_strcmp0(data, "retry") && g_strcmp0(data, "manual-conflict") && g_strcmp0(data, "manual-recovery"))
			{
				g_autofree gchar *body = g_strdup_printf("{\"id\":\"evt_auto_paid\",\"type\":\"invoice.payment_succeeded\",\"api_version\":\"2024-06-20\",\"livemode\":false,\"created\":%" G_GINT64_FORMAT ",\"data\":{\"object\":{\"id\":\"in_auto\",\"customer\":\"cus_offline\",\"currency\":\"usd\",\"amount_paid\":5000,\"payment_intent\":\"pi_auto\"}}}", g_get_real_time() / G_USEC_PER_SEC);
				g_autofree gchar *sig = binding_signature(body, "whsec_original");
				g_autoptr(GBytes) raw = g_bytes_new(body, strlen(body));
				fake_invoice_latest((FakeTransport *)transport)->paid = TRUE;
				g_assert_true(venture_stripe_service_handle_webhook(service, raw, sig, &error));
				g_assert_no_error(error);
				g_clear_pointer(&payments, g_ptr_array_unref); payments = rows(f, "payment");
				g_assert_cmpuint(payments->len, ==, 1);
				{
					g_autoptr(VentureEntity) sub = venture_database_get(f->database, VENTURE_TYPE_CUSTOMER_SUBSCRIPTION, subscription_id, &error);
					gint sub_status;
					g_object_get(sub, "status", &sub_status, NULL);
					g_assert_cmpint(sub_status, ==, !g_strcmp0(data, "paused-recovery") ? 3 : 1);
					g_test_message("Verified settlement: payment rows=%u, subscription status=%d (active=1, paused=3)", payments->len, sub_status);
				}

				g_assert_true(venture_stripe_service_handle_webhook(service, raw, sig, &error));
				g_assert_no_error(error);
				g_clear_pointer(&payments, g_ptr_array_unref); payments = rows(f, "payment");
				g_assert_cmpuint(payments->len, ==, 1);
			}

		}
		g_assert_true(venture_stripe_service_revoke_authorization(service, venture_entity_get_id(stored), NULL, &error));
		g_assert_no_error(error);
		g_assert_false(venture_stripe_service_verify_authorization(service, venture_entity_get_id(stored), NULL, &error));
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);

	}
}
