/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * POST /api/v1/lightsite/signups: Lightsite's daemon finishing a self
 * sign-up. A workspace administrator's service token asserts a verified
 * email; Venture creates or links the user, makes the business they own and
 * keeps a receipt, all in one transaction. Everything here goes through a
 * real hosted server and real bearer tokens, because the authority checks
 * live in the request path as much as in the service.
 *
 * What breaks if these regress: a retried sign-up makes a second user or a
 * second business; a failed step leaves an orphan; an ordinary member's
 * token -- or a browser session -- creates identities; an unknown member is
 * silently ignored; a deactivated person is revived by an email address.
 */
#include <venture.h>
#include <libsoup/soup.h>
#include <string.h>
#include "venture-test-accounting.h"
#include "venture-test-util.h"

#define WORKSPACE "8f062b79-1d2b-4d7f-99e5-bd3bf588e05a"
#define ORIGIN "https://example.test:8443"
#define AUTHORITY "example.test:8443"
#define ISSUER "https://id.example.test/realms/lightsite"
#define SIGNUPS "/api/v1/lightsite/signups"

typedef struct {
	VentureDatabase *db;
	VentureConfig *config;
	VentureContext *context;
	VentureWebServer *server;
	VentureTenantService *service;
	SoupSession *session;
	gchar *directory;
	gchar *admin_secret;
	gint64 admin_id;
	gint64 home;
} Fixture;

typedef struct { gboolean done; GBytes *body; GError *error; } Reply;

typedef struct {
	guint users;
	guint organizations;
	guint org_members;
	guint tenant_members;
	guint receipts;
} Counts;

static void
received(GObject *source, GAsyncResult *result, gpointer data)
{
	Reply *reply = data;
	reply->body = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &reply->error);
	reply->done = TRUE;
}

/* One exchange; the status is returned, the body handed back as text. */
static guint
exchange(Fixture *f, const gchar *method, const gchar *path, const gchar *host, const gchar *cookie,
	const gchar *token, const gchar *content_type, const gchar *payload, gchar **out_cookie, gchar **out_body,
	gboolean *no_store)
{
	g_autofree gchar *url = g_strconcat(venture_web_server_get_base_url(f->server), path, NULL);
	g_autoptr(SoupMessage) message = soup_message_new(method, url);
	SoupMessageHeaders *headers = soup_message_get_request_headers(message);
	Reply reply = { FALSE, NULL, NULL };
	const gchar *bytes, *cache;
	gsize length;
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	soup_message_headers_replace(headers, "Host", host ? host : AUTHORITY);
	if (cookie) soup_message_headers_replace(headers, "Cookie", cookie);
	if (token) {
		g_autofree gchar *header = g_strconcat("Bearer ", token, NULL);
		soup_message_headers_replace(headers, "Authorization", header);
	}
	if (payload) {
		g_autoptr(GBytes) body = g_bytes_new(payload, strlen(payload));
		soup_message_set_request_body_from_bytes(message, content_type, body);
	}
	soup_session_send_and_read_async(f->session, message, G_PRIORITY_DEFAULT, NULL, received, &reply);
	while (!reply.done) g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(reply.error);
	if (out_cookie) {
		const gchar *set = soup_message_headers_get_one(soup_message_get_response_headers(message), "Set-Cookie");
		*out_cookie = set ? g_strndup(set, strcspn(set, ";")) : NULL;
	}
	cache = soup_message_headers_get_one(soup_message_get_response_headers(message), "Cache-Control");
	if (no_store) *no_store = g_strcmp0(cache, "no-store") == 0;
	bytes = g_bytes_get_data(reply.body, &length);
	if (out_body) *out_body = g_strndup(bytes, length);
	g_bytes_unref(reply.body);
	return soup_message_get_status(message);
}

/* A sign-up POST. Every answer from this route, refusals included, must be
 * uncacheable: a cached 200 would replay one person's account to another. */
static JsonNode *
signup_full(Fixture *f, const gchar *token, const gchar *host, const gchar *payload, guint expected)
{
	g_autofree gchar *body = NULL;
	g_autoptr(GError) error = NULL;
	JsonNode *node;
	gboolean no_store = FALSE;
	guint status = exchange(f, "POST", SIGNUPS, host, NULL, token, "application/json", payload, NULL, &body, &no_store);
	if (status != expected) g_test_message("unexpected %u for %s: %s", status, payload, body);
	g_assert_cmpuint(status, ==, expected);
	g_assert_true(no_store);
	node = venture_json_parse(body, &error);
	g_assert_no_error(error); g_assert_nonnull(node);
	return node;
}

static JsonNode *
signup(Fixture *f, const gchar *payload, guint expected)
{
	return signup_full(f, f->admin_secret, NULL, payload, expected);
}

static gchar *
body_for(const gchar *key, const gchar *email, const gchar *subject, const gchar *business)
{
	return g_strdup_printf("{\"idempotency_key\":\"%s\",\"email\":\"%s\",\"issuer\":\"" ISSUER "\","
		"\"subject\":\"%s\",\"business_name\":\"%s\"}", key, email, subject, business);
}

