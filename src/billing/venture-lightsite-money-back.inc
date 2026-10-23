/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* The money-back promise: within thirty days of go-live the business can
 * have every settled hosted payment back. The decision, the cancellation and
 * the list of payments to refund commit together as one receipt; the refunds
 * then go to the provider one by one, each safe to repeat, and a second
 * receipt records that they are all done. */
#define MONEY_BACK_DAYS 30

/* Settled payments received against the subscription's invoices. */
static JsonArray *
money_back_payments(VentureDatabase *db, gint64 billing, gint64 subscription, GError **error)
{
	g_autoptr(VentureQuery) events = venture_query_new(VENTURE_TYPE_SUBSCRIPTION_EVENT);
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GHashTable) seen = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	g_autoptr(JsonArray) out = json_array_new();
	guint i, j;
	venture_query_set_organization(events, billing);
	venture_query_add_filter_int(events, "subscription-id", VENTURE_FILTER_OP_EQ, subscription, NULL);
	venture_query_add_filter_int(events, "invoice-id", VENTURE_FILTER_OP_GT, 0, NULL);
	venture_query_add_order(events, "id", VENTURE_SORT_ASCENDING, NULL);
	rows = find(db, events, error);
	if (!rows) return NULL;
	for (i = 0; i < rows->len; i++) {
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_PAYMENT_ALLOCATION);
		g_autoptr(GPtrArray) allocations = NULL;
		venture_query_set_organization(query, billing);
		venture_query_add_filter_int(query, "invoice-id", VENTURE_FILTER_OP_EQ, number(g_ptr_array_index(rows, i), "invoice-id"), NULL);
		allocations = find(db, query, error);
		if (!allocations) return NULL;
		for (j = 0; j < allocations->len; j++) {
			gint64 id = number(g_ptr_array_index(allocations, j), "payment-id");
			g_autoptr(VentureEntity) payment = NULL;
			g_autoptr(VentureMoney) amount = NULL;
			g_autofree gchar *reference = NULL;
			JsonObject *item;
			if (id <= 0 || g_hash_table_contains(seen, &id)) continue;
			g_hash_table_add(seen, g_memdup2(&id, sizeof id));
			payment = get_record(db, VENTURE_TYPE_PAYMENT, id, error);
			if (!payment) return NULL;
			if (venture_entity_is_deleted(payment) || venture_payment_is_bookkeeping(VENTURE_PAYMENT(payment))) continue;
			g_object_get(payment, "amount", &amount, "reference", &reference, NULL);
			item = json_object_new();
			json_object_set_int_member(item, "payment_id", id);
			json_object_set_member(item, "amount", money_node(amount));
			/* A payment that did not come through the card provider is
			 * listed for staff to return by hand. */
			json_object_set_boolean_member(item, "by_hand", !reference || !g_str_has_prefix(reference, "pi_"));
			json_object_set_null_member(item, "refund");
			json_array_add_object_element(out, item);
		}
	}
	return g_steal_pointer(&out);
}

