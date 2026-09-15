/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_CLAIM_RECORDS_H
#define VENTURE_CLAIM_RECORDS_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

G_BEGIN_DECLS

#define VENTURE_TYPE_EXPENSE_CLAIM (venture_expense_claim_get_type())
VENTURE_DECLARE_ENTITY(VentureExpenseClaim, venture_expense_claim, EXPENSE_CLAIM)
#define VENTURE_TYPE_EXPENSE_CLAIM_LINE (venture_expense_claim_line_get_type())
VENTURE_DECLARE_ENTITY(VentureExpenseClaimLine, venture_expense_claim_line, EXPENSE_CLAIM_LINE)

/**
 * venture_expense_claim_new:
 *
 * Returns: (transfer full): a draft employee expense claim
 */
/**
 * venture_expense_claim_line_new:
 *
 * Returns: (transfer full): a receipt or mileage line
 */
/**
 * venture_expense_claim_line_get_amount:
 * @self: the line
 * @error: (out) (optional): invalid miles, rate, currency or overflow
 *
 * Mileage is rate times miles, rounded half to even. Receipt lines use
 * their stored amount.
 *
 * Returns: (transfer full) (nullable): the line total
 */
VentureMoney *venture_expense_claim_line_get_amount(VentureExpenseClaimLine *self, GError **error);

G_END_DECLS
#endif
