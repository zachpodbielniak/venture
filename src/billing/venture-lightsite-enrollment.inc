/* SPDX-License-Identifier: AGPL-3.0-or-later */
static VentureMoney *enrollment_money(JsonObject *object, const gchar *name, GError **error);

/* Immutable operation receipts are also the enrollment journal. Their unique
 * keys include tombstones, so removing a customer cannot reset the guarantee. */
static gchar *
enrollment_key(gint64 business, const gchar *operation)
{
	return g_strdup_printf("hosted:%" G_GINT64_FORMAT ":%s", business, operation);
}

static JsonNode *
enrollment_read(VentureDatabase *db, gint64 billing, gint64 business, const gchar *operation, GError **error)
{
	g_autofree gchar *key = enrollment_key(business, operation);
	g_autoptr(VentureEntity) receipt = find_receipt(db, billing, key, error);
	g_autofree gchar *text = NULL, *outcome = NULL;
	if (!receipt) return NULL;
	g_object_get(receipt, "result", &text, "outcome", &outcome, NULL);
	if (number(receipt, "business-id") != business || g_strcmp0(outcome, operation)) {
		refuse(error, VENTURE_ERROR_CONFLICT, "enrollment identity conflicts with an older instruction");
		return NULL;
	}
	return venture_json_parse(text, error);
}

static gboolean
enrollment_write(VentureDatabase *db, gint64 billing, gint64 business, const gchar *operation,
	JsonNode *result, const VentureActor *actor, GError **error)
{
	g_autofree gchar *key = enrollment_key(business, operation);
	g_autofree gchar *text = venture_json_to_string(result, FALSE);
	g_autofree gchar *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, text, -1);
	g_autoptr(VentureEntity) receipt = g_object_new(VENTURE_TYPE_LIGHTSITE_BILLING_RECEIPT,
		"idempotency-key", key, "request-hash", hash, "business-id", business,
		"outcome", operation, "result", text, NULL);
	venture_entity_set_organization_id(receipt, billing);
	return save_receipt(db, receipt, actor, error);
}

/* Hosted shares are minor units of the prepaid total. Stripe collects ISK
 * and UGX in two-decimal charge units, so the same integers are scaled
 * once, here, before the provider journal stores them. */
static gboolean
enrollment_charge_share(const VentureMoney *total, gint64 minor, gint64 *units, GError **error)
{
	g_autoptr(VentureMoney) piece = NULL;
	GError *scale_error = NULL;
	if (minor <= 0) {
		refuse(error, VENTURE_ERROR_VALIDATION, "shares must be positive integer minor units totaling the prepaid price");
		return FALSE;
	}
	piece = venture_money_new(minor, venture_money_get_currency(total), venture_money_get_exponent(total));
	if (!piece || !venture_stripe_charge_units(piece, units, &scale_error) || *units <= 0) {
		g_clear_error(&scale_error);
		refuse(error, VENTURE_ERROR_VALIDATION, "shares must be positive integral Stripe charge units totaling the prepaid price");
		return FALSE;
	}
	return TRUE;
}

static JsonArray *
enrollment_charge_shares(JsonArray *shares, const VentureMoney *total, GError **error)
{
	JsonArray *scaled = json_array_new();
	gint64 expected = 0, sum = 0;
	guint i;
	if (!total || !venture_stripe_charge_units(total, &expected, error)) {
		json_array_unref(scaled);
		return NULL;
	}
	for (i = 0; i < json_array_get_length(shares); i++) {
		JsonNode *share = json_array_get_element(shares, i);
		gint64 part = 0;
		if (!JSON_NODE_HOLDS_VALUE(share) || json_node_get_value_type(share) != G_TYPE_INT64 ||
			!enrollment_charge_share(total, json_node_get_int(share), &part, error) ||
			part > expected - sum) {
			json_array_unref(scaled);
			if (error && !*error)
				refuse(error, VENTURE_ERROR_VALIDATION, "shares must be positive integral Stripe charge units totaling the prepaid price");
			return NULL;
		}
		sum += part;
		json_array_add_int_element(scaled, part);
	}
	if (sum != expected) {
		json_array_unref(scaled);
		refuse(error, VENTURE_ERROR_VALIDATION, "shares must total the prepaid price");
		return NULL;
	}
	return scaled;
}

