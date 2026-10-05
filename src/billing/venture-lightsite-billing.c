/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
#include <string.h>

/*
 * Lightsite billing: the operator billing each Lightsite customer business from
 * the operator's own Venture organization, the billing organization.
 *
 * Two organizations are involved and they must not be confused. The
 * *business* is the customer's own organization (made by the sign-up); it
 * keeps its own books and connects its own Stripe. The *billing
 * organization* is where the operator's plans, prices, invoices and Stripe
 * connection live, and where the business appears as an ordinary customer
 * company whose external ID points back at the business. Nothing here
 * writes to the business's organization, and every subscription change is
 * an ordinary instruction through VentureBillingService, so the invoices,
 * proration, dunning and Stripe collection are the existing ones.
 *
 * The authority is the trusted-service one the sign-up uses, judged in the
 * tenant service; nothing here suspends anything -- an overdue invoice or a
 * failed payment is reported, not acted on.
 */

#define LIGHTSITE_REFERENCE_PREFIX "lightsite:organization:"
#define LIGHTSITE_WRITING_KEY "venture-lightsite-billing-writing"
/* Past this many customers the overview refuses rather than truncating:
 * a staff list that silently stops is a customer nobody chases. */
#define LIGHTSITE_MAX_CUSTOMERS 2000

static const gchar *const plan_codes[] = { "team", "growth", "starter" };

static gboolean
refuse(GError **error, VentureError code, const gchar *message)
{
	g_set_error(error, VENTURE_ERROR, code, "Lightsite billing: %s", message);
	return FALSE;
}

static gint64
number(VentureEntity *entity, const gchar *name)
{
	gint64 value = 0;
	g_object_get(entity, name, &value, NULL);
	return value;
}

static const gchar *
enum_nick(VentureEntity *entity, const gchar *name)
{
	GParamSpec *spec = g_object_class_find_property(G_OBJECT_GET_CLASS(entity), name);
	gint value = 0;
	if (!spec) return NULL;
	g_object_get(entity, name, &value, NULL);
	return venture_enum_to_nick(G_PARAM_SPEC_VALUE_TYPE(spec), value);
}

static gboolean
key_valid(const gchar *key)
{
	gsize i, length = key ? strlen(key) : 0;
	if (length < 1 || length > 128) return FALSE;
	for (i = 0; i < length; i++)
		if (!g_ascii_isalnum(key[i]) && !strchr("._:-", key[i])) return FALSE;
	return TRUE;
}

static gboolean
plan_code_valid(const gchar *code)
{
	guint i;
	for (i = 0; i < G_N_ELEMENTS(plan_codes); i++)
		if (g_strcmp0(code, plan_codes[i]) == 0) return TRUE;
	return FALSE;
}

/* Exact decimal in the amount's own exponent, without its code. */
static gchar *
decimal(const VentureMoney *money)
{
	g_autofree gchar *text = venture_money_to_string(money);
	gchar *space = text ? strrchr(text, ' ') : NULL;
	if (space) *space = '\0';
	return g_strdup(text);
}

static JsonNode *
money_node(const VentureMoney *money)
{
	JsonNode *node = json_node_new(JSON_NODE_OBJECT);
	JsonObject *object = json_object_new();
	g_autofree gchar *amount = decimal(money);
	json_object_set_string_member(object, "amount", amount);
	json_object_set_string_member(object, "currency", venture_money_get_currency(money));
	json_node_take_object(node, object);
	return node;
}

static void
set_time(JsonObject *object, const gchar *member, GDateTime *when)
{
	g_autofree gchar *text = when ? venture_time_to_string(when) : NULL;
	if (text) json_object_set_string_member(object, member, text);
	else json_object_set_null_member(object, member);
}

static VentureEntity *
get_record(VentureDatabase *database, GType type, gint64 id, GError **error)
{
	g_autoptr(GError) local = NULL;
	VentureEntity *record = id > 0 ? venture_database_get(database, type, id, &local) : NULL;
	if (local) { g_propagate_error(error, g_steal_pointer(&local)); return NULL; }
	return record;
}

/* An organization this workspace has, live: deleted and inactive ones are
 * not there as far as billing is concerned. */
