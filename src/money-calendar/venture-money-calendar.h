/*
 * venture-money-calendar.h - every dated money event on one grid
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A money calendar event is one dated in|out amount that points back at
 * the record it came from. Sources (recurring schedules, bills, invoices,
 * dunning steps, payroll runs, tax packs) implement #VentureMoneyCalendarSource
 * and are held by a #VentureMoneyCalendarExpander, so a plugin or a later
 * module can add a source without touching the report, the page, the feed
 * or the CLI, all of which read the expanded array.
 */
#ifndef VENTURE_MONEY_CALENDAR_H
#define VENTURE_MONEY_CALENDAR_H

#include <glib-object.h>
#include <json-glib/json-glib.h>

#include "boxed/venture-money.h"
#include "boxed/venture-date-range.h"
#include "core/venture-context.h"
#include "report/venture-report.h"

G_BEGIN_DECLS

/* --- One event ------------------------------------------------------------ */

#define VENTURE_TYPE_MONEY_CALENDAR_EVENT (venture_money_calendar_event_get_type())
G_DECLARE_FINAL_TYPE(VentureMoneyCalendarEvent, venture_money_calendar_event,
                     VENTURE, MONEY_CALENDAR_EVENT, GObject)

/**
 * venture_money_calendar_event_new:
 * @kind: the source kind: recurring, bill, invoice, dunning, payroll or tax
 * @date: the calendar day the event sits on
 * @title: what a person reads on the grid
 * @amount: the amount, in the record's currency
 * @direction: "in" or "out"
 * @record_type: the entity name the event links to
 * @record_id: that record's id
 *
 * The anchor day defaults to @date; a source that carries an overdue event
 * to today sets the anchor to the original due date so the feed's UID stays
 * the same from one day to the next.
 *
 * Returns: (transfer full): a new event
 */
VentureMoneyCalendarEvent *
venture_money_calendar_event_new(
	const gchar		*kind,
	GDateTime		*date,
	const gchar		*title,
	const VentureMoney	*amount,
	const gchar		*direction,
	const gchar		*record_type,
	gint64			 record_id
);

const gchar *venture_money_calendar_event_get_kind(VentureMoneyCalendarEvent *self);
GDateTime *venture_money_calendar_event_get_date(VentureMoneyCalendarEvent *self);
const gchar *venture_money_calendar_event_get_day(VentureMoneyCalendarEvent *self);
const gchar *venture_money_calendar_event_get_week(VentureMoneyCalendarEvent *self);
const gchar *venture_money_calendar_event_get_anchor_day(VentureMoneyCalendarEvent *self);
const gchar *venture_money_calendar_event_get_title(VentureMoneyCalendarEvent *self);
const VentureMoney *venture_money_calendar_event_get_amount(VentureMoneyCalendarEvent *self);
const gchar *venture_money_calendar_event_get_currency(VentureMoneyCalendarEvent *self);
const gchar *venture_money_calendar_event_get_direction(VentureMoneyCalendarEvent *self);
const gchar *venture_money_calendar_event_get_record_type(VentureMoneyCalendarEvent *self);
gint64 venture_money_calendar_event_get_record_id(VentureMoneyCalendarEvent *self);
const gchar *venture_money_calendar_event_get_counterparty(VentureMoneyCalendarEvent *self);
gint64 venture_money_calendar_event_get_counterparty_id(VentureMoneyCalendarEvent *self);
gint64 venture_money_calendar_event_get_venture_id(VentureMoneyCalendarEvent *self);
gboolean venture_money_calendar_event_get_overdue(VentureMoneyCalendarEvent *self);
gboolean venture_money_calendar_event_get_counts_in_net(VentureMoneyCalendarEvent *self);
const gchar *venture_money_calendar_event_get_slot(VentureMoneyCalendarEvent *self);
gchar *venture_money_calendar_event_get_uid(VentureMoneyCalendarEvent *self);

/**
 * venture_money_calendar_event_signed_amount:
 * @self: an event
 *
 * Returns: (transfer full): the amount, negated when the direction is out
 */
VentureMoney *venture_money_calendar_event_signed_amount(VentureMoneyCalendarEvent *self);

/* --- A source ------------------------------------------------------------ */

#define VENTURE_TYPE_MONEY_CALENDAR_SOURCE (venture_money_calendar_source_get_type())
G_DECLARE_INTERFACE(VentureMoneyCalendarSource, venture_money_calendar_source,
                    VENTURE, MONEY_CALENDAR_SOURCE, GObject)

/**
 * VentureMoneyCalendarSourceInterface:
 * @get_kind: the kind every event this source emits carries
 * @get_module: (nullable): the module that must be on for the source to run
 * @expand: appends events dated inside @range to @events; @today is the
 *   day overdue items are carried to
 */
struct _VentureMoneyCalendarSourceInterface
{
	GTypeInterface parent_iface;

	const gchar *	(*get_kind)	(VentureMoneyCalendarSource *self);
	const gchar *	(*get_module)	(VentureMoneyCalendarSource *self);
	gboolean	(*expand)	(VentureMoneyCalendarSource *self,
					 VentureContext *context,
					 gint64 organization_id,
					 const VentureDateRange *range,
					 GDateTime *today,
					 GPtrArray *events,
					 GError **error);
};

