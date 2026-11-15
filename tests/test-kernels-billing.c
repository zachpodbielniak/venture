/*
 * test-kernels-billing.c - The depreciation, billing and line kernels
 * through their generated C API
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * vk_depreciation, vk_billing and vk_lines (src/kernels) test themselves
 * (`make kernels-check`). This suite tests them from the side VENTURE
 * uses: the vk_* functions `tallow render` writes, called the way the
 * shims call them, and the public C API built on them. A regression case
 * here fails on the C the kernels replaced.
 */

#include <venture.h>
#include <vk-kernels.h>

/* The sum of a kernel's gint64 array. */
static gint64
sum_of(GArray *parts)
{
	gint64 total;
	guint i;

	total = 0;
	for (i = 0; i < parts->len; i++)
		total += g_array_index(parts, gint64, i);
	return total;
}

/* An array of gint64 from a C array. */
static GArray *
array_of(const gint64 *values, guint n)
{
	GArray *array;

	array = g_array_new(FALSE, FALSE, sizeof(gint64));
	g_array_append_vals(array, values, n);
	return array;
}

/* --- vk_depreciation ------------------------------------------------------- */

/*
 * Straight line: the even share every month, every leftover unit on the
 * last, the register's convention. Declining balance: twice the
 * straight-line rate of the book value, half to even, stopping at what is
 * left to charge. Both sum to cost less salvage and never go negative.
 */
static void
test_kernels_depreciation_schedule(void)
{
	g_autoptr(GArray) straight = NULL;
	g_autoptr(GArray) declining = NULL;
	g_autoptr(GArray) stopped = NULL;
	guint i;

	straight = vk_depreciation_schedule(100000, 10000, 7, VK_DEPRECIATION_METHOD_STRAIGHT_LINE);
	g_assert_nonnull(straight);
	g_assert_cmpuint(straight->len, ==, 7);
	for (i = 0; i + 1 < straight->len; i++)
		g_assert_cmpint(g_array_index(straight, gint64, i), ==, 12857);
	g_assert_cmpint(g_array_index(straight, gint64, 6), ==, 12858);
	g_assert_cmpint(sum_of(straight), ==, 90000);

	declining = vk_depreciation_schedule(120000, 0, 12, VK_DEPRECIATION_METHOD_DECLINING_BALANCE);
	g_assert_nonnull(declining);
	g_assert_cmpint(g_array_index(declining, gint64, 0), ==, 20000);
	g_assert_cmpint(g_array_index(declining, gint64, 1), ==, 16667);
	g_assert_cmpint(sum_of(declining), ==, 120000);
	for (i = 0; i < declining->len; i++)
		g_assert_cmpint(g_array_index(declining, gint64, i), >=, 0);

	/* Salvage stops the charges once the basis is used up. */
	stopped = vk_depreciation_schedule(10000, 6000, 4, VK_DEPRECIATION_METHOD_DECLINING_BALANCE);
	g_assert_nonnull(stopped);
	g_assert_cmpint(g_array_index(stopped, gint64, 0), ==, 4000);
	g_assert_cmpint(g_array_index(stopped, gint64, 3), ==, 0);
}

/*
 * A resumed schedule (an opening import) and a book value held at fewer
 * decimals than the basis: the charge rounds at the book value's own
 * exponent (the grain), as multiplying that money did.
 */
static void
test_kernels_depreciation_resume(void)
{
	g_autoptr(GArray) resumed = NULL;
	g_autoptr(GArray) coarse = NULL;

	resumed = vk_depreciation_resume(600, 800, 3, 12, 1, VK_DEPRECIATION_METHOD_DECLINING_BALANCE);
	g_assert_nonnull(resumed);
	g_assert_cmpint(g_array_index(resumed, gint64, 0), ==, 133);
	g_assert_cmpint(g_array_index(resumed, gint64, 1), ==, 111);
	g_assert_cmpint(g_array_index(resumed, gint64, 2), ==, 356);

	coarse = vk_depreciation_resume(9995, 10000, 2, 4, 10, VK_DEPRECIATION_METHOD_DECLINING_BALANCE);
	g_assert_nonnull(coarse);
	g_assert_cmpint(g_array_index(coarse, gint64, 0), ==, 5000);
	g_assert_cmpint(g_array_index(coarse, gint64, 1), ==, 4995);
}

/* The half-year convention moves the untaken half of month one to the
 * last month; one month has nowhere to move it. The input is unchanged. */
static void
test_kernels_depreciation_convene(void)
{
	static const gint64 values[] = { 101, 100, 100 };
	g_autoptr(GArray) parts = NULL;
	g_autoptr(GArray) moved = NULL;
	g_autoptr(GArray) single = NULL;
	g_autoptr(GArray) one = NULL;

	parts = array_of(values, G_N_ELEMENTS(values));
	moved = vk_depreciation_convene(parts, VK_DEPRECIATION_CONVENTION_HALF_YEAR);
	g_assert_nonnull(moved);
	g_assert_cmpint(g_array_index(moved, gint64, 0), ==, 50);
	g_assert_cmpint(g_array_index(moved, gint64, 2), ==, 151);
	g_assert_cmpint(g_array_index(parts, gint64, 0), ==, 101);

	one = array_of(values, 1);
	single = vk_depreciation_convene(one, VK_DEPRECIATION_CONVENTION_HALF_YEAR);
	g_assert_nonnull(single);
	g_assert_cmpint(g_array_index(single, gint64, 0), ==, 101);
}

/* --- vk_billing ------------------------------------------------------------ */

