/*
 * venture-date-range.c - Half-open time intervals
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "venture.h"

#include <stdio.h>
#include <string.h>

/*
 * Resolves a caller-supplied timezone, defaulting to local time. Every
 * period boundary in this file goes through this, so a range built without
 * an explicit zone still agrees with the operator's calendar.
 */
static GTimeZone *
venture_date_range_resolve_tz(GTimeZone *timezone)
{
	if (NULL != timezone)
		return g_time_zone_ref(timezone);

	return g_time_zone_new_local();
}

/*
 * Midnight at the start of the given calendar day, in the given zone.
 */
static GDateTime *
venture_date_range_day_start(
	GDateTime	*when,
	GTimeZone	*timezone
){
	g_autoptr(GDateTime) local = NULL;

	local = g_date_time_to_timezone(when, timezone);

	if (NULL == local)
		return NULL;

	return g_date_time_new(timezone,
	                       g_date_time_get_year(local),
	                       g_date_time_get_month(local),
	                       g_date_time_get_day_of_month(local),
	                       0, 0, 0.0);
}

G_DEFINE_BOXED_TYPE(VentureDateRange, venture_date_range,
                    venture_date_range_copy, venture_date_range_free)

/* --- Construction -------------------------------------------------------- */

VentureDateRange *
venture_date_range_new(
	GDateTime	*start,
	GDateTime	*end
){
	return venture_date_range_new_labelled(start, end, NULL);
}

VentureDateRange *
venture_date_range_new_labelled(
	GDateTime	*start,
	GDateTime	*end,
	const gchar	*label
){
	VentureDateRange *self;

	/* An inverted range would silently produce empty reports rather than
	 * an error, which is the worst kind of bug in a system people trust
	 * numbers from. Refuse it. */
	if ((NULL != start) && (NULL != end) &&
	    (g_date_time_compare(start, end) > 0))
	{
		g_warning("venture_date_range: the start of a range must not "
		          "follow its end");
		return NULL;
	}

	self = g_new0(VentureDateRange, 1);
	self->start = (NULL != start) ? g_date_time_ref(start) : NULL;
	self->end = (NULL != end) ? g_date_time_ref(end) : NULL;
	self->label = g_strdup(label);

	return self;
}

VentureDateRange *
venture_date_range_new_day(
	GDateTime	*day,
	GTimeZone	*timezone
){
	g_autoptr(GTimeZone) tz = NULL;
	g_autoptr(GDateTime) start = NULL;
	g_autoptr(GDateTime) end = NULL;
	g_autofree gchar *label = NULL;

	g_return_val_if_fail(NULL != day, NULL);

	tz = venture_date_range_resolve_tz(timezone);
	start = venture_date_range_day_start(day, tz);

	if (NULL == start)
		return NULL;

	end = g_date_time_add_days(start, 1);
	label = g_date_time_format(start, "%Y-%m-%d");

	return venture_date_range_new_labelled(start, end, label);
}

VentureDateRange *
venture_date_range_new_month(
	gint		 year,
	gint		 month,
	GTimeZone	*timezone
){
	g_autoptr(GTimeZone) tz = NULL;
	g_autoptr(GDateTime) start = NULL;
	g_autoptr(GDateTime) end = NULL;
	g_autofree gchar *label = NULL;

	g_return_val_if_fail((month >= 1) && (month <= 12), NULL);

	tz = venture_date_range_resolve_tz(timezone);
	start = g_date_time_new(tz, year, month, 1, 0, 0, 0.0);

	if (NULL == start)
		return NULL;

	/* Adding a month rather than a fixed number of days is what makes
	 * February and the month after a 31st behave correctly. */
	end = g_date_time_add_months(start, 1);
	label = g_date_time_format(start, "%B %Y");

	return venture_date_range_new_labelled(start, end, label);
}

VentureDateRange *
venture_date_range_new_quarter(
	gint		 year,
	gint		 quarter,
	GTimeZone	*timezone
){
	g_autoptr(GTimeZone) tz = NULL;
	g_autoptr(GDateTime) start = NULL;
	g_autoptr(GDateTime) end = NULL;
	g_autofree gchar *label = NULL;

	g_return_val_if_fail((quarter >= 1) && (quarter <= 4), NULL);

	tz = venture_date_range_resolve_tz(timezone);
	start = g_date_time_new(tz, year, ((quarter - 1) * 3) + 1, 1, 0, 0, 0.0);

	if (NULL == start)
		return NULL;

	end = g_date_time_add_months(start, 3);
	label = g_strdup_printf("Q%d %d", quarter, year);

	return venture_date_range_new_labelled(start, end, label);
}

