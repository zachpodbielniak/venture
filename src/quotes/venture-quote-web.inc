/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */

static HtmxResponse *
quote_error(guint code, const gchar *message)
{
	HtmxResponse *response = htmx_response_new_with_content(message);
	htmx_response_set_status(response, code);
	return response;
}

static void
quote_money_html(GString *html, VentureEntity *record, const gchar *field)
{
	g_autoptr(VentureMoney) amount = NULL;
	g_autofree gchar *text = NULL;
	g_object_get(record, field, &amount, NULL);
	if (amount == NULL) return;
	text = venture_money_to_display_string(amount, TRUE);
	venture_html_escape_append(html, text);
}

/* Whitelisted commercial fields only: public pages never serialize entities. */
static HtmxResponse *
quote_document(VentureWebServer *self, VentureEntity *quote, gboolean public_page)
{
	g_autoptr(GString) html = g_string_new("<!doctype html><html><head><meta charset=\"utf-8\"><title>Quote</title><style>");
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_QUOTE_LINE);
	g_autoptr(GPtrArray) lines = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *number = NULL;
	g_autofree gchar *terms = NULL;
	g_autoptr(GDateTime) until = NULL;
	gint state;
	gint64 revision;
	guint i;
	const gchar *fields[] = { "subtotal", "discount", "tax", "total" };
	venture_query_set_organization(query, venture_entity_get_organization_id(quote));
	venture_query_set_limit(query, 0);
	venture_query_add_filter_int(query, "quote-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(quote), NULL);
	venture_query_add_order(query, "position", VENTURE_SORT_ASCENDING, NULL);
	lines = venture_database_find(venture_context_get_database(self->context), query, &error);
	if (lines == NULL) return venture_web_error_response(error);
	g_object_get(quote, "number", &number, "terms", &terms, "valid-until", &until,
		"status", &state, "revision", &revision, NULL);
	g_string_append(html, VENTURE_DOCUMENT_PRINT_STYLE);
	g_string_append(html, "</style></head><body><h1>Quote ");
	venture_html_escape_append(html, number);
	g_string_append_printf(html, "</h1><p>Revision %" G_GINT64_FORMAT " · ", revision);
	venture_html_escape_append(html, venture_enum_to_nick(venture_quote_status_get_type(), state));
	g_string_append(html, "</p>");
	if (until != NULL)
	{
		g_autofree gchar *date = g_date_time_format(until, "%Y-%m-%d %H:%M UTC");
		g_string_append(html, "<p>Valid until ");
		venture_html_escape_append(html, date);
		g_string_append(html, "</p>");
	}
	g_string_append(html, "<table><thead><tr><th>Description</th><th>Quantity</th><th>Unit price</th><th>Discount %</th><th>Tax %</th></tr></thead><tbody>");
	for (i = 0; i < lines->len; i++)
	{
		VentureEntity *line = g_ptr_array_index(lines, i);
		g_autofree gchar *description = NULL;
		gint64 quantity, discount, tax;
		g_object_get(line, "description", &description, "quantity", &quantity,
			"discount-percent", &discount, "tax-percent", &tax, NULL);
		g_string_append(html, "<tr><td>");
		venture_html_escape_append(html, description);
		g_string_append_printf(html, "</td><td>%" G_GINT64_FORMAT "</td><td>", quantity);
		quote_money_html(html, line, "unit-price");
		g_string_append_printf(html, "</td><td>%" G_GINT64_FORMAT "</td><td>%" G_GINT64_FORMAT "</td></tr>", discount, tax);
	}
	g_string_append(html, "</tbody></table><table>");
	for (i = 0; i < G_N_ELEMENTS(fields); i++)
	{
		g_string_append(html, "<tr><th>");
		venture_html_escape_append(html, fields[i]);
		g_string_append(html, "</th><td class=\"num\">");
		quote_money_html(html, quote, fields[i]);
		g_string_append(html, "</td></tr>");
	}
	g_string_append(html, "</table><div class=\"terms\">");
	venture_html_escape_append(html, terms);
	g_string_append(html, "</div>");
	if (public_page && state == VENTURE_QUOTE_SENT)
		g_string_append(html, "<form method=\"post\" action=\"\"><label>Your full name <input name=\"accepted_by\" maxlength=\"200\" required></label><button>Accept quote</button></form>");
	g_string_append(html, "</body></html>");
	return htmx_response_new_with_content(html->str);
}