/* Stored intervals read back by number; anything unknown is a month. */
static void
test_kernels_billing_interval(void)
{
	g_assert_cmpint(venture_billing_interval_months(0), ==, 1);
	g_assert_cmpint(venture_billing_interval_months(1), ==, 12);
	g_assert_cmpint(venture_billing_interval_months(2), ==, 3);
	g_assert_cmpint(venture_billing_interval_months(3), ==, 6);
	g_assert_cmpint(venture_billing_interval_months(7), ==, 1);
	g_assert_cmpint(venture_billing_interval_months(-1), ==, 1);
	g_assert_cmpint(vk_billing_served(vk_billing_months_between(2025, 11, 2026, 11)), ==, 12);
	g_assert_cmpint(vk_billing_served(vk_billing_months_between(2026, 1, 2026, 3)), ==, 1);
}

/*
 * The closed-form share of the days left equals what it replaced: an even
 * spread of the amount over every day, summed over the days not yet
 * elapsed -- for every period length, every change day and both signs.
 */
static void
test_kernels_billing_tail_share(void)
{
	static const gint64 deltas[] = { 3000, -3000, 1, -1, 99999, -100, 0 };
	gint64 days;
	gint64 elapsed;
	guint k;

	for (k = 0; k < G_N_ELEMENTS(deltas); k++)
	{
		for (days = 1; days <= 366; days += 5)
		{
			g_autoptr(VentureMoney) delta = venture_money_new(deltas[k], "USD", 2);
			g_autoptr(GPtrArray) parts = venture_money_allocate_evenly(delta, (gsize)days, NULL);
			gint64 expected = 0;
			gint64 i;

			g_assert_nonnull(parts);
			for (elapsed = days - 1; elapsed >= 0; elapsed--)
			{
				expected = 0;
				for (i = elapsed; i < days; i++)
					expected += venture_money_get_amount(g_ptr_array_index(parts, (guint)i));
				g_assert_true(vk_billing_inside(days, elapsed));
				g_assert_cmpint(vk_billing_tail_share(deltas[k], days, elapsed), ==, expected);
			}
		}
	}
	g_assert_false(vk_billing_inside(31, 31));
	g_assert_false(vk_billing_inside(367, 0));
	g_assert_false(vk_billing_inside(31, -1));
}

/* A percent off rounds half to even; an amount off never goes below
 * zero, at the finer of the two exponents. */
static void
test_kernels_billing_discount(void)
{
	g_autoptr(VkBillingDiscount) percent = vk_billing_discount_new_percent_off(50);
	g_autoptr(VkBillingDiscount) amount = vk_billing_discount_new_amount_off(2500, 3);
	g_autoptr(VkBillingDiscount) larger = vk_billing_discount_new_amount_off(2000, 2);
	g_autoptr(VkBillingTaken) half = NULL;
	g_autoptr(VkBillingTaken) less = NULL;
	g_autoptr(VkBillingTaken) free = NULL;

	half = vk_billing_take(999, 2, percent);
	g_assert_nonnull(half);
	g_assert_cmpint(half->tag, ==, VK_BILLING_TAKEN_NET);
	g_assert_cmpint(half->u.net.amount, ==, 500);

	less = vk_billing_take(1000, 2, amount);
	g_assert_nonnull(less);
	g_assert_cmpint(less->u.net.amount, ==, 7500);

	free = vk_billing_take(1000, 2, larger);
	g_assert_nonnull(free);
	g_assert_cmpint(free->tag, ==, VK_BILLING_TAKEN_FREE);
}

/* The report's buckets and rates. */
static void
test_kernels_billing_reports(void)
{
	TlwOptI64 r;

	g_assert_cmpint(vk_billing_movement(0, 100, 100, FALSE), ==, VK_BILLING_MOVEMENT_NEW);
	g_assert_cmpint(vk_billing_movement(0, 100, 100, TRUE), ==, VK_BILLING_MOVEMENT_REACTIVATION);
	g_assert_cmpint(vk_billing_movement(100, 0, -100, TRUE), ==, VK_BILLING_MOVEMENT_CHURN);
	g_assert_cmpint(vk_billing_movement(100, 120, 20, TRUE), ==, VK_BILLING_MOVEMENT_EXPANSION);
	g_assert_cmpint(vk_billing_movement(100, 80, -20, TRUE), ==, VK_BILLING_MOVEMENT_CONTRACTION);
	g_assert_cmpint(vk_billing_movement(100, 100, 0, TRUE), ==, VK_BILLING_MOVEMENT_FLAT);
	r = vk_billing_magnitude(G_MININT64);
	g_assert_false(r.has);
	r = vk_billing_rate_bp(1, 3);
	g_assert_true(r.has);
	g_assert_cmpint(r.val, ==, 3333);
	g_assert_cmpint(vk_billing_retained(r.val), ==, 6667);
	r = vk_billing_hundredths(5, 4);
	g_assert_true(r.has);
	g_assert_cmpint(r.val, ==, 125);
}

int
main(
	int	  argc,
	char	**argv
){
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/kernels-billing/depreciation/schedule", test_kernels_depreciation_schedule);
	g_test_add_func("/kernels-billing/depreciation/resume", test_kernels_depreciation_resume);
	g_test_add_func("/kernels-billing/depreciation/convene", test_kernels_depreciation_convene);
	g_test_add_func("/kernels-billing/billing/interval", test_kernels_billing_interval);
	g_test_add_func("/kernels-billing/billing/tail-share", test_kernels_billing_tail_share);
	g_test_add_func("/kernels-billing/billing/discount", test_kernels_billing_discount);
	g_test_add_func("/kernels-billing/billing/reports", test_kernels_billing_reports);

	return g_test_run();
}