VentureDateRange *
venture_date_range_new_year(
	gint		 year,
	GTimeZone	*timezone
){
	g_autoptr(GTimeZone) tz = NULL;
	g_autoptr(GDateTime) start = NULL;
	g_autoptr(GDateTime) end = NULL;
	g_autofree gchar *label = NULL;

	tz = venture_date_range_resolve_tz(timezone);
	start = g_date_time_new(tz, year, 1, 1, 0, 0, 0.0);

	if (NULL == start)
		return NULL;

	end = g_date_time_add_years(start, 1);
	label = g_strdup_printf("%d", year);

	return venture_date_range_new_labelled(start, end, label);
}

VentureDateRange *
venture_date_range_new_fiscal_year(
	gint		 year,
	gint		 start_month,
	GTimeZone	*timezone
){
	g_autoptr(GTimeZone) tz = NULL;
	g_autoptr(GDateTime) start = NULL;
	g_autoptr(GDateTime) end = NULL;
	g_autofree gchar *label = NULL;

	g_return_val_if_fail((start_month >= 1) && (start_month <= 12), NULL);

	if (1 == start_month)
		return venture_date_range_new_year(year, timezone);

	tz = venture_date_range_resolve_tz(timezone);
	start = g_date_time_new(tz, year, start_month, 1, 0, 0, 0.0);

	if (NULL == start)
		return NULL;

	end = g_date_time_add_years(start, 1);

	/* A fiscal year that straddles the calendar is named for both, which
	 * is how it appears on the paperwork. */
	label = g_strdup_printf("FY %d-%d", year, year + 1);

	return venture_date_range_new_labelled(start, end, label);
}

VentureDateRange *
venture_date_range_new_last_days(
	guint		 days,
	GTimeZone	*timezone
){
	g_autoptr(GTimeZone) tz = NULL;
	g_autoptr(GDateTime) now = NULL;
	g_autoptr(GDateTime) today = NULL;
	g_autoptr(GDateTime) start = NULL;
	g_autoptr(GDateTime) end = NULL;
	g_autofree gchar *label = NULL;

	g_return_val_if_fail(days > 0, NULL);

	tz = venture_date_range_resolve_tz(timezone);
	now = g_date_time_new_now(tz);
	today = venture_date_range_day_start(now, tz);

	if (NULL == today)
		return NULL;

	/* Whole days ending at the end of today: the answer is stable for the
	 * whole of today rather than sliding with the clock. */
	end = g_date_time_add_days(today, 1);
	start = g_date_time_add_days(end, -(gint)days);
	label = g_strdup_printf("Last %u days", days);

	return venture_date_range_new_labelled(start, end, label);
}

VentureDateRange *
venture_date_range_new_all_time(void)
{
	return venture_date_range_new_labelled(NULL, NULL, "All time");
}

VentureDateRange *
venture_date_range_copy(const VentureDateRange *self)
{
	if (NULL == self)
		return NULL;

	return venture_date_range_new_labelled(self->start, self->end,
	                                       self->label);
}

void
venture_date_range_free(VentureDateRange *self)
{
	if (NULL == self)
		return;

	g_clear_pointer(&self->start, g_date_time_unref);
	g_clear_pointer(&self->end, g_date_time_unref);
	g_clear_pointer(&self->label, g_free);
	g_free(self);
}

/* --- Parsing ------------------------------------------------------------- */

/*
 * Parses "YYYY-MM-DD" into midnight at the start of that day. Returns NULL
 * if the text is not exactly that shape, so the caller can try other forms.
 */
static GDateTime *
venture_date_range_parse_iso_day(
	const gchar	*text,
	GTimeZone	*timezone
){
	gint year;
	gint month;
	gint day;

	if (3 != sscanf(text, "%4d-%2d-%2d", &year, &month, &day))
		return NULL;

	if ((month < 1) || (month > 12) || (day < 1) || (day > 31))
		return NULL;

	return g_date_time_new(timezone, year, month, day, 0, 0, 0.0);
}

