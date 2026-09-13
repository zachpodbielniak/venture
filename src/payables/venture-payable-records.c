/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

static const VentureFieldDecl payment_fields[] = {
	VENTURE_FIELD_REF("vendor-id", "Vendor", NULL, "company", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("date", "Date", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_MONEY("amount", "Amount", "The amount received, including any overpayment"),
	VENTURE_FIELD_NAME("method", "Method", "Cash, transfer, card, manual, or a provider name"),
	VENTURE_FIELD("reference", "Reference", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD("external-id", "External ID", "Unique within the owning organization", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	/* An ordinary UNIQUE column encodes the pair without a hand-written schema. */
	VENTURE_FIELD("external-key", "External key", "Derived by VenturePayablesService", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_UNIQUE | VENTURE_COLUMN_FLAG_IMMUTABLE),
	VENTURE_FIELD("bank-account-id", "Bank account", "Optional bank identifier", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("bill-id", "Allocate to bill", "Optional: applies up to the invoice balance; the rest remains a credit", "vendor_bill", VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VentureBillPayment, venture_bill_payment, payment_fields)

static const VentureFieldDecl allocation_fields[] = {
	VENTURE_FIELD_REF("payment-id", "Payment", "Supply a payment or a credit, not both", "bill_payment", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("credit-id", "Credit", "A credit note or unused receipt", "vendor_credit", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("bill-id", "Invoice", NULL, "vendor_bill", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("date", "Date", "When this amount was applied", VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_MONEY("amount", "Amount", NULL),
	VENTURE_FIELD_REF("expense-id", "Sale", "Derived cash-revenue record", "expense", VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VentureBillPaymentAllocation, venture_bill_payment_allocation, allocation_fields)

static const VentureFieldDecl credit_fields[] = {
	VENTURE_FIELD_REF("vendor-id", "Vendor", NULL, "company", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("date", "Date", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_MONEY("amount", "Original amount", NULL),
	VENTURE_FIELD_MONEY("remaining", "Remaining", "Derived from allocations and refunds"),
	VENTURE_FIELD_NAME("kind", "Kind", "credit_note, deposit, or overpayment; receipts create deposits and overpayments"),
	VENTURE_FIELD_REF("payment-id", "Payment", "Set by the service for unused receipts", "bill_payment", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("reference", "Reference", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_SEARCHABLE)
};
VENTURE_DEFINE_ENTITY(VentureVendorCredit, venture_vendor_credit, credit_fields)

static const VentureFieldDecl refund_fields[] = {
	VENTURE_FIELD_REF("vendor-id", "Vendor", NULL, "company", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_REF("allocation-id", "Allocation", "Refund an applied cash receipt, or an unused credit", "bill_payment_allocation", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("credit-id", "Credit", NULL, "vendor_credit", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("date", "Date", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_MONEY("amount", "Amount", NULL),
	VENTURE_FIELD("reference", "Reference", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD_REF("expense-id", "Cash adjustment", "Derived cash report adjustment at the refund date", "expense", VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VentureBillRefund, venture_bill_refund, refund_fields)

static const VentureFieldDecl event_fields[] = {
	VENTURE_FIELD_REF("bill-id", "Invoice", NULL, "vendor_bill", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_REF("vendor-id", "Vendor", NULL, "company", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_NAME("state", "State", "Issued, void, or a plugin workflow state"),
	VENTURE_FIELD_NAME("kind", "Event", "issue, void, or transition"),
	VENTURE_FIELD("date", "Date", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("due-date", "Due", "Frozen at issue", VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_MONEY("amount", "Issued amount", "Frozen from the invoice lines at issue"),
	VENTURE_FIELD_REF("venture-id", "Venture", NULL, "venture", VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VentureVendorBillEvent, venture_vendor_bill_event, event_fields)

static const VentureFieldDecl bill_fields[] = {
	VENTURE_FIELD_REF("company-id", "Vendor", "A supplier company", "company", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("number", "Number", NULL, VENTURE_FIELD_KIND_STRING,
		VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION | VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD("bill-date", "Bill date", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("due-date", "Due date", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_NAME("currency", "Currency", "Uppercase ISO 4217 code"),
	VENTURE_FIELD_NAME("status", "Status", "draft, approved, partially_paid, paid, void; VenturePayablesService owns transitions"),
	VENTURE_FIELD_REF("venture-id", "Venture", NULL, "venture", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("memo", "Memo", NULL)
};
VENTURE_DEFINE_ENTITY(VentureVendorBill, venture_vendor_bill, bill_fields)

static const VentureFieldDecl bill_line_fields[] = {
	VENTURE_FIELD_REF("bill-id", "Bill", NULL, "vendor_bill", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_NAME("description", "Description", NULL),
	/* A decimal string is parsed to an integer rational, never binary floating point. */
	VENTURE_FIELD_NAME("quantity", "Quantity", "Exact decimal quantity, up to three decimal places"),
	VENTURE_FIELD_MONEY("unit-price", "Unit price", NULL),
	VENTURE_FIELD_REF("account-id", "Expense account", NULL, "account", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("category", "Category", "General expenses when no account is specified", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_MONEY("tax-amount", "Tax amount", "Tax included in expense cost"),
	VENTURE_FIELD("position", "Position", NULL, VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VentureVendorBillLine, venture_vendor_bill_line, bill_line_fields)

VentureMoney *
venture_vendor_bill_line_get_amount(VentureVendorBillLine *self, GError **error)
{
	g_autofree gchar *quantity = NULL;
	g_autoptr(VentureMoney) price = NULL;
	g_autoptr(VentureMoney) tax = NULL;
	g_autoptr(VentureMoney) net = NULL;
	const gchar *p;
	gint64 numerator = 0;
	gint64 denominator = 1;
	guint decimals = 0;
	gboolean point = FALSE;
	gboolean digit = FALSE;

	g_object_get(self, "quantity", &quantity, "unit-price", &price, "tax-amount", &tax, NULL);
	if (quantity == NULL || price == NULL)
		goto invalid;
	for (p = quantity; *p != '\0'; p++)
	{
		if (*p == '.' && !point)
		{
			point = TRUE;
			continue;
		}
		if (!g_ascii_isdigit(*p) || numerator > (G_MAXINT64 - (*p - '0')) / 10)
			goto invalid;
		digit = TRUE;
		numerator = numerator * 10 + (*p - '0');
		if (point)
		{
			if (++decimals > 3)
				goto invalid;
			denominator *= 10;
		}
	}
	if (!digit || numerator <= 0 || venture_money_get_amount(price) < 0 ||
		(tax != NULL && venture_money_get_amount(tax) < 0))
		goto invalid;
	net = venture_money_multiply_rational(price, numerator, denominator, error);
	if (net == NULL)
		return NULL;
	return tax != NULL ? venture_money_add(net, tax, error) : g_steal_pointer(&net);
invalid:
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		"VenturePayablesService: positive exact quantity (up to three decimals) and nonnegative price/tax required");
	return NULL;
}
