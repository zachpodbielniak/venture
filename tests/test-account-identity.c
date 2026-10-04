/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <libsoup/soup.h>
#include <string.h>
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
/* One exchange with an optional session cookie, bearer token and JSON body;
 * the reply's status is the point, so the body is handed back unparsed. */
static guint exchange(SoupSession *session, const gchar *base, const gchar *method, const gchar *path,
	const gchar *cookie, const gchar *token, const gchar *content_type, const gchar *payload,
	gchar **out_cookie, gchar **out_body)
{
	g_autofree gchar *url = g_strconcat(base, path, NULL);
	g_autoptr(SoupMessage) message = soup_message_new(method, url);
	SoupMessageHeaders *headers = soup_message_get_request_headers(message);
	Reply reply = { FALSE, NULL, NULL };
	gsize length;
	const gchar *bytes;
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	soup_message_headers_replace(headers, "Host", "127.0.0.1:8443");
	if (cookie) soup_message_headers_replace(headers, "Cookie", cookie);
	if (token) {
		g_autofree gchar *header = g_strconcat("Bearer ", token, NULL);
		soup_message_headers_replace(headers, "Authorization", header);
	}
	if (payload) {
		g_autoptr(GBytes) body = g_bytes_new(payload, strlen(payload));
		soup_message_set_request_body_from_bytes(message, content_type, body);
	}
	soup_session_send_and_read_async(session, message, G_PRIORITY_DEFAULT, NULL, received, &reply);
	while (!reply.done) g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(reply.error);
	if (out_cookie) {
		const gchar *set = soup_message_headers_get_one(soup_message_get_response_headers(message), "Set-Cookie");
		*out_cookie = set ? g_strndup(set, strcspn(set, ";")) : NULL;
	}
	bytes = g_bytes_get_data(reply.body, &length);
	if (out_body) *out_body = g_strndup(bytes, length);
	g_bytes_unref(reply.body);
	return soup_message_get_status(message);
}
/* Days from now until a stored token expires; it must expire. */
static gdouble token_days_left(VentureDatabase *db, gint64 id)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) stored = venture_database_get(db, VENTURE_TYPE_API_TOKEN, id, &error);
	g_autoptr(GDateTime) expires = NULL;
	g_autoptr(GDateTime) now = g_date_time_new_now_utc();
	g_assert_no_error(error); g_assert_nonnull(stored);
	g_object_get(stored, "expires-at", &expires, NULL);
	g_assert_nonnull(expires);
	return (gdouble)g_date_time_difference(expires, now) / (gdouble)G_TIME_SPAN_DAY;
}
/* Lightsite is told to use the signed-in owner's own bearer token, so an
 * ordinary hosted member must be able to mint one from a browser session.
 * The token is theirs: their role, their memberships at the moment of
 * minting, and nothing a token can use to mint another. */
