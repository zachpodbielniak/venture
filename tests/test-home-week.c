/*
 * test-home-week.c - A heating and plumbing owner's week, as a walk found it.
 *
 * An owner walked a whole week through the web UI: reminders that could
 * only be set up in JSON, technicians that could only be sent from a typed
 * address, a year box that turned 2014 into 20140, an accepted quote that
 * billed the customer before the work was booked, booked jobs on no
 * calendar, a maintenance plan that billed and never booked the visit, a
 * home page in finance jargon, a customer page that led nowhere, and a
 * setup checklist printed as JSON. What breaks if this regresses: one of
 * those steps is refused or unreachable again for an ordinary owner.
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
start(Fixture *f, const gchar *preset)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureCompany) company = NULL;
	f->config = venture_config_new();
	if (preset != NULL)
	{
		g_autofree gchar *path = g_build_filename(VENTURE_TEST_EXAMPLES, preset, NULL);
		g_assert_true(venture_config_apply_yaml_file(f->config, path, &error));
		g_assert_no_error(error);
	}
	f->db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->db);
	f->org = venture_context_get_default_organization_id(f->context);
	company = venture_company_new();
	g_object_set(company, "name", "Example Household", "address", "48 Example Street, Exampleton",
		"email", "household@example.test", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(company), f->org);
	save(f, VENTURE_ENTITY(company));
	f->company = venture_entity_get_id(VENTURE_ENTITY(company));
	f->dir = g_dir_make_tmp("venture-home-week-XXXXXX", NULL);
	g_object_set(f->config, "state-dir", f->dir, "server-bind-address", "127.0.0.1",
		"server-port", (gint64)0, "security-require-auth", FALSE, NULL);
	f->server = venture_web_server_new(f->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(f->server, &error));
	g_assert_no_error(error);
}

static void
setup(Fixture *f, gconstpointer data)
{
	start(f, data);
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

static gchar *
get(Fixture *f, const gchar *path)
{
	gchar *page = NULL;
	g_assert_cmpuint(request(f, "GET", path, NULL, NULL, &page), ==, 200);
	return page;
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

static gint64
technician(Fixture *f, const gchar *username, const gchar *display)
{
	g_autoptr(VentureUser) user = venture_user_new();
	g_object_set(user, "username", username, "display-name", display, "active", TRUE, NULL);
	/* As the Users page makes them: in the default business. */
	venture_entity_set_organization_id(VENTURE_ENTITY(user), f->org);
	save(f, VENTURE_ENTITY(user));
	return venture_entity_get_id(VENTURE_ENTITY(user));
}

static gint64
job_at(Fixture *f, const gchar *title, GDateTime *from)
{
	g_autoptr(VentureEntity) job = g_object_new(VENTURE_TYPE_JOB, "title", title, "company-id", f->company, NULL);
	venture_entity_set_organization_id(job, f->org);
	if (from != NULL)
		g_object_set(job, "window-start", from, NULL);
	save(f, job);
	return venture_entity_get_id(job);
}

static void
assign(Fixture *f, gint64 job, gint64 user)
{
	g_autoptr(VentureEntity) row = g_object_new(VENTURE_TYPE_JOB_TECHNICIAN, "job-id", job, "user-id", user, NULL);
	venture_entity_set_organization_id(row, f->org);
	save(f, row);
}

static gint
status_of(Fixture *f, GType type, gint64 id)
{
	g_autoptr(VentureEntity) record = venture_database_get(f->db, type, id, NULL);
	gint status = -1;
	g_assert_nonnull(record);
	g_object_get(record, "status", &status, NULL);
	return status;
}

/* Today at a time of day, in the business's zone. */
static GDateTime *
today_at(Fixture *f, gint hour)
{
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(GDateTime) local = g_date_time_to_timezone(now, venture_context_get_timezone(f->context));
	return g_date_time_new(venture_context_get_timezone(f->context), g_date_time_get_year(local),
		g_date_time_get_month(local), g_date_time_get_day_of_month(local), hour, 0, 0);
}

