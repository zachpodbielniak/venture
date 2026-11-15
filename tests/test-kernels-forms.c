/*
 * test-kernels-forms.c - The forms kernel through its generated C API
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * vk_forms (src/kernels) tests itself (`make kernels-check`). This suite
 * tests it from the side VENTURE uses: the vk_forms_* functions `tallow
 * render` writes, called the way venture-forms-quiz.c and
 * venture-forms-pricing.c call them. The forms suite (test-forms) covers
 * the same arithmetic end to end through published forms.
 */

#include <venture.h>
#include <math.h>
#include <vk-kernels.h>

/* An array of gint64 from a C array. */
static GArray *
array_of(const gint64 *values, guint n)
{
	GArray *array;

	array = g_array_new(FALSE, FALSE, sizeof(gint64));
	g_array_append_vals(array, values, n);
	return array;
}

/*
 * A numeric answer earns the points of every range it falls in, bounds
 * included, in the ranges' order.
 */
static void
test_kernels_forms_ranges(void)
{
	g_autoptr(GPtrArray) ranges = NULL;
	g_autoptr(GArray) earned = NULL;
	g_autoptr(GArray) none_earned = NULL;

	ranges = g_ptr_array_new_with_free_func((GDestroyNotify)vk_forms_range_free);
	g_ptr_array_add(ranges, vk_forms_range_new(0.0, 2.0, 1));
	g_ptr_array_add(ranges, vk_forms_range_new(3.0, 4.0, 5));
	g_ptr_array_add(ranges, vk_forms_range_new(4.0, 9.5, -2));

	earned = vk_forms_ranged(4.0, ranges);
	g_assert_cmpuint(earned->len, ==, 2);
	g_assert_cmpint(g_array_index(earned, gint64, 0), ==, 5);
	g_assert_cmpint(g_array_index(earned, gint64, 1), ==, -2);

	none_earned = vk_forms_ranged(2.5, ranges);
	g_assert_cmpuint(none_earned->len, ==, 0);
}

/*
 * The score is the running sum, checked at every step: a sum that leaves
 * the i64 range is refused even when a later point would bring it back.
 */
static void
test_kernels_forms_tally(void)
{
	const gint64 some[] = { 5, -2, 7 };
	const gint64 over[] = { 1, -1 };
	const gint64 under[] = { -1, 1 };
	g_autoptr(GArray) a = array_of(some, G_N_ELEMENTS(some));
	g_autoptr(GArray) b = array_of(over, G_N_ELEMENTS(over));
	g_autoptr(GArray) c = array_of(under, G_N_ELEMENTS(under));
	g_autoptr(VkFormsScore) scored = NULL;
	g_autoptr(VkFormsScore) out = NULL;
	g_autoptr(VkFormsScore) back = NULL;

	scored = vk_forms_tally(10, a);
	g_assert_cmpint(scored->tag, ==, VK_FORMS_SCORE_SCORED);
	g_assert_cmpint(scored->u.scored.points, ==, 20);

	out = vk_forms_tally(G_MAXINT64, b);
	g_assert_cmpint(out->tag, ==, VK_FORMS_SCORE_OUT_OF_RANGE);

	back = vk_forms_tally(G_MAXINT64, c);
	g_assert_cmpint(back->tag, ==, VK_FORMS_SCORE_SCORED);
	g_assert_cmpint(back->u.scored.points, ==, G_MAXINT64);
}

/* The first band whose inclusive bounds hold the score, or none. */
static void
test_kernels_forms_band(void)
{
	g_autoptr(GPtrArray) bands = NULL;
	TlwOptI64 found;

	bands = g_ptr_array_new_with_free_func((GDestroyNotify)vk_forms_band_free);
	g_ptr_array_add(bands, vk_forms_band_new(0, 4));
	g_ptr_array_add(bands, vk_forms_band_new(5, 9));

	found = vk_forms_band(5, bands);
	g_assert_true(found.has);
	g_assert_cmpint(found.val, ==, 1);
	found = vk_forms_band(4, bands);
	g_assert_true(found.has);
	g_assert_cmpint(found.val, ==, 0);
	found = vk_forms_band(10, bands);
	g_assert_false(found.has);
}

/*
 * The score report: sorted replies counted into one bin per (version,
 * score), each with its version's total, so the shares of one version sum
 * to one.
 */
