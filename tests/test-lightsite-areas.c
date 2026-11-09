/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* GET /api/v1/organizations/{id}/lightsite-areas names the area types this
 * caller may open. A type whose list is 403 or 404 is absent. A missing
 * organization is 404. No credential is 401. */
#include <venture.h>
#include <libsoup/soup.h>
#include <string.h>
#include "venture-test-accounting.h"
#include "venture-test-util.h"

typedef struct {
	VentureDatabase *db;
	VentureConfig *config;
	VentureContext *context;
	VentureWebServer *server;
	SoupSession *session;
	gchar *directory, *token;
	gint64 business;
} Fixture;
typedef struct { gboolean done; GBytes *bytes; GError *error; } Reply;

static void received(GObject *source, GAsyncResult *result, gpointer data)
{
	Reply *reply = data;
	reply->bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &reply->error);
	reply->done = TRUE;
}

static JsonNode *exchange(Fixture *f, const gchar *path, gboolean auth, guint expected)
{
	g_autofree gchar *url = g_strconcat(venture_web_server_get_base_url(f->server), path, NULL);
	g_autofree gchar *header = auth ? g_strconcat("Bearer ", f->token, NULL) : NULL;
	g_autoptr(SoupMessage) message = soup_message_new("GET", url);
	g_autofree gchar *terminated = NULL;
	Reply reply = { FALSE, NULL, NULL };
	JsonNode *answer;
	const gchar *text;
	gsize length;
	soup_message_headers_replace(soup_message_get_request_headers(message), "Host", "example.test");
	if (header) soup_message_headers_replace(soup_message_get_request_headers(message), "Authorization", header);
	soup_session_send_and_read_async(f->session, message, G_PRIORITY_DEFAULT, NULL, received, &reply);
	while (!reply.done) g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(reply.error);
	text = g_bytes_get_data(reply.bytes, &length);
	if (soup_message_get_status(message) != expected) g_test_message("Response for %s: %.*s", path, (gint)length, text);
	g_assert_cmpuint(soup_message_get_status(message), ==, expected);
	terminated = g_strndup(text, length);
	answer = length ? venture_json_parse(terminated, NULL) : NULL;
	g_bytes_unref(reply.bytes);
	return answer;
}

static void save(Fixture *f, VentureEntity *row)
{
	g_autoptr(GError) error = NULL;
	gboolean ok = venture_database_save(f->db, row, NULL, &error);
	g_assert_no_error(error); g_assert_true(ok);
}

static gchar *mint(Fixture *f, gint64 user, VentureUserRole role)
{
	g_autoptr(VentureApiToken) token = venture_api_token_new();
	gchar *secret;
	g_object_set(token, "name", "Area fixture", "user-id", user, "role", role, NULL);
	secret = venture_api_token_generate(token); save(f, VENTURE_ENTITY(token));
	return secret;
}

static gboolean type_listed(JsonNode *body, const gchar *type)
{
	JsonArray *types = json_object_get_array_member(json_node_get_object(body), "types");
	guint i;
	g_assert_nonnull(types);
	for (i = 0; i < json_array_get_length(types); i++)
		if (!g_strcmp0(json_array_get_string_element(types, i), type)) return TRUE;
	return FALSE;
}

static void setup(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GInetAddress) address = g_inet_address_new_loopback(G_SOCKET_FAMILY_IPV4);
	g_autofree gchar *bind = g_inet_address_to_string(address);
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_USER);
	g_autoptr(GPtrArray) users = NULL;
	gchar *admin = NULL;
	VentureTenantService *tenant;
	(void)unused;
	f->db = venture_test_accounting_database(&error); g_assert_no_error(error);
	f->config = venture_config_new();
	f->directory = g_dir_make_tmp("venture-lightsite-areas-XXXXXX", &error); g_assert_no_error(error);
	g_object_set(f->config, "hosted-enabled", TRUE, "hosted-workspace-id", "8f062b79-1d2b-4d7f-99e5-bd3bf588e05a",
		"hosted-origin", "https://example.test", "security-password-iterations", (gint64)2000,
		"state-dir", f->directory, "server-bind-address", bind, "server-port", (gint64)0, NULL);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error)); g_assert_no_error(error);
	tenant = venture_tenant_service_get(f->db);
	g_assert_true(venture_tenant_service_configure(tenant, f->config, &error)); g_assert_no_error(error);
	g_assert_true(venture_tenant_service_initialize(tenant, &error)); g_assert_no_error(error);
	g_assert_true(venture_tenant_service_bootstrap_admin(tenant, f->config, "operator", "test-password", FALSE, "Fixture", &error));
	g_assert_no_error(error);
	users = venture_database_find(f->db, query, &error); g_assert_no_error(error);
	admin = mint(f, venture_entity_get_id(g_ptr_array_index(users, 0)), VENTURE_USER_ROLE_EDITOR);
	f->token = admin;
	f->context = venture_context_new(f->config, f->db);
	f->server = venture_web_server_new(f->context, &error); g_assert_no_error(error);
	g_assert_true(venture_web_server_start(f->server, &error)); g_assert_no_error(error);
	f->session = soup_session_new();
}

