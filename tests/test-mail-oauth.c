/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Token mailbox sign-in (docs/mail.org, Token sign-in): the authorization
 * code flow with PKCE, refresh, a revoked grant, and a sync that signs in
 * with XOAUTH2. The provider is a fake OAuth server on loopback in its own
 * thread (the code under test blocks the main loop on the token endpoint,
 * so a server on the same context would never answer); the mailbox is the
 * fake IMAP client. Nothing leaves the machine. */
#include <venture.h>
#include <string.h>
#include <libsoup/soup.h>
#include "venture-test-util.h"
#include "venture-test-accounting.h"

#define CLIENT_ID "venture-test-client"
#define CLIENT_SECRET "venture-test-secret"
#define REDIRECT "https://venture.example.com/mail/oauth/callback"

typedef struct {
	GThread *thread;
	GMainContext *context;
	GMainLoop *loop;
	SoupServer *server;
	GMutex lock;
	GCond ready;
	gboolean started;
	guint16 port;
	/* Read and written under the lock. */
	gchar *challenge;
	gchar *code;
	gchar *refresh;
	gint issued;
	gint64 expires_in;
	gboolean revoked;
	guint exchanges;
	guint refreshes;
} FakeOauth;

typedef struct {
	VentureDatabase *db;
	VentureConfig *config;
	VentureFakeImapClient *imap;
	VentureMailSyncService *service;
	FakeOauth oauth;
	gchar *root;
	gint64 org;
	gint64 contact;
	VentureEntity *provider;
	VentureEntity *account;
} Fixture;

/* BASE64URL without padding, computed here rather than by the code under
 * test so the server checks the verifier independently. */
static gchar *
b64url_sha256(const gchar *text)
{
	g_autoptr(GChecksum) sum = g_checksum_new(G_CHECKSUM_SHA256);
	guint8 digest[32];
	gsize length = sizeof digest;
	gchar *encoded, *p;
	g_checksum_update(sum, (const guchar *)text, -1);
	g_checksum_get_digest(sum, digest, &length);
	encoded = g_base64_encode(digest, length);
	for (p = encoded; *p; p++) { if (*p == '+') *p = '-'; else if (*p == '/') *p = '_'; }
	p = strchr(encoded, '=');
	if (p) *p = '\0';
	return encoded;
}

static void
oauth_reply(SoupServerMessage *message, guint status, const gchar *json)
{
	soup_server_message_set_status(message, status, NULL);
	soup_server_message_set_response(message, "application/json", SOUP_MEMORY_COPY, json, strlen(json));
}

static void
oauth_token(SoupServer *server, SoupServerMessage *message, const gchar *path, GHashTable *query, gpointer data)
{
	FakeOauth *f = data;
	g_autoptr(GBytes) body = soup_message_body_flatten(soup_server_message_get_request_body(message));
	g_autofree gchar *text = g_strndup(g_bytes_get_data(body, NULL), g_bytes_get_size(body));
	g_autoptr(GHashTable) form = soup_form_decode(text);
	const gchar *grant = g_hash_table_lookup(form, "grant_type");
	g_autofree gchar *reply = NULL;
	(void)server; (void)path; (void)query;
	g_mutex_lock(&f->lock);
	/* The client authenticates in the body (client_secret_post). A wrong
	 * secret is refused with a description that repeats it, the way real
	 * servers do; the client must never show that text. */
	if (g_strcmp0(g_hash_table_lookup(form, "client_id"), CLIENT_ID) ||
		g_strcmp0(g_hash_table_lookup(form, "client_secret"), CLIENT_SECRET))
		oauth_reply(message, 401, "{\"error\":\"invalid_client\",\"error_description\":\"bad secret echo-secret\"}");
	else if (!g_strcmp0(grant, "authorization_code")) {
		g_autofree gchar *derived = NULL;
		const gchar *verifier = g_hash_table_lookup(form, "code_verifier");
		f->exchanges++;
		derived = verifier ? b64url_sha256(verifier) : NULL;
		if (!f->code || g_strcmp0(g_hash_table_lookup(form, "code"), f->code) ||
			g_strcmp0(g_hash_table_lookup(form, "redirect_uri"), REDIRECT) || !derived || g_strcmp0(derived, f->challenge))
			oauth_reply(message, 400, "{\"error\":\"invalid_grant\",\"error_description\":\"echo-code-refused\"}");
		else {
			g_clear_pointer(&f->code, g_free);
			f->issued++;
			g_free(f->refresh);
			f->refresh = g_strdup_printf("refresh-%d", f->issued);
			reply = g_strdup_printf("{\"access_token\":\"access-%d\",\"refresh_token\":\"%s\",\"expires_in\":%" G_GINT64_FORMAT ",\"token_type\":\"Bearer\"}",
				f->issued, f->refresh, f->expires_in);
			oauth_reply(message, 200, reply);
		}
	} else if (!g_strcmp0(grant, "refresh_token")) {
		f->refreshes++;
		if (f->revoked || !f->refresh || g_strcmp0(g_hash_table_lookup(form, "refresh_token"), f->refresh))
			oauth_reply(message, 400, "{\"error\":\"invalid_grant\",\"error_description\":\"Token has been revoked: echo-refresh\"}");
		else {
			f->issued++;
			g_free(f->refresh);
			f->refresh = g_strdup_printf("refresh-%d", f->issued);
			reply = g_strdup_printf("{\"access_token\":\"access-%d\",\"refresh_token\":\"%s\",\"expires_in\":%" G_GINT64_FORMAT ",\"token_type\":\"Bearer\"}",
				f->issued, f->refresh, f->expires_in);
			oauth_reply(message, 200, reply);
		}
	} else oauth_reply(message, 400, "{\"error\":\"unsupported_grant_type\"}");
	g_mutex_unlock(&f->lock);
}

