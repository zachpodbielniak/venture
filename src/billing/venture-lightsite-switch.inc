/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * The owner's plan switch. The published promise is "move up or down any
 * time; the change applies from your next bill", so a switch is only ever
 * the billing service's change at the end of the period: nothing is
 * charged or credited today and nothing is prorated. Before go-live the
 * next bill is the first one, which publication issues at the pending
 * price. The preview and the confirmation build one answer from one
 * function, so what the owner agreed to is what was scheduled.
 */

/* The owner of the business may switch it; so may the trusted service,
 * which checks its dashboard user owns the business before forwarding. */
static gboolean
switch_prepare(VentureDatabase *database, gint64 billing, gint64 business, gint64 *actor_id, GError **error)
{
	g_autoptr(GError) service = NULL;
	g_autoptr(VentureEntity) organization = NULL;
	g_autoptr(VentureAccessScope) internal = NULL;
	VentureTenantService *tenant = venture_tenant_service_get(database);
	if (!venture_tenant_service_check_trusted_service(tenant, actor_id, &service) &&
	    !venture_tenant_service_check_organization_owner(tenant, business, NULL, error))
		return FALSE;
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "customer_subscription") == G_TYPE_INVALID)
		return refuse(error, VENTURE_ERROR_NOT_FOUND, "the billing module is off");
	if (billing <= 0)
		return refuse(error, VENTURE_ERROR_NOT_FOUND, "no billing organization is configured");
	internal = venture_access_policy_enter(venture_database_get_access_policy(database), NULL);
	organization = live_organization(database, billing, "the configured billing organization is unavailable", error);
	return organization != NULL;
}

/* {plan_price_id, code, name, amount, currency, interval} of one price. */
static JsonObject *
switch_plan(VentureDatabase *database, VentureEntity *price, GError **error)
{
	g_autoptr(VentureEntity) plan = get_record(database, VENTURE_TYPE_PLAN, number(price, "plan-id"), error);
	g_autoptr(VentureMoney) amount = NULL;
	g_autofree gchar *code = NULL, *name = NULL, *currency = NULL, *text = NULL;
	JsonObject *object;
	if (!plan) return NULL;
	g_object_get(plan, "code", &code, "name", &name, NULL);
	g_object_get(price, "amount", &amount, "currency", &currency, NULL);
	text = amount ? decimal(amount) : NULL;
	object = json_object_new();
	json_object_set_int_member(object, "plan_price_id", venture_entity_get_id(price));
	json_object_set_string_member(object, "code", code ? code : "");
	json_object_set_string_member(object, "name", name ? name : "");
	if (text) json_object_set_string_member(object, "amount", text);
	else json_object_set_null_member(object, "amount");
	json_object_set_string_member(object, "currency", currency ? currency : "");
	json_object_set_string_member(object, "interval", enum_nick(price, "interval"));
	return object;
}

/* What the billing organization offers an owner: the rule enrollment uses,
 * in the currency the business is already billed in. Anything else is not
 * a price this business can be on, so it is 422, never a guess. */
static VentureEntity *
switch_offered(VentureDatabase *database, gint64 billing, gint64 price_id, VentureEntity *current, GError **error)
{
	g_autoptr(GError) local = NULL;
	g_autoptr(VentureEntity) price = NULL;
	g_autoptr(VentureMoney) amount = NULL;
	g_autofree gchar *currency = NULL, *billed = NULL;
	const gchar *interval;
	gboolean active = FALSE;
	price = get_record(database, VENTURE_TYPE_PLAN_PRICE, price_id, &local);
	if (local && !g_error_matches(local, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND)) {
		g_propagate_error(error, g_steal_pointer(&local));
		return NULL;
	}
	if (price) {
		g_object_get(price, "active", &active, "amount", &amount, "currency", &currency, NULL);
		g_object_get(current, "currency", &billed, NULL);
	}
	interval = price ? enum_nick(price, "interval") : NULL;
	if (!price || venture_entity_is_deleted(price) || venture_entity_get_organization_id(price) != billing ||
	    !active || !amount || venture_money_get_amount(amount) <= 0 ||
	    (g_strcmp0(interval, "month") && g_strcmp0(interval, "year"))) {
		refuse(error, VENTURE_ERROR_VALIDATION, "that price is not offered");
		return NULL;
	}
	if (g_strcmp0(currency, billed) != 0) {
		refuse(error, VENTURE_ERROR_VALIDATION, "that price is in another currency than the plan the business is on");
		return NULL;
	}
	return g_steal_pointer(&price);
}

