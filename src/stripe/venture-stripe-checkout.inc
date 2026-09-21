/* SPDX-License-Identifier: AGPL-3.0-or-later */
static gboolean
checkout_pending(const gchar *state)
{
	return !g_strcmp0(state, "initiated") || !g_strcmp0(state, "open") ||
		!g_strcmp0(state, "processing") || !g_strcmp0(state, "exception");
}

/* Transport uncertainty is durable operational work even when no signed event
 * exists yet. It never grants authority to release the reserved debit. */
static gboolean
stripe_checkout_exception(VentureStripeService *self, VentureEntity *checkout,
	gboolean resolved, GError **error)
{
	g_autoptr(GPtrArray) rows = find_id(self, VENTURE_TYPE_PROCESSOR_EXCEPTION,
		"checkout-id", venture_entity_get_id(checkout), error);
	g_autoptr(VentureProcessorException) exception = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	gint64 invoice_id;
	guint i;
	if (!rows) return FALSE;
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(rows, i);
		gint64 event_id;
		g_object_get(row, "event-id", &event_id, NULL);
		if (!event_id) { exception = g_object_ref(VENTURE_PROCESSOR_EXCEPTION(row)); break; }
	}
	if (!exception && resolved) return TRUE;
	if (!exception)
	{
		exception = venture_processor_exception_new();
		venture_entity_set_organization_id(VENTURE_ENTITY(exception), self->organization_id);
		g_object_get(checkout, "invoice-id", &invoice_id, NULL);
		g_object_set(exception, "connection-id", self->connection_id, "invoice-id", invoice_id,
			"checkout-id", venture_entity_get_id(checkout), "provider-id", venture_entity_get_uuid(checkout),
			"kind", "creation_uncertain", "occurred-at", now, NULL);
	}
	g_object_set(exception, "resolved", resolved,
		"reason", resolved ? "Provider Checkout identity recovered" : "Checkout creation is uncertain; retry the same reservation or reconcile provider evidence", NULL);
	return venture_stripe_save_owned(self->database, VENTURE_ENTITY(exception), NULL, error);
}

