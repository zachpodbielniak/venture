/* SPDX-License-Identifier: AGPL-3.0-or-later */
static gboolean
ach_delivery(VentureStripeService *service, const gchar *event_id,
	const gchar *type, const gchar *status, GError **error)
{
	g_autofree gchar *body = g_strdup_printf("{\"id\":\"%s\",\"type\":\"checkout.session.%s\",\"data\":{\"object\":{\"id\":\"cs_offline\",\"amount_total\":10000,\"currency\":\"usd\",\"payment_status\":\"%s\",\"payment_intent\":\"pi_ach\"}}}", event_id, type, status);
	g_autofree gchar *sig = signature(body);
	g_autoptr(GBytes) bytes = g_bytes_new(body, strlen(body));
	return venture_stripe_service_handle_webhook(service, bytes, sig, error);
}

/* Hosted authorization is not cash: a completed unpaid session remains
 * outstanding until the later signed bank settlement, even after restart. */
static void
test_ach_delayed(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GObject) transport = g_object_new(fake_transport_get_type(), NULL);
	g_autoptr(VentureStripeService) service = venture_stripe_service_new(f->database, f->organization_id, STRIPE_TRANSPORT(transport), &error);
	g_autoptr(VentureEntity) invoice = invoice_new(f, "ACH-1", "2026-01-01", "100 USD");
	g_autoptr(VentureStripeCheckout) checkout = NULL;
	g_autoptr(GPtrArray) payments = NULL;
	g_autofree gchar *state = NULL;
	gint64 checkout_id;
	(void)data;
	g_assert_no_error(error);
	checkout = venture_stripe_service_checkout(service, venture_entity_get_id(invoice), NULL, &error);
	g_assert_no_error(error);
	checkout_id = venture_entity_get_id(VENTURE_ENTITY(checkout));
	g_assert_true(ach_delivery(service, "evt_ach_pending", "completed", "unpaid", &error));
	g_assert_no_error(error);
	g_clear_object(&checkout);
	checkout = (VentureStripeCheckout *)venture_database_get(f->database, VENTURE_TYPE_STRIPE_CHECKOUT, checkout_id, &error);
	g_assert_no_error(error);
	g_object_get(checkout, "status", &state, NULL);
	g_assert_cmpstr(state, ==, "processing");
	payments = rows(f, "payment");
	g_assert_cmpuint(payments->len, ==, 0);
	g_clear_object(&service);
	service = venture_stripe_service_new(f->database, f->organization_id, STRIPE_TRANSPORT(transport), &error);
	g_assert_no_error(error);
	g_assert_true(ach_delivery(service, "evt_ach_success", "async_payment_succeeded", "paid", &error));
	g_assert_no_error(error);
	g_assert_true(ach_delivery(service, "evt_ach_success", "async_payment_succeeded", "paid", &error));
	g_assert_true(ach_delivery(service, "evt_ach_late", "completed", "unpaid", &error));
	g_assert_no_error(error);
	g_clear_pointer(&payments, g_ptr_array_unref);
	payments = rows(f, "payment");
	g_assert_cmpuint(payments->len, ==, 1);
	/* A fresh event/session cannot spend the same provider payment identity
	 * against another invoice, even though its expected amount also matches. */
	{
		g_autoptr(VentureEntity) other = invoice_new(f, "ACH-IDENTITY", "2026-01-01", "100 USD");
		g_autoptr(VentureStripeCheckout) second = venture_stripe_service_checkout(service, venture_entity_get_id(other), NULL, &error);
		const gchar *body = "{\"id\":\"evt_duplicate_payment_identity\",\"type\":\"checkout.session.completed\",\"data\":{\"object\":{\"id\":\"cs_second\",\"amount_total\":10000,\"currency\":\"usd\",\"payment_status\":\"paid\",\"payment_intent\":\"pi_ach\"}}}";
		g_autofree gchar *sig = signature(body);
		g_autoptr(GBytes) raw = g_bytes_new(body, strlen(body));
		g_assert_no_error(error); g_assert_nonnull(second);
		g_assert_false(venture_stripe_service_handle_webhook(service, raw, sig, &error));
		g_assert_nonnull(error); g_clear_error(&error);
		g_clear_pointer(&payments, g_ptr_array_unref);
		payments = rows(f, "payment"); g_assert_cmpuint(payments->len, ==, 1);
	}
}

