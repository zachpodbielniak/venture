/*
 * venture-financial-documents.c - Invoices and receipts as PDF documents
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "venture.h"

#include <string.h>

/* A4, with the same margin on every side. */
#define DOC_WIDTH	595.0
#define DOC_HEIGHT	842.0
#define DOC_MARGIN	48.0
#define DOC_RIGHT	(DOC_WIDTH - DOC_MARGIN)
#define DOC_BOTTOM	(DOC_HEIGHT - 72.0)

/* Text colours: body ink, and the grey of a label. */
#define INK	0.08
#define LABEL	0.42

static gchar *
money_text(const VentureMoney *money)
{
	return (NULL != money) ? venture_money_to_display_string(money, TRUE) : g_strdup("");
}

static gchar *
day_text(VentureContext *context, GDateTime *when)
{
	return (NULL != when) ? venture_time_to_date_string(when, venture_context_get_timezone(context)) : NULL;
}

/*
 * The seller's letterhead: the organization's name large, and under it
 * whatever of its contact details are filled in. Returns the y below it.
 */
static gdouble
draw_letterhead(
	VenturePdfWriter	*pdf,
	VentureContext		*context,
	gint64			 organization
){
	g_autoptr(VentureEntity) org = NULL;
	g_autofree gchar *name = NULL, *email = NULL, *phone = NULL, *website = NULL, *address = NULL;
	gdouble y = DOC_MARGIN + 18;

	org = venture_database_get(venture_context_get_database(context), VENTURE_TYPE_ORGANIZATION,
	                           organization, NULL);
	if (NULL != org)
	{
		g_object_get(org, "legal-name", &name, "email", &email, "phone", &phone,
		             "website", &website, "address", &address, NULL);
		if (venture_string_is_empty(name))
		{
			g_free(name);
			name = venture_entity_get_display_name(org);
		}
	}

	venture_pdf_writer_set_grey(pdf, INK);
	venture_pdf_writer_text(pdf, DOC_MARGIN, y, 16, TRUE, VENTURE_PDF_ALIGN_LEFT,
	                        (NULL != name) ? name : "");
	y += 16;
	venture_pdf_writer_set_grey(pdf, LABEL);
	if (!venture_string_is_empty(address))
		y = venture_pdf_writer_wrap(pdf, DOC_MARGIN, y, 240, 9, FALSE, address);
	if (!venture_string_is_empty(email))
	{
		venture_pdf_writer_text(pdf, DOC_MARGIN, y, 9, FALSE, VENTURE_PDF_ALIGN_LEFT, email);
		y += 12;
	}
	if (!venture_string_is_empty(phone))
	{
		venture_pdf_writer_text(pdf, DOC_MARGIN, y, 9, FALSE, VENTURE_PDF_ALIGN_LEFT, phone);
		y += 12;
	}
	if (!venture_string_is_empty(website))
	{
		venture_pdf_writer_text(pdf, DOC_MARGIN, y, 9, FALSE, VENTURE_PDF_ALIGN_LEFT, website);
		y += 12;
	}
	venture_pdf_writer_set_grey(pdf, INK);

	return y;
}

/* A label over a value, right-aligned at the page edge. */
static gdouble
draw_meta(
	VenturePdfWriter	*pdf,
	gdouble			 y,
	const gchar		*label,
	const gchar		*value
){
	if (venture_string_is_empty(value))
		return y;

	venture_pdf_writer_set_grey(pdf, LABEL);
	venture_pdf_writer_text(pdf, DOC_RIGHT - 110, y, 9, FALSE, VENTURE_PDF_ALIGN_RIGHT, label);
	venture_pdf_writer_set_grey(pdf, INK);
	venture_pdf_writer_text(pdf, DOC_RIGHT, y, 9, TRUE, VENTURE_PDF_ALIGN_RIGHT, value);

	return y + 14;
}

