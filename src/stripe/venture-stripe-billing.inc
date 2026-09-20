/* SPDX-License-Identifier: AGPL-3.0-or-later */
static gboolean
stripe_billing_payment_state(VentureStripeService *self, gint64 invoice_id,
	gboolean success, const VentureActor *actor, GError **error)
{
	g_autoptr(GPtrArray) sources = NULL, invoices = NULL;
	g_autoptr(VentureEntity) subscription = NULL;
	g_autoptr(VentureBillingRequest) request = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	gint64 subscription_id;
	gint state;
	guint i;
	/* Disabling billing must not discard verified cash for retained invoices.
	 * Its next enabled sweep can derive the outstanding recovery transition. */
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "customer_subscription") == G_TYPE_INVALID) return TRUE;
	sources = find_id(self, VENTURE_TYPE_SUBSCRIPTION_EVENT, "invoice-id", invoice_id, error);
	if (!sources) return FALSE;
	if (!sources->len) return TRUE;
	if (sources->len != 1) return refuse(error, "Invoice has ambiguous subscription history");
	subscription_id = stripe_number(g_ptr_array_index(sources, 0), "subscription-id");
	subscription = owned_get(self, VENTURE_TYPE_CUSTOMER_SUBSCRIPTION, subscription_id, error);
	if (!subscription) return FALSE;
	g_object_get(subscription, "status", &state, NULL);
	if ((!success && state != 1) || (success && state != 2)) return TRUE;
	if (success)
	{
		invoices = find_id(self, VENTURE_TYPE_SUBSCRIPTION_EVENT, "subscription-id", subscription_id, error);
		if (!invoices) return FALSE;
		for (i = 0; i < invoices->len; i++)
		{
			gint64 outstanding_id = stripe_number(g_ptr_array_index(invoices, i), "invoice-id");
			g_autoptr(VentureMoney) balance = NULL;
			if (!outstanding_id) continue;
			balance = venture_settlement_service_invoice_balance(venture_settlement_service_get(self->database), outstanding_id, NULL, error);
			if (!balance) return FALSE;
			if (venture_money_get_amount(balance) > 0) return TRUE;
			{
				g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_STRIPE_CHECKOUT);
				g_autoptr(GPtrArray) pending = NULL;
				venture_query_set_organization(query, self->organization_id); venture_query_set_limit(query, 1);
				if (!venture_query_add_filter_int(query, "invoice-id", VENTURE_FILTER_OP_EQ, outstanding_id, error) ||
					!venture_query_add_filter_int(query, "active", VENTURE_FILTER_OP_EQ, TRUE, error)) return FALSE;
				pending = venture_database_find(self->database, query, error);
				if (!pending) return FALSE;
				/* Manual receipts may cover the balance while another provider
				 * attempt remains uncertain; recovery waits for reconciliation. */
				if (pending->len) return TRUE;
			}
		}
	}
	request = venture_billing_request_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(request), self->organization_id);
	g_object_set(request, "action", success ? "recover" : "mark-payment-failed",
		"subscription-id", subscription_id, "at", now, NULL);
	return venture_billing_service_execute(venture_billing_service_get(self->database), request, actor, error);
}

static gboolean
stripe_resolve_payment_failures(VentureStripeService *self, gint64 checkout_id,
	const VentureActor *actor, GError **error)
{
	g_autoptr(GPtrArray) exceptions = find_id(self, VENTURE_TYPE_PROCESSOR_EXCEPTION, "checkout-id", checkout_id, error);
	guint i;
	if (!exceptions) return FALSE;
	for (i = 0; i < exceptions->len; i++)
	{
		VentureEntity *exception = g_ptr_array_index(exceptions, i);
		g_autofree gchar *kind = NULL;
		g_object_get(exception, "kind", &kind, NULL);
		if (g_strcmp0(kind, "payment_failed") || stripe_flag(exception, "resolved")) continue;
		g_object_set(exception, "resolved", TRUE, NULL);
		if (!venture_stripe_save_owned(self->database, exception, actor, error)) return FALSE;
	}
	return TRUE;
}
