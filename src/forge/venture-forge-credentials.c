/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
#include <string.h>

struct _VentureForgeCredentials
{
	GObject parent_instance;
	GCancellable *cancellable;
	gint64 forge_id, forge_version, organization_id, organization_version;
	gint64 connection_id, connection_version;
	gchar *base_url, *token, *secret, *account;
};
G_DEFINE_FINAL_TYPE(VentureForgeCredentials, venture_forge_credentials, G_TYPE_OBJECT)

static gboolean refuse(GError **error, const gchar *message)
{
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG, message);
	return FALSE;
}
static void erase(gchar *value)
{
	if (value != NULL) { volatile gchar *p = (volatile gchar *)value; gsize n = strlen(value); while (n--) *p++ = 0; g_free(value); }
}
static void credentials_finalize(GObject *object)
{
	VentureForgeCredentials *self = VENTURE_FORGE_CREDENTIALS(object);
	g_clear_object(&self->cancellable);
	g_free(self->base_url); g_free(self->account); erase(self->token); erase(self->secret);
	G_OBJECT_CLASS(venture_forge_credentials_parent_class)->finalize(object);
}
static void venture_forge_credentials_class_init(VentureForgeCredentialsClass *klass)
{ G_OBJECT_CLASS(klass)->finalize = credentials_finalize; }
static void venture_forge_credentials_init(VentureForgeCredentials *self)
{ self->cancellable = g_cancellable_new(); }

/* Database-owned weak entries keep finalization on either thread safe, without
 * retaining secrets until database shutdown or touching DB from a worker. */
