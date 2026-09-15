/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_LEDGER_BALANCES_H
#define VENTURE_LEDGER_BALANCES_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
#define VENTURE_TYPE_LEDGER_BALANCES (venture_ledger_balances_get_type())
G_DECLARE_FINAL_TYPE(VentureLedgerBalances, venture_ledger_balances, VENTURE, LEDGER_BALANCES, GObject)
/**
 * venture_ledger_balances_new:
 * @database: the repository
 * Returns: (transfer full): a posted-ledger query object
 */
VentureLedgerBalances *venture_ledger_balances_new(VentureDatabase *database);
/**
 * venture_ledger_balances_set_dimension:
 * @self: the query object
 * @dimension: (nullable): journal-line dimension, or %NULL for all
 *
 * Restricts subsequent queries to lines tagged with @dimension.
 */
void venture_ledger_balances_set_dimension(VentureLedgerBalances *self, const gchar *dimension);
/**
 * venture_ledger_balances_query:
 * @self: the query object
 * @organization_id: one exact legal entity
 * @currency: (nullable): book currency, or all currencies separately
 * @period: the inclusive-start, exclusive-end movement range
 * @as_of: (nullable): inclusive accounting cutoff, capped at period end
 * @rollup: include descendants in each parent row
 * @error: (out) (optional): invalid scope, hierarchy, or arithmetic error
 *
 * Opening and closing are debit minus credit. Movements are unsigned debits
 * and credits. Mutable account opening balances and draft journals are not
 * evidence. Reversed originals remain evidence at their original date.
 *
 * Returns: (transfer full) (nullable): account rows with exact monetary columns
 */
VentureReportResult *venture_ledger_balances_query(VentureLedgerBalances *self,
	gint64 organization_id, const gchar *currency, VentureDateRange *period,
	GDateTime *as_of, gboolean rollup, GError **error);
/**
 * venture_statements_register_reports:
 * @registry: the report registry
 *
 * Registers financial statements and source-document P&L reconciliation.
 */
void venture_statements_register_reports(VentureReportRegistry *registry);
G_END_DECLS
#endif
