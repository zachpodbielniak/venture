/* SPDX-License-Identifier: AGPL-3.0-or-later */
static gboolean
stripe_collection_advance(VentureStripeService *self, VentureStripeCheckout **checkout,
	const gchar *from, const gchar *to, const gchar *provider_id, gboolean requested,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) current = NULL;
	g_autofree gchar *phase = NULL, *stored = NULL;
	if (!venture_database_begin_serializable(self->database, error)) return FALSE;
	current = owned_get(self, VENTURE_TYPE_STRIPE_CHECKOUT, venture_entity_get_id(VENTURE_ENTITY(*checkout)), error);
	if (!current) goto fail;
	g_object_get(current, "collection-phase", &phase, "provider-invoice-id", &stored, NULL);
	if (provider_id && stored && *stored && g_strcmp0(provider_id, stored))
	{
		refuse(error, "Provider collection identity conflicts with its durable reservation"); goto fail;
	}
	/* Another request or an early callback may already have advanced it.
	 * Replayed remote responses must never move the reservation backwards. */
	if (!g_strcmp0(phase, from))
	{
		g_object_set(current, "collection-phase", to, NULL);
		if (provider_id) g_object_set(current, "provider-invoice-id", provider_id, NULL);
		if (requested && !stripe_number(current, "payment-id"))
			g_object_set(current, "status", "processing", "active", TRUE, NULL);
		if (!venture_stripe_save_owned(self->database, current, actor, error)) goto fail;
	}
	if (!venture_database_commit(self->database, error)) goto fail;
	g_set_object(checkout, VENTURE_STRIPE_CHECKOUT(current));
	return TRUE;
fail:
	venture_database_rollback(self->database); return FALSE;
}

static gboolean
stripe_collection_invoice_matches(JsonObject *object, const gchar *id,
	const gchar *customer, const gchar *method, const VentureMoney *expected, const gchar *correlation, GError **error)
{
	gint64 amount;
	const gchar *currency = string_member(object, "currency");
	if (!to_stripe_amount(expected, &amount, error)) return FALSE;
	if (g_strcmp0(string_member(object, "id"), id) ||
		g_strcmp0(string_member(object_member(object, "metadata"), "venture_collection_uuid"), correlation) ||
		g_strcmp0(string_member(object, "customer"), customer) ||
		g_strcmp0(string_member(object, "default_payment_method"), method) ||
		g_strcmp0(string_member(object, "collection_method"), "charge_automatically") ||
		!stripe_json_boolean(object, "auto_advance", FALSE) || !currency ||
		g_ascii_strcasecmp(currency, venture_money_get_currency(expected)) ||
		!stripe_json_integer(object, "total", amount) || !stripe_json_integer(object, "amount_due", amount) ||
		!stripe_json_integer(object, "starting_balance", 0) ||
		!stripe_json_integer(object, "pre_payment_credit_notes_amount", 0) ||
		!stripe_json_integer(object, "post_payment_credit_notes_amount", 0))
		return refuse(error, "Provider invoice differs from its exact authorized collection reservation");
	return TRUE;
}

