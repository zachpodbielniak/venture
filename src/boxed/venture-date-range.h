/*
 * venture-date-range.h - Half-open time intervals
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Nearly every question VENTURE answers is scoped to a period: profit this
 * quarter, expenses last month, sales year to date. #VentureDateRange is the
 * single representation of such a period.
 *
 * The interval is half open -- the start is included, the end is not. That
 * makes adjacent periods tile without gaps or overlaps, so "January" and
 * "February" cannot both claim midnight on the first of February, and a
 * running total over twelve consecutive months adds up to the year exactly.
 *
 * Ranges carry their timezone, because "today" is a different set of
 * instants in different places, and a report bucketed by day must agree with
 * the operator's idea of when a day starts.
 */

#ifndef VENTURE_DATE_RANGE_H
#define VENTURE_DATE_RANGE_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

#define VENTURE_TYPE_DATE_RANGE (venture_date_range_get_type())

/**
 * VentureDateRange:
 *
 * A half-open interval `[start, end)`. Opaque; use the accessors.
 */
struct _VentureDateRange
{
	/*< private >*/
	GDateTime	*start;
	GDateTime	*end;
	gchar		*label;
};

GType
venture_date_range_get_type(void) G_GNUC_CONST;

/* --- Construction -------------------------------------------------------- */

/**
 * venture_date_range_new:
 * @start: the inclusive start of the interval
 * @end: the exclusive end of the interval
 *
 * Creates a range. @end must not precede @start; an inverted range is a
 * programming error and yields %NULL after a warning.
 *
 * Returns: (transfer full) (nullable): a new #VentureDateRange
 */
VentureDateRange *
venture_date_range_new(
	GDateTime	*start,
	GDateTime	*end
);

/**
 * venture_date_range_new_labelled:
 * @start: the inclusive start
 * @end: the exclusive end
 * @label: (nullable): a human-readable name such as "Q1 2026"
 *
 * As venture_date_range_new(), but attaches a label that report headers and
 * chart axes use instead of printing two timestamps.
 *
 * Returns: (transfer full) (nullable): a new #VentureDateRange
 */
VentureDateRange *
venture_date_range_new_labelled(
	GDateTime	*start,
	GDateTime	*end,
	const gchar	*label
);

/**
 * venture_date_range_new_day:
 * @day: any instant within the wanted day
 * @timezone: (nullable): the timezone defining the day boundary; %NULL means
 *   the local timezone
 *
 * Creates the range covering the calendar day containing @day.
 *
 * Returns: (transfer full): a new #VentureDateRange
 */
VentureDateRange *
venture_date_range_new_day(
	GDateTime	*day,
	GTimeZone	*timezone
);

/**
 * venture_date_range_new_month:
 * @year: the calendar year
 * @month: the month, 1-12
 * @timezone: (nullable): the timezone defining the month boundary
 *
 * Returns: (transfer full): the range covering that calendar month
 */
VentureDateRange *
venture_date_range_new_month(
	gint		 year,
	gint		 month,
	GTimeZone	*timezone
);

/**
 * venture_date_range_new_quarter:
 * @year: the calendar year
 * @quarter: the quarter, 1-4
 * @timezone: (nullable): the timezone defining the boundary
 *
 * Returns: (transfer full): the range covering that calendar quarter
 */
VentureDateRange *
venture_date_range_new_quarter(
	gint		 year,
	gint		 quarter,
	GTimeZone	*timezone
);

/**
 * venture_date_range_new_year:
 * @year: the calendar year
 * @timezone: (nullable): the timezone defining the boundary
 *
 * Returns: (transfer full): the range covering that calendar year
 */
VentureDateRange *
venture_date_range_new_year(
	gint		 year,
	GTimeZone	*timezone
);

/**
 * venture_date_range_new_fiscal_year:
 * @year: the year the fiscal year is named for
 * @start_month: the month the fiscal year begins, 1-12
 * @timezone: (nullable): the timezone defining the boundary
 *
 * Creates a fiscal year beginning on the first of @start_month. When
 * @start_month is not January the range begins in @year and ends in the
 * following calendar year, which is the convention accountants use.
 *
 * Returns: (transfer full): a new #VentureDateRange
 */
VentureDateRange *
venture_date_range_new_fiscal_year(
	gint		 year,
	gint		 start_month,
	GTimeZone	*timezone
);

