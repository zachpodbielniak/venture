/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

static const VentureFieldDecl claim_fields[] = {
	VENTURE_FIELD("number", "Number", NULL, VENTURE_FIELD_KIND_STRING,
		VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION | VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD_REF("employee-id", "Employee", "The user being reimbursed", "user", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("claim-date", "Claim date", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_NAME("currency", "Currency", "Uppercase ISO 4217 code"),
	VENTURE_FIELD_NAME("status", "Status", "draft, submitted, approved, paid, rejected; VentureClaimsService owns transitions"),
	VENTURE_FIELD("settlement", "Settlement", "cash or payable; payment posts expense plus cash or AP",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_MONEY("total", "Total", "Derived from lines at submit"),
	VENTURE_FIELD_TEXT("memo", "Memo", NULL)
};
VENTURE_DEFINE_ENTITY(VentureExpenseClaim, venture_expense_claim, claim_fields)

static const VentureFieldDecl claim_line_fields[] = {
	VENTURE_FIELD_REF("claim-id", "Claim", NULL, "expense_claim", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_NAME("kind", "Kind", "receipt or mileage"),
	VENTURE_FIELD_NAME("description", "Description", NULL),
	VENTURE_FIELD_MONEY("amount", "Amount", "Receipt total, or computed mileage"),
	VENTURE_FIELD("miles", "Miles", "Exact decimal miles, up to three decimal places",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_MONEY("mileage-rate", "Mileage rate", "Amount per mile"),
	VENTURE_FIELD_REF("document-id", "Receipt", "Filed document or capture original", "document", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("receipt-hash", "Receipt hash", "Copied from the document checksum; refused when duplicated",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_REF("account-id", "Expense account", NULL, "account", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("category", "Category", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("occurred-at", "When", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("position", "Position", NULL, VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VentureExpenseClaimLine, venture_expense_claim_line, claim_line_fields)

static gboolean
parse_miles(const gchar *quantity, gint64 *numerator, gint64 *denominator, GError **error)
{
	const gchar *p;
	gint64 n = 0;
	gint64 d = 1;
	guint decimals = 0;
	gboolean point = FALSE;
	gboolean digit = FALSE;

	if (quantity == NULL)
		goto invalid;
	for (p = quantity; *p != '\0'; p++)
	{
		if (*p == '.' && !point)
		{
			point = TRUE;
			continue;
		}
		if (!g_ascii_isdigit(*p) || n > (G_MAXINT64 - (*p - '0')) / 10)
			goto invalid;
		digit = TRUE;
		n = n * 10 + (*p - '0');
		if (point)
		{
			if (++decimals > 3)
				goto invalid;
			d *= 10;
		}
	}
	if (!digit)
		goto invalid;
	*numerator = n;
	*denominator = d;
	return TRUE;
invalid:
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		"VentureClaimsService: exact miles (up to three decimals) required");
	return FALSE;
}

VentureMoney *
venture_expense_claim_line_get_amount(VentureExpenseClaimLine *self, GError **error)
{
	g_autofree gchar *kind = NULL;
	g_autofree gchar *miles = NULL;
	g_autoptr(VentureMoney) amount = NULL;
	g_autoptr(VentureMoney) rate = NULL;

	g_object_get(self, "kind", &kind, "amount", &amount, "miles", &miles,
		"mileage-rate", &rate, NULL);
	if (g_strcmp0(kind, "mileage") == 0)
	{
		gint64 numerator = 0;
		gint64 denominator = 1;

		if (rate == NULL || venture_money_get_amount(rate) < 0)
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
				"VentureClaimsService: mileage needs a nonnegative rate");
			return NULL;
		}
		if (!parse_miles(miles, &numerator, &denominator, error))
			return NULL;
		return venture_money_multiply_rational(rate, numerator, denominator, error);
	}
	if (amount == NULL || venture_money_get_amount(amount) < 0)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"VentureClaimsService: a receipt line needs a nonnegative amount");
		return NULL;
	}
	return g_steal_pointer(&amount);
}
