/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_PAYABLES_SERVICE_H
#define VENTURE_PAYABLES_SERVICE_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

G_BEGIN_DECLS
#define VENTURE_TYPE_PAYABLES_SERVICE (venture_payables_service_get_type())
G_DECLARE_FINAL_TYPE(VenturePayablesService, venture_payables_service,
	VENTURE, PAYABLES_SERVICE, GObject)

/**
 * venture_payables_service_get:
 * @database: the owning database
 * Returns: (transfer none): the database's single settlement service
 */
VenturePayablesService *venture_payables_service_get(VentureDatabase *database);

/**
 * venture_payables_service_apply_payment:
 * @self: the service
 * @payment: an unsaved supplier payment
 * @allocations: (nullable) (element-type VentureBillPaymentAllocation): unsaved allocations;
 *   NULL uses payment.bill-id, or leaves the payment as a deposit
 * @actor: (nullable): the audit actor
 * @error: (out) (optional): the error
 *
 * All allocations, credit balances, bill states, expenses and postings are
 * written in one transaction. Failure also restores the caller's objects.
 * Returns: TRUE on success
 */
gboolean venture_payables_service_apply_payment(VenturePayablesService *self,
	VentureBillPayment *payment, GPtrArray *allocations, const VentureActor *actor, GError **error);

/**
 * venture_payables_service_transition:
 * @self: the service
 * @bill: a persisted bill, with its expected version
 * @state: a declared workflow state; paid and partially_paid require allocations
 * @date: the effective event time
 * @actor: (nullable): the audit actor
 * @error: (out) (optional): the error
 * Returns: TRUE on success
 */
gboolean venture_payables_service_transition(VenturePayablesService *self,
	VentureVendorBill *bill, const gchar *state, GDateTime *date,
	const VentureActor *actor, GError **error);

/**
 * venture_payables_service_settle_bill:
 * @self: the service
 * @bill_id: the bill
 * @date: the payment time
 * @actor: (nullable): the audit actor
 * @error: (out) (optional): the error
 *
 * The web Pay action: records a manual payment for the outstanding amount.
 * Returns: TRUE on success
 */
gboolean venture_payables_service_settle_bill(VenturePayablesService *self,
	gint64 bill_id, GDateTime *date, const VentureActor *actor, GError **error);

/**
 * venture_payables_service_bill_balance:
 * @self: the service
 * @bill_id: the bill
 * @as_of: (nullable): exclusive cutoff, NULL includes all events
 * @error: (out) (optional): the error
 * Returns: (transfer full) (nullable): issued less allocated plus refunded, or NULL on error
 */
VentureMoney *venture_payables_service_bill_balance(VenturePayablesService *self,
	gint64 bill_id, GDateTime *as_of, GError **error);

/**
 * venture_payables_service_vendor_balance:
 * @self: the service
 * @organization_id: one legal entity
 * @vendor_id: the vendor
 * @as_of: (nullable): exclusive event cutoff
 * @currency: the currency to report, never converted
 * @error: (out) (optional): the error
 * Returns: (transfer full) (nullable): payables less unused credit in that currency
 */
VentureMoney *venture_payables_service_vendor_balance(VenturePayablesService *self,
	gint64 organization_id, gint64 vendor_id, GDateTime *as_of,
	const gchar *currency, GError **error);

/**
 * venture_payables_service_refresh_credit:
 * @self: the service
 * @credit_id: a vendor credit
 * @actor: (nullable): the audit actor
 * @error: (out) (optional): the error
 *
 * Rewrites remaining from allocations, refunds and reversed postings.
 * Returns: TRUE on success
 */
gboolean venture_payables_service_refresh_credit(VenturePayablesService *self,
	gint64 credit_id, const VentureActor *actor, GError **error);

/**
 * venture_payables_save_hook: (skip)
 * @database: the database, with its save lock held
 * @record: the proposed record
 * @actor: (nullable): the audit actor
 * @handled: (out): TRUE if settlement owns this save
 * @authorized: (out): TRUE if the service consumed its internal write permit
 * @error: (out) (optional): the error
 * Returns: TRUE if allowed or completed; the generic database save hook
 */
