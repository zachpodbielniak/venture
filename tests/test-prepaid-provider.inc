/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Stateful provider fixture: idempotency, consent, delayed settlement and
 * lost responses are independent of the implementation's local journal. */
typedef struct {
	GObject parent;
	FakeTransport *base;
	GHashTable *objects, *keys;
	guint sessions, intents, confirms, captures, refunds;
	guint decline, capture_failure;
	guint mismatch;
	gboolean bank, settled, drop_confirmation, drop_refund, wrong_customer;
	gboolean expire_setup;
} PrepaidTransport;
typedef struct { GObjectClass parent; } PrepaidTransportClass;
GType prepaid_transport_get_type(void);
static void prepaid_transport_iface(StripeTransportInterface *iface);
G_DEFINE_TYPE_WITH_CODE(PrepaidTransport, prepaid_transport, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(STRIPE_TYPE_TRANSPORT, prepaid_transport_iface))
static StripeResponse *prepaid_response(JsonObject *object)
{
	g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);
	g_autofree gchar *body = NULL;
	json_node_set_object(node, object); body = venture_json_to_string(node, FALSE);
	return stripe_response_new(200, body, NULL, NULL);
}
static StripeResponse *prepaid_send(StripeTransport *transport, const StripeHttpRequest *request,
	GCancellable *cancellable, GError **error)
{
	PrepaidTransport *self = (PrepaidTransport *)transport;
	g_autoptr(GUri) uri = g_uri_parse(request->url, G_URI_FLAGS_NONE, NULL);
	g_auto(GStrv) parts = g_strsplit(g_uri_get_path(uri), "/", 0);
	g_autoptr(GHashTable) form = soup_form_decode(!strcmp(request->method, "GET") ?
		(g_uri_get_query(uri) ? g_uri_get_query(uri) : "") : request->body);
	const gchar *path = g_uri_get_path(uri), *id = g_strv_length(parts) > 3 ? parts[3] : NULL;
	JsonObject *object = id ? g_hash_table_lookup(self->objects, id) : NULL;
	const gchar *existing = request->idempotency_key ? g_hash_table_lookup(self->keys, request->idempotency_key) : NULL;
	if (!strcmp(path, "/v1/checkout/sessions") && !strcmp(request->method, "POST")) {
		g_autofree gchar *session_id = NULL, *method_id = NULL, *setup_id = NULL;
		JsonObject *setup, *method, *custom, *submit;
		if (existing) return prepaid_response(g_hash_table_lookup(self->objects, existing));
		self->sessions++;
		session_id = g_strdup_printf("cs_prepaid_%u", self->sessions);
		method_id = g_strdup_printf("pm_prepaid_%u", self->sessions); setup_id = g_strdup_printf("seti_prepaid_%u", self->sessions);
		object = json_object_new(); setup = json_object_new(); method = json_object_new(); custom = json_object_new(); submit = json_object_new();
		json_object_set_string_member(object, "id", session_id); json_object_set_string_member(object, "mode", "setup");
		json_object_set_string_member(object, "status", "complete"); json_object_set_string_member(object, "url", "https://checkout.stripe.com/fixture");
		json_object_set_string_member(object, "customer", g_hash_table_lookup(form, "customer"));
		json_object_set_string_member(object, "client_reference_id", g_hash_table_lookup(form, "client_reference_id"));
		json_object_set_boolean_member(object, "livemode", FALSE);
		json_object_set_int_member(object, "expires_at", g_ascii_strtoll(g_hash_table_lookup(form, "expires_at"), NULL, 10));
		json_object_set_string_member(submit, "message", g_hash_table_lookup(form, "custom_text[submit][message]"));
		json_object_set_object_member(custom, "submit", submit); json_object_set_object_member(object, "custom_text", custom);
		json_object_set_string_member(setup, "id", setup_id); json_object_set_string_member(setup, "status", "succeeded");
		json_object_set_string_member(setup, "usage", "off_session"); json_object_set_string_member(setup, "customer", g_hash_table_lookup(form, "customer"));
		json_object_set_boolean_member(setup, "livemode", FALSE); json_object_set_boolean_member(method, "livemode", FALSE);
		json_object_set_string_member(method, "id", method_id); json_object_set_string_member(method, "customer", g_hash_table_lookup(form, "customer"));
		json_object_set_string_member(method, "type", self->bank ? "us_bank_account" : "card");
		g_assert_cmpstr(g_hash_table_lookup(form, "payment_method_types[0]"), ==, self->bank ? "us_bank_account" : "card");
		g_assert_null(g_hash_table_lookup(form, "payment_method_types[1]"));
		json_object_set_object_member(setup, "payment_method", method);
		if (self->bank) {
			JsonObject *mandate = json_object_new();
			json_object_set_string_member(mandate, "id", "mandate_example"); json_object_set_string_member(mandate, "type", "multi_use");
			json_object_set_string_member(mandate, "status", "active"); json_object_set_string_member(mandate, "payment_method", method_id);
			json_object_set_object_member(setup, "mandate", mandate);
		}
		json_object_set_object_member(object, "setup_intent", setup);
		g_hash_table_insert(self->objects, g_strdup(session_id), object);
		g_hash_table_insert(self->keys, g_strdup(request->idempotency_key), g_strdup(session_id));
		return prepaid_response(object);
	}
	if (g_str_has_prefix(path, "/v1/checkout/sessions/")) {
		object = g_hash_table_lookup(self->objects, parts[4]); g_assert_nonnull(object);
		if (self->expire_setup) { self->expire_setup = FALSE; json_object_set_string_member(object, "status", "expired"); }
		return prepaid_response(object);
	}
	if (!strcmp(path, "/v1/payment_intents") && !strcmp(request->method, "POST")) {
		g_autofree gchar *intent = NULL;
		JsonObject *metadata;
		if (existing) return prepaid_response(g_hash_table_lookup(self->objects, existing));
		g_assert_cmpstr(g_hash_table_lookup(form, "confirm"), ==, "false");
		self->intents++; intent = g_strdup_printf("pi_prepaid_%u", self->intents);
		object = json_object_new(); metadata = json_object_new();
		json_object_set_string_member(object, "id", intent); json_object_set_string_member(object, "customer", g_hash_table_lookup(form, "customer"));
		json_object_set_string_member(object, "currency", g_hash_table_lookup(form, "currency"));
		json_object_set_int_member(object, "amount", g_ascii_strtoll(g_hash_table_lookup(form, "amount"), NULL, 10));
		json_object_set_string_member(object, "capture_method", g_hash_table_lookup(form, "capture_method"));
		json_object_set_string_member(object, "kind", g_hash_table_lookup(form, "payment_method_types[0]"));
		if (g_hash_table_lookup(form, "payment_method")) json_object_set_string_member(object, "payment_method", g_hash_table_lookup(form, "payment_method"));
		json_object_set_string_member(object, "status", "requires_confirmation"); json_object_set_boolean_member(object, "livemode", FALSE);
		json_object_set_string_member(metadata, "prepayment", g_hash_table_lookup(form, "metadata[prepayment]"));
		json_object_set_object_member(object, "metadata", metadata);
		g_hash_table_insert(self->objects, g_strdup(intent), object); g_hash_table_insert(self->keys, g_strdup(request->idempotency_key), g_strdup(intent));
		return prepaid_response(object);
	}
	if (g_str_has_prefix(path, "/v1/payment_intents/")) {
		const gchar *kind;
		guint number;
		g_assert_nonnull(object); kind = json_object_get_string_member(object, "kind");
		number = (guint)g_ascii_strtoull(id + strlen("pi_prepaid_"), NULL, 10);
		if (g_str_has_suffix(path, "/confirm")) {
			if (!existing) { self->confirms++; g_hash_table_insert(self->keys, g_strdup(request->idempotency_key), g_strdup(id)); }
			if (self->decline == number) {
				json_object_set_string_member(object, "status", "requires_payment_method");
				return stripe_response_new(402, "{\"error\":{\"type\":\"card_error\",\"code\":\"card_declined\"}}", NULL, NULL);
			}
			if (!strcmp(kind, "card") && !strcmp(json_object_get_string_member(object, "capture_method"), "manual")) {
				json_object_set_string_member(object, "status", "requires_capture");
				json_object_set_int_member(object, "amount_capturable", json_object_get_int_member(object, "amount"));
			} else json_object_set_string_member(object, "status", !strcmp(kind, "card") ? "succeeded" : !strcmp(kind, "customer_balance") ? "requires_action" : "processing");
			if (self->drop_confirmation) { self->drop_confirmation = FALSE; g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CONNECTION_CLOSED, "Fixture response lost"); return NULL; }
		}
		if (g_str_has_suffix(path, "/capture")) {
			GHashTableIter iter; gpointer value;
			g_hash_table_iter_init(&iter, self->objects);
			while (g_hash_table_iter_next(&iter, NULL, &value)) {
				JsonObject *other = value;
				if (g_str_has_prefix(json_object_get_string_member(other, "id"), "pi_")) {
					const gchar *status = json_object_get_string_member(other, "status");
					g_assert_true(!strcmp(status, "requires_capture") || !strcmp(status, "succeeded"));
				}
			}
			if (self->capture_failure == number) return stripe_response_new(402, "{\"error\":{\"type\":\"card_error\",\"code\":\"card_declined\"}}", NULL, NULL);
			if (!existing) { self->captures++; g_hash_table_insert(self->keys, g_strdup(request->idempotency_key), g_strdup(id)); }
			json_object_set_string_member(object, "status", "succeeded");
		}
		if (g_str_has_suffix(path, "/cancel")) json_object_set_string_member(object, "status", "canceled");
		if (self->settled && strcmp(kind, "card")) json_object_set_string_member(object, "status", "succeeded");
		if (!strcmp(json_object_get_string_member(object, "status"), "succeeded")) json_object_set_int_member(object, "amount_received", json_object_get_int_member(object, "amount"));
		if (!strcmp(kind, "customer_balance")) {
			JsonObject *action = json_object_new(), *instructions = json_object_new();
			json_object_set_string_member(instructions, "hosted_instructions_url", "https://payments.stripe.com/fixture");
			json_object_set_object_member(action, "display_bank_transfer_instructions", instructions); json_object_set_object_member(object, "next_action", action);
		}
		if (self->wrong_customer) json_object_set_string_member(object, "customer", "cus_unrelated");
		if (self->mismatch == 1) json_object_set_string_member(object, "payment_method", "pm_unrelated");
		if (self->mismatch == 2) json_object_set_int_member(object, "amount", 1);
		if (self->mismatch == 3) json_object_set_string_member(object, "customer", "cus_unrelated");
		if (self->mismatch == 4) json_object_set_string_member(object, "capture_method", "automatic");
		return prepaid_response(object);
	}
	if (!strcmp(path, "/v1/refunds")) {
		g_autofree gchar *refund_id = NULL;
		JsonObject *intent;
		if (!strcmp(request->method, "GET")) {
			g_autoptr(JsonObject) list = json_object_new();
			JsonArray *items = json_array_new();
			GHashTableIter iter; gpointer value;
			g_hash_table_iter_init(&iter, self->objects);
			while (g_hash_table_iter_next(&iter, NULL, &value)) {
				JsonObject *entry = value;
				if (g_str_has_prefix(json_object_get_string_member(entry, "id"), "re_") &&
					!g_strcmp0(json_object_get_string_member(entry, "payment_intent"), g_hash_table_lookup(form, "payment_intent")))
					json_array_add_object_element(items, json_object_ref(entry));
			}
			json_object_set_array_member(list, "data", items); json_object_set_boolean_member(list, "has_more", FALSE);
			return prepaid_response(list);
		}
		if (existing) return prepaid_response(g_hash_table_lookup(self->objects, existing));
		intent = g_hash_table_lookup(self->objects, g_hash_table_lookup(form, "payment_intent")); g_assert_nonnull(intent);
		self->refunds++; refund_id = g_strdup_printf("re_prepaid_%u", self->refunds);
		object = json_object_new(); json_object_set_string_member(object, "id", refund_id);
		json_object_set_string_member(object, "payment_intent", g_hash_table_lookup(form, "payment_intent"));
		json_object_set_int_member(object, "amount", json_object_get_int_member(intent, "amount"));
		json_object_set_string_member(object, "currency", "usd"); json_object_set_string_member(object, "status", "succeeded");
		g_hash_table_insert(self->objects, g_strdup(refund_id), object); g_hash_table_insert(self->keys, g_strdup(request->idempotency_key), g_strdup(refund_id));
		if (self->drop_refund) { self->drop_refund = FALSE; g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CONNECTION_CLOSED, "Fixture refund response lost"); return NULL; }
		return prepaid_response(object);
	}
	if (g_str_has_prefix(path, "/v1/refunds/")) { g_assert_nonnull(object); return prepaid_response(object); }
	return fake_send(STRIPE_TRANSPORT(self->base), request, cancellable, error);
}
static void prepaid_transport_iface(StripeTransportInterface *iface) { iface->send = prepaid_send; }
static void prepaid_finalize(GObject *object)
{
	PrepaidTransport *self = (PrepaidTransport *)object;
	g_clear_object(&self->base); g_hash_table_unref(self->objects); g_hash_table_unref(self->keys);
	G_OBJECT_CLASS(prepaid_transport_parent_class)->finalize(object);
}
static void prepaid_transport_class_init(PrepaidTransportClass *klass) { G_OBJECT_CLASS(klass)->finalize = prepaid_finalize; }
static void prepaid_transport_init(PrepaidTransport *self)
{
	self->base = g_object_new(fake_transport_get_type(), NULL);
	self->objects = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)json_object_unref);
	self->keys = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
}
