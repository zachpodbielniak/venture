/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
#include <string.h>

#define AI_RESERVATION_SECONDS 90

struct _VentureAiProviderService {
	GObject parent_instance;
	VentureDatabase *database;
	VentureConfig *config;
	VentureEntity *writing;
	gboolean platform_write;
};
G_DEFINE_FINAL_TYPE(VentureAiProviderService, venture_ai_provider_service, G_TYPE_OBJECT)

typedef struct {
	const gchar *name;
	const gchar *base_url;
	AiProviderType kind;
	AiProvider *(*create)(AiConfig *config);
} ProviderSpec;
static AiProvider *openai_create(AiConfig *config) { return AI_PROVIDER(ai_openai_client_new_with_config(config)); }
static AiProvider *claude_create(AiConfig *config) { return AI_PROVIDER(ai_claude_client_new_with_config(config)); }
static AiProvider *gemini_create(AiConfig *config) { return AI_PROVIDER(ai_gemini_client_new_with_config(config)); }
static AiProvider *grok_create(AiConfig *config) { return AI_PROVIDER(ai_grok_client_new_with_config(config)); }
static const ProviderSpec providers[] = {
	{ "openai", "https://api.openai.com", AI_PROVIDER_OPENAI, openai_create },
	{ "claude", "https://api.anthropic.com", AI_PROVIDER_CLAUDE, claude_create },
	{ "gemini", "https://generativelanguage.googleapis.com", AI_PROVIDER_GEMINI, gemini_create },
	{ "grok", "https://api.x.ai", AI_PROVIDER_GROK, grok_create }
};
static gboolean deny(GError **error, VentureError code, const gchar *message)
{ g_set_error_literal(error, VENTURE_ERROR, code, message); return FALSE; }
static gint64 integer(VentureEntity *entity, const gchar *name)
{ gint64 value = 0; if (entity) g_object_get(entity, name, &value, NULL); return value; }
static gboolean enabled(VentureEntity *entity)
{ gboolean value = FALSE; if (entity && !venture_entity_is_deleted(entity)) g_object_get(entity, "enabled", &value, NULL); return value; }
static void venture_ai_provider_service_finalize(GObject *object)
{
	VentureAiProviderService *self = VENTURE_AI_PROVIDER_SERVICE(object);
	if (self->database) g_object_remove_weak_pointer(G_OBJECT(self->database), (gpointer *)&self->database);
	g_clear_object(&self->config);
	G_OBJECT_CLASS(venture_ai_provider_service_parent_class)->finalize(object);
}
static void venture_ai_provider_service_class_init(VentureAiProviderServiceClass *klass)
{ G_OBJECT_CLASS(klass)->finalize = venture_ai_provider_service_finalize; }
static void venture_ai_provider_service_init(VentureAiProviderService *self) { (void)self; }
VentureAiProviderService *venture_ai_provider_service_get(VentureDatabase *database)
{
	VentureAiProviderService *self;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	self = g_object_get_data(G_OBJECT(database), "venture-ai-provider-service");
	if (!self) {
		self = g_object_new(VENTURE_TYPE_AI_PROVIDER_SERVICE, NULL); self->database = database;
		g_object_add_weak_pointer(G_OBJECT(database), (gpointer *)&self->database);
		g_object_set_data_full(G_OBJECT(database), "venture-ai-provider-service", self, g_object_unref);
	}
	return self;
}
void venture_ai_provider_service_set_config(VentureAiProviderService *self, VentureConfig *config)
{ g_return_if_fail(VENTURE_IS_AI_PROVIDER_SERVICE(self)); g_set_object(&self->config, config); }
static gboolean manage(VentureAiProviderService *self, gint64 org, gboolean platform, GError **error)
{
	VentureAccessPolicy *policy;
	const VentureAuthPrincipal *actor;
	const gint roles[] = { VENTURE_ORGANIZATION_ROLE_OWNER, VENTURE_ORGANIZATION_ROLE_ADMIN };
	g_autoptr(VentureEntity) organization = NULL;
	g_autoptr(VentureAccessScope) internal = NULL;
	if (!self->database || !self->config) return deny(error, VENTURE_ERROR_CONFIG, "AI provider settings are unavailable");
	policy = venture_database_get_access_policy(self->database); actor = venture_access_policy_get_actor(policy);
	if (venture_access_policy_get_organization(policy) != 0 &&
		venture_access_policy_get_organization(policy) != org)
		return deny(error, VENTURE_ERROR_PERMISSION_DENIED, "AI settings are outside this request's organization");
	if (actor && !(platform ? venture_access_policy_is_administrator(actor) :
		venture_access_policy_has_organization_role(policy, actor, org, roles, G_N_ELEMENTS(roles))))
		return deny(error, VENTURE_ERROR_PERMISSION_DENIED, "Only the authorized integration administrator may change these settings");
	if (platform && org == 0) return TRUE;
	internal = venture_access_policy_enter(policy, NULL);
	organization = venture_database_get(self->database, VENTURE_TYPE_ORGANIZATION, org, error);
	return organization && !venture_entity_is_deleted(organization);
}
static gboolean limits(gint64 monthly, gint64 concurrency, GError **error)
{
	return (monthly > 0 && monthly <= 10000000 && concurrency > 0 && concurrency <= 32) ||
		deny(error, VENTURE_ERROR_VALIDATION, "Use 1 through 10000000 monthly requests and 1 through 32 concurrent requests");
}
static const ProviderSpec *settings_spec(VentureAiProviderService *self, JsonNode *settings, GError **error)
{
	JsonObject *object;
	const gchar *name, *model, *key, *base;
	const ProviderSpec *spec = NULL;
	g_autoptr(GUri) uri = NULL;
	g_auto(GStrv) allowed = NULL;
	gboolean loopback = FALSE, approved = FALSE;
	guint i;
	if (!settings || !JSON_NODE_HOLDS_OBJECT(settings)) goto invalid;
	object = json_node_get_object(settings);
	name = venture_json_object_get_string(object, "provider", NULL);
	model = venture_json_object_get_string(object, "model", NULL);
	key = venture_json_object_get_string(object, "api_key", NULL);
	base = venture_json_object_get_string(object, "base_url", NULL);
	if (json_object_get_size(object) != 4 || !name || !model || !*model || strlen(model) > 255 ||
		!key || !*key || strlen(key) > 4096 || !base || strlen(base) > 2048 || !self->config) goto invalid;
	for (i = 0; i < G_N_ELEMENTS(providers); i++) if (!strcmp(name, providers[i].name)) spec = &providers[i];
	if (!spec) goto invalid;
	uri = g_uri_parse(base, G_URI_FLAGS_NONE, NULL);
	if (!uri || !g_uri_get_host(uri) || g_uri_get_userinfo(uri) || g_uri_get_query(uri) || g_uri_get_fragment(uri)) goto invalid;
	g_object_get(self->config, "ai-allowed-base-urls", &allowed, "ai-allow-loopback", &loopback, NULL);
	approved = !strcmp(base, spec->base_url);
	for (i = 0; allowed && allowed[i]; i++) if (!strcmp(base, allowed[i])) approved = TRUE;
	if (!approved) goto invalid;
	if (g_strcmp0(g_uri_get_scheme(uri), "https") && !(loopback && !g_strcmp0(g_uri_get_scheme(uri), "http") &&
		(!g_strcmp0(g_uri_get_host(uri), "127.0.0.1") || !g_strcmp0(g_uri_get_host(uri), "::1")))) goto invalid;
	return spec;
invalid:
	deny(error, VENTURE_ERROR_CONFIG, "Provide an approved HTTP API provider, model, nonempty API key and exact allowed base URL"); return NULL;
}
static gboolean save(VentureAiProviderService *self, VentureEntity *entity, const VentureActor *actor, GError **error)
{
	VentureEntity *previous = self->writing;
	gboolean result;
	self->writing = entity; result = venture_database_save(self->database, entity, actor, error); self->writing = previous;
	return result;
}
static VentureEntity *by_key(VentureAiProviderService *self, GType type, const gchar *field, const gchar *key, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(type);
	venture_query_add_filter_string(query, field, VENTURE_FILTER_OP_EQ, key, NULL);
	return venture_database_find_one(self->database, query, error);
}
static gboolean purpose_valid(const gchar *purpose, GError **error)
{ return (!g_strcmp0(purpose, "chat") || !g_strcmp0(purpose, "embedding") || !g_strcmp0(purpose, "coding")) || deny(error, VENTURE_ERROR_VALIDATION, "Select chat, embedding or coding purpose"); }
static VentureEntity *configuration(VentureAiProviderService *self, gint64 org, const gchar *purpose, GError **error)
{
	g_autofree gchar *key = g_strdup_printf("org-%" G_GINT64_FORMAT "-%s", org, purpose);
	return by_key(self, VENTURE_TYPE_AI_CONFIGURATION, "configuration-key", key, error);
}
static gboolean revision(VentureEntity *entity, gint64 expected, GError **error)
{
	return ((!entity && expected == 0) || (entity && !venture_entity_is_deleted(entity) &&
		venture_entity_get_version(entity) == expected)) || deny(error, VENTURE_ERROR_CONFLICT, "AI settings changed; reload and try again");
}
static VentureEntity *new_configuration(gint64 org, const gchar *purpose)
{
	g_autofree gchar *key = g_strdup_printf("org-%" G_GINT64_FORMAT "-%s", org, purpose);
	return g_object_new(VENTURE_TYPE_AI_CONFIGURATION, "organization-id", org, "title", "Organization AI", "purpose", purpose, "configuration-key", key, "mode", "disabled", NULL);
}
static VentureIntegrationConnection *connect_provider(VentureAiProviderService *self, gint64 org,
	const gchar *provider, JsonNode *settings, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureIntegrationConnection) existing = NULL;
	g_autofree gchar *identity = NULL, *key = NULL;
	JsonObject *object = json_node_get_object(settings);
	existing = venture_integration_service_find(venture_integration_service_get(self->database), org, provider, NULL);
	identity = g_strconcat(json_object_get_string_member(object, "provider"), "\n", json_object_get_string_member(object, "base_url"), NULL);
	key = g_compute_checksum_for_string(G_CHECKSUM_SHA256, identity, -1);
	return venture_integration_service_configure(venture_integration_service_get(self->database), org, provider,
		key, "live", settings, existing ? venture_entity_get_version(VENTURE_ENTITY(existing)) : 0, actor, error);
}
VentureAiConfiguration *venture_ai_provider_service_configure(VentureAiProviderService *self,
	gint64 org, const gchar *purpose, JsonNode *settings, gint64 monthly, gint64 concurrency, gint64 expected, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) row = NULL;
	g_autoptr(VentureIntegrationConnection) binding = NULL;
	g_autoptr(VentureAccessScope) internal = NULL;
	g_return_val_if_fail(VENTURE_IS_AI_PROVIDER_SERVICE(self), NULL);
	if (!purpose_valid(purpose, error) || !manage(self, org, FALSE, error) || !limits(monthly, concurrency, error) || !settings_spec(self, settings, error)) return NULL;
	internal = venture_access_policy_enter(venture_database_get_access_policy(self->database), NULL);
	if (!venture_database_begin(self->database, error)) return NULL;
	row = configuration(self, org, purpose, error);
	if ((error && *error) || !revision(row, expected, error)) goto fail;
	if (!row) row = new_configuration(org, purpose);
	{ g_autofree gchar *identity = g_strconcat("ai-", purpose, NULL);
		binding = connect_provider(self, org, identity, settings, actor, error); }
	if (!binding) goto fail;
	g_object_set(row, "mode", "organization", "connection-id", venture_entity_get_id(VENTURE_ENTITY(binding)),
		"grant-id", (gint64)0, "monthly-requests", monthly, "concurrency-limit", concurrency, NULL);
	if (!save(self, row, actor, error) || !venture_database_commit(self->database, error)) goto fail;
	return VENTURE_AI_CONFIGURATION(g_steal_pointer(&row));
