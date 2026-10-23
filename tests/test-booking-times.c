/*
 * test-booking-times.c - Bookings at a time of day, and a technician's day.
 *
 * Every date-and-time field was drawn as a date box, so a booking for 8 to
 * 10 on Tuesday was saved at midnight, a same-day booking was refused for
 * ending before it started, and the calendar showed a time nobody chose.
 * What breaks if this regresses: arrival windows lose their hours, wall
 * time is read in the wrong zone, or a technician's day lists the wrong
 * jobs or in the wrong order.
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <venture.h>
#include <stdio.h>
#include <string.h>
#include <libsoup/soup.h>
#include "venture-test-util.h"

typedef struct
{
	VentureDatabase *db;
	VentureConfig *config;
	VentureContext *context;
	VentureWebServer *server;
	gchar *dir;
	gint64 org;
	gint64 company;
} Fixture;

typedef struct
{
	GBytes *bytes;
	GError *error;
	gboolean done;
} Response;

static void
save(Fixture *f, VentureEntity *record)
{
	g_autoptr(GError) error = NULL;
	g_assert_true(venture_database_save(f->db, record, NULL, &error));
	g_assert_no_error(error);
}

static void
setup(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureCompany) company = NULL;
	(void)data;
	f->config = venture_config_new();
	g_object_set(f->config, "locale-timezone", "America/New_York", NULL);
	f->db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->db);
	f->org = venture_context_get_default_organization_id(f->context);
	company = venture_company_new();
	g_object_set(company, "name", "Example Household", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(company), f->org);
	save(f, VENTURE_ENTITY(company));
	f->company = venture_entity_get_id(VENTURE_ENTITY(company));
	f->dir = g_dir_make_tmp("venture-booking-times-XXXXXX", NULL);
	g_object_set(f->config, "state-dir", f->dir, "server-bind-address", "127.0.0.1",
		"server-port", (gint64)0, "security-require-auth", FALSE, NULL);
	f->server = venture_web_server_new(f->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(f->server, &error));
	g_assert_no_error(error);
}

static void
teardown(Fixture *f, gconstpointer data)
{
	(void)data;
	venture_web_server_stop(f->server);
	g_clear_object(&f->server);
	venture_test_remove_tree(f->dir);
	g_clear_pointer(&f->dir, g_free);
	g_clear_object(&f->context);
	g_clear_object(&f->db);
	g_clear_object(&f->config);
}

static void
response_done(GObject *source, GAsyncResult *result, gpointer data)
{
	Response *response = data;
	response->bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &response->error);
	response->done = TRUE;
}

/* The server shares this context, so the request runs on the loop. */
static guint
request(Fixture *f, const gchar *method, const gchar *path, const gchar *form,
	gchar **location, gchar **out)
{
	g_autoptr(SoupSession) session = soup_session_new_with_options("timeout", 15, NULL);
	g_autoptr(SoupMessage) message = NULL;
	g_autofree gchar *url = g_strconcat(venture_web_server_get_base_url(f->server), path, NULL);
	Response response;
	memset(&response, 0, sizeof(response));
	message = soup_message_new(method, url);
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	if (form != NULL)
	{
		g_autoptr(GBytes) payload = g_bytes_new(form, strlen(form));
		soup_message_set_request_body_from_bytes(message, "application/x-www-form-urlencoded", payload);
	}
	soup_session_send_and_read_async(session, message, G_PRIORITY_DEFAULT, NULL, response_done, &response);
	while (!response.done)
		g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(response.error);
	if (out != NULL)
		*out = response.bytes != NULL
			? g_strndup(g_bytes_get_data(response.bytes, NULL), g_bytes_get_size(response.bytes))
			: g_strdup("");
	if (location != NULL)
		*location = g_strdup(soup_message_headers_get_one(soup_message_get_response_headers(message), "Location"));
	g_clear_pointer(&response.bytes, g_bytes_unref);
	return soup_message_get_status(message);
}

static GPtrArray *
rows(Fixture *f, GType type)
{
	g_autoptr(VentureQuery) query = venture_query_new(type);
	g_autoptr(GError) error = NULL;
	GPtrArray *found;
	venture_query_set_limit(query, 0);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	found = venture_database_find(f->db, query, &error);
	g_assert_no_error(error);
	g_assert_nonnull(found);
	return found;
}

