/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <string.h>
#include "venture-test-util.h"

typedef struct {
	VentureDatabase *db;
	VentureFakeCalDavClient *caldav;
	VentureCalendarSyncService *service;
	VentureBookingService *booking;
	gint64 org;
	gint64 contact;
} Fixture;

static void save(Fixture *f, VentureEntity *e)
{
	g_autoptr(GError) error = NULL;
	g_assert_true(venture_database_save(f->db, e, NULL, &error));
	g_assert_no_error(error);
}
static gint64 count_type(Fixture *f, const gchar *name)
{
	g_autoptr(VentureQuery) q = venture_query_new(venture_entity_registry_lookup(venture_entity_registry_get_default(), name));
	venture_query_set_organization(q, f->org);
	return venture_database_count(f->db, q, NULL);
}
static void setup(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) contact = NULL;
	(void)data;
	f->db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ORGANIZATION);
		g_autoptr(VentureEntity) org = venture_database_find_one(f->db, query, &error);
		f->org = venture_entity_get_id(org);
	}
	contact = g_object_new(VENTURE_TYPE_CONTACT, "organization-id", f->org, "name", "Ada", "email", "Ada+crm@Example.test", NULL);
	save(f, contact);
	f->contact = venture_entity_get_id(contact);
	f->caldav = venture_fake_caldav_client_new();
	f->service = venture_calendar_sync_service_new(f->db, VENTURE_CALDAV_CLIENT(f->caldav));
	f->booking = venture_booking_service_new(f->db);
	g_setenv("VENTURE_CALDAV_SECRET", "app-password", TRUE);
}
static void teardown(Fixture *f, gconstpointer data)
{
	(void)data;
	g_clear_object(&f->booking);
	g_clear_object(&f->service);
	g_clear_object(&f->caldav);
	g_clear_object(&f->db);
}
static VentureEntity *account(Fixture *f, const gchar *secret_env)
{
	VentureEntity *a = g_object_new(VENTURE_TYPE_CALENDAR_ACCOUNT, "organization-id", f->org, "url", "https://dav.venture.test/",
		"owner", "ben", "username", "ben@venture.test", "secret-env", secret_env, "calendar-path", "/calendars/ben/default/", "active", TRUE, NULL);
	save(f, a);
	return a;
}
static VentureEntity *meeting(Fixture *f, const gchar *owner, const gchar *subject, const gchar *starts, const gchar *ends)
{
	g_autoptr(GDateTime) s = venture_time_from_string(starts, NULL);
	g_autoptr(GDateTime) e = ends ? venture_time_from_string(ends, NULL) : NULL;
	VentureEntity *m = g_object_new(VENTURE_TYPE_ACTIVITY, "organization-id", f->org, "kind", VENTURE_ACTIVITY_KIND_MEETING, "owner", owner,
		"subject", subject, "starts-at", s, "ends-at", e, "due-at", s, "status", VENTURE_ACTIVITY_STATUS_PLANNED, NULL);
	save(f, m);
	return m;
}
static gint run_sync(Fixture *f, VentureEntity *a)
{
	g_autoptr(GError) error = NULL;
	gint n = venture_calendar_sync_service_sync(f->service, a, NULL, &error);
	g_assert_no_error(error);
	return n;
}
static VentureEntity *activity_by_subject(Fixture *f, const gchar *subject)
{
	g_autoptr(VentureQuery) q = venture_query_new(VENTURE_TYPE_ACTIVITY);
	venture_query_set_organization(q, f->org);
	venture_query_add_filter_string(q, "subject", VENTURE_FILTER_OP_EQ, subject, NULL);
	return venture_database_find_one(f->db, q, NULL);
}
static gchar *remote_ics(const gchar *uid, const gchar *summary, const gchar *start, const gchar *end, const gchar *modified, const gchar *extra)
{
	return g_strdup_printf("BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//Other//EN\r\nBEGIN:VEVENT\r\nUID:%s\r\nDTSTAMP:20260917T100000Z\r\n"
		"DTSTART:%s\r\nDTEND:%s\r\nSUMMARY:%s\r\nLAST-MODIFIED:%s\r\n%sEND:VEVENT\r\nEND:VCALENDAR\r\n", uid, start, end, summary, modified, extra ? extra : "");
}
/* The audit notes the sync leaves on an activity's timeline. */
static GPtrArray *timeline_notes(Fixture *f, VentureEntity *activity)
{
	g_autoptr(VentureQuery) q = venture_query_new(VENTURE_TYPE_AUDIT_ENTRY);
	venture_query_add_filter_string(q, "target-type", VENTURE_FILTER_OP_EQ, "activity", NULL);
	venture_query_add_filter_int(q, "target-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(activity), NULL);
	venture_query_add_filter_string(q, "source", VENTURE_FILTER_OP_EQ, "calendar", NULL);
	venture_query_add_order(q, "id", VENTURE_SORT_ASCENDING, NULL);
	return venture_database_find(f->db, q, NULL);
}

/* Rule 1: the record types exist, the module exists, and a missing secret is a named refusal. */
static void test_records(void)
{
	VentureEntityRegistry *r = venture_entity_registry_get_default();
	g_assert_cmpuint(venture_entity_registry_lookup(r, "calendar_account"), !=, G_TYPE_INVALID);
	g_assert_cmpuint(venture_entity_registry_lookup(r, "calendar_event"), !=, G_TYPE_INVALID);
	g_assert_cmpuint(venture_entity_registry_lookup(r, "booking_page"), !=, G_TYPE_INVALID);
	{
		g_autoptr(VentureModuleRegistry) modules = venture_module_registry_new();
		VentureModule *module;
		venture_module_registry_register_builtins(modules);
		module = venture_module_registry_lookup(modules, "calendar");
		g_assert_nonnull(module);
		g_assert_true(g_strv_contains(venture_module_get_requires(module), "activities"));
	}
}
static void test_missing_secret(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) a = account(f, "VENTURE_CALDAV_MISSING");
	g_autoptr(GError) error = NULL;
	(void)data;
	g_unsetenv("VENTURE_CALDAV_MISSING");
	g_assert_cmpint(venture_calendar_sync_service_sync(f->service, a, NULL, &error), ==, -1);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
	g_assert_nonnull(strstr(error->message, "VENTURE_CALDAV_MISSING"));
	g_assert_cmpint(venture_fake_caldav_client_get_connects(f->caldav), ==, 0);
	{
		g_autofree gchar *last_error = NULL;
		g_object_get(a, "last-error", &last_error, NULL);
		g_assert_null(last_error);
	}
}
/* A row must not be able to name VENTURE_SMTP_PASSWORD or the session secret. */
static void test_secret_env_prefix(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) a = account(f, "VENTURE_SMTP_PASSWORD");
	g_autoptr(GError) error = NULL;
	(void)data;
	g_setenv("VENTURE_SMTP_PASSWORD", "not-for-caldav", TRUE);
	g_assert_cmpint(venture_calendar_sync_service_sync(f->service, a, NULL, &error), ==, -1);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
	g_assert_nonnull(strstr(error->message, "VENTURE_CALDAV_"));
	g_assert_null(strstr(error->message, "not-for-caldav"));
	g_assert_cmpint(venture_fake_caldav_client_get_connects(f->caldav), ==, 0);
	g_unsetenv("VENTURE_SMTP_PASSWORD");
}

