/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_GROUP_SERVICE_H
#define VENTURE_GROUP_SERVICE_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
#define VENTURE_TYPE_GROUP_SERVICE (venture_group_service_get_type())
G_DECLARE_FINAL_TYPE(VentureGroupService, venture_group_service, VENTURE, GROUP_SERVICE, GObject)
/**
 * venture_group_service_get:
 * @database: database owning the records
 *
 * Returns the per-database service. The database owns this reference.
 *
 * Returns: (transfer none): borrowed result
 */
VentureGroupService *venture_group_service_get(VentureDatabase *database);
/**
 * venture_group_check_write:
 * @database: database owning the records
 * @record: candidate record
 * @removal: whether this is a removal operation
 * @error: (out) (optional): return location for an error
 *
 * Checks service ownership and lifecycle restrictions before a generic write.
 *
 * Returns: TRUE on success, FALSE on failure
 */
gboolean venture_group_check_write(VentureDatabase *database, VentureEntity *record,
	gboolean removal, GError **error);
/**
 * venture_group_service_eliminate:
 * @self: the service or registry instance
 * @parent_id: parent id
 * @contra_id: contra id
 * @debit_account_id: debit account id
 * @credit_account_id: credit account id
 * @amount: amount with an explicit currency
 * @period: reporting period
 * @memo: operator memo
 * @actor: (nullable): audit actor; NULL for internal service work
 * @error: (out) (optional): return location for an error
 *
 * Returns: (transfer full) (nullable): owned result
 */
VentureEntity *venture_group_service_eliminate(VentureGroupService *self, gint64 parent_id,
	gint64 contra_id, gint64 debit_account_id, gint64 credit_account_id, const VentureMoney *amount,
	const gchar *period, const gchar *memo, const VentureActor *actor, GError **error);
/**
 * venture_group_service_consolidated:
 * @self: the service or registry instance
 * @parent_id: parent id
 * @report_name: registered report name
 * @period: reporting period
 * @currency: ISO currency code
 * @error: (out) (optional): return location for an error
 *
 * Returns: (transfer full) (nullable): owned result
 */
VentureReportResult *venture_group_service_consolidated(VentureGroupService *self, gint64 parent_id,
	const gchar *report_name, VentureDateRange *period, const gchar *currency, GError **error);
/**
 * venture_group_register_reports:
 * @registry: registry receiving the registrations
 */
void venture_group_register_reports(VentureReportRegistry *registry);
G_END_DECLS
#endif
