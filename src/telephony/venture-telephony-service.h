/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * venture-telephony-service.h - Phone calls and text messages
 *
 * One service per database owns every telephony write: connecting a
 * provider account (credentials in the integration vault), click-to-call,
 * logging calls through the existing log_call, matching numbers to
 * contacts, the inbound webhook, and the one send path for texts, which
 * checks text consent and quiet hours immediately before a provider is
 * asked. Sequences, journeys and campaigns only queue texts.
 */
#ifndef VENTURE_TELEPHONY_SERVICE_H
#define VENTURE_TELEPHONY_SERVICE_H
G_BEGIN_DECLS

/**
 * VENTURE_TELEPHONY_TEXT_MAX:
 *
 * The longest text, in characters.
 */
#define VENTURE_TELEPHONY_TEXT_MAX 1600
/**
 * VENTURE_TELEPHONY_QUIET_HOURS:
 *
 * The quiet hours when an organization sets none, in the recipient's
 * local time: no texts from 21:00 until 08:00.
 */
#define VENTURE_TELEPHONY_QUIET_HOURS "21:00-08:00"
/**
 * VENTURE_TELEPHONY_WEBHOOK_MAX_BODY:
 *
 * The largest webhook body accepted, in bytes.
 */
#define VENTURE_TELEPHONY_WEBHOOK_MAX_BODY 65536

#define VENTURE_TYPE_TELEPHONY_SERVICE (venture_telephony_service_get_type())
G_DECLARE_FINAL_TYPE(VentureTelephonyService, venture_telephony_service, VENTURE, TELEPHONY_SERVICE, GObject)

/**
 * venture_telephony_service_get:
 * @database: repository, weakly held
 * Returns: (transfer none): the repository's telephony service
 */
VentureTelephonyService *venture_telephony_service_get(VentureDatabase *database);
/**
 * venture_telephony_install:
 * @context: the wiring
 *
 * Installs the write guards, the sequence text channel and the campaign
 * text channel.
 */
void venture_telephony_install(VentureContext *context);
/**
 * venture_telephony_service_connect:
 * @self: the service
 * @organization_id: the organization
 * @provider: a registered provider id
 * @settings: name, base_url, phone_number, webhook_sources, media_hosts and
 *   the provider's credential fields
 * @account_id: zero for a new line, or the line whose settings change
 * @actor: (nullable): audit actor
 * @webhook_token: (out) (optional) (transfer full): a new line's webhook
 *   address token, shown once
 * @error: (out) (optional): refusal in plain words
 *
 * Requires an organization owner or administrator. Credentials go to the
 * integration vault, encrypted; nothing secret is stored on the line.
 *
 * Returns: (transfer full) (nullable): the line
 */
VentureEntity *venture_telephony_service_connect(VentureTelephonyService *self, gint64 organization_id,
	const gchar *provider, JsonObject *settings, gint64 account_id, const VentureActor *actor,
	gchar **webhook_token, GError **error);
/**
 * venture_telephony_service_new_webhook_token:
 * @self: the service
 * @account: a line
 * @actor: (nullable): audit actor
 * @error: (out) (optional): refusal
 *
 * The old address stops working at once.
 *
 * Returns: (transfer full) (nullable): the new token, shown once
 */
gchar *venture_telephony_service_new_webhook_token(VentureTelephonyService *self, VentureEntity *account,
	const VentureActor *actor, GError **error);
/**
 * venture_telephony_service_disconnect:
 * @self: the service
 * @account: a line
 * @actor: (nullable): audit actor
 * @error: (out) (optional): refusal
 * Returns: whether the line and its credentials were switched off
 */
gboolean venture_telephony_service_disconnect(VentureTelephonyService *self, VentureEntity *account,
	const VentureActor *actor, GError **error);
/**
 * venture_telephony_service_line:
 * @self: the service
 * @organization_id: the organization
 * @needs: capabilities the line must have
 * @error: (out) (optional): none is connected, in plain words
 * Returns: (transfer full) (nullable): the organization's first active line with @needs
 */
VentureEntity *venture_telephony_service_line(VentureTelephonyService *self, gint64 organization_id,
	VentureTelephonyCapabilities needs, GError **error);
/**
 * venture_telephony_service_extension:
 * @self: the service
 * @organization_id: the organization
 * @user_id: a person
 * Returns: (transfer full) (nullable): their extension there
 */
gchar *venture_telephony_service_extension(VentureTelephonyService *self, gint64 organization_id, gint64 user_id);
/**
 * venture_telephony_service_click_to_call:
 * @self: the service
 * @subject: a contact, deal or ticket
 * @user_id: the person calling
 * @number: (nullable): the number to dial; %NULL is the contact's phone
 * @actor: (nullable): audit actor
 * @error: (out) (optional): refusal in plain words
 *
 * Rings the person's own extension first; when they pick up, the phone
 * system dials the contact. The call is logged when the phone system
 * reports it ended.
 *
 * Returns: (transfer full) (nullable): the phone system's call id
 */
gchar *venture_telephony_service_click_to_call(VentureTelephonyService *self, VentureEntity *subject,
	gint64 user_id, const gchar *number, const VentureActor *actor, GError **error);
/**
 * venture_telephony_service_match_contact:
 * @self: the service
 * @organization_id: the organization
 * @number: a number as the provider wrote it
 * @error: (out) (optional): storage failure
 * Returns: (transfer full) (nullable): the contact whose phone is the same
 *   number once both are normalised, the oldest when several are
 */
