/*
 * venture-time-util.c - Timestamp parsing and formatting
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "venture.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/*
 * Timestamps beyond this second count cannot plausibly be seconds -- that
 * would be the year 5138 -- so a value above it is milliseconds. Marketplace
 * exports and JavaScript both emit milliseconds, and guessing wrong puts a
 * sale in the wrong millennium.
 */
#define VENTURE_TIME_MILLISECOND_THRESHOLD (100000000000LL)

GDateTime *
venture_time_now(void)
{
	return g_date_time_new_now_utc();
}

GDateTime *
venture_time_from_string(
	const gchar	 *text,
	GError		**error
){
	g_autofree gchar *trimmed = NULL;
	g_autoptr(GTimeZone) utc = NULL;
	gint year;
	gint month;
	gint day;
	gint hour;
	gint minute;
	gint second;

	g_return_val_if_fail(NULL != text, NULL);

	trimmed = g_strstrip(g_strdup(text));

	if ('\0' == trimmed[0])
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "Cannot parse an empty string as a timestamp");
		return NULL;
	}

	utc = g_time_zone_new_utc();

	/* Relative words first: they are unambiguous and common in AI tool
	 * arguments, where "today" is far more likely than a literal date. */
	if (0 == g_ascii_strcasecmp(trimmed, "now"))
		return venture_time_now();

	if (0 == g_ascii_strcasecmp(trimmed, "today"))
	{
		g_autoptr(GDateTime) now = NULL;

		now = g_date_time_new_now_local();

		return g_date_time_new(utc, g_date_time_get_year(now),
		                       g_date_time_get_month(now),
		                       g_date_time_get_day_of_month(now),
		                       0, 0, 0.0);
	}

	if ((0 == g_ascii_strcasecmp(trimmed, "yesterday")) ||
	    (0 == g_ascii_strcasecmp(trimmed, "tomorrow")))
	{
		g_autoptr(GDateTime) now = NULL;
		g_autoptr(GDateTime) midnight = NULL;
		gint offset;

		offset = (0 == g_ascii_strcasecmp(trimmed, "yesterday")) ? -1 : 1;
		now = g_date_time_new_now_local();
		midnight = g_date_time_new(utc, g_date_time_get_year(now),
		                           g_date_time_get_month(now),
		                           g_date_time_get_day_of_month(now),
		                           0, 0, 0.0);

		return g_date_time_add_days(midnight, offset);
	}

	/* A full ISO 8601 timestamp: let GLib do it, since it handles offsets,
	 * fractional seconds and the Z suffix correctly. */
	if ((NULL != strchr(trimmed, 'T')) ||
	    (NULL != strchr(trimmed, 'Z')) ||
	    (NULL != strchr(trimmed, '+')))
	{
		g_autoptr(GDateTime) parsed = NULL;

		parsed = g_date_time_new_from_iso8601(trimmed, utc);

		if (NULL != parsed)
			return g_date_time_to_utc(parsed);
	}

	/* "YYYY-MM-DD HH:MM:SS" and "YYYY-MM-DD HH:MM", assumed UTC. */
	if (6 == sscanf(trimmed, "%4d-%2d-%2d %2d:%2d:%2d",
	                &year, &month, &day, &hour, &minute, &second))
	{
		return g_date_time_new(utc, year, month, day,
		                       hour, minute, (gdouble)second);
	}

	if (5 == sscanf(trimmed, "%4d-%2d-%2d %2d:%2d",
	                &year, &month, &day, &hour, &minute))
	{
		return g_date_time_new(utc, year, month, day, hour, minute, 0.0);
	}

	/* A bare calendar day becomes midnight UTC. An expense dated
	 * "2026-03-14" happened on that day; pinning it to midnight keeps it
	 * inside the month it belongs to. */
	if (3 == sscanf(trimmed, "%4d-%2d-%2d", &year, &month, &day))
		return g_date_time_new(utc, year, month, day, 0, 0, 0.0);

	if (3 == sscanf(trimmed, "%4d/%2d/%2d", &year, &month, &day))
		return g_date_time_new(utc, year, month, day, 0, 0, 0.0);

	/* US-style MM/DD/YYYY, which is what a spreadsheet exports and what
	 * most marketplace reports use. Recognised by the four-digit year in
	 * the trailing position, so it cannot be confused with the above. */
	if (3 == sscanf(trimmed, "%2d/%2d/%4d", &month, &day, &year))
	{
		if ((month >= 1) && (month <= 12) && (day >= 1) && (day <= 31))
			return g_date_time_new(utc, year, month, day, 0, 0, 0.0);
	}

	/* A bare integer is an epoch timestamp in seconds or milliseconds. */
	{
		gchar *end = NULL;
		gint64 epoch;

		errno = 0;
		epoch = g_ascii_strtoll(trimmed, &end, 10);

		if ((0 == errno) && (NULL != end) && ('\0' == *end) &&
		    (trimmed != end))
		{
			if (epoch > VENTURE_TIME_MILLISECOND_THRESHOLD)
				return g_date_time_new_from_unix_utc(epoch / 1000);

			return g_date_time_new_from_unix_utc(epoch);
		}
	}

	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
	            "\"%s\" is not a timestamp I recognise. Use an ISO 8601 "
	            "timestamp, YYYY-MM-DD, or one of: now, today, yesterday, "
	            "tomorrow", text);

	return NULL;
}

