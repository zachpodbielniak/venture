/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Restrict the guided workflow to inputs accepted by the shared composer; field types stay metadata-derived. */
/* How many line rows one composed document may carry. */
#define VENTURE_DOCUMENT_MAX_LINES 200

static const gchar *const document_header_fields[] = { "number", "company-id", "contact-id", "terms", "notes", NULL };
static const gchar *const document_line_fields[] = { "description", "quantity", "unit-price", "product-id", "discount-percent", "tax-percent", NULL };

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
		if (!document_read_fields(request, header, document_header_fields, "", error))
		{
			json_array_unref(lines);
			return NULL;
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
			if (!document_read_fields(request, line, document_line_fields, prefix, error))
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
		g_string_append_printf(html, "<option value=\"%" G_GINT64_FORMAT "\">", venture_entity_get_id(row));
		venture_html_escape_append(html, name);
		g_string_append(html, "</option>");
	}
	g_string_append(html, "</select></label>");

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

/* One line row. The same names the composer has always read, so the API,
 * the CLI and this page post one contract. */
static void
document_append_line_row(GString *html, const gchar *index, const gchar *products)
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
		"step=\"any\" aria-label=\"Discount percent\" data-discount></td>"
		"<td class=\"num\"><input type=\"number\" name=\"line-%s-tax-percent\" placeholder=\"0\" min=\"0\" step=\"any\" "
		"aria-label=\"Tax percent\" data-tax></td>"
		"<td class=\"num line-amount\" data-amount>0.00</td>"
		"<td class=\"line-remove\"><button type=\"button\" class=\"btn btn-ghost btn-sm\" data-remove-line "
		"aria-label=\"Remove line\" title=\"Remove line\">\xc3\x97</button></td></tr>",
		index, index, products, index, index, index, index);
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
	gint64 organization = venture_web_active_organization(self, request);
	g_autofree gchar *products = document_product_options(self, organization);
	const gchar *noun = quote ? "quote" : "invoice";

	g_string_append(html, "<form method=\"post\" class=\"doc-compose\" data-doc-compose>"
		"<input type=\"hidden\" name=\"compose-form\" value=\"1\">"
		"<div class=\"page-head\"><div class=\"page-title\">");
	venture_web_append_crumbs(html, quote ? VENTURE_TYPE_QUOTE : VENTURE_TYPE_INVOICE, noun, TRUE);
	g_string_append_printf(html, "<h1>New %s</h1></div><div class=\"page-actions\">"
		"<a class=\"btn btn-ghost\" href=\"/e/%s\">Cancel</a>"
		"<button class=\"btn\" type=\"submit\">Save draft</button>"
		"<button class=\"btn btn-primary\" type=\"submit\" name=\"send\" value=\"1\">Save and send</button>"
		"</div></div>", noun, noun);

	g_string_append(html, "<div class=\"doc-layout\"><section class=\"card doc-sheet\"><div class=\"card-body\">"
		"<div class=\"doc-parties\">");
	document_append_parties(self, html, organization);
	g_string_append_printf(html, "<label class=\"doc-field\"><span class=\"field-label\">Number</span>"
		"<input type=\"text\" name=\"number\" placeholder=\"%s\"></label></div>",
		quote ? "Q-2026-001" : "INV-2026-001");

	g_string_append(html, "<table class=\"line-items\"><thead><tr>"
		"<th>Item</th><th class=\"num\">Qty</th><th class=\"num\">Unit price</th>"
		"<th class=\"num\">Discount %</th><th class=\"num\">Tax %</th><th class=\"num\">Amount</th><th></th>"
		"</tr></thead><tbody data-lines>");
	document_append_line_row(html, "0", products);
	g_string_append(html, "</tbody></table>"
		"<button type=\"button\" class=\"btn btn-ghost add-line\" data-add-line>+ Add a line</button>"
		"<template data-line-template>");
	document_append_line_row(html, "__N__", products);
	g_string_append(html, "</template></div></section>");

	g_string_append_printf(html, "<aside class=\"doc-aside\"><section class=\"card doc-totals\"><div class=\"card-body\">"
		"<dl class=\"totals\"><div><dt>Subtotal</dt><dd data-subtotal>0.00</dd></div>"
		"<div><dt>Discount</dt><dd data-discount-total>0.00</dd></div>"
		"<div><dt>Tax</dt><dd data-tax-total>0.00</dd></div>"
		"<div class=\"grand\"><dt>Total</dt><dd data-total>0.00</dd></div></dl>"
		"<p class=\"muted small\">Worked out as you type; the saved %s is the final word.</p>"
		"</div></section>", noun);
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
		"function num(v){v=parseFloat(String(v||'').replace(/[^0-9.\\-]/g,''));return isNaN(v)?0:v;}"
		"function recalc(){var sub=0,dis=0,tax=0;"
		"body.querySelectorAll('[data-line]').forEach(function(r){"
		"var q=num(r.querySelector('[data-qty]').value),p=num(r.querySelector('[data-unit]').value),"
		"d=num(r.querySelector('[data-discount]').value),t=num(r.querySelector('[data-tax]').value);"
		"var g=q*p,dd=g*d/100,net=g-dd,tt=net*t/100;sub+=g;dis+=dd;tax+=tt;"
		"r.querySelector('[data-amount]').textContent=(net+tt).toFixed(2);});"
		"f.querySelector('[data-subtotal]').textContent=sub.toFixed(2);"
		"f.querySelector('[data-discount-total]').textContent=(dis?'-':'')+dis.toFixed(2);"
		"f.querySelector('[data-tax-total]').textContent=tax.toFixed(2);"
		"f.querySelector('[data-total]').textContent=(sub-dis+tax).toFixed(2);}"
		"f.addEventListener('input',recalc);"
		"f.addEventListener('change',function(e){var s=e.target;"
		"if(s.classList.contains('line-product')){var o=s.options[s.selectedIndex],r=s.closest('[data-line]');"
		"var d=r.querySelector('input[name$=\"-description\"]'),p=r.querySelector('[data-unit]');"
		"if(o&&o.value){if(!d.value)d.value=o.textContent;if(!p.value&&o.dataset.price)p.value=o.dataset.price;}recalc();}"
		"if(s.matches('[data-doc-company]')){var c=s.value,k=f.querySelector('[data-doc-contact]');"
		"Array.prototype.forEach.call(k.options,function(o){if(o.value)o.hidden=c&&o.dataset.company!==c;});"
		"if(k.selectedOptions[0]&&k.selectedOptions[0].hidden)k.value='';}});"
		"f.querySelector('[data-add-line]').addEventListener('click',function(){"
		"var row=tpl.innerHTML.replace(/__N__/g,String(n++));body.insertAdjacentHTML('beforeend',row);"
		"body.lastElementChild.querySelector('input').focus();});"
		"body.addEventListener('click',function(e){var b=e.target.closest('[data-remove-line]');"
		"if(!b)return;if(body.querySelectorAll('[data-line]').length>1)b.closest('[data-line]').remove();recalc();});"
		"recalc();})();</script>");

	return g_string_free(html, FALSE);
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
