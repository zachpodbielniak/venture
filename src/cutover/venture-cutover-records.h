/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_CUTOVER_RECORDS_H
#define VENTURE_CUTOVER_RECORDS_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
#define VENTURE_TYPE_ACCOUNTING_CUTOVER (venture_accounting_cutover_get_type())
VENTURE_DECLARE_ENTITY(VentureAccountingCutover, venture_accounting_cutover, ACCOUNTING_CUTOVER)
#define VENTURE_TYPE_ACCOUNTING_CUTOVER_ROW (venture_accounting_cutover_row_get_type())
VENTURE_DECLARE_ENTITY(VentureAccountingCutoverRow, venture_accounting_cutover_row, ACCOUNTING_CUTOVER_ROW)
G_END_DECLS
#endif
