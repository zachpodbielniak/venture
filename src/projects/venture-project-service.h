/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_PROJECT_SERVICE_H
#define VENTURE_PROJECT_SERVICE_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
#define VENTURE_TYPE_PROJECT_SERVICE (venture_project_service_get_type())
G_DECLARE_FINAL_TYPE(VentureProjectService, venture_project_service, VENTURE, PROJECT_SERVICE, GObject)
VentureProjectService *venture_project_service_get(VentureDatabase *database);
gboolean venture_project_service_save(VentureProjectService *self, VentureEntity *record, const VentureActor *actor, GError **error);
gboolean venture_project_service_approve_time(VentureProjectService *self, VentureEntity *time, const VentureActor *actor, GError **error);
VentureEntity *venture_project_service_bill(VentureProjectService *self, gint64 project_id, GDateTime *date, const VentureActor *actor, GError **error);
gboolean venture_projects_save_hook(VentureDatabase *database, VentureEntity *record, const VentureActor *actor, gboolean *handled, GError **error);
void venture_projects_register_reports(VentureReportRegistry *registry);
G_END_DECLS
#endif
