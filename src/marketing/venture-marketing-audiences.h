/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_MARKETING_AUDIENCES_H
#define VENTURE_MARKETING_AUDIENCES_H
G_BEGIN_DECLS

/* Audiences (docs/marketing.org): behaviour segments, the preference
 * centre, A/B tests, local-time sends and resends. Each extends the one
 * VentureMarketingService pipeline; none keeps a second audience. */

/**
 * VENTURE_MARKETING_AB_WAIT_HOURS:
 *
 * Hours after the test arms are queued before a winner is picked, when
 * the send names none.
 */
#define VENTURE_MARKETING_AB_WAIT_HOURS 4

/**
 * venture_marketing_service_segment_members:
 * @self: service
 * @organization_id: exact organization
 * @list_id: a saved-search list
 * @now: the clock "within N days" counts back from
 * @error: (out) (optional): an invalid search or one past the bound
 *
 * The records a saved search selects: its field filters, tag and
 * behaviour criteria, ordered by id. The preview freezes exactly this.
 *
 * Returns: (transfer full) (element-type VentureEntity) (nullable): the records
 */
GPtrArray *venture_marketing_service_segment_members(VentureMarketingService *self,
	gint64 organization_id, gint64 list_id, GDateTime *now, GError **error);
/**
 * venture_marketing_service_set_preferences:
 * @self: service
 * @token: the recipient's preference capability from a delivered message
 * @topics_off: (nullable): comma-separated topic ids turned off
 * @frequency: any, weekly or monthly
 * @error: (out) (optional): unknown capability or invalid choice
 *
 * Saves the address's choices. Like unsubscribing, the capability is the
 * only authority: no session or organization is read from the caller.
 *
 * Returns: whether the choices are saved
 */
gboolean venture_marketing_service_set_preferences(VentureMarketingService *self, const gchar *token,
	const gchar *topics_off, const gchar *frequency, GError **error);
/**
 * venture_marketing_service_record_preferences:
 * @self: service
 * @organization_id: exact organization
 * @email: the address
 * @topics_off: (nullable): comma-separated topic ids turned off
 * @frequency: any, weekly or monthly
 * @source: what the person asked and how
 * @actor: (nullable): audit actor
 * @error: (out) (optional): authorization or invalid choice
 *
 * An operator recording what somebody asked for, by phone or in person.
 *
 * Returns: whether the choices are saved
 */
gboolean venture_marketing_service_record_preferences(VentureMarketingService *self, gint64 organization_id,
	const gchar *email, const gchar *topics_off, const gchar *frequency, const gchar *source,
	const VentureActor *actor, GError **error);
/**
 * venture_marketing_service_preferences_for:
 * @self: service
 * @organization_id: exact organization
 * @email: the address, any capitals
 * @error: (out) (optional): persistence failure
 * Returns: (transfer full) (nullable): the address's choices, %NULL for none
 */
VentureEntity *venture_marketing_service_preferences_for(VentureMarketingService *self,
	gint64 organization_id, const gchar *email, GError **error);
/**
 * venture_marketing_service_preference_view:
 * @self: service
 * @token: the recipient's preference capability
 * @topics: (out) (transfer full) (element-type VentureEntity): the offered topics
 * @preference: (out) (transfer full) (nullable): the address's choices
 * @error: (out) (optional): unknown capability
 *
 * What the preference page shows. Neither the address nor the person is
 * returned: the page must not disclose who the link belongs to.
 *
 * Returns: whether the capability is known
 */
gboolean venture_marketing_service_preference_view(VentureMarketingService *self, const gchar *token,
	GPtrArray **topics, VentureEntity **preference, GError **error);
/**
 * venture_marketing_preference_allows:
 * @database: repository
 * @organization_id: organization
 * @email: normalized address
 * @topic_id: the message's topic, 0 for none
 * @now: the sending clock
 * @reason: (out) (optional) (transfer none): why not, in plain words
 * @error: (out) (optional): persistence failure
 *
 * The one preference rule campaigns and marketing journeys share.
 *
 * Returns: whether the address's choices allow a message now; FALSE with
 *   @error set on failure
 */
gboolean venture_marketing_preference_allows(VentureDatabase *database, gint64 organization_id,
	const gchar *email, gint64 topic_id, GDateTime *now, const gchar **reason, GError **error);
/**
 * venture_marketing_ab_assign:
 * @send_uuid: the send's identity
 * @emails: (array zero-terminated=1): the eligible addresses
 * @percent: the test share, 1 to 100
 *
 * Deterministic: the addresses are ordered by a digest of the send and the
 * address, the first @percent (rounded, at least two) are the test, split
 * alternately A and B.
 *
 * Returns: (transfer full) (array zero-terminated=1): "a", "b" or "rest"
 *   for each address, in @emails' order
 */
gchar **venture_marketing_ab_assign(const gchar *send_uuid, const gchar *const *emails, guint percent);
/**
 * venture_marketing_local_due:
 * @zone: an IANA zone
 * @hour: 0 to 23
 * @day: (nullable): YYYY-MM-DD; %NULL is the next time @hour comes round
 * @after: the earliest moment
 * @error: (out) (optional): an unknown zone or hour
 *
 * When @hour on the wall clock in @zone falls, in UTC. A day already past
 * is due at @after. An hour the clocks skip is the first moment after it.
 *
 * Returns: (transfer full) (nullable): the moment
 */
GDateTime *venture_marketing_local_due(const gchar *zone, gint hour, const gchar *day,
	GDateTime *after, GError **error);
/**
 * venture_marketing_service_resend:
 * @self: service
 * @organization_id: exact organization
 * @send_id: an approved send that has sent
 * @days: 0 to 365: how long after the first message before a non-opener counts
 * @subject: (nullable): a new subject; %NULL keeps the first
 * @actor: (nullable): audit actor
 * @error: (out) (optional): the send cannot be resent
 *
 * A new draft whose audience, at its preview, is the first send's
 * recipients with no human open after @days.
 *
 * Returns: (transfer full) (nullable): the draft
 */
VentureMarketingSend *venture_marketing_service_resend(VentureMarketingService *self, gint64 organization_id,
	gint64 send_id, gint64 days, const gchar *subject, const VentureActor *actor, GError **error);
/**
 * venture_marketing_service_default_resend_days:
 * @self: the service
 * @organization_id: the organization
 *
 * The days a resend to non-openers waits when it names none: the
 * organization's `marketing.resend-days` setting, 3 when unset.
 *
 * Returns: days, 0 to 365
 */
gint64 venture_marketing_service_default_resend_days(VentureMarketingService *self, gint64 organization_id);

G_END_DECLS
#endif