static gint64
quote_from_composer(Fixture *f, const gchar *extra)
{
	g_autofree gchar *form = g_strdup_printf("compose-form=1&company-id=%" G_GINT64_FORMAT
		"&line-0-description=Replace+water+heater&line-0-quantity=1&line-0-unit-price=1545+USD%s",
		f->company, extra != NULL ? extra : "");
	g_autoptr(GPtrArray) quotes = NULL;
	g_assert_cmpuint(request(f, "POST", "/quotes/compose", form, NULL, NULL) / 100, ==, 3);
	quotes = rows(f, VENTURE_TYPE_QUOTE);
	g_assert_cmpuint(quotes->len, >, 0);
	return venture_entity_get_id(g_ptr_array_index(quotes, quotes->len - 1));
}

static void
share_and_accept(Fixture *f, gint64 quote)
{
	g_autofree gchar *share = g_strdup_printf("/quotes/%" G_GINT64_FORMAT "/share", quote);
	g_autofree gchar *accept = g_strdup_printf("/quotes/%" G_GINT64_FORMAT "/accept", quote);
	g_assert_cmpuint(request(f, "POST", share, "", NULL, NULL) / 100, ==, 3);
	g_assert_cmpuint(request(f, "POST", accept, "accepted_by=Example+Customer", NULL, NULL) / 100, ==, 3);
}

static gint64
integer(Fixture *f, GType type, gint64 id, const gchar *field)
{
	g_autoptr(VentureEntity) record = venture_database_get(f->db, type, id, NULL);
	gint64 value = 0;
	g_assert_nonnull(record);
	g_object_get(record, field, &value, NULL);
	return value;
}

/* Payment reminders are in the menu, and one press sets up the usual
 * three, after which "Send a reminder now" sends one. Before that the
 * refusal says where to go. */
static void
test_reminders_ready(Fixture *f, gconstpointer data)
{
	g_autofree gchar *home = NULL, *list = NULL, *refused = NULL, *location = NULL;
	g_autofree gchar *remind = NULL, *steps = NULL;
	g_autoptr(GPtrArray) invoices = NULL, policies = NULL, messages = NULL;
	gint64 invoice;
	gboolean is_default = FALSE;
	(void)data;
	home = get(f, "/");
	g_assert_nonnull(strstr(home, "href=\"/e/dunning_policy\""));
	g_assert_nonnull(strstr(home, "Payment reminders"));

	{
		g_autofree gchar *form = g_strdup_printf("compose-form=1&company-id=%" G_GINT64_FORMAT
			"&line-0-description=Drain+cleared&line-0-quantity=1&line-0-unit-price=180+USD&send=1", f->company);
		g_assert_cmpuint(request(f, "POST", "/invoices/compose", form, NULL, NULL) / 100, ==, 3);
	}
	invoices = rows(f, VENTURE_TYPE_INVOICE);
	g_assert_cmpuint(invoices->len, ==, 1);
	invoice = venture_entity_get_id(g_ptr_array_index(invoices, 0));
	remind = g_strdup_printf("/api/v1/invoice/%" G_GINT64_FORMAT "/actions/remind", invoice);
	g_assert_cmpuint(request(f, "POST", remind, "", NULL, &refused), ==, 422);
	g_assert_nonnull(strstr(refused, "Payment reminders"));

	list = get(f, "/e/dunning_policy");
	g_assert_nonnull(strstr(list, "/actions/standard"));
	g_assert_cmpuint(request(f, "POST", "/api/v1/dunning_policy/0/actions/standard", "", &location, NULL) / 100, ==, 3);
	policies = rows(f, VENTURE_TYPE_DUNNING_POLICY);
	g_assert_cmpuint(policies->len, ==, 1);
	g_object_get(g_ptr_array_index(policies, 0), "is-default", &is_default, NULL);
	g_assert_true(is_default);
	g_object_get(g_ptr_array_index(policies, 0), "steps", &steps, NULL);
	g_assert_nonnull(steps);
	g_assert_nonnull(strstr(steps, "template_id"));
	/* A second press is refused rather than making a second default. */
	g_assert_cmpuint(request(f, "POST", "/api/v1/dunning_policy/0/actions/standard", "", NULL, NULL), ==, 422);

	messages = rows(f, VENTURE_TYPE_MAIL_MESSAGE);
	{
		guint before = messages->len;
		g_autoptr(GPtrArray) after = NULL;
		g_assert_cmpuint(request(f, "POST", remind, "", NULL, NULL) / 100, ==, 3);
		after = rows(f, VENTURE_TYPE_MAIL_MESSAGE);
		g_assert_cmpuint(after->len, ==, before + 1);
	}
}

