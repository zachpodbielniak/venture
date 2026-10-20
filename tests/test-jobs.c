/*
 * test-jobs.c - A job: work at a customer's address, from booked to billed.
 *
 * A home-services business booked a no-hot-water call as a calendar
 * "meeting", because there was nothing else: no record of the problem, the
 * technicians sent, the window promised, where the job had got to, or the
 * estimate and invoice it belonged with. What breaks if this regresses: a
 * job without its customer or with a window that ends before it starts, a
 * technician who can't sign in sent out, an "invoiced" job with no invoice,
 * or a button that moves a finished job back onto the road.
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <venture.h>
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
	f->dir = g_dir_make_tmp("venture-jobs-XXXXXX", NULL);
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

static gchar *
compose_form(Fixture *f, gboolean send)
{
	return g_strdup_printf("compose-form=1&company-id=%" G_GINT64_FORMAT
		"&line-0-description=Replace+water+heater&line-0-quantity=1&line-0-unit-price=1450+USD%s",
		f->company, send ? "&send=1" : "");
}

/* A draft quote with one line, made the way the sheet makes it. */
static gint64
draft_quote(Fixture *f)
{
	g_autofree gchar *form = compose_form(f, FALSE);
	g_autoptr(GPtrArray) quotes = NULL;
	g_assert_cmpuint(request(f, "POST", "/quotes/compose", form, NULL, NULL) / 100, ==, 3);
	quotes = rows(f, VENTURE_TYPE_QUOTE);
	g_assert_cmpuint(quotes->len, ==, 1);
	return venture_entity_get_id(g_ptr_array_index(quotes, 0));
}


static VentureEntity *
new_job(Fixture *f, const gchar *title, gint64 company)
{
	VentureEntity *job = g_object_new(VENTURE_TYPE_JOB, "title", title, "company-id", company, NULL);
	venture_entity_set_organization_id(job, f->org);
	return job;
}

static gboolean
try_save(Fixture *f, VentureEntity *record, GError **error)
{
	return venture_database_save(f->db, record, NULL, error);
}

static gint64
other_customer(Fixture *f)
{
	g_autoptr(VentureCompany) company = venture_company_new();
	g_object_set(company, "name", "Example Neighbour", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(company), f->org);
	save(f, VENTURE_ENTITY(company));
	return venture_entity_get_id(VENTURE_ENTITY(company));
}

static gint
job_status(Fixture *f, gint64 id)
{
	g_autoptr(VentureEntity) job = venture_database_get(f->db, VENTURE_TYPE_JOB, id, NULL);
	gint status = -1;
	g_assert_nonnull(job);
	g_object_get(job, "status", &status, NULL);
	return status;
}

static guint
press(Fixture *f, gint64 id, const gchar *button, const gchar *form, gchar **out)
{
	g_autofree gchar *path = g_strdup_printf("/api/v1/job/%" G_GINT64_FORMAT "/actions/%s", id, button);
	return request(f, "POST", path, form != NULL ? form : "", NULL, out);
}

/* A job saved without a number gets the next J- number; it must be for a
 * customer of this business. */
