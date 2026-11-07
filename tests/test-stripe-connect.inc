/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * Connect with Stripe: a Standard-style connected account per organization,
 * made and read with the installation's platform keys (stripe.connect.*).
 * Every charge is then a direct charge on the owner's own account, and one
 * Connect webhook routes each event to its business by the event's account.
 * Ben chose this on 2026-11-06; there is no platform fee anywhere.
 */

#define CONNECT_SECRET "sk_test_platform_fixture"
#define CONNECT_WEBHOOK "whsec_connect_fixture"

/* The connected-account side of the provider fixture: v2 creation, the v1
 * read and onboarding links. Everything else falls through to fake_send. */
static StripeResponse *
fake_connect_send(FakeTransport *self, const StripeHttpRequest *request)
{
	if (strstr(request->url, "/v2/core/accounts"))
	{
		g_autofree gchar *body = g_strdup_printf("{\"id\":\"%s\",\"object\":\"v2.core.account\",\"dashboard\":\"full\",\"livemode\":false}",
			self->connect_id ? self->connect_id : "acct_connectA");
		self->account_creates++;
		g_assert_cmpstr(request->method, ==, "POST");
		g_free(self->create_body); self->create_body = g_strdup(request->body);
		g_free(self->create_key); self->create_key = g_strdup(request->idempotency_key);
		g_free(self->create_version); self->create_version = g_strdup(request->api_version);
		g_free(self->create_type); self->create_type = g_strdup(request->content_type);
		g_free(self->create_authorization); self->create_authorization = g_strdup(request->authorization);
		return stripe_response_new(200, body, NULL, NULL);
	}
	if (strstr(request->url, "/v1/accounts/"))
	{
		const gchar *id = strstr(request->url, "/v1/accounts/") + strlen("/v1/accounts/");
		g_autofree gchar *body = g_strdup_printf("{\"id\":\"%s\",\"object\":\"account\",\"type\":\"standard\",\"charges_enabled\":%s,\"details_submitted\":%s,\"requirements\":{\"currently_due\":%s,\"disabled_reason\":%s}}",
			id, self->charges_enabled ? "true" : "false", self->charges_enabled ? "true" : "false",
			self->charges_enabled ? "[]" : "[\"external_account\"]", self->charges_enabled ? "null" : "\"requirements.past_due\"");
		self->account_reads++;
		g_assert_cmpstr(request->method, ==, "GET");
		g_free(self->read_header); self->read_header = g_strdup(request->stripe_account);
		g_free(self->read_authorization); self->read_authorization = g_strdup(request->authorization);
		return stripe_response_new(200, body, NULL, NULL);
	}
	self->account_links++;
	g_assert_cmpstr(request->method, ==, "POST");
	g_free(self->link_body); self->link_body = g_strdup(request->body);
	g_free(self->link_header); self->link_header = g_strdup(request->stripe_account);
	return stripe_response_new(200, "{\"object\":\"account_link\",\"created\":1793923200,\"expires_at\":1793923500,\"url\":\"https://connect.stripe.com/setup/s/acct_fixture/fixture\"}", NULL, NULL);
}

static void
connect_platform(Fixture *f)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) key = g_bytes_new_static("01234567890123456789012345678901", 32);
	g_assert_true(venture_integration_service_set_key(venture_integration_service_get(f->database), key, &error));
	g_assert_no_error(error);
	g_object_set(f->config, "stripe-connect-secret-key", CONNECT_SECRET,
		"stripe-connect-publishable-key", "pk_test_platform_fixture",
		"stripe-connect-webhook-secret", CONNECT_WEBHOOK,
		"stripe-connect-environment", "test",
		"stripe-connect-api-version", "2024-06-20", NULL);
}

static JsonNode *
connect_start(Fixture *f, gint64 organization, FakeTransport *transport, const gchar *success, GError **error)
{
	return venture_stripe_connect_start(f->database, organization,
		"https://app.example.test/setup/connections", "https://app.example.test/setup/connections?again=1",
		success, "https://app.example.test/pay/cancel", STRIPE_TRANSPORT(transport), NULL, error);
}

