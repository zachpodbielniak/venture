/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <stripe-glib.h>
#include <math.h>
#include <string.h>

struct _VentureStripeService
{
	GObject parent_instance;
	VentureDatabase *database;
	StripeClient *client;
	StripeTransport *transport;
	gint64 organization_id;
	gint64 connection_id;
	gint64 connection_version;
	gchar *account_id;
	gchar *environment;
	gchar *connection_uuid;
	gchar *yaml;
	gchar *success_url;
	gchar *cancel_url;
};
G_DEFINE_TYPE(VentureStripeService, venture_stripe_service, G_TYPE_OBJECT)
enum { PROP_0, PROP_DATABASE, PROP_ORGANIZATION, N_PROPS };

/* Sorting equality warns and returns zero on rescaling overflow. Financial
 * validation must instead propagate failure through checked arithmetic. */
static gboolean
money_equal(const VentureMoney *a, const VentureMoney *b)
{
	g_autoptr(VentureMoney) difference = NULL;
	if (!a || !b) return FALSE;
	difference = venture_money_subtract(a, b, NULL);
	return difference && venture_money_get_amount(difference) == 0;
}

/* Stripe charge units differ from ISO for MGA and the legacy ISK/UGX
 * representation. Use the same mapper for Prices and signed settlement data;
 * https://docs.stripe.com/currencies documents these API exceptions. */
static guint8
charge_exponent(const gchar *currency)
{
	static const gchar *const zero[] = { "BIF", "CLP", "DJF", "GNF", "JPY", "KMF", "KRW", "MGA", "PYG", "RWF", "VND", "VUV", "XAF", "XOF", "XPF" };
	guint8 exponent = venture_currency_get_exponent(currency);
	guint i;
	for (i = 0; i < G_N_ELEMENTS(zero); i++)
		if (!g_ascii_strcasecmp(currency, zero[i])) return 0;
	if (!g_ascii_strcasecmp(currency, "ISK") || !g_ascii_strcasecmp(currency, "UGX"))
		return 2;
	return exponent;
}

static VentureMoney *
charge_money(gint64 amount, const gchar *currency)
{
	guint8 exponent = charge_exponent(currency);
	if ((!g_ascii_strcasecmp(currency, "ISK") || !g_ascii_strcasecmp(currency, "UGX")) && amount % 100)
		return NULL;
	return venture_money_new(amount, currency, exponent);
}

static gboolean
refuse(GError **error, const gchar *rule)
{
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, rule);
	return FALSE;
}

static gboolean
to_stripe_amount(const VentureMoney *money, gint64 *out, GError **error)
{
	guint8 from = venture_money_get_exponent(money);
	guint8 to = charge_exponent(venture_money_get_currency(money));
	gint64 amount = venture_money_get_amount(money);
	gint64 factor = 1;
	gint64 scaled;

	if (from == to)
	{
		if (amount <= 0) return refuse(error, "Checkout requires a positive open balance");
		*out = amount;
		return TRUE;
	}
	if (to > from)
	{
		while (from < to)
		{
			if (__builtin_mul_overflow(factor, (gint64)10, &factor))
				return refuse(error, "Checkout amount overflows Stripe charge units");
			from++;
		}
		if (__builtin_mul_overflow(amount, factor, &scaled))
			return refuse(error, "Checkout amount overflows Stripe charge units");
		if (scaled <= 0) return refuse(error, "Checkout requires a positive open balance");
		*out = scaled;
		return TRUE;
	}
	while (to < from)
	{
		if (__builtin_mul_overflow(factor, (gint64)10, &factor))
			return refuse(error, "Checkout amount overflows Stripe charge units");
		to++;
	}
	if (amount % factor)
		return refuse(error, "Checkout amount is not an integral Stripe charge unit");
	scaled = amount / factor;
	if (scaled <= 0) return refuse(error, "Checkout requires a positive open balance");
	*out = scaled;
	return TRUE;
}

static void
set_property(GObject *object, guint id, const GValue *value, GParamSpec *pspec)
{
	VentureStripeService *self = VENTURE_STRIPE_SERVICE(object);
	if (id == PROP_DATABASE) self->database = g_value_dup_object(value);
	else if (id == PROP_ORGANIZATION) self->organization_id = g_value_get_int64(value);
	else G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
}

static void
get_property(GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
	VentureStripeService *self = VENTURE_STRIPE_SERVICE(object);
	if (id == PROP_DATABASE) g_value_set_object(value, self->database);
	else if (id == PROP_ORGANIZATION) g_value_set_int64(value, self->organization_id);
	else G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
}

static void
finalize(GObject *object)
{
	VentureStripeService *self = VENTURE_STRIPE_SERVICE(object);
	g_clear_object(&self->client);
	g_clear_object(&self->transport);
	g_clear_object(&self->database);
	g_free(self->account_id);
	g_free(self->environment);
	g_free(self->connection_uuid);
	g_free(self->yaml);
	g_free(self->success_url);
	g_free(self->cancel_url);
	G_OBJECT_CLASS(venture_stripe_service_parent_class)->finalize(object);
}

