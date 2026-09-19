/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_ICALENDAR_H
#define VENTURE_ICALENDAR_H
#include <glib-object.h>
G_BEGIN_DECLS
/**
 * VentureICalEvent:
 * @uid: the UID
 * @summary: SUMMARY, unescaped
 * @description: DESCRIPTION, unescaped
 * @status: STATUS as written, for example CONFIRMED or CANCELLED
 * @rrule: RRULE as written, or %NULL; kept for display, never expanded
 * @starts: DTSTART in UTC
 * @ends: DTEND in UTC, or %NULL
 * @last_modified: LAST-MODIFIED in UTC, or %NULL
 * @sequence: SEQUENCE, zero when absent
 * @all_day: whether DTSTART was a DATE rather than a DATE-TIME
 *
 * One VEVENT, reduced to what an activity carries. A calendar's other
 * components (VTODO, VTIMEZONE) are ignored; TZID references are resolved
 * with the IANA database, and floating times are read as UTC.
 */
typedef struct {
	gchar *uid;
	gchar *summary;
	gchar *description;
	gchar *status;
	gchar *rrule;
	GDateTime *starts;
	GDateTime *ends;
	GDateTime *last_modified;
	gint64 sequence;
	gboolean all_day;
} VentureICalEvent;
#define VENTURE_TYPE_ICAL_EVENT (venture_ical_event_get_type())
GType venture_ical_event_get_type(void) G_GNUC_CONST;
/**
 * venture_ical_event_new:
 * Returns: (transfer full): an empty event
 */
VentureICalEvent *venture_ical_event_new(void);
/**
 * venture_ical_event_copy:
 * @event: an event
 * Returns: (transfer full): a copy
 */
VentureICalEvent *venture_ical_event_copy(const VentureICalEvent *event);
/**
 * venture_ical_event_free:
 * @event: (transfer full) (nullable): an event
 */
void venture_ical_event_free(VentureICalEvent *event);
/**
 * venture_ical_event_parse:
 * @text: iCalendar text, CRLF or LF, folded or not
 * @error: (out) (optional): %VENTURE_ERROR_NOT_FOUND when there is no VEVENT, %VENTURE_ERROR_VALIDATION when it has no UID or DTSTART
 *
 * Reads the first VEVENT of a VCALENDAR.
 * Returns: (transfer full) (nullable): the event
 */
VentureICalEvent *venture_ical_event_parse(const gchar *text, GError **error);
/**
 * venture_ical_event_format:
 * @event: an event with at least a UID and a start
 * @stamp: DTSTAMP, normally now
 *
 * Writes one VCALENDAR holding one VEVENT, CRLF-terminated, escaped and
 * folded the way the activities export writes its calendar.
 * Returns: (transfer full): the text
 */
gchar *venture_ical_event_format(const VentureICalEvent *event, GDateTime *stamp);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(VentureICalEvent, venture_ical_event_free)
G_END_DECLS
#endif
