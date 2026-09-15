/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_BUDGET_SERVICE_H
#define VENTURE_BUDGET_SERVICE_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
#define VENTURE_TYPE_BUDGET_SERVICE (venture_budget_service_get_type())
G_DECLARE_FINAL_TYPE(VentureBudgetService, venture_budget_service, VENTURE, BUDGET_SERVICE, GObject)
VentureBudgetService *venture_budget_service_get(VentureDatabase *database);
gboolean venture_budget_check_write(VentureDatabase *database, VentureEntity *record,
	gboolean removal, GError **error);
/**
 * venture_budget_service_vs_actual:
 * @self: the service
 * @organization_id: legal entity
 * @period: YYYY or YYYY-MM
 * @dimension: (nullable): restrict lines and ledger actuals
 * @error: (out) (optional)
 *
 * Returns: (transfer full) (nullable)
 */
VentureReportResult *venture_budget_service_vs_actual(VentureBudgetService *self,
	gint64 organization_id, const gchar *period, const gchar *dimension, GError **error);
VentureReportResult *venture_budget_service_cash_forecast(VentureBudgetService *self,
	gint64 organization_id, const gchar *period, GError **error);
void venture_budgets_register_reports(VentureReportRegistry *registry);
G_END_DECLS
#endif
