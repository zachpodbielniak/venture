/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture-stripe-disputes.inc"
static gboolean
stripe_process_verified(VentureStripeService *self, JsonObject *event, GBytes *raw,
	GPtrArray *observations, gboolean accept_balance_change,
	const VentureActor *actor, GError **error)
{
	guint observation_index;
	g_autoptr(GPtrArray) prior = NULL;
	g_autoptr(GPtrArray) sessions = NULL;
	g_autoptr(VentureStripeEvent) record = NULL;
	g_autoptr(VentureMoney) expected = NULL;
	g_autoptr(VentureMoney) received = NULL;
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(VenturePayment) payment = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(GDateTime) settled_at = NULL;
	g_autofree gchar *payload = NULL;
	const gchar *event_id, *type, *session_id, *intent, *currency;
	JsonObject *object;
	JsonNode *amount_node;
	VentureEntity *checkout;
	gint64 invoice_id = 0, company_id, record_id = 0, checkout_id = 0;
	gboolean mismatch = FALSE;

	if (!(venture_database_has_transaction(self->database) ? venture_database_begin(self->database, error) :
		venture_database_begin_serializable(self->database, error))) return FALSE;
	event_id = string_member(event, "id");
	type = string_member(event, "type");
	if (!event_id || !*event_id || !type) { refuse(error, "Webhook requires event id and type"); goto fail; }
	prior = find_rows(self, venture_stripe_event_get_type(), "event-id", event_id, error);
	if (!prior) goto fail;
	if (prior->len)
	{
		g_autofree gchar *result = NULL;
		g_autofree gchar *stored = NULL;
		g_autoptr(JsonParser) parser = json_parser_new();
		g_autoptr(JsonNode) presented = json_node_new(JSON_NODE_OBJECT);
		record = g_object_ref(g_ptr_array_index(prior, 0));
		g_object_get(record, "result", &result, "payload", &stored, NULL);
		json_node_set_object(presented, event);
		if (!stored || !json_parser_load_from_data(parser, stored, -1, error)) goto fail;
		if (!json_node_equal(json_parser_get_root(parser), presented))
		{
			refuse(error, "Stripe event identity was reused with different evidence");
			goto fail;
		}
		/* Failed local work is retryable; durable payment identity still
		 * protects the books when an event arrives under a fresh event ID. */
		if (!g_strcmp0(result, "exception") || !g_strcmp0(result, "pending") || !g_strcmp0(result, "unknown"))
			goto resume;
		venture_database_rollback(self->database);
		return TRUE;
	}
	for (observation_index = 0; observation_index < (observations ? observations->len : 0); observation_index++)
	{
		Observation *observation = g_ptr_array_index(observations, observation_index);
		audit_event(self->client, observation->name, observation->payload, self);
	}
	record = venture_stripe_event_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(record), self->organization_id);
	payload = g_strndup(g_bytes_get_data(raw, NULL), g_bytes_get_size(raw));
	g_object_set(record, "connection-id", self->connection_id, "event-id", event_id, "type", type, "received-at", now,
		"payload", payload, "result", "pending", NULL);
	if (!venture_stripe_save_owned(self->database, VENTURE_ENTITY(record), NULL, error)) goto fail;