/* Who the document is to: name, then email and address as filled in. */
static gdouble
draw_party(
	VenturePdfWriter	*pdf,
	gdouble			 y,
	const gchar		*label,
	VentureEntity		*company
){
	g_autofree gchar *name = NULL, *email = NULL, *address = NULL;

	venture_pdf_writer_set_grey(pdf, LABEL);
	venture_pdf_writer_text(pdf, DOC_MARGIN, y, 9, TRUE, VENTURE_PDF_ALIGN_LEFT, label);
	venture_pdf_writer_set_grey(pdf, INK);
	y += 15;

	if (NULL == company)
		return y;

	name = venture_entity_get_display_name(company);
	g_object_get(company, "email", &email, "address", &address, NULL);
	venture_pdf_writer_text(pdf, DOC_MARGIN, y, 11, TRUE, VENTURE_PDF_ALIGN_LEFT, name);
	y += 14;
	venture_pdf_writer_set_grey(pdf, LABEL);
	if (!venture_string_is_empty(address))
		y = venture_pdf_writer_wrap(pdf, DOC_MARGIN, y, 240, 9, FALSE, address);
	if (!venture_string_is_empty(email))
	{
		venture_pdf_writer_text(pdf, DOC_MARGIN, y, 9, FALSE, VENTURE_PDF_ALIGN_LEFT, email);
		y += 12;
	}
	venture_pdf_writer_set_grey(pdf, INK);

	return y;
}

/* Column right edges of the line table, and the width descriptions wrap in. */
static const gdouble column_qty = DOC_MARGIN + 290;
static const gdouble column_unit = DOC_MARGIN + 370;
static const gdouble column_tax = DOC_MARGIN + 430;
static const gdouble column_amount = DOC_RIGHT - 8;
static const gdouble column_description_width = 260;

static gdouble
draw_line_header(VenturePdfWriter *pdf, gdouble y)
{
	venture_pdf_writer_fill(pdf, DOC_MARGIN, y - 13, DOC_RIGHT - DOC_MARGIN, 20, 0.93);
	venture_pdf_writer_set_grey(pdf, LABEL);
	venture_pdf_writer_text(pdf, DOC_MARGIN + 8, y, 9, TRUE, VENTURE_PDF_ALIGN_LEFT, "Description");
	venture_pdf_writer_text(pdf, column_qty, y, 9, TRUE, VENTURE_PDF_ALIGN_RIGHT, "Qty");
	venture_pdf_writer_text(pdf, column_unit, y, 9, TRUE, VENTURE_PDF_ALIGN_RIGHT, "Unit price");
	venture_pdf_writer_text(pdf, column_tax, y, 9, TRUE, VENTURE_PDF_ALIGN_RIGHT, "Tax");
	venture_pdf_writer_text(pdf, column_amount, y, 9, TRUE, VENTURE_PDF_ALIGN_RIGHT, "Amount");
	venture_pdf_writer_set_grey(pdf, INK);

	return y + 22;
}

/* One total row: label and amount at the right, the last one bold. */
static gdouble
draw_total(
	VenturePdfWriter	*pdf,
	gdouble			 y,
	const gchar		*label,
	const VentureMoney	*money,
	gboolean		 strong
){
	g_autofree gchar *text = money_text(money);

	if (strong)
	{
		venture_pdf_writer_rule(pdf, column_unit - 60, y - 13, DOC_RIGHT, y - 13, 0.8);
		y += 4;
	}
	venture_pdf_writer_set_grey(pdf, strong ? INK : LABEL);
	venture_pdf_writer_text(pdf, column_tax, y, strong ? 12 : 10, strong, VENTURE_PDF_ALIGN_RIGHT, label);
	venture_pdf_writer_set_grey(pdf, INK);
	venture_pdf_writer_text(pdf, column_amount, y, strong ? 12 : 10, strong, VENTURE_PDF_ALIGN_RIGHT, text);

	return y + (strong ? 20 : 16);
}

/* Adds @amount into *@sum; a mixed currency leaves the sum alone. */
static void
accumulate(VentureMoney **sum, const VentureMoney *amount)
{
	VentureMoney *next;

	if (NULL == amount)
		return;
	if (NULL == *sum)
	{
		*sum = venture_money_copy(amount);
		return;
	}
	next = venture_money_add(*sum, amount, NULL);
	if (NULL != next)
	{
		venture_money_free(*sum);
		*sum = next;
	}
}