static void weak_free(gpointer data)
{ GWeakRef *ref = data; g_weak_ref_clear(ref); g_free(ref); }
static void credentials_changed(VentureDatabase *database, VentureEntity *entity, gpointer data)
{
	GPtrArray *entries = data;
	guint i;
	(void)database;
	for (i = 0; i < entries->len;)
	{
		g_autoptr(VentureForgeCredentials) lease = g_weak_ref_get(g_ptr_array_index(entries, i));
		gint64 id = venture_entity_get_id(entity);
		if (lease == NULL) { g_ptr_array_remove_index_fast(entries, i); continue; }
		if ((VENTURE_IS_FORGE(entity) && id == lease->forge_id) ||
			(VENTURE_IS_INTEGRATION_CONNECTION(entity) && id == lease->connection_id) ||
			(VENTURE_IS_ORGANIZATION(entity) && id == lease->organization_id))
			g_cancellable_cancel(lease->cancellable);
		i++;
	}
}
static void credentials_saved(VentureDatabase *database, VentureEntity *entity, gboolean created, gpointer data)
{ (void)created; credentials_changed(database, entity, data); }
static void credentials_entries_free(gpointer data)
{
	GPtrArray *entries = data;
	guint i;
	for (i = 0; i < entries->len; i++)
	{
		g_autoptr(VentureForgeCredentials) lease = g_weak_ref_get(g_ptr_array_index(entries, i));
		if (lease != NULL) g_cancellable_cancel(lease->cancellable);
	}
	g_ptr_array_unref(entries);
}
static void credentials_watch(VentureDatabase *database, VentureForgeCredentials *self)
{
	GPtrArray *entries = g_object_get_data(G_OBJECT(database), "venture-forge-credentials");
	GWeakRef *ref;
	guint i;
	if (entries == NULL)
	{
		entries = g_ptr_array_new_with_free_func(weak_free);
		g_object_set_data_full(G_OBJECT(database), "venture-forge-credentials", entries, credentials_entries_free);
		g_signal_connect(database, "entity-saved", G_CALLBACK(credentials_saved), entries);
		g_signal_connect(database, "entity-deleted", G_CALLBACK(credentials_changed), entries);
	}
	for (i = 0; i < entries->len;)
	{
		g_autoptr(VentureForgeCredentials) old = g_weak_ref_get(g_ptr_array_index(entries, i));
		if (!old) { g_ptr_array_remove_index_fast(entries, i); continue; }
		i++;
	}
	ref = g_new0(GWeakRef, 1); g_weak_ref_init(ref, self); g_ptr_array_add(entries, ref);
}
static gchar *provider_key(VentureEntity *forge)
{ return g_strdup_printf("forge_%" G_GINT64_FORMAT, venture_entity_get_id(forge)); }
static VentureEntity *load_forge(VentureDatabase *database, gint64 id, GError **error)
{
	g_autoptr(VentureEntity) forge = venture_database_get(database, VENTURE_TYPE_FORGE, id, error);
	if (forge == NULL) return NULL;
	if (venture_entity_is_deleted(forge) || venture_entity_get_organization_id(forge) <= 0 ||
		venture_entity_registry_lookup(venture_entity_registry_get_default(), "forge") == G_TYPE_INVALID)
	{ refuse(error, "Forge requires an active explicit organization and enabled module"); return NULL; }
	return g_steal_pointer(&forge);
}
VentureIntegrationConnection *venture_forge_settings_find(VentureDatabase *database, gint64 forge_id, GError **error)
{
	g_autoptr(VentureEntity) forge = load_forge(database, forge_id, error);
	g_autofree gchar *provider = NULL;
	if (forge == NULL) return NULL;
	provider = provider_key(forge);
	return venture_integration_service_find(venture_integration_service_get(database), venture_entity_get_organization_id(forge), provider, error);
}
VentureForgeCredentials *venture_forge_credentials_acquire(VentureDatabase *database, gint64 forge_id, GError **error)
{
	g_autoptr(VentureEntity) forge = load_forge(database, forge_id, error);
	g_autoptr(VentureEntity) organization = NULL;
	g_autoptr(VentureIntegrationConnection) binding = NULL;
	g_autoptr(JsonNode) settings = NULL;
	g_autoptr(VentureForgeCredentials) self = NULL;
	g_autofree gchar *base = NULL;
	gboolean active = FALSE, organization_active = FALSE;
	VentureForgeKind kind;
	JsonObject *object;
	if (forge == NULL) return NULL;
	binding = venture_forge_settings_find(database, forge_id, error);
	if (binding == NULL) return NULL;
	organization = venture_database_get(database, VENTURE_TYPE_ORGANIZATION, venture_entity_get_organization_id(forge), error);
	if (organization == NULL) return NULL;
	g_object_get(forge, "base-url", &base, "active", &active, "kind", &kind, NULL);
	g_object_get(organization, "active", &organization_active, NULL);
	if ((kind != VENTURE_FORGE_KIND_FORGEJO && kind != VENTURE_FORGE_KIND_GITEA) || !active || !organization_active || venture_entity_is_deleted(organization))
	{ refuse(error, "Forge or organization is inactive"); return NULL; }
	settings = venture_integration_service_resolve_version(venture_integration_service_get(database),
		venture_entity_get_organization_id(forge), venture_entity_get_id(VENTURE_ENTITY(binding)),
		venture_entity_get_version(VENTURE_ENTITY(binding)), FALSE, error);
	if (settings == NULL) return NULL;
	object = json_node_get_object(settings);
	if (g_strcmp0(base, venture_json_object_get_string(object, "base_url", NULL)) != 0 ||
		venture_json_object_get_int(object, "kind", -1) != (gint64)kind)
	{ refuse(error, "Forge origin changed; disconnect and explicitly configure the new origin"); return NULL; }
	self = g_object_new(VENTURE_TYPE_FORGE_CREDENTIALS, NULL);
	self->forge_id = forge_id; self->forge_version = venture_entity_get_version(forge);
	self->organization_id = venture_entity_get_organization_id(forge);
	self->organization_version = venture_entity_get_version(organization);
	self->connection_id = venture_entity_get_id(VENTURE_ENTITY(binding));
	self->connection_version = venture_entity_get_version(VENTURE_ENTITY(binding));
	self->base_url = g_strdup(base);
	g_object_get(binding, "account-id", &self->account, NULL);
	self->token = g_strdup(venture_json_object_get_string(object, "token", NULL));
	self->secret = g_strdup(venture_json_object_get_string(object, "webhook_secret", NULL));
	if (venture_string_is_empty(self->token) || venture_string_is_empty(self->secret))
	{ refuse(error, "Forge binding lacks required credentials"); return NULL; }
	credentials_watch(database, self);
	return g_steal_pointer(&self);
}
gboolean venture_forge_credentials_check(VentureForgeCredentials *self, GError **error)
{
	g_return_val_if_fail(VENTURE_IS_FORGE_CREDENTIALS(self), FALSE);
	return !g_cancellable_is_cancelled(self->cancellable) || refuse(error, "Forge credentials changed or were revoked; external outcome may be uncertain");
}
gboolean venture_forge_credentials_revalidate(VentureForgeCredentials *self, VentureDatabase *database, GError **error)
{
	g_autoptr(VentureForgeCredentials) current = NULL;
	if (!venture_forge_credentials_check(self, error)) return FALSE;
	current = venture_forge_credentials_acquire(database, self->forge_id, error);
	if (current == NULL) return FALSE;
	if (current->forge_version != self->forge_version || current->organization_id != self->organization_id ||
		current->organization_version != self->organization_version || current->connection_id != self->connection_id ||
		current->connection_version != self->connection_version)
	{ g_cancellable_cancel(self->cancellable); return refuse(error, "Forge configuration changed before result application"); }
	return TRUE;
}
GCancellable *venture_forge_credentials_get_cancellable(VentureForgeCredentials *self) { return self->cancellable; }
const gchar *venture_forge_credentials_get_token(VentureForgeCredentials *self) { return self->token; }
const gchar *venture_forge_credentials_get_webhook_secret(VentureForgeCredentials *self) { return self->secret; }
const gchar *venture_forge_credentials_get_account(VentureForgeCredentials *self) { return self->account; }
const gchar *venture_forge_credentials_get_base_url(VentureForgeCredentials *self) { return self->base_url; }
gboolean venture_forge_credentials_check_clone(VentureForgeCredentials *self, const gchar *url, GError **error)
{
	g_autoptr(GUri) clone = NULL, base = NULL;
	const gchar *scheme;
	if (!venture_forge_credentials_check(self, error)) return FALSE;
	if (venture_string_is_empty(url)) return refuse(error, "Clone address is empty");
	/* Only the established scp spelling is accepted without a URI scheme. */
	if (strstr(url, "://") == NULL)
	{
		const gchar *at = strchr(url, '@'), *colon = strchr(url, ':');
		if (at != NULL && at > url && colon != NULL && colon > at + 1 && colon[1] && url[0] != '-' && at[1] != '-')
		{
			const gchar *p;
			for (p = url; p < colon; p++) if (!g_ascii_isalnum(*p) && !strchr("@_.-", *p)) break;
			if (p == colon && !strpbrk(url, "\r\n")) return TRUE;
		}
		return refuse(error, "A clone must use HTTPS or explicit SSH key-agent transport");
	}
	clone = g_uri_parse(url, G_URI_FLAGS_NONE, NULL);
	base = g_uri_parse(self->base_url, G_URI_FLAGS_NONE, NULL);
	if (clone == NULL || base == NULL || venture_string_is_empty(g_uri_get_host(clone)) ||
		venture_string_is_empty(g_uri_get_host(base)) || g_uri_get_query(clone) || g_uri_get_fragment(clone)) return refuse(error, "Invalid clone address");
	scheme = g_uri_get_scheme(clone);
	if (g_strcmp0(scheme, "ssh") == 0 && !venture_string_is_empty(g_uri_get_host(clone)) && (g_uri_get_userinfo(clone) == NULL || strchr(g_uri_get_userinfo(clone), ':') == NULL)) return TRUE;
	if (g_strcmp0(scheme, "https") != 0 || g_uri_get_userinfo(clone) != NULL ||
		g_ascii_strcasecmp(g_uri_get_host(clone), g_uri_get_host(base)) != 0 ||
		(g_uri_get_port(clone) == -1 ? 443 : g_uri_get_port(clone)) != (g_uri_get_port(base) == -1 ? 443 : g_uri_get_port(base)))
		return refuse(error, "HTTPS clone authority must match the configured forge; other hosts require SSH key-agent transport");
	return TRUE;
}

