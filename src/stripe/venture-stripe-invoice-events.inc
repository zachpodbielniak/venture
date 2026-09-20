/* SPDX-License-Identifier: AGPL-3.0-or-later */
static gboolean
stripe_invoice_event_kind(JsonObject *event)
{
	const gchar *type = string_member(event, "type");
	return !g_strcmp0(type, "invoice.payment_succeeded") || !g_strcmp0(type, "invoice.paid") ||
		!g_strcmp0(type, "invoice.payment_failed");
}

static gboolean
stripe_prepare_invoice_evidence(VentureStripeService *self, VentureStripeEvent *record,
	JsonObject *event, GError **error)
{
	g_autofree gchar *stored = NULL, *customer = NULL, *method = NULL, *serialized = NULL;
	g_autoptr(GPtrArray) attempts = NULL;
	g_autoptr(VentureEntity) authorization = NULL;
	g_autoptr(VentureMoney) expected = NULL;
	g_autoptr(StripeRequest) request = stripe_request_new(STRIPE_INVOICE_RETRIEVE);
	g_autoptr(StripeResource) resource = NULL;
	g_autoptr(JsonObject) invoice = NULL, evidence = json_object_new();
	g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);
	g_autoptr(GDateTime) now = venture_time_now();
	JsonObject *object = object_member(object_member(event, "data"), "object");
	JsonObject *payment;
	VentureEntity *attempt;
	const gchar *id = string_member(object, "id"), *intent = string_member(object, "payment_intent");
	const gchar *payment_state, *currency;
	gboolean success = g_strcmp0(string_member(event, "type"), "invoice.payment_failed") != 0;
	gint64 amount, configuration_version;
	g_object_get(record, "provider-evidence", &stored, NULL);
	if (stored && *stored) return TRUE;
	if (venture_database_has_transaction(self->database))
		return refuse(error, "Invoice evidence must be read before opening an accounting transaction");
	if (g_strcmp0(self->api_version, "2024-06-20") || g_strcmp0(string_member(event, "api_version"), "2024-06-20"))
		return refuse(error, "Automatic invoice evidence requires API and webhook version 2024-06-20");
	if (!stripe_identifier(id, "in_") || !stripe_identifier(intent, "pi_"))
		return refuse(error, "Invoice event has no supported provider invoice/payment identity");
	attempts = find_rows(self, VENTURE_TYPE_STRIPE_CHECKOUT, "provider-invoice-id", id, error);
	if (!attempts) return FALSE;
	if (attempts->len != 1) return refuse(error, "Verified invoice has no unique retained collection attempt");
	attempt = g_ptr_array_index(attempts, 0);
	authorization = owned_get(self, VENTURE_TYPE_STRIPE_AUTHORIZATION, stripe_number(attempt, "authorization-id"), error);
	if (!authorization) return FALSE;
	g_object_get(attempt, "expected", &expected, NULL);
	g_object_get(authorization, "customer-id", &customer, "payment-method-id", &method, NULL);
	if (!expected || !to_stripe_amount(expected, &amount, error)) return FALSE;
	currency = string_member(object, "currency");
	if (g_strcmp0(string_member(object, "customer"), customer) || !currency ||
		g_ascii_strcasecmp(currency, venture_money_get_currency(expected)) ||
		!stripe_json_integer(object, "amount_paid", success ? amount : 0))
		return refuse(error, "Signed invoice evidence differs from the reserved customer, currency or amount");
	request->id = g_strdup(id); request->expansion = STRIPE_EXPAND_INVOICE_PAYMENT;
	configuration_version = self->connection_version;
	resource = stripe_client_execute(self->client, request, NULL, error);
	if (!resource) return FALSE;
	invoice = stripe_resource_get_json(resource); payment = object_member(invoice, "payment_intent");
	payment_state = string_member(payment, "status");
	currency = string_member(payment, "currency");
	if (!stripe_collection_invoice_matches(invoice, id, customer, method, expected, venture_entity_get_uuid(attempt), error)) return FALSE;
	if (!stripe_json_boolean(invoice, "livemode", !g_strcmp0(self->environment, "live")) ||
		!stripe_json_boolean(invoice, "paid_out_of_band", FALSE) ||
		!stripe_json_integer(invoice, "amount_paid", success ? amount : 0) ||
		!stripe_json_integer(invoice, "amount_remaining", success ? 0 : amount) ||
		g_strcmp0(string_member(invoice, "status"), success ? "paid" : "open") ||
		g_strcmp0(string_member(payment, "id"), intent) ||
		g_strcmp0(string_member(payment, "invoice"), id) || g_strcmp0(string_member(payment, "customer"), customer) ||
		g_strcmp0(string_member(payment, "payment_method"), method) || !currency ||
		g_ascii_strcasecmp(currency, venture_money_get_currency(expected)) ||
		!stripe_json_boolean(payment, "livemode", !g_strcmp0(self->environment, "live")) ||
		!stripe_json_integer(payment, "amount", amount) || !stripe_json_integer(payment, "amount_received", success ? amount : 0) ||
		(success ? g_strcmp0(payment_state, "succeeded") != 0 :
		 (g_strcmp0(payment_state, "requires_payment_method") && g_strcmp0(payment_state, "requires_action"))))
		return refuse(error, "Authenticated invoice/payment evidence does not prove the signed collection outcome");
	json_object_set_string_member(evidence, "outcome", success ? "succeeded" : "failed");
	json_object_set_string_member(evidence, "invoice", id);
	json_object_set_string_member(evidence, "payment_intent", intent);
	json_object_set_string_member(evidence, "customer", customer);
	json_object_set_string_member(evidence, "payment_method", method);
	json_object_set_string_member(evidence, "payment_status", payment_state);
	json_object_set_string_member(evidence, "currency", venture_money_get_currency(expected));
	json_object_set_int_member(evidence, "amount", amount);
	json_object_set_int_member(evidence, "attempt_id", venture_entity_get_id(attempt));
	json_object_set_string_member(evidence, "connection", self->connection_uuid);
	json_object_set_string_member(evidence, "api_version", "2024-06-20");
	json_object_set_int_member(evidence, "configuration_version", configuration_version);
	json_object_set_int_member(evidence, "verified_at", g_date_time_to_unix(now));
	json_node_set_object(node, evidence); serialized = venture_json_to_string(node, FALSE);
	g_object_set(record, "provider-evidence", serialized, NULL);
	return venture_stripe_save_owned(self->database, VENTURE_ENTITY(record), NULL, error);
}

