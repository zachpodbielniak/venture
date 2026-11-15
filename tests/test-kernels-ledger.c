/*
 * test-kernels-ledger.c - The accounting kernels through their generated C API
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The kernels in src/kernels test themselves (`make kernels-check`). This
 * suite tests the accounting ones (vk_ledger and the kernels beside it)
 * from the side VENTURE uses: the vk_* functions `tallow render` writes,
 * called the way the shims in src/ledger, src/payroll, src/budgets,
 * src/reconciliation, src/tax and src/goods call them, plus one
 * regression case for each bug the move to kernels fixed. A regression
 * case here fails on the C the kernel replaced.
 */

#include <venture.h>
#include <vk-kernels.h>

/* --- vk_ledger ------------------------------------------------------------- */

/* Lines as the shims build them: amount, exponent, debit. */
static GPtrArray *
ledger_lines(
	const gint64	*values,
	guint		 n
){
	GPtrArray *lines;
	guint i;

	/* values holds triples: amount, exponent, 1 for a debit */
	lines = g_ptr_array_new_with_free_func((GDestroyNotify)vk_ledger_line_free);
	for (i = 0; i + 2 < n; i += 3)
		g_ptr_array_add(lines, vk_ledger_line_new(values[i], values[i + 1], 0 != values[i + 2]));
	return lines;
}

/*
 * A journal balances on its valued totals compared at their common
 * exponent; 60.000 of credit is 60.00 of debit.
 */
static void
test_kernels_ledger_journal(void)
{
	static const gint64 even[] = { 6000, 2, 1, 60000, 3, 0 };
	static const gint64 uneven[] = { 6000, 2, 1, 5999, 2, 0 };
	static const gint64 single[] = { 6000, 2, 1 };
	static const gint64 huge[] = { G_MAXINT64, 2, 1, 1, 2, 1, 1, 2, 0 };
	g_autoptr(GPtrArray) a = ledger_lines(even, G_N_ELEMENTS(even));
	g_autoptr(GPtrArray) b = ledger_lines(uneven, G_N_ELEMENTS(uneven));
	g_autoptr(GPtrArray) c = ledger_lines(single, G_N_ELEMENTS(single));
	g_autoptr(GPtrArray) d = ledger_lines(huge, G_N_ELEMENTS(huge));
	g_autoptr(VkLedgerJournal) ok = vk_ledger_check_journal(a, 2);
	g_autoptr(VkLedgerJournal) off = vk_ledger_check_journal(b, 2);
	g_autoptr(VkLedgerJournal) short_ = vk_ledger_check_journal(c, 2);
	g_autoptr(VkLedgerJournal) over = vk_ledger_check_journal(d, 2);

	g_assert_cmpint(ok->tag, ==, VK_LEDGER_JOURNAL_BALANCED);
	g_assert_cmpint(ok->u.balanced.amount, ==, 6000);
	g_assert_cmpint(off->tag, ==, VK_LEDGER_JOURNAL_UNBALANCED);
	g_assert_cmpint(short_->tag, ==, VK_LEDGER_JOURNAL_TOO_FEW_LINES);
	g_assert_cmpint(over->tag, ==, VK_LEDGER_JOURNAL_TOO_LARGE);
	g_assert_false(vk_ledger_enough_lines(1));
	g_assert_true(vk_ledger_enough_lines(2));
}

/*
 * Debits add and credits subtract, and the total keeps the finest
 * exponent it met, as the VentureMoney running sum did.
 */
