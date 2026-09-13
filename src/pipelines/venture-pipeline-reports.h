/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_PIPELINE_REPORTS_H
#define VENTURE_PIPELINE_REPORTS_H
/**
 * venture_pipeline_reports_register:
 * @registry: report registry
 *
 * Registers stage history, forecasting, losses and overdue counts.
 */
void venture_pipeline_reports_register(VentureReportRegistry *registry);
#endif
