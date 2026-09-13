/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_BANK_RECORDS_H
#define VENTURE_BANK_RECORDS_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
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
G_END_DECLS
#endif