static VentureEntity *
live_organization(VentureDatabase *database, gint64 id, const gchar *what, GError **error)
{
	g_autoptr(GError) local = NULL;
	g_autoptr(VentureEntity) organization = get_record(database, VENTURE_TYPE_ORGANIZATION, id, &local);
	gboolean active = FALSE;
	if (local && !g_error_matches(local, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND)) {
		g_propagate_error(error, g_steal_pointer(&local));
		return NULL;
	}
	if (organization) g_object_get(organization, "active", &active, NULL);
	if (!organization || !active || venture_entity_is_deleted(organization)) {
		refuse(error, VENTURE_ERROR_NOT_FOUND, what);
		return NULL;
	}
	return g_steal_pointer(&organization);
}

static GPtrArray *
find(VentureDatabase *database, VentureQuery *query, GError **error)
{
	return venture_database_find(database, query, error);
}

/* The customer company standing for @organization_id in the billing
 * organization, or NULL with no error when there is none yet. */
static VentureEntity *
customer_company(VentureDatabase *database, gint64 billing, gint64 organization_id, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_COMPANY);
	g_autoptr(GPtrArray) rows = NULL;
	g_autofree gchar *reference = g_strdup_printf(LIGHTSITE_REFERENCE_PREFIX "%" G_GINT64_FORMAT, organization_id);
	venture_query_set_organization(query, billing);
	venture_query_add_filter_string(query, "external-id", VENTURE_FILTER_OP_EQ, reference, NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	rows = find(database, query, error);
	if (!rows || !rows->len) return NULL;
	return g_object_ref(g_ptr_array_index(rows, 0));
}

static gboolean
status_live(gint status)
{
	/* trialing, active, past_due, paused: the subscription still exists. */
	return status >= 0 && status <= 3;
}

/* The company's live subscription, else its latest one, else NULL. */
static VentureEntity *
current_subscription(VentureDatabase *database, gint64 billing, gint64 company_id, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_CUSTOMER_SUBSCRIPTION);
	g_autoptr(GPtrArray) rows = NULL;
	VentureEntity *live = NULL, *latest = NULL;
	guint i;
	venture_query_set_organization(query, billing);
	venture_query_add_filter_int(query, "company-id", VENTURE_FILTER_OP_EQ, company_id, NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	rows = find(database, query, error);
	if (!rows) return NULL;
	for (i = 0; i < rows->len; i++) {
		VentureEntity *row = g_ptr_array_index(rows, i);
		gint status = 0;
		g_object_get(row, "status", &status, NULL);
		latest = row;
		if (status_live(status)) live = row;
	}
	if (live) return g_object_ref(live);
	return latest ? g_object_ref(latest) : NULL;
}

/* The one active monthly price of the plan coded @code in the billing
 * organization. Two would be a guess, so they are refused. */
static VentureEntity *
monthly_price(VentureDatabase *database, gint64 billing, const gchar *code, GError **error)
{
	g_autoptr(VentureQuery) plans = venture_query_new(VENTURE_TYPE_PLAN);
	g_autoptr(VentureQuery) prices = NULL;
	g_autoptr(GPtrArray) rows = NULL, price_rows = NULL;
	VentureEntity *plan = NULL, *chosen = NULL;
	guint i, found = 0;
	venture_query_set_organization(plans, billing);
	venture_query_add_filter_string(plans, "code", VENTURE_FILTER_OP_EQ, code, NULL);
	rows = find(database, plans, error);
	if (!rows) return NULL;
	for (i = 0; i < rows->len; i++) {
		gboolean active = FALSE;
		g_object_get(g_ptr_array_index(rows, i), "active", &active, NULL);
		if (active) plan = g_ptr_array_index(rows, i);
	}
	if (!plan) {
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
			"Lightsite billing: the billing organization does not offer the %s plan", code);
		return NULL;
	}
	prices = venture_query_new(VENTURE_TYPE_PLAN_PRICE);
	venture_query_set_organization(prices, billing);
	venture_query_add_filter_int(prices, "plan-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(plan), NULL);
	price_rows = find(database, prices, error);
	if (!price_rows) return NULL;
	for (i = 0; i < price_rows->len; i++) {
		VentureEntity *price = g_ptr_array_index(price_rows, i);
		gboolean active = FALSE;
		g_object_get(price, "active", &active, NULL);
		if (!active || g_strcmp0(enum_nick(price, "interval"), "month") != 0) continue;
		chosen = price;
		found++;
	}
	if (found == 0) {
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
			"Lightsite billing: the %s plan has no active monthly price", code);
		return NULL;
	}
	if (found > 1) {
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT,
			"Lightsite billing: the %s plan has more than one active monthly price; retire all but one", code);
		return NULL;
	}
	return g_object_ref(chosen);
}