/* A lost response can mean money collection already exists remotely. The
 * durable reservation and exact idempotency key must survive service restart. */
static void
test_ach_reservation(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GObject) transport = g_object_new(fake_transport_get_type(), NULL);
	g_autoptr(VentureStripeService) service = venture_stripe_service_new(f->database, f->organization_id, STRIPE_TRANSPORT(transport), &error);
	g_autoptr(VentureEntity) invoice = invoice_new(f, "ACH-RESERVE", "2026-01-01", "100 USD");
	g_autoptr(VentureStripeCheckout) checkout = NULL;
	g_autoptr(GPtrArray) attempts = NULL;
	g_autofree gchar *key = NULL;
	g_autofree gchar *deadline = NULL;
	FakeTransport *fake = (FakeTransport *)transport;
	(void)data;
	g_assert_no_error(error);
	fake->checkout_drop = TRUE;
	checkout = venture_stripe_service_checkout(service, venture_entity_get_id(invoice), NULL, &error);
	g_assert_null(checkout);
	g_assert_error(error, STRIPE_ERROR, STRIPE_ERROR_TRANSPORT);
	g_clear_error(&error);
	attempts = rows(f, "stripe_checkout");
	g_assert_cmpuint(attempts->len, ==, 1);
	/* A persisted provider deadline must never move when HTTP is retried. */
	g_assert_nonnull(g_object_class_find_property(G_OBJECT_GET_CLASS(g_ptr_array_index(attempts, 0)), "expires-at"));
	{
		g_autoptr(GPtrArray) exceptions = rows(f, "processor_exception");
		g_assert_cmpuint(exceptions->len, ==, 1);
	}
	key = g_strdup(fake->key);
	deadline = g_strdup(fake->deadline);
	g_assert_nonnull(deadline);
	g_clear_object(&service);
	service = venture_stripe_service_new(f->database, f->organization_id, STRIPE_TRANSPORT(transport), &error);
	g_assert_no_error(error);
	fake->checkout_drop = FALSE;
	fake->reservation_database = f->database;
	checkout = venture_stripe_service_checkout(service, venture_entity_get_id(invoice), NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(checkout);
	g_assert_cmpstr(fake->key, ==, key);
	g_assert_cmpstr(fake->deadline, ==, deadline);
	g_assert_cmpuint(fake->customers, ==, 1);
	{
		g_autoptr(GPtrArray) exceptions = rows(f, "processor_exception");
		gboolean resolved;
		g_assert_cmpuint(exceptions->len, ==, 1);
		g_object_get(g_ptr_array_index(exceptions, 0), "resolved", &resolved, NULL);
		g_assert_true(resolved);
	}
}

/* A failed local posting keeps authenticated evidence and an operator-visible
 * exception. Retrying the same provider event after repair posts once. */
static void
test_ach_posting_retry(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GObject) transport = g_object_new(fake_transport_get_type(), NULL);
	g_autoptr(VentureStripeService) service = venture_stripe_service_new(f->database, f->organization_id, STRIPE_TRANSPORT(transport), &error);
	g_autoptr(VentureEntity) invoice = invoice_new(f, "ACH-RETRY", "2026-01-01", "100 USD");
	g_autoptr(VentureStripeCheckout) checkout = NULL;
	g_autoptr(GPtrArray) evidence = NULL;
	gulong handler;
	(void)data;
	g_assert_no_error(error);
	checkout = venture_stripe_service_checkout(service, venture_entity_get_id(invoice), NULL, &error);
	g_assert_no_error(error);
	handler = g_signal_connect(venture_database_get_posting_service(f->database), "posting", G_CALLBACK(fail_posting), NULL);
	g_assert_false(ach_delivery(service, "evt_ach_retry", "async_payment_succeeded", "paid", &error));
	g_assert_nonnull(error);
	g_clear_error(&error);
	g_signal_handler_disconnect(venture_database_get_posting_service(f->database), handler);
	evidence = rows(f, "stripe_event");
	g_assert_cmpuint(evidence->len, ==, 1);
	g_clear_pointer(&evidence, g_ptr_array_unref);
	evidence = rows(f, "processor_exception");
	g_assert_cmpuint(evidence->len, ==, 1);
	g_assert_true(ach_delivery(service, "evt_ach_retry", "async_payment_succeeded", "paid", &error));
	g_assert_no_error(error);
	g_clear_pointer(&evidence, g_ptr_array_unref);
	evidence = rows(f, "payment");
	g_assert_cmpuint(evidence->len, ==, 1);
}