resume:
	if (!venture_database_commit(self->database, error)) goto fail;
	record_id = venture_entity_get_id(VENTURE_ENTITY(record));
	/* Retain verified input before accounting; nested posting rollback must
	 * not erase the evidence needed to recover after repair or restart. */
	if (!(venture_database_has_transaction(self->database) ? venture_database_begin(self->database, error) :
		venture_database_begin_serializable(self->database, error))) return FALSE;
	{
		JsonNode *created = json_object_get_member(event, "created");
		g_object_get(record, "received-at", &settled_at, NULL);
		if (created)
		{
			if (!JSON_NODE_HOLDS_VALUE(created) || json_node_get_value_type(created) != G_TYPE_INT64 ||
				json_node_get_int(created) < 946684800 || json_node_get_int(created) > g_date_time_to_unix(now) + 300)
			{
				refuse(error, "Stripe event has an invalid effective timestamp");
				goto fail;
			}
			g_clear_pointer(&settled_at, g_date_time_unref);
			settled_at = g_date_time_new_from_unix_utc(json_node_get_int(created));
		}
	}
	g_object_set(record, "result", "ignored", NULL);
	if (!g_strcmp0(type, "charge.dispute.created") || !g_strcmp0(type, "charge.dispute.updated") ||
		!g_strcmp0(type, "charge.dispute.closed"))
	{
		object = object_member(object_member(event, "data"), "object");
		if (!stripe_dispute_event(self, object, settled_at, actor, &invoice_id, error)) goto fail;
		g_object_set(record, "result", "processed", NULL);
		if (!stripe_event_exception(self, record, "recovered", "Verified dispute outcome applied", invoice_id, 0, TRUE, error)) goto fail;
		goto done;
	}
	if (g_strcmp0(type, "checkout.session.completed") &&
		g_strcmp0(type, "checkout.session.async_payment_succeeded") &&
		g_strcmp0(type, "checkout.session.async_payment_failed") &&
		g_strcmp0(type, "checkout.session.expired")) goto done;
	object = object_member(object_member(event, "data"), "object");
	session_id = string_member(object, "id");
	if (!session_id) { refuse(error, "Checkout event requires session id"); goto fail; }
	sessions = find_rows(self, venture_stripe_checkout_get_type(), "session-id", session_id, error);
	if (!sessions) goto fail;
	if (!sessions->len)
	{
		const gchar *reference = string_member(object, "client_reference_id");
		/* The signed provider reference bridges the interval between durable
		 * reservation and response persistence. It is scoped to this account. */
		if (reference && g_uuid_string_is_valid(reference))
		{
			g_clear_pointer(&sessions, g_ptr_array_unref);
			sessions = find_rows(self, VENTURE_TYPE_STRIPE_CHECKOUT, "uuid", reference, error);
			if (!sessions) goto fail;
		}
	}
	if (!sessions->len)
	{
		g_object_set(record, "result", "unknown", NULL);
		if (!stripe_event_exception(self, record, "out_of_order", "Verified event names an unknown Checkout; retry after linking provider evidence", 0, 0, FALSE, error)) goto fail;
		goto done;
	}
	checkout = g_ptr_array_index(sessions, 0);
	checkout_id = venture_entity_get_id(checkout);
	g_object_get(checkout, "invoice-id", &invoice_id, NULL);
	g_object_get(checkout, "expected", &expected, "invoice-id", &invoice_id, NULL);
	amount_node = json_object_get_member(object, "amount_total");
	currency = string_member(object, "currency");
	intent = string_member(object, "payment_intent");
	if (expected && amount_node && json_node_get_value_type(amount_node) == G_TYPE_INT64 &&
	    currency && !g_ascii_strcasecmp(currency, venture_money_get_currency(expected)))
		received = charge_money(json_node_get_int(amount_node), currency);
	if (!received || !money_equal(received, expected))
	{
		mismatch = TRUE;
		g_object_set(record, "result", "mismatch", NULL);
		if (!stripe_event_exception(self, record, "mismatch", "Provider amount or currency differs from the reserved invoice balance", invoice_id, checkout_id, FALSE, error)) goto fail;
		goto done;
	}

	{
		g_autofree gchar *stored_id = NULL;
		g_object_get(checkout, "session-id", &stored_id, NULL);
		if (stored_id && *stored_id && g_strcmp0(stored_id, session_id))
		{
			refuse(error, "Verified correlation conflicts with retained Checkout identity");
			goto fail;
		}
		if (!stored_id || !*stored_id)
		{
			g_object_set(checkout, "session-id", session_id, NULL);
			if (!venture_stripe_save_owned(self->database, checkout, NULL, error)) goto fail;
		}
	}

	{
		gint64 payment_id;
		g_autofree gchar *state = NULL;
		g_object_get(checkout, "payment-id", &payment_id, "status", &state, NULL);
		/* Success is terminal for this attempt; stale authorization/failure
		 * deliveries must never erase posted payment evidence. */
		if (payment_id) goto done;
		if (!g_strcmp0(type, "checkout.session.expired"))
		{
			if (!g_strcmp0(state, "open") || !g_strcmp0(state, "initiated"))
			{
				g_object_set(checkout, "status", "expired", "active", FALSE, NULL);
				if (!venture_stripe_save_owned(self->database, checkout, NULL, error)) goto fail;
			}
			goto done;
		}
		if (!g_strcmp0(type, "checkout.session.async_payment_failed"))
		{
			g_object_set(checkout, "status", "failed", "active", FALSE, NULL);
			if (!venture_stripe_save_owned(self->database, checkout, NULL, error)) goto fail;
			g_object_set(record, "result", "failed", NULL);
			goto done;
		}
		if (!g_strcmp0(type, "checkout.session.completed") &&
			!g_strcmp0(string_member(object, "payment_status"), "unpaid"))
		{
			if (!g_strcmp0(state, "open") || !g_strcmp0(state, "initiated"))
			{
				g_object_set(checkout, "status", "processing", NULL);
				if (!venture_stripe_save_owned(self->database, checkout, NULL, error)) goto fail;
			}
			g_object_set(record, "result", "processing", NULL);
			goto done;
		}
	}
	if (g_strcmp0(string_member(object, "payment_status"), "paid") || !intent || !g_str_has_prefix(intent, "pi_"))
	{ refuse(error, "Checkout completion requires paid status and payment_intent"); goto fail; }
	{
		gint64 payment_id;
		g_object_get(checkout, "payment-id", &payment_id, NULL);
		if (payment_id) goto done;
	}
	{
		g_autoptr(VentureQuery) active = venture_query_new(VENTURE_TYPE_STRIPE_CHECKOUT);
		g_autoptr(GPtrArray) others = NULL;
		guint i;
		venture_query_set_limit(active, 0);
		venture_query_set_organization(active, self->organization_id);
		if (!venture_query_add_filter_int(active, "invoice-id", VENTURE_FILTER_OP_EQ, invoice_id, error)) goto fail;
		others = venture_database_find(self->database, active, error);
		if (!others) goto fail;
		for (i = 0; i < others->len; i++)
		{
			VentureEntity *other = g_ptr_array_index(others, i);
			g_autofree gchar *state = NULL;
			g_object_get(other, "status", &state, NULL);
			if (venture_entity_get_id(other) != checkout_id && checkout_pending(state))
			{
				refuse(error, "Another attempt can still collect this invoice; reconcile the contradictory payment evidence first");
				goto fail;
			}
		}
	}
	invoice = owned_get(self, VENTURE_TYPE_INVOICE, invoice_id, error);
	if (!invoice) goto fail;
	{
		g_autoptr(VentureMoney) balance = venture_settlement_service_invoice_balance(
			venture_settlement_service_get(self->database), invoice_id, NULL, error);
		if (!balance) goto fail;
		if (!accept_balance_change && !money_equal(balance, expected))
		{
			refuse(error, "Invoice balance changed while payment was pending; reconcile the verified payment before allocating it");
			goto fail;
		}
	}
	g_object_get(invoice, "company-id", &company_id, NULL);
	payment = venture_payment_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(payment), self->organization_id);
	{
		g_autofree gchar *identity = self->connection_uuid ?
			g_strdup_printf("stripe:%s:%s", self->connection_uuid, intent) : g_strdup(intent);
		g_object_set(payment, "customer-id", company_id, "invoice-id", invoice_id,
			"amount", expected, "date", settled_at, "method", "stripe", "reference", intent, "external-id", identity, NULL);
	}
	if (!venture_settlement_service_apply_payment(venture_settlement_service_get(self->database), payment, NULL, actor, error)) goto fail;
	g_object_set(checkout, "status", "complete", "active", FALSE, "payment-id", venture_entity_get_id(VENTURE_ENTITY(payment)), NULL);
	if (!venture_stripe_save_owned(self->database, checkout, NULL, error) ||
		!stripe_checkout_exception(self, checkout, TRUE, error)) goto fail;
	g_object_set(record, "result", "processed", NULL);
	if (!stripe_event_exception(self, record, "recovered", "Verified settlement applied after retry", invoice_id, checkout_id, TRUE, error)) goto fail;
