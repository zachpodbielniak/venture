/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_SALES_RECORDS_H
#define VENTURE_SALES_RECORDS_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
#define VENTURE_TYPE_SALES_TERRITORY (venture_sales_territory_get_type())
G_DECLARE_FINAL_TYPE(VentureSalesTerritory, venture_sales_territory, VENTURE, SALES_TERRITORY, VentureEntity)
/**
 * venture_sales_territory_new:
 * Returns: (transfer full): a new metadata-backed sales record
 */
VentureSalesTerritory *venture_sales_territory_new(void);
#define VENTURE_TYPE_SALES_QUOTA (venture_sales_quota_get_type())
G_DECLARE_FINAL_TYPE(VentureSalesQuota, venture_sales_quota, VENTURE, SALES_QUOTA, VentureEntity)
/**
 * venture_sales_quota_new:
 * Returns: (transfer full): a new metadata-backed sales record
 */
VentureSalesQuota *venture_sales_quota_new(void);
#define VENTURE_TYPE_SALES_CREDIT (venture_sales_credit_get_type())
G_DECLARE_FINAL_TYPE(VentureSalesCredit, venture_sales_credit, VENTURE, SALES_CREDIT, VentureEntity)
/**
 * venture_sales_credit_new:
 * Returns: (transfer full): a new metadata-backed sales record
 */
VentureSalesCredit *venture_sales_credit_new(void);
#define VENTURE_TYPE_SALES_ASSIGNMENT (venture_sales_assignment_get_type())
G_DECLARE_FINAL_TYPE(VentureSalesAssignment, venture_sales_assignment, VENTURE, SALES_ASSIGNMENT, VentureEntity)
/**
 * venture_sales_assignment_new:
 * Returns: (transfer full): a new metadata-backed sales record
 */
VentureSalesAssignment *venture_sales_assignment_new(void);
G_END_DECLS
#endif
