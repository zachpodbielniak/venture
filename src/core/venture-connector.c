/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <openssl/crypto.h>
#include <string.h>

/* Transport implementations remain the IMAP/CalDAV interfaces. This table
 * declares only the stable account identity used by the shared vault. */
typedef struct {
	GType (*type)(void);
	const gchar *provider;
	const gchar *fields[12];
} ConnectorKind;
static const ConnectorKind kinds[] = {
	{ venture_mail_account_get_type, "imap", { "address", "imap-host", "imap-port", "imap-tls", "username", "private-owner-id", NULL } },
	{ venture_calendar_account_get_type, "caldav", { "url", "username", "calendar-path", "owner", "private-owner-id", NULL } }
};
struct _VentureConnectorSession {
	GObject parent_instance;
	VentureDatabase *database;
	VentureConfig *config;
	VentureEntity *account;
	GThread *owner;
	gchar *identity;
	gchar *selection;
	gchar *password;
	gint64 binding;
	gint64 version;
};
G_DEFINE_FINAL_TYPE(VentureConnectorSession, venture_connector_session, G_TYPE_OBJECT)
static gboolean refuse(GError **error, VentureError code, const gchar *message)
{
	g_set_error_literal(error, VENTURE_ERROR, code, message);
	return FALSE;
}
static void finalize(GObject *object)
{
	VentureConnectorSession *self = VENTURE_CONNECTOR_SESSION(object);
	if (self->database) g_object_remove_weak_pointer(G_OBJECT(self->database), (gpointer *)&self->database);
	g_clear_object(&self->config);
	g_clear_object(&self->account);
	g_free(self->identity);
	g_free(self->selection);
	if (self->password) OPENSSL_cleanse(self->password, strlen(self->password));
	g_free(self->password);
	g_thread_unref(self->owner);
	G_OBJECT_CLASS(venture_connector_session_parent_class)->finalize(object);
}
static void venture_connector_session_class_init(VentureConnectorSessionClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = finalize;
}
static void venture_connector_session_init(VentureConnectorSession *self)
{
	self->owner = g_thread_ref(g_thread_self());
}
static const ConnectorKind *kind_for(VentureEntity *account)
{
	guint i;
	for (i = 0; i < G_N_ELEMENTS(kinds); i++)
		if (g_type_is_a(G_OBJECT_TYPE(account), kinds[i].type())) return &kinds[i];
	return NULL;
}
gchar *venture_connector_binding_key(VentureEntity *account)
{
	const ConnectorKind *kind;
	g_return_val_if_fail(VENTURE_IS_ENTITY(account), NULL);
	kind = kind_for(account);
	if (!kind || !venture_entity_get_id(account) || !venture_entity_get_uuid(account)) return NULL;
	return g_strdup_printf("%s-%s", kind->provider, venture_entity_get_uuid(account));
}
static gboolean owner_active(VentureDatabase *database, VentureEntity *account, GError **error)
{
	VentureAccessPolicy *policy = venture_database_get_access_policy(database);
	gint64 owner = venture_access_policy_get_personal_owner(policy, account);
	g_autoptr(VentureEntity) user = NULL, member = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureAccessScope) internal = NULL;
	gboolean active = FALSE;
	if (owner == 0) return TRUE;
	if (owner < 0) return refuse(error, VENTURE_ERROR_PERMISSION_DENIED, "Private connector ownership is unavailable");
	internal = venture_access_policy_enter(policy, NULL);
	user = venture_database_get(database, VENTURE_TYPE_USER, owner, NULL);
	if (user && !venture_entity_is_deleted(user)) g_object_get(user, "active", &active, NULL);
	if (!active) return refuse(error, VENTURE_ERROR_PERMISSION_DENIED, "Private connector owner is inactive");
	query = venture_query_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP);
	venture_query_set_organization(query, venture_entity_get_organization_id(account));
	venture_query_add_filter_int(query, "user-id", VENTURE_FILTER_OP_EQ, owner, NULL);
	venture_query_add_filter_string(query, "active", VENTURE_FILTER_OP_EQ, "true", NULL);
	member = venture_database_find_one(database, query, error);
	if (!member) {
		if (error && *error) return FALSE;
		return refuse(error, VENTURE_ERROR_PERMISSION_DENIED, "Private connector owner is no longer an organization member");
	}
	return TRUE;
}
static VentureEntity *current_account(VentureDatabase *database, VentureEntity *account, GError **error)
{
	g_autoptr(VentureEntity) current = NULL;
	if (!kind_for(account) || venture_entity_get_id(account) <= 0)
	{ refuse(error, VENTURE_ERROR_VALIDATION, "Select a saved mailbox or calendar account"); return NULL; }
	current = venture_database_get(database, G_OBJECT_TYPE(account), venture_entity_get_id(account), error);
	if (!current) return NULL;
	if (venture_entity_is_deleted(current) || venture_entity_get_organization_id(current) <= 0)
	{ refuse(error, VENTURE_ERROR_NOT_FOUND, "Connector account is unavailable"); return NULL; }
	return g_steal_pointer(&current);
}
gboolean venture_connector_can_manage(VentureDatabase *database, VentureEntity *account, GError **error)
{
	static const gint roles[] = { VENTURE_ORGANIZATION_ROLE_OWNER, VENTURE_ORGANIZATION_ROLE_ADMIN };
	VentureAccessPolicy *policy;
	const VentureAuthPrincipal *principal;
	gint64 owner;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), FALSE);
	g_return_val_if_fail(VENTURE_IS_ENTITY(account), FALSE);
	policy = venture_database_get_access_policy(database);
	principal = venture_access_policy_get_actor(policy);
	owner = venture_access_policy_get_personal_owner(policy, account);
	if (!principal) return TRUE;
	if (!venture_access_policy_can(policy, principal, "read", account, error)) return FALSE;
	if (owner > 0 && principal->user_id == owner) return TRUE;
	if (venture_access_policy_has_organization_role(policy, principal,
		venture_entity_get_organization_id(account), roles, G_N_ELEMENTS(roles))) return TRUE;
	return refuse(error, VENTURE_ERROR_PERMISSION_DENIED, "Connector settings require its assigned owner or organization administration");
}
static void hash_value(GChecksum *hash, const gchar *value)
{
	g_autofree gchar *length = g_strdup_printf("%" G_GSIZE_FORMAT ":", value ? strlen(value) : 0);
	g_checksum_update(hash, (const guchar *)length, -1);
	if (value) g_checksum_update(hash, (const guchar *)value, -1);
}
static gchar *account_identity(VentureEntity *account)
{
	const ConnectorKind *kind = kind_for(account);
	g_autoptr(GChecksum) hash = g_checksum_new(G_CHECKSUM_SHA256);
	guint i;
	hash_value(hash, kind->provider);
	hash_value(hash, venture_entity_get_uuid(account));
	for (i = 0; kind->fields[i]; i++)
	{
		GParamSpec *field = g_object_class_find_property(G_OBJECT_GET_CLASS(account), kind->fields[i]);
		g_auto(GValue) value = G_VALUE_INIT;
		g_autofree gchar *text = NULL;
		g_value_init(&value, G_PARAM_SPEC_VALUE_TYPE(field));
		g_object_get_property(G_OBJECT(account), kind->fields[i], &value);
		text = g_strdup_value_contents(&value);
		hash_value(hash, text);
	}
	return g_strdup(g_checksum_get_string(hash));
}
/* Selection changes stop an in-flight import, but do not require rotating
 * credentials. Cursors, leases and status are intentionally excluded. */