/* The sealed settings of the organization's enabled Stripe connection. */
static JsonNode *
connect_settings(Fixture *f, gint64 organization, gchar **account)
{
	g_autoptr(GError) error = NULL;
	VentureIntegrationService *integrations = venture_integration_service_get(f->database);
	g_autoptr(VentureIntegrationConnection) connection = venture_integration_service_find(integrations, organization, "stripe", &error);
	JsonNode *settings;
	g_assert_no_error(error); g_assert_nonnull(connection);
	if (account) g_object_get(connection, "account-id", account, NULL);
	settings = venture_integration_service_resolve(integrations, organization,
		venture_entity_get_id(VENTURE_ENTITY(connection)), FALSE, &error);
	g_assert_no_error(error); g_assert_nonnull(settings);
	return settings;
}

static void
assert_connect_answer(JsonNode *answer, const gchar *state, const gchar *mode)
{
	JsonObject *object;
	g_assert_nonnull(answer);
	object = json_node_get_object(answer);
	g_assert_cmpstr(json_object_get_string_member(object, "state"), ==, state);
	if (mode) g_assert_cmpstr(json_object_get_string_member(object, "mode"), ==, mode);
	else g_assert_true(json_object_get_null_member(object, "mode"));
}

static gint64
connect_second_business(Fixture *f, const gchar *name, const gchar *slug, gint64 *customer, gint64 *product)
{
	static const gchar *const codes[] = { "1000", "1100", "4000", "2100" };
	g_autoptr(VentureOrganization) organization = venture_organization_new();
	g_autoptr(VentureCompany) company = venture_company_new();
	g_autoptr(VentureProduct) work = venture_product_new();
	gint64 id;
	guint j;
	g_object_set(organization, "name", name, "slug", slug, NULL);
	save(f, VENTURE_ENTITY(organization));
	id = venture_entity_get_id(VENTURE_ENTITY(organization));
	for (j = 0; j < G_N_ELEMENTS(codes); j++)
	{
		g_autoptr(VentureAccount) account = venture_account_new();
		g_object_set(account, "name", "Connect account", "code", codes[j], "active", TRUE,
			"kind", j < 2 ? VENTURE_ACCOUNT_KIND_ASSET : (j == 2 ? VENTURE_ACCOUNT_KIND_INCOME : VENTURE_ACCOUNT_KIND_LIABILITY), NULL);
		venture_entity_set_organization_id(VENTURE_ENTITY(account), id);
		save(f, VENTURE_ENTITY(account));
	}
	g_object_set(company, "name", "Connect customer", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(company), id);
	save(f, VENTURE_ENTITY(company));
	*customer = venture_entity_get_id(VENTURE_ENTITY(company));
	g_object_set(work, "name", "Connect work", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(work), id);
	save(f, VENTURE_ENTITY(work));
	*product = venture_entity_get_id(VENTURE_ENTITY(work));
	return id;
}

/* A request with an exact Stripe-Signature, or none. */
static guint
connect_http(VentureWebServer *server, const gchar *method, const gchar *path,
	const gchar *sig, const gchar *body, gchar **out)
{
	g_autoptr(SoupSession) session = soup_session_new_with_options("timeout", 15, NULL);
	g_autofree gchar *url = g_strconcat(venture_web_server_get_base_url(server), path, NULL);
	g_autoptr(SoupMessage) message = soup_message_new(method, url);
	SurfaceResult response;
	guint status;
	memset(&response, 0, sizeof(response));
	if (sig) soup_message_headers_replace(soup_message_get_request_headers(message), "Stripe-Signature", sig);
	if (body)
	{
		g_autoptr(GBytes) bytes = g_bytes_new(body, strlen(body));
		soup_message_set_request_body_from_bytes(message, "application/json", bytes);
	}
	soup_session_send_and_read_async(session, message, G_PRIORITY_DEFAULT, NULL, http_done, &response);
	while (!response.done)
		g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(response.error);
	if (out) *out = g_strndup(g_bytes_get_data(response.bytes, NULL), g_bytes_get_size(response.bytes));
	status = soup_message_get_status(message);
	g_clear_pointer(&response.bytes, g_bytes_unref);
	return status;
}

static gboolean
connect_event(Fixture *f, FakeTransport *transport, const gchar *body, const gchar *secret, GError **error)
{
	g_autofree gchar *sig = binding_signature(body, secret);
	g_autoptr(GBytes) bytes = g_bytes_new(body, strlen(body));
	return venture_stripe_connect_webhook(f->database, bytes, sig, STRIPE_TRANSPORT(transport), error);
}