JsonNode *venture_forge_settings_schema(void)
{
	return json_from_string("{\"type\":\"object\",\"additionalProperties\":false,\"required\":[\"token\",\"webhook_secret\"],\"properties\":{\"token\":{\"type\":\"string\",\"title\":\"Access token\",\"x-sensitive\":true},\"webhook_secret\":{\"type\":\"string\",\"title\":\"Webhook secret\",\"x-sensitive\":true}}}", NULL);
}
static gboolean manage_forge(VentureDatabase *database, GError **error)
{
	const VentureAuthPrincipal *principal = venture_access_policy_get_actor(venture_database_get_access_policy(database));
	/* A forge selects network origins and host execution. Organization ownership
	 * never turns this into a tenant-controlled host capability. */
	if (principal != NULL && !venture_access_policy_is_administrator(principal))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED, "Platform administrator required for forge configuration");
		return FALSE;
	}
	return TRUE;
}
VentureIntegrationConnection *venture_forge_settings_configure(VentureDatabase *database, gint64 forge_id,
	JsonNode *settings, gint64 expected_version, gint64 expected_connection, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) forge = NULL, fresh = NULL;
	g_autoptr(VentureForgejoClient) client = NULL;
	g_autoptr(VentureIntegrationConnection) old = NULL;
	g_autoptr(JsonNode) stored = NULL, old_settings = NULL;
	g_autofree gchar *base = NULL, *provider = NULL, *login = NULL;
	JsonObject *object;
	const gchar *token, *secret;
	GList *members, *iter;
	VentureForgeKind kind;
	if (!manage_forge(database, error)) return NULL;
	if (expected_version < 0 || expected_connection < 0 || ((expected_version == 0) != (expected_connection == 0)))
	{ refuse(error, "Select an exact connection and version"); return NULL; }
	forge = load_forge(database, forge_id, error);
	if (forge == NULL) return NULL;
	if (settings == NULL || !JSON_NODE_HOLDS_OBJECT(settings))
	{ refuse(error, "Forge settings must be an object"); return NULL; }
	object = json_node_get_object(settings);
	members = json_object_get_members(object);
	for (iter = members; iter; iter = iter->next)
	{
		JsonNode *node = json_object_get_member(object, iter->data);
		if ((strcmp(iter->data, "token") && strcmp(iter->data, "webhook_secret")) ||
			!JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_STRING)
		{ g_list_free(members); refuse(error, "Unknown or invalid forge credential field"); return NULL; }
	}
	g_list_free(members);
	token = venture_json_object_get_string(object, "token", NULL);
	secret = venture_json_object_get_string(object, "webhook_secret", NULL);
	if (venture_string_is_empty(token) || strlen(token) > 4096 || strpbrk(token, "\r\n") ||
		venture_string_is_empty(secret) || strlen(secret) < 32 || strlen(secret) > 4096 || strpbrk(secret, "\r\n"))
	{ refuse(error, "A bounded access token and at least 32-byte webhook secret are required"); return NULL; }
	g_object_get(forge, "base-url", &base, "kind", &kind, NULL);
	if (kind != VENTURE_FORGE_KIND_FORGEJO && kind != VENTURE_FORGE_KIND_GITEA)
	{ refuse(error, "Only Forgejo and Gitea are supported"); return NULL; }
	{
		g_autoptr(GUri) uri = g_uri_parse(base, G_URI_FLAGS_NONE, NULL);
		gboolean loopback = uri && (!g_strcmp0(g_uri_get_host(uri), "127.0.0.1") || !g_strcmp0(g_uri_get_host(uri), "::1"));
		if (!uri || g_uri_get_userinfo(uri) || g_uri_get_query(uri) || g_uri_get_fragment(uri) ||
			(g_strcmp0(g_uri_get_scheme(uri), "https") && !(loopback && !g_strcmp0(g_uri_get_scheme(uri), "http"))))
		{ refuse(error, "Forge credentials require HTTPS (HTTP only on loopback fixtures)"); return NULL; }
	}
	provider = provider_key(forge);
	if (expected_version > 0)
	{
		old = venture_forge_settings_find(database, forge_id, error);
		if (!old) return NULL;
		if (venture_entity_get_id(VENTURE_ENTITY(old)) != expected_connection)
		{ refuse(error, "Forge binding was replaced; reload settings"); return NULL; }
		old_settings = venture_integration_service_resolve_version(venture_integration_service_get(database),
			venture_entity_get_organization_id(forge), venture_entity_get_id(VENTURE_ENTITY(old)), expected_version, FALSE, error);
		if (!old_settings) return NULL;
		if (g_strcmp0(base, venture_json_object_get_string(json_node_get_object(old_settings), "base_url", NULL)))
		{ refuse(error, "Disconnect before changing the forge origin"); return NULL; }
	}
	client = venture_forgejo_client_new(base, token, 30, error);
	if (!client) return NULL;
	login = venture_forge_client_whoami(VENTURE_FORGE_CLIENT(client), error);
	if (venture_string_is_empty(login)) { if (error == NULL || *error == NULL) refuse(error, "Forge did not identify an account"); return NULL; }
	{
		const gchar *p;
		if (strlen(login) > 128 || !g_strcmp0(login, token) || !g_strcmp0(login, secret))
		{ refuse(error, "Forge returned an invalid account identity"); return NULL; }
		for (p = login; *p; p++) if (!g_ascii_isalnum(*p) && !strchr("_.-", *p))
		{ refuse(error, "Forge returned an invalid account identity"); return NULL; }
	}
	fresh = load_forge(database, forge_id, error);
	if (!fresh) return NULL;
	if (venture_entity_get_version(fresh) != venture_entity_get_version(forge))
	{ refuse(error, "Forge changed while verifying credentials"); return NULL; }
	stored = json_node_new(JSON_NODE_OBJECT); json_node_take_object(stored, json_object_new());
	json_object_set_string_member(json_node_get_object(stored), "token", token);
	json_object_set_string_member(json_node_get_object(stored), "webhook_secret", secret);
	json_object_set_string_member(json_node_get_object(stored), "base_url", base);
	json_object_set_int_member(json_node_get_object(stored), "kind", kind);
	return venture_integration_service_configure(venture_integration_service_get(database),
		venture_entity_get_organization_id(forge), provider, login, "live", stored, expected_version, actor, error);
}
gboolean venture_forge_settings_disconnect(VentureDatabase *database, gint64 forge_id, gint64 expected_version, gint64 expected_connection,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureIntegrationConnection) binding = NULL;
	if (!manage_forge(database, error)) return FALSE;
	binding = venture_forge_settings_find(database, forge_id, error);
	if (!binding) return FALSE;
	if (venture_entity_get_id(VENTURE_ENTITY(binding)) != expected_connection) return refuse(error, "Forge binding was replaced; reload settings");
	return venture_integration_service_disable(venture_integration_service_get(database),
		venture_entity_get_organization_id(VENTURE_ENTITY(binding)), venture_entity_get_id(VENTURE_ENTITY(binding)), expected_version, actor, error);
}
gboolean venture_forge_settings_import_legacy(VentureDatabase *database, gint64 forge_id, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) forge = NULL;
	g_autoptr(VentureIntegrationConnection) binding = NULL;
	g_autoptr(JsonNode) settings = NULL;
	g_autofree gchar *token = NULL, *secret = NULL;
	gboolean ok = FALSE;
	if (!manage_forge(database, error)) return FALSE;
	if (venture_database_has_transaction(database)) return refuse(error, "Legacy import requires an independent transaction");
	if (!venture_database_begin(database, error)) return FALSE;
	forge = load_forge(database, forge_id, error);
	if (forge)
	{
		g_object_get(forge, "token", &token, "webhook-secret", &secret, NULL);
		settings = json_node_new(JSON_NODE_OBJECT); json_node_take_object(settings, json_object_new());
		json_object_set_string_member(json_node_get_object(settings), "token", token ? token : "");
		json_object_set_string_member(json_node_get_object(settings), "webhook_secret", secret ? secret : "");
		binding = venture_forge_settings_configure(database, forge_id, settings, 0, 0, actor, error);
		if (binding)
		{
			g_object_set(forge, "token", NULL, "webhook-secret", NULL, NULL);
			g_object_set_data(G_OBJECT(database), "venture-forge-import-permit", forge);
			ok = venture_database_save(database, forge, actor, error);
			g_object_set_data(G_OBJECT(database), "venture-forge-import-permit", NULL);
		}
	}
	if (!ok) { venture_database_rollback(database); return FALSE; }
	return venture_database_commit(database, error);
}
VentureForgeClient *venture_forge_client_for_database(VentureDatabase *database, gint64 forge_id, gint timeout_seconds, GError **error)
{
	g_autoptr(VentureForgeCredentials) lease = venture_forge_credentials_acquire(database, forge_id, error);
	VentureForgejoClient *client;
	if (!lease) return NULL;
	client = venture_forgejo_client_new(lease->base_url, lease->token, timeout_seconds, error);
	if (!client) return NULL;
	g_object_set_data_full(G_OBJECT(client), "venture-forge-credentials", g_steal_pointer(&lease), g_object_unref);
	return VENTURE_FORGE_CLIENT(client);
}
VentureForgeCredentials *venture_forge_client_get_credentials(VentureForgeClient *client)
{ return g_object_get_data(G_OBJECT(client), "venture-forge-credentials"); }
gboolean venture_forge_client_check_credentials(VentureForgeClient *client, GError **error)
{
	VentureForgeCredentials *lease = venture_forge_client_get_credentials(client);
	return lease == NULL || venture_forge_credentials_check(lease, error);
}