static void
venture_stripe_service_class_init(VentureStripeServiceClass *klass)
{
	GObjectClass *oc = G_OBJECT_CLASS(klass);
	oc->finalize = finalize;
	oc->set_property = set_property;
	oc->get_property = get_property;
	g_object_class_install_property(oc, PROP_DATABASE, g_param_spec_object("database", "Database", "Provider storage", VENTURE_TYPE_DATABASE, G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
	g_object_class_install_property(oc, PROP_ORGANIZATION, g_param_spec_int64("organization-id", "Organization", "Endpoint legal entity", 1, G_MAXINT64, 1, G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
}
static void venture_stripe_service_init(VentureStripeService *self) { (void)self; }

typedef struct
{
	gchar *name;
	JsonObject *payload;
} Observation;

static void
observation_free(gpointer data)
{
	Observation *observation = data;
	g_free(observation->name);
	json_object_unref(observation->payload);
	g_free(observation);
}

static void
collect_event(StripeClient *client, const gchar *name, JsonObject *payload, gpointer data)
{
	Observation *observation = g_new0(Observation, 1);
	(void)client;
	observation->name = g_strdup(name);
	observation->payload = json_object_ref(payload);
	g_ptr_array_add(data, observation);
}

static void
audit_event(StripeClient *client, const gchar *name, JsonObject *payload, gpointer data)
{
	VentureStripeService *self = data;
	g_autoptr(VentureAuditEntry) entry = venture_audit_entry_new();
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);
	g_autofree gchar *json = NULL;
	(void)client;
	json_node_set_object(node, payload);
	json = venture_json_to_string(node, FALSE);
	g_object_set(entry, "actor", "stripe", "source", name,
		"occurred-at", now, "diff", json, NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(entry), self->organization_id);
	venture_database_save(self->database, VENTURE_ENTITY(entry), NULL, NULL);
}

#include "venture-stripe-settings.inc"

VentureStripeService *
venture_stripe_service_new(VentureDatabase *database, gint64 organization_id,
	StripeTransport *transport, GError **error)
{
	static const gchar *const names[] = { "VENTURE_STRIPE_SECRET_KEY", "VENTURE_STRIPE_PUBLISHABLE_KEY", "VENTURE_STRIPE_WEBHOOK_SECRET", "VENTURE_STRIPE_API_VERSION", "VENTURE_STRIPE_SUCCESS_URL", "VENTURE_STRIPE_CANCEL_URL" };
	const gchar *values[G_N_ELEMENTS(names)];
	g_autoptr(VentureStripeService) self = NULL;
	guint i;

	if (transport == NULL)
		return venture_stripe_service_for_organization(database, organization_id, NULL, error);
	for (i = 0; i < G_N_ELEMENTS(names); i++)
	{
		values[i] = g_getenv(names[i]);
		if (!values[i] || !*values[i])
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG, "Stripe module requires %s", names[i]);
			return NULL;
		}
		/* These values are tokens or HTTPS URLs, never YAML fragments. */
		if (strchr(values[i], '\n') || strchr(values[i], '\r') || strchr(values[i], '\''))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG, "Invalid %s", names[i]);
			return NULL;
		}
	}
	if (!g_str_has_prefix(values[4], "https://") || !g_str_has_prefix(values[5], "https://"))
	{
		refuse(error, "Stripe success and cancel URLs require HTTPS");
		return NULL;
	}
	self = g_object_new(VENTURE_TYPE_STRIPE_SERVICE, "database", database, "organization-id", organization_id, NULL);
	self->yaml = g_strdup_printf("secret_key: '%s'\npublishable_key: '%s'\nwebhook_signing_secrets: ['%s']\napi_version: '%s'\n", values[0], values[1], values[2], values[3]);
	self->success_url = g_strdup(values[4]);
	self->cancel_url = g_strdup(values[5]);
	if (transport) self->transport = g_object_ref(transport);
	self->client = stripe_client_new(self->yaml, transport, NULL, error);
	if (!self->client) return NULL;
	g_signal_connect(self->client, "event", G_CALLBACK(audit_event), self);
	return g_steal_pointer(&self);
}

static GPtrArray *
find_rows(VentureStripeService *self, GType type, const gchar *field,
	const gchar *value, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(type);
	venture_query_set_limit(query, 0);
	{
		g_autoptr(VentureEntityClass) klass = g_type_class_ref(type);
		if (g_object_class_find_property(G_OBJECT_CLASS(klass), "connection-id") &&
			!venture_query_add_filter_int(query, "connection-id", VENTURE_FILTER_OP_EQ, self->connection_id, error)) return NULL;
	}
	if (!venture_query_add_filter_int(query, "organization-id", VENTURE_FILTER_OP_EQ, self->organization_id, error) ||
	    !venture_query_add_filter_string(query, field, VENTURE_FILTER_OP_EQ, value, error)) return NULL;
	return venture_database_find(self->database, query, error);
}

