/*
 * test-venture-financial-documents.c - Invoices and receipts as documents.
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * An invoice is a PDF the customer can print and file, attached to the
 * email that sends it; a payment is answered with a receipt, sent on its
 * own the moment the money is recorded. If this regresses a customer gets
 * an email with a table pasted into it and nothing to file, or pays and
 * hears nothing back.
 */

#include <venture.h>

#include <string.h>

#include "venture-test-util.h"

typedef struct
{
	VentureDatabase *db;
	VentureConfig *config;
	VentureContext *context;
	gint64 org;
	gint64 company;
} Fixture;

static void
save(Fixture *f, VentureEntity *record)
{
	g_autoptr(GError) error = NULL;

	if (!venture_database_save(f->db, record, NULL, &error))
		g_error("save %s: %s", G_OBJECT_TYPE_NAME(record), error->message);
}

static void
set_up(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureCompany) company = NULL;

	f->config = venture_config_new();
	if (NULL != data)
		g_object_set(f->config, "mail-receipts", FALSE, NULL);
	f->db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->db);
	f->org = venture_context_get_default_organization_id(f->context);

	company = venture_company_new();
	g_object_set(company, "name", "Bellhaven (Books)", "email", "orders@bellhaven.example", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(company), f->org);
	save(f, VENTURE_ENTITY(company));
	f->company = venture_entity_get_id(VENTURE_ENTITY(company));
}

static void
tear_down(Fixture *f, gconstpointer data)
{
	(void)data;
	g_clear_object(&f->context);
	g_clear_object(&f->db);
	g_clear_object(&f->config);
}

static VentureEntity *
invoice_full(Fixture *f, const gchar *number, const gchar *description, const gchar *terms, const gchar *price)
{
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(JsonNode) root = NULL;
	g_autoptr(GError) error = NULL;
	VentureEntity *invoice;

	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "company_id");
	json_builder_add_int_value(builder, f->company);
	json_builder_set_member_name(builder, "number");
	json_builder_add_string_value(builder, number);
	json_builder_set_member_name(builder, "terms");
	json_builder_add_string_value(builder, terms);
	json_builder_set_member_name(builder, "send");
	json_builder_add_boolean_value(builder, TRUE);
	json_builder_set_member_name(builder, "lines");
	json_builder_begin_array(builder);
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "description");
	json_builder_add_string_value(builder, description);
	json_builder_set_member_name(builder, "quantity");
	json_builder_add_double_value(builder, 120);
	json_builder_set_member_name(builder, "unit_price");
	json_builder_add_string_value(builder, price);
	json_builder_end_object(builder);
	json_builder_end_array(builder);
	json_builder_end_object(builder);
	root = json_builder_get_root(builder);

	invoice = venture_document_service_compose_invoice(venture_document_service_get(f->db),
		f->org, json_node_get_object(root), NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(invoice);

	return invoice;
}

static VentureEntity *
invoice_new_numbered(Fixture *f, const gchar *number)
{
	return invoice_full(f, number, "Paperbacks, 120", "Payment within thirty days.", "9.50 USD");
}

static VentureEntity *
invoice_new(Fixture *f)
{
	return invoice_new_numbered(f, "INV-0041");
}

static gchar *
bytes_text(GBytes *bytes)
{
	gsize size;
	const gchar *data = g_bytes_get_data(bytes, &size);

	return g_strndup(data, size);
}

/* The invoice PDF says who it is to, what it is for and what is owed,
 * with a bracket in the customer's name drawn, not breaking the file. */
static void
test_invoice_pdf(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) invoice = invoice_new(f);
	g_autoptr(GBytes) pdf = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *text = NULL;
	g_autofree gchar *name = NULL;

	(void)data;

	pdf = venture_financial_documents_invoice_pdf(f->context, invoice, &error);
	g_assert_no_error(error);
	g_assert_nonnull(pdf);
	text = bytes_text(pdf);

	g_assert_true(g_str_has_prefix(text, "%PDF-1.4"));
	g_assert_nonnull(strstr(text, "(Invoice INV-0041)"));
	g_assert_nonnull(strstr(text, "(Bellhaven \\(Books\\))"));
	g_assert_nonnull(strstr(text, "(Paperbacks, 120)"));
	g_assert_nonnull(strstr(text, "($1,140.00)"));
	g_assert_nonnull(strstr(text, "(Payment within thirty days.)"));

	name = venture_financial_documents_filename(invoice);
	g_assert_cmpstr(name, ==, "Invoice INV-0041.pdf");
}