VentureDateRange *
venture_date_range_parse(
	const gchar	 *text,
	GTimeZone	 *timezone,
	gint		  fiscal_year_start_month,
	GError		**error
){
	g_autoptr(GTimeZone) tz = NULL;
	g_autoptr(GDateTime) now = NULL;
	g_autofree gchar *normalised = NULL;
	const gchar *separator;
	gint year;
	gint month;
	gint quarter;
	guint days;

	g_return_val_if_fail(NULL != text, NULL);

	if ((fiscal_year_start_month < 1) || (fiscal_year_start_month > 12))
		fiscal_year_start_month = 1;

	tz = venture_date_range_resolve_tz(timezone);
	now = g_date_time_new_now(tz);

	/* Accept dashes, spaces and mixed case interchangeably: this string
	 * arrives from a CLI flag, a query parameter and an AI tool argument,
	 * and none of them agree on spelling. */
	normalised = g_ascii_strdown(g_strstrip(g_strdup(text)), -1);
	g_strdelimit(normalised, "- ", '_');

	/* An explicit inclusive span: "2026-01-01..2026-03-31". Checked
	 * against the original text, which still has its dashes. */
	separator = strstr(text, "..");

	if (NULL != separator)
	{
		g_autofree gchar *start_text = NULL;
		g_autofree gchar *end_text = NULL;
		g_autoptr(GDateTime) start = NULL;
		g_autoptr(GDateTime) parsed_end = NULL;
		g_autoptr(GDateTime) end = NULL;

		/* Both halves are copied before trimming: @text is const and may
		 * be a string literal, so trimming it in place would be undefined
		 * behaviour rather than merely rude. */
		start_text = g_strstrip(g_strndup(text, (gsize)(separator - text)));
		end_text = g_strstrip(g_strdup(separator + 2));

		start = venture_date_range_parse_iso_day(start_text, tz);
		parsed_end = venture_date_range_parse_iso_day(end_text, tz);

		if ((NULL == start) || (NULL == parsed_end))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			            "\"%s\" is not a valid YYYY-MM-DD..YYYY-MM-DD span",
			            text);
			return NULL;
		}

		/* The written form is inclusive of both ends; the internal form
		 * is not. Advancing by a day is the conversion. */
		end = g_date_time_add_days(parsed_end, 1);

		return venture_date_range_new(start, end);
	}

	if ((0 == g_strcmp0(normalised, "all")) ||
	    (0 == g_strcmp0(normalised, "all_time")) ||
	    (0 == g_strcmp0(normalised, "ever")))
	{
		return venture_date_range_new_all_time();
	}

	if (0 == g_strcmp0(normalised, "today"))
		return venture_date_range_new_day(now, tz);

	if (0 == g_strcmp0(normalised, "yesterday"))
	{
		g_autoptr(GDateTime) yesterday = NULL;

		yesterday = g_date_time_add_days(now, -1);

		return venture_date_range_new_day(yesterday, tz);
	}

	if ((0 == g_strcmp0(normalised, "this_week")) ||
	    (0 == g_strcmp0(normalised, "last_week")))
	{
		g_autoptr(GDateTime) today = NULL;
		g_autoptr(GDateTime) start = NULL;
		g_autoptr(GDateTime) end = NULL;
		gint weekday;

		today = venture_date_range_day_start(now, tz);
		/* GLib numbers Monday as 1 through Sunday as 7. Weeks here start
		 * on Monday, which is the ISO convention and the one that makes
		 * a "this week" report match a calendar. */
		weekday = g_date_time_get_day_of_week(today);
		start = g_date_time_add_days(today, -(weekday - 1));

		if (0 == g_strcmp0(normalised, "last_week"))
		{
			g_autoptr(GDateTime) shifted = NULL;

			shifted = g_date_time_add_days(start, -7);
			g_clear_pointer(&start, g_date_time_unref);
			start = g_steal_pointer(&shifted);
		}

		end = g_date_time_add_days(start, 7);

		return venture_date_range_new_labelled(start, end,
			(0 == g_strcmp0(normalised, "last_week")) ? "Last week"
			                                          : "This week");
	}

	if (0 == g_strcmp0(normalised, "this_month"))
	{
		return venture_date_range_new_month(g_date_time_get_year(now),
		                                    g_date_time_get_month(now), tz);
	}

	if (0 == g_strcmp0(normalised, "last_month"))
	{
		g_autoptr(GDateTime) previous = NULL;

		previous = g_date_time_add_months(now, -1);

		return venture_date_range_new_month(g_date_time_get_year(previous),
		                                    g_date_time_get_month(previous),
		                                    tz);
	}

	if (0 == g_strcmp0(normalised, "this_quarter"))
	{
		quarter = ((g_date_time_get_month(now) - 1) / 3) + 1;

		return venture_date_range_new_quarter(g_date_time_get_year(now),
		                                      quarter, tz);
	}

	if (0 == g_strcmp0(normalised, "last_quarter"))
	{
		g_autoptr(GDateTime) previous = NULL;

		previous = g_date_time_add_months(now, -3);
		quarter = ((g_date_time_get_month(previous) - 1) / 3) + 1;

		return venture_date_range_new_quarter(g_date_time_get_year(previous),
		                                      quarter, tz);
	}

	if (0 == g_strcmp0(normalised, "this_year"))
		return venture_date_range_new_year(g_date_time_get_year(now), tz);

	if (0 == g_strcmp0(normalised, "last_year"))
		return venture_date_range_new_year(g_date_time_get_year(now) - 1, tz);

	/* The "to date" forms all share a shape: the start of some period up
	 * to the end of today. */
	if ((0 == g_strcmp0(normalised, "ytd")) ||
	    (0 == g_strcmp0(normalised, "mtd")) ||
	    (0 == g_strcmp0(normalised, "qtd")))
	{
		g_autoptr(GDateTime) today = NULL;
		g_autoptr(GDateTime) start = NULL;
		g_autoptr(GDateTime) end = NULL;
		const gchar *label;

		today = venture_date_range_day_start(now, tz);
		end = g_date_time_add_days(today, 1);

		if (0 == g_strcmp0(normalised, "ytd"))
		{
			start = g_date_time_new(tz, g_date_time_get_year(now),
			                        1, 1, 0, 0, 0.0);
			label = "Year to date";
		}
		else if (0 == g_strcmp0(normalised, "mtd"))
		{
			start = g_date_time_new(tz, g_date_time_get_year(now),
			                        g_date_time_get_month(now), 1, 0, 0, 0.0);
			label = "Month to date";
		}
		else
		{
			quarter = ((g_date_time_get_month(now) - 1) / 3) + 1;
			start = g_date_time_new(tz, g_date_time_get_year(now),
			                        ((quarter - 1) * 3) + 1, 1, 0, 0, 0.0);
			label = "Quarter to date";
		}

		return venture_date_range_new_labelled(start, end, label);
	}

	if (0 == g_strcmp0(normalised, "fy"))
	{
		/* The current fiscal year is the one containing today, which is
		 * the previous calendar year's when the start month has not been
		 * reached yet. */
		year = g_date_time_get_year(now);

		if (g_date_time_get_month(now) < fiscal_year_start_month)
			year -= 1;

		return venture_date_range_new_fiscal_year(year,
		                                          fiscal_year_start_month, tz);
	}

	if (1 == sscanf(normalised, "fy_%4d", &year))
	{
		return venture_date_range_new_fiscal_year(year,
		                                          fiscal_year_start_month, tz);
	}

	if (1 == sscanf(normalised, "last_%u_days", &days))
		return venture_date_range_new_last_days(days, tz);

	if (2 == sscanf(normalised, "%4d_q%1d", &year, &quarter))
		return venture_date_range_new_quarter(year, quarter, tz);

	if (2 == sscanf(normalised, "%4d_%2d", &year, &month))
		return venture_date_range_new_month(year, month, tz);

	if ((4 == strlen(normalised)) && (1 == sscanf(normalised, "%4d", &year)))
		return venture_date_range_new_year(year, tz);

	/* A bare ISO day is a one-day range, which is what someone asking for
	 * "2026-03-14" almost certainly means. */
	{
		g_autoptr(GDateTime) day = NULL;

		day = venture_date_range_parse_iso_day(text, tz);

		if (NULL != day)
			return venture_date_range_new_day(day, tz);
	}

	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
	            "\"%s\" is not a period I recognise. Try one of: today, "
	            "yesterday, this_week, this_month, last_month, this_quarter, "
	            "this_year, ytd, mtd, qtd, fy, last_30_days, 2026, 2026-03, "
	            "2026-Q2, 2026-01-01..2026-03-31, all",
	            text);

	return NULL;
}

