/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_BILLING_REPORTS_H
#define VENTURE_BILLING_REPORTS_H
/**
 * venture_billing_register_reports:
 * @registry: the report registry
 *
 * Registers recurring revenue, cohort churn and upcoming renewals.
 */
void venture_billing_register_reports(VentureReportRegistry *registry);
#endif
