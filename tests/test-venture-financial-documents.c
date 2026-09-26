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
invoice_new_numbered(Fixture *f, const gchar *number)
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
	json_builder_add_string_value(builder, "Payment within thirty days.");
	json_builder_set_member_name(builder, "send");
	json_builder_add_boolean_value(builder, TRUE);
	json_builder_set_member_name(builder, "lines");
	json_builder_begin_array(builder);
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "description");
	json_builder_add_string_value(builder, "Paperbacks, 120");
	json_builder_set_member_name(builder, "quantity");
	json_builder_add_double_value(builder, 120);
	json_builder_set_member_name(builder, "unit_price");
	json_builder_add_string_value(builder, "9.50 USD");
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

			if (a->left < b->right && b->left < a->right && a->top < b->bottom && b->top < a->bottom)
				g_error("\"%s\" overlaps \"%s\"", a->text, b->text);
		}
	}
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

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add("/financial-documents/invoice-pdf", Fixture, NULL, set_up, test_invoice_pdf, tear_down);
	g_test_add("/financial-documents/invoice-pdf-long-names", Fixture, NULL, set_up, test_invoice_pdf_long_names, tear_down);
	g_test_add("/financial-documents/invoice-email-attaches-pdf", Fixture, NULL, set_up,
	           test_invoice_email_attaches_pdf, tear_down);
	g_test_add("/financial-documents/receipt-sent-on-payment", Fixture, NULL, set_up,
	           test_receipt_sent_on_payment, tear_down);
	g_test_add("/financial-documents/receipts-can-be-switched-off", Fixture, "off", set_up,
	           test_receipts_can_be_switched_off, tear_down);

	return g_test_run();
}