static GPtrArray *
find_id(VentureStripeService *self, GType type, const gchar *field, gint64 id, GError **error)
{
	g_autofree gchar *text = g_strdup_printf("%" G_GINT64_FORMAT, id);
	return find_rows(self, type, field, text, error);
}

static VentureEntity *
owned_get(VentureStripeService *self, GType type, gint64 id, GError **error)
{
	VentureEntity *entity = venture_database_get(self->database, type, id, error);
	/* Historical references stay editable, but must not create new provider
	 * objects for a customer or invoice that the operator has removed. */
	if (entity && venture_entity_is_deleted(entity))
	{
		g_object_unref(entity);
		refuse(error, "Stripe record has been deleted");
		return NULL;
	}
	if (entity && venture_entity_get_organization_id(entity) != self->organization_id)
	{
		g_object_unref(entity);
		refuse(error, "Stripe record must belong to the endpoint organization");
		return NULL;
	}
	if (entity && g_object_class_find_property(G_OBJECT_GET_CLASS(entity), "connection-id"))
	{
		gint64 connection = 0;
		g_object_get(entity, "connection-id", &connection, NULL);
		if (connection != self->connection_id)
		{
			g_object_unref(entity);
			refuse(error, "Stripe record belongs to another account binding");
			return NULL;
		}
	}
	if (entity && type == VENTURE_TYPE_PAYMENT && self->connection_uuid)
	{
		g_autofree gchar *identity = NULL;
		g_autofree gchar *prefix = g_strdup_printf("stripe:%s:", self->connection_uuid);
		g_object_get(entity, "external-id", &identity, NULL);
		if (!identity || !g_str_has_prefix(identity, prefix))
		{
			g_object_unref(entity);
			refuse(error, "Stripe receipt belongs to another account binding");
			return NULL;
		}
	}
	return entity;
}

static gboolean
eligible(VentureStripeService *self, gint64 invoice_id, VentureEntity **invoice_out,
	VentureEntity **link_out, guint *quantity_out, VentureMoney **expected_out, GError **error)
{
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(GPtrArray) lines = NULL;
	g_autoptr(VentureMoney) balance = NULL;
	gint status;

	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "stripe_checkout") == G_TYPE_INVALID)
		return refuse(error, "Stripe module is disabled (stripe.enabled)");
	if (!stripe_refresh(self, FALSE, error)) return FALSE;
	invoice = owned_get(self, VENTURE_TYPE_INVOICE, invoice_id, error);
	if (!invoice) return FALSE;
	g_object_get(invoice, "status", &status, NULL);
	if (status != VENTURE_INVOICE_STATUS_SENT && status != VENTURE_INVOICE_STATUS_PARTIALLY_PAID)
		return refuse(error, "Checkout requires invoice status sent");
	lines = find_id(self, VENTURE_TYPE_INVOICE_LINE, "invoice-id", invoice_id, error);
	if (!lines) return FALSE;
	if (lines->len < 1) return refuse(error, "Checkout requires at least one invoice line");
	balance = venture_settlement_service_invoice_balance(venture_settlement_service_get(self->database), invoice_id, NULL, error);
	if (!balance) return FALSE;
	if (venture_money_get_amount(balance) <= 0)
		return refuse(error, "Checkout requires a positive open balance");
	if (invoice_out) *invoice_out = g_steal_pointer(&invoice);
	if (link_out) *link_out = NULL;
	if (quantity_out) *quantity_out = 1;
	if (expected_out) *expected_out = g_steal_pointer(&balance);
	return TRUE;
}

gboolean
venture_stripe_service_can_checkout(VentureStripeService *self, gint64 invoice_id, GError **error)
{
	return eligible(self, invoice_id, NULL, NULL, NULL, NULL, error);
}

/* A local product link identifies a Price; it does not establish its amount.
 * Validate the immutable provider price before either external write. Refuse
 * quantity transforms, recurring/tiered pricing and fractional minor units
 * because this checkout has no exact representation for those contracts. */
static gboolean G_GNUC_UNUSED
check_price(VentureStripeService *self, VentureEntity *price, guint quantity,
	const VentureMoney *expected, GError **error)
{
	g_autoptr(StripeRequest) request = stripe_request_new(STRIPE_PRICE_RETRIEVE);
	g_autoptr(StripeResource) resource = NULL;
	g_autoptr(JsonObject) object = NULL;
	g_autoptr(StripeMoney) remote = NULL;
	g_autoptr(VentureMoney) unit = NULL;
	g_autoptr(VentureMoney) total = NULL;
	JsonNode *active, *transform;

	g_object_get(price, "stripe-price-id", &request->id, NULL);
	resource = stripe_client_execute(self->client, request, NULL, error);
	if (!resource) return FALSE;
	object = stripe_resource_get_json(resource);
	active = json_object_get_member(object, "active");
	transform = json_object_get_member(object, "transform_quantity");
	if (!active || json_node_get_value_type(active) != G_TYPE_BOOLEAN || !json_node_get_boolean(active) ||
	    g_strcmp0(stripe_resource_get_string(resource, "id"), request->id) ||
	    g_strcmp0(stripe_resource_get_string(resource, "type"), "one_time") ||
	    g_strcmp0(stripe_resource_get_string(resource, "billing_scheme"), "per_unit") ||
	    (transform && !JSON_NODE_HOLDS_NULL(transform)))
		return refuse(error, "Checkout requires an active one-time per-unit Stripe Price without quantity transforms");
	remote = stripe_resource_get_money(resource, STRIPE_UNIT_AMOUNT, error);
	if (!remote) return FALSE;
	unit = charge_money(stripe_money_get_amount(remote), stripe_money_get_currency(remote));
	if (unit) total = venture_money_multiply_int(unit, quantity, NULL);
	if (!total || !money_equal(total, expected))
		return refuse(error, "Stripe Price times quantity must exactly equal the invoice open balance and currency");
	return TRUE;
}

