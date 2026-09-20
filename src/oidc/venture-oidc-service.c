/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
#include "venture-oidc-verifier-private.h"
#include <libsoup/soup.h>
#include <string.h>
#define OIDC_ATTEMPT_SECONDS 300
#define OIDC_MAX_ATTEMPTS 1024
#define OIDC_MAX_RESPONSE (128 * 1024)
typedef struct {
	gint64 connection, version, user, user_version, expires;
	gchar *issuer, *client, *token_uri, *nonce, *verifier, *browser_hash, *peer_hash;
	GObject *validator;
} Attempt;
typedef struct { gint64 expires; guint count; } Budget;
struct _VentureOidcService { GObject parent; VentureDatabase *database; VentureConfig *config; GHashTable *attempts; GHashTable *budgets; GHashTable *validators; VentureEntity *writing; };
G_DEFINE_FINAL_TYPE(VentureOidcService, venture_oidc_service, G_TYPE_OBJECT)
static void attempt_free(gpointer data)
{
	Attempt *a = data;
	g_free(a->issuer); g_free(a->client); g_free(a->token_uri); g_free(a->nonce); g_free(a->verifier);
	g_free(a->browser_hash); g_free(a->peer_hash); g_clear_object(&a->validator); g_free(a);
}
static void finalize(GObject *object)
{
	VentureOidcService *self = VENTURE_OIDC_SERVICE(object);
	if (self->database) g_object_remove_weak_pointer(G_OBJECT(self->database), (gpointer *)&self->database);
	g_clear_object(&self->config); g_hash_table_unref(self->attempts); g_hash_table_unref(self->budgets); g_hash_table_unref(self->validators);
	G_OBJECT_CLASS(venture_oidc_service_parent_class)->finalize(object);
}
static void venture_oidc_service_class_init(VentureOidcServiceClass *klass) { G_OBJECT_CLASS(klass)->finalize = finalize; }
static void venture_oidc_service_init(VentureOidcService *self)
{
	self->attempts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, attempt_free);
	self->budgets = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	self->validators = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
}
static gboolean fail(GError **error, VentureError code, const gchar *message)
{ g_set_error_literal(error, VENTURE_ERROR, code, message); return FALSE; }
static gboolean denied(GError **error) { return fail(error, VENTURE_ERROR_UNAUTHENTICATED, "Organization sign-in could not be verified; restart sign-in or use local recovery"); }
VentureOidcService *venture_oidc_service_get(VentureDatabase *database)
{
	VentureOidcService *self;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	self = g_object_get_data(G_OBJECT(database), "venture-oidc-service");
	if (!self) {
		self = g_object_new(VENTURE_TYPE_OIDC_SERVICE, NULL); self->database = database;
		g_object_add_weak_pointer(G_OBJECT(database), (gpointer *)&self->database);
		g_object_set_data_full(G_OBJECT(database), "venture-oidc-service", self, g_object_unref);
	}
	return self;
}
void venture_oidc_service_set_config(VentureOidcService *self, VentureConfig *config)
{ g_return_if_fail(VENTURE_IS_OIDC_SERVICE(self)); g_set_object(&self->config, config); }
static gboolean ready(VentureOidcService *self, GError **error)
{
	gboolean enabled = FALSE, require_auth = FALSE;
	if (self->config) g_object_get(self->config, "oidc-enabled", &enabled, "security-require-auth", &require_auth, NULL);
	if (!self->database || !enabled || venture_entity_registry_lookup(venture_entity_registry_get_default(), "oidc_identity") == G_TYPE_INVALID)
		return fail(error, VENTURE_ERROR_CONFIG, "Organization OIDC sign-in is disabled");
	if (!require_auth) return fail(error, VENTURE_ERROR_CONFIG, "OIDC requires local authentication to remain enabled");
	return TRUE;
}
gboolean venture_oidc_check_write(VentureDatabase *database, VentureEntity *entity, gboolean removal, GError **error)
{
	(void)removal;
	if (!VENTURE_IS_OIDC_IDENTITY(entity)) return TRUE;
	if (venture_oidc_service_get(database)->writing != entity)
		return fail(error, VENTURE_ERROR_VALIDATION, "Identity links are written only by verified linking and unlinking");
	return TRUE;
}
static gboolean save(VentureOidcService *self, VentureEntity *entity, const VentureActor *actor, GError **error)
{
	gboolean ok;
	VentureEntity *previous = self->writing;
	self->writing = entity; ok = venture_database_save(self->database, entity, actor, error); self->writing = previous;
	return ok;
}
static gboolean safe_uri(VentureOidcService *self, const gchar *value)
{
	g_autoptr(GUri) uri = value ? g_uri_parse(value, G_URI_FLAGS_NONE, NULL) : NULL;
	g_autoptr(GInetAddress) address = NULL;
	gboolean loopback = FALSE;
	const gchar *host, *scheme;
	if (!uri || strlen(value) > 4096 || g_uri_get_userinfo(uri) || g_uri_get_fragment(uri) || g_uri_get_query(uri)) return FALSE;
	host = g_uri_get_host(uri); scheme = g_uri_get_scheme(uri);
	if (!host || !*host) return FALSE;
	if (!g_strcmp0(scheme, "https")) return TRUE;
	g_object_get(self->config, "oidc-allow-loopback", &loopback, NULL);
	address = g_inet_address_new_from_string(host);
	return loopback && !g_strcmp0(scheme, "http") &&
		((address && g_inet_address_get_is_loopback(address)) || !g_ascii_strcasecmp(host, "localhost"));
}
static gint port(GUri *uri)
{ gint value = g_uri_get_port(uri); return value >= 0 ? value : (!g_strcmp0(g_uri_get_scheme(uri), "https") ? 443 : 80); }
static gboolean same_origin(VentureOidcService *self, const gchar *issuer, const gchar *endpoint)
{
	g_autoptr(GUri) a = g_uri_parse(issuer, G_URI_FLAGS_NONE, NULL), b = endpoint ? g_uri_parse(endpoint, G_URI_FLAGS_NONE, NULL) : NULL;
	return safe_uri(self, endpoint) && a && b && !g_strcmp0(g_uri_get_scheme(a), g_uri_get_scheme(b)) &&
		!g_ascii_strcasecmp(g_uri_get_host(a), g_uri_get_host(b)) && port(a) == port(b);
}
static gboolean approved_issuer(VentureOidcService *self, const gchar *issuer, GError **error)
{
	g_auto(GStrv) issuers = NULL;
	g_object_get(self->config, "oidc-allowed-issuers", &issuers, NULL);
	if (!issuer || strlen(issuer) > 2048 || !safe_uri(self, issuer) || !issuers || !g_strv_contains((const gchar * const *)issuers, issuer))
		return fail(error, VENTURE_ERROR_CONFIG, "OIDC issuer must exactly match the platform allowlist and use a permitted origin");
	return TRUE;
}
gchar *venture_oidc_service_callback_uri(VentureOidcService *self, GError **error)
{
	g_autofree gchar *base = NULL;
	g_autoptr(GUri) uri = NULL;
	gboolean secure = FALSE;
	g_return_val_if_fail(VENTURE_IS_OIDC_SERVICE(self), NULL);
	if (!self->config) { fail(error, VENTURE_ERROR_CONFIG, "OIDC needs platform configuration"); return NULL; }
	g_object_get(self->config, "server-base-url", &base, "security-cookie-secure", &secure, NULL);
	uri = base ? g_uri_parse(base, G_URI_FLAGS_NONE, NULL) : NULL;
	if (!safe_uri(self, base) || !uri || (g_uri_get_path(uri) && *g_uri_get_path(uri) && strcmp(g_uri_get_path(uri), "/")) ||
		(!g_strcmp0(g_uri_get_scheme(uri), "https") && !secure)) {
		fail(error, VENTURE_ERROR_CONFIG, "OIDC needs an exact root server.base_url and secure cookies for HTTPS"); return NULL;
	}
	if (g_str_has_suffix(base, "/")) base[strlen(base) - 1] = 0;
	return g_strconcat(base, "/auth/oidc/callback", NULL);
}
typedef struct {
	SoupMessage *message;
	GInputStream *stream;
	GCancellable *cancel;
	GByteArray *bytes;
	gboolean done;
	GError *error;
} HttpRead;
static gboolean http_deadline(gpointer data) { g_cancellable_cancel(data); return G_SOURCE_REMOVE; }
static void http_read_more(HttpRead *read);
static void http_read_done(GObject *object, GAsyncResult *result, gpointer data)
{
	HttpRead *read = data;
	g_autoptr(GBytes) bytes = g_input_stream_read_bytes_finish(G_INPUT_STREAM(object), result, &read->error);
	gsize length = bytes ? g_bytes_get_size(bytes) : 0;
	if (!bytes || !length) { read->done = TRUE; return; }
	if (length > OIDC_MAX_RESPONSE - read->bytes->len) {
		g_set_error_literal(&read->error, G_IO_ERROR, G_IO_ERROR_NO_SPACE, "OIDC response exceeds its bound"); read->done = TRUE; return;
	}
	g_byte_array_append(read->bytes, g_bytes_get_data(bytes, NULL), length); http_read_more(read);
}
static void http_read_more(HttpRead *read)
{ g_input_stream_read_bytes_async(read->stream, 8192, G_PRIORITY_DEFAULT, read->cancel, http_read_done, read); }
static void http_sent(GObject *object, GAsyncResult *result, gpointer data)
{
	HttpRead *read = data;
	read->stream = soup_session_send_finish(SOUP_SESSION(object), result, &read->error);
	if (!read->stream || soup_message_get_status(read->message) != 200) { read->done = TRUE; return; }
	http_read_more(read);
}
static JsonNode *http_json(const gchar *uri, const gchar *form, GError **error)
{
	g_autoptr(GMainContext) context = g_main_context_new();
	g_autoptr(GSource) deadline = g_timeout_source_new(5000);
	g_autoptr(GCancellable) cancel = g_cancellable_new();
	g_autoptr(SoupSession) session = NULL;
	g_autoptr(SoupMessage) message = soup_message_new(form ? "POST" : "GET", uri);
	g_autoptr(GByteArray) bytes = g_byte_array_new();
	g_autoptr(GError) parse_error = NULL;
	JsonNode *result = NULL;
	HttpRead read;
	if (!message) { denied(error); return NULL; }
	read.message = message; read.stream = NULL; read.cancel = cancel;
	read.bytes = bytes; read.done = FALSE; read.error = NULL;
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	if (form) { g_autoptr(GBytes) body = g_bytes_new(form, strlen(form)); soup_message_set_request_body_from_bytes(message, "application/x-www-form-urlencoded", body); }
	/* Both total time and bytes are bounded. A provider trickling bytes must
	 * not hold the application thread indefinitely despite an idle timeout. */
	g_main_context_push_thread_default(context);
	session = soup_session_new_with_options("timeout", 10, "user-agent", "VENTURE OIDC", NULL);
	g_source_set_callback(deadline, http_deadline, cancel, NULL); g_source_attach(deadline, context);
	soup_session_send_async(session, message, G_PRIORITY_DEFAULT, cancel, http_sent, &read);
	while (!read.done) g_main_context_iteration(context, TRUE);
	g_source_destroy(deadline); g_clear_object(&read.stream);
	g_main_context_pop_thread_default(context);
	if (!read.error && soup_message_get_status(message) == 200 && !memchr(bytes->data, 0, bytes->len)) {
		g_byte_array_append(bytes, (const guchar *)"", 1);
		result = venture_json_parse((gchar *)bytes->data, &parse_error);
	}
	g_clear_error(&read.error);
	if (!result || !JSON_NODE_HOLDS_OBJECT(result)) { g_clear_pointer(&result, json_node_unref); denied(error); return NULL; }
	return result;
}
static VentureIntegrationConnection *connection(VentureOidcService *self, gint64 id, const gchar *uuid, GError **error)
{
	g_autoptr(VentureAccessScope) internal = venture_access_policy_enter(venture_database_get_access_policy(self->database), NULL);
	g_autoptr(VentureEntity) row = NULL, org = NULL;
	g_autofree gchar *provider = NULL;
	gboolean enabled = FALSE;
	if (uuid) {
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_INTEGRATION_CONNECTION);
		if (strlen(uuid) > 64) { denied(error); return NULL; }
		venture_query_add_filter_string(query, "uuid", VENTURE_FILTER_OP_EQ, uuid, NULL);
		row = venture_database_find_one(self->database, query, NULL);
	} else row = venture_database_get(self->database, VENTURE_TYPE_INTEGRATION_CONNECTION, id, NULL);
	if (row) g_object_get(row, "provider", &provider, "enabled", &enabled, NULL);
	if (!row || venture_entity_is_deleted(row) || !enabled || g_strcmp0(provider, "oidc")) { denied(error); return NULL; }
	org = venture_database_get(self->database, VENTURE_TYPE_ORGANIZATION, venture_entity_get_organization_id(row), NULL);
	if (!org || venture_entity_is_deleted(org)) { denied(error); return NULL; }
	return VENTURE_INTEGRATION_CONNECTION(g_steal_pointer(&row));
}
static JsonNode *credentials(VentureOidcService *self, VentureIntegrationConnection *binding, GError **error)
{
	g_autoptr(VentureAccessScope) internal = venture_access_policy_enter(venture_database_get_access_policy(self->database), NULL);
	return venture_integration_service_resolve_version(venture_integration_service_get(self->database),
		venture_entity_get_organization_id(VENTURE_ENTITY(binding)), venture_entity_get_id(VENTURE_ENTITY(binding)),
		venture_entity_get_version(VENTURE_ENTITY(binding)), FALSE, error);
}
VentureIntegrationConnection *venture_oidc_service_connect(VentureOidcService *self, gint64 org,
	const gchar *issuer, const gchar *client, const gchar *secret, gint64 version, const VentureActor *actor, GError **error)
{
	g_autoptr(JsonNode) settings = json_node_new(JSON_NODE_OBJECT);
	g_autofree gchar *callback = NULL, *account_source = NULL, *account = NULL;
	g_return_val_if_fail(VENTURE_IS_OIDC_SERVICE(self), NULL);
	if (!ready(self, error) || !approved_issuer(self, issuer, error)) return NULL;
	callback = venture_oidc_service_callback_uri(self, error); if (!callback) return NULL;
	if (!client || !*client || strlen(client) > 255 || !secret || !*secret || strlen(secret) > 4096)
		{ fail(error, VENTURE_ERROR_VALIDATION, "OIDC requires a bounded confidential client ID and secret"); return NULL; }
	json_node_take_object(settings, json_object_new());
	json_object_set_string_member(json_node_get_object(settings), "issuer", issuer);
	json_object_set_string_member(json_node_get_object(settings), "client_id", client);
	json_object_set_string_member(json_node_get_object(settings), "client_secret", secret);
	account_source = g_strdup_printf("%s\n%s", issuer, client); account = g_compute_checksum_for_string(G_CHECKSUM_SHA256, account_source, -1);
	return venture_integration_service_configure(venture_integration_service_get(self->database), org,
		"oidc", account, g_str_has_prefix(issuer, "https:") ? "live" : "test", settings, version, actor, error);
}
static const gchar *member(JsonObject *object, const gchar *name)
{
	JsonNode *node = json_object_get_member(object, name);
	return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING ? json_node_get_string(node) : NULL;
}
static gboolean contains(JsonObject *object, const gchar *name, const gchar *value)
{
	JsonNode *node = json_object_get_member(object, name);
	JsonArray *array;
	guint i;
	if (!node || !JSON_NODE_HOLDS_ARRAY(node)) return FALSE;
	array = json_node_get_array(node);
	for (i = 0; i < json_array_get_length(array); i++) {
		JsonNode *item = json_array_get_element(array, i);
		if (JSON_NODE_HOLDS_VALUE(item) && json_node_get_value_type(item) == G_TYPE_STRING &&
			!g_strcmp0(json_node_get_string(item), value)) return TRUE;
	}
	return FALSE;
}
static GObject *cached_validator(VentureOidcService *self, VentureEntity *binding,
	const gchar *issuer, const gchar *client, const gchar *jwks, GError **error)
{
	g_autofree gchar *key = g_strdup_printf("%" G_GINT64_FORMAT ":%" G_GINT64_FORMAT ":%s",
		venture_entity_get_id(binding), venture_entity_get_version(binding), jwks);
	GObject *validator = g_hash_table_lookup(self->validators, key);
	if (!validator) {
		validator = venture_oidc_verifier_new(issuer, client, jwks, error); if (!validator) return NULL;
		/* Reuse the library's bounded JWKS/rotation cache; provider churn cannot
		 * retain an unbounded collection of verifier instances in this process. */
		if (g_hash_table_size(self->validators) >= 32) g_hash_table_remove_all(self->validators);
		g_hash_table_insert(self->validators, g_steal_pointer(&key), validator);
	}
	return g_object_ref(validator);
}
static VentureUser *local_user(VentureOidcService *self, gint64 id, gint64 org, GError **error)
{
	g_autoptr(VentureEntity) user = venture_database_get(self->database, VENTURE_TYPE_USER, id, NULL);
	g_autoptr(GEnumClass) roles = g_type_class_ref(VENTURE_TYPE_ORGANIZATION_ROLE);
	g_autofree gint *allowed = g_new(gint, roles->n_values);
	VentureAuthPrincipal principal;
	gboolean active = FALSE;
	guint i;
	if (user) g_object_get(user, "active", &active, NULL);
	if (!user || !active || venture_entity_is_deleted(user)) { denied(error); return NULL; }
	principal.user_id = id; principal.token_id = 0; principal.authenticated = TRUE; principal.name = NULL;
	g_object_get(user, "role", &principal.role, NULL);
	for (i = 0; i < roles->n_values; i++) allowed[i] = roles->values[i].value;
	if (!venture_access_policy_has_organization_role(venture_database_get_access_policy(self->database),
		&principal, org, allowed, roles->n_values)) { denied(error); return NULL; }
	return VENTURE_USER(g_steal_pointer(&user));
}
static gchar *challenge(const gchar *verifier)
{
	g_autoptr(GChecksum) hash = g_checksum_new(G_CHECKSUM_SHA256);
	guchar bytes[32];
	gsize length = sizeof bytes;
	gchar *value, *p;
	g_checksum_update(hash, (const guchar *)verifier, strlen(verifier));
	g_checksum_get_digest(hash, bytes, &length); value = g_base64_encode(bytes, length);
	for (p = value; *p; p++) { if (*p == '+') *p = '-'; else if (*p == '/') *p = '_'; else if (*p == '=') { *p = 0; break; } }
	return value;
}
static gboolean budget(VentureOidcService *self, const gchar *peer, GError **error)
{
	GHashTableIter iter;
	gpointer value;
	guint same = 0;
	gint64 now = g_get_monotonic_time();
	Budget *counter;
	g_hash_table_iter_init(&iter, self->attempts);
	while (g_hash_table_iter_next(&iter, NULL, &value)) {
		Attempt *a = value;
		if (a->expires <= now) g_hash_table_iter_remove(&iter);
		else if (!g_strcmp0(a->peer_hash, peer)) same++;
	}
	g_hash_table_iter_init(&iter, self->budgets);
	while (g_hash_table_iter_next(&iter, NULL, &value))
		if (((Budget *)value)->expires <= now) g_hash_table_iter_remove(&iter);
	if (same >= 10 || g_hash_table_size(self->attempts) >= OIDC_MAX_ATTEMPTS) return denied(error);
	counter = g_hash_table_lookup(self->budgets, peer);
	if (!counter) {
		if (g_hash_table_size(self->budgets) >= OIDC_MAX_ATTEMPTS) return denied(error);
		counter = g_new0(Budget, 1); counter->expires = now + OIDC_ATTEMPT_SECONDS * G_USEC_PER_SEC;
		g_hash_table_insert(self->budgets, g_strdup(peer), counter);
	}
	if (counter->count >= 10) return denied(error);
	counter->count++;
	return TRUE;
}
gchar *venture_oidc_service_begin(VentureOidcService *self, const gchar *uuid, gint64 user_id,
	const gchar *password, const gchar *remote, gchar **browser_token, GError **error)
{
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(VentureIntegrationConnection) binding = NULL;
	g_autoptr(VentureUser) user = NULL;
	g_autoptr(JsonNode) settings = NULL, discovery = NULL;
	g_autofree gchar *callback = NULL, *discovery_uri = NULL, *peer = NULL, *state = NULL, *form = NULL, *s256 = NULL;
	const gchar *issuer, *client, *authorize, *token, *jwks;
	JsonObject *metadata;
	Attempt *a;
	g_return_val_if_fail(VENTURE_IS_OIDC_SERVICE(self), NULL);
	g_return_val_if_fail(browser_token != NULL, NULL);
	*browser_token = NULL;
	if (!ready(self, error)) return NULL;
	peer = g_compute_checksum_for_string(G_CHECKSUM_SHA256, remote ? remote : "", -1);
	if (!budget(self, peer, error)) return NULL;
	internal = venture_access_policy_enter(venture_database_get_access_policy(self->database), NULL);
	binding = connection(self, 0, uuid, error); if (!binding) return NULL;
	if (user_id) {
		user = local_user(self, user_id, venture_entity_get_organization_id(VENTURE_ENTITY(binding)), error);
		if (!user) return NULL;
		if (!password || !venture_user_check_password(user, password)) { denied(error); return NULL; }
	}
	settings = credentials(self, binding, error); if (!settings) return NULL;
	issuer = member(json_node_get_object(settings), "issuer"); client = member(json_node_get_object(settings), "client_id");
	if (!approved_issuer(self, issuer, error)) return NULL;
	if (!client || !*client || strlen(client) > 255) { denied(error); return NULL; }
	callback = venture_oidc_service_callback_uri(self, error); if (!callback) return NULL;
	discovery_uri = g_strconcat(issuer, g_str_has_suffix(issuer, "/") ? ".well-known/openid-configuration" : "/.well-known/openid-configuration", NULL);
	discovery = http_json(discovery_uri, NULL, error); if (!discovery) return NULL;
	metadata = json_node_get_object(discovery);
	authorize = member(metadata, "authorization_endpoint"); token = member(metadata, "token_endpoint"); jwks = member(metadata, "jwks_uri");
	if (g_strcmp0(member(metadata, "issuer"), issuer) || !same_origin(self, issuer, authorize) ||
		!same_origin(self, issuer, token) || !same_origin(self, issuer, jwks) ||
		!contains(metadata, "response_types_supported", "code") || !contains(metadata, "code_challenge_methods_supported", "S256") ||
		!contains(metadata, "token_endpoint_auth_methods_supported", "client_secret_post")) { denied(error); return NULL; }
	a = g_new0(Attempt, 1); a->connection = venture_entity_get_id(VENTURE_ENTITY(binding));
	a->version = venture_entity_get_version(VENTURE_ENTITY(binding)); a->user = user_id;
	a->user_version = user ? venture_entity_get_version(VENTURE_ENTITY(user)) : 0;
	a->expires = g_get_monotonic_time() + OIDC_ATTEMPT_SECONDS * G_USEC_PER_SEC;
	a->issuer = g_strdup(issuer); a->client = g_strdup(client); a->token_uri = g_strdup(token);
	a->nonce = venture_generate_token(32); a->verifier = venture_generate_token(32); a->peer_hash = g_steal_pointer(&peer);
	a->validator = cached_validator(self, VENTURE_ENTITY(binding), issuer, client, jwks, error);
	if (!a->validator) { attempt_free(a); return NULL; }
	*browser_token = venture_generate_token(32);
	a->browser_hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, *browser_token, -1);
	state = venture_generate_token(32); s256 = challenge(a->verifier);
	form = soup_form_encode("response_type", "code", "scope", "openid", "client_id", client,
		"redirect_uri", callback, "state", state, "nonce", a->nonce, "code_challenge", s256,
		"code_challenge_method", "S256", NULL);
	g_hash_table_insert(self->attempts, g_steal_pointer(&state), a);
	return g_strconcat(authorize, "?", form, NULL);
}
VentureUser *venture_oidc_service_identity_user(VentureOidcService *self, gint64 id, GError **error)
{
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(VentureEntity) identity = NULL;
	g_autoptr(VentureIntegrationConnection) binding = NULL;
	g_autoptr(JsonNode) settings = NULL;
	g_autofree gchar *issuer = NULL;
	gint64 connection_id = 0, user_id = 0;
	gboolean active = FALSE;
	g_return_val_if_fail(VENTURE_IS_OIDC_SERVICE(self), NULL);
	if (!ready(self, error)) return NULL;
	internal = venture_access_policy_enter(venture_database_get_access_policy(self->database), NULL);
	identity = venture_database_get(self->database, VENTURE_TYPE_OIDC_IDENTITY, id, NULL);
	if (identity) g_object_get(identity, "active", &active, "user-id", &user_id, "connection-id", &connection_id, "issuer", &issuer, NULL);
	if (!identity || venture_entity_is_deleted(identity) || !active || !approved_issuer(self, issuer, error)) { if (!error || !*error) denied(error); return NULL; }
	binding = connection(self, connection_id, NULL, error); if (!binding) return NULL;
	if (venture_entity_get_organization_id(identity) != venture_entity_get_organization_id(VENTURE_ENTITY(binding))) { denied(error); return NULL; }
	settings = credentials(self, binding, error); if (!settings) return NULL;
	if (g_strcmp0(issuer, member(json_node_get_object(settings), "issuer"))) { denied(error); return NULL; }
	return local_user(self, user_id, venture_entity_get_organization_id(identity), error);
}
gchar *venture_oidc_service_dup_session_binding(VentureOidcService *self, gint64 id, GError **error)
{
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(VentureEntity) identity = NULL;
	g_autoptr(VentureIntegrationConnection) binding = NULL;
	gint64 connection_id = 0;
	gboolean active = FALSE;
	g_return_val_if_fail(VENTURE_IS_OIDC_SERVICE(self), NULL);
	if (!ready(self, error)) return NULL;
	internal = venture_access_policy_enter(venture_database_get_access_policy(self->database), NULL);
	identity = venture_database_get(self->database, VENTURE_TYPE_OIDC_IDENTITY, id, NULL);
	if (identity) g_object_get(identity, "connection-id", &connection_id, "active", &active, NULL);
	if (!identity || !active || venture_entity_is_deleted(identity)) { denied(error); return NULL; }
	binding = connection(self, connection_id, NULL, error); if (!binding) return NULL;
	return g_strdup_printf("%" G_GINT64_FORMAT ":%" G_GINT64_FORMAT, venture_entity_get_version(identity),
		venture_entity_get_version(VENTURE_ENTITY(binding)));
}
VentureOidcIdentity *venture_oidc_service_finish(VentureOidcService *self, const gchar *state,
	const gchar *code, const gchar *browser, gint64 user_id, GError **error)
{
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(VentureIntegrationConnection) binding = NULL;
	g_autoptr(JsonNode) settings = NULL, response = NULL;
	g_autoptr(VentureUser) user = NULL;
	g_autoptr(VentureEntity) identity = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autofree gchar *browser_hash = NULL, *form = NULL, *callback = NULL, *subject = NULL, *source = NULL, *key = NULL, *actor_name = NULL;
	gpointer stored_key = NULL, stored_value = NULL;
	Attempt *a;
	gint64 org, linked_user = 0;
	const gchar *secret, *token;
	VentureActor actor;
	gboolean ok = FALSE, transaction = FALSE;
	g_return_val_if_fail(VENTURE_IS_OIDC_SERVICE(self), NULL);
	if (!ready(self, error)) return NULL;
	if (!state || strlen(state) != 64 || !g_hash_table_steal_extended(self->attempts, state, &stored_key, &stored_value)) { denied(error); return NULL; }
	g_free(stored_key); a = stored_value;
	/* Even an invalid response consumes state: retry always starts a new flow. */
	if (!code || !*code || strlen(code) > 4096 || !browser || strlen(browser) != 64 ||
		a->expires <= g_get_monotonic_time() || a->user != user_id) { denied(error); goto done; }
	browser_hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, browser, -1);
	if (!venture_constant_time_equal(browser_hash, a->browser_hash)) { denied(error); goto done; }
	internal = venture_access_policy_enter(venture_database_get_access_policy(self->database), NULL);
	binding = connection(self, a->connection, NULL, error); if (!binding) goto done;
	if (venture_entity_get_version(VENTURE_ENTITY(binding)) != a->version || !approved_issuer(self, a->issuer, error)) { if (!error || !*error) denied(error); goto done; }
	org = venture_entity_get_organization_id(VENTURE_ENTITY(binding));
	if (user_id) {
		user = local_user(self, user_id, org, error); if (!user) goto done;
		if (venture_entity_get_version(VENTURE_ENTITY(user)) != a->user_version) { denied(error); goto done; }
	}
	settings = credentials(self, binding, error); if (!settings) goto done;
	secret = member(json_node_get_object(settings), "client_secret");
	if (!secret || !*secret || strlen(secret) > 4096) { denied(error); goto done; }
	callback = venture_oidc_service_callback_uri(self, error); if (!callback) goto done;
	form = soup_form_encode("grant_type", "authorization_code", "code", code, "client_id", a->client,
		"client_secret", secret, "redirect_uri", callback, "code_verifier", a->verifier, NULL);
	response = http_json(a->token_uri, form, error); if (!response) goto done;
	token = member(json_node_get_object(response), "id_token");
	if (!token || !*token) { denied(error); goto done; }
	subject = venture_oidc_verifier_subject(a->validator, a->issuer, a->client, token, a->nonce, error); if (!subject) goto done;
	if (!venture_database_begin(self->database, error)) goto done;
	transaction = TRUE;
	/* HTTP may outlive a password change or credential rotation. Re-read the
	 * locally authoritative rows inside the final write transaction. */
	g_clear_object(&binding); binding = connection(self, a->connection, NULL, error); if (!binding) goto done;
	if (venture_entity_get_version(VENTURE_ENTITY(binding)) != a->version) { denied(error); goto done; }
	if (user_id) {
		g_clear_object(&user); user = local_user(self, user_id, org, error); if (!user) goto done;
		if (venture_entity_get_version(VENTURE_ENTITY(user)) != a->user_version) { denied(error); goto done; }
	}
	source = g_strdup_printf("%" G_GINT64_FORMAT "\n%s\n%s", org, a->issuer, subject);
	key = g_compute_checksum_for_string(G_CHECKSUM_SHA256, source, -1);
	query = venture_query_new(VENTURE_TYPE_OIDC_IDENTITY);
	venture_query_add_filter_string(query, "identity-key", VENTURE_FILTER_OP_EQ, key, NULL);
	identity = venture_database_find_one(self->database, query, error);
	if (error && *error) goto done;
	if (identity) g_object_get(identity, "user-id", &linked_user, NULL);
	if (user_id) {
		if (identity && linked_user != user_id) { denied(error); goto done; }
		if (!identity) { identity = g_object_new(VENTURE_TYPE_OIDC_IDENTITY, NULL); venture_entity_set_organization_id(identity, org); }
		g_object_set(identity, "title", a->issuer, "user-id", user_id, "connection-id", a->connection,
			"issuer", a->issuer, "subject", subject, "identity-key", key, "active", TRUE, NULL);
		g_object_get(user, "username", &actor_name, NULL);
		actor.kind = VENTURE_ACTOR_KIND_USER; actor.name = actor_name; actor.prompt = NULL; actor.request_id = NULL; actor.approved_by = NULL;
		if (!save(self, identity, &actor, error)) goto done;
	} else if (!identity) { denied(error); goto done; }
	g_clear_object(&user);
	user = venture_oidc_service_identity_user(self, venture_entity_get_id(identity), error);
	ok = user != NULL;
	if (ok) { ok = venture_database_commit(self->database, error); transaction = FALSE; }
 done:
	if (transaction) venture_database_rollback(self->database);
	attempt_free(a);
	return ok ? VENTURE_OIDC_IDENTITY(g_steal_pointer(&identity)) : NULL;
}
gboolean venture_oidc_service_unlink(VentureOidcService *self, gint64 id, gint64 user_id,
	const gchar *password, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(VentureEntity) identity = NULL, user = NULL;
	g_autoptr(GDateTime) now = NULL;
	g_autofree gchar *budget_key = NULL;
	gint64 linked_user = 0;
	gboolean ok;
	g_return_val_if_fail(VENTURE_IS_OIDC_SERVICE(self), FALSE);
	if (!self->database) return fail(error, VENTURE_ERROR_FAILED, "OIDC repository is no longer available");
	budget_key = g_strdup_printf("unlink:%" G_GINT64_FORMAT, user_id);
	if (!budget(self, budget_key, error)) return FALSE;
	internal = venture_access_policy_enter(venture_database_get_access_policy(self->database), NULL);
	identity = venture_database_get(self->database, VENTURE_TYPE_OIDC_IDENTITY, id, NULL);
	user = venture_database_get(self->database, VENTURE_TYPE_USER, user_id, NULL);
	if (identity) g_object_get(identity, "user-id", &linked_user, NULL);
	if (!identity || linked_user != user_id || !user || !password || !venture_user_check_password(VENTURE_USER(user), password)) return denied(error);
	if (!venture_database_begin(self->database, error)) return FALSE;
	g_object_set(identity, "active", FALSE, NULL); now = venture_time_now();
	g_object_set(user, "sessions-invalidated-at", now, NULL);
	ok = save(self, identity, actor, error) && venture_database_save(self->database, user, actor, error);
	if (ok) ok = venture_database_commit(self->database, error); else venture_database_rollback(self->database);
	return ok;
}
