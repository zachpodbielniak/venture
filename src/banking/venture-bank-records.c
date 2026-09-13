/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

static const VentureFieldDecl bank_account_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", NULL),
	VENTURE_FIELD_REF("account-id", "Account id", NULL, "account", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("currency", "Currency", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_MONEY("last-statement-balance", "Last statement balance", NULL),
	VENTURE_FIELD("last-statement-date", "Last statement date", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("date-column", "Date column", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("amount-column", "Amount column", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("description-column", "Description column", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("reference-column", "Reference column", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("external-id-column", "External id column", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("date-format", "Date format", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("sign-convention", "Sign convention", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
};
VENTURE_DEFINE_ENTITY(VentureBankAccount, venture_bank_account, bank_account_fields)

static const VentureFieldDecl bank_statement_fields[] = {
	VENTURE_FIELD_REF("bank-account-id", "Bank account id", NULL, "bank_account", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("period-start", "Period start", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("period-end", "Period end", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_MONEY("opening-balance", "Opening balance", NULL),
	VENTURE_FIELD_MONEY("closing-balance", "Closing balance", NULL),
	VENTURE_FIELD("source-file-hash", "Source file hash", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("imported-at", "Imported at", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
};
VENTURE_DEFINE_ENTITY(VentureBankStatement, venture_bank_statement, bank_statement_fields)

static const VentureFieldDecl bank_transaction_fields[] = {
	VENTURE_FIELD_REF("statement-id", "Statement id", NULL, "bank_statement", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("bank-account-id", "Bank account id", NULL, "bank_account", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("date", "Date", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_MONEY("amount", "Amount", NULL),
	VENTURE_FIELD_NAME("description", "Description", NULL),
	VENTURE_FIELD("reference", "Reference", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("external-id", "External id", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("external-key", "External key", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_UNIQUE),
	VENTURE_FIELD("state", "State", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("match-id", "Match id", NULL, "bank_match", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("exclusion-reason", "Exclusion reason", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
};
VENTURE_DEFINE_ENTITY(VentureBankTransaction, venture_bank_transaction, bank_transaction_fields)

static const VentureFieldDecl bank_match_fields[] = {
	VENTURE_FIELD_REF("transaction-id", "Transaction id", NULL, "bank_transaction", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("record-type", "Record type", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("record-id", "Record id", NULL, VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_MONEY("amount", "Amount", NULL),
	VENTURE_FIELD("kind", "Kind", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("created-by", "Created by", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
};
VENTURE_DEFINE_ENTITY(VentureBankMatch, venture_bank_match, bank_match_fields)

static const VentureFieldDecl reconciliation_fields[] = {
	VENTURE_FIELD_REF("bank-account-id", "Bank account id", NULL, "bank_account", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("period-end", "Period end", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_MONEY("statement-balance", "Statement balance", NULL),
	VENTURE_FIELD_MONEY("book-balance", "Book balance", NULL),
	VENTURE_FIELD_MONEY("difference", "Difference", NULL),
	VENTURE_FIELD("state", "State", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("reconciled-by", "Reconciled by", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("reconciled-at", "Reconciled at", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
};
VENTURE_DEFINE_ENTITY(VentureReconciliation, venture_reconciliation, reconciliation_fields)
