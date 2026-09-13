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
	gchar *yaml;
	gchar *success_url;
	gchar *cancel_url;
};
G_DEFINE_TYPE(VentureStripeService, venture_stripe_service, G_TYPE_OBJECT)
enum { PROP_0, PROP_DATABASE, PROP_ORGANIZATION, N_PROPS };

static gboolean
refuse(GError **error, const gchar *rule)
{
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, rule);
	return FALSE;
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

VentureStripeService *
venture_stripe_service_new(VentureDatabase *database, gint64 organization_id,
	StripeTransport *transport, GError **error)
{
	static const gchar *const names[] = { "VENTURE_STRIPE_SECRET_KEY", "VENTURE_STRIPE_PUBLISHABLE_KEY", "VENTURE_STRIPE_WEBHOOK_SECRET", "VENTURE_STRIPE_API_VERSION", "VENTURE_STRIPE_SUCCESS_URL", "VENTURE_STRIPE_CANCEL_URL" };
	const gchar *values[G_N_ELEMENTS(names)];
	g_autoptr(VentureStripeService) self = NULL;
	guint i;

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
	if (entity && venture_entity_get_organization_id(entity) != self->organization_id)
	{
		g_object_unref(entity);
		refuse(error, "Stripe record must belong to the endpoint organization");
		return NULL;
	}
	return entity;
}

static gboolean
eligible(VentureStripeService *self, gint64 invoice_id, VentureEntity **invoice_out,
	VentureEntity **link_out, guint *quantity_out, VentureMoney **expected_out, GError **error)
{
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(GPtrArray) lines = NULL;
	g_autoptr(GPtrArray) links = NULL;
	g_autoptr(VentureMoney) amount = NULL;
	g_autoptr(VentureMoney) balance = NULL;
	VentureEntity *line;
	gint status;
	gint64 product;
	gdouble quantity;

	invoice = owned_get(self, VENTURE_TYPE_INVOICE, invoice_id, error);
	if (!invoice) return FALSE;
	g_object_get(invoice, "status", &status, NULL);
	if (status != VENTURE_INVOICE_STATUS_SENT) return refuse(error, "Checkout requires invoice status sent");
	lines = find_id(self, VENTURE_TYPE_INVOICE_LINE, "invoice-id", invoice_id, error);
	if (!lines) return FALSE;
	if (lines->len != 1) return refuse(error, "Checkout requires exactly one invoice line");
	line = g_ptr_array_index(lines, 0);
	g_object_get(line, "product-id", &product, "quantity", &quantity, NULL);
	if (!isfinite(quantity) || quantity < 1 || quantity > G_MAXUINT || floor(quantity) != quantity)
		return refuse(error, "Checkout requires a positive integral line quantity");
	links = find_id(self, venture_stripe_price_link_get_type(), "product-id", product, error);
	if (!links) return FALSE;
	if (links->len != 1) return refuse(error, "Checkout requires a stripe_price_link for the line product");
	amount = venture_invoice_line_get_amount(VENTURE_INVOICE_LINE(line), error);
	if (!amount) return FALSE;
	balance = venture_settlement_service_invoice_balance(venture_settlement_service_get(self->database), invoice_id, NULL, error);
	if (!balance) return FALSE;
	if (venture_money_get_amount(balance) <= 0 || venture_money_get_amount(amount) != venture_money_get_amount(balance) ||
	    g_strcmp0(venture_money_get_currency(amount), venture_money_get_currency(balance)))
		return refuse(error, "Checkout line total must equal the positive open balance");
	if (invoice_out) *invoice_out = g_steal_pointer(&invoice);
	if (link_out) *link_out = g_object_ref(g_ptr_array_index(links, 0));
	if (quantity_out) *quantity_out = (guint)quantity;
	if (expected_out) *expected_out = g_steal_pointer(&balance);
	return TRUE;
}

gboolean
venture_stripe_service_can_checkout(VentureStripeService *self, gint64 invoice_id, GError **error)
{
	return eligible(self, invoice_id, NULL, NULL, NULL, NULL, error);
}