fail:
	venture_database_rollback(self->database); return NULL;
}
VentureAiConfiguration *venture_ai_provider_service_select(VentureAiProviderService *self,
	gint64 org, const gchar *purpose, gint64 grant_id, gint64 expected, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) row = NULL, grant = NULL, connection = NULL, offer = NULL;
	g_autofree gchar *offer_purpose = NULL;
	g_autoptr(VentureAccessScope) internal = NULL;
	g_return_val_if_fail(VENTURE_IS_AI_PROVIDER_SERVICE(self), NULL);
	if (!purpose_valid(purpose, error) || !manage(self, org, FALSE, error)) return NULL;
	if (grant_id < 0) { deny(error, VENTURE_ERROR_VALIDATION, "Select a positive grant or zero to disable"); return NULL; }
	internal = venture_access_policy_enter(venture_database_get_access_policy(self->database), NULL);
	if (!venture_database_begin(self->database, error)) return NULL;
	row = configuration(self, org, purpose, error);
	if ((error && *error) || !revision(row, expected, error)) goto fail;
	if (!row) row = new_configuration(org, purpose);
	if (grant_id > 0) {
		grant = venture_database_get(self->database, VENTURE_TYPE_AI_GRANT, grant_id, error);
		if (!enabled(grant) || venture_entity_get_organization_id(grant) != org) {
			if (!(error && *error)) deny(error, VENTURE_ERROR_PERMISSION_DENIED, "No active platform grant for this organization");
			goto fail;
		}
		offer = venture_database_get(self->database, VENTURE_TYPE_AI_PLATFORM_OFFER, integer(grant, "offer-id"), error);
		if (offer) g_object_get(offer, "purpose", &offer_purpose, NULL);
		if (!enabled(offer) || g_strcmp0(offer_purpose, purpose)) {
			if (!(error && *error)) deny(error, VENTURE_ERROR_VALIDATION, "The grant has a different purpose or its offer is disabled");
			goto fail;
		}
	}
	if (integer(row, "connection-id") > 0) {
		connection = venture_database_get(self->database, VENTURE_TYPE_INTEGRATION_CONNECTION, integer(row, "connection-id"), error);
		if (!connection || !venture_integration_service_disable(venture_integration_service_get(self->database), org,
			venture_entity_get_id(connection), venture_entity_get_version(connection), actor, error)) goto fail;
	}
	g_object_set(row, "mode", grant_id > 0 ? "platform" : "disabled", "grant-id", grant_id,
		"connection-id", (gint64)0, NULL);
	if (!save(self, row, actor, error) || !venture_database_commit(self->database, error)) goto fail;
	return VENTURE_AI_CONFIGURATION(g_steal_pointer(&row));
