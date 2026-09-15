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
	VENTURE_FIELD("debit-column", "Debit column", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("credit-column", "Credit column", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("locale", "Locale", "CSV decimal and date locale, for example en_US or de_DE",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("last-inbox", "Last inbox", "Latest review suggestions; service-owned",
		VENTURE_FIELD_KIND_JSON, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("last-mapping-preview", "Last mapping preview", "Latest CSV mapping preview; service-owned",
		VENTURE_FIELD_KIND_JSON, VENTURE_COLUMN_FLAG_NONE),
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
	VENTURE_FIELD("review-confidence", "Review confidence", "0-100 suggestion strength",
		VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("review-explanation", "Review explanation", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("suggested-rule-id", "Suggested rule", NULL, "bank_rule", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("suggested-action", "Suggested action", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
};
VENTURE_DEFINE_ENTITY(VentureBankTransaction, venture_bank_transaction, bank_transaction_fields)

static const VentureFieldDecl bank_match_fields[] = {
	VENTURE_FIELD_REF("transaction-id", "Transaction id", NULL, "bank_transaction", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("record-type", "Record type", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("record-id", "Record id", NULL, VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_MONEY("amount", "Amount", NULL),
	VENTURE_FIELD("kind", "Kind", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("created-by", "Created by", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("cleared-at", "Cleared at", "Bank statement date, independent of the book accounting date",
		VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE)
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
	VENTURE_FIELD_MONEY("outstanding-checks", "Outstanding checks", "Book credits not cleared by the statement cutoff"),
	VENTURE_FIELD_MONEY("deposits-in-transit", "Deposits in transit", "Book debits not cleared by the statement cutoff"),
	VENTURE_FIELD_TEXT("outstanding-items", "Outstanding items", "JSON evidence of uncleared book movements"),
	VENTURE_FIELD("reopened-by", "Reopened by", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("reopened-at", "Reopened at", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("reopen-reason", "Reopen reason", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VentureReconciliation, venture_reconciliation, reconciliation_fields)

static const VentureFieldDecl bank_rule_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", NULL),
	VENTURE_FIELD("priority", "Priority", "Lower numbers win; equal priority that both match is ambiguous",
		VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("enabled", "Enabled", "Requires a historical preview before turning on",
		VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("merchant", "Merchant", "Case-insensitive substring of the statement description",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("description-contains", "Description contains", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_MONEY("amount-min", "Amount min", "Inclusive signed amount bound; unset is unbounded"),
	VENTURE_FIELD_MONEY("amount-max", "Amount max", "Inclusive signed amount bound; unset is unbounded"),
	VENTURE_FIELD_REF("bank-account-id", "Bank account", "Empty means every account in the organization",
		"bank_account", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("account-id", "Ledger account", "Expense or income account for categorize",
		"account", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("action", "Action", "categorize, match, split or transfer",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("category", "Category", "Expense category code when categorizing",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("create-type", "Create type", "expense or receipt", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("splits", "Splits", "JSON array of category and amount or ratio parts",
		VENTURE_FIELD_KIND_JSON, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("last-preview", "Last preview", "Historical sample matches; service-owned",
		VENTURE_FIELD_KIND_JSON, VENTURE_COLUMN_FLAG_NONE),
};
VENTURE_DEFINE_ENTITY(VentureBankRule, venture_bank_rule, bank_rule_fields)

static const VentureFieldDecl bank_transfer_fields[] = {
	VENTURE_FIELD_REF("from-bank-account-id", "From bank", NULL, "bank_account", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("to-bank-account-id", "To bank", NULL, "bank_account", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_MONEY("amount", "Amount", "Gross movement at the source, excluding optional fee"),
	VENTURE_FIELD_MONEY("fee", "Fee", "Optional source-side fee; never income on the destination"),
	VENTURE_FIELD("date", "Date", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("cleared-at", "Cleared at", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("memo", "Memo", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("transfer-key", "Transfer key", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_UNIQUE),
	VENTURE_FIELD("state", "State", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
};
VENTURE_DEFINE_ENTITY(VentureBankTransfer, venture_bank_transfer, bank_transfer_fields)