static gboolean
stripe_invoice_event_apply(VentureStripeService *self, VentureStripeEvent *record,
	GDateTime *settled_at, gboolean accept_balance_change, const VentureActor *actor,
	gint64 *invoice_id, gint64 *checkout_id, GError **error)
{
	g_autofree gchar *serialized = NULL, *provider_id = NULL, *customer = NULL, *method = NULL, *phase = NULL;
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(VentureEntity) checkout = NULL, authorization = NULL;
	g_autoptr(VentureMoney) expected = NULL;
	JsonObject *evidence;
	JsonNode *attempt_node;
	gint64 amount;
	g_object_get(record, "provider-evidence", &serialized, NULL);
	if (!serialized || !json_parser_load_from_data(parser, serialized, -1, error)) return FALSE;
	if (!JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) return refuse(error, "Retained collection evidence is invalid");
	evidence = json_node_get_object(json_parser_get_root(parser));
	attempt_node = json_object_get_member(evidence, "attempt_id");
	if (!attempt_node || json_node_get_value_type(attempt_node) != G_TYPE_INT64 ||
		g_strcmp0(string_member(evidence, "connection"), self->connection_uuid))
		return refuse(error, "Retained collection evidence belongs to another reservation");
	*checkout_id = json_node_get_int(attempt_node);
	checkout = owned_get(self, VENTURE_TYPE_STRIPE_CHECKOUT, *checkout_id, error);
	if (!checkout) return FALSE;
	*invoice_id = stripe_number(checkout, "invoice-id");
	g_object_get(checkout, "expected", &expected, "provider-invoice-id", &provider_id, "collection-phase", &phase, NULL);
	authorization = owned_get(self, VENTURE_TYPE_STRIPE_AUTHORIZATION, stripe_number(checkout, "authorization-id"), error);
	if (!authorization || !expected || !to_stripe_amount(expected, &amount, error)) return FALSE;
	g_object_get(authorization, "customer-id", &customer, "payment-method-id", &method, NULL);
	if (g_strcmp0(string_member(evidence, "invoice"), provider_id) ||
		g_strcmp0(string_member(evidence, "customer"), customer) || g_strcmp0(string_member(evidence, "payment_method"), method) ||
		g_strcmp0(string_member(evidence, "currency"), venture_money_get_currency(expected)) ||
		!stripe_json_integer(evidence, "amount", amount) ||
		!stripe_identifier(string_member(evidence, "payment_intent"), "pi_") ||
		(g_strcmp0(string_member(evidence, "outcome"), "succeeded") && g_strcmp0(string_member(evidence, "outcome"), "failed")))
		return refuse(error, "Retained collection proof no longer matches the immutable reservation");
	if (stripe_number(checkout, "payment-id")) return TRUE;
	if (!g_strcmp0(string_member(evidence, "outcome"), "failed") && !stripe_flag(checkout, "active") && !g_strcmp0(phase, "void"))
		return TRUE;
	if (!g_strcmp0(string_member(evidence, "outcome"), "succeeded"))
		return stripe_settle_checkout(self, checkout, expected, string_member(evidence, "payment_intent"),
			settled_at, accept_balance_change, actor, error);
	{
		gboolean customer_action = !g_strcmp0(string_member(evidence, "payment_status"), "requires_action");
		gboolean exhausted = stripe_number(checkout, "pay-attempt") >= 3;
		g_autoptr(GDateTime) next = customer_action || exhausted ? NULL : g_date_time_add_days(settled_at, 1);
		g_object_set(checkout, "status", "exception", "active", TRUE,
			"collection-phase", customer_action ? "requires_action" : exhausted ? "exhausted" : "retry_wait",
			"next-attempt-at", next, NULL);
		if (!venture_stripe_save_owned(self->database, checkout, actor, error) ||
			!stripe_billing_payment_state(self, *invoice_id, FALSE, actor, error)) return FALSE;
		g_object_set(record, "result", "failed", NULL);
		return stripe_event_exception(self, record, "payment_failed",
			customer_action ? "Customer authentication is required; reconcile this attempt before new hosted authorization" :
			exhausted ? "Automatic collection retries are exhausted; reconcile the failed attempt and request customer recovery" :
			"Verified collection failed; one bounded retry is eligible after the recorded delay",
			*invoice_id, *checkout_id, FALSE, error);
	}
}
