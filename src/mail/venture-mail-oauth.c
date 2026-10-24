/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * venture-mail-oauth.c - Token sign-in for a mailbox (OAuth 2.0 + PKCE)
 *
 * The provider is the operator's mail_oauth_provider record; its client id
 * and secret are an encrypted integration binding of their own. A started
 * sign-in is a one-time state in memory with its PKCE verifier and the
 * person who started it. The tokens the provider returns become the
 * mailbox's connector credential (venture-connector.c), so the IMAP
 * session reads them exactly as it reads a password, then says XOAUTH2.
 *
 * Exchanges with the token endpoint are blocking and run on the main
 * thread (the database is touched before and after), bounded by
 * VENTURE_MAIL_OAUTH_TIMEOUT_SECONDS. Redirects are refused: a 302 would
 * carry the client secret to a host the response chose. Nothing the
 * provider says is shown: only an OAuth error code from a fixed alphabet.
 */
#include <venture.h>
#include <openssl/rand.h>
#include <openssl/crypto.h>
#include <string.h>

#define OAUTH_PENDING_KEY "venture-mail-oauth-pending"
#define OAUTH_MAX_PENDING 256
#define OAUTH_MAX_REPLY (64 * 1024)
#define OAUTH_DEFAULT_LIFETIME 3600

typedef struct {
	gint64 account_id;
	gint64 user_id;
	gint64 expires;
	gchar *verifier;
	gchar *redirect;
} OauthPending;

static void
oauth_pending_free(gpointer data)
{
	OauthPending *pending = data;
	if (pending->verifier)
		OPENSSL_cleanse(pending->verifier, strlen(pending->verifier));
	g_free(pending->verifier);
	g_free(pending->redirect);
	g_free(pending);
}

static gboolean
oauth_refuse(GError **error, VentureError code, const gchar *message)
{
	g_set_error_literal(error, VENTURE_ERROR, code, message);
	return FALSE;
}

static gchar *
oauth_b64url(const guint8 *bytes, gsize length)
{
	gchar *encoded = g_base64_encode(bytes, length);
	gchar *p;
	for (p = encoded; *p; p++)
	{
		if (*p == '+') *p = '-';
		else if (*p == '/') *p = '_';
	}
	p = strchr(encoded, '=');
	if (p)
		*p = '\0';
	return encoded;
}

static gchar *
oauth_random(gsize bytes)
{
	guint8 buffer[64];
	gchar *text;
	g_assert(bytes <= sizeof buffer);
	if (RAND_bytes(buffer, (int)bytes) != 1)
		return NULL;
	text = oauth_b64url(buffer, bytes);
	OPENSSL_cleanse(buffer, sizeof buffer);
	return text;
}

gchar *
venture_mail_oauth_pkce_challenge(const gchar *verifier)
{
	g_autoptr(GChecksum) sum = g_checksum_new(G_CHECKSUM_SHA256);
	guint8 digest[32];
	gsize length = sizeof digest;
	g_return_val_if_fail(verifier != NULL, NULL);
	g_checksum_update(sum, (const guchar *)verifier, -1);
	g_checksum_get_digest(sum, digest, &length);
	return oauth_b64url(digest, length);
}

gchar *
venture_mail_oauth_xoauth2(const gchar *username, const gchar *access_token)
{
	GString *raw;
	const gchar *p;
	gchar *encoded;
	if (venture_string_is_empty(username) || venture_string_is_empty(access_token))
		return NULL;
	/* ^A separates the fields; any control character in either value would
	 * end a field early or start a second IMAP command. */
	for (p = username; *p; p++) if ((guchar)*p < 0x20 || *p == 0x7f) return NULL;
	for (p = access_token; *p; p++) if ((guchar)*p < 0x20 || *p == 0x7f) return NULL;
	raw = g_string_new(NULL);
	g_string_append_printf(raw, "user=%s\001auth=Bearer %s\001\001", username, access_token);
	encoded = g_base64_encode((const guchar *)raw->str, raw->len);
	OPENSSL_cleanse(raw->str, raw->len);
	g_string_free(raw, TRUE);
	return encoded;
}