/**
 * venture_date_range_new_last_days:
 * @days: how many days back to reach
 * @timezone: (nullable): the timezone defining the day boundary
 *
 * Creates the range covering the last @days whole days up to and including
 * today. Thirty days ending today, not thirty times twenty-four hours ending
 * now, so the answer does not change every second.
 *
 * Returns: (transfer full): a new #VentureDateRange
 */
VentureDateRange *
venture_date_range_new_last_days(
	guint		 days,
	GTimeZone	*timezone
);

/**
 * venture_date_range_new_all_time:
 *
 * Creates an unbounded range that contains every instant. Used when a report
 * should not filter by date at all.
 *
 * Returns: (transfer full): a new #VentureDateRange
 */
VentureDateRange *
venture_date_range_new_all_time(void);

/**
 * venture_date_range_parse:
 * @text: the period expression to parse
 * @timezone: (nullable): the operator's calendar, which decides what "today", "this month" and "this year" are; %NULL means local time
 * @fiscal_year_start_month: the month the fiscal year begins, 1-12
 * @error: (out) (optional): return location for a #GError
 *
 * Parses a period expression. Every boundary of the result is a midnight
 * UTC, whatever @timezone is, because a calendar date is stored as midnight
 * UTC on that day (see venture_time_from_string()); only the reading of the
 * current date uses @timezone. This is the vocabulary the CLI, the REST API
 * and the AI's report tool all accept, so a person and a model can ask for
 * the same period the same way:
 *
 *   today, yesterday, this_week, last_week,
 *   this_month, last_month, this_quarter, last_quarter,
 *   this_year, last_year, ytd, mtd, qtd,
 *   fy, fy_2026, last_7_days, last_30_days, last_90_days, last_365_days,
 *   2026, 2026-03, 2026-Q2,
 *   2026-01-01..2026-03-31   (an explicit inclusive span),
 *   all
 *
 * An explicit span is inclusive of both endpoints in the text, and is
 * converted to the half-open internal form by advancing the end by one day.
 * That is what a person means when they write two dates.
 *
 * Returns: (transfer full) (nullable): the parsed range, or %NULL on error
 */
VentureDateRange *
venture_date_range_parse(
	const gchar	 *text,
	GTimeZone	 *timezone,
	gint		  fiscal_year_start_month,
	GError		**error
);

/**
 * venture_date_range_copy:
 * @self: (nullable): a #VentureDateRange
 *
 * Returns: (transfer full) (nullable): a copy of @self
 */
VentureDateRange *
venture_date_range_copy(const VentureDateRange *self);

/**
 * venture_date_range_free:
 * @self: (nullable): a #VentureDateRange
 *
 * Frees @self. Safe to call with %NULL.
 */
