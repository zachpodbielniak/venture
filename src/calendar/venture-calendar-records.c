/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#define STR(n, l, h) VENTURE_FIELD(n, l, h, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE)
#define INT(n, l, h) VENTURE_FIELD(n, l, h, VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE)
#define DATE(n, l) VENTURE_FIELD(n, l, NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_INDEXED)
/* Account identity is public metadata; an explicit encrypted binding supplies
 * the credential. Historical environment labels are never resolved. */
static const VentureFieldDecl account_fields[] = {
	VENTURE_FIELD_REF("private-owner-id", "Private owner", "User whose private connector this is; zero explicitly shares business data with the organization", "user", VENTURE_COLUMN_FLAG_OPTIONAL_PERSONAL_OWNER),
	VENTURE_FIELD_NAME("url", "CalDAV URL", "The server's CalDAV base, for example https://dav.example.net/"),
	VENTURE_FIELD("owner", "Owner", "Username whose activities this calendar mirrors", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED | VENTURE_COLUMN_FLAG_ASSIGNED_USERNAME),
	STR("username", "Username", NULL),
	STR("secret-env", "Legacy secret variable", "Unused historical metadata; configure an explicit encrypted connector binding"),
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
	VENTURE_FIELD_NAME("subject", "Subject", "Imported calendar event title"),
	DATE("starts-at", "Starts"),
	DATE("ends-at", "Ends"),
	VENTURE_FIELD_ENUM("status", "Status", "Imported calendar event state", venture_activity_status_get_type, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_TEXT("body", "Description", "Imported event description; ownership follows its account"),
	DATE("due-at", "Due"),
	VENTURE_FIELD_REF("account-id", "Account", NULL, "calendar_account", VENTURE_COLUMN_FLAG_OPTIONAL_PERSONAL_OWNER),
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
	INT("capacity", "Seats per slot", "Zero uses one seat; at most 10000"),
	STR("public-origin", "Public origin", "HTTPS origin for private booking management links"),
	VENTURE_FIELD_TEXT("description", "Description", "Shown above the slots"),
	VENTURE_FIELD("active", "Active", "Whether the public page answers", VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VentureBookingPage, venture_booking_page, booking_fields)

/* Holds are service-owned working copies: a payment hold or a nested intake
 * must reserve capacity without emitting a business event or exposing a key. */
static const VentureFieldDecl reservation_fields[] = {
	VENTURE_FIELD_NAME("name", "Reservation", "Service-owned slot reservation"),
	VENTURE_FIELD_REF("page-id", "Booking page", NULL, "booking_page", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_REF("activity-id", "Meeting", NULL, "activity", VENTURE_COLUMN_FLAG_NONE),
	STR("owner", "Owner", "Owner whose calendar is reserved"),
	DATE("starts-at", "Starts"), DATE("ends-at", "Ends"), DATE("expires-at", "Hold expires"),
	VENTURE_FIELD("state", "State", "held, confirmed, cancelled or expired", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("secret", "Signing secret", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_SENSITIVE),
	VENTURE_FIELD("public-origin", "Public origin", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_SENSITIVE),
	INT("generation", "Internal revision", "Changed only by the service")
};
VENTURE_DEFINE_ENTITY_WITH_CODE(VentureBookingReservation, venture_booking_reservation, reservation_fields,
	venture_entity_class_set_working_copy(VENTURE_ENTITY_CLASS(klass));)
