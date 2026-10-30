/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* A business owner invites their own staff. Without this an owner could
 * not add a groomer or an office manager at all: inviting needed a
 * workspace administrator, and the owner is not one. What must hold: only
 * the owner of that business may invite to it, the mail is the business's
 * own, a retry sends nothing twice, and a revoked or superseded link, or
 * one from an owner who is no longer the owner, never lets anybody in. */
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
	gchar *directory, *token, *admin_token;
	gint64 business, owner, stranger_business, stranger;
} Fixture;
typedef struct { gboolean done; GBytes *bytes; GError *error; } Reply;

static void received(GObject *source, GAsyncResult *result, gpointer data)
{
	Reply *reply = data;
	reply->bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &reply->error);
	reply->done = TRUE;
}

static JsonNode *call(Fixture *f, const gchar *path, const gchar *body, guint expected)
{
	g_autofree gchar *url = g_strconcat(venture_web_server_get_base_url(f->server), path, NULL);
	g_autofree gchar *auth = g_strconcat("Bearer ", f->token, NULL);
	g_autoptr(SoupMessage) message = soup_message_new(body ? "POST" : "GET", url);
	g_autoptr(GBytes) payload = body ? g_bytes_new(body, strlen(body)) : NULL;
	g_autofree gchar *terminated = NULL;
	Reply reply = { FALSE, NULL, NULL };
	JsonNode *answer;
	const gchar *text;
	gsize length;
	soup_message_headers_replace(soup_message_get_request_headers(message), "Host", "example.test");
	soup_message_headers_replace(soup_message_get_request_headers(message), "Authorization", auth);
	if (payload) soup_message_set_request_body_from_bytes(message, "application/json", payload);
	soup_session_send_and_read_async(f->session, message, G_PRIORITY_DEFAULT, NULL, received, &reply);
	while (!reply.done) g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(reply.error);
	text = g_bytes_get_data(reply.bytes, &length);
	if (soup_message_get_status(message) != expected) g_test_message("Response for %s: %.*s", path, (gint)length, text);
	g_assert_cmpuint(soup_message_get_status(message), ==, expected);
	g_assert_cmpstr(soup_message_headers_get_one(soup_message_get_response_headers(message), "Cache-Control"), ==, "no-store");
	terminated = g_strndup(text, length);
	answer = venture_json_parse(terminated, NULL);
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
	g_object_set(token, "name", "Invitation fixture", "user-id", user, "role", role, NULL);
	secret = venture_api_token_generate(token); save(f, VENTURE_ENTITY(token));
	return secret;
}

static void use_token(Fixture *f, gchar *token)
{
	g_free(f->token); f->token = token;
}

static void setup(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GInetAddress) address = g_inet_address_new_loopback(G_SOCKET_FAMILY_IPV4);
	g_autofree gchar *bind = g_inet_address_to_string(address);
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_USER);
	g_autoptr(GPtrArray) users = NULL;
	g_autoptr(JsonNode) business = NULL, stranger = NULL;
	VentureTenantService *tenant;
	(void)unused;
	f->db = venture_test_accounting_database(&error); g_assert_no_error(error);
	f->config = venture_config_new();
	f->directory = g_dir_make_tmp("venture-owner-invitations-XXXXXX", &error); g_assert_no_error(error);
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
	f->admin_token = mint(f, venture_entity_get_id(g_ptr_array_index(users, 0)), VENTURE_USER_ROLE_EDITOR);
	f->token = g_strdup(f->admin_token);
	f->context = venture_context_new(f->config, f->db);
	f->server = venture_web_server_new(f->context, &error); g_assert_no_error(error);
	g_assert_true(venture_web_server_start(f->server, &error)); g_assert_no_error(error);
	f->session = soup_session_new();
	business = call(f, "/api/v1/lightsite/signups", "{\"idempotency_key\":\"invite-owner\",\"email\":\"owner@example.test\",\"issuer\":\"https://id.example.test\",\"subject\":\"invite-owner\",\"business_name\":\"Example Grooming\",\"first_name\":\"Alex\",\"last_name\":\"Example\"}", 201);
	f->business = json_object_get_int_member(json_node_get_object(business), "organization_id");
	f->owner = json_object_get_int_member(json_node_get_object(business), "user_id");
	stranger = call(f, "/api/v1/lightsite/signups", "{\"idempotency_key\":\"invite-stranger\",\"email\":\"stranger@example.test\",\"issuer\":\"https://id.example.test\",\"subject\":\"invite-stranger\",\"business_name\":\"Other Business\",\"first_name\":\"Sam\",\"last_name\":\"Other\"}", 201);
	f->stranger_business = json_object_get_int_member(json_node_get_object(stranger), "organization_id");
	f->stranger = json_object_get_int_member(json_node_get_object(stranger), "user_id");
	use_token(f, mint(f, f->owner, VENTURE_USER_ROLE_EDITOR));
}

