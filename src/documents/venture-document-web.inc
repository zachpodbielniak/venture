/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Restrict the guided workflow to inputs accepted by the shared composer; field types stay metadata-derived. */
/* How many line rows one composed document may carry. */
#define VENTURE_DOCUMENT_MAX_LINES 200

static const gchar *const document_header_fields[] = { "number", "company-id", "contact-id", "terms", "notes", NULL };
static const gchar *const document_invoice_fields[] = { "tax-exempt", NULL };
static const gchar *const document_line_fields[] = { "description", "quantity", "unit-price", "product-id", "discount-percent", "tax-percent", NULL };
static const gchar *const document_invoice_line_fields[] = { "tax-code-id", NULL };

static gboolean
document_read_fields(HtmxRequest *request, VentureEntity *record, const gchar *const *fields,
	const gchar *prefix, GError **error)
{
	guint i;
	for (i = 0; fields[i] != NULL; i++)
	{
		g_autofree gchar *name = g_strconcat(prefix, fields[i], NULL);
		const gchar *value = htmx_request_get_form_value(request, name);
		if (value != NULL && value[0] != '\0' &&
			!venture_entity_set_field_from_string(record, fields[i], value, error))
			return FALSE;
	}
	return TRUE;
}

/*
 * Why a customer pays no tax, as the return wants it: the kind of
 * exemption and the certificate that proves it, in one line that is
 * frozen onto the invoice at issue.
 */
static gchar *
document_exemption_reason(HtmxRequest *request)
{
	const gchar *kind = htmx_request_get_form_value(request, "exempt-kind");
	const gchar *number = htmx_request_get_form_value(request, "exempt-number");

	if (htmx_request_get_form_value(request, "tax-exempt") == NULL)
		return NULL;
	if (kind == NULL || *kind == '\0')
		kind = "Exempt";
	if (number != NULL && *number != '\0')
		return g_strdup_printf("%s, certificate %s", kind, number);
	return g_strdup(kind);
}

static JsonObject *
document_spec_from_request(HtmxRequest *request, GError **error)
{
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(JsonNode) body = NULL;
	const gchar *form = htmx_request_get_form_value(request, "payload");
	if (htmx_request_get_form_value(request, "compose-form") != NULL)
	{
		gboolean quote = strstr(htmx_request_get_path(request), "quote") != NULL;
		g_autoptr(VentureEntity) header = g_object_new(quote ? VENTURE_TYPE_QUOTE : VENTURE_TYPE_INVOICE, NULL);
		g_autoptr(JsonNode) node = NULL;
		JsonObject *spec;
		JsonArray *lines = json_array_new();
		guint i;
		if (!document_read_fields(request, header, document_header_fields, "", error) ||
			(!quote && !document_read_fields(request, header, document_invoice_fields, "", error)))
		{
			json_array_unref(lines);
			return NULL;
		}
		if (!quote)
		{
			g_autofree gchar *reason = document_exemption_reason(request);
			if (reason != NULL)
				g_object_set(header, "tax-exempt-reason", reason, NULL);
		}
		node = venture_serializable_to_json(VENTURE_SERIALIZABLE(header), FALSE);
		spec = json_object_ref(json_node_get_object(node));
		json_object_set_array_member(spec, "lines", lines);
		json_object_set_boolean_member(spec, "send", htmx_request_get_form_value(request, "send") != NULL);
		/* Rows are numbered by the page, which adds and removes them;
		 * a gap in the numbering is a row somebody deleted. */
		for (i = 0; i < VENTURE_DOCUMENT_MAX_LINES; i++)
		{
			g_autofree gchar *prefix = g_strdup_printf("line-%u-", i);
			g_autofree gchar *description_name = g_strconcat(prefix, "description", NULL);
			const gchar *description = htmx_request_get_form_value(request, description_name);
			g_autoptr(VentureEntity) line = NULL;
			g_autoptr(JsonNode) line_node = NULL;
			g_autoptr(VentureMoney) price = NULL;
			g_autofree gchar *price_text = NULL;
			if (description == NULL || description[0] == '\0')
				continue;
			line = g_object_new(quote ? VENTURE_TYPE_QUOTE_LINE : VENTURE_TYPE_INVOICE_LINE, NULL);
			if (!document_read_fields(request, line, document_line_fields, prefix, error) ||
				(!quote && !document_read_fields(request, line, document_invoice_line_fields, prefix, error)))
			{
				json_object_unref(spec);
				return NULL;
			}
			line_node = venture_serializable_to_json(VENTURE_SERIALIZABLE(line), FALSE);
			/* Record JSON represents money as an object; the compose contract
			 * accepts the exact textual amount shared with the CLI. */
			g_object_get(line, "unit-price", &price, NULL);
			if (price != NULL)
			{
				price_text = venture_money_to_string(price);
				json_object_set_string_member(json_node_get_object(line_node), "unit_price", price_text);
			}
			json_array_add_element(lines, g_steal_pointer(&line_node));
		}
		return spec;
	}
	if (form != NULL)
	{
		if (!json_parser_load_from_data(parser, form, -1, error))
			return NULL;
		body = json_node_copy(json_parser_get_root(parser));
	}
	else
		body = htmx_request_get_json(request, error);
	if (body == NULL || !JSON_NODE_HOLDS_OBJECT(body))
	{
		g_clear_error(error);
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Compose requires a JSON object");
		return NULL;
	}
	return json_object_ref(json_node_get_object(body));
}

