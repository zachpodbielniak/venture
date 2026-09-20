/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/crypto.h>
#include <string.h>

#define SETTINGS_LIMIT 65536

struct _VentureIntegrationService
{
	GObject parent_instance;
	VentureDatabase *database;
	GBytes *key;
	VentureEntity *permit;
};
G_DEFINE_FINAL_TYPE(VentureIntegrationService, venture_integration_service, G_TYPE_OBJECT)

static gboolean
refuse(GError **error, VentureError code, const gchar *message)
{
	g_set_error_literal(error, VENTURE_ERROR, code, message);
	return FALSE;
}

static void
venture_integration_service_finalize(GObject *object)
{
	VentureIntegrationService *self = VENTURE_INTEGRATION_SERVICE(object);
	g_clear_pointer(&self->key, g_bytes_unref);
	if (self->database)
		g_object_remove_weak_pointer(G_OBJECT(self->database), (gpointer *)&self->database);
	G_OBJECT_CLASS(venture_integration_service_parent_class)->finalize(object);
}

static void
venture_integration_service_class_init(VentureIntegrationServiceClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = venture_integration_service_finalize;
}

static void
venture_integration_service_init(VentureIntegrationService *self)
{
	(void)self;
}

static void
clear_key(gpointer data)
{
	OPENSSL_cleanse(data, 32);
	g_free(data);
}

VentureIntegrationService *
venture_integration_service_get(VentureDatabase *database)
{
	VentureIntegrationService *self;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	self = g_object_get_data(G_OBJECT(database), "venture-integration-service");
	if (!self)
	{
		self = g_object_new(VENTURE_TYPE_INTEGRATION_SERVICE, NULL);
		self->database = database;
		g_object_add_weak_pointer(G_OBJECT(database), (gpointer *)&self->database);
		g_object_set_data_full(G_OBJECT(database), "venture-integration-service", self, g_object_unref);
	}
	return self;
}

gboolean
venture_integration_service_set_key(VentureIntegrationService *self, GBytes *key, GError **error)
{
	g_return_val_if_fail(VENTURE_IS_INTEGRATION_SERVICE(self), FALSE);
	if (!key || g_bytes_get_size(key) != 32)
		return refuse(error, VENTURE_ERROR_CONFIG, "Integration encryption needs exactly 32 random key bytes");
	if (self->key && !g_bytes_equal(self->key, key))
		return refuse(error, VENTURE_ERROR_CONFLICT, "Changing a live integration encryption key requires re-encryption");
	if (!self->key)
	{
		gpointer owned = g_memdup2(g_bytes_get_data(key, NULL), 32);
		self->key = g_bytes_new_with_free_func(owned, 32, clear_key, owned);
	}
	return TRUE;
}

static gboolean
ensure_key(VentureIntegrationService *self, GError **error)
{
	const gchar *encoded;
	guchar *decoded;
	gsize length = 0;
	g_autoptr(GBytes) key = NULL;
	g_autofree gchar *canonical = NULL;
	if (self->key) return TRUE;
	encoded = g_getenv("VENTURE_INTEGRATION_KEY");
	if (!encoded || strlen(encoded) != 44)
		return refuse(error, VENTURE_ERROR_CONFIG, "Set VENTURE_INTEGRATION_KEY to base64-encoded 32 random bytes");
	decoded = g_base64_decode(encoded, &length);
	canonical = g_base64_encode(decoded, length);
	if (length != 32 || g_strcmp0(canonical, encoded))
	{
		OPENSSL_cleanse(decoded, length);
		g_free(decoded);
		return refuse(error, VENTURE_ERROR_CONFIG, "VENTURE_INTEGRATION_KEY is not a canonical 32-byte base64 key");
	}
	key = g_bytes_new_with_free_func(decoded, length, clear_key, decoded);
	return venture_integration_service_set_key(self, key, error);
}