/* HTTPS with a host and no credentials in it; plain HTTP only to loopback
 * and only with the operator's loopback switch (the tests' fake server). */
static gboolean
oauth_endpoint_allowed(VentureConfig *config, const gchar *url, GError **error)
{
	g_autoptr(GUri) uri = url != NULL ? g_uri_parse(url, G_URI_FLAGS_NONE, NULL) : NULL;
	gboolean loopback_ok = FALSE;
	const gchar *host;
	if (uri == NULL || g_uri_get_host(uri) == NULL || g_uri_get_userinfo(uri) != NULL || g_uri_get_fragment(uri) != NULL)
		return oauth_refuse(error, VENTURE_ERROR_CONFIG, "The sign-in provider needs absolute authorize and token addresses without credentials");
	host = g_uri_get_host(uri);
	if (!g_strcmp0(g_uri_get_scheme(uri), "https"))
		return TRUE;
	g_object_get(config, "connectors-allow-plaintext-loopback", &loopback_ok, NULL);
	if (!g_strcmp0(g_uri_get_scheme(uri), "http") && loopback_ok &&
		(!g_strcmp0(host, "127.0.0.1") || !g_strcmp0(host, "::1") || !g_strcmp0(host, "[::1]")))
		return TRUE;
	return oauth_refuse(error, VENTURE_ERROR_CONFIG, "The sign-in provider's addresses must use HTTPS");
}

static gchar *
oauth_client_key(VentureEntity *provider)
{
	return g_strdup_printf("mail-oauth-%s", venture_entity_get_uuid(provider));
}

static gboolean
oauth_provider_check(VentureConfig *config, VentureEntity *provider, GError **error)
{
	g_autofree gchar *authorize = NULL, *token = NULL;
	g_object_get(provider, "authorize-url", &authorize, "token-url", &token, NULL);
	return oauth_endpoint_allowed(config, authorize, error) && oauth_endpoint_allowed(config, token, error);
}

gboolean
venture_mail_oauth_configure_client(VentureDatabase *database, VentureConfig *config,
	VentureEntity *provider, const gchar *client_id, const gchar *client_secret,
	const VentureActor *actor, GError **error)
{
	VentureIntegrationService *vault;
	g_autoptr(VentureIntegrationConnection) existing = NULL;
	g_autoptr(VentureIntegrationConnection) saved = NULL;
	g_autoptr(GError) lookup = NULL;
	g_autoptr(JsonNode) values = NULL;
	g_autofree gchar *key = NULL;
	JsonObject *object;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), FALSE);
	g_return_val_if_fail(VENTURE_IS_CONFIG(config), FALSE);
	g_return_val_if_fail(VENTURE_IS_MAIL_OAUTH_PROVIDER(provider), FALSE);
	if (venture_string_is_empty(client_id) || strlen(client_id) > 1024 || strpbrk(client_id, "\r\n") ||
		(client_secret != NULL && (strlen(client_secret) > 4096 || strpbrk(client_secret, "\r\n"))))
		return oauth_refuse(error, VENTURE_ERROR_VALIDATION, "Give the client ID (and secret, if the provider issued one) on one line each");
	if (!venture_entity_is_persisted(provider) || !oauth_provider_check(config, provider, error))
		return FALSE;
	vault = venture_integration_service_get(database);
	key = oauth_client_key(provider);
	existing = venture_integration_service_find(vault, venture_entity_get_organization_id(provider), key, &lookup);
	if (existing == NULL && lookup != NULL && !g_error_matches(lookup, VENTURE_ERROR, VENTURE_ERROR_CONFIG))
	{
		g_propagate_error(error, g_steal_pointer(&lookup));
		return FALSE;
	}
	object = json_object_new();
	json_object_set_string_member(object, "client_id", client_id);
	json_object_set_string_member(object, "client_secret", client_secret != NULL ? client_secret : "");
	values = json_node_new(JSON_NODE_OBJECT);
	json_node_take_object(values, object);
	saved = venture_integration_service_configure(vault, venture_entity_get_organization_id(provider), key,
		venture_entity_get_uuid(provider), "live", values,
		existing != NULL ? venture_entity_get_version(VENTURE_ENTITY(existing)) : 0, actor, error);
	return saved != NULL;
}

