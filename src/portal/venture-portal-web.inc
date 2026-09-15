/* SPDX-License-Identifier: AGPL-3.0-or-later */
static HtmxResponse *
venture_portal_web(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	VentureWebServer *self = user_data;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) access = NULL;
	g_autoptr(GPtrArray) invoices = NULL;
	g_autoptr(GString) html = g_string_new("<!doctype html><html><head><meta charset=\"utf-8\"><title>Customer portal</title></head><body>");
	const gchar *token = g_hash_table_lookup(params, "token");
	guint i;
	access = venture_portal_service_lookup(venture_portal_service_get(venture_context_get_database(self->context)), token, &error);
	if (access == NULL)
	{
		HtmxResponse *response = htmx_response_new_with_content("Not found");
		htmx_response_set_status(response, 404);
		return response;
	}
	if (htmx_request_get_method(request) == HTMX_METHOD_POST)
	{
		const gchar *invoice = htmx_request_get_form_value(request, "invoice_id");
		const gchar *amount = htmx_request_get_form_value(request, "amount");
		g_autofree gchar *currency = NULL;
		g_autoptr(VentureEntity) organization = NULL;
		g_autoptr(VentureMoney) money = NULL;
		organization = venture_database_get(venture_context_get_database(self->context),
			VENTURE_TYPE_ORGANIZATION, venture_entity_get_organization_id(access), NULL);
		if (organization != NULL)
			g_object_get(organization, "default-currency", &currency, NULL);
		money = amount ? venture_money_from_string(amount, currency, &error) : NULL;
		VentureActor actor;
		actor.kind = VENTURE_ACTOR_KIND_USER;
		actor.name = "customer portal";
		actor.prompt = NULL;
		actor.request_id = NULL;
		actor.approved_by = NULL;
		if (money == NULL || !venture_portal_service_pay(venture_portal_service_get(venture_context_get_database(self->context)),
			VENTURE_CUSTOMER_PORTAL_ACCESS(access), invoice ? g_ascii_strtoll(invoice, NULL, 10) : 0, money, &actor, &error))
			return venture_web_error_response(error);
		return htmx_response_new_with_content("<p>Use Checkout to pay this invoice.</p>");
	}
	invoices = venture_portal_service_invoices(venture_portal_service_get(venture_context_get_database(self->context)),
		VENTURE_CUSTOMER_PORTAL_ACCESS(access), &error);
	if (invoices == NULL)
		return venture_web_error_response(error);
	g_string_append(html, "<h1>Your invoices</h1><table>");
	for (i = 0; i < invoices->len; i++)
	{
		VentureEntity *invoice = g_ptr_array_index(invoices, i);
		g_autofree gchar *number = NULL;
		gint status;
		g_object_get(invoice, "number", &number, "status", &status, NULL);
		g_string_append(html, "<tr><td>");
		venture_html_escape_append(html, number);
		g_string_append(html, "</td><td>");
		if (status == VENTURE_INVOICE_STATUS_SENT || status == VENTURE_INVOICE_STATUS_PARTIALLY_PAID)
			g_string_append_printf(html, "<form method=\"post\"><input type=\"hidden\" name=\"invoice_id\" value=\"%" G_GINT64_FORMAT "\">"
				"<input name=\"amount\" placeholder=\"10 USD\" required><button>Request Checkout</button></form>",
				venture_entity_get_id(invoice));
		g_string_append(html, "</td></tr>");
	}
	g_string_append(html, "</table></body></html>");
	return htmx_response_new_with_content(html->str);
}

static void
venture_portal_web_register(HtmxRouter *router, VentureWebServer *self)
{
	htmx_router_get(router, "/portal/:token", venture_portal_web, self);
	htmx_router_post(router, "/portal/:token", venture_portal_web, self);
}
