/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_SMTP_MAILER_H
#define VENTURE_SMTP_MAILER_H
#include "mail/venture-mailer.h"
#include "config/venture-config.h"
#define VENTURE_TYPE_SMTP_MAILER (venture_smtp_mailer_get_type())
G_DECLARE_FINAL_TYPE(VentureSmtpMailer, venture_smtp_mailer, VENTURE, SMTP_MAILER, GObject)
/**
 * venture_smtp_mailer_new:
 * @config: mail configuration; credentials are environment variable names
 * Returns: (transfer full): SMTP adapter; invalid configuration fails on send
 */
VentureSmtpMailer *venture_smtp_mailer_new(VentureConfig *config);
/**
 * venture_smtp_mailer_new_from_values:
 * @values: explicit flat SMTP profile, including from and retries=0
 * @error: (out) (optional): redacted configuration refusal
 *
 * Copies credentials directly into an immutable in-memory mail-glib config;
 * no environment variable or temporary file is consulted. Requires TLS,
 * a sender and a timeout no greater than 30 seconds. Outbox owns retries.
 * The caller must authorize the organization and permitted endpoint before
 * constructing this adapter. Construction makes no network request.
 *
 * Returns: (transfer full) (nullable): SMTP adapter, or NULL on invalid settings
 */
VentureSmtpMailer *venture_smtp_mailer_new_from_values(JsonObject *values, GError **error);
/**
 * venture_smtp_mailer_new_for_connection:
 * @values: explicit validated SMTP settings
 * @connection_id: verified persisted organization integration identity
 * @version: current persisted connection version
 * @error: (out) (optional): redacted refusal
 *
 * Like venture_smtp_mailer_new_from_values(), with immutable binding evidence
 * that the outbox stores before transport. The caller must resolve and verify
 * the connection and endpoint on the repository thread first.
 *
 * Returns: (transfer full) (nullable): bound SMTP transport
 */
VentureSmtpMailer *venture_smtp_mailer_new_for_connection(JsonObject *values,
	gint64 connection_id, gint64 version, GError **error);
#endif
