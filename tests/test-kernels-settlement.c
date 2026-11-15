/*
 * test-kernels-settlement.c - The open-balance kernel through its
 * generated C API and the shim receivables and payables share
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * src/kernels/vk_settlement.tlw tests itself (`make kernels-check`).
 * This suite calls vk_settlement_balance() the way
 * venture-settlement-private.h builds its records, and the shim itself
 * on records built in memory: currency numbering, the answer's currency
 * and exponent, and the errors worded as adding VentureMoney worded
 * them.
 */

#include <venture.h>
#include <vk-kernels.h>

#include "receivables/venture-settlement-private.h"

/* --- the kernel ------------------------------------------------------------ */

/* records: kind, amount, exponent, currency (currency < 0: no amount) */
static GPtrArray *
settlement_records(
	const gint64	*values,
	guint		 n
){
	GPtrArray *records;
	guint i;

	records = g_ptr_array_new_with_free_func((GDestroyNotify)vk_settlement_record_free);
	for (i = 0; i + 3 < n; i += 4)
		g_ptr_array_add(records, values[i + 3] < 0
			? vk_settlement_record_new_unset((VkSettlementKind)values[i])
			: vk_settlement_record_new_money((VkSettlementKind)values[i], values[i + 1], values[i + 2], values[i + 3]));
	return records;
}

#define ISSUE (VK_SETTLEMENT_KIND_ISSUE)
#define FEE (VK_SETTLEMENT_KIND_LATE_FEE)
#define VOID_ (VK_SETTLEMENT_KIND_VOID)
#define ALLOC (VK_SETTLEMENT_KIND_ALLOCATION)
#define REFUND (VK_SETTLEMENT_KIND_REFUND)

/*
 * Issued 100.00, a late fee of 5.000, 40.00 paid and 10.00 of it
 * refunded: 75.000 open, at the finest exponent reached. Late fees count
 * after every issue whatever the order the events were read in.
 */
static void
test_kernels_settlement_open(void)
{
	static const gint64 values[] = {
		FEE, 5000, 3, 0,
		ISSUE, 10000, 2, 0,
		ALLOC, 4000, 2, 0,
		REFUND, 1000, 2, 0,
	};
	static const gint64 paid[] = { ISSUE, 100, 2, 0, ALLOC, 1000, 3, 0 };
	static const gint64 over[] = { ISSUE, 100, 2, 0, ALLOC, 150, 2, 0 };
	g_autoptr(GPtrArray) a = settlement_records(values, G_N_ELEMENTS(values));
	g_autoptr(GPtrArray) b = settlement_records(paid, G_N_ELEMENTS(paid));
	g_autoptr(GPtrArray) c = settlement_records(over, G_N_ELEMENTS(over));
	g_autoptr(VkSettlementBalance) open = vk_settlement_balance(a);
	g_autoptr(VkSettlementBalance) settled = vk_settlement_balance(b);
	g_autoptr(VkSettlementBalance) overpaid = vk_settlement_balance(c);

	g_assert_cmpint(open->tag, ==, VK_SETTLEMENT_BALANCE_OPEN);
	g_assert_cmpint(open->u.open_.amount, ==, 75000);
	g_assert_cmpint(open->u.open_.exponent, ==, 3);
	/* the issue opened the total, so its currency is the answer's */
	g_assert_cmpint(open->u.open_.first, ==, 1);
	g_assert_cmpint(settled->tag, ==, VK_SETTLEMENT_BALANCE_OPEN);
	g_assert_cmpint(settled->u.open_.amount, ==, 0);
	g_assert_cmpint(settled->u.open_.exponent, ==, 3);
	g_assert_cmpint(overpaid->u.open_.amount, ==, -50);
}

/* Nothing issued, and voided: what the fold never reaches is never
 * refused, as the C never read it. */