/* A draft quote with one line, made the way the sheet makes it. */
/* The wall time a stored instant reads in the business's zone, "HH:MM". */
static gchar *
wall(Fixture *f, GDateTime *when)
{
	g_autoptr(GDateTime) local = g_date_time_to_timezone(when, venture_context_get_timezone(f->context));
	return g_date_time_format(local, "%Y-%m-%d %H:%M");
}

static gint64
make_user(Fixture *f, const gchar *username)
{
	g_autoptr(VentureUser) user = venture_user_new();
	g_object_set(user, "username", username, "active", TRUE, NULL);
	save(f, VENTURE_ENTITY(user));
	return venture_entity_get_id(VENTURE_ENTITY(user));
}

/* A booking on @day from @from to @to, in the business's wall time. */
static gint64
booked(Fixture *f, const gchar *title, const gchar *from, const gchar *to, gint64 tech, gint status)
{
	GTimeZone *zone = venture_context_get_timezone(f->context);
	gint y, mo, d, h, mi;
	g_autoptr(GDateTime) start = NULL, end = NULL;
	g_autoptr(VentureEntity) job = g_object_new(VENTURE_TYPE_JOB, "title", title, "company-id", f->company,
		"status", status, NULL);
	g_assert_cmpint(sscanf(from, "%d-%d-%dT%d:%d", &y, &mo, &d, &h, &mi), ==, 5);
	start = g_date_time_new(zone, y, mo, d, h, mi, 0);
	g_assert_cmpint(sscanf(to, "%d-%d-%dT%d:%d", &y, &mo, &d, &h, &mi), ==, 5);
	end = g_date_time_new(zone, y, mo, d, h, mi, 0);
	g_object_set(job, "window-start", start, "window-end", end, "service-address", "12 Example Way", NULL);
	venture_entity_set_organization_id(job, f->org);
	save(f, job);
	if (tech > 0)
	{
		g_autoptr(VentureEntity) on = g_object_new(VENTURE_TYPE_JOB_TECHNICIAN, "job-id", venture_entity_get_id(job),
			"user-id", tech, NULL);
		venture_entity_set_organization_id(on, f->org);
		save(f, on);
	}
	return venture_entity_get_id(job);
}

/* Date-and-time fields are date-and-time boxes; dates stay dates. */
static void
test_datetime_boxes(Fixture *f, gconstpointer data)
{
	g_autofree gchar *job = NULL, *activity = NULL, *equipment = NULL;
	(void)data;
	g_assert_cmpuint(request(f, "GET", "/e/job/new", NULL, NULL, &job), ==, 200);
	g_assert_nonnull(strstr(job, "type=\"datetime-local\" name=\"window-start\""));
	g_assert_nonnull(strstr(job, "type=\"datetime-local\" name=\"window-end\""));
	g_assert_cmpuint(request(f, "GET", "/e/activity/new", NULL, NULL, &activity), ==, 200);
	g_assert_nonnull(strstr(activity, "type=\"datetime-local\" name=\"starts-at\""));
	g_assert_cmpuint(request(f, "GET", "/e/equipment/new", NULL, NULL, &equipment), ==, 200);
	g_assert_nonnull(strstr(equipment, "type=\"date\" name=\"warranty-until\""));
}

/* An arrival window typed as 8 to 10 is 8 to 10 in the business's zone,
 * and reads back the same in the form. */
