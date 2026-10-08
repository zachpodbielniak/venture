/*
 * venture-bill-compose-web.inc - Entering a supplier's bill as the bill
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Included by venture-web-server.c. A bill is entered the way it arrives:
 * who sent it, its number and dates, and its lines with the account each
 * one is spent from. "How often?" asks the same question the invoice sheet
 * asks: one bill, or the same bill on a schedule (rent, a subscription you
 * pay for, insurance). The bill and its lines are written in one
 * transaction, and approval -- the step that puts it in the books -- goes
 * through the payables service like every other writer's.
 */

/* How many line rows one entered bill may carry. */
#define VENTURE_BILL_MAX_LINES 200

static const gchar *const venture_bill_line_fields[] = {
	"description", "quantity", "unit-price", "account-id", "tax-amount", NULL
};

/* The organization's book currency, which every amount on the sheet is
 * in; the install's when it names none, never "USD". */
static gchar *
venture_bill_currency(VentureWebServer *self, gint64 organization)
{
	return venture_database_get_book_currency(venture_context_get_database(self->context),
	                                          organization);
}

/* The expense accounts a line can be spent from, as options. */
static gchar *
venture_bill_account_options(VentureWebServer *self, gint64 organization)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ACCOUNT);
	g_autoptr(GPtrArray) rows = NULL;
	GString *html = g_string_new("<option value=\"\">General expenses</option>");
	guint i;

	venture_query_set_limit(query, 0);
	venture_query_add_order(query, "code", VENTURE_SORT_ASCENDING, NULL);
	venture_query_add_filter_string(query, "kind", VENTURE_FILTER_OP_EQ, "expense", NULL);
	if (organization > 0)
		venture_query_set_organization(query, organization);
	rows = venture_database_find(venture_context_get_database(self->context), query, NULL);
	for (i = 0; rows != NULL && i < rows->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(rows, i);
		g_autofree gchar *name = venture_entity_get_display_name(row);
		g_string_append_printf(html, "<option value=\"%" G_GINT64_FORMAT "\">", venture_entity_get_id(row));
		venture_html_escape_append(html, name);
		g_string_append(html, "</option>");
	}
	return g_string_free(html, FALSE);
}

static void
venture_bill_append_line_row(GString *html, const gchar *index, const gchar *accounts)
{
	g_string_append_printf(html,
		"<tr class=\"line-row\" data-line>"
		"<td class=\"line-item\"><input type=\"text\" name=\"line-%s-description\" "
		"placeholder=\"What it was for\" aria-label=\"Description\">"
		"<select name=\"line-%s-account-id\" class=\"line-product\" aria-label=\"Spent from\" data-no-picker>%s</select></td>"
		"<td class=\"num\" data-label=\"Qty\"><input type=\"number\" name=\"line-%s-quantity\" value=\"1\" min=\"0\" step=\"any\" "
		"inputmode=\"decimal\" aria-label=\"Quantity\" data-qty></td>"
		"<td class=\"num\" data-label=\"Unit price\"><input type=\"text\" name=\"line-%s-unit-price\" placeholder=\"0.00\" "
		"inputmode=\"decimal\" aria-label=\"Unit price\" data-unit></td>"
		"<td class=\"num\" data-label=\"Tax\"><input type=\"text\" name=\"line-%s-tax-amount\" placeholder=\"0.00\" "
		"inputmode=\"decimal\" aria-label=\"Tax\" data-tax-amount></td>"
		"<td class=\"num line-amount\" data-label=\"Amount\" data-amount>0.00</td>"
		VENTURE_WEB_LINE_DELETE "</tr>",
		index, index, accounts, index, index, index);
}

