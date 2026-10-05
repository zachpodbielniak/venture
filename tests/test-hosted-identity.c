/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * A hosted workspace that trusts one identity provider (Lightsite's Keycloak
 * realm) accepts that provider's access token as the bearer of an API call:
 * the person's one passwordless sign-in, with no Venture key pasted anywhere.
 * The token must be signed by the provider's key, name the configured issuer
 * and audience, and be current; its subject must be linked to an active local
 * user, which only two things do: the trusted sign-up that created the owner,
 * and the person themselves, signed in to Venture, proving the token is theirs.
 * The call then acts as that user under every ordinary check.
 *
 * What breaks if these regress: anybody with a token from the realm reaches
 * Venture as somebody; a token for another audience, expired or forged is
 * accepted; an email or a claim picks the user; one business reads another's
 * people; a deactivated or signed-out person keeps working through a token.
 */
#include <venture.h>
#include <libsoup/soup.h>
#include <openssl/evp.h>
#include <openssl/core_names.h>
#include <openssl/rsa.h>
#include <string.h>
#include "venture-test-accounting.h"
#include "venture-test-util.h"

#define WORKSPACE "8f062b79-1d2b-4d7f-99e5-bd3bf588e05a"
#define ORIGIN "https://example.test:8443"
#define AUTHORITY "example.test:8443"
#define ISSUER "https://id.example.test/realms/lightsite"
#define AUDIENCE "venture-api"

/* The provider's key set, served from its own thread: verification waits on a
 * private context, so the server must not share the main one. */
typedef struct {
	GMutex mutex;
	GCond ready;
	GThread *thread;
	GMainContext *context;
	GMainLoop *loop;
	gchar *jwks_uri, *jwk;
	EVP_PKEY *key;
} Provider;
typedef struct {
	Provider provider;
	VentureDatabase *db;
	VentureConfig *config;
	VentureContext *context;
	VentureWebServer *server;
	VentureTenantService *service;
	SoupSession *session;
	gchar *directory, *admin_secret;
	gint64 admin_id;
} Fixture;
typedef struct { gboolean done; GBytes *body; GError *error; } Reply;
/* What a token says, and how it may be wrong. */
typedef struct {
	const gchar *subject, *issuer, *audience;
	gint64 issued, expires;
	gboolean forged;
} Claims;

static gchar *base64url(const guchar *bytes, gsize length)
{
	gchar *result = g_base64_encode(bytes, length), *p;
	for (p = result; *p; p++) { if (*p == '+') *p = '-'; else if (*p == '/') *p = '_'; else if (*p == '=') { *p = 0; break; } }
	return result;
}
static gchar *public_number(EVP_PKEY *key, const gchar *name)
{
	BIGNUM *number = NULL;
	g_autofree guchar *bytes = NULL;
	gchar *result;
	gint length;
	g_assert_cmpint(EVP_PKEY_get_bn_param(key, name, &number), ==, 1);
	length = BN_num_bytes(number); bytes = g_malloc(length);
	g_assert_cmpint(BN_bn2bin(number, bytes), ==, length);
	result = base64url(bytes, length); BN_free(number); return result;
}
static gint64 now_seconds(void) { return g_get_real_time() / G_USEC_PER_SEC; }
/* A Keycloak-shaped access token: two audiences, an authorized party, a
 * realm role and an email, none of which may pick the user or grant anything. */