static JsonNode *
get_json(Fixture *f, const gchar *path, const gchar *token, guint expected)
{
	g_autofree gchar *body = NULL;
	g_autoptr(GError) error = NULL;
	JsonNode *node;
	g_assert_cmpuint(exchange(f, "GET", path, NULL, NULL, token, NULL, NULL, NULL, &body, NULL), ==, expected);
	node = venture_json_parse(body, &error);
	g_assert_no_error(error);
	return node;
}

static void
sql(VentureDatabase *db, const gchar *statement)
{
	g_autoptr(GError) error = NULL;
	gboolean ok = venture_database_execute(db, statement, NULL, &error);
	g_assert_no_error(error); g_assert_true(ok);
}

static guint
count_type(VentureDatabase *db, GType type)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	if (type == G_TYPE_INVALID) return 0;
	query = venture_query_new(type);
	venture_query_set_include_deleted(query, TRUE);
	rows = venture_database_find(db, query, &error);
	g_assert_no_error(error); g_assert_nonnull(rows);
	return rows->len;
}

static GType
receipt_type(void)
{
	return venture_entity_registry_lookup(venture_entity_registry_get_default(), "tenant_signup");
}

static Counts
counts(Fixture *f, gboolean with_org_members)
{
	Counts c;
	c.users = count_type(f->db, VENTURE_TYPE_USER);
	c.organizations = count_type(f->db, VENTURE_TYPE_ORGANIZATION);
	c.org_members = with_org_members ? count_type(f->db, VENTURE_TYPE_ORGANIZATION_MEMBERSHIP) : 0;
	c.tenant_members = count_type(f->db, VENTURE_TYPE_TENANT_MEMBERSHIP);
	c.receipts = count_type(f->db, receipt_type());
	return c;
}

static void
assert_counts(Counts a, Counts b)
{
	g_assert_cmpuint(a.users, ==, b.users);
	g_assert_cmpuint(a.organizations, ==, b.organizations);
	g_assert_cmpuint(a.org_members, ==, b.org_members);
	g_assert_cmpuint(a.tenant_members, ==, b.tenant_members);
	g_assert_cmpuint(a.receipts, ==, b.receipts);
}

/* Tokens are minted the way the account-identity tests mint them: saved
 * directly, so the access policy captures the membership snapshot. */
static gchar *
mint(VentureDatabase *db, gint64 user_id, gint role)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureApiToken) token = venture_api_token_new();
	gchar *secret;
	g_object_set(token, "name", "Lightsite service", "user-id", user_id, "role", role, NULL);
	secret = venture_api_token_generate(token);
	g_assert_true(venture_database_save(db, VENTURE_ENTITY(token), NULL, &error)); g_assert_no_error(error);
	return secret;
}

/* What an accepted invitation produces, made under maintenance. */
static VentureEntity *
hosted_member(Fixture *f, const gchar *username, const gchar *email, gint64 organization, gint role, gboolean tenant)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureTenantMaintenance) maintenance = venture_tenant_service_enter_maintenance(f->service, "Provision a member", &error);
	g_autoptr(VentureEntity) tenant_member = NULL;
	g_autoptr(VentureEntity) org_member = NULL;
	VentureEntity *user = g_object_new(VENTURE_TYPE_USER, "username", username, "email", email, "organization-id", organization,
		"role", VENTURE_USER_ROLE_EDITOR, "active", TRUE, NULL);
	g_assert_no_error(error); g_assert_nonnull(maintenance);
	g_assert_true(venture_user_set_password(VENTURE_USER(user), "member-test-password", 2000, &error));
	{ gboolean saved = venture_database_save(f->db, user, NULL, &error); g_assert_no_error(error); g_assert_true(saved); }
	if (tenant) {
		tenant_member = g_object_new(VENTURE_TYPE_TENANT_MEMBERSHIP, "name", username,
			"user-id", venture_entity_get_id(user), "role", VENTURE_TENANT_ROLE_MEMBER, "active", TRUE, NULL);
		g_assert_true(venture_database_save(f->db, tenant_member, NULL, &error)); g_assert_no_error(error);
		org_member = g_object_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP, "organization-id", organization,
			"user-id", venture_entity_get_id(user), "role", role, "active", TRUE, NULL);
		g_assert_true(venture_database_save(f->db, org_member, NULL, &error)); g_assert_no_error(error);
	}
	g_assert_true(venture_tenant_maintenance_finish(maintenance, &error)); g_assert_no_error(error);
	return user;
}

static VentureEntity *
organization_member(Fixture *f, gint64 organization, gint64 user)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP);
	VentureEntity *member;
	venture_query_set_organization(query, organization);
	venture_query_add_filter_int(query, "user-id", VENTURE_FILTER_OP_EQ, user, NULL);
	member = venture_database_find_one(f->db, query, &error);
	g_assert_no_error(error);
	return member;
}

static guint
login_status(Fixture *f, const gchar *username, const gchar *password, gchar **cookie)
{
	g_autofree gchar *form = g_strdup_printf("username=%s&password=%s", username, password);
	return exchange(f, "POST", "/login", NULL, NULL, NULL, "application/x-www-form-urlencoded", form, cookie, NULL, NULL);
}

