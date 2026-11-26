/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <libsoup/soup.h>
#include <string.h>
#include "venture-test-util.h"

/* Outbound webhooks are a business's destinations. Hosted mode refuses
 * platform records, so the type itself has to be tenant data. */
static void
test_webhook_records_belong_to_the_business(void)
{
	/* Classification is applied when the built-in registry is built. */
	g_assert_nonnull(venture_entity_registry_get_default());
	g_assert_cmpint(venture_data_class_for_type(VENTURE_TYPE_WEBHOOK),
		==, VENTURE_DATA_CLASS_TENANT);
	g_assert_cmpint(venture_data_class_for_type(VENTURE_TYPE_WEBHOOK_DELIVERY),
		==, VENTURE_DATA_CLASS_TENANT);
}

/* --- Signed-in HTTP ------------------------------------------------------- */

typedef struct
{
	VentureConfig *config;
	VentureDatabase *db;
	VentureContext *context;
	VentureWebServer *server;
	SoupSession *session;
	gchar *state_dir;
	gchar *cookie;
	guint port;
	gint64 org;
	gint64 other;
} ServerFixture;

typedef struct
{
	gboolean done;
	GBytes *body;
	GError *error;
} RequestResult;

static void
request_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
	RequestResult *outcome = user_data;

	outcome->body = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &outcome->error);
	outcome->done = TRUE;
}

/*
 * One exchange. @form is an application/x-www-form-urlencoded body, or
 * NULL. The query string is left for the caller to encode: htmx decodes
 * it once before a handler sees it, so a value the handler must still
 * see percent-encoded is escaped a second time on the wire.
 */
static guint
server_exchange(
	ServerFixture *f,
	const gchar *method,
	const gchar *path,
	const gchar *form,
	gchar **location,
	gchar **set_cookie
){
	g_autoptr(SoupMessage) message = NULL;
	g_autofree gchar *url = g_strdup_printf("http://127.0.0.1:%u%s", f->port, path);
	RequestResult outcome = { FALSE, NULL, NULL };

	message = soup_message_new(method, url);
	g_assert_nonnull(message);
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);

	if (NULL != f->cookie)
		soup_message_headers_append(soup_message_get_request_headers(message), "Cookie", f->cookie);

	if (NULL != form)
	{
		g_autoptr(GBytes) bytes = g_bytes_new(form, strlen(form));

		soup_message_set_request_body_from_bytes(message, "application/x-www-form-urlencoded", bytes);
	}

	soup_session_send_and_read_async(f->session, message, G_PRIORITY_DEFAULT, NULL, request_done, &outcome);

	while (!outcome.done)
		g_main_context_iteration(NULL, TRUE);

	g_assert_no_error(outcome.error);
	g_clear_pointer(&outcome.body, g_bytes_unref);

	if (NULL != location)
		*location = g_strdup(soup_message_headers_get_one(soup_message_get_response_headers(message), "Location"));

	if (NULL != set_cookie)
		*set_cookie = g_strdup(soup_message_headers_get_list(soup_message_get_response_headers(message), "Set-Cookie"));

	return soup_message_get_status(message);
}

static void
server_login(ServerFixture *f, const gchar *username, const gchar *password)
{
	g_autofree gchar *form = g_strdup_printf("username=%s&password=%s", username, password);
	g_autofree gchar *set_cookie = NULL;
	gchar *semicolon;

	g_assert_cmpuint(server_exchange(f, "POST", "/login", form, NULL, &set_cookie), ==, 302);
	g_assert_nonnull(set_cookie);
	semicolon = strchr(set_cookie, ';');

	if (NULL != semicolon)
		*semicolon = '\0';

	g_clear_pointer(&f->cookie, g_free);
	f->cookie = g_steal_pointer(&set_cookie);
}