static gpointer
oauth_serve(gpointer data)
{
	FakeOauth *f = data;
	g_autoptr(GError) error = NULL;
	GSList *uris;
	g_main_context_push_thread_default(f->context);
	f->server = soup_server_new(NULL, NULL);
	soup_server_add_handler(f->server, "/token", oauth_token, f, NULL);
	g_assert_true(soup_server_listen_local(f->server, 0, SOUP_SERVER_LISTEN_IPV4_ONLY, &error));
	uris = soup_server_get_uris(f->server);
	g_mutex_lock(&f->lock);
	f->port = (guint16)g_uri_get_port(uris->data);
	f->started = TRUE;
	g_cond_signal(&f->ready);
	g_mutex_unlock(&f->lock);
	g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);
	g_main_loop_run(f->loop);
	soup_server_disconnect(f->server);
	g_clear_object(&f->server);
	g_main_context_pop_thread_default(f->context);
	return NULL;
}

static void
oauth_start(FakeOauth *f)
{
	memset(f, 0, sizeof *f);
	g_mutex_init(&f->lock);
	g_cond_init(&f->ready);
	f->expires_in = 3600;
	f->context = g_main_context_new();
	f->loop = g_main_loop_new(f->context, FALSE);
	f->thread = g_thread_new("fake-oauth", oauth_serve, f);
	g_mutex_lock(&f->lock);
	while (!f->started) g_cond_wait(&f->ready, &f->lock);
	g_mutex_unlock(&f->lock);
}

static gboolean
oauth_quit(gpointer data)
{
	g_main_loop_quit(data);
	return G_SOURCE_REMOVE;
}

static void
oauth_stop(FakeOauth *f)
{
	GSource *source = g_idle_source_new();
	g_source_set_callback(source, oauth_quit, f->loop, NULL);
	g_source_attach(source, f->context);
	g_source_unref(source);
	g_thread_join(f->thread);
	g_main_loop_unref(f->loop);
	g_main_context_unref(f->context);
	g_free(f->challenge); g_free(f->code); g_free(f->refresh);
	g_mutex_clear(&f->lock);
	g_cond_clear(&f->ready);
}

static void
save(Fixture *f, VentureEntity *e)
{
	g_autoptr(GError) error = NULL;
	g_assert_true(venture_database_save(f->db, e, NULL, &error));
	g_assert_no_error(error);
}