/* Manual cash arriving while a bank debit is pending must not silently turn
 * the later bank debit into guessed overpayment or a second collection. */
static void
test_ach_balance_conflict(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GObject) transport = g_object_new(fake_transport_get_type(), NULL);
	g_autoptr(VentureStripeService) service = venture_stripe_service_new(f->database, f->organization_id, STRIPE_TRANSPORT(transport), &error);
	g_autoptr(VentureEntity) invoice = invoice_new(f, "ACH-MANUAL", "2026-01-01", "100 USD");
	g_autoptr(VentureStripeCheckout) checkout = NULL, again = NULL;
	g_autoptr(VenturePayment) manual = venture_payment_new();
	g_autoptr(VentureMoney) amount = venture_money_new(3000, "USD", 2);
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(GPtrArray) payments = NULL;
	(void)data;
	g_assert_no_error(error);
	checkout = venture_stripe_service_checkout(service, venture_entity_get_id(invoice), NULL, &error);
	g_assert_no_error(error);
	g_assert_true(ach_delivery(service, "evt_ach_authorized", "completed", "unpaid", &error));
	g_assert_no_error(error);
	venture_entity_set_organization_id(VENTURE_ENTITY(manual), f->organization_id);
	g_object_set(manual, "customer-id", f->customer_id, "invoice-id", venture_entity_get_id(invoice), "amount", amount, "date", now, "method", "cash", NULL);
	g_assert_true(venture_settlement_service_apply_payment(venture_settlement_service_get(f->database), manual, NULL, NULL, &error));
	g_assert_no_error(error);
	again = venture_stripe_service_checkout(service, venture_entity_get_id(invoice), NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpint(venture_entity_get_id(VENTURE_ENTITY(again)), ==, venture_entity_get_id(VENTURE_ENTITY(checkout)));
	g_assert_cmpuint(((FakeTransport *)transport)->checkouts, ==, 1);
	g_assert_false(ach_delivery(service, "evt_ach_balance", "async_payment_succeeded", "paid", &error));
	g_assert_nonnull(error);
	payments = rows(f, "payment");
	g_assert_cmpuint(payments->len, ==, 1);
	g_clear_error(&error);
	{
		g_autoptr(GPtrArray) events = rows(f, "stripe_event");
		gint64 event_id = venture_entity_get_id(g_ptr_array_index(events, events->len - 1));
		g_assert_false(venture_stripe_service_retry_event(service, event_id, FALSE, NULL, &error));
		g_assert_nonnull(error);
		g_clear_error(&error);
		if (data != NULL)
		{
			g_autoptr(VentureAccountingApprovalRule) rule = venture_accounting_approval_rule_new();
			VentureActor alice, bob;
			alice.kind = VENTURE_ACTOR_KIND_USER;
			alice.name = "finance-alice";
			alice.prompt = NULL;
			alice.request_id = NULL;
			alice.approved_by = NULL;
			bob = alice;
			bob.name = "finance-bob";
			venture_entity_set_organization_id(VENTURE_ENTITY(rule), f->organization_id);
			g_object_set(rule, "action", "pay", "require-second-actor", TRUE, NULL);
			g_assert_true(venture_database_save(f->database, VENTURE_ENTITY(rule), NULL, &error));
			g_assert_no_error(error);
			g_assert_false(venture_stripe_service_retry_event(service, event_id, TRUE, &alice, &error));
			g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
			g_clear_error(&error);
			g_assert_false(venture_stripe_service_retry_event(service, event_id, TRUE, &alice, &error));
			g_clear_error(&error);
			g_assert_true(venture_stripe_service_retry_event(service, event_id, TRUE, &bob, &error));
		}
		else g_assert_true(venture_stripe_service_retry_event(service, event_id, TRUE, NULL, &error));
		g_assert_no_error(error);
		g_clear_pointer(&payments, g_ptr_array_unref);
		payments = rows(f, "payment");
		g_assert_cmpuint(payments->len, ==, 2);
	}
}