/*
 * The customer and contact pickers. A contact carries the company it
 * works for, so choosing a customer narrows "Attention of" to their
 * people; with scripting off every contact is offered.
 */
static void
document_append_parties(VentureWebServer *self, GString *html, gint64 organization)
{
	g_autoptr(VentureQuery) companies = venture_query_new(VENTURE_TYPE_COMPANY);
	g_autoptr(VentureQuery) contacts = venture_query_new(VENTURE_TYPE_CONTACT);
	g_autoptr(GPtrArray) company_rows = NULL;
	g_autoptr(GPtrArray) contact_rows = NULL;
	guint i;

	venture_query_set_limit(companies, 0);
	venture_query_set_limit(contacts, 0);
	venture_query_add_order(companies, "name", VENTURE_SORT_ASCENDING, NULL);
	venture_query_add_order(contacts, "name", VENTURE_SORT_ASCENDING, NULL);
	if (organization > 0)
	{
		venture_query_set_organization(companies, organization);
		venture_query_set_organization(contacts, organization);
	}
	company_rows = venture_database_find(venture_context_get_database(self->context), companies, NULL);
	contact_rows = venture_database_find(venture_context_get_database(self->context), contacts, NULL);

	g_string_append(html, "<label class=\"doc-field doc-field-wide\"><span class=\"field-label\">Customer"
		"<span class=\"required\">*</span></span><select name=\"company-id\" required data-doc-company>"
		"<option value=\"\">Choose a customer\xe2\x80\xa6</option>");
	for (i = 0; company_rows != NULL && i < company_rows->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(company_rows, i);
		g_autofree gchar *name = venture_entity_get_display_name(row);
		g_autofree gchar *reason = NULL, *number = NULL;
		gboolean exempt = FALSE;

		/* What the customer's record already says about tax rides on the
		 * option, so choosing them fills the exemption in. */
		g_object_get(row, "tax-exempt", &exempt, "tax-exempt-reason", &reason,
			"tax-exemption-number", &number, NULL);
		g_string_append_printf(html, "<option value=\"%" G_GINT64_FORMAT "\"", venture_entity_get_id(row));
		if (exempt)
		{
			g_string_append(html, " data-exempt=\"1\" data-exempt-kind=\"");
			venture_html_escape_append(html, reason != NULL ? reason : "");
			g_string_append(html, "\" data-exempt-number=\"");
			venture_html_escape_append(html, number != NULL ? number : "");
			g_string_append(html, "\"");
		}
		g_string_append(html, ">");
		venture_html_escape_append(html, name);
		g_string_append(html, "</option>");
	}
	g_string_append(html, "</select></label>");

	/* Only the chosen customer's people: an invoice to one company is
	 * never for the attention of somebody at another. The save refuses it
	 * as well; this only stops it being offered. */
	g_string_append(html, "<label class=\"doc-field\"><span class=\"field-label\">Attention of</span>"
		"<select name=\"contact-id\" data-doc-contact><option value=\"\">Nobody in particular</option>");
	for (i = 0; contact_rows != NULL && i < contact_rows->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(contact_rows, i);
		g_autofree gchar *name = venture_entity_get_display_name(row);
		gint64 company = 0;
		g_object_get(row, "company-id", &company, NULL);
		g_string_append_printf(html, "<option value=\"%" G_GINT64_FORMAT "\" data-company=\"%" G_GINT64_FORMAT "\">",
			venture_entity_get_id(row), company);
		venture_html_escape_append(html, name);
		g_string_append(html, "</option>");
	}
	g_string_append(html, "</select></label>");
}

