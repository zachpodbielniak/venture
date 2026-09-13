/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Included by the web server so these thin action adapters share its guards. */
static HtmxResponse *venture_web_mail_action(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) body = NULL, node = NULL;
	g_autoptr(VentureMailMessage) message = NULL, queued = NULL;
	g_autoptr(JsonBuilder) result = json_builder_new();
	VentureMailOutbox *outbox;
	VentureActor actor;
	HtmxResponse *gate;
	const gchar *action = g_hash_table_lookup(params, "action");
	const gchar *id_text = g_hash_table_lookup(params, "id");
	gint64 org, limit = 100;
	gint delivered;
	JsonObject *object = NULL;
	gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_EDITOR);
	if (gate) return gate;
	gate = venture_web_require_module_api(self, "mail");
	if (gate) return gate;
	principal = venture_auth_authenticate(self->auth, request);
	venture_auth_to_actor(principal, &actor);
	body = htmx_request_get_json(request, NULL);
	if (body && JSON_NODE_HOLDS_OBJECT(body)) object = json_node_get_object(body);
	org = venture_context_get_default_organization_id(self->context);
	if (object && json_object_has_member(object, "organization_id")) org = venture_json_object_get_int(object, "organization_id", org);
	outbox = venture_context_get_mail_outbox(self->context);
	if (id_text) {
		if (!venture_mail_outbox_retry(outbox, org, g_ascii_strtoll(id_text, NULL, 10), &actor, &error)) return venture_web_error_response(error);
	} else if (!g_strcmp0(action, "deliver")) {
		if (object) limit = venture_json_object_get_int(object, "limit", 100);
		if (limit < 1 || limit > 1000) {
			venture_set_error_validation(&error, "limit", "Expected 1 through 1000"); return venture_web_error_response(error);
		}
		delivered = venture_mail_outbox_deliver_due(outbox, org, (guint)limit, NULL, NULL, &error);
		if (delivered < 0) return venture_web_error_response(error);
		json_builder_begin_object(result); json_builder_set_member_name(result, "attempted"); json_builder_add_int_value(result, delivered); json_builder_end_object(result);
		node = json_builder_get_root(result); return venture_web_json_response(node, 200);
	} else if (!g_strcmp0(action, "send") || !g_strcmp0(action, "test")) {
		message = venture_mail_message_new();
		if (!body || !JSON_NODE_HOLDS_OBJECT(body) || !venture_serializable_from_json(VENTURE_SERIALIZABLE(message), body, &error)) {
			if (!error) venture_set_error_validation(&error, "mail", "A message object is required");
			return venture_web_error_response(error);
		}
		g_object_set(message, "organization-id", org, NULL);
		if (!g_strcmp0(action, "test")) {
			venture_entity_set_attribute(VENTURE_ENTITY(message), "_mail_attachments", NULL);
			g_object_set(message, "subject", "Venture mail test", "text-body", "Your Venture SMTP configuration is working.", NULL);
			if (!venture_mailer_send(venture_context_get_mailer(self->context), message, NULL, &error)) return venture_web_error_response(error);
		} else {
			queued = venture_mail_outbox_enqueue(outbox, message, &actor, &error);
			if (!queued) return venture_web_error_response(error);
			node = venture_serializable_to_json(VENTURE_SERIALIZABLE(queued), FALSE);
			return venture_web_json_response(node, 202);
		}
	} else {
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Unknown mail action"); return venture_web_error_response(error);
	}
	json_builder_begin_object(result); json_builder_set_member_name(result, "ok"); json_builder_add_boolean_value(result, TRUE); json_builder_end_object(result);
	node = json_builder_get_root(result); return venture_web_json_response(node, 200);
}
static HtmxResponse *venture_web_mail_invoice(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureMailMessage) message = NULL;
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GError) error = NULL;
	VentureActor actor;
	HtmxResponse *gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_EDITOR);
	if (gate) return gate;
	gate = venture_web_require_module_api(self, "mail"); if (gate) return gate;
	gate = venture_web_require_module_api(self, "invoicing"); if (gate) return gate;
	principal = venture_auth_authenticate(self->auth, request);
	venture_auth_to_actor(principal, &actor);
	invoice = venture_database_get(venture_context_get_database(self->context), VENTURE_TYPE_INVOICE,
		g_ascii_strtoll(g_hash_table_lookup(params, "id"), NULL, 10), &error);
	if (!invoice || venture_entity_is_deleted(invoice)) {
		if (!error) g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Invoice not found");
		return venture_web_error_response(error);
	}
	message = venture_mail_send_invoice(self->context, venture_entity_get_organization_id(invoice), venture_entity_get_id(invoice), &actor, &error);
	if (!message) return venture_web_error_response(error);
	node = venture_serializable_to_json(VENTURE_SERIALIZABLE(message), FALSE);
	return venture_web_json_response(node, 202);
}

static HtmxResponse *venture_web_mail_invoice_ui(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	HtmxResponse *gate = venture_web_ui_require_session(self, request);
	HtmxResponse *response;
	g_autofree gchar *path = NULL;
	if (gate) return gate;
	response = venture_web_mail_invoice(request, params, data);
	if (htmx_response_get_status(response) != 202) return response;
	g_object_unref(response);
	path = g_strdup_printf("/e/invoice/%s", (gchar *)g_hash_table_lookup(params, "id"));
	response = htmx_response_new();
	htmx_response_set_status(response, 303);
	htmx_response_add_header(response, "Location", path);
	return response;
}
