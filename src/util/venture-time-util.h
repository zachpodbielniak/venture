/*
 * venture-time-util.h - Timestamp parsing and formatting
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Timestamps enter VENTURE from marketplace CSV exports, web forms, REST
 * bodies, AI tool calls and the database, and no two of them agree on a
 * format. These helpers accept every shape that actually turns up and emit
 * exactly one, so what goes into storage is always the same.
 *
 * Everything is stored in UTC. Display and period boundaries convert to the
 * configured timezone at the edge; nothing in the middle of the system has
 * to think about it.
 */

#ifndef VENTURE_TIME_UTIL_H
#define VENTURE_TIME_UTIL_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib.h>

G_BEGIN_DECLS

/**
 * venture_time_now:
 *
 * Returns: (transfer full): the current instant in UTC
 */
GDateTime *
venture_time_now(void);

/**
 * venture_time_from_string:
 * @text: the timestamp to parse
 * @error: (out) (optional): return location for a #GError
 *
 * Parses a timestamp. Accepted forms, in the order they are tried:
 *
 *   - ISO 8601 with an offset or a Z suffix
 *   - "YYYY-MM-DD HH:MM:SS" and "YYYY-MM-DD HH:MM", assumed UTC
 *   - "YYYY-MM-DD", becoming midnight UTC on that day
 *   - "YYYY/MM/DD" and "MM/DD/YYYY", which is what a spreadsheet exports
 *   - a bare integer, interpreted as seconds since the epoch, or as
 *     milliseconds if it is large enough that seconds would place it
 *     beyond the year 5000
 *   - "now", "today", "yesterday", "tomorrow"
 *
 * Returns: (transfer full) (nullable): the parsed instant in UTC, or %NULL
 */
GDateTime *
venture_time_from_string(
	const gchar	 *text,
	GError		**error
);

/**
 * venture_time_to_string:
 * @when: (nullable): the instant to format
 *
 * Formats as ISO 8601 in UTC. This is the canonical stored and transmitted
 * form.
 *
 * Returns: (transfer full) (nullable): the formatted timestamp, or %NULL
 */
gchar *
venture_time_to_string(GDateTime *when);

/**
 * venture_time_to_date_string:
 * @when: (nullable): the instant to format
 * @timezone: (nullable): the timezone to render in; %NULL means UTC
 *
 * Formats as "YYYY-MM-DD" in @timezone. Used wherever only the calendar day
 * matters, such as an expense date. A value at exactly midnight UTC is a
 * stored calendar date and is formatted as that date, whatever @timezone is.
 *
 * Returns: (transfer full) (nullable): the formatted date, or %NULL
 */
gchar *
venture_time_to_date_string(
	GDateTime	*when,
	GTimeZone	*timezone
);

/**
 * venture_time_to_display_string:
 * @when: (nullable): the instant to format
 * @timezone: (nullable): the timezone to render in
 *
 * Formats for a human, as "14 Mar 2026, 09:32".
 *
 * Returns: (transfer full) (nullable): the formatted timestamp
 */
gchar *
venture_time_to_display_string(
	GDateTime	*when,
	GTimeZone	*timezone
);

/**
 * venture_time_to_relative_string:
 * @when: (nullable): the instant to describe
 *
 * Describes an instant relative to now, as "3 minutes ago", "in 2 days" or
 * "just now". Used in activity feeds and list views where the exact time is
 * noise.
 *
 * Returns: (transfer full) (nullable): the description
 */
gchar *
venture_time_to_relative_string(GDateTime *when);

/**
 * venture_time_today:
 * @timezone: (nullable): the zone whose calendar date "today" means; %NULL
 *   means the process-local zone
 *
 * The calendar date current in @timezone, encoded as midnight UTC on that
 * date -- the same instant a date picker submits for it. A business date
 * must be read in the configured zone, not the zone the process happens to
 * run in, or an evening acceptance issues tomorrow's invoice.
 *
 * Returns: (transfer full): midnight UTC on today's date in @timezone
 */
GDateTime *
venture_time_today(GTimeZone *timezone);

/**
 * venture_time_get_timezone:
 * @name: (nullable): an IANA timezone name
 *
 * Resolves a timezone by name, falling back to local time with a warning if
 * the name is not recognised -- a mistyped timezone in configuration should
 * degrade rather than prevent startup.
 *
 * Returns: (transfer full): a #GTimeZone
 */
GTimeZone *
venture_time_get_timezone(const gchar *name);

/**
 * venture_time_equal:
 * @a: (nullable): the first instant
 * @b: (nullable): the second instant
 *
 * Returns: %TRUE if both are %NULL or both denote the same instant
 */
gboolean
venture_time_equal(
	GDateTime	*a,
	GDateTime	*b
);

G_END_DECLS

#endif /* VENTURE_TIME_UTIL_H */