static void teardown(Fixture *f, gconstpointer unused)
{
	(void)unused;
	venture_web_server_stop(f->server);
	g_clear_object(&f->server); g_clear_object(&f->context); g_clear_object(&f->session);
	venture_test_accounting_database_cleanup(f->db); g_clear_object(&f->db); g_clear_object(&f->config);
	venture_test_remove_tree(f->directory); g_free(f->directory); g_free(f->token); g_free(f->admin_token);
}

static gchar *invitations_path(gint64 business, const gchar *suffix)
{
	return g_strdup_printf("/api/v1/organizations/%" G_GINT64_FORMAT "/invitations%s", business, suffix ? suffix : "");
}

static gchar *invite_body(const gchar *email, const gchar *role, const gchar *key)
{
	return g_strdup_printf("{\"email\":\"%s\",\"name\":\"Ellis Example\",\"role\":\"%s\",\"idempotency_key\":\"%s\",\"confirmed\":true}", email, role, key);
}

/* The invitation messages queued for one invitation, oldest first. */
static GPtrArray *messages_for(Fixture *f, gint64 invitation)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_MAIL_MESSAGE);
	g_autoptr(GError) error = NULL;
	GPtrArray *rows;
	venture_query_add_filter_string(query, "related-type", VENTURE_FILTER_OP_EQ, "tenant_invitation", NULL);
	venture_query_add_filter_int(query, "related-id", VENTURE_FILTER_OP_EQ, invitation, NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	rows = venture_database_find(f->db, query, &error); g_assert_no_error(error);
	return rows;
}

/* The capability in the latest message's private link. */
static gchar *latest_capability(Fixture *f, gint64 invitation)
{
	g_autoptr(GPtrArray) rows = messages_for(f, invitation);
	g_autofree gchar *body = NULL;
	const gchar *code;
	g_assert_cmpuint(rows->len, >, 0);
	g_object_get(g_ptr_array_index(rows, rows->len - 1), "private-text-body", &body, NULL);
	code = strstr(body, "https://example.test/account/invitation#code=");
	g_assert_nonnull(code);
	code += strlen("https://example.test/account/invitation#code=");
	return g_strndup(code, strcspn(code, "\n"));
}

static VentureUser *accept(Fixture *f, const gchar *capability, const gchar *username)
{
	g_autoptr(GError) error = NULL;
	VentureUser *user = venture_tenant_service_accept_invitation(venture_tenant_service_get(f->db), f->config,
		capability, username, "private-staff-password", &error);
	if (!user) g_test_message("acceptance refused: %s", error ? error->message : "(no error)");
	return user;
}

static gint organization_role(Fixture *f, gint64 user, gint64 organization)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP);
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GError) error = NULL;
	gint role = -1;
	venture_query_set_organization(query, organization);
	venture_query_add_filter_int(query, "user-id", VENTURE_FILTER_OP_EQ, user, NULL);
	rows = venture_database_find(f->db, query, &error); g_assert_no_error(error);
	if (rows->len) g_object_get(g_ptr_array_index(rows, 0), "role", &role, NULL);
	return role;
}

