/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_MONEY_CALENDAR_PRIVATE_H
#define VENTURE_MONEY_CALENDAR_PRIVATE_H

#include "money-calendar/venture-money-calendar.h"

/* The built-in sources, added to a fresh expander in kind order. */
void venture_money_calendar_add_builtin_sources(VentureMoneyCalendarExpander *expander);

VentureReportResult *venture_report_household_subscriptions(VentureContext *context, VentureDateRange *period, JsonObject *options, GError **error);

#endif
