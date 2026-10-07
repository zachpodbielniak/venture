/* Read-only, minimal billing facts for the owner's transactional emails.
 * The caller renders them and persists its handoff identity. No secrets leave.
 * SPDX-License-Identifier: AGPL-3.0-or-later */
static JsonObject *
mail_fact(JsonArray *events, const gchar *kind, gint64 id, GDateTime *at, const VentureMoney *amount)
{
	JsonObject *event = json_object_new();
	g_autofree gchar *key = g_strdup_printf("%s:%" G_GINT64_FORMAT, kind, id);
	g_autofree gchar *formatted = amount ? venture_money_to_string(amount) : NULL;
	json_object_set_string_member(event, "id", key);
	json_object_set_string_member(event, "kind", kind);
	if (at) set_time(event, "at", at);
	if (formatted) json_object_set_string_member(event, "amount", formatted);
	json_array_add_object_element(events, event);
	return event;
}
static GPtrArray *
mail_rows(VentureDatabase *db, GType type, gint64 billing, const gchar *field, gint64 id, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(type);
	venture_query_set_organization(query, billing);
	venture_query_add_filter_int(query, field, VENTURE_FILTER_OP_EQ, id, NULL);
	return venture_database_find(db, query, error);
}
static gboolean
mail_cards(VentureDatabase *db, gint64 billing, gint64 subscription, JsonArray *events, GError **error)
{
	g_autoptr(GPtrArray) rows = NULL;
	guint i;
	if (!venture_entity_registry_is_type_enabled(venture_entity_registry_get_default(), "stripe_authorization")) return TRUE;
	rows = mail_rows(db, VENTURE_TYPE_STRIPE_AUTHORIZATION, billing, "subscription-id", subscription, error);
	if (!rows) return FALSE;
	for (i = 0; i < rows->len; i++) {
		VentureEntity *row = g_ptr_array_index(rows, i);
		g_autoptr(GDateTime) at = NULL;
		g_object_get(row, "authorized-at", &at, NULL);
		if (at) mail_fact(events, "card_saved", venture_entity_get_id(row), at, NULL);
	}
	return TRUE;
}
static gboolean
mail_receipts(VentureDatabase *db, gint64 billing, gint64 company, JsonArray *events, GError **error)
{
	const GType types[] = {VENTURE_TYPE_PAYMENT, VENTURE_TYPE_REFUND, VENTURE_TYPE_CUSTOMER_CREDIT};
	const gchar *kinds[] = {"receipt", "refund", "credit"};
	guint i, j;
	for (i = 0; i < G_N_ELEMENTS(types); i++) {
		g_autoptr(GPtrArray) rows = mail_rows(db, types[i], billing, "customer-id", company, error);
		if (!rows) return FALSE;
		for (j = 0; j < rows->len; j++) {
			VentureEntity *row = g_ptr_array_index(rows, j);
			if (i == 0 && venture_payment_is_bookkeeping(VENTURE_PAYMENT(row))) continue;
			if (i == 0) {
				g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_MAIL_MESSAGE);
				g_autoptr(GPtrArray) existing = NULL;
				g_autofree gchar *key = g_strdup_printf("receipt:%s", venture_entity_get_uuid(row));
				venture_query_set_organization(query, billing);
				venture_query_set_limit(query, 1);
				venture_query_set_include_deleted(query, TRUE);
				venture_query_add_filter_string(query, "idempotency-key", VENTURE_FILTER_OP_EQ, key, NULL);
				existing = venture_database_find(db, query, error);
				if (!existing) return FALSE;
				/* An upgrade must not compete with a receipt already owned by
				 * the old sender, including retries, uncertainty or retention. */
				if (existing->len) continue;
			}
			g_autoptr(VentureMoney) amount = NULL;
			g_autoptr(GDateTime) date = NULL;
			g_autofree gchar *kind = NULL;
			if (i == 2) {
				g_object_get(row, "kind", &kind, NULL);
				if (g_strcmp0(kind, "credit_note")) continue; /* deposits are already receipts */
			}
			g_object_get(row, "amount", &amount, "date", &date, NULL);
			if (amount && venture_money_get_amount(amount) > 0)
				mail_fact(events, kinds[i], venture_entity_get_id(row), date, amount);
		}
	}
	return TRUE;
}
static gboolean
mail_failures(VentureDatabase *db, gint64 billing, gint64 company, JsonArray *events, GError **error)
{
	g_autoptr(GPtrArray) invoices = mail_rows(db, VENTURE_TYPE_INVOICE, billing, "company-id", company, error);
	guint i, j;
	if (!invoices) return FALSE;
	if (!venture_entity_registry_is_type_enabled(venture_entity_registry_get_default(), "stripe_checkout")) return TRUE;
	for (i = 0; i < invoices->len; i++) {
		VentureEntity *invoice = g_ptr_array_index(invoices, i);
		g_autoptr(GPtrArray) attempts = NULL;
		if (!g_strcmp0(enum_nick(invoice, "status"), "paid") || !g_strcmp0(enum_nick(invoice, "status"), "void")) continue;
		attempts = mail_rows(db, VENTURE_TYPE_STRIPE_CHECKOUT, billing, "invoice-id", venture_entity_get_id(invoice), error);
		if (!attempts) return FALSE;
		for (j = 0; j < attempts->len; j++) {
			VentureEntity *attempt = g_ptr_array_index(attempts, j);
			g_autofree gchar *state = NULL;
			g_autoptr(GDateTime) at = NULL;
			g_object_get(attempt, "status", &state, "updated-at", &at, NULL);
			if (!g_strcmp0(state, "failed")) mail_fact(events, "payment_failed", venture_entity_get_id(attempt), at, NULL);
		}
	}
	return TRUE;
}
static gboolean
mail_approaching(GDateTime *due, GDateTime *now)
{
	gint64 remaining = due ? g_date_time_difference(due, now) : -1;
	return remaining > 0 && remaining <= 2 * G_TIME_SPAN_DAY;
}
static gboolean
mail_installment(VentureDatabase *db, gint64 billing, gint64 subscription, JsonArray *events, GDateTime *now, GError **error)
{
	g_autoptr(GPtrArray) groups = NULL;
	guint i;
	if (!venture_entity_registry_is_type_enabled(venture_entity_registry_get_default(), "stripe_prepayment")) return TRUE;
	groups = mail_rows(db, VENTURE_TYPE_STRIPE_PREPAYMENT, billing, "subscription-id", subscription, error);
	if (!groups) return FALSE;
	for (i = 0; i < groups->len; i++) {
		VentureEntity *group = g_ptr_array_index(groups, i);
		g_autofree gchar *path = NULL, *state = NULL, *journal = NULL;
		g_autoptr(GDateTime) live = NULL, due = NULL;
		g_autoptr(JsonNode) parsed = NULL;
		g_autoptr(VentureMoney) total = NULL, amount = NULL;
		JsonArray *legs;
		JsonObject *leg, *event;
		g_object_get(group, "payment-path", &path, "state", &state, "published-at", &live,
			"legs", &journal, "total", &total, NULL);
		parsed = venture_json_parse(journal, error);
		if (!parsed) return FALSE;
		legs = json_object_get_array_member(json_node_get_object(parsed), "legs");
		if (!legs || !total) return refuse(error, VENTURE_ERROR_FAILED, "Prepayment evidence is incomplete");
		{
			guint slot;
			gboolean ready = json_array_get_length(legs) > 0;
			for (slot = 0; slot < json_array_get_length(legs); slot++) {
				JsonObject *part = json_array_get_object_element(legs, slot);
				gint64 attempt = json_object_has_member(part, "attempt") ? json_object_get_int_member(part, "attempt") : 0;
				gboolean paid = json_object_has_member(part, "paid") && json_object_get_boolean_member(part, "paid");
				/* The second installment reuses the first verified Setup. */
				if (!json_object_has_member(part, "method") || (!slot && !json_object_has_member(part, "setup"))) ready = FALSE;
				if (!paid && g_strcmp0(state, "cancelled") && (attempt > 0 || !g_strcmp0(state, "failed") || !g_strcmp0(state, "compensating"))) {
					g_autofree gchar *identity = g_strdup_printf("payment_failed:prepaid:%" G_GINT64_FORMAT ":%u:%" G_GINT64_FORMAT,
						venture_entity_get_id(group), slot, attempt);
					g_autoptr(VentureMoney) failed_amount = venture_money_new_for_currency(json_object_get_int_member(part, "amount"), venture_money_get_currency(total));
					event = mail_fact(events, "payment_failed", venture_entity_get_id(group), NULL, failed_amount);
					json_object_set_string_member(event, "id", identity);
				}
			}
			if (ready) {
				g_autofree gchar *identity = g_strdup_printf("card_saved:prepaid:%" G_GINT64_FORMAT, venture_entity_get_id(group));
				event = mail_fact(events, "card_saved", venture_entity_get_id(group), NULL, NULL);
				json_object_set_string_member(event, "id", identity);
			}
		}
		if (g_strcmp0(path, "installments") || !live || !g_strcmp0(state, "paid") ||
			!g_strcmp0(state, "cancelled") || !g_strcmp0(state, "compensating")) continue;
		due = g_date_time_add_days(live, 30);
		if (!mail_approaching(due, now)) continue;
		if (json_array_get_length(legs) != 2) {
			return refuse(error, VENTURE_ERROR_FAILED, "Installment evidence is incomplete");
		}
		leg = json_array_get_object_element(legs, 1);
		if (json_object_has_member(leg, "paid") && json_object_get_boolean_member(leg, "paid")) continue;
		/* Provider units have the currency exponent, as used by prepayment. */
		amount = venture_money_new_for_currency(json_object_get_int_member(leg, "amount"), venture_money_get_currency(total));
		event = mail_fact(events, "installment_due", venture_entity_get_id(group), live, amount);
		set_time(event, "due_at", due);
	}
	return TRUE;
}
/**
 * venture_lightsite_billing_notifications:
 * @database: hosted source database
 * @billing: configured operator billing organization
 * @business: customer business identity
 * @error: (out) (optional): unauthorized or incomplete evidence
 * Returns: (transfer full) (nullable): scoped, minimal events with stable identities
 */