/* A technician is sent from the job's own page, by name, and the job says
 * who is on it, how to get in and what equipment is there. */
static void
test_send_technician(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) property = g_object_new(VENTURE_TYPE_PROPERTY, "name", "Home", "company-id", f->company,
		"address", "48 Example Street", "access-notes", "Side gate code 0000. Dog in the yard.", NULL);
	g_autoptr(VentureEntity) heater = NULL;
	g_autoptr(GPtrArray) sent = NULL;
	g_autofree gchar *path = NULL, *page = NULL, *after = NULL, *assign_path = NULL, *form = NULL, *list = NULL;
	gint64 sam = technician(f, "sam@example.test", "Sam Example");
	gint64 job;
	gboolean lead = FALSE;
	(void)data;
	technician(f, "owner", NULL);
	venture_entity_set_organization_id(property, f->org);
	save(f, property);
	heater = g_object_new(VENTURE_TYPE_EQUIPMENT, "name", "Water heater", "property-id", venture_entity_get_id(property),
		"make", "Examplecraft", "serial", "SN-0001", NULL);
	venture_entity_set_organization_id(heater, f->org);
	save(f, heater);
	job = job_at(f, "No hot water", NULL);
	{
		g_autoptr(VentureEntity) record = venture_database_get(f->db, VENTURE_TYPE_JOB, job, NULL);
		g_object_set(record, "property-id", venture_entity_get_id(property), NULL);
		save(f, record);
	}
	path = g_strdup_printf("/e/job/%" G_GINT64_FORMAT, job);
	page = get(f, path);
	g_assert_nonnull(strstr(page, "Send a technician"));
	g_assert_nonnull(strstr(page, "/actions/assign"));
	g_assert_nonnull(strstr(page, "Sam Example"));
	/* The picker names people, not "User #2". */
	g_assert_null(strstr(page, "User #"));
	g_assert_nonnull(strstr(page, "Side gate code 0000"));
	g_assert_nonnull(strstr(page, "Water heater"));
	g_assert_nonnull(strstr(page, "SN-0001"));

	assign_path = g_strdup_printf("/api/v1/job/%" G_GINT64_FORMAT "/actions/assign", job);
	form = g_strdup_printf("user_id=%" G_GINT64_FORMAT "&lead=true", sam);
	g_assert_cmpuint(request(f, "POST", assign_path, form, NULL, NULL) / 100, ==, 3);
	sent = rows(f, VENTURE_TYPE_JOB_TECHNICIAN);
	g_assert_cmpuint(sent->len, ==, 1);
	g_object_get(g_ptr_array_index(sent, 0), "lead", &lead, NULL);
	g_assert_true(lead);
	after = get(f, path);
	g_assert_nonnull(strstr(after, "Sam Example (lead)"));
	list = get(f, "/e/job_technician");
	g_assert_nonnull(strstr(list, "Sam Example"));
	g_assert_null(strstr(list, "sam@example.test"));
}

/* A new piece of equipment's year box starts empty, so typing 2014 is
 * 2014 and not 20140. */
static void
test_equipment_year_blank(Fixture *f, gconstpointer data)
{
	g_autofree gchar *form = NULL;
	(void)data;
	form = get(f, "/e/equipment/new");
	g_assert_nonnull(strstr(form, "name=\"installed-year\" value=\"\""));
	g_assert_null(strstr(form, "name=\"installed-year\" value=\"0\""));
}

/* With jobs on, the composer bills after the work by default: accepting
 * makes a draft invoice, and the job's "Invoiced" issues it. */
