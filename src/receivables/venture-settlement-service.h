/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_SETTLEMENT_SERVICE_H
#define VENTURE_SETTLEMENT_SERVICE_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

G_BEGIN_DECLS
#define VENTURE_TYPE_SETTLEMENT_SERVICE (venture_settlement_service_get_type())
G_DECLARE_FINAL_TYPE(VentureSettlementService, venture_settlement_service,
	VENTURE, SETTLEMENT_SERVICE, GObject)

/**
 * venture_settlement_service_get:
 * @database: the owning database
 * Returns: (transfer none): the database's single settlement service
 */
VentureSettlementService *venture_settlement_service_get(VentureDatabase *database);

/**
 * venture_settlement_service_get_state_machine:
 * @self: the service
 * Returns: (transfer none): its extensible, vetoable invoice lifecycle
 */
VentureInvoiceStateMachine *venture_settlement_service_get_state_machine(VentureSettlementService *self);

/**
 * venture_settlement_service_apply_payment:
 * @self: the service
 * @payment: an unsaved receipt
 * @allocations: (nullable) (element-type VenturePaymentAllocation): unsaved allocations;
 *   NULL uses payment.invoice-id, or leaves the receipt as a deposit
 * @actor: (nullable): the audit actor
 * @error: (out) (optional): the error
 *
 * All allocations, credit balances, invoice states, sales and postings are
 * written in one transaction. Failure also restores the caller's objects.
 * Returns: TRUE on success
 */
gboolean venture_settlement_service_apply_payment(VentureSettlementService *self,
	VenturePayment *payment, GPtrArray *allocations, const VentureActor *actor, GError **error);

/**
 * venture_settlement_service_transition:
 * @self: the service
 * @invoice: a persisted invoice, with its expected version
 * @state: a declared workflow state; paid and partially_paid require allocations
 * @date: the effective event time
 * @actor: (nullable): the audit actor
 * @error: (out) (optional): the error
 * Returns: TRUE on success
 */
gboolean venture_settlement_service_transition(VentureSettlementService *self,
	VentureInvoice *invoice, const gchar *state, GDateTime *date,
	const VentureActor *actor, GError **error);

/**
 * venture_settlement_service_settle_invoice:
 * @self: the service
 * @invoice_id: the invoice
 * @date: the receipt time
 * @actor: (nullable): the audit actor
 * @error: (out) (optional): the error
 *
 * The web Pay action: records a manual receipt for the outstanding amount.
 * Returns: TRUE on success
 */
gboolean venture_settlement_service_settle_invoice(VentureSettlementService *self,
	gint64 invoice_id, GDateTime *date, const VentureActor *actor, GError **error);

/**
 * venture_settlement_service_invoice_balance:
 * @self: the service
 * @invoice_id: the invoice
 * @as_of: (nullable): exclusive cutoff, NULL includes all events
 * @error: (out) (optional): the error
 * Returns: (transfer full) (nullable): issued less allocated plus refunded, or NULL on error
 */
VentureMoney *venture_settlement_service_invoice_balance(VentureSettlementService *self,
	gint64 invoice_id, GDateTime *as_of, GError **error);

/**
 * venture_settlement_service_customer_balance:
 * @self: the service
 * @organization_id: one legal entity
 * @customer_id: the customer
 * @as_of: (nullable): exclusive event cutoff
 * @currency: the currency to report, never converted
 * @error: (out) (optional): the error
 * Returns: (transfer full) (nullable): receivables less unused credit in that currency
 */
VentureMoney *venture_settlement_service_customer_balance(VentureSettlementService *self,
	gint64 organization_id, gint64 customer_id, GDateTime *as_of,
	const gchar *currency, GError **error);

/**
 * venture_receivables_save_hook: (skip)
 * @database: the database, with its save lock held
 * @record: the proposed record
 * @actor: (nullable): the audit actor
 * @handled: (out): TRUE if settlement owns this save
 * @authorized: (out): TRUE if the service consumed its internal write permit
 * @error: (out) (optional): the error
 * Returns: TRUE if allowed or completed; the generic database save hook
 */
gboolean venture_receivables_save_hook(VentureDatabase *database, VentureEntity *record,
	const VentureActor *actor, gboolean *handled, gboolean *authorized, GError **error);

/**
 * venture_receivables_check_removal: (skip)
 * @database: the database
 * @record: the proposed deletion, restoration or purge
 * @error: (out) (optional): the error
 * Returns: TRUE if removing the record cannot change settlement history
 */
gboolean venture_receivables_check_removal(VentureDatabase *database,
	VentureEntity *record, GError **error);

/**
 * venture_receivables_register_reports:
 * @registry: the report registry
 *
 * Registers aging and customer statements over the same dated subledger.
 */
void venture_receivables_register_reports(VentureReportRegistry *registry);
/**
 * venture_receivables_is_projection_write: (skip)
 * @database: the owning database
 * @record: the proposed record
 * Returns: TRUE only for the current service-owned cash report projection
 */
gboolean venture_receivables_is_projection_write(VentureDatabase *database, VentureEntity *record);

/**
 * venture_receivables_check_sale: (skip)
 * @database: the owning database
 * @record: a sale
 * @error: (out) (optional): the error
 * Returns: TRUE if the sale is not immutable settlement evidence
 */
gboolean venture_receivables_check_sale(VentureDatabase *database, VentureEntity *record, GError **error);

/** venture_settlement_service_record_mail:
 * @self: settlement service
 * @invoice: issued invoice
 * @actor: (nullable): audit actor
 * @error: (out) (optional): persistence failure
 * Returns: whether a nonfinancial mail-queued invoice event was recorded
 */
gboolean venture_settlement_service_record_mail(VentureSettlementService *self, VentureInvoice *invoice, const VentureActor *actor, GError **error);
/**
 * venture_settlement_service_correct_tax_allocation:
 * @self: settlement service
 * @organization_id: legal entity
 * @date: dated correction
 * @actor: (nullable): audit actor
 * @error: (out) (optional): refusal
 *
 * Posts explicit dated corrections moving tax out of income for issued
 * invoices whose original journals credited income for the tax-inclusive total.
 * Returns: %TRUE if every needed correction posted or was already present
 */
gboolean venture_settlement_service_correct_tax_allocation(VentureSettlementService *self,
	gint64 organization_id, GDateTime *date, const VentureActor *actor, GError **error);

/**
 * venture_settlement_service_write_off:
 * @self: settlement service
 * @invoice_id: an issued invoice with remaining AR
 * @date: correction date
 * @actor: (nullable): audit actor
 * @error: (out) (optional): refusal
 *
 * Writes off remaining AR through a dated credit and allocation. Original
 * journals stay posted; cash is not returned.
 * Returns: %TRUE on success
 */
gboolean venture_settlement_service_write_off(VentureSettlementService *self,
	gint64 invoice_id, GDateTime *date, const VentureActor *actor, GError **error);
G_END_DECLS
#endif
