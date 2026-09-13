/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_PAYABLE_RECORDS_H
#define VENTURE_PAYABLE_RECORDS_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

G_BEGIN_DECLS

#define VENTURE_TYPE_BILL_PAYMENT (venture_bill_payment_get_type())
VENTURE_DECLARE_ENTITY(VentureBillPayment, venture_bill_payment, BILL_PAYMENT)
#define VENTURE_TYPE_BILL_PAYMENT_ALLOCATION (venture_bill_payment_allocation_get_type())
VENTURE_DECLARE_ENTITY(VentureBillPaymentAllocation, venture_bill_payment_allocation, BILL_PAYMENT_ALLOCATION)
#define VENTURE_TYPE_VENDOR_CREDIT (venture_vendor_credit_get_type())
VENTURE_DECLARE_ENTITY(VentureVendorCredit, venture_vendor_credit, VENDOR_CREDIT)
#define VENTURE_TYPE_BILL_REFUND (venture_bill_refund_get_type())
VENTURE_DECLARE_ENTITY(VentureBillRefund, venture_bill_refund, BILL_REFUND)
#define VENTURE_TYPE_VENDOR_BILL_EVENT (venture_vendor_bill_event_get_type())
VENTURE_DECLARE_ENTITY(VentureVendorBillEvent, venture_vendor_bill_event, VENDOR_BILL_EVENT)

/**
 * venture_bill_payment_new:
 *
 * Returns: (transfer full): a vendor receipt
 */
/**
 * venture_bill_payment_allocation_new:
 *
 * Returns: (transfer full): an allocation of a receipt or credit
 */
/**
 * venture_vendor_credit_new:
 *
 * Returns: (transfer full): a credit note or unapplied receipt
 */
/**
 * venture_bill_refund_new:
 *
 * Returns: (transfer full): a refund against an allocation or unused credit
 */
/**
 * venture_vendor_bill_event_new:
 *
 * Returns: (transfer full): an immutable invoice lifecycle event
 */

#define VENTURE_TYPE_VENDOR_BILL (venture_vendor_bill_get_type())
VENTURE_DECLARE_ENTITY(VentureVendorBill, venture_vendor_bill, VENDOR_BILL)
#define VENTURE_TYPE_VENDOR_BILL_LINE (venture_vendor_bill_line_get_type())
VENTURE_DECLARE_ENTITY(VentureVendorBillLine, venture_vendor_bill_line, VENDOR_BILL_LINE)

/**
 * venture_vendor_bill_new:
 * Returns: (transfer full): a draft supplier bill
 */
/**
 * venture_vendor_bill_line_new:
 * Returns: (transfer full): a supplier bill line with exact quantity
 */
G_END_DECLS
#endif
