/* SPDX-License-Identifier: AGPL-3.0-or-later */
static gboolean
stripe_dispute_event(VentureStripeService *self, JsonObject *object,
	GDateTime *date, const VentureActor *actor, gint64 *invoice_id, GError **error)
{
	const gchar *id = string_member(object, "id");
	const gchar *intent = string_member(object, "payment_intent");
	const gchar *currency = string_member(object, "currency");
	const gchar *status = string_member(object, "status");
	JsonNode *amount_node = object ? json_object_get_member(object, "amount") : NULL;
	g_autofree gchar *identity = NULL;
	g_autofree gchar *prior_state = NULL;
	g_autoptr(GPtrArray) payments = NULL, disputes = NULL;
	g_autoptr(VentureMoney) amount = NULL, paid = NULL, difference = NULL;
	g_autoptr(VentureProcessorDispute) dispute = NULL;
	VentureEntity *payment;
	gint64 payment_id;
	if (!id || !g_str_has_prefix(id, "dp_") || !intent || !g_str_has_prefix(intent, "pi_") ||
		!currency || !status || !amount_node || json_node_get_value_type(amount_node) != G_TYPE_INT64 ||
		json_node_get_int(amount_node) <= 0)
		return refuse(error, "Dispute requires a payment identity and exact positive amount");
	identity = self->connection_uuid ? g_strdup_printf("stripe:%s:%s", self->connection_uuid, intent) : g_strdup(intent);
	payments = find_rows(self, VENTURE_TYPE_PAYMENT, "external-id", identity, error);
	if (!payments) return FALSE;
	if (payments->len != 1) return refuse(error, "Dispute names an unknown verified receipt");
	payment = g_ptr_array_index(payments, 0);
	payment_id = venture_entity_get_id(payment);
	g_object_get(payment, "amount", &paid, "invoice-id", invoice_id, NULL);
	if (!paid || g_ascii_strcasecmp(currency, venture_money_get_currency(paid)))
		return refuse(error, "Dispute currency differs from the verified receipt");
	amount = charge_money(json_node_get_int(amount_node), currency);
	if (!amount) return refuse(error, "Dispute amount cannot be represented exactly");
	difference = venture_money_subtract(paid, amount, error);
	if (!difference) return FALSE;
	if (venture_money_get_amount(difference) < 0) return refuse(error, "Dispute exceeds the verified receipt");
	disputes = find_rows(self, VENTURE_TYPE_PROCESSOR_DISPUTE, "provider-id", id, error);
	if (!disputes) return FALSE;
	if (disputes->len)
	{
		g_autoptr(VentureMoney) original = NULL;
		gint64 original_payment;
		dispute = g_object_ref(g_ptr_array_index(disputes, 0));
		g_object_get(dispute, "amount", &original, "payment-id", &original_payment, "status", &prior_state, NULL);
		if (!money_equal(original, amount) || original_payment != payment_id)
			return refuse(error, "Dispute identity was reused with different receipt evidence");
		if (!g_strcmp0(prior_state, "lost") || !g_strcmp0(prior_state, "won"))
		{
			if (!g_strcmp0(status, "needs_response") || !g_strcmp0(status, "under_review") ||
				!g_strcmp0(status, "warning_needs_response") || !g_strcmp0(status, "warning_under_review"))
				return TRUE;
			return !g_strcmp0(prior_state, status) || refuse(error, "Dispute contradicts its recorded final outcome");
		}
	}
	else
	{
		dispute = venture_stripe_service_open_dispute(self, id, payment_id, date, amount, actor, error);
		if (!dispute) return FALSE;
	}
	if (!g_strcmp0(status, "lost"))
		return venture_stripe_service_lose_chargeback(self, venture_entity_get_id(VENTURE_ENTITY(dispute)), date, actor, error);
	if (!g_strcmp0(status, "won"))
	{
		if (!venture_settlement_service_refresh_invoice(venture_settlement_service_get(self->database), *invoice_id, actor, error))
			return FALSE;
		g_object_set(dispute, "status", "won", "closed-at", date, NULL);
		return venture_stripe_save_owned(self->database, VENTURE_ENTITY(dispute), actor, error);
	}
	if (!g_strcmp0(status, "needs_response") || !g_strcmp0(status, "under_review") ||
		!g_strcmp0(status, "warning_needs_response") || !g_strcmp0(status, "warning_under_review"))
		return TRUE;
	return refuse(error, "Unknown dispute outcome requires processor reconciliation");
}
