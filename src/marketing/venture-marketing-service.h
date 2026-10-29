/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_MARKETING_SERVICE_H
#define VENTURE_MARKETING_SERVICE_H
G_BEGIN_DECLS
#define VENTURE_TYPE_MARKETING_SERVICE (venture_marketing_service_get_type())
G_DECLARE_FINAL_TYPE(VentureMarketingService, venture_marketing_service, VENTURE, MARKETING_SERVICE, GObject)
/**
 * venture_marketing_service_get:
 * @database: owning database
 * Returns: (transfer none): its single marketing policy service
 */
VentureMarketingService *venture_marketing_service_get(VentureDatabase *database);
/**
 * venture_marketing_service_configure:
 * @self: service
 * @context: runtime context with live public origin and organization mail routing
 */
void venture_marketing_service_configure(VentureMarketingService *self, VentureContext *context);
/**
 * venture_marketing_install:
 * @database: owning database
 *
 * Installs shared save guards, actions and synchronous outbox policy hooks.
 */
void venture_marketing_install(VentureDatabase *database);
/**
 * venture_marketing_check_removal:
 * @entity: proposed delete, restore or purge
 * @error: (out) (optional): retained-evidence refusal
 * Returns: whether removal preserves approved audiences and consent history
 */
gboolean venture_marketing_check_removal(VentureEntity *entity, GError **error);
/**
 * venture_marketing_service_consent:
 * @self: service
 * @organization_id: exact organization
 * @subject: persisted contact, company or lead in that organization
 * @source: how permission was obtained
 * @evidence: recorded permission statement or reference
 * @evidence_key: organization-scoped replay identity
 * @occurred_at: time permission was obtained, no later than now
 * @actor: (nullable): audit actor
 * @error: (out) (optional): permission or validation failure
 *
 * Captures marketing permission for the subject's current normalized address.
 * It never clears suppression or creates transactional-mail permission.
 *
 * Returns: (transfer full) (nullable): retained consent evidence
 */
VentureMarketingConsent *venture_marketing_service_consent(VentureMarketingService *self,
	gint64 organization_id, VentureEntity *subject, const gchar *source, const gchar *evidence,
	const gchar *evidence_key, GDateTime *occurred_at, const VentureActor *actor, GError **error);
/**
 * venture_marketing_service_current_permission:
 * @self: service
 * @organization_id: exact organization
 * @email: recipient address
 * @purpose: must be marketing
 * @state: (out): given, withdrawn or none
 * @recorded_at: (out) (nullable): time of the grant
 * @withdrawn_at: (out) (nullable): time of the withdrawal
 * @error: (out) (optional): scope or query failure
 *
 * Return the newest permission evidence for one address.
 * Keep suppression separate from this evidence state.
 * Returns: whether the read succeeded
 */
gboolean venture_marketing_service_current_permission(VentureMarketingService *self,
	gint64 organization_id, const gchar *email, const gchar *purpose,
	const gchar **state, GDateTime **recorded_at, GDateTime **withdrawn_at, GError **error);
/**
 * venture_marketing_service_preview:
 * @self: service
 * @organization_id: exact organization
 * @send_id: draft send
 * @base_url: public HTTPS origin, or loopback HTTP for development
 * @actor: (nullable): audit actor
 * @error: (out) (optional): scope, content or audience validation failure
 *
 * Freezes eligible and excluded records and rendered content atomically.
 * Refreshing a segment requires another draft; approval never rerenders it.
 *
 * Returns: (transfer full) (nullable): immutable preview with recipient counts
 */
VentureMarketingSend *venture_marketing_service_preview(VentureMarketingService *self,
	gint64 organization_id, gint64 send_id, const gchar *base_url, const VentureActor *actor, GError **error);
/**
 * venture_marketing_service_transition:
 * @self: service
 * @organization_id: exact organization
 * @send_id: retained send
 * @transition: approve, pause, resume or cancel
 * @actor: (nullable): audit actor
 * @error: (out) (optional): authorization, state or persistence error
 * Returns: whether the transition committed
 */
gboolean venture_marketing_service_transition(VentureMarketingService *self, gint64 organization_id,
	gint64 send_id, const gchar *transition, const VentureActor *actor, GError **error);