static JsonNode *
enrollment_create(VentureDatabase *database, gint64 billing, gint64 business,
	gint64 price_id, const gchar *payment_path, JsonArray *shares, gboolean *created, GError **error)
{
	g_autoptr(GError) discarded = NULL;
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(VentureEntity) company = NULL, price = NULL, existing = NULL;
	g_autoptr(VentureBillingRequest) instruction = venture_billing_request_new();
	g_autoptr(JsonNode) result = NULL;
	g_autoptr(GDateTime) now = venture_time_now(), deadline = NULL;
	g_autoptr(VentureMoney) amount = NULL, cap = NULL;
	g_autofree gchar *reference = NULL;
	JsonObject *object;
	VentureActor actor = { VENTURE_ACTOR_KIND_IMPORT, "Hosted enrollment", NULL, NULL, NULL };
	gboolean active = FALSE;
	const gchar *interval;
	if (!error) error = &discarded;
	if (created) *created = FALSE;
	if (shares && (g_strcmp0(payment_path, "split") || json_array_get_length(shares) < 2 || json_array_get_length(shares) > 3)) {
		refuse(error, VENTURE_ERROR_VALIDATION, "only split payments accept two or three shares"); return NULL;
	}
	if (payment_path && strcmp(payment_path, "single") && strcmp(payment_path, "installments") &&
		strcmp(payment_path, "ach") && strcmp(payment_path, "wire") && strcmp(payment_path, "split")) {
		refuse(error, VENTURE_ERROR_VALIDATION, "unknown prepaid payment path");
		return NULL;
	}
	if (!prepare(database, billing, NULL, error)) return NULL;
	internal = venture_access_policy_enter(venture_database_get_access_policy(database), NULL);
	if (!business_valid(database, billing, business, error)) return NULL;
	if (!venture_database_begin_serializable(database, error)) return NULL;
	{
		g_autoptr(JsonNode) cancellation = enrollment_read(database, billing, business, "cancel", error);
		if (cancellation) {
			refuse(error, VENTURE_ERROR_CONFLICT, "cancelled enrollment cannot enroll again"); goto fail;
		}
		if (error && *error) goto fail;
	}
	result = enrollment_read(database, billing, business, "enroll", error);
	if (result) {
		JsonObject *prior = json_node_get_object(result);
		const gchar *prior_path = json_object_has_member(prior, "payment_path") ?
			json_object_get_string_member(prior, "payment_path") : NULL;
		JsonArray *prior_shares = json_object_has_member(prior, "shares") ? json_object_get_array_member(prior, "shares") : NULL;
		if (json_object_get_int_member(prior, "plan_price_id") != price_id || g_strcmp0(prior_path, payment_path) ||
			((shares != NULL) != (prior_shares != NULL)) || (shares && !json_array_equal(shares, prior_shares))) {
			refuse(error, VENTURE_ERROR_CONFLICT, "business already enrolled with different terms");
			goto fail;
		}
		venture_database_rollback(database);
		return g_steal_pointer(&result);
	}
	if (error && *error) goto fail;
	price = get_record(database, VENTURE_TYPE_PLAN_PRICE, price_id, error);
	if (!price) goto fail;
	g_object_get(price, "active", &active, "amount", &amount, NULL);
	interval = enum_nick(price, "interval");
	if (venture_entity_get_organization_id(price) != billing || !active || !amount ||
		venture_money_get_amount(amount) <= 0 ||
		(g_strcmp0(interval, "month") && g_strcmp0(interval, "year"))) {
		refuse(error, VENTURE_ERROR_VALIDATION, "an active positive monthly or annual price is required");
		goto fail;
	}
	if (payment_path && (strcmp(interval, "year") || number(price, "tax-code-id") ||
		venture_plan_price_is_metered(VENTURE_PLAN_PRICE(price)))) {
		refuse(error, VENTURE_ERROR_VALIDATION, "prepayment requires an annual fixed price without tax or metered additions");
		goto fail;
	}
	cap = venture_money_multiply_rational(amount, !strcmp(interval, "year") ? 1 : 3,
		!strcmp(interval, "year") ? 4 : 1, error);
	if (!cap) goto fail;
	company = customer_company(database, billing, business, error);
	if (error && *error) goto fail;
	if (company) {
		existing = current_subscription(database, billing, venture_entity_get_id(company), error);
		if (existing || (error && *error)) {
			if (!existing) goto fail;
			refuse(error, VENTURE_ERROR_CONFLICT, "an existing subscription cannot enroll again");
			goto fail;
		}
	} else {
		g_autoptr(VentureEntity) organization = get_record(database, VENTURE_TYPE_ORGANIZATION, business, error);
		g_autofree gchar *name = NULL;
		if (!organization) goto fail;
		g_object_get(organization, "name", &name, NULL);
		reference = g_strdup_printf(LIGHTSITE_REFERENCE_PREFIX "%" G_GINT64_FORMAT, business);
		company = g_object_new(VENTURE_TYPE_COMPANY, "name", name, "external-id", reference,
			"source", "lightsite", "active", TRUE, NULL);
		venture_entity_set_organization_id(company, billing);
		if (!venture_entity_set_field_from_string(company, "kind", "customer", error) ||
			!venture_database_save(database, company, &actor, error)) goto fail;
	}
	venture_entity_set_organization_id(VENTURE_ENTITY(instruction), billing);
	g_object_set(instruction, "action", "start", "company-id", venture_entity_get_id(company),
		"plan-price-id", price_id, "seats", (gint64)1, "defer-days", (gint64)14, "at", now, NULL);
	if (payment_path) {
		g_autofree gchar *identity = g_strdup_printf("prepaid:%" G_GINT64_FORMAT, business);
		g_object_set(instruction, "external-id", identity, NULL);
	}
	if (!execute(database, instruction, &actor, error)) goto fail;
	deadline = g_date_time_add_days(now, 14);
	object = json_object_new();
	result = json_node_new(JSON_NODE_OBJECT);
	json_node_take_object(result, object);
	json_object_set_int_member(object, "organization_id", business);
	json_object_set_int_member(object, "company_id", venture_entity_get_id(company));
	json_object_set_int_member(object, "subscription_id", number(VENTURE_ENTITY(instruction), "subscription-id"));
	json_object_set_int_member(object, "plan_price_id", price_id);
	json_object_set_member(object, "guarantee_cap", money_node(cap));
	if (payment_path) {
		g_autoptr(VentureMoney) total = venture_money_multiply_rational(amount, 2, 1, error);
		if (!total) goto fail;
		json_object_set_string_member(object, "payment_path", payment_path);
		json_object_set_int_member(object, "term_months", 24);
		json_object_set_int_member(object, "price_version", venture_entity_get_version(price));
		json_object_set_member(object, "total", money_node(total));
		if (shares) {
			gint64 remaining = venture_money_get_amount(total);
			g_autoptr(JsonArray) scaled = NULL;
			guint i;
			for (i = 0; i < json_array_get_length(shares); i++) {
				JsonNode *share = json_array_get_element(shares, i);
				if (!JSON_NODE_HOLDS_VALUE(share) || json_node_get_value_type(share) != G_TYPE_INT64 ||
					json_node_get_int(share) <= 0 || json_node_get_int(share) > remaining) {
					refuse(error, VENTURE_ERROR_VALIDATION, "shares must be positive integer minor units totaling the prepaid price"); goto fail;
				}
				remaining -= json_node_get_int(share);
			}
			if (remaining) { refuse(error, VENTURE_ERROR_VALIDATION, "shares must total the prepaid price"); goto fail; }
			/* Prove the minor units survive the Stripe scale before the
			 * receipt freezes the caller's own integers. */
			scaled = enrollment_charge_shares(shares, total, error);
			if (!scaled) goto fail;
			json_object_set_array_member(object, "shares", json_array_ref(shares));
		}
	}
	set_time(object, "enrolled_at", now);
	if (!payment_path) set_time(object, "charge_due_at", deadline);
	if (!enrollment_write(database, billing, business, "enroll", result, &actor, error)) goto fail;
	if (!venture_database_commit(database, error)) return NULL;
	if (created) *created = TRUE;
	return g_steal_pointer(&result);
fail:
	venture_database_rollback(database);
	return NULL;
}

