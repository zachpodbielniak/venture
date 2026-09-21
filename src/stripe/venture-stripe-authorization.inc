/* SPDX-License-Identifier: AGPL-3.0-or-later */
static gint64
stripe_number(VentureEntity *entity, const gchar *field)
{
	gint64 value;
	g_object_get(entity, field, &value, NULL);
	return value;
}

static gboolean
stripe_flag(VentureEntity *entity, const gchar *field)
{
	gboolean value;
	g_object_get(entity, field, &value, NULL);
	return value;
}

static gboolean
stripe_automatic_ready(VentureStripeService *self, GError **error)
{
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "customer_subscription") == G_TYPE_INVALID ||
		venture_entity_registry_lookup(venture_entity_registry_get_default(), "stripe_authorization") == G_TYPE_INVALID)
		return refuse(error, "Automatic collection requires the billing and Stripe modules");
	if (!stripe_refresh(self, FALSE, error)) return FALSE;
	if (!self->connection_id || g_strcmp0(self->api_version, "2024-06-20"))
		return refuse(error, "Automatic collection requires an organization connection pinned to Stripe API 2024-06-20");
	return TRUE;
}

VentureStripeAuthorization *
venture_stripe_service_authorize_subscription(VentureStripeService *self,
	gint64 subscription_id, const VentureMoney *limit, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) subscription = NULL, price = NULL;
	g_autoptr(VentureStripeAuthorization) authorization = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_STRIPE_AUTHORIZATION);
	g_autoptr(GPtrArray) prior = NULL;
	g_autoptr(VentureMoney) amount = NULL, recurring = NULL, difference = NULL;
	g_autoptr(GDateTime) now = venture_time_now(), deadline = NULL;
	g_autoptr(StripeRequest) request = NULL;
	g_autoptr(StripeResource) resource = NULL;
	g_autofree gchar *customer_id = NULL, *consent = NULL, *formatted = NULL, *currency = NULL;
	gint64 seats, company_id, price_id, provider_amount;
	guint i;
	gint state;
	gboolean transaction = FALSE;
	const gchar *session, *url;
	g_return_val_if_fail(VENTURE_IS_STRIPE_SERVICE(self), NULL);
	if (!stripe_finance(self, error) || !stripe_automatic_ready(self, error)) return NULL;
	if (!limit || venture_money_get_amount(limit) <= 0 || !to_stripe_amount(limit, &provider_amount, error))
	{
		if (!error || !*error) refuse(error, "Authorization requires a positive exact per-invoice limit");
		return NULL;
	}
	if (!venture_database_begin_serializable(self->database, error)) return NULL;
	transaction = TRUE;
	subscription = owned_get(self, VENTURE_TYPE_CUSTOMER_SUBSCRIPTION, subscription_id, error);
	if (!subscription || !venture_access_policy_check_write(venture_database_get_access_policy(self->database), subscription, "write", error)) goto fail;
	g_object_get(subscription, "status", &state, "company-id", &company_id, "plan-price-id", &price_id, "seats", &seats, NULL);
	if (state > 2) { refuse(error, "Only trialing, active or past-due subscriptions can authorize collection"); goto fail; }
	price = owned_get(self, VENTURE_TYPE_PLAN_PRICE, price_id, error);
	if (!price) goto fail;
	g_object_get(price, "amount", &amount, "currency", &currency, NULL);
	recurring = amount ? venture_money_multiply_int(amount, stripe_flag(price, "per-seat") ? seats : 1, error) : NULL;
	if (!recurring) goto fail;
	difference = venture_money_subtract(limit, recurring, error);
	if (!difference) goto fail;
	if (venture_money_get_amount(difference) < 0 || g_ascii_strcasecmp(currency, venture_money_get_currency(limit)))
	{
		refuse(error, "Authorization limit must cover the recurring price in its currency"); goto fail;
	}
	venture_query_set_organization(query, self->organization_id);
	venture_query_set_limit(query, 0);
	if (!venture_query_add_filter_int(query, "subscription-id", VENTURE_FILTER_OP_EQ, subscription_id, error)) goto fail;
	prior = venture_database_find(self->database, query, error);
	if (!prior) goto fail;
	for (i = 0; i < prior->len; i++)
	{
		VentureEntity *existing = g_ptr_array_index(prior, i);
		g_autofree gchar *status = NULL;
		g_autoptr(VentureMoney) previous_limit = NULL;
		if (!stripe_flag(existing, "enabled")) continue;
		g_object_get(existing, "status", &status, "limit", &previous_limit, NULL);
		if (stripe_number(existing, "connection-id") != self->connection_id ||
			stripe_number(existing, "plan-price-id") != price_id ||
			stripe_number(existing, "seats") != seats || !money_equal(limit, previous_limit) || g_strcmp0(status, "pending"))
		{
			refuse(error, "Revoke the existing authorization before changing its terms or account"); goto fail;
		}
		authorization = g_object_ref(VENTURE_STRIPE_AUTHORIZATION(existing));
	}
	if (!authorization)
	{
		authorization = venture_stripe_authorization_new();
		venture_entity_set_organization_id(VENTURE_ENTITY(authorization), self->organization_id);
		deadline = g_date_time_add_hours(now, 23);
		formatted = venture_money_to_string(limit);
		consent = g_strdup_printf("Authorize recurring invoices for this subscription, up to %s per invoice, using the saved payment method while authorization remains enabled. Changes to the plan or seat count require new authorization.", formatted);
		g_object_set(authorization, "connection-id", self->connection_id, "subscription-id", subscription_id,
			"company-id", company_id, "plan-price-id", price_id, "price-version", venture_entity_get_version(price),
			"seats", seats, "limit", limit, "consent-text", consent, "enabled", TRUE, "status", "pending", "next-check-at", now,
			"expires-at", deadline, "success-url", self->success_url, "cancel-url", self->cancel_url,
			"ach-enabled", self->ach_enabled && !g_ascii_strcasecmp(currency, "USD"), NULL);
		if (!venture_stripe_save_owned(self->database, VENTURE_ENTITY(authorization), actor, error)) goto fail;
	}
	else g_object_get(authorization, "expires-at", &deadline, NULL);
	if (!deadline || g_date_time_difference(deadline, now) < 30 * G_TIME_SPAN_MINUTE)
	{
		refuse(error, "Hosted setup deadline expired; revoke this request and create a new authorization"); goto fail;
	}
	if (!venture_database_commit(self->database, error)) goto fail;
	transaction = FALSE;
	customer_id = stripe_customer(self, company_id, 0, actor, error);
	if (!customer_id) goto fail;
	/* Customer identity is durable before the hosted operation can deliver an
	 * early callback. Its idempotency key is shared with ordinary Checkout. */
	g_object_set(authorization, "customer-id", customer_id, NULL);
	if (!venture_stripe_save_owned(self->database, VENTURE_ENTITY(authorization), actor, error)) goto fail;
	request = stripe_request_new(STRIPE_CHECKOUT_CREATE);
	request->mode = STRIPE_CHECKOUT_SETUP;
	request->customer = g_steal_pointer(&customer_id);
	request->currency = g_ascii_strdown(currency, -1);
	request->payment_methods = stripe_flag(VENTURE_ENTITY(authorization), "ach-enabled") ? STRIPE_CHECKOUT_CARD_AND_US_BANK : STRIPE_CHECKOUT_CARD;
	request->expires_at = (guint64)g_date_time_to_unix(deadline);
	request->client_reference_id = g_strdup(venture_entity_get_uuid(VENTURE_ENTITY(authorization)));
	g_object_get(authorization, "success-url", &request->success_url, "cancel-url", &request->cancel_url, "consent-text", &request->consent_text, NULL);
	g_free(request->idempotency_key);
	request->idempotency_key = g_strdup_printf("venture-authorization-%s", venture_entity_get_uuid(VENTURE_ENTITY(authorization)));
	resource = stripe_client_execute(self->client, request, NULL, error);
	if (!resource) goto fail;
	session = stripe_resource_get_string(resource, "id"); url = stripe_resource_get_string(resource, "url");
	if (!session || !g_str_has_prefix(session, "cs_") || !url || !g_str_has_prefix(url, "https://checkout.stripe.com/"))
	{
		refuse(error, "Provider returned no valid hosted authorization session"); goto fail;
	}
	{
		g_autoptr(VentureEntity) live = owned_get(self, VENTURE_TYPE_STRIPE_AUTHORIZATION, venture_entity_get_id(VENTURE_ENTITY(authorization)), error);
		g_autofree gchar *stored = NULL;
		if (!live) goto fail;
		g_object_get(live, "session-id", &stored, NULL);
		if (stored && *stored && g_strcmp0(stored, session))
		{
			refuse(error, "Hosted authorization identity conflicts with retained evidence"); goto fail;
		}
		g_object_set(live, "session-id", session, NULL);
		if (!venture_stripe_save_owned(self->database, live, actor, error)) goto fail;
		g_object_set(live, "url", url, NULL);
		return VENTURE_STRIPE_AUTHORIZATION(g_steal_pointer(&live));
	}