static void
test_accept_bills_after_work(Fixture *f, gconstpointer data)
{
	g_autofree gchar *composer = NULL;
	g_autoptr(VentureEntity) job = NULL;
	g_autoptr(VentureEntity) issued = NULL;
	g_autoptr(GDateTime) due = NULL;
	gint64 quote, invoice, job_id;
	g_autofree gchar *complete = NULL, *invoiced = NULL;
	(void)data;
	composer = get(f, "/quotes/compose");
	g_assert_nonnull(strstr(composer, "name=\"billing-mode\""));
	g_assert_nonnull(strstr(composer, "value=\"after_work\" selected"));
	quote = quote_from_composer(f, "&billing-mode=after_work");
	share_and_accept(f, quote);
	g_assert_cmpint(status_of(f, VENTURE_TYPE_QUOTE, quote), ==, VENTURE_QUOTE_ACCEPTED);
	invoice = integer(f, VENTURE_TYPE_QUOTE, quote, "invoice-id");
	g_assert_cmpint(invoice, >, 0);
	g_assert_cmpint(status_of(f, VENTURE_TYPE_INVOICE, invoice), ==, VENTURE_INVOICE_STATUS_DRAFT);

	job = g_object_new(VENTURE_TYPE_JOB, "title", "Replace water heater", "company-id", f->company, "quote-id", quote, NULL);
	venture_entity_set_organization_id(job, f->org);
	save(f, job);
	job_id = venture_entity_get_id(job);
	complete = g_strdup_printf("/api/v1/job/%" G_GINT64_FORMAT "/actions/complete", job_id);
	invoiced = g_strdup_printf("/api/v1/job/%" G_GINT64_FORMAT "/actions/invoiced", job_id);
	g_assert_cmpuint(request(f, "POST", complete, "", NULL, NULL) / 100, ==, 3);
	g_assert_cmpuint(request(f, "POST", invoiced, "", NULL, NULL) / 100, ==, 3);
	g_assert_cmpint(status_of(f, VENTURE_TYPE_INVOICE, invoice), ==, VENTURE_INVOICE_STATUS_SENT);
	issued = venture_database_get(f->db, VENTURE_TYPE_INVOICE, invoice, NULL);
	g_object_get(issued, "due-at", &due, NULL);
	g_assert_nonnull(due);
	g_assert_cmpint(integer(f, VENTURE_TYPE_JOB, job_id, "invoice-id"), ==, invoice);

	/* Billing at acceptance is still there for whoever asks for it. */
	{
		gint64 upfront = quote_from_composer(f, "&billing-mode=full");
		share_and_accept(f, upfront);
		g_assert_cmpint(status_of(f, VENTURE_TYPE_INVOICE, integer(f, VENTURE_TYPE_QUOTE, upfront, "invoice-id")),
			==, VENTURE_INVOICE_STATUS_SENT);
	}
}

/* A booked job is on the calendar and on My day. */
static void
test_jobs_on_calendar(Fixture *f, gconstpointer data)
{
	g_autoptr(GDateTime) eight = today_at(f, 8);
	g_autofree gchar *day = g_date_time_format(eight, "%Y-%m-%d");
	g_autofree gchar *calendar_path = g_strdup_printf("/calendar?view=agenda&date=%s", day);
	g_autofree gchar *calendar = NULL, *mine = NULL, *link = NULL;
	gint64 job;
	(void)data;
	job = job_at(f, "Furnace not lighting", eight);
	link = g_strdup_printf("/e/job/%" G_GINT64_FORMAT, job);
	calendar = get(f, calendar_path);
	g_assert_nonnull(strstr(calendar, "Furnace not lighting"));
	g_assert_nonnull(strstr(calendar, link));
	mine = get(f, "/worklist");
	g_assert_nonnull(strstr(mine, "Furnace not lighting"));
	g_assert_nonnull(strstr(mine, "/jobs/day"));
}

/* A maintenance plan says whose visit is due and books it as a job, and
 * the next one moves on by the plan's interval. */