static void
test_kernels_ledger_net(void)
{
	static const gint64 values[] = { 1000, 2, 1, 250, 2, 0, 5000, 3, 1 };
	g_autoptr(GPtrArray) lines = ledger_lines(values, G_N_ELEMENTS(values));
	g_autoptr(VkLedgerTotal) total = vk_ledger_net(lines, 2);
	g_autoptr(VkLedgerClearing) settle = NULL;
	g_autoptr(VkLedgerClearing) none = vk_ledger_clearing(0);
	g_autoptr(VkLedgerClearing) min = vk_ledger_clearing(G_MININT64);

	g_assert_cmpint(total->tag, ==, VK_LEDGER_TOTAL_SUM);
	g_assert_cmpint(total->u.sum.amount, ==, 12500);
	g_assert_cmpint(total->u.sum.exponent, ==, 3);

	/* a net debit is cleared by a credit of its size, and the reverse */
	settle = vk_ledger_clearing(-12500);
	g_assert_cmpint(settle->tag, ==, VK_LEDGER_CLEARING_POST);
	g_assert_cmpint(settle->u.post.amount, ==, 12500);
	g_assert_true(settle->u.post.debit);
	g_assert_cmpint(none->tag, ==, VK_LEDGER_CLEARING_SETTLED);
	g_assert_cmpint(min->tag, ==, VK_LEDGER_CLEARING_UNSETTLEABLE);
}

/* A trial balance's columns and totals. */
static void
test_kernels_ledger_trial(void)
{
	static const gint64 values[] = { 500, 2, 1, 5000, 3, 0 };
	static const gint64 off[] = { 500, 2, 1, 400, 2, 0 };
	g_autoptr(GPtrArray) lines = ledger_lines(values, G_N_ELEMENTS(values));
	g_autoptr(GPtrArray) bad = ledger_lines(off, G_N_ELEMENTS(off));
	g_autoptr(VkLedgerTrial) even = vk_ledger_trial(lines, 2);
	g_autoptr(VkLedgerTrial) uneven = vk_ledger_trial(bad, 2);
	g_autoptr(VkLedgerColumn) credit = vk_ledger_column(-7);
	g_autoptr(VkLedgerColumn) skip = vk_ledger_column(0);

	g_assert_cmpint(even->tag, ==, VK_LEDGER_TRIAL_EVEN);
	g_assert_cmpint(even->u.even.debits, ==, 500);
	g_assert_cmpint(even->u.even.credits, ==, 5000);
	g_assert_cmpint(even->u.even.credit_exponent, ==, 3);
	g_assert_cmpint(uneven->tag, ==, VK_LEDGER_TRIAL_UNEVEN);
	g_assert_cmpint(credit->tag, ==, VK_LEDGER_COLUMN_CREDIT);
	g_assert_cmpint(credit->u.credit.amount, ==, 7);
	g_assert_cmpint(skip->tag, ==, VK_LEDGER_COLUMN_SKIP);
}

/* The normal side by kind, and the balance on it. */
static void
test_kernels_ledger_normal(void)
{
	TlwOptI64 r;

	g_assert_true(venture_account_kind_is_debit_normal(VENTURE_ACCOUNT_KIND_ASSET));
	g_assert_true(venture_account_kind_is_debit_normal(VENTURE_ACCOUNT_KIND_EXPENSE));
	g_assert_false(venture_account_kind_is_debit_normal(VENTURE_ACCOUNT_KIND_LIABILITY));
	g_assert_false(venture_account_kind_is_debit_normal(VENTURE_ACCOUNT_KIND_EQUITY));
	g_assert_false(venture_account_kind_is_debit_normal(VENTURE_ACCOUNT_KIND_INCOME));

	r = vk_ledger_normal_balance(VK_LEDGER_KIND_INCOME, 100, 2, 9000, 3);
	g_assert_true(r.has);
	g_assert_cmpint(r.val, ==, 8000);
	r = vk_ledger_normal_balance(VK_LEDGER_KIND_ASSET, G_MININT64, 0, 1, 0);
	g_assert_false(r.has);
}

/*
 * Rate selection regression. The rate table sorted its rows with a
 * comparator that returned 0 whenever either date was missing. That is
 * not an order: with rows [10, undated, 30] GLib's stable sort compares
 * 10 with the undated row ("equal") and the undated row with 30 ("equal")
 * and leaves [10, undated, 30], so the scan that followed picked the rate
 * effective at 10 for a day after 30. The kernel's order is strict --
 * dated rows first, later first, then by position -- and the latest
 * applicable rate wins.
 */