/* ACH is an organization opt-in, carried through the real encrypted binding
 * into the pinned provider request, not an installation environment switch. */
static void
test_ach_settings(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GObject) transport = g_object_new(fake_transport_get_type(), NULL);
	g_autoptr(JsonNode) settings = binding_settings("whsec_original");
	g_autoptr(GBytes) key = g_bytes_new_static("01234567890123456789012345678901", 32);
	g_autoptr(VentureIntegrationConnection) connection = NULL;
	g_autoptr(VentureStripeService) service = NULL;
	g_autoptr(VentureEntity) invoice = invoice_new(f, "ACH-ENABLED", "2026-01-01", "100 USD");
	g_autoptr(VentureStripeCheckout) checkout = NULL;
	(void)data;
	g_assert_true(venture_integration_service_set_key(venture_integration_service_get(f->database), key, &error));
	json_object_set_boolean_member(json_node_get_object(settings), "ach_enabled", TRUE);
	connection = venture_stripe_settings_configure(f->database, f->organization_id, settings, 0, 0, STRIPE_TRANSPORT(transport), NULL, &error);
	g_assert_no_error(error);
	service = venture_stripe_service_for_organization(f->database, f->organization_id, STRIPE_TRANSPORT(transport), &error);
	g_assert_no_error(error);
	checkout = venture_stripe_service_checkout(service, venture_entity_get_id(invoice), NULL, &error);
	g_assert_no_error(error);
	g_assert_true(((FakeTransport *)transport)->checkout_ach);
}

/* Delayed notification or operator retry cannot move a bank settlement out of
 * its closed accounting period by substituting today's clock. */
static void
test_ach_closed_period(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GObject) transport = g_object_new(fake_transport_get_type(), NULL);
	g_autoptr(VentureStripeService) service = venture_stripe_service_new(f->database, f->organization_id, STRIPE_TRANSPORT(transport), &error);
	g_autoptr(VentureEntity) invoice = invoice_new(f, "ACH-CLOSED", "2026-01-01", "100 USD");
	g_autoptr(VentureStripeCheckout) checkout = NULL;
	g_autoptr(GDateTime) start = venture_time_from_string("2026-01-01", NULL);
	g_autoptr(GDateTime) settled = venture_time_from_string("2026-01-15", NULL);
	g_autoptr(VentureFiscalYear) year = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_FISCAL_PERIOD);
	g_autoptr(GPtrArray) periods = NULL;
	g_autoptr(GBytes) raw = NULL;
	g_autofree gchar *body = NULL, *sig = NULL;
	VentureActor actor;
	(void)data;
	actor.kind = VENTURE_ACTOR_KIND_USER;
	actor.name = "finance-closer";
	actor.prompt = NULL;
	actor.request_id = NULL;
	actor.approved_by = NULL;
	g_assert_no_error(error);
	checkout = venture_stripe_service_checkout(service, venture_entity_get_id(invoice), NULL, &error);
	g_assert_no_error(error);
	year = venture_period_service_generate(venture_period_service_get(f->database), f->organization_id, "2026", start, VENTURE_PERIOD_MONTHLY, &actor, &error);
	g_assert_no_error(error);
	venture_query_set_organization(query, f->organization_id);
	g_assert_true(venture_query_add_order(query, "start-at", VENTURE_SORT_ASCENDING, &error));
	periods = venture_database_find(f->database, query, &error);
	g_assert_no_error(error);
	g_object_set(g_ptr_array_index(periods, 0), "state", VENTURE_PERIOD_CLOSED, NULL);
	g_assert_true(venture_database_save(f->database, g_ptr_array_index(periods, 0), &actor, &error));
	g_assert_no_error(error);
	body = g_strdup_printf("{\"id\":\"evt_ach_closed\",\"created\":%" G_GINT64_FORMAT ",\"type\":\"checkout.session.async_payment_succeeded\",\"data\":{\"object\":{\"id\":\"cs_offline\",\"amount_total\":10000,\"currency\":\"usd\",\"payment_status\":\"paid\",\"payment_intent\":\"pi_ach_closed\"}}}", g_date_time_to_unix(settled));
	sig = signature(body);
	raw = g_bytes_new(body, strlen(body));
	g_assert_false(venture_stripe_service_handle_webhook(service, raw, sig, &error));
	g_assert_nonnull(error);
	g_clear_error(&error);
	{
		g_autoptr(GPtrArray) events = rows(f, "stripe_event");
		g_assert_cmpuint(events->len, ==, 1);
		g_assert_false(venture_stripe_service_retry_event(service,
			venture_entity_get_id(g_ptr_array_index(events, 0)), TRUE, &actor, &error));
		g_assert_nonnull(error);
	}
}

