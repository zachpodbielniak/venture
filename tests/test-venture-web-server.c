/*
 * A business owner mints their own API token from the page the MCP docs
 * name. They are an editor, not a platform administrator, and the page
 * must not hand them anyone else's credentials.
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <venture.h>
#include <libsoup/soup.h>
#include <string.h>
#include "venture-test-util.h"

/* Hosted outbound webhooks belong to the business, not the platform. */
static void
test_webhook_records_belong_to_the_business(void)
{
	g_assert_nonnull(venture_entity_registry_get_default());
	g_assert_cmpint(venture_data_class_for_type(VENTURE_TYPE_WEBHOOK),
		==, VENTURE_DATA_CLASS_TENANT);
	g_assert_cmpint(venture_data_class_for_type(VENTURE_TYPE_WEBHOOK_DELIVERY),
		==, VENTURE_DATA_CLASS_TENANT);
}

typedef struct
{
	VentureDatabase *database;
	VentureConfig *config;
	VentureContext *context;
	VentureWebServer *server;
	SoupSession *session;
	gchar *state_dir;
	gchar *url;
} Fixture;

typedef struct
{
	gboolean done;
	GBytes *body;
	GError *error;
} Reply;

static void
reply_done(GObject *source, GAsyncResult *result, gpointer data)
{
	Reply *reply = data;

	reply->body = soup_session_send_and_read_finish(SOUP_SESSION(source),
	                                                 result, &reply->error);
	reply->done = TRUE;
}

static guint
exchange(Fixture *fixture, const gchar *method, const gchar *path,
         const gchar *cookie, const gchar *form, gchar **body,
         gchar **set_cookie)
{
	g_autofree gchar *url = g_strconcat(fixture->url, path, NULL);
	g_autoptr(SoupMessage) message = soup_message_new(method, url);
	Reply reply = { FALSE, NULL, NULL };

	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);

	if (NULL != cookie)
		soup_message_headers_append(soup_message_get_request_headers(message),
		                            "Cookie", cookie);

	if (NULL != form)
	{
		g_autoptr(GBytes) bytes = g_bytes_new(form, strlen(form));

		soup_message_set_request_body_from_bytes(message,
			"application/x-www-form-urlencoded", bytes);
	}

	soup_session_send_and_read_async(fixture->session, message,
	                                 G_PRIORITY_DEFAULT, NULL, reply_done,
	                                 &reply);

	while (!reply.done)
		g_main_context_iteration(NULL, TRUE);

	g_assert_no_error(reply.error);

	if (NULL != body)
		*body = g_strndup(g_bytes_get_data(reply.body, NULL),
		                  g_bytes_get_size(reply.body));

	if (NULL != set_cookie)
		*set_cookie = g_strdup(soup_message_headers_get_one(
			soup_message_get_response_headers(message), "Set-Cookie"));

	g_bytes_unref(reply.body);

	return soup_message_get_status(message);
}

static void
fixture_set_up(Fixture *fixture, gconstpointer data)
{
	g_autoptr(GError) error = NULL;

	fixture->state_dir = g_dir_make_tmp("venture-web-server-XXXXXX", NULL);
	fixture->config = venture_config_new();
	g_object_set(fixture->config,
	             "state-dir", fixture->state_dir,
	             "server-bind-address", "127.0.0.1",
	             "server-port", (gint64)0,
	             "security-require-auth", TRUE,
	             "security-password-iterations", (gint64)2000,
	             NULL);
	fixture->database = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(fixture->database,
		venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	fixture->context = venture_context_new(fixture->config, fixture->database);
	fixture->server = venture_web_server_new(fixture->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(fixture->server, &error));
	g_assert_no_error(error);
	fixture->url = g_strdup_printf("http://127.0.0.1:%u",
		venture_web_server_get_port(fixture->server));
	fixture->session = soup_session_new();
}

static void
fixture_tear_down(Fixture *fixture, gconstpointer data)
{
	if (NULL != fixture->server)
		venture_web_server_stop(fixture->server);

	g_clear_object(&fixture->session);
	g_clear_object(&fixture->server);
	g_clear_object(&fixture->context);
	g_clear_object(&fixture->database);
	g_clear_object(&fixture->config);

	if (NULL != fixture->state_dir)
	{
		venture_test_remove_tree(fixture->state_dir);
		g_clear_pointer(&fixture->state_dir, g_free);
	}

	g_clear_pointer(&fixture->url, g_free);
}

static gchar *
sign_in(Fixture *fixture, const gchar *username, const gchar *password)
{
	g_autofree gchar *form = NULL;
	g_autofree gchar *set_cookie = NULL;
	gchar *semicolon;
	guint status;

	form = g_strdup_printf("username=%s&password=%s", username, password);
	status = exchange(fixture, "POST", "/login", NULL, form, NULL, &set_cookie);
	g_assert_cmpuint(status, ==, SOUP_STATUS_FOUND);
	g_assert_nonnull(set_cookie);
	semicolon = strchr(set_cookie, ';');

	if (NULL != semicolon)
		*semicolon = '\0';

	return g_steal_pointer(&set_cookie);
}

static void
add_member(Fixture *fixture, const gchar *username, const gchar *password,
           VentureUserRole role)
{
	g_autoptr(VentureUser) user = venture_user_new();
	g_autoptr(VentureEntity) member = NULL;

	g_object_set(user, "username", username, "role", role, "active", TRUE, NULL);
	g_assert_true(venture_user_set_password(user, password, 2000, NULL));
	g_assert_true(venture_database_save(fixture->database, VENTURE_ENTITY(user),
	                                    NULL, NULL));
	member = g_object_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP,
		"user-id", venture_entity_get_id(VENTURE_ENTITY(user)),
		"organization-id", venture_context_get_default_organization_id(fixture->context),
		"role", VENTURE_ORGANIZATION_ROLE_ADMIN, "active", TRUE, NULL);
	g_assert_true(venture_database_save(fixture->database, member, NULL, NULL));
}

/*
 * docs/mcp.org sends a signed-in owner to /account/tokens. That owner is
 * not a platform administrator. The page mints a token in their own role
 * and does not offer one that never expires.
 */
static void
test_owner_opens_token_page(Fixture *fixture, gconstpointer data)
{
	g_autofree gchar *cookie = NULL;
	g_autofree gchar *page = NULL;
	guint status;

	add_member(fixture, "olivia", "o-long-password", VENTURE_USER_ROLE_EDITOR);
	cookie = sign_in(fixture, "olivia", "o-long-password");
	status = exchange(fixture, "GET", "/account/tokens", cookie, NULL, &page, NULL);
	g_assert_cmpuint(status, ==, SOUP_STATUS_OK);
	g_assert_nonnull(strstr(page, "Mint a token"));
	g_assert_null(strstr(page, "never</option>"));
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/web-server/webhook-records-are-tenant",
		test_webhook_records_belong_to_the_business);
	g_test_add("/web-server/owner-opens-token-page", Fixture, NULL,
	           fixture_set_up, test_owner_opens_token_page, fixture_tear_down);

	return g_test_run();
}