GBytes *
venture_financial_documents_invoice_pdf(
	VentureContext	 *context,
	VentureEntity	 *invoice,
	GError		**error
){
	VentureDatabase *db;
	g_autoptr(VenturePdfWriter) pdf = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) lines = NULL;
	g_autoptr(VentureEntity) company = NULL;
	g_autoptr(GDateTime) issued = NULL, due = NULL;
	g_autoptr(VentureMoney) subtotal = NULL, tax_total = NULL, total = NULL, balance = NULL;
	g_autofree gchar *number = NULL, *terms = NULL, *title = NULL, *issued_text = NULL, *due_text = NULL;
	gint64 company_id = 0;
	gdouble y, top;
	guint i;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);
	g_return_val_if_fail(VENTURE_IS_INVOICE(invoice), NULL);

	db = venture_context_get_database(context);
	g_object_get(invoice, "number", &number, "terms", &terms, "issued-at", &issued,
	             "due-at", &due, "company-id", &company_id, NULL);

	query = venture_query_new(VENTURE_TYPE_INVOICE_LINE);
	venture_query_set_limit(query, 0);
	venture_query_add_filter_int(query, "invoice-id", VENTURE_FILTER_OP_EQ,
	                             venture_entity_get_id(invoice), NULL);
	venture_query_add_order(query, "position", VENTURE_SORT_ASCENDING, NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	lines = venture_database_find(db, query, error);
	if (NULL == lines)
		return NULL;

	if (company_id > 0)
		company = venture_database_get(db, VENTURE_TYPE_COMPANY, company_id, NULL);

	title = g_strdup_printf("Invoice %s", (NULL != number) ? number : "");
	pdf = venture_pdf_writer_new(DOC_WIDTH, DOC_HEIGHT);
	venture_pdf_writer_set_title(pdf, title);

	y = draw_letterhead(pdf, context, venture_entity_get_organization_id(invoice));

	/* The title and its dates, on the right, level with the letterhead. */
	top = DOC_MARGIN + 18;
	venture_pdf_writer_text(pdf, DOC_RIGHT, top, 20, TRUE, VENTURE_PDF_ALIGN_RIGHT, title);
	top += 24;
	issued_text = day_text(context, issued);
	due_text = day_text(context, due);
	top = draw_meta(pdf, top, "Issued", issued_text);
	top = draw_meta(pdf, top, "Due", due_text);

	y = MAX(y, top) + 22;
	y = draw_party(pdf, y, "BILL TO", company);
	y += 18;

	y = draw_line_header(pdf, y);
	for (i = 0; i < lines->len; i++)
	{
		VentureEntity *line = g_ptr_array_index(lines, i);
		g_autofree gchar *description = NULL, *unit_text = NULL, *tax_text = NULL, *amount_text = NULL;
		g_autoptr(VentureMoney) unit = NULL, net = NULL, tax = NULL, amount = NULL;
		gchar quantity[G_ASCII_DTOSTR_BUF_SIZE];
		gdouble qty = 0, next;

		g_object_get(line, "description", &description, "quantity", &qty, "unit-price", &unit,
		             "income-amount", &net, "tax-amount", &tax, NULL);
		amount = venture_invoice_line_get_amount(VENTURE_INVOICE_LINE(line), NULL);
		unit_text = money_text(unit);
		tax_text = (NULL != tax && 0 != venture_money_get_amount(tax)) ? money_text(tax) : g_strdup("");
		amount_text = money_text(amount);
		g_ascii_formatd(quantity, sizeof(quantity), "%g", qty);

		if (y > DOC_BOTTOM)
		{
			venture_pdf_writer_new_page(pdf);
			y = draw_line_header(pdf, DOC_MARGIN + 18);
		}

		next = venture_pdf_writer_wrap(pdf, DOC_MARGIN + 8, y, column_description_width, 10, FALSE,
		                               description);
		venture_pdf_writer_text(pdf, column_qty, y, 10, FALSE, VENTURE_PDF_ALIGN_RIGHT, quantity);
		venture_pdf_writer_text(pdf, column_unit, y, 10, FALSE, VENTURE_PDF_ALIGN_RIGHT, unit_text);
		venture_pdf_writer_text(pdf, column_tax, y, 10, FALSE, VENTURE_PDF_ALIGN_RIGHT, tax_text);
		venture_pdf_writer_text(pdf, column_amount, y, 10, FALSE, VENTURE_PDF_ALIGN_RIGHT, amount_text);
		y = next + 4;
		venture_pdf_writer_set_grey(pdf, 0.85);
		venture_pdf_writer_rule(pdf, DOC_MARGIN, y - 10, DOC_RIGHT, y - 10, 0.5);
		venture_pdf_writer_set_grey(pdf, INK);

		/* A frozen line's net and tax, else the whole as net. */
		if (NULL != net)
		{
			accumulate(&subtotal, net);
			accumulate(&tax_total, tax);
		}
		else
			accumulate(&subtotal, amount);
		accumulate(&total, amount);
	}

	if (y > DOC_BOTTOM - 80)
	{
		venture_pdf_writer_new_page(pdf);
		y = DOC_MARGIN + 18;
	}

	y += 10;
	if (NULL != tax_total && 0 != venture_money_get_amount(tax_total))
	{
		y = draw_total(pdf, y, "Subtotal", subtotal, FALSE);
		y = draw_total(pdf, y, "Tax", tax_total, FALSE);
	}
	y = draw_total(pdf, y, "Total", total, TRUE);

	balance = venture_settlement_service_invoice_balance(venture_settlement_service_get(db),
		venture_entity_get_id(invoice), NULL, NULL);
	if (NULL != balance && NULL != total && 0 != venture_money_get_amount(total))
	{
		if (0 == venture_money_get_amount(balance))
		{
			/* Settled: say so, rather than print a balance of nothing. */
			venture_pdf_writer_set_grey(pdf, INK);
			venture_pdf_writer_text(pdf, column_amount, y + 4, 12, TRUE,
			                        VENTURE_PDF_ALIGN_RIGHT, "Paid in full");
			y += 24;
		}
		else if (venture_money_get_amount(balance) != venture_money_get_amount(total))
			y = draw_total(pdf, y, "Balance due", balance, TRUE);
	}

	if (!venture_string_is_empty(terms))
	{
		y += 16;
		venture_pdf_writer_set_grey(pdf, LABEL);
		venture_pdf_writer_text(pdf, DOC_MARGIN, y, 9, TRUE, VENTURE_PDF_ALIGN_LEFT, "TERMS");
		venture_pdf_writer_set_grey(pdf, INK);
		y = venture_pdf_writer_wrap(pdf, DOC_MARGIN, y + 14, DOC_RIGHT - DOC_MARGIN, 10, FALSE, terms);
	}

	venture_pdf_writer_set_grey(pdf, LABEL);
	venture_pdf_writer_text(pdf, DOC_MARGIN, DOC_HEIGHT - 36, 9, FALSE, VENTURE_PDF_ALIGN_LEFT,
	                        "Thank you for your business.");

	return venture_pdf_writer_finish(pdf);
}

