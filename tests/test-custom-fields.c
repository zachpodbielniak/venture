/*
 * test-custom-fields.c - Accounting custom fields, layouts and required values.
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
	gint64 org;
} Fixture;

static void
actor_init(VentureActor *actor)
{
	actor->kind = VENTURE_ACTOR_KIND_USER;
	actor->name = "fields";
	actor->prompt = NULL;
	actor->request_id = NULL;
	actor->approved_by = NULL;
}

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
	(void)data;
	f->config = venture_config_new();
	f->db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->db);
	f->org = venture_context_get_default_organization_id(f->context);
}

static void
teardown(Fixture *f, gconstpointer data)
{
	(void)data;
	g_clear_object(&f->context);
	g_clear_object(&f->db);
	g_clear_object(&f->config);
}

typedef struct
{
	gboolean done;
	GBytes *bytes;
	GError *error;
} HttpResult;

static void
http_done(GObject *source, GAsyncResult *result, gpointer data)
{
	HttpResult *response = data;
	response->bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &response->error);
	response->done = TRUE;
}

static guint
http_request(VentureWebServer *server, const gchar *method, const gchar *path,
	const gchar *content_type, const gchar *body, gchar **out)
{
	g_autoptr(SoupSession) session = soup_session_new_with_options("timeout", 15, NULL);
	g_autoptr(SoupMessage) message = NULL;
	g_autofree gchar *url = g_strconcat(venture_web_server_get_base_url(server), path, NULL);
	HttpResult response;
	memset(&response, 0, sizeof(response));
	message = soup_message_new(method, url);
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	if (body != NULL)
	{
		g_autoptr(GBytes) payload = g_bytes_new(body, strlen(body));
		soup_message_set_request_body_from_bytes(message,
			content_type ? content_type : "application/json", payload);
	}
	soup_session_send_and_read_async(session, message, G_PRIORITY_DEFAULT, NULL, http_done, &response);
	while (!response.done)
		g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(response.error);
	if (out && response.bytes)
		*out = g_strndup(g_bytes_get_data(response.bytes, NULL), g_bytes_get_size(response.bytes));
	g_clear_pointer(&response.bytes, g_bytes_unref);
	return soup_message_get_status(message);
}

static void
test_required_value_and_layout(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) field = NULL;
	g_autoptr(VentureEntity) layout = NULL;
	g_autoptr(VentureCompany) company = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) values = NULL;
	g_autofree gchar *order = NULL;
	VentureActor actor;
	(void)data;
	actor_init(&actor);
	company = venture_company_new();
	g_object_set(company, "name", "With extras", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(company), f->org);
	save(f, VENTURE_ENTITY(company));
	field = venture_custom_fields_service_define(venture_custom_fields_service_get(f->db),
		f->org, "company", "po_number", "string", TRUE, NULL, &actor, &error);
	g_assert_no_error(error);
	g_assert_nonnull(field);
	g_assert_false(venture_database_save(f->db, VENTURE_ENTITY(company), &actor, &error));
	g_assert_nonnull(error);
	g_assert_nonnull(strstr(error->message, "po_number"));
	g_clear_error(&error);
	g_assert_true(venture_custom_fields_service_put_value(venture_custom_fields_service_get(f->db),
		f->org, "company", venture_entity_get_id(VENTURE_ENTITY(company)),
		"po_number", "PO-42", &actor, &error));
	g_assert_no_error(error);
	g_object_set(company, "name", "With extras named", NULL);
	save(f, VENTURE_ENTITY(company));
	query = venture_query_new(VENTURE_TYPE_CUSTOM_FIELD_VALUE);
	venture_query_set_organization(query, f->org);
	venture_query_add_filter_string(query, "record-type", VENTURE_FILTER_OP_EQ, "company", NULL);
	venture_query_add_filter_int(query, "record-id", VENTURE_FILTER_OP_EQ,
		venture_entity_get_id(VENTURE_ENTITY(company)), NULL);
	values = venture_database_find(f->db, query, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(values->len, ==, 1);
	layout = venture_custom_fields_service_set_layout(venture_custom_fields_service_get(f->db),
		f->org, "company", "[\"name\",\"po_number\"]", &actor, &error);
	g_assert_no_error(error);
	g_object_get(layout, "field-order", &order, NULL);
	g_assert_nonnull(strstr(order, "po_number"));
}

static void
test_attribute_satisfies_required(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) field = NULL;
	g_autoptr(VentureCompany) company = NULL;
	VentureActor actor;
	(void)data;
	actor_init(&actor);
	field = venture_custom_fields_service_define(venture_custom_fields_service_get(f->db),
		f->org, "company", "cost_center", "string", TRUE, NULL, &actor, &error);
	g_assert_no_error(error);
	g_assert_nonnull(field);
	company = venture_company_new();
	g_object_set(company, "name", "Needs center", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(company), f->org);
	g_assert_false(venture_database_save(f->db, VENTURE_ENTITY(company), &actor, &error));
	g_clear_error(&error);
	venture_entity_set_attribute(VENTURE_ENTITY(company), "cost_center", "OPS");
	save(f, VENTURE_ENTITY(company));
}

static void
test_settings_fields_page(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureWebServer) server = NULL;
	g_autofree gchar *dir = NULL;
	g_autofree gchar *body = NULL;
	g_autoptr(GSocketListener) listener = g_socket_listener_new();
	guint16 port;
	VentureActor actor;
	(void)data;
	actor_init(&actor);
	g_assert_nonnull(venture_custom_fields_service_define(venture_custom_fields_service_get(f->db),
		f->org, "vendor_bill", "job_code", "string", FALSE, NULL, &actor, &error));
	dir = g_dir_make_tmp("venture-fields-XXXXXX", &error);
	g_assert_no_error(error);
	port = g_socket_listener_add_any_inet_port(listener, NULL, &error);
	g_socket_listener_close(listener);
	g_object_set(f->config, "state-dir", dir, "server-bind-address", "127.0.0.1",
		"server-port", (gint64)port, "security-require-auth", FALSE, NULL);
	server = venture_web_server_new(f->context, &error);
	g_assert_true(venture_web_server_start(server, &error));
	g_assert_cmpuint(http_request(server, "GET", "/settings/fields", NULL, NULL, &body), ==, 200);
	g_assert_nonnull(strstr(body, "job_code"));
	g_assert_cmpuint(http_request(server, "POST", "/settings/fields",
		"application/x-www-form-urlencoded",
		"action=field&record_type=vendor_bill&name=site&kind=string&required=0", NULL), ==, 302);
	venture_test_remove_tree(dir);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/custom-fields/required-and-layout", Fixture, NULL, setup,
		test_required_value_and_layout, teardown);
	g_test_add("/custom-fields/attribute-satisfies-required", Fixture, NULL, setup,
		test_attribute_satisfies_required, teardown);
	g_test_add("/custom-fields/settings-page", Fixture, NULL, setup,
		test_settings_fields_page, teardown);
	return g_test_run();
}
