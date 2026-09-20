/* SPDX-License-Identifier: AGPL-3.0-or-later */
VentureStripeCheckout *
venture_stripe_service_reconcile_collection(VentureStripeService *self, gint64 checkout_id,
	const gchar *provider_invoice_id, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) checkout = NULL, authorization = NULL, current = NULL;
	g_autoptr(VentureMoney) expected = NULL;
	g_autoptr(StripeRequest) request = stripe_request_new(STRIPE_INVOICE_RETRIEVE);
	g_autoptr(StripeResource) resource = NULL;
	g_autoptr(JsonObject) object = NULL;
	g_autofree gchar *retained = NULL, *channel = NULL, *customer = NULL, *method = NULL;
	const gchar *currency;
	g_return_val_if_fail(VENTURE_IS_STRIPE_SERVICE(self), NULL);
	if (!stripe_finance(self, error) || !stripe_refresh(self, TRUE, error)) return NULL;
	if (venture_database_has_transaction(self->database) || g_strcmp0(self->api_version, "2024-06-20") ||
		!stripe_identifier(provider_invoice_id, "in_"))
	{
		refuse(error, "Collection reconciliation requires an idle transaction, API 2024-06-20 and an invoice identity"); return NULL;
	}
	checkout = owned_get(self, VENTURE_TYPE_STRIPE_CHECKOUT, checkout_id, error);
	if (!checkout || !venture_access_policy_check_write(venture_database_get_access_policy(self->database), checkout, "write", error)) return NULL;
	g_object_get(checkout, "channel", &channel, "provider-invoice-id", &retained, "expected", &expected, NULL);
	if (g_strcmp0(channel, "automatic") || !expected || (retained && *retained && g_strcmp0(retained, provider_invoice_id)))
	{
		refuse(error, "Reconciliation cannot replace a retained automatic collection identity"); return NULL;
	}
	authorization = owned_get(self, VENTURE_TYPE_STRIPE_AUTHORIZATION, stripe_number(checkout, "authorization-id"), error);
	if (!authorization) return NULL;
	g_object_get(authorization, "customer-id", &customer, "payment-method-id", &method, NULL);
	request->id = g_strdup(provider_invoice_id); request->expansion = STRIPE_EXPAND_INVOICE_PAYMENT;
	resource = stripe_client_execute(self->client, request, NULL, error);
	if (!resource) return NULL;
	object = stripe_resource_get_json(resource); currency = string_member(object, "currency");
	if (g_strcmp0(string_member(object, "id"), provider_invoice_id) ||
		g_strcmp0(string_member(object_member(object, "metadata"), "venture_collection_uuid"), venture_entity_get_uuid(checkout)) ||
		g_strcmp0(string_member(object, "customer"), customer) || g_strcmp0(string_member(object, "default_payment_method"), method) ||
		g_strcmp0(string_member(object, "collection_method"), "charge_automatically") ||
		!stripe_json_boolean(object, "auto_advance", FALSE) ||
		!stripe_json_boolean(object, "livemode", !g_strcmp0(self->environment, "live")) ||
		!currency || g_ascii_strcasecmp(currency, venture_money_get_currency(expected)))
	{
		refuse(error, "Provider invoice does not carry this reservation's account, customer, method and opaque correlation"); return NULL;
	}
	if (!venture_database_begin_serializable(self->database, error)) return NULL;
	current = owned_get(self, VENTURE_TYPE_STRIPE_CHECKOUT, checkout_id, error);
	if (!current) goto fail;
	g_clear_pointer(&retained, g_free); g_object_get(current, "provider-invoice-id", &retained, NULL);
	if (retained && *retained && g_strcmp0(retained, provider_invoice_id))
	{
		refuse(error, "Another request already linked a different provider invoice"); goto fail;
	}
	if (!retained || !*retained)
	{
		/* Recovered identity is not permission to repeat an old payment. The
		 * operator can cancel with fresh proof or replay a retained signed event. */
		g_object_set(current, "provider-invoice-id", provider_invoice_id, "collection-phase", "reconciled", "status", "exception", NULL);
		if (!venture_stripe_save_owned(self->database, current, actor, error)) goto fail;
	}
	if (!stripe_checkout_exception(self, current, TRUE, error) || !venture_database_commit(self->database, error)) goto fail;
	return VENTURE_STRIPE_CHECKOUT(g_steal_pointer(&current));
fail:
	venture_database_rollback(self->database); return NULL;
}
