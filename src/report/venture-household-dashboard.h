/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_HOUSEHOLD_DASHBOARD_H
#define VENTURE_HOUSEHOLD_DASHBOARD_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
/**
 * venture_household_dashboard_register_reports:
 * @registry: the report registry
 *
 * Registers =household_spending= (spending by category) and
 * =household_cash_forecast= (spendable balance week by week through what
 * is due), the figures the household dashboard template charts.
 */
void venture_household_dashboard_register_reports(VentureReportRegistry *registry);
G_END_DECLS
#endif