static void
server_set_up(ServerFixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureUser) viewer = venture_user_new();
	g_autoptr(VentureEntity) other = g_object_new(VENTURE_TYPE_ORGANIZATION, "name", "Elsewhere", "slug", "elsewhere", NULL);
	g_autoptr(VentureEntity) membership = NULL;

	(void)unused;
	g_setenv("VENTURE_TEST_REDIRECT_SECRET", "safe-redirect-test-secret", TRUE);
	f->state_dir = g_dir_make_tmp("venture-redirect-XXXXXX", NULL);
	f->config = venture_config_new();
	g_object_set(f->config, "state-dir", f->state_dir, "server-bind-address", "127.0.0.1",
		"server-port", (gint64)0, "security-session-secret-env", "VENTURE_TEST_REDIRECT_SECRET",
		"security-password-iterations", (gint64)100000, NULL);
	f->db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->db);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->org = venture_context_get_default_organization_id(f->context);
	g_assert_true(venture_database_save(f->db, other, NULL, &error));
	g_assert_no_error(error);
	f->other = venture_entity_get_id(other);
	g_object_set(viewer, "username", "viewer", "role", VENTURE_USER_ROLE_VIEWER, "active", TRUE, NULL);
	g_assert_true(venture_user_set_password(viewer, "viewer-password-1", 100000, NULL));
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(viewer), NULL, &error));
	g_assert_no_error(error);
	/* One membership, so /entity is reached. The other organization
	 * exists and this person is not a member of it. */
	membership = g_object_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP,
		"user-id", venture_entity_get_id(VENTURE_ENTITY(viewer)),
		"organization-id", f->org,
		"active", TRUE,
		"role", VENTURE_ORGANIZATION_ROLE_VIEWER,
		NULL);
	g_assert_true(venture_database_save(f->db, membership, NULL, &error));
	g_assert_no_error(error);
	f->server = venture_web_server_new(f->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(f->server, &error));
	g_assert_no_error(error);
	f->port = venture_web_server_get_port(f->server);
	f->session = soup_session_new();
	server_login(f, "viewer", "viewer-password-1");
}

static void
server_tear_down(ServerFixture *f, gconstpointer unused)
{
	(void)unused;

	if (NULL != f->server)
		venture_web_server_stop(f->server);

	g_clear_pointer(&f->cookie, g_free);
	g_clear_object(&f->session);
	g_clear_object(&f->server);
	g_clear_object(&f->context);
	g_clear_object(&f->db);
	g_clear_object(&f->config);

	if (NULL != f->state_dir)
	{
		venture_test_remove_tree(f->state_dir);
		g_clear_pointer(&f->state_dir, g_free);
	}
}

static gboolean
cookie_is_entity(const gchar *set_cookie, const gchar *value)
{
	g_autofree gchar *expect = g_strdup_printf(
		"venture_entity=%s; Path=/; HttpOnly; SameSite=Lax; Max-Age=31536000", value);

	return (NULL != set_cookie) && (NULL != strstr(set_cookie, expect));
}

/* GET /entity/all?back= keeps only a same-site path. htmx decodes the
 * query once, so the wire value is escaped again and the handler still
 * sees the spellings below, including "%2F" and "%0D%0A". */
static void
test_entity_back_stays_local(ServerFixture *f, gconstpointer unused)
{
	static const gchar *rejected[] = {
		"https://evil.example/login",
		"//evil.example",
		"/\\evil.example",
		"/%2F%2Fevil.example",
		"/%5Cevil.example",
		"/ok%0D%0ASet-Cookie:%20x=1",
		"javascript:alert(1)",
		"",
		NULL
	};
	guint i;

	(void)unused;

	for (i = 0; NULL != rejected[i]; i++)
	{
		g_autofree gchar *escaped = g_uri_escape_string(rejected[i], NULL, FALSE);
		g_autofree gchar *path = g_strdup_printf("/entity/all?back=%s", escaped);
		g_autofree gchar *location = NULL;
		g_autofree gchar *set_cookie = NULL;

		g_assert_cmpuint(server_exchange(f, "GET", path, NULL, &location, &set_cookie), ==, 302);
		g_assert_cmpstr(location, ==, "/");
		g_assert_true(cookie_is_entity(set_cookie, "all"));
		g_assert_null(strstr(set_cookie, "evil"));
		g_assert_null(strstr(location, "\r"));
		g_assert_null(strstr(location, "\n"));
	}

	{
		g_autofree gchar *escaped = g_uri_escape_string("/e/ticket/12", NULL, FALSE);
		g_autofree gchar *path = g_strdup_printf("/entity/all?back=%s", escaped);
		g_autofree gchar *location = NULL;
		g_autofree gchar *set_cookie = NULL;

		g_assert_cmpuint(server_exchange(f, "GET", path, NULL, &location, &set_cookie), ==, 302);
		g_assert_cmpstr(location, ==, "/e/ticket/12");
		g_assert_true(cookie_is_entity(set_cookie, "all"));
	}
}

