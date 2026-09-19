/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_MFA_SERVICE_H
#define VENTURE_MFA_SERVICE_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS

#define VENTURE_TYPE_MFA_SERVICE (venture_mfa_service_get_type())
G_DECLARE_FINAL_TYPE(VentureMfaService, venture_mfa_service, VENTURE, MFA_SERVICE, GObject)

/**
 * VENTURE_MFA_FAILURE_LIMIT:
 *
 * Wrong codes tolerated per account inside one window before verification
 * refuses even a correct code.
 */
#define VENTURE_MFA_FAILURE_LIMIT 5

/**
 * VENTURE_MFA_FAILURE_WINDOW_SECONDS:
 *
 * The window the failure limit is counted over: fifteen minutes.
 */
#define VENTURE_MFA_FAILURE_WINDOW_SECONDS (15 * 60)

/**
 * VENTURE_MFA_RECOVERY_CODE_COUNT:
 *
 * Recovery codes issued at enrolment and on regeneration.
 */
#define VENTURE_MFA_RECOVERY_CODE_COUNT 10

/**
 * VentureMfaEnrolment:
 * @secret: the shared secret in base32, for typing into an app by hand
 * @uri: the otpauth:// URI the QR code carries
 * @svg: the QR code as an SVG document
 *
 * What a person needs to add the account to an authenticator app. Shown on
 * the enrolment page and never stored in the clear.
 */
typedef struct
{
	gchar	*secret;
	gchar	*uri;
	gchar	*svg;
} VentureMfaEnrolment;

#define VENTURE_TYPE_MFA_ENROLMENT (venture_mfa_enrolment_get_type())
GType venture_mfa_enrolment_get_type(void) G_GNUC_CONST;
VentureMfaEnrolment *venture_mfa_enrolment_copy(const VentureMfaEnrolment *self);
void venture_mfa_enrolment_free(VentureMfaEnrolment *self);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(VentureMfaEnrolment, venture_mfa_enrolment_free)

/**
 * venture_mfa_service_get:
 * @database: database owning the records
 *
 * Returns the per-database second-factor service, installing on first use
 * the save validators that keep =user_mfa= and =mfa_recovery_code= rows
 * writable only through this service and =mfa_policy= to one row per
 * organization.
 *
 * Returns: (transfer none): the service; the database owns it
 */
VentureMfaService *venture_mfa_service_get(VentureDatabase *database);

/**
 * venture_mfa_service_configure:
 * @self: the service
 * @config: the configuration
 *
 * Resolves the key that encrypts stored secrets: the environment variable
 * named by =security.mfa_key_env=, falling back to the one named by
 * =security.session_secret_env=. The same shape as the mail and Stripe
 * modules: configuration names a variable, never holds a value. Without
 * either, enrolment is refused with %VENTURE_ERROR_CONFIG.
 */
void venture_mfa_service_configure(VentureMfaService *self, VentureConfig *config);

/**
 * venture_mfa_service_now:
 * @self: the service
 *
 * The service's clock: the =fixed-time= property when a test set one,
 * otherwise venture_time_now().
 *
 * Returns: (transfer full): the current instant
 */
GDateTime *venture_mfa_service_now(VentureMfaService *self);

/**
 * venture_mfa_service_is_enabled:
 * @self: the service
 * @user_id: an account
 *
 * Returns: %TRUE when @user_id has a confirmed second factor and the
 *   module is on
 */
gboolean venture_mfa_service_is_enabled(VentureMfaService *self, gint64 user_id);

/**
 * venture_mfa_service_get_row:
 * @self: the service
 * @user_id: an account
 *
 * Returns: (transfer full) (nullable): the account's =user_mfa= row, enabled
 *   or pending, or %NULL
 */
VentureUserMfa *venture_mfa_service_get_row(VentureMfaService *self, gint64 user_id);

/**
 * venture_mfa_service_begin_enrolment:
 * @self: the service
 * @user_id: the account enrolling
 * @issuer: (nullable): the install's name for the authenticator app
 * @account: (nullable): the username for the authenticator app
 * @actor: (nullable): audit attribution
 * @error: (out) (optional): refusal
 *
 * Generates a fresh secret, stores it encrypted with =enabled= false and
 * returns the URI and QR code. A pending enrolment replaces any earlier
 * pending one; an enabled second factor must be disabled first.
 *
 * Returns: (transfer full) (nullable): what to show the person, or %NULL
 */
VentureMfaEnrolment *venture_mfa_service_begin_enrolment(VentureMfaService *self, gint64 user_id,
	const gchar *issuer, const gchar *account, const VentureActor *actor, GError **error);

/**
 * venture_mfa_service_pending_enrolment:
 * @self: the service
 * @user_id: the account enrolling
 * @issuer: (nullable): the install's name for the authenticator app
 * @account: (nullable): the username for the authenticator app
 * @error: (out) (optional): refusal
 *
 * Re-renders the QR code for a pending enrolment, so the page can be
 * refreshed without minting a second secret.
 *
 * Returns: (transfer full) (nullable): the pending enrolment, or %NULL
 */
