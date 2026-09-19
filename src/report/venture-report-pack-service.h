/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_REPORT_PACK_SERVICE_H
#define VENTURE_REPORT_PACK_SERVICE_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
#define VENTURE_TYPE_REPORT_PACK_SERVICE (venture_report_pack_service_get_type())
G_DECLARE_FINAL_TYPE(VentureReportPackService, venture_report_pack_service, VENTURE, REPORT_PACK_SERVICE, GObject)
/**
 * venture_report_pack_service_get:
 * @database: the owning database
 *
 * Returns: (transfer none): the per-database service
 */
VentureReportPackService *venture_report_pack_service_get(VentureDatabase *database);
/**
 * venture_report_pack_service_save:
 * @self: the service or registry instance
 * @organization_id: target legal entity ID
 * @name: name or registry key
 * @report_name: registered report name
 * @period: reporting period
 * @options: report or field options
 * @dimension: optional accounting dimension
 * @actor: (nullable): audit actor; NULL for internal service work
 * @error: (out) (optional): return location for an error
 *
 * Returns: (transfer full) (nullable): owned result
 */
VentureEntity *venture_report_pack_service_save(VentureReportPackService *self, gint64 organization_id,
	const gchar *name, const gchar *report_name, const gchar *period, JsonObject *options,
	const gchar *dimension, const VentureActor *actor, GError **error);
/**
 * venture_report_pack_service_run:
 * @self: the service or registry instance
 * @context: application context
 * @saved: saved report definition
 * @error: (out) (optional): return location for an error
 *
 * Returns: (transfer full) (nullable): owned result
 */
VentureReportResult *venture_report_pack_service_run(VentureReportPackService *self, VentureContext *context,
	VentureSavedReport *saved, GError **error);
/**
 * venture_report_pack_service_schedule:
 * @self: the service or registry instance
 * @organization_id: target legal entity ID
 * @name: name or registry key
 * @schedule: schedule record or expression
 * @saved_report_id: saved report id
 * @actor: (nullable): audit actor; NULL for internal service work
 * @error: (out) (optional): return location for an error
 *
 * Returns: (transfer full) (nullable): owned result
 */
VentureEntity *venture_report_pack_service_schedule(VentureReportPackService *self, gint64 organization_id,
	const gchar *name, const gchar *schedule, gint64 saved_report_id, const VentureActor *actor, GError **error);
/**
 * venture_report_pack_service_run_pack:
 * @self: the service
 * @context: report context
 * @pack: a scheduled pack
 * @error: (out) (optional)
 *
 * Returns: (transfer full) (element-type VentureReportResult) (nullable)
 */
GPtrArray *venture_report_pack_service_run_pack(VentureReportPackService *self, VentureContext *context,
	VentureReportPack *pack, GError **error);
/**
 * venture_report_pack_service_run_due:
 * @self: the service
 * @context: report context
 * @organization_id: legal entity whose packs to dispatch
 * @as_of: (nullable): now when omitted
 * @actor: (nullable)
 * @error: (out) (optional)
 *
 * Dispatches packs whose cron-like schedule is due and stamps last-run-at.
 *
 * Returns: number of packs run, or -1
 */
gint venture_report_pack_service_run_due(VentureReportPackService *self, VentureContext *context,
	gint64 organization_id, GDateTime *as_of, const VentureActor *actor, GError **error);
/**
 * venture_report_pack_schedule_validate:
 * @schedule: =daily= or five numeric/=*= cron fields
 * @error: (out) (optional)
 *
 * Returns: TRUE when the expression is one a sweep can evaluate
 */
gboolean venture_report_pack_schedule_validate(const gchar *schedule, GError **error);
/**
 * venture_report_pack_schedule_due:
 * @schedule: (nullable): the expression; empty means never
 * @last: (nullable): when the schedule last ran
 * @as_of: the sweep time, UTC
 * @error: (out) (optional)
 *
 * The same catch-up-once rule scheduled report packs use.
 *
 * Returns: 1 when due, 0 when not, -1 on an invalid expression
 */
gint venture_report_pack_schedule_due(const gchar *schedule, GDateTime *last, GDateTime *as_of, GError **error);
G_END_DECLS
#endif