static gboolean
manage(VentureIntegrationService *self, gint64 organization_id, GError **error)
{
	static const gint roles[] = { VENTURE_ORGANIZATION_ROLE_OWNER, VENTURE_ORGANIZATION_ROLE_ADMIN };
	VentureAccessPolicy *policy = venture_database_get_access_policy(self->database);
	const VentureAuthPrincipal *principal = venture_access_policy_get_actor(policy);
	g_autoptr(VentureEntity) organization = NULL;
	if (organization_id <= 0)
		return refuse(error, VENTURE_ERROR_VALIDATION, "An integration needs an explicit organization");
	if (principal && !venture_access_policy_has_organization_role(policy, principal,
		organization_id, roles, G_N_ELEMENTS(roles)))
		return refuse(error, VENTURE_ERROR_PERMISSION_DENIED, "Organization integration administration is required");
	organization = venture_database_get(self->database, VENTURE_TYPE_ORGANIZATION, organization_id, error);
	if (!organization) return FALSE;
	if (venture_entity_is_deleted(organization))
		return refuse(error, VENTURE_ERROR_NOT_FOUND, "Organization is unavailable");
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "integration_connection") == G_TYPE_INVALID)
		return refuse(error, VENTURE_ERROR_PERMISSION_DENIED, "Integrations module is disabled");
	return TRUE;
}

static gboolean
identifier(const gchar *text)
{
	const gchar *p;
	if (!text || !*text || strlen(text) > 128) return FALSE;
	for (p = text; *p; p++)
		if (!g_ascii_isalnum(*p) && *p != '_' && *p != '-' && *p != '.') return FALSE;
	return TRUE;
}

/* Bind the envelope to immutable business identity, not merely to a key.
 * Copying ciphertext between rows/organizations must fail authentication. */
static gchar *
authenticated_identity(VentureEntity *entity)
{
	g_autofree gchar *provider = NULL, *account = NULL, *environment = NULL, *uuid = NULL;
	g_object_get(entity, "provider", &provider, "account-id", &account,
		"environment", &environment, NULL);
	uuid = g_strdup(venture_entity_get_uuid(entity));
	return g_strdup_printf("venture.integration.v1\n%" G_GINT64_FORMAT "\n%s\n%s\n%s\n%s",
		venture_entity_get_organization_id(entity), uuid, provider, account, environment);
}

static gchar *
seal(VentureIntegrationService *self, VentureEntity *entity, JsonNode *settings, GError **error)
{
	g_autofree gchar *plain = NULL, *aad = NULL, *encoded = NULL;
	g_autofree guchar *envelope = NULL;
	EVP_CIPHER_CTX *cipher;
	gsize length;
	gint written = 0, tail = 0, ignored = 0;
	gboolean ok;
	if (!ensure_key(self, error)) return NULL;
	if (!settings || !JSON_NODE_HOLDS_OBJECT(settings))
	{ refuse(error, VENTURE_ERROR_VALIDATION, "Integration settings must be a JSON object"); return NULL; }
	plain = venture_json_to_string(settings, FALSE);
	length = strlen(plain);
	if (length > SETTINGS_LIMIT)
	{ OPENSSL_cleanse(plain, length); refuse(error, VENTURE_ERROR_VALIDATION, "Integration settings exceed 64 KiB"); return NULL; }
	aad = authenticated_identity(entity);
	envelope = g_malloc(length + 28);
	cipher = EVP_CIPHER_CTX_new();
	ok = cipher && RAND_bytes(envelope, 12) == 1 &&
		EVP_EncryptInit_ex(cipher, EVP_aes_256_gcm(), NULL, g_bytes_get_data(self->key, NULL), envelope) == 1 &&
		EVP_EncryptUpdate(cipher, NULL, &ignored, (const guchar *)aad, (gint)strlen(aad)) == 1 &&
		EVP_EncryptUpdate(cipher, envelope + 12, &written, (const guchar *)plain, (gint)length) == 1 &&
		EVP_EncryptFinal_ex(cipher, envelope + 12 + written, &tail) == 1 &&
		EVP_CIPHER_CTX_ctrl(cipher, EVP_CTRL_GCM_GET_TAG, 16, envelope + 12 + written + tail) == 1;
	EVP_CIPHER_CTX_free(cipher);
	OPENSSL_cleanse(plain, length);
	if (!ok) { refuse(error, VENTURE_ERROR_FAILED, "Integration settings could not be encrypted"); return NULL; }
	encoded = g_base64_encode(envelope, (gsize)(written + tail + 28));
	return g_strconcat("v1:", encoded, NULL);
}