GBytes *
venture_financial_documents_receipt_pdf(
	VentureContext	 *context,
	VentureEntity	 *payment,
	GError		**error
){
	VentureDatabase *db;
	g_autoptr(VenturePdfWriter) pdf = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) allocations = NULL;
	g_autoptr(VentureEntity) company = NULL;
	g_autoptr(GDateTime) date = NULL;
	g_autoptr(VentureMoney) amount = NULL, applied = NULL;
	g_autofree gchar *method = NULL, *reference = NULL, *amount_text = NULL, *date_text = NULL, *number = NULL;
	gint64 customer_id = 0;
	gdouble y, top;
	guint i;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);
	g_return_val_if_fail(VENTURE_IS_PAYMENT(payment), NULL);

	db = venture_context_get_database(context);
	g_object_get(payment, "customer-id", &customer_id, "date", &date, "amount", &amount,
	             "method", &method, "reference", &reference, NULL);
	if (customer_id > 0)
		company = venture_database_get(db, VENTURE_TYPE_COMPANY, customer_id, NULL);

	query = venture_query_new(VENTURE_TYPE_PAYMENT_ALLOCATION);
	venture_query_set_limit(query, 0);
	venture_query_add_filter_int(query, "payment-id", VENTURE_FILTER_OP_EQ,
	                             venture_entity_get_id(payment), NULL);
	allocations = venture_database_find(db, query, error);
	if (NULL == allocations)
		return NULL;

	number = g_strdup_printf("%" G_GINT64_FORMAT, venture_entity_get_id(payment));
	pdf = venture_pdf_writer_new(DOC_WIDTH, DOC_HEIGHT);
	{
		g_autofree gchar *title = g_strdup_printf("Receipt %s", number);
		venture_pdf_writer_set_title(pdf, title);
	}

	y = draw_letterhead(pdf, context, venture_entity_get_organization_id(payment));

	top = DOC_MARGIN + 18;
	venture_pdf_writer_text(pdf, DOC_RIGHT, top, 20, TRUE, VENTURE_PDF_ALIGN_RIGHT, "Receipt");
	top += 24;
	date_text = day_text(context, date);
	top = draw_meta(pdf, top, "Receipt no.", number);
	top = draw_meta(pdf, top, "Paid on", date_text);
	if (!venture_string_is_empty(method))
	{
		g_autofree gchar *shown = g_strdup(method);
		shown[0] = g_ascii_toupper(shown[0]);
		top = draw_meta(pdf, top, "Paid by", shown);
	}
	top = draw_meta(pdf, top, "Reference", reference);

	y = MAX(y, top) + 22;
	y = draw_party(pdf, y, "RECEIVED FROM", company);
	y += 24;

	/* The amount, large: the one figure a receipt exists to say. */
	amount_text = money_text(amount);
	venture_pdf_writer_fill(pdf, DOC_MARGIN, y - 20, DOC_RIGHT - DOC_MARGIN, 44, 0.95);
	venture_pdf_writer_set_grey(pdf, LABEL);
	venture_pdf_writer_text(pdf, DOC_MARGIN + 12, y + 6, 10, TRUE, VENTURE_PDF_ALIGN_LEFT, "Amount received");
	venture_pdf_writer_set_grey(pdf, INK);
	venture_pdf_writer_text(pdf, DOC_RIGHT - 12, y + 8, 18, TRUE, VENTURE_PDF_ALIGN_RIGHT, amount_text);
	y += 56;

	if (allocations->len > 0)
	{
		venture_pdf_writer_fill(pdf, DOC_MARGIN, y - 13, DOC_RIGHT - DOC_MARGIN, 20, 0.93);
		venture_pdf_writer_set_grey(pdf, LABEL);
		venture_pdf_writer_text(pdf, DOC_MARGIN + 8, y, 9, TRUE, VENTURE_PDF_ALIGN_LEFT, "Applied to");
		venture_pdf_writer_text(pdf, column_amount, y, 9, TRUE, VENTURE_PDF_ALIGN_RIGHT, "Amount");
		venture_pdf_writer_set_grey(pdf, INK);
		y += 22;
	}

	for (i = 0; i < allocations->len; i++)
	{
		VentureEntity *allocation = g_ptr_array_index(allocations, i);
		g_autoptr(VentureMoney) part = NULL;
		g_autoptr(VentureEntity) invoice = NULL;
		g_autofree gchar *label = NULL, *part_text = NULL, *invoice_number = NULL;
		gint64 invoice_id = 0;

		g_object_get(allocation, "invoice-id", &invoice_id, "amount", &part, NULL);
		invoice = venture_database_get(db, VENTURE_TYPE_INVOICE, invoice_id, NULL);
		if (NULL != invoice)
			g_object_get(invoice, "number", &invoice_number, NULL);
		label = g_strdup(!venture_string_is_empty(invoice_number) ? invoice_number : "Invoice");
		part_text = money_text(part);
		venture_pdf_writer_text(pdf, DOC_MARGIN + 8, y, 10, FALSE, VENTURE_PDF_ALIGN_LEFT, label);
		venture_pdf_writer_text(pdf, column_amount, y, 10, FALSE, VENTURE_PDF_ALIGN_RIGHT, part_text);
		y += 16;
		accumulate(&applied, part);
	}

	/* What was not applied stays with the customer as credit. */
	if (NULL != amount && NULL != applied)
	{
		g_autoptr(VentureMoney) left = venture_money_subtract(amount, applied, NULL);

		if (NULL != left && venture_money_get_amount(left) > 0)
		{
			y += 6;
			y = draw_total(pdf, y, "Kept as credit", left, FALSE);
		}
	}

	venture_pdf_writer_set_grey(pdf, LABEL);
	venture_pdf_writer_text(pdf, DOC_MARGIN, DOC_HEIGHT - 36, 9, FALSE, VENTURE_PDF_ALIGN_LEFT,
	                        "Thank you for your payment.");

	return venture_pdf_writer_finish(pdf);
}