static void
test_kernels_forms_bins(void)
{
	g_autoptr(GPtrArray) replies = NULL;
	g_autoptr(GPtrArray) bins = NULL;
	const gint64 expected[][4] = {
		{ 1, 3, 2, 3 },
		{ 1, 5, 1, 3 },
		{ 2, -3, 1, 2 },
		{ 2, 4, 1, 2 },
	};
	guint i;

	replies = g_ptr_array_new_with_free_func((GDestroyNotify)vk_forms_reply_free);
	g_ptr_array_add(replies, vk_forms_reply_new(1, 3));
	g_ptr_array_add(replies, vk_forms_reply_new(1, 3));
	g_ptr_array_add(replies, vk_forms_reply_new(1, 5));
	g_ptr_array_add(replies, vk_forms_reply_new(2, -3));
	g_ptr_array_add(replies, vk_forms_reply_new(2, 4));

	bins = vk_forms_bins(replies);
	g_assert_cmpuint(bins->len, ==, G_N_ELEMENTS(expected));
	for (i = 0; i < bins->len; i++)
	{
		const VkFormsBin *bin = g_ptr_array_index(bins, i);

		g_assert_cmpint(bin->version, ==, expected[i][0]);
		g_assert_cmpint(bin->score, ==, expected[i][1]);
		g_assert_cmpint(bin->count, ==, expected[i][2]);
		g_assert_cmpint(bin->of, ==, expected[i][3]);
	}
}

/*
 * A quantity is a whole count from 0 to a million; anything else, NaN
 * and infinity included, is refused.
 */
static void
test_kernels_forms_quantity(void)
{
	const gdouble refused[] = { -1.0, 2.5, 1000001.0, NAN, INFINITY };
	TlwOptI64 whole;
	guint i;

	whole = vk_forms_quantity(3.0);
	g_assert_true(whole.has);
	g_assert_cmpint(whole.val, ==, 3);
	whole = vk_forms_quantity(1000000.0);
	g_assert_true(whole.has);
	g_assert_cmpint(whole.val, ==, 1000000);
	whole = vk_forms_quantity(0.0);
	g_assert_true(whole.has);
	g_assert_cmpint(whole.val, ==, 0);

	for (i = 0; i < G_N_ELEMENTS(refused); i++)
	{
		whole = vk_forms_quantity(refused[i]);
		g_assert_false(whole.has);
	}
}

/*
 * A line's amount is exact or refused; the order total is made at the
 * lines' common exponent, as adding VentureMoney does, and must be
 * positive.
 */
static void
test_kernels_forms_order(void)
{
	g_autoptr(GPtrArray) mixed = NULL;
	g_autoptr(GPtrArray) empty = NULL;
	g_autoptr(GPtrArray) huge = NULL;
	g_autoptr(VkFormsOrder) total = NULL;
	g_autoptr(VkFormsOrder) nothing = NULL;
	g_autoptr(VkFormsOrder) overflow = NULL;
	TlwOptI64 amount;

	amount = vk_forms_line_amount(1250, 3);
	g_assert_true(amount.has);
	g_assert_cmpint(amount.val, ==, 3750);
	amount = vk_forms_line_amount(G_MAXINT64 / 2 + 1, 2);
	g_assert_false(amount.has);

	mixed = g_ptr_array_new_with_free_func((GDestroyNotify)vk_forms_line_free);
	g_ptr_array_add(mixed, vk_forms_line_new(3750, 2));
	g_ptr_array_add(mixed, vk_forms_line_new(5, 3));
	total = vk_forms_order(mixed);
	g_assert_cmpint(total->tag, ==, VK_FORMS_ORDER_PAYABLE);
	g_assert_cmpint(total->u.payable.amount, ==, 37505);
	g_assert_cmpint(total->u.payable.exponent, ==, 3);

	empty = g_ptr_array_new_with_free_func((GDestroyNotify)vk_forms_line_free);
	nothing = vk_forms_order(empty);
	g_assert_cmpint(nothing->tag, ==, VK_FORMS_ORDER_NOT_POSITIVE);

	huge = g_ptr_array_new_with_free_func((GDestroyNotify)vk_forms_line_free);
	g_ptr_array_add(huge, vk_forms_line_new(G_MAXINT64, 2));
	g_ptr_array_add(huge, vk_forms_line_new(1, 2));
	overflow = vk_forms_order(huge);
	g_assert_cmpint(overflow->tag, ==, VK_FORMS_ORDER_OVERFLOW);
}

int
main(
	int	  argc,
	char	**argv
){
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/kernels-forms/quiz/ranges", test_kernels_forms_ranges);
	g_test_add_func("/kernels-forms/quiz/tally", test_kernels_forms_tally);
	g_test_add_func("/kernels-forms/quiz/band", test_kernels_forms_band);
	g_test_add_func("/kernels-forms/quiz/bins", test_kernels_forms_bins);
	g_test_add_func("/kernels-forms/pricing/quantity", test_kernels_forms_quantity);
	g_test_add_func("/kernels-forms/pricing/order", test_kernels_forms_order);

	return g_test_run();
}
