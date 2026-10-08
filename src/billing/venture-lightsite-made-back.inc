/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Kept beside the hosted boundary: reads only one business's retained
 * acquisition and ordinary financial evidence, never current source labels. */
static GPtrArray *
guarantee_rows(VentureDatabase *db, GType type, gint64 organization, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(type);
	venture_query_set_organization(query, organization);
	venture_query_set_include_deleted(query, TRUE);
	venture_query_set_limit(query, 0);
	return venture_database_find(db, query, error);
}

static VentureEntity *
guarantee_row(GPtrArray *rows, gint64 id)
{
	guint i;
	for (i = 0; i < rows->len; i++) {
		VentureEntity *row = g_ptr_array_index(rows, i);
		if (venture_entity_get_id(row) == id) return row;
	}
	return NULL;
}

static gboolean
guarantee_add(VentureMoney **sum, const VentureMoney *amount, GError **error)
{
	VentureMoney *next;
	if (!amount) return refuse(error, VENTURE_ERROR_VALIDATION, "qualifying financial evidence has no exact amount");
	next = venture_money_add(*sum, amount, error);
	if (!next) return FALSE;
	venture_money_free(*sum); *sum = next;
	return TRUE;
}

static gboolean
guarantee_within(GDateTime *date, GDateTime *start, GDateTime *end)
{
	return date && g_date_time_compare(date, start) >= 0 && g_date_time_compare(date, end) < 0;
}

/* Only bindings whose original capture came through a configured hosted site
 * qualify. Analytics consent is independent of a submitted business enquiry. */
static gboolean
guarantee_binding(VentureEntity *binding, GPtrArray *submissions,
	GPtrArray *connections, GDateTime *start, GDateTime *end)
{
	VentureEntity *submission = NULL;
	guint i;
	VentureEntity *connection;
	g_autoptr(GDateTime) captured = NULL;
	g_autofree gchar *provider = NULL;
	g_object_get(binding, "captured-at", &captured, NULL);
	/* The binding points at its latest submission. Qualification belongs to
	 * the first acquisition; a later signed delivery cannot relabel it. */
	for (i = 0; i < submissions->len; i++) {
		VentureEntity *candidate = g_ptr_array_index(submissions, i);
		g_autoptr(GDateTime) submitted = NULL;
		if (number(candidate, "lead-id") != number(binding, "lead-id")) continue;
		g_object_get(candidate, "submitted-at", &submitted, NULL);
		if (!captured || !submitted || g_date_time_compare(submitted, captured)) continue;
		if (!submission || venture_entity_get_id(candidate) < venture_entity_get_id(submission)) submission = candidate;
	}
	if (!submission) return FALSE;
	connection = guarantee_row(connections, number(submission, "connection-id"));
	if (!connection) return FALSE;
	g_object_get(connection, "provider", &provider, NULL);
	return !g_strcmp0(provider, "lightsite_capture") && number(submission, "connection-version") > 0 &&
		guarantee_within(captured, start, end);
}

static VentureEntity *
guarantee_acquisition(GPtrArray *bindings, gint64 deal, gint64 company, GDateTime *event, GError **error)
{
	VentureEntity *found = NULL;
	guint i;
	for (i = 0; i < bindings->len; i++) {
		VentureEntity *binding = g_ptr_array_index(bindings, i);
		g_autoptr(GDateTime) bound = NULL;
		if (deal ? number(binding, "deal-id") != deal : number(binding, "company-id") != company) continue;
		g_object_get(binding, "bound-at", &bound, NULL);
		if (!bound || !event || g_date_time_compare(bound, event) > 0) continue;
		if (found && venture_entity_get_id(found) != venture_entity_get_id(binding)) {
			refuse(error, VENTURE_ERROR_CONFLICT, "financial acquisition is ambiguous"); return NULL;
		}
		found = binding;
	}
	return found;
}