/* The supplier picker. */
static void
venture_bill_append_supplier(VentureWebServer *self, GString *html, gint64 organization)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_COMPANY);
	g_autoptr(GPtrArray) rows = NULL;
	guint i;

	/* Only suppliers: the payables service refuses a bill from anyone
	 * else, so offering them would be offering an error. */
	venture_query_set_limit(query, 0);
	venture_query_add_order(query, "name", VENTURE_SORT_ASCENDING, NULL);
	venture_query_add_filter_string(query, "kind", VENTURE_FILTER_OP_EQ, "supplier", NULL);
	if (organization > 0)
		venture_query_set_organization(query, organization);
	rows = venture_database_find(venture_context_get_database(self->context), query, NULL);
	g_string_append(html, "<label class=\"doc-field doc-field-wide\"><span class=\"field-label\">Supplier"
		"<span class=\"required\">*</span></span><select name=\"company-id\" required>"
		"<option value=\"\">Who sent it\xe2\x80\xa6</option>");
	for (i = 0; rows != NULL && i < rows->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(rows, i);
		g_autofree gchar *name = venture_entity_get_display_name(row);
		g_string_append_printf(html, "<option value=\"%" G_GINT64_FORMAT "\">", venture_entity_get_id(row));
		venture_html_escape_append(html, name);
		g_string_append(html, "</option>");
	}
	g_string_append(html, "</select><span class=\"field-help\">Not listed? <a href=\"/e/company/new\">Add them</a> "
		"as a company whose relationship is Supplier.</span></label>");
}