static gboolean
stripe_connected(VentureDatabase *database, gint64 organization_id, gboolean *connected, GError **error)
{
	g_autoptr(GError) local = NULL;
	g_autoptr(VentureIntegrationConnection) connection = venture_integration_service_find(
		venture_integration_service_get(database), organization_id, "stripe", &local);
	*connected = connection != NULL;
	/* Unconfigured, ambiguous or the integrations module off all mean "not
	 * connected"; only a storage failure is a failure. */
	if (!connection && local && !g_error_matches(local, VENTURE_ERROR, VENTURE_ERROR_CONFIG) &&
	    !g_error_matches(local, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED) &&
	    !g_error_matches(local, VENTURE_ERROR, VENTURE_ERROR_VALIDATION)) {
		g_propagate_error(error, g_steal_pointer(&local));
		return FALSE;
	}
	return TRUE;
}

/* The issued total of an invoice, from its issue event; NULL when it was
 * never issued. */
static VentureMoney *
issued_total(VentureDatabase *database, gint64 invoice_id, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_INVOICE_EVENT);
	g_autoptr(GPtrArray) rows = NULL;
	guint i;
	venture_query_add_filter_int(query, "invoice-id", VENTURE_FILTER_OP_EQ, invoice_id, NULL);
	rows = find(database, query, error);
	if (!rows) return NULL;
	for (i = 0; i < rows->len; i++) {
		g_autofree gchar *kind = NULL;
		VentureMoney *amount = NULL;
		g_object_get(g_ptr_array_index(rows, i), "kind", &kind, NULL);
		if (g_strcmp0(kind, "issue") != 0) continue;
		g_object_get(g_ptr_array_index(rows, i), "amount", &amount, NULL);
		return amount;
	}
	return NULL;
}

/* The invoices half of the view: whether any is overdue -- open and a
 * whole day past due, the receivables aging's rule -- and the latest. */
static gboolean
invoice_facts(VentureDatabase *database, gint64 billing, gint64 company_id, gboolean *overdue,
	JsonNode **last, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_INVOICE);
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	VentureSettlementService *settlement = venture_settlement_service_get(database);
	VentureEntity *latest = NULL;
	guint i;
	*overdue = FALSE;
	*last = NULL;
	venture_query_set_organization(query, billing);
	venture_query_add_filter_int(query, "company-id", VENTURE_FILTER_OP_EQ, company_id, NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	rows = find(database, query, error);
	if (!rows) return FALSE;
	for (i = 0; i < rows->len; i++) {
		VentureEntity *invoice = g_ptr_array_index(rows, i);
		g_autoptr(VentureMoney) balance = NULL;
		g_autoptr(GDateTime) due = NULL;
		const gchar *status = enum_nick(invoice, "status");
		latest = invoice;
		if (*overdue || g_strcmp0(status, "draft") == 0 || g_strcmp0(status, "void") == 0) continue;
		g_object_get(invoice, "due-at", &due, NULL);
		if (!due || g_date_time_difference(now, due) / G_TIME_SPAN_DAY < 1) continue;
		balance = venture_settlement_service_invoice_balance(settlement, venture_entity_get_id(invoice), NULL, error);
		if (!balance) return FALSE;
		if (venture_money_get_amount(balance) > 0) *overdue = TRUE;
	}
	if (latest) {
		g_autoptr(VentureMoney) total = NULL;
		g_autoptr(GDateTime) due = NULL, paid = NULL;
		g_autofree gchar *invoice_number = NULL;
		g_autofree gchar *total_text = NULL;
		JsonObject *object = json_object_new();
		g_autoptr(GError) local = NULL;
		total = issued_total(database, venture_entity_get_id(latest), &local);
		if (local) { g_propagate_error(error, g_steal_pointer(&local)); json_object_unref(object); return FALSE; }
		g_object_get(latest, "number", &invoice_number, "due-at", &due, "paid-at", &paid, NULL);
		json_object_set_string_member(object, "number", invoice_number ? invoice_number : "");
		json_object_set_string_member(object, "status", enum_nick(latest, "status"));
		if (total) {
			total_text = decimal(total);
			json_object_set_string_member(object, "total", total_text);
		} else
			json_object_set_null_member(object, "total");
		set_time(object, "due_date", due);
		set_time(object, "paid_at", paid);
		*last = json_node_new(JSON_NODE_OBJECT);
		json_node_take_object(*last, object);
	}
	return TRUE;
}