/* The product picker's options, each carrying what it fills in. */
static gchar *
document_product_options(VentureWebServer *self, gint64 organization)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_PRODUCT);
	g_autoptr(GPtrArray) rows = NULL;
	GString *html = g_string_new("<option value=\"\">\xe2\x80\x94</option>");
	guint i;

	venture_query_set_limit(query, 0);
	venture_query_add_order(query, "name", VENTURE_SORT_ASCENDING, NULL);
	if (organization > 0)
		venture_query_set_organization(query, organization);
	rows = venture_database_find(venture_context_get_database(self->context), query, NULL);
	for (i = 0; rows != NULL && i < rows->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(rows, i);
		g_autofree gchar *name = venture_entity_get_display_name(row);
		g_autoptr(VentureMoney) price = NULL;
		g_autofree gchar *price_text = NULL;
		g_object_get(row, "list-price", &price, NULL);
		price_text = price != NULL ? venture_money_to_string(price) : g_strdup("");
		g_string_append_printf(html, "<option value=\"%" G_GINT64_FORMAT "\" data-price=\"", venture_entity_get_id(row));
		venture_html_escape_append(html, price_text);
		g_string_append(html, "\">");
		venture_html_escape_append(html, name);
		g_string_append(html, "</option>");
	}
	return g_string_free(html, FALSE);
}

/*
 * The tax picker's options: no tax, then every active tax rate, each
 * carrying its percent so the preview can add it up. A rate is a record,
 * not a number typed on each line, so the return reads the jurisdiction
 * it was charged under.
 */
static gchar *
document_tax_options(VentureWebServer *self, gint64 organization)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_TAX_CODE);
	g_autoptr(GPtrArray) rows = NULL;
	GString *html = g_string_new("<option value=\"\" data-rate=\"0\">No tax</option>");
	guint i;

	venture_query_set_limit(query, 0);
	venture_query_add_order(query, "name", VENTURE_SORT_ASCENDING, NULL);
	if (organization > 0)
		venture_query_set_organization(query, organization);
	rows = venture_database_find(venture_context_get_database(self->context), query, NULL);
	for (i = 0; rows != NULL && i < rows->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(rows, i);
		g_autofree gchar *name = venture_entity_get_display_name(row);
		gint64 numerator = 0, denominator = 0;
		gboolean active = TRUE;
		gchar percent[G_ASCII_DTOSTR_BUF_SIZE];

		g_object_get(row, "active", &active, NULL);
		if (!active || !venture_tax_code_get_rate(VENTURE_TAX_CODE(row), &numerator, &denominator, NULL))
			continue;
		g_ascii_formatd(percent, sizeof percent, "%.4g", (gdouble)numerator * 100.0 / (gdouble)denominator);
		g_string_append_printf(html, "<option value=\"%" G_GINT64_FORMAT "\" data-rate=\"%s\">",
			venture_entity_get_id(row), percent);
		venture_html_escape_append(html, name);
		g_string_append_printf(html, " \xc2\xb7 %s%%</option>", percent);
	}
	return g_string_free(html, FALSE);
}

/* One line row. The same names the composer has always read, so the API,
 * the CLI and this page post one contract. */
static void
document_append_line_row(GString *html, const gchar *index, const gchar *products, const gchar *taxes)
{
	g_string_append_printf(html,
		"<tr class=\"line-row\" data-line>"
		"<td class=\"line-item\"><input type=\"text\" name=\"line-%s-description\" "
		"placeholder=\"What you are charging for\" aria-label=\"Description\">"
		"<select name=\"line-%s-product-id\" class=\"line-product\" aria-label=\"Product\" data-no-picker>%s</select></td>"
		"<td class=\"num\"><input type=\"number\" name=\"line-%s-quantity\" value=\"1\" min=\"0\" step=\"any\" "
		"inputmode=\"decimal\" aria-label=\"Quantity\" data-qty></td>"
		"<td class=\"num\"><input type=\"text\" name=\"line-%s-unit-price\" placeholder=\"0.00\" "
		"inputmode=\"decimal\" aria-label=\"Unit price\" data-unit></td>"
		"<td class=\"num\"><input type=\"number\" name=\"line-%s-discount-percent\" placeholder=\"0\" min=\"0\" max=\"100\" "
		"step=\"any\" aria-label=\"Discount percent\" data-discount></td>",
		index, index, products, index, index, index);
	/* An invoice line is taxed by a rate record; a quote line still
	 * carries a whole percent, which is all its table can hold. */
	if (taxes != NULL)
		g_string_append_printf(html, "<td class=\"line-tax\"><select name=\"line-%s-tax-code-id\" "
			"aria-label=\"Tax\" data-no-picker data-tax>%s</select></td>", index, taxes);
	else
		g_string_append_printf(html, "<td class=\"num\"><input type=\"number\" name=\"line-%s-tax-percent\" "
			"placeholder=\"0\" min=\"0\" step=\"any\" aria-label=\"Tax percent\" data-tax></td>", index);
	g_string_append(html, "<td class=\"num line-amount\" data-amount>0.00</td>"
		VENTURE_WEB_LINE_DELETE "</tr>");
}

