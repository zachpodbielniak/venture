/*
 * test-kernels.c - The Tallow kernels through their generated C API
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * the kernels in src/kernels test themselves (`make kernels-check`: their own test
 * blocks, the fuzzer and the SMT prover). This suite tests them from the
 * side VENTURE actually uses: the C functions `tallow render` writes
 * (vk_*), called the way the shims call them, plus one regression case for
 * each bug the move to kernels fixed. A case here fails on the C the kernel
 * replaced.
 */

#include <venture.h>
#include <vk-kernels.h>

/* --- vk_round -------------------------------------------------------------- */

/*
 * Half to even at the edges of the range. The C this replaced took the
 * magnitude of G_MININT64 by negating it, which overflows (undefined
 * behaviour, and in practice a wrong quotient).
 */
static void
test_kernels_round_min(void)
{
	g_assert_cmpint(vk_round_half_even(G_MININT64, 10), ==, G_GINT64_CONSTANT(-922337203685477581));
	g_assert_cmpint(vk_round_half_even(G_MAXINT64, 10), ==, G_GINT64_CONSTANT(922337203685477581));
	g_assert_cmpint(vk_round_div(G_MININT64, 1, VK_ROUND_MODE_HALF_EVEN), ==, G_MININT64);
}

/* Every rule on the same ties, signs mirrored. */
static void
test_kernels_round_modes(void)
{
	g_assert_cmpint(vk_round_div(25, 10, VK_ROUND_MODE_HALF_EVEN), ==, 2);
	g_assert_cmpint(vk_round_div(35, 10, VK_ROUND_MODE_HALF_EVEN), ==, 4);
	g_assert_cmpint(vk_round_div(-25, 10, VK_ROUND_MODE_HALF_EVEN), ==, -2);
	g_assert_cmpint(vk_round_div(25, 10, VK_ROUND_MODE_HALF_UP), ==, 3);
	g_assert_cmpint(vk_round_div(-25, 10, VK_ROUND_MODE_HALF_UP), ==, -3);
	g_assert_cmpint(vk_round_div(21, 10, VK_ROUND_MODE_UP), ==, 3);
	g_assert_cmpint(vk_round_div(-21, 10, VK_ROUND_MODE_UP), ==, -3);
	g_assert_cmpint(vk_round_div(29, 10, VK_ROUND_MODE_DOWN), ==, 2);
	g_assert_cmpint(vk_round_div(-29, 10, VK_ROUND_MODE_DOWN), ==, -2);
	g_assert_cmpint(vk_round_div(-21, 10, VK_ROUND_MODE_CEILING), ==, -2);
	g_assert_cmpint(vk_round_div(-21, 10, VK_ROUND_MODE_FLOOR), ==, -3);
}

/* a * b / d over a product that does not fit 64 bits. */
static void
test_kernels_round_mul_div(void)
{
	TlwOptI64 r;

	r = vk_round_mul_div(G_MAXINT64, 3, 4, VK_ROUND_MODE_HALF_EVEN);
	g_assert_true(r.has);
	g_assert_cmpint(r.val, ==, G_GINT64_CONSTANT(6917529027641081855));

	r = vk_round_mul_div(G_MAXINT64, 2, 1, VK_ROUND_MODE_HALF_EVEN);
	g_assert_false(r.has);
}

/* --- vk_money -------------------------------------------------------------- */

/*
 * Rescaling G_MININT64 to fewer decimals went through the same broken
 * negation; through the money API it now rounds like any other amount.
 */
static void
test_kernels_money_rescale_min(void)
{
	g_autoptr(VentureMoney) low = NULL;
	g_autoptr(VentureMoney) scaled = NULL;
	g_autoptr(GError) error = NULL;

	low = venture_money_new(G_MININT64, "USD", 2);
	scaled = venture_money_rescale(low, 1, &error);
	g_assert_no_error(error);
	g_assert_cmpint(venture_money_get_amount(scaled), ==, G_GINT64_CONSTANT(-922337203685477581));
}

/*
 * Multiplying by a ratio used to refuse any product that overflowed 64
 * bits, even when the rounded result fitted: three quarters of a very
 * large amount is a perfectly good amount.
 */
static void
test_kernels_money_rational_wide(void)
{
	g_autoptr(VentureMoney) big = NULL;
	g_autoptr(VentureMoney) part = NULL;
	g_autoptr(GError) error = NULL;

	big = venture_money_new(G_MAXINT64, "USD", 2);
	part = venture_money_multiply_rational(big, 3, 4, &error);
	g_assert_no_error(error);
	g_assert_cmpint(venture_money_get_amount(part), ==, G_GINT64_CONSTANT(6917529027641081855));
}

/*
 * Converting to a currency spelled in lowercase is still converting to
 * the same currency: the shim compares codes case-insensitively, like
 * every other money operation (it used g_strcmp0 here alone).
 */
static void
test_kernels_money_convert_case(void)
{
	g_autoptr(VentureMoney) dollars = NULL;
	g_autoptr(VentureMoney) same = NULL;
	g_autoptr(GError) error = NULL;

	dollars = venture_money_new(1234, "USD", 2);
	same = venture_money_convert_at_rate(dollars, 3, 2, "usd", &error);
	g_assert_no_error(error);
	g_assert_cmpint(venture_money_get_amount(same), ==, 1234);
}

/* The kernel's allocation sums back exactly, the leftover going to the
 * largest ratios first, ties by position. */
static void
test_kernels_money_allocate(void)
{
	g_autoptr(GArray) ratios = NULL;
	g_autoptr(GArray) parts = NULL;
	static const gint64 values[] = { 3, 3, 1 };

	ratios = g_array_new(FALSE, FALSE, sizeof(gint64));
	g_array_append_vals(ratios, values, G_N_ELEMENTS(values));
	parts = vk_money_allocate(5, ratios);
	g_assert_nonnull(parts);
	g_assert_cmpuint(parts->len, ==, 3);
	g_assert_cmpint(g_array_index(parts, gint64, 0), ==, 3);
	g_assert_cmpint(g_array_index(parts, gint64, 1), ==, 2);
	g_assert_cmpint(g_array_index(parts, gint64, 2), ==, 0);

	g_array_set_size(ratios, 0);
	g_assert_null(vk_money_allocate(5, ratios));
}

int
main(
	int	  argc,
	char	**argv
){
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/kernels/round/min", test_kernels_round_min);
	g_test_add_func("/kernels/round/modes", test_kernels_round_modes);
	g_test_add_func("/kernels/round/mul-div", test_kernels_round_mul_div);
	g_test_add_func("/kernels/money/rescale-min", test_kernels_money_rescale_min);
	g_test_add_func("/kernels/money/rational-wide", test_kernels_money_rational_wide);
	g_test_add_func("/kernels/money/convert-case", test_kernels_money_convert_case);
	g_test_add_func("/kernels/money/allocate", test_kernels_money_allocate);

	return g_test_run();
}
