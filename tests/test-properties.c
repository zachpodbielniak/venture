/*
 * test-properties.c - A customer's properties and the equipment at each.
 *
 * A landlord's rentals, the tenant's gate code and the 2009 water heater in
 * the basement all went into one free-text address and the notes, because
 * a customer had nowhere else to keep them. What breaks if this regresses:
 * a property without its address, equipment filed nowhere or installed in
 * a year that never happened, a job sent to another customer's property,
 * or a technician arriving with no address because the job only named the
 * property.
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
	f->dir = g_dir_make_tmp("venture-properties-XXXXXX", NULL);
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

/* A draft quote with one line, made the way the sheet makes it. */
static gboolean
try_save(Fixture *f, VentureEntity *record, GError **error)
{
	return venture_database_save(f->db, record, NULL, error);
}

static VentureEntity *
new_property(Fixture *f, const gchar *name, gint64 company, const gchar *address)
{
	VentureEntity *property = g_object_new(VENTURE_TYPE_PROPERTY, "name", name, "company-id", company,
		"address", address, NULL);
	venture_entity_set_organization_id(property, f->org);
	return property;
}

static VentureEntity *
new_equipment(Fixture *f, const gchar *name, gint64 property, gint64 year)
{
	VentureEntity *equipment = g_object_new(VENTURE_TYPE_EQUIPMENT, "name", name, "property-id", property,
		"installed-year", year, NULL);
	venture_entity_set_organization_id(equipment, f->org);
	return equipment;
}

/* A property is a customer's, and has an address. */
static void
test_property_needs_customer_and_address(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) rental = new_property(f, "14 Example St rental", f->company, "14 Example St\nSpringfield");
	g_autoptr(VentureEntity) nowhere = new_property(f, "Somewhere", f->company, "   ");
	g_autoptr(VentureEntity) nobody = new_property(f, "Orphan", 0, "1 Example Rd");
	g_autoptr(GError) error = NULL;
	(void)data;
	g_object_set(rental, "access-notes", "Gate code 4412. Dog in the yard.", NULL);
	save(f, rental);
	g_assert_false(try_save(f, nowhere, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	g_assert_false(try_save(f, nobody, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
}

/* Equipment is at a property, went in a year that could have happened,
 * and its serial number finds it. */
static void
test_equipment(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) home = new_property(f, "Home", f->company, "88 Example Ct");
	g_autoptr(VentureEntity) heater = NULL, ancient = NULL, future = NULL, floating = NULL, unknown = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *found = NULL;
	gint64 property;
	(void)data;
	save(f, home);
	property = venture_entity_get_id(home);
	heater = new_equipment(f, "Water heater", property, 2009);
	g_object_set(heater, "make", "Example Make", "model", "EX-40G", "serial", "SN-2009-4417", NULL);
	save(f, heater);
	unknown = new_equipment(f, "Furnace", property, 0);
	save(f, unknown);
	ancient = new_equipment(f, "Boiler", property, 1850);
	g_assert_false(try_save(f, ancient, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	future = new_equipment(f, "Heat pump", property, 2999);
	g_assert_false(try_save(f, future, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	floating = new_equipment(f, "Softener", 0, 2015);
	g_assert_false(try_save(f, floating, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_cmpuint(request(f, "GET", "/search?q=SN-2009-4417", NULL, NULL, &found), ==, 200);
	g_assert_nonnull(strstr(found, "Water heater"));
}

/* A job at a property is the property's customer's, and is sent to its
 * address unless it was given one of its own. */
static void
test_job_at_property(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) rental = new_property(f, "Rental", f->company, "14 Example St");
	g_autoptr(VentureCompany) neighbour = venture_company_new();
	g_autoptr(VentureEntity) theirs = NULL, sent = NULL, typed = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *address = NULL, *kept = NULL;
	(void)data;
	save(f, rental);
	g_object_set(neighbour, "name", "Example Neighbour", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(neighbour), f->org);
	save(f, VENTURE_ENTITY(neighbour));
	theirs = g_object_new(VENTURE_TYPE_JOB, "title", "Drain", "company-id",
		venture_entity_get_id(VENTURE_ENTITY(neighbour)), "property-id", venture_entity_get_id(rental), NULL);
	venture_entity_set_organization_id(theirs, f->org);
	g_assert_false(try_save(f, theirs, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	sent = g_object_new(VENTURE_TYPE_JOB, "title", "No hot water", "company-id", f->company,
		"property-id", venture_entity_get_id(rental), NULL);
	venture_entity_set_organization_id(sent, f->org);
	save(f, sent);
	g_object_get(sent, "service-address", &address, NULL);
	g_assert_cmpstr(address, ==, "14 Example St");
	typed = g_object_new(VENTURE_TYPE_JOB, "title", "Outdoor tap", "company-id", f->company,
		"property-id", venture_entity_get_id(rental), "service-address", "Side of the garage", NULL);
	venture_entity_set_organization_id(typed, f->org);
	save(f, typed);
	g_object_get(typed, "service-address", &kept, NULL);
	g_assert_cmpstr(kept, ==, "Side of the garage");
}

/* Properties and equipment are in the sidebar, the form asks how to get
 * in, and a property's page lists what is there. */
static void
test_properties_reachable(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) home = new_property(f, "Home", f->company, "88 Example Ct");
	g_autoptr(VentureEntity) heater = NULL;
	g_autofree gchar *sidebar = NULL, *form = NULL, *page = NULL, *path = NULL;
	(void)data;
	save(f, home);
	heater = new_equipment(f, "Water heater", venture_entity_get_id(home), 2009);
	save(f, heater);
	g_assert_cmpuint(request(f, "GET", "/", NULL, NULL, &sidebar), ==, 200);
	g_assert_nonnull(strstr(sidebar, "href=\"/e/property\""));
	g_assert_nonnull(strstr(sidebar, "href=\"/e/equipment\""));
	g_assert_cmpuint(request(f, "GET", "/e/property/new", NULL, NULL, &form), ==, 200);
	g_assert_nonnull(strstr(form, "name=\"access-notes\""));
	path = g_strdup_printf("/e/property/%" G_GINT64_FORMAT, venture_entity_get_id(home));
	g_assert_cmpuint(request(f, "GET", path, NULL, NULL, &page), ==, 200);
	g_assert_nonnull(strstr(page, "Water heater"));
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/properties/needs-customer-and-address", Fixture, NULL, setup, test_property_needs_customer_and_address, teardown);
	g_test_add("/properties/equipment", Fixture, NULL, setup, test_equipment, teardown);
	g_test_add("/properties/job-at-property", Fixture, NULL, setup, test_job_at_property, teardown);
	g_test_add("/properties/reachable", Fixture, NULL, setup, test_properties_reachable, teardown);
	return g_test_run();
}