JsonNode *
venture_lightsite_billing_enroll(VentureDatabase *database, gint64 billing, gint64 business,
	gint64 price_id, gboolean *created, GError **error)
{
	return enrollment_create(database, billing, business, price_id, NULL, NULL, created, error);
}

JsonNode *
venture_lightsite_billing_prepay(VentureDatabase *database, gint64 billing, gint64 business,
	gint64 price_id, const gchar *payment_path, JsonArray *shares, gboolean *created, GError **error)
{
	if (!payment_path) {
		refuse(error, VENTURE_ERROR_VALIDATION, "a prepaid payment path is required");
		return NULL;
	}
	return enrollment_create(database, billing, business, price_id, payment_path, shares, created, error);
}

static VentureStripeService *
enrollment_provider(VentureContext *context, gint64 billing, GError **error)
{
	VentureStripeService *injected = venture_context_get_stripe_service(context);
	gint64 owner = 0;
	if (injected) {
		g_object_get(injected, "organization-id", &owner, NULL);
		if (owner == billing) return g_object_ref(injected);
	}
	return venture_stripe_service_for_organization(venture_context_get_database(context), billing, NULL, error);
}

static gboolean
enrollment_prepaid_price(VentureDatabase *db, gint64 billing, JsonNode *enrollment, GError **error)
{
	JsonObject *terms = json_node_get_object(enrollment);
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_STRIPE_PREPAYMENT);
	g_autoptr(VentureEntity) group = NULL, price = NULL;
	venture_query_set_organization(query, billing);
	venture_query_add_filter_int(query, "subscription-id", VENTURE_FILTER_OP_EQ,
		json_object_get_int_member(terms, "subscription_id"), NULL);
	group = venture_database_find_one(db, query, error);
	if (group) return TRUE;
	if (error && *error) return FALSE;
	price = get_record(db, VENTURE_TYPE_PLAN_PRICE, json_object_get_int_member(terms, "plan_price_id"), error);
	if (!price) return FALSE;
	if (venture_entity_get_organization_id(price) != billing || venture_entity_get_version(price) != json_object_get_int_member(terms, "price_version"))
		return refuse(error, VENTURE_ERROR_CONFLICT, "the agreed prepaid price changed before payment setup");
	return TRUE;
}