/* The business's subscription, if it is one an owner may switch now. */
static VentureEntity *
switch_subscription(VentureDatabase *database, gint64 billing, gint64 business, VentureEntity **company, GError **error)
{
	g_autoptr(VentureEntity) sub = NULL;
	g_autofree gchar *identity = NULL;
	gint status = 0;
	gboolean ending = FALSE;
	*company = customer_company(database, billing, business, error);
	if (!*company) {
		if (!error || !*error) refuse(error, VENTURE_ERROR_VALIDATION, "the business has no plan to switch yet");
		return NULL;
	}
	sub = current_subscription(database, billing, venture_entity_get_id(*company), error);
	if (sub) g_object_get(sub, "status", &status, "cancel-at-period-end", &ending, "external-id", &identity, NULL);
	if (!sub || !status_live(status)) {
		if (!error || !*error) refuse(error, VENTURE_ERROR_VALIDATION, "the business has no plan to switch yet");
		return NULL;
	}
	if (status == 3) {
		refuse(error, VENTURE_ERROR_VALIDATION, "a paused plan cannot switch; resume it first");
		return NULL;
	}
	if (ending) {
		refuse(error, VENTURE_ERROR_CONFLICT, "the plan is ending at the close of this period");
		return NULL;
	}
	if (identity && g_str_has_prefix(identity, "prepaid:")) {
		refuse(error, VENTURE_ERROR_CONFLICT, "prepaid terms run to their end and cannot switch");
		return NULL;
	}
	return g_steal_pointer(&sub);
}

static gchar *
switch_describe(JsonObject *plan)
{
	return g_strdup_printf("%s (%s %s a %s)", json_object_get_string_member(plan, "name"),
		json_object_get_string_member(plan, "amount"), json_object_get_string_member(plan, "currency"),
		json_object_get_string_member(plan, "interval"));
}