fail:
	venture_database_rollback(self->database); return NULL;
}
VentureAiPlatformOffer *venture_ai_provider_service_offer(VentureAiProviderService *self,
	gint64 org, const gchar *purpose, const gchar *title, JsonNode *settings, gint64 offer_id, gint64 expected, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) row = NULL, old_connection = NULL;
	g_autoptr(VentureIntegrationConnection) connection = NULL;
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autofree gchar *provider = NULL, *old_purpose = NULL;
	gboolean previous;
	g_return_val_if_fail(VENTURE_IS_AI_PROVIDER_SERVICE(self), NULL);
	if (!purpose_valid(purpose, error) || !manage(self, org, TRUE, error) || !settings_spec(self, settings, error)) return NULL;
	if (!title || !*title || strlen(title) > 255) { deny(error, VENTURE_ERROR_VALIDATION, "A short safe service label is required"); return NULL; }
	internal = venture_access_policy_enter(venture_database_get_access_policy(self->database), NULL);
	if (!venture_database_begin(self->database, error)) return NULL;
	if (offer_id) row = venture_database_get(self->database, VENTURE_TYPE_AI_PLATFORM_OFFER, offer_id, error);
	if ((error && *error) || !revision(row, expected, error)) goto fail;
	if (!row) row = g_object_new(VENTURE_TYPE_AI_PLATFORM_OFFER, "purpose", purpose, NULL);
	g_object_get(row, "purpose", &old_purpose, NULL);
	if (g_strcmp0(old_purpose, purpose)) { deny(error, VENTURE_ERROR_CONFLICT, "An offer cannot change purpose"); goto fail; }
	if (integer(row, "billing-organization-id") > 0 && integer(row, "billing-organization-id") != org) {
		deny(error, VENTURE_ERROR_CONFLICT, "A platform offer cannot change its paying organization"); goto fail;
	}
	if (integer(row, "connection-id") > 0) {
		old_connection = venture_database_get(self->database, VENTURE_TYPE_INTEGRATION_CONNECTION, integer(row, "connection-id"), error);
		if (!old_connection) goto fail;
		g_object_get(old_connection, "provider", &provider, NULL);
	} else { g_autofree gchar *uuid = g_uuid_string_random(); provider = g_strconcat("platform-ai-", uuid, NULL); }
	previous = self->platform_write; self->platform_write = TRUE;
	connection = connect_provider(self, org, provider, settings, actor, error); self->platform_write = previous;
	if (!connection) goto fail;
	g_object_set(row, "title", title, "billing-organization-id", org, "connection-id", venture_entity_get_id(VENTURE_ENTITY(connection)), "enabled", TRUE, NULL);
	if (!save(self, row, actor, error) || !venture_database_commit(self->database, error)) goto fail;
	return VENTURE_AI_PLATFORM_OFFER(g_steal_pointer(&row));
fail:
	venture_database_rollback(self->database); return NULL;
}
VentureAiGrant *venture_ai_provider_service_grant(VentureAiProviderService *self,
	gint64 org, gint64 offer_id, gboolean active, gint64 monthly, gint64 concurrency, gint64 expected, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) row = NULL, offer = NULL;
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autofree gchar *key = g_strdup_printf("org-%" G_GINT64_FORMAT "-offer-%" G_GINT64_FORMAT, org, offer_id);
	g_return_val_if_fail(VENTURE_IS_AI_PROVIDER_SERVICE(self), NULL);
	if (!manage(self, org, TRUE, error) || !limits(monthly, concurrency, error)) return NULL;
	internal = venture_access_policy_enter(venture_database_get_access_policy(self->database), NULL);
	if (!venture_database_begin(self->database, error)) return NULL;
	offer = venture_database_get(self->database, VENTURE_TYPE_AI_PLATFORM_OFFER, offer_id, error);
	if (!offer || (active && !enabled(offer))) { if (!(error && *error)) deny(error, VENTURE_ERROR_CONFIG, "The platform offer is disabled"); goto fail; }
	row = by_key(self, VENTURE_TYPE_AI_GRANT, "grant-key", key, error);
	if ((error && *error) || !revision(row, expected, error)) goto fail;
	if (!row) row = g_object_new(VENTURE_TYPE_AI_GRANT, "organization-id", org, "grant-key", key, "title", "Platform AI grant", "offer-id", offer_id, NULL);
	g_object_set(row, "enabled", active, "monthly-requests", monthly, "concurrency-limit", concurrency, NULL);
	if (!save(self, row, actor, error) || !venture_database_commit(self->database, error)) goto fail;
	return VENTURE_AI_GRANT(g_steal_pointer(&row));
fail:
	venture_database_rollback(self->database); return NULL;
}
gboolean venture_ai_provider_service_disable_offer(VentureAiProviderService *self,
	gint64 offer_id, gint64 expected, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) offer = NULL, connection = NULL;
	g_autoptr(VentureAccessScope) internal = NULL;
	gboolean previous, ok;
	g_return_val_if_fail(VENTURE_IS_AI_PROVIDER_SERVICE(self), FALSE);
	if (!manage(self, 0, TRUE, error)) return FALSE;
	internal = venture_access_policy_enter(venture_database_get_access_policy(self->database), NULL);
	if (!venture_database_begin(self->database, error)) return FALSE;
	offer = venture_database_get(self->database, VENTURE_TYPE_AI_PLATFORM_OFFER, offer_id, error);
	if (!offer || !revision(offer, expected, error)) goto fail;
	connection = venture_database_get(self->database, VENTURE_TYPE_INTEGRATION_CONNECTION, integer(offer, "connection-id"), error);
	if (!connection) goto fail;
	previous = self->platform_write; self->platform_write = TRUE;
	ok = venture_integration_service_disable(venture_integration_service_get(self->database), integer(offer, "billing-organization-id"),
		venture_entity_get_id(connection), venture_entity_get_version(connection), actor, error); self->platform_write = previous;
	if (!ok) goto fail;
	g_object_set(offer, "enabled", FALSE, NULL);
	if (!save(self, offer, actor, error) || !venture_database_commit(self->database, error)) goto fail;
	return TRUE;
fail:
	venture_database_rollback(self->database); return FALSE;
}
gboolean venture_ai_provider_check_write(VentureDatabase *database, VentureEntity *entity, gboolean removal, GError **error)
{
	VentureAiProviderService *self = venture_ai_provider_service_get(database);
	(void)removal;
	if (VENTURE_IS_INTEGRATION_CONNECTION(entity)) {
		g_autofree gchar *provider = NULL;
		g_object_get(entity, "provider", &provider, NULL);
		if (provider && g_str_has_prefix(provider, "platform-ai-") && !self->platform_write)
			return deny(error, VENTURE_ERROR_PERMISSION_DENIED, "Platform AI credentials are managed only by the platform operator");
		return TRUE;
	}
	if (!(VENTURE_IS_AI_CONFIGURATION(entity) || VENTURE_IS_AI_PLATFORM_OFFER(entity) || VENTURE_IS_AI_GRANT(entity) ||
		VENTURE_IS_AI_USAGE_PERIOD(entity) || VENTURE_IS_AI_USAGE(entity))) return TRUE;
	return self->writing == entity || deny(error, VENTURE_ERROR_VALIDATION, "AI settings, grants and usage are written only through their service");
}