static void
test_kernels_settlement_closed(void)
{
	static const gint64 unissued[] = { FEE, 500, 2, 0, ALLOC, 0, 0, -1 };
	static const gint64 voided[] = { ISSUE, 100, 2, 0, FEE, 1, 3, 0, VOID_, 0, 0, -1, ALLOC, 0, 0, -1 };
	g_autoptr(GPtrArray) a = settlement_records(unissued, G_N_ELEMENTS(unissued));
	g_autoptr(GPtrArray) b = settlement_records(voided, G_N_ELEMENTS(voided));
	g_autoptr(VkSettlementBalance) none = vk_settlement_balance(a);
	g_autoptr(VkSettlementBalance) gone = vk_settlement_balance(b);

	g_assert_cmpint(none->tag, ==, VK_SETTLEMENT_BALANCE_NOT_ISSUED);
	g_assert_cmpint(gone->tag, ==, VK_SETTLEMENT_BALANCE_VOIDED);
	g_assert_cmpint(gone->u.voided.exponent, ==, 3);
	g_assert_cmpint(gone->u.voided.first, ==, 0);
}

/* The record the fold stopped at, for the shim's message. */
static void
test_kernels_settlement_refused(void)
{
	static const gint64 missing[] = { ISSUE, 100, 2, 0, ALLOC, 0, 0, -1 };
	static const gint64 mismatch[] = { ISSUE, 100, 2, 0, FEE, 5, 2, 0, ALLOC, 1, 2, 2 };
	static const gint64 overflow[] = { ISSUE, G_MAXINT64, 0, 0, FEE, 1, 0, 0 };
	static const gint64 order[] = { ISSUE, G_MAXINT64, 0, 0, ALLOC, 1, 0, 0, REFUND, 1, 0, 0 };
	g_autoptr(GPtrArray) a = settlement_records(missing, G_N_ELEMENTS(missing));
	g_autoptr(GPtrArray) b = settlement_records(mismatch, G_N_ELEMENTS(mismatch));
	g_autoptr(GPtrArray) c = settlement_records(overflow, G_N_ELEMENTS(overflow));
	g_autoptr(GPtrArray) d = settlement_records(order, G_N_ELEMENTS(order));
	g_autoptr(VkSettlementBalance) no_amount = vk_settlement_balance(a);
	g_autoptr(VkSettlementBalance) other = vk_settlement_balance(b);
	g_autoptr(VkSettlementBalance) large = vk_settlement_balance(c);
	g_autoptr(VkSettlementBalance) kept = vk_settlement_balance(d);

	g_assert_cmpint(no_amount->tag, ==, VK_SETTLEMENT_BALANCE_MISSING);
	g_assert_cmpint(no_amount->u.missing.at, ==, 1);
	g_assert_cmpint(other->tag, ==, VK_SETTLEMENT_BALANCE_MISMATCH);
	g_assert_cmpint(other->u.mismatch.at, ==, 2);
	g_assert_cmpint(other->u.mismatch.first, ==, 0);
	g_assert_cmpint(large->tag, ==, VK_SETTLEMENT_BALANCE_OVERFLOW);
	g_assert_cmpint(large->u.overflow.at, ==, 1);
	/* the running order is kept: the payment brings it back in range */
	g_assert_cmpint(kept->tag, ==, VK_SETTLEMENT_BALANCE_OPEN);
	g_assert_cmpint(kept->u.open_.amount, ==, G_MAXINT64);
}

/* --- the shim -------------------------------------------------------------- */

static VentureEntity *
event_new(
	const gchar	*kind,
	gint64		 amount,
	const gchar	*currency,
	guint8		 exponent
){
	g_autoptr(VentureMoney) money = NULL;

	money = currency != NULL ? venture_money_new(amount, currency, exponent) : NULL;
	return g_object_new(VENTURE_TYPE_INVOICE_EVENT, "kind", kind, "amount", money, NULL);
}