/* A new business gets one Standard-style account, made once, and a link. */
static void
test_connect_create(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GObject) object = g_object_new(fake_transport_get_type(), NULL);
	FakeTransport *transport = (FakeTransport *)object;
	g_autoptr(JsonNode) answer = NULL, again = NULL, settings = NULL;
	g_autoptr(VentureEntity) organization = NULL;
	g_autofree gchar *account = NULL, *expected_key = NULL, *expected_body = NULL;
	g_autoptr(GHashTable) link = NULL;
	JsonObject *stored;
	(void)data;
	connect_platform(f);
	organization = venture_database_get(f->database, VENTURE_TYPE_ORGANIZATION, f->organization_id, &error);
	g_assert_no_error(error);
	g_object_set(organization, "name", "Example \"Plumbing\"", "email", "owner@example.test", NULL);
	save(f, organization);
	answer = connect_start(f, f->organization_id, transport, "https://app.example.test/pay/done", &error);
	g_assert_no_error(error);
	assert_connect_answer(answer, "needs_more", "connect");
	g_assert_cmpstr(json_object_get_string_member(json_node_get_object(answer), "url"), ==,
		"https://connect.stripe.com/setup/s/acct_fixture/fixture");
	/* The account: v2, the pinned version, JSON, the platform key, no
	 * connected-account header, the Standard-style fields and no fee. */
	g_assert_cmpuint(transport->account_creates, ==, 1);
	g_assert_cmpstr(transport->create_version, ==, "2026-09-30.endive");
	g_assert_cmpstr(transport->create_type, ==, "application/json");
	g_assert_cmpstr(transport->create_authorization, ==, "Bearer " CONNECT_SECRET);
	g_assert_null(transport->last_account);
	expected_key = g_strdup_printf("venture-connect-account-%s", venture_entity_get_uuid(organization));
	g_assert_cmpstr(transport->create_key, ==, expected_key);
	expected_body = g_strdup("{\"contact_email\":\"owner@example.test\",\"display_name\":\"Example \\\"Plumbing\\\"\","
		"\"dashboard\":\"full\",\"identity\":{\"country\":\"us\"},"
		"\"defaults\":{\"responsibilities\":{\"fees_collector\":\"stripe\",\"losses_collector\":\"stripe\"}},"
		"\"configuration\":{\"merchant\":{\"capabilities\":{\"card_payments\":{\"requested\":true}}}}}");
	g_assert_cmpstr(transport->create_body, ==, expected_body);
	g_assert_null(strstr(transport->create_body, "application_fee"));
	/* The onboarding link for that account, platform key, no header. */
	g_assert_cmpuint(transport->account_links, ==, 1);
	g_assert_null(transport->link_header);
	link = soup_form_decode(transport->link_body);
	g_assert_cmpstr(g_hash_table_lookup(link, "account"), ==, "acct_connectA");
	g_assert_cmpstr(g_hash_table_lookup(link, "type"), ==, "account_onboarding");
	g_assert_cmpstr(g_hash_table_lookup(link, "return_url"), ==, "https://app.example.test/setup/connections");
	g_assert_cmpstr(g_hash_table_lookup(link, "refresh_url"), ==, "https://app.example.test/setup/connections?again=1");
	/* The stored connection: mode connect, the account, needs_more. */
	settings = connect_settings(f, f->organization_id, &account);
	stored = json_node_get_object(settings);
	g_assert_cmpstr(account, ==, "acct_connectA");
	g_assert_cmpstr(json_object_get_string_member(stored, "mode"), ==, "connect");
	g_assert_cmpstr(json_object_get_string_member(stored, "account"), ==, "acct_connectA");
	g_assert_cmpstr(json_object_get_string_member(stored, "environment"), ==, "test");
	g_assert_cmpstr(json_object_get_string_member(stored, "state"), ==, "needs_more");
	g_assert_cmpstr(json_object_get_string_member(stored, "success_url"), ==, "https://app.example.test/pay/done");
	g_assert_false(json_object_has_member(stored, "secret_key"));
	g_clear_pointer(&settings, json_node_unref);
	/* Again: the same account, a fresh link, the new success URL kept. */
	again = connect_start(f, f->organization_id, transport, "https://app.example.test/pay/thanks", &error);
	g_assert_no_error(error);
	assert_connect_answer(again, "needs_more", "connect");
	g_assert_cmpuint(transport->account_creates, ==, 1);
	g_assert_cmpuint(transport->account_links, ==, 2);
	settings = connect_settings(f, f->organization_id, NULL);
	g_assert_cmpstr(json_object_get_string_member(json_node_get_object(settings), "success_url"), ==,
		"https://app.example.test/pay/thanks");
	g_test_message("Connect: one v2 account (%s), two onboarding links", transport->create_key);
}

