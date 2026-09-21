/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_ATTRIBUTION_SERVICE_H
#define VENTURE_ATTRIBUTION_SERVICE_H
G_BEGIN_DECLS
#define VENTURE_TYPE_ATTRIBUTION_SERVICE (venture_attribution_service_get_type())
G_DECLARE_FINAL_TYPE(VentureAttributionService, venture_attribution_service, VENTURE, ATTRIBUTION_SERVICE, GObject)
/**
 * venture_attribution_service_get:
 * @database: owning repository
 * Returns: (transfer none): its attribution service
 */
VentureAttributionService *venture_attribution_service_get(VentureDatabase *database);
/**
 * venture_attribution_install:
 * @database: owning repository
 *
 * Installs save guards and capture/conversion invariants on every writer.
 */
void venture_attribution_install(VentureDatabase *database);
/**
 * venture_attribution_check_removal:
 * @entity: proposed deletion, restoration or purge
 * @error: (out) (optional): retained evidence refusal
 * Returns: whether ordinary removal preserves attribution history
 */
gboolean venture_attribution_check_removal(VentureEntity *entity, GError **error);
/**
 * venture_attribution_service_grant:
 * @self: service
 * @site_uuid: configured public site identity
 * @origin: exact browser origin
 * @policy: explicitly accepted current analytics policy version
 * @now: trusted server receipt clock
 * @error: (out) (optional): site, consent or quota refusal
 *
 * Creates anonymous analytics permission independently of email permission.
 * The returned capability is write-only presentation data, never an audit field.
 *
 * Returns: (transfer full) (nullable): token and expiration object
 */
JsonNode *venture_attribution_service_grant(VentureAttributionService *self,
	const gchar *site_uuid, const gchar *origin, const gchar *policy, GDateTime *now, GError **error);
/**
 * venture_attribution_service_observe:
 * @self: service
 * @site_uuid: configured public site identity
 * @origin: exact browser origin
 * @token: anonymous analytics capability
 * @event_id: caller's stable observation identity
 * @fields: bounded page, referrer and UTM strings
 * @now: trusted server receipt clock
 * @error: (out) (optional): consent, payload, replay or quota refusal
 * Returns: (transfer full) (nullable): retained observation, including exact replay
 */
VentureAttributionTouch *venture_attribution_service_observe(VentureAttributionService *self,
	const gchar *site_uuid, const gchar *origin, const gchar *token, const gchar *event_id,
	JsonObject *fields, GDateTime *now, GError **error);
/**
 * venture_attribution_service_withdraw:
 * @self: service
 * @site_uuid: exact site identity
 * @token: visitor capability
 * @now: trusted server receipt clock
 * @error: (out) (optional): capability or storage refusal
 *
 * Stops analytics and removes visitor linkage and private navigation details.
 * Coarse acquisition snapshots and independent CRM/email evidence remain.
 * Withdrawal must own its transaction: an existing transaction is refused with
 * %G_IO_ERROR_BUSY before mutation. Retry independently after that transaction
 * finishes; a nested commit cannot promise durable revocation.
 *
 * Returns: whether withdrawal committed, including replay
 */
gboolean venture_attribution_service_withdraw(VentureAttributionService *self,
	const gchar *site_uuid, const gchar *token, GDateTime *now, GError **error);
/**
 * venture_attribution_service_capture:
 * @self: service
 * @site_uuid: configured site identity
 * @origin: exact browser origin
 * @payload: version-one form envelope with stable submission identity
 * @now: trusted server receipt clock
 * @error: (out) (optional): scope, replay, consent or capture failure
 *
 * Reuses lead capture and records independent analytics/email permissions.
 * A honeypot returns success with no submission or CRM record.
 *
 * Returns: (transfer full) (nullable): accepted submission; NULL without error for a honeypot
 */
VentureAttributionSubmission *venture_attribution_service_capture(VentureAttributionService *self,
	const gchar *site_uuid, const gchar *origin, JsonObject *payload, GDateTime *now, GError **error);
/**
 * venture_attribution_service_receive:
 * @self: service
 * @site_uuid: configured site identity
 * @connection_id: exact Lightsite account binding
 * @timestamp: signed decimal Unix timestamp
 * @signature: hexadecimal HMAC-SHA256 of timestamp, newline and exact body
 * @body: bounded version-one Lightsite submission bytes
 * @now: trusted server receipt clock
 * @error: (out) (optional): authentication or capture refusal
 * Returns: (transfer full) (nullable): accepted submission, or NULL for refused/bot input
 */
VentureAttributionSubmission *venture_attribution_service_receive(VentureAttributionService *self,
	const gchar *site_uuid, gint64 connection_id, const gchar *timestamp,
	const gchar *signature, GBytes *body, GDateTime *now, GError **error);
/**
 * venture_attribution_service_sweep:
 * @self: service
 * @organization_id: exact authorized organization
 * @limit: maximum visitors to redact, one through 100
 * @now: sweep clock
 * @actor: (nullable): audit actor
 * @error: (out) (optional): authorization or storage failure
 * Returns: number redacted, or minus one on failure
 */
gint venture_attribution_service_sweep(VentureAttributionService *self, gint64 organization_id,
	guint limit, GDateTime *now, const VentureActor *actor, GError **error);
/**
 * venture_attribution_settings_schema:
 * Returns: (transfer full): schema for write-only Lightsite credentials
 */
JsonNode *venture_attribution_settings_schema(void);
/**
 * venture_attribution_settings_configure:
 * @database: repository
 * @organization_id: exact organization administered by current principal
 * @settings: tenant, environment and write-only shared signing secret
 * @expected_version: zero to create, current version to rotate
 * @expected_connection: zero to create, exact existing connection to rotate
 * @actor: (nullable): audit actor
 * @error: (out) (optional): redacted validation or authorization refusal
 * Returns: (transfer full) (nullable): encrypted account binding
 */
VentureIntegrationConnection *venture_attribution_settings_configure(VentureDatabase *database,
	gint64 organization_id, JsonNode *settings, gint64 expected_version, gint64 expected_connection,
	const VentureActor *actor, GError **error);
/**
 * venture_attribution_actions_register:
 * @database: owning repository
 *
 * Registers bounded retention actions on metadata-owned records.
 */
void venture_attribution_actions_register(VentureDatabase *database);
/**
 * venture_attribution_register_reports:
 * @registry: report registry
 *
 * Registers source-linked first/last-touch attribution.
 */
void venture_attribution_register_reports(VentureReportRegistry *registry);
G_END_DECLS
#endif
