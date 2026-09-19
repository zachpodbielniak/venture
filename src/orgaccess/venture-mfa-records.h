/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_MFA_RECORDS_H
#define VENTURE_MFA_RECORDS_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS

/**
 * VentureUserMfa:
 *
 * One row per account holding the encrypted TOTP secret, whether the second
 * factor is enabled, the counter last accepted (replay refusal) and the
 * failure window. The secret is marked sensitive, so no generated surface
 * ever emits it; only the MFA service reads it, and only to verify a code.
 */
#define VENTURE_TYPE_USER_MFA (venture_user_mfa_get_type())
VENTURE_DECLARE_ENTITY(VentureUserMfa, venture_user_mfa, USER_MFA)

/**
 * VentureMfaRecoveryCode:
 *
 * A single-use recovery code, stored as a password hash plus a short prefix
 * for lookup. The clear text is shown once at enrolment and never again.
 */
#define VENTURE_TYPE_MFA_RECOVERY_CODE (venture_mfa_recovery_code_get_type())
VENTURE_DECLARE_ENTITY(VentureMfaRecoveryCode, venture_mfa_recovery_code, MFA_RECOVERY_CODE)

/**
 * VentureMfaPolicy:
 *
 * The organisation setting =require_mfa_for_admins=: one row per
 * organization. While it is on, an organization owner or admin who has not
 * enrolled is sent to enrolment and every other route refuses them.
 */
#define VENTURE_TYPE_MFA_POLICY (venture_mfa_policy_get_type())
VENTURE_DECLARE_ENTITY(VentureMfaPolicy, venture_mfa_policy, MFA_POLICY)

G_END_DECLS
#endif
