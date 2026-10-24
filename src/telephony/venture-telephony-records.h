/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_TELEPHONY_RECORDS_H
#define VENTURE_TELEPHONY_RECORDS_H
#include "model/venture-entity.h"
#include "model/venture-entity-macros.h"
G_BEGIN_DECLS

/**
 * VentureTextDirection:
 * @VENTURE_TEXT_DIRECTION_OUTBOUND: sent by the organization
 * @VENTURE_TEXT_DIRECTION_INBOUND: received from a contact
 *
 * Which way a text message went.
 */
typedef enum {
	VENTURE_TEXT_DIRECTION_OUTBOUND,
	VENTURE_TEXT_DIRECTION_INBOUND
} VentureTextDirection;
/**
 * venture_text_direction_get_type:
 * Returns: the outbound/inbound enumeration
 */
GType venture_text_direction_get_type(void) G_GNUC_CONST;

#define VENTURE_TYPE_TELEPHONY_ACCOUNT (venture_telephony_account_get_type())
VENTURE_DECLARE_ENTITY(VentureTelephonyAccount, venture_telephony_account, TELEPHONY_ACCOUNT)
#define VENTURE_TYPE_TELEPHONY_EXTENSION (venture_telephony_extension_get_type())
VENTURE_DECLARE_ENTITY(VentureTelephonyExtension, venture_telephony_extension, TELEPHONY_EXTENSION)
#define VENTURE_TYPE_TEXT_MESSAGE (venture_text_message_get_type())
VENTURE_DECLARE_ENTITY(VentureTextMessage, venture_text_message, TEXT_MESSAGE)

G_END_DECLS
#endif