/* The provider's client credential, or NULL with CONFIG when none is set. */
static JsonNode *
oauth_client(VentureDatabase *database, VentureEntity *provider, GError **error)
{
	VentureIntegrationService *vault = venture_integration_service_get(database);
	g_autoptr(VentureIntegrationConnection) binding = NULL;
	g_autofree gchar *key = oauth_client_key(provider);
	binding = venture_integration_service_find(vault, venture_entity_get_organization_id(provider), key, NULL);
	if (binding == NULL)
	{
		oauth_refuse(error, VENTURE_ERROR_CONFIG, "The sign-in provider has no client credential yet; an administrator sets it on the provider's page");
		return NULL;
	}
	return venture_integration_service_resolve(vault, venture_entity_get_organization_id(provider),
		venture_entity_get_id(VENTURE_ENTITY(binding)), FALSE, error);
}

gboolean
venture_mail_oauth_client_configured(VentureDatabase *database, VentureEntity *provider)
{
	g_autoptr(VentureIntegrationConnection) binding = NULL;
	g_autofree gchar *key = NULL;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), FALSE);
	if (!VENTURE_IS_MAIL_OAUTH_PROVIDER(provider) || !venture_entity_is_persisted(provider))
		return FALSE;
	key = oauth_client_key(provider);
	binding = venture_integration_service_find(venture_integration_service_get(database),
		venture_entity_get_organization_id(provider), key, NULL);
	return binding != NULL;
}

/* The account's provider, live and active, or NULL with the reason. */
static VentureEntity *
oauth_provider_for(VentureDatabase *database, VentureEntity *account, GError **error)
{
	g_autoptr(VentureEntity) provider = NULL;
	gint64 provider_id = 0;
	gboolean active = FALSE;
	g_object_get(account, "oauth-provider-id", &provider_id, NULL);
	if (provider_id <= 0)
	{
		oauth_refuse(error, VENTURE_ERROR_VALIDATION, "This mailbox signs in with a password; choose a sign-in provider on the account first");
		return NULL;
	}
	provider = venture_database_get(database, VENTURE_TYPE_MAIL_OAUTH_PROVIDER, provider_id, NULL);
	if (provider != NULL)
		g_object_get(provider, "active", &active, NULL);
	if (provider == NULL || venture_entity_is_deleted(provider) || !active ||
		venture_entity_get_organization_id(provider) != venture_entity_get_organization_id(account))
	{
		oauth_refuse(error, VENTURE_ERROR_CONFIG, "The account's sign-in provider is missing or switched off");
		return NULL;
	}
	return g_steal_pointer(&provider);
}

static GHashTable *
oauth_pending(VentureDatabase *database)
{
	GHashTable *table = g_object_get_data(G_OBJECT(database), OAUTH_PENDING_KEY);
	if (table == NULL)
	{
		table = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, oauth_pending_free);
		g_object_set_data_full(G_OBJECT(database), OAUTH_PENDING_KEY, table, (GDestroyNotify)g_hash_table_unref);
	}
	return table;
}

static gboolean
oauth_expired(gpointer key, gpointer value, gpointer now)
{
	(void)key;
	return ((OauthPending *)value)->expires <= *(gint64 *)now;
}