static VentureEntity *
enrollment_permission(VentureDatabase *db, gint64 billing, gint64 subscription, GError **error)
{
	g_autoptr(VentureQuery) query = NULL;
	if (!venture_entity_registry_is_type_enabled(venture_entity_registry_get_default(), "stripe_authorization"))
		return NULL;
	query = venture_query_new(VENTURE_TYPE_STRIPE_AUTHORIZATION);
	venture_query_set_organization(query, billing);
	venture_query_add_filter_int(query, "subscription-id", VENTURE_FILTER_OP_EQ, subscription, NULL);
	venture_query_add_filter_int(query, "enabled", VENTURE_FILTER_OP_EQ, TRUE, NULL);
	return venture_database_find_one(db, query, error);
}

JsonNode *
venture_lightsite_billing_setup(VentureContext *context, gint64 billing, gint64 business, GError **error)
{
	g_autoptr(GError) discarded = NULL;
	VentureDatabase *db = venture_context_get_database(context);
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(JsonNode) enrollment = NULL, cancellation = NULL;
	g_autoptr(VentureEntity) price = NULL, permission = NULL;
	g_autoptr(VentureStripeService) provider = NULL;
	g_autoptr(VentureStripeAuthorization) authorization = NULL;
	g_autoptr(VentureMoney) amount = NULL;
	g_autofree gchar *status = NULL, *url = NULL;
	JsonObject *object;
	g_autoptr(JsonNode) result = NULL;
	gint64 subscription;
	VentureActor actor = { VENTURE_ACTOR_KIND_IMPORT, "Hosted payment setup", NULL, NULL, NULL };
	if (!error) error = &discarded;
	if (!prepare(db, billing, NULL, error)) return NULL;
	internal = venture_access_policy_enter(venture_database_get_access_policy(db), NULL);
	if (!business_valid(db, billing, business, error)) return NULL;
	cancellation = enrollment_read(db, billing, business, "cancel", error);
	if (error && *error) return NULL;
	if (cancellation && json_object_get_boolean_member(json_node_get_object(cancellation), "no_charge")) {
		refuse(error, VENTURE_ERROR_CONFLICT, "cancelled enrollment cannot authorize payment"); return NULL;
	}
	enrollment = enrollment_read(db, billing, business, "enroll", error);
	if (!enrollment) {
		if (!error || !*error) refuse(error, VENTURE_ERROR_NOT_FOUND, "business is not enrolled");
		return NULL;
	}
	subscription = json_object_get_int_member(json_node_get_object(enrollment), "subscription_id");
	if (json_object_has_member(json_node_get_object(enrollment), "payment_path")) {
		g_autoptr(VentureMoney) prepaid_total = NULL;
		g_autoptr(JsonArray) scaled = NULL;
		JsonArray *stored = json_object_has_member(json_node_get_object(enrollment), "shares") ?
			json_object_get_array_member(json_node_get_object(enrollment), "shares") : NULL;
		if (!enrollment_prepaid_price(db, billing, enrollment, error)) return NULL;
		if (stored) {
			prepaid_total = enrollment_money(json_node_get_object(enrollment), "total", error);
			scaled = prepaid_total ? enrollment_charge_shares(stored, prepaid_total, error) : NULL;
			if (!scaled) return NULL;
		}
		provider = enrollment_provider(context, billing, error);
		if (!provider) return NULL;
		return venture_stripe_service_prepay(provider, subscription,
			json_object_get_string_member(json_node_get_object(enrollment), "payment_path"),
			scaled, NULL, &actor, error);
	}
	permission = enrollment_permission(db, billing, subscription, error);
	if (error && *error) return NULL;
	if (permission) g_object_get(permission, "status", &status, NULL);
	object = json_object_new(); result = json_node_new(JSON_NODE_OBJECT); json_node_take_object(result, object);
	if (!g_strcmp0(status, "active")) {
		json_object_set_string_member(object, "state", "card_ready");
		return g_steal_pointer(&result);
	}
	provider = enrollment_provider(context, billing, error);
	if (!provider) return NULL;
	price = get_record(db, VENTURE_TYPE_PLAN_PRICE,
		json_object_get_int_member(json_node_get_object(enrollment), "plan_price_id"), error);
	if (!price) return NULL;
	g_object_get(price, "amount", &amount, NULL);
	authorization = venture_stripe_service_authorize_subscription(provider, subscription, amount, &actor, error);
	if (!authorization) return NULL;
	g_object_get(authorization, "url", &url, NULL);
	json_object_set_string_member(object, "state", "awaiting_card");
	json_object_set_string_member(object, "url", url);
	return g_steal_pointer(&result);
}