fail:
	if (transaction) venture_database_rollback(self->database);
	return NULL;
}

static gboolean
stripe_identifier(const gchar *value, const gchar *prefix)
{
	const gchar *p;
	if (!value || !g_str_has_prefix(value, prefix) || strlen(value) > 255 || strlen(value) <= strlen(prefix)) return FALSE;
	for (p = value; *p; p++) if (!g_ascii_isalnum(*p) && *p != '_') return FALSE;
	return TRUE;
}

static gboolean
stripe_json_boolean(JsonObject *object, const gchar *field, gboolean expected)
{
	JsonNode *node = object ? json_object_get_member(object, field) : NULL;
	return node && json_node_get_value_type(node) == G_TYPE_BOOLEAN && json_node_get_boolean(node) == expected;
}

static gboolean
stripe_json_integer(JsonObject *object, const gchar *field, gint64 expected)
{
	JsonNode *node = object ? json_object_get_member(object, field) : NULL;
	return node && json_node_get_value_type(node) == G_TYPE_INT64 && json_node_get_int(node) == expected;
}

static gboolean
stripe_authorized_terms(VentureStripeService *self, VentureEntity *authorization, GError **error)
{
	g_autoptr(VentureEntity) subscription = owned_get(self, VENTURE_TYPE_CUSTOMER_SUBSCRIPTION,
		stripe_number(authorization, "subscription-id"), error);
	g_autoptr(VentureEntity) price = NULL;
	gint state;
	if (!subscription) return FALSE;
	g_object_get(subscription, "status", &state, NULL);
	price = owned_get(self, VENTURE_TYPE_PLAN_PRICE, stripe_number(authorization, "plan-price-id"), error);
	if (!price) return FALSE;
	/* Referenced prices are commercially immutable in the billing validator.
	 * Their permitted catalog-active edit must not cancel existing consent;
	 * the captured GObject version remains provenance, not permission state. */
	if (!stripe_flag(authorization, "enabled") || state > 2 ||
		stripe_number(subscription, "company-id") != stripe_number(authorization, "company-id") ||
		stripe_number(subscription, "plan-price-id") != stripe_number(authorization, "plan-price-id") ||
		stripe_number(subscription, "seats") != stripe_number(authorization, "seats"))
		return refuse(error, "Authorization is disabled or subscription terms changed; new hosted permission is required");
	return TRUE;
}