done:
	g_object_set(record, "processed-at", now, NULL);
	if (!venture_stripe_save_owned(self->database, VENTURE_ENTITY(record), NULL, error)) goto fail;
	if (!venture_database_commit(self->database, error)) goto fail;
	return mismatch ? refuse(error, "Stripe amount or currency mismatch with expected payment") : TRUE;
fail:
	venture_database_rollback(self->database);
	if (record_id > 0)
	{
		g_autoptr(GError) evidence_error = NULL;
		g_autoptr(VentureEntity) retained = NULL;
		if (venture_database_begin(self->database, &evidence_error))
		{
			retained = venture_database_get(self->database, VENTURE_TYPE_STRIPE_EVENT, record_id, &evidence_error);
			if (retained)
			{
				g_object_set(retained, "result", "exception", NULL);
				if (venture_stripe_save_owned(self->database, retained, NULL, &evidence_error) &&
					stripe_event_exception(self, VENTURE_STRIPE_EVENT(retained), "settlement_blocked",
						"Verified payment could not be applied; repair accounting or balance conflict and retry this event",
						invoice_id, checkout_id, FALSE, &evidence_error))
					venture_database_commit(self->database, &evidence_error);
			}
			if (evidence_error) venture_database_rollback(self->database);
		}
	}
	return FALSE;
}