VentureStripeCheckout *
venture_stripe_service_checkout(VentureStripeService *self, gint64 invoice_id,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(VentureEntity) customer = NULL;
	g_autoptr(GPtrArray) links = NULL;
	g_autoptr(GPtrArray) sessions = NULL;
	g_autoptr(VentureMoney) expected = NULL;
	g_autoptr(StripeRequest) request = NULL;
	g_autoptr(StripeResource) resource = NULL;
	g_autoptr(VentureStripeCheckout) checkout = NULL;
	g_autofree gchar *customer_id = NULL;
	gint64 company_id, contact_id, party_id;
	const gchar *party_field;

	if (!venture_database_begin(self->database, error)) return NULL;
	if (!eligible(self, invoice_id, &invoice, NULL, NULL, &expected, error)) goto fail;
	/* A remote customer/session cannot be undone by rolling back the local
	 * transaction. Check the invoice write policy before contacting Stripe. */
	if (!venture_access_policy_check_write(venture_database_get_access_policy(self->database), invoice, "write", error)) goto fail;
	/* Replacing credentials must never create a second payment link while an
	 * earlier account (including legacy unbound rows) can still collect. */
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_STRIPE_CHECKOUT);
		g_autoptr(GPtrArray) prior = NULL;
		guint i;
		venture_query_set_limit(query, 0);
		if (!venture_query_add_filter_int(query, "organization-id", VENTURE_FILTER_OP_EQ, self->organization_id, error) ||
			!venture_query_add_filter_int(query, "invoice-id", VENTURE_FILTER_OP_EQ, invoice_id, error)) goto fail;
		prior = venture_database_find(self->database, query, error);
		if (!prior) goto fail;
		for (i = 0; i < prior->len; i++)
		{
			gint64 binding;
			g_autofree gchar *status = NULL;
			g_object_get(g_ptr_array_index(prior, i), "connection-id", &binding, "status", &status, NULL);
			if (binding != self->connection_id && !g_strcmp0(status, "open"))
			{
				refuse(error, "A pending Checkout belongs to an earlier Stripe account; reconcile it before creating another");
				goto fail;
			}
		}
	}
	sessions = find_id(self, venture_stripe_checkout_get_type(), "invoice-id", invoice_id, error);
	if (!sessions) goto fail;
	if (sessions->len)
	{
		g_autofree gchar *status_name = NULL;
		checkout = g_object_ref(g_ptr_array_index(sessions, sessions->len - 1));
		g_object_get(checkout, "status", &status_name, NULL);
		if (g_strcmp0(status_name, "expired") != 0)
		{
			if (!venture_database_commit(self->database, error)) goto fail;
			return g_steal_pointer(&checkout);
		}
		g_clear_object(&checkout);
	}
	g_object_get(invoice, "company-id", &company_id, "contact-id", &contact_id, NULL);
	party_id = company_id ? company_id : contact_id;
	party_field = company_id ? "company-id" : "contact-id";
	customer = owned_get(self, company_id ? VENTURE_TYPE_COMPANY : VENTURE_TYPE_CONTACT, party_id, error);
	if (!customer) goto fail;
	links = find_id(self, venture_stripe_customer_link_get_type(), party_field, party_id, error);
	if (!links) goto fail;
	if (links->len) g_object_get(g_ptr_array_index(links, 0), "stripe-customer-id", &customer_id, NULL);
	else
	{
		g_autoptr(VentureStripeCustomerLink) link = venture_stripe_customer_link_new();
		request = stripe_request_new(STRIPE_CUSTOMER_CREATE);
		request->name = venture_entity_get_display_name(customer);
		request->metadata_key = g_strdup(company_id ? "venture_company_uuid" : "venture_contact_uuid");
		request->metadata_value = g_strdup(venture_entity_get_uuid(customer));
		g_free(request->idempotency_key);
		request->idempotency_key = g_strdup_printf("venture-customer-%s-%s",
			self->connection_uuid ? self->connection_uuid : "fixture", venture_entity_get_uuid(customer));
		resource = stripe_client_execute(self->client, request, NULL, error);
		if (!resource) goto fail;
		customer_id = g_strdup(stripe_resource_get_string(resource, "id"));
		g_object_set(link, party_field, party_id, "stripe-customer-id", customer_id, "connection-id", self->connection_id, NULL);
		venture_entity_set_organization_id(VENTURE_ENTITY(link), self->organization_id);
		if (!venture_stripe_save_owned(self->database, VENTURE_ENTITY(link), actor, error)) goto fail;
		g_clear_pointer(&request, stripe_request_free);
		g_clear_pointer(&resource, stripe_resource_free);
	}
	request = stripe_request_new(STRIPE_CHECKOUT_CREATE);
	request->customer = g_steal_pointer(&customer_id);
	{
		gint64 amount = 0;
		if (!to_stripe_amount(expected, &amount, error)) goto fail;
		if (amount <= 0)
		{
			refuse(error, "Checkout requires a positive open balance");
			goto fail;
		}
		request->unit_amount = (guint64)amount;
	}
	request->currency = g_ascii_strdown(venture_money_get_currency(expected), -1);
	request->product_name = g_strdup("Invoice");
	request->quantity = 1;
	request->success_url = g_strdup(self->success_url);
	request->cancel_url = g_strdup(self->cancel_url);
	g_free(request->idempotency_key);
	request->idempotency_key = g_strdup_printf("venture-invoice-%s-%s-%" G_GINT64_FORMAT,
		self->connection_uuid ? self->connection_uuid : "fixture", venture_entity_get_uuid(invoice), venture_entity_get_version(invoice));
	resource = stripe_client_execute(self->client, request, NULL, error);
	if (!resource) goto fail;
	checkout = venture_stripe_checkout_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(checkout), self->organization_id);
	g_object_set(checkout, "connection-id", self->connection_id, "invoice-id", invoice_id, "session-id", stripe_resource_get_string(resource, "id"),
		"url", stripe_resource_get_string(resource, "url"), "status", "open", "expected", expected, NULL);
	if (!venture_stripe_save_owned(self->database, VENTURE_ENTITY(checkout), actor, error)) goto fail;
	if (!venture_database_commit(self->database, error)) goto fail;
	return g_steal_pointer(&checkout);