/* The view of one business, given its customer company when known.
 * @company NULL means the business has none yet: nothing on, nothing owed. */
static JsonNode *
build_view(VentureDatabase *database, gint64 billing, gint64 organization_id, VentureEntity *company, GError **error)
{
	g_autoptr(VentureEntity) subscription = NULL, price = NULL, plan = NULL;
	g_autoptr(VentureMoney) owed = NULL;
	g_autoptr(GDateTime) period_end = NULL, past_due_at = NULL;
	g_autofree gchar *currency = NULL;
	JsonNode *last = NULL, *node;
	JsonObject *object;
	gboolean overdue = FALSE, failed = FALSE, connected = FALSE;
	const gchar *state = "none";
	if (company) {
		subscription = current_subscription(database, billing, venture_entity_get_id(company), error);
		if (!subscription && error && *error) return NULL;
	}
	if (subscription) {
		price = get_record(database, VENTURE_TYPE_PLAN_PRICE, number(subscription, "plan-price-id"), error);
		if (!price) return NULL;
		plan = get_record(database, VENTURE_TYPE_PLAN, number(price, "plan-id"), error);
		if (!plan) return NULL;
		g_object_get(price, "currency", &currency, NULL);
		g_object_get(subscription, "current-period-end", &period_end, "past-due-at", &past_due_at, NULL);
		state = enum_nick(subscription, "status");
		failed = g_strcmp0(state, "past_due") == 0 || past_due_at != NULL;
	}
	if (!currency) currency = venture_database_get_book_currency(database, billing);
	if (company) {
		owed = venture_settlement_service_customer_balance(venture_settlement_service_get(database), billing,
			venture_entity_get_id(company), NULL, currency, error);
		if (!owed) return NULL;
		if (!invoice_facts(database, billing, venture_entity_get_id(company), &overdue, &last, error)) return NULL;
	} else
		owed = venture_money_new_zero(currency);
	if (!stripe_connected(database, organization_id, &connected, error)) {
		if (last) json_node_unref(last);
		return NULL;
	}
	object = json_object_new();
	json_object_set_int_member(object, "organization_id", organization_id);
	if (plan) {
		JsonObject *about = json_object_new();
		g_autofree gchar *code = NULL, *name = NULL, *amount_text = NULL;
		g_autoptr(VentureMoney) amount = NULL;
		g_object_get(plan, "code", &code, "name", &name, NULL);
		g_object_get(price, "amount", &amount, NULL);
		json_object_set_string_member(about, "code", code ? code : "");
		json_object_set_string_member(about, "name", name ? name : "");
		if (amount) {
			amount_text = decimal(amount);
			json_object_set_string_member(about, "amount", amount_text);
		} else
			json_object_set_null_member(about, "amount");
		json_object_set_string_member(about, "currency", currency);
		json_object_set_string_member(about, "interval", enum_nick(price, "interval"));
		json_object_set_object_member(object, "plan", about);
	} else
		json_object_set_null_member(object, "plan");
	json_object_set_string_member(object, "state", state);
	set_time(object, "current_period_end", period_end);
	json_object_set_member(object, "owed", money_node(owed));
	json_object_set_boolean_member(object, "overdue", overdue);
	json_object_set_boolean_member(object, "failed_payment", failed);
	if (last) json_object_set_member(object, "last_invoice", last);
	else json_object_set_null_member(object, "last_invoice");
	json_object_set_boolean_member(object, "stripe_connected", connected);
	node = json_node_new(JSON_NODE_OBJECT);
	json_node_take_object(node, object);
	return node;
}

/* Authority first, then whether billing is on: an anonymous or ordinary
 * caller learns nothing about how this install is configured. */