static gchar *token_with(Fixture *f, Claims c)
{
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(JsonGenerator) generator = json_generator_new();
	g_autoptr(JsonNode) node = NULL;
	g_autofree gchar *json = NULL, *header = NULL, *body = NULL, *payload = NULL, *signature = NULL;
	g_autofree guchar *bytes = NULL;
	const gchar *head = "{\"alg\":\"RS256\",\"kid\":\"fixture\",\"typ\":\"JWT\"}";
	EVP_MD_CTX *ctx = EVP_MD_CTX_new();
	gsize length = 0;
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "iss"); json_builder_add_string_value(builder, c.issuer ? c.issuer : ISSUER);
	json_builder_set_member_name(builder, "aud");
	json_builder_begin_array(builder);
	json_builder_add_string_value(builder, c.audience ? c.audience : AUDIENCE);
	json_builder_add_string_value(builder, "lightsite-api");
	json_builder_end_array(builder);
	json_builder_set_member_name(builder, "azp"); json_builder_add_string_value(builder, "lightsite-app");
	json_builder_set_member_name(builder, "sub"); json_builder_add_string_value(builder, c.subject);
	json_builder_set_member_name(builder, "email"); json_builder_add_string_value(builder, "admin@provider.test");
	json_builder_set_member_name(builder, "role"); json_builder_add_string_value(builder, "owner");
	json_builder_set_member_name(builder, "iat"); json_builder_add_int_value(builder, c.issued ? c.issued : now_seconds() - 5);
	json_builder_set_member_name(builder, "exp"); json_builder_add_int_value(builder, c.expires ? c.expires : now_seconds() + 300);
	json_builder_end_object(builder); node = json_builder_get_root(builder);
	json_generator_set_root(generator, node); json = json_generator_to_data(generator, NULL);
	header = base64url((const guchar *)head, strlen(head));
	body = base64url((const guchar *)json, strlen(json)); payload = g_strconcat(header, ".", body, NULL);
	g_assert_cmpint(EVP_DigestSignInit(ctx, NULL, EVP_sha256(), NULL, f->provider.key), ==, 1);
	g_assert_cmpint(EVP_DigestSign(ctx, NULL, &length, (const guchar *)payload, strlen(payload)), ==, 1);
	bytes = g_malloc(length);
	g_assert_cmpint(EVP_DigestSign(ctx, bytes, &length, (const guchar *)payload, strlen(payload)), ==, 1);
	signature = base64url(bytes, length); EVP_MD_CTX_free(ctx);
	if (c.forged) signature[3] = signature[3] == 'A' ? 'B' : 'A';
	return g_strconcat(payload, ".", signature, NULL);
}
static gchar *token_for(Fixture *f, const gchar *subject)
{
	Claims c = { subject, NULL, NULL, 0, 0, FALSE };
	return token_with(f, c);
}
static void provider_handler(SoupServer *server, SoupServerMessage *message, const gchar *path, GHashTable *query, gpointer data)
{
	Provider *p = data;
	(void)server; (void)query;
	if (strcmp(path, "/jwks")) { soup_server_message_set_status(message, 404, NULL); return; }
	g_mutex_lock(&p->mutex);
	soup_server_message_set_status(message, 200, NULL);
	soup_server_message_set_response(message, "application/json", SOUP_MEMORY_COPY, p->jwk, strlen(p->jwk));
	g_mutex_unlock(&p->mutex);
}
static gpointer provider_thread(gpointer data)
{
	Provider *p = data;
	g_autoptr(SoupServer) server = NULL;
	g_autoptr(GError) error = NULL;
	GSList *uris;
	p->context = g_main_context_new(); g_main_context_push_thread_default(p->context);
	p->loop = g_main_loop_new(p->context, FALSE); server = soup_server_new(NULL, NULL);
	soup_server_add_handler(server, NULL, provider_handler, p, NULL);
	g_assert_true(soup_server_listen_local(server, 0, SOUP_SERVER_LISTEN_IPV4_ONLY, &error)); g_assert_no_error(error);
	uris = soup_server_get_uris(server);
	g_mutex_lock(&p->mutex); p->jwks_uri = g_uri_to_string_partial(uris->data, G_URI_HIDE_NONE);
	{
		gchar *base = p->jwks_uri;
		p->jwks_uri = g_uri_resolve_relative(base, "jwks", G_URI_FLAGS_NONE, NULL);
		g_free(base);
	}
	g_cond_signal(&p->ready); g_mutex_unlock(&p->mutex);
	g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);
	g_main_loop_run(p->loop); soup_server_disconnect(server);
	g_main_context_pop_thread_default(p->context); return NULL;
}
static gboolean stop_provider(gpointer data) { g_main_loop_quit(((Provider *)data)->loop); return G_SOURCE_REMOVE; }
static void received(GObject *source, GAsyncResult *result, gpointer data)
{
	Reply *reply = data;
	reply->body = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &reply->error);
	reply->done = TRUE;
}
/* One exchange: the status, the body as text, and a session cookie set by it. */
static guint exchange(Fixture *f, const gchar *method, const gchar *path, const gchar *cookie, const gchar *bearer,
	const gchar *content_type, const gchar *payload, gchar **out_body, gchar **out_cookie)
{
	g_autofree gchar *url = g_strconcat(venture_web_server_get_base_url(f->server), path, NULL);
	g_autoptr(SoupMessage) message = soup_message_new(method, url);
	SoupMessageHeaders *headers = soup_message_get_request_headers(message);
	Reply reply = { FALSE, NULL, NULL };
	const gchar *bytes;
	gsize length;
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	soup_message_headers_replace(headers, "Host", AUTHORITY);
	if (cookie) { soup_message_headers_replace(headers, "Cookie", cookie); soup_message_headers_replace(headers, "Origin", ORIGIN); }
	if (bearer) { g_autofree gchar *header = g_strconcat("Bearer ", bearer, NULL); soup_message_headers_replace(headers, "Authorization", header); }
	if (payload) { g_autoptr(GBytes) body = g_bytes_new(payload, strlen(payload)); soup_message_set_request_body_from_bytes(message, content_type, body); }
	soup_session_send_and_read_async(f->session, message, G_PRIORITY_DEFAULT, NULL, received, &reply);
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
static guint get_as(Fixture *f, const gchar *path, const gchar *bearer, gchar **body)
{
	return exchange(f, "GET", path, NULL, bearer, NULL, NULL, body, NULL);
}
static JsonNode *json_of(const gchar *text)
{
	g_autoptr(GError) error = NULL;
	JsonNode *node = venture_json_parse(text, &error);
	g_assert_no_error(error); g_assert_nonnull(node);
	return node;
}
static gchar *mint(VentureDatabase *db, gint64 user_id, gint role)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureApiToken) token = venture_api_token_new();
	gchar *secret;
	g_object_set(token, "name", "Lightsite service", "user-id", user_id, "role", role, NULL);
	secret = venture_api_token_generate(token);
	g_assert_true(venture_database_save(db, VENTURE_ENTITY(token), NULL, &error)); g_assert_no_error(error);
	return secret;
}
/* A hosted member with a password, as an accepted invitation leaves them. */
static gint64 member_with_password(Fixture *f, const gchar *username, gint64 organization, gint role)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureTenantMaintenance) maintenance = venture_tenant_service_enter_maintenance(f->service, "Provision a member", &error);
	g_autoptr(VentureEntity) user = g_object_new(VENTURE_TYPE_USER, "username", username, "organization-id", organization,
		"role", VENTURE_USER_ROLE_EDITOR, "active", TRUE, NULL);
	g_autoptr(VentureEntity) tenant = NULL, org_member = NULL;
	g_assert_no_error(error);
	g_assert_true(venture_user_set_password(VENTURE_USER(user), "member-test-password", 2000, &error));
	g_assert_true(venture_database_save(f->db, user, NULL, &error)); g_assert_no_error(error);
	tenant = g_object_new(VENTURE_TYPE_TENANT_MEMBERSHIP, "name", username, "user-id", venture_entity_get_id(user),
		"role", VENTURE_TENANT_ROLE_MEMBER, "active", TRUE, NULL);
	g_assert_true(venture_database_save(f->db, tenant, NULL, &error)); g_assert_no_error(error);
	org_member = g_object_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP, "organization-id", organization,
		"user-id", venture_entity_get_id(user), "role", role, "active", TRUE, NULL);
	g_assert_true(venture_database_save(f->db, org_member, NULL, &error)); g_assert_no_error(error);
	g_assert_true(venture_tenant_maintenance_finish(maintenance, &error)); g_assert_no_error(error);
	return venture_entity_get_id(user);
}
/* A business by trusted sign-up; returns its organization id, *user its owner. */
static gint64 sign_up(Fixture *f, const gchar *key, const gchar *email, const gchar *subject, const gchar *name, gint64 *user)
{
	g_autofree gchar *payload = g_strdup_printf("{\"idempotency_key\":\"%s\",\"email\":\"%s\",\"issuer\":\"" ISSUER "\","
		"\"subject\":\"%s\",\"business_name\":\"%s\"}", key, email, subject, name);
	g_autofree gchar *body = NULL;
	g_autoptr(JsonNode) node = NULL;
	guint status = exchange(f, "POST", "/api/v1/lightsite/signups", NULL, f->admin_secret, "application/json", payload, &body, NULL);
	if (status != 201) g_test_message("sign-up answered %u: %s", status, body);
	g_assert_cmpuint(status, ==, 201);
	node = json_of(body);
	if (user) *user = json_object_get_int_member(json_node_get_object(node), "user_id");
	return json_object_get_int_member(json_node_get_object(node), "organization_id");
}
static void configure(Fixture *f, gboolean trusted)
{
	g_autoptr(GInetAddress) loopback_address = g_inet_address_new_loopback(G_SOCKET_FAMILY_IPV4);
	g_autofree gchar *loopback = g_inet_address_to_string(loopback_address);
	g_object_set(f->config, "hosted-enabled", TRUE, "hosted-workspace-id", WORKSPACE, "hosted-origin", ORIGIN,
		"security-password-iterations", (gint64)2000, "state-dir", f->directory,
		"server-bind-address", loopback, "server-port", (gint64)0, NULL);
	if (trusted)
		g_object_set(f->config, "hosted-identity-issuer", ISSUER, "hosted-identity-audience", AUDIENCE,
			"hosted-identity-jwks-uri", f->provider.jwks_uri, NULL);
}
static void start(Fixture *f, gboolean trusted)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_USER);
	g_autoptr(GPtrArray) users = NULL;
	g_autoptr(VentureEntity) home = g_object_new(VENTURE_TYPE_ORGANIZATION, "name", "Provider", "active", TRUE, "is-default", TRUE, NULL);
	gboolean ok;
	configure(f, trusted);
	ok = venture_database_migrate(f->db, venture_entity_registry_get_default(), &error);
	g_assert_no_error(error); g_assert_true(ok);
	f->service = venture_tenant_service_get(f->db);
	g_assert_true(venture_tenant_service_configure(f->service, f->config, &error)); g_assert_no_error(error);
	g_assert_true(venture_tenant_service_initialize(f->service, &error)); g_assert_no_error(error);
	g_assert_true(venture_database_save(f->db, home, NULL, &error)); g_assert_no_error(error);
	g_assert_true(venture_tenant_service_bootstrap_admin(f->service, f->config, "provisioner", "private-test-password",
		FALSE, "Identity fixture", &error)); g_assert_no_error(error);
	users = venture_database_find(f->db, query, &error); g_assert_no_error(error);
	f->admin_id = venture_entity_get_id(g_ptr_array_index(users, 0));
	f->admin_secret = mint(f->db, f->admin_id, VENTURE_USER_ROLE_EDITOR);
	f->context = venture_context_new(f->config, f->db);
	f->server = venture_web_server_new(f->context, &error); g_assert_no_error(error);
	g_assert_true(venture_web_server_start(f->server, &error)); g_assert_no_error(error);
}
static void setup(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *n = NULL, *e = NULL;
	EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL);
	Provider *p = &f->provider;
	g_mutex_init(&p->mutex); g_cond_init(&p->ready);
	g_assert_cmpint(EVP_PKEY_keygen_init(ctx), ==, 1); g_assert_cmpint(EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, 2048), ==, 1);
	g_assert_cmpint(EVP_PKEY_keygen(ctx, &p->key), ==, 1); EVP_PKEY_CTX_free(ctx);
	n = public_number(p->key, OSSL_PKEY_PARAM_RSA_N); e = public_number(p->key, OSSL_PKEY_PARAM_RSA_E);
	p->jwk = g_strdup_printf("{\"keys\":[{\"kty\":\"RSA\",\"kid\":\"fixture\",\"use\":\"sig\",\"alg\":\"RS256\",\"n\":\"%s\",\"e\":\"%s\"}]}", n, e);
	g_mutex_lock(&p->mutex); p->thread = g_thread_new("identity-provider", provider_thread, p);
	while (!p->jwks_uri) g_cond_wait(&p->ready, &p->mutex);
	g_mutex_unlock(&p->mutex);
	f->db = venture_test_accounting_database(&error); g_assert_no_error(error);
	f->config = venture_config_new();
	f->directory = g_dir_make_tmp("venture-identity-XXXXXX", &error); g_assert_no_error(error);
	f->session = soup_session_new_with_options("timeout", 10, NULL);
	start(f, data == NULL);
}
static void teardown(Fixture *f, gconstpointer data)
{
	Provider *p = &f->provider;
	(void)data;
	if (f->server) venture_web_server_stop(f->server);
	g_clear_object(&f->server); g_clear_object(&f->context); g_clear_object(&f->session);
	venture_test_accounting_database_cleanup(f->db);
	g_clear_object(&f->db); g_clear_object(&f->config);
	venture_test_remove_tree(f->directory); g_free(f->directory); g_free(f->admin_secret);
	g_main_context_invoke(p->context, stop_provider, p); g_thread_join(p->thread);
	g_main_loop_unref(p->loop); g_main_context_unref(p->context);
	g_free(p->jwks_uri); g_free(p->jwk); EVP_PKEY_free(p->key);
	g_mutex_clear(&p->mutex); g_cond_clear(&p->ready);
}
/* The organizations of an account-authority answer, by id -> "role/manage". */
static gchar *authority_of(Fixture *f, const gchar *bearer, gint64 organization)
{
	g_autofree gchar *body = NULL;
	g_autoptr(JsonNode) node = NULL;
	JsonArray *list;
	guint i;
	g_assert_cmpuint(get_as(f, "/api/v1/account-authority", bearer, &body), ==, 200);
	node = json_of(body);
	list = json_object_get_array_member(json_node_get_object(node), "organizations");
	for (i = 0; i < json_array_get_length(list); i++) {
		JsonObject *entry = json_array_get_object_element(list, i);
		if (json_object_get_int_member(entry, "organization_id") == organization)
			return g_strdup_printf("%s/%s", json_object_get_string_member(entry, "role"),
				json_object_get_boolean_member(entry, "can_manage_sites") ? "manage" : "read");
	}
	return NULL;
}

