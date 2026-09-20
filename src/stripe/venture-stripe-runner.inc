/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* A sweep advances at most one renewal and one collection per permission.
 * Commercial periods still belong exclusively to the existing billing service. */
static gboolean
stripe_collect_authorization(VentureStripeService *self, VentureEntity *authorization,
	GDateTime *now, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) subscription = NULL;
	g_autoptr(GDateTime) end = NULL;
	g_autoptr(GPtrArray) sources = NULL;
	g_autoptr(VentureBillingRequest) renewal = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_SUBSCRIPTION_EVENT);
	gint state;
	guint i;
	gint64 subscription_id = stripe_number(authorization, "subscription-id");
	if (!stripe_authorized_terms(self, authorization, error)) return FALSE;
	subscription = owned_get(self, VENTURE_TYPE_CUSTOMER_SUBSCRIPTION, subscription_id, error);
	if (!subscription) return FALSE;
	g_object_get(subscription, "status", &state, "current-period-end", &end, NULL);
	if (end && g_date_time_compare(end, now) <= 0 && (state == 0 || state == 1 ||
		(state < 4 && stripe_flag(subscription, "cancel-at-period-end"))))
	{
		renewal = venture_billing_request_new();
		venture_entity_set_organization_id(VENTURE_ENTITY(renewal), self->organization_id);
		g_object_set(renewal, "action", "renew", "subscription-id", subscription_id, "at", now, NULL);
		if (!venture_billing_service_execute(venture_billing_service_get(self->database), renewal, actor, error) ||
			!stripe_authorized_terms(self, authorization, error)) return FALSE;
	}
	venture_query_set_organization(query, self->organization_id); venture_query_set_limit(query, 0);
	if (!venture_query_add_filter_int(query, "subscription-id", VENTURE_FILTER_OP_EQ, subscription_id, error) ||
		!venture_query_add_filter_int(query, "invoice-id", VENTURE_FILTER_OP_GT, 0, error) ||
		!venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, error)) return FALSE;
	sources = venture_database_find(self->database, query, error);
	if (!sources) return FALSE;
	for (i = 0; i < sources->len; i++)
	{
		gint64 invoice_id = stripe_number(g_ptr_array_index(sources, i), "invoice-id");
		g_autoptr(VentureMoney) balance = NULL;
		g_autoptr(GPtrArray) attempts = NULL;
		g_autoptr(VentureStripeCheckout) collected = NULL;
		guint j;
		if (!invoice_id) continue;
		balance = venture_settlement_service_invoice_balance(venture_settlement_service_get(self->database), invoice_id, NULL, error);
		if (!balance) return FALSE;
		if (venture_money_get_amount(balance) <= 0)
		{
			if (!stripe_billing_payment_state(self, invoice_id, TRUE, actor, error)) return FALSE;
			continue;
		}
		attempts = find_id(self, VENTURE_TYPE_STRIPE_CHECKOUT, "invoice-id", invoice_id, error);
		if (!attempts) return FALSE;
		for (j = 0; j < attempts->len; j++)
		{
			VentureEntity *attempt = g_ptr_array_index(attempts, j);
			g_autofree gchar *phase = NULL, *channel = NULL;
			g_autoptr(GDateTime) next = NULL;
			if (!stripe_flag(attempt, "active")) continue;
			g_object_get(attempt, "collection-phase", &phase, "channel", &channel, "next-attempt-at", &next, NULL);
			if (g_strcmp0(channel, "automatic")) return TRUE;
			if (!g_strcmp0(phase, "retry_wait"))
			{
				if (!next || g_date_time_compare(now, next) < 0) return TRUE;
				collected = venture_stripe_service_retry_collection(self, venture_entity_get_id(attempt), now, actor, error);
				return collected != NULL;
			}
			if (!g_strcmp0(phase, "requires_action") || !g_strcmp0(phase, "exhausted")) return TRUE;
		}
		collected = venture_stripe_service_collect_invoice(self, invoice_id, actor, error);
		return collected != NULL;
	}
	return TRUE;
}

gint
venture_stripe_service_collect_due(VentureStripeService *self, GDateTime *now,
	guint limit, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_STRIPE_AUTHORIZATION);
	g_autoptr(GPtrArray) rows = NULL;
	g_autofree gchar *cutoff = NULL;
	guint i;
	gint processed = 0;
	g_return_val_if_fail(VENTURE_IS_STRIPE_SERVICE(self), -1);
	if (!venture_tenant_service_check_operation(venture_tenant_service_get(self->database), TRUE, error)) return -1;
	if (!stripe_finance(self, error) || !stripe_automatic_ready(self, error)) return -1;
	if (!now || limit < 1 || limit > 100 || venture_database_has_transaction(self->database))
	{
		refuse(error, "Collection sweep requires a clock, a limit from 1 to 100 and an idle transaction boundary"); return -1;
	}
	cutoff = venture_time_to_string(now);
	venture_query_set_organization(query, self->organization_id); venture_query_set_limit(query, limit);
	if (!venture_query_add_filter_int(query, "connection-id", VENTURE_FILTER_OP_EQ, self->connection_id, error) ||
		!venture_query_add_filter_int(query, "enabled", VENTURE_FILTER_OP_EQ, TRUE, error) ||
		!venture_query_add_filter_string(query, "status", VENTURE_FILTER_OP_EQ, "active", error) ||
		!venture_query_add_filter_string(query, "next-check-at", VENTURE_FILTER_OP_LTE, cutoff, error) ||
		!venture_query_add_order(query, "next-check-at", VENTURE_SORT_ASCENDING, error) ||
		!venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, error)) return -1;
	rows = venture_database_find(self->database, query, error);
	if (!rows) return -1;
	for (i = 0; i < rows->len; i++)
	{
		g_autoptr(VentureEntity) current = NULL;
		g_autoptr(GDateTime) due = NULL, next = g_date_time_add_seconds(now, 60);
		g_autoptr(GError) item_error = NULL;
		gboolean ok;
		if (!venture_database_begin_serializable(self->database, error)) return -1;
		current = owned_get(self, VENTURE_TYPE_STRIPE_AUTHORIZATION,
			venture_entity_get_id(g_ptr_array_index(rows, i)), error);
		if (!current) { venture_database_rollback(self->database); return -1; }
		g_object_get(current, "next-check-at", &due, NULL);
		if (!stripe_flag(current, "enabled") || !due || g_date_time_compare(due, now) > 0)
		{
			venture_database_rollback(self->database); continue;
		}
		/* Durable rotation prevents one unavailable account or failed invoice
		 * from starving the rest, and releases the database before provider I/O. */
		g_object_set(current, "next-check-at", next, NULL);
		if (!venture_stripe_save_owned(self->database, current, actor, error) || !venture_database_commit(self->database, error))
		{
			venture_database_rollback(self->database); return -1;
		}
		ok = stripe_collect_authorization(self, current, now, actor, &item_error);
		g_clear_object(&current);
		current = owned_get(self, VENTURE_TYPE_STRIPE_AUTHORIZATION, venture_entity_get_id(g_ptr_array_index(rows, i)), error);
		if (!current) return -1;
		/* Provider errors can contain remote data. Persist a fixed operational
		 * instruction; detailed accounting conflicts already have scoped exceptions. */
		g_object_set(current, "collection-note", ok ? "Collection check completed" :
			"Collection needs attention: inspect retained attempts, authorization terms and processor exceptions", NULL);
		if (!venture_stripe_save_owned(self->database, current, actor, error)) return -1;
		processed++;
	}
	return processed;
}