static gboolean
prepare(VentureDatabase *database, gint64 billing, gint64 *actor_id, GError **error)
{
	g_autoptr(VentureEntity) organization = NULL;
	g_autoptr(VentureAccessScope) internal = NULL;
	if (!venture_tenant_service_check_trusted_service(venture_tenant_service_get(database), actor_id, error))
		return FALSE;
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "customer_subscription") == G_TYPE_INVALID)
		return refuse(error, VENTURE_ERROR_NOT_FOUND, "the billing module is off");
	if (billing <= 0)
		return refuse(error, VENTURE_ERROR_NOT_FOUND, "no billing organization is configured");
	internal = venture_access_policy_enter(venture_database_get_access_policy(database), NULL);
	organization = live_organization(database, billing, "the configured billing organization is unavailable", error);
	return organization != NULL;
}

static gboolean
business_valid(VentureDatabase *database, gint64 billing, gint64 organization_id, GError **error)
{
	g_autoptr(VentureEntity) organization = NULL;
	if (organization_id <= 0)
		return refuse(error, VENTURE_ERROR_VALIDATION, "organization_id must be a positive integer");
	if (organization_id == billing)
		return refuse(error, VENTURE_ERROR_VALIDATION, "the billing organization does not bill itself");
	organization = live_organization(database, organization_id, "no such business organization", error);
	return organization != NULL;
}

static JsonNode *
view_impl(VentureDatabase *database, gint64 billing_organization_id, gint64 organization_id,
	GError **error)
{
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(VentureEntity) company = NULL;
		if (!prepare(database, billing_organization_id, NULL, error)) return NULL;
	internal = venture_access_policy_enter(venture_database_get_access_policy(database), NULL);
	if (!business_valid(database, billing_organization_id, organization_id, error)) return NULL;
	company = customer_company(database, billing_organization_id, organization_id, error);
	if (!company && error && *error) return NULL;
	return build_view(database, billing_organization_id, organization_id, company, error);
}

static gint
by_business(gconstpointer a, gconstpointer b)
{
	gint64 x = json_object_get_int_member(json_node_get_object(*(JsonNode *const *)a), "organization_id");
	gint64 y = json_object_get_int_member(json_node_get_object(*(JsonNode *const *)b), "organization_id");
	return x < y ? -1 : (x > y ? 1 : 0);
}

static JsonNode *
overview_impl(VentureDatabase *database, gint64 billing_organization_id, GError **error)
{
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) companies = NULL;
	g_autoptr(GPtrArray) views = g_ptr_array_new_with_free_func((GDestroyNotify)json_node_unref);
	g_autoptr(GHashTable) seen = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	JsonArray *customers;
	JsonObject *object;
	JsonNode *node;
	guint i;
		if (!prepare(database, billing_organization_id, NULL, error)) return NULL;
	internal = venture_access_policy_enter(venture_database_get_access_policy(database), NULL);
	/* The LIKE is a superset (an underscore matches anything); the prefix
	 * and the number are checked exactly below. */
	query = venture_query_new(VENTURE_TYPE_COMPANY);
	venture_query_set_organization(query, billing_organization_id);
	venture_query_add_filter_string(query, "external-id", VENTURE_FILTER_OP_LIKE, LIGHTSITE_REFERENCE_PREFIX "%", NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	companies = find(database, query, error);
	if (!companies) return NULL;
	for (i = 0; i < companies->len; i++) {
		VentureEntity *company = g_ptr_array_index(companies, i);
		g_autoptr(VentureEntity) subscription = NULL;
		g_autofree gchar *reference = NULL;
		gint64 organization_id = 0;
		JsonNode *view;
		g_object_get(company, "external-id", &reference, NULL);
		if (!reference || !g_str_has_prefix(reference, LIGHTSITE_REFERENCE_PREFIX) ||
		    !g_ascii_string_to_signed(reference + strlen(LIGHTSITE_REFERENCE_PREFIX), 10, 1, G_MAXINT64, &organization_id, NULL) ||
		    g_hash_table_contains(seen, &organization_id))
			continue;
		subscription = current_subscription(database, billing_organization_id, venture_entity_get_id(company), error);
		if (!subscription) {
			if (error && *error) return NULL;
			continue;
		}
		if (views->len >= LIGHTSITE_MAX_CUSTOMERS) {
			refuse(error, VENTURE_ERROR_VALIDATION, "more Lightsite customers than one overview answers");
			return NULL;
		}
		g_hash_table_add(seen, g_memdup2(&organization_id, sizeof organization_id));
		view = build_view(database, billing_organization_id, organization_id, company, error);
		if (!view) return NULL;
		g_ptr_array_add(views, view);
	}
	g_ptr_array_sort(views, by_business);
	customers = json_array_new();
	for (i = 0; i < views->len; i++)
		json_array_add_element(customers, json_node_copy(g_ptr_array_index(views, i)));
	object = json_object_new();
	json_object_set_array_member(object, "customers", customers);
	node = json_node_new(JSON_NODE_OBJECT);
	json_node_take_object(node, object);
	return node;
}