/* One piece of text as drawn: its box on the page, from the top. */
typedef struct
{
	gdouble left, right, top, bottom;
	guint page;
	gchar *text;
} Box;

static void
box_clear(gpointer data)
{
	g_free(((Box *)data)->text);
}

/*
 * Every "BT /Fn size Tf x y Td (text) Tj ET" the writer emitted, measured
 * with the writer's own widths. A box spans the font's ascent above the
 * baseline and its descent below.
 */
static GArray *
text_boxes(GBytes *pdf)
{
	g_autofree gchar *text = bytes_text(pdf);
	g_autoptr(VenturePdfWriter) measure = venture_pdf_writer_new(595, 842);
	GArray *boxes = g_array_new(FALSE, TRUE, sizeof(Box));
	const gchar *p = text;

	g_array_set_clear_func(boxes, box_clear);
	while ((p = strstr(p, "BT /F")) != NULL)
	{
		gboolean bold = p[5] == '2';
		gdouble size, x, y;
		gchar *end = NULL;
		GString *literal = g_string_new(NULL);
		Box box;

		size = g_ascii_strtod(p + 7, &end);
		x = g_ascii_strtod(strstr(end, "Tf") + 2, &end);
		y = g_ascii_strtod(end, &end);
		p = strstr(end, "(") + 1;
		for (; *p != '\0' && *p != ')'; p++)
		{
			if (*p == '\\' && p[1] != '\0')
				p++;
			g_string_append_c(literal, *p);
		}
		{
			const gchar *stream = text;
			box.page = 0;
			while ((stream = strstr(stream, "\nstream\n")) != NULL && stream < p)
			{
				box.page++;
				stream++;
			}
		}
		box.left = x;
		box.right = x + venture_pdf_writer_text_width(measure, size, bold, literal->str);
		box.top = (842 - y) - size * 0.75;
		box.bottom = (842 - y) + size * 0.2;
		box.text = g_string_free(literal, FALSE);
		g_array_append_val(boxes, box);
	}
	return boxes;
}

/*
 * A long invoice number and a long customer name stay on the page and
 * clear of everything else: the title shrinks and wraps on the right,
 * names wrap in their column. If this regresses, the number is drawn
 * straight across the business's letterhead.
 */
static void
test_invoice_pdf_long_names(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(VentureEntity) company = venture_database_get(f->db, VENTURE_TYPE_COMPANY, f->company, NULL);
	g_autoptr(VentureEntity) org = venture_database_get(f->db, VENTURE_TYPE_ORGANIZATION, f->org, NULL);
	g_autoptr(GBytes) pdf = NULL;
	g_autoptr(GArray) boxes = NULL;
	g_autoptr(GError) error = NULL;
	guint i, j;

	(void)data;
	g_object_set(company, "name", "The Extraordinarily Long-Named International Consolidated Book Distribution Company of Bellhaven", NULL);
	g_assert_true(venture_database_save(f->db, company, NULL, &error));
	g_object_set(org, "name", "Wrenmouth Press and Allied Publishing Enterprises of the Northern Coast", NULL);
	g_assert_true(venture_database_save(f->db, org, NULL, &error));
	g_assert_no_error(error);
	invoice = invoice_new_numbered(f, "INV-2026-ACME-CORPORATION-INTERNATIONAL-HOLDINGS-SUBSIDIARY-0000000041");

	pdf = venture_financial_documents_invoice_pdf(f->context, invoice, &error);
	g_assert_no_error(error);
	boxes = text_boxes(pdf);
	g_assert_cmpuint(boxes->len, >, 5);
	for (i = 0; i < boxes->len; i++)
	{
		Box *a = &g_array_index(boxes, Box, i);

		if (a->left < 47.5 || a->right > 547.5)
			g_error("\"%s\" runs off the page: %.1f to %.1f", a->text, a->left, a->right);
		for (j = i + 1; j < boxes->len; j++)
		{
			Box *b = &g_array_index(boxes, Box, j);

			if (a->page == b->page && a->left < b->right && b->left < a->right && a->top < b->bottom && b->top < a->bottom)
				g_error("\"%s\" overlaps \"%s\"", a->text, b->text);
		}
	}
}

