/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_PAYROLL_SERVICE_H
#define VENTURE_PAYROLL_SERVICE_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
#define VENTURE_TYPE_PAYROLL_SERVICE (venture_payroll_service_get_type())
G_DECLARE_FINAL_TYPE(VenturePayrollService, venture_payroll_service, VENTURE, PAYROLL_SERVICE, GObject)
VenturePayrollService *venture_payroll_service_get(VentureDatabase *database);
VentureEntity *venture_payroll_service_import_json(VenturePayrollService *self, gint64 organization_id,
	JsonObject *payload, const VentureActor *actor, GError **error);
VentureEntity *venture_payroll_service_import_csv(VenturePayrollService *self, gint64 organization_id,
	const gchar *run_key, const gchar *period_start, const gchar *period_end, const gchar *currency,
	const gchar *csv, const VentureActor *actor, GError **error);
gboolean venture_payroll_service_disburse(VenturePayrollService *self, VentureEntity *run,
	const gchar *kind, const VentureActor *actor, GError **error);
gboolean venture_payroll_service_reverse(VenturePayrollService *self, VentureEntity *run,
	const VentureActor *actor, GError **error);
gboolean venture_payroll_save_hook(VentureDatabase *database, VentureEntity *record,
	const VentureActor *actor, gboolean *handled, GError **error);
gboolean venture_payroll_check_write(VentureDatabase *database, VentureEntity *record,
	gboolean removal, GError **error);
void venture_payroll_register_reports(VentureReportRegistry *registry);
G_END_DECLS
#endif