/* A business on pasted keys is refused, and nothing is sent or changed. */
static void
test_connect_refusals(Fixture *f, gconstpointer data)
{
	const gchar *mode = data;
	g_autoptr(GError) error = NULL;
	g_autoptr(GObject) object = g_object_new(fake_transport_get_type(), NULL);
	FakeTransport *transport = (FakeTransport *)object;
	g_autoptr(JsonNode) answer = NULL;
	if (!g_strcmp0(mode, "off"))
	{
		/* No stripe.connect.* configuration: Connect is off, plainly. */
		g_autoptr(GBytes) key = g_bytes_new_static("01234567890123456789012345678901", 32);
		g_assert_true(venture_integration_service_set_key(venture_integration_service_get(f->database), key, &error));
		answer = connect_start(f, f->organization_id, transport, "https://app.example.test/pay/done", &error);
		g_assert_null(answer); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
		g_assert_nonnull(strstr(error->message, "Stripe Connect is not configured"));
		g_clear_error(&error);
		answer = venture_stripe_connect_status(f->database, f->organization_id, STRIPE_TRANSPORT(transport), NULL, &error);
		g_assert_null(answer); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
		g_clear_error(&error);
		g_assert_false(connect_event(f, transport, "{\"id\":\"evt_off\",\"type\":\"account.updated\",\"livemode\":false,\"account\":\"acct_connectA\",\"data\":{\"object\":{}}}", CONNECT_WEBHOOK, &error));
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
		g_assert_cmpuint(transport->calls, ==, 0);
		return;
	}
	connect_platform(f);
	if (!g_strcmp0(mode, "keys"))
	{
		g_autoptr(JsonNode) keys = binding_settings("whsec_original");
		g_autoptr(VentureIntegrationConnection) connection = venture_stripe_settings_configure(f->database,
			f->organization_id, keys, 0, 0, STRIPE_TRANSPORT(transport), NULL, &error);
		guint calls;
		gint64 version;
		g_assert_no_error(error); g_assert_nonnull(connection);
		version = venture_entity_get_version(VENTURE_ENTITY(connection));
		calls = transport->calls;
		answer = connect_start(f, f->organization_id, transport, "https://app.example.test/pay/done", &error);
		g_assert_null(answer); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
		g_clear_error(&error);
		g_assert_cmpuint(transport->calls, ==, calls);
		g_assert_cmpuint(transport->account_creates, ==, 0);
		{
			g_autoptr(VentureIntegrationConnection) still = venture_integration_service_find(
				venture_integration_service_get(f->database), f->organization_id, "stripe", &error);
			g_autoptr(JsonNode) kept = NULL;
			g_assert_no_error(error);
			g_assert_cmpint(venture_entity_get_version(VENTURE_ENTITY(still)), ==, version);
			kept = connect_settings(f, f->organization_id, NULL);
			g_assert_false(json_object_has_member(json_node_get_object(kept), "mode"));
		}
		/* Pasted keys that work answer ready, mode keys, with no read. */
		answer = venture_stripe_connect_status(f->database, f->organization_id, STRIPE_TRANSPORT(transport), NULL, &error);
		g_assert_no_error(error);
		assert_connect_answer(answer, "ready", "keys");
		g_assert_cmpuint(transport->calls, ==, calls);
		return;
	}
	if (!g_strcmp0(mode, "viewer"))
	{
		/* Only an owner or admin of the business connects it. */
		g_autoptr(VentureUser) user = venture_user_new();
		g_autoptr(VentureOrganizationMembership) membership = venture_organization_membership_new();
		g_autoptr(VentureAccessScope) scope = NULL;
		VentureAuthPrincipal principal;
		memset(&principal, 0, sizeof(principal));
		g_object_set(user, "username", "connect-viewer", "role", VENTURE_USER_ROLE_EDITOR, "active", TRUE, NULL);
		save(f, VENTURE_ENTITY(user));
		venture_entity_set_organization_id(VENTURE_ENTITY(membership), f->organization_id);
		g_object_set(membership, "user-id", venture_entity_get_id(VENTURE_ENTITY(user)),
			"role", VENTURE_ORGANIZATION_ROLE_FINANCE, "active", TRUE, NULL);
		save(f, VENTURE_ENTITY(membership));
		principal.user_id = venture_entity_get_id(VENTURE_ENTITY(user));
		principal.role = VENTURE_USER_ROLE_EDITOR; principal.name = (gchar *)"connect-viewer"; principal.authenticated = TRUE;
		scope = venture_access_policy_enter(venture_database_get_access_policy(f->database), &principal);
		answer = connect_start(f, f->organization_id, transport, "https://app.example.test/pay/done", &error);
		g_assert_null(answer); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
		g_clear_error(&error);
		answer = venture_stripe_connect_status(f->database, f->organization_id, STRIPE_TRANSPORT(transport), NULL, &error);
		g_assert_null(answer); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
		g_assert_cmpuint(transport->calls, ==, 0);
		return;
	}
	/* "http": every URL must be HTTPS before anything is sent. */
	answer = venture_stripe_connect_start(f->database, f->organization_id,
		"http://app.example.test/setup", "https://app.example.test/setup", "https://app.example.test/done",
		"https://app.example.test/cancel", STRIPE_TRANSPORT(transport), NULL, &error);
	g_assert_null(answer); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_cmpuint(transport->calls, ==, 0);
}