/* Rule 2a: dated calls and meetings of the owner are pushed with a stable UID; a rerun is a no-op. */
static void test_push(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) a = account(f, "VENTURE_CALDAV_SECRET");
	g_autoptr(VentureEntity) m = meeting(f, "ben", "Kickoff, phase 1", "2026-09-21T14:00:00Z", "2026-09-21T15:00:00Z");
	g_autoptr(VentureEntity) call = NULL, task = NULL, other = NULL;
	g_autofree gchar *uid = venture_calendar_sync_event_uid(m);
	g_autofree gchar *href = g_strdup_printf("/calendars/ben/default/%s.ics", uid);
	g_auto(GStrv) hrefs = NULL;
	g_autoptr(VentureICalEvent) event = NULL;
	g_autoptr(GError) error = NULL;
	(void)data;
	{
		g_autoptr(GDateTime) due = venture_time_from_string("2026-09-22T09:00:00Z", NULL);
		call = g_object_new(VENTURE_TYPE_ACTIVITY, "organization-id", f->org, "kind", VENTURE_ACTIVITY_KIND_CALL, "owner", "ben", "subject", "Call Ada", "due-at", due, NULL);
		save(f, call);
		task = g_object_new(VENTURE_TYPE_ACTIVITY, "organization-id", f->org, "kind", VENTURE_ACTIVITY_KIND_TASK, "owner", "ben", "subject", "Write proposal", "due-at", due, NULL);
		save(f, task);
	}
	other = meeting(f, "someone-else", "Not Ben's", "2026-09-21T16:00:00Z", NULL);
	g_assert_cmpint(run_sync(f, a), ==, 2);
	g_assert_cmpint(venture_fake_caldav_client_get_puts(f->caldav), ==, 2);
	hrefs = venture_fake_caldav_client_get_hrefs(f->caldav);
	g_assert_cmpuint(g_strv_length(hrefs), ==, 2);
	g_assert_true(g_strv_contains((const gchar *const *)hrefs, href));
	event = venture_ical_event_parse(venture_fake_caldav_client_get_remote(f->caldav, href), &error);
	g_assert_no_error(error);
	g_assert_cmpstr(event->uid, ==, uid);
	g_assert_cmpstr(event->summary, ==, "Kickoff, phase 1");
	g_assert_cmpstr(event->status, ==, "CONFIRMED");
	g_assert_nonnull(strstr(venture_fake_caldav_client_get_remote(f->caldav, href), "SUMMARY:Kickoff\\, phase 1"));
	{
		g_autoptr(GDateTime) starts = venture_time_from_string("2026-09-21T14:00:00Z", NULL);
		g_assert_true(g_date_time_equal(event->starts, starts));
	}
	g_assert_cmpint(count_type(f, "calendar_event"), ==, 2);
	{
		g_autofree gchar *token = NULL;
		g_autoptr(GDateTime) synced = NULL;
		g_object_get(a, "sync-token", &token, "last-synced-at", &synced, NULL);
		g_assert_nonnull(token);
		g_assert_nonnull(synced);
	}
	/* Nothing changed anywhere: no fetch, no put, no write. */
	g_assert_cmpint(run_sync(f, a), ==, 0);
	g_assert_cmpint(venture_fake_caldav_client_get_puts(f->caldav), ==, 2);
	g_assert_cmpint(venture_fake_caldav_client_get_fetches(f->caldav), ==, 0);
	g_assert_cmpint(count_type(f, "calendar_event"), ==, 2);
	/* A local edit is pushed once, with the server's precondition. */
	g_object_set(m, "subject", "Kickoff, phase 2", NULL);
	save(f, m);
	g_assert_cmpint(run_sync(f, a), ==, 1);
	g_assert_cmpint(venture_fake_caldav_client_get_puts(f->caldav), ==, 3);
	g_assert_nonnull(strstr(venture_fake_caldav_client_get_remote(f->caldav, href), "phase 2"));
	g_assert_cmpint(run_sync(f, a), ==, 0);
}

