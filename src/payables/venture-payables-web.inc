/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
static HtmxResponse *
venture_web_payables_action(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) command = NULL;
	g_autoptr(JsonNode) input = NULL;
	g_autoptr(JsonNode) output = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *destination = NULL;
	const gchar *action = g_hash_table_lookup(params, "action");
	gint64 id = g_ascii_strtoll(g_hash_table_lookup(params, "id"), NULL, 10);
	gboolean api = g_str_has_prefix(htmx_request_get_path(request), "/api/");
	gboolean stage = FALSE;
	HtmxResponse *gate;
	VentureActor actor;

	if (!api)
	{
		gate = venture_web_ui_require_session(self, request);
		if (gate != NULL)
			return gate;
	}
	principal = venture_auth_authenticate(self->auth, request);
	if (!venture_web_hosted_auth_require(self, request, principal, VENTURE_USER_ROLE_EDITOR, &error))
		return venture_web_error_response(error);
	gate = venture_web_require_module_api(self, "payables");
	if (gate != NULL)
		return gate;
	if (api)
	{
		input = htmx_request_get_json(request, &error);
		if (input == NULL || !venture_web_stage_requested(request, &stage, &error))
			return venture_web_error_response(error);
	}
	command = venture_payables_service_prepare_action(
		venture_payables_service_get(venture_context_get_database(self->context)), id, action, input, &error);
	if (command == NULL)
		return venture_web_error_response(error);
	if (stage)
		return venture_web_api_stage(self, command, NULL, principal, VENTURE_AUDIT_ACTION_CREATE);
	venture_auth_to_actor(principal, &actor);
	if (!venture_database_save(venture_context_get_database(self->context), command, &actor, &error))
		return venture_web_error_response(error);
	if (api)
	{
		output = venture_serializable_to_json(VENTURE_SERIALIZABLE(command), FALSE);
		return venture_web_json_response(output, 201);
	}
	destination = g_strdup_printf("/e/vendor_bill/%" G_GINT64_FORMAT, id);
	return venture_web_redirect_to(destination);
}