static void member_mint_http(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureDatabase) db = venture_test_accounting_database(&error);
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureContext) context = NULL;
	g_autoptr(VentureWebServer) server = NULL;
	g_autoptr(VentureEntity) org = g_object_new(VENTURE_TYPE_ORGANIZATION, "name", "Member account", "active", TRUE, NULL);
	g_autoptr(VentureEntity) other = g_object_new(VENTURE_TYPE_ORGANIZATION, "name", "Somebody else", "active", TRUE, NULL);
	g_autoptr(VentureEntity) user = g_object_new(VENTURE_TYPE_USER, "username", "site-owner",
		"role", VENTURE_USER_ROLE_EDITOR, "active", TRUE, NULL);
	g_autoptr(VentureEntity) tenant_member = NULL;
	g_autoptr(VentureEntity) org_member = NULL;
	g_autoptr(SoupSession) session = soup_session_new_with_options("timeout", 10, NULL);
	g_autoptr(JsonNode) minted = NULL;
	g_autoptr(JsonNode) result = NULL;
	g_autofree gchar *directory = g_dir_make_tmp("venture-member-XXXXXX", &error);
	g_autofree gchar *cookie = NULL, *body = NULL, *secret = NULL;
	const gchar *base;
	VentureTenantService *service;
	gboolean ok;
	g_assert_no_error(error);
	g_object_set(config, "hosted-enabled", TRUE, "hosted-workspace-id", "8f062b79-1d2b-4d7f-99e5-bd3bf588e05a",
		"hosted-origin", "http://127.0.0.1:8443", "security-password-iterations", (gint64)2000,
		"state-dir", directory, "server-bind-address", "127.0.0.1", "server-port", (gint64)0, NULL);
	ok = venture_database_migrate(db, venture_entity_registry_get_default(), &error);
	g_assert_no_error(error); g_assert_true(ok);
	service = venture_tenant_service_get(db);
	g_assert_true(venture_tenant_service_configure(service, config, &error)); g_assert_no_error(error);
	g_assert_true(venture_tenant_service_initialize(service, &error)); g_assert_no_error(error);
	g_assert_true(venture_tenant_service_bootstrap_admin(service, config, "provisioner", "private-test-password",
		FALSE, "Member fixture", &error)); g_assert_no_error(error);
	g_assert_true(venture_database_save(db, org, NULL, &error)); g_assert_no_error(error);
	g_assert_true(venture_database_save(db, other, NULL, &error)); g_assert_no_error(error);
	g_assert_true(venture_user_set_password(VENTURE_USER(user), "member-test-password", 2000, &error));
	{
		/* Accounts are made under maintenance in a hosted workspace; an
		 * invitation is the production path, and this is its outcome. */
		g_autoptr(VentureTenantMaintenance) maintenance = venture_tenant_service_enter_maintenance(service, "Provision a site owner", &error);
		g_assert_no_error(error); g_assert_nonnull(maintenance);
		g_assert_true(venture_database_save(db, user, NULL, &error)); g_assert_no_error(error);
		tenant_member = g_object_new(VENTURE_TYPE_TENANT_MEMBERSHIP, "name", "site-owner",
			"user-id", venture_entity_get_id(user), "role", VENTURE_TENANT_ROLE_MEMBER, "active", TRUE, NULL);
		g_assert_true(venture_database_save(db, tenant_member, NULL, &error)); g_assert_no_error(error);
		org_member = g_object_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP, "organization-id", venture_entity_get_id(org),
			"user-id", venture_entity_get_id(user), "role", VENTURE_ORGANIZATION_ROLE_ADMIN, "active", TRUE, NULL);
		g_assert_true(venture_database_save(db, org_member, NULL, &error)); g_assert_no_error(error);
		g_assert_true(venture_tenant_maintenance_finish(maintenance, &error)); g_assert_no_error(error);
	}
	context = venture_context_new(config, db);
	server = venture_web_server_new(context, &error); g_assert_no_error(error);
	g_assert_true(venture_web_server_start(server, &error)); g_assert_no_error(error);
	base = venture_web_server_get_base_url(server);
	g_assert_cmpuint(exchange(session, base, "POST", "/login", NULL, NULL, "application/x-www-form-urlencoded",
		"username=site-owner&password=member-test-password", &cookie, NULL), ==, 302);
	g_assert_nonnull(cookie);
	g_assert_cmpuint(exchange(session, base, "POST", "/api/v1/tokens", NULL, NULL, "application/json",
		"{\"name\":\"Lightsite\"}", NULL, NULL), ==, 401);
	g_assert_cmpuint(exchange(session, base, "POST", "/api/v1/tokens", cookie, NULL, "application/json",
		"{\"name\":\"Lightsite\"}", NULL, &body), ==, 201);
	minted = venture_json_parse(body, &error); g_assert_no_error(error);
	secret = g_strdup(json_object_get_string_member(json_node_get_object(minted), "token"));
	g_assert_nonnull(secret);
	result = request(session, base, "/api/v1/account-authority", secret, "127.0.0.1:8443", 200);
	g_assert_cmpint(json_object_get_int_member(json_node_get_object(result), "user_id"), ==, venture_entity_get_id(user));
	authority_entry(result, venture_entity_get_id(org), "admin", TRUE);
	authority_entry(result, venture_entity_get_id(other), NULL, FALSE);
	{
		g_autoptr(VentureApiToken) stored = VENTURE_API_TOKEN(venture_database_get(db, VENTURE_TYPE_API_TOKEN,
			json_object_get_int_member(json_node_get_object(minted), "id"), &error));
		gint64 owner = 0;
		gint role = 0;
		g_assert_no_error(error); g_assert_nonnull(stored);
		g_object_get(stored, "user-id", &owner, "role", &role, NULL);
		g_assert_cmpint(owner, ==, venture_entity_get_id(user));
		g_assert_cmpint(role, ==, VENTURE_USER_ROLE_EDITOR);
	}
	/* A bearer token never mints another one for a non-administrator. */
	g_assert_cmpuint(exchange(session, base, "POST", "/api/v1/tokens", NULL, secret, "application/json",
		"{\"name\":\"Second\"}", NULL, NULL), ==, 403);
	/* A member's token is never open-ended: thirty days unless asked for
	 * fewer, at most ninety. A stolen session must not become a credential
	 * that outlives every sign-out and password change. */
	g_assert_cmpfloat(token_days_left(db, json_object_get_int_member(json_node_get_object(minted), "id")), >, 29.9);
	g_assert_cmpfloat(token_days_left(db, json_object_get_int_member(json_node_get_object(minted), "id")), <=, 30.0);
	{
		g_autoptr(JsonNode) week = NULL;
		g_clear_pointer(&body, g_free);
		g_assert_cmpuint(exchange(session, base, "POST", "/api/v1/tokens", cookie, NULL, "application/json",
			"{\"name\":\"Week\",\"expires_in_days\":7}", NULL, &body), ==, 201);
		week = venture_json_parse(body, &error); g_assert_no_error(error);
		g_assert_cmpfloat(token_days_left(db, json_object_get_int_member(json_node_get_object(week), "id")), >, 6.9);
		g_assert_cmpfloat(token_days_left(db, json_object_get_int_member(json_node_get_object(week), "id")), <=, 7.0);
	}
	{
		const gchar *refused[] = { "0", "91", "-1", "\"7\"", "7.5", "null" };
		guint i;
		for (i = 0; i < G_N_ELEMENTS(refused); i++) {
			g_autofree gchar *payload = g_strdup_printf("{\"name\":\"Bad\",\"expires_in_days\":%s}", refused[i]);
			g_assert_cmpuint(exchange(session, base, "POST", "/api/v1/tokens", cookie, NULL, "application/json",
				payload, NULL, NULL), ==, 422);
		}
	}
	{
		g_autofree gchar *expire = g_strdup_printf("UPDATE api_tokens SET expires_at='2000-01-01T00:00:00Z' WHERE id=%" G_GINT64_FORMAT,
			json_object_get_int_member(json_node_get_object(minted), "id"));
		sql(db, expire);
		g_clear_pointer(&result, json_node_unref);
		result = request(session, base, "/api/v1/account-authority", secret, "127.0.0.1:8443", 401);
	}
	/* Lifecycle still governs the session: no minting from a read-only workspace. */
	sql(db, "UPDATE tenant_workspaces SET state='read_only'");
	g_assert_cmpuint(exchange(session, base, "POST", "/api/v1/tokens", cookie, NULL, "application/json",
		"{\"name\":\"Frozen\"}", NULL, NULL), ==, 403);
	sql(db, "UPDATE tenant_workspaces SET state='active'");
	venture_web_server_stop(server); g_clear_object(&server); g_clear_object(&context);
	venture_test_accounting_database_cleanup(db);
	venture_test_remove_tree(directory);
}
/* A global editor with a tenant membership and one organization role: what
 * an invitation produces. Made under maintenance, as in the mint test. */