static JsonNode *
unseal(VentureIntegrationService *self, VentureEntity *entity, GError **error)
{
	g_autofree gchar *stored = NULL, *aad = NULL;
	g_autofree guchar *envelope = NULL, *plain = NULL;
	g_autoptr(JsonParser) parser = json_parser_new();
	EVP_CIPHER_CTX *cipher;
	gsize length = 0;
	gint written = 0, tail = 0, ignored = 0, encrypted;
	gboolean ok;
	JsonNode *result = NULL;
	if (!ensure_key(self, error)) return NULL;
	g_object_get(entity, "sealed-settings", &stored, NULL);
	if (!stored || !g_str_has_prefix(stored, "v1:") || strlen(stored) > 4 * (SETTINGS_LIMIT + 28) / 3 + 8)
	{ refuse(error, VENTURE_ERROR_CONFIG, "Integration credential envelope is invalid"); return NULL; }
	envelope = g_base64_decode(stored + 3, &length);
	if (length < 28 || length > SETTINGS_LIMIT + 28)
	{ refuse(error, VENTURE_ERROR_CONFIG, "Integration credential envelope is invalid"); return NULL; }
	encrypted = (gint)length - 28;
	plain = g_malloc0((gsize)encrypted + 1);
	aad = authenticated_identity(entity);
	cipher = EVP_CIPHER_CTX_new();
	ok = cipher && EVP_DecryptInit_ex(cipher, EVP_aes_256_gcm(), NULL, g_bytes_get_data(self->key, NULL), envelope) == 1 &&
		EVP_DecryptUpdate(cipher, NULL, &ignored, (const guchar *)aad, (gint)strlen(aad)) == 1 &&
		EVP_DecryptUpdate(cipher, plain, &written, envelope + 12, encrypted) == 1 &&
		EVP_CIPHER_CTX_ctrl(cipher, EVP_CTRL_GCM_SET_TAG, 16, envelope + 12 + encrypted) == 1 &&
		EVP_DecryptFinal_ex(cipher, plain + written, &tail) == 1;
	EVP_CIPHER_CTX_free(cipher);
	/* Parser diagnostics may include the credential value, so do not expose them. */
	if (ok && json_parser_load_from_data(parser, (const gchar *)plain, written + tail, NULL) &&
		JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser)))
		result = json_node_copy(json_parser_get_root(parser));
	OPENSSL_cleanse(plain, (gsize)encrypted + 1);
	if (!result) refuse(error, VENTURE_ERROR_CONFIG, "Integration credentials failed authentication with the configured key");
	return result;
}

static GPtrArray *
active_rows(VentureIntegrationService *self, gint64 organization_id, const gchar *provider, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_INTEGRATION_CONNECTION);
	venture_query_set_organization(query, organization_id);
	venture_query_set_limit(query, 2);
	if (!venture_query_add_filter_string(query, "provider", VENTURE_FILTER_OP_EQ, provider, error) ||
		!venture_query_add_filter_int(query, "enabled", VENTURE_FILTER_OP_EQ, 1, error)) return NULL;
	return venture_database_find(self->database, query, error);
}