VentureEntity *venture_telephony_service_match_contact(VentureTelephonyService *self, gint64 organization_id,
	const gchar *number, GError **error);
/**
 * venture_telephony_service_receive:
 * @self: the service
 * @token: the address token from the webhook path
 * @hook: the request
 * @now: the clock
 * @reply: (out) (optional) (transfer full): the body the provider expects
 * @error: (out) (optional): unknown token (not found), refused source or
 *   signature (permission denied), or an unreadable request
 *
 * Verifies, reads and applies a webhook. Calls are logged through
 * log_call with the provider's call id as replay identity; texts and
 * status updates are keyed by the provider's message id, so the same
 * request twice changes nothing the second time.
 *
 * Returns: (transfer full) (nullable): counts of calls, messages,
 *   statuses and duplicates
 */
JsonNode *venture_telephony_service_receive(VentureTelephonyService *self, const gchar *token,
	const VentureTelephonyWebhook *hook, GDateTime *now, gchar **reply, GError **error);
/**
 * venture_telephony_service_queue_text:
 * @self: the service
 * @contact: the recipient
 * @body: the words
 * @media: (nullable) (array zero-terminated=1): public picture addresses
 * @origin: manual, sequence:..., campaign:...
 * @now: the clock
 * @actor: (nullable): audit actor
 * @error: (out) (optional): no consent, no number, no line, in plain words
 *
 * Queues one text, or holds it for quiet hours. Never talks to a provider,
 * so it may run inside another transaction.
 *
 * Returns: (transfer full) (nullable): the queued or held message
 */
VentureEntity *venture_telephony_service_queue_text(VentureTelephonyService *self, VentureEntity *contact,
	const gchar *body, const gchar *const *media, const gchar *origin, GDateTime *now,
	const VentureActor *actor, GError **error);
/**
 * venture_telephony_service_send_text:
 * @self: the service
 * @contact: the recipient
 * @body: the words
 * @hosted_image_id: zero, or a hosted image to send as a picture
 * @now: the clock
 * @actor: (nullable): audit actor
 * @error: (out) (optional): refusal in plain words
 *
 * Queues and, unless quiet hours hold it, sends at once.
 *
 * Returns: (transfer full) (nullable): the message in its resulting state
 */
VentureEntity *venture_telephony_service_send_text(VentureTelephonyService *self, VentureEntity *contact,
	const gchar *body, gint64 hosted_image_id, GDateTime *now, const VentureActor *actor, GError **error);
/**
 * venture_telephony_service_deliver_due:
 * @self: the service
 * @organization_id: the organization
 * @now: the clock
 * @limit: the most messages sent, 1 to 100
 * @actor: (nullable): audit actor
 * @error: (out) (optional): storage failure
 *
 * The send path: each due message's consent and quiet hours are checked
 * again, then it is handed to its provider once.
 *
 * Returns: messages handed to a provider, or -1
 */
gint venture_telephony_service_deliver_due(VentureTelephonyService *self, gint64 organization_id,
	GDateTime *now, guint limit, const VentureActor *actor, GError **error);
/**
 * venture_telephony_service_quiet_until:
 * @self: the service
 * @contact: the recipient
 * @now: the clock
 * @error: (out) (optional): storage failure
 * Returns: (transfer full) (nullable): when quiet hours end in the
 *   contact's time zone, or %NULL when a text may go now
 */
GDateTime *venture_telephony_service_quiet_until(VentureTelephonyService *self, VentureEntity *contact,
	GDateTime *now, GError **error);
/**
 * venture_telephony_service_keyword_reply:
 * @self: the service
 * @organization_id: the organization
 * @keyword: `stop`, `start` or `help`
 *
 * The answer to a text keyword: the organization's words for it (the
 * `telephony.*-reply` settings) or the default, with `{business}`
 * replaced by the organization's name.
 *
 * Returns: (transfer full) (nullable): the answer, %NULL for another word
 */
gchar *venture_telephony_service_keyword_reply(VentureTelephonyService *self, gint64 organization_id,
	const gchar *keyword);
/**
 * venture_telephony_service_record_consent:
 * @self: the service
 * @contact: the contact
 * @granted: agreed (TRUE) or withdrew (FALSE)
 * @evidence: how they said so, in words
 * @key: (nullable): replay identity; %NULL makes a fresh one
 * @at: when
 * @actor: (nullable): audit actor
 * @error: (out) (optional): no phone number, or storage failure
 * Returns: whether the evidence was recorded
 */
gboolean venture_telephony_service_record_consent(VentureTelephonyService *self, VentureEntity *contact,
	gboolean granted, const gchar *evidence, const gchar *key, GDateTime *at, const VentureActor *actor, GError **error);
/**
 * venture_telephony_service_has_consent:
 * @self: the service
 * @contact: the contact
 * @error: (out) (optional): storage failure
 * Returns: whether their latest text consent evidence is an agreement
 */
gboolean venture_telephony_service_has_consent(VentureTelephonyService *self, VentureEntity *contact, GError **error);
/**
 * venture_telephony_service_thread:
 * @self: the service
 * @contact: the contact
 * @limit: the most messages
 * @error: (out) (optional): storage failure
 * Returns: (transfer full) (element-type VentureEntity) (nullable): their texts, oldest first
 */
GPtrArray *venture_telephony_service_thread(VentureTelephonyService *self, VentureEntity *contact,
	guint limit, GError **error);
G_END_DECLS
#endif