gchar *
venture_financial_documents_filename(VentureEntity *record)
{
	g_return_val_if_fail(VENTURE_IS_ENTITY(record), NULL);

	if (VENTURE_IS_INVOICE(record))
	{
		g_autofree gchar *number = NULL;

		g_object_get(record, "number", &number, NULL);
		if (!venture_string_is_empty(number))
		{
			/* A file name, so nothing that means a directory. */
			g_strdelimit(number, "/\\:", '-');
			return g_strdup_printf("Invoice %s.pdf", number);
		}
		return g_strdup_printf("Invoice %" G_GINT64_FORMAT ".pdf", venture_entity_get_id(record));
	}

	return g_strdup_printf("Receipt %" G_GINT64_FORMAT ".pdf", venture_entity_get_id(record));
}

/* --- Receipts, sent ----------------------------------------------------- */

typedef struct
{
	VentureContext *context;	/* not owned: the context owns this */
	VentureDatabase *database;
	GArray *pending;		/* payment ids saved in the open transaction */
	gulong saved_handler;
	gulong finished_handler;
} VentureReceiptSender;

static void
receipt_sender_free(gpointer data)
{
	VentureReceiptSender *sender = data;

	if (NULL != sender->database)
	{
		g_signal_handler_disconnect(sender->database, sender->saved_handler);
		g_signal_handler_disconnect(sender->database, sender->finished_handler);
	}
	g_array_unref(sender->pending);
	g_free(sender);
}