static gboolean
save_owned(VentureIntegrationService *self, VentureEntity *entity, const VentureActor *actor, GError **error)
{
	gboolean ok;
	self->permit = entity;
	ok = venture_database_save(self->database, entity, actor, error);
	self->permit = NULL;
	return ok;
}

gboolean
venture_integration_check_write(VentureDatabase *database, VentureEntity *entity, gboolean removal, GError **error)
{
	VentureIntegrationService *self;
	if (!VENTURE_IS_INTEGRATION_CONNECTION(entity)) return TRUE;
	self = venture_integration_service_get(database);
	if (removal || self->permit != entity)
		return refuse(error, VENTURE_ERROR_PERMISSION_DENIED, "Integration bindings are changed only through organization settings; disconnect instead of deleting");
	/* Consume authorization before database signals can invoke another write. */
	self->permit = NULL;
	return TRUE;
}

VentureIntegrationConnection *
venture_integration_service_find(VentureIntegrationService *self, gint64 organization_id,
	const gchar *provider, GError **error)
{
	g_autoptr(GPtrArray) rows = NULL;
	g_return_val_if_fail(VENTURE_IS_INTEGRATION_SERVICE(self), NULL);
	if (!self->database) { refuse(error, VENTURE_ERROR_FAILED, "Integration repository is no longer available"); return NULL; }
	if (organization_id <= 0 || !identifier(provider))
	{ refuse(error, VENTURE_ERROR_VALIDATION, "Explicit organization and provider are required"); return NULL; }
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "integration_connection") == G_TYPE_INVALID)
	{ refuse(error, VENTURE_ERROR_PERMISSION_DENIED, "Integrations module is disabled"); return NULL; }
	rows = active_rows(self, organization_id, provider, error);
	if (!rows) return NULL;
	if (rows->len != 1)
	{ refuse(error, VENTURE_ERROR_CONFIG, "Organization integration is unconfigured or ambiguous"); return NULL; }
	return g_object_ref(g_ptr_array_index(rows, 0));
}

VentureIntegrationConnection *
venture_integration_service_configure(VentureIntegrationService *self, gint64 organization_id,
	const gchar *provider, const gchar *account_id, const gchar *environment, JsonNode *settings,
	gint64 expected_version, const VentureActor *actor, GError **error)
{
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(VentureIntegrationConnection) connection = NULL;
	g_autofree gchar *sealed = NULL;
	gint64 revision = 0;
	g_return_val_if_fail(VENTURE_IS_INTEGRATION_SERVICE(self), NULL);
	if (!self->database) { refuse(error, VENTURE_ERROR_FAILED, "Integration repository is no longer available"); return NULL; }
	if (!manage(self, organization_id, error)) return NULL;
	if (!identifier(provider) || !identifier(account_id) ||
		(g_strcmp0(environment, "test") && g_strcmp0(environment, "live")))
	{ refuse(error, VENTURE_ERROR_VALIDATION, "Provider/account identifiers and test/live environment are required"); return NULL; }
	if (!venture_database_begin(self->database, error)) return NULL;
	rows = active_rows(self, organization_id, provider, error);
	if (!rows) goto fail;
	if (rows->len > 1) { refuse(error, VENTURE_ERROR_CONFLICT, "Organization has ambiguous provider bindings"); goto fail; }
	if (rows->len)
	{
		g_autofree gchar *old_account = NULL, *old_environment = NULL;
		connection = g_object_ref(g_ptr_array_index(rows, 0));
		g_object_get(connection, "account-id", &old_account, "environment", &old_environment,
			"credential-revision", &revision, NULL);
		if (g_strcmp0(account_id, old_account) || g_strcmp0(environment, old_environment))
		{ refuse(error, VENTURE_ERROR_CONFLICT, "Disconnect the existing account before replacing its identity or environment"); goto fail; }
		if (venture_entity_get_version(VENTURE_ENTITY(connection)) != expected_version)
		{ refuse(error, VENTURE_ERROR_CONFLICT, "Integration configuration changed; reload settings"); goto fail; }
	}
	else
	{
		if (expected_version != 0) { refuse(error, VENTURE_ERROR_CONFLICT, "Integration configuration changed; reload settings"); goto fail; }
		connection = venture_integration_connection_new();
		venture_entity_set_organization_id(VENTURE_ENTITY(connection), organization_id);
		g_object_set(connection, "provider", provider, "account-id", account_id, "environment", environment, NULL);
	}
	if (revision == G_MAXINT64) { refuse(error, VENTURE_ERROR_CONFLICT, "Integration revision limit reached"); goto fail; }
	sealed = seal(self, VENTURE_ENTITY(connection), settings, error);
	if (!sealed) goto fail;
	/* Sensitive fields do not enter the audit diff. The public revision both
	 * audits rotation and prevents a credential-only save short-circuit. */
	g_object_set(connection, "sealed-settings", sealed, "enabled", TRUE, "credential-revision", revision + 1, NULL);
	if (!save_owned(self, VENTURE_ENTITY(connection), actor, error)) goto fail;
	if (!venture_database_commit(self->database, error)) return NULL;
	return g_steal_pointer(&connection);
fail:
	venture_database_rollback(self->database);
	return NULL;
}