/**
 * venture_marketing_service_run:
 * @self: service
 * @organization_id: exact organization
 * @send_id: approved send
 * @limit: maximum recipients examined, from one through 100
 * @now: execution clock
 * @actor: (nullable): audit actor
 * @error: (out) (optional): authorization or persistence failure
 *
 * Queues at most one due recipient, retaining its outbox identity. Further
 * execution waits for its known outcome and the configured interval. SMTP
 * delivery uses the existing outbox; uncertainty requires operator review.
 *
 * Returns: newly queued count, or minus one on failure
 */
gint venture_marketing_service_run(VentureMarketingService *self, gint64 organization_id,
	gint64 send_id, guint limit, GDateTime *now, const VentureActor *actor, GError **error);
/**
 * venture_marketing_service_unsubscribe:
 * @self: service
 * @token: opaque unsubscribe capability from the delivered message
 * @error: (out) (optional): unknown capability or persistence failure
 *
 * Idempotently withdraws marketing consent and suppresses the retained
 * organization/address. No login or caller-selected organization is used.
 *
 * Returns: whether withdrawal is retained
 */
gboolean venture_marketing_service_unsubscribe(VentureMarketingService *self, const gchar *token, GError **error);
/**
 * venture_marketing_service_record_open:
 * @self: service
 * @token: opaque tracking token
 * @user_agent: (nullable): the request's User-Agent; judged, never stored
 * @now: observation time
 * @error: (out) (optional): unknown token or persistence failure
 *
 * An open venture_marketing_machine_reason() judges a machine's is kept
 * as evidence with its reason and counted apart, never as an open.
 *
 * Returns: whether the first open and bounded daily evidence were retained
 */
gboolean venture_marketing_service_record_open(VentureMarketingService *self, const gchar *token,
	const gchar *user_agent, GDateTime *now, GError **error);
/**
 * venture_marketing_service_record_click:
 * @self: service
 * @token: opaque tracking token
 * @position: frozen destination number, starting at one
 * @user_agent: (nullable): the request's User-Agent; judged, never stored
 * @now: observation time
 * @error: (out) (optional): unknown token/link or persistence failure
 * Returns: (transfer full) (nullable): original web destination
 */
gchar *venture_marketing_service_record_click(VentureMarketingService *self, const gchar *token,
	gint64 position, const gchar *user_agent, GDateTime *now, GError **error);
/**
 * venture_marketing_service_set_machine_seconds:
 * @self: service
 * @seconds: an open or click this soon after SMTP acceptance is judged a
 *   machine's; 0 turns the timing rule off (default 10)
 */
void venture_marketing_service_set_machine_seconds(VentureMarketingService *self, gint64 seconds);
/**
 * venture_marketing_service_feedback:
 * @self: service
 * @organization_id: exact organization
 * @recipient_id: recipient with an outbox attempt
 * @kind: hard_bounce, temporary_bounce, complaint, or delivered (a provider's
 *   word that the receiving server accepted it; no suppression)
 * @source: evidence source and operator explanation
 * @event_key: organization-scoped input replay identity
 * @occurred_at: evidence time, no later than now
 * @actor: (nullable): audit actor
 * @error: (out) (optional): invalid evidence or persistence failure
 *
 * Explicit evidence is required; SMTP failure alone is not a hard bounce.
 * Only hard bounce and complaint create retained address suppression.
 *
 * Returns: whether the evidence and applicable suppression committed
 */
gboolean venture_marketing_service_feedback(VentureMarketingService *self, gint64 organization_id,
	gint64 recipient_id, const gchar *kind, const gchar *source, const gchar *event_key,
	GDateTime *occurred_at, const VentureActor *actor, GError **error);
/**
 * venture_marketing_preview_register:
 *
 * Registers the campaign mail preview kind: one frozen recipient's message.
 */
void venture_marketing_preview_register(void);
/**
 * venture_marketing_register_reports:
 * @registry: report registry
 *
 * Registers send-cohort SMTP acceptance and observed-engagement reporting.
 */
void venture_marketing_register_reports(VentureReportRegistry *registry);
/**
 * venture_marketing_actions_register:
 * @database: owning action registry database
 *
 * Adds typed, organization-authorized marketing actions.
 */
void venture_marketing_actions_register(VentureDatabase *database);
G_END_DECLS
#endif