static void
setup(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) key = g_bytes_new_static("01234567890123456789012345678901", 32);
	g_autoptr(VentureEntity) contact = NULL;
	g_autofree gchar *authorize = NULL, *token = NULL;
	(void)data;
	oauth_start(&f->oauth);
	f->config = venture_config_new();
	g_object_set(f->config, "imap-allowed-endpoints", "imap.example.net:993", "connectors-allow-plaintext-loopback", TRUE, NULL);
	f->db = venture_test_accounting_database(&error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	g_assert_true(venture_integration_service_set_key(venture_integration_service_get(f->db), key, &error));
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ORGANIZATION);
		g_autoptr(VentureEntity) org = venture_database_find_one(f->db, query, &error);
		f->org = venture_entity_get_id(org);
	}
	f->root = g_dir_make_tmp("venture-mail-oauth-XXXXXX", &error);
	g_assert_no_error(error);
	contact = g_object_new(VENTURE_TYPE_CONTACT, "organization-id", f->org, "name", "Ada", "email", "ada@example.com", NULL);
	save(f, contact);
	f->contact = venture_entity_get_id(contact);
	authorize = g_strdup_printf("http://127.0.0.1:%u/authorize", f->oauth.port);
	token = g_strdup_printf("http://127.0.0.1:%u/token", f->oauth.port);
	f->provider = g_object_new(VENTURE_TYPE_MAIL_OAUTH_PROVIDER, "organization-id", f->org, "name", "Example mail",
		"authorize-url", authorize, "token-url", token, "scopes", "mail offline", "active", TRUE, NULL);
	save(f, f->provider);
	f->account = g_object_new(VENTURE_TYPE_MAIL_ACCOUNT, "organization-id", f->org,
		"address", "ops@example.net", "imap-host", "imap.example.net", "imap-port", (gint64)993, "imap-tls", "tls",
		"username", "ops@example.net", "folders", "INBOX", "active", TRUE,
		"oauth-provider-id", venture_entity_get_id(f->provider), NULL);
	save(f, f->account);
	f->imap = venture_fake_imap_client_new();
	f->service = venture_mail_sync_service_new(f->db, VENTURE_IMAP_CLIENT(f->imap));
	g_object_set(f->service, "attachment-root", f->root, "config", f->config, NULL);
}

static void
teardown(Fixture *f, gconstpointer data)
{
	g_autofree gchar *real = realpath(f->root, NULL);
	(void)data;
	g_clear_object(&f->service);
	g_clear_object(&f->imap);
	g_clear_object(&f->account);
	g_clear_object(&f->provider);
	g_clear_object(&f->config);
	venture_test_accounting_database_cleanup(f->db); g_clear_object(&f->db);
	if (real) venture_test_remove_within(f->root, real);
	g_free(f->root);
	oauth_stop(&f->oauth);
}

static void
configure_client(Fixture *f)
{
	g_autoptr(GError) error = NULL;
	g_assert_false(venture_mail_oauth_client_configured(f->db, f->provider));
	g_assert_true(venture_mail_oauth_configure_client(f->db, f->config, f->provider, CLIENT_ID, CLIENT_SECRET, NULL, &error));
	g_assert_no_error(error);
	g_assert_true(venture_mail_oauth_client_configured(f->db, f->provider));
}

/* Starts a sign-in as user 7, plays the provider's consent screen (records
 * the challenge and issues a code) and returns the state. */
