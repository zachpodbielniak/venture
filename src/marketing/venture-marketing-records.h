/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_MARKETING_RECORDS_H
#define VENTURE_MARKETING_RECORDS_H
#include "model/venture-entity.h"
#include "model/venture-entity-macros.h"
G_BEGIN_DECLS
#define VENTURE_TYPE_MARKETING_LIST (venture_marketing_list_get_type())
VENTURE_DECLARE_ENTITY(VentureMarketingList, venture_marketing_list, MARKETING_LIST)
#define VENTURE_TYPE_MARKETING_MEMBER (venture_marketing_member_get_type())
VENTURE_DECLARE_ENTITY(VentureMarketingMember, venture_marketing_member, MARKETING_MEMBER)
#define VENTURE_TYPE_MARKETING_CONSENT (venture_marketing_consent_get_type())
VENTURE_DECLARE_ENTITY(VentureMarketingConsent, venture_marketing_consent, MARKETING_CONSENT)
#define VENTURE_TYPE_MARKETING_SEND (venture_marketing_send_get_type())
VENTURE_DECLARE_ENTITY(VentureMarketingSend, venture_marketing_send, MARKETING_SEND)
#define VENTURE_TYPE_MARKETING_RECIPIENT (venture_marketing_recipient_get_type())
VENTURE_DECLARE_ENTITY(VentureMarketingRecipient, venture_marketing_recipient, MARKETING_RECIPIENT)
#define VENTURE_TYPE_MARKETING_EVENT (venture_marketing_event_get_type())
VENTURE_DECLARE_ENTITY(VentureMarketingEvent, venture_marketing_event, MARKETING_EVENT)
/**
 * venture_marketing_list_mode_get_type:
 * Returns: the static/segment audience mode enumeration
 */
GType venture_marketing_list_mode_get_type(void) G_GNUC_CONST;
/**
 * venture_marketing_target_get_type:
 * Returns: the contact/company/lead segment target enumeration
 */
GType venture_marketing_target_get_type(void) G_GNUC_CONST;
/**
 * venture_marketing_send_state_get_type:
 * Returns: the draft/preview/approved/paused/cancelled/complete enumeration
 */
GType venture_marketing_send_state_get_type(void) G_GNUC_CONST;
G_END_DECLS
#endif
