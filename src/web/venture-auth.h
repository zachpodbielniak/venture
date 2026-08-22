/*
 * venture-auth.h - Authentication and session handling
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Two credentials reach the server: a signed session cookie from a browser,
 * and a bearer token from venturectl or an integration. Both resolve to the
 * same thing -- an identified principal with a role -- so every route below
 * this layer asks one question and gets one answer.
 *
 * Session cookies are signed rather than stored. The cookie carries the user
 * id, an expiry and an HMAC over both, keyed by a server secret, so a
 * tampered cookie fails verification and a restart with a persistent secret
 * keeps sessions alive without a session table.
 */

#ifndef VENTURE_AUTH_H
#define VENTURE_AUTH_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>
#include <htmx-glib.h>

G_BEGIN_DECLS

#define VENTURE_TYPE_AUTH (venture_auth_get_type())

G_DECLARE_FINAL_TYPE(VentureAuth, venture_auth, VENTURE, AUTH, GObject)

/**
 * VentureAuthPrincipal:
 * @user_id: the authenticated user, or 0 for a token with no user
 * @token_id: the API token used, or 0 for a browser session
 * @role: what the principal may do
 * @name: (nullable): a display name for audit records
 * @authenticated: whether any credential was accepted
 *
 * Who is making a request.
 */
typedef struct
{
	gint64		 user_id;
	gint64		 token_id;
	VentureUserRole	 role;
	gchar		*name;
	gboolean	 authenticated;
} VentureAuthPrincipal;

/**
 * venture_auth_new:
 * @context: the wiring
 *
 * Creates the authenticator, resolving the session signing secret from the
 * environment variable named in configuration. When that variable is unset a
 * random secret is generated, which is secure but means sessions do not
 * survive a restart -- a warning says so.
 *
 * Returns: (transfer full): a new #VentureAuth
 */
VentureAuth *
venture_auth_new(VentureContext *context);

/**
 * venture_auth_principal_free:
 * @principal: (nullable): a principal
 *
 * Frees a principal.
 */
void
venture_auth_principal_free(VentureAuthPrincipal *principal);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(VentureAuthPrincipal, venture_auth_principal_free)

/**
 * venture_auth_authenticate:
 * @self: a #VentureAuth
 * @request: the incoming request
 *
 * Resolves the request's credentials. A request with none, or with invalid
 * ones, yields an unauthenticated principal rather than %NULL, so callers
 * always have something to inspect.
 *
 * When authentication is disabled in configuration the principal is returned
 * authenticated with the owner role -- which is why that setting is refused
 * on a non-loopback address.
 *
 * Returns: (transfer full): the principal
 */
VentureAuthPrincipal *
venture_auth_authenticate(
	VentureAuth	*self,
	HtmxRequest	*request
);

/**
 * venture_auth_login:
 * @self: a #VentureAuth
 * @username: the username or email
 * @password: the password
 * @remote_address: (nullable): the peer address the attempt came from
 * @out_cookie: (out) (transfer full): the session cookie to set
 * @error: (out) (optional): return location for a #GError
 *
 * Verifies a password and mints a session cookie.
 *
 * Failures are deliberately indistinguishable: an unknown user, a wrong
 * password and a deactivated account all produce the same message, so the
 * response cannot be used to enumerate accounts.
 *
 * Attempts are rate limited per @remote_address according to
 * security.login_rate_limit, and refused outright once the budget for the
 * minute is spent -- a password check that still runs is a guess that still
 * counts. %NULL skips the limit and is for in-process callers only; a route
 * handler must pass the real peer, which venture_auth_remote_address()
 * extracts.
 *
 * Returns: %TRUE on success
 */
gboolean
venture_auth_login(
	VentureAuth	 *self,
	const gchar	 *username,
	const gchar	 *password,
	const gchar	 *remote_address,
	gchar		**out_cookie,
	GError		**error
);

