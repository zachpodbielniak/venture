/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_MAIL_RECORDS_H
#define VENTURE_MAIL_RECORDS_H
#include "model/venture-entity.h"
#include "model/venture-entity-macros.h"
G_BEGIN_DECLS
#define VENTURE_TYPE_MAIL_MESSAGE (venture_mail_message_get_type())
VENTURE_DECLARE_ENTITY(VentureMailMessage, venture_mail_message, MAIL_MESSAGE)
#define VENTURE_TYPE_MAIL_TEMPLATE (venture_mail_template_get_type())
VENTURE_DECLARE_ENTITY(VentureMailTemplate, venture_mail_template, MAIL_TEMPLATE)
G_END_DECLS
#endif
