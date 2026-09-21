/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* A copied URL is a narrowly bound capability, not an invoice selector or a
 * persisted bearer secret. Expiry, revocation and invoice edits invalidate it. */
static void
test_payment_link(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GObject) transport = g_object_new(fake_transport_get_type(), NULL);
	g_autoptr(JsonNode) settings = binding_settings("whsec_original");
	g_autoptr(GBytes) key = g_bytes_new_static("01234567890123456789012345678901", 32);
	g_autoptr(VentureIntegrationConnection) connection = NULL;
	g_autoptr(VentureStripeService) service = NULL;
	g_autoptr(VentureEntity) invoice = invoice_new(f, "LINK-1", "2026-01-01", "100 USD");
	g_autoptr(VentureStripePaymentLink) link = NULL, resolved = NULL;
	g_autoptr(VentureStripeCheckout) checkout = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(GDateTime) expires = g_date_time_add_hours(now, 2);
	g_autoptr(GDateTime) later = g_date_time_add_hours(now, 3);
	g_autofree gchar *url = NULL, *hash = NULL, *saved_url = NULL;
	g_autofree gchar *bad = NULL;
	const gchar *token;
	const gchar *mode = data;
	g_assert_true(venture_integration_service_set_key(venture_integration_service_get(f->database), key, &error));
	connection = venture_stripe_settings_configure(f->database, f->organization_id, settings, 0, 0, STRIPE_TRANSPORT(transport), NULL, &error);
	g_assert_no_error(error);
	service = venture_stripe_service_for_organization(f->database, f->organization_id, STRIPE_TRANSPORT(transport), &error);
	g_assert_no_error(error);
	if (!g_strcmp0(mode, "finance"))
	{
		g_autoptr(VentureUser) user = venture_user_new();
		g_autoptr(VentureOrganizationMembership) membership = venture_organization_membership_new();
		g_autoptr(VentureAccessScope) scope = NULL;
		g_autoptr(GHashTable) parameters = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)json_node_free);
		g_autoptr(VentureEntity) result = NULL;
		VentureAuthPrincipal principal;
		VentureActor actor;
		g_object_set(user, "username", "link-finance", "role", VENTURE_USER_ROLE_EDITOR, "active", TRUE, NULL);
		save(f, VENTURE_ENTITY(user));
		venture_entity_set_organization_id(VENTURE_ENTITY(membership), f->organization_id);
		g_object_set(membership, "user-id", venture_entity_get_id(VENTURE_ENTITY(user)), "role", VENTURE_ORGANIZATION_ROLE_FINANCE, "active", TRUE, NULL);
		save(f, VENTURE_ENTITY(membership));
		principal.user_id = venture_entity_get_id(VENTURE_ENTITY(user)); principal.token_id = 0;
		principal.role = VENTURE_USER_ROLE_EDITOR; principal.name = (gchar *)"link-finance"; principal.authenticated = TRUE;
		venture_auth_to_actor(&principal, &actor);
		venture_context_set_stripe_service(f->context, service);
		g_object_set(f->config, "server-base-url", "https://venture.example", NULL);
		scope = venture_access_policy_enter(venture_database_get_access_policy(f->database), &principal);
		result = venture_action_registry_perform(venture_database_get_action_registry(f->database), "invoice",
			venture_entity_get_id(invoice), "payment_link", parameters, &actor, principal.role, &error);
		g_assert_no_error(error); g_assert_nonnull(result);
		return;
	}
	if (!g_strcmp0(mode, "authorization"))
	{
		VentureAuthPrincipal principal;
		g_autoptr(VentureAccessScope) scope = NULL;
		guint calls = ((FakeTransport *)transport)->calls;
		principal.user_id = 0; principal.token_id = 0;
		principal.role = VENTURE_USER_ROLE_EDITOR;
		principal.name = (gchar *)"unrelated-editor"; principal.authenticated = TRUE;
		scope = venture_access_policy_enter(venture_database_get_access_policy(f->database), &principal);
		link = venture_stripe_service_create_payment_link(service, venture_entity_get_id(invoice), "https://venture.example", expires, NULL, &error);
		g_assert_null(link); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
		g_assert_cmpuint(((FakeTransport *)transport)->calls, ==, calls);
		return;
	}
	link = venture_stripe_service_create_payment_link(service, venture_entity_get_id(invoice), "https://venture.example", expires, NULL, &error);
	g_assert_no_error(error); g_assert_nonnull(link);
	g_object_get(link, "url", &url, "token-hash", &hash, NULL);
	g_assert_true(g_str_has_prefix(url, "https://venture.example/pay/"));
	token = url + strlen("https://venture.example/pay/");
	g_assert_cmpuint(strlen(token), ==, 101);
	g_assert_null(strstr(hash, token + 37));
	resolved = venture_stripe_service_resolve_payment_link(f->database, token, now, &error);
	g_assert_no_error(error); g_assert_nonnull(resolved);
	g_object_get(resolved, "url", &saved_url, NULL);
	g_assert_true(!saved_url || !*saved_url);
	if (!g_strcmp0(mode, "changed") || !g_strcmp0(mode, "deleted"))
	{
		if (!g_strcmp0(mode, "deleted"))
		{
			/* Current writers cannot delete issued history, but an older
			 * imported database may already contain a deleted invoice. */
			g_autofree gchar *sql = g_strdup_printf("UPDATE invoices SET deleted_at = '2026-09-01T00:00:00Z' WHERE id = %" G_GINT64_FORMAT, venture_entity_get_id(invoice));
			g_assert_true(venture_database_execute(f->database, sql, NULL, &error));
			g_assert_no_error(error);
		}
		else
		{
			g_object_set(invoice, "notes", "Revised terms", NULL);
			save(f, invoice);
		}
		g_clear_object(&resolved);
		resolved = venture_stripe_service_resolve_payment_link(f->database, token, now, &error);
		g_assert_null(resolved); g_assert_nonnull(error);
		return;
	}
	if (!g_strcmp0(mode, "replacement"))
	{
		g_autoptr(VentureStripePaymentLink) next = venture_stripe_service_create_payment_link(service,
			venture_entity_get_id(invoice), "https://venture.example", expires, NULL, &error);
		g_assert_no_error(error); g_assert_nonnull(next);
		g_clear_object(&resolved);
		resolved = venture_stripe_service_resolve_payment_link(f->database, token, now, &error);
		g_assert_null(resolved); g_assert_nonnull(error);
		return;
	}
	if (!g_strcmp0(mode, "uncertain"))
	{
		g_autoptr(VentureStripePaymentLink) next = NULL;
		g_autoptr(GPtrArray) attempts = NULL;
		((FakeTransport *)transport)->checkout_drop = TRUE;
		checkout = venture_stripe_service_pay_link(service, token, now, &error);
		g_assert_null(checkout); g_assert_nonnull(error); g_clear_error(&error);
		g_assert_false(venture_stripe_service_revoke_payment_link(service, venture_entity_get_id(VENTURE_ENTITY(link)), NULL, &error));
		g_assert_nonnull(error); g_clear_error(&error);
		next = venture_stripe_service_create_payment_link(service, venture_entity_get_id(invoice), "https://venture.example", expires, NULL, &error);
		g_assert_null(next); g_assert_nonnull(error); g_clear_error(&error);
		attempts = rows(f, "stripe_checkout"); g_assert_cmpuint(attempts->len, ==, 1);
		((FakeTransport *)transport)->checkout_drop = FALSE;
		checkout = venture_stripe_service_checkout(service, venture_entity_get_id(invoice), NULL, &error);
		g_assert_no_error(error); g_assert_nonnull(checkout);
		return;
	}
	bad = g_strdup(token); bad[100] = bad[100] == 'a' ? 'b' : 'a';
	g_clear_object(&resolved);
	resolved = venture_stripe_service_resolve_payment_link(f->database, bad, now, &error);
	g_assert_null(resolved); g_assert_nonnull(error); g_clear_error(&error);
	resolved = venture_stripe_service_resolve_payment_link(f->database, token, later, &error);
	g_assert_null(resolved); g_assert_nonnull(error); g_clear_error(&error);
	{
		g_autoptr(VentureWebServer) server = NULL;
		g_autofree gchar *state_directory = NULL, *path = g_strdup_printf("/pay/%s", token), *body = NULL;
		venture_context_set_stripe_service(f->context, service);
		server = start_server(f, &state_directory);
		if (!g_strcmp0(mode, "form") || !g_strcmp0(mode, "api"))
		{
			g_autofree gchar *action_path = g_strdup_printf("/api/v1/invoice/%" G_GINT64_FORMAT "/actions/payment_link", venture_entity_get_id(invoice));
			g_object_set(f->config, "server-base-url", "https://venture.example", NULL);
			g_assert_cmpuint(http_request(server, "POST", action_path, !g_strcmp0(mode, "api") ? "application/json" : "application/x-www-form-urlencoded", !g_strcmp0(mode, "api") ? "{}" : "", &body), ==, 200);
			g_assert_nonnull(strstr(body, "https://venture.example/pay/"));
			if (g_strcmp0(mode, "api")) g_assert_nonnull(strstr(body, "Copy payment link"));
			g_assert_null(strstr(body, "token_hash"));
			venture_web_server_stop(server);
			venture_test_remove_tree(state_directory);
			return;
		}
		g_assert_cmpuint(http_request(server, "GET", path, NULL, NULL, &body), ==, 200);
		g_assert_nonnull(strstr(body, "100.00 USD"));
		g_assert_null(strstr(body, "LINK-1"));
		g_assert_null(strstr(body, "Customer"));
		g_assert_cmpuint(http_request(server, "POST", path, "application/x-www-form-urlencoded", "invoice_id=999999", NULL), ==, 303);
		g_assert_cmpuint(http_request(server, "GET", "/pay/invalid", NULL, NULL, NULL), ==, 404);
		venture_web_server_stop(server);
		venture_test_remove_tree(state_directory);
	}
	checkout = venture_stripe_service_pay_link(service, token, now, &error);
	g_assert_no_error(error); g_assert_nonnull(checkout);
	g_assert_cmpint(g_ascii_strtoll(((FakeTransport *)transport)->deadline, NULL, 10), <=, g_date_time_to_unix(expires));
	if (!g_strcmp0(mode, "paid") || !g_strcmp0(mode, "processing"))
	{
		gboolean processing = !g_strcmp0(mode, "processing");
		g_autofree gchar *body = g_strdup_printf("{\"id\":\"evt_link_completion\",\"livemode\":false,\"type\":\"checkout.session.completed\",\"data\":{\"object\":{\"id\":\"cs_offline\",\"amount_total\":10000,\"currency\":\"usd\",\"payment_status\":\"%s\",\"payment_intent\":\"pi_link\"}}}", processing ? "unpaid" : "paid");
		g_autofree gchar *sig = binding_signature(body, "whsec_original");
		g_autoptr(GBytes) raw = g_bytes_new(body, strlen(body));
		g_assert_true(venture_stripe_service_handle_webhook(service, raw, sig, &error));
		g_assert_no_error(error);
		if (processing)
		{
			guint attempts = ((FakeTransport *)transport)->checkouts;
			g_clear_object(&checkout);
			checkout = venture_stripe_service_pay_link(service, token, now, &error);
			g_assert_no_error(error); g_assert_nonnull(checkout);
			g_assert_cmpuint(((FakeTransport *)transport)->checkouts, ==, attempts);
			g_assert_true(venture_stripe_service_revoke_payment_link(service, venture_entity_get_id(VENTURE_ENTITY(link)), NULL, &error));
			g_assert_no_error(error);
			g_clear_pointer(&body, g_free); g_clear_pointer(&sig, g_free); g_clear_pointer(&raw, g_bytes_unref);
			body = g_strdup("{\"id\":\"evt_link_settled\",\"livemode\":false,\"type\":\"checkout.session.async_payment_succeeded\",\"data\":{\"object\":{\"id\":\"cs_offline\",\"amount_total\":10000,\"currency\":\"usd\",\"payment_status\":\"paid\",\"payment_intent\":\"pi_link\"}}}");
			sig = binding_signature(body, "whsec_original"); raw = g_bytes_new(body, strlen(body));
			g_assert_true(venture_stripe_service_handle_webhook(service, raw, sig, &error));
			g_assert_no_error(error);
		}
		g_clear_object(&resolved);
		resolved = venture_stripe_service_resolve_payment_link(f->database, token, now, &error);
		g_assert_null(resolved); g_assert_nonnull(error);
		return;
	}
	g_assert_true(venture_stripe_service_revoke_payment_link(service, venture_entity_get_id(VENTURE_ENTITY(link)), NULL, &error));
	g_assert_no_error(error);
	resolved = venture_stripe_service_resolve_payment_link(f->database, token, now, &error);
	g_assert_null(resolved); g_assert_nonnull(error);
}