gchar *
venture_time_to_string(GDateTime *when)
{
	g_autoptr(GDateTime) utc = NULL;

	if (NULL == when)
		return NULL;

	/* Normalising to UTC before formatting means a value read back is
	 * byte-identical regardless of the zone it was created in, which
	 * matters for comparing exports and for database indexes. */
	utc = g_date_time_to_utc(when);

	if (NULL == utc)
		return NULL;

	return g_date_time_format_iso8601(utc);
}

gchar *
venture_time_to_date_string(
	GDateTime	*when,
	GTimeZone	*timezone
){
	g_autoptr(GTimeZone) tz = NULL;
	g_autoptr(GDateTime) local = NULL;

	g_autoptr(GDateTime) utc = NULL;

	if (NULL == when)
		return NULL;

	/* Midnight UTC to the microsecond is how a calendar date is stored
	 * (venture_time_from_string() on "2026-03-01", or on "today"), and a
	 * calendar date has no zone to convert: rendered in New York, 1 March
	 * became 28 February on every list while the record page, which reads
	 * the stored value, still said 1 March. A real instant lands there
	 * exactly only by coincidence, so it keeps the caller's zone. */
	utc = g_date_time_to_utc(when);

	if ((NULL != utc) && (0 == g_date_time_get_hour(utc)) &&
	    (0 == g_date_time_get_minute(utc)) &&
	    (0 == g_date_time_get_second(utc)) &&
	    (0 == g_date_time_get_microsecond(utc)))
		return g_date_time_format(utc, "%Y-%m-%d");

	tz = (NULL != timezone) ? g_time_zone_ref(timezone) : g_time_zone_new_utc();
	local = g_date_time_to_timezone(when, tz);

	if (NULL == local)
		return NULL;

	return g_date_time_format(local, "%Y-%m-%d");
}

gchar *
venture_time_to_display_string(
	GDateTime	*when,
	GTimeZone	*timezone
){
	g_autoptr(GTimeZone) tz = NULL;
	g_autoptr(GDateTime) local = NULL;

	if (NULL == when)
		return NULL;

	tz = (NULL != timezone) ? g_time_zone_ref(timezone) : g_time_zone_new_utc();
	local = g_date_time_to_timezone(when, tz);

	if (NULL == local)
		return NULL;

	return g_date_time_format(local, "%-d %b %Y, %H:%M");
}

gchar *
venture_time_to_relative_string(GDateTime *when)
{
	g_autoptr(GDateTime) now = NULL;
	GTimeSpan span;
	gboolean future;
	gint64 seconds;
	gint64 magnitude;
	const gchar *unit;

	if (NULL == when)
		return NULL;

	now = venture_time_now();
	span = g_date_time_difference(now, when);
	future = (span < 0);
	seconds = ABS(span) / G_TIME_SPAN_SECOND;

	if (seconds < 45)
		return g_strdup("just now");

	/* Step down through the units and stop at the first that gives a
	 * number a person would actually say. */
	if (seconds < 3600)
	{
		magnitude = seconds / 60;
		unit = "minute";
	}
	else if (seconds < 86400)
	{
		magnitude = seconds / 3600;
		unit = "hour";
	}
	else if (seconds < 2592000)
	{
		magnitude = seconds / 86400;
		unit = "day";
	}
	else if (seconds < 31536000)
	{
		magnitude = seconds / 2592000;
		unit = "month";
	}
	else
	{
		magnitude = seconds / 31536000;
		unit = "year";
	}

	if (future)
	{
		return g_strdup_printf("in %" G_GINT64_FORMAT " %s%s",
		                       magnitude, unit, (1 == magnitude) ? "" : "s");
	}

	return g_strdup_printf("%" G_GINT64_FORMAT " %s%s ago",
	                       magnitude, unit, (1 == magnitude) ? "" : "s");
}

GTimeZone *
venture_time_get_timezone(const gchar *name)
{
	GTimeZone *tz;

	if ((NULL == name) || ('\0' == name[0]))
		return g_time_zone_new_local();

	tz = g_time_zone_new_identifier(name);

	if (NULL == tz)
	{
		/* A bad timezone in configuration should not stop the server
		 * from starting; falling back to local time is wrong by at most
		 * a few hours and the warning says so. */
		g_warning("Unknown timezone \"%s\"; falling back to local time",
		          name);
		return g_time_zone_new_local();
	}

	return tz;
}

gboolean
venture_time_equal(
	GDateTime	*a,
	GDateTime	*b
){
	if (a == b)
		return TRUE;

	if ((NULL == a) || (NULL == b))
		return FALSE;

	return (0 == g_date_time_compare(a, b));
}
