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

/* --- vk_holdings ----------------------------------------------------------- */

/* A movement as holdings_check_floor hands it over, at exponent 2. */
static VkHoldingsEvent *
floor_event(
	gint64		at,
	gint64		amount,
	gboolean	before,
	gboolean	after
){
	return vk_holdings_event_new(at, amount, 2, before, after);
}

/*
 * The floor is judged on the running balance a moment at a time: a spend
 * after takings at the same instant passes, a spend that takes the
 * holding below zero is refused at the moment it does, and a shortfall
 * the change leaves as it was is not the change's fault.
 */
static void
test_kernels_ledger_holdings_floor(void)
{
	g_autoptr(GPtrArray) events = g_ptr_array_new_with_free_func((GDestroyNotify)vk_holdings_event_free);
	g_autoptr(VkHoldingsFloor) same_instant = NULL;
	g_autoptr(VkHoldingsFloor) short_ = NULL;
	g_autoptr(VkHoldingsFloor) old_shortfall = NULL;

	g_ptr_array_add(events, floor_event(5, -400, FALSE, TRUE));
	g_ptr_array_add(events, floor_event(5, 1000, TRUE, TRUE));
	same_instant = vk_holdings_check_floor(events, 2);
	g_assert_cmpint(same_instant->tag, ==, VK_HOLDINGS_FLOOR_CLEAR);

	g_ptr_array_set_size(events, 0);
	g_ptr_array_add(events, floor_event(1, 1000, TRUE, TRUE));
	g_ptr_array_add(events, floor_event(2, -1500, FALSE, TRUE));
	g_ptr_array_add(events, floor_event(3, 10000, TRUE, TRUE));
	short_ = vk_holdings_check_floor(events, 2);
	g_assert_cmpint(short_->tag, ==, VK_HOLDINGS_FLOOR_SHORT);
	g_assert_cmpint(short_->u.short_.event, ==, 1);
	g_assert_cmpint(short_->u.short_.before, ==, 1000);
	g_assert_cmpint(short_->u.short_.after, ==, -500);

	g_ptr_array_set_size(events, 0);
	g_ptr_array_add(events, floor_event(1, 100, TRUE, TRUE));
	g_ptr_array_add(events, floor_event(2, -400, TRUE, TRUE));
	g_ptr_array_add(events, floor_event(3, 1000, TRUE, TRUE));
	g_ptr_array_add(events, floor_event(4, -100, FALSE, TRUE));
	old_shortfall = vk_holdings_check_floor(events, 2);
	g_assert_cmpint(old_shortfall->tag, ==, VK_HOLDINGS_FLOOR_CLEAR);
}

/* --- vk_reconcile ---------------------------------------------------------- */

/*
 * The relation the matcher computed before the kernel, in 128 bits: kept
 * here as the reference the kernel must agree with everywhere.
 */
static gint
reference_relation(
	gint64	a,
	guint	ae,
	gint64	b,
	guint	be
){
	__int128 av, bv, delta, base;

	av = a;
	bv = b;
	if (ae > VENTURE_MONEY_MAX_EXPONENT || be > VENTURE_MONEY_MAX_EXPONENT)
		return 0;
	while (ae < be) { av *= 10; ae++; }
	while (be < ae) { bv *= 10; be++; }
	if (av == bv)
		return 2;
	delta = av - bv;
	if (delta < 0) delta = -delta;
	base = av < 0 ? -av : av;
	return delta * 100 <= base * 2 ? 1 : 0;
}

static gint
kernel_relation(
	gint64	a,
	guint	ae,
	gint64	b,
	guint	be
){
	switch (vk_reconcile_relation(a, (gint64)ae, b, (gint64)be))
	{
	case VK_RECONCILE_RELATION_EQUAL:
		return 2;
	case VK_RECONCILE_RELATION_NEAR:
		return 1;
	case VK_RECONCILE_RELATION_APART:
	default:
		return 0;
	}
}

/* A random amount: small, near a 2% edge of @near, or anywhere in range. */
static gint64
random_amount(
	gint64	near
){
	switch (g_test_rand_int_range(0, 4))
	{
	case 0:
		return (gint64)g_test_rand_int_range(-100000, 100000);
	case 1:
		return near + near / 50 + g_test_rand_int_range(-3, 4);
	case 2:
		return near - near / 50 + g_test_rand_int_range(-3, 4);
	default:
		return (gint64)(((guint64)g_test_rand_int() << 32) | (guint32)g_test_rand_int());
	}
}

/*
 * The kernel answers what the 128-bit C answered for every pair: equal at
 * the common exponent, within 2%, or apart -- including amounts whose
 * value at the common exponent leaves the 64-bit range, which the kernel
 * never computes.
 */