JsonNode *
venture_lightsite_billing_notifications(VentureDatabase *database, gint64 billing, gint64 business, GError **error)
{
	g_autoptr(GError) discarded = NULL;
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(VentureEntity) company = NULL, price = NULL;
	g_autoptr(JsonNode) enrollment = NULL, cancellation = NULL, guarantee = NULL;
	g_autoptr(JsonArray) events = json_array_new();
	g_autoptr(GDateTime) now = venture_time_now();
	JsonObject *result, *terms;
	gint64 company_id = 0, subscription = 0;
	if (!error) error = &discarded;
	if (!prepare(database, billing, NULL, error)) return NULL;
	internal = venture_access_policy_enter(venture_database_get_access_policy(database), NULL);
	if (!business_valid(database, billing, business, error)) return NULL;
	company = customer_company(database, billing, business, error);
	if (*error) return NULL;
	if (company) company_id = venture_entity_get_id(company);
	enrollment = enrollment_read(database, billing, business, "enroll", error);
	if (*error) return NULL;
	cancellation = enrollment_read(database, billing, business, "cancel", error);
	if (*error) return NULL;
	guarantee = enrollment_read(database, billing, business, "guarantee", error);
	if (*error) return NULL;
	if (enrollment) {
		g_autoptr(GDateTime) due = NULL;
		const gchar *deadline;
		terms = json_node_get_object(enrollment);
		subscription = json_object_get_int_member(terms, "subscription_id");
		if (!mail_cards(database, billing, subscription, events, error)) return NULL;
		deadline = json_object_has_member(terms, "charge_due_at") ? json_object_get_string_member(terms, "charge_due_at") : NULL;
		if (deadline) due = venture_time_from_string(deadline, error);
		if (*error) return NULL;
		if (!cancellation && mail_approaching(due, now) && !enrollment_first_invoice(database, billing, subscription, error)) {
			g_autoptr(VentureMoney) amount = NULL;
			JsonObject *event;
			if (*error) return NULL;
			price = get_record(database, VENTURE_TYPE_PLAN_PRICE, json_object_get_int_member(terms, "plan_price_id"), error);
			if (!price) return NULL;
			g_object_get(price, "amount", &amount, NULL);
			event = mail_fact(events, "charge_due", subscription, NULL, amount);
			set_time(event, "due_at", due);
		}
		if (*error) return NULL;
		if (!mail_installment(database, billing, subscription, events, now, error)) return NULL;
	}
	if (cancellation) {
		JsonObject *cancel = json_node_get_object(cancellation);
		g_autoptr(GDateTime) at = venture_time_from_string(json_object_get_string_member(cancel, "cancelled_at"), error);
		JsonObject *event;
		if (!at) return NULL;
		event = mail_fact(events, "cancelled", business, at, NULL);
		json_object_set_string_member(event, "state", json_object_get_string_member(cancel, "state"));
		if (!JSON_NODE_HOLDS_NULL(json_object_get_member(cancel, "effective_at")))
			json_object_set_string_member(event, "due_at", json_object_get_string_member(cancel, "effective_at"));
	}
	if (guarantee) {
		JsonObject *record = json_node_get_object(guarantee);
		g_autoptr(VentureMoney) credit = enrollment_money(record, "credit", error);
		g_autoptr(GDateTime) at = venture_time_from_string(json_object_get_string_member(record, "measured_at"), error);
		JsonObject *event;
		if (!credit || !at) return NULL;
		event = mail_fact(events, "guarantee", business, at, credit);
		json_object_set_string_member(event, "state", !json_object_get_boolean_member(record, "eligible") ? "ineligible" :
			venture_money_get_amount(credit) > 0 ? "credited" : "met");
	}
	if (company_id && (!mail_receipts(database, billing, company_id, events, error) ||
		!mail_failures(database, billing, company_id, events, error))) return NULL;
	result = json_object_new();
	json_object_set_int_member(result, "organization_id", business);
	json_object_set_string_member(result, "workspace_id", venture_tenant_service_get_workspace_id(venture_tenant_service_get(database)));
	json_object_set_array_member(result, "events", g_steal_pointer(&events));
	{
		JsonNode *node = json_node_new(JSON_NODE_OBJECT);
		json_node_take_object(node, result);
		return node;
	}
}
