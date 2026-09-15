/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_GROUP_SERVICE_H
#define VENTURE_GROUP_SERVICE_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
#define VENTURE_TYPE_GROUP_SERVICE (venture_group_service_get_type())
G_DECLARE_FINAL_TYPE(VentureGroupService, venture_group_service, VENTURE, GROUP_SERVICE, GObject)
VentureGroupService *venture_group_service_get(VentureDatabase *database);
gboolean venture_group_check_write(VentureDatabase *database, VentureEntity *record,
	gboolean removal, GError **error);
VentureEntity *venture_group_service_eliminate(VentureGroupService *self, gint64 parent_id,
	gint64 contra_id, gint64 debit_account_id, gint64 credit_account_id, const VentureMoney *amount,
	const gchar *period, const gchar *memo, const VentureActor *actor, GError **error);
VentureReportResult *venture_group_service_consolidated(VentureGroupService *self, gint64 parent_id,
	const gchar *report_name, VentureDateRange *period, const gchar *currency, GError **error);
void venture_group_register_reports(VentureReportRegistry *registry);
G_END_DECLS
#endif