static void
test_maintenance_visits(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) property = g_object_new(VENTURE_TYPE_PROPERTY, "name", "Home", "company-id", f->company,
		"address", "48 Example Street", NULL);
	g_autoptr(VentureEntity) plan = NULL;
	g_autoptr(VentureEntity) moved = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(GDateTime) soon = g_date_time_add_days(now, 5);
	g_autoptr(GDateTime) next = NULL;
	g_autoptr(GPtrArray) jobs = NULL;
	g_autofree gchar *due = NULL, *book = NULL, *title = NULL, *after = NULL;
	GType plan_type = venture_entity_registry_lookup(venture_entity_registry_get_default(), "maintenance_plan");
	gint64 plan_id;
	(void)data;
	g_assert_true(plan_type != G_TYPE_INVALID);
	venture_entity_set_organization_id(property, f->org);
	save(f, property);
	plan = g_object_new(plan_type, "name", "Furnace care", "company-id", f->company,
		"property-id", venture_entity_get_id(property), "visit", "Yearly furnace tune-up",
		"every-months", (gint64)12, "next-visit", soon, NULL);
	venture_entity_set_organization_id(plan, f->org);
	save(f, plan);
	plan_id = venture_entity_get_id(plan);

	due = get(f, "/jobs/visits");
	g_assert_nonnull(strstr(due, "Yearly furnace tune-up"));
	g_assert_nonnull(strstr(due, "Example Household"));
	book = g_strdup_printf("/api/v1/maintenance_plan/%" G_GINT64_FORMAT "/actions/book", plan_id);
	g_assert_nonnull(strstr(due, book));
	g_assert_cmpuint(request(f, "POST", book, "", NULL, NULL) / 100, ==, 3);
	jobs = rows(f, VENTURE_TYPE_JOB);
	g_assert_cmpuint(jobs->len, ==, 1);
	g_object_get(g_ptr_array_index(jobs, 0), "title", &title, NULL);
	g_assert_cmpstr(title, ==, "Yearly furnace tune-up");
	g_assert_cmpint(integer(f, VENTURE_TYPE_JOB, venture_entity_get_id(g_ptr_array_index(jobs, 0)), "property-id"),
		==, venture_entity_get_id(property));
	g_assert_cmpint(integer(f, VENTURE_TYPE_JOB, venture_entity_get_id(g_ptr_array_index(jobs, 0)), "maintenance-plan-id"),
		==, plan_id);
	moved = venture_database_get(f->db, plan_type, plan_id, NULL);
	g_object_get(moved, "next-visit", &next, NULL);
	g_assert_nonnull(next);
	g_assert_cmpint(g_date_time_get_year(next), ==, g_date_time_get_year(soon) + 1);
	after = get(f, "/jobs/visits");
	g_assert_null(strstr(after, book));
}

/* The first screen says what is on today and who owes money, in words. */
static void
test_home_today(Fixture *f, gconstpointer data)
{
	g_autoptr(GDateTime) nine = today_at(f, 9);
	g_autofree gchar *home = NULL;
	(void)data;
	job_at(f, "Leaking kitchen tap", nine);
	home = get(f, "/");
	g_assert_nonnull(strstr(home, "class=\"card home-today\""));
	g_assert_nonnull(strstr(home, "Leaking kitchen tap"));
	g_assert_nonnull(strstr(home, "Quotes waiting"));
	g_assert_nonnull(strstr(home, "Overdue invoices"));
	g_assert_nonnull(strstr(home, ">This month<"));
	g_assert_null(strstr(home, ">this_month<"));
	g_assert_null(strstr(home, ">ytd<"));
}

/* The home-services preset leaves the menu to what such a business does. */
static void
test_menu_preset(Fixture *f, gconstpointer data)
{
	g_autofree gchar *home = NULL;
	(void)data;
	home = get(f, "/");
	g_assert_nonnull(strstr(home, "href=\"/e/job\""));
	g_assert_nonnull(strstr(home, "href=\"/e/invoice\""));
	g_assert_nonnull(strstr(home, "href=\"/e/dunning_policy\""));
	g_assert_null(strstr(home, "href=\"/arbitrage\""));
	g_assert_null(strstr(home, "href=\"/e/forge_repo\""));
	g_assert_null(strstr(home, "href=\"/e/recipe\""));
	g_assert_null(strstr(home, "href=\"/e/release\""));
	g_assert_null(strstr(home, "href=\"/e/idea\""));
}

/* A customer's page leads to their properties, jobs, quotes and invoices,
 * with a way to start each, before anything has been made. */
static void
test_customer_work(Fixture *f, gconstpointer data)
{
	g_autofree gchar *path = g_strdup_printf("/e/company/%" G_GINT64_FORMAT, f->company);
	g_autofree gchar *page = NULL, *new_property = NULL, *new_job = NULL;
	(void)data;
	page = get(f, path);
	new_property = g_strdup_printf("/e/property/new?company_id=%" G_GINT64_FORMAT, f->company);
	new_job = g_strdup_printf("/e/job/new?company_id=%" G_GINT64_FORMAT, f->company);
	g_assert_nonnull(strstr(page, "class=\"card customer-work\""));
	g_assert_nonnull(strstr(page, new_property));
	g_assert_nonnull(strstr(page, new_job));
	g_assert_nonnull(strstr(page, "Quotes"));
	g_assert_nonnull(strstr(page, "Invoices"));
}

