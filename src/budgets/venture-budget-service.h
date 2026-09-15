/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_BUDGET_SERVICE_H
#define VENTURE_BUDGET_SERVICE_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
#define VENTURE_TYPE_BUDGET_SERVICE (venture_budget_service_get_type())
G_DECLARE_FINAL_TYPE(VentureBudgetService, venture_budget_service, VENTURE, BUDGET_SERVICE, GObject)
/**
 * venture_budget_service_get:
 * @database: database owning the records
 *
 * Returns the per-database service. The database owns this reference.
 *
 * Returns: (transfer none): borrowed result
 */
VentureBudgetService *venture_budget_service_get(VentureDatabase *database);
/**
 * venture_budget_check_write:
 * @database: database owning the records
 * @record: candidate record
 * @removal: whether this is a removal operation
 * @error: (out) (optional): return location for an error
 *
 * Checks service ownership and lifecycle restrictions before a generic write.
 *
 * Returns: TRUE on success, FALSE on failure
 */
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
/**
 * venture_budget_service_cash_forecast:
 * @self: the service or registry instance
 * @organization_id: target legal entity ID
 * @period: reporting period
 * @error: (out) (optional): return location for an error
 *
 * Returns: (transfer full) (nullable): owned result
 */
VentureReportResult *venture_budget_service_cash_forecast(VentureBudgetService *self,
	gint64 organization_id, const gchar *period, GError **error);
/**
 * venture_budgets_register_reports:
 * @registry: registry receiving the registrations
 */
void venture_budgets_register_reports(VentureReportRegistry *registry);
G_END_DECLS
#endif