static void
setup(Fixture *f, gconstpointer data)
{
	g_autoptr(GInetAddress) loopback_address = g_inet_address_new_loopback(G_SOCKET_FAMILY_IPV4);
	g_autofree gchar *loopback = g_inet_address_to_string(loopback_address);
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_USER);
	g_autoptr(GPtrArray) users = NULL;
	g_autoptr(VentureEntity) home = g_object_new(VENTURE_TYPE_ORGANIZATION, "name", "Provider", "active", TRUE,
		"is-default", TRUE, NULL);
	gboolean ok;
	(void)data;
	f->db = venture_test_accounting_database(&error); g_assert_no_error(error);
	f->config = venture_config_new();
	f->directory = g_dir_make_tmp("venture-signup-XXXXXX", &error); g_assert_no_error(error);
	f->session = soup_session_new_with_options("timeout", 10, NULL);
	g_object_set(f->config, "hosted-enabled", TRUE, "hosted-workspace-id", WORKSPACE, "hosted-origin", ORIGIN,
		"security-password-iterations", (gint64)2000, "state-dir", f->directory,
		"server-bind-address", loopback, "server-port", (gint64)0, NULL);
	ok = venture_database_migrate(f->db, venture_entity_registry_get_default(), &error);
	g_assert_no_error(error); g_assert_true(ok);
	f->service = venture_tenant_service_get(f->db);
	g_assert_true(venture_tenant_service_configure(f->service, f->config, &error)); g_assert_no_error(error);
	g_assert_true(venture_tenant_service_initialize(f->service, &error)); g_assert_no_error(error);
	g_assert_true(venture_database_save(f->db, home, NULL, &error)); g_assert_no_error(error);
	f->home = venture_entity_get_id(home);
	g_assert_true(venture_tenant_service_bootstrap_admin(f->service, f->config, "provisioner", "private-test-password",
		FALSE, "Sign-up fixture", &error)); g_assert_no_error(error);
	users = venture_database_find(f->db, query, &error); g_assert_no_error(error);
	g_assert_cmpuint(users->len, ==, 1);
	f->admin_id = venture_entity_get_id(g_ptr_array_index(users, 0));
	f->admin_secret = mint(f->db, f->admin_id, VENTURE_USER_ROLE_EDITOR);
	f->context = venture_context_new(f->config, f->db);
	f->server = venture_web_server_new(f->context, &error); g_assert_no_error(error);
	g_assert_true(venture_web_server_start(f->server, &error)); g_assert_no_error(error);
}

static void
teardown(Fixture *f, gconstpointer data)
{
	(void)data;
	venture_web_server_stop(f->server);
	g_clear_object(&f->server);
	g_clear_object(&f->context);
	g_clear_object(&f->session);
	venture_test_accounting_database_cleanup(f->db);
	g_clear_object(&f->db);
	g_clear_object(&f->config);
	venture_test_remove_tree(f->directory);
	g_free(f->directory);
	g_free(f->admin_secret);
}

/* A new person: a passwordless active user, a business, an owner
 * membership, a receipt, and an ordinary account reference. */