/* --- Accessors ----------------------------------------------------------- */

GDateTime *
venture_date_range_get_start(const VentureDateRange *self)
{
	g_return_val_if_fail(NULL != self, NULL);

	return self->start;
}

GDateTime *
venture_date_range_get_end(const VentureDateRange *self)
{
	g_return_val_if_fail(NULL != self, NULL);

	return self->end;
}

const gchar *
venture_date_range_get_label(VentureDateRange *self)
{
	g_return_val_if_fail(NULL != self, NULL);

	/* Generate a label lazily rather than at construction: most ranges
	 * are never displayed, and the ones built by the named constructors
	 * already have a better label than the bounds would produce. */
	if (NULL == self->label)
		self->label = venture_date_range_to_string(self);

	return self->label;
}

gboolean
venture_date_range_contains(
	const VentureDateRange	*self,
	GDateTime		*when
){
	g_return_val_if_fail(NULL != self, FALSE);
	g_return_val_if_fail(NULL != when, FALSE);

	if ((NULL != self->start) && (g_date_time_compare(when, self->start) < 0))
		return FALSE;

	/* Half open: an instant exactly at the end belongs to the next
	 * period, not this one. */
	if ((NULL != self->end) && (g_date_time_compare(when, self->end) >= 0))
		return FALSE;

	return TRUE;
}