static void
test_kernels_ledger_rate_order(void)
{
	g_autoptr(GPtrArray) rates = g_ptr_array_new_with_free_func((GDestroyNotify)vk_ledger_rate_free);
	TlwOptI64 chosen;

	g_ptr_array_add(rates, vk_ledger_rate_new(10, TRUE, TRUE));
	g_ptr_array_add(rates, vk_ledger_rate_new(0, FALSE, TRUE));
	g_ptr_array_add(rates, vk_ledger_rate_new(30, TRUE, TRUE));
	chosen = vk_ledger_pick_rate(rates, 40);
	g_assert_true(chosen.has);
	g_assert_cmpint(chosen.val, ==, 2);

	/* before the newer rate the older applies; before both, none */
	chosen = vk_ledger_pick_rate(rates, 20);
	g_assert_true(chosen.has);
	g_assert_cmpint(chosen.val, ==, 0);
	chosen = vk_ledger_pick_rate(rates, 5);
	g_assert_false(chosen.has);

	/* the order is antisymmetric, undated last */
	g_assert_true(vk_ledger_rate_ahead(TRUE, 1, 5, FALSE, 0, 0));
	g_assert_false(vk_ledger_rate_ahead(FALSE, 0, 0, TRUE, 1, 5));
	g_assert_true(vk_ledger_rate_ahead(TRUE, 30, 1, TRUE, 30, 2));
	g_assert_false(vk_ledger_rate_ahead(TRUE, 30, 2, TRUE, 30, 1));

	/* equal dates keep the earliest row, as the stable sort did; an
	 * unusable row (deleted, or a ratio that is not positive) never wins */
	g_ptr_array_set_size(rates, 0);
	g_ptr_array_add(rates, vk_ledger_rate_new(30, TRUE, FALSE));
	g_ptr_array_add(rates, vk_ledger_rate_new(30, TRUE, TRUE));
	g_ptr_array_add(rates, vk_ledger_rate_new(30, TRUE, TRUE));
	chosen = vk_ledger_pick_rate(rates, 30);
	g_assert_true(chosen.has);
	g_assert_cmpint(chosen.val, ==, 1);
}

/*
 * Payroll regression: gross + employer and net + liabilities were compared
 * by raw minor units after adding, so the same pay at two exponents was
 * "different" and 5400.00 against 540.000 was "equal".
 */
static void
test_kernels_ledger_pay_line(void)
{
	g_assert_cmpint(vk_ledger_pay_line(500000, 2, 40000, 2, 400000, 2, 1400000, 3), ==,
		VK_LEDGER_PAY_LINE_BALANCES);
	g_assert_cmpint(vk_ledger_pay_line(500000, 2, 40000, 2, 400000, 3, 140000, 3), ==,
		VK_LEDGER_PAY_LINE_DIFFERS);
	g_assert_cmpint(vk_ledger_pay_line(G_MAXINT64, 0, 1, 0, 0, 0, 0, 0), ==,
		VK_LEDGER_PAY_LINE_OVERFLOWS);
}

gint
main(
	gint	 argc,
	gchar	**argv
){
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/kernels-ledger/ledger/journal", test_kernels_ledger_journal);
	g_test_add_func("/kernels-ledger/ledger/net", test_kernels_ledger_net);
	g_test_add_func("/kernels-ledger/ledger/trial", test_kernels_ledger_trial);
	g_test_add_func("/kernels-ledger/ledger/normal", test_kernels_ledger_normal);
	g_test_add_func("/kernels-ledger/ledger/rate-order", test_kernels_ledger_rate_order);
	g_test_add_func("/kernels-ledger/ledger/pay-line", test_kernels_ledger_pay_line);

	return g_test_run();
}