/* Rule 2b: new and changed VEVENTs become meetings, idempotently by UID. */
static void test_pull(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) a = account(f, "VENTURE_CALDAV_SECRET");
	g_autoptr(VentureEntity) activity = NULL, again = NULL;
	g_autofree gchar *ics = remote_ics("abc-123@phone", "Dentist", "20260922T130000Z", "20260922T140000Z", "20260917T090000Z",
		"DESCRIPTION:Bring the\\, forms\\nSecond line\r\nRRULE:FREQ=WEEKLY\r\nBEGIN:VALARM\r\nTRIGGER:-PT10M\r\nEND:VALARM\r\n");
	g_autofree gchar *changed = NULL;
	(void)data;
	venture_fake_caldav_client_set_remote(f->caldav, "/calendars/ben/default/abc-123.ics", ics);
	g_assert_cmpint(run_sync(f, a), ==, 1);
	g_assert_cmpint(count_type(f, "activity"), ==, 1);
	activity = activity_by_subject(f, "Dentist");
	g_assert_nonnull(activity);
	{
		g_autoptr(GDateTime) starts = NULL, ends = NULL, due = NULL, expected = venture_time_from_string("2026-09-22T13:00:00Z", NULL);
		g_autofree gchar *owner = NULL, *body = NULL;
		gint kind, status, recurrence;
		g_object_get(activity, "kind", &kind, "owner", &owner, "starts-at", &starts, "ends-at", &ends, "due-at", &due, "status", &status, "body", &body, "recurrence", &recurrence, NULL);
		g_assert_cmpint(kind, ==, VENTURE_ACTIVITY_KIND_MEETING);
		g_assert_cmpstr(owner, ==, "ben");
		g_assert_true(g_date_time_equal(starts, expected));
		g_assert_true(g_date_time_equal(due, expected));
		g_assert_cmpint(g_date_time_difference(ends, starts), ==, G_TIME_SPAN_HOUR);
		g_assert_cmpint(status, ==, VENTURE_ACTIVITY_STATUS_PLANNED);
		g_assert_cmpstr(body, ==, "Bring the, forms\nSecond line");
		g_assert_cmpint(recurrence, ==, VENTURE_ACTIVITY_RECURRENCE_WEEKLY);
	}
	/* The pulled event is not pushed back, and a rerun creates nothing. */
	g_assert_cmpint(venture_fake_caldav_client_get_puts(f->caldav), ==, 0);
	g_assert_cmpint(run_sync(f, a), ==, 0);
	g_assert_cmpint(count_type(f, "activity"), ==, 1);
	g_assert_cmpint(venture_fake_caldav_client_get_fetches(f->caldav), ==, 1);
	/* The same UID changed on the server updates the one activity. */
	changed = remote_ics("abc-123@phone", "Dentist (moved)", "20260923T130000Z", "20260923T140000Z", "20260917T100000Z", NULL);
	venture_fake_caldav_client_set_remote(f->caldav, "/calendars/ben/default/abc-123.ics", changed);
	g_assert_cmpint(run_sync(f, a), ==, 1);
	g_assert_cmpint(count_type(f, "activity"), ==, 1);
	again = activity_by_subject(f, "Dentist (moved)");
	g_assert_nonnull(again);
	g_assert_cmpint(venture_entity_get_id(again), ==, venture_entity_get_id(activity));
	g_assert_cmpint(venture_fake_caldav_client_get_puts(f->caldav), ==, 0);
	g_assert_cmpint(run_sync(f, a), ==, 0);
}

