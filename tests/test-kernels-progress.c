/*
 * test-kernels-progress.c - The progress kernel through its generated C API
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * vk_progress (src/kernels) tests itself (`make kernels-check`). This
 * suite tests it from the side VENTURE uses: the vk_progress_* functions
 * `tallow render` writes, called the way the goals report and the
 * dashboard's progress widget call them. A regression case here fails on
 * the C the kernel replaced.
 */

#include <venture.h>
#include <vk-kernels.h>

/* A day as a GTimeSpan. */
#define DAY (G_TIME_SPAN_DAY)

/* An elapsed time the kernel can take. */
static TlwOptI64
elapsed_of(gint64 microseconds)
{
	TlwOptI64 elapsed;

	elapsed.has = TRUE;
	elapsed.val = microseconds;
	return elapsed;
}

/* --- goals ---------------------------------------------------------------- */

/*
 * The fraction a goal has covered, both ways round, and a goal with no
 * distance reading as none rather than dividing by zero.
 */
static void
test_kernels_progress_fraction(void)
{
	g_assert_cmpfloat_with_epsilon(venture_goals_fraction(1.0, 150.5, 300.0), 0.5, 1e-9);
	g_assert_cmpfloat(venture_goals_fraction(90.0, 85.0, 80.0), ==, 0.5);
	g_assert_cmpfloat(venture_goals_fraction(0.0, 12.0, 10.0), ==, 1.2);
	g_assert_cmpfloat(venture_goals_fraction(5.0, 9.0, 5.0), ==, 0.0);
}

/*
 * Each forecast case, in the order the report checks them: no creation
 * date and nothing covered are no progress; an as_of at or before
 * creation is before the start; under a day is too soon; past a century
 * is too far, decided on the exact value (a century to the microsecond is
 * still a forecast).
 */
static void
test_kernels_progress_forecast(void)
{
	TlwOptI64 none_yet;
	struct
	{
		gint64				elapsed;
		gdouble				start;
		gdouble				current;
		gdouble				target;
		VkProgressForecastTag		tag;
		gint64				after;
	} cases[] = {
		{ 10 * DAY, 0.0, 50.0, 100.0, VK_PROGRESS_FORECAST_DUE, 10 * DAY },
		{ 10 * DAY, 90.0, 85.0, 80.0, VK_PROGRESS_FORECAST_DUE, 10 * DAY },
		{ 3 * DAY, 0.0, 1.0, 3.0, VK_PROGRESS_FORECAST_DUE, 6 * DAY },
		{ 10 * DAY, 0.0, 0.0, 100.0, VK_PROGRESS_FORECAST_NO_PROGRESS, 0 },
		{ 10 * DAY, 0.0, -5.0, 100.0, VK_PROGRESS_FORECAST_NO_PROGRESS, 0 },
		{ 10 * DAY, 5.0, 7.0, 5.0, VK_PROGRESS_FORECAST_NO_PROGRESS, 0 },
		{ 0, 0.0, 50.0, 100.0, VK_PROGRESS_FORECAST_BEFORE_START, 0 },
		{ -DAY, 0.0, 50.0, 100.0, VK_PROGRESS_FORECAST_BEFORE_START, 0 },
		{ 1, 0.0, 50.0, 100.0, VK_PROGRESS_FORECAST_TOO_SOON, 0 },
		{ DAY - 1, 0.0, 50.0, 100.0, VK_PROGRESS_FORECAST_TOO_SOON, 0 },
		{ DAY, 0.0, 0.000001, 1.0, VK_PROGRESS_FORECAST_TOO_FAR, 0 },
		{ 36525 * DAY, 0.0, 1.0, 2.0, VK_PROGRESS_FORECAST_DUE, 36525 * DAY },
		{ 36525 * DAY + 1, 0.0, 1.0, 2.0, VK_PROGRESS_FORECAST_TOO_FAR, 0 },
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(cases); i++)
	{
		g_autoptr(VkProgressForecast) forecast = NULL;

		forecast = vk_progress_forecast(elapsed_of(cases[i].elapsed), cases[i].start,
		                                cases[i].current, cases[i].target);
		g_assert_cmpint(forecast->tag, ==, cases[i].tag);
		if (VK_PROGRESS_FORECAST_DUE == cases[i].tag)
			g_assert_cmpint(forecast->u.due.after, ==, cases[i].after);
	}

	none_yet.has = FALSE;
	none_yet.val = 0;
	{
		g_autoptr(VkProgressForecast) forecast = NULL;

		forecast = vk_progress_forecast(none_yet, 0.0, 50.0, 100.0);
		g_assert_cmpint(forecast->tag, ==, VK_PROGRESS_FORECAST_NO_PROGRESS);
	}
}

/*
 * Regression: the forecast is one rounding of an exact rational. 27% of
 * the way in a day is 233600 seconds more, to the microsecond; the C's
 * double chain (86400 x (1 - 0.27) / 0.27 = 233599.99999999997) was cut
 * to whole microseconds by g_date_time_add_seconds, one short. Readings
 * too large for any exact scale go through the fraction, and a pace that
 * is not a number is too far, as the C found it.
 */