/* Adoption of a scheduler-created first invoice uses retained billing events,
 * not the newest invoice, which might belong to a later recurring period. */
static gint64
enrollment_first_invoice(VentureDatabase *db, gint64 billing, gint64 subscription, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_SUBSCRIPTION_EVENT);
	g_autoptr(VentureEntity) event = NULL;
	venture_query_set_organization(query, billing);
	venture_query_add_filter_int(query, "subscription-id", VENTURE_FILTER_OP_EQ, subscription, NULL);
	venture_query_add_filter_int(query, "invoice-id", VENTURE_FILTER_OP_GT, 0, NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	event = venture_database_find_one(db, query, error);
	return event ? number(event, "invoice-id") : 0;
}

JsonNode *
venture_lightsite_billing_first_charge(VentureContext *context, gint64 billing, gint64 business,
	gboolean published, GError **error)
{
	g_autoptr(GError) discarded = NULL;
	VentureDatabase *db = venture_context_get_database(context);
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(VentureAccountingOperation) operation = NULL;
	g_autoptr(JsonNode) enrollment = NULL, publication = NULL, receipt = NULL, result = NULL, cancellation = NULL;
	g_autoptr(VentureEntity) permission = NULL, sub = NULL;
	g_autoptr(VentureBillingRequest) instruction = NULL;
	g_autoptr(VentureStripeService) provider = NULL;
	g_autoptr(VentureStripeCheckout) checkout = NULL;
	g_autoptr(GDateTime) now = venture_time_now(), deadline = NULL;
	g_autofree gchar *status = NULL;
	JsonObject *object;
	gint64 subscription, invoice = 0;
	VentureActor actor = { VENTURE_ACTOR_KIND_IMPORT, "Hosted first charge", NULL, NULL, NULL };
	if (!error) error = &discarded;
	if (!prepare(db, billing, NULL, error)) return NULL;
	internal = venture_access_policy_enter(venture_database_get_access_policy(db), NULL);
	if (!business_valid(db, billing, business, error)) return NULL;
	/* Bind financial consent before the serializable business transaction. */
	operation = venture_accounting_operation_begin(db, "hosted.first-charge", NULL, NULL,
		g_variant_new("(xb)", business, published), billing, &actor, error);
	if (!operation || !venture_database_begin(db, error)) return NULL;
	cancellation = enrollment_read(db, billing, business, "cancel", error);
	if (error && *error) goto fail;
	if (cancellation && json_object_get_boolean_member(json_node_get_object(cancellation), "no_charge")) {
		venture_database_rollback(db);
		return g_steal_pointer(&cancellation);
	}
	enrollment = enrollment_read(db, billing, business, "enroll", error);
	if (!enrollment) {
		if (!error || !*error) refuse(error, VENTURE_ERROR_NOT_FOUND, "business is not enrolled");
		goto fail;
	}
	subscription = json_object_get_int_member(json_node_get_object(enrollment), "subscription_id");
	publication = enrollment_read(db, billing, business, "publish", error);
	if (error && *error) goto fail;
	if (published && !publication) {
		publication = json_node_new(JSON_NODE_OBJECT);
		json_node_take_object(publication, json_object_new());
		set_time(json_node_get_object(publication), "go_live_at", now);
		if (!enrollment_write(db, billing, business, "publish", publication, &actor, error)) goto fail;
	}
	if (json_object_has_member(json_node_get_object(enrollment), "payment_path")) {
		g_autoptr(GDateTime) first_publication = publication ? venture_time_from_string(
			json_object_get_string_member(json_node_get_object(publication), "go_live_at"), error) : NULL;
		g_autoptr(VentureMoney) prepaid_total = NULL;
		g_autoptr(JsonArray) scaled = NULL;
		JsonArray *stored = json_object_has_member(json_node_get_object(enrollment), "shares") ?
			json_object_get_array_member(json_node_get_object(enrollment), "shares") : NULL;
		if (publication && !first_publication) goto fail;
		if (stored) {
			prepaid_total = enrollment_money(json_node_get_object(enrollment), "total", error);
			scaled = prepaid_total ? enrollment_charge_shares(stored, prepaid_total, error) : NULL;
			if (!scaled) goto fail;
		}
		if (!venture_database_commit(db, error) || !venture_accounting_operation_finish(operation, error)) return NULL;
		if (!enrollment_prepaid_price(db, billing, enrollment, error)) return NULL;
		provider = enrollment_provider(context, billing, error);
		if (!provider) return NULL;
		return venture_stripe_service_prepay(provider, subscription,
			json_object_get_string_member(json_node_get_object(enrollment), "payment_path"),
			scaled, first_publication, &actor, error);
	}
	result = json_node_new(JSON_NODE_OBJECT); object = json_object_new(); json_node_take_object(result, object);
	json_object_set_int_member(object, "organization_id", business);
	if (publication) json_object_set_member(object, "go_live_at", json_node_copy(json_object_get_member(json_node_get_object(publication), "go_live_at")));
	else json_object_set_null_member(object, "go_live_at");
	receipt = enrollment_read(db, billing, business, "first-charge", error);
	if (error && *error) goto fail;
	if (receipt) invoice = json_object_get_int_member(json_node_get_object(receipt), "invoice_id");
	deadline = venture_time_from_string(json_object_get_string_member(json_node_get_object(enrollment), "charge_due_at"), error);
	if (!deadline) goto fail;
	permission = enrollment_permission(db, billing, subscription, error);
	if (error && *error) goto fail;
	if (permission) g_object_get(permission, "status", &status, NULL);
	if (!invoice && !g_strcmp0(status, "active") && (publication || g_date_time_compare(now, deadline) >= 0)) {
		invoice = enrollment_first_invoice(db, billing, subscription, error);
		if (error && *error) goto fail;
		if (!invoice) {
			sub = get_record(db, VENTURE_TYPE_CUSTOMER_SUBSCRIPTION, subscription, error);
			if (!sub) goto fail;
			instruction = venture_billing_request_new();
			venture_entity_set_organization_id(VENTURE_ENTITY(instruction), billing);
			g_object_set(instruction, "action", publication ? "activate" : "renew", "subscription-id", subscription,
				"at", now, "expected-version", venture_entity_get_version(sub), NULL);
			if (!execute(db, instruction, &actor, error)) goto fail;
			invoice = number(VENTURE_ENTITY(instruction), "invoice-id");
			if (!invoice) { refuse(error, VENTURE_ERROR_CONFLICT, "first period has no invoice"); goto fail; }
		}
		receipt = json_node_new(JSON_NODE_OBJECT); json_node_take_object(receipt, json_object_new());
		json_object_set_int_member(json_node_get_object(receipt), "invoice_id", invoice);
		if (!enrollment_write(db, billing, business, "first-charge", receipt, &actor, error)) goto fail;
	}
	json_object_set_int_member(object, "invoice_id", invoice);
	json_object_set_string_member(object, "state", invoice ? "collection_pending" :
		g_strcmp0(status, "active") ? "awaiting_card" : "building");
	if (!venture_database_commit(db, error)) return NULL;
	if (!venture_accounting_operation_finish(operation, error)) return NULL;
	if (invoice) {
		g_autoptr(GError) collection_error = NULL;
		g_autoptr(VentureEntity) row = get_record(db, VENTURE_TYPE_INVOICE, invoice, error);
		if (!row) return NULL;
		if (g_strcmp0(enum_nick(row, "status"), "paid")) {
			provider = enrollment_provider(context, billing, &collection_error);
			if (provider) checkout = venture_stripe_service_collect_invoice(provider, invoice, &actor, &collection_error);
			g_clear_object(&row);
			row = get_record(db, VENTURE_TYPE_INVOICE, invoice, error);
			if (!row) return NULL;
		}
		/* The durable invoice survives a network outage. A retry invokes the
		 * existing idempotent collector; no raw provider error reaches owners. */
		json_object_set_string_member(object, "state", !g_strcmp0(enum_nick(row, "status"), "paid") ? "paid" : "collection_pending");
	}
	return g_steal_pointer(&result);
fail:
	venture_database_rollback(db);
	return NULL;
}