static gchar *
consent(Fixture *f, gchar **url_out)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *url = venture_mail_oauth_begin(f->db, f->config, f->account, REDIRECT, 7, &error);
	g_autoptr(GUri) uri = NULL;
	g_autoptr(GHashTable) params = NULL;
	gchar *state;
	g_assert_no_error(error);
	g_assert_nonnull(url);
	uri = g_uri_parse(url, G_URI_FLAGS_ENCODED_QUERY, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(g_uri_get_path(uri), ==, "/authorize");
	params = g_uri_parse_params(g_uri_get_query(uri), -1, "&", G_URI_PARAMS_NONE, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(g_hash_table_lookup(params, "response_type"), ==, "code");
	g_assert_cmpstr(g_hash_table_lookup(params, "client_id"), ==, CLIENT_ID);
	g_assert_cmpstr(g_hash_table_lookup(params, "redirect_uri"), ==, REDIRECT);
	g_assert_cmpstr(g_hash_table_lookup(params, "scope"), ==, "mail offline");
	g_assert_cmpstr(g_hash_table_lookup(params, "code_challenge_method"), ==, "S256");
	g_assert_cmpuint(strlen(g_hash_table_lookup(params, "code_challenge")), ==, 43);
	g_assert_cmpuint(strlen(g_hash_table_lookup(params, "state")), >=, 32);
	/* The secret never travels through the browser. */
	g_assert_null(g_hash_table_lookup(params, "client_secret"));
	g_assert_null(strstr(url, CLIENT_SECRET));
	g_mutex_lock(&f->oauth.lock);
	g_free(f->oauth.challenge);
	f->oauth.challenge = g_strdup(g_hash_table_lookup(params, "code_challenge"));
	g_free(f->oauth.code);
	f->oauth.code = g_strdup("granted-code");
	g_mutex_unlock(&f->oauth.lock);
	state = g_strdup(g_hash_table_lookup(params, "state"));
	if (url_out) *url_out = g_steal_pointer(&url);
	return state;
}

static gchar *
stored_token(Fixture *f, const gchar **mechanism_out)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureConnectorSession) session = venture_connector_open(f->db, f->config, f->account, &error);
	g_assert_no_error(error);
	g_assert_nonnull(session);
	if (mechanism_out) *mechanism_out = g_intern_string(venture_connector_session_get_mechanism(session));
	return g_strdup(venture_connector_session_get_password(session));
}

static void
signed_in(Fixture *f)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *state = NULL;
	configure_client(f);
	state = consent(f, NULL);
	g_assert_cmpint(venture_mail_oauth_complete(f->db, f->config, state, "granted-code", 7, NULL, &error), ==, venture_entity_get_id(f->account));
	g_assert_no_error(error);
}

/* The helpers are the published shapes: RFC 7636 appendix B's challenge
 * and the XOAUTH2 initial response, which refuses a control character
 * that would end the field early. */
static void
test_helpers(void)
{
	g_autofree gchar *challenge = venture_mail_oauth_pkce_challenge("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk");
	g_autofree gchar *response = venture_mail_oauth_xoauth2("ops@example.net", "ya29.token");
	g_autofree guchar *decoded = NULL;
	gsize length = 0;
	g_assert_cmpstr(challenge, ==, "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM");
	g_assert_nonnull(response);
	decoded = g_base64_decode(response, &length);
	g_assert_cmpmem(decoded, length, "user=ops@example.net\001auth=Bearer ya29.token\001\001", 45);
	g_assert_null(venture_mail_oauth_xoauth2("ops@example.net\001x", "t"));
	g_assert_null(venture_mail_oauth_xoauth2("ops@example.net", "t\r\nA1 LOGOUT"));
}

/* The code flow: a sign-in needs the client credential, the state is the
 * starting user's alone and is used once, the verifier proves the code
 * (the fake recomputes S256 itself), and the tokens become the mailbox's
 * connector credential with the XOAUTH2 mechanism. */