/* A new owner's sign-up links their identity: from then on their own token
 * reads their business, its people and its connections, and nobody else's. */
static void test_signup_links(Fixture *f, gconstpointer data)
{
	g_autofree gchar *token = NULL, *other_token = NULL, *body = NULL, *path = NULL, *seen = NULL;
	gint64 owner = 0, business, other;
	(void)data;
	business = sign_up(f, "signup-a", "owner-a@example.test", "kc-subject-bea", "Example Co", &owner);
	other = sign_up(f, "signup-b", "owner-b@example.test", "kc-subject-carl", "Second Example Co", NULL);
	token = token_for(f, "kc-subject-bea");
	other_token = token_for(f, "kc-subject-carl");
	seen = authority_of(f, token, business);
	g_assert_cmpstr(seen, ==, "owner/manage");
	g_clear_pointer(&seen, g_free);
	seen = authority_of(f, token, other);
	g_assert_null(seen);
	/* Business details. */
	path = g_strdup_printf("/api/v1/organization/%" G_GINT64_FORMAT, business);
	g_assert_cmpuint(get_as(f, path, token, &body), ==, 200);
	g_assert_nonnull(strstr(body, "Example Co"));
	g_clear_pointer(&body, g_free);
	/* People. */
	g_free(path); path = g_strdup_printf("/api/v1/organization_membership?organization_id=%" G_GINT64_FORMAT, business);
	g_assert_cmpuint(get_as(f, path, token, &body), ==, 200);
	{
		g_autoptr(JsonNode) node = json_of(body);
		JsonArray *records = json_object_get_array_member(json_node_get_object(node), "records");
		g_assert_cmpuint(json_array_get_length(records), ==, 1);
		g_assert_cmpint(json_object_get_int_member(json_array_get_object_element(records, 0), "user_id"), ==, owner);
	}
	g_clear_pointer(&body, g_free);
	/* Connections (where Stripe's state lives) answer for their own business. */
	g_free(path); path = g_strdup_printf("/api/v1/integration_connection?organization_id=%" G_GINT64_FORMAT, business);
	g_assert_cmpuint(get_as(f, path, token, NULL), ==, 200);
	/* Another business stays another's. */
	g_free(path); path = g_strdup_printf("/api/v1/organization/%" G_GINT64_FORMAT, other);
	g_assert_cmpuint(get_as(f, path, token, NULL), !=, 200);
	g_assert_cmpuint(get_as(f, path, other_token, NULL), ==, 200);
	g_free(path); path = g_strdup_printf("/api/v1/organization_membership?organization_id=%" G_GINT64_FORMAT, other);
	{
		guint status = get_as(f, path, token, &body);
		if (status == 200) {
			g_autoptr(JsonNode) node = json_of(body);
			g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_node_get_object(node), "records")), ==, 0);
		}
	}
}
/* Only a signed, current token for this issuer and audience, linked to an
 * active user, is anybody; everything else is a plain 401. */