fail:
	venture_database_rollback(self->database);
	return NULL;
}

/* Typed accessors prevent malformed authenticated events from triggering GLib assertions. */
static const gchar *
string_member(JsonObject *object, const gchar *name)
{
	JsonNode *node = object ? json_object_get_member(object, name) : NULL;
	return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING ? json_node_get_string(node) : NULL;
}
static JsonObject *
object_member(JsonObject *object, const gchar *name)
{
	JsonNode *node = object ? json_object_get_member(object, name) : NULL;
	return node && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
}

gboolean
venture_stripe_service_handle_webhook(VentureStripeService *self, GBytes *raw,
	const gchar *signature, GError **error)
{
	g_autoptr(StripeClient) verifier = NULL;
	g_autoptr(GPtrArray) observations = g_ptr_array_new_with_free_func(observation_free);
	guint observation_index;
	g_autoptr(JsonObject) event = NULL;
	g_autoptr(GPtrArray) prior = NULL;
	g_autoptr(GPtrArray) sessions = NULL;
	g_autoptr(VentureStripeEvent) record = NULL;
	g_autoptr(VentureMoney) expected = NULL;
	g_autoptr(VentureMoney) received = NULL;
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(VenturePayment) payment = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	g_autofree gchar *payload = NULL;
	const gchar *event_id, *type, *session_id, *intent, *currency;
	JsonObject *object;
	JsonNode *amount_node;
	VentureEntity *checkout;
	gint64 invoice_id, company_id;
	gboolean mismatch = FALSE;

	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "stripe_checkout") == G_TYPE_INVALID)
		return refuse(error, "Stripe module is disabled (stripe.enabled)");
	if (!stripe_refresh(self, TRUE, error)) return FALSE;
	if (!venture_database_begin(self->database, error)) return FALSE;
	/* Durable event IDs replace the library's per-client replay cache. Each
	 * delivery is authenticated afresh, including identical signed retries. */
	verifier = stripe_client_new(self->yaml, self->transport, NULL, error);
	if (!verifier) goto fail;
	g_signal_connect(verifier, "event", G_CALLBACK(collect_event), observations);
	event = stripe_client_verify_webhook(verifier, raw, signature, NULL, error);
	if (!event)
	{
		for (observation_index = 0; observation_index < observations->len; observation_index++)
		{
			Observation *observation = g_ptr_array_index(observations, observation_index);
			audit_event(verifier, observation->name, observation->payload, self);
		}
		/* Rejection evidence contains no raw body or signature. */
		venture_database_commit(self->database, NULL);
		return FALSE;
	}
	if (self->connection_id > 0)
	{
		JsonNode *live = json_object_get_member(event, "livemode");
		const gchar *account = string_member(event, "account");
		if (!live || json_node_get_value_type(live) != G_TYPE_BOOLEAN ||
			json_node_get_boolean(live) != !g_strcmp0(self->environment, "live") ||
			(json_object_has_member(event, "account") && !account) ||
			(account && g_strcmp0(account, self->account_id)))
		{
			refuse(error, "Stripe event account or environment does not match its binding");
			goto fail;
		}
	}
	event_id = string_member(event, "id");
	type = string_member(event, "type");
	if (!event_id || !*event_id || !type) { refuse(error, "Webhook requires event id and type"); goto fail; }
	prior = find_rows(self, venture_stripe_event_get_type(), "event-id", event_id, error);
	if (!prior) goto fail;
	if (prior->len)
	{
		/* Do not forward observations for a duplicate: no audit side effects. */
		venture_database_rollback(self->database);
		return TRUE;
	}
	for (observation_index = 0; observation_index < observations->len; observation_index++)
	{
		Observation *observation = g_ptr_array_index(observations, observation_index);
		audit_event(verifier, observation->name, observation->payload, self);
	}
	record = venture_stripe_event_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(record), self->organization_id);
	payload = g_strndup(g_bytes_get_data(raw, NULL), g_bytes_get_size(raw));
	g_object_set(record, "connection-id", self->connection_id, "event-id", event_id, "type", type, "received-at", now,
		"payload", payload, "result", "ignored", NULL);
	if (!venture_stripe_save_owned(self->database, VENTURE_ENTITY(record), NULL, error)) goto fail;
	if (g_strcmp0(type, "checkout.session.completed")) goto done;
	object = object_member(object_member(event, "data"), "object");
	session_id = string_member(object, "id");
	if (!session_id) { refuse(error, "Checkout event requires session id"); goto fail; }
	sessions = find_rows(self, venture_stripe_checkout_get_type(), "session-id", session_id, error);
	if (!sessions) goto fail;
	if (!sessions->len) goto done;
	checkout = g_ptr_array_index(sessions, 0);
	g_object_get(checkout, "expected", &expected, "invoice-id", &invoice_id, NULL);
	amount_node = json_object_get_member(object, "amount_total");
	currency = string_member(object, "currency");
	intent = string_member(object, "payment_intent");
	if (expected && amount_node && json_node_get_value_type(amount_node) == G_TYPE_INT64 &&
	    currency && !g_ascii_strcasecmp(currency, venture_money_get_currency(expected)))
		received = charge_money(json_node_get_int(amount_node), currency);
	if (!received || !money_equal(received, expected))
	{
		mismatch = TRUE;
		g_object_set(record, "result", "mismatch", NULL);
		goto done;
	}
	if (g_strcmp0(string_member(object, "payment_status"), "paid") || !intent || !g_str_has_prefix(intent, "pi_"))
	{ refuse(error, "Checkout completion requires paid status and payment_intent"); goto fail; }
	{
		gint64 payment_id;
		g_object_get(checkout, "payment-id", &payment_id, NULL);
		if (payment_id) goto done;
	}
	invoice = owned_get(self, VENTURE_TYPE_INVOICE, invoice_id, error);
	if (!invoice) goto fail;
	g_object_get(invoice, "company-id", &company_id, NULL);
	payment = venture_payment_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(payment), self->organization_id);
	{
		g_autofree gchar *identity = self->connection_uuid ?
			g_strdup_printf("stripe:%s:%s", self->connection_uuid, intent) : g_strdup(intent);
		g_object_set(payment, "customer-id", company_id, "invoice-id", invoice_id,
			"amount", expected, "date", now, "method", "stripe", "reference", intent, "external-id", identity, NULL);
	}
	if (!venture_settlement_service_apply_payment(venture_settlement_service_get(self->database), payment, NULL, NULL, error)) goto fail;
	g_object_set(checkout, "status", "complete", "payment-id", venture_entity_get_id(VENTURE_ENTITY(payment)), NULL);
	if (!venture_stripe_save_owned(self->database, checkout, NULL, error)) goto fail;
	g_object_set(record, "result", "processed", NULL);
