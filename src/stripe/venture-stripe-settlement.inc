/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Hosted and automatic channels post through the same verified receipt path. */
static gboolean
stripe_settle_checkout(VentureStripeService *self, VentureEntity *checkout,
	const VentureMoney *expected, const gchar *intent, GDateTime *settled_at,
	gboolean accept_balance_change, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(VenturePayment) payment = NULL;
	gint64 checkout_id = venture_entity_get_id(checkout);
	gint64 invoice_id = stripe_number(checkout, "invoice-id");
	gint64 company_id;
	if (stripe_number(checkout, "payment-id")) return TRUE;
	{
		g_autoptr(VentureQuery) active = venture_query_new(VENTURE_TYPE_STRIPE_CHECKOUT);
		g_autoptr(GPtrArray) others = NULL;
		guint i;
		venture_query_set_limit(active, 0);
		venture_query_set_organization(active, self->organization_id);
		if (!venture_query_add_filter_int(active, "invoice-id", VENTURE_FILTER_OP_EQ, invoice_id, error)) return FALSE;
		others = venture_database_find(self->database, active, error);
		if (!others) return FALSE;
		for (i = 0; i < others->len; i++)
		{
			VentureEntity *other = g_ptr_array_index(others, i);
			g_autofree gchar *state = NULL;
			g_object_get(other, "status", &state, NULL);
			if (venture_entity_get_id(other) != checkout_id && checkout_pending(state))
			{
				refuse(error, "Another attempt can still collect this invoice; reconcile the contradictory payment evidence first");
				return FALSE;
			}
		}
	}
	invoice = owned_get(self, VENTURE_TYPE_INVOICE, invoice_id, error);
	if (!invoice) return FALSE;
	{
		g_autoptr(VentureMoney) balance = venture_settlement_service_invoice_balance(
			venture_settlement_service_get(self->database), invoice_id, NULL, error);
		if (!balance) return FALSE;
		if (!accept_balance_change && !money_equal(balance, expected))
		{
			refuse(error, "Invoice balance changed while payment was pending; reconcile the verified payment before allocating it");
			return FALSE;
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
	if (!venture_settlement_service_apply_payment(venture_settlement_service_get(self->database), payment, NULL, actor, error)) return FALSE;
	g_object_set(checkout, "status", "complete", "active", FALSE, "payment-id", venture_entity_get_id(VENTURE_ENTITY(payment)), NULL);
	if (!venture_stripe_save_owned(self->database, checkout, NULL, error) ||
		!stripe_checkout_exception(self, checkout, TRUE, error)) return FALSE;
	if (!stripe_billing_payment_state(self, invoice_id, TRUE, actor, error)) return FALSE;
	return stripe_resolve_payment_failures(self, checkout_id, actor, error);
}
