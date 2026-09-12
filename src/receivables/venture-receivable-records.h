/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_RECEIVABLE_RECORDS_H
#define VENTURE_RECEIVABLE_RECORDS_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

G_BEGIN_DECLS

#define VENTURE_TYPE_PAYMENT (venture_payment_get_type())
VENTURE_DECLARE_ENTITY(VenturePayment, venture_payment, PAYMENT)
#define VENTURE_TYPE_PAYMENT_ALLOCATION (venture_payment_allocation_get_type())
VENTURE_DECLARE_ENTITY(VenturePaymentAllocation, venture_payment_allocation, PAYMENT_ALLOCATION)
#define VENTURE_TYPE_CUSTOMER_CREDIT (venture_customer_credit_get_type())
VENTURE_DECLARE_ENTITY(VentureCustomerCredit, venture_customer_credit, CUSTOMER_CREDIT)
#define VENTURE_TYPE_REFUND (venture_refund_get_type())
VENTURE_DECLARE_ENTITY(VentureRefund, venture_refund, REFUND)
#define VENTURE_TYPE_INVOICE_EVENT (venture_invoice_event_get_type())
VENTURE_DECLARE_ENTITY(VentureInvoiceEvent, venture_invoice_event, INVOICE_EVENT)

/** venture_payment_new: Returns: (transfer full): a customer receipt. */
/** venture_payment_allocation_new: Returns: (transfer full): an allocation of a receipt or credit. */
/** venture_customer_credit_new: Returns: (transfer full): a credit note or unapplied receipt. */
/** venture_refund_new: Returns: (transfer full): a refund against an allocation or unused credit. */
/** venture_invoice_event_new: Returns: (transfer full): an immutable invoice lifecycle event. */

G_END_DECLS
#endif