static void
test_number_and_customer(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) first = new_job(f, "No hot water", f->company);
	g_autoptr(VentureEntity) second = new_job(f, "Furnace tune-up", f->company);
	g_autoptr(VentureEntity) nobody = new_job(f, "Leaking tap", 0);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *one = NULL, *two = NULL;
	gint status = -1;
	(void)data;
	save(f, first);
	save(f, second);
	g_object_get(first, "number", &one, "status", &status, NULL);
	g_object_get(second, "number", &two, NULL);
	g_assert_cmpstr(one, ==, "J-1");
	g_assert_cmpstr(two, ==, "J-2");
	g_assert_cmpint(status, ==, VENTURE_JOB_BOOKED);
	g_assert_false(try_save(f, nobody, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
}

/* The arrival window promised to the customer ends after it starts. */
static void
test_arrival_window(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) backwards = new_job(f, "No hot water", f->company);
	g_autoptr(VentureEntity) endless = new_job(f, "No hot water", f->company);
	g_autoptr(VentureEntity) good = new_job(f, "No hot water", f->company);
	g_autoptr(GDateTime) eight = g_date_time_new_utc(2026, 10, 21, 13, 0, 0);
	g_autoptr(GDateTime) ten = g_date_time_new_utc(2026, 10, 21, 15, 0, 0);
	g_autoptr(GError) error = NULL;
	(void)data;
	g_object_set(backwards, "window-start", ten, "window-end", eight, NULL);
	g_assert_false(try_save(f, backwards, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	g_object_set(endless, "window-end", ten, NULL);
	g_assert_false(try_save(f, endless, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_object_set(good, "window-start", eight, "window-end", ten, NULL);
	save(f, good);
}

/* The estimate and invoice a job names are its own customer's. */
static void
test_estimate_same_customer(Fixture *f, gconstpointer data)
{
	gint64 quote = draft_quote(f);
	gint64 neighbour = other_customer(f);
	g_autoptr(VentureEntity) theirs = new_job(f, "Drain cleaning", neighbour);
	g_autoptr(VentureEntity) ours = new_job(f, "Replace water heater", f->company);
	g_autoptr(GError) error = NULL;
	(void)data;
	g_object_set(theirs, "quote-id", quote, NULL);
	g_assert_false(try_save(f, theirs, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_object_set(ours, "quote-id", quote, NULL);
	save(f, ours);
}

/* The day's buttons: each applies where it should, stamps the time, and a
 * finished job can't be sent back onto the road. The page offers only the
 * buttons that apply. */
static void
test_day_buttons(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) job = new_job(f, "No hot water", f->company);
	g_autoptr(VentureEntity) after = NULL;
	g_autoptr(GDateTime) left = NULL, arrived = NULL, completed = NULL;
	g_autofree gchar *refused = NULL, *booked_page = NULL, *done_page = NULL, *record = NULL;
	gint64 id;
	(void)data;
	save(f, job);
	id = venture_entity_get_id(job);
	record = g_strdup_printf("/e/job/%" G_GINT64_FORMAT, id);
	g_assert_cmpuint(request(f, "GET", record, NULL, NULL, &booked_page), ==, 200);
	g_assert_nonnull(strstr(booked_page, "On my way"));
	g_assert_null(strstr(booked_page, "/actions/invoiced"));
	g_assert_cmpuint(press(f, id, "on_the_way", NULL, NULL) / 100, ==, 3);
	g_assert_cmpint(job_status(f, id), ==, VENTURE_JOB_ON_THE_WAY);
	g_assert_cmpuint(press(f, id, "arrive", NULL, NULL) / 100, ==, 3);
	g_assert_cmpint(job_status(f, id), ==, VENTURE_JOB_ON_SITE);
	g_assert_cmpuint(press(f, id, "complete", NULL, NULL) / 100, ==, 3);
	g_assert_cmpint(job_status(f, id), ==, VENTURE_JOB_DONE);
	after = venture_database_get(f->db, VENTURE_TYPE_JOB, id, NULL);
	g_object_get(after, "left-at", &left, "arrived-at", &arrived, "completed-at", &completed, NULL);
	g_assert_nonnull(left);
	g_assert_nonnull(arrived);
	g_assert_nonnull(completed);
	g_assert_cmpuint(press(f, id, "on_the_way", NULL, &refused), ==, 422);
	g_assert_nonnull(strstr(refused, "done"));
	g_assert_cmpint(job_status(f, id), ==, VENTURE_JOB_DONE);
	g_assert_cmpuint(request(f, "GET", record, NULL, NULL, &done_page), ==, 200);
	g_assert_nonnull(strstr(done_page, "/actions/invoiced"));
	g_assert_null(strstr(done_page, "/actions/on_the_way"));
}

/* "Invoiced" takes the invoice its accepted estimate became, and refuses
 * when there is no invoice anywhere. */
static void
test_invoiced_takes_estimate_invoice(Fixture *f, gconstpointer data)
{
	gint64 quote = draft_quote(f);
	g_autofree gchar *share = g_strdup_printf("/quotes/%" G_GINT64_FORMAT "/share", quote);
	g_autofree gchar *accept = g_strdup_printf("/quotes/%" G_GINT64_FORMAT "/accept", quote);
	g_autoptr(VentureEntity) bare = new_job(f, "Leaking tap", f->company);
	g_autoptr(VentureEntity) quoted = new_job(f, "Replace water heater", f->company);
	g_autoptr(VentureEntity) billed = NULL;
	g_autoptr(VentureEntity) accepted = NULL;
	gint64 invoice = 0, linked = 0;
	(void)data;
	save(f, bare);
	g_assert_cmpuint(press(f, venture_entity_get_id(bare), "complete", NULL, NULL) / 100, ==, 3);
	g_assert_cmpuint(press(f, venture_entity_get_id(bare), "invoiced", NULL, NULL), ==, 422);
	g_assert_cmpint(job_status(f, venture_entity_get_id(bare)), ==, VENTURE_JOB_DONE);
	g_assert_cmpuint(request(f, "POST", share, "", NULL, NULL) / 100, ==, 3);
	g_assert_cmpuint(request(f, "POST", accept, "accepted_by=Example+Customer", NULL, NULL) / 100, ==, 3);
	accepted = venture_database_get(f->db, VENTURE_TYPE_QUOTE, quote, NULL);
	g_object_get(accepted, "invoice-id", &invoice, NULL);
	g_assert_cmpint(invoice, >, 0);
	g_object_set(quoted, "quote-id", quote, NULL);
	save(f, quoted);
	g_assert_cmpuint(press(f, venture_entity_get_id(quoted), "complete", NULL, NULL) / 100, ==, 3);
	g_assert_cmpuint(press(f, venture_entity_get_id(quoted), "invoiced", NULL, NULL) / 100, ==, 3);
	billed = venture_database_get(f->db, VENTURE_TYPE_JOB, venture_entity_get_id(quoted), NULL);
	g_object_get(billed, "invoice-id", &linked, NULL);
	g_assert_cmpint(linked, ==, invoice);
	g_assert_cmpint(job_status(f, venture_entity_get_id(quoted)), ==, VENTURE_JOB_INVOICED);
}

/* Technicians are people who sign in, each once per job. */
static void
test_technicians(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) job = new_job(f, "No hot water", f->company);
	g_autoptr(VentureUser) tech = venture_user_new();
	g_autoptr(VentureUser) gone = venture_user_new();
	g_autoptr(VentureEntity) first = NULL, again = NULL, inactive = NULL;
	g_autoptr(GError) error = NULL;
	(void)data;
	save(f, job);
	g_object_set(tech, "username", "examplet", "active", TRUE, NULL);
	save(f, VENTURE_ENTITY(tech));
	g_object_set(gone, "username", "examplef", "active", FALSE, NULL);
	save(f, VENTURE_ENTITY(gone));
	first = g_object_new(VENTURE_TYPE_JOB_TECHNICIAN, "job-id", venture_entity_get_id(job),
		"user-id", venture_entity_get_id(VENTURE_ENTITY(tech)), "lead", TRUE, NULL);
	venture_entity_set_organization_id(first, f->org);
	save(f, first);
	again = g_object_new(VENTURE_TYPE_JOB_TECHNICIAN, "job-id", venture_entity_get_id(job),
		"user-id", venture_entity_get_id(VENTURE_ENTITY(tech)), NULL);
	venture_entity_set_organization_id(again, f->org);
	g_assert_false(try_save(f, again, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	inactive = g_object_new(VENTURE_TYPE_JOB_TECHNICIAN, "job-id", venture_entity_get_id(job),
		"user-id", venture_entity_get_id(VENTURE_ENTITY(gone)), NULL);
	venture_entity_set_organization_id(inactive, f->org);
	g_assert_false(try_save(f, inactive, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
}

/* Jobs are in the sidebar under Customers, and the form asks for the
 * things a job needs. */
static void
test_jobs_reachable(Fixture *f, gconstpointer data)
{
	g_autofree gchar *home = NULL, *form = NULL;
	(void)data;
	g_assert_cmpuint(request(f, "GET", "/", NULL, NULL, &home), ==, 200);
	g_assert_nonnull(strstr(home, "href=\"/e/job\""));
	g_assert_cmpuint(request(f, "GET", "/e/job/new", NULL, NULL, &form), ==, 200);
	g_assert_nonnull(strstr(form, "name=\"company-id\""));
	g_assert_nonnull(strstr(form, "name=\"window-start\""));
	g_assert_nonnull(strstr(form, "name=\"problem\""));
	g_assert_nonnull(strstr(form, "name=\"service-address\""));
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/jobs/number-and-customer", Fixture, NULL, setup, test_number_and_customer, teardown);
	g_test_add("/jobs/arrival-window", Fixture, NULL, setup, test_arrival_window, teardown);
	g_test_add("/jobs/estimate-same-customer", Fixture, NULL, setup, test_estimate_same_customer, teardown);
	g_test_add("/jobs/day-buttons", Fixture, NULL, setup, test_day_buttons, teardown);
	g_test_add("/jobs/invoiced-takes-estimate-invoice", Fixture, NULL, setup, test_invoiced_takes_estimate_invoice, teardown);
	g_test_add("/jobs/technicians", Fixture, NULL, setup, test_technicians, teardown);
	g_test_add("/jobs/reachable", Fixture, NULL, setup, test_jobs_reachable, teardown);
	return g_test_run();
}
