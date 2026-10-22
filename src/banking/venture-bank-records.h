/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_BANK_RECORDS_H
#define VENTURE_BANK_RECORDS_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
/**
 * VentureBankAccountKind:
 * @VENTURE_BANK_ACCOUNT_UNSPECIFIED: existing account without a chosen kind
 * @VENTURE_BANK_ACCOUNT_CHECKING: checking account
 * @VENTURE_BANK_ACCOUNT_SAVINGS: savings account
 * @VENTURE_BANK_ACCOUNT_CARD: credit card liability
 * @VENTURE_BANK_ACCOUNT_CASH: cash wallet
 * @VENTURE_BANK_ACCOUNT_LOAN: loan liability
 * @VENTURE_BANK_ACCOUNT_INVESTMENT: investment account
 *
 * User-facing account purpose, separate from its linked ledger class.
 */
typedef enum
{
	VENTURE_BANK_ACCOUNT_UNSPECIFIED = 0,
	VENTURE_BANK_ACCOUNT_CHECKING,
	VENTURE_BANK_ACCOUNT_SAVINGS,
	VENTURE_BANK_ACCOUNT_CARD,
	VENTURE_BANK_ACCOUNT_CASH,
	VENTURE_BANK_ACCOUNT_LOAN,
	VENTURE_BANK_ACCOUNT_INVESTMENT
} VentureBankAccountKind;

/**
 * venture_bank_account_kind_get_type:
 *
 * Returns: the registered #VentureBankAccountKind enumeration type
 */
GType venture_bank_account_kind_get_type(void) G_GNUC_CONST;

#define VENTURE_TYPE_BANK_ACCOUNT (venture_bank_account_get_type())
VENTURE_DECLARE_ENTITY(VentureBankAccount, venture_bank_account, BANK_ACCOUNT)
#define VENTURE_TYPE_BANK_STATEMENT (venture_bank_statement_get_type())
VENTURE_DECLARE_ENTITY(VentureBankStatement, venture_bank_statement, BANK_STATEMENT)
#define VENTURE_TYPE_BANK_TRANSACTION (venture_bank_transaction_get_type())
VENTURE_DECLARE_ENTITY(VentureBankTransaction, venture_bank_transaction, BANK_TRANSACTION)
#define VENTURE_TYPE_BANK_MATCH (venture_bank_match_get_type())
VENTURE_DECLARE_ENTITY(VentureBankMatch, venture_bank_match, BANK_MATCH)
#define VENTURE_TYPE_RECONCILIATION (venture_reconciliation_get_type())
VENTURE_DECLARE_ENTITY(VentureReconciliation, venture_reconciliation, RECONCILIATION)
#define VENTURE_TYPE_BANK_RULE (venture_bank_rule_get_type())
VENTURE_DECLARE_ENTITY(VentureBankRule, venture_bank_rule, BANK_RULE)
#define VENTURE_TYPE_BANK_TRANSFER (venture_bank_transfer_get_type())
VENTURE_DECLARE_ENTITY(VentureBankTransfer, venture_bank_transfer, BANK_TRANSFER)

#define VENTURE_TYPE_HOUSEHOLD_SHARE (venture_household_share_get_type())
VENTURE_DECLARE_ENTITY(VentureHouseholdShare, venture_household_share, HOUSEHOLD_SHARE)
G_END_DECLS
#endif