/* The setup checklist is a list a person reads, not JSON. */
static void
test_setup_checklist(Fixture *f, gconstpointer data)
{
	g_autofree gchar *location = NULL, *page = NULL;
	const gchar *list, *raw, *folded;
	(void)data;
	g_assert_cmpuint(request(f, "GET", "/setup", NULL, &location, NULL) / 100, ==, 3);
	g_assert_nonnull(location);
	page = get(f, location);
	list = strstr(page, "<ul class=\"setup-checklist\">");
	g_assert_nonnull(list);
	g_assert_nonnull(strstr(list, "Legal entity"));
	/* The raw text only under the folded "All fields". */
	raw = strstr(page, "[{&quot;key&quot;");
	folded = strstr(page, "class=\"all-fields\"");
	g_assert_true(raw == NULL || (folded != NULL && raw > folded));
}

/* An accepted quote offers booking the job, not sending, accepting or
 * declining it again. */
static void
test_accepted_quote_buttons(Fixture *f, gconstpointer data)
{
	gint64 quote;
	g_autofree gchar *path = NULL, *draft = NULL, *accepted = NULL, *book = NULL;
	(void)data;
	quote = quote_from_composer(f, NULL);
	path = g_strdup_printf("/e/quote/%" G_GINT64_FORMAT, quote);
	draft = get(f, path);
	g_assert_nonnull(strstr(draft, "Send quote"));
	share_and_accept(f, quote);
	accepted = get(f, path);
	g_assert_null(strstr(accepted, "Send quote</button>"));
	g_assert_null(strstr(accepted, "/accept\">"));
	g_assert_null(strstr(accepted, "/decline\">"));
	book = g_strdup_printf("/e/job/new?company_id=%" G_GINT64_FORMAT "&amp;quote_id=%" G_GINT64_FORMAT, f->company, quote);
	g_assert_nonnull(strstr(accepted, book));
	g_assert_nonnull(strstr(accepted, "Book the job"));
}

/* The customer's quote page gives a plain date and, once accepted, says
 * what happens next. */
static void
test_customer_quote_words(Fixture *f, gconstpointer data)
{
	gint64 quote;
	g_autofree gchar *token = NULL, *public_path = NULL, *page = NULL, *thanks = NULL;
	g_autoptr(VentureEntity) record = NULL;
	(void)data;
	quote = quote_from_composer(f, NULL);
	{
		g_autofree gchar *share = g_strdup_printf("/quotes/%" G_GINT64_FORMAT "/share", quote);
		g_assert_cmpuint(request(f, "POST", share, "", NULL, NULL) / 100, ==, 3);
	}
	record = venture_database_get(f->db, VENTURE_TYPE_QUOTE, quote, NULL);
	g_object_get(record, "acceptance-token", &token, NULL);
	g_assert_nonnull(token);
	public_path = g_strdup_printf("/q/%s", token);
	page = get(f, public_path);
	g_assert_nonnull(strstr(page, "Valid until"));
	g_assert_null(strstr(page, " UTC"));
	g_assert_nonnull(strstr(page, ">Total<"));
	g_assert_cmpuint(request(f, "POST", public_path, "accepted_by=Example+Customer", NULL, &thanks), ==, 200);
	g_assert_nonnull(strstr(thanks, "Quote accepted"));
	g_assert_nonnull(strstr(thanks, "in touch to book"));
}

/* A new property starts with the customer's address, and the property
 * picker says whose each one is. */
static void
test_property_from_customer(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) property = g_object_new(VENTURE_TYPE_PROPERTY, "name", "Home", "company-id", f->company,
		"address", "48 Example Street", NULL);
	g_autofree gchar *path = g_strdup_printf("/e/property/new?company_id=%" G_GINT64_FORMAT, f->company);
	g_autofree gchar *form = NULL, *equipment = NULL;
	(void)data;
	form = get(f, path);
	g_assert_nonnull(strstr(form, "48 Example Street, Exampleton"));
	venture_entity_set_organization_id(property, f->org);
	save(f, property);
	equipment = get(f, "/e/equipment/new");
	g_assert_nonnull(strstr(equipment, "Home (Example Household)"));
}

