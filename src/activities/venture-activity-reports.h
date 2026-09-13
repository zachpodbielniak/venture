/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_ACTIVITY_REPORTS_H
#define VENTURE_ACTIVITY_REPORTS_H
/**
 * venture_activity_register_reports:
 * @registry: report registry
 *
 * Registers the per-owner worklist and its overdue/today metrics.
 */
void venture_activity_register_reports(VentureReportRegistry *registry);
#endif