VentureStripeCheckout *
venture_stripe_service_collect_invoice(VentureStripeService *self, gint64 invoice_id,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) invoice = NULL, authorization = NULL;
	g_autoptr(VentureStripeCheckout) checkout = NULL;
	g_autoptr(GPtrArray) sources = NULL, authorizations = NULL, attempts = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_STRIPE_CHECKOUT);
	g_autoptr(VentureMoney) expected = NULL, limit = NULL, difference = NULL;
	g_autoptr(GDateTime) now = venture_time_now(), created = NULL;
	g_autofree gchar *customer = NULL, *method = NULL, *phase = NULL, *provider_id = NULL;
	g_autoptr(StripeRequest) request = NULL;
	g_autoptr(StripeResource) resource = NULL;
	g_autoptr(JsonObject) provider_invoice = NULL;
	gint64 subscription_id, source_id, amount;
	guint i;
	gint64 attempt_number = 1;
	gboolean transaction = FALSE;
	g_return_val_if_fail(VENTURE_IS_STRIPE_SERVICE(self), NULL);
	if (!stripe_finance(self, error) || !stripe_automatic_ready(self, error)) return NULL;
	if (!venture_database_begin_serializable(self->database, error)) return NULL;
	transaction = TRUE;
	if (!eligible(self, invoice_id, &invoice, NULL, NULL, &expected, error) ||
		!venture_access_policy_check_write(venture_database_get_access_policy(self->database), invoice, "write", error)) goto fail;
	sources = find_id(self, VENTURE_TYPE_SUBSCRIPTION_EVENT, "invoice-id", invoice_id, error);
	if (!sources) goto fail;
	if (sources->len != 1) { refuse(error, "Automatic collection requires one immutable billing source event"); goto fail; }
	source_id = venture_entity_get_id(g_ptr_array_index(sources, 0));
	subscription_id = stripe_number(g_ptr_array_index(sources, 0), "subscription-id");
	authorizations = find_id(self, VENTURE_TYPE_STRIPE_AUTHORIZATION, "subscription-id", subscription_id, error);
	if (!authorizations) goto fail;
	for (i = 0; i < authorizations->len; i++)
	{
		VentureEntity *candidate = g_ptr_array_index(authorizations, i);
		g_autofree gchar *status = NULL;
		g_object_get(candidate, "status", &status, NULL);
		if (stripe_flag(candidate, "enabled") && !g_strcmp0(status, "active")) g_set_object(&authorization, candidate);
	}
	if (!authorization) { refuse(error, "Subscription requires verified reusable payment authorization"); goto fail; }
	if (!stripe_authorized_terms(self, authorization, error)) goto fail;
	if (stripe_number(authorization, "company-id") != stripe_number(invoice, "company-id") ||
		stripe_number(g_ptr_array_index(sources, 0), "to-plan-price-id") != stripe_number(authorization, "plan-price-id") ||
		stripe_number(g_ptr_array_index(sources, 0), "to-seats") != stripe_number(authorization, "seats"))
	{
		refuse(error, "Billing source does not match the customer's authorized subscription terms"); goto fail;
	}
	g_object_get(authorization, "limit", &limit, "customer-id", &customer, "payment-method-id", &method, NULL);
	difference = limit ? venture_money_subtract(limit, expected, error) : NULL;
	if (!difference) goto fail;
	if (venture_money_get_amount(difference) < 0) { refuse(error, "Invoice exceeds the customer's authorized collection limit"); goto fail; }
	if (!stripe_identifier(customer, "cus_") || !stripe_identifier(method, "pm_") || !to_stripe_amount(expected, &amount, error))
	{
		if (!error || !*error) refuse(error, "Verified authorization has no usable provider method");
		goto fail;
	}
	venture_query_set_organization(query, self->organization_id); venture_query_set_limit(query, 0);
	if (!venture_query_add_filter_int(query, "invoice-id", VENTURE_FILTER_OP_EQ, invoice_id, error)) goto fail;
	attempts = venture_database_find(self->database, query, error);
	if (!attempts) goto fail;
	for (i = 0; i < attempts->len; i++)
	{
		VentureEntity *prior = g_ptr_array_index(attempts, i);
		g_autofree gchar *channel = NULL;
		g_autoptr(VentureMoney) reserved = NULL;
		g_object_get(prior, "channel", &channel, "expected", &reserved, NULL);
		if (!g_strcmp0(channel, "automatic") && stripe_number(prior, "authorization-id") == venture_entity_get_id(authorization))
		{
			attempt_number = MAX(attempt_number, stripe_number(prior, "pay-attempt") + 1);
			if (!stripe_flag(prior, "active") && !stripe_flag(prior, "retry-allowed"))
			{
				refuse(error, "Collection was cancelled; new customer authorization is required before automatic collection resumes"); goto fail;
			}
		}
		if (!stripe_flag(prior, "active")) continue;
		if (g_strcmp0(channel, "automatic") || stripe_number(prior, "connection-id") != self->connection_id ||
			stripe_number(prior, "authorization-id") != venture_entity_get_id(authorization) || !money_equal(reserved, expected))
		{
			refuse(error, "An existing payment attempt must be reconciled before another collection"); goto fail;
		}
		checkout = g_object_ref(VENTURE_STRIPE_CHECKOUT(prior));
	}
	if (!checkout)
	{
		if (attempt_number > 3) { refuse(error, "Automatic collection has exhausted three attempts for this authorization"); goto fail; }
		checkout = venture_stripe_checkout_new();
		venture_entity_set_organization_id(VENTURE_ENTITY(checkout), self->organization_id);
		g_object_set(checkout, "connection-id", self->connection_id, "invoice-id", invoice_id,
			"authorization-id", venture_entity_get_id(authorization), "source-event-id", source_id,
			"channel", "automatic", "collection-phase", "reserved", "status", "initiated",
			"active", TRUE, "expected", expected, "pay-attempt", attempt_number, "retry-allowed", TRUE, NULL);
		if (!venture_stripe_save_owned(self->database, VENTURE_ENTITY(checkout), actor, error)) goto fail;
	}
	g_object_get(checkout, "created-at", &created, "collection-phase", &phase, NULL);
	if (g_strcmp0(phase, "requested") && (!created || g_date_time_difference(now, created) >= 23 * G_TIME_SPAN_HOUR))
	{
		refuse(error, "Collection creation exceeds its idempotency recovery window; reconcile provider evidence"); goto fail;
	}
	if (!venture_database_commit(self->database, error)) goto fail;
	transaction = FALSE;
	if (!g_strcmp0(phase, "requested")) return g_steal_pointer(&checkout);
	/* Each step is independently recoverable. Fixed fields and persisted keys
	 * survive process loss between a provider mutation and its local response. */
	for (i = 0; i < 5; i++)
	{
		const gchar *step, *next;
		g_clear_pointer(&phase, g_free); g_clear_pointer(&provider_id, g_free);
		g_object_get(checkout, "collection-phase", &phase, "provider-invoice-id", &provider_id, NULL);
		g_clear_pointer(&request, stripe_request_free); g_clear_pointer(&resource, stripe_resource_free);
		if (!g_strcmp0(phase, "requested") || stripe_number(VENTURE_ENTITY(checkout), "payment-id")) break;
		if (!g_strcmp0(phase, "reserved")) { request = stripe_request_new(STRIPE_INVOICE_CREATE); step = "create"; next = "draft"; }
		else if (!g_strcmp0(phase, "draft")) { request = stripe_request_new(STRIPE_INVOICE_ITEM_CREATE); step = "item"; next = "itemized"; }
		else if (!g_strcmp0(phase, "itemized")) { request = stripe_request_new(STRIPE_INVOICE_FINALIZE); step = "finalize"; next = "finalized"; }
		else if (!g_strcmp0(phase, "finalized") || !g_strcmp0(phase, "paying"))
		{
			g_autoptr(StripeRequest) verify = stripe_request_new(STRIPE_INVOICE_RETRIEVE);
			g_autoptr(StripeResource) verified = NULL;
			g_autoptr(VentureMoney) balance = NULL;
			verify->id = g_strdup(provider_id);
			verified = stripe_client_execute(self->client, verify, NULL, error);
			if (!verified) goto fail;
			g_clear_pointer(&provider_invoice, json_object_unref); provider_invoice = stripe_resource_get_json(verified);
			if (!stripe_collection_invoice_matches(provider_invoice, provider_id, customer, method, expected, venture_entity_get_uuid(VENTURE_ENTITY(checkout)), error) ||
				!stripe_json_integer(provider_invoice, "amount_paid", 0) || g_strcmp0(string_member(provider_invoice, "status"), "open"))
			{
				if (!error || !*error) refuse(error, "Provider invoice is not an unpaid open collection");
				goto fail;
			}
			balance = venture_settlement_service_invoice_balance(venture_settlement_service_get(self->database), invoice_id, NULL, error);
			if (!balance || !money_equal(balance, expected))
			{
				if (!error || !*error) refuse(error, "Invoice balance changed before collection; reconcile the reserved attempt");
				goto fail;
			}
			{
				g_autoptr(VentureEntity) permission = owned_get(self, VENTURE_TYPE_STRIPE_AUTHORIZATION,
					venture_entity_get_id(authorization), error);
				if (!permission || !stripe_flag(permission, "enabled") || !stripe_authorized_terms(self, permission, error))
				{
					if (!error || !*error) refuse(error, "Customer authorization was revoked before collection");
					goto fail;
				}
			}
			/* Record ambiguous payment intent before HTTP, not only after its
			 * response. A lost response must never look like an unattempted pay. */
			if (!g_strcmp0(phase, "finalized"))
			{
				if (!stripe_collection_advance(self, &checkout, "finalized", "paying", provider_id, FALSE, actor, error)) goto fail;
				g_free(phase); phase = g_strdup("paying");
			}
			request = stripe_request_new(STRIPE_INVOICE_PAY); step = "pay-1"; next = "requested";
		}
		else { refuse(error, "Collection phase requires operator reconciliation"); goto fail; }
		request->id = g_strdup(provider_id); request->customer = g_strdup(customer);
		request->payment_method = g_strdup(method); request->currency = g_ascii_strdown(venture_money_get_currency(expected), -1);
		request->metadata_key = g_strdup("venture_collection_uuid"); request->metadata_value = g_strdup(venture_entity_get_uuid(VENTURE_ENTITY(checkout)));
		request->unit_amount = (guint64)amount; request->product_name = g_strdup_printf("Venture invoice %s", venture_entity_get_uuid(invoice));
		g_free(request->idempotency_key); request->idempotency_key = g_strdup_printf("venture-collection-%s-%s", venture_entity_get_uuid(VENTURE_ENTITY(checkout)), step);
		resource = stripe_client_execute(self->client, request, NULL, error);
		if (!resource) goto fail;
		if (!g_strcmp0(phase, "reserved"))
		{
			const gchar *created_id = stripe_resource_get_string(resource, "id");
			if (!stripe_identifier(created_id, "in_")) { refuse(error, "Provider returned no collection invoice identity"); goto fail; }
			if (!stripe_collection_advance(self, &checkout, phase, next, created_id, FALSE, actor, error)) goto fail;
		}
		else if (!stripe_collection_advance(self, &checkout, phase, next, provider_id,
			!g_strcmp0(next, "requested"), actor, error)) goto fail;
	}
	if (!stripe_checkout_exception(self, VENTURE_ENTITY(checkout), TRUE, error)) goto fail;
	return g_steal_pointer(&checkout);