static VentureStripeCheckout *
stripe_checkout_create(VentureStripeService *self, gint64 invoice_id, GDateTime *deadline,
	VentureStripePaymentLink *payment_link,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(GPtrArray) sessions = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureMoney) expected = NULL;
	g_autoptr(GDateTime) expires = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(StripeRequest) request = NULL;
	g_autoptr(StripeResource) resource = NULL;
	g_autoptr(VentureStripeCheckout) checkout = NULL;
	g_autofree gchar *customer_id = NULL;
	g_autofree gchar *success_url = NULL;
	g_autofree gchar *cancel_url = NULL;
	gint64 company_id, contact_id, checkout_id;
	gboolean transaction = FALSE;
	gboolean ach = FALSE;
	guint i;

	/* No remote effect may be nested in another writer's rollback scope. */
	if (!venture_database_begin_serializable(self->database, error)) return NULL;
	transaction = TRUE;
	if (!eligible(self, invoice_id, &invoice, NULL, NULL, &expected, error)) goto fail;
	if (!venture_access_policy_check_write(venture_database_get_access_policy(self->database), invoice, "write", error)) goto fail;
	if (payment_link)
	{
		g_autoptr(VentureEntity) live = venture_database_get(self->database, VENTURE_TYPE_STRIPE_PAYMENT_LINK,
			venture_entity_get_id(VENTURE_ENTITY(payment_link)), error);
		gboolean enabled;
		gint64 version;
		if (!live) goto fail;
		g_object_get(live, "enabled", &enabled, "invoice-version", &version, NULL);
		if (!enabled || venture_entity_get_version(live) != venture_entity_get_version(VENTURE_ENTITY(payment_link)) ||
			version != venture_entity_get_version(invoice))
		{
			refuse(error, "Payment capability changed before provider reservation");
			goto fail;
		}
	}

	query = venture_query_new(VENTURE_TYPE_STRIPE_CHECKOUT);
	venture_query_set_limit(query, 0);
	if (!venture_query_add_filter_int(query, "organization-id", VENTURE_FILTER_OP_EQ, self->organization_id, error) ||
		!venture_query_add_filter_int(query, "invoice-id", VENTURE_FILTER_OP_EQ, invoice_id, error)) goto fail;
	sessions = venture_database_find(self->database, query, error);
	if (!sessions) goto fail;
	for (i = 0; i < sessions->len; i++)
	{
		VentureEntity *prior = g_ptr_array_index(sessions, i);
		gint64 binding;
		g_autofree gchar *state = NULL;
		g_object_get(prior, "connection-id", &binding, "status", &state, NULL);
		if (!checkout_pending(state)) continue;
		{
			g_autofree gchar *channel = NULL;
			g_object_get(prior, "channel", &channel, NULL);
			if (!g_strcmp0(channel, "automatic"))
			{
				refuse(error, "Automatic collection is pending; reconcile it before creating hosted Checkout"); goto fail;
			}
		}

		if (binding != self->connection_id)
		{
			refuse(error, "A pending Checkout belongs to an earlier Stripe account; reconcile it before creating another");
			goto fail;
		}
		checkout = g_object_ref(VENTURE_STRIPE_CHECKOUT(prior));
		if (g_strcmp0(state, "initiated"))
		{
			g_object_get(checkout, "expires-at", &expires, NULL);
			if (deadline && (!expires || g_date_time_compare(expires, deadline) > 0))
			{
				refuse(error, "Existing Checkout exceeds this payment link lifetime");
				goto fail;
			}
			if (payment_link)
			{
				g_object_set(payment_link, "checkout-id", venture_entity_get_id(VENTURE_ENTITY(checkout)), NULL);
				if (!venture_stripe_save_owned(self->database, VENTURE_ENTITY(payment_link), actor, error)) goto fail;
			}
			if (!venture_database_commit(self->database, error)) goto fail;
			return g_steal_pointer(&checkout);
		}
		break;
	}
	if (!checkout)
	{
		gint64 amount;
		expires = deadline ? g_date_time_ref(deadline) : g_date_time_add_hours(now, 23);
		if (g_date_time_difference(expires, now) < 30 * G_TIME_SPAN_MINUTE ||
			g_date_time_difference(expires, now) > 24 * G_TIME_SPAN_HOUR)
		{
			refuse(error, "Checkout deadline must be 30 minutes through 24 hours from creation");
			goto fail;
		}
		if (!to_stripe_amount(expected, &amount, error)) goto fail;
		ach = self->ach_enabled && !g_strcmp0(venture_money_get_currency(expected), "USD") && amount >= 50;
		checkout = venture_stripe_checkout_new();
		venture_entity_set_organization_id(VENTURE_ENTITY(checkout), self->organization_id);
		g_object_set(checkout, "connection-id", self->connection_id, "invoice-id", invoice_id, "channel", "hosted",
			"session-id", "", "url", "", "status", "initiated", "active", TRUE,
			"expected", expected, "success-url", self->success_url,
			"cancel-url", self->cancel_url, "ach-enabled", ach, "expires-at", expires, NULL);
		if (!venture_stripe_save_owned(self->database, VENTURE_ENTITY(checkout), actor, error)) goto fail;
	}
	else
	{
		g_autoptr(VentureMoney) reserved = NULL;
		g_autoptr(GDateTime) created = NULL;
		g_object_get(checkout, "expected", &reserved, "created-at", &created, "expires-at", &expires, NULL);
		/* Stripe may prune idempotency keys after 24h. An old uncertain
		 * request requires reconciliation, never a blind second debit. */
		if (!money_equal(expected, reserved) || !created || !expires ||
			g_date_time_difference(expires, now) < 30 * G_TIME_SPAN_MINUTE ||
			(deadline && g_date_time_compare(expires, deadline) > 0) ||
			g_date_time_difference(now, created) >= 23 * G_TIME_SPAN_HOUR)
		{
			refuse(error, "Uncertain Checkout requires reconciliation: balance changed or retry window elapsed");
			goto fail;
		}
	}
	checkout_id = venture_entity_get_id(VENTURE_ENTITY(checkout));
	if (payment_link)
	{
		g_object_set(payment_link, "checkout-id", checkout_id, NULL);
		if (!venture_stripe_save_owned(self->database, VENTURE_ENTITY(payment_link), actor, error)) goto fail;
	}

	if (!venture_database_commit(self->database, error)) goto fail;
	transaction = FALSE;
	g_object_get(checkout, "success-url", &success_url, "cancel-url", &cancel_url, "ach-enabled", &ach, NULL);
	g_object_get(invoice, "company-id", &company_id, "contact-id", &contact_id, NULL);
	customer_id = stripe_customer(self, company_id, contact_id, actor, error);
	if (!customer_id) goto fail;
	request = stripe_request_new(STRIPE_CHECKOUT_CREATE);
	request->customer = g_steal_pointer(&customer_id);
	{
		gint64 amount;
		if (!to_stripe_amount(expected, &amount, error)) goto fail;
		request->unit_amount = (guint64)amount;
	}
	request->currency = g_ascii_strdown(venture_money_get_currency(expected), -1);
	request->product_name = g_strdup("Invoice");
	request->quantity = 1;
	request->expires_at = (guint64)g_date_time_to_unix(expires);
	request->client_reference_id = g_strdup(venture_entity_get_uuid(VENTURE_ENTITY(checkout)));
	request->success_url = g_steal_pointer(&success_url);
	request->cancel_url = g_steal_pointer(&cancel_url);
	request->payment_methods = ach ? STRIPE_CHECKOUT_CARD_AND_US_BANK : STRIPE_CHECKOUT_CARD;
	g_free(request->idempotency_key);
	request->idempotency_key = g_strdup_printf("venture-checkout-%s", venture_entity_get_uuid(VENTURE_ENTITY(checkout)));
	resource = stripe_client_execute(self->client, request, NULL, error);
	if (!resource) goto fail;
	if (!stripe_resource_get_string(resource, "id") || !stripe_resource_get_string(resource, "url"))
	{
		refuse(error, "Stripe returned no Checkout identity or URL; retain the attempt for retry");
		goto fail;
	}
	if (!venture_database_begin(self->database, error)) goto fail;
	transaction = TRUE;
	g_clear_object(&checkout);
	checkout = (VentureStripeCheckout *)venture_database_get(self->database, VENTURE_TYPE_STRIPE_CHECKOUT, checkout_id, error);
	if (!checkout) goto fail;
	{
		g_autofree gchar *stored_id = NULL;
		g_autofree gchar *state = NULL;
		g_object_get(checkout, "session-id", &stored_id, "status", &state, NULL);
		if (stored_id && *stored_id && g_strcmp0(stored_id, stripe_resource_get_string(resource, "id")))
		{
			refuse(error, "Checkout response conflicts with retained webhook identity");
			goto fail;
		}
		g_object_set(checkout, "session-id", stripe_resource_get_string(resource, "id"),
			"url", stripe_resource_get_string(resource, "url"), NULL);
		/* A signed callback may already have settled this reservation. */
		if (!g_strcmp0(state, "initiated")) g_object_set(checkout, "status", "open", NULL);
	}
	if (!venture_stripe_save_owned(self->database, VENTURE_ENTITY(checkout), actor, error) ||
		!stripe_checkout_exception(self, VENTURE_ENTITY(checkout), TRUE, error)) goto fail;
	if (!venture_database_commit(self->database, error)) goto fail;
	return g_steal_pointer(&checkout);
fail:
	if (transaction) venture_database_rollback(self->database);
	if (checkout && venture_entity_is_persisted(VENTURE_ENTITY(checkout)))
	{
		g_autofree gchar *state = NULL;
		g_object_get(checkout, "status", &state, NULL);
		if (!g_strcmp0(state, "initiated"))
			stripe_checkout_exception(self, VENTURE_ENTITY(checkout), FALSE, NULL);
	}
	return NULL;
}

VentureStripeCheckout *
venture_stripe_service_checkout(VentureStripeService *self, gint64 invoice_id,
	const VentureActor *actor, GError **error)
{
	return stripe_checkout_create(self, invoice_id, NULL, NULL, actor, error);
}