static void
test_pkce_sign_in(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *state = NULL, *token = NULL, *url = NULL;
	const gchar *mechanism = NULL;
	(void)data;
	g_assert_null(venture_mail_oauth_begin(f->db, f->config, f->account, REDIRECT, 7, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
	g_clear_error(&error);
	configure_client(f);
	state = consent(f, &url);
	/* Somebody else's browser cannot finish it, and trying does not spend it. */
	g_assert_cmpint(venture_mail_oauth_complete(f->db, f->config, state, "granted-code", 8, NULL, &error), ==, 0);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
	g_clear_error(&error);
	g_assert_cmpint(venture_mail_oauth_complete(f->db, f->config, "made-up-state", "granted-code", 7, NULL, &error), ==, 0);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
	g_clear_error(&error);
	g_assert_cmpint(venture_mail_oauth_complete(f->db, f->config, state, "granted-code", 7, NULL, &error), ==, venture_entity_get_id(f->account));
	g_assert_no_error(error);
	g_assert_cmpuint(f->oauth.exchanges, ==, 1);
	token = stored_token(f, &mechanism);
	g_assert_cmpstr(token, ==, "access-1");
	g_assert_cmpstr(mechanism, ==, "xoauth2");
	/* A state is used once. */
	g_assert_cmpint(venture_mail_oauth_complete(f->db, f->config, state, "granted-code", 7, NULL, &error), ==, 0);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
	g_clear_error(&error);
	/* A refused code says so without the server's words. */
	g_clear_pointer(&state, g_free);
	state = consent(f, NULL);
	g_assert_cmpint(venture_mail_oauth_complete(f->db, f->config, state, "stolen-code", 7, NULL, &error), ==, 0);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_UNAUTHENTICATED);
	g_assert_null(strstr(error->message, "echo"));
	/* A password account has no sign-in to start. */
	g_clear_error(&error);
	g_object_set(f->account, "oauth-provider-id", (gint64)0, NULL);
	g_assert_null(venture_mail_oauth_begin(f->db, f->config, f->account, REDIRECT, 7, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
}

/* Endpoints are the operator's, and plain HTTP is refused outside the
 * loopback test switch, so a client secret is never posted in the clear. */
static void
test_endpoint_policy(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	(void)data;
	g_object_set(f->config, "connectors-allow-plaintext-loopback", FALSE, NULL);
	g_assert_false(venture_mail_oauth_configure_client(f->db, f->config, f->provider, CLIENT_ID, CLIENT_SECRET, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
	g_clear_error(&error);
	g_object_set(f->config, "connectors-allow-plaintext-loopback", TRUE, NULL);
	g_object_set(f->provider, "token-url", "http://mail.example.net/token", NULL);
	save(f, f->provider);
	g_assert_false(venture_mail_oauth_configure_client(f->db, f->config, f->provider, CLIENT_ID, CLIENT_SECRET, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
}

/* Refresh: nothing is asked while the token is fresh; a forced or due
 * refresh stores the new access token and the rotated refresh token, and
 * the next refresh presents the rotated one. */
static void
test_refresh(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *token = NULL;
	(void)data;
	signed_in(f);
	g_assert_true(venture_mail_oauth_refresh(f->db, f->config, f->account, FALSE, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(f->oauth.refreshes, ==, 0);
	g_assert_true(venture_mail_oauth_refresh(f->db, f->config, f->account, TRUE, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(f->oauth.refreshes, ==, 1);
	token = stored_token(f, NULL);
	g_assert_cmpstr(token, ==, "access-2");
	g_clear_pointer(&token, g_free);
	/* The server rotated the refresh token; only the new one works. */
	g_assert_true(venture_mail_oauth_refresh(f->db, f->config, f->account, TRUE, &error));
	g_assert_no_error(error);
	token = stored_token(f, NULL);
	g_assert_cmpstr(token, ==, "access-3");
}

/* A revoked grant is a refused sign-in, in words that say what to do and
 * never repeat the server's description. The sync treats it as a refused
 * login: it stops, backs off and counts toward switching the account off. */
static void
test_revoked(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	gint64 failures = 0;
	g_autofree gchar *last = NULL;
	(void)data;
	signed_in(f);
	f->oauth.revoked = TRUE;
	g_assert_false(venture_mail_oauth_refresh(f->db, f->config, f->account, TRUE, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_UNAUTHENTICATED);
	g_assert_nonnull(strstr(error->message, "sign in again"));
	g_assert_null(strstr(error->message, "echo"));
	g_clear_error(&error);
	/* Make the stored token due so the sync has to refresh it. */
	g_mutex_lock(&f->oauth.lock);
	f->oauth.revoked = FALSE;
	f->oauth.expires_in = 1;
	g_mutex_unlock(&f->oauth.lock);
	g_assert_true(venture_mail_oauth_refresh(f->db, f->config, f->account, TRUE, &error));
	g_assert_no_error(error);
	f->oauth.revoked = TRUE;
	venture_fake_imap_client_add_message(f->imap, "INBOX", 1,
		"From: Ada <ada@example.com>\r\nTo: ops@example.net\r\nSubject: Hello\r\n"
		"Date: Tue, 15 Sep 2026 09:00:00 +0000\r\nMessage-ID: <revoked@example.com>\r\n\r\nHi\r\n");
	g_assert_cmpint(venture_mail_sync_service_sync(f->service, f->account, NULL, &error), ==, -1);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_UNAUTHENTICATED);
	g_assert_cmpint(venture_fake_imap_client_get_connects(f->imap), ==, 0);
	{
		g_autoptr(VentureEntity) current = venture_database_get(f->db, VENTURE_TYPE_MAIL_ACCOUNT, venture_entity_get_id(f->account), NULL);
		g_object_get(current, "consecutive-failures", &failures, "last-error", &last, NULL);
	}
	g_assert_cmpint(failures, ==, 1);
	g_assert_nonnull(strstr(last, "sign in again"));
}

/* The sync signs in with XOAUTH2 and the current token, refreshing a token
 * that is about to expire first, and files mail as it does for a password
 * account. */
static void
test_sync_xoauth2(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	(void)data;
	signed_in(f);
	venture_fake_imap_client_set_token(f->imap, "access-1");
	venture_fake_imap_client_add_message(f->imap, "INBOX", 1,
		"From: Ada <ada@example.com>\r\nTo: ops@example.net\r\nSubject: Order\r\n"
		"Date: Tue, 15 Sep 2026 09:00:00 +0000\r\nMessage-ID: <order@example.com>\r\n\r\nTwo please\r\n");
	g_assert_cmpint(venture_mail_sync_service_sync(f->service, f->account, NULL, &error), ==, 1);
	g_assert_no_error(error);
	g_assert_cmpstr(venture_fake_imap_client_get_mechanism(f->imap), ==, "xoauth2");
	g_assert_cmpstr(venture_fake_imap_client_get_last_token(f->imap), ==, "access-1");
	g_assert_cmpuint(f->oauth.refreshes, ==, 0);
	/* A token within the refresh margin is renewed before the session. */
	g_mutex_lock(&f->oauth.lock);
	f->oauth.expires_in = 30;
	g_mutex_unlock(&f->oauth.lock);
	g_assert_true(venture_mail_oauth_refresh(f->db, f->config, f->account, TRUE, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(f->oauth.refreshes, ==, 1);
	g_mutex_lock(&f->oauth.lock);
	f->oauth.expires_in = 3600;
	g_mutex_unlock(&f->oauth.lock);
	venture_fake_imap_client_set_token(f->imap, "access-3");
	venture_fake_imap_client_add_message(f->imap, "INBOX", 2,
		"From: Ada <ada@example.com>\r\nTo: ops@example.net\r\nSubject: Order again\r\n"
		"Date: Tue, 15 Sep 2026 10:00:00 +0000\r\nMessage-ID: <order2@example.com>\r\n\r\nThree please\r\n");
	g_assert_cmpint(venture_mail_sync_service_sync(f->service, f->account, NULL, &error), ==, 1);
	g_assert_no_error(error);
	g_assert_cmpuint(f->oauth.refreshes, ==, 2);
	g_assert_cmpstr(venture_fake_imap_client_get_last_token(f->imap), ==, "access-3");
	/* A token the server refuses is a refused login like a bad password. */
	venture_fake_imap_client_set_token(f->imap, "something-else");
	venture_fake_imap_client_add_message(f->imap, "INBOX", 3,
		"From: Ada <ada@example.com>\r\nTo: ops@example.net\r\nSubject: Third\r\n"
		"Date: Tue, 15 Sep 2026 11:00:00 +0000\r\nMessage-ID: <order3@example.com>\r\n\r\nFour\r\n");
	g_assert_cmpint(venture_mail_sync_service_sync(f->service, f->account, NULL, &error), ==, -1);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_UNAUTHENTICATED);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/mail-oauth/helpers", test_helpers);
	g_test_add("/mail-oauth/pkce-sign-in", Fixture, NULL, setup, test_pkce_sign_in, teardown);
	g_test_add("/mail-oauth/endpoint-policy", Fixture, NULL, setup, test_endpoint_policy, teardown);
	g_test_add("/mail-oauth/refresh", Fixture, NULL, setup, test_refresh, teardown);
	g_test_add("/mail-oauth/revoked", Fixture, NULL, setup, test_revoked, teardown);
	g_test_add("/mail-oauth/sync-xoauth2", Fixture, NULL, setup, test_sync_xoauth2, teardown);
	return g_test_run();
}