gboolean
venture_stripe_service_verify_authorization(VentureStripeService *self,
	gint64 authorization_id, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) authorization = NULL, current = NULL;
	g_autoptr(StripeRequest) request = stripe_request_new(STRIPE_CHECKOUT_RETRIEVE);
	g_autoptr(StripeResource) resource = NULL;
	g_autoptr(JsonObject) remote = NULL, evidence = json_object_new();
	g_autoptr(JsonNode) evidence_node = json_node_new(JSON_NODE_OBJECT);
	g_autoptr(GDateTime) deadline = NULL, now = venture_time_now();
	g_autofree gchar *customer = NULL, *consent = NULL, *digest = NULL, *serialized = NULL;
	JsonObject *setup, *method, *mandate;
	const gchar *method_id, *method_type, *setup_id;
	gint64 config_version;
	g_return_val_if_fail(VENTURE_IS_STRIPE_SERVICE(self), FALSE);
	if (!stripe_finance(self, error) || !stripe_automatic_ready(self, error)) return FALSE;
	if (venture_database_has_transaction(self->database))
		return refuse(error, "Provider authorization verification requires an idle transaction boundary");
	authorization = owned_get(self, VENTURE_TYPE_STRIPE_AUTHORIZATION, authorization_id, error);
	if (!authorization || !stripe_authorized_terms(self, authorization, error) ||
		!venture_access_policy_check_write(venture_database_get_access_policy(self->database), authorization, "write", error)) return FALSE;
	g_object_get(authorization, "session-id", &request->id, "customer-id", &customer,
		"consent-text", &consent, "expires-at", &deadline, NULL);
	if (!stripe_identifier(request->id, "cs_") || !stripe_identifier(customer, "cus_") || !deadline || !consent)
		return refuse(error, "Hosted setup identity is incomplete; retry creation with its retained key");
	request->expansion = STRIPE_EXPAND_CHECKOUT_SETUP;
	config_version = self->connection_version;
	resource = stripe_client_execute(self->client, request, NULL, error);
	if (!resource) return FALSE;
	remote = stripe_resource_get_json(resource);
	setup = object_member(remote, "setup_intent");
	method = object_member(setup, "payment_method");
	mandate = object_member(setup, "mandate");
	method_id = string_member(method, "id"); method_type = string_member(method, "type"); setup_id = string_member(setup, "id");
	if (g_strcmp0(string_member(remote, "id"), request->id) ||
		g_strcmp0(string_member(remote, "mode"), "setup") || g_strcmp0(string_member(remote, "status"), "complete") ||
		g_strcmp0(string_member(remote, "client_reference_id"), venture_entity_get_uuid(authorization)) ||
		!stripe_json_boolean(remote, "livemode", !g_strcmp0(self->environment, "live")) ||
		!stripe_json_integer(remote, "expires_at", g_date_time_to_unix(deadline)) ||
		g_strcmp0(string_member(remote, "customer"), customer) ||
		g_strcmp0(string_member(object_member(object_member(remote, "custom_text"), "submit"), "message"), consent) ||
		!stripe_json_boolean(setup, "livemode", !g_strcmp0(self->environment, "live")) ||
		!stripe_json_boolean(method, "livemode", !g_strcmp0(self->environment, "live")) ||
		g_strcmp0(string_member(setup, "status"), "succeeded") || g_strcmp0(string_member(setup, "usage"), "off_session") ||
		g_strcmp0(string_member(setup, "customer"), customer) || g_strcmp0(string_member(method, "customer"), customer) ||
		!stripe_identifier(setup_id, "seti_") || !stripe_identifier(method_id, "pm_") ||
		(g_strcmp0(method_type, "card") && g_strcmp0(method_type, "us_bank_account")))
		return refuse(error, "Provider Setup evidence does not match the authorized customer, terms and reusable method");
	if (!g_strcmp0(method_type, "us_bank_account") &&
		(!stripe_flag(authorization, "ach-enabled") || !stripe_identifier(string_member(mandate, "id"), "mandate_") ||
		 g_strcmp0(string_member(mandate, "status"), "active") || g_strcmp0(string_member(mandate, "type"), "multi_use") ||
		 g_strcmp0(string_member(mandate, "payment_method"), method_id)))
		return refuse(error, "Reusable bank collection requires the verified active mandate for this method");
	/* Only the identifiers and bounded contract survive. Expanded provider
	 * objects can contain bank details and client secrets and are discarded. */
	json_object_set_string_member(evidence, "session", request->id);
	json_object_set_string_member(evidence, "setup", setup_id);
	json_object_set_string_member(evidence, "customer", customer);
	json_object_set_string_member(evidence, "method", method_id);
	json_object_set_string_member(evidence, "method_type", method_type);
	json_object_set_string_member(evidence, "connection", self->connection_uuid);
	json_object_set_string_member(evidence, "api_version", "2024-06-20");
	json_object_set_int_member(evidence, "configuration_version", config_version);
	json_object_set_int_member(evidence, "verified_at", g_date_time_to_unix(now));
	digest = g_compute_checksum_for_string(G_CHECKSUM_SHA256, consent, -1);
	json_object_set_string_member(evidence, "consent_sha256", digest);
	if (mandate && string_member(mandate, "id")) json_object_set_string_member(evidence, "mandate", string_member(mandate, "id"));
	json_node_set_object(evidence_node, evidence);
	serialized = venture_json_to_string(evidence_node, FALSE);
	if (!venture_database_begin_serializable(self->database, error)) return FALSE;
	current = owned_get(self, VENTURE_TYPE_STRIPE_AUTHORIZATION, authorization_id, error);
	if (!current || !stripe_authorized_terms(self, current, error)) goto fail;
	if (venture_entity_get_version(current) != venture_entity_get_version(authorization))
	{
		refuse(error, "Authorization changed during provider verification; retry the current record"); goto fail;
	}
	{
		g_autofree gchar *status = NULL, *saved_setup = NULL, *saved_method = NULL, *saved_mandate = NULL;
		g_object_get(current, "status", &status, "setup-intent-id", &saved_setup,
			"payment-method-id", &saved_method, "mandate-id", &saved_mandate, NULL);
		if (!g_strcmp0(status, "active"))
		{
			if (g_strcmp0(saved_setup, setup_id) || g_strcmp0(saved_method, method_id) ||
				g_strcmp0(saved_mandate && *saved_mandate ? saved_mandate : NULL, string_member(mandate, "id")))
			{
				refuse(error, "Verified authorization evidence cannot be replaced"); goto fail;
			}
			/* Repeated verification confirms the original authority; it must
			 * not rewrite its first verified time or credential provenance. */
			venture_database_rollback(self->database);
			return TRUE;
		}
	}
	g_object_set(current, "setup-intent-id", setup_id, "payment-method-id", method_id,
		"mandate-id", string_member(mandate, "id"), "status", "active", "evidence", serialized, "authorized-at", now, NULL);
	if (!venture_stripe_save_owned(self->database, current, actor, error) || !venture_database_commit(self->database, error)) goto fail;
	return TRUE;
