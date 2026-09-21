/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_SALES_SERVICE_H
#define VENTURE_SALES_SERVICE_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
#define VENTURE_TYPE_SALES_SERVICE (venture_sales_service_get_type())
G_DECLARE_FINAL_TYPE(VentureSalesService, venture_sales_service, VENTURE, SALES_SERVICE, GObject)
/**
 * venture_sales_service_get:
 * @database: owning database
 * Returns: (transfer none): database-owned sales lifecycle service
 */
VentureSalesService *venture_sales_service_get(VentureDatabase *database);
/**
 * venture_sales_routing_validate:
 * @database: repository
 * @rule: lead routing rule with optional territory
 * @error: (out) (optional): error location
 * Returns: whether the territory and routing team agree within the organization
 */
gboolean venture_sales_routing_validate(VentureDatabase *database, VentureEntity *rule, GError **error);
/**
 * venture_sales_routing_territory:
 * @database: repository
 * @rule: matched routing rule
 * @lead: proposed lead assignment
 * @eligible: (out): whether an active territory permits this rule
 * @error: (out) (optional): error location
 *
 * Applies territory/team ownership to a matched rule without advancing its
 * existing round-robin cursor. Inactive territories are skipped.
 * Returns: whether evaluation succeeded
 */
gboolean venture_sales_routing_territory(VentureDatabase *database, VentureEntity *rule,
	VentureEntity *lead, gboolean *eligible, GError **error);
/**
 * venture_sales_reports_register:
 * @registry: report registry
 *
 * Registers currency-separated quota attainment beside the current forecast.
 */
void venture_sales_reports_register(VentureReportRegistry *registry);
G_END_DECLS
#endif