/* Long descriptions and terms must remain printable on continuation
 * pages, and large amounts must not overwrite another column. */
static void
test_invoice_pdf_pages(Fixture *f, gconstpointer data)
{
	g_autoptr(GString) prose = g_string_new(NULL);
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(GBytes) pdf = NULL;
	g_autoptr(GArray) boxes = NULL;
	g_autoptr(GError) error = NULL;
	guint i, j, pages = 0;

	(void)data;
	for (i = 0; i < 150; i++)
		g_string_append(prose, "A long description needs room to remain readable.\n");
	invoice = invoice_full(f, "LARGE", prose->str, prose->str, "999999999.99 USD");
	pdf = venture_financial_documents_invoice_pdf(f->context, invoice, &error);
	g_assert_no_error(error);
	boxes = text_boxes(pdf);
	for (i = 0; i < boxes->len; i++)
	{
		Box *a = &g_array_index(boxes, Box, i);
		pages = MAX(pages, a->page);
		g_assert_cmpfloat(a->top, >=, 40);
		g_assert_cmpfloat(a->bottom, <=, 810);
		g_assert_cmpfloat(a->left, >=, 47.5);
		g_assert_cmpfloat(a->right, <=, 547.5);
		for (j = i + 1; j < boxes->len; j++)
		{
			Box *b = &g_array_index(boxes, Box, j);
			if (a->page == b->page && a->left < b->right && b->left < a->right &&
			    a->top < b->bottom && b->top < a->bottom)
				g_error("%s overlaps %s", a->text, b->text);
		}
	}
	g_assert_cmpuint(pages, >, 3);
}

/* A printable draft is not paid, and shipping belongs in its total. */
static void
test_invoice_pdf_shipping_draft(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureInvoice) invoice = venture_invoice_new();
	g_autoptr(VentureInvoiceLine) line = venture_invoice_line_new();
	g_autoptr(VentureMoney) shipping = venture_money_new(2500, "USD", 2);
	g_autoptr(VentureMoney) unit = venture_money_new(10000, "USD", 2);
	g_autoptr(GBytes) pdf = NULL;
	g_autofree gchar *text = NULL;
	g_autoptr(GError) error = NULL;

	(void)data;
	venture_entity_set_organization_id(VENTURE_ENTITY(invoice), f->org);
	g_object_set(invoice, "number", "DRAFT", "company-id", f->company, "shipping-amount", shipping, NULL);
	save(f, VENTURE_ENTITY(invoice));
	venture_entity_set_organization_id(VENTURE_ENTITY(line), f->org);
	g_object_set(line, "invoice-id", venture_entity_get_id(VENTURE_ENTITY(invoice)),
		"description", "Goods", "quantity", 1.0, "unit-price", unit, NULL);
	save(f, VENTURE_ENTITY(line));
	pdf = venture_financial_documents_invoice_pdf(f->context, VENTURE_ENTITY(invoice), &error);
	g_assert_no_error(error);
	text = bytes_text(pdf);
	g_assert_nonnull(strstr(text, "(Shipping)"));
	g_assert_nonnull(strstr(text, "($125.00)"));
	g_assert_null(strstr(text, "(Paid in full)"));
}

/* A user-supplied invoice number must remain a single quoted filename,
 * never become another HTTP header or Content-Disposition parameter. */
static void
test_invoice_filename(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureInvoice) invoice = venture_invoice_new();
	g_autofree gchar *name = NULL;

	(void)f;
	(void)data;
	g_object_set(invoice, "number", "INV\"\r\nX-Bad: yes/\\file", NULL);
	name = venture_financial_documents_filename(VENTURE_ENTITY(invoice));
	g_assert_null(strchr(name, '\r'));
	g_assert_null(strchr(name, '\n'));
	g_assert_null(strchr(name, '\"'));
	g_assert_null(strchr(name, '\\'));
	g_assert_null(strchr(name, '/'));
}