typedef struct {
	VentureEntity *configuration, *grant, *offer, *connection;
	JsonNode *settings;
	gchar *mode, *stamp, *purpose;
	gint64 organization, payer, monthly, concurrency, deadline;
} Selection;
static void selection_free(Selection *selection)
{
	if (!selection) return;
	g_clear_object(&selection->configuration); g_clear_object(&selection->grant);
	g_clear_object(&selection->offer); g_clear_object(&selection->connection);
	g_clear_pointer(&selection->settings, json_node_unref);
	g_free(selection->purpose); g_free(selection->mode); g_free(selection->stamp); g_free(selection);
}
G_DEFINE_AUTOPTR_CLEANUP_FUNC(Selection, selection_free)
static gboolean may_use(VentureAiProviderService *self, gint64 org, const VentureAuthPrincipal *principal, GError **error)
{
	g_autoptr(VentureEntity) organization = NULL;
	g_autoptr(VentureAccessScope) internal = NULL;
	VentureAccessPolicy *policy;
	gboolean active = FALSE;
	if (!self->database || !self->config || org <= 0) return deny(error, VENTURE_ERROR_CONFIG, "Select an explicit organization for AI");
	policy = venture_database_get_access_policy(self->database);
	if (venture_access_policy_get_organization(policy) != 0 &&
		venture_access_policy_get_organization(policy) != org)
		return deny(error, VENTURE_ERROR_PERMISSION_DENIED, "AI provider is outside this request's organization");
	internal = venture_access_policy_enter(policy, NULL);
	organization = venture_database_get(self->database, VENTURE_TYPE_ORGANIZATION, org, error);
	if (organization) g_object_get(organization, "active", &active, NULL);
	if (!organization || !active || venture_entity_is_deleted(organization)) {
		if (!(error && *error)) deny(error, VENTURE_ERROR_NOT_FOUND, "No such organization");
		return FALSE;
	}
	if (principal) {
		g_autoptr(VentureEntity) user = NULL, token = NULL;
		g_autoptr(GDateTime) expires = NULL, now = venture_time_now();
		VentureUserRole role = VENTURE_USER_ROLE_VIEWER;
		gboolean current = FALSE;
		if (!principal->authenticated) goto revoked;
		if (principal->user_id > 0) {
			user = venture_database_get(self->database, VENTURE_TYPE_USER, principal->user_id, NULL);
			if (user) g_object_get(user, "active", &current, "role", &role, NULL);
			if (!user || !current || venture_entity_is_deleted(user)) goto revoked;
			/* SERVICE has editor authority; other role values are ordered
			 * from most to least privileged by the public enum contract. */
			if ((role == VENTURE_USER_ROLE_SERVICE ? VENTURE_USER_ROLE_EDITOR : role) >
				(principal->role == VENTURE_USER_ROLE_SERVICE ? VENTURE_USER_ROLE_EDITOR : principal->role)) goto revoked;
		}
		if (principal->token_id > 0) {
			current = FALSE;
			token = venture_database_get(self->database, VENTURE_TYPE_API_TOKEN, principal->token_id, NULL);
			if (token) g_object_get(token, "active", &current, "role", &role, "expires-at", &expires, NULL);
			if (!token || !current || venture_entity_is_deleted(token) || integer(token, "user-id") != principal->user_id ||
				(expires && g_date_time_compare(now, expires) >= 0)) goto revoked;
			if ((role == VENTURE_USER_ROLE_SERVICE ? VENTURE_USER_ROLE_EDITOR : role) >
				(principal->role == VENTURE_USER_ROLE_SERVICE ? VENTURE_USER_ROLE_EDITOR : principal->role)) goto revoked;
		} else if (!user) goto revoked;
		return venture_access_policy_can(policy, principal, "read", organization, error);
	}
	return TRUE;
revoked:
	return deny(error, VENTURE_ERROR_PERMISSION_DENIED, "Local AI caller authority was revoked or reduced");
}
static Selection *selection_new(VentureAiProviderService *self, gint64 org, const gchar *purpose,
	const VentureAuthPrincipal *principal, GError **error)
{
	g_autoptr(Selection) selection = g_new0(Selection, 1);
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autofree gchar *provider = NULL;
	gint64 connection_id;
	gboolean available = FALSE;
	if (!purpose_valid(purpose, error) || !may_use(self, org, principal, error)) return NULL;
	g_object_get(self->config, !g_strcmp0(purpose, "embedding") ? "kb-enabled" : "ai-enabled", &available, NULL);
	if (!available || venture_entity_registry_lookup(venture_entity_registry_get_default(), "ai_configuration") == G_TYPE_INVALID) goto unavailable;
	internal = venture_access_policy_enter(venture_database_get_access_policy(self->database), NULL);
	selection->organization = org; selection->purpose = g_strdup(purpose);
	g_object_get(self->config, "ai-provider-deadline-seconds", &selection->deadline, NULL);
	if (selection->deadline < 1 || selection->deadline > 60) {
		deny(error, VENTURE_ERROR_CONFIG, "AI provider deadline must be 1 through 60 seconds"); return NULL;
	}
	selection->configuration = configuration(self, org, purpose, error);
	if (!selection->configuration || venture_entity_is_deleted(selection->configuration)) goto unavailable;
	g_object_get(selection->configuration, "mode", &selection->mode, NULL);
	if (!g_strcmp0(selection->mode, "organization")) {
		selection->payer = org;
		selection->monthly = integer(selection->configuration, "monthly-requests");
		selection->concurrency = integer(selection->configuration, "concurrency-limit");
		connection_id = integer(selection->configuration, "connection-id");
	} else if (!g_strcmp0(selection->mode, "platform")) {
		selection->grant = venture_database_get(self->database, VENTURE_TYPE_AI_GRANT, integer(selection->configuration, "grant-id"), error);
		if (!enabled(selection->grant) || venture_entity_get_organization_id(selection->grant) != org) goto unavailable;
		selection->offer = venture_database_get(self->database, VENTURE_TYPE_AI_PLATFORM_OFFER, integer(selection->grant, "offer-id"), error);
		if (!enabled(selection->offer)) goto unavailable;
		{ g_autofree gchar *offer_purpose = NULL;
			g_object_get(selection->offer, "purpose", &offer_purpose, NULL);
			if (g_strcmp0(offer_purpose, purpose)) goto unavailable; }
		selection->payer = integer(selection->offer, "billing-organization-id");
		selection->monthly = integer(selection->grant, "monthly-requests");
		selection->concurrency = integer(selection->grant, "concurrency-limit");
		connection_id = integer(selection->offer, "connection-id");
	} else goto unavailable;
	selection->connection = venture_database_get(self->database, VENTURE_TYPE_INTEGRATION_CONNECTION, connection_id, error);
	if (!enabled(selection->connection) || venture_entity_get_organization_id(selection->connection) != selection->payer ||
		!limits(selection->monthly, selection->concurrency, error)) goto unavailable;
	g_object_get(selection->connection, "provider", &provider, NULL);
	{ g_autofree gchar *identity = g_strconcat("ai-", purpose, NULL);
		if (selection->offer ? !g_str_has_prefix(provider ? provider : "", "platform-ai-") : g_strcmp0(provider, identity) != 0) goto unavailable; }
	selection->settings = venture_integration_service_resolve_version(venture_integration_service_get(self->database), selection->payer,
		connection_id, venture_entity_get_version(selection->connection), FALSE, error);
	if (!selection->settings || !settings_spec(self, selection->settings, error)) goto unavailable;
	selection->stamp = g_strdup_printf("%" G_GINT64_FORMAT ":%" G_GINT64_FORMAT ":%" G_GINT64_FORMAT ":%" G_GINT64_FORMAT
		":%" G_GINT64_FORMAT ":%" G_GINT64_FORMAT ":%" G_GINT64_FORMAT ":%" G_GINT64_FORMAT,
		venture_entity_get_id(selection->configuration), venture_entity_get_version(selection->configuration),
		selection->grant ? venture_entity_get_id(selection->grant) : 0, selection->grant ? venture_entity_get_version(selection->grant) : 0,
		selection->offer ? venture_entity_get_id(selection->offer) : 0, selection->offer ? venture_entity_get_version(selection->offer) : 0,
		connection_id, venture_entity_get_version(selection->connection));
	if (principal) {
		g_autoptr(VentureEntity) user = principal->user_id > 0 ? venture_database_get(self->database, VENTURE_TYPE_USER, principal->user_id, NULL) : NULL;
		g_autofree gchar *previous_stamp = g_steal_pointer(&selection->stamp);
		selection->stamp = g_strdup_printf("%s:user:%" G_GINT64_FORMAT, previous_stamp,
			user ? venture_entity_get_version(user) : 0);
	}
	return g_steal_pointer(&selection);
unavailable:
	if (!(error && *error)) deny(error, VENTURE_ERROR_CONFIG, "AI is disabled, unconfigured or its selected provider/grant is unavailable; no fallback is permitted");
	return NULL;
}
JsonNode *venture_ai_provider_service_effective(VentureAiProviderService *self, gint64 org, const gchar *purpose, GError **error)
{
	g_autoptr(Selection) selection = NULL;
	g_autoptr(VentureEntity) configuration_row = NULL;
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autofree gchar *mode = NULL;
	const VentureAuthPrincipal *principal;
	g_return_val_if_fail(VENTURE_IS_AI_PROVIDER_SERVICE(self), NULL);
	if (!self->database) { deny(error, VENTURE_ERROR_CONFIG, "AI settings are unavailable"); return NULL; }
	principal = venture_access_policy_get_actor(venture_database_get_access_policy(self->database));
	if (!purpose_valid(purpose, error) || !may_use(self, org, principal, error)) return NULL;
	selection = selection_new(self, org, purpose, principal, NULL);
	internal = venture_access_policy_enter(venture_database_get_access_policy(self->database), NULL);
	configuration_row = configuration(self, org, purpose, error);
	if (error && *error) return NULL;
	if (configuration_row) g_object_get(configuration_row, "mode", &mode, NULL);
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "organization_id"); json_builder_add_int_value(builder, org);
	json_builder_set_member_name(builder, "mode"); json_builder_add_string_value(builder, mode ? mode : "disabled");
	json_builder_set_member_name(builder, "available"); json_builder_add_boolean_value(builder, selection != NULL);
	json_builder_set_member_name(builder, "configuration_version"); json_builder_add_int_value(builder, configuration_row ? venture_entity_get_version(configuration_row) : 0);
	if (selection) {
		JsonObject *settings = json_node_get_object(selection->settings);
		json_builder_set_member_name(builder, "provider"); json_builder_add_string_value(builder, json_object_get_string_member(settings, "provider"));
		json_builder_set_member_name(builder, "model"); json_builder_add_string_value(builder, json_object_get_string_member(settings, "model"));
		json_builder_set_member_name(builder, "payer_organization_id"); json_builder_add_int_value(builder, selection->payer);
		json_builder_set_member_name(builder, "monthly_requests"); json_builder_add_int_value(builder, selection->monthly);
		json_builder_set_member_name(builder, "concurrency_limit"); json_builder_add_int_value(builder, selection->concurrency);
	}
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_AI_USAGE);
		g_autoptr(VentureEntity) latest = NULL;
		g_autofree gchar *state = NULL;
		venture_query_set_organization(query, org);
		venture_query_add_filter_string(query, "purpose", VENTURE_FILTER_OP_EQ, purpose, NULL);
		venture_query_add_order(query, "id", VENTURE_SORT_DESCENDING, NULL);
		latest = venture_database_find_one(self->database, query, NULL);
		if (latest) g_object_get(latest, "state", &state, NULL);
		json_builder_set_member_name(builder, "last_request_state"); json_builder_add_string_value(builder, state ? state : "untested");
	}
	json_builder_end_object(builder); return json_builder_get_root(builder);
}