/*
 * One receipt for one payment. Keyed by the payment's identity, so however
 * often the payment is re-saved, the customer gets one receipt. Nothing is
 * sent to a customer with no email address, and nothing is an error: a
 * receipt that cannot be sent must never stop money being recorded.
 */
static void
send_receipt(VentureReceiptSender *sender, gint64 payment_id)
{
	VentureContext *context = sender->context;
	VentureDatabase *db = sender->database;
	g_autoptr(VentureEntity) payment = NULL;
	g_autoptr(VentureEntity) company = NULL;
	g_autoptr(VentureMailMessage) message = NULL;
	g_autoptr(VentureMailMessage) queued = NULL;
	g_autoptr(GBytes) pdf = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) root = NULL;
	g_autoptr(VentureMoney) amount = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *email = NULL, *key = NULL, *name = NULL, *encoded = NULL, *attachments = NULL;
	g_autofree gchar *amount_text = NULL, *subject = NULL, *body = NULL, *customer = NULL;
	gint64 customer_id = 0;
	gconstpointer data;
	gsize size;

	payment = venture_database_get(db, VENTURE_TYPE_PAYMENT, payment_id, NULL);
	if (NULL == payment || venture_entity_is_deleted(payment))
		return;

	g_object_get(payment, "customer-id", &customer_id, "amount", &amount, NULL);
	company = venture_database_get(db, VENTURE_TYPE_COMPANY, customer_id, NULL);
	if (NULL == company)
		return;
	g_object_get(company, "email", &email, NULL);
	if (venture_string_is_empty(email))
		return;

	pdf = venture_financial_documents_receipt_pdf(context, payment, &error);
	if (NULL == pdf)
	{
		g_message("Receipt for payment %" G_GINT64_FORMAT " not drawn: %s", payment_id, error->message);
		return;
	}

	data = g_bytes_get_data(pdf, &size);
	encoded = g_base64_encode(data, size);
	name = venture_financial_documents_filename(payment);
	builder = json_builder_new();
	json_builder_begin_array(builder);
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "type");
	json_builder_add_string_value(builder, "inline");
	json_builder_set_member_name(builder, "name");
	json_builder_add_string_value(builder, name);
	json_builder_set_member_name(builder, "mime");
	json_builder_add_string_value(builder, "application/pdf");
	json_builder_set_member_name(builder, "data");
	json_builder_add_string_value(builder, encoded);
	json_builder_end_object(builder);
	json_builder_end_array(builder);
	root = json_builder_get_root(builder);
	attachments = venture_json_to_string(root, FALSE);

	amount_text = money_text(amount);
	customer = venture_entity_get_display_name(company);
	subject = g_strdup_printf("Receipt for your payment of %s", amount_text);
	body = g_strdup_printf("Hello %s,\n\nThank you -- we have received your payment of %s. "
	                       "Your receipt is attached.\n", customer, amount_text);
	key = g_strdup_printf("receipt:%s", venture_entity_get_uuid(payment));

	message = venture_mail_message_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(message), venture_entity_get_organization_id(payment));
	g_object_set(message, "to", email, "subject", subject, "text-body", body,
	             "attachments", attachments, "idempotency-key", key,
	             "related-type", "payment", "related-id", payment_id, NULL);

	queued = venture_mail_outbox_enqueue(venture_context_get_mail_outbox(context), message, NULL, &error);
	if (NULL == queued)
		g_message("Receipt for payment %" G_GINT64_FORMAT " not queued: %s", payment_id, error->message);
}