static void
test_creates_owner(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *payload = body_for("signup:new-1", "new.owner@example.test", "kc-sub-1", "Example Co");
	g_autofree gchar *identity_path = NULL, *owner_secret = NULL, *password_hash = NULL, *username = NULL;
	g_autofree gchar *email = NULL, *name = NULL, *cookie = NULL;
	g_autoptr(JsonNode) result = NULL, identity = NULL;
	g_autoptr(VentureEntity) user = NULL, organization = NULL, member = NULL;
	JsonObject *object;
	Counts before = counts(f, TRUE), after;
	gint64 user_id, organization_id;
	gboolean active = FALSE;
	gint role = 0;
	(void)data;
	result = signup(f, payload, 201);
	object = json_node_get_object(result);
	g_assert_cmpuint(json_object_get_size(object), ==, 6);
	g_assert_cmpstr(json_object_get_string_member(object, "origin"), ==, ORIGIN);
	g_assert_cmpstr(json_object_get_string_member(object, "workspace_id"), ==, WORKSPACE);
	g_assert_false(json_object_get_boolean_member(object, "linked"));
	g_assert_cmpstr(json_object_get_string_member(object, "state"), ==, "active");
	organization_id = json_object_get_int_member(object, "organization_id");
	user_id = json_object_get_int_member(object, "user_id");
	g_assert_cmpint(organization_id, >, 0); g_assert_cmpint(user_id, >, 0);
	after = counts(f, TRUE);
	g_assert_cmpuint(after.users, ==, before.users + 1);
	g_assert_cmpuint(after.organizations, ==, before.organizations + 1);
	g_assert_cmpuint(after.org_members, ==, before.org_members + 1);
	g_assert_cmpuint(after.tenant_members, ==, before.tenant_members + 1);
	g_assert_cmpuint(after.receipts, ==, before.receipts + 1);

	user = venture_database_get(f->db, VENTURE_TYPE_USER, user_id, &error); g_assert_no_error(error);
	g_object_get(user, "username", &username, "email", &email, "password-hash", &password_hash,
		"active", &active, "role", &role, NULL);
	g_assert_cmpstr(username, ==, "new.owner");
	{
		/* A friendly name for screens and mail, never a raw ID. */
		g_autofree gchar *display = NULL;
		g_object_get(user, "display-name", &display, NULL);
		g_assert_cmpstr(display, ==, "new");
	}
	g_assert_cmpstr(email, ==, "new.owner@example.test");
	g_assert_true(venture_string_is_empty(password_hash));
	g_assert_true(active);
	g_assert_cmpint(role, ==, VENTURE_USER_ROLE_EDITOR);
	/* A member of the workspace, never its administrator. */
	g_assert_true(venture_tenant_service_is_member(f->service, user_id, FALSE));
	g_assert_false(venture_tenant_service_is_member(f->service, user_id, TRUE));

	organization = venture_database_get(f->db, VENTURE_TYPE_ORGANIZATION, organization_id, &error); g_assert_no_error(error);
	g_object_get(organization, "name", &name, "active", &active, NULL);
	g_assert_cmpstr(name, ==, "Example Co"); g_assert_true(active);
	member = organization_member(f, organization_id, user_id);
	g_assert_nonnull(member);
	g_object_get(member, "role", &role, "active", &active, NULL);
	g_assert_cmpint(role, ==, VENTURE_ORGANIZATION_ROLE_OWNER); g_assert_true(active);

	/* No password exists, so password sign-in cannot succeed. */
	g_assert_false(venture_user_check_password(VENTURE_USER(user), "anything"));
	g_assert_cmpuint(login_status(f, "new.owner", "anything", &cookie), !=, 302);

	/* The reference is an ordinary account: the owner's own credential
	 * validates it exactly like any other business. */
	owner_secret = mint(f->db, user_id, VENTURE_USER_ROLE_EDITOR);
	identity_path = g_strdup_printf("/api/v1/account-identity/%" G_GINT64_FORMAT, organization_id);
	identity = get_json(f, identity_path, owner_secret, 200);
	g_assert_cmpuint(json_object_get_size(json_node_get_object(identity)), ==, 4);
	g_assert_cmpstr(json_object_get_string_member(json_node_get_object(identity), "origin"), ==, ORIGIN);
	g_assert_cmpstr(json_object_get_string_member(json_node_get_object(identity), "workspace_id"), ==, WORKSPACE);
	g_assert_cmpint(json_object_get_int_member(json_node_get_object(identity), "organization_id"), ==, organization_id);
	g_assert_cmpstr(json_object_get_string_member(json_node_get_object(identity), "state"), ==, "active");
	/* The service token is not enlarged by the business it just made: its
	 * snapshot predates that organization, as for any later grant. */
	g_clear_pointer(&identity, json_node_unref);
	identity = get_json(f, identity_path, f->admin_secret, 403);
}

/* A known person, by a differently-cased address: linked, not duplicated. */
static void
test_links_existing(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) olive = hosted_member(f, "olive", "Olive.Owner@Example.Test", f->home, VENTURE_ORGANIZATION_ROLE_VIEWER, TRUE);
	g_autofree gchar *payload = body_for("signup:olive", "olive.owner@example.test", "kc-sub-olive", "Example Studio");
	g_autoptr(JsonNode) result = NULL;
	g_autoptr(VentureEntity) user = NULL, member = NULL;
	Counts before = counts(f, TRUE), after;
	gint64 organization_id;
	gint role = 0;
	gboolean active = FALSE;
	(void)data;
	result = signup(f, payload, 201);
	g_assert_true(json_object_get_boolean_member(json_node_get_object(result), "linked"));
	g_assert_cmpint(json_object_get_int_member(json_node_get_object(result), "user_id"), ==, venture_entity_get_id(olive));
	organization_id = json_object_get_int_member(json_node_get_object(result), "organization_id");
	after = counts(f, TRUE);
	g_assert_cmpuint(after.users, ==, before.users);
	g_assert_cmpuint(after.tenant_members, ==, before.tenant_members);
	g_assert_cmpuint(after.organizations, ==, before.organizations + 1);
	g_assert_cmpuint(after.org_members, ==, before.org_members + 1);
	g_assert_cmpuint(after.receipts, ==, before.receipts + 1);
	/* Linking changes nothing about the person: password, role, workspace
	 * membership and existing businesses stay as they were. */
	user = venture_database_get(f->db, VENTURE_TYPE_USER, venture_entity_get_id(olive), &error); g_assert_no_error(error);
	g_assert_true(venture_user_check_password(VENTURE_USER(user), "member-test-password"));
	g_assert_true(venture_tenant_service_is_member(f->service, venture_entity_get_id(olive), FALSE));
	g_assert_false(venture_tenant_service_is_member(f->service, venture_entity_get_id(olive), TRUE));
	member = organization_member(f, organization_id, venture_entity_get_id(olive));
	g_object_get(member, "role", &role, "active", &active, NULL);
	g_assert_cmpint(role, ==, VENTURE_ORGANIZATION_ROLE_OWNER); g_assert_true(active);
	g_clear_object(&member);
	member = organization_member(f, f->home, venture_entity_get_id(olive));
	g_object_get(member, "role", &role, NULL);
	g_assert_cmpint(role, ==, VENTURE_ORGANIZATION_ROLE_VIEWER);
}