static gchar *
venture_bill_compose_page(VentureWebServer *self, HtmxRequest *request)
{
	GString *html = g_string_new(NULL);
	gint64 organization = venture_web_active_organization(self, request);
	gboolean repeat = htmx_request_get_query_param(request, "repeat") != NULL &&
		venture_web_module_enabled(self, "recurring");
	g_autofree gchar *accounts = NULL;
	g_autofree gchar *currency = NULL;
	g_autoptr(GDateTime) today = venture_time_from_string("today", NULL);
	g_autofree gchar *day = venture_time_to_date_string(today, NULL);

	/* The organization the post will file the bill under -- the same
	 * fallback, or the sheet offers every organization's accounts and
	 * totals in a currency the saved bill is not in. */
	if (organization == 0)
		organization = venture_context_get_default_organization_id(self->context);
	accounts = venture_bill_account_options(self, organization);
	currency = venture_bill_currency(self, organization);

	g_string_append(html, "<form method=\"post\" action=\"/bills/compose\" class=\"doc-compose\" data-doc-compose>"
		"<div class=\"page-head\"><div class=\"page-title\">");
	venture_web_append_crumbs(html, VENTURE_TYPE_VENDOR_BILL, "vendor_bill", TRUE);
	if (repeat)
		g_string_append(html, "<h1>New repeating bill</h1></div><div class=\"page-actions\">"
			"<input type=\"hidden\" name=\"repeat\" value=\"1\">"
			"<a class=\"btn btn-ghost\" href=\"/e/recurring_schedule\">Cancel</a>"
			"<button class=\"btn btn-primary\" type=\"submit\">Save schedule</button></div></div>");
	else
		g_string_append(html, "<h1>New bill</h1></div><div class=\"page-actions\">"
			"<a class=\"btn btn-ghost\" href=\"/e/vendor_bill\">Cancel</a>"
			"<button class=\"btn\" type=\"submit\">Save draft</button>"
			"<button class=\"btn btn-primary\" type=\"submit\" name=\"approve\" value=\"1\">Save and approve</button>"
			"</div></div>");

	if (venture_web_module_enabled(self, "recurring"))
		g_string_append_printf(html, "<nav class=\"how-often\" aria-label=\"How often\">"
			"<span class=\"how-often-label\">How often?</span>"
			"<a class=\"how-often-choice%s\" href=\"/bills/compose\"><strong>Just this bill</strong>"
			"<span>One bill from a supplier, as it arrived.</span></a>"
			"<a class=\"how-often-choice%s\" href=\"/bills/compose?repeat=1\"><strong>Same bill, on a schedule</strong>"
			"<span>Rent, insurance, a service you pay for. Edits change the bills still to come.</span></a>"
			"</nav>", repeat ? "" : " active", repeat ? " active" : "");

	g_string_append(html, "<div class=\"doc-layout\"><section class=\"card doc-sheet\"><div class=\"card-body\">"
		"<div class=\"doc-parties\">");
	venture_bill_append_supplier(self, html, organization);
	if (!repeat)
		g_string_append_printf(html,
			"<label class=\"doc-field\"><span class=\"field-label\">Their bill number<span class=\"required\">*</span></span>"
			"<input type=\"text\" name=\"number\" required placeholder=\"As printed on the bill\"></label>"
			"<label class=\"doc-field\"><span class=\"field-label\">Bill date</span>"
			"<input type=\"date\" name=\"bill-date\" value=\"%s\" required></label>", day);
	g_string_append(html, "</div>");
	if (!repeat)
		g_string_append(html, "<div class=\"doc-parties\"><label class=\"doc-field\"><span class=\"field-label\">Due</span>"
			"<input type=\"date\" name=\"due-date\"><span class=\"field-help\">When you have to pay it.</span></label></div>");

	/* A line editor: its rows post in order (data-no-sort). */
	g_string_append_printf(html, "<table class=\"line-items\" data-no-sort><thead><tr>"
		"<th>What it was for, and spent from</th><th class=\"num\">Qty</th><th class=\"num\">Unit price</th>"
		"<th class=\"num\">Tax</th><th class=\"num\">Amount</th>"
		"<th><span class=\"visually-hidden\">Remove</span></th></tr></thead><tbody data-lines>");
	venture_bill_append_line_row(html, "0", accounts);
	g_string_append(html, "</tbody></table>"
		"<button type=\"button\" class=\"btn btn-ghost add-line\" data-add-line>+ Add a line</button>"
		"<template data-line-template>");
	venture_bill_append_line_row(html, "__N__", accounts);
	g_string_append(html, "</template></div></section><aside class=\"doc-aside\">");

	/* The currency is whatever the organization's settings say. */
	g_string_append(html, "<section class=\"card doc-totals\"><div class=\"card-body\">"
		"<dl class=\"totals\"><div><dt>Before tax</dt><dd data-subtotal>0.00</dd></div>"
		"<div><dt>Tax</dt><dd data-tax-total>0.00</dd></div>"
		"<div class=\"grand\"><dt>Total, ");
	venture_html_escape_append(html, currency);
	g_string_append(html, "</dt><dd data-total>0.00</dd></div></dl></div></section>");

	if (repeat)
		g_string_append_printf(html, "<section class=\"card\"><div class=\"card-head\"><h2>Schedule</h2></div><div class=\"card-body\">"
			"<label class=\"doc-field\"><span class=\"field-label\">Every</span><select name=\"repeat-frequency\">"
			"<option value=\"monthly\" selected>Month</option><option value=\"weekly\">Week</option>"
			"<option value=\"yearly\">Year</option></select></label>"
			"<label class=\"doc-field\"><span class=\"field-label\">First bill on</span>"
			"<input type=\"date\" name=\"repeat-start\" value=\"%s\" required></label>"
			"<label class=\"doc-field\"><span class=\"field-label\">Last bill on</span>"
			"<input type=\"date\" name=\"repeat-end\"><span class=\"field-help\">Leave empty to keep going.</span></label>"
			"<label class=\"checkbox\"><input type=\"checkbox\" name=\"repeat-auto\" value=\"true\"> "
			"Approve each one automatically</label></div></section>", day);

	g_string_append(html, "<section class=\"card\"><div class=\"card-body explainer\">"
		"<h3>If the bill changes</h3>"
		"<p><strong>Before you approve it</strong>, change anything. It is not in the books yet.</p>"
		"<p><strong>After approval</strong> its amount and dates are fixed, because they are in the books. "
		"If the supplier sends a lower amount, record their <a href=\"/e/vendor_credit/new\">credit</a>; if it was "
		"wrong and nothing is paid yet, void it and enter it again.</p>"
		"<p><strong>A repeating bill</strong> that changes price: edit the schedule, and the bills still to come "
		"use the new amount. Bills already made are left as they were.</p>"
		"</div></section>"
		"<details class=\"card form-section\"><summary>Memo</summary><div class=\"card-body\">"
		"<label class=\"doc-field\"><span class=\"field-label\">Memo</span><textarea name=\"memo\" rows=\"3\"></textarea>"
		"<span class=\"field-help\">For you; kept with the bill.</span></label></div></details>"
		"</aside></div></form>");

	g_string_append(html, "<script>(function(){"
		"var f=document.querySelector('[data-doc-compose]');if(!f)return;"
		"var body=f.querySelector('[data-lines]'),tpl=f.querySelector('[data-line-template]'),n=1;"
		"function num(v){v=parseFloat(String(v||'').replace(/[^0-9.\\-]/g,''));return isNaN(v)?0:v;}"
		"function recalc(){var sub=0,tax=0;"
		"body.querySelectorAll('[data-line]').forEach(function(r){"
		"var q=num(r.querySelector('[data-qty]').value),p=num(r.querySelector('[data-unit]').value),"
		"t=num(r.querySelector('[data-tax-amount]').value);sub+=q*p;tax+=t;"
		"r.querySelector('[data-amount]').textContent=(q*p+t).toFixed(2);});"
		"f.querySelector('[data-subtotal]').textContent=sub.toFixed(2);"
		"f.querySelector('[data-tax-total]').textContent=tax.toFixed(2);"
		"f.querySelector('[data-total]').textContent=(sub+tax).toFixed(2);}"
		"f.addEventListener('input',recalc);"
		"f.querySelector('[data-add-line]').addEventListener('click',function(){"
		"body.insertAdjacentHTML('beforeend',tpl.innerHTML.replace(/__N__/g,String(n++)));"
		"body.lastElementChild.querySelector('input').focus();});"
		"body.addEventListener('click',function(e){var b=e.target.closest('[data-remove-line]');"
		"if(!b)return;if(body.querySelectorAll('[data-line]').length>1)b.closest('[data-line]').remove();recalc();});"
		"recalc();})();</script>");

	return g_string_free(html, FALSE);
}