static VentureEntity *hosted_member(VentureDatabase *db, VentureTenantService *service,
	const gchar *username, gint64 organization, gint role)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureTenantMaintenance) maintenance = venture_tenant_service_enter_maintenance(service, "Provision a member", &error);
	g_autoptr(VentureEntity) tenant_member = NULL;
	g_autoptr(VentureEntity) org_member = NULL;
	VentureEntity *user = g_object_new(VENTURE_TYPE_USER, "username", username,
		"role", VENTURE_USER_ROLE_EDITOR, "active", TRUE, NULL);
	g_assert_no_error(error); g_assert_nonnull(maintenance);
	g_assert_true(venture_user_set_password(VENTURE_USER(user), "member-test-password", 2000, &error));
	g_assert_true(venture_database_save(db, user, NULL, &error)); g_assert_no_error(error);
	tenant_member = g_object_new(VENTURE_TYPE_TENANT_MEMBERSHIP, "name", username,
		"user-id", venture_entity_get_id(user), "role", VENTURE_TENANT_ROLE_MEMBER, "active", TRUE, NULL);
	g_assert_true(venture_database_save(db, tenant_member, NULL, &error)); g_assert_no_error(error);
	org_member = g_object_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP, "organization-id", organization,
		"user-id", venture_entity_get_id(user), "role", role, "active", TRUE, NULL);
	g_assert_true(venture_database_save(db, org_member, NULL, &error)); g_assert_no_error(error);
	g_assert_true(venture_tenant_maintenance_finish(maintenance, &error)); g_assert_no_error(error);
	return user;
}
static gchar *sign_in(SoupSession *session, const gchar *base, const gchar *username, const gchar *password)
{
	g_autofree gchar *form = g_strdup_printf("username=%s&password=%s", username, password);
	gchar *cookie = NULL;
	g_assert_cmpuint(exchange(session, base, "POST", "/login", NULL, NULL, "application/x-www-form-urlencoded",
		form, &cookie, NULL), ==, 302);
	g_assert_nonnull(cookie);
	return cookie;
}
static gchar *organization_text(VentureDatabase *db, gint64 id, const gchar *property)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) stored = venture_database_get(db, VENTURE_TYPE_ORGANIZATION, id, &error);
	gchar *value = NULL;
	g_assert_no_error(error); g_assert_nonnull(stored);
	g_object_get(stored, property, &value, NULL);
	return value;
}
/* A workspace administrator creates a customer's organization; its owner
 * then keeps the business profile -- phone, email, address -- up to date.
 * An organization is not filed against another one, so creating it must
 * not stamp the creator's current organization on the row, and the owner's
 * write must be judged in the organization the row *is*. The owner gets no
 * further: not another organization, not the hierarchy, not the default. */
