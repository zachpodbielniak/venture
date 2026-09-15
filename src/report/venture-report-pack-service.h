/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_REPORT_PACK_SERVICE_H
#define VENTURE_REPORT_PACK_SERVICE_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
#define VENTURE_TYPE_REPORT_PACK_SERVICE (venture_report_pack_service_get_type())
G_DECLARE_FINAL_TYPE(VentureReportPackService, venture_report_pack_service, VENTURE, REPORT_PACK_SERVICE, GObject)
VentureReportPackService *venture_report_pack_service_get(VentureDatabase *database);
VentureEntity *venture_report_pack_service_save(VentureReportPackService *self, gint64 organization_id,
	const gchar *name, const gchar *report_name, const gchar *period, JsonObject *options,
	const gchar *dimension, const VentureActor *actor, GError **error);
VentureReportResult *venture_report_pack_service_run(VentureReportPackService *self, VentureContext *context,
	VentureSavedReport *saved, GError **error);
VentureEntity *venture_report_pack_service_schedule(VentureReportPackService *self, gint64 organization_id,
	const gchar *name, const gchar *schedule, gint64 saved_report_id, const VentureActor *actor, GError **error);
GPtrArray *venture_report_pack_service_run_pack(VentureReportPackService *self, VentureContext *context,
	VentureReportPack *pack, GError **error);
G_END_DECLS
#endif
