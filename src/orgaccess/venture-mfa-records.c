/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

/*
 * A second factor is personal: the user-id carries the personal-owner flag,
 * so the repository's boundary lets only that account (and a global
 * administrator) read the row through generic surfaces. The secret and
 * every hash are sensitive: omitted from JSON, REST, CSV, the audit diff
 * and anything the assistant can see.
 */
static const VentureFieldDecl user_mfa_fields[] = {
	VENTURE_FIELD_REF("user-id", "User", NULL, "user",
	                  VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED |
	                  VENTURE_COLUMN_FLAG_UNIQUE | VENTURE_COLUMN_FLAG_PERSONAL_OWNER),
	/* The TOTP secret, encrypted with the key named by
	 * security.mfa_key_env; never the clear value. */
	VENTURE_FIELD("secret-ref", "Secret", "Encrypted TOTP secret; only the MFA service reads it",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_SENSITIVE),
	VENTURE_FIELD("enabled", "Enabled", "Set only after one valid code was verified",
	              VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("enrolled-at", "Enrolled", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	/* The last accepted time step; a code for the same step is refused,
	 * which is what makes an observed code useless a second time. */
	VENTURE_FIELD("last-used-counter", "Last used step", "Highest RFC 6238 step accepted",
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("last-used-at", "Last used", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("failure-count", "Failures", "Wrong codes in the current window",
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("failure-window-started-at", "Failure window", NULL,
	              VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VentureUserMfa, venture_user_mfa, user_mfa_fields)

static const VentureFieldDecl mfa_recovery_code_fields[] = {
	VENTURE_FIELD_REF("user-id", "User", NULL, "user",
	                  VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED |
	                  VENTURE_COLUMN_FLAG_PERSONAL_OWNER),
	/* The same shape as an API token: a short prefix in the clear to find
	 * the candidate rows, a password hash to prove the rest. */
	VENTURE_FIELD("prefix", "Prefix", "First characters, for lookup",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("code-hash", "Code", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_SENSITIVE),
	VENTURE_FIELD("used-at", "Used", "Set when spent; a spent code never works again",
	              VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_INDEXED)
};
VENTURE_DEFINE_ENTITY(VentureMfaRecoveryCode, venture_mfa_recovery_code, mfa_recovery_code_fields)

static const VentureFieldDecl mfa_policy_fields[] = {
	VENTURE_FIELD("require-mfa-for-admins", "Require MFA for owners and admins",
	              "Organization owners and admins must enrol before anything else",
	              VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_TEXT("notes", "Notes", NULL)
};
VENTURE_DEFINE_ENTITY(VentureMfaPolicy, venture_mfa_policy, mfa_policy_fields)