/* An amount typed on the sheet, in the bill's currency. */
static gchar *
venture_bill_money_text(const gchar *typed, const gchar *currency)
{
	if (venture_string_is_empty(typed))
		return NULL;
	if (strpbrk(typed, "ABCDEFGHIJKLMNOPQRSTUVWXYZ") != NULL)
		return g_strdup(typed);
	return g_strdup_printf("%s %s", typed, currency);
}

/*
 * The same bill on a schedule. The template is built from the form's own
 * fields -- supplier, memo and lines -- so nothing of one bill (its number,
 * its dates) is stamped on every bill the schedule makes.
 */
static HtmxResponse *
venture_bill_save_schedule(VentureWebServer *self, HtmxRequest *request, gint64 organization,
	const gchar *currency, const VentureActor *actor)
{
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(JsonNode) root = NULL;
	g_autoptr(VentureEntity) schedule = NULL;
	g_autoptr(VentureEntity) supplier = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *template_text = NULL, *timezone = NULL, *name = NULL, *supplier_name = NULL;
	const gchar *company_id = htmx_request_get_form_value(request, "company-id");
	const gchar *memo = htmx_request_get_form_value(request, "memo");
	const gchar *end = htmx_request_get_form_value(request, "repeat-end");
	guint i, lines = 0;

	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "company_id");
	json_builder_add_int_value(builder, company_id != NULL ? g_ascii_strtoll(company_id, NULL, 10) : 0);
	json_builder_set_member_name(builder, "currency");
	json_builder_add_string_value(builder, currency);
	if (!venture_string_is_empty(memo))
	{
		json_builder_set_member_name(builder, "memo");
		json_builder_add_string_value(builder, memo);
	}
	json_builder_set_member_name(builder, "lines");
	json_builder_begin_array(builder);
	for (i = 0; i < VENTURE_BILL_MAX_LINES; i++)
	{
		g_autofree gchar *description_name = g_strdup_printf("line-%u-description", i);
		const gchar *description = htmx_request_get_form_value(request, description_name);
		guint f;
		if (venture_string_is_empty(description))
			continue;
		json_builder_begin_object(builder);
		for (f = 0; venture_bill_line_fields[f] != NULL; f++)
		{
			g_autofree gchar *field = g_strdup_printf("line-%u-%s", i, venture_bill_line_fields[f]);
			g_autofree gchar *wire = g_strdup(venture_bill_line_fields[f]);
			const gchar *value = htmx_request_get_form_value(request, field);
			if (venture_string_is_empty(value))
				continue;
			g_strdelimit(wire, "-", '_');
			json_builder_set_member_name(builder, wire);
			if (g_strcmp0(venture_bill_line_fields[f], "account-id") == 0)
				json_builder_add_int_value(builder, g_ascii_strtoll(value, NULL, 10));
			else if (g_str_has_suffix(venture_bill_line_fields[f], "price") ||
			         g_str_has_suffix(venture_bill_line_fields[f], "amount"))
			{
				g_autofree gchar *money = venture_bill_money_text(value, currency);
				json_builder_add_string_value(builder, money);
			}
			else
				json_builder_add_string_value(builder, value);
		}
		json_builder_end_object(builder);
		lines++;
	}
	json_builder_end_array(builder);
	json_builder_end_object(builder);
	if (venture_string_is_empty(company_id) || lines == 0)
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"A repeating bill needs a supplier and at least one line");
		return venture_web_error_response(error);
	}
	root = json_builder_get_root(builder);
	template_text = venture_json_to_string(root, FALSE);
	supplier = venture_database_get(venture_context_get_database(self->context), VENTURE_TYPE_COMPANY,
		g_ascii_strtoll(company_id, NULL, 10), NULL);
	supplier_name = supplier != NULL ? venture_entity_get_display_name(supplier) : g_strdup("Supplier");
	name = g_strdup_printf("%s \xe2\x80\x94 repeating bill", supplier_name);
	g_object_get(venture_context_get_config(self->context), "locale-timezone", &timezone, NULL);

	schedule = g_object_new(VENTURE_TYPE_RECURRING_SCHEDULE, NULL);
	venture_entity_set_organization_id(schedule, organization);
	if (!venture_entity_set_field_from_string(schedule, "name", name, &error) ||
		!venture_entity_set_field_from_string(schedule, "kind", "bill", &error) ||
		!venture_entity_set_field_from_string(schedule, "frequency",
			htmx_request_get_form_value(request, "repeat-frequency"), &error) ||
		!venture_entity_set_field_from_string(schedule, "timezone", timezone, &error) ||
		!venture_entity_set_field_from_string(schedule, "start-at",
			htmx_request_get_form_value(request, "repeat-start"), &error) ||
		(!venture_string_is_empty(end) && !venture_entity_set_field_from_string(schedule, "end-at", end, &error)) ||
		!venture_entity_set_field_from_string(schedule, "auto-post",
			htmx_request_get_form_value(request, "repeat-auto") != NULL ? "true" : "false", &error) ||
		!venture_entity_set_field_from_string(schedule, "template", template_text, &error) ||
		!venture_database_save(venture_context_get_database(self->context), schedule, actor, &error))
		return venture_web_error_response(error);
	{
		g_autofree gchar *path = g_strdup_printf("/e/recurring_schedule/%" G_GINT64_FORMAT,
			venture_entity_get_id(schedule));
		return venture_web_redirect_to(path);
	}
}