VentureMfaEnrolment *venture_mfa_service_pending_enrolment(VentureMfaService *self, gint64 user_id,
	const gchar *issuer, const gchar *account, GError **error);

/**
 * venture_mfa_service_confirm_enrolment:
 * @self: the service
 * @user_id: the account enrolling
 * @code: a code from the app
 * @actor: (nullable): audit attribution
 * @error: (out) (optional): refusal
 *
 * Enables the second factor once @code verifies against the pending
 * secret, issues the recovery codes and records an =mfa_enrolled= audit
 * row, all in one transaction.
 *
 * Returns: (transfer full) (nullable): the ten recovery codes in the clear,
 *   shown once and never again, or %NULL when @code did not verify
 */
GStrv venture_mfa_service_confirm_enrolment(VentureMfaService *self, gint64 user_id,
	const gchar *code, const VentureActor *actor, GError **error);

/**
 * venture_mfa_service_verify:
 * @self: the service
 * @user_id: the account signing in
 * @code: a six-digit code or a recovery code
 * @actor: (nullable): audit attribution
 * @remote_address: (nullable): the peer, recorded on the audit row
 * @error: (out) (optional): why the code was refused
 *
 * RFC 6238 verification with one step of drift either side, refusing a
 * step already accepted, then a single-use recovery code. Five failures in
 * fifteen minutes lock the account's second factor for the rest of the
 * window. Every outcome is audited.
 *
 * Returns: %TRUE when the code was accepted
 */
gboolean venture_mfa_service_verify(VentureMfaService *self, gint64 user_id, const gchar *code,
	const VentureActor *actor, const gchar *remote_address, GError **error);

/**
 * venture_mfa_service_disable:
 * @self: the service
 * @user_id: the account
 * @actor: (nullable): audit attribution
 * @error: (out) (optional): refusal
 *
 * Turns the second factor off, discards the secret and spends every
 * unused recovery code. Audited as =mfa_disabled=.
 *
 * Returns: %TRUE on success
 */
gboolean venture_mfa_service_disable(VentureMfaService *self, gint64 user_id, const VentureActor *actor, GError **error);

/**
 * venture_mfa_service_reset:
 * @self: the service
 * @user_id: the account an owner is rescuing
 * @actor: (nullable): audit attribution
 * @error: (out) (optional): refusal
 *
 * The break-glass path: the same effect as venture_mfa_service_disable(),
 * audited as =mfa_reset= by whoever ran it. Reached from
 * =venturectl user mfa reset= and =POST /api/v1/users/:id/actions/mfa_reset=.
 *
 * Returns: %TRUE on success
 */
gboolean venture_mfa_service_reset(VentureMfaService *self, gint64 user_id, const VentureActor *actor, GError **error);

/**
 * venture_mfa_service_regenerate_recovery_codes:
 * @self: the service
 * @user_id: the account
 * @actor: (nullable): audit attribution
 * @error: (out) (optional): refusal
 *
 * Spends the unused recovery codes and issues ten new ones.
 *
 * Returns: (transfer full) (nullable): the new codes in the clear
 */
GStrv venture_mfa_service_regenerate_recovery_codes(VentureMfaService *self, gint64 user_id,
	const VentureActor *actor, GError **error);

/**
 * venture_mfa_service_requires_enrolment:
 * @self: the service
 * @principal: the request principal
 *
 * Whether @principal must enrol before doing anything else: an account
 * without a confirmed second factor that is an organization owner or
 * admin in an organization whose =mfa_policy= requires one, or a global
 * owner or admin while any organization requires one.
 *
 * Returns: %TRUE when every route but enrolment must refuse the principal
 */
gboolean venture_mfa_service_requires_enrolment(VentureMfaService *self, const VentureAuthPrincipal *principal);

/**
 * venture_mfa_actions_register:
 * @database: database owning the action registry
 *
 * Registers the owner-only =mfa_reset= action on =user=, which is the REST
 * endpoint, the CLI verb and the assistant tool for break-glass resets.
 */
void venture_mfa_actions_register(VentureDatabase *database);

/**
 * venture_mfa_web_gate:
 * @context: the wiring
 * @http: the request
 * @principal: (nullable): the authenticated principal
 *
 * The enrolment gate: when @principal must enrol, sets a 302 to the
 * enrolment page on a browser path or a 403 on an API path, leaving the
 * enrolment, sign-out and account-password routes alone.
 *
 * Returns: %TRUE when a response was set and the pipeline must stop
 */
gboolean venture_mfa_web_gate(VentureContext *context, HtmxContext *http, const VentureAuthPrincipal *principal);

G_END_DECLS
#endif
