/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_EQUITY_RECORDS_H
#define VENTURE_EQUITY_RECORDS_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
/**
 * VentureEquityKind:
 * @VENTURE_EQUITY_KIND_CONTRIBUTION: owner contribution
 * @VENTURE_EQUITY_KIND_DRAW: owner draw
 * @VENTURE_EQUITY_KIND_LOAN_PROCEED: loan proceeds
 * @VENTURE_EQUITY_KIND_LOAN_PAYMENT: loan payment
 * @VENTURE_EQUITY_KIND_TRANSFER: cash transfer
 */
typedef enum {
	VENTURE_EQUITY_KIND_CONTRIBUTION,
	VENTURE_EQUITY_KIND_DRAW,
	VENTURE_EQUITY_KIND_LOAN_PROCEED,
	VENTURE_EQUITY_KIND_LOAN_PAYMENT,
	VENTURE_EQUITY_KIND_TRANSFER
} VentureEquityKind;
GType venture_equity_kind_get_type(void) G_GNUC_CONST;
#define VENTURE_TYPE_EQUITY_KIND (venture_equity_kind_get_type())
#define VENTURE_TYPE_EQUITY_TRANSACTION (venture_equity_transaction_get_type())
VENTURE_DECLARE_ENTITY(VentureEquityTransaction, venture_equity_transaction, EQUITY_TRANSACTION)
G_END_DECLS
#endif