done:
	g_object_set(record, "processed-at", now, NULL);
	if (!venture_stripe_save_owned(self->database, VENTURE_ENTITY(record), NULL, error)) goto fail;
	if (!venture_database_commit(self->database, error)) goto fail;
	return mismatch ? refuse(error, "Stripe amount or currency mismatch with expected payment") : TRUE;
fail:
	venture_database_rollback(self->database);
	return FALSE;
}

static gint64
account_code(VentureStripeService *self, const gchar *code, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ACCOUNT);
	g_autoptr(VentureEntity) account = NULL;
	venture_query_set_organization(query, self->organization_id);
	if (!venture_query_add_filter_string(query, "code", VENTURE_FILTER_OP_EQ, code, error))
		return 0;
	account = venture_database_find_one(self->database, query, error);
	if (account == NULL) return 0;
	return venture_entity_get_id(account);
}

static gboolean
ensure_disputed(VentureSettlementService *settlement, GError **error)
{
	VentureInvoiceStateMachine *machine = venture_settlement_service_get_state_machine(settlement);
	g_autoptr(GError) local = NULL;
	if (!venture_invoice_state_machine_add_state(machine, "disputed", VENTURE_INVOICE_STATUS_SENT, &local))
	{
		if (local == NULL || local->code != VENTURE_ERROR_ALREADY_EXISTS)
		{
			g_propagate_error(error, g_steal_pointer(&local));
			return FALSE;
		}
		g_clear_error(&local);
	}
	venture_invoice_state_machine_add_transition(machine, "sent", "disputed", NULL);
	venture_invoice_state_machine_add_transition(machine, "partially_paid", "disputed", NULL);
	venture_invoice_state_machine_add_transition(machine, "paid", "disputed", NULL);
	venture_invoice_state_machine_add_transition(machine, "disputed", "sent", NULL);
	venture_invoice_state_machine_add_transition(machine, "disputed", "partially_paid", NULL);
	venture_invoice_state_machine_add_transition(machine, "disputed", "paid", NULL);
	return TRUE;
}

