/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <libsoup/soup.h>
#include <unistd.h>
#include "venture-test-accounting.h"
#include "venture-test-util.h"

typedef struct { gboolean done; GBytes *body; GError *error; } Reply;
static void received(GObject *source, GAsyncResult *result, gpointer data)
{
	Reply *reply = data;
	reply->body = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &reply->error);
	reply->done = TRUE;
}
static JsonNode *request(SoupSession *session, const gchar *base, const gchar *path,
	const gchar *token, const gchar *host, guint expected)
{
	g_autofree gchar *url = g_strconcat(base, path, NULL);
	g_autoptr(SoupMessage) message = soup_message_new("GET", url);
	g_autoptr(JsonParser) parser = json_parser_new();
	Reply reply = { FALSE, NULL, NULL };
	gsize length;
	const gchar *bytes;
	if (token) {
		g_autofree gchar *header = g_strconcat("Bearer ", token, NULL);
		soup_message_headers_replace(soup_message_get_request_headers(message), "Authorization", header);
	}
	if (host) soup_message_headers_replace(soup_message_get_request_headers(message), "Host", host);
	soup_session_send_and_read_async(session, message, G_PRIORITY_DEFAULT, NULL, received, &reply);
	while (!reply.done) g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(reply.error);
	g_assert_cmpuint(soup_message_get_status(message), ==, expected);
	g_assert_cmpstr(soup_message_headers_get_one(soup_message_get_response_headers(message), "Cache-Control"), ==, "no-store");
	bytes = g_bytes_get_data(reply.body, &length);
	if (g_str_has_prefix(path, "/api/v1/account-authority")) g_assert_cmpuint(length, <=, 4096);
	g_assert_true(json_parser_load_from_data(parser, bytes, (gssize)length, &reply.error));
	g_assert_no_error(reply.error);
	g_bytes_unref(reply.body);
	return json_node_copy(json_parser_get_root(parser));
}
static void sql(VentureDatabase *db, const gchar *statement)
{
	g_autoptr(GError) error = NULL;
	gboolean ok = venture_database_execute(db, statement, NULL, &error);
	g_assert_no_error(error); g_assert_true(ok);
}
static void authority_entry(JsonNode *result, gint64 organization, const gchar *role, gboolean manage)
{
	JsonArray *entries = json_object_get_array_member(json_node_get_object(result), "organizations");
	guint i;
	for (i = 0; i < json_array_get_length(entries); i++) {
		JsonObject *entry = json_array_get_object_element(entries, i);
		if (json_object_get_int_member(entry, "organization_id") != organization) continue;
		g_assert_nonnull(role);
		g_assert_cmpstr(json_object_get_string_member(entry, "role"), ==, role);
		g_assert_cmpint(json_object_get_boolean_member(entry, "can_manage_sites"), ==, manage);
		return;
	}
	g_assert_null(role);
}
/* The integration uses a real authenticated token; no mocked identity service
 * may accidentally make tenant administration readable to token callers. */
