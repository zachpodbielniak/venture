/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <string.h>
static gchar *invoice_html(VentureDatabase *db, VentureEntity *invoice, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_INVOICE_LINE);
	g_autoptr(GPtrArray) lines = NULL;
	g_autoptr(VentureMoney) total = NULL;
	g_autoptr(GString) html = g_string_new("<!doctype html><html><body><h1>Invoice ");
	g_autofree gchar *number = NULL, *terms = NULL;
	guint i;
	g_object_get(invoice, "number", &number, "terms", &terms, NULL);
	venture_query_set_organization(query, venture_entity_get_organization_id(invoice));
	venture_query_add_filter_int(query, "invoice-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(invoice), NULL);
	venture_query_add_order(query, "position", VENTURE_SORT_ASCENDING, NULL);
	lines = venture_database_find(db, query, error);
	if (!lines) return NULL;
	venture_html_escape_append(html, number);
	g_string_append(html, "</h1><table><thead><tr><th>Description</th><th>Amount</th></tr></thead><tbody>");
	for (i = 0; i < lines->len; i++) {
		VentureInvoiceLine *line = g_ptr_array_index(lines, i);
		g_autoptr(VentureMoney) amount = venture_invoice_line_get_amount(line, error);
		g_autofree gchar *description = NULL, *formatted = NULL;
		if (!amount) return NULL;
		if (!total) total = venture_money_copy(amount);
		else {
			VentureMoney *sum = venture_money_add(total, amount, error);
			if (!sum) return NULL;
			venture_money_free(g_steal_pointer(&total)); total = sum;
		}
		g_object_get(line, "description", &description, NULL);
		formatted = venture_money_to_display_string(amount, TRUE);
		g_string_append(html, "<tr><td>"); venture_html_escape_append(html, description);
		g_string_append(html, "</td><td>"); venture_html_escape_append(html, formatted);
		g_string_append(html, "</td></tr>");
	}
	g_string_append(html, "</tbody><tfoot><tr><th>Total</th><td>");
	if (total) {
		g_autofree gchar *formatted = venture_money_to_display_string(total, TRUE);
		venture_html_escape_append(html, formatted);
	}
	g_string_append(html, "</td></tr></tfoot></table><p>");
	venture_html_escape_append(html, terms);
	g_string_append(html, "</p></body></html>");
	return g_string_free(g_steal_pointer(&html), FALSE);
}
VentureMailMessage *venture_mail_send_invoice(VentureContext *context, gint64 org, gint64 invoice_id, const VentureActor *actor, GError **error)
{
	VentureDatabase *db = venture_context_get_database(context);
	VentureMailOutbox *outbox = venture_context_get_mail_outbox(context);
	VentureSettlementService *settlement = venture_settlement_service_get(db);
	g_autoptr(VentureEntity) invoice = NULL, company = NULL, stored_template = NULL, existing = NULL;
	g_autoptr(VentureMailTemplate) template = NULL;
	g_autoptr(VentureMailMessage) message = NULL, queued = NULL;
	g_autoptr(VentureQuery) query = NULL, keys = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	g_autofree gchar *email = NULL, *html = NULL, *key = NULL;
	gint64 company_id;
	gint status;
	if (!outbox || !venture_context_module_enabled(context, "invoicing")) {
		venture_set_error_validation(error, "mail", "Mail and invoicing modules must be enabled"); return NULL;
	}
	if (!venture_database_begin(db, error)) return NULL;
	invoice = venture_database_get(db, VENTURE_TYPE_INVOICE, invoice_id, error);
	if (!invoice || venture_entity_is_deleted(invoice)) { if (!error || !*error) g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Invoice not found"); goto fail; }
	if (org <= 0 || venture_entity_get_organization_id(invoice) != org) {
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Invoice not found"); goto fail;
	}
	key = g_strdup_printf("invoice:%s", venture_entity_get_uuid(invoice));
	keys = venture_query_new(VENTURE_TYPE_MAIL_MESSAGE);
	venture_query_set_organization(keys, org); venture_query_set_include_deleted(keys, TRUE);
	venture_query_add_filter_string(keys, "idempotency-key", VENTURE_FILTER_OP_EQ, key, NULL);
	existing = venture_database_find_one(db, keys, error);
	if (error && *error) goto fail;
	if (existing) {
		if (!venture_database_commit(db, error)) return NULL;
		return VENTURE_MAIL_MESSAGE(g_steal_pointer(&existing));
	}
	g_object_get(invoice, "company-id", &company_id, "status", &status, NULL);
	company = venture_database_get(db, VENTURE_TYPE_COMPANY, company_id, error);
	if (!company || venture_entity_is_deleted(company)) { if (!error || !*error) g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Invoice customer not found"); goto fail; }
	g_object_get(company, "email", &email, NULL);
	if (venture_entity_get_organization_id(company) != org || !email || !*email) {
		venture_set_error_validation(error, "company", "Invoice needs a customer email in the same organization"); goto fail;
	}
	if (status == VENTURE_INVOICE_STATUS_VOID) {
		venture_set_error_validation(error, "invoice", "Cannot send a void invoice"); goto fail;
	}
	if (status == VENTURE_INVOICE_STATUS_DRAFT && !venture_settlement_service_transition(settlement, VENTURE_INVOICE(invoice), "sent", now, actor, error)) goto fail;
	query = venture_query_new(VENTURE_TYPE_MAIL_TEMPLATE);
	venture_query_set_organization(query, org);
	venture_query_add_filter_string(query, "name", VENTURE_FILTER_OP_EQ, "invoice", NULL);
	stored_template = venture_database_find_one(db, query, error);
	if (error && *error) goto fail;
	if (stored_template) template = g_object_ref(VENTURE_MAIL_TEMPLATE(stored_template));
	else {
		template = venture_mail_template_new();
		g_object_set(template, "organization-id", org, "name", "invoice", "subject", "Invoice {number}",
			"text-body", "Please find invoice {number} below. {terms}", "html-body", "", NULL);
		if (!venture_database_save(db, VENTURE_ENTITY(template), actor, error)) goto fail;
	}
	message = venture_mail_template_render(template, invoice, error);
	if (!message) goto fail;
	html = invoice_html(db, invoice, error);
	if (!html) goto fail;
	{
		g_autofree gchar *intro = NULL, *body = NULL;
		g_object_get(message, "html-body", &intro, NULL);
		body = g_strconcat(intro ? intro : "", html, NULL);
		g_object_set(message, "to", email, "html-body", body, "idempotency-key", key,
			"related-type", "invoice", "related-id", invoice_id, NULL);
	}
	queued = venture_mail_outbox_enqueue(outbox, message, actor, error);
	if (!queued || !venture_settlement_service_record_mail(settlement, VENTURE_INVOICE(invoice), actor, error)) goto fail;
	if (!venture_database_commit(db, error)) return NULL;
	return g_steal_pointer(&queued);
fail:
	venture_database_rollback(db);
	return NULL;
}
