/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_PROGRESS_SERVICE_H
#define VENTURE_PROGRESS_SERVICE_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
#define VENTURE_TYPE_PROGRESS_SERVICE (venture_progress_service_get_type())
G_DECLARE_FINAL_TYPE(VentureProgressService, venture_progress_service, VENTURE, PROGRESS_SERVICE, GObject)
/**
 * venture_progress_service_get:
 * @database: database owning the records
 *
 * Returns the per-database service. The database owns this reference.
 *
 * Returns: (transfer none): borrowed result
 */
VentureProgressService *venture_progress_service_get(VentureDatabase *database);
/**
 * venture_progress_check_write:
 * @database: database owning the records
 * @record: candidate record
 * @removal: whether this is a removal operation
 * @error: (out) (optional): return location for an error
 *
 * Checks service ownership and lifecycle restrictions before a generic write.
 *
 * Returns: TRUE on success, FALSE on failure
 */
gboolean venture_progress_check_write(VentureDatabase *database, VentureEntity *record,
	gboolean removal, GError **error);
/**
 * venture_progress_service_remaining:
 * @self: the service or registry instance
 * @quote: source quote
 * @error: (out) (optional): return location for an error
 *
 * Returns: (transfer full) (nullable): owned result
 */
VentureMoney *venture_progress_service_remaining(VentureProgressService *self,
	VentureQuote *quote, GError **error);
/**
 * venture_progress_service_invoice:
 * @self: the service or registry instance
 * @quote: source quote
 * @percent: percentage of the quote to invoice
 * @amount: amount with an explicit currency
 * @actor: (nullable): audit actor; NULL for internal service work
 * @error: (out) (optional): return location for an error
 *
 * Returns: (transfer full) (nullable): owned result
 */
VentureEntity *venture_progress_service_invoice(VentureProgressService *self,
	VentureQuote *quote, gint64 percent, const VentureMoney *amount,
	const VentureActor *actor, GError **error);
/**
 * venture_progress_service_collect_retainer:
 * @self: the service or registry instance
 * @organization_id: target legal entity ID
 * @company_id: company id
 * @liability_account_id: liability account id
 * @amount: amount with an explicit currency
 * @actor: (nullable): audit actor; NULL for internal service work
 * @error: (out) (optional): return location for an error
 *
 * Returns: (transfer full) (nullable): owned result
 */
VentureEntity *venture_progress_service_collect_retainer(VentureProgressService *self,
	gint64 organization_id, gint64 company_id, gint64 liability_account_id,
	const VentureMoney *amount, const VentureActor *actor, GError **error);
/**
 * venture_progress_service_release_retainer:
 * @self: the service or registry instance
 * @retainer: customer retainer
 * @amount: amount with an explicit currency
 * @actor: (nullable): audit actor; NULL for internal service work
 * @error: (out) (optional): return location for an error
 *
 * Returns: TRUE on success, FALSE on failure
 */
gboolean venture_progress_service_release_retainer(VentureProgressService *self,
	VentureCustomerRetainer *retainer, const VentureMoney *amount,
	const VentureActor *actor, GError **error);
/**
 * venture_progress_service_hold_retention:
 * @self: the service or registry instance
 * @quote: source quote
 * @liability_account_id: liability account id
 * @amount: amount with an explicit currency
 * @actor: (nullable): audit actor; NULL for internal service work
 * @error: (out) (optional): return location for an error
 *
 * Returns: (transfer full) (nullable): owned result
 */
VentureEntity *venture_progress_service_hold_retention(VentureProgressService *self,
	VentureQuote *quote, gint64 liability_account_id, const VentureMoney *amount,
	const VentureActor *actor, GError **error);
/**
 * venture_progress_service_release_retention:
 * @self: the service or registry instance
 * @retention: contract retention
 * @amount: amount with an explicit currency
 * @actor: (nullable): audit actor; NULL for internal service work
 * @error: (out) (optional): return location for an error
 *
 * Returns: TRUE on success, FALSE on failure
 */
gboolean venture_progress_service_release_retention(VentureProgressService *self,
	VentureContractRetention *retention, const VentureMoney *amount,
	const VentureActor *actor, GError **error);
/**
 * venture_progress_actions_register:
 * @database: database owning the records
 */
void venture_progress_actions_register(VentureDatabase *database);
G_END_DECLS
#endif
