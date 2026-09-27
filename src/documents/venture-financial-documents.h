/*
 * venture-financial-documents.h - Invoices and receipts as PDF documents
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * What a customer keeps: the invoice they are asked to pay and the receipt
 * for what they paid, each a PDF drawn from the records -- the same
 * figures the invoice page shows, because they are read the same way. The
 * receipt is also sent: once a payment's transaction commits, the customer
 * is mailed a receipt with the PDF attached, unless the operator turned
 * mail.receipts off or the customer has no email address.
 */

#ifndef VENTURE_FINANCIAL_DOCUMENTS_H
#define VENTURE_FINANCIAL_DOCUMENTS_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>

G_BEGIN_DECLS


/**
 * venture_financial_documents_invoice_pdf:
 * @context: the application context
 * @invoice: a saved invoice
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer full) (nullable): the invoice as a PDF file
 */
GBytes *
venture_financial_documents_invoice_pdf(
	VentureContext	 *context,
	VentureEntity	 *invoice,
	GError		**error
);

/**
 * venture_financial_documents_receipt_pdf:
 * @context: the application context
 * @payment: a saved customer payment
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer full) (nullable): the receipt for @payment as a PDF file
 */
GBytes *
venture_financial_documents_receipt_pdf(
	VentureContext	 *context,
	VentureEntity	 *payment,
	GError		**error
);

/**
 * venture_financial_documents_filename:
 * @record: an invoice or a payment
 *
 * Returns: (transfer full): the file name the document is offered under,
 *   such as "Invoice INV-0041.pdf" or "Receipt 12.pdf"
 */
gchar *
venture_financial_documents_filename(VentureEntity *record);

/**
 * venture_financial_documents_content_disposition:
 * @disposition: "inline" or "attachment"
 * @filename: a UTF-8 file name, such as venture_financial_documents_filename() returns
 *
 * Builds a Content-Disposition value naming @filename twice: an ASCII
 * `filename=` every client reads, and an RFC 5987 `filename*=UTF-8''...`
 * that clients which understand it prefer, so "Invoice Müller.pdf" is
 * saved under its own name rather than as mojibake.
 *
 * Returns: (transfer full): the header value
 */
gchar *
venture_financial_documents_content_disposition(
	const gchar	*disposition,
	const gchar	*filename
);

/**
 * venture_financial_documents_install_receipts:
 * @context: the application context
 *
 * Mails a receipt for every customer payment once the transaction that
 * recorded it commits. Called once, by the context.
 */
void
venture_financial_documents_install_receipts(VentureContext *context);

/**
 * venture_financial_documents_thermal:
 * @context: application context
 * @record: readable payment or invoice
 * @printer: configured layout defaults
 * @error: return location for an error
 * Returns: (transfer full) (nullable): receipt or invoice summary ESC/POS bytes
 */
GBytes *
venture_financial_documents_thermal(
	VentureContext			*context,
	VentureEntity			*record,
	const VenturePrinter	*printer,
	GError					**error
);

G_END_DECLS

#endif /* VENTURE_FINANCIAL_DOCUMENTS_H */