JsonNode *venture_forge_settings_apply(VentureDatabase *database, gint64 forge_id, JsonNode *request,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureIntegrationConnection) binding = NULL, changed = NULL;
	g_autoptr(VentureForgeClient) client = NULL;
	g_autoptr(JsonNode) result = NULL;
	g_autofree gchar *account = NULL, *login = NULL;
	JsonObject *object;
	const gchar *operation;
	gint64 connection, version;
	GList *members, *iter;
	if (!manage_forge(database, error)) return NULL;
	if (!request || !JSON_NODE_HOLDS_OBJECT(request)) { refuse(error, "Settings request must be an object"); return NULL; }
	object = json_node_get_object(request);
	members = json_object_get_members(object);
	for (iter = members; iter; iter = iter->next)
	{
		const gchar *name = iter->data;
		JsonNode *node = json_object_get_member(object, name);
		gboolean valid = !strcmp(name, "settings") ? JSON_NODE_HOLDS_OBJECT(node) :
			!strcmp(name, "operation") ? JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING :
			(!strcmp(name, "version") || !strcmp(name, "connection_id")) && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_INT64;
		if (!valid) { g_list_free(members); refuse(error, "Unknown or incorrectly typed settings parameter"); return NULL; }
	}
	g_list_free(members);
	operation = venture_json_object_get_string(object, "operation", "");
	connection = venture_json_object_get_int(object, "connection_id", -1);
	version = venture_json_object_get_int(object, "version", -1);
	if (connection < 0 || version < 0) { refuse(error, "An exact connection and version are required"); return NULL; }
	binding = venture_forge_settings_find(database, forge_id, NULL);
	if ((binding && (connection != venture_entity_get_id(VENTURE_ENTITY(binding)) || version != venture_entity_get_version(VENTURE_ENTITY(binding)))) ||
		(!binding && (connection != 0 || version != 0)))
	{ refuse(error, "Binding changed; reload settings"); return NULL; }
	if (!strcmp(operation, "configure"))
	{
		changed = venture_forge_settings_configure(database, forge_id, json_object_get_member(object, "settings"), version, connection, actor, error);
		if (!changed) return NULL;
		g_set_object(&binding, changed);
	}
	else if (!strcmp(operation, "disconnect"))
	{
		if (!venture_forge_settings_disconnect(database, forge_id, version, connection, actor, error)) return NULL;
		g_clear_object(&binding);
	}
	else if (!strcmp(operation, "import"))
	{
		if (connection != 0 || version != 0) { refuse(error, "Disconnect before importing legacy credentials"); return NULL; }
		if (!venture_forge_settings_import_legacy(database, forge_id, actor, error)) return NULL;
		binding = venture_forge_settings_find(database, forge_id, error);
		if (!binding) return NULL;
	}
	else if (!strcmp(operation, "test"))
	{
		client = venture_forge_client_for_database(database, forge_id, 30, error);
		if (!client) return NULL;
		login = venture_forge_client_whoami(client, error);
		if (!login || !venture_forge_credentials_revalidate(venture_forge_client_get_credentials(client), database, error)) return NULL;
		g_object_get(binding, "account-id", &account, NULL);
		if (g_strcmp0(account, login)) { refuse(error, "The credential now identifies another account; disconnect and configure explicitly"); return NULL; }
	}
	else { refuse(error, "Choose configure, test, disconnect or import"); return NULL; }
	result = json_node_new(JSON_NODE_OBJECT); json_node_take_object(result, json_object_new());
	object = json_node_get_object(result);
	json_object_set_int_member(object, "forge_id", forge_id);
	json_object_set_boolean_member(object, "connected", binding != NULL);
	json_object_set_int_member(object, "connection_id", binding ? venture_entity_get_id(VENTURE_ENTITY(binding)) : 0);
	json_object_set_int_member(object, "version", binding ? venture_entity_get_version(VENTURE_ENTITY(binding)) : 0);
	return g_steal_pointer(&result);
}