static void test_send_and_accept(Fixture *f, gconstpointer unused)
{
	g_autofree gchar *path = invitations_path(f->business, NULL);
	g_autofree gchar *body = invite_body("ellis@example.test", "editor", "invite-ellis");
	g_autofree gchar *capability = NULL, *subject = NULL, *text = NULL, *to = NULL;
	g_autoptr(JsonNode) first = NULL, replay = NULL, refused = NULL, listed = NULL;
	g_autoptr(GPtrArray) messages = NULL;
	g_autoptr(VentureUser) user = NULL, again = NULL;
	JsonObject *answer, *delivery, *entry;
	gint64 invitation;
	(void)unused;
	first = call(f, path, body, 201);
	answer = json_node_get_object(first);
	invitation = json_object_get_int_member(answer, "invitation_id");
	g_assert_cmpint(json_object_get_int_member(answer, "organization_id"), ==, f->business);
	g_assert_cmpstr(json_object_get_string_member(answer, "email"), ==, "ellis@example.test");
	g_assert_cmpstr(json_object_get_string_member(answer, "name"), ==, "Ellis Example");
	g_assert_cmpstr(json_object_get_string_member(answer, "role"), ==, "editor");
	g_assert_cmpstr(json_object_get_string_member(answer, "state"), ==, "pending");
	g_assert_false(json_object_has_member(answer, "capability"));
	g_assert_false(json_object_has_member(answer, "token_hash"));
	delivery = json_object_get_object_member(answer, "delivery");
	g_assert_cmpstr(json_object_get_string_member(delivery, "state"), ==, "queued");
	g_assert_cmpint(json_object_get_int_member(delivery, "sends"), ==, 1);
	/* Honest about delivery: this business has no mail account yet. */
	g_assert_false(json_object_get_boolean_member(delivery, "mail_ready"));
	/* The business's own mail: its outbox, its name, the link private. */
	messages = messages_for(f, invitation);
	g_assert_cmpuint(messages->len, ==, 1);
	g_assert_cmpint(venture_entity_get_organization_id(g_ptr_array_index(messages, 0)), ==, f->business);
	g_object_get(g_ptr_array_index(messages, 0), "subject", &subject, "text-body", &text, "to", &to, NULL);
	g_assert_cmpstr(to, ==, "ellis@example.test");
	g_assert_nonnull(strstr(subject, "Example Grooming"));
	g_assert_null(strstr(subject, "Venture"));
	g_assert_null(strstr(text, "#code="));
	capability = latest_capability(f, invitation);
	/* A retry is the first answer and no second message. */
	replay = call(f, path, body, 200);
	g_assert_cmpint(json_object_get_int_member(json_node_get_object(replay), "invitation_id"), ==, invitation);
	g_clear_pointer(&messages, g_ptr_array_unref);
	messages = messages_for(f, invitation);
	g_assert_cmpuint(messages->len, ==, 1);
	/* The same key for another request, and a second live invitation for
	 * the same address, are both refused. */
	{
		g_autofree gchar *other = invite_body("ellis@example.test", "viewer", "invite-ellis");
		g_autofree gchar *twice = invite_body("ELLIS@example.test", "editor", "invite-ellis-again");
		refused = call(f, path, other, 409); g_clear_pointer(&refused, json_node_unref);
		refused = call(f, path, twice, 409); g_clear_pointer(&refused, json_node_unref);
	}
	/* The invitee accepts into a new account with the role the owner chose. */
	user = accept(f, capability, "ellis");
	g_assert_nonnull(user);
	g_assert_cmpint(organization_role(f, venture_entity_get_id(VENTURE_ENTITY(user)), f->business), ==, VENTURE_ORGANIZATION_ROLE_EDITOR);
	g_assert_cmpint(organization_role(f, venture_entity_get_id(VENTURE_ENTITY(user)), f->stranger_business), ==, -1);
	g_assert_true(venture_tenant_service_is_member(venture_tenant_service_get(f->db), venture_entity_get_id(VENTURE_ENTITY(user)), FALSE));
	g_assert_false(venture_tenant_service_is_member(venture_tenant_service_get(f->db), venture_entity_get_id(VENTURE_ENTITY(user)), TRUE));
	again = accept(f, capability, "ellis-twice");
	g_assert_null(again);
	listed = call(f, path, NULL, 200);
	entry = json_array_get_object_element(json_object_get_array_member(json_node_get_object(listed), "invitations"), 0);
	g_assert_cmpstr(json_object_get_string_member(entry, "state"), ==, "accepted");
	g_assert_cmpint(json_object_get_int_member(entry, "accepted_user_id"), ==, venture_entity_get_id(VENTURE_ENTITY(user)));
	g_test_message("Owner invitation: sent in the business's mail, retried without a second send, accepted once");
}

