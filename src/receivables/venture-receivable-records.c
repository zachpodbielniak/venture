/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

static const VentureFieldDecl payment_fields[] = {
	VENTURE_FIELD_REF("customer-id", "Customer", NULL, "company", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("date", "Date", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_MONEY("amount", "Amount", "The amount received, including any overpayment"),
	VENTURE_FIELD_NAME("method", "Method", "Cash, transfer, card, manual, or a provider name"),
	VENTURE_FIELD("reference", "Reference", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD("external-id", "External ID", "Unique within the owning organization", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	/* An ordinary UNIQUE column encodes the pair without a hand-written schema. */
	VENTURE_FIELD("external-key", "External key", "Derived by VentureSettlementService", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_UNIQUE | VENTURE_COLUMN_FLAG_IMMUTABLE),
	VENTURE_FIELD_REF("invoice-id", "Allocate to invoice", "Optional: applies up to the invoice balance; the rest remains a credit", "invoice", VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VenturePayment, venture_payment, payment_fields)

static const VentureFieldDecl allocation_fields[] = {
	VENTURE_FIELD_REF("payment-id", "Payment", "Supply a payment or a credit, not both", "payment", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("credit-id", "Credit", "A credit note or unused receipt", "customer_credit", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("invoice-id", "Invoice", NULL, "invoice", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("date", "Date", "When this amount was applied", VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_MONEY("amount", "Amount", NULL),
	VENTURE_FIELD_REF("sale-id", "Sale", "Derived cash-revenue record", "sale", VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VenturePaymentAllocation, venture_payment_allocation, allocation_fields)

static const VentureFieldDecl credit_fields[] = {
	VENTURE_FIELD_REF("customer-id", "Customer", NULL, "company", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("date", "Date", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_MONEY("amount", "Original amount", NULL),
	VENTURE_FIELD_MONEY("remaining", "Remaining", "Derived from allocations and refunds"),
	VENTURE_FIELD_NAME("kind", "Kind", "credit_note, write_off, deposit, or overpayment; receipts create deposits and overpayments"),
	VENTURE_FIELD_REF("payment-id", "Payment", "Set by the service for unused receipts", "payment", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("reference", "Reference", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD_MONEY("tax-amount", "Tax amount", "Optional tax portion of a credit note"),
	VENTURE_FIELD("opening-at", "Opening balance at", "Cutover instant a migrated credit entered the ledger",
		VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("tax-jurisdiction-id", "Tax jurisdiction", "Jurisdiction the credited tax belongs to on the sales tax return",
		"tax_jurisdiction", VENTURE_COLUMN_FLAG_INDEXED)
};
VENTURE_DEFINE_ENTITY(VentureCustomerCredit, venture_customer_credit, credit_fields)

static const VentureFieldDecl refund_fields[] = {
	VENTURE_FIELD_REF("customer-id", "Customer", NULL, "company", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_REF("allocation-id", "Allocation", "Refund an applied cash receipt, or an unused credit", "payment_allocation", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("credit-id", "Credit", NULL, "customer_credit", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("date", "Date", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_MONEY("amount", "Amount", NULL),
	VENTURE_FIELD("reference", "Reference", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD_REF("sale-id", "Cash adjustment", "Derived cash report adjustment at the refund date", "sale", VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VentureRefund, venture_refund, refund_fields)

static const VentureFieldDecl event_fields[] = {
	VENTURE_FIELD_REF("invoice-id", "Invoice", NULL, "invoice", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_REF("customer-id", "Customer", NULL, "company", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_NAME("state", "State", "Issued, void, or a plugin workflow state"),
	VENTURE_FIELD_NAME("kind", "Event", "issue, void, or transition"),
	VENTURE_FIELD("date", "Date", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("due-at", "Due", "Frozen at issue", VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_MONEY("amount", "Issued amount", "Frozen from the invoice lines at issue"),
	VENTURE_FIELD_MONEY("book-amount", "Book amount", "Issued amount valued in the organization book currency"),
	VENTURE_FIELD_MONEY("net-amount", "Frozen net", "Income after discount, frozen at issue"),
	VENTURE_FIELD_MONEY("tax-amount", "Frozen tax", "Tax liability frozen at issue"),
	VENTURE_FIELD_MONEY("discount-amount", "Frozen discount", "Discount frozen at issue"),
	VENTURE_FIELD_MONEY("shipping-amount", "Frozen shipping", "Shipping frozen at issue"),
	VENTURE_FIELD_REF("venture-id", "Venture", NULL, "venture", VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VentureInvoiceEvent, venture_invoice_event, event_fields)