static HtmxResponse *
venture_web_payables_workbench(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	VentureWebServer *self = user_data;
	g_autoptr(GError) error = NULL;
	HtmxResponse *gate;
	(void)params;
	if (g_str_has_prefix(htmx_request_get_path(request), "/api/"))
	{
		g_autoptr(VentureAuthPrincipal) principal = NULL;
		g_autoptr(JsonNode) body = NULL;
		g_autoptr(GArray) ids = g_array_new(FALSE, FALSE, sizeof(gint64));
		g_autoptr(GDateTime) date = NULL;
		JsonObject *object;
		JsonArray *bills;
		VentureActor actor;
		guint i;
		gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_EDITOR);
		if (gate != NULL)
			return gate;
		gate = venture_web_require_module_api(self, "payables");
		if (gate != NULL)
			return gate;
		principal = venture_auth_authenticate(self->auth, request);
		venture_auth_to_actor(principal, &actor);
		body = htmx_request_get_json(request, &error);
		if (body == NULL || !JSON_NODE_HOLDS_OBJECT(body))
			return venture_web_error_response(error);
		object = json_node_get_object(body);
		bills = json_object_get_array_member(object, "bills");
		if (bills == NULL)
		{
			g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Select bills");
			return venture_web_error_response(error);
		}
		for (i = 0; i < json_array_get_length(bills); i++)
		{
			gint64 id = json_array_get_int_element(bills, i);
			g_array_append_val(ids, id);
		}
		if (venture_json_object_get_string(object, "date", NULL) != NULL)
			{
			date = venture_time_from_string(venture_json_object_get_string(object, "date", NULL), &error);
			if (date == NULL) return venture_web_error_response(error);
		}
		if (date == NULL)
			date = venture_time_from_string("today", NULL);
		if (!venture_payables_service_pay_bills(venture_payables_service_get(venture_context_get_database(self->context)),
			ids, date, venture_json_object_get_string(object, "method", NULL),
			venture_json_object_get_string(object, "adapter", "transfer"),
			venture_json_object_get_string(object, "reference", NULL), &actor, &error))
			return venture_web_error_response(error);
		{
			g_autoptr(JsonNode) ok = json_node_new(JSON_NODE_OBJECT);
			json_node_take_object(ok, json_object_new());
			json_object_set_boolean_member(json_node_get_object(ok), "ok", TRUE);
			return venture_web_json_response(ok, 200);
		}
	}
	{
		g_autoptr(GString) content = g_string_new(NULL);
		g_autoptr(VentureQuery) query = NULL;
		g_autoptr(GPtrArray) rows = NULL;
		g_autoptr(GDateTime) today = venture_time_from_string("today", NULL);
		guint i, shown = 0;
		gate = venture_web_ui_require_session(self, request);
		if (gate != NULL)
			return gate;
		gate = venture_web_require_module_ui(self, request, "payables");
		if (gate != NULL)
			return gate;
		query = venture_query_new(VENTURE_TYPE_VENDOR_BILL);
		venture_query_set_organization(query, venture_web_active_organization(self, request));
		venture_query_add_order(query, "due-date", VENTURE_SORT_ASCENDING, NULL);
		rows = venture_database_find(venture_context_get_database(self->context), query, &error);
		if (rows == NULL)
			return venture_web_error_response(error);
		/* One form, with the action where every page keeps it: pick the
		 * bills, pick how, pay. Oldest due first, because that is the
		 * order they get paid in. */
		g_string_append(content, "<form method=\"post\" action=\"/payables/pay\">"
			"<div class=\"page-head\"><div class=\"page-title\"><nav class=\"crumbs\" aria-label=\"Breadcrumb\">"
			"<span class=\"crumb\">Money out</span></nav><h1>Pay bills</h1>"
			"<span class=\"subtitle\">Approved supplier bills waiting to be paid.</span></div>"
			"<div class=\"page-actions\"><label class=\"inline-field\"><span class=\"field-label\">Pay by</span>"
			"<select name=\"adapter\"><option value=\"transfer\">Bank transfer</option><option value=\"ach\">ACH</option>"
			"<option value=\"check\">Check</option><option value=\"card\">Card</option><option value=\"manual\">Recorded by hand</option></select></label>"
			"<button class=\"btn btn-primary\" type=\"submit\">Pay selected</button></div></div>"
			"<div class=\"card\"><div class=\"table-wrap\"><table class=\"data selectable\"><thead><tr>"
			"<th class=\"tick\"><input type=\"checkbox\" data-bulk-all title=\"Select all\"></th>"
			"<th>Bill</th><th>Supplier</th><th>Billed</th><th>Due</th><th>Status</th></tr></thead><tbody>");
		for (i = 0; i < rows->len; i++)
		{
			VentureEntity *bill = g_ptr_array_index(rows, i);
			g_autofree gchar *status = NULL, *number = NULL, *billed = NULL, *due_text = NULL, *label = NULL;
			g_autoptr(GDateTime) billed_at = NULL, due = NULL;
			g_autoptr(VentureEntity) supplier = NULL;
			g_autofree gchar *supplier_name = NULL;
			gint64 company = 0;
			gboolean overdue;
			g_object_get(bill, "status", &status, "number", &number, "company-id", &company,
				"bill-date", &billed_at, "due-date", &due, NULL);
			if (g_strcmp0(status, "approved") != 0 && g_strcmp0(status, "partially_paid") != 0)
				continue;
			shown++;
			supplier = company > 0 ? venture_database_get(venture_context_get_database(self->context),
				VENTURE_TYPE_COMPANY, company, NULL) : NULL;
			supplier_name = supplier != NULL ? venture_entity_get_display_name(supplier) : g_strdup("");
			billed = venture_time_to_date_string(billed_at, venture_context_get_timezone(self->context));
			due_text = venture_time_to_date_string(due, venture_context_get_timezone(self->context));
			overdue = due != NULL && g_date_time_compare(due, today) < 0;
			label = venture_web_label_from_name(status);
			g_string_append_printf(content, "<tr><td class=\"tick\"><input type=\"checkbox\" name=\"bill\" "
				"value=\"%" G_GINT64_FORMAT "\" data-bulk-id=\"%" G_GINT64_FORMAT "\"></td>"
				"<td class=\"row-title\"><a href=\"/e/vendor_bill/%" G_GINT64_FORMAT "\">",
				venture_entity_get_id(bill), venture_entity_get_id(bill), venture_entity_get_id(bill));
			venture_html_escape_append(content, number);
			g_string_append(content, "</a></td><td>");
			venture_html_escape_append(content, supplier_name);
			g_string_append(content, "</td><td><time>");
			venture_html_escape_append(content, billed != NULL ? billed : "");
			g_string_append_printf(content, "</time></td><td%s><time>", overdue ? " class=\"overdue\"" : "");
			venture_html_escape_append(content, due_text != NULL ? due_text : "\xe2\x80\x94");
			g_string_append_printf(content, "</time>%s</td><td><span class=\"badge %s\">",
				overdue ? " <span class=\"badge negative\">Overdue</span>" : "",
				g_strcmp0(status, "approved") == 0 ? "info" : "warning");
			venture_html_escape_append(content, label);
			g_string_append(content, "</span></td></tr>");
		}
		g_string_append(content, "</tbody></table></div>");
		if (shown == 0)
			g_string_append(content, "<div class=\"empty\"><h3>Nothing to pay</h3><p class=\"muted\">"
				"Approved supplier bills appear here until they are paid.</p></div>");
		g_string_append(content, "</div></form>"
			"<script>(function(){var a=document.querySelector('[data-bulk-all]');if(!a)return;"
			"a.addEventListener('change',function(){document.querySelectorAll('[data-bulk-id]')"
			".forEach(function(b){b.checked=a.checked;});});})();</script>");
		return venture_web_html_response(venture_web_page(self, request, "/payables", "Pay bills", content->str), 200);
	}
}

