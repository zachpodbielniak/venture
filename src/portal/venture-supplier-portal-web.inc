/* SPDX-License-Identifier: AGPL-3.0-or-later */
static HtmxResponse *
supplier_portal_not_found(void)
{
	HtmxResponse *response = htmx_response_new_with_content("Not found");
	htmx_response_set_status(response, 404);
	return response;
}

static gboolean
supplier_portal_owns_bill(VentureSupplierPortalAccess *access, VentureEntity *bill)
{
	gint64 company, bill_company;
	g_object_get(access, "company-id", &company, NULL);
	g_object_get(bill, "company-id", &bill_company, NULL);
	return venture_entity_get_organization_id(VENTURE_ENTITY(access)) ==
		venture_entity_get_organization_id(bill) && company == bill_company;
}

static HtmxResponse *
venture_supplier_portal_print(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	VentureWebServer *self = user_data;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) access = NULL;
	g_autoptr(VentureEntity) bill = NULL;
	g_autoptr(VentureQuery) lines_query = NULL;
	g_autoptr(GPtrArray) lines = NULL;
	g_autoptr(VentureMoney) balance = NULL;
	g_autoptr(GString) html = NULL;
	g_autofree gchar *number = NULL;
	g_autofree gchar *status = NULL;
	g_autofree gchar *memo = NULL;
	const gchar *token = g_hash_table_lookup(params, "token");
	gint64 bill_id = g_ascii_strtoll(g_hash_table_lookup(params, "id"), NULL, 10);
	guint i;
	(void)request;
	access = venture_portal_service_lookup_supplier(venture_portal_service_get(venture_context_get_database(self->context)), token, &error);
	if (access == NULL)
		return supplier_portal_not_found();
	bill = venture_database_get(venture_context_get_database(self->context), VENTURE_TYPE_VENDOR_BILL, bill_id, &error);
	if (bill == NULL || !supplier_portal_owns_bill(VENTURE_SUPPLIER_PORTAL_ACCESS(access), bill))
		return supplier_portal_not_found();
	g_object_get(bill, "number", &number, "status", &status, "memo", &memo, NULL);
	lines_query = venture_query_new(VENTURE_TYPE_VENDOR_BILL_LINE);
	venture_query_set_organization(lines_query, venture_entity_get_organization_id(bill));
	venture_query_set_limit(lines_query, 0);
	venture_query_add_filter_int(lines_query, "bill-id", VENTURE_FILTER_OP_EQ, bill_id, NULL);
	lines = venture_database_find(venture_context_get_database(self->context), lines_query, NULL);
	balance = venture_payables_service_bill_balance(venture_payables_service_get(venture_context_get_database(self->context)),
		bill_id, NULL, NULL);
	html = g_string_new("<!doctype html><html><head><meta charset=\"utf-8\"><title>Bill ");
	venture_html_escape_append(html, number);
	g_string_append(html, "</title><style>" VENTURE_DOCUMENT_PRINT_STYLE "</style></head><body>");
	g_string_append(html, "<div class=\"head\"><div><h1>Bill ");
	venture_html_escape_append(html, number);
	g_string_append(html, "</h1></div><span class=\"status\">");
	venture_html_escape_append(html, status);
	g_string_append(html, "</span></div><table><thead><tr><th>Description</th><th class=\"num\">Qty</th></tr></thead><tbody>");
	if (lines != NULL)
	{
		for (i = 0; i < lines->len; i++)
		{
			VentureEntity *line = g_ptr_array_index(lines, i);
			g_autofree gchar *description = NULL;
			g_autofree gchar *quantity = NULL;
			g_object_get(line, "description", &description, "quantity", &quantity, NULL);
			g_string_append(html, "<tr><td>");
			venture_html_escape_append(html, description);
			g_string_append(html, "</td><td class=\"num\">");
			venture_html_escape_append(html, quantity);
			g_string_append(html, "</td></tr>");
		}
	}
	g_string_append(html, "</tbody></table><p>Status: ");
	venture_html_escape_append(html, status);
	g_string_append(html, "</p>");
	if (balance != NULL)
	{
		g_autofree gchar *remaining = venture_money_to_string(balance);
		g_string_append(html, "<p>Remaining: ");
		venture_html_escape_append(html, remaining);
		g_string_append(html, "</p>");
	}
	if (memo != NULL && memo[0] != '\0')
	{
		g_string_append(html, "<p class=\"terms\">");
		venture_html_escape_append(html, memo);
		g_string_append(html, "</p>");
	}
	g_string_append(html, "</body></html>");
	return htmx_response_new_with_content(html->str);
}

static HtmxResponse *
venture_supplier_portal_web(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	VentureWebServer *self = user_data;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) access = NULL;
	g_autoptr(GPtrArray) bills = NULL;
	g_autoptr(GString) html = g_string_new("<!doctype html><html><head><meta charset=\"utf-8\"><title>Supplier portal</title></head><body>");
	const gchar *token = g_hash_table_lookup(params, "token");
	guint i;
	(void)request;
	access = venture_portal_service_lookup_supplier(venture_portal_service_get(venture_context_get_database(self->context)), token, &error);
	if (access == NULL)
		return supplier_portal_not_found();
	bills = venture_portal_service_bills(venture_portal_service_get(venture_context_get_database(self->context)),
		VENTURE_SUPPLIER_PORTAL_ACCESS(access), &error);
	if (bills == NULL)
		return venture_web_error_response(error);
	g_string_append(html, "<h1>Your bills</h1><table><thead><tr><th>Number</th><th>Status</th><th>Remaining</th><th></th></tr></thead><tbody>");
	for (i = 0; i < bills->len; i++)
	{
		VentureEntity *bill = g_ptr_array_index(bills, i);
		g_autofree gchar *number = NULL;
		g_autofree gchar *status = NULL;
		g_autoptr(VentureMoney) balance = NULL;
		g_autofree gchar *remaining = NULL;
		g_object_get(bill, "number", &number, "status", &status, NULL);
		balance = venture_payables_service_bill_balance(venture_payables_service_get(venture_context_get_database(self->context)),
			venture_entity_get_id(bill), NULL, NULL);
		remaining = balance ? venture_money_to_string(balance) : g_strdup("");
		g_string_append(html, "<tr><td>");
		venture_html_escape_append(html, number);
		g_string_append(html, "</td><td>");
		venture_html_escape_append(html, status);
		g_string_append(html, "</td><td>");
		venture_html_escape_append(html, remaining);
		g_string_append_printf(html, "</td><td><a href=\"/supplier/%s/bills/%" G_GINT64_FORMAT "/print\">Print</a></td></tr>",
			token, venture_entity_get_id(bill));
	}
	g_string_append(html, "</tbody></table></body></html>");
	return htmx_response_new_with_content(html->str);
}

static HtmxResponse *
venture_supplier_portal_invite_api(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	(void)params;
	return venture_portal_invite_request(user_data, request, TRUE);
}

static void
venture_supplier_portal_web_register(HtmxRouter *router, VentureWebServer *self)
{
	(void)router;
	venture_web_server_add_classified_route(self, HTMX_METHOD_GET, "/supplier/:token/bills/:id/print", VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, venture_supplier_portal_print, self);
	venture_web_server_add_classified_route(self, HTMX_METHOD_GET, "/supplier/:token", VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, venture_supplier_portal_web, self);
	venture_web_server_add_classified_route(self, HTMX_METHOD_POST, "/api/v1/supplier_portal/invite", VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, venture_supplier_portal_invite_api, self);
}