JsonNode *
venture_integration_service_resolve(VentureIntegrationService *self, gint64 organization_id,
	gint64 connection_id, gboolean allow_disabled, GError **error)
{
	g_autoptr(VentureEntity) entity = NULL;
	gboolean enabled = FALSE;
	g_return_val_if_fail(VENTURE_IS_INTEGRATION_SERVICE(self), NULL);
	if (!self->database) { refuse(error, VENTURE_ERROR_FAILED, "Integration repository is no longer available"); return NULL; }
	if (organization_id <= 0 || connection_id <= 0)
	{ refuse(error, VENTURE_ERROR_VALIDATION, "Explicit organization and connection are required"); return NULL; }
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "integration_connection") == G_TYPE_INVALID)
	{ refuse(error, VENTURE_ERROR_PERMISSION_DENIED, "Integrations module is disabled"); return NULL; }
	entity = venture_database_get(self->database, VENTURE_TYPE_INTEGRATION_CONNECTION, connection_id, error);
	if (!entity) return NULL;
	if (venture_entity_get_organization_id(entity) != organization_id || venture_entity_is_deleted(entity))
	{ refuse(error, VENTURE_ERROR_NOT_FOUND, "Integration connection is unavailable"); return NULL; }
	g_object_get(entity, "enabled", &enabled, NULL);
	if (!enabled && !allow_disabled)
	{ refuse(error, VENTURE_ERROR_CONFIG, "Organization integration is disabled"); return NULL; }
	return unseal(self, entity, error);
}

gboolean
venture_integration_service_disable(VentureIntegrationService *self, gint64 organization_id,
	gint64 connection_id, gint64 expected_version, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) entity = NULL;
	g_return_val_if_fail(VENTURE_IS_INTEGRATION_SERVICE(self), FALSE);
	if (!self->database) return refuse(error, VENTURE_ERROR_FAILED, "Integration repository is no longer available");
	if (!manage(self, organization_id, error)) return FALSE;
	entity = venture_database_get(self->database, VENTURE_TYPE_INTEGRATION_CONNECTION, connection_id, error);
	if (!entity) return FALSE;
	if (venture_entity_get_organization_id(entity) != organization_id || venture_entity_is_deleted(entity))
		return refuse(error, VENTURE_ERROR_NOT_FOUND, "Integration connection is unavailable");
	if (venture_entity_get_version(entity) != expected_version)
		return refuse(error, VENTURE_ERROR_CONFLICT, "Integration configuration changed; reload settings");
	g_object_set(entity, "enabled", FALSE, NULL);
	return save_owned(self, entity, actor, error);
}
