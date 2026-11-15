/*
 * test-kernels-dates.c - The date, late-fee and dunning kernels through their
 * generated C API
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * vk_dates, vk_latefees and vk_dunning test themselves (`make kernels-check`).
 * This suite checks them from the side VENTURE calls: the day numbers agree
 * with GDate on every day it can hold, a recurring step lands where the GLib
 * calls the kernels replaced landed, and one regression case for each bug the
 * move fixed. The database-backed regressions live in the area suites
 * (test-dunning, test-late-fees).
 */

#include <venture.h>
#include <vk-kernels.h>

/* --- vk_dates -------------------------------------------------------------- */

/*
 * Every day GDate can hold, both ways: the kernel's day number is
 * g_date_get_julian, and the date it reads back is the GDate's.
 */
static void
test_kernels_dates_julian(void)
{
	GDate date;
	guint32 j;

	g_date_clear(&date, 1);
	g_date_set_dmy(&date, 1, G_DATE_JANUARY, 1);
	for (j = 1; j <= 3652059; j++)
	{
		gint64 y = (gint64)g_date_get_year(&date);
		gint64 m = (gint64)g_date_get_month(&date);
		gint64 d = (gint64)g_date_get_day(&date);

		g_assert_cmpuint(g_date_get_julian(&date), ==, j);
		if ((gint64)j != vk_dates_julian(y, m, d) || y != vk_dates_year_of((gint64)j) ||
		    m != vk_dates_month_of((gint64)j) || d != vk_dates_day_of((gint64)j))
		{
			g_error("day %u (%04" G_GINT64_FORMAT "-%02" G_GINT64_FORMAT "-%02" G_GINT64_FORMAT
			        ") disagrees with GDate", j, y, m, d);
		}
		if (j < 3652059)
			g_date_add_days(&date, 1);
	}
}

/* The day difference and the day after, at the edges of the calendar. */
static void
test_kernels_dates_days(void)
{
	TlwOptI64 next;

	g_assert_cmpint(vk_dates_calendar_days(vk_dates_julian(2026, 1, 31), vk_dates_julian(2026, 3, 1)), ==, 29);
	g_assert_cmpint(vk_dates_calendar_days(vk_dates_julian(2024, 3, 1), vk_dates_julian(2024, 1, 31)), ==, -30);

	next = vk_dates_day_after(vk_dates_julian(2026, 12, 31), 1);
	g_assert_true(next.has);
	g_assert_cmpint(vk_dates_ymd(next.val), ==, 20270101);

	g_assert_false(vk_dates_day_after(3652059, 1).has);
	g_assert_false(vk_dates_day_after(1, -1).has);
}

/*
 * A recurring cycle, as the recurring shim computes it (the kernel's day,
 * then g_date_time_add_days from the start), lands on exactly the instant
 * g_date_time_add_days / _add_months / _add_years gave: same day, the month
 * end held, the local time kept across daylight saving.
 */
static void
test_kernels_dates_cycle_matches_glib(void)
{
	static const gint starts[][3] = {
		{ 2026, 1, 31 }, { 2024, 2, 29 }, { 2026, 3, 8 }, { 2026, 10, 31 }, { 2025, 12, 31 }, { 9998, 6, 30 },
	};
	static const VkDatesFrequency steps[] = {
		VK_DATES_FREQUENCY_DAILY, VK_DATES_FREQUENCY_WEEKLY, VK_DATES_FREQUENCY_MONTHLY, VK_DATES_FREQUENCY_YEARLY,
	};
	g_autoptr(GTimeZone) zone = g_time_zone_new_identifier("America/New_York");
	guint s, k;
	gint64 index;

	g_assert_nonnull(zone);
	for (s = 0; s < G_N_ELEMENTS(starts); s++)
	{
		g_autoptr(GDateTime) start = g_date_time_new(zone, starts[s][0], starts[s][1], starts[s][2], 1, 30, 0);
		gint64 first = vk_dates_julian(starts[s][0], starts[s][1], starts[s][2]);

		for (k = 0; k < G_N_ELEMENTS(steps); k++)
			for (index = 0; index < 400; index++)
			{
				g_autoptr(GDateTime) expected = NULL;
				g_autoptr(GDateTime) got = NULL;
				TlwOptI64 day;

				switch (steps[k])
				{
				case VK_DATES_FREQUENCY_DAILY:
					expected = g_date_time_add_days(start, (gint)index);
					break;
				case VK_DATES_FREQUENCY_WEEKLY:
					expected = g_date_time_add_days(start, 7 * (gint)index);
					break;
				case VK_DATES_FREQUENCY_YEARLY:
					expected = g_date_time_add_years(start, (gint)index);
					break;
				case VK_DATES_FREQUENCY_MONTHLY:
				default:
					expected = g_date_time_add_months(start, (gint)index);
					break;
				}
				day = vk_dates_cycle_step(first, steps[k], index);
				if (day.has)
					got = g_date_time_add_days(start, (gint)(day.val - first));
				g_assert_true((expected == NULL) == (got == NULL));
				if (expected != NULL)
					g_assert_true(g_date_time_equal(expected, got));
			}
	}
	/* the largest index the shim lets through runs off the calendar */
	g_assert_false(vk_dates_cycle_step(vk_dates_julian(2026, 1, 1), VK_DATES_FREQUENCY_YEARLY, G_MAXINT / 7).has);
}

/* --- vk_latefees ----------------------------------------------------------- */

/*
 * A tier's "up" rule rounds away from zero. The C rounded a negative base
 * toward zero (it stepped the truncated quotient only on a positive
 * remainder), so 1.5% of -100.50 came to -1.50 instead of -1.51.
 */