static HtmxResponse *
quote_public(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_QUOTE);
	g_autoptr(VentureEntity) quote = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *ip = venture_auth_remote_address(request);
	const gchar *token = g_hash_table_lookup(params, "token");
	gint state;
	if (!venture_web_module_enabled(self, "quotes")) return quote_error(404, "Not found");
	if (self->quote_limiter == NULL) self->quote_limiter = htmx_rate_limiter_new(30, 0.5);
	if (!htmx_rate_limiter_allow(self->quote_limiter, ip != NULL ? ip : "unknown"))
		return quote_error(429, "Try again later");
	if (token == NULL || strlen(token) != 64) return quote_error(404, "Not found");
	venture_query_add_filter_string(query, "acceptance-token", VENTURE_FILTER_OP_EQ, token, NULL);
	quote = venture_database_find_one(venture_context_get_database(self->context), query, &error);
	if (quote == NULL) return quote_error(404, "Not found");
	g_object_get(quote, "status", &state, NULL);
	if (state == VENTURE_QUOTE_DRAFT) return quote_error(404, "Not found");
	if (htmx_request_get_method(request) == HTMX_METHOD_POST)
	{
		g_autoptr(VentureEntity) action = VENTURE_ENTITY(venture_quote_action_new());
		g_autoptr(JsonNode) body = htmx_request_get_json(request, NULL);
		const gchar *by = htmx_request_get_form_value(request, "accepted_by");
		VentureActor actor;
		if (body != NULL && JSON_NODE_HOLDS_OBJECT(body))
			by = venture_json_object_get_string(json_node_get_object(body), "accepted_by", "");
		actor.kind = VENTURE_ACTOR_KIND_USER;
		actor.name = "public quote acceptance";
		actor.prompt = NULL;
		actor.request_id = NULL;
		actor.approved_by = NULL;
		venture_entity_set_organization_id(action, venture_entity_get_organization_id(quote));
		g_object_set(action, "quote-id", venture_entity_get_id(quote), "action", "accept",
			"expected-version", venture_entity_get_version(quote), "accepted-by", by, NULL);
		if (!venture_quote_service_execute(venture_database_get_quote_service(venture_context_get_database(self->context)),
			action, "web", ip, &actor, &error))
			return quote_error(400, "This quote cannot be accepted. Contact the sender.");
		return htmx_response_new_with_content("<p>Quote accepted. Thank you.</p>");
	}
	return quote_document(self, quote, TRUE);
}

static HtmxResponse *
quote_route(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	g_autoptr(VentureEntity) quote = NULL;
	g_autoptr(VentureEntity) action = NULL;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) body = NULL;
	g_autoptr(JsonNode) result = NULL;
	const gchar *verb = g_hash_table_lookup(params, "action");
	const gchar *by;
	const gchar *reason;
	HtmxResponse *gate;
	VentureActor actor;
	gint64 id = g_ascii_strtoll(g_hash_table_lookup(params, "id"), NULL, 10);
	gboolean api = g_str_has_prefix(htmx_request_get_path(request), "/api/");
	gboolean staged = FALSE;
	if (!api)
	{
		gate = venture_web_ui_require_session(self, request);
		if (gate != NULL) return gate;
	}
	gate = venture_web_api_require(self, request, verb == NULL ? VENTURE_USER_ROLE_VIEWER : VENTURE_USER_ROLE_EDITOR);
	if (gate != NULL) return gate;
	gate = venture_web_require_module_api(self, "quotes");
	if (gate != NULL) return gate;
	quote = venture_database_get(venture_context_get_database(self->context), VENTURE_TYPE_QUOTE, id, &error);
	if (quote == NULL || venture_entity_is_deleted(quote))
	{
		if (error == NULL) g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Quote not found");
		return venture_web_error_response(error);
	}
	if (verb == NULL) return quote_document(self, quote, FALSE);
	principal = venture_auth_authenticate(self->auth, request);
	venture_auth_to_actor(principal, &actor);
	body = htmx_request_get_json(request, NULL);
	by = htmx_request_get_form_value(request, "accepted_by");
	reason = htmx_request_get_form_value(request, "reason");
	if (body != NULL && JSON_NODE_HOLDS_OBJECT(body))
	{
		by = venture_json_object_get_string(json_node_get_object(body), "accepted_by", "");
		reason = venture_json_object_get_string(json_node_get_object(body), "reason", "");
	}
	action = VENTURE_ENTITY(venture_quote_action_new());
	venture_entity_set_organization_id(action, venture_entity_get_organization_id(quote));
	g_object_set(action, "quote-id", id, "action", verb, "expected-version", venture_entity_get_version(quote),
		"accepted-by", by, "reason", reason, NULL);
	if (!venture_confirmation_parse_stage_flag(htmx_request_get_query_param(request, "stage"), &staged, &error))
		return venture_web_error_response(error);
	if (staged)
		return venture_web_api_stage(self, action, NULL, principal, VENTURE_AUDIT_ACTION_CREATE);
	if (!venture_quote_service_execute(venture_database_get_quote_service(venture_context_get_database(self->context)),
		action, "manual", NULL, &actor, &error)) return venture_web_error_response(error);
	if (!api)
	{
		gint64 result_id = 0;
		g_autofree gchar *destination = NULL;
		g_object_get(action, "result-quote-id", &result_id, NULL);
		destination = g_strdup_printf("/e/quote/%" G_GINT64_FORMAT, result_id != 0 ? result_id : id);
		return venture_web_redirect_to(destination);
	}
	result = venture_serializable_to_json(VENTURE_SERIALIZABLE(action), FALSE);
	return venture_web_json_response(result, 200);
}

static void
quote_buttons(GString *html, VentureEntity *record)
{
	gint64 id;
	if (!VENTURE_IS_QUOTE(record)) return;
	id = venture_entity_get_id(record);
	g_string_append_printf(html, "<p><a href=\"/quotes/%" G_GINT64_FORMAT "/print\">Print quote</a></p>"
		"<form method=\"post\" action=\"/quotes/%" G_GINT64_FORMAT "/send\"><button>Send quote</button></form>"
		"<form method=\"post\" action=\"/quotes/%" G_GINT64_FORMAT "/accept\"><input name=\"accepted_by\" placeholder=\"Acceptor's full name\" required><button>Accept</button></form>"
		"<form method=\"post\" action=\"/quotes/%" G_GINT64_FORMAT "/decline\"><input name=\"reason\" placeholder=\"Reason\" required><button>Decline</button></form>"
		"<form method=\"post\" action=\"/quotes/%" G_GINT64_FORMAT "/revise\"><button>New revision</button></form>", id, id, id, id, id);
}