/* AiProvider is the substitution boundary: every executor round, including
 * tool continuations, must reserve and revalidate rather than one chat UI path. */
typedef struct { GObject parent_instance; VentureAiProviderService *service; AiProvider *delegate;
	VentureAuthPrincipal *principal; Selection *selection; GMainContext *context; GThread *owner_thread; } VentureScopedAiProvider;
typedef struct { GObjectClass parent_class; } VentureScopedAiProviderClass;
static GType venture_scoped_ai_provider_get_type(void);
static void scoped_provider_iface(AiProviderInterface *iface);
static void scoped_embedder_iface(AiEmbedderInterface *iface);
G_DEFINE_TYPE_WITH_CODE(VentureScopedAiProvider, venture_scoped_ai_provider, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(AI_TYPE_PROVIDER, scoped_provider_iface)
	G_IMPLEMENT_INTERFACE(AI_TYPE_EMBEDDER, scoped_embedder_iface))
static void scoped_finalize(GObject *object)
{
	VentureScopedAiProvider *self = (VentureScopedAiProvider *)object;
	g_clear_object(&self->service); g_clear_object(&self->delegate);
	g_clear_pointer(&self->principal, venture_auth_principal_free); selection_free(self->selection);
	g_clear_pointer(&self->context, g_main_context_unref);
	G_OBJECT_CLASS(venture_scoped_ai_provider_parent_class)->finalize(object);
}
static void venture_scoped_ai_provider_class_init(VentureScopedAiProviderClass *klass)
{ G_OBJECT_CLASS(klass)->finalize = scoped_finalize; }
static void venture_scoped_ai_provider_init(VentureScopedAiProvider *self) { (void)self; }
gboolean venture_ai_provider_service_check_provider(AiProvider *provider, GError **error)
{
	VentureScopedAiProvider *self;
	g_autoptr(Selection) current = NULL;
	if (!provider) return deny(error, VENTURE_ERROR_CONFIG, "Select an organization AI provider");
	if (!G_TYPE_CHECK_INSTANCE_TYPE(provider, venture_scoped_ai_provider_get_type())) return TRUE;
	self = (VentureScopedAiProvider *)provider;
	current = selection_new(self->service, self->selection->organization, self->selection->purpose, self->principal, error);
	if (!current) return FALSE;
	return !g_strcmp0(current->stamp, self->selection->stamp) ||
		deny(error, VENTURE_ERROR_CONFLICT, "AI settings or platform eligibility changed; start a new request");
}
static VentureEntity *reserve_request(VentureScopedAiProvider *self, GError **error)
{
	VentureAiProviderService *service = self->service;
	Selection *selection = self->selection;
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(VentureEntity) period = NULL, usage = NULL;
	g_autoptr(VentureQuery) pending = venture_query_new(VENTURE_TYPE_AI_USAGE);
	g_autoptr(GDateTime) now = venture_time_now(), lease = NULL;
	g_autofree gchar *month = g_date_time_format(now, "%Y-%m"), *key = NULL, *cutoff = venture_time_to_string(now);
	VentureActor actor;
	gint64 active;
	if (!venture_ai_provider_service_check_provider(AI_PROVIDER(self), error)) return NULL;
	internal = venture_access_policy_enter(venture_database_get_access_policy(service->database), NULL);
	if (!venture_database_begin(service->database, error)) return NULL;
	key = g_strdup_printf("org-%" G_GINT64_FORMAT "-%s-%s", selection->organization, selection->purpose, month);
	period = by_key(service, VENTURE_TYPE_AI_USAGE_PERIOD, "unique-key", key, error);
	if (error && *error) goto fail;
	if (!period) period = g_object_new(VENTURE_TYPE_AI_USAGE_PERIOD, "organization-id", selection->organization,
		"period-key", key, "unique-key", key, "month", month, NULL);
	venture_query_set_organization(pending, selection->organization);
	venture_query_add_filter_string(pending, "purpose", VENTURE_FILTER_OP_EQ, selection->purpose, NULL);
	venture_query_add_filter_string(pending, "state", VENTURE_FILTER_OP_EQ, "reserved", NULL);
	venture_query_add_filter_string(pending, "lease-until", VENTURE_FILTER_OP_GT, cutoff, NULL);
	active = venture_database_count(service->database, pending, error);
	if (active < 0) goto fail;
	if (integer(period, "requests") >= selection->monthly || active >= selection->concurrency) {
		deny(error, VENTURE_ERROR_CONFLICT, "This organization's AI request quota or concurrency limit is exhausted"); goto fail;
	}
	venture_auth_to_actor(self->principal, &actor);
	g_object_set(period, "requests", integer(period, "requests") + 1, NULL);
	/* Optimistic versioning serializes competing reservations before any
	 * network I/O. A loser refuses without spending provider credentials. */
	if (!save(service, period, &actor, error)) goto fail;
	lease = g_date_time_add_seconds(now, AI_RESERVATION_SECONDS);
	usage = g_object_new(VENTURE_TYPE_AI_USAGE, "organization-id", selection->organization,
		"title", "AI provider request", "purpose", selection->purpose, "period-id", venture_entity_get_id(period),
		"connection-id", venture_entity_get_id(selection->connection), "connection-version", venture_entity_get_version(selection->connection),
		"grant-id", selection->grant ? venture_entity_get_id(selection->grant) : (gint64)0,
		"user-id", self->principal ? self->principal->user_id : (gint64)0, "payer-organization-id", selection->payer,
		"source", selection->mode, "state", "reserved", "lease-until", lease, NULL);
	if (!save(service, usage, &actor, error) || !venture_database_commit(service->database, error)) goto fail;
	return g_steal_pointer(&usage);
fail:
	venture_database_rollback(service->database); return NULL;
}
typedef struct { VentureEntity *usage; VentureDatabase *database; GCancellable *cancel; GCancellable *outer;
	gulong outer_handler; GSource *deadline; GMainContext *context; GList *messages, *tools; gchar *system_prompt; gint max_tokens; gchar **texts; } ScopedRequest;
