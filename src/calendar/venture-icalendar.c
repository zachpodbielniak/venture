/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <string.h>

G_DEFINE_BOXED_TYPE(VentureICalEvent, venture_ical_event, venture_ical_event_copy, venture_ical_event_free)
VentureICalEvent *venture_ical_event_new(void) { return g_new0(VentureICalEvent, 1); }
VentureICalEvent *venture_ical_event_copy(const VentureICalEvent *event)
{
	VentureICalEvent *copy;
	g_return_val_if_fail(event != NULL, NULL);
	copy = venture_ical_event_new();
	copy->uid = g_strdup(event->uid);
	copy->summary = g_strdup(event->summary);
	copy->description = g_strdup(event->description);
	copy->status = g_strdup(event->status);
	copy->rrule = g_strdup(event->rrule);
	copy->starts = event->starts ? g_date_time_ref(event->starts) : NULL;
	copy->ends = event->ends ? g_date_time_ref(event->ends) : NULL;
	copy->last_modified = event->last_modified ? g_date_time_ref(event->last_modified) : NULL;
	copy->sequence = event->sequence;
	copy->all_day = event->all_day;
	return copy;
}
void venture_ical_event_free(VentureICalEvent *event)
{
	if (!event) return;
	g_free(event->uid); g_free(event->summary); g_free(event->description); g_free(event->status); g_free(event->rrule);
	g_clear_pointer(&event->starts, g_date_time_unref);
	g_clear_pointer(&event->ends, g_date_time_unref);
	g_clear_pointer(&event->last_modified, g_date_time_unref);
	g_free(event);
}

/* RFC 5545 3.3.11: backslash escapes in TEXT values. */
static gchar *unescape(const gchar *value)
{
	GString *out = g_string_new(NULL);
	const gchar *p;
	for (p = value ? value : ""; *p; p++) {
		if (*p == '\\' && p[1]) {
			p++;
			if (*p == 'n' || *p == 'N') g_string_append_c(out, '\n');
			else g_string_append_c(out, *p);
		} else g_string_append_c(out, *p);
	}
	return g_string_free(out, FALSE);
}
/* Unfold: a CRLF or LF followed by a space or tab continues the line. */
static gchar *unfold(const gchar *text)
{
	GString *out = g_string_new(NULL);
	const gchar *p;
	for (p = text; *p; p++) {
		if (*p == '\r') continue;
		if (*p == '\n' && (p[1] == ' ' || p[1] == '\t')) { p++; continue; }
		g_string_append_c(out, *p);
	}
	return g_string_free(out, FALSE);
}
static gint digits(const gchar *s, guint n)
{
	gint value = 0;
	guint i;
	for (i = 0; i < n; i++) {
		if (!g_ascii_isdigit(s[i])) return -1;
		value = value * 10 + (s[i] - '0');
	}
	return value;
}
/* DATE (YYYYMMDD) or DATE-TIME (YYYYMMDDTHHMMSS[Z]); a TZID parameter names
 * the zone, a trailing Z is UTC, and a floating time is read as UTC. */
