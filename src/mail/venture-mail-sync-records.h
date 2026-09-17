/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_MAIL_SYNC_RECORDS_H
#define VENTURE_MAIL_SYNC_RECORDS_H
#include "model/venture-entity.h"
#include "model/venture-entity-macros.h"
G_BEGIN_DECLS
#define VENTURE_TYPE_MAIL_ACCOUNT (venture_mail_account_get_type())
VENTURE_DECLARE_ENTITY(VentureMailAccount, venture_mail_account, MAIL_ACCOUNT)
#define VENTURE_TYPE_MAIL_INBOUND (venture_mail_inbound_get_type())
VENTURE_DECLARE_ENTITY(VentureMailInbound, venture_mail_inbound, MAIL_INBOUND)
#define VENTURE_TYPE_MAIL_UNMATCHED_SENDER (venture_mail_unmatched_sender_get_type())
VENTURE_DECLARE_ENTITY(VentureMailUnmatchedSender, venture_mail_unmatched_sender, MAIL_UNMATCHED_SENDER)
G_END_DECLS
#endif
