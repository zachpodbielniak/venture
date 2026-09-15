/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

GType
venture_equity_kind_get_type(void)
{
	static gsize type_id = 0;
	if (g_once_init_enter(&type_id))
	{
		static const GEnumValue values[] = {
			{ VENTURE_EQUITY_KIND_CONTRIBUTION, "VENTURE_EQUITY_KIND_CONTRIBUTION", "contribution" },
			{ VENTURE_EQUITY_KIND_DRAW, "VENTURE_EQUITY_KIND_DRAW", "draw" },
			{ VENTURE_EQUITY_KIND_LOAN_PROCEED, "VENTURE_EQUITY_KIND_LOAN_PROCEED", "loan_proceed" },
			{ VENTURE_EQUITY_KIND_LOAN_PAYMENT, "VENTURE_EQUITY_KIND_LOAN_PAYMENT", "loan_payment" },
			{ VENTURE_EQUITY_KIND_TRANSFER, "VENTURE_EQUITY_KIND_TRANSFER", "transfer" },
			{ 0, NULL, NULL }
		};
		g_once_init_leave(&type_id, g_enum_register_static("VentureEquityKind", values));
	}
	return (GType)type_id;
}

static const VentureFieldDecl equity_fields[] = {
	VENTURE_FIELD_ENUM("kind", "Kind", NULL, venture_equity_kind_get_type, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_MONEY("amount", "Amount", NULL),
	VENTURE_FIELD("occurred-at", "Date", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("memo", "Memo", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD_REF("debit-account-id", "Debit account", "Override mapped debit", "account", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("credit-account-id", "Credit account", "Override mapped credit", "account", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("journal-id", "Journal", NULL, "journal", VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VentureEquityTransaction, venture_equity_transaction, equity_fields)