static void
test_window_is_wall_time(Fixture *f, gconstpointer data)
{
	g_autofree gchar *form = g_strdup_printf("title=No+hot+water&company-id=%" G_GINT64_FORMAT
		"&window-start=2026-10-21T08:00&window-end=2026-10-21T10:00", f->company);
	g_autoptr(GPtrArray) jobs = NULL;
	g_autoptr(GDateTime) start = NULL, end = NULL;
	g_autofree gchar *from = NULL, *to = NULL, *edit = NULL, *page = NULL;
	(void)data;
	g_assert_cmpuint(request(f, "POST", "/e/job", form, NULL, NULL) / 100, ==, 3);
	jobs = rows(f, VENTURE_TYPE_JOB);
	g_assert_cmpuint(jobs->len, ==, 1);
	g_object_get(g_ptr_array_index(jobs, 0), "window-start", &start, "window-end", &end, NULL);
	from = wall(f, start);
	to = wall(f, end);
	g_assert_cmpstr(from, ==, "2026-10-21 08:00");
	g_assert_cmpstr(to, ==, "2026-10-21 10:00");
	edit = g_strdup_printf("/e/job/%" G_GINT64_FORMAT "/edit", venture_entity_get_id(g_ptr_array_index(jobs, 0)));
	g_assert_cmpuint(request(f, "GET", edit, NULL, NULL, &page), ==, 200);
	g_assert_nonnull(strstr(page, "value=\"2026-10-21T08:00\""));
	g_assert_nonnull(strstr(page, "value=\"2026-10-21T10:00\""));
}

/* A same-day call booked 8 to 10 is accepted; before, both ends were the
 * same midnight and it was refused for ending before it started. */
static void
test_same_day_booking(Fixture *f, gconstpointer data)
{
	g_autofree gchar *form = g_strdup("subject=Replace+water+heater&kind=meeting"
		"&starts-at=2026-10-21T08:00&ends-at=2026-10-21T10:00");
	g_autoptr(GPtrArray) activities = NULL;
	(void)data;
	g_assert_cmpuint(request(f, "POST", "/e/activity", form, NULL, NULL) / 100, ==, 3);
	activities = rows(f, venture_entity_registry_lookup(venture_entity_registry_get_default(), "activity"));
	g_assert_cmpuint(activities->len, ==, 1);
}

/* A DATE field keeps its day, even though its stored value is midnight UTC. */
static void
test_old_date_value(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) plan = g_object_new(VENTURE_TYPE_MAINTENANCE_PLAN,
		"name", "Annual service", "company-id", f->company, NULL);
	g_autoptr(GDateTime) midnight = g_date_time_new_utc(2026, 10, 21, 0, 0, 0);
	g_autofree gchar *edit = NULL, *page = NULL;
	(void)data;
	g_object_set(plan, "next-visit", midnight, NULL);
	venture_entity_set_organization_id(plan, f->org);
	save(f, plan);
	edit = g_strdup_printf("/e/maintenance_plan/%" G_GINT64_FORMAT "/edit", venture_entity_get_id(plan));
	g_assert_cmpuint(request(f, "GET", edit, NULL, NULL, &page), ==, 200);
	g_assert_nonnull(strstr(page, "type=\"date\" name=\"next-visit\" value=\"2026-10-21\""));
}

/* 8 p.m. is midnight UTC, still a timed booking: an untouched edit must
 * neither move it to tomorrow nor invert its arrival window. */
static void
test_utc_midnight_roundtrip(Fixture *f, gconstpointer data)
{
	g_autofree gchar *form = g_strdup_printf("title=Evening+repair&company-id=%" G_GINT64_FORMAT
		"&window-start=2026-10-21T20:00&window-end=2026-10-21T21:00", f->company);
	g_autoptr(GPtrArray) jobs = NULL;
	g_autoptr(VentureEntity) after = NULL;
	g_autoptr(GDateTime) before = NULL, persisted = NULL;
	g_autofree gchar *edit = NULL, *path = NULL, *page = NULL, *versioned = NULL;
	VentureEntity *job;
	(void)data;
	g_assert_cmpuint(request(f, "POST", "/e/job", form, NULL, NULL) / 100, ==, 3);
	jobs = rows(f, VENTURE_TYPE_JOB);
	job = g_ptr_array_index(jobs, 0);
	g_object_get(job, "window-start", &before, NULL);
	g_assert_cmpint(g_date_time_get_hour(before), ==, 0);
	path = g_strdup_printf("/e/job/%" G_GINT64_FORMAT, venture_entity_get_id(job));
	edit = g_strconcat(path, "/edit", NULL);
	g_assert_cmpuint(request(f, "GET", edit, NULL, NULL, &page), ==, 200);
	g_assert_nonnull(strstr(page, "value=\"2026-10-21T20:00\""));
	g_assert_nonnull(strstr(page, "value=\"2026-10-21T21:00\""));
	versioned = g_strdup_printf("%s&version=%" G_GINT64_FORMAT, form, venture_entity_get_version(job));
	g_assert_cmpuint(request(f, "POST", path, versioned, NULL, NULL) / 100, ==, 3);
	after = venture_database_get(f->db, VENTURE_TYPE_JOB, venture_entity_get_id(job), NULL);
	g_object_get(after, "window-start", &persisted, NULL);
	g_assert_cmpint(g_date_time_compare(before, persisted), ==, 0);
}

