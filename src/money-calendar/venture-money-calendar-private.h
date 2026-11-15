/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_MONEY_CALENDAR_PRIVATE_H
#define VENTURE_MONEY_CALENDAR_PRIVATE_H

#include "money-calendar/venture-money-calendar.h"

/* The built-in sources, added to a fresh expander in kind order. */
void venture_money_calendar_add_builtin_sources(VentureMoneyCalendarExpander *expander);

/**
 * venture_money_calendar_totals: (skip)
 * @events: (element-type VentureMoneyCalendarEvent): the events, in order
 *
 * The money calendar report's span totals: keyed "in|CUR", "out|CUR"
 * (the amounts as given) and "net|CUR" (signed), over the events that
 * count in a net. Each is summed in order by vk_settlement_net_fitting,
 * so an amount that would take a total out of range is left out.
 *
 * Returns: (transfer full) (element-type utf8 VentureMoney): the totals
 */
GHashTable *venture_money_calendar_totals(GPtrArray *events);

VentureReportResult *venture_report_household_subscriptions(VentureContext *context, VentureDateRange *period, JsonObject *options, GError **error);

#endif