static JsonNode *post(Fixture *f, const gchar *path, const gchar *body, guint expected)
{
	g_autofree gchar *url = g_strconcat(venture_web_server_get_base_url(f->server), path, NULL);
	g_autofree gchar *header = g_strconcat("Bearer ", f->token, NULL);
	g_autoptr(SoupMessage) message = soup_message_new("POST", url);
	g_autoptr(GBytes) payload = g_bytes_new(body, strlen(body));
	g_autofree gchar *terminated = NULL;
	Reply reply = { FALSE, NULL, NULL };
	JsonNode *answer;
	const gchar *text;
	gsize length;
	soup_message_headers_replace(soup_message_get_request_headers(message), "Host", "example.test");
	soup_message_headers_replace(soup_message_get_request_headers(message), "Authorization", header);
	soup_message_set_request_body_from_bytes(message, "application/json", payload);
	soup_session_send_and_read_async(f->session, message, G_PRIORITY_DEFAULT, NULL, received, &reply);
	while (!reply.done) g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(reply.error);
	text = g_bytes_get_data(reply.bytes, &length);
	if (soup_message_get_status(message) != expected) g_test_message("Response for %s: %.*s", path, (gint)length, text);
	g_assert_cmpuint(soup_message_get_status(message), ==, expected);
	terminated = g_strndup(text, length);
	answer = venture_json_parse(terminated, NULL);
	g_bytes_unref(reply.bytes);
	return answer;
}

static void setup_business(Fixture *f, gconstpointer unused)
{
	g_autoptr(JsonNode) business = NULL;
	gint64 owner;
	setup(f, unused);
	business = post(f, "/api/v1/lightsite/signups",
		"{\"idempotency_key\":\"areas-owner\",\"email\":\"owner@example.test\",\"issuer\":\"https://id.example.test\",\"subject\":\"areas-owner\",\"business_name\":\"Example Grooming\",\"first_name\":\"Alex\",\"last_name\":\"Example\"}", 201);
	f->business = json_object_get_int_member(json_node_get_object(business), "organization_id");
	owner = json_object_get_int_member(json_node_get_object(business), "user_id");
	g_free(f->token);
	f->token = mint(f, owner, VENTURE_USER_ROLE_EDITOR);
}

static void teardown(Fixture *f, gconstpointer unused)
{
	(void)unused;
	if (f->server) venture_web_server_stop(f->server);
	g_clear_object(&f->server); g_clear_object(&f->context); g_clear_object(&f->session);
	if (f->db) venture_test_accounting_database_cleanup(f->db);
	g_clear_object(&f->db); g_clear_object(&f->config);
	if (f->directory) venture_test_remove_tree(f->directory);
	g_free(f->directory); g_free(f->token);
}

static void test_areas(Fixture *f, gconstpointer unused)
{
	g_autofree gchar *path = g_strdup_printf("/api/v1/organizations/%" G_GINT64_FORMAT "/lightsite-areas", f->business);
	g_autofree gchar *missing = g_strdup_printf("/api/v1/organizations/%" G_GINT64_FORMAT "/lightsite-areas", f->business + 1000);
	g_autoptr(JsonNode) anon = NULL, body = NULL, gone = NULL;
	(void)unused;
	anon = exchange(f, path, FALSE, 401);
	g_clear_pointer(&anon, json_node_unref);
	body = exchange(f, path, TRUE, 200);
	g_assert_true(type_listed(body, "company"));
	g_assert_true(type_listed(body, "invoice"));
	g_assert_false(type_listed(body, "user"));
	gone = exchange(f, missing, TRUE, 404);
	g_clear_pointer(&gone, json_node_unref);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/lightsite-areas/caller-types", Fixture, NULL, setup_business, test_areas, teardown);
	return g_test_run();
}
