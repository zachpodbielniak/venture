/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* A full refund of one settled card payment, made once. An earlier refund for
 * the same payment intent is looked for first, so a lost provider answer is
 * recovered instead of repeated; the idempotency key covers a retry inside the
 * provider's own window. The refund is booked against the payment's receipt
 * once, keyed by the provider's refund id. */
gboolean
venture_stripe_service_refund_payment(VentureStripeService *self, gint64 payment_id, const gchar *key,
	gchar **refund_id, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) payment = NULL;
	g_autoptr(VentureMoney) amount = NULL;
	g_autoptr(StripeRequest) lookup = NULL;
	g_autoptr(StripeResource) found = NULL, made = NULL;
	g_autoptr(JsonObject) list = NULL, remote = NULL;
	g_autoptr(GPtrArray) booked = NULL, allocations = NULL;
	g_autoptr(VentureRefund) refund = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	g_autofree gchar *intent = NULL, *currency = NULL, *id = NULL, *reference = NULL;
	gint64 cents = 0, customer = 0;
	JsonArray *refunds;
	guint i;
	g_return_val_if_fail(key && *key, FALSE);
	payment = owned_get(self, VENTURE_TYPE_PAYMENT, payment_id, error);
	if (!payment) return FALSE;
	g_object_get(payment, "reference", &intent, "amount", &amount, "customer-id", &customer, NULL);
	if (!stripe_identifier(intent, "pi_")) return refuse(error, "Only a payment settled through Stripe can be refunded here");
	if (!amount || !to_stripe_amount(amount, &cents, error)) return FALSE;
	currency = g_ascii_strdown(venture_money_get_currency(amount), -1);
	lookup = stripe_request_new(STRIPE_REFUND_LIST);
	lookup->id = g_strdup(intent);
	found = stripe_client_execute(self->client, lookup, NULL, error);
	if (!found) return FALSE;
	list = stripe_resource_get_json(found);
	if (!stripe_json_boolean(list, "has_more", FALSE) || !json_object_has_member(list, "data") ||
		!JSON_NODE_HOLDS_ARRAY(json_object_get_member(list, "data")))
		return refuse(error, "Refund history is incomplete; the refund remains pending");
	refunds = json_object_get_array_member(list, "data");
	for (i = 0; i < json_array_get_length(refunds); i++) {
		JsonNode *entry = json_array_get_element(refunds, i);
		JsonObject *prior = JSON_NODE_HOLDS_OBJECT(entry) ? json_node_get_object(entry) : NULL;
		const gchar *status = string_member(prior, "status");
		if (!g_strcmp0(status, "failed") || !g_strcmp0(status, "canceled")) continue;
		if (id || g_strcmp0(string_member(prior, "payment_intent"), intent) || !stripe_json_integer(prior, "amount", cents) ||
			g_strcmp0(string_member(prior, "currency"), currency) || !stripe_identifier(string_member(prior, "id"), "re_"))
			return refuse(error, "An existing refund differs from this payment");
		id = g_strdup(string_member(prior, "id"));
	}
	if (!id) {
		g_autoptr(StripeRequest) create = stripe_request_new(STRIPE_REFUND_CREATE);
		create->id = g_strdup(intent);
		g_free(create->idempotency_key); create->idempotency_key = g_strdup(key);
		made = stripe_client_execute(self->client, create, NULL, error);
		if (!made) return FALSE;
		remote = stripe_resource_get_json(made);
		if (!stripe_identifier(string_member(remote, "id"), "re_") || g_strcmp0(string_member(remote, "payment_intent"), intent) ||
			!stripe_json_integer(remote, "amount", cents) || g_strcmp0(string_member(remote, "currency"), currency))
			return refuse(error, "Refund evidence does not match the payment");
		id = g_strdup(string_member(remote, "id"));
	}
	/* Booked once: the provider's refund id is the reference. */
	reference = g_strdup_printf("stripe:%s", id);
	if (!venture_database_begin(self->database, error)) return FALSE;
	booked = find_rows(self, VENTURE_TYPE_REFUND, "reference", reference, error);
	if (!booked) goto fail;
	if (!booked->len) {
		allocations = find_id(self, VENTURE_TYPE_PAYMENT_ALLOCATION, "payment-id", payment_id, error);
		if (!allocations) goto fail;
		if (!allocations->len) { refuse(error, "A refund requires an allocated receipt"); goto fail; }
		refund = venture_refund_new();
		venture_entity_set_organization_id(VENTURE_ENTITY(refund), self->organization_id);
		g_object_set(refund, "customer-id", customer, "allocation-id", venture_entity_get_id(g_ptr_array_index(allocations, 0)),
			"amount", amount, "date", now, "reference", reference, NULL);
		if (!venture_database_save(self->database, VENTURE_ENTITY(refund), actor, error)) goto fail;
	}
	if (!venture_database_commit(self->database, error)) goto fail;
	if (refund_id) *refund_id = g_steal_pointer(&id);
	return TRUE;
fail:
	venture_database_rollback(self->database);
	return FALSE;
}
