/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Each leg keeps its provider identity before any confirmation or capture.
 * Unknown outcomes are read back, never replaced with a new payment. */
static gboolean
prepay_save(VentureStripeService *self, VentureEntity *group, JsonNode *journal,
	const VentureActor *actor, GError **error)
{
	g_autofree gchar *text = venture_json_to_string(journal, FALSE);
	g_object_set(group, "legs", text, NULL);
	return venture_stripe_save_owned(self->database, group, actor, error);
}

static StripeRequest *
prepay_request(VentureEntity *group, guint slot, const gchar *step, StripeOperation operation)
{
	StripeRequest *request = stripe_request_new(operation);
	g_free(request->idempotency_key);
	request->idempotency_key = g_strdup_printf("prepay:%s:%u:%s", venture_entity_get_uuid(group), slot, step);
	return request;
}

static VentureEntity *
prepay_find(VentureStripeService *self, gint64 subscription, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_STRIPE_PREPAYMENT);
	venture_query_set_organization(query, self->organization_id);
	venture_query_add_filter_int(query, "subscription-id", VENTURE_FILTER_OP_EQ, subscription, NULL);
	return venture_database_find_one(self->database, query, error);
}

static VentureEntity *
prepay_reserve(VentureStripeService *self, gint64 subscription, const gchar *path,
	JsonArray *shares, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) group = NULL, sub = NULL, price = NULL;
	g_autoptr(VentureMoney) annual = NULL, total = NULL;
	g_autoptr(JsonNode) journal = json_node_new(JSON_NODE_OBJECT);
	g_autoptr(GDateTime) now = venture_time_now();
	g_autofree gchar *identity = NULL, *prior_path = NULL;
	JsonArray *legs;
	gint64 units, sum = 0;
	gint interval = 0, subscription_state = 0;
	guint count = 1, i;
	if (!path || (strcmp(path, "single") && strcmp(path, "installments") && strcmp(path, "ach") && strcmp(path, "wire") && strcmp(path, "split"))) {
		refuse(error, "A prepaid payment choice is required"); return NULL;
	}
	if (!venture_database_begin_serializable(self->database, error)) return NULL;
	group = prepay_find(self, subscription, error);
	if (group) {
		g_object_get(group, "payment-path", &prior_path, NULL);
		if (stripe_number(group, "connection-id") != self->connection_id || g_strcmp0(path, prior_path)) {
			refuse(error, "Prepaid terms or connection changed"); goto fail;
		}
		if (shares) {
			g_autofree gchar *stored = NULL;
			g_autoptr(JsonNode) previous = NULL;
			JsonArray *parts;
			g_object_get(group, "legs", &stored, NULL);
			previous = venture_json_parse(stored, error); if (!previous) goto fail;
			parts = json_object_get_array_member(json_node_get_object(previous), "legs");
			if (strcmp(path, "split") || json_array_get_length(parts) != json_array_get_length(shares)) {
				refuse(error, "Reserved shares cannot change"); goto fail;
			}
			for (i = 0; i < json_array_get_length(shares); i++) {
				JsonNode *share = json_array_get_element(shares, i);
				if (!JSON_NODE_HOLDS_VALUE(share) || json_node_get_value_type(share) != G_TYPE_INT64 ||
					json_node_get_int(share) != json_object_get_int_member(json_array_get_object_element(parts, i), "amount")) {
					refuse(error, "Reserved shares cannot change"); goto fail;
				}
			}
		}
		venture_database_rollback(self->database);
		return g_steal_pointer(&group);
	}
	if (error && *error) goto fail;
	sub = owned_get(self, VENTURE_TYPE_CUSTOMER_SUBSCRIPTION, subscription, error);
	if (!sub) goto fail;
	g_object_get(sub, "status", &subscription_state, NULL);
	if (subscription_state >= 4) {
		refuse(error, "An ended subscription cannot begin payment setup"); goto fail;
	}
	g_object_get(sub, "external-id", &identity, NULL);
	price = owned_get(self, VENTURE_TYPE_PLAN_PRICE, stripe_number(sub, "plan-price-id"), error);
	if (!price) goto fail;
	g_object_get(price, "interval", &interval, NULL);
	if (!identity || !g_str_has_prefix(identity, "prepaid:") ||
		interval != 1) {
		refuse(error, "Only a reserved annual prepaid enrollment can collect"); goto fail;
	}
	g_object_get(price, "amount", &annual, NULL);
	total = annual ? venture_money_multiply_int(annual, 2, error) : NULL;
	if (!total || !to_stripe_amount(total, &units, error)) goto fail;
	if ((!strcmp(path, "ach") || !strcmp(path, "wire")) && strcmp(venture_money_get_currency(total), "USD")) {
		refuse(error, "Bank prepayment requires USD"); goto fail;
	}
	if (!strcmp(path, "ach") && !self->ach_enabled) { refuse(error, "Bank authorization is not enabled"); goto fail; }
	if (!strcmp(path, "installments")) count = 2;
	if (!strcmp(path, "split")) count = shares ? json_array_get_length(shares) : 2;
	if (count > 3 || (!strcmp(path, "split") && count < 2) || (shares && strcmp(path, "split"))) {
		refuse(error, "Split payment requires two or three positive exact shares"); goto fail;
	}
	json_node_take_object(journal, json_object_new()); legs = json_array_new();
	json_object_set_array_member(json_node_get_object(journal), "legs", legs);
	for (i = 0; i < count; i++) {
		JsonObject *leg = json_object_new();
		gint64 part = i + 1 == count ? units - sum : units / count;
		if (shares) {
			JsonNode *value = json_array_get_element(shares, i);
			if (!JSON_NODE_HOLDS_VALUE(value) || json_node_get_value_type(value) != G_TYPE_INT64) {
				json_object_unref(leg); refuse(error, "Shares must be integer charge units"); goto fail;
			}
			part = json_node_get_int(value);
		}
		if (part <= 0 || part > units - sum) {
			json_object_unref(leg); refuse(error, "Shares must be positive and sum to the prepaid total"); goto fail;
		}
		sum += part;
		json_object_set_int_member(leg, "amount", part);
		json_object_set_int_member(leg, "attempt", 0);
		json_object_set_int_member(leg, "setup_expires", g_date_time_to_unix(now) + 23 * 60 * 60);
		json_array_add_object_element(legs, leg);
	}
	if (sum != units) { refuse(error, "Shares must sum to the prepaid total"); goto fail; }
	group = g_object_new(VENTURE_TYPE_STRIPE_PREPAYMENT, "subscription-id", subscription,
		"company-id", stripe_number(sub, "company-id"), "connection-id", self->connection_id,
		"price-version", venture_entity_get_version(price), "payment-path", path, "total", total,
		"state", "setup", "next-attempt-at", now, NULL);
	venture_entity_set_organization_id(group, self->organization_id);
	if (!prepay_save(self, group, journal, actor, error) || !venture_database_commit(self->database, error)) goto fail;
	return g_steal_pointer(&group);