fail:
	if (transaction) venture_database_rollback(self->database);
	if (checkout && venture_entity_get_id(VENTURE_ENTITY(checkout)))
	{
		g_autoptr(GError) evidence_error = NULL;
		stripe_checkout_exception(self, VENTURE_ENTITY(checkout), FALSE, &evidence_error);
	}
	return NULL;
}

static gboolean
stripe_cancel_collection(VentureStripeService *self, gint64 checkout_id,
	gboolean retry_allowed, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) checkout = NULL, authorization = NULL, current = NULL;
	g_autoptr(StripeRequest) request = NULL;
	g_autoptr(StripeResource) resource = NULL;
	g_autoptr(JsonObject) invoice = NULL, evidence = json_object_new();
	g_autoptr(JsonNode) evidence_node = json_node_new(JSON_NODE_OBJECT);
	g_autoptr(VentureMoney) expected = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	g_autofree gchar *provider_id = NULL, *customer = NULL, *method = NULL, *phase = NULL, *channel = NULL, *serialized = NULL;
	g_autoptr(GError) provider_error = NULL;
	JsonObject *payment;
	const gchar *state, *outcome = "void";
	gboolean deleted = FALSE, confirmed = FALSE;
	g_return_val_if_fail(VENTURE_IS_STRIPE_SERVICE(self), FALSE);
	if (!stripe_finance(self, error) || !stripe_refresh(self, TRUE, error)) return FALSE;
	if (venture_database_has_transaction(self->database)) return refuse(error, "Collection cancellation requires an idle transaction boundary");
	if (g_strcmp0(self->api_version, "2024-06-20")) return refuse(error, "Cancellation evidence requires Stripe API 2024-06-20");
	checkout = owned_get(self, VENTURE_TYPE_STRIPE_CHECKOUT, checkout_id, error);
	if (!checkout || !venture_access_policy_check_write(venture_database_get_access_policy(self->database), checkout, "write", error)) return FALSE;
	g_object_get(checkout, "provider-invoice-id", &provider_id, "collection-phase", &phase, "channel", &channel, "expected", &expected, NULL);
	if (g_strcmp0(channel, "automatic") || stripe_number(checkout, "payment-id")) return refuse(error, "Only an unsettled automatic attempt can be cancelled");
	if (!stripe_flag(checkout, "active") && !g_strcmp0(phase, "void")) return TRUE;
	if (!stripe_identifier(provider_id, "in_")) return refuse(error, "Unknown creation outcome requires provider identity reconciliation before cancellation");
	authorization = owned_get(self, VENTURE_TYPE_STRIPE_AUTHORIZATION, stripe_number(checkout, "authorization-id"), error);
	if (!authorization) return FALSE;
	g_object_get(authorization, "customer-id", &customer, "payment-method-id", &method, NULL);
	request = stripe_request_new(STRIPE_INVOICE_RETRIEVE); request->id = g_strdup(provider_id);
	request->expansion = STRIPE_EXPAND_INVOICE_PAYMENT;
	resource = stripe_client_execute(self->client, request, NULL, &provider_error);
	if (!resource)
	{
		if (!g_strcmp0(phase, "deleting") && g_error_matches(provider_error, STRIPE_ERROR, STRIPE_ERROR_NOT_FOUND))
		{
			deleted = TRUE; confirmed = TRUE; outcome = "deleted";
		}
		else { g_propagate_error(error, g_steal_pointer(&provider_error)); return FALSE; }
	}
	if (!confirmed)
	{
		invoice = stripe_resource_get_json(resource); payment = object_member(invoice, "payment_intent");
		state = string_member(invoice, "status");
		if (g_strcmp0(string_member(invoice, "id"), provider_id) || g_strcmp0(string_member(invoice, "customer"), customer) ||
			g_strcmp0(string_member(object_member(invoice, "metadata"), "venture_collection_uuid"), venture_entity_get_uuid(checkout)) ||
			!stripe_json_boolean(invoice, "livemode", !g_strcmp0(self->environment, "live")) ||
			!stripe_json_boolean(invoice, "paid_out_of_band", FALSE) || !stripe_json_integer(invoice, "amount_paid", 0))
			return refuse(error, "Cancellation evidence does not match the unpaid reserved provider invoice");
		if (payment && (!stripe_json_integer(payment, "amount_received", 0) ||
			(g_strcmp0(string_member(payment, "status"), "requires_payment_method") &&
			 g_strcmp0(string_member(payment, "status"), "requires_action") && g_strcmp0(string_member(payment, "status"), "canceled"))))
			return refuse(error, "Provider payment is processing or settled; retain its reservation and reconcile the outcome");
		if (!g_strcmp0(state, "void")) confirmed = TRUE;
		else if (!g_strcmp0(state, "draft")) deleted = TRUE;
		else if (g_strcmp0(state, "open")) return refuse(error, "Provider invoice is not cancellable");
		else if (!stripe_collection_invoice_matches(invoice, provider_id, customer, method, expected, venture_entity_get_uuid(VENTURE_ENTITY(checkout)), error)) return FALSE;
	}
	if (!confirmed)
	{
		/* Persist cancellation intent before the external mutation so a lost
		 * draft-delete response can be verified as an authenticated absence. */
		g_object_set(checkout, "collection-phase", deleted ? "deleting" : "voiding", NULL);
		if (!venture_stripe_save_owned(self->database, checkout, actor, error)) return FALSE;
		g_clear_pointer(&request, stripe_request_free); g_clear_pointer(&resource, stripe_resource_free);
		request = stripe_request_new(deleted ? STRIPE_INVOICE_DELETE : STRIPE_INVOICE_VOID);
		request->id = g_strdup(provider_id);
		g_free(request->idempotency_key); request->idempotency_key = g_strdup_printf("venture-collection-%s-%s", venture_entity_get_uuid(checkout), deleted ? "delete" : "void");
		resource = stripe_client_execute(self->client, request, NULL, error);
		if (!resource) return FALSE;
		g_clear_pointer(&invoice, json_object_unref); invoice = stripe_resource_get_json(resource);
		if (deleted)
		{
			if (g_strcmp0(string_member(invoice, "id"), provider_id) || !stripe_json_boolean(invoice, "deleted", TRUE))
				return refuse(error, "Provider did not confirm draft deletion");
			outcome = "deleted";
		}
		else
		{
			g_clear_pointer(&request, stripe_request_free); g_clear_pointer(&resource, stripe_resource_free);
			request = stripe_request_new(STRIPE_INVOICE_RETRIEVE); request->id = g_strdup(provider_id); request->expansion = STRIPE_EXPAND_INVOICE_PAYMENT;
			resource = stripe_client_execute(self->client, request, NULL, error);
			if (!resource) return FALSE;
			g_clear_pointer(&invoice, json_object_unref); invoice = stripe_resource_get_json(resource); payment = object_member(invoice, "payment_intent");
			if (g_strcmp0(string_member(invoice, "id"), provider_id) || g_strcmp0(string_member(invoice, "status"), "void") ||
				!stripe_json_integer(invoice, "amount_paid", 0) || (payment &&
				(!stripe_json_integer(payment, "amount_received", 0) || g_strcmp0(string_member(payment, "status"), "canceled"))))
				return refuse(error, "Provider voiding has not confirmed cancellation of the pending payment");
		}
	}
	json_object_set_string_member(evidence, "invoice", provider_id); json_object_set_string_member(evidence, "outcome", outcome);
	json_object_set_string_member(evidence, "connection", self->connection_uuid);
	json_object_set_int_member(evidence, "verified_at", g_date_time_to_unix(now));
	json_object_set_int_member(evidence, "configuration_version", self->connection_version);
	json_node_set_object(evidence_node, evidence); serialized = venture_json_to_string(evidence_node, FALSE);
	if (!venture_database_begin_serializable(self->database, error)) return FALSE;
	current = owned_get(self, VENTURE_TYPE_STRIPE_CHECKOUT, checkout_id, error);
	if (!current) goto fail;
	if (stripe_number(current, "payment-id")) { refuse(error, "Settlement arrived during cancellation; preserve its payment evidence"); goto fail; }
	g_object_set(current, "status", "failed", "active", FALSE, "collection-phase", "void",
		"retry-allowed", retry_allowed, "cancellation-evidence", serialized, NULL);
	if (!venture_stripe_save_owned(self->database, current, actor, error) ||
		!stripe_checkout_exception(self, current, TRUE, error) ||
		!stripe_resolve_payment_failures(self, checkout_id, actor, error) ||
		!stripe_billing_payment_state(self, stripe_number(current, "invoice-id"), TRUE, actor, error) ||
		!venture_database_commit(self->database, error)) goto fail;
	return TRUE;