/* A retry is answered from the receipt; a reused key with another body is
 * a conflict. Neither writes a row. */
static void
test_replay(Fixture *f, gconstpointer data)
{
	g_autofree gchar *payload = body_for("signup:replay", "replay@example.test", "kc-sub-replay", "Replay Co");
	g_autofree gchar *other_name = body_for("signup:replay", "replay@example.test", "kc-sub-replay", "Replay Company");
	g_autofree gchar *other_email = body_for("signup:replay", "other@example.test", "kc-sub-replay", "Replay Co");
	g_autofree gchar *other_subject = body_for("signup:replay", "replay@example.test", "kc-sub-other", "Replay Co");
	g_autofree gchar *first_text = NULL, *second_text = NULL;
	g_autoptr(JsonNode) first = NULL, second = NULL, conflict = NULL;
	Counts created;
	(void)data;
	first = signup(f, payload, 201);
	created = counts(f, TRUE);
	second = signup(f, payload, 200);
	first_text = venture_json_to_string(first, FALSE);
	second_text = venture_json_to_string(second, FALSE);
	g_assert_cmpstr(first_text, ==, second_text);
	assert_counts(counts(f, TRUE), created);
	conflict = signup(f, other_name, 409); g_clear_pointer(&conflict, json_node_unref);
	conflict = signup(f, other_email, 409); g_clear_pointer(&conflict, json_node_unref);
	conflict = signup(f, other_subject, 409); g_clear_pointer(&conflict, json_node_unref);
	assert_counts(counts(f, TRUE), created);
	/* Still answered from the receipt after the conflicts. */
	g_clear_pointer(&second, json_node_unref); g_clear_pointer(&second_text, g_free);
	second = signup(f, payload, 200);
	second_text = venture_json_to_string(second, FALSE);
	g_assert_cmpstr(first_text, ==, second_text);
	assert_counts(counts(f, TRUE), created);
}

/* An email is evidence of who someone is, never a way to revive them. */
static void
test_refuses_unavailable_identity(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) ivy = hosted_member(f, "ivy", "ivy@example.test", f->home, VENTURE_ORGANIZATION_ROLE_VIEWER, TRUE);
	g_autoptr(VentureEntity) dee = hosted_member(f, "dee", "dee@example.test", f->home, VENTURE_ORGANIZATION_ROLE_VIEWER, TRUE);
	g_autoptr(VentureEntity) ops = hosted_member(f, "ops", "ops@example.test", f->home, VENTURE_ORGANIZATION_ROLE_VIEWER, FALSE);
	g_autofree gchar *inactive = g_strdup_printf("UPDATE users SET active=FALSE WHERE id=%" G_GINT64_FORMAT, venture_entity_get_id(ivy));
	g_autofree gchar *deleted = g_strdup_printf("UPDATE users SET deleted_at='2026-01-01T00:00:00Z' WHERE id=%" G_GINT64_FORMAT, venture_entity_get_id(dee));
	g_autofree gchar *ivy_body = body_for("signup:ivy", "IVY@example.test", "kc-ivy", "Ivy Ltd");
	g_autofree gchar *dee_body = body_for("signup:dee", "dee@example.test", "kc-dee", "Dee Ltd");
	g_autofree gchar *ops_body = body_for("signup:ops", "ops@example.test", "kc-ops", "Ops Ltd");
	g_autoptr(JsonNode) result = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) stored = NULL;
	Counts before;
	gboolean active = TRUE;
	(void)data;
	sql(f->db, inactive);
	sql(f->db, deleted);
	before = counts(f, TRUE);
	result = signup(f, ivy_body, 409); g_clear_pointer(&result, json_node_unref);
	result = signup(f, dee_body, 409); g_clear_pointer(&result, json_node_unref);
	/* An identity outside the workspace membership -- a support operator,
	 * say -- cannot be turned into a customer by its address either. */
	result = signup(f, ops_body, 409); g_clear_pointer(&result, json_node_unref);
	assert_counts(counts(f, TRUE), before);
	stored = venture_database_get(f->db, VENTURE_TYPE_USER, venture_entity_get_id(ivy), &error); g_assert_no_error(error);
	g_object_get(stored, "active", &active, NULL);
	g_assert_false(active);
	g_assert_false(venture_tenant_service_is_member(f->service, venture_entity_get_id(ops), FALSE));
}

/* Only a workspace administrator's bearer token may sign people up; its
 * authority is checked now and as captured when the token was minted. */