/* Saving a new customer opens the customer. */
static void
test_save_opens_record(Fixture *f, gconstpointer data)
{
	g_autofree gchar *location = NULL;
	g_autoptr(GPtrArray) companies = NULL;
	g_autofree gchar *expected = NULL;
	(void)data;
	g_assert_cmpuint(request(f, "POST", "/e/company/new", "name=Example+Neighbour", &location, NULL) / 100, ==, 3);
	companies = rows(f, VENTURE_TYPE_COMPANY);
	expected = g_strdup_printf("/e/company/%" G_GINT64_FORMAT,
		venture_entity_get_id(g_ptr_array_index(companies, companies->len - 1)));
	g_assert_cmpstr(location, ==, expected);
}

/* The composer suggests the number the quote will get. */
static void
test_quote_number_hint(Fixture *f, gconstpointer data)
{
	g_autofree gchar *composer = NULL;
	g_autofree gchar *number = NULL;
	g_autoptr(VentureEntity) quote = NULL;
	gint64 id;
	(void)data;
	composer = get(f, "/quotes/compose");
	g_assert_null(strstr(composer, "Q-2026-001"));
	id = quote_from_composer(f, NULL);
	quote = venture_database_get(f->db, VENTURE_TYPE_QUOTE, id, NULL);
	g_object_get(quote, "number", &number, NULL);
	{
		g_autofree gchar *hint = g_strdup_printf("placeholder=\"%s\"", number);
		g_assert_nonnull(strstr(composer, hint));
	}
}

/* Jobs today and the calendar read the technicians a job carries. */
static void
test_tech_day_names(Fixture *f, gconstpointer data)
{
	g_autoptr(GDateTime) ten = today_at(f, 10);
	gint64 sam = technician(f, "sam@example.test", "Sam Example");
	gint64 job = job_at(f, "Boiler pressure low", ten);
	g_autofree gchar *path = NULL, *page = NULL;
	(void)data;
	assign(f, job, sam);
	path = g_strdup_printf("/e/job/%" G_GINT64_FORMAT, job);
	page = get(f, path);
	g_assert_nonnull(strstr(page, "Sam Example"));
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/home-week/reminders-ready", Fixture, NULL, setup, test_reminders_ready, teardown);
	g_test_add("/home-week/send-technician", Fixture, NULL, setup, test_send_technician, teardown);
	g_test_add("/home-week/equipment-year-blank", Fixture, NULL, setup, test_equipment_year_blank, teardown);
	g_test_add("/home-week/accept-bills-after-work", Fixture, NULL, setup, test_accept_bills_after_work, teardown);
	g_test_add("/home-week/jobs-on-calendar", Fixture, NULL, setup, test_jobs_on_calendar, teardown);
	g_test_add("/home-week/maintenance-visits", Fixture, NULL, setup, test_maintenance_visits, teardown);
	g_test_add("/home-week/home-today", Fixture, NULL, setup, test_home_today, teardown);
	g_test_add("/home-week/menu-preset", Fixture, "home-services.yaml", setup, test_menu_preset, teardown);
	g_test_add("/home-week/customer-work", Fixture, NULL, setup, test_customer_work, teardown);
	g_test_add("/home-week/setup-checklist", Fixture, NULL, setup, test_setup_checklist, teardown);
	g_test_add("/home-week/accepted-quote-buttons", Fixture, NULL, setup, test_accepted_quote_buttons, teardown);
	g_test_add("/home-week/customer-quote-words", Fixture, NULL, setup, test_customer_quote_words, teardown);
	g_test_add("/home-week/property-from-customer", Fixture, NULL, setup, test_property_from_customer, teardown);
	g_test_add("/home-week/save-opens-record", Fixture, NULL, setup, test_save_opens_record, teardown);
	g_test_add("/home-week/quote-number-hint", Fixture, NULL, setup, test_quote_number_hint, teardown);
	g_test_add("/home-week/tech-day-names", Fixture, NULL, setup, test_tech_day_names, teardown);
	return g_test_run();
}
