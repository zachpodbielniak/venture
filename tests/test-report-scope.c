/*
 * test-report-scope.c - Reports answer for the organization picked in the
 * sidebar.
 *
 * A report names its organization with ?organization_id=; without one it
 * used the installation's default organization even when the viewer had
 * another picked, so the Profit and loss page of a second business showed
 * the first business's sales under the second one's name.
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <venture.h>
#include <libsoup/soup.h>
#include <string.h>
#include "venture-test-util.h"

typedef struct
{
	VentureDatabase *database;
	VentureConfig *config;
	VentureContext *context;
	VentureWebServer *server;
	SoupSession *session;
	gchar *state_dir;
	gchar *url;
	gint64 first;
	gint64 second;
} Fixture;

static void
sale(Fixture *f, gint64 org, const gchar *gross)
{
	g_autoptr(VentureEntity) record = venture_entity_registry_create(venture_entity_registry_get_default(), "sale", NULL);
	g_autoptr(GError) error = NULL;
	venture_entity_set_organization_id(record, org);
	g_assert_true(venture_entity_set_field_from_string(record, "gross", gross, &error));
	g_assert_true(venture_entity_set_field_from_string(record, "occurred-at", "2025-03-04", &error));
	g_object_set(record, "quantity", (gint64)1, "channel", "direct", NULL);
	g_assert_true(venture_database_save(f->database, record, NULL, &error));
	g_assert_no_error(error);
}

static void
set_up(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) second = NULL;
	(void)data;
	f->state_dir = g_dir_make_tmp("venture-report-scope-XXXXXX", NULL);
	f->config = venture_config_new();
	g_object_set(f->config, "state-dir", f->state_dir, "server-bind-address", "127.0.0.1",
		"server-port", (gint64)0, "security-require-auth", FALSE, NULL);
	f->database = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->database);
	g_assert_true(venture_database_migrate(f->database, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->first = venture_context_get_default_organization_id(f->context);
	second = venture_entity_registry_create(venture_entity_registry_get_default(), "organization", NULL);
	g_object_set(second, "name", "Second Business", NULL);
	g_assert_true(venture_database_save(f->database, second, NULL, &error));
	g_assert_no_error(error);
	f->second = venture_entity_get_id(second);
	sale(f, f->first, "100.00 USD");
	sale(f, f->second, "7.00 USD");
	f->server = venture_web_server_new(f->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(f->server, &error));
	g_assert_no_error(error);
	f->url = g_strdup_printf("http://127.0.0.1:%u", venture_web_server_get_port(f->server));
	f->session = soup_session_new();
}

static void
tear_down(Fixture *f, gconstpointer data)
{
	(void)data;
	venture_web_server_stop(f->server);
	g_clear_object(&f->session);
	g_clear_object(&f->server);
	g_clear_object(&f->context);
	g_clear_object(&f->database);
	g_clear_object(&f->config);
	venture_test_remove_tree(f->state_dir);
	g_free(f->state_dir);
	g_free(f->url);
}

typedef struct
{
	gboolean done;
	GBytes *body;
	GError *error;
} Request;

static void
request_done(GObject *source, GAsyncResult *result, gpointer data)
{
	Request *request = data;
	request->body = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &request->error);
	request->done = TRUE;
}

/* GETs @path with @entity as the sidebar pick (NULL for none made). */
static gchar *
get(Fixture *f, const gchar *path, const gchar *entity)
{
	g_autofree gchar *url = g_strconcat(f->url, path, NULL);
	g_autoptr(SoupMessage) message = soup_message_new("GET", url);
	Request result = { FALSE, NULL, NULL };
	gchar *body;
	if (entity != NULL)
	{
		g_autofree gchar *cookie = g_strdup_printf("venture_entity=%s", entity);
		soup_message_headers_append(soup_message_get_request_headers(message), "Cookie", cookie);
	}
	soup_session_send_and_read_async(f->session, message, G_PRIORITY_DEFAULT, NULL, request_done, &result);
	while (!result.done)
		g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(result.error);
	g_assert_cmpuint(soup_message_get_status(message), ==, 200);
	body = g_strndup(g_bytes_get_data(result.body, NULL), g_bytes_get_size(result.body));
	g_bytes_unref(result.body);
	return body;
}

static void
test_api_follows_pick(Fixture *f, gconstpointer data)
{
	g_autofree gchar *second = g_strdup_printf("%" G_GINT64_FORMAT, f->second);
	g_autofree gchar *picked = get(f, "/api/v1/reports/pnl?period=all", second);
	g_autofree gchar *none = get(f, "/api/v1/reports/pnl?period=all", NULL);
	g_autofree gchar *path = g_strdup_printf("/api/v1/reports/pnl?period=all&organization_id=%" G_GINT64_FORMAT, f->first);
	g_autofree gchar *named = get(f, path, second);
	(void)data;
	g_assert_nonnull(strstr(picked, "7.00 USD"));
	g_assert_null(strstr(picked, "100.00 USD"));
	/* No pick: the default organization, as before. */
	g_assert_nonnull(strstr(none, "100.00 USD"));
	/* A named organization wins over the pick. */
	g_assert_nonnull(strstr(named, "100.00 USD"));
	g_assert_null(strstr(named, "\"7.00 USD\""));
}

static void
test_page_follows_pick(Fixture *f, gconstpointer data)
{
	g_autofree gchar *second = g_strdup_printf("%" G_GINT64_FORMAT, f->second);
	g_autofree gchar *page = get(f, "/reports/pnl?period=all", second);
	(void)data;
	g_assert_nonnull(strstr(page, "$7.00"));
	g_assert_null(strstr(page, "$100.00"));
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	venture_entity_registry_register_builtins(venture_entity_registry_get_default());
	g_test_add("/report-scope/api-follows-pick", Fixture, NULL, set_up, test_api_follows_pick, tear_down);
	g_test_add("/report-scope/page-follows-pick", Fixture, NULL, set_up, test_page_follows_pick, tear_down);
	return g_test_run();
}