VentureProcessorPayout *
venture_stripe_service_record_payout(VentureStripeService *self, const gchar *provider_id,
	GDateTime *date, const VentureMoney *gross, const VentureMoney *fees, const VentureMoney *net,
	gint64 cash_account_id, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureAccountingOperation) operation = NULL;
	g_autofree gchar *date_text = date ? g_date_time_format_iso8601(date) : g_strdup("");
	g_autofree gchar *gross_text = gross ? venture_money_to_string(gross) : g_strdup("");
	g_autofree gchar *fees_text = fees ? venture_money_to_string(fees) : g_strdup("");
	g_autofree gchar *net_text = net ? venture_money_to_string(net) : g_strdup("");
	g_autoptr(VentureProcessorPayout) payout = NULL;
	g_autoptr(GPtrArray) entries = NULL;
	if (provider_id == NULL || *provider_id == '\0' || date == NULL || net == NULL)
	{
		refuse(error, "A payout needs a provider id, date and net amount");
		return NULL;
	}
	if (fees != NULL && !venture_money_is_zero(fees) && cash_account_id > 0)
	{
		operation = venture_accounting_operation_begin(self->database, "stripe.payout", NULL, NULL,
			g_variant_new("(sssssx)", provider_id ? provider_id : "", date_text, gross_text, fees_text, net_text, cash_account_id), self->organization_id, actor, error);
		if (operation == NULL) return NULL;
	}
	if (!venture_database_begin(self->database, error)) return NULL;
	payout = venture_processor_payout_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(payout), self->organization_id);
	g_object_set(payout, "connection-id", self->connection_id, "provider-id", provider_id, "date", date, "gross", gross, "fees", fees,
		"amount", net, "bank-account-id", cash_account_id, "status", "paid", NULL);
	if (!venture_stripe_save_owned(self->database, VENTURE_ENTITY(payout), actor, error)) goto fail;
	if (fees != NULL && !venture_money_is_zero(fees) && cash_account_id > 0)
	{
		gint64 fee_account = account_code(self, "6000", error);
		g_autofree gchar *transaction = g_strdup_printf("processor:payout:%s", venture_entity_get_uuid(VENTURE_ENTITY(payout)));
		VentureLedgerEntry *debit, *credit;
		if (fee_account == 0) goto fail;
		entries = g_ptr_array_new_with_free_func(g_object_unref);
		debit = venture_ledger_entry_new();
		credit = venture_ledger_entry_new();
		g_object_set(debit, "transaction-id", transaction, "account-id", fee_account,
			"side", VENTURE_LEDGER_SIDE_DEBIT, "amount", fees, "occurred-at", date,
			"source-type", "processor_payout", "source-id", venture_entity_get_id(VENTURE_ENTITY(payout)), NULL);
		g_object_set(credit, "transaction-id", transaction, "account-id", cash_account_id,
			"side", VENTURE_LEDGER_SIDE_CREDIT, "amount", fees, "occurred-at", date,
			"source-type", "processor_payout", "source-id", venture_entity_get_id(VENTURE_ENTITY(payout)), NULL);
		venture_entity_set_organization_id(VENTURE_ENTITY(debit), self->organization_id);
		venture_entity_set_organization_id(VENTURE_ENTITY(credit), self->organization_id);
		g_ptr_array_add(entries, debit);
		g_ptr_array_add(entries, credit);
		if (!venture_posting_service_post_entries(venture_database_get_posting_service(self->database),
			entries, NULL, actor, error)) goto fail;
	}
	if (!venture_database_commit(self->database, error)) goto fail;
	if (operation != NULL && !venture_accounting_operation_finish(operation, error)) return NULL;
	return g_steal_pointer(&payout);
fail:
	venture_database_rollback(self->database);
	return NULL;
}

gboolean
venture_stripe_service_link_payout_item(VentureStripeService *self, gint64 payout_id, gint64 payment_id,
	const VentureMoney *amount, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureProcessorPayoutItem) item = NULL;
	g_autoptr(VentureEntity) payout = NULL;
	g_autoptr(VentureEntity) payment = NULL;
	if (!venture_database_begin(self->database, error)) return FALSE;
	payout = owned_get(self, VENTURE_TYPE_PROCESSOR_PAYOUT, payout_id, error);
	payment = owned_get(self, VENTURE_TYPE_PAYMENT, payment_id, error);
	if (payout == NULL || payment == NULL) goto fail;
	item = venture_processor_payout_item_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(item), self->organization_id);
	g_object_set(item, "payout-id", payout_id, "payment-id", payment_id, "amount", amount, NULL);
	if (!venture_stripe_save_owned(self->database, VENTURE_ENTITY(item), actor, error)) goto fail;
	if (!venture_database_commit(self->database, error)) goto fail;
	return TRUE;