static void test_refusals(Fixture *f, gconstpointer data)
{
	g_autofree gchar *expired = NULL, *wrong_audience = NULL, *wrong_issuer = NULL, *forged = NULL, *unlinked = NULL, *good = NULL;
	Claims c;
	(void)data;
	sign_up(f, "signup-a", "owner-a@example.test", "kc-subject-bea", "Example Co", NULL);
	good = token_for(f, "kc-subject-bea");
	g_assert_cmpuint(get_as(f, "/api/v1/account-authority", good, NULL), ==, 200);
	c = (Claims){ "kc-subject-bea", NULL, NULL, now_seconds() - 600, now_seconds() - 300, FALSE }; expired = token_with(f, c);
	c = (Claims){ "kc-subject-bea", NULL, "lightsite-only", 0, 0, FALSE }; wrong_audience = token_with(f, c);
	c = (Claims){ "kc-subject-bea", "https://other.example.test/realms/lightsite", NULL, 0, 0, FALSE }; wrong_issuer = token_with(f, c);
	c = (Claims){ "kc-subject-bea", NULL, NULL, 0, 0, TRUE }; forged = token_with(f, c);
	unlinked = token_for(f, "kc-subject-nobody");
	g_assert_cmpuint(get_as(f, "/api/v1/account-authority", expired, NULL), ==, 401);
	g_assert_cmpuint(get_as(f, "/api/v1/account-authority", wrong_audience, NULL), ==, 401);
	g_assert_cmpuint(get_as(f, "/api/v1/account-authority", wrong_issuer, NULL), ==, 401);
	g_assert_cmpuint(get_as(f, "/api/v1/account-authority", forged, NULL), ==, 401);
	g_assert_cmpuint(get_as(f, "/api/v1/account-authority", unlinked, NULL), ==, 401);
	g_assert_cmpuint(get_as(f, "/api/v1/account-authority", "not.a-token.at-all", NULL), ==, 401);
	/* Ordinary Venture tokens still work beside it. */
	g_assert_cmpuint(get_as(f, "/api/v1/account-authority", f->admin_secret, NULL), ==, 200);
}
/* Without a trusted provider, a provider token is nobody. */
static void test_untrusted(Fixture *f, gconstpointer data)
{
	g_autofree gchar *token = NULL;
	(void)data;
	sign_up(f, "signup-a", "owner-a@example.test", "kc-subject-bea", "Example Co", NULL);
	token = token_for(f, "kc-subject-bea");
	g_assert_cmpuint(get_as(f, "/api/v1/account-authority", token, NULL), ==, 401);
}
/* Someone who already had a Venture account links their identity once,
 * signed in to Venture and presenting their own token. A token cannot link
 * itself, and one identity belongs to one person. */
