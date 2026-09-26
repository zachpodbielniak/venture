/*
 * venture-financial-documents-web.inc - Invoice and receipt PDFs on the web
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Included by venture-web-server.c. The same documents the customer is
 * mailed, one click from the invoice and the payment: the PDF opens in the
 * browser, which is also where it is printed or saved.
 */

/* A PDF answered inline, with the file name a save uses. */
static HtmxResponse *
venture_web_pdf_response(GBytes *pdf, const gchar *filename)
{
	HtmxResponse *response = htmx_response_new();
	g_autofree gchar *disposition = NULL;

	htmx_response_set_bytes(response, pdf);
	htmx_response_set_content_type(response, "application/pdf");
	htmx_response_set_status(response, 200);
	disposition = g_strdup_printf("inline; filename=\"%s\"", filename);
	htmx_response_add_header(response, "Content-Disposition", disposition);

	return response;
}

/*
 * GET /invoices/:id/pdf and GET /payments/:id/receipt. Readable by whoever
 * may read the record, like the record's own page.
 */
static HtmxResponse *
venture_web_financial_document(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) record = NULL;
	g_autoptr(GBytes) pdf = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *filename = NULL;
	gboolean receipt = g_str_has_prefix(htmx_request_get_path(request), "/payments/");
	GType type = receipt ? VENTURE_TYPE_PAYMENT : VENTURE_TYPE_INVOICE;
	HtmxResponse *gate;

	gate = venture_web_ui_require_session(self, request);
	if (gate != NULL)
		return gate;
	gate = venture_web_require_module_ui(self, request, receipt ? "receivables" : "invoicing");
	if (gate != NULL)
		return gate;
	principal = venture_auth_authenticate(self->auth, request);
	if (!venture_web_require_for_type(self, principal, type, VENTURE_USER_ROLE_VIEWER, &error))
		return venture_web_error_response(error);

	record = venture_database_get(venture_context_get_database(self->context), type,
		g_ascii_strtoll(g_hash_table_lookup(params, "id"), NULL, 10), &error);
	if (record == NULL)
		return venture_web_error_response(error);

	pdf = receipt ? venture_financial_documents_receipt_pdf(self->context, record, &error)
	              : venture_financial_documents_invoice_pdf(self->context, record, &error);
	if (pdf == NULL)
		return venture_web_error_response(error);

	filename = venture_financial_documents_filename(record);

	return venture_web_pdf_response(pdf, filename);
}

/*
 * On a payment's page: its receipt, and whether the customer was sent it
 * -- the question anybody asks the day a customer says they never got one.
 */
static void
venture_web_append_payment_receipt(VentureWebServer *self, GString *html, VentureEntity *record)
{
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureEntity) mail = NULL;
	gint64 id;

	if (!VENTURE_IS_PAYMENT(record) || !venture_web_module_enabled(self, "receivables"))
		return;
	id = venture_entity_get_id(record);

	g_string_append_printf(html, "<section class=\"card next-up\"><div class=\"card-head\"><h2>Receipt</h2></div>"
		"<div class=\"card-body\"><p class=\"next-up-lead\">");

	if (venture_web_module_enabled(self, "mail"))
	{
		query = venture_query_new(VENTURE_TYPE_MAIL_MESSAGE);
		venture_query_add_filter_string(query, "related-type", VENTURE_FILTER_OP_EQ, "payment", NULL);
		venture_query_add_filter_int(query, "related-id", VENTURE_FILTER_OP_EQ, id, NULL);
		mail = venture_database_find_one(venture_context_get_database(self->context), query, NULL);
	}

	if (mail != NULL)
	{
		g_autofree gchar *to = NULL, *state = NULL, *state_label = NULL;

		g_object_get(mail, "to", &to, "state", &state, NULL);
		state_label = venture_web_label_from_name(state != NULL ? state : "queued");
		g_string_append(html, "Sent to <strong>");
		venture_html_escape_append(html, to);
		g_string_append(html, "</strong> <span class=\"badge info\">");
		venture_html_escape_append(html, state_label);
		g_string_append_printf(html, "</span></p><p><a href=\"/e/mail_message/%" G_GINT64_FORMAT "\">See the email</a></p>",
			venture_entity_get_id(mail));
	}
	else
		g_string_append(html, "Not emailed \xe2\x80\x94 the customer has no email address, or receipts "
			"are switched off.</p>");

	g_string_append_printf(html, "<p><a class=\"btn\" href=\"/payments/%" G_GINT64_FORMAT "/receipt\" "
		"target=\"_blank\">Download receipt</a></p></div></section>", id);
}