/* Decide, cancel now, and list what to refund, all in one commit. */
static JsonNode *
money_back_decide(VentureDatabase *db, gint64 billing, gint64 business, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureAccountingOperation) operation = NULL;
	g_autoptr(JsonNode) enrollment = NULL, publication = NULL, plan = NULL;
	g_autoptr(VentureEntity) sub = NULL;
	g_autoptr(VentureBillingRequest) instruction = NULL;
	g_autoptr(GDateTime) now = venture_time_now(), live = NULL, deadline = NULL;
	JsonArray *payments;
	JsonObject *object;
	gint64 subscription;
	operation = venture_accounting_operation_begin(db, "hosted.money-back", NULL, NULL,
		g_variant_new("(x)", business), billing, actor, error);
	if (!operation || !venture_database_begin(db, error)) return NULL;
	plan = enrollment_read(db, billing, business, "money-back", error);
	if (plan || *error) {
		venture_database_rollback(db);
		return g_steal_pointer(&plan);
	}
	enrollment = enrollment_read(db, billing, business, "enroll", error);
	if (!enrollment) {
		if (!*error) refuse(error, VENTURE_ERROR_NOT_FOUND, "business is not enrolled");
		goto fail;
	}
	publication = enrollment_read(db, billing, business, "publish", error);
	if (!publication) {
		if (!*error) refuse(error, VENTURE_ERROR_CONFLICT, "the business has not gone live, so nothing was charged; cancel instead");
		goto fail;
	}
	live = venture_time_from_string(json_object_get_string_member(json_node_get_object(publication), "go_live_at"), error);
	if (!live) goto fail;
	deadline = g_date_time_add_days(live, MONEY_BACK_DAYS);
	if (g_date_time_compare(now, deadline) >= 0) {
		refuse(error, VENTURE_ERROR_CONFLICT, "the thirty days for money back have passed"); goto fail;
	}
	subscription = json_object_get_int_member(json_node_get_object(enrollment), "subscription_id");
	sub = get_record(db, VENTURE_TYPE_CUSTOMER_SUBSCRIPTION, subscription, error);
	if (!sub) goto fail;
	if (strcmp(enum_nick(sub, "status"), "cancelled")) {
		instruction = venture_billing_request_new();
		venture_entity_set_organization_id(VENTURE_ENTITY(instruction), billing);
		g_object_set(instruction, "action", "cancel", "subscription-id", subscription,
			"at-period-end", FALSE, "at", now, "expected-version", venture_entity_get_version(sub), NULL);
		if (!execute(db, instruction, actor, error)) goto fail;
	}
	payments = money_back_payments(db, billing, subscription, error);
	if (!payments) goto fail;
	plan = json_node_new(JSON_NODE_OBJECT); object = json_object_new(); json_node_take_object(plan, object);
	json_object_set_int_member(object, "organization_id", business);
	json_object_set_int_member(object, "subscription_id", subscription);
	json_object_set_string_member(object, "state", "refunding");
	set_time(object, "go_live_at", live);
	set_time(object, "cancelled_at", now);
	json_object_set_array_member(object, "refunds", payments);
	if (!enrollment_write(db, billing, business, "money-back", plan, actor, error)) goto fail;
	if (!venture_database_commit(db, error) || !venture_accounting_operation_finish(operation, error)) return NULL;
	return g_steal_pointer(&plan);
fail:
	venture_database_rollback(db);
	return NULL;
}

JsonNode *
venture_lightsite_billing_money_back(VentureContext *context, gint64 billing, gint64 business, GError **error)
{
	g_autoptr(GError) discarded = NULL;
	VentureDatabase *db = venture_context_get_database(context);
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(VentureStripeService) provider = NULL;
	g_autoptr(JsonNode) done = NULL, plan = NULL;
	VentureActor actor = { VENTURE_ACTOR_KIND_IMPORT, "Hosted money back", NULL, NULL, NULL };
	JsonArray *refunds;
	guint i;
	if (!error) error = &discarded;
	if (!prepare(db, billing, NULL, error)) return NULL;
	internal = venture_access_policy_enter(venture_database_get_access_policy(db), NULL);
	if (!business_valid(db, billing, business, error)) return NULL;
	done = enrollment_read(db, billing, business, "money-back-done", error);
	if (done || *error) return g_steal_pointer(&done);
	plan = money_back_decide(db, billing, business, &actor, error);
	if (!plan) return NULL;
	refunds = json_object_get_array_member(json_node_get_object(plan), "refunds");
	for (i = 0; i < json_array_get_length(refunds); i++) {
		JsonObject *item = json_array_get_object_element(refunds, i);
		g_autofree gchar *key = NULL, *refund = NULL;
		gint64 payment = json_object_get_int_member(item, "payment_id");
		if (json_object_get_boolean_member(item, "by_hand")) continue;
		if (!provider && !(provider = enrollment_provider(context, billing, error))) return NULL;
		key = g_strdup_printf("hosted:%" G_GINT64_FORMAT ":money-back:%" G_GINT64_FORMAT, business, payment);
		if (!venture_stripe_service_refund_payment(provider, payment, key, &refund, &actor, error)) return NULL;
		json_object_set_string_member(item, "refund", refund);
	}
	json_object_set_string_member(json_node_get_object(plan), "state", "refunded");
	if (!venture_database_begin(db, error)) return NULL;
	done = enrollment_read(db, billing, business, "money-back-done", error);
	if (!done && !*error && !enrollment_write(db, billing, business, "money-back-done", plan, &actor, error)) {
		venture_database_rollback(db);
		return NULL;
	}
	if (*error || !venture_database_commit(db, error)) {
		venture_database_rollback(db);
		return NULL;
	}
	return done ? g_steal_pointer(&done) : g_steal_pointer(&plan);
}