/*
 * One bill: the header and its lines in one transaction, then -- when
 * "Save and approve" was pressed -- approval through the payables service,
 * which freezes the amount and posts it. A failure anywhere leaves nothing.
 */
static gboolean
venture_bill_save(VentureWebServer *self, HtmxRequest *request, gint64 organization,
	const gchar *currency, const VentureActor *actor, gint64 *out_id, GError **error)
{
	static const gchar *const header[] = { "company-id", "number", "bill-date", "due-date", "memo", NULL };
	VentureDatabase *database = venture_context_get_database(self->context);
	g_autoptr(VentureEntity) bill = g_object_new(VENTURE_TYPE_VENDOR_BILL, NULL);
	guint i, position = 0;

	venture_entity_set_organization_id(bill, organization);
	for (i = 0; header[i] != NULL; i++)
	{
		const gchar *value = htmx_request_get_form_value(request, header[i]);
		if (!venture_string_is_empty(value) &&
			!venture_entity_set_field_from_string(bill, header[i], value, error))
			return FALSE;
	}
	g_object_set(bill, "currency", currency, "status", "draft", NULL);

	if (!venture_database_begin(database, error))
		return FALSE;
	if (!venture_database_save(database, bill, actor, error))
		goto fail;
	for (i = 0; i < VENTURE_BILL_MAX_LINES; i++)
	{
		g_autofree gchar *description_name = g_strdup_printf("line-%u-description", i);
		const gchar *description = htmx_request_get_form_value(request, description_name);
		g_autoptr(VentureEntity) line = NULL;
		guint f;
		if (venture_string_is_empty(description))
			continue;
		line = g_object_new(VENTURE_TYPE_VENDOR_BILL_LINE, NULL);
		venture_entity_set_organization_id(line, organization);
		g_object_set(line, "bill-id", venture_entity_get_id(bill), "position", (gint64)position++, NULL);
		for (f = 0; venture_bill_line_fields[f] != NULL; f++)
		{
			g_autofree gchar *field = g_strdup_printf("line-%u-%s", i, venture_bill_line_fields[f]);
			g_autofree gchar *text = NULL;
			const gchar *value = htmx_request_get_form_value(request, field);
			if (venture_string_is_empty(value))
				continue;
			text = (g_str_has_suffix(venture_bill_line_fields[f], "price") ||
			        g_str_has_suffix(venture_bill_line_fields[f], "amount"))
				? venture_bill_money_text(value, currency) : g_strdup(value);
			if (!venture_entity_set_field_from_string(line, venture_bill_line_fields[f], text, error))
				goto fail;
		}
		if (!venture_database_save(database, line, actor, error))
			goto fail;
	}
	if (position == 0)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "A bill needs at least one line");
		goto fail;
	}
	if (htmx_request_get_form_value(request, "approve") != NULL)
	{
		g_autoptr(VentureEntity) command = venture_payables_service_prepare_action(
			venture_payables_service_get(database), venture_entity_get_id(bill), "approve", NULL, error);
		if (command == NULL || !venture_database_save(database, command, actor, error))
			goto fail;
	}
	if (!venture_database_commit(database, error))
		return FALSE;
	*out_id = venture_entity_get_id(bill);
	return TRUE;