static void test_resend_and_revoke(Fixture *f, gconstpointer unused)
{
	g_autofree gchar *path = invitations_path(f->business, NULL);
	g_autofree gchar *robin = invite_body("robin@example.test", "viewer", "invite-robin");
	g_autofree gchar *casey = invite_body("casey@example.test", "viewer", "invite-casey");
	g_autofree gchar *old_link = NULL, *new_link = NULL, *casey_link = NULL, *resend = NULL, *revoke = NULL;
	g_autoptr(JsonNode) first = NULL, sent = NULL, repeat = NULL, other = NULL, revoked = NULL, refused = NULL;
	g_autoptr(GPtrArray) messages = NULL;
	g_autoptr(VentureUser) stale = NULL, fresh = NULL, late = NULL;
	g_autofree gchar *state = NULL;
	gint64 invitation, casey_invitation;
	(void)unused;
	first = call(f, path, robin, 201);
	invitation = json_object_get_int_member(json_node_get_object(first), "invitation_id");
	old_link = latest_capability(f, invitation);
	resend = g_strdup_printf("/api/v1/organizations/%" G_GINT64_FORMAT "/invitations/%" G_GINT64_FORMAT "/resend", f->business, invitation);
	sent = call(f, resend, "{\"idempotency_key\":\"resend-robin-1\"}", 201);
	g_assert_cmpint(json_object_get_int_member(json_object_get_object_member(json_node_get_object(sent), "delivery"), "sends"), ==, 2);
	/* The same resend again sends nothing more. */
	repeat = call(f, resend, "{\"idempotency_key\":\"resend-robin-1\"}", 200);
	messages = messages_for(f, invitation);
	g_assert_cmpuint(messages->len, ==, 2);
	new_link = latest_capability(f, invitation);
	g_assert_cmpstr(old_link, !=, new_link);
	/* Only the newest link works. */
	stale = accept(f, old_link, "robin-old");
	g_assert_null(stale);
	fresh = accept(f, new_link, "robin");
	g_assert_nonnull(fresh);
	/* Revoking kills the link and holds back mail the relay has not got. */
	other = call(f, path, casey, 201);
	casey_invitation = json_object_get_int_member(json_node_get_object(other), "invitation_id");
	casey_link = latest_capability(f, casey_invitation);
	revoke = g_strdup_printf("/api/v1/organizations/%" G_GINT64_FORMAT "/invitations/%" G_GINT64_FORMAT "/revoke", f->business, casey_invitation);
	revoked = call(f, revoke, "{}", 200);
	g_assert_cmpstr(json_object_get_string_member(json_node_get_object(revoked), "state"), ==, "revoked");
	g_clear_pointer(&messages, g_ptr_array_unref);
	messages = messages_for(f, casey_invitation);
	g_object_get(g_ptr_array_index(messages, 0), "state", &state, NULL);
	g_assert_cmpstr(state, ==, "cancelled");
	late = accept(f, casey_link, "casey");
	g_assert_null(late);
	refused = call(f, revoke, "{}", 200); g_clear_pointer(&refused, json_node_unref);
	{
		g_autofree gchar *casey_resend = g_strdup_printf("/api/v1/organizations/%" G_GINT64_FORMAT "/invitations/%" G_GINT64_FORMAT "/resend", f->business, casey_invitation);
		refused = call(f, casey_resend, "{\"idempotency_key\":\"resend-casey\"}", 409); g_clear_pointer(&refused, json_node_unref);
		/* An accepted invitation is neither resent nor revoked. */
		refused = call(f, resend, "{\"idempotency_key\":\"resend-robin-2\"}", 409); g_clear_pointer(&refused, json_node_unref);
		g_free(casey_resend);
		casey_resend = g_strdup_printf("/api/v1/organizations/%" G_GINT64_FORMAT "/invitations/%" G_GINT64_FORMAT "/revoke", f->business, invitation);
		refused = call(f, casey_resend, "{}", 409); g_clear_pointer(&refused, json_node_unref);
	}
	g_test_message("Resend rotates the link; revoke kills it and cancels unsent mail");
}