/*
 * A document laid out as the document: who it is for at the top, the
 * lines as a table you add rows to, the total beside them as you type,
 * and the two ways to finish in the top right where every page keeps its
 * actions. Terms and notes fold, because most invoices use neither.
 */
static gchar *
document_compose_page(VentureWebServer *self, HtmxRequest *request, gboolean quote)
{
	GString *html = g_string_new(NULL);
	gboolean repeat = !quote && htmx_request_get_query_param(request, "repeat") != NULL &&
		venture_web_module_enabled(self, "recurring");
	gint64 organization = venture_web_active_organization(self, request);
	g_autofree gchar *products = document_product_options(self, organization);
	g_autofree gchar *taxes = quote ? NULL : document_tax_options(self, organization);
	const gchar *noun = quote ? "quote" : "invoice";

	g_string_append(html, "<form method=\"post\" class=\"doc-compose\" data-doc-compose>"
		"<input type=\"hidden\" name=\"compose-form\" value=\"1\">"
		"<div class=\"page-head\"><div class=\"page-title\">");
	venture_web_append_crumbs(html, quote ? VENTURE_TYPE_QUOTE : VENTURE_TYPE_INVOICE, noun, TRUE);
	if (repeat)
		g_string_append(html, "<h1>New repeating invoice</h1></div><div class=\"page-actions\">"
			"<input type=\"hidden\" name=\"repeat\" value=\"1\">"
			"<a class=\"btn btn-ghost\" href=\"/e/recurring_schedule\">Cancel</a>"
			"<button class=\"btn btn-primary\" type=\"submit\">Save schedule</button></div></div>");
	else
		g_string_append_printf(html, "<h1>New %s</h1></div><div class=\"page-actions\">"
			"<a class=\"btn btn-ghost\" href=\"/e/%s\">Cancel</a>"
			"<button class=\"btn\" type=\"submit\">Save draft</button>"
			"<button class=\"btn btn-primary\" type=\"submit\" name=\"send\" value=\"1\">Save and send</button>"
			"</div></div>", noun, noun);

	/*
	 * The question people get wrong before they start: is this one
	 * invoice, the same invoice every month, or a customer on a plan?
	 * Three answers, each saying what it means, each one click away.
	 */
	if (!quote)
	{
		g_string_append_printf(html, "<nav class=\"how-often\" aria-label=\"How often\">"
			"<span class=\"how-often-label\">How often?</span>"
			"<a class=\"how-often-choice%s\" href=\"/invoices/compose\"%s>"
			"<strong>Just once</strong><span>One invoice, sent now.</span></a>",
			repeat ? "" : " active", repeat ? "" : " aria-current=\"page\"");
		if (venture_web_module_enabled(self, "recurring"))
			g_string_append_printf(html, "<a class=\"how-often-choice%s\" href=\"/invoices/compose?repeat=1\"%s>",
				repeat ? " active" : "", repeat ? " aria-current=\"page\"" : "");
		if (venture_web_module_enabled(self, "recurring"))
			g_string_append(html, ""
				"<strong>Same invoice, on a schedule</strong><span>Rent, a retainer, hosting. "
				"Edits change the invoices still to come.</span></a>");
		if (venture_web_module_enabled(self, "billing"))
			g_string_append(html, "<a class=\"how-often-choice\" href=\"/billing/subscriptions/new\">"
				"<strong>A customer on a plan</strong><span>A monthly or yearly price, seats and trials. "
				"Mid-period changes are prorated.</span></a>");
		g_string_append(html, "</nav>");
	}

	g_string_append(html, "<div class=\"doc-layout\"><section class=\"card doc-sheet\"><div class=\"card-body\">"
		"<div class=\"doc-parties\">");
	document_append_parties(self, html, organization);
	if (!repeat)
		g_string_append_printf(html, "<label class=\"doc-field\"><span class=\"field-label\">Number</span>"
			"<input type=\"text\" name=\"number\" placeholder=\"%s\"></label>",
			quote ? "Q-2026-001" : "INV-2026-001");
	g_string_append(html, "</div>");

	g_string_append_printf(html, "<table class=\"line-items\"><thead><tr>"
		"<th>Item</th><th class=\"num\">Qty</th><th class=\"num\">Unit price</th>"
		"<th class=\"num\">Discount %%</th><th%s>Tax%s</th><th class=\"num\">Amount</th><th></th>"
		"</tr></thead><tbody data-lines>", quote ? " class=\"num\"" : "", quote ? " %" : "");
	document_append_line_row(html, "0", products, taxes);
	g_string_append(html, "</tbody></table>"
		"<button type=\"button\" class=\"btn btn-ghost add-line\" data-add-line>+ Add a line</button>"
		"<template data-line-template>");
	document_append_line_row(html, "__N__", products, taxes);
	g_string_append(html, "</template></div></section>");

	g_string_append_printf(html, "<aside class=\"doc-aside\"><section class=\"card doc-totals\"><div class=\"card-body\">"
		"<dl class=\"totals\"><div><dt>Subtotal</dt><dd data-subtotal>0.00</dd></div>"
		"<div><dt>Discount</dt><dd data-discount-total>0.00</dd></div>"
		"<div><dt>Tax</dt><dd data-tax-total>0.00</dd></div>"
		"<div class=\"grand\"><dt>Total</dt><dd data-total>0.00</dd></div></dl>"
		"<p class=\"muted small\">Worked out as you type; the saved %s is the final word.</p>"
		"</div></section>", noun);
	if (repeat)
	{
		g_autoptr(GDateTime) today = venture_time_from_string("today", NULL);
		g_autofree gchar *start = venture_time_to_date_string(today, NULL);
		g_string_append_printf(html, "<section class=\"card\"><div class=\"card-head\"><h2>Schedule</h2></div><div class=\"card-body\">"
			"<label class=\"doc-field\"><span class=\"field-label\">Every</span><select name=\"repeat-frequency\">"
			"<option value=\"monthly\" selected>Month</option><option value=\"weekly\">Week</option>"
			"<option value=\"yearly\">Year</option></select></label>"
			"<label class=\"doc-field\"><span class=\"field-label\">First invoice on</span>"
			"<input type=\"date\" name=\"repeat-start\" value=\"%s\" required></label>"
			"<label class=\"doc-field\"><span class=\"field-label\">Last invoice on</span>"
			"<input type=\"date\" name=\"repeat-end\"><span class=\"field-help\">Leave empty to keep going.</span></label>"
			"<label class=\"checkbox\"><input type=\"checkbox\" name=\"repeat-auto\" value=\"true\"> "
			"Send each one automatically</label><span class=\"field-help\">Otherwise each arrives as a draft for you to check. "
			"Editing the schedule changes the invoices still to come, never the ones already sent.</span>"
			"</div></section>", start);
	}
	/*
	 * Exemption. Ticked for a customer whose record says they are exempt
	 * the moment they are chosen; ticking it by hand asks for the proof a
	 * non-profit or reseller gives, and by default keeps it on the
	 * customer so the next invoice fills it in.
	 */
	if (!quote)
		g_string_append(html, "<section class=\"card doc-exempt\" data-exempt-card><div class=\"card-body\">"
			"<label class=\"checkbox\"><input type=\"checkbox\" name=\"tax-exempt\" value=\"true\" data-exempt> "
			"This customer doesn't pay tax</label>"
			"<div class=\"exempt-detail\" data-exempt-detail hidden>"
			"<label class=\"doc-field\"><span class=\"field-label\">Why</span><select name=\"exempt-kind\" data-exempt-kind>"
			"<option>Non-profit (501(c)(3))</option><option>Resale</option><option>Government</option>"
			"<option>Religious or educational</option><option>Other exemption</option></select></label>"
			"<label class=\"doc-field\"><span class=\"field-label\">Certificate number</span>"
			"<input type=\"text\" name=\"exempt-number\" placeholder=\"From their exemption certificate\" data-exempt-number>"
			"<span class=\"field-help\">Printed on the invoice and kept for the tax return.</span></label>"
			"<label class=\"checkbox\"><input type=\"checkbox\" name=\"exempt-remember\" value=\"1\" checked> "
			"Remember for this customer</label></div></div></section>");
	g_string_append(html, "<details class=\"card form-section\"><summary>Terms and notes</summary><div class=\"card-body\">"
		"<label class=\"doc-field\"><span class=\"field-label\">Payment terms</span>"
		"<textarea name=\"terms\" rows=\"2\" placeholder=\"For example: 30 days\"></textarea>"
		"<span class=\"field-help\">Printed on the document.</span></label>"
		"<label class=\"doc-field\"><span class=\"field-label\">Private notes</span>"
		"<textarea name=\"notes\" rows=\"3\"></textarea>"
		"<span class=\"field-help\">For you only; never printed.</span></label>"
		"</div></details></aside></div></form>");

	/*
	 * Rows and totals. Adding a row clones the template under the next
	 * unused number; a product fills an empty description and price. The
	 * arithmetic is a preview only -- the server prices the document.
	 */
	g_string_append(html, "<script>(function(){"
		"var f=document.querySelector('[data-doc-compose]');if(!f)return;"
		"var body=f.querySelector('[data-lines]'),tpl=f.querySelector('[data-line-template]'),n=1;"
		"var ex=f.querySelector('[data-exempt]'),detail=f.querySelector('[data-exempt-detail]');"
		"function num(v){v=parseFloat(String(v||'').replace(/[^0-9.\\-]/g,''));return isNaN(v)?0:v;}"
		"function rate(r){var t=r.querySelector('[data-tax]');if(!t)return 0;"
		"if(t.tagName==='SELECT'){var o=t.options[t.selectedIndex];return o?num(o.dataset.rate):0;}return num(t.value);}"
		"function recalc(){var sub=0,dis=0,tax=0,free=ex&&ex.checked;"
		"body.querySelectorAll('[data-line]').forEach(function(r){"
		"var q=num(r.querySelector('[data-qty]').value),p=num(r.querySelector('[data-unit]').value),"
		"d=num(r.querySelector('[data-discount]').value),t=free?0:rate(r);"
		"var g=q*p,dd=g*d/100,net=g-dd,tt=net*t/100;sub+=g;dis+=dd;tax+=tt;"
		"r.querySelector('[data-amount]').textContent=(net+tt).toFixed(2);"
		"var s=r.querySelector('select[data-tax]');if(s)s.disabled=!!free;});"
		"f.querySelector('[data-subtotal]').textContent=sub.toFixed(2);"
		"f.querySelector('[data-discount-total]').textContent=(dis?'-':'')+dis.toFixed(2);"
		"f.querySelector('[data-tax-total]').textContent=free?'Exempt':tax.toFixed(2);"
		"f.querySelector('[data-total]').textContent=(sub-dis+tax).toFixed(2);}"
		"function showExempt(){if(detail)detail.hidden=!(ex&&ex.checked);recalc();}"
		"function customer(c){var k=f.querySelector('[data-doc-contact]'),mine=[];"
		"Array.prototype.forEach.call(k.options,function(o){if(!o.value)return;o.hidden=!c||o.dataset.company!==c;o.disabled=o.hidden;if(!o.hidden)mine.push(o);});"
		"if(!k.selectedOptions[0]||k.selectedOptions[0].hidden)k.value=mine.length===1?mine[0].value:'';"
		"k.disabled=!c;k.dispatchEvent(new Event('change',{bubbles:true}));"
		"if(!ex)return;var o=f.querySelector('[data-doc-company]').selectedOptions[0];"
		"var on=!!(o&&o.dataset.exempt);ex.checked=on;"
		"if(on){var kind=f.querySelector('[data-exempt-kind]'),v=o.dataset.exemptKind||'';"
		"if(v&&!Array.prototype.some.call(kind.options,function(x){return x.value===v;}))kind.add(new Option(v,v));"
		"if(v)kind.value=v;f.querySelector('[data-exempt-number]').value=o.dataset.exemptNumber||'';"
		"f.querySelector('[name=exempt-remember]').checked=false;}"
		"else f.querySelector('[name=exempt-remember]').checked=true;"
		"showExempt();}"
		"f.addEventListener('input',recalc);"
		"f.addEventListener('change',function(e){var s=e.target;"
		"if(s.classList.contains('line-product')){var o=s.options[s.selectedIndex],r=s.closest('[data-line]');"
		"var d=r.querySelector('input[name$=\"-description\"]'),p=r.querySelector('[data-unit]');"
		"if(o&&o.value){if(!d.value)d.value=o.textContent;if(!p.value&&o.dataset.price)p.value=o.dataset.price;}recalc();}"
		"if(s.matches('[data-doc-company]'))customer(s.value);"
		"if(s===ex)showExempt();"
		"if(s.matches('[data-tax]'))recalc();});"
		"f.querySelector('[data-add-line]').addEventListener('click',function(){"
		"var row=tpl.innerHTML.replace(/__N__/g,String(n++));body.insertAdjacentHTML('beforeend',row);"
		"var last=body.lastElementChild,prev=last.previousElementSibling;"
		"if(prev){var pt=prev.querySelector('select[data-tax]'),nt=last.querySelector('select[data-tax]');if(pt&&nt)nt.value=pt.value;}"
		"last.querySelector('input').focus();recalc();});"
		"body.addEventListener('click',function(e){var b=e.target.closest('[data-remove-line]');"
		"if(!b)return;var rows=body.querySelectorAll('[data-line]'),r=b.closest('[data-line]');"
		"if(rows.length>1)r.remove();else r.querySelectorAll('input').forEach(function(i){i.value=i.name.indexOf('quantity')>0?'1':'';});recalc();});"
		"customer(f.querySelector('[data-doc-company]').value);"
		"})();</script>");

	return g_string_free(html, FALSE);
}

