/* SPDX-License-Identifier: AGPL-3.0-or-later */
static JsonNode *
binding_settings(const gchar *webhook)
{
	JsonNode *node = json_node_new(JSON_NODE_OBJECT);
	JsonObject *object = json_object_new();
	json_node_take_object(node, object);
	json_object_set_string_member(object, "secret_key", "sk_test_fixture_only");
	json_object_set_string_member(object, "publishable_key", "pk_test_fixture_only");
	json_object_set_string_member(object, "webhook_secret", webhook);
	json_object_set_string_member(object, "api_version", "2024-06-20");
	json_object_set_string_member(object, "success_url", "https://example.test/paid");
	json_object_set_string_member(object, "cancel_url", "https://example.test/cancel");
	json_object_set_string_member(object, "environment", "test");
	return node;
}

static gchar *
binding_signature(const gchar *body, const gchar *secret)
{
	gint64 timestamp = g_get_real_time() / G_USEC_PER_SEC;
	g_autofree gchar *signed_text = g_strdup_printf("%" G_GINT64_FORMAT ".%s", timestamp, body);
	g_autofree gchar *mac = g_compute_hmac_for_string(G_CHECKSUM_SHA256,
		(const guchar *)secret, strlen(secret), signed_text, -1);
	return g_strdup_printf("t=%" G_GINT64_FORMAT ",v1=%s", timestamp, mac);
}

static gboolean
binding_event(VentureStripeService *service, const gchar *id, const gchar *secret,
	gboolean live, const gchar *account, GError **error)
{
	g_autofree gchar *body = g_strdup_printf("{\"id\":\"%s\",\"object\":\"event\",\"type\":\"unhandled.fixture\",\"livemode\":%s,\"account\":\"%s\",\"data\":{\"object\":{}}}", id, live ? "true" : "false", account);
	g_autofree gchar *sig = binding_signature(body, secret);
	g_autoptr(GBytes) bytes = g_bytes_new(body, strlen(body));
	return venture_stripe_service_handle_webhook(service, bytes, sig, error);
}

/* Credentials must remain bound to the verified account across cached
 * services, rotations, replacement, duplicate remote IDs and callbacks. */