/* Legacy values remain readable only to the explicit import path. Generic
 * writers may neither replace them nor accidentally erase import evidence. */
gboolean venture_forge_check_write(VentureDatabase *database, VentureEntity *entity, gboolean removal, GError **error)
{
	g_autoptr(VentureEntity) old = NULL;
	g_autoptr(VentureIntegrationConnection) binding = NULL;
	g_autofree gchar *token = NULL, *secret = NULL, *old_token = NULL, *old_secret = NULL;
	g_autofree gchar *base = NULL, *old_base = NULL;
	VentureForgeKind kind, old_kind;
	gboolean importing;
	if (VENTURE_IS_FORGE_RUN(entity))
	{
		gint64 connection = 0, version = 0, previous_connection = 0, previous_version = 0;
		if (removal) return refuse(error, "Coding-run evidence is retained");
		g_object_get(entity, "connection-id", &connection, "credential-version", &version, NULL);
		if (venture_entity_get_id(entity) > 0)
		{
			old = venture_database_get(database, VENTURE_TYPE_FORGE_RUN, venture_entity_get_id(entity), error);
			if (!old) return FALSE;
			g_object_get(old, "connection-id", &previous_connection, "credential-version", &previous_version, NULL);
			if (connection != previous_connection || version != previous_version || venture_entity_get_organization_id(entity) != venture_entity_get_organization_id(old))
				return refuse(error, "Retained run credential identity cannot change");
		}
		else if ((connection == 0) != (version == 0) || connection < 0 || version < 0)
			return refuse(error, "Run credential evidence must name both connection and version, or remain unknown");
		return TRUE;
	}
	if (removal)
	{
		GPtrArray *entries = g_object_get_data(G_OBJECT(database), "venture-forge-credentials");
		/* Purge has no entity-deleted signal. Revoke before every removal path,
		 * conservatively including a refused removal or later rollback. */
		if (entries && (VENTURE_IS_FORGE(entity) || VENTURE_IS_ORGANIZATION(entity))) credentials_changed(database, entity, entries);
		return TRUE;
	}
	if (!VENTURE_IS_FORGE(entity)) return TRUE;
	importing = g_object_get_data(G_OBJECT(database), "venture-forge-import-permit") == entity;
	if (importing) g_object_set_data(G_OBJECT(database), "venture-forge-import-permit", NULL);
	if (venture_entity_get_id(entity) > 0)
	{
		old = venture_database_get(database, VENTURE_TYPE_FORGE, venture_entity_get_id(entity), error);
		if (!old) return FALSE;
		g_object_get(old, "token", &old_token, "webhook-secret", &old_secret, "base-url", &old_base, "kind", &old_kind, NULL);
	}
	g_object_get(entity, "token", &token, "webhook-secret", &secret, "base-url", &base, "kind", &kind, NULL);
	if ((!importing && (g_strcmp0(token, old_token) || g_strcmp0(secret, old_secret))) ||
		(importing && (!venture_string_is_empty(token) || !venture_string_is_empty(secret))))
		return refuse(error, "Forge secrets are write-only encrypted settings; explicit legacy import clears old plaintext");
	if (old && venture_entity_get_organization_id(old) > 0 && venture_entity_get_organization_id(old) != venture_entity_get_organization_id(entity))
		return refuse(error, "A persisted forge cannot move organizations");
	if (old && venture_entity_get_organization_id(old) > 0)
		binding = venture_forge_settings_find(database, venture_entity_get_id(old), NULL);
	if (binding && (g_strcmp0(base, old_base) || kind != old_kind))
		return refuse(error, "Disconnect before changing the forge origin or kind");
	return TRUE;
}

gint64 venture_forge_credentials_get_connection_id(VentureForgeCredentials *self) { return self->connection_id; }
gint64 venture_forge_credentials_get_connection_version(VentureForgeCredentials *self) { return self->connection_version; }