/*
 * The same invoice on a schedule: the sheet's customer and lines become
 * the template a repeating schedule issues from, every week, month or
 * year from the first date. Built from the form's own fields rather than
 * from a serialised invoice, so no identity of a draft leaks into every
 * invoice the schedule will ever make.
 */
static HtmxResponse *
document_save_schedule(VentureWebServer *self, HtmxRequest *request, const VentureActor *actor)
{
	static const gchar *const header[] = { "company-id", "contact-id", "terms", "notes", NULL };
	static const gchar *const line_fields[] = { "description", "quantity", "unit-price", "product-id",
		"discount-percent", "tax-code-id", NULL };
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(JsonNode) root = NULL;
	g_autoptr(VentureEntity) schedule = NULL;
	g_autoptr(VentureEntity) company = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *template_text = NULL, *timezone = NULL, *name = NULL, *company_name = NULL;
	const gchar *company_id = htmx_request_get_form_value(request, "company-id");
	const gchar *end = htmx_request_get_form_value(request, "repeat-end");
	guint i, lines = 0;
	gint64 organization = venture_web_active_organization(self, request);

	if (!venture_web_module_enabled(self, "recurring"))
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Repeating invoices are switched off");
		return venture_web_error_response(error);
	}
	if (organization == 0)
		organization = venture_context_get_default_organization_id(self->context);
	json_builder_begin_object(builder);
	for (i = 0; header[i] != NULL; i++)
	{
		const gchar *value = htmx_request_get_form_value(request, header[i]);
		g_autofree gchar *wire = g_strdup(header[i]);
		if (value == NULL || *value == '\0')
			continue;
		g_strdelimit(wire, "-", '_');
		json_builder_set_member_name(builder, wire);
		if (g_str_has_suffix(header[i], "-id"))
			json_builder_add_int_value(builder, g_ascii_strtoll(value, NULL, 10));
		else
			json_builder_add_string_value(builder, value);
	}
	if (htmx_request_get_form_value(request, "tax-exempt") != NULL)
	{
		g_autofree gchar *reason = document_exemption_reason(request);
		json_builder_set_member_name(builder, "tax_exempt");
		json_builder_add_boolean_value(builder, TRUE);
		json_builder_set_member_name(builder, "tax_exempt_reason");
		json_builder_add_string_value(builder, reason);
	}
	json_builder_set_member_name(builder, "lines");
	json_builder_begin_array(builder);
	for (i = 0; i < VENTURE_DOCUMENT_MAX_LINES; i++)
	{
		g_autofree gchar *description_name = g_strdup_printf("line-%u-description", i);
		const gchar *description = htmx_request_get_form_value(request, description_name);
		guint f;
		if (description == NULL || *description == '\0')
			continue;
		json_builder_begin_object(builder);
		for (f = 0; line_fields[f] != NULL; f++)
		{
			g_autofree gchar *field = g_strdup_printf("line-%u-%s", i, line_fields[f]);
			g_autofree gchar *wire = g_strdup(line_fields[f]);
			const gchar *value = htmx_request_get_form_value(request, field);
			if (value == NULL || *value == '\0')
				continue;
			g_strdelimit(wire, "-", '_');
			json_builder_set_member_name(builder, wire);
			if (g_str_has_suffix(line_fields[f], "-id"))
				json_builder_add_int_value(builder, g_ascii_strtoll(value, NULL, 10));
			else
				json_builder_add_string_value(builder, value);
		}
		json_builder_end_object(builder);
		lines++;
	}
	json_builder_end_array(builder);
	json_builder_end_object(builder);
	if (company_id == NULL || *company_id == '\0' || lines == 0)
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"A repeating invoice needs a customer and at least one line");
		return venture_web_error_response(error);
	}
	root = json_builder_get_root(builder);
	template_text = venture_json_to_string(root, FALSE);
	company = venture_database_get(venture_context_get_database(self->context), VENTURE_TYPE_COMPANY,
		g_ascii_strtoll(company_id, NULL, 10), NULL);
	company_name = company != NULL ? venture_entity_get_display_name(company) : g_strdup("Customer");
	name = g_strdup_printf("%s \xe2\x80\x94 repeating invoice", company_name);
	g_object_get(venture_context_get_config(self->context), "locale-timezone", &timezone, NULL);

	schedule = g_object_new(VENTURE_TYPE_RECURRING_SCHEDULE, NULL);
	venture_entity_set_organization_id(schedule, organization);
	if (!venture_entity_set_field_from_string(schedule, "name", name, &error) ||
		!venture_entity_set_field_from_string(schedule, "kind", "invoice", &error) ||
		!venture_entity_set_field_from_string(schedule, "frequency",
			htmx_request_get_form_value(request, "repeat-frequency"), &error) ||
		!venture_entity_set_field_from_string(schedule, "timezone", timezone, &error) ||
		!venture_entity_set_field_from_string(schedule, "start-at",
			htmx_request_get_form_value(request, "repeat-start"), &error) ||
		(end != NULL && *end != '\0' && !venture_entity_set_field_from_string(schedule, "end-at", end, &error)) ||
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
 * "Remember for this customer": the exemption typed on the invoice goes
 * onto the customer, whose record is what every later invoice freezes its
 * tax from. Saved through the ordinary save, so it is audited like an
 * edit of the customer made by hand.
 */
static gboolean
document_remember_exemption(VentureWebServer *self, HtmxRequest *request, const VentureActor *actor,
	GError **error)
{
	const gchar *company_id = htmx_request_get_form_value(request, "company-id");
	const gchar *kind = htmx_request_get_form_value(request, "exempt-kind");
	const gchar *number = htmx_request_get_form_value(request, "exempt-number");
	g_autoptr(VentureEntity) company = NULL;

	if (htmx_request_get_form_value(request, "tax-exempt") == NULL ||
		htmx_request_get_form_value(request, "exempt-remember") == NULL ||
		company_id == NULL || *company_id == '\0')
		return TRUE;
	company = venture_database_get(venture_context_get_database(self->context), VENTURE_TYPE_COMPANY,
		g_ascii_strtoll(company_id, NULL, 10), error);
	if (company == NULL)
		return FALSE;
	g_object_set(company, "tax-exempt", TRUE, "tax-exempt-reason", kind != NULL ? kind : "",
		"tax-exemption-number", number != NULL ? number : "", NULL);
	return venture_database_save(venture_context_get_database(self->context), company, actor, error);
}

static HtmxResponse *
venture_document_web_compose(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(JsonObject) spec = NULL;
	g_autoptr(VentureEntity) result = NULL;
	g_autoptr(GError) error = NULL;
	VentureActor actor;
	HtmxResponse *gate;
	gboolean quote = strstr(htmx_request_get_path(request), "quote") != NULL;
	gboolean api = g_str_has_prefix(htmx_request_get_path(request), "/api/");
	(void)params;
	if (htmx_request_get_method(request) == HTMX_METHOD_GET)
	{
		gate = venture_web_ui_require_session(self, request);
		if (gate != NULL)
			return gate;
		gate = venture_web_require_module_ui(self, request, quote ? "quotes" : "invoicing");
		if (gate != NULL)
			return gate;
		return venture_web_html_response(venture_web_page(self, request,
			quote ? "/quotes/compose" : "/invoices/compose",
			quote ? "New quote" : "New invoice",
			document_compose_page(self, request, quote)), 200);
	}
	gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_EDITOR);
	if (gate != NULL)
		return gate;
	gate = venture_web_require_module_api(self, quote ? "quotes" : "invoicing");
	if (gate != NULL)
		return gate;
	principal = venture_auth_authenticate(self->auth, request);
	venture_auth_to_actor(principal, &actor);
	if (!quote && !api && !document_remember_exemption(self, request, &actor, &error))
		return venture_web_error_response(error);
	if (!quote && !api && htmx_request_get_form_value(request, "repeat") != NULL)
		return document_save_schedule(self, request, &actor);
	spec = document_spec_from_request(request, &error);
	if (spec == NULL)
		return venture_web_error_response(error);
	if (quote)
		result = venture_document_service_compose_quote(venture_document_service_get(venture_context_get_database(self->context)),
			venture_web_active_organization(self, request), spec, &actor, &error);
	else
		result = venture_document_service_compose_invoice(venture_document_service_get(venture_context_get_database(self->context)),
			venture_web_active_organization(self, request), spec, &actor, &error);
	if (result == NULL)
		return venture_web_error_response(error);
	if (!api)
	{
		g_autofree gchar *path = g_strdup_printf("/e/%s/%" G_GINT64_FORMAT, quote ? "quote" : "invoice",
			venture_entity_get_id(result));
		return venture_web_redirect_to(path);
	}
	return venture_web_json_response(venture_serializable_to_json(VENTURE_SERIALIZABLE(result), FALSE), 200);
}

static void
venture_document_web_register(HtmxRouter *router, VentureWebServer *self)
{
	(void)router;
	venture_web_server_add_classified_route(self, HTMX_METHOD_GET, "/invoices/compose", VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, venture_document_web_compose, self);
	venture_web_server_add_classified_route(self, HTMX_METHOD_POST, "/invoices/compose", VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, venture_document_web_compose, self);
	venture_web_server_add_classified_route(self, HTMX_METHOD_POST, "/api/v1/invoices/compose", VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, venture_document_web_compose, self);
	venture_web_server_add_classified_route(self, HTMX_METHOD_GET, "/quotes/compose", VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, venture_document_web_compose, self);
	venture_web_server_add_classified_route(self, HTMX_METHOD_POST, "/quotes/compose", VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, venture_document_web_compose, self);
	venture_web_server_add_classified_route(self, HTMX_METHOD_POST, "/api/v1/quotes/compose", VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, venture_document_web_compose, self);
}