static gchar *account_selection(VentureEntity *account)
{
	static const gchar *fields[] = { "folders", "capture-address", "capture-folder", "ignore-patterns", "internal-domains", "sync-since", NULL };
	g_autoptr(GChecksum) hash = g_checksum_new(G_CHECKSUM_SHA256);
	guint i;
	for (i = 0; VENTURE_IS_MAIL_ACCOUNT(account) && fields[i]; i++) {
		GParamSpec *field = g_object_class_find_property(G_OBJECT_GET_CLASS(account), fields[i]);
		g_auto(GValue) value = G_VALUE_INIT;
		g_autofree gchar *text = NULL;
		g_value_init(&value, G_PARAM_SPEC_VALUE_TYPE(field));
		g_object_get_property(G_OBJECT(account), fields[i], &value);
		if (G_VALUE_HOLDS(&value, G_TYPE_DATE_TIME)) {
			GDateTime *date = g_value_get_boxed(&value);
			text = date ? venture_time_to_string(date) : g_strdup("");
		} else text = g_strdup_value_contents(&value);
		hash_value(hash, text);
	}
	return g_strdup(g_checksum_get_string(hash));
}
static gboolean listed(const gchar *list, const gchar *value)
{
	g_auto(GStrv) entries = g_strsplit(list ? list : "", ",", -1);
	guint i;
	for (i = 0; entries[i]; i++)
		if (!g_ascii_strcasecmp(g_strstrip(entries[i]), value)) return TRUE;
	return FALSE;
}
static gboolean endpoint_allowed(VentureConfig *config, VentureEntity *account, GError **error)
{
	g_autofree gchar *allowed = NULL, *endpoint = NULL, *username = NULL;
	g_object_get(account, "username", &username, NULL);
	if (venture_string_is_empty(username) || strlen(username) > 4096)
		return refuse(error, VENTURE_ERROR_CONFIG, "Connector needs a bounded login name");
	if (VENTURE_IS_MAIL_ACCOUNT(account))
	{
		g_autofree gchar *host = NULL, *security = NULL;
		gint64 port;
		gboolean plaintext = FALSE;
		g_object_get(config, "imap-allowed-endpoints", &allowed, "connectors-allow-plaintext-loopback", &plaintext, NULL);
		g_object_get(account, "imap-host", &host, "imap-port", &port, "imap-tls", &security, NULL);
		if (venture_string_is_empty(host) || strpbrk(host, "/@?#\\ \t\r\n") || port <= 0 || port > 65535)
			return refuse(error, VENTURE_ERROR_CONFIG, "Connector needs an explicit IMAP host and port");
		if (g_strcmp0(security, "tls") && g_strcmp0(security, "starttls") &&
			!(plaintext && !g_strcmp0(security, "none") && (!g_strcmp0(host, "127.0.0.1") || !g_strcmp0(host, "::1"))))
			return refuse(error, VENTURE_ERROR_CONFIG, "IMAP requires TLS or STARTTLS");
		endpoint = g_strdup_printf("%s:%" G_GINT64_FORMAT, host, port);
	}
	else
	{
		g_autofree gchar *url = NULL, *path = NULL;
		g_autoptr(GUri) uri = NULL;
		gint port;
		g_object_get(config, "calendar-allowed-origins", &allowed, NULL);
		g_object_get(account, "url", &url, "calendar-path", &path, NULL);
		uri = url ? g_uri_parse(url, G_URI_FLAGS_NONE, NULL) : NULL;
		if (!uri || g_strcmp0(g_uri_get_scheme(uri), "https") || !g_uri_get_host(uri) ||
			g_uri_get_userinfo(uri) || g_uri_get_query(uri) || g_uri_get_fragment(uri) ||
			venture_string_is_empty(path) || *path != '/' || g_str_has_prefix(path, "//") || strstr(path, "://"))
			return refuse(error, VENTURE_ERROR_CONFIG, "CalDAV requires an HTTPS origin and a collection path without credentials");
		port = g_uri_get_port(uri);
		endpoint = g_uri_join(G_URI_FLAGS_NONE, "https", NULL, g_uri_get_host(uri),
			port == 443 ? -1 : port, "", NULL, NULL);
	}
	if (!listed(allowed, endpoint))
		return refuse(error, VENTURE_ERROR_CONFIG, "Connector endpoint is not in the operator allowlist");
	return TRUE;
}
JsonNode *venture_connector_settings_schema(void)
{
	return json_from_string("{\"type\":\"object\",\"additionalProperties\":false,\"required\":[\"password\"],\"properties\":{\"password\":{\"type\":\"string\",\"title\":\"Password or app token\",\"x-sensitive\":true}}}", NULL);
}
static const gchar *password_value(JsonObject *settings, GError **error)
{
	JsonNode *node = json_object_get_member(settings, "password");
	const gchar *password;
	if (json_object_get_size(settings) != 1 || !node || !JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_STRING)
	{ refuse(error, VENTURE_ERROR_CONFIG, "Connector settings require only a password string"); return NULL; }
	password = json_node_get_string(node);
	if (venture_string_is_empty(password) || strlen(password) > 16384)
	{ refuse(error, VENTURE_ERROR_CONFIG, "Connector password is empty or exceeds its limit"); return NULL; }
	return password;
}
static VentureIntegrationConnection *binding_for(VentureDatabase *database, VentureEntity *account, GError **error)
{
	g_autofree gchar *key = venture_connector_binding_key(account);
	return venture_integration_service_find(venture_integration_service_get(database), venture_entity_get_organization_id(account), key, error);
}
static gboolean exact_binding(VentureIntegrationConnection *binding, gint64 id, gint64 version, GError **error)
{
	if (id != (binding ? venture_entity_get_id(VENTURE_ENTITY(binding)) : 0) ||
		version != (binding ? venture_entity_get_version(VENTURE_ENTITY(binding)) : 0))
		return refuse(error, VENTURE_ERROR_CONFLICT, "Connector binding changed; reload settings");
	return TRUE;
}
VentureIntegrationConnection *venture_connector_configure(VentureDatabase *database,
	VentureConfig *config, VentureEntity *account, JsonObject *settings,
	gint64 expected_binding, gint64 expected_version, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) current = NULL;
	g_autoptr(VentureIntegrationConnection) binding = NULL, result = NULL;
	g_autoptr(GError) lookup_error = NULL;
	g_autoptr(VentureAccessScope) delegated = NULL;
	g_autoptr(JsonNode) values = json_node_new(JSON_NODE_OBJECT);
	g_autofree gchar *key = NULL, *identity = NULL;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	g_return_val_if_fail(VENTURE_IS_CONFIG(config), NULL);
	g_return_val_if_fail(VENTURE_IS_ENTITY(account), NULL);
	g_return_val_if_fail(settings != NULL, NULL);
	if (!password_value(settings, error)) return NULL;
	if (!venture_database_begin(database, error)) return NULL;
	current = current_account(database, account, error);
	if (!current || !owner_active(database, current, error) || !venture_connector_can_manage(database, current, error) || !endpoint_allowed(config, current, error)) goto fail;
	binding = binding_for(database, current, &lookup_error);
	if (lookup_error && !g_error_matches(lookup_error, VENTURE_ERROR, VENTURE_ERROR_CONFIG)) {
		g_propagate_error(error, g_steal_pointer(&lookup_error)); goto fail;
	}
	if (!exact_binding(binding, expected_binding, expected_version, error)) goto fail;
	key = venture_connector_binding_key(current);
	identity = account_identity(current);
	json_node_set_object(values, settings);
	delegated = venture_access_policy_enter(venture_database_get_access_policy(database), NULL);
	result = venture_integration_service_configure_for_owner(venture_integration_service_get(database),
		venture_entity_get_organization_id(current), key, identity, "live", values, expected_version,
		venture_access_policy_get_personal_owner(venture_database_get_access_policy(database), current), actor, error);
	if (!result) goto fail;
	if (!venture_database_commit(database, error)) return NULL;
	return g_steal_pointer(&result);