void
venture_date_range_free(VentureDateRange *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(VentureDateRange, venture_date_range_free)

/* --- Accessors ----------------------------------------------------------- */

/**
 * venture_date_range_get_start:
 * @self: a #VentureDateRange
 *
 * Returns: (transfer none) (nullable): the inclusive start, or %NULL if the
 *   range is unbounded below
 */
GDateTime *
venture_date_range_get_start(const VentureDateRange *self);

/**
 * venture_date_range_get_end:
 * @self: a #VentureDateRange
 *
 * Returns: (transfer none) (nullable): the exclusive end, or %NULL if the
 *   range is unbounded above
 */
GDateTime *
venture_date_range_get_end(const VentureDateRange *self);

/**
 * venture_date_range_get_label:
 * @self: a #VentureDateRange
 *
 * Retrieves the range's label, generating one from its bounds if it was not
 * given an explicit one.
 *
 * Returns: (transfer none): the label
 */
const gchar *
venture_date_range_get_label(VentureDateRange *self);

/**
 * venture_date_range_contains:
 * @self: a #VentureDateRange
 * @when: the instant to test
 *
 * Tests membership under the half-open rule: an instant exactly equal to the
 * end is not contained.
 *
 * Returns: %TRUE if @when falls inside the range
 */
gboolean
venture_date_range_contains(
	const VentureDateRange	*self,
	GDateTime		*when
);

/**
 * venture_date_range_overlaps:
 * @a: the first range
 * @b: the second range
 *
 * Returns: %TRUE if the two ranges share at least one instant
 */
gboolean
venture_date_range_overlaps(
	const VentureDateRange	*a,
	const VentureDateRange	*b
);

/**
 * venture_date_range_get_days:
 * @self: a #VentureDateRange
 *
 * Returns: the length of the range in whole days, or -1 if it is unbounded
 */
gint64
venture_date_range_get_days(const VentureDateRange *self);

/**
 * venture_date_range_previous_period:
 * @self: a #VentureDateRange
 *
 * Produces the equally-long period immediately preceding @self. This is what
 * every "vs. previous period" comparison in the reporting layer uses, so a
 * month is compared against the month before rather than against a fixed
 * thirty days.
 *
 * Returns: (transfer full) (nullable): the preceding range, or %NULL if
 *   @self is unbounded
 */
VentureDateRange *
venture_date_range_previous_period(const VentureDateRange *self);

/**
 * venture_date_range_comparison_period:
 * @self: a #VentureDateRange
 * @now: (nullable): the instant treated as now, or %NULL for a range that
 *   is never in progress
 *
 * The period a figure for @self is compared against, as a person reading a
 * calendar would pick it. A range made of whole calendar months in its own
 * timezone -- a month, a quarter, a year, a fiscal year -- is compared with
 * the same number of months immediately before it, found by calendar
 * arithmetic, so September is compared with August and March with February
 * whatever their lengths and whichever of them crosses a daylight-saving
 * change. A range made of whole days is compared with the same number of
 * calendar days before it. Anything else falls back to
 * venture_date_range_previous_period().
 *
 * A range that contains @now is compared with the same elapsed part of the
 * previous one: the third of September at ten in the morning compares the
 * first two days and ten hours of September with the first two days and ten
 * hours of August, never with the whole of August. A "to date" range (month,
 * quarter or year to date) starts on a month boundary but ends tomorrow; it
 * is compared against the smallest calendar unit that starts where it starts
 * and contains it. The elapsed part is capped at the start of @self, so the
 * thirty-first of March compares against the whole of February.
 *
 * Unlike venture_date_range_previous_period(), the result need not be as
 * long as @self: that is the point.
 *
 * Returns: (transfer full) (nullable): the comparison range, or %NULL if
 *   @self is unbounded
 */
VentureDateRange *
venture_date_range_comparison_period(
	const VentureDateRange	*self,
	GDateTime		*now
);

/**
 * venture_date_range_split_by_month:
 * @self: a #VentureDateRange
 *
 * Divides the range into one sub-range per calendar month it touches,
 * clipped to the original bounds. Used to build a monthly time series.
 *
 * Returns: (transfer full) (element-type VentureDateRange): the sub-ranges
 */
GPtrArray *
venture_date_range_split_by_month(const VentureDateRange *self);

/**
 * venture_date_range_split_by_day:
 * @self: a #VentureDateRange
 *
 * Divides the range into one sub-range per calendar day it touches.
 *
 * Returns: (transfer full) (element-type VentureDateRange): the sub-ranges
 */
GPtrArray *
venture_date_range_split_by_day(const VentureDateRange *self);

/**
 * venture_date_range_split_by_week:
 * @self: a #VentureDateRange
 *
 * Divides the range into one sub-range per calendar week it touches. Weeks
 * run Monday to Sunday; the first and last are clipped to the range, and
 * each is labelled by its Monday ("Week of 6 Jul 2026") even when the range
 * begins later in that week.
 *
 * Returns: (transfer full) (element-type VentureDateRange): the sub-ranges
 */
GPtrArray *
venture_date_range_split_by_week(const VentureDateRange *self);

/**
 * venture_date_range_equal:
 * @a: (nullable): the first range
 * @b: (nullable): the second range
 *
 * Returns: %TRUE if both describe the same interval
 */
gboolean
venture_date_range_equal(
	const VentureDateRange	*a,
	const VentureDateRange	*b
);

/**
 * venture_date_range_to_string:
 * @self: a #VentureDateRange
 *
 * Formats the range as an inclusive "YYYY-MM-DD..YYYY-MM-DD" span, which
 * venture_date_range_parse() accepts back.
 *
 * Returns: (transfer full): the formatted range
 */
gchar *
venture_date_range_to_string(const VentureDateRange *self);

/**
 * venture_date_range_to_json:
 * @self: a #VentureDateRange
 *
 * Returns: (transfer full): a JSON object with "start", "end" and "label"
 */
JsonNode *
venture_date_range_to_json(VentureDateRange *self);

G_END_DECLS

#endif /* VENTURE_DATE_RANGE_H */