static void
test_authority(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) mo = hosted_member(f, "mo", "mo@example.test", f->home, VENTURE_ORGANIZATION_ROLE_OWNER, TRUE);
	g_autofree gchar *payload = body_for("signup:authority", "authority@example.test", "kc-auth", "Authority Ltd");
	g_autofree gchar *member_secret = mint(f->db, venture_entity_get_id(mo), VENTURE_USER_ROLE_EDITOR);
	g_autofree gchar *viewer_secret = mint(f->db, f->admin_id, VENTURE_USER_ROLE_VIEWER);
	g_autofree gchar *unsnapshotted = mint(f->db, f->admin_id, VENTURE_USER_ROLE_EDITOR);
	g_autofree gchar *cookie = NULL, *body = NULL, *strip = NULL;
	g_autoptr(JsonNode) result = NULL;
	Counts before = counts(f, TRUE);
	(void)data;
	result = signup_full(f, NULL, NULL, payload, 401); g_clear_pointer(&result, json_node_unref);
	result = signup_full(f, "invalid", NULL, payload, 401); g_clear_pointer(&result, json_node_unref);
	/* A browser session is not a service credential. */
	g_assert_cmpuint(login_status(f, "provisioner", "private-test-password", &cookie), ==, 302);
	g_assert_nonnull(cookie);
	g_assert_cmpuint(exchange(f, "POST", SIGNUPS, NULL, cookie, NULL, "application/json", payload, NULL, &body, NULL), ==, 401);
	result = signup_full(f, member_secret, NULL, payload, 403); g_clear_pointer(&result, json_node_unref);
	result = signup_full(f, viewer_secret, NULL, payload, 403); g_clear_pointer(&result, json_node_unref);
	result = signup_full(f, f->admin_secret, "wrong.example", payload, 403); g_clear_pointer(&result, json_node_unref);
	/* The token's own record of workspace authority is required as well as
	 * the current membership. */
	strip = g_strdup_printf("UPDATE api_tokens SET membership_snapshot='{}' WHERE id=(SELECT MAX(id) FROM api_tokens)");
	sql(f->db, strip);
	result = signup_full(f, unsnapshotted, NULL, payload, 403); g_clear_pointer(&result, json_node_unref);
	sql(f->db, "UPDATE tenant_workspaces SET state='read_only'");
	result = signup(f, payload, 403); g_clear_pointer(&result, json_node_unref);
	sql(f->db, "UPDATE tenant_workspaces SET state='active'");
	sql(f->db, "UPDATE tenant_memberships SET role='member' WHERE name='provisioner'");
	result = signup(f, payload, 403); g_clear_pointer(&result, json_node_unref);
	sql(f->db, "UPDATE tenant_memberships SET role='admin' WHERE name='provisioner'");
	assert_counts(counts(f, TRUE), before);
	result = signup(f, payload, 201);
}