static void identity_http(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureDatabase) db = venture_test_accounting_database(&error);
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureContext) context = NULL;
	g_autoptr(VentureWebServer) server = NULL;
	g_autoptr(VentureEntity) org = g_object_new(VENTURE_TYPE_ORGANIZATION, "name", "Account A", "active", TRUE, NULL);
	g_autoptr(VentureEntity) other = g_object_new(VENTURE_TYPE_ORGANIZATION, "name", "Account B", "active", TRUE, NULL);
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_USER);
	g_autoptr(GPtrArray) users = NULL;
	g_autoptr(VentureApiToken) token = venture_api_token_new();
	g_autoptr(SoupSession) session = soup_session_new_with_options("timeout", 10, NULL);
	g_autoptr(JsonNode) result = NULL;
	g_autofree gchar *directory = g_dir_make_tmp("venture-account-XXXXXX", &error);
	g_autofree gchar *secret = NULL, *path = NULL, *other_path = NULL;
	/*
	 * The public origin is what a proxy in front would answer on, not the
	 * port the server binds: the server listens on port 0 -- whatever the
	 * kernel picks -- and every request carries the origin's authority as
	 * its Host, exactly as it would behind a TLS terminator. A listening
	 * port chosen in advance (it used to come from the pid) is a port
	 * somebody else can already hold.
	 */
	const gchar *origin = "http://127.0.0.1:8443";
	const gchar *authority = "127.0.0.1:8443";
	VentureTenantService *service;
	gboolean ok;
	g_assert_no_error(error);
	g_object_set(config, "hosted-enabled", TRUE, "hosted-workspace-id", "8f062b79-1d2b-4d7f-99e5-bd3bf588e05a",
		"hosted-origin", origin, "security-password-iterations", (gint64)2000,
		"state-dir", directory, "server-bind-address", "127.0.0.1", "server-port", (gint64)0, NULL);
	ok = venture_database_migrate(db, venture_entity_registry_get_default(), &error);
	g_assert_no_error(error); g_assert_true(ok);
	service = venture_tenant_service_get(db);
	g_assert_true(venture_tenant_service_configure(service, config, &error)); g_assert_no_error(error);
	g_assert_true(venture_tenant_service_initialize(service, &error)); g_assert_no_error(error);
	g_assert_true(venture_database_save(db, org, NULL, &error)); g_assert_no_error(error);
	g_assert_true(venture_tenant_service_bootstrap_admin(service, config, "provisioner", "private-test-password",
		FALSE, "Account fixture", &error)); g_assert_no_error(error);
	/* Created after bootstrap: this organization is outside the token snapshot. */
	g_assert_true(venture_database_save(db, other, NULL, &error)); g_assert_no_error(error);
	users = venture_database_find(db, query, &error); g_assert_no_error(error);
	g_object_set(token, "name", "Lightsite provisioning", "user-id", venture_entity_get_id(g_ptr_array_index(users, 0)),
		"role", VENTURE_USER_ROLE_EDITOR, NULL);
	secret = venture_api_token_generate(token);
	g_assert_true(venture_database_save(db, VENTURE_ENTITY(token), NULL, &error)); g_assert_no_error(error);
	context = venture_context_new(config, db);
	server = venture_web_server_new(context, &error); g_assert_no_error(error);
	g_assert_true(venture_web_server_start(server, &error)); g_assert_no_error(error);
	path = g_strdup_printf("/api/v1/account-identity/%" G_GINT64_FORMAT, venture_entity_get_id(org));
	other_path = g_strdup_printf("/api/v1/account-identity/%" G_GINT64_FORMAT, venture_entity_get_id(other));
