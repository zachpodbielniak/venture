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
#endif