gchar *
venture_mail_oauth_begin(VentureDatabase *database, VentureConfig *config,
	VentureEntity *account, const gchar *redirect_uri, gint64 user_id, GError **error)
{
	g_autoptr(VentureEntity) current = NULL;
	g_autoptr(VentureEntity) provider = NULL;
	g_autoptr(JsonNode) client = NULL;
	g_autofree gchar *authorize = NULL, *scopes = NULL, *state = NULL, *verifier = NULL, *challenge = NULL;
	g_autofree gchar *e_client = NULL, *e_redirect = NULL, *e_scope = NULL;
	GHashTable *table;
	OauthPending *pending;
	gint64 now = g_get_monotonic_time();
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	g_return_val_if_fail(VENTURE_IS_CONFIG(config), NULL);
	g_return_val_if_fail(VENTURE_IS_ENTITY(account), NULL);
	if (!VENTURE_IS_MAIL_ACCOUNT(account) || !venture_entity_is_persisted(account))
	{
		oauth_refuse(error, VENTURE_ERROR_VALIDATION, "Sign in a saved mail account");
		return NULL;
	}
	if (user_id <= 0 || venture_string_is_empty(redirect_uri))
	{
		oauth_refuse(error, VENTURE_ERROR_UNAUTHENTICATED, "Sign in to Venture before signing in the mailbox");
		return NULL;
	}
	if (oauth_provider_for(database, account, error) == NULL)
		return NULL;
	current = venture_database_get(database, VENTURE_TYPE_MAIL_ACCOUNT, venture_entity_get_id(account), error);
	if (current == NULL)
		return NULL;
	provider = oauth_provider_for(database, current, error);
	if (provider == NULL || !oauth_provider_check(config, provider, error))
		return NULL;
	client = oauth_client(database, provider, error);
	if (client == NULL)
		return NULL;
	if (!venture_connector_can_manage(database, current, error))
		return NULL;
	table = oauth_pending(database);
	g_hash_table_foreach_remove(table, oauth_expired, &now);
	if (g_hash_table_size(table) >= OAUTH_MAX_PENDING)
	{
		oauth_refuse(error, VENTURE_ERROR_CONFLICT, "Too many mailbox sign-ins are waiting; try again in a few minutes");
		return NULL;
	}
	state = oauth_random(32);
	verifier = oauth_random(32);
	if (state == NULL || verifier == NULL)
	{
		oauth_refuse(error, VENTURE_ERROR_FAILED, "Could not make a sign-in state");
		return NULL;
	}
	challenge = venture_mail_oauth_pkce_challenge(verifier);
	g_object_get(provider, "authorize-url", &authorize, "scopes", &scopes, NULL);
	e_client = g_uri_escape_string(json_object_get_string_member(json_node_get_object(client), "client_id"), NULL, FALSE);
	e_redirect = g_uri_escape_string(redirect_uri, NULL, FALSE);
	e_scope = g_uri_escape_string(scopes != NULL ? scopes : "", NULL, FALSE);
	pending = g_new0(OauthPending, 1);
	pending->account_id = venture_entity_get_id(current);
	pending->user_id = user_id;
	pending->expires = now + (gint64)VENTURE_MAIL_OAUTH_STATE_SECONDS * G_USEC_PER_SEC;
	pending->verifier = g_steal_pointer(&verifier);
	pending->redirect = g_strdup(redirect_uri);
	g_hash_table_insert(table, g_strdup(state), pending);
	return g_strdup_printf("%s%sresponse_type=code&client_id=%s&redirect_uri=%s&scope=%s&state=%s&code_challenge=%s&code_challenge_method=S256",
		authorize, strchr(authorize, '?') != NULL ? "&" : "?", e_client, e_redirect, e_scope, state, challenge);
}

/* An OAuth error code is shown only when it is one: lower case and
 * underscores. The description beside it is the server's prose and may
 * repeat what it was sent. */
static gchar *
oauth_error_code(JsonNode *reply)
{
	const gchar *code, *p;
	if (reply == NULL || !JSON_NODE_HOLDS_OBJECT(reply))
		return NULL;
	code = venture_json_object_get_string(json_node_get_object(reply), "error", NULL);
	if (venture_string_is_empty(code) || strlen(code) > 40)
		return NULL;
	for (p = code; *p; p++)
		if (!g_ascii_islower(*p) && *p != '_')
			return NULL;
	return g_strdup(code);
}

/* POSTs @form to the provider's token address and returns its JSON object,
 * or NULL with UNAUTHENTICATED (refused) or NETWORK (unreachable). */