/* The one answer of a preview and a confirmation. */
static JsonNode *
switch_answer(VentureDatabase *database, gint64 business, VentureEntity *sub, VentureEntity *current,
	VentureEntity *chosen, const gchar *state, GError **error)
{
	g_autoptr(VentureEntity) pending = NULL;
	g_autoptr(GDateTime) period_end = NULL;
	g_autoptr(VentureMoney) next_amount = NULL;
	g_autofree gchar *identity = NULL, *from = NULL, *to = NULL, *when = NULL, *summary = NULL;
	JsonObject *object, *current_plan, *chosen_plan, *pending_plan = NULL;
	JsonNode *node;
	gint status = 0;
	gboolean unpublished;
	g_object_get(sub, "current-period-end", &period_end, "external-id", &identity, "status", &status, NULL);
	/* An enrollment is not billed until its site goes live, whatever its
	 * trial end says: publication issues the first bill. */
	unpublished = identity && g_str_has_prefix(identity, "hosted:") && status == 0;
	if (number(sub, "pending-plan-price-id") != 0) {
		pending = get_record(database, VENTURE_TYPE_PLAN_PRICE, number(sub, "pending-plan-price-id"), error);
		if (!pending) return NULL;
		pending_plan = switch_plan(database, pending, error);
		if (!pending_plan) return NULL;
	}
	current_plan = switch_plan(database, current, error);
	if (!current_plan) { if (pending_plan) json_object_unref(pending_plan); return NULL; }
	chosen_plan = switch_plan(database, chosen, error);
	if (!chosen_plan) {
		json_object_unref(current_plan);
		if (pending_plan) json_object_unref(pending_plan);
		return NULL;
	}
	g_object_get(chosen, "amount", &next_amount, NULL);
	from = switch_describe(current_plan);
	to = switch_describe(chosen_plan);
	when = unpublished ? NULL : venture_time_to_date_string(period_end, NULL);
	if (venture_entity_get_id(chosen) == venture_entity_get_id(current))
		summary = g_strdup_printf("You stay on %s. Nothing changes and nothing is charged today.", from);
	else if (unpublished)
		summary = g_strdup_printf("Your plan changes from %s to %s from your first bill, when your site goes live. "
			"Nothing is charged today and nothing is prorated.", from, to);
	else
		summary = g_strdup_printf("Your plan changes from %s to %s from your next bill, on %s. "
			"Nothing is charged today and nothing is prorated.", from, to, when);
	object = json_object_new();
	json_object_set_int_member(object, "organization_id", business);
	json_object_set_int_member(object, "subscription_id", venture_entity_get_id(sub));
	json_object_set_object_member(object, "current", current_plan);
	if (pending_plan) json_object_set_object_member(object, "pending", pending_plan);
	else json_object_set_null_member(object, "pending");
	json_object_set_object_member(object, "chosen", chosen_plan);
	json_object_set_string_member(object, "effective", "next_bill");
	set_time(object, "next_bill_at", unpublished ? NULL : period_end);
	json_object_set_boolean_member(object, "starts_at_go_live", unpublished);
	json_object_set_member(object, "next_amount", money_node(next_amount));
	json_object_set_boolean_member(object, "charged_today", FALSE);
	json_object_set_boolean_member(object, "prorated", FALSE);
	json_object_set_string_member(object, "state", state);
	json_object_set_string_member(object, "summary", summary);
	node = json_node_new(JSON_NODE_OBJECT);
	json_node_take_object(node, object);
	return node;
}

/* The latest switch receipt of the business, the answer a repeated
 * confirmation of the price already pending gives back. */
static VentureEntity *
switch_last_receipt(VentureDatabase *database, gint64 billing, gint64 business, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_LIGHTSITE_BILLING_RECEIPT);
	g_autoptr(GPtrArray) rows = NULL;
	venture_query_set_organization(query, billing);
	venture_query_add_filter_int(query, "business-id", VENTURE_FILTER_OP_EQ, business, NULL);
	venture_query_add_filter_string(query, "outcome", VENTURE_FILTER_OP_EQ, "switch", NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_DESCENDING, NULL);
	venture_query_set_limit(query, 1);
	rows = find(database, query, error);
	return rows && rows->len ? g_object_ref(g_ptr_array_index(rows, 0)) : NULL;
}