/* Rule 2c: a deletion on either side cancels the other side; nothing is deleted. */
static void test_deletions_cancel(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) a = account(f, "VENTURE_CALDAV_SECRET");
	g_autoptr(VentureEntity) pulled = NULL, pushed = NULL, dropped = NULL;
	g_autofree gchar *ics = remote_ics("gone@phone", "Vanishing", "20260922T130000Z", "20260922T140000Z", "20260917T090000Z", NULL);
	g_autofree gchar *href = NULL;
	g_autoptr(GError) error = NULL;
	gint status;
	(void)data;
	venture_fake_caldav_client_set_remote(f->caldav, "/calendars/ben/default/gone.ics", ics);
	pushed = meeting(f, "ben", "Ours", "2026-09-24T14:00:00Z", "2026-09-24T15:00:00Z");
	dropped = meeting(f, "ben", "Deleted here", "2026-09-25T14:00:00Z", "2026-09-25T15:00:00Z");
	g_assert_cmpint(run_sync(f, a), ==, 3);
	pulled = activity_by_subject(f, "Vanishing");
	g_assert_nonnull(pulled);
	/* Removed on the server: the activity is cancelled, still there, with a note. */
	venture_fake_caldav_client_remove_remote(f->caldav, "/calendars/ben/default/gone.ics");
	g_assert_cmpint(run_sync(f, a), ==, 1);
	g_clear_object(&pulled);
	pulled = activity_by_subject(f, "Vanishing");
	g_assert_nonnull(pulled);
	g_assert_false(venture_entity_is_deleted(pulled));
	g_object_get(pulled, "status", &status, NULL);
	g_assert_cmpint(status, ==, VENTURE_ACTIVITY_STATUS_CANCELLED);
	{
		g_autoptr(GPtrArray) notes = timeline_notes(f, pulled);
		g_assert_cmpuint(notes->len, ==, 1);
	}
	g_assert_cmpint(run_sync(f, a), ==, 0);
	/* Cancelled here: the calendar copy becomes STATUS:CANCELLED rather than disappearing. */
	{
		g_autoptr(VentureEntity) acted = venture_activity_service_act(venture_database_get_activity_service(f->db), pushed, "cancel", NULL, NULL, &error);
		g_assert_no_error(error);
		g_assert_nonnull(acted);
	}
	href = g_strdup_printf("/calendars/ben/default/%s@venture.ics", venture_entity_get_uuid(pushed));
	g_assert_cmpint(run_sync(f, a), ==, 1);
	g_assert_nonnull(venture_fake_caldav_client_get_remote(f->caldav, href));
	g_assert_nonnull(strstr(venture_fake_caldav_client_get_remote(f->caldav, href), "STATUS:CANCELLED"));
	/* Deleted here (soft): the same, once. */
	g_assert_true(venture_database_delete(f->db, dropped, NULL, &error));
	g_assert_no_error(error);
	g_free(href);
	href = g_strdup_printf("/calendars/ben/default/%s@venture.ics", venture_entity_get_uuid(dropped));
	g_assert_cmpint(run_sync(f, a), ==, 1);
	g_assert_nonnull(strstr(venture_fake_caldav_client_get_remote(f->caldav, href), "STATUS:CANCELLED"));
	g_assert_cmpint(run_sync(f, a), ==, 0);
}

