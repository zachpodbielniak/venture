/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_MAIL_TEMPLATE_H
#define VENTURE_MAIL_TEMPLATE_H
#include "mail/venture-mail-records.h"
/**
 * venture_mail_template_render:
 * @self: organization template
 * @record: record supplying non-sensitive field values
 * @error: (out) (optional): missing placeholder or mismatched organization
 * Returns: (transfer full) (nullable): unsaved message with rendered bodies
 */
VentureMailMessage *venture_mail_template_render(VentureMailTemplate *self, VentureEntity *record, GError **error);
VentureMailMessage *venture_mail_template_render_values(VentureMailTemplate *self, JsonObject *values, GError **error);
#endif