static void
test_kernels_progress_forecast_exact(void)
{
	g_autoptr(VkProgressForecast) share = NULL;
	g_autoptr(VkProgressForecast) huge = NULL;
	g_autoptr(VkProgressForecast) tiny = NULL;
	g_autoptr(VkProgressForecast) trillions = NULL;

	share = vk_progress_forecast(elapsed_of(DAY), 0.0, 27.0, 100.0);
	g_assert_cmpint(share->tag, ==, VK_PROGRESS_FORECAST_DUE);
	g_assert_cmpint(share->u.due.after, ==, 233600 * G_TIME_SPAN_SECOND);

	huge = vk_progress_forecast(elapsed_of(10 * DAY), 0.0, 1.0e300, 2.0e300);
	g_assert_cmpint(huge->tag, ==, VK_PROGRESS_FORECAST_DUE);
	g_assert_cmpint(huge->u.due.after, ==, 10 * DAY);

	tiny = vk_progress_forecast(elapsed_of(10 * DAY), 0.0, 1.0, 1.0e300);
	g_assert_cmpint(tiny->tag, ==, VK_PROGRESS_FORECAST_TOO_FAR);

	/* Readings in the trillions are read at a coarser scale, still exactly. */
	trillions = vk_progress_forecast(elapsed_of(10 * DAY), 0.0, 5.0e12, 1.0e13);
	g_assert_cmpint(trillions->tag, ==, VK_PROGRESS_FORECAST_DUE);
	g_assert_cmpint(trillions->u.due.after, ==, 10 * DAY);
}

/* --- dashboards ------------------------------------------------------------- */

/*
 * Money progress is covered over distance at the common exponent; equal
 * start and target is no share; amounts that do not fit are too large.
 */
static void
test_kernels_progress_money(void)
{
	g_autoptr(VkProgressProgress) quarter = NULL;
	g_autoptr(VkProgressProgress) down = NULL;
	g_autoptr(VkProgressProgress) flat = NULL;
	g_autoptr(VkProgressProgress) huge = NULL;
	TlwOptF64 percent;

	quarter = vk_progress_money_progress(2500, 2, 10000, 2, 0, 2);
	g_assert_cmpint(quarter->tag, ==, VK_PROGRESS_PROGRESS_RATIO);
	g_assert_cmpint(quarter->u.ratio.covered, ==, 2500);
	g_assert_cmpint(quarter->u.ratio.distance, ==, 10000);

	down = vk_progress_money_progress(85, 0, 80, 0, 90, 0);
	g_assert_cmpint(down->tag, ==, VK_PROGRESS_PROGRESS_RATIO);
	g_assert_cmpint(down->u.ratio.covered, ==, -5);
	g_assert_cmpint(down->u.ratio.distance, ==, -10);

	flat = vk_progress_money_progress(5, 0, 7, 0, 7, 0);
	g_assert_cmpint(flat->tag, ==, VK_PROGRESS_PROGRESS_UNDEFINED);

	huge = vk_progress_money_progress(G_MAXINT64, 0, 1, 2, 0, 0);
	g_assert_cmpint(huge->tag, ==, VK_PROGRESS_PROGRESS_TOO_LARGE);

	percent = vk_progress_number_percent(25.0, 100.0, 0.0);
	g_assert_true(percent.has);
	g_assert_cmpfloat(percent.val, ==, 25.0);
	percent = vk_progress_number_percent(25.0, 3.0, 3.0);
	g_assert_false(percent.has);
}

/*
 * Regression: a value finer than its target. The widget used to rescale
 * the value to the target's exponent first, rounding 1.005 to 1.00 (half
 * to even) and showing 10% of 10.00; compared at the common exponent it
 * is 10.05%.
 */
static void
test_kernels_progress_money_exponent(void)
{
	g_autoptr(VkProgressProgress) fine = NULL;
	gdouble percent;

	fine = vk_progress_money_progress(1005, 3, 1000, 2, 0, 2);
	g_assert_cmpint(fine->tag, ==, VK_PROGRESS_PROGRESS_RATIO);
	g_assert_cmpint(fine->u.ratio.covered, ==, 1005);
	g_assert_cmpint(fine->u.ratio.distance, ==, 10000);
	percent = 100.0 * (gdouble)fine->u.ratio.covered / (gdouble)fine->u.ratio.distance;
	g_assert_cmpfloat_with_epsilon(percent, 10.05, 1e-9);
}

int
main(
	int	  argc,
	char	**argv
){
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/kernels-progress/goals/fraction", test_kernels_progress_fraction);
	g_test_add_func("/kernels-progress/goals/forecast", test_kernels_progress_forecast);
	g_test_add_func("/kernels-progress/goals/forecast-exact", test_kernels_progress_forecast_exact);
	g_test_add_func("/kernels-progress/dashboard/money", test_kernels_progress_money);
	g_test_add_func("/kernels-progress/dashboard/money-exponent", test_kernels_progress_money_exponent);

	return g_test_run();
}