static void request_free(gpointer data)
{
	ScopedRequest *request = data;
	if (request->outer_handler) g_cancellable_disconnect(request->outer, request->outer_handler);
	if (request->deadline) { g_source_destroy(request->deadline); g_source_unref(request->deadline); }
	g_clear_object(&request->usage); g_clear_object(&request->database);
	g_clear_object(&request->cancel); g_clear_object(&request->outer);
	g_list_free_full(request->messages, g_object_unref); g_list_free_full(request->tools, g_object_unref);
	g_clear_pointer(&request->context, g_main_context_unref);
	g_strfreev(request->texts); g_free(request->system_prompt); g_free(request);
}
static void request_cancel(GCancellable *outer, gpointer data) { (void)outer; g_cancellable_cancel(data); }
static gboolean request_deadline(gpointer data) { g_cancellable_cancel(data); return G_SOURCE_REMOVE; }
static gboolean request_finish_usage(VentureScopedAiProvider *self, ScopedRequest *request,
	gboolean success, gint64 input, gint64 output, GError **error)
{
	g_autoptr(VentureAccessScope) internal = NULL;
	VentureActor actor;
	gboolean valid = venture_ai_provider_service_check_provider(AI_PROVIDER(self), NULL);
	internal = venture_access_policy_enter(venture_database_get_access_policy(self->service->database), NULL);
	venture_auth_to_actor(self->principal, &actor);
	g_object_set(request->usage, "state", !valid ? "revoked" : success ? "completed" : "failed",
		"input-tokens", input, "output-tokens", output, NULL);
	if (!save(self->service, request->usage, &actor, NULL))
		return deny(error, VENTURE_ERROR_AI, "AI usage could not be finalized; the request remains charged");
	/* Save observers can revoke a grant too; check after every derived write. */
	if (!valid || !venture_ai_provider_service_check_provider(AI_PROVIDER(self), NULL))
		return deny(error, VENTURE_ERROR_PERMISSION_DENIED, "AI provider access was revoked or changed during this request");
	if (!success || g_cancellable_is_cancelled(request->cancel))
		return deny(error, VENTURE_ERROR_AI, "The selected AI provider failed or the request was cancelled; no fallback was attempted");
	return TRUE;
}
static void provider_done(GObject *source, GAsyncResult *result, gpointer data)
{
	g_autoptr(GTask) task = data;
	VentureScopedAiProvider *self = g_task_get_source_object(task);
	ScopedRequest *request = g_task_get_task_data(task);
	g_autoptr(AiResponse) response = ai_provider_chat_finish(AI_PROVIDER(source), result, NULL);
	g_autoptr(GError) error = NULL;
	AiUsage *usage = response ? ai_response_get_usage(response) : NULL;
	if (!request_finish_usage(self, request, response != NULL,
		usage ? (gint64)MAX(0, ai_usage_get_input_tokens(usage)) : 0,
		usage ? (gint64)MAX(0, ai_usage_get_output_tokens(usage)) : 0, &error)) {
		g_task_return_error(task, g_steal_pointer(&error)); return;
	}
	g_task_return_pointer(task, g_steal_pointer(&response), g_object_unref);
}
static void embedding_done(GObject *source, GAsyncResult *result, gpointer data)
{
	g_autoptr(GTask) task = data;
	VentureScopedAiProvider *self = g_task_get_source_object(task);
	ScopedRequest *request = g_task_get_task_data(task);
	g_autoptr(AiEmbedding) embedding = ai_embedder_embed_finish(AI_EMBEDDER(source), result, NULL);
	g_autoptr(GError) error = NULL;
	/* AiEmbedding exposes vectors and model, but no provider token usage. */
	if (!request_finish_usage(self, request, embedding != NULL, 0, 0, &error)) {
		g_task_return_error(task, g_steal_pointer(&error)); return;
	}
	g_task_return_pointer(task, g_steal_pointer(&embedding), (GDestroyNotify)ai_embedding_unref);
}
/* Dispatch through a source, not invoke_full(): the latter may acquire an
 * idle context and execute immediately on the coding worker. */