fail:
	venture_database_rollback(database);
	return NULL;
}
gboolean venture_connector_disconnect(VentureDatabase *database, VentureEntity *account,
	gint64 expected_binding, gint64 expected_version, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) current = NULL;
	g_autoptr(VentureIntegrationConnection) binding = NULL;
	g_autoptr(VentureAccessScope) delegated = NULL;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), FALSE);
	g_return_val_if_fail(VENTURE_IS_ENTITY(account), FALSE);
	current = current_account(database, account, error);
	if (!current || !venture_connector_can_manage(database, current, error)) return FALSE;
	binding = binding_for(database, current, error);
	if (!binding || !exact_binding(binding, expected_binding, expected_version, error)) return FALSE;
	delegated = venture_access_policy_enter(venture_database_get_access_policy(database), NULL);
	return venture_integration_service_disable(venture_integration_service_get(database),
		venture_entity_get_organization_id(current), expected_binding, expected_version, actor, error);
}
VentureConnectorSession *venture_connector_open(VentureDatabase *database,
	VentureConfig *config, VentureEntity *account, GError **error)
{
	g_autoptr(VentureConnectorSession) session = NULL;
	g_autoptr(VentureEntity) current = NULL;
	g_autoptr(VentureIntegrationConnection) binding = NULL;
	g_autoptr(JsonNode) settings = NULL;
	g_autofree gchar *identity = NULL, *retained = NULL;
	const gchar *password;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	g_return_val_if_fail(VENTURE_IS_CONFIG(config), NULL);
	g_return_val_if_fail(VENTURE_IS_ENTITY(account), NULL);
	current = current_account(database, account, error);
	if (!current || !owner_active(database, current, error) || !endpoint_allowed(config, current, error)) return NULL;
	binding = binding_for(database, current, error);
	if (!binding) return NULL;
	identity = account_identity(current);
	g_object_get(binding, "account-id", &retained, NULL);
	if (g_strcmp0(identity, retained))
	{ refuse(error, VENTURE_ERROR_CONFLICT, "Connector account identity changed; disconnect and configure it explicitly"); return NULL; }
	settings = venture_integration_service_resolve_version(venture_integration_service_get(database),
		venture_entity_get_organization_id(current), venture_entity_get_id(VENTURE_ENTITY(binding)),
		venture_entity_get_version(VENTURE_ENTITY(binding)), FALSE, error);
	if (!settings) return NULL;
	password = password_value(json_node_get_object(settings), error);
	if (!password) return NULL;
	session = g_object_new(VENTURE_TYPE_CONNECTOR_SESSION, NULL);
	session->database = database;
	g_object_add_weak_pointer(G_OBJECT(database), (gpointer *)&session->database);
	session->config = g_object_ref(config);
	session->account = g_steal_pointer(&current);
	session->password = g_strdup(password);
	session->identity = g_steal_pointer(&identity);
	session->selection = account_selection(session->account);
	session->binding = venture_entity_get_id(VENTURE_ENTITY(binding));
	session->version = venture_entity_get_version(VENTURE_ENTITY(binding));
	return g_steal_pointer(&session);
}
VentureEntity *venture_connector_session_get_account(VentureConnectorSession *self)
{
	g_return_val_if_fail(VENTURE_IS_CONNECTOR_SESSION(self), NULL);
	return self->account;
}
const gchar *venture_connector_session_get_password(VentureConnectorSession *self)
{
	g_return_val_if_fail(VENTURE_IS_CONNECTOR_SESSION(self), NULL);
	return self->password;
}
gboolean venture_connector_session_validate(VentureConnectorSession *self, GError **error)
{
	g_autoptr(VentureEntity) current = NULL;
	g_autoptr(JsonNode) settings = NULL;
	g_autofree gchar *identity = NULL, *selection = NULL;
	g_return_val_if_fail(VENTURE_IS_CONNECTOR_SESSION(self), FALSE);
	if (self->owner != g_thread_self()) return refuse(error, VENTURE_ERROR_CONFLICT, "Connector validation belongs to its owning thread");
	if (!self->database) return refuse(error, VENTURE_ERROR_CONFIG, "Connector repository is closed");
	current = current_account(self->database, self->account, error);
	if (!current || !owner_active(self->database, current, error) || !endpoint_allowed(self->config, current, error)) return FALSE;
	identity = account_identity(current);
	if (g_strcmp0(identity, self->identity)) return refuse(error, VENTURE_ERROR_CONFLICT, "Connector account identity changed during the request");
	selection = account_selection(current);
	if (g_strcmp0(selection, self->selection)) return refuse(error, VENTURE_ERROR_CONFLICT, "Connector selection changed during the request");
	settings = venture_integration_service_resolve_version(venture_integration_service_get(self->database),
		venture_entity_get_organization_id(current), self->binding, self->version, FALSE, error);
	return settings != NULL;
}