#define CHECK(p, t, h, s) G_STMT_START { g_clear_pointer(&result, json_node_unref); result = request(session, venture_web_server_get_base_url(server), p, t, (h) ? (h) : authority, s); } G_STMT_END
	CHECK("/api/v1/account-authority", secret, NULL, 200);
	g_assert_true(json_object_has_member(json_node_get_object(result), "organizations"));
	authority_entry(result, venture_entity_get_id(org), "admin", TRUE);
	authority_entry(result, venture_entity_get_id(other), NULL, FALSE);
	g_assert_cmpint(json_object_get_int_member(json_node_get_object(result), "user_id"), ==,
		venture_entity_get_id(g_ptr_array_index(users, 0)));
	{
		g_autoptr(VentureApiToken) readonly = venture_api_token_new();
		g_autofree gchar *readonly_secret = NULL;
		g_object_set(readonly, "name", "Read-only authority", "user-id",
			venture_entity_get_id(g_ptr_array_index(users, 0)), "role", VENTURE_USER_ROLE_VIEWER, NULL);
		readonly_secret = venture_api_token_generate(readonly);
		g_assert_true(venture_database_save(db, VENTURE_ENTITY(readonly), NULL, &error)); g_assert_no_error(error);
		CHECK("/api/v1/account-authority", readonly_secret, NULL, 200);
		authority_entry(result, venture_entity_get_id(org), "admin", FALSE);
	}
	CHECK(path, secret, NULL, 200);
	g_assert_cmpuint(json_object_get_size(json_node_get_object(result)), ==, 4);
	g_assert_cmpstr(json_object_get_string_member(json_node_get_object(result), "origin"), ==, origin);
	g_assert_cmpstr(json_object_get_string_member(json_node_get_object(result), "workspace_id"), ==, "8f062b79-1d2b-4d7f-99e5-bd3bf588e05a");
	g_assert_cmpint(json_object_get_int_member(json_node_get_object(result), "organization_id"), ==, venture_entity_get_id(org));
	g_assert_cmpstr(json_object_get_string_member(json_node_get_object(result), "state"), ==, "active");
	CHECK("/api/v1/account-authority", NULL, NULL, 401);
	CHECK("/api/v1/account-authority", "invalid", NULL, 401);
	CHECK("/api/v1/account-authority", secret, "wrong.example", 403);
	CHECK(path, NULL, NULL, 401);
	CHECK(path, "invalid", NULL, 401);
	CHECK(path, secret, "wrong.example", 403);
	CHECK(other_path, secret, NULL, 403);
	{
		g_autoptr(VentureEntity) grant = g_object_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP,
			"organization-id", venture_entity_get_id(other), "user-id", venture_entity_get_id(g_ptr_array_index(users, 0)),
			"role", VENTURE_ORGANIZATION_ROLE_ADMIN, "active", TRUE, NULL);
		g_assert_true(venture_database_save(db, grant, NULL, &error)); g_assert_no_error(error);
		/* A later grant cannot broaden a previously minted service token. */
		CHECK(other_path, secret, NULL, 403);
	}
	/* Both sides of the authority intersection matter: demotion must revoke an
	 * old admin token, and promotion must not enlarge a viewer-minted token. */
	{
		g_autoptr(VentureQuery) members = venture_query_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP);
		g_autoptr(VentureEntity) member = NULL;
		g_autoptr(VentureApiToken) limited = venture_api_token_new();
		g_autofree gchar *limited_secret = NULL;
		venture_query_set_organization(members, venture_entity_get_id(org));
		member = venture_database_find_one(db, members, &error);
		g_assert_no_error(error); g_assert_nonnull(member);
		g_object_set(member, "role", VENTURE_ORGANIZATION_ROLE_VIEWER, NULL);
		g_assert_true(venture_database_save(db, member, NULL, &error)); g_assert_no_error(error);
		CHECK(path, secret, NULL, 403);
		CHECK("/api/v1/account-authority", secret, NULL, 200);
		authority_entry(result, venture_entity_get_id(org), "viewer", FALSE);
		g_object_set(limited, "name", "Limited provisioning", "user-id",
			venture_entity_get_id(g_ptr_array_index(users, 0)), "role", VENTURE_USER_ROLE_EDITOR, NULL);
		limited_secret = venture_api_token_generate(limited);
		g_assert_true(venture_database_save(db, VENTURE_ENTITY(limited), NULL, &error)); g_assert_no_error(error);
		CHECK(path, limited_secret, NULL, 403);
		g_object_set(member, "role", VENTURE_ORGANIZATION_ROLE_ADMIN, NULL);
		g_assert_true(venture_database_save(db, member, NULL, &error)); g_assert_no_error(error);
		CHECK(path, limited_secret, NULL, 403);
		CHECK(path, secret, NULL, 200);
		CHECK("/api/v1/account-authority", limited_secret, NULL, 200);
		authority_entry(result, venture_entity_get_id(org), "admin", FALSE);
		CHECK("/api/v1/account-authority", secret, NULL, 200);
		authority_entry(result, venture_entity_get_id(org), "admin", TRUE);
		authority_entry(result, venture_entity_get_id(other), NULL, FALSE);
	}
	sql(db, "ALTER TABLE organization_memberships RENAME TO authority_unavailable");
	CHECK("/api/v1/account-authority", secret, NULL, 500);
	sql(db, "ALTER TABLE authority_unavailable RENAME TO organization_memberships");
	CHECK("/api/v1/account-authority", secret, NULL, 200);
	authority_entry(result, venture_entity_get_id(org), "admin", TRUE);
	g_test_expect_message("Venture", G_LOG_LEVEL_MESSAGE,
		"Account identity request failed: cause=validation domain=* code=*");
	CHECK("/api/v1/account-identity/-1", secret, NULL, 422);
	g_test_assert_expected_messages();
	CHECK("/api/v1/account-identity/9223372036854775808", secret, NULL, 422);
	CHECK("/api/v1/account-identity/not-an-id", secret, NULL, 422);
	CHECK("/api/v1/account-identity/0", secret, NULL, 422);
	CHECK("/api/v1/tenant_workspace", secret, NULL, 403);
	sql(db, "UPDATE api_tokens SET expires_at='2000-01-01T00:00:00Z'");
	CHECK(path, secret, NULL, 401);
	CHECK("/api/v1/account-authority", secret, NULL, 401);
	sql(db, "UPDATE api_tokens SET expires_at=NULL");
	/* Retained rows and durable identity drift must never validate a new
	 * external account, even while the credential itself remains valid. */
	sql(db, "UPDATE organizations SET deleted_at='2026-01-01T00:00:00Z'");
	CHECK(path, secret, NULL, 403);
	CHECK("/api/v1/account-authority", secret, NULL, 200);
	authority_entry(result, venture_entity_get_id(org), NULL, FALSE);
	sql(db, "UPDATE organizations SET deleted_at=NULL; UPDATE users SET active=FALSE");
	CHECK(path, secret, NULL, 401);
	CHECK("/api/v1/account-authority", secret, NULL, 401);
	sql(db, "UPDATE users SET active=TRUE; UPDATE tenant_workspaces SET workspace_id='changed'");
	CHECK(path, secret, NULL, 403);
	sql(db, "UPDATE tenant_workspaces SET workspace_id='8f062b79-1d2b-4d7f-99e5-bd3bf588e05a'");
	CHECK(path, secret, NULL, 200);
	sql(db, "UPDATE organizations SET active=FALSE");
	CHECK(path, secret, NULL, 403);
	sql(db, "UPDATE organizations SET active=TRUE");
	sql(db, "UPDATE tenant_workspaces SET state='read_only'");
	CHECK(path, secret, NULL, 403);
	sql(db, "UPDATE tenant_workspaces SET state='suspended'");
	CHECK(path, secret, NULL, 403);
	CHECK("/api/v1/account-authority", secret, NULL, 403);
	sql(db, "UPDATE tenant_workspaces SET state='unknown'");
	CHECK(path, secret, NULL, 403);
	sql(db, "UPDATE tenant_workspaces SET state='active'; UPDATE organization_memberships SET active=FALSE");
	CHECK(path, secret, NULL, 403);
	sql(db, "UPDATE organization_memberships SET active=TRUE; UPDATE tenant_memberships SET active=FALSE");
	CHECK(path, secret, NULL, 403);
	sql(db, "UPDATE tenant_memberships SET active=TRUE; UPDATE api_tokens SET active=FALSE");
	CHECK(path, secret, NULL, 401);
	CHECK("/api/v1/account-authority", secret, NULL, 401);
#undef CHECK
	venture_web_server_stop(server); g_clear_object(&server); g_clear_object(&context);
	venture_test_accounting_database_cleanup(db);
	venture_test_remove_tree(directory);
}
int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/account-identity/http-authority", identity_http);
	return g_test_run();
}