/**
 * venture_auth_remote_address:
 * @request: an #HtmxRequest
 *
 * Extracts the peer address a request arrived from, for rate limiting.
 *
 * Returns: (transfer full) (nullable): the address, or %NULL if the
 * request has no network peer
 */
gchar *
venture_auth_remote_address(HtmxRequest *request);

/**
 * venture_auth_end_sessions:
 * @self: a #VentureAuth
 * @user_id: the account signing out
 * @error: (out) (optional): return location for a #GError
 *
 * Invalidates every session already issued for @user_id.
 *
 * Session cookies are stateless -- signed, not stored -- so clearing the
 * browser's copy is all a logout can do on its own, and anyone still holding
 * a copy stays signed in until it expires. Recording the instant lets
 * authentication refuse anything older, which is what makes signing out on a
 * shared machine mean something.
 *
 * It ends every session for the account, not just the one that asked. That
 * is the useful behaviour when the reason for signing out is that somebody
 * else may have the cookie.
 *
 * Returns: %TRUE if the cut-off was recorded
 */
gboolean
venture_auth_end_sessions(
	VentureAuth	 *self,
	gint64		  user_id,
	GError		**error
);

/**
 * venture_auth_logout_cookie:
 * @self: a #VentureAuth
 *
 * Returns: (transfer full): a Set-Cookie value that clears the session
 */
gchar *
venture_auth_logout_cookie(VentureAuth *self);

/**
 * venture_auth_require:
 * @self: a #VentureAuth
 * @principal: the principal to check
 * @minimum_role: the least privileged role permitted
 * @error: (out) (optional): return location for a #GError
 *
 * Checks that a principal is authenticated and holds at least @minimum_role.
 *
 * Returns: %TRUE if the request may proceed
 */
gboolean
venture_auth_require(
	VentureAuth		 *self,
	VentureAuthPrincipal	 *principal,
	VentureUserRole		  minimum_role,
	GError			**error
);

/**
 * venture_auth_is_required:
 * @self: a #VentureAuth
 *
 * Returns: %TRUE if authentication is enforced
 */
gboolean
venture_auth_is_required(VentureAuth *self);

/**
 * venture_auth_to_actor:
 * @principal: the principal
 * @actor: (out caller-allocates): filled in with the audit actor
 *
 * Converts a principal into the actor recorded against a mutation.
 */
void
venture_auth_to_actor(
	VentureAuthPrincipal	*principal,
	VentureActor		*actor
);

/**
 * venture_auth_set_password:
 * @self: a #VentureAuth
 * @user: the account to change
 * @password: the new password, in the clear
 * @error: (out) (optional): return location for a #GError
 *
 * Applies the configured password policy and, if it passes, hashes
 * @password into @user with the configured iteration count.
 *
 * The account is not saved: the caller writes it, so that a password change
 * and whatever else it accompanies land in one audited write.
 *
 * Returns: %TRUE if the password was accepted and set
 */
gboolean
venture_auth_set_password(
	VentureAuth	 *self,
	VentureUser	 *user,
	const gchar	 *password,
	GError		**error
);

/**
 * venture_auth_get_password_min_length:
 * @self: a #VentureAuth
 *
 * Returns: the shortest password the policy accepts
 */
guint
venture_auth_get_password_min_length(VentureAuth *self);

/**
 * venture_auth_ensure_owner:
 * @self: a #VentureAuth
 * @password: (nullable): the password to set
 * @error: (out) (optional): return location for a #GError
 *
 * Creates the owner account if no users exist, which is what makes a fresh
 * install reachable. With no password supplied one is generated and printed
 * once, so an unattended first run does not leave an account with no
 * credential at all.
 *
 * Returns: (transfer full) (nullable): the generated password if one was
 *   made, %NULL if an account already existed or a password was supplied
 */
gchar *
venture_auth_ensure_owner(
	VentureAuth	 *self,
	const gchar	 *password,
	GError		**error
);

G_END_DECLS

#endif /* VENTURE_AUTH_H */