fail:
	venture_database_rollback(self->database);
	return NULL;
}

/* Setup is separate from collection. Only verified identifiers survive; no
 * expanded card/bank objects or client secrets enter the journal. */
static gboolean
prepay_setup_leg(VentureStripeService *self, VentureEntity *group, JsonNode *journal,
	guint slot, const gchar *path, const gchar *customer, const VentureActor *actor, GError **error)
{
	JsonArray *legs = json_object_get_array_member(json_node_get_object(journal), "legs");
	JsonObject *leg = json_array_get_object_element(legs, slot);
	g_autoptr(StripeRequest) request = NULL;
	g_autoptr(StripeResource) response = NULL;
	g_autoptr(JsonObject) remote = NULL;
	g_autofree gchar *reference = g_strdup_printf("%s:%u", venture_entity_get_uuid(group), slot);
	g_autoptr(VentureMoney) total = NULL, amount = NULL;
	g_autofree gchar *consent = NULL, *formatted = NULL, *currency = NULL;
	const gchar *session = string_member(leg, "session"), *method_id, *method_type;
	JsonObject *setup, *method, *mandate;
	guint i;
	if (string_member(leg, "method") || !strcmp(path, "wire")) return TRUE;
	g_object_get(group, "total", &total, NULL);
	currency = g_ascii_strdown(venture_money_get_currency(total), -1);
	amount = charge_money(json_object_get_int_member(leg, "amount"), currency);
	if (!amount) return refuse(error, "Unsupported authorization currency units");
	formatted = venture_money_to_string(amount);
	consent = !strcmp(path, "installments") ?
		g_strdup_printf("Authorize two payments of %s for the 24-month term: one at publication and one 30 days later. Failed payments may be retried with this saved method.", formatted) :
		g_strdup_printf("Authorize %s for your share of the 24-month prepaid term at publication. Failed payments may be retried with this saved method.", formatted);
	if (!session) {
		g_autofree gchar *step = NULL;
		g_autoptr(GDateTime) now = venture_time_now();
		gint64 generation = json_object_has_member(leg, "setup_attempt") ? json_object_get_int_member(leg, "setup_attempt") : 0;
		if (json_object_get_int_member(leg, "setup_expires") <= g_date_time_to_unix(now)) {
			generation++;
			json_object_set_int_member(leg, "setup_attempt", generation);
			json_object_set_int_member(leg, "setup_expires", g_date_time_to_unix(now) + 23 * 60 * 60);
			if (!prepay_save(self, group, journal, actor, error)) return FALSE;
		}
		step = g_strdup_printf("setup-%" G_GINT64_FORMAT, generation);
		request = prepay_request(group, slot, step, STRIPE_CHECKOUT_CREATE);
		request->mode = STRIPE_CHECKOUT_SETUP; request->customer = g_strdup(customer);
		request->currency = g_strdup(currency); request->success_url = g_strdup(self->success_url);
		request->cancel_url = g_strdup(self->cancel_url); request->client_reference_id = g_strdup(reference);
		request->payment_methods = !strcmp(path, "ach") ? STRIPE_CHECKOUT_US_BANK : STRIPE_CHECKOUT_CARD;
		request->consent_text = g_strdup(consent); request->expires_at = json_object_get_int_member(leg, "setup_expires");
		response = stripe_client_execute(self->client, request, NULL, error);
		if (!response) return FALSE;
		remote = stripe_resource_get_json(response);
		session = string_member(remote, "id");
		if (!stripe_identifier(session, "cs_") || !string_member(remote, "url") ||
			!g_str_has_prefix(string_member(remote, "url"), "https://checkout.stripe.com/")) return refuse(error, "Setup response is incomplete");
		json_object_set_string_member(leg, "session", session);
		json_object_set_string_member(leg, "url", string_member(remote, "url"));
		return prepay_save(self, group, journal, actor, error);
	}
	request = stripe_request_new(STRIPE_CHECKOUT_RETRIEVE); request->id = g_strdup(session);
	request->expansion = STRIPE_EXPAND_CHECKOUT_SETUP;
	response = stripe_client_execute(self->client, request, NULL, error);
	if (!response) return FALSE;
	remote = stripe_resource_get_json(response);
	if (g_strcmp0(string_member(remote, "id"), session) || g_strcmp0(string_member(remote, "customer"), customer) ||
		g_strcmp0(string_member(remote, "client_reference_id"), reference) ||
		!stripe_json_integer(remote, "expires_at", json_object_get_int_member(leg, "setup_expires")))
		return refuse(error, "Setup identity changed");
	if (!g_strcmp0(string_member(remote, "status"), "expired")) {
		gint64 generation = json_object_has_member(leg, "setup_attempt") ? json_object_get_int_member(leg, "setup_attempt") : 0;
		g_autoptr(GDateTime) now = venture_time_now();
		json_object_remove_member(leg, "session"); json_object_remove_member(leg, "url");
		json_object_set_int_member(leg, "setup_attempt", generation + 1);
		json_object_set_int_member(leg, "setup_expires", g_date_time_to_unix(now) + 23 * 60 * 60);
		if (!prepay_save(self, group, journal, actor, error)) return FALSE;
		return prepay_setup_leg(self, group, journal, slot, path, customer, actor, error);
	}
	if (g_strcmp0(string_member(remote, "status"), "complete")) return TRUE;
	setup = object_member(remote, "setup_intent"); method = object_member(setup, "payment_method"); mandate = object_member(setup, "mandate");
	method_id = string_member(method, "id"); method_type = string_member(method, "type");
	if (g_strcmp0(string_member(remote, "id"), session) || g_strcmp0(string_member(remote, "mode"), "setup") ||
		g_strcmp0(string_member(remote, "client_reference_id"), reference) || g_strcmp0(string_member(remote, "customer"), customer) ||
		g_strcmp0(string_member(object_member(object_member(remote, "custom_text"), "submit"), "message"), consent) ||
		!stripe_json_boolean(remote, "livemode", !g_strcmp0(self->environment, "live")) ||
		!stripe_json_boolean(setup, "livemode", !g_strcmp0(self->environment, "live")) ||
		!stripe_json_boolean(method, "livemode", !g_strcmp0(self->environment, "live")) ||
		g_strcmp0(string_member(setup, "status"), "succeeded") || g_strcmp0(string_member(setup, "usage"), "off_session") ||
		g_strcmp0(string_member(setup, "customer"), customer) || g_strcmp0(string_member(method, "customer"), customer) ||
		!stripe_identifier(string_member(setup, "id"), "seti_") || !stripe_identifier(method_id, "pm_") ||
		g_strcmp0(method_type, !strcmp(path, "ach") ? "us_bank_account" : "card"))
		return refuse(error, "Setup does not prove this customer's prepaid consent");
	if (!strcmp(path, "ach") && (!stripe_identifier(string_member(mandate, "id"), "mandate_") ||
		g_strcmp0(string_member(mandate, "status"), "active") || g_strcmp0(string_member(mandate, "type"), "multi_use") ||
		g_strcmp0(string_member(mandate, "payment_method"), method_id))) return refuse(error, "A reusable bank mandate is required");
	for (i = 0; !strcmp(path, "split") && i < json_array_get_length(legs); i++)
		if (i != slot && !g_strcmp0(string_member(json_array_get_object_element(legs, i), "method"), method_id))
			return refuse(error, "Each split share must use a different card");
	json_object_set_string_member(leg, "method", method_id);
	json_object_set_string_member(leg, "setup", string_member(setup, "id"));
	if (!strcmp(path, "ach")) json_object_set_string_member(leg, "mandate", string_member(mandate, "id"));
	json_object_remove_member(leg, "url");
	if (!strcmp(path, "installments")) json_object_set_string_member(json_array_get_object_element(legs, 1), "method", method_id);
	return prepay_save(self, group, journal, actor, error);
}

