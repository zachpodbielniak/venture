/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_PAYROLL_RECORDS_H
#define VENTURE_PAYROLL_RECORDS_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
#define VENTURE_TYPE_PAYROLL_RUN (venture_payroll_run_get_type())
VENTURE_DECLARE_ENTITY(VenturePayrollRun, venture_payroll_run, PAYROLL_RUN)
#define VENTURE_TYPE_PAYROLL_LINE (venture_payroll_line_get_type())
VENTURE_DECLARE_ENTITY(VenturePayrollLine, venture_payroll_line, PAYROLL_LINE)
/**
 * venture_payroll_run_new:
 * Returns: (transfer full): an imported pay run
 */
/**
 * venture_payroll_line_new:
 * Returns: (transfer full): one employee's imported pay line
 */
G_END_DECLS
#endif