/* The body is a closed, bounded object. Every refusal writes nothing. */
static void
test_body(Fixture *f, gconstpointer data)
{
	static const gchar *const refused[] = {
		"not json",
		"[]",
		"{}",
		"\"text\"",
		"{\"idempotency_key\":\"k1\",\"email\":\"a@example.test\",\"issuer\":\"" ISSUER "\",\"subject\":\"s\"}",
		"{\"idempotency_key\":\"k1\",\"email\":\"a@example.test\",\"issuer\":\"" ISSUER "\",\"subject\":\"s\",\"business_name\":\"B\",\"role\":\"admin\"}",
		"{\"idempotency_key\":1,\"email\":\"a@example.test\",\"issuer\":\"" ISSUER "\",\"subject\":\"s\",\"business_name\":\"B\"}",
		"{\"idempotency_key\":\"k1\",\"email\":null,\"issuer\":\"" ISSUER "\",\"subject\":\"s\",\"business_name\":\"B\"}",
		"{\"idempotency_key\":\"k1\",\"email\":\"a@example.test\",\"issuer\":\"" ISSUER "\",\"subject\":\"s\",\"business_name\":{\"x\":1}}",
		"{\"idempotency_key\":\"\",\"email\":\"a@example.test\",\"issuer\":\"" ISSUER "\",\"subject\":\"s\",\"business_name\":\"B\"}",
		"{\"idempotency_key\":\"has space\",\"email\":\"a@example.test\",\"issuer\":\"" ISSUER "\",\"subject\":\"s\",\"business_name\":\"B\"}",
		"{\"idempotency_key\":\"k/1\",\"email\":\"a@example.test\",\"issuer\":\"" ISSUER "\",\"subject\":\"s\",\"business_name\":\"B\"}",
		"{\"idempotency_key\":\"k1\",\"email\":\"no-at.example.test\",\"issuer\":\"" ISSUER "\",\"subject\":\"s\",\"business_name\":\"B\"}",
		"{\"idempotency_key\":\"k1\",\"email\":\"a@b@example.test\",\"issuer\":\"" ISSUER "\",\"subject\":\"s\",\"business_name\":\"B\"}",
		"{\"idempotency_key\":\"k1\",\"email\":\"a b@example.test\",\"issuer\":\"" ISSUER "\",\"subject\":\"s\",\"business_name\":\"B\"}",
		"{\"idempotency_key\":\"k1\",\"email\":\"@example.test\",\"issuer\":\"" ISSUER "\",\"subject\":\"s\",\"business_name\":\"B\"}",
		"{\"idempotency_key\":\"k1\",\"email\":\"a@localhost\",\"issuer\":\"" ISSUER "\",\"subject\":\"s\",\"business_name\":\"B\"}",
		"{\"idempotency_key\":\"k1\",\"email\":\"a@example.test\",\"issuer\":\"http://id.example.test/realms/x\",\"subject\":\"s\",\"business_name\":\"B\"}",
		"{\"idempotency_key\":\"k1\",\"email\":\"a@example.test\",\"issuer\":\"https://u:p@id.example.test/realms/x\",\"subject\":\"s\",\"business_name\":\"B\"}",
		"{\"idempotency_key\":\"k1\",\"email\":\"a@example.test\",\"issuer\":\"https://id.example.test/realms/x?q=1\",\"subject\":\"s\",\"business_name\":\"B\"}",
		"{\"idempotency_key\":\"k1\",\"email\":\"a@example.test\",\"issuer\":\"https://id.example.test/realms/x#f\",\"subject\":\"s\",\"business_name\":\"B\"}",
		"{\"idempotency_key\":\"k1\",\"email\":\"a@example.test\",\"issuer\":\"not a url\",\"subject\":\"s\",\"business_name\":\"B\"}",
		"{\"idempotency_key\":\"k1\",\"email\":\"a@example.test\",\"issuer\":\"" ISSUER "\",\"subject\":\"\",\"business_name\":\"B\"}",
		"{\"idempotency_key\":\"k1\",\"email\":\"a@example.test\",\"issuer\":\"" ISSUER "\",\"subject\":\"s\\u0001\",\"business_name\":\"B\"}",
		"{\"idempotency_key\":\"k1\",\"email\":\"a@example.test\",\"issuer\":\"" ISSUER "\",\"subject\":\"s\",\"business_name\":\"\"}",
		"{\"idempotency_key\":\"k1\",\"email\":\"a@example.test\",\"issuer\":\"" ISSUER "\",\"subject\":\"s\",\"business_name\":\"Bad\\u0007Name\"}",
		"{\"idempotency_key\":\"k1\",\"email\":\"a@example.test\",\"issuer\":\"" ISSUER "\",\"subject\":\"s\",\"business_name\":\"Line\\nBreak\"}",
		"{\"idempotency_key\":\"k1\",\"email\":\"a@example.test\",\"issuer\":\"" ISSUER "\",\"subject\":\"s\",\"business_name\":\"   \"}",
	};
	g_autoptr(GString) long_key = g_string_new(NULL), long_subject = g_string_new(NULL);
	g_autoptr(GString) long_name = g_string_new(NULL), max_name = g_string_new(NULL), huge = g_string_new(NULL);
	g_autoptr(JsonNode) result = NULL;
	g_autofree gchar *payload = NULL;
	Counts before = counts(f, TRUE), after;
	guint i;
	(void)data;
	for (i = 0; i < G_N_ELEMENTS(refused); i++) {
		result = signup(f, refused[i], 422);
		g_clear_pointer(&result, json_node_unref);
	}
	for (i = 0; i < 129; i++) g_string_append_c(long_key, 'k');
	payload = body_for(long_key->str, "a@example.test", "s", "B");
	result = signup(f, payload, 422); g_clear_pointer(&result, json_node_unref); g_clear_pointer(&payload, g_free);
	for (i = 0; i < 256; i++) g_string_append_c(long_subject, 's');
	payload = body_for("k1", "a@example.test", long_subject->str, "B");
	result = signup(f, payload, 422); g_clear_pointer(&result, json_node_unref); g_clear_pointer(&payload, g_free);
	for (i = 0; i < 201; i++) g_string_append(long_name, "\xc3\xa9");
	payload = body_for("k1", "a@example.test", "s", long_name->str);
	result = signup(f, payload, 422); g_clear_pointer(&result, json_node_unref); g_clear_pointer(&payload, g_free);
	/* Larger than the 4096-byte bound, whatever it says. */
	for (i = 0; i < 4200; i++) g_string_append_c(huge, 'x');
	payload = body_for("k1", "a@example.test", "s", huge->str);
	result = signup(f, payload, 413); g_clear_pointer(&result, json_node_unref); g_clear_pointer(&payload, g_free);
	assert_counts(counts(f, TRUE), before);
	/* The bounds themselves are accepted: a 128-character key, 255-character
	 * subject and a 200-character (400-byte) name. */
	g_string_truncate(long_key, 128);
	g_string_truncate(long_subject, 255);
	for (i = 0; i < 200; i++) g_string_append(max_name, "\xc3\xa9");
	g_string_truncate(long_key, 120); g_string_append(long_key, "Az09._:-");
	payload = body_for(long_key->str, "bounds@example.test", long_subject->str, max_name->str);
	result = signup(f, payload, 201);
	after = counts(f, TRUE);
	g_assert_cmpuint(after.organizations, ==, before.organizations + 1);
}

/* A failure after the user and organization were written leaves none of
 * them, and the same request then succeeds: undone, and resumable. */
static void
test_rollback(Fixture *f, gconstpointer data)
{
	g_autofree gchar *payload = body_for("signup:rollback", "rollback@example.test", "kc-rb", "Rollback Ltd");
	g_autoptr(JsonNode) result = NULL;
	Counts before = counts(f, FALSE), after;
	(void)data;
	sql(f->db, "ALTER TABLE organization_memberships RENAME TO membership_unavailable");
	result = signup(f, payload, 500); g_clear_pointer(&result, json_node_unref);
	assert_counts(counts(f, FALSE), before);
	sql(f->db, "ALTER TABLE membership_unavailable RENAME TO organization_memberships");
	result = signup(f, payload, 201);
	after = counts(f, FALSE);
	g_assert_cmpuint(after.users, ==, before.users + 1);
	g_assert_cmpuint(after.organizations, ==, before.organizations + 1);
	g_assert_cmpuint(after.receipts, ==, before.receipts + 1);
}