/* A technician's day: their jobs on that day, earliest first, with what
 * they need at the door; nobody else's, no other day's, no cancelled. */
static void
test_technician_day(Fixture *f, gconstpointer data)
{
	gint64 mike = make_user(f, "examplem");
	gint64 rosa = make_user(f, "exampler");
	g_autofree gchar *path = NULL, *page = NULL, *all = NULL;
	g_autoptr(VentureEntity) household = venture_database_get(f->db, VENTURE_TYPE_COMPANY, f->company, NULL);
	const gchar *early, *late;
	(void)data;
	g_object_set(household, "phone", "(555) 010-0177", NULL);
	save(f, household);
	booked(f, "Afternoon furnace", "2026-10-21T13:00", "2026-10-21T15:00", mike, VENTURE_JOB_BOOKED);
	booked(f, "Morning water heater", "2026-10-21T08:00", "2026-10-21T10:00", mike, VENTURE_JOB_BOOKED);
	booked(f, "Tomorrow drain", "2026-10-22T08:00", "2026-10-22T09:00", mike, VENTURE_JOB_BOOKED);
	booked(f, "Called off", "2026-10-21T11:00", "2026-10-21T12:00", mike, VENTURE_JOB_CANCELLED);
	booked(f, "Someone else's tap", "2026-10-21T09:00", "2026-10-21T10:00", rosa, VENTURE_JOB_BOOKED);
	path = g_strdup_printf("/jobs/day?date=2026-10-21&tech=%" G_GINT64_FORMAT, mike);
	g_assert_cmpuint(request(f, "GET", path, NULL, NULL, &page), ==, 200);
	early = strstr(page, "Morning water heater");
	late = strstr(page, "Afternoon furnace");
	g_assert_nonnull(early);
	g_assert_nonnull(late);
	g_assert_true(early < late);
	g_assert_nonnull(strstr(page, "08:00\xe2\x80\x93" "10:00"));
	g_assert_null(strstr(page, "Tomorrow drain"));
	g_assert_null(strstr(page, "Called off"));
	g_assert_null(strstr(page, "Someone else"));
	g_assert_nonnull(strstr(page, "href=\"tel:5550100177\""));
	g_assert_nonnull(strstr(page, "href=\"geo:0,0?q=12%20Example%20Way\""));
	g_assert_nonnull(strstr(page, "examplem"));
	{
		g_autofree gchar *picker = NULL;
		g_assert_cmpuint(request(f, "GET", "/e/job_technician/new", NULL, NULL, &picker), ==, 200);
		g_assert_nonnull(strstr(picker, ">examplem<"));
	}
	g_assert_cmpuint(request(f, "GET", "/jobs/day?date=2026-10-21&tech=all", NULL, NULL, &all), ==, 200);
	g_assert_nonnull(strstr(all, "Someone else"));
	g_assert_null(strstr(all, "Tomorrow drain"));
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/booking-times/datetime-boxes", Fixture, NULL, setup, test_datetime_boxes, teardown);
	g_test_add("/booking-times/window-is-wall-time", Fixture, NULL, setup, test_window_is_wall_time, teardown);
	g_test_add("/booking-times/same-day-booking", Fixture, NULL, setup, test_same_day_booking, teardown);
	g_test_add("/booking-times/old-date-value", Fixture, NULL, setup, test_old_date_value, teardown);
	g_test_add("/booking-times/technician-day", Fixture, NULL, setup, test_technician_day, teardown);
	g_test_add("/booking-times/utc-midnight-roundtrip", Fixture, NULL, setup, test_utc_midnight_roundtrip, teardown);
	return g_test_run();
}