/* Emailing an invoice attaches its PDF. */
static void
test_invoice_email_attaches_pdf(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) invoice = invoice_new(f);
	g_autoptr(VentureMailMessage) message = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *attachments = NULL;

	(void)data;

	message = venture_mail_send_invoice(f->context, f->org, venture_entity_get_id(invoice), NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(message);
	g_object_get(message, "attachments", &attachments, NULL);
	g_assert_nonnull(attachments);
	g_assert_nonnull(strstr(attachments, "\"Invoice INV-0041.pdf\""));
	g_assert_nonnull(strstr(attachments, "application/pdf"));
}

/* Money recorded against an invoice sends a receipt, once, with the
 * receipt attached, to the customer's email -- and says what was paid. */
static void
test_receipt_sent_on_payment(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) invoice = invoice_new(f);
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) messages = NULL;
	g_autoptr(GPtrArray) payments = NULL;
	g_autoptr(GBytes) pdf = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	g_autofree gchar *to = NULL, *attachments = NULL, *text = NULL, *key = NULL;
	guint i, receipts = 0;

	(void)data;

	g_assert_true(venture_settlement_service_settle_invoice(venture_settlement_service_get(f->db),
		venture_entity_get_id(invoice), now, NULL, &error));
	g_assert_no_error(error);

	query = venture_query_new(VENTURE_TYPE_MAIL_MESSAGE);
	venture_query_set_limit(query, 0);
	messages = venture_database_find(f->db, query, &error);
	g_assert_no_error(error);
	for (i = 0; i < messages->len; i++)
	{
		g_autofree gchar *this_key = NULL;

		g_object_get(g_ptr_array_index(messages, i), "idempotency-key", &this_key, NULL);
		if (g_str_has_prefix(this_key, "receipt:"))
		{
			receipts++;
			g_object_get(g_ptr_array_index(messages, i), "to", &to,
			             "attachments", &attachments, "idempotency-key", &key, NULL);
		}
	}
	g_assert_cmpuint(receipts, ==, 1);
	g_assert_cmpstr(to, ==, "orders@bellhaven.example");
	g_assert_nonnull(strstr(attachments, "application/pdf"));

	g_clear_object(&query);
	query = venture_query_new(VENTURE_TYPE_PAYMENT);
	payments = venture_database_find(f->db, query, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(payments->len, ==, 1);
	pdf = venture_financial_documents_receipt_pdf(f->context, g_ptr_array_index(payments, 0), &error);
	g_assert_no_error(error);
	text = bytes_text(pdf);
	g_assert_nonnull(strstr(text, "(Receipt)"));
	g_assert_nonnull(strstr(text, "($1,140.00)"));
	g_assert_nonnull(strstr(text, "(INV-0041)"));
}

/* A receipt must preserve a long allocated invoice reference across
 * pages rather than clip the customer's evidence of what was paid. */
static void
test_receipt_pdf_pages(Fixture *f, gconstpointer data)
{
	g_autofree gchar *number = g_strnfill(12000, 'W');
	g_autoptr(VentureEntity) invoice = invoice_new_numbered(f, number);
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_PAYMENT);
	g_autoptr(VentureEntity) payment = NULL;
	g_autoptr(GBytes) pdf = NULL;
	g_autoptr(GArray) boxes = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(GError) error = NULL;
	guint i, pages = 0;

	(void)data;
	g_assert_true(venture_settlement_service_settle_invoice(venture_settlement_service_get(f->db),
		venture_entity_get_id(invoice), now, NULL, &error));
	g_assert_no_error(error);
	payment = venture_database_find_one(f->db, query, &error);
	g_assert_no_error(error);
	pdf = venture_financial_documents_receipt_pdf(f->context, payment, &error);
	g_assert_no_error(error);
	boxes = text_boxes(pdf);
	for (i = 0; i < boxes->len; i++)
	{
		Box *box = &g_array_index(boxes, Box, i);
		pages = MAX(pages, box->page);
		g_assert_cmpfloat(box->top, >=, 40);
		g_assert_cmpfloat(box->bottom, <=, 810);
	}
	g_assert_cmpuint(pages, >, 1);
}