/* Rule 3: changed on both sides, the later modification wins and the loser is noted. */
static void test_conflict_last_modified_wins(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) a = account(f, "VENTURE_CALDAV_SECRET");
	g_autoptr(VentureEntity) m = meeting(f, "ben", "Local title", "2026-09-21T14:00:00Z", "2026-09-21T15:00:00Z");
	g_autofree gchar *uid = venture_calendar_sync_event_uid(m);
	g_autofree gchar *href = g_strdup_printf("/calendars/ben/default/%s.ics", uid);
	g_autofree gchar *newer = NULL, *older = NULL;
	(void)data;
	g_assert_cmpint(run_sync(f, a), ==, 1);
	/* Both move; the calendar's LAST-MODIFIED is far later than the local edit. */
	g_object_set(m, "subject", "Local edit", NULL);
	save(f, m);
	newer = remote_ics(uid, "Remote edit", "20260921T140000Z", "20260921T150000Z", "20991231T000000Z", NULL);
	venture_fake_caldav_client_set_remote(f->caldav, href, newer);
	g_assert_cmpint(run_sync(f, a), ==, 1);
	{
		g_autoptr(VentureEntity) now = venture_database_get(f->db, VENTURE_TYPE_ACTIVITY, venture_entity_get_id(m), NULL);
		g_autoptr(GPtrArray) notes = timeline_notes(f, m);
		g_autofree gchar *subject = NULL, *diff = NULL;
		g_object_get(now, "subject", &subject, NULL);
		g_assert_cmpstr(subject, ==, "Remote edit");
		g_assert_cmpuint(notes->len, ==, 1);
		g_object_get(g_ptr_array_index(notes, 0), "diff", &diff, NULL);
		g_assert_nonnull(strstr(diff, "Local edit"));
		g_assert_nonnull(strstr(diff, "\"note\""));
	}
	g_assert_cmpint(venture_fake_caldav_client_get_puts(f->caldav), ==, 1);
	g_assert_cmpint(run_sync(f, a), ==, 0);
	/* Both move again; this time the calendar copy is older than the local edit, so ours wins and is pushed. */
	{
		g_autoptr(VentureEntity) fresh = venture_database_get(f->db, VENTURE_TYPE_ACTIVITY, venture_entity_get_id(m), NULL);
		g_object_set(fresh, "subject", "Local wins", NULL);
		save(f, fresh);
	}
	older = remote_ics(uid, "Stale remote", "20260921T140000Z", "20260921T150000Z", "20000101T000000Z", NULL);
	venture_fake_caldav_client_set_remote(f->caldav, href, older);
	g_assert_cmpint(run_sync(f, a), ==, 1);
	g_assert_nonnull(strstr(venture_fake_caldav_client_get_remote(f->caldav, href), "SUMMARY:Local wins"));
	{
		g_autoptr(VentureEntity) now = venture_database_get(f->db, VENTURE_TYPE_ACTIVITY, venture_entity_get_id(m), NULL);
		g_autoptr(GPtrArray) notes = timeline_notes(f, m);
		g_autofree gchar *subject = NULL, *diff = NULL;
		g_object_get(now, "subject", &subject, NULL);
		g_assert_cmpstr(subject, ==, "Local wins");
		g_assert_cmpuint(notes->len, ==, 2);
		g_object_get(g_ptr_array_index(notes, 1), "diff", &diff, NULL);
		g_assert_nonnull(strstr(diff, "Stale remote"));
	}
	g_assert_cmpint(run_sync(f, a), ==, 0);
}