gboolean
venture_date_range_overlaps(
	const VentureDateRange	*a,
	const VentureDateRange	*b
){
	g_return_val_if_fail(NULL != a, FALSE);
	g_return_val_if_fail(NULL != b, FALSE);

	if ((NULL != a->end) && (NULL != b->start) &&
	    (g_date_time_compare(a->end, b->start) <= 0))
		return FALSE;

	if ((NULL != b->end) && (NULL != a->start) &&
	    (g_date_time_compare(b->end, a->start) <= 0))
		return FALSE;

	return TRUE;
}

gint64
venture_date_range_get_days(const VentureDateRange *self)
{
	GTimeSpan span;

	g_return_val_if_fail(NULL != self, -1);

	if ((NULL == self->start) || (NULL == self->end))
		return -1;

	span = g_date_time_difference(self->end, self->start);

	return span / G_TIME_SPAN_DAY;
}

VentureDateRange *
venture_date_range_previous_period(const VentureDateRange *self)
{
	g_autoptr(GDateTime) start = NULL;
	GTimeSpan span;

	g_return_val_if_fail(NULL != self, NULL);

	if ((NULL == self->start) || (NULL == self->end))
		return NULL;

	/* The preceding period is exactly as long and ends where this one
	 * begins, so consecutive periods tile with no gap. */
	span = g_date_time_difference(self->end, self->start);
	start = g_date_time_add(self->start, -span);

	return venture_date_range_new(start, self->start);
}

GPtrArray *
venture_date_range_split_by_month(const VentureDateRange *self)
{
	g_autoptr(GPtrArray) parts = NULL;
	g_autoptr(GDateTime) cursor = NULL;

	g_return_val_if_fail(NULL != self, NULL);

	parts = g_ptr_array_new_with_free_func(
		(GDestroyNotify)venture_date_range_free);

	if ((NULL == self->start) || (NULL == self->end))
		return g_steal_pointer(&parts);

	/* Start at the first of the month containing the range start, then
	 * clip each bucket to the range so the first and last are partial
	 * months when the range does not begin or end on a boundary. */
	cursor = g_date_time_new(g_date_time_get_timezone(self->start),
	                         g_date_time_get_year(self->start),
	                         g_date_time_get_month(self->start),
	                         1, 0, 0, 0.0);

	while ((NULL != cursor) && (g_date_time_compare(cursor, self->end) < 0))
	{
		g_autoptr(GDateTime) next = NULL;
		g_autoptr(GDateTime) bucket_start = NULL;
		g_autoptr(GDateTime) bucket_end = NULL;
		g_autofree gchar *label = NULL;

		next = g_date_time_add_months(cursor, 1);

		bucket_start = (g_date_time_compare(cursor, self->start) < 0)
			? g_date_time_ref(self->start)
			: g_date_time_ref(cursor);

		bucket_end = (g_date_time_compare(next, self->end) > 0)
			? g_date_time_ref(self->end)
			: g_date_time_ref(next);

		label = g_date_time_format(cursor, "%b %Y");

		g_ptr_array_add(parts,
			venture_date_range_new_labelled(bucket_start, bucket_end, label));

		g_clear_pointer(&cursor, g_date_time_unref);
		cursor = g_steal_pointer(&next);
	}

	return g_steal_pointer(&parts);
}