static gboolean
prepay_intent_matches(VentureStripeService *self, VentureEntity *group, guint slot,
	JsonObject *leg, JsonObject *remote, const gchar *customer, const gchar *currency, GError **error)
{
	g_autofree gchar *correlation = g_strdup_printf("%s:%u", venture_entity_get_uuid(group), slot);
	g_autofree gchar *path = NULL;
	const gchar *method = string_member(remote, "payment_method"), *status = string_member(remote, "status");
	gboolean cleared_after_failure = !method && (!g_strcmp0(status, "requires_payment_method") || !g_strcmp0(status, "canceled"));
	g_object_get(group, "payment-path", &path, NULL);
	if (!stripe_identifier(string_member(remote, "id"), "pi_") ||
		g_strcmp0(string_member(remote, "customer"), customer) || g_strcmp0(string_member(remote, "currency"), currency) ||
		!stripe_json_integer(remote, "amount", json_object_get_int_member(leg, "amount")) ||
		!stripe_json_boolean(remote, "livemode", !g_strcmp0(self->environment, "live")) ||
		g_strcmp0(string_member(remote, "capture_method"), !g_strcmp0(path, "split") ? "manual" : "automatic") ||
		(string_member(leg, "method") && !cleared_after_failure && g_strcmp0(method, string_member(leg, "method"))) ||
		g_strcmp0(string_member(object_member(remote, "metadata"), "prepayment"), correlation) ||
		(string_member(leg, "intent") && g_strcmp0(string_member(leg, "intent"), string_member(remote, "id"))))
		return refuse(error, "Payment evidence does not match the reserved prepaid share");
	return TRUE;
}