/* Length-prefixed fields in a fixed order, so no two different requests
 * share a canonical form. */
static gchar *
request_hash(const gchar *key, gint64 organization_id, const gchar *plan_code)
{
	g_autoptr(GChecksum) checksum = g_checksum_new(G_CHECKSUM_SHA256);
	g_autofree gchar *id = g_strdup_printf("%" G_GINT64_FORMAT, organization_id);
	const gchar *fields[3];
	guint i;
	fields[0] = key; fields[1] = id; fields[2] = plan_code;
	g_checksum_update(checksum, (const guchar *)"lightsite-billing-subscription/v1\n", -1);
	for (i = 0; i < G_N_ELEMENTS(fields); i++) {
		g_autofree gchar *prefix = g_strdup_printf("%" G_GSIZE_FORMAT ":", strlen(fields[i]));
		g_checksum_update(checksum, (const guchar *)prefix, -1);
		g_checksum_update(checksum, (const guchar *)fields[i], -1);
		g_checksum_update(checksum, (const guchar *)"\n", 1);
	}
	return g_strdup(g_checksum_get_string(checksum));
}

static VentureEntity *
find_receipt(VentureDatabase *database, gint64 billing, const gchar *key, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_LIGHTSITE_BILLING_RECEIPT);
	g_autoptr(GPtrArray) rows = NULL;
	venture_query_set_organization(query, billing);
	venture_query_set_include_deleted(query, TRUE);
	venture_query_add_filter_string(query, "idempotency-key", VENTURE_FILTER_OP_EQ, key, NULL);
	venture_query_set_limit(query, 1);
	rows = find(database, query, error);
	if (!rows || !rows->len) return NULL;
	return g_object_ref(g_ptr_array_index(rows, 0));
}

/* A receipt answers its own request again, verbatim, and refuses any other. */
static JsonNode *
replay(VentureEntity *receipt, const gchar *hash, GError **error)
{
	g_autofree gchar *stored = NULL, *result = NULL;
	g_object_get(receipt, "request-hash", &stored, "result", &result, NULL);
	if (g_strcmp0(stored, hash) != 0) {
		refuse(error, VENTURE_ERROR_CONFLICT, "idempotency_key was already used for a different request");
		return NULL;
	}
	return venture_json_parse(result, error);
}

gboolean
venture_lightsite_billing_check_save(VentureDatabase *database, VentureEntity *record, GError **error)
{
	if (g_object_get_data(G_OBJECT(database), LIGHTSITE_WRITING_KEY) == record) return TRUE;
	return refuse(error, VENTURE_ERROR_VALIDATION, "receipts are written only by the Lightsite billing operation");
}

static gboolean
save_receipt(VentureDatabase *database, VentureEntity *receipt, const VentureActor *actor, GError **error)
{
	gboolean ok;
	g_object_set_data(G_OBJECT(database), LIGHTSITE_WRITING_KEY, receipt);
	ok = venture_database_save(database, receipt, actor, error);
	g_object_set_data(G_OBJECT(database), LIGHTSITE_WRITING_KEY, NULL);
	return ok;
}

static gboolean
execute(VentureDatabase *database, VentureBillingRequest *request, const VentureActor *actor, GError **error)
{
	return venture_billing_service_execute(venture_billing_service_get(database), request, actor, error);
}