const gchar *venture_money_calendar_source_get_kind(VentureMoneyCalendarSource *self);
const gchar *venture_money_calendar_source_get_module(VentureMoneyCalendarSource *self);
/**
 * venture_money_calendar_source_expand:
 * @self: the event source
 * @context: the application context
 * @organization_id: the organization whose events are requested
 * @range: the requested calendar date range
 * @today: the day to which overdue events are carried
 * @events: (element-type VentureMoneyCalendarEvent): destination array owning its events
 * @error: (out) (optional): the error
 *
 * Appends this source's events to @events. The caller owns the array and
 * must arrange to unref each event when removing it.
 *
 * Returns: TRUE on success
 */
gboolean venture_money_calendar_source_expand(VentureMoneyCalendarSource *self,
	VentureContext *context, gint64 organization_id, const VentureDateRange *range,
	GDateTime *today, GPtrArray *events, GError **error);

/* --- The expander ---------------------------------------------------------- */

#define VENTURE_TYPE_MONEY_CALENDAR_EXPANDER (venture_money_calendar_expander_get_type())
G_DECLARE_FINAL_TYPE(VentureMoneyCalendarExpander, venture_money_calendar_expander,
                     VENTURE, MONEY_CALENDAR_EXPANDER, GObject)

VentureMoneyCalendarExpander *venture_money_calendar_expander_new(void);

/**
 * venture_money_calendar_expander_add:
 * @self: the expander
 * @source: (transfer none): a source; a source with the same kind replaces it
 */
void venture_money_calendar_expander_add(VentureMoneyCalendarExpander *self,
	VentureMoneyCalendarSource *source);

/**
 * venture_money_calendar_expander_kinds:
 * @self: the expander
 *
 * Returns: (transfer container) (element-type utf8): the registered kinds
 */
GPtrArray *venture_money_calendar_expander_kinds(VentureMoneyCalendarExpander *self);

/**
 * venture_money_calendar_expander_expand:
 * @self: the expander
 * @context: the context; sources whose module is off are skipped
 * @organization_id: the organization
 * @range: the inclusive start, exclusive end, of the calendar
 * @today: overdue items are carried to this day
 * @error: (out) (optional): the error
 *
 * Returns: (transfer container) (element-type VentureMoneyCalendarEvent):
 *   every event from every enabled source, sorted by day then kind
 */
GPtrArray *venture_money_calendar_expander_expand(VentureMoneyCalendarExpander *self,
	VentureContext *context, gint64 organization_id, const VentureDateRange *range,
	GDateTime *today, GError **error);

/**
 * venture_money_calendar_expander_get_default:
 *
 * Returns: (transfer none): the process-wide expander with the built-in sources
 */
VentureMoneyCalendarExpander *venture_money_calendar_expander_get_default(void);

/* --- Convenience over the default expander ------------------------------------ */

/**
 * venture_money_calendar_events:
 * @context: the context
 * @organization_id: the organization
 * @range: the calendar span
 * @today: (nullable): the carry day; %NULL means now
 * @error: (out) (optional): the error
 *
 * Refused with %VENTURE_ERROR_VALIDATION when the money_calendar module is off.
 *
 * Returns: (transfer container) (element-type VentureMoneyCalendarEvent): the events
 */
GPtrArray *venture_money_calendar_events(VentureContext *context, gint64 organization_id,
	const VentureDateRange *range, GDateTime *today, GError **error);

/**
 * venture_money_calendar_filter:
 * @events: (element-type VentureMoneyCalendarEvent): expanded events
 * @kind: (nullable): keep only this kind
 * @company_id: keep only this counterparty, or 0
 * @venture_id: keep only this venture, or 0
 * @currency: (nullable): keep only this currency
 *
 * Returns: (transfer container) (element-type VentureMoneyCalendarEvent): the kept events
 */
GPtrArray *venture_money_calendar_filter(GPtrArray *events, const gchar *kind,
	gint64 company_id, gint64 venture_id, const gchar *currency);

/**
 * venture_money_calendar_nets:
 * @events: (element-type VentureMoneyCalendarEvent): expanded events
 * @by_week: %TRUE for ISO-week nets, %FALSE for daily nets
 *
 * Only events that count in the net are summed, per currency, never across.
 *
 * Returns: (transfer container) (element-type utf8 VentureMoney): net by
 *   "YYYY-MM-DD|CUR" or "YYYY-Www|CUR"
 */
GHashTable *venture_money_calendar_nets(GPtrArray *events, gboolean by_week);

/**
 * venture_money_calendar_nets_format:
 * @nets: the table from venture_money_calendar_nets()
 * @bucket: the day or week
 *
 * Returns: (transfer full): every currency's net for @bucket, space separated
 */
gchar *venture_money_calendar_nets_format(GHashTable *nets, const gchar *bucket);

/**
 * venture_money_calendar_ics:
 * @events: (element-type VentureMoneyCalendarEvent): expanded events
 *
 * Returns: (transfer full): an RFC 5545 calendar of all-day events with stable UIDs
 */
gchar *venture_money_calendar_ics(GPtrArray *events);

/**
 * venture_money_calendar_register_reports:
 * @registry: the report registry
 *
 * Registers the =money_calendar= report.
 */
void venture_money_calendar_register_reports(VentureReportRegistry *registry);

G_END_DECLS

#endif /* VENTURE_MONEY_CALENDAR_H */