/* The iCalendar reader and writer: folding, TZID, escapes, all-day dates. */
static void test_icalendar(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureICalEvent) event = venture_ical_event_parse(
		"BEGIN:VCALENDAR\r\nBEGIN:VTIMEZONE\r\nTZID:America/Chicago\r\nEND:VTIMEZONE\r\nBEGIN:VEVENT\r\nUID:x1\r\n"
		"DTSTART;TZID=America/Chicago:20260921T090000\r\nDTEND;TZID=America/Chicago:20260921T093000\r\n"
		"SUMMARY:A very long summary that the writer will certainly have to fold across more than one\r\n  line of seventy-five octets\r\n"
		"END:VEVENT\r\nEND:VCALENDAR\r\n", &error);
	g_autoptr(VentureICalEvent) day = NULL, none = NULL, back = NULL;
	g_autofree gchar *text = NULL;
	g_autoptr(GDateTime) expected = venture_time_from_string("2026-09-21T14:00:00Z", NULL);
	g_assert_no_error(error);
	g_assert_nonnull(event);
	g_assert_true(g_date_time_equal(event->starts, expected));
	g_assert_cmpstr(event->summary, ==, "A very long summary that the writer will certainly have to fold across more than one line of seventy-five octets");
	day = venture_ical_event_parse("BEGIN:VCALENDAR\nBEGIN:VEVENT\nUID:d1\nDTSTART;VALUE=DATE:20260922\nSUMMARY:Holiday\nEND:VEVENT\nEND:VCALENDAR\n", &error);
	g_assert_no_error(error);
	g_assert_true(day->all_day);
	g_assert_cmpint(g_date_time_difference(day->ends, day->starts), ==, G_TIME_SPAN_DAY);
	none = venture_ical_event_parse("BEGIN:VCALENDAR\r\nBEGIN:VTODO\r\nUID:t\r\nEND:VTODO\r\nEND:VCALENDAR\r\n", &error);
	g_assert_null(none);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
	g_clear_error(&error);
	text = venture_ical_event_format(event, expected);
	back = venture_ical_event_parse(text, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(back->summary, ==, event->summary);
	g_assert_true(g_date_time_equal(back->starts, event->starts));
	g_assert_nonnull(strstr(text, "DTSTART:20260921T140000Z\r\n"));
	g_assert_nonnull(strstr(text, "\r\n "));
}

