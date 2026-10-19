/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * venture-marketing-text.h - Text consent and text campaigns
 *
 * Text consent is marketing consent evidence, in the same records as email
 * consent (purpose "text", keyed by the E.164 number): an agreement, a
 * STOP and a START are each one retained row, and the latest decides. A
 * text campaign is an ordinary marketing send with channel text: the same
 * preview, approval, audience, pacing and once-only recipients, queued
 * through the channel the telephony module registers.
 */
#ifndef VENTURE_MARKETING_TEXT_H
#define VENTURE_MARKETING_TEXT_H
G_BEGIN_DECLS

/**
 * VentureMarketingTextChannel:
 * @recipient: the E.164 number a campaign texts @source at, or why not
 * @queue: queue the frozen text for one approved recipient
 *
 * What the telephony module gives campaigns. Both run inside the
 * marketing service's transaction and must not talk to a provider.
 */
typedef struct {
	gboolean (*recipient)(VentureDatabase *database, VentureEntity *source, gchar **address,
		const gchar **reason, gpointer data, GError **error);
	gboolean (*queue)(VentureDatabase *database, VentureEntity *recipient, VentureEntity *source,
		const gchar *body, GDateTime *now, const VentureActor *actor, gpointer data, GError **error);
} VentureMarketingTextChannel;
/**
 * venture_marketing_text_channel_register:
 * @channel: (nullable): the channel; %NULL removes it
 * @data: (nullable): passed to its functions
 */
void venture_marketing_text_channel_register(const VentureMarketingTextChannel *channel, gpointer data);
/**
 * venture_marketing_service_text_consent:
 * @self: service
 * @organization_id: organization
 * @contact: the contact
 * @phone: the E.164 number
 * @granted: agreed, or withdrew
 * @source: where it came from
 * @evidence: what they said or did
 * @evidence_key: replay identity, unique in the organization
 * @at: when
 * @actor: (nullable): audit actor
 * @error: (out) (optional): refusal
 *
 * A replay of the same key returns the first row.
 *
 * Returns: (transfer full) (nullable): the evidence row
 */
VentureEntity *venture_marketing_service_text_consent(VentureMarketingService *self, gint64 organization_id,
	VentureEntity *contact, const gchar *phone, gboolean granted, const gchar *source,
	const gchar *evidence, const gchar *evidence_key, GDateTime *at, const VentureActor *actor, GError **error);
/**
 * venture_marketing_service_text_permitted:
 * @self: service
 * @organization_id: organization
 * @phone: the E.164 number
 * @consent_id: (out) (optional): the agreement that permits it
 * @error: (out) (optional): storage failure
 * Returns: whether the latest text consent evidence for @phone agrees
 */
gboolean venture_marketing_service_text_permitted(VentureMarketingService *self, gint64 organization_id,
	const gchar *phone, gint64 *consent_id, GError **error);
G_END_DECLS
#endif
