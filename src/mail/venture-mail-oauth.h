/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * venture-mail-oauth.h - Token sign-in for a mailbox
 *
 * A generic OAuth 2.0 authorization-code flow with PKCE (RFC 7636, S256)
 * against a provider the operator configures as a mail_oauth_provider
 * record; nothing vendor-specific ships. The client id and secret are an
 * encrypted integration credential of the provider. The tokens a sign-in
 * returns are the mailbox's own connector credential, beside where a
 * password would be, so rotation, disconnection, private ownership and the
 * in-flight revocation checks are the connector's, unchanged. The IMAP
 * session then authenticates with SASL XOAUTH2.
 *
 * Everything here runs on the main thread and blocks on the token
 * endpoint for at most %VENTURE_MAIL_OAUTH_TIMEOUT_SECONDS.
 */
#ifndef VENTURE_MAIL_OAUTH_H
#define VENTURE_MAIL_OAUTH_H
G_BEGIN_DECLS

/**
 * VENTURE_MAIL_OAUTH_TIMEOUT_SECONDS:
 *
 * How long one exchange with a token endpoint may take.
 */
#define VENTURE_MAIL_OAUTH_TIMEOUT_SECONDS 15
/**
 * VENTURE_MAIL_OAUTH_STATE_SECONDS:
 *
 * How long a started sign-in waits for the provider to send the browser back.
 */
#define VENTURE_MAIL_OAUTH_STATE_SECONDS 600
/**
 * VENTURE_MAIL_OAUTH_REFRESH_MARGIN_SECONDS:
 *
 * An access token this close to expiry is refreshed before a sync uses it.
 */
#define VENTURE_MAIL_OAUTH_REFRESH_MARGIN_SECONDS 120

/**
 * venture_mail_oauth_configure_client:
 * @database: repository
 * @config: operator policy (loopback endpoints are allowed only with
 *   connectors.allow_plaintext_loopback)
 * @provider: a saved mail_oauth_provider
 * @client_id: the client id the provider issued
 * @client_secret: (nullable): its secret; empty for a public client
 * @actor: (nullable): audit attribution
 * @error: (out) (optional): refusal
 *
 * Validates the provider's endpoints and stores the client credential
 * encrypted. Requires organization owner or admin.
 * Returns: whether the credential was stored
 */
gboolean venture_mail_oauth_configure_client(VentureDatabase *database, VentureConfig *config,
	VentureEntity *provider, const gchar *client_id, const gchar *client_secret,
	const VentureActor *actor, GError **error);

/**
 * venture_mail_oauth_client_configured:
 * @database: repository
 * @provider: a saved mail_oauth_provider
 * Returns: whether a client credential is stored for it
 */
gboolean venture_mail_oauth_client_configured(VentureDatabase *database, VentureEntity *provider);

/**
 * venture_mail_oauth_begin:
 * @database: repository
 * @config: operator policy
 * @account: a saved mail_account naming an oauth-provider-id
 * @redirect_uri: the absolute callback address the provider sends the browser to
 * @user_id: the signed-in person, who alone may finish this sign-in
 * @error: (out) (optional): refusal
 *
 * Makes a one-time state and a PKCE verifier, kept in memory for
 * %VENTURE_MAIL_OAUTH_STATE_SECONDS, and returns where to send the browser.
 * Requires the same authority as the account's connector settings.
 * Returns: (transfer full) (nullable): the authorization URL
 */
gchar *venture_mail_oauth_begin(VentureDatabase *database, VentureConfig *config,
	VentureEntity *account, const gchar *redirect_uri, gint64 user_id, GError **error);

/**
 * venture_mail_oauth_complete:
 * @database: repository
 * @config: operator policy
 * @state: the state the provider echoed
 * @code: the authorization code
 * @user_id: the signed-in person
 * @actor: (nullable): audit attribution
 * @error: (out) (optional): unknown, expired or foreign state
 *   (%VENTURE_ERROR_NOT_FOUND), or a refused exchange
 *   (%VENTURE_ERROR_UNAUTHENTICATED)
 *
 * Exchanges the code with the verifier and stores the tokens as the
 * mailbox's connector credential. A state is used once.
 * Returns: the mail account id, or 0 on error
 */
gint64 venture_mail_oauth_complete(VentureDatabase *database, VentureConfig *config,
	const gchar *state, const gchar *code, gint64 user_id, const VentureActor *actor, GError **error);

/**
 * venture_mail_oauth_refresh:
 * @database: repository
 * @config: operator policy
 * @account: a saved mail_account signed in with tokens
 * @force: refresh even when the access token is not near expiry
 * @error: (out) (optional): %VENTURE_ERROR_UNAUTHENTICATED when the
 *   provider refused the refresh token (revoked or expired: sign in again)
 *
 * Returns: whether a usable access token is stored
 */
gboolean venture_mail_oauth_refresh(VentureDatabase *database, VentureConfig *config,
	VentureEntity *account, gboolean force, GError **error);

/**
 * venture_mail_oauth_pkce_challenge:
 * @verifier: a PKCE code verifier
 * Returns: (transfer full): BASE64URL(SHA256(verifier)) without padding
 */
gchar *venture_mail_oauth_pkce_challenge(const gchar *verifier);

/**
 * venture_mail_oauth_xoauth2:
 * @username: the mailbox login
 * @access_token: a bearer token
 * Returns: (transfer full) (nullable): the base64 SASL XOAUTH2 initial
 *   response, or %NULL when either argument holds a control character
 */
gchar *venture_mail_oauth_xoauth2(const gchar *username, const gchar *access_token);

G_END_DECLS
#endif