static void
test_kernels_latefees_up_negative(void)
{
	TlwOptI64 fee;

	fee = vk_latefees_percent_of(-10050, 2, 15000, VK_LATEFEES_ROUNDING_UP);
	g_assert_true(fee.has);
	g_assert_cmpint(fee.val, ==, -151);

	fee = vk_latefees_percent_of(10050, 2, 15000, VK_LATEFEES_ROUNDING_UP);
	g_assert_true(fee.has);
	g_assert_cmpint(fee.val, ==, 151);

	fee = vk_latefees_percent_of(-10050, 2, 15000, VK_LATEFEES_ROUNDING_DOWN);
	g_assert_true(fee.has);
	g_assert_cmpint(fee.val, ==, -150);
}

/* Every rule on one base, and the overflow the C refused stays refused. */
static void
test_kernels_latefees_rules(void)
{
	TlwOptI64 fee;

	fee = vk_latefees_percent_of(10050, 2, 15000, VK_LATEFEES_ROUNDING_HALF_EVEN);
	g_assert_true(fee.has);
	g_assert_cmpint(fee.val, ==, 151);
	fee = vk_latefees_percent_of(10050, 2, 15000, VK_LATEFEES_ROUNDING_WHOLE);
	g_assert_true(fee.has);
	g_assert_cmpint(fee.val, ==, 200);
	fee = vk_latefees_percent_of(10050, 0, 15000, VK_LATEFEES_ROUNDING_WHOLE);
	g_assert_true(fee.has);
	g_assert_cmpint(fee.val, ==, 151);
	g_assert_false(vk_latefees_percent_of(G_MAXINT64, 2, 15000, VK_LATEFEES_ROUNDING_UP).has);
	g_assert_false(vk_latefees_percent_of(G_MAXINT64, 2, 15000, VK_LATEFEES_ROUNDING_DOWN).has);
}

/* A tier's days: every 30 from 30 to 360; the grace counts toward times. */
static void
test_kernels_latefees_occurrences(void)
{
	g_autoptr(GArray) every = vk_latefees_occurrences(30, 30, 0, 0);
	g_autoptr(GArray) graced = vk_latefees_occurrences(5, 10, 3, 15);

	g_assert_cmpuint(every->len, ==, 12);
	g_assert_cmpint(g_array_index(every, gint64, 0), ==, 30);
	g_assert_cmpint(g_array_index(every, gint64, 11), ==, 360);
	g_assert_cmpuint(graced->len, ==, 1);
	g_assert_cmpint(g_array_index(graced, gint64, 0), ==, 25);
}

/* The caps, in order, each cutting the fee to the room left. */
static void
test_kernels_latefees_caps(void)
{
	TlwOptI64 none = { FALSE, 0 };
	TlwOptI64 cap = { TRUE, 700 };
	TlwOptI64 total = { TRUE, 1000 };
	TlwOptI64 rate = { TRUE, 50000 };

	g_assert_cmpint(vk_latefees_occurrence_amount(500, none, 0, none, none, 10000, 0), ==, 500);
	g_assert_cmpint(vk_latefees_occurrence_amount(500, cap, 400, none, none, 10000, 0), ==, 300);
	g_assert_cmpint(vk_latefees_occurrence_amount(500, none, 0, total, none, 10000, 800), ==, 200);
	g_assert_cmpint(vk_latefees_occurrence_amount(500, none, 0, none, rate, 10000, 450), ==, 50);
	g_assert_cmpint(vk_latefees_occurrence_amount(500, cap, 900, total, rate, 10000, 0), ==, 0);
}

/* --- vk_dunning ------------------------------------------------------------ */

/* The next step after the highest recorded offset, and the run due today. */
static void
test_kernels_dunning_steps(void)
{
	static const gint64 values[] = { -3, 0, 7, 14 };
	g_autoptr(GArray) offsets = g_array_new(FALSE, FALSE, sizeof(gint64));
	g_autoptr(GArray) due = NULL;
	g_autoptr(GArray) waiting = NULL;
	TlwOptI64 none = { FALSE, 0 };
	TlwOptI64 caught_up = { TRUE, 20 };
	TlwOptI64 next;

	g_array_append_vals(offsets, values, G_N_ELEMENTS(values));
	next = vk_dunning_next_step(offsets, TRUE, 0);
	g_assert_true(next.has);
	g_assert_cmpint(next.val, ==, 2);
	g_assert_false(vk_dunning_next_step(offsets, TRUE, 14).has);

	due = vk_dunning_pending(offsets, FALSE, 0, none, 7);
	g_assert_cmpuint(due->len, ==, 3);

	/* after a catch-up at 7 on adoption day 20, the step at 14 waits for 27 */
	g_assert_cmpint(vk_dunning_eligible_day(14, 7, caught_up), ==, 27);
	waiting = vk_dunning_pending(offsets, TRUE, 7, caught_up, 26);
	g_assert_cmpuint(waiting->len, ==, 0);
}

int
main(
	int	  argc,
	char	**argv
){
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/kernels/dates/julian", test_kernels_dates_julian);
	g_test_add_func("/kernels/dates/days", test_kernels_dates_days);
	g_test_add_func("/kernels/dates/cycle-matches-glib", test_kernels_dates_cycle_matches_glib);
	g_test_add_func("/kernels/latefees/up-negative", test_kernels_latefees_up_negative);
	g_test_add_func("/kernels/latefees/rules", test_kernels_latefees_rules);
	g_test_add_func("/kernels/latefees/occurrences", test_kernels_latefees_occurrences);
	g_test_add_func("/kernels/latefees/caps", test_kernels_latefees_caps);
	g_test_add_func("/kernels/dunning/steps", test_kernels_dunning_steps);

	return g_test_run();
}
