/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#define STR(n, l, h) VENTURE_FIELD(n, l, h, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE)
#define INT(n, l, h) VENTURE_FIELD(n, l, h, VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE)
#define DATE(n, l) VENTURE_FIELD(n, l, NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_INDEXED)
/* A calendar account never holds a password: secret-env names the variable
 * whose value is read at sync time, the same shape as a mail_account. */
static const VentureFieldDecl account_fields[] = {
	VENTURE_FIELD_NAME("url", "CalDAV URL", "The server's CalDAV base, for example https://dav.example.net/"),
	VENTURE_FIELD("owner", "Owner", "Username whose activities this calendar mirrors", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED | VENTURE_COLUMN_FLAG_ASSIGNED_USERNAME),
	STR("username", "Username", NULL),
	STR("secret-env", "Secret variable", "NAME of a VENTURE_CALDAV_* environment variable holding the password or app token; never the value"),
	STR("calendar-path", "Calendar path", "The collection under the URL, for example /calendars/ben/default/"),
	STR("sync-token", "Sync token", "The collection's ctag or sync token after the last sweep, maintained by the sync service"),
	VENTURE_FIELD("active", "Active", "Included in the sweep", VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	DATE("last-synced-at", "Last synced"),
	VENTURE_FIELD_TEXT("last-error", "Last error", NULL)
};
VENTURE_DEFINE_ENTITY(VentureCalendarAccount, venture_calendar_account, account_fields)
/* One row per VEVENT the sync knows about: which activity it is, where it
 * lives on the server and what both sides looked like after the last sweep.
 * The UID key is what makes a rerun a no-op. */
static const VentureFieldDecl event_fields[] = {
	VENTURE_FIELD_REF("account-id", "Account", NULL, "calendar_account", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("activity-id", "Activity", NULL, "activity", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("uid", "UID", "The VEVENT UID", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("uid-key", "UID key", "account:uid", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION),
	STR("href", "Href", "The event's path on the server"),
	STR("etag", "ETag", "Server entity tag after the last sweep"),
	INT("local-version", "Local version", "Activity version after the last sweep"),
	DATE("remote-modified-at", "Remote modified"),
	DATE("synced-at", "Synced")
};
VENTURE_DEFINE_ENTITY(VentureCalendarEvent, venture_calendar_event, event_fields)
/* A public scheduling link. Availability is a JSON object of weekday to
 * "HH:MM-HH:MM[,HH:MM-HH:MM]" windows in the owner's IANA timezone. */
static const VentureFieldDecl booking_fields[] = {
	VENTURE_FIELD_NAME("title", "Title", "Shown on the public page"),
	VENTURE_FIELD("slug", "Slug", "The public path is /book/<slug>", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION),
	VENTURE_FIELD("owner", "Owner", "Username who takes the meetings", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED | VENTURE_COLUMN_FLAG_ASSIGNED_USERNAME),
	INT("duration-minutes", "Duration", "Minutes per slot"),
	INT("buffer-minutes", "Buffer", "Minutes kept free before and after every meeting"),
	STR("timezone", "Timezone", "IANA zone the availability windows are written in, for example America/Chicago"),
	VENTURE_FIELD("availability", "Availability", "{\"mon\":\"09:00-12:00,13:00-17:00\",...}", VENTURE_FIELD_KIND_JSON, VENTURE_COLUMN_FLAG_NONE),
	INT("horizon-days", "Horizon", "How many days ahead may be booked; 14 when empty"),
	VENTURE_FIELD_TEXT("description", "Description", "Shown above the slots"),
	VENTURE_FIELD("active", "Active", "Whether the public page answers", VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VentureBookingPage, venture_booking_page, booking_fields)