/* The three words from charges_enabled, stored when they change. */
static void
test_connect_status(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GObject) object = g_object_new(fake_transport_get_type(), NULL);
	FakeTransport *transport = (FakeTransport *)object;
	g_autoptr(JsonNode) answer = NULL, started = NULL, settings = NULL;
	(void)data;
	connect_platform(f);
	/* A missing business is not found. A NULL from the database used to
	 * leave the error unset, and the route answered 500. */
	answer = venture_stripe_connect_status(f->database, 9000000001, STRIPE_TRANSPORT(transport), NULL, &error);
	g_assert_null(answer);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
	g_assert_cmpstr(error->message, ==, "Organization not found");
	g_clear_error(&error);
	g_clear_pointer(&answer, json_node_unref);
	answer = venture_stripe_connect_status(f->database, f->organization_id, STRIPE_TRANSPORT(transport), NULL, &error);
	g_assert_no_error(error);
	assert_connect_answer(answer, "not_connected", NULL);
	g_assert_cmpuint(transport->calls, ==, 0);
	g_clear_pointer(&answer, json_node_unref);
	started = connect_start(f, f->organization_id, transport, "https://app.example.test/pay/done", &error);
	g_assert_no_error(error);
	transport->charges_enabled = FALSE;
	answer = venture_stripe_connect_status(f->database, f->organization_id, STRIPE_TRANSPORT(transport), NULL, &error);
	g_assert_no_error(error);
	assert_connect_answer(answer, "needs_more", "connect");
	g_assert_cmpuint(transport->account_reads, ==, 1);
	g_assert_null(transport->read_header);
	g_assert_cmpstr(transport->read_authorization, ==, "Bearer " CONNECT_SECRET);
	g_clear_pointer(&answer, json_node_unref);
	transport->charges_enabled = TRUE;
	answer = venture_stripe_connect_status(f->database, f->organization_id, STRIPE_TRANSPORT(transport), NULL, &error);
	g_assert_no_error(error);
	assert_connect_answer(answer, "ready", "connect");
	g_assert_null(json_object_get_member(json_node_get_object(answer), "url"));
	settings = connect_settings(f, f->organization_id, NULL);
	g_assert_cmpstr(json_object_get_string_member(json_node_get_object(settings), "state"), ==, "ready");
	g_clear_pointer(&answer, json_node_unref);
	transport->charges_enabled = FALSE;
	answer = venture_stripe_connect_status(f->database, f->organization_id, STRIPE_TRANSPORT(transport), NULL, &error);
	g_assert_no_error(error);
	assert_connect_answer(answer, "needs_more", "connect");
	g_test_message("Connect status: not_connected, needs_more, ready, needs_more again");
}

