/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <openssl/evp.h>
#include <openssl/core_names.h>
#include <openssl/rsa.h>
#include <libxml/HTMLparser.h>
#include <libxml/xpath.h>
#include <string.h>
#include <unistd.h>
#include "venture-test-accounting.h"
#include "venture-test-util.h"

/* The fixture signs real ID tokens. Only the provider's HTTP server runs on
 * another thread; every Venture operation and database access stays here. */
typedef struct {
	GMutex mutex;
	GCond ready;
	GThread *thread;
	GMainContext *context;
	GMainLoop *loop;
	gchar *issuer, *nonce, *pkce;
	EVP_PKEY *key;
	gchar *jwk;
	guint exchanges;
	SoupServerMessage *stalled;
	gboolean stall_discovery, stall_jwks;
	gboolean bad_nonce, bad_audience, bad_issuer, expired, bad_azp, bad_signature, redirect;
} Provider;
typedef struct {
	Provider provider;
	VentureDatabase *db;
	VentureConfig *config;
	VentureContext *context;
	VentureOidcService *service;
	VentureIntegrationConnection *binding;
	gchar *directory;
	gint64 org, user;
} Fixture;
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
static gchar *signed_token(Provider *p)
{
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(JsonGenerator) generator = json_generator_new();
	g_autoptr(JsonNode) node = NULL;
	g_autofree gchar *json = NULL, *header = NULL, *body = NULL, *payload = NULL, *signature = NULL;
	g_autofree guchar *bytes = NULL;
	EVP_MD_CTX *ctx = EVP_MD_CTX_new();
	gsize length = 0;
	gint64 now = g_get_real_time() / G_USEC_PER_SEC;
	json_builder_begin_object(builder);
#define CLAIM(name, value) json_builder_set_member_name(builder, name); json_builder_add_string_value(builder, value)
	CLAIM("iss", p->bad_issuer ? "https://wrong.invalid" : p->issuer);
	CLAIM("aud", p->bad_audience ? "wrong-client" : "venture-client");
	CLAIM("sub", "opaque-local-test-subject");
	CLAIM("nonce", p->bad_nonce ? "wrong-nonce" : p->nonce);
	CLAIM("azp", p->bad_azp ? "other-client" : "venture-client");
	/* An administrator claim must have no effect on local authority. */
	CLAIM("role", "admin");
#undef CLAIM
	json_builder_set_member_name(builder, "iat"); json_builder_add_int_value(builder, now - 5);
	json_builder_set_member_name(builder, "exp"); json_builder_add_int_value(builder, p->expired ? now - 60 : now + 300);
	json_builder_end_object(builder); node = json_builder_get_root(builder);
	json_generator_set_root(generator, node); json = json_generator_to_data(generator, NULL);
	header = base64url((const guchar *)"{\"alg\":\"RS256\",\"kid\":\"fixture\",\"typ\":\"JWT\"}", strlen("{\"alg\":\"RS256\",\"kid\":\"fixture\",\"typ\":\"JWT\"}"));
	body = base64url((const guchar *)json, strlen(json)); payload = g_strconcat(header, ".", body, NULL);
	g_assert_cmpint(EVP_DigestSignInit(ctx, NULL, EVP_sha256(), NULL, p->key), ==, 1);
	g_assert_cmpint(EVP_DigestSign(ctx, NULL, &length, (const guchar *)payload, strlen(payload)), ==, 1);
	bytes = g_malloc(length);
	g_assert_cmpint(EVP_DigestSign(ctx, bytes, &length, (const guchar *)payload, strlen(payload)), ==, 1);
	signature = base64url(bytes, length); EVP_MD_CTX_free(ctx);
	if (p->bad_signature) signature[0] = signature[0] == 'A' ? 'B' : 'A';
	return g_strconcat(payload, ".", signature, NULL);
}
static void provider_handler(SoupServer *server, SoupServerMessage *message, const gchar *path, GHashTable *query, gpointer data)
{
	Provider *p = data;
	g_autofree gchar *response = NULL;
	(void)server; (void)query;
	g_mutex_lock(&p->mutex);
	if ((p->stall_discovery && !strcmp(path, "/.well-known/openid-configuration")) ||
		(p->stall_jwks && !strcmp(path, "/jwks"))) {
		p->stalled = g_object_ref(message); soup_server_message_pause(message);
		g_mutex_unlock(&p->mutex); return;
	}
	if (p->redirect) soup_server_message_set_redirect(message, 302, "http://127.0.0.1:1/refused");
	else if (!strcmp(path, "/.well-known/openid-configuration")) response = g_strdup_printf(
		"{\"issuer\":\"%s\",\"authorization_endpoint\":\"%s/authorize\",\"token_endpoint\":\"%s/token\",\"jwks_uri\":\"%s/jwks\","
		"\"response_types_supported\":[\"code\"],\"code_challenge_methods_supported\":[\"S256\"],\"token_endpoint_auth_methods_supported\":[\"client_secret_post\"]}",
		p->issuer, p->issuer, p->issuer, p->issuer);
	else if (!strcmp(path, "/jwks")) response = g_strdup(p->jwk);
	else if (!strcmp(path, "/token")) {
		SoupMessageBody *body = soup_server_message_get_request_body(message);
		g_autofree gchar *encoded = g_strndup(body->data, body->length);
		g_autoptr(GHashTable) form = soup_form_decode(encoded);
		const gchar *verifier = g_hash_table_lookup(form, "code_verifier");
		g_autoptr(GChecksum) hash = g_checksum_new(G_CHECKSUM_SHA256);
		guchar digest[32]; gsize length = sizeof digest;
		g_autofree gchar *actual = NULL, *token = NULL;
		g_assert_cmpstr(g_hash_table_lookup(form, "grant_type"), ==, "authorization_code");
		g_assert_cmpstr(g_hash_table_lookup(form, "client_id"), ==, "venture-client");
		g_assert_cmpstr(g_hash_table_lookup(form, "client_secret"), ==, "fixture-client-secret");
		g_assert_cmpstr(g_hash_table_lookup(form, "redirect_uri"), ==, "http://127.0.0.1:8748/auth/oidc/callback");
		g_assert_nonnull(verifier); g_assert_cmpuint(strlen(verifier), >=, 43);
		g_checksum_update(hash, (const guchar *)verifier, strlen(verifier)); g_checksum_get_digest(hash, digest, &length);
		actual = base64url(digest, length); g_assert_cmpstr(actual, ==, p->pkce);
		token = signed_token(p); response = g_strdup_printf("{\"id_token\":\"%s\",\"token_type\":\"Bearer\",\"access_token\":\"never-persist-me\"}", token);
		p->exchanges++;
	} else soup_server_message_set_status(message, 404, NULL);
	if (response) { soup_server_message_set_status(message, 200, NULL); soup_server_message_set_response(message, "application/json", SOUP_MEMORY_COPY, response, strlen(response)); }
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
	g_mutex_lock(&p->mutex); p->issuer = g_strdup_printf("http://127.0.0.1:%d", g_uri_get_port(uris->data));
	g_cond_signal(&p->ready); g_mutex_unlock(&p->mutex);
	g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);
	g_main_loop_run(p->loop); soup_server_disconnect(server);
	g_main_context_pop_thread_default(p->context); return NULL;
}
static gboolean stop_provider(gpointer data)
{
	Provider *p = data;
	if (p->stalled) { soup_server_message_set_status(p->stalled, 503, NULL); soup_server_message_unpause(p->stalled); g_clear_object(&p->stalled); }
	g_main_loop_quit(p->loop); return G_SOURCE_REMOVE;
}
static void setup(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureOrganization) org = venture_organization_new();
	g_autoptr(VentureUser) user = venture_user_new();
	g_autoptr(VentureOrganizationMembership) membership = venture_organization_membership_new();
	g_autoptr(GBytes) key = g_bytes_new_static("01234567890123456789012345678901", 32);
	g_autofree gchar *n = NULL, *e = NULL;
	const gchar *issuers[2];
	EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL);
	Provider *p = &f->provider;
	(void)data;
	g_mutex_init(&p->mutex); g_cond_init(&p->ready);
	g_assert_cmpint(EVP_PKEY_keygen_init(ctx), ==, 1); g_assert_cmpint(EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, 2048), ==, 1);
	g_assert_cmpint(EVP_PKEY_keygen(ctx, &p->key), ==, 1); EVP_PKEY_CTX_free(ctx);
	n = public_number(p->key, OSSL_PKEY_PARAM_RSA_N); e = public_number(p->key, OSSL_PKEY_PARAM_RSA_E);
	p->jwk = g_strdup_printf("{\"keys\":[{\"kty\":\"RSA\",\"kid\":\"fixture\",\"use\":\"sig\",\"alg\":\"RS256\",\"n\":\"%s\",\"e\":\"%s\"}]}", n, e);
	g_mutex_lock(&p->mutex); p->thread = g_thread_new("oidc-fixture", provider_thread, p);
	while (!p->issuer) g_cond_wait(&p->ready, &p->mutex);
	g_mutex_unlock(&p->mutex);
	f->db = venture_test_accounting_database(&error); g_assert_no_error(error);
	f->directory = g_dir_make_tmp("venture-oidc-XXXXXX", &error); g_assert_no_error(error);
	f->config = venture_config_new(); issuers[0] = p->issuer; issuers[1] = NULL;
	g_object_set(f->config, "oidc-enabled", TRUE, "oidc-allow-loopback", TRUE, "oidc-allowed-issuers", issuers,
		"server-base-url", "http://127.0.0.1:8748", "state-dir", f->directory, "security-require-auth", TRUE,
		"security-session-secret-env", "VENTURE_TEST_OIDC_SESSION", "security-mfa-key-env", "VENTURE_TEST_OIDC_MFA", NULL);
	f->context = venture_context_new(f->config, f->db);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error)); g_assert_no_error(error);
	g_object_set(org, "name", "OIDC fixture", NULL); g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(org), NULL, &error));
	f->org = venture_entity_get_id(VENTURE_ENTITY(org));
	g_object_set(user, "username", "local-member", "active", TRUE, "role", VENTURE_USER_ROLE_VIEWER, NULL);
	g_assert_true(venture_user_set_password(user, "fixture-password", 100000, &error));
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(user), NULL, &error)); f->user = venture_entity_get_id(VENTURE_ENTITY(user));
	venture_entity_set_organization_id(VENTURE_ENTITY(membership), f->org);
	g_object_set(membership, "user-id", f->user, "role", VENTURE_ORGANIZATION_ROLE_VIEWER, "active", TRUE, NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(membership), NULL, &error)); g_assert_no_error(error);
	g_assert_true(venture_integration_service_set_key(venture_integration_service_get(f->db), key, &error)); g_assert_no_error(error);
	f->service = venture_oidc_service_get(f->db);
	f->binding = venture_oidc_service_connect(f->service, f->org, p->issuer, "venture-client", "fixture-client-secret", 0, NULL, &error);
	g_assert_no_error(error); g_assert_nonnull(f->binding);
}
static void teardown(Fixture *f, gconstpointer data)
{
	Provider *p = &f->provider;
	(void)data;
	g_main_context_invoke(p->context, stop_provider, p); g_thread_join(p->thread);
	g_main_loop_unref(p->loop); g_main_context_unref(p->context);
	g_free(p->issuer); g_free(p->nonce); g_free(p->pkce); g_free(p->jwk); EVP_PKEY_free(p->key);
	g_mutex_clear(&p->mutex); g_cond_clear(&p->ready);
	g_clear_object(&f->binding); g_clear_object(&f->context); g_clear_object(&f->config);
	venture_test_accounting_database_cleanup(f->db); g_clear_object(&f->db);
	venture_test_remove_tree(f->directory); g_free(f->directory);
	venture_entity_registry_set_type_module(venture_entity_registry_get_default(), "oidc_identity", "oidc", TRUE);
}
static GHashTable *begin(Fixture *f, gint64 user, gchar **browser)
{
	g_autofree gchar *uuid = NULL, *url = NULL;
	g_autoptr(GUri) uri = NULL;
	g_autoptr(GError) error = NULL;
	GHashTable *params;
	g_object_get(f->binding, "uuid", &uuid, NULL);
	url = venture_oidc_service_begin(f->service, uuid, user, user ? "fixture-password" : NULL, "fixture-peer", browser, &error);
	g_assert_no_error(error); g_assert_nonnull(url); uri = g_uri_parse(url, G_URI_FLAGS_NONE, &error); g_assert_no_error(error);
	params = soup_form_decode(g_uri_get_query(uri));
	g_assert_cmpstr(g_hash_table_lookup(params, "code_challenge_method"), ==, "S256");
	g_mutex_lock(&f->provider.mutex);
	g_free(f->provider.nonce); f->provider.nonce = g_strdup(g_hash_table_lookup(params, "nonce"));
	g_free(f->provider.pkce); f->provider.pkce = g_strdup(g_hash_table_lookup(params, "code_challenge"));
	g_mutex_unlock(&f->provider.mutex); return params;
}
static VentureOidcIdentity *link_identity(Fixture *f)
{
	g_autofree gchar *browser = NULL;
	g_autoptr(GHashTable) params = begin(f, f->user, &browser);
	g_autoptr(GError) error = NULL;
	VentureOidcIdentity *identity = venture_oidc_service_finish(f->service, g_hash_table_lookup(params, "state"), "fixture-code", browser, f->user, &error);
	g_assert_no_error(error); g_assert_nonnull(identity); return identity;
}
static void test_link_signin_replay(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureOidcIdentity) identity = link_identity(f), result = NULL;
	g_autoptr(VentureUser) user = NULL;
	g_autofree gchar *browser = NULL;
	g_autoptr(GHashTable) params = begin(f, 0, &browser);
	g_autoptr(GError) error = NULL;
	VentureUserRole role;
	(void)data;
	result = venture_oidc_service_finish(f->service, g_hash_table_lookup(params, "state"), "fixture-code", browser, 0, &error);
	g_assert_no_error(error); g_assert_nonnull(result);
	g_assert_cmpint(venture_entity_get_id(VENTURE_ENTITY(result)), ==, venture_entity_get_id(VENTURE_ENTITY(identity)));
	user = venture_oidc_service_identity_user(f->service, venture_entity_get_id(VENTURE_ENTITY(result)), &error);
	g_assert_no_error(error); g_assert_nonnull(user); g_object_get(user, "role", &role, NULL); g_assert_cmpint(role, ==, VENTURE_USER_ROLE_VIEWER);
	g_clear_object(&result);
	result = venture_oidc_service_finish(f->service, g_hash_table_lookup(params, "state"), "fixture-code", browser, 0, &error);
	g_assert_null(result); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_UNAUTHENTICATED);
	g_assert_cmpuint(f->provider.exchanges, ==, 2);
}
static void test_unlinked_refused(Fixture *f, gconstpointer data)
{
	g_autofree gchar *browser = NULL;
	g_autoptr(GHashTable) params = begin(f, 0, &browser);
	g_autoptr(VentureOidcIdentity) identity = NULL;
	g_autoptr(GError) error = NULL;
	(void)data;
	identity = venture_oidc_service_finish(f->service, g_hash_table_lookup(params, "state"), "fixture-code", browser, 0, &error);
	g_assert_null(identity); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_UNAUTHENTICATED);
}
static void test_identity_registered(void)
{
	g_assert_cmpuint(venture_entity_registry_lookup(venture_entity_registry_get_default(), "oidc_identity"), !=, G_TYPE_INVALID);
}
/* A forged or stale response must fail before any identity can be granted. */
static void test_invalid_claims(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureOidcIdentity) link = link_identity(f);
	gboolean *flags[] = { &f->provider.bad_nonce, &f->provider.bad_audience, &f->provider.bad_issuer,
		&f->provider.expired, &f->provider.bad_azp, &f->provider.bad_signature };
	guint i;
	(void)data;
	for (i = 0; i < G_N_ELEMENTS(flags); i++) {
		g_autofree gchar *browser = NULL;
		g_autoptr(GHashTable) params = begin(f, 0, &browser);
		g_autoptr(GError) error = NULL;
		g_autoptr(VentureOidcIdentity) result = NULL;
		g_mutex_lock(&f->provider.mutex); *flags[i] = TRUE; g_mutex_unlock(&f->provider.mutex);
		result = venture_oidc_service_finish(f->service, g_hash_table_lookup(params, "state"), "fixture-code", browser, 0, &error);
		g_assert_null(result); g_assert_nonnull(error);
		g_mutex_lock(&f->provider.mutex); *flags[i] = FALSE; g_mutex_unlock(&f->provider.mutex);
	}
}
static void test_browser_binding(Fixture *f, gconstpointer data)
{
	g_autofree gchar *browser = NULL;
	g_autoptr(GHashTable) params = begin(f, f->user, &browser);
	g_autoptr(VentureOidcIdentity) result = NULL;
	g_autoptr(GError) error = NULL;
	(void)data;
	result = venture_oidc_service_finish(f->service, g_hash_table_lookup(params, "state"), "fixture-code",
		"0000000000000000000000000000000000000000000000000000000000000000", f->user, &error);
	g_assert_null(result); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_UNAUTHENTICATED); g_clear_error(&error);
	result = venture_oidc_service_finish(f->service, g_hash_table_lookup(params, "state"), "fixture-code", browser, f->user, &error);
	g_assert_null(result); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_UNAUTHENTICATED);
	g_assert_cmpuint(f->provider.exchanges, ==, 0);
}
static void test_binding_rotation(Fixture *f, gconstpointer data)
{
	g_autofree gchar *browser = NULL;
	g_autoptr(GHashTable) params = begin(f, f->user, &browser);
	g_autoptr(VentureIntegrationConnection) rotated = NULL;
	g_autoptr(VentureOidcIdentity) result = NULL;
	g_autoptr(GError) error = NULL;
	(void)data;
	rotated = venture_oidc_service_connect(f->service, f->org, f->provider.issuer, "venture-client", "rotated-secret",
		venture_entity_get_version(VENTURE_ENTITY(f->binding)), NULL, &error);
	g_assert_no_error(error); g_assert_nonnull(rotated);
	result = venture_oidc_service_finish(f->service, g_hash_table_lookup(params, "state"), "fixture-code", browser, f->user, &error);
	g_assert_null(result); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_UNAUTHENTICATED);
	g_assert_cmpuint(f->provider.exchanges, ==, 0);
}
static void test_generic_writes(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureOidcIdentity) identity = g_object_new(VENTURE_TYPE_OIDC_IDENTITY, NULL);
	g_autoptr(GError) error = NULL;
	(void)data;
	venture_entity_set_organization_id(VENTURE_ENTITY(identity), f->org);
	g_object_set(identity, "user-id", f->user, "connection-id", venture_entity_get_id(VENTURE_ENTITY(f->binding)),
		"issuer", f->provider.issuer, "subject", "forged", "identity-key", "forged", "active", TRUE, NULL);
	g_assert_false(venture_database_save(f->db, VENTURE_ENTITY(identity), NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION); g_clear_error(&error); g_clear_object(&identity);
	identity = link_identity(f);
	g_object_set(identity, "active", FALSE, NULL);
	g_assert_false(venture_database_save(f->db, VENTURE_ENTITY(identity), NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION); g_clear_error(&error);
	g_assert_false(venture_database_delete(f->db, VENTURE_ENTITY(identity), NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION); g_clear_error(&error);
	g_assert_false(venture_database_restore(f->db, VENTURE_ENTITY(identity), NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION); g_clear_error(&error);
	g_assert_false(venture_database_purge(f->db, VENTURE_ENTITY(identity), NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
}
typedef struct { gboolean done; GBytes *body; GError *error; } HttpResult;
static void http_done(GObject *object, GAsyncResult *result, gpointer data)
{
	HttpResult *out = data;
	out->body = soup_session_send_and_read_finish(SOUP_SESSION(object), result, &out->error); out->done = TRUE;
}
static void auth_probe(SoupServer *server, SoupServerMessage *message, const gchar *path, GHashTable *query, gpointer data)
{
	VentureAuth *auth = data;
	g_autoptr(HtmxRequest) request = htmx_request_new_from_message(message);
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autofree gchar *cookie = NULL;
	gboolean ok;
	(void)server; (void)query;
	if (!strcmp(path, "/challenge")) ok = venture_auth_mfa_challenge_user(auth, request) > 0;
	else if (!strcmp(path, "/complete")) {
		ok = venture_auth_complete_mfa(auth, request, htmx_request_get_form_value(request, "code"), "fixture-peer", &cookie, NULL);
		if (cookie) soup_message_headers_append(soup_server_message_get_response_headers(message), "Set-Cookie", cookie);
	} else { principal = venture_auth_authenticate(auth, request); ok = principal->authenticated; }
	soup_server_message_set_status(message, ok ? 200 : 401, NULL);
}
static guint probe(VentureAuth *auth, const gchar *path, const gchar *cookie, const gchar *form, gchar **new_cookie)
{
	g_autoptr(SoupServer) server = soup_server_new(NULL, NULL);
	g_autoptr(SoupSession) session = soup_session_new_with_options("timeout", 10, NULL);
	g_autoptr(SoupMessage) message = NULL;
	g_autofree gchar *url = NULL, *sent = NULL;
	g_autoptr(GError) error = NULL;
	GSList *uris;
	HttpResult result;
	result.done = FALSE; result.body = NULL; result.error = NULL;
	soup_server_add_handler(server, NULL, auth_probe, auth, NULL);
	g_assert_true(soup_server_listen_local(server, 0, SOUP_SERVER_LISTEN_IPV4_ONLY, &error)); g_assert_no_error(error);
	uris = soup_server_get_uris(server); url = g_strdup_printf("http://127.0.0.1:%d%s", g_uri_get_port(uris->data), path);
	g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);
	message = soup_message_new(form ? "POST" : "GET", url);
	if (cookie) { sent = g_strndup(cookie, strcspn(cookie, ";")); soup_message_headers_append(soup_message_get_request_headers(message), "Cookie", sent); }
	if (form) { g_autoptr(GBytes) bytes = g_bytes_new(form, strlen(form)); soup_message_set_request_body_from_bytes(message, "application/x-www-form-urlencoded", bytes); }
	soup_session_send_and_read_async(session, message, G_PRIORITY_DEFAULT, NULL, http_done, &result);
	while (!result.done) g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(result.error); g_clear_pointer(&result.body, g_bytes_unref);
	if (new_cookie) *new_cookie = g_strdup(soup_message_headers_get_one(soup_message_get_response_headers(message), "Set-Cookie"));
	soup_server_disconnect(server); return soup_message_get_status(message);
}
static void test_provider_session_revocation(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureOidcIdentity) identity = link_identity(f);
	g_autoptr(VentureAuth) auth = venture_auth_new(f->context);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *cookie = NULL, *local = NULL;
	gboolean pending = FALSE;
	(void)data;
	g_assert_true(venture_auth_login_identity(auth, venture_entity_get_id(VENTURE_ENTITY(identity)), &cookie, &pending, &error));
	g_assert_no_error(error); g_assert_false(pending); g_assert_cmpuint(probe(auth, "/session", cookie, NULL, NULL), ==, 200);
	g_assert_true(venture_integration_service_disable(venture_integration_service_get(f->db), f->org,
		venture_entity_get_id(VENTURE_ENTITY(f->binding)), venture_entity_get_version(VENTURE_ENTITY(f->binding)), NULL, &error));
	g_assert_no_error(error); g_assert_cmpuint(probe(auth, "/session", cookie, NULL, NULL), ==, 401);
	/* Provider outage or revocation never removes independent local recovery. */
	g_assert_true(venture_auth_login(auth, "local-member", "fixture-password", "fixture-peer", &local, &error));
	g_assert_no_error(error); g_assert_cmpuint(probe(auth, "/session", local, NULL, NULL), ==, 200);
}
static void test_unlink_revokes(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureOidcIdentity) identity = link_identity(f);
	g_autoptr(VentureAuth) auth = venture_auth_new(f->context);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *cookie = NULL;
	gboolean pending = FALSE;
	gint64 id = venture_entity_get_id(VENTURE_ENTITY(identity));
	(void)data;
	g_assert_true(venture_auth_login_identity(auth, id, &cookie, &pending, &error)); g_assert_no_error(error);
	g_assert_false(venture_oidc_service_unlink(f->service, id, f->user, "wrong", NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_UNAUTHENTICATED); g_clear_error(&error);
	g_assert_cmpuint(probe(auth, "/session", cookie, NULL, NULL), ==, 200);
	g_assert_true(venture_oidc_service_unlink(f->service, id, f->user, "fixture-password", NULL, &error)); g_assert_no_error(error);
	g_assert_cmpuint(probe(auth, "/session", cookie, NULL, NULL), ==, 401);
}
static void revoke_provider_on_user_save(VentureDatabase *database, VentureEntity *entity, gboolean created, gpointer data)
{
	Fixture *f = data;
	g_autoptr(GError) error = NULL;
	(void)database; (void)created;
	if (!VENTURE_IS_USER(entity)) return;
	g_assert_true(venture_integration_service_disable(venture_integration_service_get(f->db), f->org,
		venture_entity_get_id(VENTURE_ENTITY(f->binding)), venture_entity_get_version(VENTURE_ENTITY(f->binding)), NULL, &error));
	g_assert_no_error(error);
}
static void test_mfa_provenance_recovery(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureOidcIdentity) identity = link_identity(f);
	g_autoptr(VentureEntity) administrator = venture_database_get(f->db, VENTURE_TYPE_USER, f->user, NULL);
	g_autoptr(VentureAuth) auth = venture_auth_new(f->context);
	VentureMfaService *mfa = venture_mfa_service_get(f->db);
	g_autoptr(VentureMfaEnrolment) enrolment = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	g_autofree guchar *secret = NULL;
	g_autofree gchar *code = NULL, *challenge_cookie = NULL, *session = NULL, *pending_cookie = NULL, *form = NULL, *local_cookie = NULL;
	g_auto(GStrv) recovery = NULL;
	gsize length = 0;
	gboolean pending = FALSE;
	gint64 id = venture_entity_get_id(VENTURE_ENTITY(identity));
	(void)data;
	/* The emergency path must work for an explicitly granted local
	 * administrator, not just an ordinary account or an IdP role claim. */
	g_object_set(administrator, "role", VENTURE_USER_ROLE_OWNER, NULL);
	g_assert_true(venture_database_save(f->db, administrator, NULL, &error)); g_assert_no_error(error);
	enrolment = venture_mfa_service_begin_enrolment(mfa, f->user, "Fixture", "local-member", NULL, &error);
	g_assert_no_error(error); g_assert_nonnull(enrolment);
	secret = venture_base32_decode(enrolment->secret, &length); code = venture_totp_code(secret, length, venture_totp_counter(now), VENTURE_TOTP_DIGITS);
	recovery = venture_mfa_service_confirm_enrolment(mfa, f->user, code, NULL, &error); g_assert_no_error(error); g_assert_nonnull(recovery);
	g_assert_true(venture_auth_login_identity(auth, id, &challenge_cookie, &pending, &error));
	g_assert_no_error(error); g_assert_true(pending);
	g_assert_cmpuint(probe(auth, "/session", challenge_cookie, NULL, NULL), ==, 401);
	g_assert_cmpuint(probe(auth, "/challenge", challenge_cookie, NULL, NULL), ==, 200);
	form = soup_form_encode("code", recovery[0], NULL);
	if (data) {
		/* A synchronous save observer can revoke the provider while the
		 * successful factor is opening its session; success needs a cookie. */
		gulong handler = g_signal_connect(f->db, "entity-saved", G_CALLBACK(revoke_provider_on_user_save), f);
		g_assert_cmpuint(probe(auth, "/complete", challenge_cookie, form, &session), ==, 401);
		g_assert_null(session); g_signal_handler_disconnect(f->db, handler); return;
	}
	g_assert_cmpuint(probe(auth, "/complete", challenge_cookie, form, &session), ==, 200);
	g_assert_cmpuint(probe(auth, "/session", session, NULL, NULL), ==, 200);
	g_assert_true(venture_auth_login_identity(auth, id, &pending_cookie, &pending, &error)); g_assert_no_error(error); g_assert_true(pending);
	g_assert_true(venture_oidc_service_unlink(f->service, id, f->user, "fixture-password", NULL, &error)); g_assert_no_error(error);
	g_assert_cmpuint(probe(auth, "/session", session, NULL, NULL), ==, 401);
	g_assert_cmpuint(probe(auth, "/challenge", pending_cookie, NULL, NULL), ==, 401);
	g_clear_pointer(&form, g_free); form = soup_form_encode("code", recovery[1], NULL);
	g_assert_cmpuint(probe(auth, "/complete", pending_cookie, form, NULL), ==, 401);
	/* The refused provider challenge must not burn independent local recovery. */
	g_assert_true(venture_auth_login_with_mfa(auth, "local-member", "fixture-password", "fixture-peer", &local_cookie, &pending, &error));
	g_assert_no_error(error); g_assert_true(pending); g_clear_pointer(&session, g_free);
	g_assert_cmpuint(probe(auth, "/complete", local_cookie, form, &session), ==, 200);
	g_assert_cmpuint(probe(auth, "/session", session, NULL, NULL), ==, 200);
	g_test_message("Local administrator recovered with password and an unused MFA recovery code after provider unlinking");
}
static void test_reconnect_no_cookie_revival(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureOidcIdentity) identity = link_identity(f), relinked = NULL;
	g_autoptr(VentureAuth) auth = venture_auth_new(f->context);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *cookie = NULL, *fresh = NULL;
	gboolean pending = FALSE;
	(void)data;
	g_assert_true(venture_auth_login_identity(auth, venture_entity_get_id(VENTURE_ENTITY(identity)), &cookie, &pending, &error)); g_assert_no_error(error);
	g_assert_true(venture_integration_service_disable(venture_integration_service_get(f->db), f->org,
		venture_entity_get_id(VENTURE_ENTITY(f->binding)), venture_entity_get_version(VENTURE_ENTITY(f->binding)), NULL, &error)); g_assert_no_error(error);
	g_clear_object(&f->binding);
	f->binding = venture_oidc_service_connect(f->service, f->org, f->provider.issuer, "venture-client", "fixture-client-secret", 0, NULL, &error);
	g_assert_no_error(error); g_assert_nonnull(f->binding); relinked = link_identity(f);
	g_assert_cmpint(venture_entity_get_id(VENTURE_ENTITY(identity)), ==, venture_entity_get_id(VENTURE_ENTITY(relinked)));
	g_assert_cmpuint(probe(auth, "/session", cookie, NULL, NULL), ==, 401);
	g_assert_true(venture_auth_login_identity(auth, venture_entity_get_id(VENTURE_ENTITY(relinked)), &fresh, &pending, &error)); g_assert_no_error(error);
	g_assert_cmpuint(probe(auth, "/session", fresh, NULL, NULL), ==, 200);
}
static void test_deadline(Fixture *f, gconstpointer data)
{
	g_autofree gchar *browser = NULL, *uuid = NULL, *url = NULL;
	g_autoptr(GHashTable) params = NULL;
	g_autoptr(VentureOidcIdentity) result = NULL;
	g_autoptr(GError) error = NULL;
	gint64 started, elapsed;
	gboolean jwks = GPOINTER_TO_INT(data);
	if (jwks) params = begin(f, f->user, &browser);
	g_mutex_lock(&f->provider.mutex);
	if (jwks) f->provider.stall_jwks = TRUE; else f->provider.stall_discovery = TRUE;
	g_mutex_unlock(&f->provider.mutex);
	started = g_get_monotonic_time();
	if (jwks) result = venture_oidc_service_finish(f->service, g_hash_table_lookup(params, "state"), "fixture-code", browser, f->user, &error);
	else {
		g_object_get(f->binding, "uuid", &uuid, NULL);
		url = venture_oidc_service_begin(f->service, uuid, f->user, "fixture-password", "fixture-peer", &browser, &error);
	}
	elapsed = g_get_monotonic_time() - started;
	g_assert_null(url); g_assert_null(result); g_assert_nonnull(error);
	/* The library/transport idle timeout is ten seconds. This shorter bound
	 * specifically proves the application's total cancellation deadline. */
	g_assert_cmpint(elapsed, >=, 4 * G_USEC_PER_SEC);
	g_assert_cmpint(elapsed, <, 8 * G_USEC_PER_SEC);
}
static void test_password_failure_budget(Fixture *f, gconstpointer data)
{
	g_autofree gchar *uuid = NULL;
	guint i;
	(void)data;
	g_object_get(f->binding, "uuid", &uuid, NULL);
	for (i = 0; i < 10; i++) {
		g_autofree gchar *browser = NULL, *url = NULL;
		g_autoptr(GError) error = NULL;
		url = venture_oidc_service_begin(f->service, uuid, f->user, "wrong", "fixture-peer", &browser, &error);
		g_assert_null(url); g_assert_nonnull(error);
	}
	{
		g_autofree gchar *browser = NULL, *url = NULL;
		g_autoptr(GError) error = NULL;
		url = venture_oidc_service_begin(f->service, uuid, f->user, "fixture-password", "fixture-peer", &browser, &error);
		g_assert_null(url); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_UNAUTHENTICATED);
	}
}
/* Opt-in integration against a disposable, imported Keycloak realm. This
 * drives its real browser authorization-code login, including the cookie jar. */