static JsonNode *
oauth_post(VentureEntity *provider, GHashTable *form, const gchar *refused, GError **error)
{
	g_autoptr(SoupSession) session = soup_session_new();
	g_autoptr(SoupMessage) message = NULL;
	g_autoptr(GBytes) body = NULL;
	g_autoptr(GBytes) request = NULL;
	g_autoptr(JsonNode) parsed = NULL;
	g_autoptr(GError) local = NULL;
	g_autofree gchar *token_url = NULL, *encoded = NULL, *text = NULL, *code = NULL;
	guint status;
	g_object_get(provider, "token-url", &token_url, NULL);
	message = soup_message_new(SOUP_METHOD_POST, token_url);
	if (message == NULL)
	{
		oauth_refuse(error, VENTURE_ERROR_CONFIG, "The sign-in provider's token address is not usable");
		return NULL;
	}
	soup_session_set_timeout(session, VENTURE_MAIL_OAUTH_TIMEOUT_SECONDS);
	soup_session_set_idle_timeout(session, VENTURE_MAIL_OAUTH_TIMEOUT_SECONDS);
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	soup_message_headers_append(soup_message_get_request_headers(message), "Accept", "application/json");
	encoded = soup_form_encode_hash(form);
	request = g_bytes_new(encoded, strlen(encoded));
	OPENSSL_cleanse(encoded, strlen(encoded));
	soup_message_set_request_body_from_bytes(message, "application/x-www-form-urlencoded", request);
	body = soup_session_send_and_read(session, message, NULL, &local);
	if (body == NULL)
	{
		oauth_refuse(error, VENTURE_ERROR_NETWORK, "The sign-in provider could not be reached; nothing was changed");
		return NULL;
	}
	status = soup_message_get_status(message);
	if (g_bytes_get_size(body) > OAUTH_MAX_REPLY)
	{
		oauth_refuse(error, VENTURE_ERROR_NETWORK, "The sign-in provider answered with too much");
		return NULL;
	}
	text = g_strndup(g_bytes_get_data(body, NULL), g_bytes_get_size(body));
	parsed = json_from_string(text, NULL);
	OPENSSL_cleanse(text, strlen(text));
	if (status != 200)
	{
		code = oauth_error_code(parsed);
		if (status >= 500 || status == 429)
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NETWORK, "The sign-in provider is unavailable (HTTP %u); try again later", status);
		else
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_UNAUTHENTICATED, "%s%s%s%s", refused,
				code != NULL ? " (" : "", code != NULL ? code : "", code != NULL ? ")" : "");
		return NULL;
	}
	if (parsed == NULL || !JSON_NODE_HOLDS_OBJECT(parsed) ||
		venture_string_is_empty(venture_json_object_get_string(json_node_get_object(parsed), "access_token", NULL)))
	{
		oauth_refuse(error, VENTURE_ERROR_UNAUTHENTICATED, "The sign-in provider did not return an access token");
		return NULL;
	}
	{
		const gchar *type = venture_json_object_get_string(json_node_get_object(parsed), "token_type", NULL);
		if (type != NULL && g_ascii_strcasecmp(type, "bearer") != 0)
		{
			oauth_refuse(error, VENTURE_ERROR_UNAUTHENTICATED, "The sign-in provider returned a token that is not a bearer token");
			return NULL;
		}
	}
	return g_steal_pointer(&parsed);
}

static void
oauth_form_client(GHashTable *form, JsonNode *client)
{
	JsonObject *object = json_node_get_object(client);
	const gchar *secret = venture_json_object_get_string(object, "client_secret", NULL);
	g_hash_table_insert(form, g_strdup("client_id"), g_strdup(venture_json_object_get_string(object, "client_id", "")));
	if (!venture_string_is_empty(secret))
		g_hash_table_insert(form, g_strdup("client_secret"), g_strdup(secret));
}

static void
oauth_wipe_value(gpointer data)
{
	if (data != NULL)
		OPENSSL_cleanse(data, strlen(data));
	g_free(data);
}

/* Stores @reply's tokens as the account's connector credential, keeping
 * @previous_refresh when the provider did not rotate it. */