fail:
	venture_database_rollback(database);
	return FALSE;
}

static HtmxResponse *
venture_bill_compose(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *currency = NULL;
	HtmxResponse *gate;
	VentureActor actor;
	gint64 organization, id = 0;

	(void)params;
	gate = venture_web_ui_require_session(self, request);
	if (gate != NULL)
		return gate;
	gate = venture_web_require_module_ui(self, request, "payables");
	if (gate != NULL)
		return gate;
	if (htmx_request_get_method(request) == HTMX_METHOD_GET)
		return venture_web_html_response(venture_web_page(self, request, "/e/vendor_bill", "New bill",
			venture_bill_compose_page(self, request)), 200);

	principal = venture_auth_authenticate(self->auth, request);
	if (!venture_web_hosted_auth_require(self, request, principal, VENTURE_USER_ROLE_EDITOR, &error))
		return venture_web_error_response(error);
	venture_auth_to_actor(principal, &actor);
	organization = venture_web_active_organization(self, request);
	if (organization == 0)
		organization = venture_context_get_default_organization_id(self->context);
	currency = venture_bill_currency(self, organization);

	if (htmx_request_get_form_value(request, "repeat") != NULL)
	{
		if (!venture_web_module_enabled(self, "recurring"))
		{
			g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Repeating bills are switched off");
			return venture_web_error_response(error);
		}
		return venture_bill_save_schedule(self, request, organization, currency, &actor);
	}
	if (!venture_bill_save(self, request, organization, currency, &actor, &id, &error))
		return venture_web_error_response(error);
	{
		g_autofree gchar *path = g_strdup_printf("/e/vendor_bill/%" G_GINT64_FORMAT, id);
		return venture_web_redirect_to(path);
	}
}