static VentureMoney *
enrollment_money(JsonObject *object, const gchar *name, GError **error)
{
	JsonObject *value = json_object_get_object_member(object, name);
	return venture_money_from_string(json_object_get_string_member(value, "amount"),
		json_object_get_string_member(value, "currency"), error);
}

JsonNode *
venture_lightsite_billing_guarantee(VentureDatabase *database, gint64 billing, gint64 business,
	const VentureMoney *shortfall, gboolean eligible, const gchar *measurement, GError **error)
{
	g_autoptr(GError) discarded = NULL;
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(VentureAccountingOperation) operation = NULL;
	g_autoptr(JsonNode) enrollment = NULL, publication = NULL, result = NULL;
	g_autoptr(GDateTime) now = venture_time_now(), live = NULL, due = NULL;
	g_autoptr(VentureMoney) cap = NULL, amount = NULL;
	g_autoptr(VentureEntity) credit = NULL;
	g_autofree gchar *reference = NULL, *formatted = NULL;
	JsonObject *object;
	VentureActor actor = { VENTURE_ACTOR_KIND_IMPORT, "Hosted guarantee", NULL, NULL, NULL };
	if (!error) error = &discarded;
	if (!shortfall || venture_money_get_amount(shortfall) < 0 || !key_valid(measurement)) {
		refuse(error, VENTURE_ERROR_VALIDATION, "a nonnegative shortfall and measurement identity are required");
		return NULL;
	}
	if (!prepare(database, billing, NULL, error)) return NULL;
	internal = venture_access_policy_enter(venture_database_get_access_policy(database), NULL);
	if (!business_valid(database, billing, business, error)) return NULL;
	formatted = venture_money_to_string(shortfall);
	operation = venture_accounting_operation_begin(database, "hosted.guarantee", NULL, NULL,
		g_variant_new("(xsbs)", business, formatted, eligible, measurement), billing, &actor, error);
	if (!operation || !venture_database_begin(database, error)) return NULL;
	result = enrollment_read(database, billing, business, "guarantee", error);
	if (result) {
		g_autoptr(VentureMoney) prior = enrollment_money(json_node_get_object(result), "shortfall", error);
		object = json_node_get_object(result);
		if (!prior) goto fail;
		if (g_strcmp0(json_object_get_string_member(object, "measurement"), measurement) ||
			json_object_get_boolean_member(object, "eligible") != eligible || !venture_money_equal(prior, shortfall)) {
			refuse(error, VENTURE_ERROR_CONFLICT, "guarantee was already measured; a second payout is refused");
			goto fail;
		}
		venture_database_rollback(database);
		return g_steal_pointer(&result);
	}
	if (error && *error) goto fail;
	enrollment = enrollment_read(database, billing, business, "enroll", error);
	if (error && *error) goto fail;
	publication = enrollment_read(database, billing, business, "publish", error);
	if (error && *error) goto fail;
	if (!enrollment || !publication) {
		refuse(error, VENTURE_ERROR_CONFLICT, "an enrolled and published business is required"); goto fail;
	}
	live = venture_time_from_string(json_object_get_string_member(json_node_get_object(publication), "go_live_at"), error);
	if (!live) goto fail;
	due = g_date_time_add_days(live, 90);
	if (g_date_time_compare(now, due) < 0) {
		refuse(error, VENTURE_ERROR_CONFLICT, "guarantee measurement is not due yet"); goto fail;
	}
	cap = enrollment_money(json_node_get_object(enrollment), "guarantee_cap", error);
	if (!cap) goto fail;
	if (g_strcmp0(venture_money_get_currency(shortfall), venture_money_get_currency(cap))) {
		refuse(error, VENTURE_ERROR_VALIDATION, "shortfall must use the enrolled currency"); goto fail;
	}
	amount = eligible ? venture_money_copy(venture_money_compare(shortfall, cap) < 0 ? shortfall : cap) :
		venture_money_new_zero(venture_money_get_currency(cap));
	if (venture_money_get_amount(amount) > 0) {
		reference = enrollment_key(business, "guarantee");
		credit = g_object_new(VENTURE_TYPE_CUSTOMER_CREDIT, "kind", "credit_note", "amount", amount,
			"customer-id", json_object_get_int_member(json_node_get_object(enrollment), "company_id"),
			"date", now, "reference", reference, NULL);
		venture_entity_set_organization_id(credit, billing);
		if (!venture_database_save(database, credit, &actor, error)) goto fail;
	}
	result = json_node_new(JSON_NODE_OBJECT); object = json_object_new(); json_node_take_object(result, object);
	json_object_set_int_member(object, "organization_id", business);
	json_object_set_string_member(object, "measurement", measurement);
	json_object_set_boolean_member(object, "eligible", eligible);
	json_object_set_member(object, "shortfall", money_node(shortfall));
	json_object_set_member(object, "cap", money_node(cap));
	json_object_set_member(object, "credit", money_node(amount));
	json_object_set_int_member(object, "credit_id", credit ? venture_entity_get_id(credit) : 0);
	set_time(object, "period_end", due);
	set_time(object, "measured_at", now);
	if (!enrollment_write(database, billing, business, "guarantee", result, &actor, error)) goto fail;
	if (!venture_database_commit(database, error)) return NULL;
	if (!venture_accounting_operation_finish(operation, error)) return NULL;
	return g_steal_pointer(&result);
fail:
	venture_database_rollback(database);
	return NULL;
}
