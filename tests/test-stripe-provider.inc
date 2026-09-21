/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Stateful invoice transport: late events and retries must keep addressing the
 * original provider invoice, rather than a global current response fixture. */
typedef struct
{
	gchar *id, *reference, *customer, *method, *item_key, *pay_key, *description;
	gint64 amount, received_override;
	gboolean draft, processing, paid, voided, deleted, action_required;
	guint lines;
} FakeInvoice;

static void
fake_invoice_free(gpointer data)
{
	FakeInvoice *invoice = data;
	g_free(invoice->id); g_free(invoice->reference); g_free(invoice->customer); g_free(invoice->method);
	g_free(invoice->item_key); g_free(invoice->pay_key); g_free(invoice->description); g_free(invoice);
}

static FakeInvoice *
fake_invoice_latest(FakeTransport *self)
{
	return self->invoices ? g_hash_table_lookup(self->invoices, self->last_invoice_id) : NULL;
}

static StripeResponse *
fake_invoice_send(FakeTransport *self, const StripeHttpRequest *request)
{
	g_autoptr(GUri) uri = g_uri_parse(request->url, G_URI_FLAGS_NONE, NULL);
	g_auto(GStrv) parts = g_strsplit(g_uri_get_path(uri), "/", 0);
	g_autoptr(GHashTable) form = soup_form_decode(request->body);
	g_autoptr(JsonObject) object = json_object_new(), metadata = json_object_new(), payment = json_object_new(), lines = json_object_new();
	g_autoptr(JsonArray) data = json_array_new();
	g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);
	g_autofree gchar *serialized = NULL, *intent = NULL;
	const gchar *id = g_strv_length(parts) > 3 ? parts[3] : NULL;
	FakeInvoice *invoice = NULL;
	if (self->reservation_database) g_assert_false(venture_database_has_transaction(self->reservation_database));
	if (!self->invoices)
	{
		self->invoices = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, fake_invoice_free);
		self->invoice_keys = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	}
	if (!id && !g_strcmp0(parts[2], "invoices"))
	{
		const gchar *prior_id = g_hash_table_lookup(self->invoice_keys, request->idempotency_key);
		g_assert_cmpstr(request->method, ==, "POST");
		g_assert_cmpstr(g_hash_table_lookup(form, "auto_advance"), ==, "false");
		g_assert_cmpstr(g_hash_table_lookup(form, "pending_invoice_items_behavior"), ==, "exclude");
		if (prior_id) invoice = g_hash_table_lookup(self->invoices, prior_id);
		else
		{
			invoice = g_new0(FakeInvoice, 1); invoice->draft = TRUE; invoice->received_override = -1;
			self->invoice_count++;
			invoice->id = self->invoice_count == 1 ? g_strdup("in_auto") : g_strdup_printf("in_auto_%u", self->invoice_count);
			invoice->reference = g_strdup(g_hash_table_lookup(form, "metadata[venture_collection_uuid]"));
			invoice->customer = g_strdup(g_hash_table_lookup(form, "customer"));
			invoice->method = g_strdup(g_hash_table_lookup(form, "default_payment_method"));
			g_hash_table_insert(self->invoices, g_strdup(invoice->id), invoice);
			g_hash_table_insert(self->invoice_keys, g_strdup(request->idempotency_key), g_strdup(invoice->id));
			g_free(self->last_invoice_id); self->last_invoice_id = g_strdup(invoice->id);
		}
	}
	else
	{
		if (!g_strcmp0(parts[2], "invoiceitems")) id = g_hash_table_lookup(form, "invoice");
		invoice = id ? g_hash_table_lookup(self->invoices, id) : NULL;
	}
	if (!invoice || (invoice->deleted && !g_strcmp0(request->method, "GET")))
		return stripe_response_new(404, "{\"error\":{\"type\":\"invalid_request_error\",\"code\":\"resource_missing\"}}", NULL, NULL);
	if (!g_strcmp0(parts[2], "invoiceitems"))
	{
		g_assert_cmpstr(g_hash_table_lookup(form, "metadata[venture_collection_uuid]"), ==, invoice->reference);
		if (g_strcmp0(invoice->item_key, request->idempotency_key))
		{
			invoice->amount += g_ascii_strtoll(g_hash_table_lookup(form, "amount"), NULL, 10); invoice->lines++;
			g_free(invoice->item_key); invoice->item_key = g_strdup(request->idempotency_key);
			g_free(invoice->description); invoice->description = g_strdup(g_hash_table_lookup(form, "description"));
		}
		return stripe_response_new(200, "{\"id\":\"ii_auto\"}", NULL, NULL);
	}
	if (g_str_has_suffix(request->url, "/finalize")) invoice->draft = FALSE;
	if (g_str_has_suffix(request->url, "/pay"))
	{
		g_assert_false(invoice->draft); g_assert_false(invoice->voided); g_assert_false(invoice->deleted);
		g_assert_cmpstr(g_hash_table_lookup(form, "payment_method"), ==, invoice->method);
		g_assert_cmpstr(g_hash_table_lookup(form, "off_session"), ==, "true");
		if (g_strcmp0(invoice->pay_key, request->idempotency_key))
		{
			self->payment_calls++; invoice->processing = TRUE;
			g_free(invoice->pay_key); invoice->pay_key = g_strdup(request->idempotency_key);
		}
		/* A truncated successful response cannot erase the provider effect. */
		if (self->hide_pay_response) return stripe_response_new(200, "{", NULL, NULL);
	}
	if (g_str_has_suffix(request->url, "/void"))
	{
		g_assert_false(invoice->processing); g_assert_false(invoice->paid); invoice->voided = TRUE;
	}
	if (!g_strcmp0(request->method, "DELETE"))
	{
		g_assert_true(invoice->draft); g_assert_false(invoice->processing); g_assert_false(invoice->paid); invoice->deleted = TRUE;
		json_object_set_string_member(object, "id", invoice->id); json_object_set_boolean_member(object, "deleted", TRUE);
	}
	else
	{
		json_object_set_string_member(object, "id", invoice->id); json_object_set_string_member(object, "object", "invoice");
		json_object_set_string_member(object, "status", invoice->voided ? "void" : invoice->paid ? "paid" : invoice->draft ? "draft" : "open");
		json_object_set_string_member(object, "customer", invoice->customer); json_object_set_string_member(object, "default_payment_method", invoice->method);
		json_object_set_string_member(object, "collection_method", "charge_automatically"); json_object_set_string_member(object, "currency", "usd");
		json_object_set_boolean_member(object, "auto_advance", FALSE); json_object_set_boolean_member(object, "livemode", FALSE); json_object_set_boolean_member(object, "paid_out_of_band", FALSE);
		json_object_set_int_member(object, "total", invoice->amount); json_object_set_int_member(object, "amount_due", invoice->amount);
		json_object_set_int_member(object, "amount_paid", invoice->paid ? invoice->amount : 0);
		json_object_set_int_member(object, "amount_remaining", invoice->paid || invoice->voided ? 0 : invoice->amount);
		json_object_set_int_member(object, "starting_balance", 0); json_object_set_int_member(object, "pre_payment_credit_notes_amount", 0); json_object_set_int_member(object, "post_payment_credit_notes_amount", 0);
		if (invoice->reference) json_object_set_string_member(metadata, "venture_collection_uuid", invoice->reference);
		json_object_set_object_member(object, "metadata", json_object_ref(metadata));
		if (invoice->lines)
		{
			JsonObject *line = json_object_new();
			json_object_set_int_member(line, "amount", invoice->amount); json_object_set_string_member(line, "description", invoice->description);
			json_object_set_string_member(line, "currency", "usd"); json_object_set_object_member(line, "metadata", json_object_ref(metadata));
			json_array_add_object_element(data, line);
		}
		json_object_set_array_member(lines, "data", json_array_ref(data)); json_object_set_boolean_member(lines, "has_more", FALSE);
		json_object_set_object_member(object, "lines", json_object_ref(lines));
		if (!invoice->draft)
		{
			intent = g_strdup_printf("pi_%s", invoice->id + 3);
			json_object_set_string_member(payment, "id", intent); json_object_set_string_member(payment, "object", "payment_intent");
			json_object_set_string_member(payment, "invoice", invoice->id); json_object_set_string_member(payment, "customer", invoice->customer);
			json_object_set_string_member(payment, "payment_method", invoice->method); json_object_set_string_member(payment, "currency", "usd");
			json_object_set_int_member(payment, "amount", invoice->amount);
			json_object_set_int_member(payment, "amount_received", invoice->received_override >= 0 ? invoice->received_override : invoice->paid ? invoice->amount : 0);
			json_object_set_boolean_member(payment, "livemode", FALSE);
			json_object_set_string_member(payment, "status", invoice->voided ? "canceled" : invoice->paid ? "succeeded" : invoice->processing ? "processing" : invoice->action_required ? "requires_action" : "requires_payment_method");
			json_object_set_string_member(payment, "client_secret", "should-never-retain");
			json_object_set_object_member(object, "payment_intent", json_object_ref(payment));
		}
	}
	if (!id && !g_strcmp0(parts[2], "invoices") && self->hide_invoice_response)
		return stripe_response_new(200, "{}", NULL, NULL);
	json_node_set_object(node, object); serialized = venture_json_to_string(node, FALSE);
	return stripe_response_new(200, serialized, NULL, NULL);
}