GPtrArray *
venture_date_range_split_by_day(const VentureDateRange *self)
{
	g_autoptr(GPtrArray) parts = NULL;
	g_autoptr(GDateTime) cursor = NULL;

	g_return_val_if_fail(NULL != self, NULL);

	parts = g_ptr_array_new_with_free_func(
		(GDestroyNotify)venture_date_range_free);

	if ((NULL == self->start) || (NULL == self->end))
		return g_steal_pointer(&parts);

	cursor = g_date_time_ref(self->start);

	while (g_date_time_compare(cursor, self->end) < 0)
	{
		g_autoptr(GDateTime) next = NULL;
		g_autoptr(GDateTime) bucket_end = NULL;
		g_autofree gchar *label = NULL;

		next = g_date_time_add_days(cursor, 1);

		bucket_end = (g_date_time_compare(next, self->end) > 0)
			? g_date_time_ref(self->end)
			: g_date_time_ref(next);

		label = g_date_time_format(cursor, "%Y-%m-%d");

		g_ptr_array_add(parts,
			venture_date_range_new_labelled(cursor, bucket_end, label));

		g_clear_pointer(&cursor, g_date_time_unref);
		cursor = g_steal_pointer(&next);
	}

	return g_steal_pointer(&parts);
}

gboolean
venture_date_range_equal(
	const VentureDateRange	*a,
	const VentureDateRange	*b
){
	if (a == b)
		return TRUE;

	if ((NULL == a) || (NULL == b))
		return FALSE;

	if ((NULL == a->start) != (NULL == b->start))
		return FALSE;

	if ((NULL == a->end) != (NULL == b->end))
		return FALSE;

	if ((NULL != a->start) && (0 != g_date_time_compare(a->start, b->start)))
		return FALSE;

	if ((NULL != a->end) && (0 != g_date_time_compare(a->end, b->end)))
		return FALSE;

	return TRUE;
}

gchar *
venture_date_range_to_string(const VentureDateRange *self)
{
	g_autofree gchar *start_text = NULL;
	g_autofree gchar *end_text = NULL;
	g_autoptr(GDateTime) inclusive_end = NULL;

	g_return_val_if_fail(NULL != self, NULL);

	if ((NULL == self->start) && (NULL == self->end))
		return g_strdup("all");

	start_text = (NULL != self->start)
		? g_date_time_format(self->start, "%Y-%m-%d")
		: g_strdup("");

	if (NULL != self->end)
	{
		/* Print the inclusive end, matching the form the parser accepts
		 * and the way a person reads a date span. */
		inclusive_end = g_date_time_add_days(self->end, -1);
		end_text = g_date_time_format(inclusive_end, "%Y-%m-%d");
	}
	else
	{
		end_text = g_strdup("");
	}

	return g_strdup_printf("%s..%s", start_text, end_text);
}

JsonNode *
venture_date_range_to_json(VentureDateRange *self)
{
	g_autoptr(JsonBuilder) builder = NULL;

	g_return_val_if_fail(NULL != self, NULL);

	builder = json_builder_new();
	json_builder_begin_object(builder);

	json_builder_set_member_name(builder, "start");

	if (NULL != self->start)
	{
		g_autofree gchar *text = NULL;

		text = g_date_time_format_iso8601(self->start);
		json_builder_add_string_value(builder, text);
	}
	else
	{
		json_builder_add_null_value(builder);
	}

	json_builder_set_member_name(builder, "end");

	if (NULL != self->end)
	{
		g_autofree gchar *text = NULL;

		text = g_date_time_format_iso8601(self->end);
		json_builder_add_string_value(builder, text);
	}
	else
	{
		json_builder_add_null_value(builder);
	}

	json_builder_set_member_name(builder, "label");
	json_builder_add_string_value(builder, venture_date_range_get_label(self));

	json_builder_end_object(builder);

	return json_builder_get_root(builder);
}