/* 1 settled, 0 pending/authorized, 2 definite failure, -1 unknown/error. */
static gint
prepay_leg(VentureStripeService *self, VentureEntity *group, JsonNode *journal, guint slot,
	const gchar *path, const gchar *customer, const gchar *currency, gboolean capture,
	const VentureActor *actor, GError **error)
{
	JsonObject *leg = json_array_get_object_element(json_object_get_array_member(json_node_get_object(journal), "legs"), slot);
	g_autoptr(StripeRequest) request = NULL;
	g_autoptr(StripeResource) response = NULL;
	g_autoptr(JsonObject) remote = NULL;
	const gchar *status;
	gboolean mutated = FALSE;
	if (!string_member(leg, "intent")) {
		request = prepay_request(group, slot, "create", STRIPE_INTENT_CREATE);
		request->customer = g_strdup(customer); request->currency = g_strdup(currency);
		request->unit_amount = json_object_get_int_member(leg, "amount");
		request->payment_method = g_strdup(string_member(leg, "method"));
		request->mandate = g_strdup(string_member(leg, "mandate"));
		request->payment_kind = !strcmp(path, "wire") ? STRIPE_SAVED_BANK_TRANSFER : !strcmp(path, "ach") ? STRIPE_SAVED_US_BANK : STRIPE_SAVED_CARD;
		request->manual_capture = !strcmp(path, "split");
		request->metadata_key = g_strdup("prepayment");
		request->metadata_value = g_strdup_printf("%s:%u", venture_entity_get_uuid(group), slot);
		response = stripe_client_execute(self->client, request, NULL, error);
		if (!response) return -1;
		remote = stripe_resource_get_json(response);
		if (!prepay_intent_matches(self, group, slot, leg, remote, customer, currency, error)) return -1;
		json_object_set_string_member(leg, "intent", string_member(remote, "id"));
		if (!prepay_save(self, group, journal, actor, error)) return -1;
		g_clear_pointer(&request, stripe_request_free); g_clear_pointer(&response, stripe_resource_free); g_clear_pointer(&remote, json_object_unref);
	}
	request = stripe_request_new(STRIPE_INTENT_RETRIEVE); request->id = g_strdup(string_member(leg, "intent"));
	response = stripe_client_execute(self->client, request, NULL, error);
	if (!response) return -1;
	remote = stripe_resource_get_json(response);
inspect:
	if (!prepay_intent_matches(self, group, slot, leg, remote, customer, currency, error)) return -1;
	status = string_member(remote, "status");
	if (!g_strcmp0(status, "succeeded")) {
		if (!stripe_json_integer(remote, "amount_received", json_object_get_int_member(leg, "amount"))) {
			refuse(error, "Settled amount differs from the prepaid share"); return -1;
		}
		json_object_set_boolean_member(leg, "paid", TRUE);
		return prepay_save(self, group, journal, actor, error) ? 1 : -1;
	}
	if (!g_strcmp0(status, "canceled")) return 2;
	if (!g_strcmp0(status, "requires_action")) {
		if (!strcmp(path, "wire")) {
			JsonObject *instructions = object_member(object_member(remote, "next_action"), "display_bank_transfer_instructions");
			const gchar *url = string_member(instructions, "hosted_instructions_url");
			if (url && g_str_has_prefix(url, "https://payments.stripe.com/")) json_object_set_string_member(leg, "instructions_url", url);
			if (!prepay_save(self, group, journal, actor, error)) return -1;
			return 0;
		}
		return 2;
	}
	if (!g_strcmp0(status, "processing")) return 0;
	if (!g_strcmp0(status, "requires_capture")) {
		if (strcmp(path, "split") || !stripe_json_integer(remote, "amount_capturable", json_object_get_int_member(leg, "amount"))) {
			refuse(error, "Authorization does not cover the reserved share"); return -1;
		}
		json_object_set_boolean_member(leg, "authorized", TRUE);
		if (!prepay_save(self, group, journal, actor, error)) return -1;
		if (!capture || mutated) return 0;
		g_clear_pointer(&request, stripe_request_free);
		request = prepay_request(group, slot, "capture", STRIPE_INTENT_CAPTURE);
	} else if (!g_strcmp0(status, "requires_confirmation") || !g_strcmp0(status, "requires_payment_method")) {
		g_autofree gchar *step = NULL;
		if (mutated || (!g_strcmp0(status, "requires_payment_method") &&
			json_object_has_member(leg, "confirm_started") && json_object_get_boolean_member(leg, "confirm_started"))) return 2;
		step = g_strdup_printf("confirm-%" G_GINT64_FORMAT, json_object_get_int_member(leg, "attempt"));
		g_clear_pointer(&request, stripe_request_free);
		request = prepay_request(group, slot, step, STRIPE_INTENT_CONFIRM);
		request->payment_kind = !strcmp(path, "wire") ? STRIPE_SAVED_BANK_TRANSFER : !strcmp(path, "ach") ? STRIPE_SAVED_US_BANK : STRIPE_SAVED_CARD;
		request->payment_method = g_strdup(string_member(leg, "method")); request->mandate = g_strdup(string_member(leg, "mandate"));
		json_object_set_boolean_member(leg, "confirm_started", TRUE);
		if (!prepay_save(self, group, journal, actor, error)) return -1;
	} else {
		refuse(error, "Payment is in an unsupported state; retained for reconciliation"); return -1;
	}
	request->id = g_strdup(string_member(leg, "intent"));
	g_clear_pointer(&response, stripe_resource_free); g_clear_pointer(&remote, json_object_unref);
	response = stripe_client_execute(self->client, request, NULL, error);
	if (!response) {
		if (error && *error && g_error_matches(*error, STRIPE_ERROR, STRIPE_ERROR_CARD_DECLINED)) { g_clear_error(error); return 2; }
		return -1;
	}
	remote = stripe_resource_get_json(response); mutated = TRUE;
	goto inspect;
}