/* A connect connection's client is the platform key plus Stripe-Account:
 * the Checkout Session for /pay/<token> is a direct charge. Two businesses
 * on two accounts, and one webhook that routes by the event's account. */
static void
test_connect_charges_and_webhooks(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GObject) object = g_object_new(fake_transport_get_type(), NULL);
	FakeTransport *transport = (FakeTransport *)object;
	g_autoptr(JsonNode) first = NULL, second = NULL, settings = NULL;
	g_autoptr(VentureStripeService) service_a = NULL, service_b = NULL;
	g_autoptr(VentureEntity) invoice_a = NULL, invoice_b = NULL;
	g_autoptr(VentureStripeCheckout) checkout_a = NULL, checkout_b = NULL;
	g_autoptr(VentureMoney) balance_a = NULL, balance_b = NULL;
	g_autoptr(GPtrArray) events = NULL;
	gint64 business_a = f->organization_id, business_b, customer_b = 0, product_b = 0;
	gint64 original_customer = f->customer_id, original_product = f->product_id;
	const gchar *paid = "{\"id\":\"evt_connect_paid\",\"object\":\"event\",\"type\":\"checkout.session.completed\",\"livemode\":false,\"account\":\"acct_connectB\",\"data\":{\"object\":{\"id\":\"cs_offline\",\"amount_total\":10000,\"currency\":\"usd\",\"payment_status\":\"paid\",\"payment_intent\":\"pi_connect\"}}}";
	const gchar *mode = data;
	connect_platform(f);
	first = connect_start(f, business_a, transport, "https://app.example.test/pay/done", &error);
	g_assert_no_error(error);
	business_b = connect_second_business(f, "Second business", "connect-second", &customer_b, &product_b);
	transport->connect_id = "acct_connectB";
	second = connect_start(f, business_b, transport, "https://app.example.test/pay/done", &error);
	g_assert_no_error(error);
	g_assert_cmpuint(transport->account_creates, ==, 2);
	/* Business B charges first, so its session is cs_offline. */
	f->organization_id = business_b;
	f->customer_id = customer_b;
	f->product_id = product_b;
	service_b = venture_stripe_service_for_organization(f->database, business_b, STRIPE_TRANSPORT(transport), &error);
	g_assert_no_error(error); g_assert_nonnull(service_b);
	invoice_b = invoice_new(f, "connect-invoice", "2026-01-01", "100 USD");
	checkout_b = venture_stripe_service_checkout(service_b, venture_entity_get_id(invoice_b), NULL, &error);
	g_assert_no_error(error); g_assert_nonnull(checkout_b);
	g_assert_cmpstr(transport->checkout_account, ==, "acct_connectB");
	g_assert_cmpstr(transport->checkout_authorization, ==, "Bearer " CONNECT_SECRET);
	g_assert_null(transport->checkout_fee);
	f->organization_id = business_a;
	f->customer_id = original_customer;
	f->product_id = original_product;
	service_a = venture_stripe_service_for_organization(f->database, business_a, STRIPE_TRANSPORT(transport), &error);
	g_assert_no_error(error);
	invoice_a = invoice_new(f, "connect-invoice", "2026-01-01", "100 USD");
	checkout_a = venture_stripe_service_checkout(service_a, venture_entity_get_id(invoice_a), NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(transport->checkout_account, ==, "acct_connectA");
	if (!g_strcmp0(mode, "http"))
	{
		/* The same routing through the public route and its refusals. */
		g_autoptr(VentureWebServer) server = NULL;
		g_autofree gchar *state = NULL, *body = NULL;
		g_autoptr(VentureStripeService) installed = venture_stripe_service_new(f->database, f->organization_id, STRIPE_TRANSPORT(transport), &error);
		g_autofree gchar *path = g_strdup_printf("/api/v1/organizations/%" G_GINT64_FORMAT "/stripe/connect", business_a);
		g_autofree gchar *good = binding_signature(paid, CONNECT_WEBHOOK);
		g_autofree gchar *bad = binding_signature(paid, "whsec_original");
		g_assert_no_error(error);
		venture_context_set_stripe_service(f->context, installed);
		server = start_server(f, &state);
		g_assert_cmpuint(connect_http(server, "GET", path, NULL, NULL, &body), ==, 200);
		{
			g_autoptr(JsonNode) read = json_from_string(body, &error);
			g_assert_no_error(error);
			assert_connect_answer(read, "needs_more", "connect");
		}
		g_clear_pointer(&body, g_free);
		g_assert_cmpuint(connect_http(server, "POST", path, NULL,
			"{\"return_url\":\"https://app.example.test/r\",\"refresh_url\":\"https://app.example.test/f\","
			"\"success_url\":\"https://app.example.test/s\",\"cancel_url\":\"https://app.example.test/c\"}", &body), ==, 200);
		{
			g_autoptr(JsonNode) made = json_from_string(body, &error);
			g_assert_no_error(error);
			assert_connect_answer(made, "needs_more", "connect");
			g_assert_true(g_str_has_prefix(json_object_get_string_member(json_node_get_object(made), "url"), "https://connect.stripe.com/"));
		}
		g_clear_pointer(&body, g_free);
		g_assert_cmpuint(connect_http(server, "POST", path, NULL, "{\"return_url\":\"https://app.example.test/r\"}", &body), ==, 422);
		g_clear_pointer(&body, g_free);
		g_assert_cmpuint(connect_http(server, "POST", "/webhooks/stripe/connect", bad, paid, NULL), ==, 400);
		g_assert_cmpuint(connect_http(server, "POST", "/webhooks/stripe/connect", good, paid, NULL), ==, 200);
		events = rows(f, "stripe_event");
		g_assert_cmpuint(events->len, ==, 1);
		g_object_set(f->config, "stripe-connect-webhook-secret", "", NULL);
		g_assert_cmpuint(connect_http(server, "POST", "/webhooks/stripe/connect", good, paid, NULL), ==, 503);
		g_assert_cmpuint(connect_http(server, "GET", path, NULL, NULL, NULL), ==, 404);
		venture_web_server_stop(server);
		g_clear_object(&server);
		venture_test_remove_tree(state);
	}
	else
	{
		/* A wrong secret and an unknown account record nothing. */
		g_assert_false(connect_event(f, transport, paid, "whsec_original", &error));
		g_assert_nonnull(error); g_clear_error(&error);
		g_assert_true(connect_event(f, transport, "{\"id\":\"evt_stranger\",\"object\":\"event\",\"type\":\"checkout.session.completed\",\"livemode\":false,\"account\":\"acct_unknown\",\"data\":{\"object\":{\"id\":\"cs_offline\"}}}", CONNECT_WEBHOOK, &error));
		g_assert_no_error(error);
		events = rows(f, "stripe_event");
		g_assert_cmpuint(events->len, ==, 0);
		g_clear_pointer(&events, g_ptr_array_unref);
		g_assert_true(connect_event(f, transport, paid, CONNECT_WEBHOOK, &error));
		g_assert_no_error(error);
		events = rows(f, "stripe_event");
		g_assert_cmpuint(events->len, ==, 1);
	}
	g_assert_cmpint(venture_entity_get_organization_id(g_ptr_array_index(events, 0)), ==, business_b);
	balance_b = venture_settlement_service_invoice_balance(venture_settlement_service_get(f->database), venture_entity_get_id(invoice_b), NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpint(venture_money_get_amount(balance_b), ==, 0);
	balance_a = venture_settlement_service_invoice_balance(venture_settlement_service_get(f->database), venture_entity_get_id(invoice_a), NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpint(venture_money_get_amount(balance_a), ==, 10000);
	if (!g_strcmp0(mode, "http")) return;
	/* account.updated moves business A to ready, and only A. */
	g_assert_true(connect_event(f, transport, "{\"id\":\"evt_ready\",\"object\":\"event\",\"type\":\"account.updated\",\"livemode\":false,\"account\":\"acct_connectA\",\"data\":{\"object\":{\"id\":\"acct_connectA\",\"object\":\"account\",\"charges_enabled\":true}}}", CONNECT_WEBHOOK, &error));
	g_assert_no_error(error);
	settings = connect_settings(f, business_a, NULL);
	g_assert_cmpstr(json_object_get_string_member(json_node_get_object(settings), "state"), ==, "ready");
	g_clear_pointer(&settings, json_node_unref);
	settings = connect_settings(f, business_b, NULL);
	g_assert_cmpstr(json_object_get_string_member(json_node_get_object(settings), "state"), ==, "needs_more");
	g_test_message("Connect webhooks: paid for B only; unknown account and bad signature recorded nothing; A ready");
}