static void
receipt_on_saved(
	VentureDatabase	*database,
	VentureEntity	*record,
	gboolean	 created,
	gpointer	 data
){
	VentureReceiptSender *sender = data;
	gint64 id;
	gboolean enabled = FALSE;

	(void)database;

	if (!created || !VENTURE_IS_PAYMENT(record))
		return;

	g_object_get(venture_context_get_config(sender->context), "mail-receipts", &enabled, NULL);
	if (!enabled || !venture_context_module_enabled(sender->context, "mail") ||
	    !venture_context_module_enabled(sender->context, "receivables"))
		return;

	id = venture_entity_get_id(record);
	g_array_append_val(sender->pending, id);
}

/*
 * Receipts go out once the payment -- and its allocations, which are
 * saved after it -- are committed: a receipt for money whose transaction
 * then rolled back would be a receipt for nothing.
 */
static void
receipt_on_finished(
	VentureDatabase	*database,
	gboolean	 committed,
	gpointer	 data
){
	VentureReceiptSender *sender = data;
	g_autoptr(GArray) ready = NULL;
	guint i;

	(void)database;

	if (0 == sender->pending->len)
		return;

	/* Taken before sending: queuing a receipt is itself a transaction,
	 * which finishes back into this handler. */
	ready = sender->pending;
	sender->pending = g_array_new(FALSE, FALSE, sizeof(gint64));

	if (!committed)
		return;

	for (i = 0; i < ready->len; i++)
		send_receipt(sender, g_array_index(ready, gint64, i));
}

void
venture_financial_documents_install_receipts(VentureContext *context)
{
	VentureReceiptSender *sender;

	g_return_if_fail(VENTURE_IS_CONTEXT(context));

	sender = g_new0(VentureReceiptSender, 1);
	sender->context = context;
	sender->database = venture_context_get_database(context);
	sender->pending = g_array_new(FALSE, FALSE, sizeof(gint64));
	sender->saved_handler = g_signal_connect(sender->database, "entity-saved",
		G_CALLBACK(receipt_on_saved), sender);
	sender->finished_handler = g_signal_connect(sender->database, "transaction-finished",
		G_CALLBACK(receipt_on_finished), sender);

	/* Owned by the context, so the handlers go when it does. */
	g_object_set_data_full(G_OBJECT(context), "venture-receipt-sender", sender, receipt_sender_free);
}