static gboolean
prepay_compensate(VentureStripeService *self, VentureEntity *group, JsonNode *journal,
	const gchar *customer, const gchar *currency, const VentureActor *actor, GError **error)
{
	JsonArray *legs = json_object_get_array_member(json_node_get_object(journal), "legs");
	guint i;
	gboolean complete = TRUE;
	for (i = 0; i < json_array_get_length(legs); i++) {
		JsonObject *leg = json_array_get_object_element(legs, i);
		g_autoptr(StripeRequest) request = stripe_request_new(STRIPE_INTENT_RETRIEVE);
		g_autoptr(StripeResource) response = NULL;
		g_autoptr(JsonObject) remote = NULL;
		const gchar *status;
		if (!string_member(leg, "intent") || (json_object_has_member(leg, "reversed") && json_object_get_boolean_member(leg, "reversed"))) continue;
		request->id = g_strdup(string_member(leg, "intent"));
		response = stripe_client_execute(self->client, request, NULL, error); if (!response) return FALSE;
		remote = stripe_resource_get_json(response);
		if (!prepay_intent_matches(self, group, i, leg, remote, customer, currency, error)) return FALSE;
		status = string_member(remote, "status");
		if (!g_strcmp0(status, "canceled")) json_object_set_boolean_member(leg, "reversed", TRUE);
		else {
			gboolean refund = !g_strcmp0(status, "succeeded");
			if (refund && !string_member(leg, "refund")) {
				g_autoptr(StripeRequest) lookup = stripe_request_new(STRIPE_REFUND_LIST);
				g_autoptr(StripeResource) found = NULL;
				g_autoptr(JsonObject) list = NULL;
				JsonArray *refunds;
				guint j;
				lookup->id = g_strdup(string_member(leg, "intent"));
				found = stripe_client_execute(self->client, lookup, NULL, error); if (!found) return FALSE;
				list = stripe_resource_get_json(found);
				if (!stripe_json_boolean(list, "has_more", FALSE) || !json_object_has_member(list, "data") ||
					!JSON_NODE_HOLDS_ARRAY(json_object_get_member(list, "data")))
					return refuse(error, "Refund history is incomplete; compensation remains pending");
				refunds = json_object_get_array_member(list, "data");
				for (j = 0; j < json_array_get_length(refunds); j++) {
					JsonNode *entry = json_array_get_element(refunds, j);
					JsonObject *prior = JSON_NODE_HOLDS_OBJECT(entry) ? json_node_get_object(entry) : NULL;
					if (g_strcmp0(string_member(prior, "payment_intent"), string_member(leg, "intent")) ||
						!stripe_json_integer(prior, "amount", json_object_get_int_member(leg, "amount")) ||
						g_strcmp0(string_member(prior, "currency"), currency) || !stripe_identifier(string_member(prior, "id"), "re_"))
						return refuse(error, "Existing refund differs from the reserved compensation");
					json_object_set_string_member(leg, "refund", string_member(prior, "id"));
				}
			}
			g_clear_pointer(&request, stripe_request_free);
			request = prepay_request(group, i, refund ? "refund" : "cancel", refund ? STRIPE_REFUND_CREATE : STRIPE_INTENT_CANCEL);
			request->id = g_strdup(string_member(leg, "intent"));
			if (refund && string_member(leg, "refund")) {
				request->operation = STRIPE_REFUND_RETRIEVE;
				g_free(request->id); request->id = g_strdup(string_member(leg, "refund"));
			}
			g_clear_pointer(&response, stripe_resource_free); g_clear_pointer(&remote, json_object_unref);
			response = stripe_client_execute(self->client, request, NULL, error); if (!response) return FALSE;
			remote = stripe_resource_get_json(response);
			if (refund) {
				if (!stripe_identifier(string_member(remote, "id"), "re_") ||
					g_strcmp0(string_member(remote, "payment_intent"), string_member(leg, "intent")) ||
					!stripe_json_integer(remote, "amount", json_object_get_int_member(leg, "amount")) ||
					g_strcmp0(string_member(remote, "currency"), currency)) return refuse(error, "Refund evidence does not match the captured share");
				json_object_set_string_member(leg, "refund", string_member(remote, "id"));
				json_object_set_boolean_member(leg, "reversed", !g_strcmp0(string_member(remote, "status"), "succeeded"));
			} else {
				if (!prepay_intent_matches(self, group, i, leg, remote, customer, currency, error)) return FALSE;
				json_object_set_boolean_member(leg, "reversed", !g_strcmp0(string_member(remote, "status"), "canceled"));
			}
		}
		if (!json_object_get_boolean_member(leg, "reversed")) complete = FALSE;
		if (!prepay_save(self, group, journal, actor, error)) return FALSE;
	}
	if (complete) g_object_set(group, "state", "failed", NULL);
	return prepay_save(self, group, journal, actor, error);
}