/* A failed debit permits a new attempt, but a contradictory late success
 * must not silently post while that replacement can still collect. */
static void
test_ach_failed_replacement(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GObject) transport = g_object_new(fake_transport_get_type(), NULL);
	g_autoptr(VentureStripeService) service = venture_stripe_service_new(f->database, f->organization_id, STRIPE_TRANSPORT(transport), &error);
	g_autoptr(VentureEntity) invoice = invoice_new(f, "ACH-FAILED", "2026-01-01", "100 USD");
	g_autoptr(VentureStripeCheckout) checkout = NULL, replacement = NULL;
	g_autoptr(GPtrArray) payments = NULL;
	(void)data;
	g_assert_no_error(error);
	checkout = venture_stripe_service_checkout(service, venture_entity_get_id(invoice), NULL, &error);
	g_assert_no_error(error);
	g_assert_true(ach_delivery(service, "evt_ach_failed", "async_payment_failed", "unpaid", &error));
	g_assert_no_error(error);
	g_assert_true(ach_delivery(service, "evt_ach_stale", "completed", "unpaid", &error));
	g_assert_no_error(error);
	replacement = venture_stripe_service_checkout(service, venture_entity_get_id(invoice), NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpint(venture_entity_get_id(VENTURE_ENTITY(checkout)), !=, venture_entity_get_id(VENTURE_ENTITY(replacement)));
	g_assert_false(ach_delivery(service, "evt_ach_contradiction", "async_payment_succeeded", "paid", &error));
	g_assert_nonnull(error);
	payments = rows(f, "payment");
	g_assert_cmpuint(payments->len, ==, 0);
}

/* Authentication does not make mismatched financial evidence correct. */
static void
test_ach_pending_mismatch(Fixture *f, gconstpointer data)
{
	const gchar *body = "{\"id\":\"evt_ach_bad_pending\",\"type\":\"checkout.session.completed\",\"data\":{\"object\":{\"id\":\"cs_offline\",\"amount_total\":9999,\"currency\":\"usd\",\"payment_status\":\"unpaid\",\"payment_intent\":\"pi_ach\"}}}";
	g_autoptr(GError) error = NULL;
	g_autoptr(GObject) transport = g_object_new(fake_transport_get_type(), NULL);
	g_autoptr(VentureStripeService) service = venture_stripe_service_new(f->database, f->organization_id, STRIPE_TRANSPORT(transport), &error);
	g_autoptr(VentureEntity) invoice = invoice_new(f, "ACH-MISMATCH", "2026-01-01", "100 USD");
	g_autoptr(VentureStripeCheckout) checkout = NULL;
	g_autoptr(GPtrArray) exceptions = NULL;
	g_autofree gchar *sig = signature(body);
	g_autoptr(GBytes) raw = g_bytes_new(body, strlen(body));
	(void)data;
	g_assert_no_error(error);
	checkout = venture_stripe_service_checkout(service, venture_entity_get_id(invoice), NULL, &error);
	g_assert_no_error(error);
	g_assert_false(venture_stripe_service_handle_webhook(service, raw, sig, &error));
	g_assert_nonnull(error);
	exceptions = rows(f, "processor_exception");
	g_assert_cmpuint(exceptions->len, ==, 1);
}

/* A bank return is a refund of verified cash, not removal of the receipt.
 * A won dispute restores status from allocation evidence without inventing a
 * new paid date. Repeated provider events cannot reverse cash twice. */