static void test_self_link(Fixture *f, gconstpointer data)
{
	g_autofree gchar *cookie = NULL, *token = NULL, *body = NULL, *payload = NULL, *seen = NULL, *other_cookie = NULL;
	gint64 business, member;
	(void)data;
	business = sign_up(f, "signup-a", "owner-a@example.test", "kc-subject-bea", "Example Co", NULL);
	member = member_with_password(f, "existing-editor", business, VENTURE_ORGANIZATION_ROLE_EDITOR);
	(void)member;
	token = token_for(f, "kc-subject-editor");
	g_assert_cmpuint(get_as(f, "/api/v1/account-authority", token, NULL), ==, 401);
	g_assert_cmpuint(exchange(f, "POST", "/login", NULL, NULL, "application/x-www-form-urlencoded",
		"username=existing-editor&password=member-test-password", NULL, &cookie), ==, 302);
	g_assert_nonnull(cookie);
	payload = g_strdup_printf("{\"access_token\":\"%s\"}", token);
	/* Not signed in to Venture: refused. */
	g_assert_cmpuint(exchange(f, "POST", "/api/v1/account/identity-link", NULL, NULL, "application/json", payload, NULL, NULL), ==, 401);
	/* The provider token cannot vouch for itself. */
	g_assert_cmpuint(exchange(f, "POST", "/api/v1/account/identity-link", NULL, token, "application/json", payload, NULL, NULL), ==, 401);
	/* A token that does not verify links nothing. */
	g_assert_cmpuint(exchange(f, "POST", "/api/v1/account/identity-link", cookie, NULL, "application/json",
		"{\"access_token\":\"not.a.token\"}", &body, NULL), ==, 422);
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(exchange(f, "POST", "/api/v1/account/identity-link", cookie, NULL, "application/json", payload, &body, NULL), ==, 200);
	g_assert_null(strstr(body, "kc-subject-editor"));
	g_clear_pointer(&body, g_free);
	seen = authority_of(f, token, business);
	g_assert_cmpstr(seen, ==, "editor/read");
	/* Linking again is the same link. */
	g_assert_cmpuint(exchange(f, "POST", "/api/v1/account/identity-link", cookie, NULL, "application/json", payload, NULL, NULL), ==, 200);
	/* Bea's identity is Bea's: another person cannot claim it. */
	member_with_password(f, "second-person", business, VENTURE_ORGANIZATION_ROLE_VIEWER);
	g_assert_cmpuint(exchange(f, "POST", "/login", NULL, NULL, "application/x-www-form-urlencoded",
		"username=second-person&password=member-test-password", NULL, &other_cookie), ==, 302);
	g_free(payload); g_free(token);
	token = token_for(f, "kc-subject-bea");
	payload = g_strdup_printf("{\"access_token\":\"%s\"}", token);
	g_assert_cmpuint(exchange(f, "POST", "/api/v1/account/identity-link", other_cookie, NULL, "application/json", payload, NULL, NULL), ==, 409);
	g_clear_pointer(&seen, g_free);
	seen = authority_of(f, token, business);
	g_assert_cmpstr(seen, ==, "owner/manage");
}
/* Deactivation and "sign out everywhere" end provider-token access at once. */
static void test_revocation(Fixture *f, gconstpointer data)
{
	g_autofree gchar *token = NULL, *later = NULL, *statement = NULL;
	gint64 owner = 0;
	(void)data;
	sign_up(f, "signup-a", "owner-a@example.test", "kc-subject-bea", "Example Co", &owner);
	token = token_for(f, "kc-subject-bea");
	g_assert_cmpuint(get_as(f, "/api/v1/account-authority", token, NULL), ==, 200);
	statement = g_strdup_printf("UPDATE users SET sessions_invalidated_at = '%s' WHERE id = %" G_GINT64_FORMAT,
		"2099-01-01T00:00:00Z", owner);
	g_assert_true(venture_database_execute(f->db, statement, NULL, NULL));
	g_assert_cmpuint(get_as(f, "/api/v1/account-authority", token, NULL), ==, 401);
	g_free(statement);
	statement = g_strdup_printf("UPDATE users SET sessions_invalidated_at = NULL, active = FALSE WHERE id = %" G_GINT64_FORMAT, owner);
	g_assert_true(venture_database_execute(f->db, statement, NULL, NULL));
	later = token_for(f, "kc-subject-bea");
	g_assert_cmpuint(get_as(f, "/api/v1/account-authority", later, NULL), ==, 401);
}
/* Half a configuration is refused at start, not discovered on a request. */
static void test_config(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureDatabase) db = venture_test_accounting_database(&error);
	g_autoptr(VentureConfig) config = venture_config_new();
	VentureTenantService *service;
	g_assert_no_error(error);
	service = venture_tenant_service_get(db);
	g_object_set(config, "hosted-enabled", TRUE, "hosted-workspace-id", WORKSPACE, "hosted-origin", ORIGIN,
		"hosted-identity-issuer", ISSUER, NULL);
	g_assert_false(venture_tenant_service_configure(service, config, &error));
	g_assert_nonnull(error);
	g_assert_nonnull(strstr(error->message, "identity"));
	venture_test_accounting_database_cleanup(db);
}
int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/hosted-identity/signup-links", Fixture, NULL, setup, test_signup_links, teardown);
	g_test_add("/hosted-identity/refusals", Fixture, NULL, setup, test_refusals, teardown);
	g_test_add("/hosted-identity/untrusted", Fixture, "untrusted", setup, test_untrusted, teardown);
	g_test_add("/hosted-identity/self-link", Fixture, NULL, setup, test_self_link, teardown);
	g_test_add("/hosted-identity/revocation", Fixture, NULL, setup, test_revocation, teardown);
	g_test_add_func("/hosted-identity/config", test_config);
	return g_test_run();
}
