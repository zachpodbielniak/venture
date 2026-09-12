/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_PERIOD_REPORT_H
#define VENTURE_PERIOD_REPORT_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
/**
 * venture_period_report_as_of:
 * @options: (nullable): report options, including optional as_of
 * @error: (out) (optional): invalid option details
 * Returns: (transfer full) (nullable): the cutoff; date-only values mean end of that UTC day
 */
GDateTime *venture_period_report_as_of(JsonObject *options, GError **error);
/**
 * venture_period_report_scope:
 * @query: a report query
 * @options: (nullable): the report options
 * @error: (out) (optional): invalid cutoff details
 * Returns: %TRUE if the visibility cutoff was applied
 */
gboolean venture_period_report_scope(VentureQuery *query, JsonObject *options, GError **error);
/**
 * venture_period_report_invoice_outstanding:
 * @database: the repository
 * @invoice: the invoice
 * @as_of: (nullable): the historical cutoff
 * @outstanding: (out): whether it was issued and unsettled at the cutoff
 * @error: (out) (optional): reconstruction failure
 * Returns: %TRUE on successful reconstruction
 */
gboolean venture_period_report_invoice_outstanding(VentureDatabase *database, VentureEntity *invoice,
	GDateTime *as_of, gboolean *outstanding, GError **error);
/**
 * venture_period_reports_register:
 * @registry: the report registry
 *
 * Marks financial builtins for snapshots and registers snapshot_vs_live.
 */
void venture_period_reports_register(VentureReportRegistry *registry);
/**
 * venture_period_report_snapshots:
 * @context: the reporting context
 * @period: the period, with close stamps already set
 * @error: (out) (optional): a report failure
 * Returns: (transfer full) (element-type VentureReportSnapshot) (nullable):
 *   unsaved immutable evidence, to save within the closing transaction
 */
GPtrArray *venture_period_report_snapshots(VentureContext *context, VentureEntity *period, GError **error);
G_END_DECLS
#endif