fail:
	venture_database_rollback(self->database);
	return FALSE;
}

gboolean
venture_stripe_service_revoke_authorization(VentureStripeService *self,
	gint64 authorization_id, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) authorization = NULL;
	g_return_val_if_fail(VENTURE_IS_STRIPE_SERVICE(self), FALSE);
	if (!stripe_finance(self, error)) return FALSE;
	authorization = owned_get(self, VENTURE_TYPE_STRIPE_AUTHORIZATION, authorization_id, error);
	if (!authorization || !venture_access_policy_check_write(venture_database_get_access_policy(self->database), authorization, "write", error)) return FALSE;
	g_object_set(authorization, "enabled", FALSE, "status", "revoked", NULL);
	return venture_stripe_save_owned(self->database, authorization, actor, error);
}

static gboolean
stripe_authorization_event_kind(JsonObject *event)
{
	const gchar *type = string_member(event, "type");
	JsonObject *object = object_member(object_member(event, "data"), "object");
	return ((!g_strcmp0(type, "checkout.session.completed") || !g_strcmp0(type, "checkout.session.expired")) &&
		!g_strcmp0(string_member(object, "mode"), "setup")) ||
		!g_strcmp0(type, "setup_intent.succeeded") || !g_strcmp0(type, "setup_intent.setup_failed") ||
		!g_strcmp0(type, "payment_method.detached") || !g_strcmp0(type, "mandate.updated");
}