static HtmxResponse *
venture_web_payables_workbench_pay(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GArray) ids = g_array_new(FALSE, FALSE, sizeof(gint64));
	g_autoptr(GDateTime) date = venture_time_from_string("today", NULL);
	VentureActor actor;
	HtmxResponse *gate;
	(void)params;
	gate = venture_web_ui_require_session(self, request);
	if (gate != NULL)
		return gate;
	principal = venture_auth_authenticate(self->auth, request);
	if (!venture_web_hosted_auth_require(self, request, principal, VENTURE_USER_ROLE_EDITOR, &error))
		return venture_web_error_response(error);
	gate = venture_web_require_module_ui(self, request, "payables");
	if (gate != NULL)
		return gate;
	/* Every ticked bill. A form posts one "bill" per box, and the form
	 * lookup keeps only one of them, which paid the first bill and
	 * quietly left the rest; the body is read for all of them. */
	{
		const gchar *body = htmx_request_get_body(request);
		g_auto(GStrv) pairs = g_strsplit(body != NULL ? body : "", "&", -1);
		guint k;
		for (k = 0; pairs[k] != NULL; k++)
		{
			if (g_str_has_prefix(pairs[k], "bill="))
			{
				gint64 id = g_ascii_strtoll(pairs[k] + 5, NULL, 10);
				if (id > 0)
					g_array_append_val(ids, id);
			}
		}
	}
	venture_auth_to_actor(principal, &actor);
	if (!venture_payables_service_pay_bills(venture_payables_service_get(venture_context_get_database(self->context)),
		ids, date, NULL, htmx_request_get_form_value(request, "adapter"), NULL, &actor, &error))
		return venture_web_error_response(error);
	return venture_web_redirect_to("/payables");
}

/*
 * A bill's page says where it stands and what can still change: a draft
 * is not in the books and can be edited freely; an approved bill is, so
 * its amount and dates are fixed and a change goes through a credit or a
 * void. The action that moves it on sits with the sentence.
 */