/* Usernames come from the address's local part, made valid and unique. */
static void
test_usernames(Fixture *f, gconstpointer data)
{
	static const struct { const gchar *email; const gchar *username; } cases[] = {
		{ "sam.plumbing@zero.example", "sam.plumbing" },
	{ "sam@one.example", "sam" },
		{ "SAM@two.example", "sam-2" },
		{ "!!!@three.example", "user" },
		{ "Jo+Shop@four.example", "joshop" },
	};
	guint i;
	(void)data;
	for (i = 0; i < G_N_ELEMENTS(cases); i++) {
		g_autoptr(GError) error = NULL;
		g_autofree gchar *key = g_strdup_printf("signup:user-%u", i);
		g_autofree gchar *payload = body_for(key, cases[i].email, key, "Named Ltd");
		g_autoptr(JsonNode) result = signup(f, payload, 201);
		g_autoptr(VentureEntity) user = venture_database_get(f->db, VENTURE_TYPE_USER,
			json_object_get_int_member(json_node_get_object(result), "user_id"), &error);
		g_autofree gchar *username = NULL;
		g_assert_no_error(error);
		g_object_get(user, "username", &username, NULL);
		g_assert_cmpstr(username, ==, cases[i].username);
	}
}

/* What a passwordless owner can do today: nothing interactive until an
 * administrator runs the existing targeted recovery, which sets the first
 * password. Ownership survives it. */
static void
test_passwordless_recovery(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *payload = body_for("signup:recover", "recover@example.test", "kc-rec", "Recovered Ltd");
	g_autofree gchar *capability = NULL, *cookie = NULL;
	g_autoptr(JsonNode) result = signup(f, payload, 201);
	g_autoptr(VentureTenantInvitation) invitation = NULL;
	g_autoptr(VentureUser) recovered = NULL;
	g_autoptr(VentureEntity) member = NULL;
	gint64 user_id = json_object_get_int_member(json_node_get_object(result), "user_id");
	gint64 organization_id = json_object_get_int_member(json_node_get_object(result), "organization_id");
	VentureAuthPrincipal administrator;
	gint role = 0;
	gboolean active = FALSE;
	(void)data;
	administrator.user_id = f->admin_id;
	administrator.token_id = 0;
	administrator.role = VENTURE_USER_ROLE_EDITOR;
	administrator.name = (gchar *)"provisioner";
	administrator.authenticated = TRUE;
	{
		g_autoptr(VentureAccessScope) scope = venture_access_policy_enter(venture_database_get_access_policy(f->db), &administrator);
		/* Activation with no password is the quarantine rule, unchanged. */
		g_assert_true(venture_tenant_service_set_membership(f->service, user_id, "member", FALSE, "Prepare first password", &error));
		g_assert_no_error(error);
		g_assert_false(venture_tenant_service_set_membership(f->service, user_id, "member", TRUE, "No shortcut", &error));
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED); g_clear_error(&error);
		invitation = venture_tenant_service_invite_recovery(f->service, user_id, 600, "First password for a signed-up owner", &error);
		g_assert_no_error(error); g_assert_nonnull(invitation);
		g_object_get(invitation, "capability", &capability, NULL);
	}
	recovered = venture_tenant_service_accept_invitation(f->service, f->config, capability, "recover", "owner-chosen-password", &error);
	g_assert_no_error(error); g_assert_nonnull(recovered);
	g_assert_cmpint(venture_entity_get_id(VENTURE_ENTITY(recovered)), ==, user_id);
	g_assert_true(venture_user_check_password(recovered, "owner-chosen-password"));
	member = organization_member(f, organization_id, user_id);
	g_object_get(member, "role", &role, "active", &active, NULL);
	g_assert_cmpint(role, ==, VENTURE_ORGANIZATION_ROLE_OWNER); g_assert_true(active);
	g_assert_cmpuint(login_status(f, "recover", "owner-chosen-password", &cookie), ==, 302);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/lightsite-signup/creates-owner", Fixture, NULL, setup, test_creates_owner, teardown);
	g_test_add("/lightsite-signup/links-existing", Fixture, NULL, setup, test_links_existing, teardown);
	g_test_add("/lightsite-signup/replay", Fixture, NULL, setup, test_replay, teardown);
	g_test_add("/lightsite-signup/refuses-unavailable-identity", Fixture, NULL, setup, test_refuses_unavailable_identity, teardown);
	g_test_add("/lightsite-signup/authority", Fixture, NULL, setup, test_authority, teardown);
	g_test_add("/lightsite-signup/body", Fixture, NULL, setup, test_body, teardown);
	g_test_add("/lightsite-signup/rollback", Fixture, NULL, setup, test_rollback, teardown);
	g_test_add("/lightsite-signup/usernames", Fixture, NULL, setup, test_usernames, teardown);
	g_test_add("/lightsite-signup/passwordless-recovery", Fixture, NULL, setup, test_passwordless_recovery, teardown);
	return g_test_run();
}
