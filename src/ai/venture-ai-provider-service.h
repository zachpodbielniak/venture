/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_AI_PROVIDER_SERVICE_H
#define VENTURE_AI_PROVIDER_SERVICE_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
#define VENTURE_TYPE_AI_PROVIDER_SERVICE (venture_ai_provider_service_get_type())
G_DECLARE_FINAL_TYPE(VentureAiProviderService, venture_ai_provider_service, VENTURE, AI_PROVIDER_SERVICE, GObject)
/**
 * venture_ai_provider_service_get:
 * @database: repository, weakly held
 * Returns: (transfer none): repository-owned provider configuration service
 */
VentureAiProviderService *venture_ai_provider_service_get(VentureDatabase *database);
/**
 * venture_ai_provider_service_set_config:
 * @self: service
 * @config: platform endpoint policy, strongly held
 */
void venture_ai_provider_service_set_config(VentureAiProviderService *self, VentureConfig *config);
/**
 * venture_ai_provider_service_configure:
 * @self: service
 * @purpose: independent chat, embedding or coding configuration
 * @organization_id: organization whose administrator is acting
 * @settings: write-only provider, model, api_key and base_url object
 * @monthly_requests: positive monthly provider-request limit
 * @concurrency_limit: positive simultaneous-request limit
 * @expected_version: current AI configuration version, or zero to create
 * @actor: (nullable): audit attribution; scope determines authority
 * @error: (out) (optional): redacted refusal
 * Returns: (transfer full) (nullable): explicit organization-owned configuration
 */
VentureAiConfiguration *venture_ai_provider_service_configure(VentureAiProviderService *self,
	gint64 organization_id, const gchar *purpose, JsonNode *settings, gint64 monthly_requests, gint64 concurrency_limit,
	gint64 expected_version, const VentureActor *actor, GError **error);
/**
 * venture_ai_provider_service_select:
 * @self: service
 * @purpose: independent chat, embedding or coding configuration
 * @organization_id: organization whose administrator is acting
 * @grant_id: positive explicit platform grant, or zero to disable and disconnect own credentials
 * @expected_version: current configuration version, or zero to create
 * @actor: (nullable): audit attribution
 * @error: (out) (optional): redacted refusal
 * Returns: (transfer full) (nullable): explicit platform or disabled configuration
 */
VentureAiConfiguration *venture_ai_provider_service_select(VentureAiProviderService *self,
	gint64 organization_id, const gchar *purpose, gint64 grant_id, gint64 expected_version, const VentureActor *actor, GError **error);
/**
 * venture_ai_provider_service_offer:
 * @self: service
 * @purpose: independent chat, embedding or coding configuration
 * @billing_organization_id: explicitly selected platform payer
 * @title: safe service label
 * @settings: write-only provider settings
 * @offer_id: existing platform offer, or zero to create
 * @expected_version: current offer version, or zero to create
 * @actor: (nullable): audit attribution; only platform authority may call
 * @error: (out) (optional): redacted refusal
 * Returns: (transfer full) (nullable): platform-owned offer with encrypted credentials
 */
VentureAiPlatformOffer *venture_ai_provider_service_offer(VentureAiProviderService *self,
	gint64 billing_organization_id, const gchar *purpose, const gchar *title, JsonNode *settings,
	gint64 offer_id, gint64 expected_version, const VentureActor *actor, GError **error);
/**
 * venture_ai_provider_service_grant:
 * @self: service
 * @organization_id: eligible recipient organization
 * @offer_id: platform-owned offer
 * @enabled: explicit eligibility or revocation
 * @monthly_requests: positive monthly request limit
 * @concurrency_limit: positive concurrent request limit
 * @expected_version: current grant version, or zero to create
 * @actor: (nullable): audit attribution; only platform authority may call
 * @error: (out) (optional): redacted refusal
 * Returns: (transfer full) (nullable): audited grant; selection still belongs to the recipient
 */
VentureAiGrant *venture_ai_provider_service_grant(VentureAiProviderService *self,
	gint64 organization_id, gint64 offer_id, gboolean enabled, gint64 monthly_requests,
	gint64 concurrency_limit, gint64 expected_version, const VentureActor *actor, GError **error);
/**
 * venture_ai_provider_service_disable_offer:
 * @self: service
 * @offer_id: platform offer to disable
 * @expected_version: current offer version
 * @actor: (nullable): audit attribution; only platform authority may call
 * @error: (out) (optional): redacted refusal
 * Returns: whether all its grants and pending results are now refused
 */
gboolean venture_ai_provider_service_disable_offer(VentureAiProviderService *self,
	gint64 offer_id, gint64 expected_version, const VentureActor *actor, GError **error);
/**
 * venture_ai_provider_service_effective:
 * @self: service
 * @purpose: independent chat, embedding or coding configuration
 * @organization_id: organization visible to the current caller
 * @error: (out) (optional): redacted refusal
 * Returns: (transfer full) (nullable): safe effective source, health, payer and limits; never credentials
 */
JsonNode *venture_ai_provider_service_effective(VentureAiProviderService *self, gint64 organization_id, const gchar *purpose, GError **error);
/**
 * venture_ai_provider_service_create_provider:
 * @self: service
 * @purpose: independent chat, embedding or coding configuration
 * @organization_id: explicit data and billing scope
 * @principal: (nullable): captured local caller; NULL is trusted internal work
 * @error: (out) (optional): redacted refusal
 *
 * Each request reserves durable quota, rechecks versions and authority, and
 * records provider-reported usage on the calling main context. No database
 * access occurs on a worker. A revoked response is never returned to tools.
 * Returns: (transfer full) (nullable): organization-bound provider adapter
 */
AiProvider *venture_ai_provider_service_create_provider(VentureAiProviderService *self,
	gint64 organization_id, const gchar *purpose, const VentureAuthPrincipal *principal, GError **error);
/**
 * venture_ai_provider_service_check_provider:
 * @provider: provider created above, or a trusted injected test provider
 * @error: (out) (optional): revoked binding or local authority
 * Returns: whether a subsequent tool action may still use this provider's authority
 */
gboolean venture_ai_provider_service_check_provider(AiProvider *provider, GError **error);
/**
 * venture_ai_provider_check_write:
 * @database: repository
 * @entity: proposed configuration, grant or usage
 * @removal: deletion, restoration or purge
 * @error: (out) (optional): refusal
 * Returns: whether this lifecycle write is service-authorized
 */
gboolean venture_ai_provider_check_write(VentureDatabase *database, VentureEntity *entity,
	gboolean removal, GError **error);
/**
 * venture_ai_provider_actions_register:
 * @database: repository whose metadata action registry receives platform operations
 */
void venture_ai_provider_actions_register(VentureDatabase *database);
/**
 * venture_ai_provider_service_test:
 * @self: service
 * @organization_id: explicit organization whose administrator is acting
 * @purpose: chat, embedding or coding
 * @error: (out) (optional): redacted failure
 *
 * Sends a bounded synthetic request without business records or tools. The
 * normal durable request quota and revocation rules apply, including cost.
 * Returns: whether the explicitly selected provider answered
 */
gboolean venture_ai_provider_service_test(VentureAiProviderService *self,
	gint64 organization_id, const gchar *purpose, GError **error);
/**
 * venture_ai_provider_service_list_providers:
 * @self: service
 * Returns: (transfer full) (array zero-terminated=1): supported explicit HTTP
 *   provider names from the same table used by credential validation
 */
gchar **venture_ai_provider_service_list_providers(VentureAiProviderService *self);
G_END_DECLS
#endif