fail:
	venture_database_rollback(self->database);
	return FALSE;
}

VentureProcessorDispute *
venture_stripe_service_open_dispute(VentureStripeService *self, const gchar *provider_id,
	gint64 payment_id, GDateTime *date, const VentureMoney *amount, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureProcessorDispute) dispute = NULL;
	g_autoptr(VentureEntity) payment = NULL;
	g_autoptr(VentureEntity) invoice = NULL;
	VentureSettlementService *settlement;
	gint64 invoice_id = 0;
	if (!venture_database_begin(self->database, error)) return NULL;
	payment = owned_get(self, VENTURE_TYPE_PAYMENT, payment_id, error);
	if (payment == NULL) goto fail;
	g_object_get(payment, "invoice-id", &invoice_id, NULL);
	settlement = venture_settlement_service_get(self->database);
	if (!ensure_disputed(settlement, error)) goto fail;
	if (invoice_id > 0)
	{
		invoice = owned_get(self, VENTURE_TYPE_INVOICE, invoice_id, error);
		if (invoice == NULL) goto fail;
		if (!venture_settlement_service_transition(settlement, VENTURE_INVOICE(invoice), "disputed", date, actor, error))
			goto fail;
	}
	dispute = venture_processor_dispute_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(dispute), self->organization_id);
	g_object_set(dispute, "connection-id", self->connection_id, "provider-id", provider_id, "payment-id", payment_id, "invoice-id", invoice_id,
		"opened-at", date, "amount", amount, "status", "open", NULL);
	if (!venture_stripe_save_owned(self->database, VENTURE_ENTITY(dispute), actor, error)) goto fail;
	if (!venture_database_commit(self->database, error)) goto fail;
	return g_steal_pointer(&dispute);
fail:
	venture_database_rollback(self->database);
	return NULL;
}

gboolean
venture_stripe_service_lose_chargeback(VentureStripeService *self, gint64 dispute_id, GDateTime *date,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureAccountingOperation) operation = NULL;
	g_autofree gchar *date_text = date ? g_date_time_format_iso8601(date) : g_strdup("");
	g_autoptr(VentureEntity) dispute = NULL;
	g_autoptr(VentureEntity) payment = NULL;
	g_autoptr(GPtrArray) allocations = NULL;
	g_autoptr(VentureRefund) refund = NULL;
	g_autoptr(VentureMoney) amount = NULL;
	g_autofree gchar *status = NULL;
	gint64 payment_id = 0, customer_id = 0;
	operation = venture_accounting_operation_begin(self->database, "stripe.chargeback", NULL, NULL,
		g_variant_new("(xs)", dispute_id, date_text), self->organization_id, actor, error);
	if (operation == NULL) return FALSE;
	if (!venture_database_begin(self->database, error)) return FALSE;
	dispute = owned_get(self, VENTURE_TYPE_PROCESSOR_DISPUTE, dispute_id, error);
	if (dispute == NULL) goto fail;
	/* One dispute may reverse cash once, including a partial chargeback. */
	g_object_get(dispute, "status", &status, NULL);
	if (g_strcmp0(status, "open") != 0)
	{
		refuse(error, "Only an open dispute can become a lost chargeback");
		goto fail;
	}
	g_object_get(dispute, "payment-id", &payment_id, "amount", &amount, NULL);
	payment = owned_get(self, VENTURE_TYPE_PAYMENT, payment_id, error);
	if (payment == NULL) goto fail;
	g_object_get(payment, "customer-id", &customer_id, NULL);
	allocations = find_id(self, VENTURE_TYPE_PAYMENT_ALLOCATION, "payment-id", payment_id, error);
	if (allocations == NULL) goto fail;
	if (allocations->len == 0)
	{
		refuse(error, "Chargeback requires an allocated receipt");
		goto fail;
	}
	refund = venture_refund_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(refund), self->organization_id);
	g_object_set(refund, "customer-id", customer_id,
		"allocation-id", venture_entity_get_id(g_ptr_array_index(allocations, 0)),
		"amount", amount, "date", date, "reference", "chargeback", NULL);
	if (!venture_database_save(self->database, VENTURE_ENTITY(refund), actor, error)) goto fail;
	g_object_set(dispute, "status", "lost", "closed-at", date, "refund-id", venture_entity_get_id(VENTURE_ENTITY(refund)), NULL);
	if (!venture_stripe_save_owned(self->database, dispute, actor, error)) goto fail;
	if (!venture_database_commit(self->database, error)) goto fail;
	if (!venture_accounting_operation_finish(operation, error)) return FALSE;
	return TRUE;
fail:
	venture_database_rollback(self->database);
	return FALSE;
}