static void
test_ach_return(Fixture *f, gconstpointer data)
{
	const gchar *final_state = data;
	g_autoptr(GError) error = NULL;
	g_autoptr(GObject) transport = g_object_new(fake_transport_get_type(), NULL);
	g_autoptr(VentureStripeService) service = venture_stripe_service_new(f->database, f->organization_id, STRIPE_TRANSPORT(transport), &error);
	g_autoptr(VentureEntity) invoice = invoice_new(f, "ACH-RETURN", "2026-01-01", "100 USD");
	g_autoptr(VentureStripeCheckout) checkout = NULL;
	g_autoptr(GDateTime) paid_at = NULL;
	g_autoptr(GPtrArray) disputes = NULL, refunds = NULL;
	guint i;
	g_assert_no_error(error);
	checkout = venture_stripe_service_checkout(service, venture_entity_get_id(invoice), NULL, &error);
	g_assert_no_error(error);
	g_assert_true(ach_delivery(service, "evt_ach_original", "async_payment_succeeded", "paid", &error));
	g_assert_no_error(error);
	{
		g_autoptr(VentureEntity) paid = venture_database_get(f->database, VENTURE_TYPE_INVOICE, venture_entity_get_id(invoice), &error);
		g_assert_no_error(error);
		g_object_get(paid, "paid-at", &paid_at, NULL);
	}
	for (i = 0; i < 3; i++)
	{
		g_autofree gchar *body = g_strdup_printf("{\"id\":\"evt_return_%u\",\"type\":\"charge.dispute.%s\",\"data\":{\"object\":{\"id\":\"dp_ach_return\",\"payment_intent\":\"pi_ach\",\"amount\":3000,\"currency\":\"usd\",\"status\":\"%s\"}}}", i, i ? "closed" : "created", i ? final_state : "needs_response");
		g_autofree gchar *sig = signature(body);
		g_autoptr(GBytes) raw = g_bytes_new(body, strlen(body));
		g_assert_true(venture_stripe_service_handle_webhook(service, raw, sig, &error));
		g_assert_no_error(error);
		g_clear_pointer(&disputes, g_ptr_array_unref);
		disputes = rows(f, "processor_dispute");
		g_assert_cmpuint(disputes->len, ==, 1);
	}
	refunds = rows(f, "refund");
	g_assert_cmpuint(refunds->len, ==, !g_strcmp0(final_state, "lost") ? 1 : 0);
	{
		g_autoptr(VentureMoney) balance = venture_settlement_service_invoice_balance(venture_settlement_service_get(f->database), venture_entity_get_id(invoice), NULL, &error);
		g_assert_no_error(error);
		g_assert_cmpint(venture_money_get_amount(balance), ==, !g_strcmp0(final_state, "lost") ? 3000 : 0);
	}
	if (!g_strcmp0(final_state, "won"))
	{
		g_autoptr(VentureEntity) paid = venture_database_get(f->database, VENTURE_TYPE_INVOICE, venture_entity_get_id(invoice), &error);
		g_autoptr(GDateTime) actual_paid_at = NULL;
		g_autofree gchar *workflow = NULL;
		g_assert_no_error(error);
		g_object_get(paid, "workflow-state", &workflow, "paid-at", &actual_paid_at, NULL);
		g_assert_cmpstr(workflow, ==, "paid");
		g_assert_nonnull(actual_paid_at);
		g_assert_true(g_date_time_equal(actual_paid_at, paid_at));
	}
}

/* Separate connections cannot reserve two live debits, and ambiguous legacy
 * rows fail upgrade without selecting a winner or rewriting old evidence. */