/* A readable organization is the only id written into the cookie.
 * A stranger, junk, and a cookie-attribute injection are 404 with no
 * venture_entity cookie. */
static void
test_entity_cookie_is_a_readable_org(ServerFixture *f, gconstpointer unused)
{
	g_autofree gchar *readable = g_strdup_printf("/entity/%" G_GINT64_FORMAT "?back=/e/ticket/12", f->org);
	g_autofree gchar *hidden = g_strdup_printf("/entity/%" G_GINT64_FORMAT, f->other);
	g_autofree gchar *injected_id = g_uri_escape_string("1;Path=/x", NULL, FALSE);
	g_autofree gchar *injected = g_strdup_printf("/entity/%s", injected_id);
	g_autofree gchar *expect_id = g_strdup_printf("%" G_GINT64_FORMAT, f->org);
	g_autofree gchar *location = NULL;
	g_autofree gchar *set_cookie = NULL;
	g_autofree gchar *hidden_cookie = NULL;
	g_autofree gchar *junk_cookie = NULL;
	g_autofree gchar *inject_cookie = NULL;

	(void)unused;
	/* The query is decoded once on the way in. Escape the kept path
	 * so the handler sees "/e/ticket/12", not "%2F". */
	g_clear_pointer(&readable, g_free);
	{
		g_autofree gchar *back = g_uri_escape_string("/e/ticket/12", NULL, FALSE);

		readable = g_strdup_printf("/entity/%" G_GINT64_FORMAT "?back=%s", f->org, back);
	}

	g_assert_cmpuint(server_exchange(f, "GET", readable, NULL, &location, &set_cookie), ==, 302);
	g_assert_cmpstr(location, ==, "/e/ticket/12");
	g_assert_true(cookie_is_entity(set_cookie, expect_id));
	g_assert_null(strstr(set_cookie, "Path=/x"));

	g_assert_cmpuint(server_exchange(f, "GET", hidden, NULL, NULL, &hidden_cookie), ==, 404);
	g_assert_true((NULL == hidden_cookie) || (NULL == strstr(hidden_cookie, "venture_entity")));

	g_assert_cmpuint(server_exchange(f, "GET", "/entity/abc", NULL, NULL, &junk_cookie), ==, 404);
	g_assert_true((NULL == junk_cookie) || (NULL == strstr(junk_cookie, "venture_entity")));

	g_assert_cmpuint(server_exchange(f, "GET", injected, NULL, NULL, &inject_cookie), ==, 404);
	g_assert_true((NULL == inject_cookie) || (NULL == strstr(inject_cookie, "venture_entity")));
}

/* The look switch is one of the swept routes. An encoded second slash
 * in its form "back" is the home page, not another host. */
static void
test_look_back_stays_local(ServerFixture *f, gconstpointer unused)
{
	g_autofree gchar *evil = g_uri_escape_string("/%2F%2Fevil.example", NULL, FALSE);
	g_autofree gchar *kept = g_uri_escape_string("/e/ticket/12", NULL, FALSE);
	g_autofree gchar *bad_form = g_strdup_printf("look=classic&back=%s", evil);
	g_autofree gchar *good_form = g_strdup_printf("look=classic&back=%s", kept);
	g_autofree gchar *location = NULL;
	g_autofree gchar *good_location = NULL;

	(void)unused;
	g_assert_cmpuint(server_exchange(f, "POST", "/look", bad_form, &location, NULL), ==, 302);
	g_assert_cmpstr(location, ==, "/");
	g_assert_cmpuint(server_exchange(f, "POST", "/look", good_form, &good_location, NULL), ==, 302);
	g_assert_cmpstr(good_location, ==, "/e/ticket/12");
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/web-server/webhook-records-are-tenant",
		test_webhook_records_belong_to_the_business);
	g_test_add("/web-server/entity-back-stays-local", ServerFixture, NULL,
		server_set_up, test_entity_back_stays_local, server_tear_down);
	g_test_add("/web-server/entity-cookie-is-a-readable-org", ServerFixture, NULL,
		server_set_up, test_entity_cookie_is_a_readable_org, server_tear_down);
	g_test_add("/web-server/look-back-stays-local", ServerFixture, NULL,
		server_set_up, test_look_back_stays_local, server_tear_down);

	return g_test_run();
}