static JsonNode *
subscribe_impl(VentureDatabase *database, gint64 billing_organization_id, gint64 organization_id,
	const gchar *plan_code, const gchar *idempotency_key, gboolean *created, GError **error)
{
	const VentureAuthPrincipal *principal;
	VentureActor audit;
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(VentureAccountingOperation) operation = NULL;
	g_autoptr(VentureEntity) receipt = NULL, price = NULL, company = NULL, subscription = NULL, business = NULL;
	g_autoptr(JsonNode) result = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	g_autofree gchar *hash = NULL, *result_text = NULL, *business_name = NULL;
	const gchar *outcome = "unchanged";
	gint64 actor_id = 0, billing = billing_organization_id, price_id;
	gboolean committed = FALSE;
		if (created) *created = FALSE;
	if (!key_valid(idempotency_key)) {
		refuse(error, VENTURE_ERROR_VALIDATION, "idempotency_key must be 1-128 of A-Z a-z 0-9 . _ : -");
		return NULL;
	}
	if (!plan_code_valid(plan_code)) {
		refuse(error, VENTURE_ERROR_VALIDATION, "plan_code must be team, growth or starter");
		return NULL;
	}
	if (organization_id <= 0) {
		refuse(error, VENTURE_ERROR_VALIDATION, "organization_id must be a positive integer");
		return NULL;
	}
	if (!prepare(database, billing, &actor_id, error)) return NULL;
	principal = venture_access_policy_get_actor(venture_database_get_access_policy(database));
	audit.kind = VENTURE_ACTOR_KIND_IMPORT;
	audit.name = principal ? principal->name : NULL;
	audit.prompt = NULL;
	audit.request_id = NULL;
	audit.approved_by = NULL;
	hash = request_hash(idempotency_key, organization_id, plan_code);
	internal = venture_access_policy_enter(venture_database_get_access_policy(database), NULL);
	receipt = find_receipt(database, billing, idempotency_key, error);
	if (receipt) return replay(receipt, hash, error);
	if (error && *error) return NULL;
	if (!business_valid(database, billing, organization_id, error)) return NULL;
	business = get_record(database, VENTURE_TYPE_ORGANIZATION, organization_id, error);
	if (!business) return NULL;
	g_object_get(business, "name", &business_name, NULL);
	price = monthly_price(database, billing, plan_code, error);
	if (!price) return NULL;
	price_id = venture_entity_get_id(price);
	company = customer_company(database, billing, organization_id, error);
	if (!company && error && *error) return NULL;
	if (company) {
		subscription = current_subscription(database, billing, venture_entity_get_id(company), error);
		if (!subscription && error && *error) return NULL;
		if (subscription) {
			gint status = 0;
			g_object_get(subscription, "status", &status, NULL);
			if (!status_live(status)) g_clear_object(&subscription);
		}
	}
	if (!subscription)
		outcome = "start";
	else if (number(subscription, "pending-plan-price-id") != 0 ?
	         number(subscription, "pending-plan-price-id") != price_id :
	         number(subscription, "plan-price-id") != price_id)
		outcome = "change";
	/* A first invoice posts money, so consent is bound before the
	 * transaction, as a quote's start does; a scheduled change posts none. */
	if (g_strcmp0(outcome, "start") == 0 && number(price, "trial-days") == 0) {
		GVariantBuilder arguments;
		g_variant_builder_init(&arguments, G_VARIANT_TYPE_VARDICT);
		g_variant_builder_add(&arguments, "{sv}", "organization_id", g_variant_new_int64(organization_id));
		g_variant_builder_add(&arguments, "{sv}", "plan_code", g_variant_new_string(plan_code));
		g_variant_builder_add(&arguments, "{sv}", "idempotency_key", g_variant_new_string(idempotency_key));
		operation = venture_accounting_operation_begin(database, "lightsite-billing.subscribe", NULL, NULL,
			g_variant_builder_end(&arguments), billing, &audit, error);
		if (!operation) return NULL;
	}
	if (!venture_database_begin(database, error)) return NULL;
	/* Answered while this one was being prepared: treat it as the replay. */
	g_clear_object(&receipt);
	receipt = find_receipt(database, billing, idempotency_key, error);
	if (receipt || (error && *error)) goto rollback;
	if (!company) {
		g_autofree gchar *reference = g_strdup_printf(LIGHTSITE_REFERENCE_PREFIX "%" G_GINT64_FORMAT, organization_id);
		company = g_object_new(VENTURE_TYPE_COMPANY, "name", business_name, "external-id", reference,
			"source", "lightsite", "active", TRUE, NULL);
		venture_entity_set_organization_id(company, billing);
		if (!venture_entity_set_field_from_string(company, "kind", "customer", error) ||
		    !venture_database_save(database, company, &audit, error)) goto rollback;
	}
	if (g_strcmp0(outcome, "unchanged") != 0) {
		g_autoptr(VentureBillingRequest) instruction = venture_billing_request_new();
		venture_entity_set_organization_id(VENTURE_ENTITY(instruction), billing);
		if (g_strcmp0(outcome, "start") == 0)
			g_object_set(instruction, "action", "start", "company-id", venture_entity_get_id(company),
				"plan-price-id", price_id, "seats", (gint64)1, "at", now, NULL);
		else
			g_object_set(instruction, "action", "change", "subscription-id", venture_entity_get_id(subscription),
				"plan-price-id", price_id, "at-period-end", TRUE, "at", now,
				"expected-version", venture_entity_get_version(subscription), NULL);
		if (!execute(database, instruction, &audit, error)) goto rollback;
		if (!subscription) {
			subscription = get_record(database, VENTURE_TYPE_CUSTOMER_SUBSCRIPTION, number(VENTURE_ENTITY(instruction), "subscription-id"), error);
			if (!subscription) goto rollback;
		}
	}
	result = build_view(database, billing, organization_id, company, error);
	if (!result) goto rollback;
	result_text = venture_json_to_string(result, FALSE);
	receipt = g_object_new(VENTURE_TYPE_LIGHTSITE_BILLING_RECEIPT, "idempotency-key", idempotency_key,
		"request-hash", hash, "business-id", organization_id, "plan-code", plan_code, "outcome", outcome,
		"company-id", venture_entity_get_id(company), "subscription-id", subscription ? venture_entity_get_id(subscription) : (gint64)0,
		"result", result_text, "actor-user-id", actor_id, NULL);
	venture_entity_set_organization_id(receipt, billing);
	if (!save_receipt(database, receipt, &audit, error)) goto rollback;
	committed = venture_database_commit(database, error);
	if (!committed) return NULL;
	if (operation && !venture_accounting_operation_finish(operation, error)) return NULL;
	if (created) *created = TRUE;
	return g_steal_pointer(&result);
rollback:
	venture_database_rollback(database);
	if (receipt && !(error && *error)) return replay(receipt, hash, error);
	return NULL;
}