static void
test_kernels_ledger_reconcile_relation(void)
{
	static const gint64 edges[] = { 0, 1, -1, 49, 50, 51, 9800, 10200, 9799, 10201,
		G_MAXINT64, G_MININT64, G_MAXINT64 / 1000000, G_MININT64 / 1000000 };
	guint i, j, k;

	for (i = 0; i < G_N_ELEMENTS(edges); i++)
		for (j = 0; j < G_N_ELEMENTS(edges); j++)
			for (k = 0; k < 49; k++)
				g_assert_cmpint(kernel_relation(edges[i], k / 7, edges[j], k % 7), ==,
					reference_relation(edges[i], k / 7, edges[j], k % 7));

	for (i = 0; i < 200000; i++)
	{
		guint ae = (guint)g_test_rand_int_range(0, 8);
		guint be = (guint)g_test_rand_int_range(0, 8);
		gint64 a = random_amount((gint64)g_test_rand_int_range(-1000000, 1000000));
		gint64 b = random_amount(a);

		if (kernel_relation(a, ae, b, be) != reference_relation(a, ae, b, be))
			g_error("relation(%" G_GINT64_FORMAT "@%u, %" G_GINT64_FORMAT "@%u): kernel %d, reference %d",
				a, ae, b, be, kernel_relation(a, ae, b, be), reference_relation(a, ae, b, be));
	}
}

/* Calendar days and the score table. */
static void
test_kernels_ledger_reconcile_score(void)
{
	TlwOptI64 a = { TRUE, 2460000 };
	TlwOptI64 b = { TRUE, 2460003 };
	TlwOptI64 missing = { FALSE, 0 };
	TlwOptI64 d;

	d = vk_reconcile_distance(b, a);
	g_assert_true(d.has);
	g_assert_cmpint(d.val, ==, 3);
	d = vk_reconcile_distance(a, missing);
	g_assert_false(d.has);

	d = vk_reconcile_distance(a, b);
	g_assert_cmpint(vk_reconcile_score(VK_RECONCILE_RELATION_EQUAL, d, 1, FALSE), ==, VK_RECONCILE_SCORE_UNIQUE);
	g_assert_cmpint(vk_reconcile_score(VK_RECONCILE_RELATION_EQUAL, d, 2, FALSE), ==, VK_RECONCILE_SCORE_CLOSE);
	g_assert_cmpint(vk_reconcile_score(VK_RECONCILE_RELATION_NEAR, d, 1, TRUE), ==, VK_RECONCILE_SCORE_SIMILAR);
	g_assert_cmpint(vk_reconcile_score(VK_RECONCILE_RELATION_EQUAL, missing, 1, FALSE), ==, VK_RECONCILE_SCORE_NOTHING);
	g_assert_cmpint(vk_reconcile_points(VK_RECONCILE_SCORE_UNIQUE), ==, 100);
	g_assert_cmpint(vk_reconcile_points(VK_RECONCILE_SCORE_CLOSE), ==, 70);
	g_assert_cmpint(vk_reconcile_points(VK_RECONCILE_SCORE_SIMILAR), ==, 40);
	g_assert_true(vk_reconcile_exact_near(VK_RECONCILE_RELATION_EQUAL, d));
}

/* --- vk_budget ------------------------------------------------------------- */

/* "YYYY" and "YYYY-MM", in range, and nothing else. */
static void
test_kernels_ledger_budget_period(void)
{
	g_autoptr(VkBudgetPeriod) year = vk_budget_period("2026");
	g_autoptr(VkBudgetPeriod) month = vk_budget_period("2026-03");
	g_autoptr(VkBudgetPeriod) bad_month = vk_budget_period("2026-13");
	g_autoptr(VkBudgetPeriod) zero = vk_budget_period("0000");
	g_autoptr(VkBudgetPeriod) late = vk_budget_period("9999");
	g_autoptr(VkBudgetPeriod) trailing = vk_budget_period("2026-03x");
	g_autoptr(VkBudgetPeriod) empty = vk_budget_period("");

	g_assert_cmpint(year->tag, ==, VK_BUDGET_PERIOD_YEAR);
	g_assert_cmpint(year->u.year.year, ==, 2026);
	g_assert_cmpint(month->tag, ==, VK_BUDGET_PERIOD_MONTH);
	g_assert_cmpint(month->u.month.month, ==, 3);
	g_assert_cmpint(bad_month->tag, ==, VK_BUDGET_PERIOD_INVALID);
	g_assert_cmpint(zero->tag, ==, VK_BUDGET_PERIOD_INVALID);
	g_assert_cmpint(late->tag, ==, VK_BUDGET_PERIOD_INVALID);
	g_assert_cmpint(trailing->tag, ==, VK_BUDGET_PERIOD_INVALID);
	g_assert_cmpint(empty->tag, ==, VK_BUDGET_PERIOD_INVALID);
}

/* A line's own period, else its plan's; a year covers its months. */
static void
test_kernels_ledger_budget_covers(void)
{
	g_assert_true(vk_budget_covers("2026", NULL, "2026-03"));
	g_assert_true(vk_budget_covers("", "2026", "2026-05"));
	g_assert_true(vk_budget_covers(NULL, "2026-05", "2026-05"));
	g_assert_false(vk_budget_covers("2026-03", NULL, "2026"));
	g_assert_false(vk_budget_covers("2025", "2026", "2026"));
	g_assert_false(vk_budget_covers(NULL, NULL, "2026"));
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
	g_test_add_func("/kernels-ledger/holdings/floor", test_kernels_ledger_holdings_floor);
	g_test_add_func("/kernels-ledger/reconcile/relation", test_kernels_ledger_reconcile_relation);
	g_test_add_func("/kernels-ledger/reconcile/score", test_kernels_ledger_reconcile_score);
	g_test_add_func("/kernels-ledger/budget/period", test_kernels_ledger_budget_period);
	g_test_add_func("/kernels-ledger/budget/covers", test_kernels_ledger_budget_covers);

	return g_test_run();
}
