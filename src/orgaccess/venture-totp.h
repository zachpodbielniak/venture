/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_TOTP_H
#define VENTURE_TOTP_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS

/**
 * VENTURE_TOTP_PERIOD:
 *
 * Seconds per RFC 6238 time step. Every authenticator app defaults to 30.
 */
#define VENTURE_TOTP_PERIOD 30

/**
 * VENTURE_TOTP_DIGITS:
 *
 * Code length. Six is what every authenticator app shows.
 */
#define VENTURE_TOTP_DIGITS 6

/**
 * venture_base32_encode:
 * @bytes: (array length=length): the bytes to encode
 * @length: how many
 *
 * RFC 4648 base32, upper case, without padding, which is the alphabet an
 * otpauth:// URI carries and an authenticator app accepts when typed.
 *
 * Returns: (transfer full): the encoded text
 */
gchar *venture_base32_encode(const guchar *bytes, gsize length);

/**
 * venture_base32_decode:
 * @text: base32 text, either case, padding and spaces ignored
 * @out_length: (out): how many bytes were decoded
 *
 * Returns: (transfer full) (array length=out_length) (nullable): the bytes,
 *   or %NULL when @text holds a character outside the alphabet
 */
guchar *venture_base32_decode(const gchar *text, gsize *out_length);

/**
 * venture_totp_code:
 * @secret: (array length=secret_length): the shared secret bytes
 * @secret_length: how many
 * @counter: the time step, that is unix seconds divided by the period
 * @digits: code length, 6 to 8
 *
 * HOTP (RFC 4226) over HMAC-SHA1 for one counter, which is what TOTP
 * (RFC 6238) is once the clock has been turned into a step.
 *
 * Returns: (transfer full): the zero-padded code
 */
gchar *venture_totp_code(const guchar *secret, gsize secret_length, guint64 counter, guint digits);

/**
 * venture_totp_verify:
 * @secret: (array length=secret_length): the shared secret bytes
 * @secret_length: how many
 * @code: what the person typed; spaces are ignored
 * @now: the instant to verify against
 * @drift_steps: how many steps either side of now to accept; one is the
 *   RFC's recommendation and what every server does
 * @out_counter: (out) (optional): the step that matched
 *
 * Compares in constant time against each step in the window, from the
 * earliest, and reports which one matched so the caller can refuse a
 * replay of that step.
 *
 * Returns: %TRUE if @code is the code for some step in the window
 */
gboolean venture_totp_verify(const guchar *secret, gsize secret_length, const gchar *code,
	GDateTime *now, guint drift_steps, guint64 *out_counter);

/**
 * venture_totp_counter:
 * @when: an instant
 *
 * Returns: the RFC 6238 step for @when at the default period
 */
guint64 venture_totp_counter(GDateTime *when);

/**
 * venture_totp_uri:
 * @issuer: the install's name, shown in the app
 * @account: the username
 * @base32_secret: the secret as venture_base32_encode() wrote it
 *
 * Builds the otpauth:// URI an authenticator app enrols from, with the
 * issuer both in the label and as a parameter, as the Key URI format
 * recommends. Both names are percent-encoded.
 *
 * Returns: (transfer full): the URI
 */
gchar *venture_totp_uri(const gchar *issuer, const gchar *account, const gchar *base32_secret);

G_END_DECLS
#endif