gboolean
venture_stripe_service_handle_webhook(VentureStripeService *self, GBytes *raw,
	const gchar *signature, GError **error)
{
	g_autoptr(StripeClient) verifier = NULL;
	g_autoptr(GPtrArray) observations = g_ptr_array_new_with_free_func(observation_free);
	g_autoptr(JsonObject) event = NULL;
	guint observation_index;
	if (venture_database_has_transaction(self->database))
		return refuse(error, "Webhook verification requires an idle database transaction boundary");
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "stripe_checkout") == G_TYPE_INVALID)
		return refuse(error, "Stripe module is disabled (stripe.enabled)");
	if (!stripe_refresh(self, TRUE, error)) return FALSE;
	/* Durable event IDs replace the library's per-client replay cache. Each
	 * delivery is authenticated afresh, including identical signed retries. */
	verifier = stripe_client_new(self->yaml, self->transport, NULL, error);
	if (!verifier) return FALSE;
	g_signal_connect(verifier, "event", G_CALLBACK(collect_event), observations);
	event = stripe_client_verify_webhook(verifier, raw, signature, NULL, error);
	if (!event)
	{
		for (observation_index = 0; observation_index < observations->len; observation_index++)
		{
			Observation *observation = g_ptr_array_index(observations, observation_index);
			audit_event(verifier, observation->name, observation->payload, self);
		}
		/* Rejection evidence contains no raw body or signature. */
		return FALSE;
	}
	if (self->connection_id > 0)
	{
		JsonNode *live = json_object_get_member(event, "livemode");
		const gchar *account = string_member(event, "account");
		if (!live || json_node_get_value_type(live) != G_TYPE_BOOLEAN ||
			json_node_get_boolean(live) != !g_strcmp0(self->environment, "live") ||
			(json_object_has_member(event, "account") && !account) ||
			(account && g_strcmp0(account, self->account_id)))
		{
			return refuse(error, "Stripe event account or environment does not match its binding");
		}
	}
	return stripe_process_verified(self, event, raw, observations, FALSE, NULL, error);
}

gboolean
venture_stripe_service_retry_event(VentureStripeService *self, gint64 event_id,
	gboolean accept_balance_change, const VentureActor *actor, GError **error)
{
	static const gint roles[] = { VENTURE_ORGANIZATION_ROLE_OWNER,
		VENTURE_ORGANIZATION_ROLE_ADMIN, VENTURE_ORGANIZATION_ROLE_FINANCE };
	VentureAccessPolicy *policy = venture_database_get_access_policy(self->database);
	const VentureAuthPrincipal *principal = venture_access_policy_get_actor(policy);
	g_autoptr(VentureEntity) record = NULL;
	g_autoptr(VentureAccountingOperation) operation = NULL;
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(GBytes) raw = NULL;
	g_autofree gchar *payload = NULL;
	if (principal && !venture_access_policy_has_organization_role(policy, principal,
		self->organization_id, roles, G_N_ELEMENTS(roles)))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED,
			"Organization finance authorization is required to retry verified settlement");
		return FALSE;
	}
	if (!stripe_refresh(self, TRUE, error)) return FALSE;
	record = owned_get(self, VENTURE_TYPE_STRIPE_EVENT, event_id, error);
	if (!record || !venture_access_policy_check_write(policy, record, "write", error)) return FALSE;
	g_object_get(record, "payload", &payload, NULL);
	if (!payload || !json_parser_load_from_data(parser, payload, -1, error)) return FALSE;
	if (!JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser)))
		return refuse(error, "Stored verified event is malformed");
	raw = g_bytes_new(payload, strlen(payload));
	operation = venture_accounting_operation_begin(self->database, "stripe.retry_event",
		record, NULL, g_variant_new("(b)", accept_balance_change), self->organization_id, actor, error);
	if (!operation) return FALSE;
	if (!stripe_process_verified(self, json_node_get_object(json_parser_get_root(parser)),
		raw, NULL, accept_balance_change, actor, error)) return FALSE;
	return venture_accounting_operation_finish(operation, error);
}