static void owner_edits_own_organization_http(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureDatabase) db = venture_test_accounting_database(&error);
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureContext) context = NULL;
	g_autoptr(VentureWebServer) server = NULL;
	g_autoptr(VentureEntity) home = g_object_new(VENTURE_TYPE_ORGANIZATION, "name", "Provider", "active", TRUE, "is-default", TRUE, NULL);
	g_autoptr(VentureEntity) owner = NULL;
	g_autoptr(VentureEntity) viewer = NULL;
	g_autoptr(VentureEntity) stored = NULL;
	g_autoptr(SoupSession) session = soup_session_new_with_options("timeout", 10, NULL);
	g_autoptr(JsonNode) created = NULL;
	g_autofree gchar *directory = g_dir_make_tmp("venture-org-profile-XXXXXX", &error);
	g_autofree gchar *admin_cookie = NULL, *owner_cookie = NULL, *viewer_cookie = NULL, *body = NULL;
	g_autofree gchar *path = NULL, *home_path = NULL, *value = NULL;
	gint64 bakery;
	gboolean is_default = FALSE;
	const gchar *base;
	VentureTenantService *service;
	gboolean ok;
	g_assert_no_error(error);
	g_object_set(config, "hosted-enabled", TRUE, "hosted-workspace-id", "8f062b79-1d2b-4d7f-99e5-bd3bf588e05a",
		"hosted-origin", "http://127.0.0.1:8443", "security-password-iterations", (gint64)2000,
		"state-dir", directory, "server-bind-address", "127.0.0.1", "server-port", (gint64)0, NULL);
	ok = venture_database_migrate(db, venture_entity_registry_get_default(), &error);
	g_assert_no_error(error); g_assert_true(ok);
	service = venture_tenant_service_get(db);
	g_assert_true(venture_tenant_service_configure(service, config, &error)); g_assert_no_error(error);
	g_assert_true(venture_tenant_service_initialize(service, &error)); g_assert_no_error(error);
	g_assert_true(venture_database_save(db, home, NULL, &error)); g_assert_no_error(error);
	g_assert_true(venture_tenant_service_bootstrap_admin(service, config, "provisioner", "private-test-password",
		FALSE, "Profile fixture", &error)); g_assert_no_error(error);
	context = venture_context_new(config, db);
	/* As at a server's start: the workspace's default organization is known. */
	g_assert_cmpint(venture_context_get_default_organization_id(context), ==, venture_entity_get_id(home));
	server = venture_web_server_new(context, &error); g_assert_no_error(error);
	g_assert_true(venture_web_server_start(server, &error)); g_assert_no_error(error);
	base = venture_web_server_get_base_url(server);
	admin_cookie = sign_in(session, base, "provisioner", "private-test-password");
	g_assert_cmpuint(exchange(session, base, "POST", "/api/v1/organization", admin_cookie, NULL, "application/json",
		"{\"name\":\"Bea's Bakery\",\"active\":true}", NULL, &body), ==, 201);
	created = venture_json_parse(body, &error); g_assert_no_error(error);
	bakery = json_object_get_int_member(json_node_get_object(created), "id");
	g_assert_cmpint(bakery, >, venture_entity_get_id(home));
	stored = venture_database_get(db, VENTURE_TYPE_ORGANIZATION, bakery, &error);
	g_assert_no_error(error); g_assert_nonnull(stored);
	g_assert_cmpint(venture_entity_get_organization_id(stored), ==, 0);
	owner = hosted_member(db, service, "bea", bakery, VENTURE_ORGANIZATION_ROLE_OWNER);
	viewer = hosted_member(db, service, "val", bakery, VENTURE_ORGANIZATION_ROLE_VIEWER);
	owner_cookie = sign_in(session, base, "bea", "member-test-password");
	viewer_cookie = sign_in(session, base, "val", "member-test-password");
	path = g_strdup_printf("/api/v1/organization/%" G_GINT64_FORMAT, bakery);
	home_path = g_strdup_printf("/api/v1/organization/%" G_GINT64_FORMAT, venture_entity_get_id(home));
	g_assert_cmpuint(exchange(session, base, "GET", path, owner_cookie, NULL, NULL, NULL, NULL, NULL), ==, 200);
	g_assert_cmpuint(exchange(session, base, "PATCH", path, owner_cookie, NULL, "application/json",
		"{\"phone\":\"+1 555 0100\",\"email\":\"hello@bea.example\",\"address\":\"1 Oven Lane\"}", NULL, NULL), ==, 200);
	value = organization_text(db, bakery, "phone"); g_assert_cmpstr(value, ==, "+1 555 0100"); g_clear_pointer(&value, g_free);
	value = organization_text(db, bakery, "email"); g_assert_cmpstr(value, ==, "hello@bea.example"); g_clear_pointer(&value, g_free);
	/* Rows already created with the creator's organization stamped on them
	 * are still their own organization's to edit. */
	{
		g_autofree gchar *stamp = g_strdup_printf("UPDATE organizations SET organization_id=%" G_GINT64_FORMAT
			" WHERE id=%" G_GINT64_FORMAT, venture_entity_get_id(home), bakery);
		sql(db, stamp);
	}
	g_assert_cmpuint(exchange(session, base, "PATCH", path, owner_cookie, NULL, "application/json",
		"{\"name\":\"Bea's Bakery & Cafe\"}", NULL, NULL), ==, 200);
	value = organization_text(db, bakery, "name"); g_assert_cmpstr(value, ==, "Bea's Bakery & Cafe"); g_clear_pointer(&value, g_free);
	/* Isolation: somebody else's organization stays invisible to her. */
	g_assert_cmpuint(exchange(session, base, "GET", home_path, owner_cookie, NULL, NULL, NULL, NULL, NULL), ==, 404);
	g_assert_cmpuint(exchange(session, base, "PATCH", home_path, owner_cookie, NULL, "application/json",
		"{\"phone\":\"+1 555 0199\"}", NULL, NULL), ==, 404);
	value = organization_text(db, venture_entity_get_id(home), "phone"); g_assert_null(value);
	/* The hierarchy and the default organization decide where other people's
	 * records land and what reports roll up: workspace authority, not hers. */
	{
		g_autofree gchar *adopt = g_strdup_printf("{\"parent_id\":%" G_GINT64_FORMAT "}", venture_entity_get_id(home));
		g_assert_cmpuint(exchange(session, base, "PATCH", path, owner_cookie, NULL, "application/json",
			adopt, NULL, NULL), ==, 403);
	}
	g_assert_cmpuint(exchange(session, base, "PATCH", path, owner_cookie, NULL, "application/json",
		"{\"is_default\":true}", NULL, NULL), ==, 403);
	g_clear_object(&stored);
	stored = venture_database_get(db, VENTURE_TYPE_ORGANIZATION, bakery, &error);
	g_assert_no_error(error); g_assert_nonnull(stored);
	{
		gint64 parent = -1;
		g_object_get(stored, "parent-id", &parent, "is-default", &is_default, NULL);
		g_assert_cmpint(parent, ==, 0);
		g_assert_false(is_default);
	}
	/* A viewer of the organization reads its profile and cannot change it. */
	g_assert_cmpuint(exchange(session, base, "GET", path, viewer_cookie, NULL, NULL, NULL, NULL, NULL), ==, 200);
	g_assert_cmpuint(exchange(session, base, "PATCH", path, viewer_cookie, NULL, "application/json",
		"{\"phone\":\"+1 555 0000\"}", NULL, NULL), ==, 403);
	value = organization_text(db, bakery, "phone"); g_assert_cmpstr(value, ==, "+1 555 0100"); g_clear_pointer(&value, g_free);
	/* The workspace administrator still manages the hierarchy. */
	{
		g_autofree gchar *adopt = g_strdup_printf("{\"parent_id\":%" G_GINT64_FORMAT "}", venture_entity_get_id(home));
		g_assert_cmpuint(exchange(session, base, "PATCH", path, admin_cookie, NULL, "application/json",
			adopt, NULL, NULL), ==, 200);
	}
	/* Her own token, for Lightsite: it belongs to her organization, not to the
	 * workspace's default one she cannot see, and manages her bakery. */
	{
		g_autofree gchar *minted = NULL, *authority = NULL;
		g_autoptr(JsonNode) token = NULL, answer = NULL;
		g_assert_cmpuint(exchange(session, base, "POST", "/api/v1/tokens", owner_cookie, NULL, "application/json",
			"{\"name\":\"lightsite\"}", NULL, &minted), ==, 201);
		token = venture_json_parse(minted, &error); g_assert_no_error(error);
		g_assert_cmpuint(exchange(session, base, "GET", "/api/v1/account-authority", NULL,
			json_object_get_string_member(json_node_get_object(token), "token"), NULL, NULL, NULL, &authority), ==, 200);
		answer = venture_json_parse(authority, &error); g_assert_no_error(error);
		authority_entry(answer, bakery, "owner", TRUE);
	}
	venture_web_server_stop(server); g_clear_object(&server); g_clear_object(&context);
	venture_test_accounting_database_cleanup(db);
	venture_test_remove_tree(directory);
}
int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/account-identity/http-authority", identity_http);
	g_test_add_func("/account-identity/member-mints-own-token", member_mint_http);
	g_test_add_func("/account-identity/owner-edits-own-organization", owner_edits_own_organization_http);
	return g_test_run();
}
