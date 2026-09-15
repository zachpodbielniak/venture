/*
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "venture.h"

GType
venture_journal_state_get_type(void)
{
	static gsize type_id;
	static const GEnumValue values[] = {
		{ VENTURE_JOURNAL_DRAFT, "VENTURE_JOURNAL_DRAFT", "draft" },
		{ VENTURE_JOURNAL_POSTED, "VENTURE_JOURNAL_POSTED", "posted" },
		{ VENTURE_JOURNAL_REVERSED, "VENTURE_JOURNAL_REVERSED", "reversed" },
		{ 0, NULL, NULL }
	};

	if (g_once_init_enter(&type_id))
	{
		GType registered;

		registered = g_enum_register_static("VentureJournalState", values);
		g_once_init_leave(&type_id, registered);
	}
	return (GType)type_id;
}

static const VentureFieldDecl journal_fields[] = {
	VENTURE_FIELD("memo", "Memo", NULL, VENTURE_FIELD_KIND_STRING,
		VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD_ENUM("state", "State", "Corrections are reversing journals",
		venture_journal_state_get_type, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("occurred-at", "Accounting date", NULL,
		VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("currency", "Book currency", "Currency of the balanced book amounts",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("source-type", "Source type", "Registered source record type",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("source-id", "Source ID", NULL,
		VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_REF("reverses-id", "Reverses", "The original posted journal",
		"journal", VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("posted-at", "Posted at", NULL,
		VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("exchange-policy", "Exchange policy",
		"Policy used to value the original amounts in the book currency",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("rule-name", "Posting rule", NULL,
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("source-version", "Source version", NULL,
		VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("posting-key", "Posting key", "Stable organization-scoped batch identity",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_UNIQUE),
	VENTURE_FIELD("tax-book", "Tax book", "TRUE posts tax depreciation that statements ignore",
		VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_INDEXED)
};

VENTURE_DEFINE_ENTITY(VentureJournal, venture_journal, journal_fields)

static const VentureFieldDecl journal_line_fields[] = {
	VENTURE_FIELD_REF("journal-id", "Journal", NULL, "journal",
		VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_REF("account-id", "Account", NULL, "account",
		VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_ENUM("side", "Side", "Debit or credit",
		venture_ledger_side_get_type, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_MONEY("amount", "Original amount", "Nonnegative amount"),
	VENTURE_FIELD_MONEY("book-amount", "Book amount", "Valued by the posting service"),
	VENTURE_FIELD("memo", "Memo", NULL, VENTURE_FIELD_KIND_STRING,
		VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD("dimension", "Dimension", "Optional department, location, project or class",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED)
};

VENTURE_DEFINE_ENTITY(VentureJournalLine, venture_journal_line, journal_line_fields)

static const VentureFieldDecl exchange_rate_fields[] = {
	VENTURE_FIELD_NAME("from-currency", "From", "ISO 4217 code of the original amount"),
	VENTURE_FIELD_NAME("to-currency", "To", "ISO 4217 book currency"),
	VENTURE_FIELD("rate-numerator", "Rate numerator", "Exact multiplier of the original amount",
		VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("rate-denominator", "Rate denominator", "Exact divisor; never invented",
		VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("effective-at", "Effective", "Inclusive start of this rate",
		VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("source", "Source", "manual or a named feed; never guessed",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("reason", "Reason", "Required for a manual override")
};
VENTURE_DEFINE_ENTITY(VentureExchangeRate, venture_exchange_rate, exchange_rate_fields)