static VentureEntity *
allocation_new(
	GType		 type,
	gint64		 amount,
	const gchar	*currency
){
	g_autoptr(VentureMoney) money = venture_money_new(amount, currency, 2);

	return g_object_new(type, "amount", money, NULL);
}

/*
 * Events in, money out: currencies are numbered case-insensitively, the
 * answer is in the issue's currency, a bill ignores late fees, and an
 * open document's payments count.
 */
static void
test_kernels_settlement_shim_balance(void)
{
	g_autoptr(GPtrArray) events = g_ptr_array_new_with_free_func(g_object_unref);
	g_autoptr(GPtrArray) refunds = g_ptr_array_new_with_free_func(g_object_unref);
	g_autoptr(VentureEntity) allocation = allocation_new(VENTURE_TYPE_PAYMENT_ALLOCATION, 3000, "usd");
	g_autoptr(VentureSettlementRecords) invoice = venture_settlement_records_new("VentureSettlementService");
	g_autoptr(VentureSettlementRecords) bill = venture_settlement_records_new("VenturePayablesService");
	g_autoptr(VentureMoney) issued = NULL;
	g_autoptr(VentureMoney) owed = NULL;
	g_autoptr(VentureMoney) billed = NULL;
	g_autoptr(GError) error = NULL;
	gboolean open = FALSE;

	g_ptr_array_add(events, event_new("issue", 10000, "USD", 2));
	g_ptr_array_add(events, event_new("sent", 0, NULL, 0));
	g_ptr_array_add(events, event_new(VENTURE_INVOICE_EVENT_LATE_FEE, 2500, "Usd", 3));
	g_ptr_array_add(refunds, allocation_new(VENTURE_TYPE_REFUND, 500, "USD"));

	venture_settlement_records_add_events(invoice, events, TRUE);
	issued = venture_settlement_records_balance(invoice, &open, &error);
	g_assert_no_error(error);
	g_assert_true(open);
	g_assert_cmpint(venture_money_get_amount(issued), ==, 102500);
	g_assert_cmpint(venture_money_get_exponent(issued), ==, 3);
	g_assert_cmpstr(venture_money_get_currency(issued), ==, "USD");

	venture_settlement_records_add_allocation(invoice, allocation, refunds);
	owed = venture_settlement_records_balance(invoice, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpint(venture_money_get_amount(owed), ==, 77500);

	venture_settlement_records_add_events(bill, events, FALSE);
	billed = venture_settlement_records_balance(bill, &open, &error);
	g_assert_no_error(error);
	g_assert_cmpint(venture_money_get_amount(billed), ==, 10000);
	g_assert_cmpint(venture_money_get_exponent(billed), ==, 2);
}

/* Nothing issued is a zero with no currency; a void a zero in the
 * issue's currency, at the exponent the issued total reached. */
static void
test_kernels_settlement_shim_closed(void)
{
	g_autoptr(GPtrArray) none = g_ptr_array_new_with_free_func(g_object_unref);
	g_autoptr(GPtrArray) voided = g_ptr_array_new_with_free_func(g_object_unref);
	g_autoptr(VentureSettlementRecords) a = venture_settlement_records_new("VentureSettlementService");
	g_autoptr(VentureSettlementRecords) b = venture_settlement_records_new("VentureSettlementService");
	g_autoptr(VentureMoney) zero = NULL;
	g_autoptr(VentureMoney) gone = NULL;
	g_autoptr(VentureMoney) expected = venture_money_new_zero(NULL);
	g_autoptr(GError) error = NULL;
	gboolean open = TRUE;

	g_ptr_array_add(none, event_new(VENTURE_INVOICE_EVENT_LATE_FEE, 100, "EUR", 2));
	venture_settlement_records_add_events(a, none, TRUE);
	zero = venture_settlement_records_balance(a, &open, &error);
	g_assert_no_error(error);
	g_assert_false(open);
	g_assert_cmpint(venture_money_get_amount(zero), ==, 0);
	g_assert_cmpstr(venture_money_get_currency(zero), ==, venture_money_get_currency(expected));

	g_ptr_array_add(voided, event_new("issue", 100, "EUR", 2));
	g_ptr_array_add(voided, event_new(VENTURE_INVOICE_EVENT_LATE_FEE, 1, "EUR", 3));
	g_ptr_array_add(voided, event_new("void", 0, NULL, 0));
	venture_settlement_records_add_events(b, voided, TRUE);
	gone = venture_settlement_records_balance(b, &open, &error);
	g_assert_no_error(error);
	g_assert_false(open);
	g_assert_cmpint(venture_money_get_amount(gone), ==, 0);
	g_assert_cmpint(venture_money_get_exponent(gone), ==, 3);
	g_assert_cmpstr(venture_money_get_currency(gone), ==, "EUR");
}

/* The errors adding VentureMoney raised, in the same words. */
static void
test_kernels_settlement_shim_errors(void)
{
	g_autoptr(GPtrArray) missing = g_ptr_array_new_with_free_func(g_object_unref);
	g_autoptr(GPtrArray) mixed = g_ptr_array_new_with_free_func(g_object_unref);
	g_autoptr(GPtrArray) large = g_ptr_array_new_with_free_func(g_object_unref);
	g_autoptr(VentureEntity) allocation = allocation_new(VENTURE_TYPE_BILL_PAYMENT_ALLOCATION, -1, "USD");
	g_autoptr(VentureSettlementRecords) a = venture_settlement_records_new("VenturePayablesService");
	g_autoptr(VentureSettlementRecords) b = venture_settlement_records_new("VentureSettlementService");
	g_autoptr(VentureSettlementRecords) c = venture_settlement_records_new("VentureSettlementService");
	g_autoptr(GError) error = NULL;
	VentureMoney *result;

	g_ptr_array_add(missing, event_new("issue", 0, NULL, 0));
	venture_settlement_records_add_events(a, missing, FALSE);
	result = venture_settlement_records_balance(a, NULL, &error);
	g_assert_null(result);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_cmpstr(error->message, ==, "VenturePayablesService: An event has no amount");
	g_clear_error(&error);

	g_ptr_array_add(mixed, event_new("issue", 100, "USD", 2));
	g_ptr_array_add(mixed, event_new(VENTURE_INVOICE_EVENT_LATE_FEE, 100, "EUR", 2));
	venture_settlement_records_add_events(b, mixed, TRUE);
	result = venture_settlement_records_balance(b, NULL, &error);
	g_assert_null(result);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_assert_true(g_str_has_prefix(error->message, "Cannot combine USD and EUR"));
	g_clear_error(&error);

	/* subtracting -0.01 from the largest amount: the subtraction's words */
	g_ptr_array_add(large, event_new("issue", G_MAXINT64, "USD", 2));
	venture_settlement_records_add_events(c, large, TRUE);
	venture_settlement_records_add_allocation(c, allocation, NULL);
	result = venture_settlement_records_balance(c, NULL, &error);
	g_assert_null(result);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_assert_cmpstr(error->message, ==, "Monetary subtraction overflows a 64-bit minor-unit amount");
}

gint
main(
	gint	 argc,
	gchar	**argv
){
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/kernels-settlement/balance/open", test_kernels_settlement_open);
	g_test_add_func("/kernels-settlement/balance/closed", test_kernels_settlement_closed);
	g_test_add_func("/kernels-settlement/balance/refused", test_kernels_settlement_refused);
	g_test_add_func("/kernels-settlement/shim/balance", test_kernels_settlement_shim_balance);
	g_test_add_func("/kernels-settlement/shim/closed", test_kernels_settlement_shim_closed);
	g_test_add_func("/kernels-settlement/shim/errors", test_kernels_settlement_shim_errors);

	return g_test_run();
}
