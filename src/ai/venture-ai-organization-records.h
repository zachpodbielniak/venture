/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_AI_ORGANIZATION_RECORDS_H
#define VENTURE_AI_ORGANIZATION_RECORDS_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
#define VENTURE_TYPE_AI_CONFIGURATION (venture_ai_configuration_get_type())
G_DECLARE_FINAL_TYPE(VentureAiConfiguration, venture_ai_configuration, VENTURE, AI_CONFIGURATION, VentureEntity)
#define VENTURE_TYPE_AI_PLATFORM_OFFER (venture_ai_platform_offer_get_type())
G_DECLARE_FINAL_TYPE(VentureAiPlatformOffer, venture_ai_platform_offer, VENTURE, AI_PLATFORM_OFFER, VentureEntity)
#define VENTURE_TYPE_AI_GRANT (venture_ai_grant_get_type())
G_DECLARE_FINAL_TYPE(VentureAiGrant, venture_ai_grant, VENTURE, AI_GRANT, VentureEntity)
#define VENTURE_TYPE_AI_USAGE_PERIOD (venture_ai_usage_period_get_type())
G_DECLARE_FINAL_TYPE(VentureAiUsagePeriod, venture_ai_usage_period, VENTURE, AI_USAGE_PERIOD, VentureEntity)
#define VENTURE_TYPE_AI_USAGE (venture_ai_usage_get_type())
G_DECLARE_FINAL_TYPE(VentureAiUsage, venture_ai_usage, VENTURE, AI_USAGE, VentureEntity)
/**
 * venture_ai_configuration_new:
 * Returns: (transfer full): an unsaved record; service operations authorize persistence
 */
VentureAiConfiguration *venture_ai_configuration_new(void);
/**
 * venture_ai_platform_offer_new:
 * Returns: (transfer full): an unsaved record; service operations authorize persistence
 */
VentureAiPlatformOffer *venture_ai_platform_offer_new(void);
/**
 * venture_ai_grant_new:
 * Returns: (transfer full): an unsaved record; service operations authorize persistence
 */
VentureAiGrant *venture_ai_grant_new(void);
/**
 * venture_ai_usage_period_new:
 * Returns: (transfer full): an unsaved record; service operations authorize persistence
 */
VentureAiUsagePeriod *venture_ai_usage_period_new(void);
/**
 * venture_ai_usage_new:
 * Returns: (transfer full): an unsaved record; service operations authorize persistence
 */
VentureAiUsage *venture_ai_usage_new(void);
G_END_DECLS
#endif
