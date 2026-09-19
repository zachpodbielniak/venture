/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_CALENDAR_RECORDS_H
#define VENTURE_CALENDAR_RECORDS_H
#include "model/venture-entity.h"
#include "model/venture-entity-macros.h"
G_BEGIN_DECLS
#define VENTURE_TYPE_CALENDAR_ACCOUNT (venture_calendar_account_get_type())
VENTURE_DECLARE_ENTITY(VentureCalendarAccount, venture_calendar_account, CALENDAR_ACCOUNT)
#define VENTURE_TYPE_CALENDAR_EVENT (venture_calendar_event_get_type())
VENTURE_DECLARE_ENTITY(VentureCalendarEvent, venture_calendar_event, CALENDAR_EVENT)
#define VENTURE_TYPE_BOOKING_PAGE (venture_booking_page_get_type())
VENTURE_DECLARE_ENTITY(VentureBookingPage, venture_booking_page, BOOKING_PAGE)
G_END_DECLS
#endif
