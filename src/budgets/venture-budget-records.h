/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_BUDGET_RECORDS_H
#define VENTURE_BUDGET_RECORDS_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
#define VENTURE_TYPE_BUDGET (venture_budget_get_type())
VENTURE_DECLARE_ENTITY(VentureBudget, venture_budget, BUDGET)
#define VENTURE_TYPE_BUDGET_LINE (venture_budget_line_get_type())
VENTURE_DECLARE_ENTITY(VentureBudgetLine, venture_budget_line, BUDGET_LINE)
G_END_DECLS
#endif