static void
test_binding(Fixture *f, gconstpointer data)
{
	const gchar *mode = data;
	g_autoptr(GError) error = NULL;
	g_autoptr(GObject) transport = g_object_new(fake_transport_get_type(), NULL);
	g_autoptr(JsonNode) settings = binding_settings("whsec_original");
	g_autoptr(GBytes) encryption_key = g_bytes_new_static("01234567890123456789012345678901", 32);
	g_autoptr(VentureIntegrationConnection) connection = NULL, next = NULL;
	g_autoptr(VentureStripeService) service = NULL, replacement = NULL;
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(VentureStripeCheckout) checkout = NULL;
	gint64 id;
	g_assert_true(venture_integration_service_set_key(venture_integration_service_get(f->database), encryption_key, &error));
	connection = venture_stripe_settings_configure(f->database, f->organization_id, settings, 0, 0, STRIPE_TRANSPORT(transport), NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(connection);
	id = venture_entity_get_id(VENTURE_ENTITY(connection));
	service = venture_stripe_service_for_organization(f->database, f->organization_id, STRIPE_TRANSPORT(transport), &error);
	g_assert_no_error(error);
	g_assert_true(venture_stripe_settings_test(f->database, f->organization_id, STRIPE_TRANSPORT(transport), &error));
	g_assert_no_error(error);
	if (!strcmp(mode, "settings-ui"))
	{
		g_autoptr(VentureWebServer) server = NULL;
		g_autofree gchar *state = NULL, *path = NULL, *body = NULL, *form = NULL;
		server = start_server(f, &state);
		path = g_strdup_printf("/organizations/%" G_GINT64_FORMAT "/settings/stripe", f->organization_id);
		{
			g_autofree gchar *detail = g_strdup_printf("/e/organization/%" G_GINT64_FORMAT, f->organization_id);
			g_assert_cmpuint(http_request(server, "GET", detail, NULL, NULL, &body), ==, 200);
			g_assert_nonnull(strstr(body, path));
			g_clear_pointer(&body, g_free);
		}
		g_assert_cmpuint(http_request(server, "GET", path, NULL, NULL, &body), ==, 200);
		g_assert_nonnull(strstr(body, "acct_offline"));
		g_assert_null(strstr(body, "whsec_original"));
		g_assert_null(strstr(body, "sk_test_fixture_only"));
		g_clear_pointer(&body, g_free);
		g_assert_cmpuint(http_request(server, "POST", path, "application/x-www-form-urlencoded", "operation=configure&secret_key=do-not-echo", &body), ==, 400);
		g_assert_null(strstr(body, "do-not-echo"));
		form = g_strdup_printf("operation=disconnect&version=%" G_GINT64_FORMAT "&connection_id=%" G_GINT64_FORMAT, venture_entity_get_version(VENTURE_ENTITY(connection)), id);
		g_clear_pointer(&body, g_free);
		g_assert_cmpuint(http_request(server, "POST", path, "application/x-www-form-urlencoded", form, &body), ==, 200);
		g_assert_nonnull(strstr(body, "No Stripe account"));
		{
			g_autofree gchar *callback = g_strdup_printf("/webhooks/stripe/%" G_GINT64_FORMAT, id);
			const gchar *event = "{\"id\":\"evt_http\",\"type\":\"fixture.ignored\",\"livemode\":false,\"data\":{\"object\":{}}}";
			g_assert_cmpuint(http_request(server, "POST", callback, "application/json", event, NULL), ==, 200);
			g_assert_cmpuint(http_request(server, "POST", callback, "application/json", "{}", NULL), ==, 400);
			g_assert_cmpuint(http_request(server, "POST", "/webhooks/stripe", "application/json", event, NULL), ==, 503);
			venture_config_set_module_enabled(f->config, "stripe", FALSE);
			g_assert_cmpuint(http_request(server, "POST", callback, "application/json", event, NULL), ==, 503);
			venture_config_set_module_enabled(f->config, "stripe", TRUE);
		}
		next = venture_stripe_settings_configure(f->database, f->organization_id, settings, 0, 0,
			STRIPE_TRANSPORT(transport), NULL, &error);
		g_assert_no_error(error);
		g_assert_nonnull(next);
		g_clear_pointer(&body, g_free);
		g_assert_cmpuint(http_request(server, "POST", path, "application/x-www-form-urlencoded", form, &body), ==, 400);
		{
			g_autoptr(VentureIntegrationConnection) stale = venture_stripe_settings_configure(f->database,
				f->organization_id, settings, venture_entity_get_version(VENTURE_ENTITY(connection)), id,
				STRIPE_TRANSPORT(transport), NULL, &error);
			g_assert_null(stale);
			g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
			g_clear_error(&error);
			g_assert_true(venture_stripe_settings_test(f->database, f->organization_id, STRIPE_TRANSPORT(transport), &error));
			g_assert_no_error(error);
		}
		venture_web_server_stop(server);
		g_clear_object(&server);
		venture_test_remove_tree(state);
		return;
	}
	if (!strcmp(mode, "rotation"))
	{
		g_assert_true(binding_event(service, "evt_before", "whsec_original", FALSE, "acct_offline", &error));
		json_object_set_string_member(json_node_get_object(settings), "webhook_secret", "whsec_rotated");
		next = venture_stripe_settings_configure(f->database, f->organization_id, settings,
			venture_entity_get_version(VENTURE_ENTITY(connection)), id, STRIPE_TRANSPORT(transport), NULL, &error);
		g_assert_no_error(error);
		g_assert_cmpint(venture_entity_get_id(VENTURE_ENTITY(next)), ==, id);
		g_assert_true(binding_event(service, "evt_rotated", "whsec_rotated", FALSE, "acct_offline", &error));
		g_assert_no_error(error);
		g_assert_false(binding_event(service, "evt_old_secret", "whsec_original", FALSE, "acct_offline", &error));
		g_assert_nonnull(error);
		g_clear_error(&error);
		g_assert_false(binding_event(service, "evt_wrong_environment", "whsec_rotated", TRUE, "acct_offline", &error));
		g_assert_nonnull(error);
		g_clear_error(&error);
		g_assert_false(binding_event(service, "evt_wrong_account", "whsec_rotated", FALSE, "acct_other", &error));
		g_assert_nonnull(error);
		g_clear_error(&error);
		{
			const gchar *body = "{\"id\":\"evt_malformed_account\",\"type\":\"fixture.ignored\",\"livemode\":false,\"account\":17,\"data\":{\"object\":{}}}";
			g_autofree gchar *sig = binding_signature(body, "whsec_rotated");
			g_autoptr(GBytes) bytes = g_bytes_new(body, strlen(body));
			g_assert_false(venture_stripe_service_handle_webhook(service, bytes, sig, &error));
			g_assert_nonnull(error);
		}
		return;
	}
	if (!strcmp(mode, "authorization"))
	{
		VentureAuthPrincipal principal;
		g_autoptr(VentureAccessScope) scope = NULL;
		guint calls = ((FakeTransport *)transport)->calls;
		principal.user_id = 0; principal.token_id = 0;
		principal.role = VENTURE_USER_ROLE_EDITOR;
		principal.name = (gchar *)"editor"; principal.authenticated = TRUE;
		scope = venture_access_policy_enter(venture_database_get_access_policy(f->database), &principal);
		next = venture_stripe_settings_configure(f->database, f->organization_id, settings,
			venture_entity_get_version(VENTURE_ENTITY(connection)), id, STRIPE_TRANSPORT(transport), NULL, &error);
		g_assert_null(next);
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
		g_assert_cmpuint(((FakeTransport *)transport)->calls, ==, calls);
		return;
	}
	invoice = invoice_new(f, "bound-invoice", "2026-01-01", "100 USD");
	checkout = venture_stripe_service_checkout(service, venture_entity_get_id(invoice), NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(checkout);
	{
		gint64 recorded;
		g_object_get(checkout, "connection-id", &recorded, NULL);
		g_assert_cmpint(recorded, ==, id);
	}
	((FakeTransport *)transport)->account_id = "acct_replacement";
	next = venture_stripe_settings_configure(f->database, f->organization_id, settings,
		venture_entity_get_version(VENTURE_ENTITY(connection)), id, STRIPE_TRANSPORT(transport), NULL, &error);
	g_assert_null(next);
	g_assert_nonnull(error);
	g_clear_error(&error);
	g_assert_true(venture_integration_service_disable(venture_integration_service_get(f->database),
		f->organization_id, id, venture_entity_get_version(VENTURE_ENTITY(connection)), NULL, &error));
	g_assert_no_error(error);
	next = venture_stripe_settings_configure(f->database, f->organization_id, settings, 0, 0, STRIPE_TRANSPORT(transport), NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(next);
	replacement = venture_stripe_service_for_organization(f->database, f->organization_id, STRIPE_TRANSPORT(transport), &error);
	g_assert_no_error(error);
	g_clear_object(&checkout);
	checkout = venture_stripe_service_checkout(replacement, venture_entity_get_id(invoice), NULL, &error);
	g_assert_null(checkout);
	g_assert_nonnull(strstr(error->message, "pending Checkout"));
	g_clear_error(&error);
	g_assert_false(venture_stripe_service_can_checkout(service, venture_entity_get_id(invoice), &error));
	g_assert_nonnull(error);
	g_clear_error(&error);
	/* A disconnected account still validates its own historical callbacks.
	 * Equal event IDs in the replacement account are independent evidence. */
	g_assert_true(binding_event(service, "evt_shared", "whsec_original", FALSE, "acct_offline", &error));
	g_assert_no_error(error);
	g_assert_true(binding_event(replacement, "evt_shared", "whsec_original", FALSE, "acct_replacement", &error));
	g_assert_no_error(error);
	{
		g_autoptr(GPtrArray) events = rows(f, "stripe_event");
		g_assert_cmpuint(events->len, ==, 2);
	}
	{
		const gchar *body = "{\"id\":\"evt_historical_payment\",\"livemode\":false,\"type\":\"checkout.session.completed\",\"data\":{\"object\":{\"id\":\"cs_offline\",\"amount_total\":10000,\"currency\":\"usd\",\"payment_status\":\"paid\",\"payment_intent\":\"pi_historical\"}}}";
		g_autofree gchar *sig = binding_signature(body, "whsec_original");
		g_autoptr(GBytes) bytes = g_bytes_new(body, strlen(body));
		g_autoptr(GPtrArray) payments = NULL;
		g_autoptr(VentureProcessorDispute) dispute = NULL;
		g_autoptr(VentureProcessorPayout) payout = NULL;
		g_autoptr(VentureMoney) amount = venture_money_new(10000, "USD", 2);
		g_autoptr(GDateTime) now = venture_time_now();
		g_assert_true(venture_stripe_service_handle_webhook(service, bytes, sig, &error));
		g_assert_no_error(error);
		payments = rows(f, "payment");
		g_assert_cmpuint(payments->len, ==, 1);
		payout = venture_stripe_service_record_payout(replacement, "po_new_account", now,
			amount, NULL, amount, 0, NULL, &error);
		g_assert_no_error(error);
		g_assert_nonnull(payout);
		g_assert_false(venture_stripe_service_link_payout_item(replacement,
			venture_entity_get_id(VENTURE_ENTITY(payout)), venture_entity_get_id(g_ptr_array_index(payments, 0)), amount, NULL, &error));
		g_assert_nonnull(error);
		g_clear_error(&error);
		dispute = venture_stripe_service_open_dispute(replacement, "dp_wrong_account",
			venture_entity_get_id(g_ptr_array_index(payments, 0)), now, amount, NULL, &error);
		g_assert_null(dispute);
		g_assert_nonnull(error);
	}
}

/* A legacy UNIQUE(event_id) must not survive the metadata upgrade, and
 * widening its scope must retain plugin columns, indexes, triggers and IDs. */
static void
test_binding_migration(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(OrmResult) result = NULL;
	OrmConnection *connection = venture_database_get_connection(f->database);
	OrmDialectType dialect = orm_engine_get_dialect_type(orm_connection_get_engine(connection));
	const gchar *ddl;
	gboolean migrated;
	(void)data;
	if (dialect == ORM_DIALECT_SQLITE)
		ddl = "DROP TABLE stripe_events; CREATE TABLE stripe_events (id INTEGER PRIMARY KEY AUTOINCREMENT, organization_id BIGINT, uuid TEXT, created_at TEXT, updated_at TEXT, deleted_at TEXT, version BIGINT, connection_id BIGINT, \"event_id\" TEXT UNIQUE, plugin_note TEXT); CREATE INDEX plugin_event_note ON stripe_events(plugin_note); CREATE TRIGGER plugin_event_insert AFTER INSERT ON stripe_events BEGIN UPDATE stripe_events SET plugin_note = 'trigger-kept' WHERE id = NEW.id; END; INSERT INTO stripe_events(id, organization_id, event_id) VALUES (100, 1, 'deleted'); DELETE FROM stripe_events WHERE id = 100";
	else
		ddl = "DROP TABLE stripe_events; CREATE TABLE stripe_events (id BIGSERIAL PRIMARY KEY, organization_id BIGINT, uuid TEXT, created_at TEXT, updated_at TEXT, deleted_at TEXT, version BIGINT, connection_id BIGINT, \"event_id\" TEXT UNIQUE, plugin_note TEXT); CREATE INDEX plugin_event_note ON stripe_events(plugin_note)";
	g_assert_true(orm_connection_execute(connection, ddl, &error));
	g_assert_no_error(error);
	g_assert_true(orm_connection_execute(connection, "CREATE UNIQUE INDEX uq_stripe_events_organization_event_id ON stripe_events(organization_id, event_id)", &error));
	migrated = venture_period_constraints_migrate(connection, VENTURE_TYPE_STRIPE_EVENT, &error);
	g_assert_no_error(error);
	g_assert_true(migrated);
	g_assert_true(orm_connection_execute(connection, "INSERT INTO stripe_events(organization_id, connection_id, event_id, plugin_note) VALUES (1, 1, 'evt_shared', 'kept'), (1, 2, 'evt_shared', 'kept'), (2, 1, 'evt_shared', 'kept')", &error));
	g_assert_no_error(error);
	g_assert_false(orm_connection_execute(connection, "INSERT INTO stripe_events(organization_id, connection_id, event_id) VALUES (1, 1, 'evt_shared')", &error));
	g_assert_nonnull(error);
	g_clear_error(&error);
	if (dialect == ORM_DIALECT_SQLITE)
	{
		result = orm_connection_query(connection, "SELECT id, plugin_note FROM stripe_events ORDER BY id", &error);
		g_assert_no_error(error);
		g_assert_true(orm_result_next(result));
		g_assert_cmpint(orm_row_get_integer(orm_result_get_row(result), 0), ==, 101);
		g_assert_cmpstr(orm_row_get_string(orm_result_get_row(result), 1), ==, "trigger-kept");
		g_clear_object(&result);
	}
	result = orm_connection_query(connection, dialect == ORM_DIALECT_SQLITE
		? "SELECT COUNT(*) FROM sqlite_master WHERE type='index' AND name='plugin_event_note'"
		: "SELECT CAST(COUNT(*) AS BIGINT) FROM pg_indexes WHERE schemaname=current_schema() AND indexname='plugin_event_note'", &error);
	g_assert_no_error(error);
	g_assert_true(orm_result_next(result));
	g_assert_cmpint(orm_row_get_integer(orm_result_get_row(result), 0), ==, 1);
}

/* Two businesses may receive colliding provider IDs. Neither invoice nor
 * receipt identity may accidentally resolve through the default business. */
static void
test_binding_organizations(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) key = g_bytes_new_static("01234567890123456789012345678901", 32);
	g_autoptr(JsonNode) settings = binding_settings("whsec_original");
	gint64 original_org = f->organization_id;
	gint64 first_connection = 0;
	guint i;
	(void)data;
	g_assert_true(venture_integration_service_set_key(venture_integration_service_get(f->database), key, &error));
	for (i = 0; i < 2; i++)
	{
		g_autoptr(GObject) transport = g_object_new(fake_transport_get_type(), NULL);
		g_autoptr(VentureIntegrationConnection) connection = NULL;
		g_autoptr(VentureStripeService) service = NULL;
		g_autoptr(VentureEntity) invoice = NULL;
		g_autoptr(VentureStripeCheckout) checkout = NULL;
		const gchar *body = "{\"id\":\"evt_collision\",\"livemode\":false,\"type\":\"checkout.session.completed\",\"data\":{\"object\":{\"id\":\"cs_offline\",\"amount_total\":10000,\"currency\":\"usd\",\"payment_status\":\"paid\",\"payment_intent\":\"pi_collision\"}}}";
		g_autofree gchar *sig = binding_signature(body, "whsec_original");
		g_autoptr(GBytes) bytes = g_bytes_new(body, strlen(body));
		g_autoptr(VentureMoney) balance = NULL;
		if (i)
		{
			g_autoptr(VentureOrganization) organization = venture_organization_new();
			g_autoptr(VentureCompany) company = venture_company_new();
			g_autoptr(VentureProduct) product = venture_product_new();
			g_object_set(organization, "name", "Another business", "slug", "another-stripe", NULL);
			save(f, VENTURE_ENTITY(organization));
			f->organization_id = venture_entity_get_id(VENTURE_ENTITY(organization));
			{
				static const gchar *const codes[] = { "1000", "1100", "4000", "2100" };
				guint j;
				for (j = 0; j < G_N_ELEMENTS(codes); j++)
				{
					g_autoptr(VentureAccount) account = venture_account_new();
					g_object_set(account, "name", "Second account", "code", codes[j], "active", TRUE,
						"kind", j < 2 ? VENTURE_ACCOUNT_KIND_ASSET : (j == 2 ? VENTURE_ACCOUNT_KIND_INCOME : VENTURE_ACCOUNT_KIND_LIABILITY), NULL);
					venture_entity_set_organization_id(VENTURE_ENTITY(account), f->organization_id);
					save(f, VENTURE_ENTITY(account));
				}
			}
			g_object_set(company, "name", "Another customer", NULL);
			venture_entity_set_organization_id(VENTURE_ENTITY(company), f->organization_id);
			save(f, VENTURE_ENTITY(company));
			f->customer_id = venture_entity_get_id(VENTURE_ENTITY(company));
			g_object_set(product, "name", "Another product", NULL);
			venture_entity_set_organization_id(VENTURE_ENTITY(product), f->organization_id);
			save(f, VENTURE_ENTITY(product));
			f->product_id = venture_entity_get_id(VENTURE_ENTITY(product));
		}
		connection = venture_stripe_settings_configure(f->database, f->organization_id, settings, 0, 0, STRIPE_TRANSPORT(transport), NULL, &error);
		g_assert_no_error(error);
		g_assert_nonnull(connection);
		if (!i) first_connection = venture_entity_get_id(VENTURE_ENTITY(connection));
		else
		{
			g_autoptr(VentureStripeService) wrong = venture_stripe_service_for_connection(f->database,
				f->organization_id, first_connection, TRUE, STRIPE_TRANSPORT(transport), &error);
			g_autoptr(VentureStripePriceLink) price = venture_stripe_price_link_new();
			g_assert_null(wrong);
			g_assert_nonnull(error);
			g_clear_error(&error);
			venture_entity_set_organization_id(VENTURE_ENTITY(price), f->organization_id);
			g_object_set(price, "connection-id", first_connection, "product-id", f->product_id, "stripe-price-id", "price_forged", NULL);
			g_assert_false(venture_database_save(f->database, VENTURE_ENTITY(price), NULL, &error));
			g_assert_nonnull(error);
			g_clear_error(&error);
		}
		service = venture_stripe_service_for_organization(f->database, f->organization_id, STRIPE_TRANSPORT(transport), &error);
		g_assert_no_error(error);
		invoice = invoice_new(f, "identical-invoice-number", "2026-01-01", "100 USD");
		checkout = venture_stripe_service_checkout(service, venture_entity_get_id(invoice), NULL, &error);
		g_assert_no_error(error);
		g_assert_nonnull(checkout);
		g_assert_true(venture_stripe_service_handle_webhook(service, bytes, sig, &error));
		g_assert_no_error(error);
		balance = venture_settlement_service_invoice_balance(venture_settlement_service_get(f->database), venture_entity_get_id(invoice), NULL, &error);
		g_assert_no_error(error);
		g_assert_cmpint(venture_money_get_amount(balance), ==, 0);
	}
	f->organization_id = original_org;
}