static JsonNode *
made_back_impl(VentureDatabase *database, gint64 billing, gint64 business,
	GDateTime *until, GError **error)
{
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(JsonNode) publication = NULL, enrollment = NULL, result = NULL;
	g_autoptr(GDateTime) now = venture_time_now(), start = NULL, cutoff = NULL, end = NULL;
	g_autoptr(GPtrArray) bindings = NULL, submissions = NULL, connections = NULL;
	g_autoptr(GPtrArray) deals = NULL, quotes = NULL, events = NULL, qualified = g_ptr_array_new();
	g_autoptr(GHashTable) won_by_deal = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, (GDestroyNotify)venture_money_free);
	g_autoptr(GHashTable) invoiced_by_deal = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, (GDestroyNotify)venture_money_free);
	g_autoptr(VentureMoney) cap = NULL, won = NULL, invoiced = NULL, overlap = NULL, total = NULL, gross = NULL;
	GHashTableIter iterator;
	gpointer key, value;
	JsonObject *object;
	guint i;
	if (!prepare(database, billing, NULL, error)) return NULL;
	internal = venture_access_policy_enter(venture_database_get_access_policy(database), NULL);
	if (!business_valid(database, billing, business, error)) return NULL;
	if (!venture_entity_registry_is_type_enabled(venture_entity_registry_get_default(), "attribution_binding")) {
		refuse(error, VENTURE_ERROR_VALIDATION, "attribution evidence is unavailable"); return NULL;
	}
	publication = enrollment_read(database, billing, business, "publish", error);
	if (error && *error) return NULL;
	enrollment = enrollment_read(database, billing, business, "enroll", error);
	if (error && *error) return NULL;
	if (!publication || !enrollment) {
		refuse(error, VENTURE_ERROR_CONFLICT, "business has not gone live"); return NULL;
	}
	start = venture_time_from_string(json_object_get_string_member(json_node_get_object(publication), "go_live_at"), error);
	if (!start) return NULL;
	cutoff = g_date_time_add_days(start, 90);
	end = g_date_time_ref(until ? until : g_date_time_compare(now, cutoff) < 0 ? now : cutoff);
	if (g_date_time_compare(end, now) > 0 || g_date_time_compare(end, cutoff) > 0 || g_date_time_compare(end, start) < 0) {
		refuse(error, VENTURE_ERROR_VALIDATION, "measurement end must be inside the elapsed guarantee period"); return NULL;
	}
	cap = enrollment_money(json_node_get_object(enrollment), "guarantee_cap", error);
	if (!cap) return NULL;
	won = venture_money_new_zero(venture_money_get_currency(cap));
	invoiced = venture_money_copy(won); overlap = venture_money_copy(won);
#define ROWS(variable, type) variable = guarantee_rows(database, type, business, error); if (!variable) return NULL
	ROWS(bindings, VENTURE_TYPE_ATTRIBUTION_BINDING);
	ROWS(submissions, VENTURE_TYPE_ATTRIBUTION_SUBMISSION);
	ROWS(connections, VENTURE_TYPE_INTEGRATION_CONNECTION);
	ROWS(deals, VENTURE_TYPE_DEAL);
	ROWS(quotes, VENTURE_TYPE_QUOTE);
	ROWS(events, VENTURE_TYPE_INVOICE_EVENT);