static GDateTime *parse_date(const gchar *value, const gchar *tzid, gboolean *all_day)
{
	g_autoptr(GTimeZone) zone = NULL;
	g_autoptr(GDateTime) local = NULL;
	gint y, m, d, hh = 0, mm = 0, ss = 0;
	gsize length = strlen(value);
	if (length != 8 && length != 15 && length != 16) return NULL;
	y = digits(value, 4); m = digits(value + 4, 2); d = digits(value + 6, 2);
	if (y < 0 || m < 1 || d < 1) return NULL;
	if (length == 8) { if (all_day) *all_day = TRUE; }
	else {
		if (value[8] != 'T') return NULL;
		hh = digits(value + 9, 2); mm = digits(value + 11, 2); ss = digits(value + 13, 2);
		if (hh < 0 || mm < 0 || ss < 0) return NULL;
		if (length == 16 && value[15] != 'Z') return NULL;
	}
	if (length == 16) zone = g_time_zone_new_utc();
	else if (tzid && *tzid) zone = g_time_zone_new_identifier(tzid);
	if (!zone) zone = g_time_zone_new_utc();
	local = g_date_time_new(zone, y, m, d, hh, mm, (gdouble)ss);
	return local ? g_date_time_to_utc(local) : NULL;
}
/* NAME;PARAM=value;PARAM="quoted:value":VALUE */
static gboolean split_line(const gchar *line, gchar **name, gchar **tzid, gboolean *is_date, gchar **value)
{
	const gchar *p = line, *colon = NULL, *name_end = NULL;
	gboolean quoted = FALSE;
	for (; *p; p++) {
		if (*p == '"') quoted = !quoted;
		else if (!quoted && (*p == ';' || *p == ':') && !name_end) name_end = p;
		if (!quoted && *p == ':') { colon = p; break; }
	}
	if (!colon || !name_end || name_end == line) return FALSE;
	*name = g_ascii_strup(g_strndup(line, name_end - line), -1);
	*value = g_strdup(colon + 1);
	*tzid = NULL;
	*is_date = FALSE;
	if (*name_end == ';') {
		g_autofree gchar *params = g_strndup(name_end + 1, colon - name_end - 1);
		g_auto(GStrv) parts = g_strsplit(params, ";", -1);
		guint i;
		for (i = 0; parts[i]; i++) {
			gchar *eq = strchr(parts[i], '=');
			if (!eq) continue;
			*eq = '\0';
			if (!g_ascii_strcasecmp(parts[i], "TZID")) {
				g_autofree gchar *raw = g_strdup(eq + 1);
				g_free(*tzid);
				*tzid = g_strdup(g_strstrip(g_strdelimit(raw, "\"", ' ')));
			} else if (!g_ascii_strcasecmp(parts[i], "VALUE") && !g_ascii_strcasecmp(eq + 1, "DATE")) *is_date = TRUE;
		}
	}
	return TRUE;
}
VentureICalEvent *venture_ical_event_parse(const gchar *text, GError **error)
{
	g_autofree gchar *flat = NULL;
	g_auto(GStrv) lines = NULL;
	VentureICalEvent *event = NULL;
	gboolean inside = FALSE, ended = FALSE;
	guint i;
	g_return_val_if_fail(text != NULL, NULL);
	flat = unfold(text);
	lines = g_strsplit(flat, "\n", -1);
	for (i = 0; lines[i] && !ended; i++) {
		g_autofree gchar *name = NULL, *tzid = NULL, *value = NULL;
		gboolean is_date = FALSE;
		if (!*lines[i] || !split_line(lines[i], &name, &tzid, &is_date, &value)) continue;
		if (!inside) {
			if (!g_strcmp0(name, "BEGIN") && !g_ascii_strcasecmp(value, "VEVENT")) { inside = TRUE; event = venture_ical_event_new(); }
			continue;
		}
		if (!g_strcmp0(name, "END") && !g_ascii_strcasecmp(value, "VEVENT")) { ended = TRUE; break; }
		if (!g_strcmp0(name, "BEGIN")) {
			/* Skip nested components such as VALARM. */
			for (i++; lines[i]; i++) if (g_str_has_prefix(lines[i], "END:")) break;
			if (!lines[i]) break;
			continue;
		}
		if (!g_strcmp0(name, "UID")) { g_free(event->uid); event->uid = g_strstrip(unescape(value)); }
		else if (!g_strcmp0(name, "SUMMARY")) { g_free(event->summary); event->summary = unescape(value); }
		else if (!g_strcmp0(name, "DESCRIPTION")) { g_free(event->description); event->description = unescape(value); }
		else if (!g_strcmp0(name, "STATUS")) { g_free(event->status); event->status = g_ascii_strup(g_strstrip(value), -1); }
		else if (!g_strcmp0(name, "RRULE")) { g_free(event->rrule); event->rrule = g_strdup(g_strstrip(value)); }
		else if (!g_strcmp0(name, "SEQUENCE")) event->sequence = g_ascii_strtoll(value, NULL, 10);
		else if (!g_strcmp0(name, "DTSTART")) { g_clear_pointer(&event->starts, g_date_time_unref); event->starts = parse_date(g_strstrip(value), tzid, &event->all_day); }
		else if (!g_strcmp0(name, "DTEND")) { g_clear_pointer(&event->ends, g_date_time_unref); event->ends = parse_date(g_strstrip(value), tzid, NULL); }
		else if (!g_strcmp0(name, "LAST-MODIFIED")) { g_clear_pointer(&event->last_modified, g_date_time_unref); event->last_modified = parse_date(g_strstrip(value), NULL, NULL); }
	}
	if (!event) { g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "The calendar holds no VEVENT"); return NULL; }
	if (venture_string_is_empty(event->uid) || !event->starts) {
		venture_ical_event_free(event);
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "A VEVENT needs a UID and a DTSTART");
		return NULL;
	}
	/* An all-day event is exclusive of its DTEND date; a bare date lasts the day. */
	if (event->all_day && !event->ends) event->ends = g_date_time_add_days(event->starts, 1);
	return event;
}
gchar *venture_ical_event_format(const VentureICalEvent *event, GDateTime *stamp)
{
	g_autoptr(GString) calendar = NULL;
	g_autoptr(GDateTime) now = NULL;
	g_return_val_if_fail(event != NULL && event->uid != NULL && event->starts != NULL, NULL);
	calendar = g_string_new("BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//VENTURE//Calendar sync//EN\r\nCALSCALE:GREGORIAN\r\nBEGIN:VEVENT\r\n");
	now = stamp ? g_date_time_ref(stamp) : venture_time_now();
	venture_activity_calendar_append_line(calendar, "UID", event->uid, FALSE);
	venture_activity_calendar_append_date(calendar, "DTSTAMP", now);
	venture_activity_calendar_append_date(calendar, "DTSTART", event->starts);
	if (event->ends) venture_activity_calendar_append_date(calendar, "DTEND", event->ends);
	venture_activity_calendar_append_line(calendar, "SUMMARY", event->summary ? event->summary : "", TRUE);
	if (event->description && *event->description) venture_activity_calendar_append_line(calendar, "DESCRIPTION", event->description, TRUE);
	venture_activity_calendar_append_line(calendar, "STATUS", event->status && *event->status ? event->status : "CONFIRMED", FALSE);
	if (event->rrule && *event->rrule) venture_activity_calendar_append_line(calendar, "RRULE", event->rrule, FALSE);
	if (event->last_modified) venture_activity_calendar_append_date(calendar, "LAST-MODIFIED", event->last_modified);
	{
		g_autofree gchar *sequence = g_strdup_printf("%" G_GINT64_FORMAT, event->sequence);
		venture_activity_calendar_append_line(calendar, "SEQUENCE", sequence, FALSE);
	}
	g_string_append(calendar, "END:VEVENT\r\nEND:VCALENDAR\r\n");
	return g_string_free(g_steal_pointer(&calendar), FALSE);
}