/* Once an account has carried credentials, reusing its cursors or event links
 * for another remote identity would conflate two sources. Use a new account. */
static gboolean validate_account(VentureDatabase *database, VentureEntity *entity,
	VentureEntity *previous, gpointer data, GError **error)
{
	static const gint roles[] = { VENTURE_ORGANIZATION_ROLE_OWNER, VENTURE_ORGANIZATION_ROLE_ADMIN };
	VentureAccessPolicy *policy = venture_database_get_access_policy(database);
	const VentureAuthPrincipal *principal = venture_access_policy_get_actor(policy);
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) bindings = NULL;
	g_autofree gchar *key = NULL, *before = NULL, *after = NULL;
	(void)data;
	if (!previous) {
		if (!principal || venture_access_policy_has_organization_role(policy, principal,
			venture_entity_get_organization_id(entity), roles, G_N_ELEMENTS(roles))) return TRUE;
		return refuse(error, VENTURE_ERROR_PERMISSION_DENIED, "Organization administration must assign a connector account before credentials are delegated");
	}
	before = account_identity(previous); after = account_identity(entity);
	if (!g_strcmp0(before, after) && venture_entity_get_organization_id(previous) == venture_entity_get_organization_id(entity)) return TRUE;
	internal = venture_access_policy_enter(policy, NULL);
	query = venture_query_new(VENTURE_TYPE_INTEGRATION_CONNECTION);
	key = venture_connector_binding_key(previous);
	venture_query_set_organization(query, venture_entity_get_organization_id(previous));
	venture_query_set_include_deleted(query, TRUE);
	venture_query_add_filter_string(query, "provider", VENTURE_FILTER_OP_EQ, key, NULL);
	bindings = venture_database_find(database, query, error);
	if (!bindings) return FALSE;
	if (bindings->len) return refuse(error, VENTURE_ERROR_VALIDATION, "A previously configured connector keeps its identity; create a new account for a different source");
	return TRUE;
}
void venture_connector_install(VentureDatabase *database)
{
	guint i;
	g_return_if_fail(VENTURE_IS_DATABASE(database));
	for (i = 0; i < G_N_ELEMENTS(kinds); i++)
		venture_database_add_save_validator(database, kinds[i].type(), validate_account, NULL, NULL);
}