static gboolean
prepay_invoice(VentureStripeService *self, VentureEntity *group, GDateTime *published,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureBillingRequest) instruction = venture_billing_request_new();
	g_autoptr(VentureAccountingOperation) operation = NULL;
	g_autoptr(VentureMoney) total = NULL, balance = NULL;
	g_autofree gchar *path = NULL;
	gint64 invoice;
	if (stripe_number(group, "invoice-id")) return TRUE;
	operation = venture_accounting_operation_begin(self->database, "stripe.prepay", group, NULL,
		NULL, self->organization_id, actor, error);
	if (!operation || !venture_database_begin(self->database, error)) return FALSE;
	venture_entity_set_organization_id(VENTURE_ENTITY(instruction), self->organization_id);
	g_object_get(group, "payment-path", &path, NULL);
	g_object_set(instruction, "action", !g_strcmp0(path, "installments") ? "prepay-installments" : "prepay",
		"subscription-id", stripe_number(group, "subscription-id"), "at", published, NULL);
	if (!venture_billing_service_execute(venture_billing_service_get(self->database), instruction, actor, error)) goto fail;
	invoice = stripe_number(VENTURE_ENTITY(instruction), "invoice-id");
	g_object_get(group, "total", &total, NULL);
	balance = venture_settlement_service_invoice_balance(venture_settlement_service_get(self->database), invoice, NULL, error);
	if (!balance) goto fail;
	if (!money_equal(total, balance)) { refuse(error, "Invoice additions differ from the agreed prepaid total"); goto fail; }
	g_object_set(group, "invoice-id", invoice, NULL);
	if (!venture_stripe_save_owned(self->database, group, actor, error) || !venture_database_commit(self->database, error)) goto fail;
	return venture_accounting_operation_finish(operation, error);
fail:
	venture_database_rollback(self->database); return FALSE;
}

static gboolean
prepay_balance_matches(VentureStripeService *self, VentureEntity *group, JsonArray *legs,
	const gchar *currency, GError **error)
{
	g_autoptr(VentureMoney) expected = NULL, balance = NULL;
	guint i;
	g_object_get(group, "total", &expected, NULL);
	for (i = 0; i < json_array_get_length(legs); i++) {
		JsonObject *leg = json_array_get_object_element(legs, i);
		g_autoptr(VentureMoney) paid = NULL, remaining = NULL;
		if (!json_object_has_member(leg, "payment_id")) continue;
		paid = charge_money(json_object_get_int_member(leg, "amount"), currency);
		remaining = paid ? venture_money_subtract(expected, paid, error) : NULL;
		if (!remaining) return FALSE;
		g_clear_pointer(&expected, venture_money_free); expected = g_steal_pointer(&remaining);
	}
	balance = venture_settlement_service_invoice_balance(venture_settlement_service_get(self->database),
		stripe_number(group, "invoice-id"), NULL, error);
	if (!balance) return FALSE;
	if (!money_equal(expected, balance))
		return refuse(error, "Invoice balance changed outside prepaid collection; reconcile before collecting again");
	return TRUE;
}

static gboolean
prepay_settle(VentureStripeService *self, VentureEntity *group, JsonNode *journal,
	const gchar *currency, const VentureActor *actor, GError **error)
{
	JsonArray *legs = json_object_get_array_member(json_node_get_object(journal), "legs");
	g_autoptr(VentureAccountingOperation) operation = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	guint i;
	operation = venture_accounting_operation_begin(self->database, "stripe.prepay-settle", group, NULL,
		NULL, self->organization_id, actor, error);
	if (!operation || !venture_database_begin(self->database, error)) return FALSE;
	for (i = 0; i < json_array_get_length(legs); i++) {
		JsonObject *leg = json_array_get_object_element(legs, i);
		g_autoptr(VenturePayment) payment = NULL;
		g_autoptr(VentureMoney) amount = NULL;
		g_autofree gchar *identity = NULL;
		if (!json_object_has_member(leg, "paid") || !json_object_get_boolean_member(leg, "paid") || json_object_has_member(leg, "payment_id")) continue;
		amount = charge_money(json_object_get_int_member(leg, "amount"), currency);
		if (!amount) { refuse(error, "Unsupported settled currency units"); goto fail; }
		identity = g_strdup_printf("stripe:%s:%s", self->connection_uuid, string_member(leg, "intent"));
		payment = venture_payment_new(); venture_entity_set_organization_id(VENTURE_ENTITY(payment), self->organization_id);
		g_object_set(payment, "customer-id", stripe_number(group, "company-id"), "invoice-id", stripe_number(group, "invoice-id"),
			"amount", amount, "date", now, "method", "stripe", "reference", string_member(leg, "intent"), "external-id", identity, NULL);
		if (!venture_settlement_service_apply_payment(venture_settlement_service_get(self->database), payment, NULL, actor, error)) goto fail;
		json_object_set_int_member(leg, "payment_id", venture_entity_get_id(VENTURE_ENTITY(payment)));
	}
	if (!prepay_save(self, group, journal, actor, error) || !venture_database_commit(self->database, error)) goto fail;
	return venture_accounting_operation_finish(operation, error);
fail:
	venture_database_rollback(self->database); return FALSE;
}