static void
test_ach_database_backstop(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GObject) transport = g_object_new(fake_transport_get_type(), NULL);
	g_autoptr(VentureStripeService) service = venture_stripe_service_new(f->database, f->organization_id, STRIPE_TRANSPORT(transport), &error);
	g_autoptr(VentureEntity) invoice = invoice_new(f, "ACH-UNIQUE", "2026-01-01", "100 USD");
	g_autoptr(VentureStripeCheckout) checkout = NULL;
	g_autofree gchar *sql = NULL, *migration = NULL;
	g_autoptr(GPtrArray) attempts = NULL;
	guint i;
	(void)data;
	g_assert_no_error(error);
	checkout = venture_stripe_service_checkout(service, venture_entity_get_id(invoice), NULL, &error);
	g_assert_no_error(error);
	sql = g_strdup_printf("INSERT INTO stripe_checkouts (uuid, organization_id, invoice_id, connection_id, session_id, status, url, active) VALUES ('legacy-duplicate', %" G_GINT64_FORMAT ", %" G_GINT64_FORMAT ", 0, 'cs_legacy_duplicate', 'open', '', TRUE)", f->organization_id, venture_entity_get_id(invoice));
	g_assert_false(venture_database_execute(f->database, sql, NULL, &error));
	g_assert_nonnull(error);
	g_clear_error(&error);
	g_assert_true(venture_database_execute(f->database, "UPDATE stripe_checkouts SET active = FALSE", NULL, &error));
	g_assert_no_error(error);
	g_assert_true(venture_database_execute(f->database, sql, NULL, &error));
	g_assert_no_error(error);
	g_assert_true(venture_database_execute(f->database, "UPDATE stripe_checkouts SET active = FALSE", NULL, &error));
	g_assert_no_error(error);
	g_assert_true(g_file_get_contents("migrations/sqlite/000450_stripe_delayed_settlement.sql", &migration, NULL, &error));
	g_assert_no_error(error);
	g_assert_true(venture_database_begin(f->database, &error));
	g_assert_false(venture_migrations_execute_sql(venture_database_get_connection(f->database), migration, &error));
	g_assert_nonnull(error);
	g_clear_error(&error);
	venture_database_rollback(f->database);
	attempts = rows(f, "stripe_checkout");
	g_assert_cmpuint(attempts->len, ==, 2);
	for (i = 0; i < attempts->len; i++)
	{
		gboolean active;
		g_object_get(g_ptr_array_index(attempts, i), "active", &active, NULL);
		g_assert_false(active);
	}
}

/* A provider can deliver success before its create response reaches us. The
 * signed reservation reference must settle once and never reopen on return. */
static void
test_ach_early(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GObject) transport = g_object_new(fake_transport_get_type(), NULL);
	g_autoptr(VentureStripeService) service = venture_stripe_service_new(f->database, f->organization_id, STRIPE_TRANSPORT(transport), &error);
	g_autoptr(VentureEntity) invoice = invoice_new(f, "ACH-EARLY", "2026-01-01", "100 USD");
	g_autoptr(VentureStripeCheckout) checkout = NULL;
	g_autoptr(GPtrArray) payments = NULL;
	g_autofree gchar *state = NULL;
	gboolean active;
	(void)data;
	g_assert_no_error(error);
	((FakeTransport *)transport)->early_service = service;
	checkout = venture_stripe_service_checkout(service, venture_entity_get_id(invoice), NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(checkout);
	g_object_get(checkout, "status", &state, "active", &active, NULL);
	g_assert_cmpstr(state, ==, "complete");
	g_assert_false(active);
	payments = rows(f, "payment");
	g_assert_cmpuint(payments->len, ==, 1);
}

/* A return cannot move to today's open period, and an operator cannot approve
 * their own refund. Replaying retained provider evidence preserves both rules. */