#undef ROWS
	for (i = 0; i < bindings->len; i++) {
		VentureEntity *binding = g_ptr_array_index(bindings, i);
		if (guarantee_binding(binding, submissions, connections, start, end)) g_ptr_array_add(qualified, binding);
	}
	for (i = 0; i < deals->len; i++) {
		VentureEntity *deal = g_ptr_array_index(deals, i);
		g_autoptr(GDateTime) closed = NULL;
		g_autoptr(VentureMoney) amount = NULL;
		gint stage = 0;
		gint64 id = venture_entity_get_id(deal);
		g_object_get(deal, "stage", &stage, "closed-at", &closed, "value", &amount, NULL);
		if (stage != VENTURE_DEAL_STAGE_WON || !guarantee_within(closed, start, end)) continue;
		if (!guarantee_acquisition(qualified, id, number(deal, "company-id"), closed, error)) {
			if (error && *error) return NULL;
			continue;
		}
		if (!amount || venture_money_get_amount(amount) < 0) {
			refuse(error, VENTURE_ERROR_VALIDATION, "won value is missing or negative"); return NULL;
		}
		if (!guarantee_add(&won, amount, error)) return NULL;
		g_hash_table_insert(won_by_deal, g_memdup2(&id, sizeof id), venture_money_copy(amount));
	}
	for (i = 0; i < events->len; i++) {
		VentureEntity *event = g_ptr_array_index(events, i);
		g_autofree gchar *kind = NULL;
		g_autoptr(GDateTime) date = NULL;
		g_autoptr(VentureMoney) amount = NULL, signed_amount = NULL;
		gint64 invoice = number(event, "invoice-id"), deal = 0;
		guint j;
		g_object_get(event, "kind", &kind, "net-amount", &amount, NULL);
		/* A book date can be midnight on the day publication happened.
		 * The retained event's creation instant proves when issuance/voiding
		 * actually occurred, independent of that accounting calendar. */
		if (venture_entity_get_created_at(event)) date = g_date_time_ref(venture_entity_get_created_at(event));
		if ((g_strcmp0(kind, "issue") && g_strcmp0(kind, "void")) || !guarantee_within(date, start, end)) continue;
		for (j = 0; j < quotes->len; j++) {
			VentureEntity *quote = g_ptr_array_index(quotes, j);
			gint64 candidate = number(quote, "deal-id");
			if (number(quote, "invoice-id") != invoice || !candidate) continue;
			if (deal && candidate != deal) {
				refuse(error, VENTURE_ERROR_CONFLICT, "invoice has conflicting deal evidence"); return NULL;
			}
			deal = candidate;
		}
		if (!guarantee_acquisition(qualified, deal, number(event, "customer-id"), date, error)) {
			if (error && *error) return NULL;
			continue;
		}
		if (!amount) { refuse(error, VENTURE_ERROR_VALIDATION, "qualifying invoice has no frozen net evidence"); return NULL; }
		signed_amount = venture_money_multiply_int(amount, !g_strcmp0(kind, "void") ? -1 : 1, error);
		if (!signed_amount || !guarantee_add(&invoiced, signed_amount, error)) return NULL;
		if (deal) {
			VentureMoney *previous = g_hash_table_lookup(invoiced_by_deal, &deal);
			VentureMoney *sum = previous ? venture_money_add(previous, signed_amount, error) : venture_money_copy(signed_amount);
			if (!sum) return NULL;
			g_hash_table_replace(invoiced_by_deal, g_memdup2(&deal, sizeof deal), sum);
		}
	}
	g_hash_table_iter_init(&iterator, won_by_deal);
	while (g_hash_table_iter_next(&iterator, &key, &value)) {
		VentureMoney *issued = g_hash_table_lookup(invoiced_by_deal, key);
		if (issued && venture_money_get_amount(issued) > 0 &&
			!guarantee_add(&overlap, venture_money_compare(value, issued) < 0 ? value : issued, error)) return NULL;
	}
	gross = venture_money_add(won, invoiced, error);
	if (!gross) return NULL;
	total = venture_money_subtract(gross, overlap, error);
	if (!total) return NULL;
	object = json_object_new(); result = json_node_new(JSON_NODE_OBJECT); json_node_take_object(result, object);
	json_object_set_int_member(object, "organization_id", business);
	set_time(object, "go_live_at", start); set_time(object, "until", end);
	json_object_set_member(object, "won", money_node(won));
	json_object_set_member(object, "invoiced", money_node(invoiced));
	json_object_set_member(object, "overlap", money_node(overlap));
	json_object_set_member(object, "made_back", money_node(total));
	return g_steal_pointer(&result);
}

JsonNode *
venture_lightsite_billing_made_back(VentureDatabase *database, gint64 billing, gint64 business,
	GDateTime *until, GError **error)
{
	g_autoptr(GError) local = NULL;
	g_autoptr(JsonNode) result = NULL;
	if (!prepare(database, billing, NULL, error)) return NULL;
	if (!venture_database_begin_serializable(database, error)) return NULL;
	result = made_back_impl(database, billing, business, until, &local);
	if (!result) {
		venture_database_rollback(database);
		if (!local) refuse(&local, VENTURE_ERROR_FAILED, "measurement could not be read");
		g_propagate_error(error, g_steal_pointer(&local));
		return NULL;
	}
	if (!venture_database_commit(database, error)) return NULL;
	return g_steal_pointer(&result);
}