static JsonNode *
prepay_view(VentureEntity *group, JsonNode *journal)
{
	JsonNode *result = json_node_new(JSON_NODE_OBJECT);
	JsonObject *object = json_object_new();
	JsonArray *links = json_array_new(), *legs = json_object_get_array_member(json_node_get_object(journal), "legs");
	g_autofree gchar *state = NULL;
	guint i;
	g_object_get(group, "state", &state, NULL);
	json_node_take_object(result, object);
	json_object_set_int_member(object, "prepayment_id", venture_entity_get_id(group));
	json_object_set_int_member(object, "invoice_id", stripe_number(group, "invoice-id"));
	json_object_set_string_member(object, "state", state);
	json_object_set_array_member(object, "payment_links", links);
	for (i = 0; i < json_array_get_length(legs); i++) {
		JsonObject *leg = json_array_get_object_element(legs, i), *link;
		const gchar *url = string_member(leg, "url"), *instructions = string_member(leg, "instructions_url");
		if (!url && !instructions) continue;
		link = json_object_new(); json_object_set_int_member(link, "slot", i);
		json_object_set_string_member(link, "url", url ? url : instructions);
		json_array_add_object_element(links, link);
	}
	return result;
}

JsonNode *
venture_stripe_service_prepay(VentureStripeService *self, gint64 subscription_id,
	const gchar *payment_path, JsonArray *shares, GDateTime *published_at, const VentureActor *actor, GError **error)
{
	g_autoptr(GError) discarded = NULL;
	g_autoptr(VentureEntity) group = NULL, sub = NULL, price = NULL;
	g_autoptr(JsonNode) journal = NULL, result = NULL;
	g_autoptr(GDateTime) now = venture_time_now(), live = NULL, next = NULL, lease = NULL, due = NULL;
	g_autoptr(VentureMoney) total = NULL;
	g_autofree gchar *text = NULL, *customer = NULL, *currency = NULL, *state = NULL;
	JsonArray *legs;
	gboolean ready = TRUE, all_paid = TRUE, all_authorized = TRUE, claimed = FALSE;
	gint subscription_state = 0;
	guint i;
	if (!error) error = &discarded;
	if (!stripe_finance(self, error) || !stripe_automatic_ready(self, error)) return NULL;
	if (published_at && g_date_time_compare(published_at, now) > 0) {
		refuse(error, "Publication cannot be in the future"); return NULL;
	}
	if (venture_database_has_transaction(self->database)) { refuse(error, "Prepayment requires an idle transaction boundary"); return NULL; }
	group = prepay_reserve(self, subscription_id, payment_path, shares, actor, error);
	if (!group) return NULL;
	g_object_get(group, "legs", &text, "state", &state, "published-at", &live,
		"customer-id", &customer, "total", &total, "lease-until", &lease, "next-attempt-at", &next, NULL);
	journal = venture_json_parse(text, error); if (!journal) return NULL;
	legs = json_object_get_array_member(json_node_get_object(journal), "legs");
	if (!strcmp(state, "cancelled")) return prepay_view(group, journal);
	sub = owned_get(self, VENTURE_TYPE_CUSTOMER_SUBSCRIPTION, subscription_id, error);
	if (!sub) return NULL;
	g_object_get(sub, "status", &subscription_state, NULL);
	/* A late Setup completion is not permission to revive an unpaid trial.
	 * Check before any provider call, including reads of a saved setup. */
	if (subscription_state >= 4 && !stripe_number(group, "invoice-id")) {
		for (i = 0; i < json_array_get_length(legs); i++) {
			JsonObject *leg = json_array_get_object_element(legs, i);
			json_object_remove_member(leg, "url");
			json_object_remove_member(leg, "instructions_url");
		}
		g_object_set(group, "state", "cancelled", "lease-until", NULL, "next-attempt-at", NULL, NULL);
		if (!prepay_save(self, group, journal, actor, error)) return NULL;
		return prepay_view(group, journal);
	}
	g_clear_object(&sub);
	if (!strcmp(state, "paid") || !strcmp(state, "failed") || (lease && g_date_time_compare(lease, now) > 0)) return prepay_view(group, journal);
	/* A database lease prevents reentrant HTTP and a second process from
	 * operating on the same group. Every provider write still has a stable key. */
	if (!venture_database_begin_serializable(self->database, error)) return NULL;
	{
		g_autoptr(VentureEntity) current = prepay_find(self, subscription_id, error);
		if (!current || venture_entity_get_version(current) != venture_entity_get_version(group)) {
			venture_database_rollback(self->database);
			if (!*error) refuse(error, "Prepayment changed; retry its current state");
			return NULL;
		}
	}
	g_clear_pointer(&lease, g_date_time_unref); lease = g_date_time_add_minutes(now, 5);
	g_object_set(group, "lease-until", lease, NULL);
	if (published_at && !live) {
		live = g_date_time_ref(published_at); g_object_set(group, "published-at", live, "next-attempt-at", now, NULL);
		g_clear_pointer(&next, g_date_time_unref);
	}
	if (!venture_stripe_save_owned(self->database, group, actor, error) || !venture_database_commit(self->database, error)) {
		venture_database_rollback(self->database); return NULL;
	}
	claimed = TRUE;
	if (!customer) {
		customer = stripe_customer(self, stripe_number(group, "company-id"), 0, actor, error);
		if (!customer) goto finish;
		g_object_set(group, "customer-id", customer, NULL);
		if (!prepay_save(self, group, journal, actor, error)) goto finish;
	}
	currency = g_ascii_strdown(venture_money_get_currency(total), -1);
	if (!strcmp(state, "compensating")) {
		if (prepay_compensate(self, group, journal, customer, currency, actor, error)) result = prepay_view(group, journal);
		goto finish;
	}
	for (i = 0; i < json_array_get_length(legs); i++) {
		JsonObject *leg = json_array_get_object_element(legs, i);
		if (!strcmp(payment_path, "installments") && i == 1) continue;
		if (!prepay_setup_leg(self, group, journal, i, payment_path, customer, actor, error)) goto finish;
		if (strcmp(payment_path, "wire") && !string_member(leg, "method")) ready = FALSE;
	}
	if (!ready || !live || (next && g_date_time_compare(next, now) > 0)) { result = prepay_view(group, journal); goto finish; }
	sub = owned_get(self, VENTURE_TYPE_CUSTOMER_SUBSCRIPTION, subscription_id, error);
	if (!sub) goto finish;
	price = owned_get(self, VENTURE_TYPE_PLAN_PRICE, stripe_number(sub, "plan-price-id"), error);
	if (!price) goto finish;
	if (!stripe_number(group, "invoice-id") && venture_entity_get_version(price) != stripe_number(group, "price-version")) {
		refuse(error, "Agreed prepaid price changed before invoicing; collection is paused"); goto finish;
	}
	if (!prepay_invoice(self, group, live, actor, error)) goto finish;
	if (!prepay_balance_matches(self, group, legs, currency, error)) goto finish;
	g_object_set(group, "state", "collecting", NULL);
	for (i = 0; i < json_array_get_length(legs); i++) {
		JsonObject *leg = json_array_get_object_element(legs, i);
		gint outcome;
		if (!strcmp(payment_path, "installments") && i == 1) {
			due = g_date_time_add_days(live, 30);
			if (g_date_time_compare(now, due) < 0) { all_paid = FALSE; continue; }
		}
		outcome = prepay_leg(self, group, journal, i, payment_path, customer, currency, FALSE, actor, error);
		if (outcome < 0) goto finish;
		if (outcome == 2) {
			if (!strcmp(payment_path, "split")) {
				g_object_set(group, "state", "compensating", NULL);
				if (!prepay_save(self, group, journal, actor, error) || !prepay_compensate(self, group, journal, customer, currency, actor, error)) goto finish;
			} else {
				g_autoptr(GDateTime) retry = g_date_time_add_days(now, 1);
				json_object_set_int_member(leg, "attempt", json_object_get_int_member(leg, "attempt") + 1);
				json_object_set_boolean_member(leg, "confirm_started", FALSE);
				g_object_set(group, "state", "waiting", "next-attempt-at", retry, NULL);
			}
			result = prepay_view(group, journal); goto finish;
		}
		if (outcome != 1) all_paid = FALSE;
		if (outcome != 1 && (!json_object_has_member(leg, "authorized") || !json_object_get_boolean_member(leg, "authorized"))) all_authorized = FALSE;
	}
	if (!strcmp(payment_path, "split") && all_authorized && !all_paid) {
		all_paid = TRUE;
		for (i = 0; i < json_array_get_length(legs); i++) {
			gint outcome = prepay_leg(self, group, journal, i, payment_path, customer, currency, TRUE, actor, error);
			if (outcome < 0) goto finish;
			if (outcome == 2) {
				g_object_set(group, "state", "compensating", NULL);
				if (!prepay_save(self, group, journal, actor, error) || !prepay_compensate(self, group, journal, customer, currency, actor, error)) goto finish;
				result = prepay_view(group, journal); goto finish;
			}
			if (outcome != 1) all_paid = FALSE;
		}
	}
	if ((strcmp(payment_path, "split") || all_paid) && !prepay_settle(self, group, journal, currency, actor, error)) goto finish;
	g_object_set(group, "state", all_paid ? "paid" : "waiting", NULL);
	result = prepay_view(group, journal);
finish:
	if (claimed) {
		g_autoptr(GError) save_error = NULL;
		g_autoptr(GDateTime) retry = g_date_time_add_minutes(now, 5);
		g_autoptr(GDateTime) current_next = NULL;
		g_object_get(group, "next-attempt-at", &current_next, NULL);
		g_object_set(group, "lease-until", NULL, NULL);
		if (!current_next || g_date_time_compare(current_next, now) <= 0) g_object_set(group, "next-attempt-at", retry, NULL);
		if (!prepay_save(self, group, journal, actor, &save_error)) {
			g_clear_pointer(&result, json_node_unref);
			if (!*error) g_propagate_error(error, g_steal_pointer(&save_error));
		}
	}
	return g_steal_pointer(&result);
}