/* Rule 4: the scheduling link offers free slots in the owner's zone and refuses a double booking. */
static VentureEntity *page(Fixture *f)
{
	VentureEntity *p = g_object_new(VENTURE_TYPE_BOOKING_PAGE, "organization-id", f->org, "title", "Intro call", "slug", "ben-intro", "owner", "ben",
		"duration-minutes", (gint64)30, "buffer-minutes", (gint64)0, "timezone", "America/Chicago",
		"availability", "{\"mon\":\"09:00-10:00\",\"tue\":\"09:00-09:30\"}", "horizon-days", (gint64)2, "active", TRUE, NULL);
	save(f, p);
	return p;
}
static void test_booking_slots(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) p = page(f);
	g_autoptr(VentureEntity) busy = NULL;
	g_autoptr(GDateTime) now = venture_time_from_string("2026-09-21T12:00:00Z", NULL); /* Monday 07:00 in Chicago */
	g_autoptr(JsonNode) slots = NULL;
	g_autoptr(GError) error = NULL;
	JsonArray *array;
	(void)data;
	slots = venture_booking_service_slots(f->booking, p, now, &error);
	g_assert_no_error(error);
	array = json_node_get_array(slots);
	/* Monday 09:00 and 09:30 CDT, Tuesday 09:00 CDT. */
	g_assert_cmpuint(json_array_get_length(array), ==, 3);
	g_assert_cmpstr(venture_json_object_get_string(json_array_get_object_element(array, 0), "start", ""), ==, "2026-09-21T14:00:00Z");
	g_assert_cmpstr(venture_json_object_get_string(json_array_get_object_element(array, 1), "start", ""), ==, "2026-09-21T14:30:00Z");
	g_assert_cmpstr(venture_json_object_get_string(json_array_get_object_element(array, 2), "start", ""), ==, "2026-09-22T14:00:00Z");
	g_assert_nonnull(strstr(venture_json_object_get_string(json_array_get_object_element(array, 0), "label", ""), "09:00"));
	/* A synced or planned meeting of the owner blocks its slot; a buffer widens it. */
	busy = meeting(f, "ben", "Blocked", "2026-09-21T14:00:00Z", "2026-09-21T14:30:00Z");
	g_clear_pointer(&slots, json_node_unref);
	slots = venture_booking_service_slots(f->booking, p, now, &error);
	g_assert_cmpuint(json_array_get_length(json_node_get_array(slots)), ==, 2);
	g_assert_cmpstr(venture_json_object_get_string(json_array_get_object_element(json_node_get_array(slots), 0), "start", ""), ==, "2026-09-21T14:30:00Z");
	g_object_set(p, "buffer-minutes", (gint64)15, NULL);
	save(f, p);
	g_clear_pointer(&slots, json_node_unref);
	slots = venture_booking_service_slots(f->booking, p, now, &error);
	g_assert_cmpuint(json_array_get_length(json_node_get_array(slots)), ==, 1);
	g_assert_cmpstr(venture_json_object_get_string(json_array_get_object_element(json_node_get_array(slots), 0), "start", ""), ==, "2026-09-22T14:00:00Z");
	/* Someone else's meeting does not block Ben. */
	g_object_set(p, "buffer-minutes", (gint64)0, NULL);
	save(f, p);
	{
		g_autoptr(VentureEntity) theirs = meeting(f, "someone-else", "Theirs", "2026-09-22T14:00:00Z", "2026-09-22T14:30:00Z");
		g_clear_pointer(&slots, json_node_unref);
		slots = venture_booking_service_slots(f->booking, p, now, &error);
		g_assert_cmpuint(json_array_get_length(json_node_get_array(slots)), ==, 2);
	}
}
static void test_booking_books_and_refuses_double(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) p = page(f);
	g_autoptr(VentureEntity) booked = NULL, dup = NULL, second = NULL, off = NULL;
	g_autoptr(GDateTime) now = venture_time_from_string("2026-09-21T12:00:00Z", NULL);
	g_autoptr(GError) error = NULL;
	(void)data;
	booked = venture_booking_service_book(f->booking, p, "2026-09-21T14:00:00Z", "Grace", "Grace+x@Hopper.test", "Talk about billing", now, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(booked);
	{
		g_autoptr(GDateTime) starts = NULL, ends = NULL, expected = venture_time_from_string("2026-09-21T14:00:00Z", NULL);
		g_autoptr(VentureEntity) contact = NULL;
		g_autofree gchar *owner = NULL, *subject = NULL, *email = NULL, *body = NULL;
		gint kind, status;
		gint64 contact_id = 0;
		g_object_get(booked, "kind", &kind, "status", &status, "owner", &owner, "subject", &subject, "starts-at", &starts, "ends-at", &ends, "contact-id", &contact_id, "body", &body, NULL);
		g_assert_cmpint(kind, ==, VENTURE_ACTIVITY_KIND_MEETING);
		g_assert_cmpint(status, ==, VENTURE_ACTIVITY_STATUS_PLANNED);
		g_assert_cmpstr(owner, ==, "ben");
		g_assert_cmpstr(subject, ==, "Intro call with Grace");
		g_assert_true(g_date_time_equal(starts, expected));
		g_assert_cmpint(g_date_time_difference(ends, starts), ==, 30 * G_TIME_SPAN_MINUTE);
		g_assert_nonnull(strstr(body, "Talk about billing"));
		g_assert_cmpint(contact_id, >, 0);
		contact = venture_database_get(f->db, VENTURE_TYPE_CONTACT, contact_id, NULL);
		g_object_get(contact, "email", &email, NULL);
		g_assert_cmpstr(email, ==, "grace@hopper.test");
		g_assert_cmpint(count_type(f, "contact"), ==, 2);
	}
	/* The same slot again is refused, and nothing is written. */
	dup = venture_booking_service_book(f->booking, p, "2026-09-21T14:00:00Z", "Linus", "linus@example.test", NULL, now, NULL, &error);
	g_assert_null(dup);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
	g_clear_error(&error);
	g_assert_cmpint(count_type(f, "contact"), ==, 2);
	g_assert_cmpint(count_type(f, "activity"), ==, 1);
	/* A time that was never offered is refused too. */
	off = venture_booking_service_book(f->booking, p, "2026-09-21T14:10:00Z", "Linus", "linus@example.test", NULL, now, NULL, &error);
	g_assert_null(off);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
	g_clear_error(&error);
	/* An existing contact is found by normalised email, the way leads deduplicate. */
	second = venture_booking_service_book(f->booking, p, "2026-09-21T14:30:00Z", "Ada L", "ada@example.test", NULL, now, NULL, &error);
	g_assert_no_error(error);
	{
		gint64 contact_id = 0;
		g_object_get(second, "contact-id", &contact_id, NULL);
		g_assert_cmpint(contact_id, ==, f->contact);
	}
	g_assert_cmpint(count_type(f, "contact"), ==, 2);
	g_assert_cmpint(count_type(f, "activity"), ==, 2);
	/* Missing name or email is a validation refusal. */
	g_clear_object(&off);
	off = venture_booking_service_book(f->booking, p, "2026-09-22T14:00:00Z", "", "x@example.test", NULL, now, NULL, &error);
	g_assert_null(off);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	off = venture_booking_service_book(f->booking, p, "2026-09-22T14:00:00Z", "X", "not-an-address", NULL, now, NULL, &error);
	g_assert_null(off);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	/* An inactive page is not found by its slug. */
	g_object_set(p, "active", FALSE, NULL);
	save(f, p);
	g_assert_null(venture_booking_service_find_page(f->booking, "ben-intro", &error));
	g_assert_no_error(error);
}