static gboolean
oauth_store(VentureDatabase *database, VentureConfig *config, VentureEntity *account, VentureEntity *provider,
	JsonNode *reply, const gchar *previous_refresh, const VentureActor *actor, GError **error)
{
	JsonObject *answer = json_node_get_object(reply);
	g_autoptr(JsonObject) settings = json_object_new();
	g_autoptr(VentureIntegrationConnection) existing = NULL;
	g_autoptr(VentureIntegrationConnection) saved = NULL;
	g_autofree gchar *key = venture_connector_binding_key(account);
	const gchar *refresh = venture_json_object_get_string(answer, "refresh_token", NULL);
	gint64 lifetime = venture_json_object_get_int(answer, "expires_in", 0);
	if (lifetime <= 0 || lifetime > 400 * 24 * 3600)
		lifetime = OAUTH_DEFAULT_LIFETIME;
	json_object_set_string_member(settings, "oauth_access_token", venture_json_object_get_string(answer, "access_token", ""));
	if (!venture_string_is_empty(refresh))
		json_object_set_string_member(settings, "oauth_refresh_token", refresh);
	else if (!venture_string_is_empty(previous_refresh))
		json_object_set_string_member(settings, "oauth_refresh_token", previous_refresh);
	json_object_set_int_member(settings, "oauth_expires_at", g_get_real_time() / G_USEC_PER_SEC + lifetime);
	json_object_set_int_member(settings, "oauth_provider_id", venture_entity_get_id(provider));
	existing = venture_integration_service_find(venture_integration_service_get(database), venture_entity_get_organization_id(account), key, NULL);
	saved = venture_connector_configure(database, config, account, settings,
		existing != NULL ? venture_entity_get_id(VENTURE_ENTITY(existing)) : 0,
		existing != NULL ? venture_entity_get_version(VENTURE_ENTITY(existing)) : 0, actor, error);
	return saved != NULL;
}

gint64
venture_mail_oauth_complete(VentureDatabase *database, VentureConfig *config,
	const gchar *state, const gchar *code, gint64 user_id, const VentureActor *actor, GError **error)
{
	GHashTable *table;
	OauthPending *found;
	g_autoptr(VentureEntity) account = NULL;
	g_autoptr(VentureEntity) provider = NULL;
	g_autoptr(JsonNode) client = NULL;
	g_autoptr(JsonNode) reply = NULL;
	g_autoptr(GHashTable) form = NULL;
	g_autofree gchar *verifier = NULL, *redirect = NULL;
	gint64 account_id, now = g_get_monotonic_time();
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), 0);
	g_return_val_if_fail(VENTURE_IS_CONFIG(config), 0);
	table = oauth_pending(database);
	g_hash_table_foreach_remove(table, oauth_expired, &now);
	found = state != NULL ? g_hash_table_lookup(table, state) : NULL;
	/* Somebody else's state is "not found" and is left for its owner. */
	if (found == NULL || found->user_id != user_id)
	{
		oauth_refuse(error, VENTURE_ERROR_NOT_FOUND, "This mailbox sign-in has expired or was not started by you; start it again");
		return 0;
	}
	account_id = found->account_id;
	verifier = g_strdup(found->verifier);
	redirect = g_strdup(found->redirect);
	/* Used once, whatever the exchange says. */
	g_hash_table_remove(table, state);
	if (venture_string_is_empty(code) || strlen(code) > 4096)
	{
		oauth_refuse(error, VENTURE_ERROR_UNAUTHENTICATED, "The provider did not send a sign-in code; start the sign-in again");
		return 0;
	}
	account = venture_database_get(database, VENTURE_TYPE_MAIL_ACCOUNT, account_id, error);
	if (account == NULL)
		return 0;
	provider = oauth_provider_for(database, account, error);
	if (provider == NULL || !oauth_provider_check(config, provider, error))
		return 0;
	client = oauth_client(database, provider, error);
	if (client == NULL)
		return 0;
	form = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, oauth_wipe_value);
	g_hash_table_insert(form, g_strdup("grant_type"), g_strdup("authorization_code"));
	g_hash_table_insert(form, g_strdup("code"), g_strdup(code));
	g_hash_table_insert(form, g_strdup("redirect_uri"), g_strdup(redirect));
	g_hash_table_insert(form, g_strdup("code_verifier"), g_strdup(verifier));
	oauth_form_client(form, client);
	OPENSSL_cleanse(verifier, strlen(verifier));
	reply = oauth_post(provider, form, "The provider refused the sign-in; start it again", error);
	if (reply == NULL)
		return 0;
	if (!oauth_store(database, config, account, provider, reply, NULL, actor, error))
		return 0;
	return account_id;
}