/* An operator who does not want receipts sent gets none. */
static void
test_receipts_can_be_switched_off(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) invoice = invoice_new(f);
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureEntity) found = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) now = venture_time_now();

	(void)data;

	g_assert_true(venture_settlement_service_settle_invoice(venture_settlement_service_get(f->db),
		venture_entity_get_id(invoice), now, NULL, &error));
	g_assert_no_error(error);
	query = venture_query_new(VENTURE_TYPE_MAIL_MESSAGE);
	venture_query_add_filter_string(query, "related-type", VENTURE_FILTER_OP_EQ, "payment", NULL);
	found = venture_database_find_one(f->db, query, &error);
	g_assert_no_error(error);
	g_assert_null(found);
}

/* How many receipt mails are queued, by the key the sender files them under. */
static guint
count_receipts(Fixture *f)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_MAIL_MESSAGE);
	g_autoptr(GPtrArray) messages = NULL;
	g_autoptr(GError) error = NULL;
	guint i, receipts = 0;

	venture_query_set_limit(query, 0);
	messages = venture_database_find(f->db, query, &error);
	g_assert_no_error(error);
	for (i = 0; i < messages->len; i++)
	{
		g_autofree gchar *key = NULL;

		g_object_get(g_ptr_array_index(messages, i), "idempotency-key", &key, NULL);
		if (g_str_has_prefix(key, "receipt:"))
			receipts++;
	}
	return receipts;
}

/* A payment through the settlement service, as every writer records one. */
static void
apply_payment(Fixture *f, VenturePayment *payment)
{
	g_autoptr(GError) error = NULL;

	g_assert_true(venture_settlement_service_apply_payment(venture_settlement_service_get(f->db),
		payment, NULL, NULL, &error));
	g_assert_no_error(error);
}

static VenturePayment *
payment_new(Fixture *f, const gchar *method)
{
	VenturePayment *payment = venture_payment_new();
	g_autoptr(VentureMoney) amount = venture_money_new(5000, "USD", 2);
	g_autoptr(GDateTime) now = venture_time_now();

	venture_entity_set_organization_id(VENTURE_ENTITY(payment), f->org);
	g_object_set(payment, "customer-id", f->company, "amount", amount, "date", now,
		"method", method, NULL);
	return payment;
}

/*
 * A cutover opening and a cutover rollback's re-deposit are money paid
 * before; only money paid now gets a receipt. If this regresses, importing
 * the books from another system emails every customer a receipt for every
 * payment they ever made.
 */
static void
test_receipt_skips_bookkeeping(Fixture *f, gconstpointer data)
{
	g_autoptr(VenturePayment) opening = payment_new(f, "opening");
	g_autoptr(VenturePayment) redeposit = payment_new(f, "transfer");
	g_autoptr(VenturePayment) paid = payment_new(f, "transfer");

	(void)data;

	apply_payment(f, opening);
	g_assert_cmpuint(count_receipts(f), ==, 0);
	venture_payment_mark_bookkeeping(redeposit);
	apply_payment(f, redeposit);
	g_assert_cmpuint(count_receipts(f), ==, 0);
	apply_payment(f, paid);
	g_assert_cmpuint(count_receipts(f), ==, 1);
}

/*
 * A payment saved with no transaction open still gets its receipt at once.
 * The sender queues on entity-saved and sends on the commit, which works
 * only because the receivables save hook wraps every payment save in the
 * settlement service's transaction. If a payment ever saves outside it,
 * the database commits before announcing the save, the receipt waits for
 * some unrelated transaction, and this fails.
 */
static void
test_receipt_for_bare_save(Fixture *f, gconstpointer data)
{
	g_autoptr(VenturePayment) payment = payment_new(f, "cash");

	(void)data;

	g_assert_false(venture_database_has_transaction(f->db));
	save(f, VENTURE_ENTITY(payment));
	g_assert_cmpuint(count_receipts(f), ==, 1);
}

