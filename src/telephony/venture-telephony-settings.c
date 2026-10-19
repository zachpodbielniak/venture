/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * venture-telephony-settings.c - Phone and text settings (docs/telephony.org,
 * docs/settings.org)
 *
 * All of them are fields on the organization, read by the one send path
 * and the keyword answers.
 */
#include "venture.h"

#include <string.h>

static gboolean clock_ok(const gchar *text, gint length)
{
	gint hours, minutes;
	if (length != 5 || !g_ascii_isdigit(text[0]) || !g_ascii_isdigit(text[1]) || text[2] != ':' ||
		!g_ascii_isdigit(text[3]) || !g_ascii_isdigit(text[4]))
		return FALSE;
	hours = (text[0] - '0') * 10 + (text[1] - '0');
	minutes = (text[3] - '0') * 10 + (text[4] - '0');
	return hours < 24 && minutes < 60;
}
/* HH:MM-HH:MM, the form the send path reads, or off. */
static gboolean quiet_hours(const gchar *value, GError **error)
{
	const gchar *dash = strchr(value, '-');
	if (g_ascii_strcasecmp(value, "off") == 0)
		return TRUE;
	if (dash != NULL && clock_ok(value, (gint)(dash - value)) && clock_ok(dash + 1, (gint)strlen(dash + 1)))
		return TRUE;
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		"Write quiet hours as two times on the 24-hour clock, for example 21:00-08:00, or off for none");
	return FALSE;
}
static gboolean country(const gchar *value, GError **error)
{
	if (strlen(value) == 2 && venture_phone_country_code(value) != NULL)
		return TRUE;
	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		"\"%s\" is not a two-letter country Venture knows the calling code of, for example US or GB", value);
	return FALSE;
}
static gboolean words(const gchar *value, gsize most, GError **error)
{
	if (g_utf8_strlen(value, -1) <= (glong)most && *value != '\0')
		return TRUE;
	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "An answer is one line of at most %" G_GSIZE_FORMAT " characters", most);
	return FALSE;
}
static gboolean says(const gchar *value, const gchar *word, GError **error)
{
	g_autofree gchar *upper = g_utf8_strup(value, -1);
	if (strstr(upper, word) != NULL)
		return TRUE;
	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "This answer must tell them to reply %s", word);
	return FALSE;
}
static gboolean stop_reply(const gchar *value, GError **error) { return words(value, 320, error) && says(value, "START", error); }
static gboolean start_reply(const gchar *value, GError **error) { return words(value, 320, error) && says(value, "STOP", error); }
static gboolean help_reply(const gchar *value, GError **error) { return words(value, 320, error) && says(value, "STOP", error); }

static const VentureSettingInfo phone[] = {
	{ "telephony.quiet-hours", "Phone and text", "telephony", "Text quiet hours",
	  "No text goes out between these times in the recipient's own time zone; one written then waits until they end. "
	  "The answers to STOP, START and HELP are not held.",
	  VENTURE_SETTING_TEXT, VENTURE_TELEPHONY_QUIET_HOURS, 0, 0, NULL, VENTURE_SETTING_PERMISSION_ADMIN,
	  "organization: telephony-quiet-hours", "telephony-quiet-hours", "", NULL, NULL, quiet_hours },
	{ "telephony.recording-links", "Phone and text", "telephony", "Keep call recording links",
	  "Store the phone system's recording link on each logged call. Recording calls has legal rules where you and your "
	  "callers are; see the recording note.",
	  VENTURE_SETTING_BOOLEAN, "false", 0, 0, NULL, VENTURE_SETTING_PERMISSION_ADMIN,
	  "organization: telephony-recording-links", "telephony-recording-links", NULL, NULL, NULL, NULL },
	{ "telephony.country", "Phone and text", "telephony", "Phone country",
	  "The country of a number written without its + code, for example GB.",
	  VENTURE_SETTING_TEXT, "US", 0, 0, NULL, VENTURE_SETTING_PERMISSION_ADMIN,
	  "organization: telephony-country", "telephony-country", "", NULL, NULL, country },
	{ "telephony.stop-reply", "Phone and text", "telephony", "Answer to STOP",
	  "Sent once when somebody texts STOP; {business} is your name. It must tell them how to come back (START).",
	  VENTURE_SETTING_TEXT, VENTURE_TELEPHONY_STOP_REPLY, 0, 0, NULL, VENTURE_SETTING_PERMISSION_ADMIN,
	  "organization: telephony-stop-reply", "telephony-stop-reply", "", NULL, NULL, stop_reply },
	{ "telephony.start-reply", "Phone and text", "telephony", "Answer to START",
	  "Sent once when somebody texts START to agree again; it must say how to stop (STOP).",
	  VENTURE_SETTING_TEXT, VENTURE_TELEPHONY_START_REPLY, 0, 0, NULL, VENTURE_SETTING_PERMISSION_ADMIN,
	  "organization: telephony-start-reply", "telephony-start-reply", "", NULL, NULL, start_reply },
	{ "telephony.help-reply", "Phone and text", "telephony", "Answer to HELP",
	  "Sent at most once a day when somebody texts HELP; it must say how to stop (STOP).",
	  VENTURE_SETTING_TEXT, VENTURE_TELEPHONY_HELP_REPLY, 0, 0, NULL, VENTURE_SETTING_PERMISSION_ADMIN,
	  "organization: telephony-help-reply", "telephony-help-reply", "", NULL, NULL, help_reply }
};

void venture_telephony_settings_register(void)
{
	gsize i;
	for (i = 0; i < G_N_ELEMENTS(phone); i++)
		venture_settings_register(&phone[i]);
}