gboolean
venture_mail_oauth_refresh(VentureDatabase *database, VentureConfig *config,
	VentureEntity *account, gboolean force, GError **error)
{
	VentureIntegrationService *vault;
	g_autoptr(VentureEntity) current = NULL;
	g_autoptr(VentureEntity) provider = NULL;
	g_autoptr(VentureIntegrationConnection) binding = NULL;
	g_autoptr(JsonNode) settings = NULL;
	g_autoptr(JsonNode) client = NULL;
	g_autoptr(JsonNode) reply = NULL;
	g_autoptr(GHashTable) form = NULL;
	g_autofree gchar *key = NULL, *refresh = NULL;
	JsonObject *object;
	gint64 expires;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), FALSE);
	g_return_val_if_fail(VENTURE_IS_CONFIG(config), FALSE);
	g_return_val_if_fail(VENTURE_IS_ENTITY(account), FALSE);
	current = venture_database_get(database, VENTURE_TYPE_MAIL_ACCOUNT, venture_entity_get_id(account), error);
	if (current == NULL)
		return FALSE;
	provider = oauth_provider_for(database, current, error);
	if (provider == NULL || !oauth_provider_check(config, provider, error))
		return FALSE;
	vault = venture_integration_service_get(database);
	key = venture_connector_binding_key(current);
	binding = venture_integration_service_find(vault, venture_entity_get_organization_id(current), key, NULL);
	if (binding != NULL)
		settings = venture_integration_service_resolve_version(vault, venture_entity_get_organization_id(current),
			venture_entity_get_id(VENTURE_ENTITY(binding)), venture_entity_get_version(VENTURE_ENTITY(binding)), FALSE, error);
	if (binding == NULL || settings == NULL || !JSON_NODE_HOLDS_OBJECT(settings) ||
		!json_object_has_member(json_node_get_object(settings), "oauth_access_token"))
	{
		g_clear_error(error);
		return oauth_refuse(error, VENTURE_ERROR_UNAUTHENTICATED, "This mailbox has not been signed in with its provider yet; sign in again from the account's page");
	}
	object = json_node_get_object(settings);
	expires = venture_json_object_get_int(object, "oauth_expires_at", 0);
	if (!force && expires - g_get_real_time() / G_USEC_PER_SEC > VENTURE_MAIL_OAUTH_REFRESH_MARGIN_SECONDS)
		return TRUE;
	refresh = g_strdup(venture_json_object_get_string(object, "oauth_refresh_token", NULL));
	if (venture_string_is_empty(refresh))
		return oauth_refuse(error, VENTURE_ERROR_UNAUTHENTICATED, "The mailbox sign-in has expired and the provider gave no way to renew it; sign in again");
	client = oauth_client(database, provider, error);
	if (client == NULL)
		return FALSE;
	form = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, oauth_wipe_value);
	g_hash_table_insert(form, g_strdup("grant_type"), g_strdup("refresh_token"));
	g_hash_table_insert(form, g_strdup("refresh_token"), g_strdup(refresh));
	oauth_form_client(form, client);
	reply = oauth_post(provider, form,
		"The provider refused to renew the mailbox sign-in (revoked or expired); sign in again from the account's page", error);
	if (reply == NULL)
	{
		OPENSSL_cleanse(refresh, strlen(refresh));
		return FALSE;
	}
	if (!oauth_store(database, config, current, provider, reply, refresh, NULL, error))
	{
		OPENSSL_cleanse(refresh, strlen(refresh));
		return FALSE;
	}
	OPENSSL_cleanse(refresh, strlen(refresh));
	return TRUE;
}