static void
test_ach_return_guard(Fixture *f, gconstpointer data)
{
	gboolean closed = !g_strcmp0(data, "closed");
	g_autoptr(GError) error = NULL;
	g_autoptr(GObject) transport = g_object_new(fake_transport_get_type(), NULL);
	g_autoptr(VentureStripeService) service = venture_stripe_service_new(f->database, f->organization_id, STRIPE_TRANSPORT(transport), &error);
	g_autoptr(VentureEntity) invoice = invoice_new(f, "RETURN-GUARD", "2026-01-01", "100 USD");
	g_autoptr(VentureStripeCheckout) checkout = NULL;
	g_autoptr(GDateTime) paid = closed ? venture_time_from_string("2026-01-10", NULL) : venture_time_now();
	g_autoptr(GDateTime) returned = closed ? venture_time_from_string("2026-01-15", NULL) : venture_time_now();
	g_autofree gchar *body = NULL, *sig = NULL;
	g_autoptr(GBytes) raw = NULL;
	g_autoptr(GPtrArray) events = NULL, refunds = NULL;
	VentureActor alice, bob;
	gint64 event_id;
	gulong posting_failure = 0;
	alice.kind = VENTURE_ACTOR_KIND_USER; alice.name = "return-alice";
	alice.prompt = NULL; alice.request_id = NULL; alice.approved_by = NULL;
	bob = alice; bob.name = "return-bob";
	g_assert_no_error(error);
	checkout = venture_stripe_service_checkout(service, venture_entity_get_id(invoice), NULL, &error);
	g_assert_no_error(error);
	body = g_strdup_printf("{\"id\":\"evt_return_receipt\",\"created\":%" G_GINT64_FORMAT ",\"type\":\"checkout.session.completed\",\"data\":{\"object\":{\"id\":\"cs_offline\",\"amount_total\":10000,\"currency\":\"usd\",\"payment_status\":\"paid\",\"payment_intent\":\"pi_return_guard\"}}}", g_date_time_to_unix(paid));
	sig = signature(body); raw = g_bytes_new(body, strlen(body));
	g_assert_true(venture_stripe_service_handle_webhook(service, raw, sig, &error));
	g_assert_no_error(error);
	if (closed)
	{
		g_autoptr(GDateTime) start = venture_time_from_string("2026-01-01", NULL);
		g_autoptr(VentureFiscalYear) year = venture_period_service_generate(venture_period_service_get(f->database), f->organization_id, "2026", start, VENTURE_PERIOD_MONTHLY, &alice, &error);
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_FISCAL_PERIOD);
		g_autoptr(GPtrArray) periods = NULL;
		g_assert_no_error(error); g_assert_nonnull(year);
		venture_query_set_organization(query, f->organization_id);
		g_assert_true(venture_query_add_order(query, "start-at", VENTURE_SORT_ASCENDING, &error));
		periods = venture_database_find(f->database, query, &error);
		g_assert_no_error(error);
		g_object_set(g_ptr_array_index(periods, 0), "state", VENTURE_PERIOD_CLOSED, NULL);
		g_assert_true(venture_database_save(f->database, g_ptr_array_index(periods, 0), &alice, &error));
		g_assert_no_error(error);
	}
	else
	{
		g_autoptr(VentureAccountingApprovalRule) rule = venture_accounting_approval_rule_new();
		venture_entity_set_organization_id(VENTURE_ENTITY(rule), f->organization_id);
		g_object_set(rule, "action", "post", "require-second-actor", TRUE, NULL);
		g_assert_true(venture_database_save(f->database, VENTURE_ENTITY(rule), NULL, &error));
		g_assert_no_error(error);
	}
	g_clear_pointer(&body, g_free); g_clear_pointer(&sig, g_free); g_clear_pointer(&raw, g_bytes_unref);
	body = g_strdup_printf("{\"id\":\"evt_return_guard\",\"created\":%" G_GINT64_FORMAT ",\"type\":\"charge.dispute.closed\",\"data\":{\"object\":{\"id\":\"dp_return_guard\",\"payment_intent\":\"pi_return_guard\",\"amount\":3000,\"currency\":\"usd\",\"status\":\"lost\"}}}", g_date_time_to_unix(returned));
	sig = signature(body); raw = g_bytes_new(body, strlen(body));
	/* Verified unattended callbacks retain existing trusted-service policy.
	 * Force a local posting failure so the operator must replay retained evidence. */
	if (!closed) posting_failure = g_signal_connect(venture_database_get_posting_service(f->database), "posting", G_CALLBACK(fail_posting), NULL);
	g_assert_false(venture_stripe_service_handle_webhook(service, raw, sig, &error));
	if (posting_failure) g_signal_handler_disconnect(venture_database_get_posting_service(f->database), posting_failure);
	g_assert_nonnull(error); g_clear_error(&error);
	refunds = rows(f, "refund"); g_assert_cmpuint(refunds->len, ==, 0);
	events = rows(f, "stripe_event"); g_assert_cmpuint(events->len, ==, 2);
	event_id = venture_entity_get_id(g_ptr_array_index(events, 1));
	g_assert_false(venture_stripe_service_retry_event(service, event_id, FALSE, &alice, &error));
	g_assert_nonnull(error); g_clear_error(&error);
	g_assert_false(venture_stripe_service_retry_event(service, event_id, FALSE, &alice, &error));
	g_assert_nonnull(error); g_clear_error(&error);
	if (closed)
	{
		g_assert_false(venture_stripe_service_retry_event(service, event_id, TRUE, &bob, &error));
		g_assert_nonnull(error);
	}
	else
	{
		g_assert_true(venture_stripe_service_retry_event(service, event_id, FALSE, &bob, &error));
		g_assert_no_error(error);
		g_clear_pointer(&refunds, g_ptr_array_unref);
		refunds = rows(f, "refund"); g_assert_cmpuint(refunds->len, ==, 1);
	}
}