/* Internally a NULL result with no error means "none", so every entry
 * point hands the work a real error location and propagates it after. */
JsonNode *
venture_lightsite_billing_view(VentureDatabase *database, gint64 billing_organization_id, gint64 organization_id,
	GError **error)
{
	g_autoptr(GError) local = NULL;
	JsonNode *result;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	result = view_impl(database, billing_organization_id, organization_id, &local);
	if (!result) {
		if (!local) refuse(&local, VENTURE_ERROR_FAILED, "the billing view could not be read");
		g_propagate_error(error, g_steal_pointer(&local));
	}
	return result;
}

JsonNode *
venture_lightsite_billing_overview(VentureDatabase *database, gint64 billing_organization_id, GError **error)
{
	g_autoptr(GError) local = NULL;
	JsonNode *result;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	result = overview_impl(database, billing_organization_id, &local);
	if (!result) {
		if (!local) refuse(&local, VENTURE_ERROR_FAILED, "the billing overview could not be read");
		g_propagate_error(error, g_steal_pointer(&local));
	}
	return result;
}

JsonNode *
venture_lightsite_billing_subscribe(VentureDatabase *database, gint64 billing_organization_id, gint64 organization_id,
	const gchar *plan_code, const gchar *idempotency_key, gboolean *created, GError **error)
{
	g_autoptr(GError) local = NULL;
	JsonNode *result;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	result = subscribe_impl(database, billing_organization_id, organization_id, plan_code, idempotency_key, created, &local);
	if (!result) {
		if (!local) refuse(&local, VENTURE_ERROR_FAILED, "the instruction did not complete");
		g_propagate_error(error, g_steal_pointer(&local));
	}
	return result;
}