/* Called only after the signed input is durable and before any accounting
 * transaction. Provider reads therefore cannot hold a database write lock. */
static gboolean
stripe_authorization_event(VentureStripeService *self, JsonObject *event,
	const VentureActor *actor, GError **error)
{
	const gchar *type = string_member(event, "type");
	JsonObject *object = object_member(object_member(event, "data"), "object");
	const gchar *id = string_member(object, "id");
	g_autoptr(GPtrArray) matches = NULL;
	guint i;
	if (!id) return refuse(error, "Authorization event requires a provider identity");
	if (!g_strcmp0(type, "payment_method.detached") || !g_strcmp0(type, "mandate.updated"))
	{
		if (!g_strcmp0(type, "mandate.updated") && g_strcmp0(string_member(object, "status"), "inactive")) return TRUE;
		matches = find_rows(self, VENTURE_TYPE_STRIPE_AUTHORIZATION,
			!g_strcmp0(type, "payment_method.detached") ? "payment-method-id" : "mandate-id", id, error);
		if (!matches) return FALSE;
		for (i = 0; i < matches->len; i++)
		{
			VentureEntity *authorization = g_ptr_array_index(matches, i);
			if (!stripe_flag(authorization, "enabled")) continue;
			g_object_set(authorization, "enabled", FALSE, "status", "revoked", NULL);
			if (!venture_stripe_save_owned(self->database, authorization, actor, error)) return FALSE;
		}
		return TRUE;
	}
	if (g_str_has_prefix(type, "setup_intent."))
		matches = find_rows(self, VENTURE_TYPE_STRIPE_AUTHORIZATION, "setup-intent-id", id, error);
	else
	{
		matches = find_rows(self, VENTURE_TYPE_STRIPE_AUTHORIZATION, "session-id", id, error);
		if (matches && !matches->len)
		{
			const gchar *reference = string_member(object, "client_reference_id");
			if (reference && g_uuid_string_is_valid(reference))
			{
				g_clear_pointer(&matches, g_ptr_array_unref);
				matches = find_rows(self, VENTURE_TYPE_STRIPE_AUTHORIZATION, "uuid", reference, error);
			}
		}
	}
	if (!matches) return FALSE;
	if (!matches->len) return refuse(error, "Verified setup names an unknown authorization; retry after its session is linked");
	for (i = 0; i < matches->len; i++)
	{
		VentureEntity *authorization = g_ptr_array_index(matches, i);
		g_autofree gchar *session = NULL, *setup = NULL, *status = NULL;
		const gchar *presented_setup = string_member(object, "setup_intent");
		if (!stripe_flag(authorization, "enabled")) continue;
		g_object_get(authorization, "session-id", &session, "setup-intent-id", &setup, "status", &status, NULL);
		if (g_str_has_prefix(type, "checkout.session.") && session && *session && g_strcmp0(session, id))
			return refuse(error, "Signed setup identity conflicts with retained authorization");
		if (!g_strcmp0(type, "checkout.session.expired"))
		{
			if (!g_strcmp0(status, "pending"))
			{
				g_object_set(authorization, "enabled", FALSE, "status", "revoked", NULL);
				if (!venture_stripe_save_owned(self->database, authorization, actor, error)) return FALSE;
			}
			continue;
		}
		if (!g_strcmp0(type, "checkout.session.completed"))
		{
			if ((session && *session && g_strcmp0(session, id)) ||
				(presented_setup && (!stripe_identifier(presented_setup, "seti_") ||
				 (setup && *setup && g_strcmp0(setup, presented_setup)))))
				return refuse(error, "Signed setup identity conflicts with retained authorization");
			g_object_set(authorization, "session-id", id, NULL);
			if (presented_setup) g_object_set(authorization, "setup-intent-id", presented_setup, NULL);
			if (!venture_stripe_save_owned(self->database, authorization, actor, error)) return FALSE;
		}
		if (!g_strcmp0(type, "setup_intent.setup_failed")) continue;
		if (!venture_stripe_service_verify_authorization(self, venture_entity_get_id(authorization), actor, error)) return FALSE;
	}
	return TRUE;
}