fail:
	venture_database_rollback(self->database); return FALSE;
}

gboolean
venture_stripe_service_cancel_collection(VentureStripeService *self, gint64 checkout_id,
	const VentureActor *actor, GError **error)
{
	return stripe_cancel_collection(self, checkout_id, FALSE, actor, error);
}

VentureStripeCheckout *
venture_stripe_service_retry_collection(VentureStripeService *self, gint64 checkout_id,
	GDateTime *now, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) checkout = NULL;
	g_autoptr(GDateTime) next = NULL;
	g_autofree gchar *phase = NULL;
	g_return_val_if_fail(VENTURE_IS_STRIPE_SERVICE(self), NULL);
	if (!stripe_finance(self, error) || !stripe_automatic_ready(self, error)) return NULL;
	if (!now) { refuse(error, "Retry scheduling requires an explicit clock"); return NULL; }
	checkout = owned_get(self, VENTURE_TYPE_STRIPE_CHECKOUT, checkout_id, error);
	if (!checkout) return NULL;
	g_object_get(checkout, "collection-phase", &phase, "next-attempt-at", &next, NULL);
	if ((g_strcmp0(phase, "retry_wait") && g_strcmp0(phase, "void")) ||
		!stripe_flag(checkout, "retry-allowed") || !next || g_date_time_compare(now, next) < 0 ||
		stripe_number(checkout, "pay-attempt") >= 3)
	{
		refuse(error, "This collection has no due automatic retry; preserve the delay and customer recovery requirement"); return NULL;
	}
	/* Each retry is a new invoice identity only after authenticated evidence
	 * closes the previous one. Late callbacks remain bound to their old row. */
	if (!stripe_cancel_collection(self, checkout_id, TRUE, actor, error)) return NULL;
	return venture_stripe_service_collect_invoice(self, stripe_number(checkout, "invoice-id"), actor, error);
}