static void
venture_web_append_payables_actions(VentureWebServer *self, GString *content, VentureEntity *record)
{
	g_autofree gchar *state = NULL, *due_text = NULL;
	g_autoptr(GDateTime) due = NULL;
	g_autoptr(VentureMoney) balance = NULL;
	gint64 id, supplier = 0;
	if (!VENTURE_IS_VENDOR_BILL(record) || !venture_web_module_enabled(self, "payables"))
		return;
	id = venture_entity_get_id(record);
	g_object_get(record, "status", &state, "due-date", &due, "company-id", &supplier, NULL);
	due_text = due != NULL ? venture_time_to_date_string(due, venture_context_get_timezone(self->context)) : NULL;
	balance = venture_payables_service_bill_balance(venture_payables_service_get(venture_context_get_database(self->context)),
		id, NULL, NULL);

	g_string_append(content, "<section class=\"card next-up\"><div class=\"card-head\"><h2>What happens next</h2></div><div class=\"card-body\">");
	if (g_strcmp0(state, "draft") == 0)
	{
		g_string_append_printf(content, "<p class=\"next-up-lead\">A draft: not in the books yet. Check the lines, then approve it "
			"to post it and put it on the list to pay.</p>"
			"<form method=\"post\" action=\"/bills/%" G_GINT64_FORMAT "/approve\"><button class=\"btn btn-primary\" type=\"submit\">Approve</button></form>"
			"<p class=\"muted small\">Need to change it? Edit it now \xe2\x80\x94 nothing is fixed until it is approved.</p>", id);
	}
	else if (g_strcmp0(state, "approved") == 0 || g_strcmp0(state, "partially_paid") == 0)
	{
		g_autofree gchar *owed = balance != NULL ? venture_money_to_display_string(balance, TRUE) : NULL;
		g_string_append(content, "<p class=\"next-up-lead\">");
		if (owed != NULL)
		{
			g_string_append(content, "<strong>");
			venture_html_escape_append(content, owed);
			g_string_append(content, "</strong> to pay");
		}
		else
			g_string_append(content, "To pay");
		if (due_text != NULL)
		{
			g_string_append(content, ", due <strong>");
			venture_html_escape_append(content, due_text);
			g_string_append(content, "</strong>");
		}
		g_string_append_printf(content, ".</p><form method=\"post\" action=\"/bills/%" G_GINT64_FORMAT "/pay\">"
			"<button class=\"btn btn-primary\" type=\"submit\">Pay it now</button></form>"
			"<p class=\"muted small\">The amount is in the books, so it cannot be edited. If the supplier lowers it, record "
			"their <a href=\"/e/vendor_credit/new?company_id=%" G_GINT64_FORMAT "\">credit</a>; if it was wrong and nothing "
			"is paid yet, void it and enter it again.</p>", id, supplier);
		if (g_strcmp0(state, "approved") == 0)
			g_string_append_printf(content, "<form method=\"post\" action=\"/bills/%" G_GINT64_FORMAT "/void\">"
				"<button class=\"btn btn-ghost btn-sm\" type=\"submit\">Void this bill</button></form>", id);
	}
	else if (g_strcmp0(state, "paid") == 0)
		g_string_append(content, "<p class=\"next-up-lead\">Paid. Nothing more to do.</p>"
			"<p class=\"muted small\">If the supplier refunds part of it, record the refund; it reopens the bill for what is owed.</p>");
	else
		g_string_append(content, "<p class=\"next-up-lead\">Void. Its posting was reversed; it is kept as evidence.</p>");
	g_string_append(content, "</div></section>");

	/* The lines, as the supplier printed them: the bill is its lines. */
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_VENDOR_BILL_LINE);
		g_autoptr(GPtrArray) lines = NULL;
		guint i;
		venture_query_set_limit(query, 0);
		venture_query_add_filter_int(query, "bill-id", VENTURE_FILTER_OP_EQ, id, NULL);
		venture_query_add_order(query, "position", VENTURE_SORT_ASCENDING, NULL);
		lines = venture_database_find(venture_context_get_database(self->context), query, NULL);
		if (lines != NULL && lines->len > 0)
		{
			g_string_append(content, "<section class=\"card\"><div class=\"card-head\"><h2>Lines</h2></div>"
				"<table class=\"data\"><thead><tr><th>What it was for</th><th class=\"num\">Qty</th>"
				"<th class=\"num\">Unit price</th><th class=\"num\">Tax</th></tr></thead><tbody>");
			for (i = 0; i < lines->len; i++)
			{
				VentureEntity *line = g_ptr_array_index(lines, i);
				g_autofree gchar *description = NULL, *quantity = NULL, *price_text = NULL, *tax_text = NULL;
				g_autoptr(VentureMoney) price = NULL, tax = NULL;
				g_object_get(line, "description", &description, "quantity", &quantity, "unit-price", &price,
					"tax-amount", &tax, NULL);
				price_text = price != NULL ? venture_money_to_display_string(price, TRUE) : g_strdup("");
				tax_text = tax != NULL && venture_money_get_amount(tax) != 0
					? venture_money_to_display_string(tax, TRUE) : g_strdup("");
				g_string_append(content, "<tr><td>");
				venture_html_escape_append(content, description);
				g_string_append(content, "</td><td class=\"num\">");
				venture_html_escape_append(content, quantity);
				g_string_append(content, "</td><td class=\"num\">");
				venture_html_escape_append(content, price_text);
				g_string_append(content, "</td><td class=\"num\">");
				venture_html_escape_append(content, tax_text);
				g_string_append(content, "</td></tr>");
			}
			g_string_append(content, "</tbody></table></section>");
		}
	}
}