/*
 * An invoice with no tax on it says why, with the certificate, because
 * the composer promises exactly that. If this regresses, an exempt
 * customer's invoice is indistinguishable from one somebody forgot to tax.
 */
static void
test_invoice_pdf_exemption(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) invoice = invoice_new(f);
	g_autoptr(GBytes) pdf = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *text = NULL;

	(void)data;

	pdf = venture_financial_documents_invoice_pdf(f->context, invoice, &error);
	g_assert_no_error(error);
	text = bytes_text(pdf);
	g_assert_null(strstr(text, "(Tax exempt"));
	g_clear_pointer(&pdf, g_bytes_unref);
	g_clear_pointer(&text, g_free);

	g_object_set(invoice, "tax-exempt", TRUE,
		"tax-exempt-reason", "Non-profit, certificate EX-12", NULL);
	pdf = venture_financial_documents_invoice_pdf(f->context, invoice, &error);
	g_assert_no_error(error);
	text = bytes_text(pdf);
	g_assert_nonnull(strstr(text, "(Tax exempt: Non-profit, certificate EX-12)"));
}

/*
 * A download keeps an accented invoice number: plain ASCII in filename=,
 * the real name percent-encoded in filename*=. If this regresses, raw
 * UTF-8 sits in a header a client decodes as Latin-1 and the file is saved
 * as mojibake -- or a quote in the name ends the parameter early.
 */
static void
test_content_disposition(Fixture *f, gconstpointer data)
{
	g_autofree gchar *value = NULL;

	(void)f;
	(void)data;

	value = venture_financial_documents_content_disposition("inline", "Invoice M\xc3\xbcller \"1\".pdf");
	g_assert_cmpstr(value, ==,
		"inline; filename=\"Invoice M_ller _1_.pdf\"; filename*=UTF-8''Invoice%20M%C3%BCller%20%221%22.pdf");
	g_clear_pointer(&value, g_free);
	value = venture_financial_documents_content_disposition("inline", "Receipt 12.pdf");
	g_assert_cmpstr(value, ==, "inline; filename=\"Receipt 12.pdf\"; filename*=UTF-8''Receipt%2012.pdf");
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add("/financial-documents/invoice-shipping-draft", Fixture, NULL, set_up, test_invoice_pdf_shipping_draft, tear_down);
	g_test_add("/financial-documents/receipt-pages", Fixture, NULL, set_up, test_receipt_pdf_pages, tear_down);
	g_test_add("/financial-documents/invoice-pages", Fixture, NULL, set_up, test_invoice_pdf_pages, tear_down);
	g_test_add("/financial-documents/invoice-filename", Fixture, NULL, set_up, test_invoice_filename, tear_down);
	g_test_add("/financial-documents/invoice-pdf", Fixture, NULL, set_up, test_invoice_pdf, tear_down);
	g_test_add("/financial-documents/invoice-pdf-long-names", Fixture, NULL, set_up, test_invoice_pdf_long_names, tear_down);
	g_test_add("/financial-documents/invoice-email-attaches-pdf", Fixture, NULL, set_up,
	           test_invoice_email_attaches_pdf, tear_down);
	g_test_add("/financial-documents/receipt-sent-on-payment", Fixture, NULL, set_up,
	           test_receipt_sent_on_payment, tear_down);
	g_test_add("/financial-documents/receipts-can-be-switched-off", Fixture, "off", set_up,
	           test_receipts_can_be_switched_off, tear_down);
	g_test_add("/financial-documents/receipt-skips-bookkeeping", Fixture, NULL, set_up,
	           test_receipt_skips_bookkeeping, tear_down);
	g_test_add("/financial-documents/receipt-for-bare-save", Fixture, NULL, set_up,
	           test_receipt_for_bare_save, tear_down);
	g_test_add("/financial-documents/invoice-pdf-exemption", Fixture, NULL, set_up,
	           test_invoice_pdf_exemption, tear_down);
	g_test_add("/financial-documents/content-disposition", Fixture, NULL, set_up,
	           test_content_disposition, tear_down);

	return g_test_run();
}