static const gchar *bound_model(VentureScopedAiProvider *self)
{ return json_object_get_string_member(json_node_get_object(self->selection->settings), "model"); }
static gboolean scoped_dispatch(gpointer data)
{
	GTask *task = data;
	VentureScopedAiProvider *self = g_task_get_source_object(task);
	ScopedRequest *request = g_task_get_task_data(task);
	g_autoptr(GError) error = NULL;
	GCancellable *outer = g_task_get_cancellable(task);
	if (g_cancellable_set_error_if_cancelled(outer, &error)) {
		g_task_return_error(task, g_steal_pointer(&error)); return G_SOURCE_REMOVE;
	}
	request->usage = reserve_request(self, &error);
	if (!request->usage) { g_task_return_error(task, g_steal_pointer(&error)); return G_SOURCE_REMOVE; }
	request->database = g_object_ref(self->service->database);
	request->cancel = g_cancellable_new();
	if (outer) {
		request->outer = g_object_ref(outer);
		request->outer_handler = g_cancellable_connect(outer, G_CALLBACK(request_cancel), request->cancel, NULL);
	}
	request->deadline = g_timeout_source_new_seconds((guint)self->selection->deadline);
	g_source_set_callback(request->deadline, request_deadline, request->cancel, NULL);
	g_source_attach(request->deadline, request->context);
	/* Reservation saves emit synchronous observers. A revocation from one
	 * must stop the request before any business input reaches the provider. */
	if (!venture_ai_provider_service_check_provider(AI_PROVIDER(self), &error)) {
		request_finish_usage(self, request, FALSE, 0, 0, NULL);
		g_task_return_error(task, g_steal_pointer(&error)); return G_SOURCE_REMOVE;
	}
	/* Bind the delegate's completion to the database-owning context even
	 * when a nested request arrived through a different thread default. */
	g_main_context_push_thread_default(request->context);
	if (request->texts)
		ai_embedder_embed_async(AI_EMBEDDER(self->delegate), (const gchar *const *)request->texts,
			bound_model(self), request->cancel, embedding_done, g_object_ref(task));
	else
		ai_provider_chat_async(self->delegate, request->messages, request->system_prompt, request->max_tokens,
			request->tools, request->cancel, provider_done, g_object_ref(task));
	g_main_context_pop_thread_default(request->context);
	return G_SOURCE_REMOVE;
}
static void scoped_schedule(VentureScopedAiProvider *self, GTask *task)
{
	ScopedRequest *request = g_task_get_task_data(task);
	g_autoptr(GSource) source = g_idle_source_new();
	if (g_thread_self() == self->owner_thread) {
		request->context = g_main_context_ref_thread_default();
		scoped_dispatch(task);
	} else {
		request->context = g_main_context_ref(self->context);
		g_source_set_callback(source, scoped_dispatch, g_object_ref(task), g_object_unref);
		g_source_attach(source, self->context);
	}
}
static void scoped_chat(AiProvider *provider, GList *messages, const gchar *system_prompt, gint max_tokens,
	GList *tools, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer data)
{
	VentureScopedAiProvider *self = (VentureScopedAiProvider *)provider;
	g_autoptr(GTask) task = g_task_new(self, cancellable, callback, data);
	ScopedRequest *request = g_new0(ScopedRequest, 1);
	GList *link;
	g_task_set_task_data(task, request, request_free);
	if (!g_strcmp0(self->selection->purpose, "embedding")) {
		g_task_return_new_error(task, VENTURE_ERROR, VENTURE_ERROR_CONFIG, "Embedding credentials cannot authorize chat"); return;
	}
	{
		gsize total = system_prompt ? strlen(system_prompt) : 0;
		guint count = 0;
		for (link = messages; link; link = link->next) {
			g_autofree gchar *text = ai_message_get_text(link->data);
			total += text ? strlen(text) : 0;
			if (++count > 128 || total > 1024 * 1024) break;
		}
		if (!messages || count > 128 || total > 1024 * 1024 || max_tokens < 1 || max_tokens > 32768) {
			g_task_return_new_error(task, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "AI request exceeds the bounded conversation or output limit"); return;
		}
	}
	for (link = messages; link; link = link->next) request->messages = g_list_prepend(request->messages, g_object_ref(link->data));
	for (link = tools; link; link = link->next) request->tools = g_list_prepend(request->tools, g_object_ref(link->data));
	request->messages = g_list_reverse(request->messages); request->tools = g_list_reverse(request->tools);
	request->system_prompt = g_strdup(system_prompt); request->max_tokens = max_tokens;
	scoped_schedule(self, task);
}
static void scoped_embed(AiEmbedder *embedder, const gchar *const *texts, const gchar *model,
	GCancellable *cancellable, GAsyncReadyCallback callback, gpointer data)
{
	VentureScopedAiProvider *self = (VentureScopedAiProvider *)embedder;
	g_autoptr(GTask) task = g_task_new(self, cancellable, callback, data);
	ScopedRequest *request = g_new0(ScopedRequest, 1);
	g_task_set_task_data(task, request, request_free);
	if (g_strcmp0(self->selection->purpose, "embedding") || !AI_IS_EMBEDDER(self->delegate) ||
		(model && g_strcmp0(model, bound_model(self)))) {
		g_task_return_new_error(task, VENTURE_ERROR, VENTURE_ERROR_CONFIG, "Select the organization's explicit embedding provider and model"); return;
	}
	if (!texts || !texts[0]) {
		g_task_return_new_error(task, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "No passages to embed"); return;
	}
	{
		gsize total = 0;
		guint i;
		for (i = 0; texts[i]; i++) {
			gsize length = strlen(texts[i]); total += length;
			if (i >= 128 || length > 32768 || total > 512 * 1024) {
				g_task_return_new_error(task, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Embedding batch exceeds 128 passages, 32 KiB per passage or 512 KiB total"); return;
			}
		}
	}
	request->texts = g_strdupv((gchar **)texts);
	scoped_schedule(self, task);
}
static AiEmbedding *scoped_embed_finish(AiEmbedder *self, GAsyncResult *result, GError **error)
{ g_return_val_if_fail(g_task_is_valid(result, self), NULL); return g_task_propagate_pointer(G_TASK(result), error); }
static const gchar *scoped_embedding_model(AiEmbedder *self)
{ return bound_model((VentureScopedAiProvider *)self); }
static GList *scoped_embedding_models(AiEmbedder *self)
{
	VentureScopedAiProvider *provider = (VentureScopedAiProvider *)self;
	return AI_IS_EMBEDDER(provider->delegate) ? ai_embedder_list_embedding_models(AI_EMBEDDER(provider->delegate)) : NULL;
}
static void scoped_embedder_iface(AiEmbedderInterface *iface)
{ iface->embed_async = scoped_embed; iface->embed_finish = scoped_embed_finish;
	iface->get_default_embedding_model = scoped_embedding_model; iface->list_embedding_models = scoped_embedding_models; }
static AiResponse *scoped_finish(AiProvider *provider, GAsyncResult *result, GError **error)
{ g_return_val_if_fail(g_task_is_valid(result, provider), NULL); return g_task_propagate_pointer(G_TASK(result), error); }
static const gchar *scoped_name(AiProvider *provider)
{ return ai_provider_get_name(((VentureScopedAiProvider *)provider)->delegate); }
static const gchar *scoped_model(AiProvider *provider)
{ return bound_model((VentureScopedAiProvider *)provider); }
static AiProviderType scoped_kind(AiProvider *provider)
{ return ai_provider_get_provider_type(((VentureScopedAiProvider *)provider)->delegate); }
static void scoped_provider_iface(AiProviderInterface *iface)
{ iface->chat_async = scoped_chat; iface->chat_finish = scoped_finish; iface->get_name = scoped_name;
	iface->get_default_model = scoped_model; iface->get_provider_type = scoped_kind; }
AiProvider *venture_ai_provider_service_create_provider(VentureAiProviderService *self,
	gint64 org, const gchar *purpose, const VentureAuthPrincipal *principal, GError **error)
{
	g_autoptr(Selection) selection = NULL;
	g_autoptr(AiConfig) config = NULL;
	const ProviderSpec *spec;
	JsonObject *settings;
	VentureScopedAiProvider *provider;
	g_return_val_if_fail(VENTURE_IS_AI_PROVIDER_SERVICE(self), NULL);
	selection = selection_new(self, org, purpose, principal, error);
	if (!selection) return NULL;
	spec = settings_spec(self, selection->settings, error);
	if (!spec) return NULL;
	settings = json_node_get_object(selection->settings);
	/* ai_config_new() loads host/user configuration. Empty construction and
	 * explicit nonempty values prevent every ambient credential fallback. */
	config = g_object_new(AI_TYPE_CONFIG, NULL);
	ai_config_set_api_key(config, spec->kind, json_object_get_string_member(settings, "api_key"));
	ai_config_set_base_url(config, spec->kind, json_object_get_string_member(settings, "base_url"));
	ai_config_set_timeout(config, 30); ai_config_set_max_retries(config, 0);
	provider = g_object_new(venture_scoped_ai_provider_get_type(), NULL);
	provider->context = g_main_context_ref_thread_default(); provider->owner_thread = g_thread_self();
	provider->service = g_object_ref(self); provider->principal = principal ? venture_auth_principal_copy(principal) : NULL;
	provider->delegate = spec->create(config);
	ai_client_set_model(AI_CLIENT(provider->delegate), json_object_get_string_member(settings, "model"));
	if (!g_strcmp0(purpose, "embedding") && !AI_IS_EMBEDDER(provider->delegate)) {
		g_object_unref(provider); deny(error, VENTURE_ERROR_CONFIG, "The selected provider has no embedding API"); return NULL;
	}
	provider->selection = g_steal_pointer(&selection);
	return AI_PROVIDER(provider);
}

static gboolean ai_platform_action_allowed(VentureAction *action, VentureEntity *entity,
	const VentureActor *actor, GError **error)
{
	(void)entity; (void)actor;
	return manage(venture_action_get_data(action), 0, TRUE, error);
}
static gint64 action_integer(GHashTable *params, const gchar *name)
{ JsonNode *node = g_hash_table_lookup(params, name); return node ? json_node_get_int(node) : 0; }
static VentureEntity *ai_platform_action_invoke(VentureAction *action, VentureEntity *entity,
	GHashTable *params, const VentureActor *actor, GError **error)
{
	VentureAiProviderService *self = venture_action_get_data(action);
	g_autofree gchar *name = NULL;
	g_object_get(action, "name", &name, NULL);
	if (!g_strcmp0(name, "grant")) {
		JsonNode *active = g_hash_table_lookup(params, "enabled");
		return VENTURE_ENTITY(venture_ai_provider_service_grant(self, action_integer(params, "organization_id"),
			venture_entity_get_id(entity), active && json_node_get_boolean(active),
			action_integer(params, "monthly_requests"), action_integer(params, "concurrency_limit"),
			action_integer(params, "grant_version"), actor, error));
	}
	if (!venture_ai_provider_service_disable_offer(self, venture_entity_get_id(entity), venture_entity_get_version(entity), actor, error)) return NULL;
	return venture_database_get(self->database, VENTURE_TYPE_AI_PLATFORM_OFFER, venture_entity_get_id(entity), error);
}
void venture_ai_provider_actions_register(VentureDatabase *database)
{
	static const gchar *const names[] = { "grant", "disable" };
	static const gchar *const labels[] = { "Grant or revoke organization eligibility", "Disable platform offer" };
	guint i;
	for (i = 0; i < G_N_ELEMENTS(names); i++) {
		g_autoptr(GPtrArray) parameters = g_ptr_array_new_with_free_func((GDestroyNotify)venture_field_spec_free);
		g_autoptr(VentureAction) action = NULL;
		g_autoptr(GError) error = NULL;
		if (i == 0) {
			static const gchar *const keys[] = { "organization_id", "enabled", "monthly_requests", "concurrency_limit", "grant_version" };
			guint j;
			for (j = 0; j < G_N_ELEMENTS(keys); j++) {
				VentureFieldSpec *field = venture_field_spec_new(keys[j], keys[j], j == 1 ? VENTURE_FIELD_KIND_BOOLEAN :
					j == 0 ? VENTURE_FIELD_KIND_REFERENCE : VENTURE_FIELD_KIND_INTEGER);
				field->required = TRUE;
				if (j == 0) field->reference_type = g_strdup("organization");
				g_ptr_array_add(parameters, field);
			}
		}
		action = g_object_new(VENTURE_TYPE_ACTION, "data-class", VENTURE_DATA_CLASS_PLATFORM, "type-name", "ai_platform_offer", "name", names[i], "label", labels[i],
			"description", "Platform operator only; a recipient must select its grant explicitly", "parameters", parameters,
			"stageable", FALSE, "service-transaction", TRUE, "roles", VENTURE_USER_ROLE_ADMIN, NULL);
		if (!venture_action_registry_register(venture_database_get_action_registry(database), action, ai_platform_action_allowed,
			ai_platform_action_invoke, venture_ai_provider_service_get(database), NULL, &error)) g_error("AI action registration: %s", error->message);
	}
}

gboolean venture_ai_provider_service_test(VentureAiProviderService *self, gint64 org, const gchar *purpose, GError **error)
{
	g_autoptr(AiProvider) provider = NULL;
	g_autoptr(AiToolExecutor) executor = NULL;
	g_autofree gchar *reply = NULL;
	GList *messages;
	g_return_val_if_fail(VENTURE_IS_AI_PROVIDER_SERVICE(self), FALSE);
	if (!manage(self, org, FALSE, error)) return FALSE;
	provider = venture_ai_provider_service_create_provider(self, org, purpose,
		venture_access_policy_get_actor(venture_database_get_access_policy(self->database)), error);
	if (!provider) return FALSE;
	if (!g_strcmp0(purpose, "embedding")) {
		const gchar *texts[] = { "Venture connection test", NULL };
		g_autoptr(AiEmbedding) embedding = ai_embedder_embed(AI_EMBEDDER(provider), texts,
			ai_embedder_get_default_embedding_model(AI_EMBEDDER(provider)), NULL, error);
		return embedding != NULL;
	}
	executor = ai_tool_executor_new_empty();
	messages = g_list_append(NULL, ai_message_new_user("Reply with OK."));
	reply = ai_tool_executor_run(executor, provider, messages, "Connection test; no business records or tools are available.", 16, NULL, error);
	g_list_free_full(messages, g_object_unref);
	return reply != NULL;
}

gchar **venture_ai_provider_service_list_providers(VentureAiProviderService *self)
{
	gchar **names;
	guint i;
	g_return_val_if_fail(VENTURE_IS_AI_PROVIDER_SERVICE(self), NULL);
	names = g_new0(gchar *, G_N_ELEMENTS(providers) + 1);
	for (i = 0; i < G_N_ELEMENTS(providers); i++) names[i] = g_strdup(providers[i].name);
	return names;
}