static JsonNode *
switch_impl(VentureDatabase *database, gint64 billing, gint64 business, gint64 price_id,
	gboolean confirm, gboolean *created, GError **error)
{
	const VentureAuthPrincipal *principal;
	VentureActor audit;
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(VentureEntity) company = NULL, sub = NULL, current = NULL, chosen = NULL, receipt = NULL;
	g_autoptr(VentureBillingRequest) instruction = NULL;
	g_autoptr(JsonNode) result = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	g_autofree gchar *key = NULL, *text = NULL, *hash = NULL, *code = NULL;
	gint64 actor_id = 0, scheduled;
	if (created) *created = FALSE;
	if (price_id <= 0) {
		refuse(error, VENTURE_ERROR_VALIDATION, "plan_price_id must be a positive integer");
		return NULL;
	}
	if (!switch_prepare(database, billing, business, &actor_id, error)) return NULL;
	principal = venture_access_policy_get_actor(venture_database_get_access_policy(database));
	if (actor_id == 0 && principal) actor_id = principal->user_id;
	audit.kind = VENTURE_ACTOR_KIND_IMPORT;
	audit.name = principal ? principal->name : NULL;
	audit.prompt = NULL;
	audit.request_id = NULL;
	audit.approved_by = NULL;
	internal = venture_access_policy_enter(venture_database_get_access_policy(database), NULL);
	if (!business_valid(database, billing, business, error)) return NULL;
	if (!subscription_not_cancelled(database, billing, business, error)) return NULL;
	if (confirm && !venture_database_begin(database, error)) return NULL;
	sub = switch_subscription(database, billing, business, &company, error);
	if (!sub) goto fail;
	current = get_record(database, VENTURE_TYPE_PLAN_PRICE, number(sub, "plan-price-id"), error);
	if (!current) goto fail;
	chosen = switch_offered(database, billing, price_id, current, error);
	if (!chosen) goto fail;
	scheduled = number(sub, "pending-plan-price-id") ? number(sub, "pending-plan-price-id") : number(sub, "plan-price-id");
	if (!confirm)
		return switch_answer(database, business, sub, current, chosen, "preview", error);
	if (scheduled == price_id) {
		/* Already what the next bill will be: the switch that made it so is
		 * answered again, and staying on today's plan changes nothing. */
		receipt = number(sub, "pending-plan-price-id") ? switch_last_receipt(database, billing, business, error) : NULL;
		if (error && *error) goto fail;
		if (receipt) {
			g_object_get(receipt, "result", &text, NULL);
			result = venture_json_parse(text, error);
		} else
			result = switch_answer(database, business, sub, current, chosen, "unchanged", error);
		venture_database_rollback(database);
		return g_steal_pointer(&result);
	}
	instruction = venture_billing_request_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(instruction), billing);
	g_object_set(instruction, "action", "change", "subscription-id", venture_entity_get_id(sub),
		"plan-price-id", price_id, "at-period-end", TRUE, "at", now,
		"expected-version", venture_entity_get_version(sub), NULL);
	if (!execute(database, instruction, &audit, error)) goto fail;
	g_clear_object(&sub);
	sub = get_record(database, VENTURE_TYPE_CUSTOMER_SUBSCRIPTION, number(VENTURE_ENTITY(instruction), "subscription-id"), error);
	if (!sub) goto fail;
	result = switch_answer(database, business, sub, current, chosen, "scheduled", error);
	if (!result) goto fail;
	text = venture_json_to_string(result, FALSE);
	hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, text, -1);
	key = g_strdup_printf("hosted:%" G_GINT64_FORMAT ":switch:%" G_GINT64_FORMAT ":%" G_GINT64_FORMAT,
		business, venture_entity_get_id(sub), venture_entity_get_version(sub));
	code = g_strdup(json_object_get_string_member(json_object_get_object_member(json_node_get_object(result), "chosen"), "code"));
	receipt = g_object_new(VENTURE_TYPE_LIGHTSITE_BILLING_RECEIPT, "idempotency-key", key, "request-hash", hash,
		"business-id", business, "plan-code", code, "outcome", "switch", "company-id", venture_entity_get_id(company),
		"subscription-id", venture_entity_get_id(sub), "result", text, "actor-user-id", actor_id, NULL);
	venture_entity_set_organization_id(receipt, billing);
	if (!save_receipt(database, receipt, &audit, error)) goto fail;
	if (!venture_database_commit(database, error)) return NULL;
	if (created) *created = TRUE;
	return g_steal_pointer(&result);
fail:
	if (confirm) venture_database_rollback(database);
	return NULL;
}

JsonNode *
venture_lightsite_billing_switch(VentureDatabase *database, gint64 billing_organization_id, gint64 organization_id,
	gint64 plan_price_id, gboolean confirm, gboolean *created, GError **error)
{
	g_autoptr(GError) local = NULL;
	JsonNode *result;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	result = switch_impl(database, billing_organization_id, organization_id, plan_price_id, confirm, created, &local);
	if (!result) {
		if (!local) refuse(&local, VENTURE_ERROR_FAILED, "the plan switch did not complete");
		g_propagate_error(error, g_steal_pointer(&local));
	}
	return result;
}