VentureStripeCheckout *
venture_stripe_service_checkout(VentureStripeService *self, gint64 invoice_id,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(VentureEntity) price = NULL;
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
	guint quantity;

	if (!venture_database_begin(self->database, error)) return NULL;
	if (!eligible(self, invoice_id, &invoice, &price, &quantity, &expected, error)) goto fail;
	sessions = find_id(self, venture_stripe_checkout_get_type(), "invoice-id", invoice_id, error);
	if (!sessions) goto fail;
	if (sessions->len)
	{
		checkout = g_object_ref(g_ptr_array_index(sessions, sessions->len - 1));
		if (!venture_database_commit(self->database, error)) goto fail;
		return g_steal_pointer(&checkout);
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
		request->idempotency_key = g_strdup_printf("venture-customer-%s", venture_entity_get_uuid(customer));
		resource = stripe_client_execute(self->client, request, NULL, error);
		if (!resource) goto fail;
		customer_id = g_strdup(stripe_resource_get_string(resource, "id"));
		g_object_set(link, party_field, party_id, "stripe-customer-id", customer_id, NULL);
		venture_entity_set_organization_id(VENTURE_ENTITY(link), self->organization_id);
		if (!venture_database_save(self->database, VENTURE_ENTITY(link), actor, error)) goto fail;
		g_clear_pointer(&request, stripe_request_free);
		g_clear_pointer(&resource, stripe_resource_free);
	}
	request = stripe_request_new(STRIPE_CHECKOUT_CREATE);
	request->customer = g_steal_pointer(&customer_id);
	g_object_get(price, "stripe-price-id", &request->price, NULL);
	request->quantity = quantity;
	request->success_url = g_strdup(self->success_url);
	request->cancel_url = g_strdup(self->cancel_url);
	g_free(request->idempotency_key);
	request->idempotency_key = g_strdup_printf("venture-invoice-%s-%" G_GINT64_FORMAT,
		venture_entity_get_uuid(invoice), venture_entity_get_version(invoice));
	resource = stripe_client_execute(self->client, request, NULL, error);
	if (!resource) goto fail;
	checkout = venture_stripe_checkout_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(checkout), self->organization_id);
	g_object_set(checkout, "invoice-id", invoice_id, "session-id", stripe_resource_get_string(resource, "id"),
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
	g_autoptr(JsonObject) event = NULL;
	g_autoptr(GPtrArray) prior = NULL;
	g_autoptr(GPtrArray) sessions = NULL;
	g_autoptr(VentureStripeEvent) record = NULL;
	g_autoptr(VentureMoney) expected = NULL;
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

	if (!venture_database_begin(self->database, error)) return FALSE;
	/* Durable event IDs replace the library's per-client replay cache. Each
	 * delivery is authenticated afresh, including identical signed retries. */
	verifier = stripe_client_new(self->yaml, self->transport, NULL, error);
	if (!verifier) goto fail;
	g_signal_connect(verifier, "event", G_CALLBACK(audit_event), self);
	event = stripe_client_verify_webhook(verifier, raw, signature, NULL, error);
	if (!event) goto fail;
	event_id = string_member(event, "id");
	type = string_member(event, "type");
	if (!event_id || !*event_id || !type) { refuse(error, "Webhook requires event id and type"); goto fail; }
	prior = find_rows(self, venture_stripe_event_get_type(), "event-id", event_id, error);
	if (!prior) goto fail;
	if (prior->len)
	{
		/* Drop the verification audit as well: a duplicate has no side effects. */
		venture_database_rollback(self->database);
		return TRUE;
	}
	record = venture_stripe_event_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(record), self->organization_id);
	payload = g_strndup(g_bytes_get_data(raw, NULL), g_bytes_get_size(raw));
	g_object_set(record, "event-id", event_id, "type", type, "received-at", now,
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
	if (!expected || !amount_node || json_node_get_value_type(amount_node) != G_TYPE_INT64 ||
	    json_node_get_int(amount_node) != venture_money_get_amount(expected) || !currency ||
	    g_ascii_strcasecmp(currency, venture_money_get_currency(expected)))
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
	g_object_set(payment, "customer-id", company_id, "invoice-id", invoice_id,
		"amount", expected, "date", now, "method", "stripe", "reference", intent, "external-id", intent, NULL);
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