/* Rule 5: the organization sweep, and its report of a failing account. */
static void test_sweep(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) a = account(f, "VENTURE_CALDAV_SECRET");
	g_autoptr(VentureEntity) broken = account(f, "VENTURE_CALDAV_MISSING");
	g_autoptr(VentureEntity) off = account(f, "VENTURE_CALDAV_MISSING");
	g_autoptr(VentureEntity) m = meeting(f, "ben", "Sweep me", "2026-09-21T14:00:00Z", NULL);
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) report = NULL;
	JsonArray *errors;
	(void)data;
	g_unsetenv("VENTURE_CALDAV_MISSING");
	g_object_set(off, "active", FALSE, NULL);
	save(f, off);
	report = venture_calendar_sync_service_sweep(f->service, f->org, 100, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(report);
	g_assert_cmpint(venture_json_object_get_int(json_node_get_object(report), "accounts", 0), ==, 2);
	g_assert_cmpint(venture_json_object_get_int(json_node_get_object(report), "changes", 0), ==, 1);
	errors = json_object_get_array_member(json_node_get_object(report), "errors");
	g_assert_cmpuint(json_array_get_length(errors), ==, 1);
	g_assert_nonnull(strstr(venture_json_object_get_string(json_array_get_object_element(errors, 0), "error", ""), "VENTURE_CALDAV_MISSING"));
	{
		g_autoptr(VentureEntity) noted = venture_database_get(f->db, VENTURE_TYPE_CALENDAR_ACCOUNT, venture_entity_get_id(broken), NULL);
		g_autofree gchar *last_error = NULL;
		g_object_get(noted, "last-error", &last_error, NULL);
		g_assert_null(last_error); /* the refusal happened before any sweep state was written */
	}
	g_assert_null(venture_calendar_sync_service_sweep(f->service, 0, 100, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/calendar/records", test_records);
	g_test_add_func("/calendar/icalendar", test_icalendar);
	g_test_add("/calendar/missing-secret", Fixture, NULL, setup, test_missing_secret, teardown);
	g_test_add("/calendar/secret-env-prefix", Fixture, NULL, setup, test_secret_env_prefix, teardown);
	g_test_add("/calendar/push", Fixture, NULL, setup, test_push, teardown);
	g_test_add("/calendar/pull", Fixture, NULL, setup, test_pull, teardown);
	g_test_add("/calendar/deletions-cancel", Fixture, NULL, setup, test_deletions_cancel, teardown);
	g_test_add("/calendar/conflict-last-modified-wins", Fixture, NULL, setup, test_conflict_last_modified_wins, teardown);
	g_test_add("/calendar/booking-slots", Fixture, NULL, setup, test_booking_slots, teardown);
	g_test_add("/calendar/booking-books-and-refuses-double", Fixture, NULL, setup, test_booking_books_and_refuses_double, teardown);
	g_test_add("/calendar/sweep", Fixture, NULL, setup, test_sweep, teardown);
	return g_test_run();
}
