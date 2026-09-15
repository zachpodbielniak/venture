/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

static const VentureFieldDecl link_fields[] = {
	VENTURE_FIELD_REF("child-organization-id", "Subsidiary", NULL, "organization",
		VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("ownership-numerator", "Ownership numerator", "Exact share of the subsidiary",
		VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("ownership-denominator", "Ownership denominator", "Exact divisor; never invented",
		VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VentureIntercompanyLink, venture_intercompany_link, link_fields)

static const VentureFieldDecl elimination_fields[] = {
	VENTURE_FIELD_REF("contra-organization-id", "Counterparty", NULL, "organization",
		VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("debit-account-id", "Debit account", NULL, "account", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_REF("credit-account-id", "Credit account", NULL, "account", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_MONEY("amount", "Amount", "Elimination in the parent book currency"),
	VENTURE_FIELD("period", "Period", "YYYY-MM the elimination applies to",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("memo", "Memo", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_SEARCHABLE)
};
VENTURE_DEFINE_ENTITY(VentureElimination, venture_elimination, elimination_fields)