gboolean venture_payables_save_hook(VentureDatabase *database, VentureEntity *record,
	const VentureActor *actor, gboolean *handled, gboolean *authorized, GError **error);

/**
 * venture_payables_check_removal: (skip)
 * @database: the database
 * @record: the proposed deletion, restoration or purge
 * @error: (out) (optional): the error
 * Returns: TRUE if removing the record cannot change settlement history
 */
gboolean venture_payables_check_removal(VentureDatabase *database,
	VentureEntity *record, GError **error);

/**
 * venture_payables_register_reports:
 * @registry: the report registry
 *
 * Registers aging and vendor statements over the same dated subledger.
 */
void venture_payables_register_reports(VentureReportRegistry *registry);
/**
 * venture_payables_is_projection_write: (skip)
 * @database: the owning database
 * @record: the proposed record
 * Returns: TRUE only for the current service-owned cash report projection
 */
gboolean venture_payables_is_projection_write(VentureDatabase *database, VentureEntity *record);

/**
 * venture_payables_check_expense: (skip)
 * @database: the owning database
 * @record: an expense
 * @error: (out) (optional): the error
 * Returns: TRUE if the expense is not immutable settlement evidence
 */
gboolean venture_payables_check_expense(VentureDatabase *database, VentureEntity *record, GError **error);

/**
 * venture_database_get_payables_service:
 * @database: the owner
 * Returns: (transfer none): the database-owned payables service
 */
VenturePayablesService *venture_database_get_payables_service(VentureDatabase *database);

/**
 * venture_payables_service_prepare_action:
 * @self: the service
 * @bill_id: the target bill
 * @action: approve, pay, or void
 * @options: (nullable): ordinary record field values
 * @error: (out) (optional): validation error
 * Returns: (transfer full) (nullable): an unsaved request for direct or staged save
 */
VentureEntity *venture_payables_service_prepare_action(VenturePayablesService *self,
	gint64 bill_id, const gchar *action, JsonNode *options, GError **error);

/**
 * venture_payables_expense_hook: (skip)
 * @database: the owner
 * @record: a proposed expense or unrelated record
 * @actor: (nullable): audit actor
 * @handled: (out): whether the conversion saved the expense
 * @error: (out) (optional): error
 * Returns: TRUE on success or for unrelated records
 */
gboolean venture_payables_expense_hook(VentureDatabase *database, VentureEntity *record,
	const VentureActor *actor, gboolean *handled, GError **error);

/**
 * venture_payables_service_execute_payment:
 * @self: the service
 * @adapter: manual, transfer, card, check or ach
 * @payment: an unsaved supplier payment
 * @allocations: (nullable) (element-type VentureBillPaymentAllocation): unsaved allocations
 * @actor: (nullable): the audit actor
 * @error: (out) (optional): the error
 *
 * Payment execution adapters all call apply_payment. They never write
 * settlement tables themselves.
 * Returns: TRUE on success
 */
gboolean venture_payables_service_execute_payment(VenturePayablesService *self,
	const gchar *adapter, VentureBillPayment *payment, GPtrArray *allocations,
	const VentureActor *actor, GError **error);

/**
 * venture_payables_service_pay_bills:
 * @self: the service
 * @bill_ids: (element-type gint64): approved or partially paid bills
 * @date: the payment date
 * @method: (nullable): recorded method; defaults to @adapter
 * @adapter: (nullable): execution adapter name, default transfer
 * @reference: (nullable): payment reference
 * @actor: (nullable): the audit actor
 * @error: (out) (optional): the error
 *
 * Pays the selected bills through one payment per vendor, with allocations,
 * inside one transaction.
 * Returns: TRUE on success
 */
gboolean venture_payables_service_pay_bills(VenturePayablesService *self, GArray *bill_ids,
	GDateTime *date, const gchar *method, const gchar *adapter, const gchar *reference,
	const VentureActor *actor, GError **error);

G_END_DECLS
#endif