/* Keycloak treats loopback as a browser-trusted secure context and emits
 * Secure cookies over this explicit HTTP fixture. libsoup is not a browser:
 * normalize only these disposable fixture cookies, never production traffic. */
static void keycloak_fixture_cookies(SoupSession *session, SoupMessage *message)
{
	SoupCookieJar *jar = SOUP_COOKIE_JAR(soup_session_get_feature(session, SOUP_TYPE_COOKIE_JAR));
	GSList *cookies = soup_cookies_from_response(message), *iter;
	g_assert_cmpstr(g_uri_get_scheme(soup_message_get_uri(message)), ==, "http");
	g_assert_cmpstr(g_uri_get_host(soup_message_get_uri(message)), ==, "127.0.0.1");
	for (iter = cookies; iter; iter = iter->next) {
		SoupCookie *cookie = soup_cookie_copy(iter->data);
		soup_cookie_set_secure(cookie, FALSE);
		soup_cookie_set_same_site_policy(cookie, SOUP_SAME_SITE_POLICY_LAX);
		soup_cookie_jar_add_cookie(jar, cookie);
	}
	soup_cookies_free(cookies);
}
static GHashTable *keycloak_response(SoupSession *session, const gchar *authorization)
{
	g_autoptr(SoupMessage) message = soup_message_new("GET", authorization);
	g_autoptr(GBytes) bytes = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GUri) uri = NULL;
	const gchar *location;
	soup_message_set_first_party(message, soup_message_get_uri(message));
	soup_message_set_is_top_level_navigation(message, TRUE);
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	bytes = soup_session_send_and_read(session, message, NULL, &error); g_assert_no_error(error); g_assert_nonnull(bytes);
	keycloak_fixture_cookies(session, message);
	if (soup_message_get_status(message) == 200) {
		xmlDoc *document;
		xmlXPathContext *context;
		xmlXPathObject *result;
		xmlChar *action;
		g_autoptr(SoupMessage) submit = NULL;
		g_autofree gchar *form = soup_form_encode("username", "fixture-provider-user", "password", "fixture-provider-password", "credentialId", "", NULL);
		g_autoptr(GBytes) body = g_bytes_new(form, strlen(form)), response = NULL;
		gsize length;
		const gchar *html = g_bytes_get_data(bytes, &length);
		g_assert_cmpuint(length, <, 128 * 1024);
		document = htmlReadMemory(html, length, NULL, NULL, HTML_PARSE_NONET | HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING);
		g_assert_nonnull(document); context = xmlXPathNewContext(document);
		result = xmlXPathEvalExpression((const xmlChar *)"//form[@id='kc-form-login']/@action", context); g_assert_nonnull(result);
		action = xmlXPathCastToString(result); g_assert_nonnull(action);
		g_assert_true(g_str_has_prefix((const gchar *)action, g_getenv("VENTURE_TEST_OIDC_KEYCLOAK_ISSUER")));
		submit = soup_message_new("POST", (const gchar *)action);
		soup_message_set_first_party(submit, soup_message_get_uri(message));
		soup_message_set_site_for_cookies(submit, soup_message_get_uri(message));
		soup_message_set_is_top_level_navigation(submit, TRUE);
		xmlFree(action); xmlXPathFreeObject(result); xmlXPathFreeContext(context); xmlFreeDoc(document);
		soup_message_set_flags(submit, SOUP_MESSAGE_NO_REDIRECT);
		soup_message_set_request_body_from_bytes(submit, "application/x-www-form-urlencoded", body);
		response = soup_session_send_and_read(session, submit, NULL, &error); g_assert_no_error(error); g_assert_nonnull(response);
		keycloak_fixture_cookies(session, submit);
		g_set_object(&message, submit);
	}
	g_assert_cmpuint(soup_message_get_status(message), ==, 302);
	location = soup_message_headers_get_one(soup_message_get_response_headers(message), "Location");
	g_assert_nonnull(location); g_assert_true(g_str_has_prefix(location, "http://127.0.0.1:8748/auth/oidc/callback?"));
	uri = g_uri_parse(location, G_URI_FLAGS_NONE, &error); g_assert_no_error(error);
	return soup_form_decode(g_uri_get_query(uri));
}
static void configure_keycloak_fixture(Fixture *f, const gchar *issuer)
{
	const gchar *issuers[2];
	g_autoptr(GError) error = NULL;
	g_assert_true(g_str_has_prefix(issuer, "http://127.0.0.1:"));
	g_assert_true(g_str_has_suffix(issuer, "/realms/venture-125-fixture"));
	issuers[0] = issuer; issuers[1] = NULL;
	g_object_set(f->config, "oidc-allowed-issuers", issuers, NULL);
	g_assert_true(venture_integration_service_disable(venture_integration_service_get(f->db), f->org,
		venture_entity_get_id(VENTURE_ENTITY(f->binding)), venture_entity_get_version(VENTURE_ENTITY(f->binding)), NULL, &error)); g_assert_no_error(error);
	g_clear_object(&f->binding); f->binding = venture_oidc_service_connect(f->service, f->org, issuer,
		"venture-client", "fixture-client-secret", 0, NULL, &error); g_assert_no_error(error); g_assert_nonnull(f->binding);
}
static void test_real_keycloak(Fixture *f, gconstpointer data)
{
	const gchar *issuer = g_getenv("VENTURE_TEST_OIDC_KEYCLOAK_ISSUER");
	g_autoptr(GError) error = NULL;
	g_autoptr(SoupSession) session = soup_session_new_with_options("timeout", 10, NULL);
	g_autoptr(SoupCookieJar) jar = soup_cookie_jar_new();
	g_autoptr(VentureOidcIdentity) linked = NULL;
	g_autofree gchar *uuid = NULL;
	guint i;
	(void)data;
	if (!issuer || !*issuer) { g_test_skip("Set VENTURE_TEST_OIDC_KEYCLOAK_ISSUER only for the disposable Keycloak fixture"); return; }
	configure_keycloak_fixture(f, issuer);
	g_object_get(f->binding, "uuid", &uuid, NULL); soup_session_add_feature(session, SOUP_SESSION_FEATURE(jar));
	for (i = 0; i < 2; i++) {
		g_autofree gchar *browser = NULL, *authorization = NULL;
		g_autoptr(GHashTable) response = NULL;
		g_autoptr(VentureOidcIdentity) identity = NULL;
		authorization = venture_oidc_service_begin(f->service, uuid, i ? 0 : f->user, i ? NULL : "fixture-password", "fixture-peer", &browser, &error);
		g_assert_no_error(error); g_assert_nonnull(authorization);
		response = keycloak_response(session, authorization);
		identity = venture_oidc_service_finish(f->service, g_hash_table_lookup(response, "state"),
			g_hash_table_lookup(response, "code"), browser, i ? 0 : f->user, &error);
		g_assert_no_error(error); g_assert_nonnull(identity);
		if (!i) g_set_object(&linked, identity);
		else g_assert_cmpint(venture_entity_get_id(VENTURE_ENTITY(identity)), ==, venture_entity_get_id(VENTURE_ENTITY(linked)));
	}
	g_test_message("Keycloak authorization-code + PKCE link and subsequent SSO resolved one existing local identity");
}
static void test_organization_isolation(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureOidcIdentity) first = link_identity(f), second = NULL;
	g_autoptr(VentureOrganization) org = venture_organization_new();
	g_autoptr(VentureUser) user = venture_user_new(), resolved = NULL;
	g_autoptr(VentureOrganizationMembership) membership = venture_organization_membership_new();
	g_autoptr(VentureIntegrationConnection) binding = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *uuid = NULL, *browser = NULL, *url = NULL;
	VentureAuthPrincipal principal;
	gint64 other_org, other_user;
	(void)data;
	g_object_set(org, "name", "Other OIDC organization", NULL); g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(org), NULL, &error)); g_assert_no_error(error);
	other_org = venture_entity_get_id(VENTURE_ENTITY(org));
	binding = venture_oidc_service_connect(f->service, other_org, f->provider.issuer, "venture-client", "fixture-client-secret", 0, NULL, &error); g_assert_no_error(error);
	g_object_get(binding, "uuid", &uuid, NULL);
	url = venture_oidc_service_begin(f->service, uuid, f->user, "fixture-password", "other-peer", &browser, &error);
	g_assert_null(url); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_UNAUTHENTICATED); g_clear_error(&error);
	g_object_set(user, "username", "other-member", "active", TRUE, "role", VENTURE_USER_ROLE_VIEWER, NULL);
	g_assert_true(venture_user_set_password(user, "fixture-password", 100000, &error)); g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(user), NULL, &error)); g_assert_no_error(error);
	other_user = venture_entity_get_id(VENTURE_ENTITY(user)); venture_entity_set_organization_id(VENTURE_ENTITY(membership), other_org);
	g_object_set(membership, "user-id", other_user, "role", VENTURE_ORGANIZATION_ROLE_VIEWER, "active", TRUE, NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(membership), NULL, &error)); g_assert_no_error(error);
	g_set_object(&f->binding, binding); f->user = other_user;
	second = link_identity(f);
	g_assert_cmpint(venture_entity_get_id(VENTURE_ENTITY(first)), !=, venture_entity_get_id(VENTURE_ENTITY(second)));
	g_assert_cmpint(venture_entity_get_organization_id(VENTURE_ENTITY(second)), ==, other_org);
	principal.user_id = other_user; principal.token_id = 0; principal.role = VENTURE_USER_ROLE_VIEWER; principal.name = NULL; principal.authenticated = TRUE;
	g_assert_false(venture_access_policy_can(venture_database_get_access_policy(f->db), &principal, "read", VENTURE_ENTITY(first), NULL));
	resolved = venture_oidc_service_identity_user(f->service, venture_entity_get_id(VENTURE_ENTITY(second)), &error); g_assert_no_error(error);
	g_assert_cmpint(venture_entity_get_id(VENTURE_ENTITY(resolved)), ==, other_user);
}
static void test_local_authority_revocation(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureOidcIdentity) identity = link_identity(f);
	g_autoptr(VentureAuth) auth = venture_auth_new(f->context);
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP);
	g_autoptr(VentureEntity) membership = NULL, user = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *cookie = NULL;
	gboolean pending = FALSE;
	(void)data;
	g_assert_true(venture_auth_login_identity(auth, venture_entity_get_id(VENTURE_ENTITY(identity)), &cookie, &pending, &error)); g_assert_no_error(error);
	venture_query_add_filter_int(query, "user-id", VENTURE_FILTER_OP_EQ, f->user, NULL);
	membership = venture_database_find_one(f->db, query, &error); g_assert_no_error(error); g_assert_nonnull(membership);
	g_object_set(membership, "active", FALSE, NULL); g_assert_true(venture_database_save(f->db, membership, NULL, &error)); g_assert_no_error(error);
	g_assert_cmpuint(probe(auth, "/session", cookie, NULL, NULL), ==, 401);
	g_object_set(membership, "active", TRUE, NULL); g_assert_true(venture_database_save(f->db, membership, NULL, &error)); g_assert_no_error(error);
	user = venture_database_get(f->db, VENTURE_TYPE_USER, f->user, &error); g_assert_no_error(error);
	g_object_set(user, "active", FALSE, NULL); g_assert_true(venture_database_save(f->db, user, NULL, &error)); g_assert_no_error(error);
	g_assert_cmpuint(probe(auth, "/session", cookie, NULL, NULL), ==, 401);
}
static void test_defaults_allowlist(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureConfig) defaults = venture_config_new();
	g_autoptr(VentureIntegrationConnection) binding = NULL;
	g_autoptr(VentureAuth) auth = venture_auth_new(f->context);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *uuid = NULL, *browser = NULL, *url = NULL, *local = NULL;
	gboolean enabled = TRUE, loopback = TRUE;
	g_auto(GStrv) issuers = NULL;
	(void)data;
	g_object_get(defaults, "oidc-enabled", &enabled, "oidc-allow-loopback", &loopback, "oidc-allowed-issuers", &issuers, NULL);
	g_assert_false(enabled); g_assert_false(loopback); g_assert_true(!issuers || !issuers[0]);
	binding = venture_oidc_service_connect(f->service, f->org, "https://unapproved.invalid", "venture-client", "fixture-client-secret", 0, NULL, &error);
	g_assert_null(binding); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG); g_clear_error(&error);
	g_object_set(f->config, "oidc-enabled", FALSE, NULL); g_object_get(f->binding, "uuid", &uuid, NULL);
	url = venture_oidc_service_begin(f->service, uuid, 0, NULL, "fixture-peer", &browser, &error);
	g_assert_null(url); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG); g_clear_error(&error);
	g_assert_true(venture_auth_login(auth, "local-member", "fixture-password", "fixture-peer", &local, &error)); g_assert_no_error(error);
	g_assert_cmpuint(probe(auth, "/session", local, NULL, NULL), ==, 200);
}
static SoupMessage *web_request(SoupSession *session, guint port_number, const gchar *path, const gchar *cookie, const gchar *form)
{
	g_autofree gchar *url = g_strdup_printf("http://127.0.0.1:%u%s", port_number, path);
	SoupMessage *message = soup_message_new(form ? "POST" : "GET", url);
	HttpResult result;
	result.done = FALSE; result.body = NULL; result.error = NULL;
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	if (cookie) soup_message_headers_append(soup_message_get_request_headers(message), "Cookie", cookie);
	if (form) { g_autoptr(GBytes) bytes = g_bytes_new(form, strlen(form)); soup_message_set_request_body_from_bytes(message, "application/x-www-form-urlencoded", bytes); }
	soup_session_send_and_read_async(session, message, G_PRIORITY_DEFAULT, NULL, http_done, &result);
	while (!result.done) g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(result.error); g_object_set_data_full(G_OBJECT(message), "fixture-body", result.body, (GDestroyNotify)g_bytes_unref);
	return message;
}
static gchar *response_cookie(SoupMessage *message, const gchar *name)
{
	GSList *cookies = soup_cookies_from_response(message), *iter;
	gchar *result = NULL;
	for (iter = cookies; iter; iter = iter->next)
		if (!g_strcmp0(soup_cookie_get_name(iter->data), name)) result = soup_cookie_to_cookie_header(iter->data);
	soup_cookies_free(cookies); return result;
}
static gchar *web_callback_path(Fixture *f, SoupMessage *message, SoupSession *session, gboolean real_provider)
{
	const gchar *location = soup_message_headers_get_one(soup_message_get_response_headers(message), "Location");
	g_autoptr(GUri) uri = g_uri_parse(location, G_URI_FLAGS_NONE, NULL);
	g_autoptr(GHashTable) query = soup_form_decode(g_uri_get_query(uri));
	g_autofree gchar *form = soup_form_encode("state", g_hash_table_lookup(query, "state"), "code", "fixture-code", NULL);
	if (real_provider) {
		g_autoptr(GHashTable) response = keycloak_response(session, location);
		g_clear_pointer(&form, g_free);
		form = soup_form_encode("state", g_hash_table_lookup(response, "state"), "code", g_hash_table_lookup(response, "code"), NULL);
		return g_strconcat("/auth/oidc/callback?", form, NULL);
	}
	g_mutex_lock(&f->provider.mutex);
	g_free(f->provider.nonce); f->provider.nonce = g_strdup(g_hash_table_lookup(query, "nonce"));
	g_free(f->provider.pkce); f->provider.pkce = g_strdup(g_hash_table_lookup(query, "code_challenge"));
	g_mutex_unlock(&f->provider.mutex);
	return g_strconcat("/auth/oidc/callback?", form, NULL);
}
static void test_browser_workflow(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureWebServer) server = NULL;
	g_autoptr(VentureAuth) auth = venture_auth_new(f->context);
	g_autoptr(SoupSession) session = soup_session_new_with_options("timeout", 15, NULL);
	g_autoptr(SoupSession) provider_session = NULL;
	g_autoptr(SoupCookieJar) jar = NULL;
	g_autoptr(SoupMessage) message = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *local = NULL, *local_header = NULL, *uuid = NULL, *form = NULL, *browser = NULL, *callback = NULL, *cookies = NULL, *path = NULL, *signed_in = NULL, *cleared = NULL;
	guint port_number = 41000 + ((guint)getpid() % 10000);
	gboolean real_provider = GPOINTER_TO_INT(data);
	if (real_provider) {
		const gchar *issuer = g_getenv("VENTURE_TEST_OIDC_KEYCLOAK_ISSUER");
		if (!issuer || !*issuer) { g_test_skip("Use the disposable Keycloak harness for the combined browser flow"); return; }
		/* Keep the provider jar from replacing explicit Venture cookies. */
		configure_keycloak_fixture(f, issuer); provider_session = soup_session_new_with_options("timeout", 15, NULL);
		jar = soup_cookie_jar_new(); soup_session_add_feature(provider_session, SOUP_SESSION_FEATURE(jar));
	}
	g_object_set(f->config, "server-bind-address", "127.0.0.1", "server-port", (gint64)port_number, NULL);
	server = venture_web_server_new(f->context, &error); g_assert_no_error(error);
	g_assert_true(venture_web_server_start(server, &error)); g_assert_no_error(error);
	g_assert_true(venture_auth_login(auth, "local-member", "fixture-password", "fixture-peer", &local, &error)); g_assert_no_error(error);
	local_header = g_strndup(local, strcspn(local, ";"));
	message = web_request(session, port_number, "/account/oidc", local_header, NULL); g_assert_cmpuint(soup_message_get_status(message), ==, 200);
	g_test_message("Local member opened provider linking page: HTTP 200");
	g_clear_object(&message); path = g_strdup_printf("/organizations/%" G_GINT64_FORMAT "/settings/oidc", f->org);
	message = web_request(session, port_number, path, local_header, NULL); g_assert_cmpuint(soup_message_get_status(message), ==, 403);
	g_test_message("Ordinary member cannot administer organization provider credentials: HTTP 403");
	g_clear_object(&message); g_object_get(f->binding, "uuid", &uuid, NULL);
	form = soup_form_encode("provider", uuid, "password", "fixture-password", NULL);
	message = web_request(session, port_number, "/account/oidc/link", local_header, form); g_assert_cmpuint(soup_message_get_status(message), ==, 302);
	browser = response_cookie(message, "venture_oidc"); g_assert_nonnull(browser); callback = web_callback_path(f, message, provider_session, real_provider);
	cookies = g_strconcat(local_header, "; ", browser, NULL); g_clear_object(&message);
	message = web_request(session, port_number, callback, cookies, NULL); g_assert_cmpuint(soup_message_get_status(message), ==, 302);
	g_assert_cmpstr(soup_message_headers_get_one(soup_message_get_response_headers(message), "Location"), ==, "/account/oidc");
	g_test_message("Password-confirmed linking callback returned to account settings: HTTP 302");
	g_clear_object(&message); g_clear_pointer(&path, g_free); path = g_strdup_printf("/auth/oidc/start?provider=%s", uuid);
	message = web_request(session, port_number, path, NULL, NULL); g_assert_cmpuint(soup_message_get_status(message), ==, 302);
	g_clear_pointer(&browser, g_free); browser = response_cookie(message, "venture_oidc");
	g_clear_pointer(&callback, g_free); callback = web_callback_path(f, message, provider_session, real_provider); g_clear_object(&message);
	message = web_request(session, port_number, callback, browser, NULL); g_assert_cmpuint(soup_message_get_status(message), ==, 302);
	signed_in = response_cookie(message, "venture_session"); cleared = response_cookie(message, "venture_oidc");
	g_assert_nonnull(signed_in); g_assert_cmpstr(cleared, ==, "venture_oidc=");
	g_test_message("Subsequent SSO callback issued a session and cleared its browser proof in separate cookies");
	g_clear_object(&message); message = web_request(session, port_number, "/account/oidc", signed_in, NULL);
	g_assert_cmpuint(soup_message_get_status(message), ==, 200);
	venture_web_server_stop(server);
	g_test_message("Provider session opened the authenticated account page: HTTP 200");
	if (real_provider) g_test_message("Combined demonstration used real Venture HTTP routes and Keycloak 26.7.3 authorization-code/PKCE login");
}
static void test_session_expiry_signout(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureOidcIdentity) identity = link_identity(f);
	g_autoptr(VentureAuth) auth = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *cookie = NULL;
	gboolean pending = FALSE;
	gint64 id = venture_entity_get_id(VENTURE_ENTITY(identity));
	(void)data;
	g_object_set(f->config, "security-session-lifetime", (gint64)1, NULL); auth = venture_auth_new(f->context);
	g_assert_true(venture_auth_login_identity(auth, id, &cookie, &pending, &error)); g_assert_no_error(error);
	g_usleep(1100000);
	g_assert_cmpuint(probe(auth, "/session", cookie, NULL, NULL), ==, 401);
	g_object_set(f->config, "security-session-lifetime", (gint64)3600, NULL); g_clear_object(&auth); auth = venture_auth_new(f->context);
	g_clear_pointer(&cookie, g_free);
	g_assert_true(venture_auth_login_identity(auth, id, &cookie, &pending, &error)); g_assert_no_error(error);
	g_assert_true(venture_auth_end_sessions(auth, f->user, &error)); g_assert_no_error(error);
	g_assert_cmpuint(probe(auth, "/session", cookie, NULL, NULL), ==, 401);
	g_test_message("Expired provider sessions and sessions predating local sign-out both return HTTP 401");
}
int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_setenv("VENTURE_TEST_OIDC_SESSION", "isolated-oidc-fixture-session", TRUE);
	g_setenv("VENTURE_TEST_OIDC_MFA", "isolated-oidc-fixture-mfa", TRUE);
	g_test_add_func("/oidc/identity-registered", test_identity_registered);
	g_test_add("/oidc/link-signin-replay", Fixture, NULL, setup, test_link_signin_replay, teardown);
	g_test_add("/oidc/unlinked-refused", Fixture, NULL, setup, test_unlinked_refused, teardown);
	g_test_add("/oidc/invalid-claims", Fixture, NULL, setup, test_invalid_claims, teardown);
	g_test_add("/oidc/browser-binding", Fixture, NULL, setup, test_browser_binding, teardown);
	g_test_add("/oidc/binding-rotation", Fixture, NULL, setup, test_binding_rotation, teardown);
	g_test_add("/oidc/generic-writes", Fixture, NULL, setup, test_generic_writes, teardown);
	g_test_add("/oidc/provider-session-revocation", Fixture, NULL, setup, test_provider_session_revocation, teardown);
	g_test_add("/oidc/unlink-revokes", Fixture, NULL, setup, test_unlink_revokes, teardown);
	g_test_add("/oidc/mfa-provenance-recovery", Fixture, NULL, setup, test_mfa_provenance_recovery, teardown);
	g_test_add("/oidc/mfa-binding-changed-during-completion", Fixture, GINT_TO_POINTER(1), setup, test_mfa_provenance_recovery, teardown);
	g_test_add("/oidc/reconnect-no-cookie-revival", Fixture, NULL, setup, test_reconnect_no_cookie_revival, teardown);
	g_test_add("/oidc/discovery-deadline", Fixture, NULL, setup, test_deadline, teardown);
	g_test_add("/oidc/jwks-deadline", Fixture, GINT_TO_POINTER(1), setup, test_deadline, teardown);
	g_test_add("/oidc/password-failure-budget", Fixture, NULL, setup, test_password_failure_budget, teardown);
	g_test_add("/oidc/real-keycloak", Fixture, NULL, setup, test_real_keycloak, teardown);
	g_test_add("/oidc/organization-isolation", Fixture, NULL, setup, test_organization_isolation, teardown);
	g_test_add("/oidc/local-authority-revocation", Fixture, NULL, setup, test_local_authority_revocation, teardown);
	g_test_add("/oidc/defaults-allowlist", Fixture, NULL, setup, test_defaults_allowlist, teardown);
	g_test_add("/oidc/browser-workflow", Fixture, NULL, setup, test_browser_workflow, teardown);
	g_test_add("/oidc/session-expiry-signout", Fixture, NULL, setup, test_session_expiry_signout, teardown);
	g_test_add("/oidc/browser-keycloak", Fixture, GINT_TO_POINTER(1), setup, test_browser_workflow, teardown);
	return g_test_run();
}