static void test_authority(Fixture *f, gconstpointer unused)
{
	g_autofree gchar *path = invitations_path(f->business, NULL);
	g_autofree gchar *body = invite_body("dana@example.test", "editor", "invite-dana");
	g_autoptr(JsonNode) refused = NULL, made = NULL;
	g_autoptr(VentureUser) staff = NULL, after = NULL;
	g_autofree gchar *link = NULL;
	gint64 invitation;
	guint i;
	static const gchar *const malformed[] = {
		"{\"email\":\"dana@example.test\",\"role\":\"owner\",\"idempotency_key\":\"k1\",\"confirmed\":true}",
		"{\"email\":\"dana@example.test\",\"role\":\"editor\",\"idempotency_key\":\"k2\",\"confirmed\":false}",
		"{\"email\":\"dana@example.test\",\"role\":\"editor\",\"idempotency_key\":\"k3\"}",
		"{\"email\":\"not an address\",\"role\":\"editor\",\"idempotency_key\":\"k4\",\"confirmed\":true}",
		"{\"email\":\"dana@example.test\",\"role\":\"editor\",\"idempotency_key\":\"k5\",\"confirmed\":true,\"note\":1}",
		"{\"email\":\"dana@example.test\",\"role\":\"editor\",\"idempotency_key\":\"bad key\",\"confirmed\":true}",
		"[]"
	};
	(void)unused;
	for (i = 0; i < G_N_ELEMENTS(malformed); i++) {
		refused = call(f, path, malformed[i], 422); g_clear_pointer(&refused, json_node_unref);
	}
	/* The workspace administrator's service token is not the owner. */
	use_token(f, g_strdup(f->admin_token));
	refused = call(f, path, body, 403); g_clear_pointer(&refused, json_node_unref);
	refused = call(f, path, NULL, 403); g_clear_pointer(&refused, json_node_unref);
	/* Another business's owner, and the owner's own read-only token. */
	use_token(f, mint(f, f->stranger, VENTURE_USER_ROLE_EDITOR));
	refused = call(f, path, body, 403); g_clear_pointer(&refused, json_node_unref);
	refused = call(f, path, NULL, 403); g_clear_pointer(&refused, json_node_unref);
	use_token(f, mint(f, f->owner, VENTURE_USER_ROLE_VIEWER));
	refused = call(f, path, body, 403); g_clear_pointer(&refused, json_node_unref);
	/* The owner invites an editor; the editor may not invite in turn. */
	use_token(f, mint(f, f->owner, VENTURE_USER_ROLE_EDITOR));
	made = call(f, path, body, 201);
	invitation = json_object_get_int_member(json_node_get_object(made), "invitation_id");
	staff = accept(f, (link = latest_capability(f, invitation)), "dana");
	g_assert_nonnull(staff);
	use_token(f, mint(f, venture_entity_get_id(VENTURE_ENTITY(staff)), VENTURE_USER_ROLE_EDITOR));
	{
		g_autofree gchar *again = invite_body("erin@example.test", "editor", "invite-erin");
		refused = call(f, path, again, 403); g_clear_pointer(&refused, json_node_unref);
	}
	/* A business's invitation is not found through another business. */
	use_token(f, mint(f, f->stranger, VENTURE_USER_ROLE_EDITOR));
	{
		g_autofree gchar *crossing = g_strdup_printf("/api/v1/organizations/%" G_GINT64_FORMAT "/invitations/%" G_GINT64_FORMAT "/revoke", f->stranger_business, invitation);
		refused = call(f, crossing, "{}", 404); g_clear_pointer(&refused, json_node_unref);
	}
	/* An owner who stops being the owner takes their pending invitations
	 * with them, and loses the right to invite. */
	use_token(f, mint(f, f->owner, VENTURE_USER_ROLE_EDITOR));
	{
		g_autofree gchar *pending = invite_body("frankie@example.test", "viewer", "invite-frankie");
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP);
		g_autoptr(GPtrArray) rows = NULL;
		g_autoptr(GError) error = NULL;
		g_autofree gchar *frankie = NULL;
		g_clear_pointer(&made, json_node_unref);
		made = call(f, path, pending, 201);
		frankie = latest_capability(f, json_object_get_int_member(json_node_get_object(made), "invitation_id"));
		venture_query_set_organization(query, f->business);
		venture_query_add_filter_int(query, "user-id", VENTURE_FILTER_OP_EQ, f->owner, NULL);
		rows = venture_database_find(f->db, query, &error); g_assert_no_error(error);
		g_object_set(g_ptr_array_index(rows, 0), "role", VENTURE_ORGANIZATION_ROLE_ADMIN, NULL);
		save(f, g_ptr_array_index(rows, 0));
		after = accept(f, frankie, "frankie");
		g_assert_null(after);
		refused = call(f, path, NULL, 403); g_clear_pointer(&refused, json_node_unref);
	}
	g_test_message("Owner invitations: owner only, closed bodies, cross-business not found, demotion revokes");
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/owner-invitations/send-and-accept", Fixture, NULL, setup, test_send_and_accept, teardown);
	g_test_add("/owner-invitations/resend-and-revoke", Fixture, NULL, setup, test_resend_and_revoke, teardown);
	g_test_add("/owner-invitations/authority", Fixture, NULL, setup, test_authority, teardown);
	return g_test_run();
}
