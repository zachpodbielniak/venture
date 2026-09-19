/*
 * venture-customer-health.h - Customer health: the list behind the churn number
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Churn is a lagging number: by the time it moves, the customer has gone.
 * Health is the same question asked early, per company, from records the
 * other modules already keep -- when they were last spoken to, what they
 * owe and for how long, how many tickets they have waiting, what the
 * reminder policy has already had to do, and what they have paid in the
 * trailing year -- reduced to a band against thresholds the organisation
 * sets. The red band is actionable: a bounded sweep turns each red company
 * into one "check in" next action for its account owner.
 */

#ifndef VENTURE_CUSTOMER_HEALTH_H
#define VENTURE_CUSTOMER_HEALTH_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>

G_BEGIN_DECLS

/**
 * VentureHealthBand:
 * @VENTURE_HEALTH_BAND_GREEN: no threshold tripped
 * @VENTURE_HEALTH_BAND_AMBER: one threshold tripped
 * @VENTURE_HEALTH_BAND_RED: two or more thresholds tripped; at risk
 *
 * How a customer stands against the organisation's thresholds.
 */
typedef enum
{
	VENTURE_HEALTH_BAND_GREEN = 0,
	VENTURE_HEALTH_BAND_AMBER,
	VENTURE_HEALTH_BAND_RED
} VentureHealthBand;

/**
 * VentureHealthFlag:
 * @VENTURE_HEALTH_FLAG_QUIET: no touch within the touch threshold
 * @VENTURE_HEALTH_FLAG_OVERDUE: an invoice overdue past the overdue threshold
 * @VENTURE_HEALTH_FLAG_TICKETS: open tickets at or over the ticket
 *   threshold, or any open ticket past its service level
 *
 * The three reasons a company can be unwell. The band is how many of
 * them apply.
 */
typedef enum
{
	VENTURE_HEALTH_FLAG_QUIET = 1 << 0,
	VENTURE_HEALTH_FLAG_OVERDUE = 1 << 1,
	VENTURE_HEALTH_FLAG_TICKETS = 1 << 2
} VentureHealthFlag;

/**
 * VentureHealthThresholds:
 * @touch_days: days without a touch before a company counts as quiet
 * @overdue_days: days past due before an overdue invoice counts against it
 * @open_tickets: open tickets at which the queue counts against it
 *
 * The organisation's thresholds, read from its headline settings row:
 * `health_touch_days`, `health_overdue_days` and `health_open_tickets`,
 * zero meaning the defaults of 30, 15 and 3.
 */
typedef struct
{
	gint64	touch_days;
	gint64	overdue_days;
	gint64	open_tickets;
} VentureHealthThresholds;

/**
 * venture_health_band_to_string:
 * @band: a band
 *
 * Returns: "green", "amber" or "red"
 */
const gchar *
venture_health_band_to_string(VentureHealthBand band);

/**
 * venture_health_band_from_string:
 * @text: "green", "amber" or "red"
 * @out_band: (out): the band
 *
 * Returns: %FALSE when @text names no band
 */
gboolean
venture_health_band_from_string(
	const gchar		*text,
	VentureHealthBand	*out_band
);

#define VENTURE_TYPE_CUSTOMER_HEALTH (venture_customer_health_get_type())
G_DECLARE_FINAL_TYPE(VentureCustomerHealth, venture_customer_health,
                     VENTURE, CUSTOMER_HEALTH, GObject)

/**
 * venture_customer_health_get_company_id:
 * @self: a health reading
 *
 * Returns: the company it describes
 */
gint64
venture_customer_health_get_company_id(VentureCustomerHealth *self);

/**
 * venture_customer_health_get_company_name:
 * @self: a health reading
 *
 * Returns: (transfer none): the company's name
 */
const gchar *
venture_customer_health_get_company_name(VentureCustomerHealth *self);

/**
 * venture_customer_health_get_owner:
 * @self: a health reading
 *
 * The account owner: the username of the company's `owner_user_id`.
 *
 * Returns: (transfer none): the username, or "" when nobody owns it
 */
const gchar *
venture_customer_health_get_owner(VentureCustomerHealth *self);

/**
 * venture_customer_health_get_last_touch:
 * @self: a health reading
 *
 * The most recent interaction, completed activity or inbound mail on or
 * before the reading's date.
 *
 * Returns: (transfer none) (nullable): when, or %NULL when there has never
 *   been one
 */
GDateTime *
venture_customer_health_get_last_touch(VentureCustomerHealth *self);

/**
 * venture_customer_health_get_days_since_touch:
 * @self: a health reading
 *
 * Days from the last touch to the reading's date; from the company's
 * creation when there has never been a touch.
 *
 * Returns: whole days
 */
gint64
venture_customer_health_get_days_since_touch(VentureCustomerHealth *self);

/**
 * venture_customer_health_get_open_deals:
 * @self: a health reading
 *
 * Returns: deals neither won nor lost
 */
gint64
venture_customer_health_get_open_deals(VentureCustomerHealth *self);

/**
 * venture_customer_health_get_overdue_invoices:
 * @self: a health reading
 *
 * Returns: issued invoices past their due date and not settled
 */
gint64
venture_customer_health_get_overdue_invoices(VentureCustomerHealth *self);

/**
 * venture_customer_health_get_overdue_days:
 * @self: a health reading
 *
 * Returns: how many days past due the oldest overdue invoice is; zero when
 *   nothing is overdue
 */
gint64
venture_customer_health_get_overdue_days(VentureCustomerHealth *self);

/**
 * venture_customer_health_get_open_tickets:
 * @self: a health reading
 *
 * Returns: tickets neither done nor cancelled; zero with the tickets
 *   module off
 */
gint64
venture_customer_health_get_open_tickets(VentureCustomerHealth *self);

/**
 * venture_customer_health_get_sla_breaches:
 * @self: a health reading
 *
 * Returns: of the open tickets, how many have missed a service level
 */
gint64
venture_customer_health_get_sla_breaches(VentureCustomerHealth *self);

/**
 * venture_customer_health_get_dunning_step:
 * @self: a health reading
 *
 * The furthest reminder step the dunning module has reached on any of the
 * company's still-open invoices.
 *
 * Returns: the 1-based step, or zero when no reminder has gone out or the
 *   dunning module is off
 */
gint64
venture_customer_health_get_dunning_step(VentureCustomerHealth *self);

/**
 * venture_customer_health_get_revenue_12m:
 * @self: a health reading
 *
 * Paid revenue -- applied receipts less refunds, as the headline reports
 * count it -- in the twelve months before the reading's date, in the
 * organisation's book currency.
 *
 * Returns: (transfer none) (nullable): the amount, or %NULL when the
 *   company paid nothing in the window
 */
const VentureMoney *
venture_customer_health_get_revenue_12m(VentureCustomerHealth *self);

/**
 * venture_customer_health_get_flags:
 * @self: a health reading
 *
 * Returns: which thresholds are tripped, as #VentureHealthFlag bits
 */
guint
venture_customer_health_get_flags(VentureCustomerHealth *self);

/**
 * venture_customer_health_get_band:
 * @self: a health reading
 *
 * Returns: green with no flag, amber with one, red with two or three
 */
VentureHealthBand
venture_customer_health_get_band(VentureCustomerHealth *self);

/**
 * venture_customer_health_get_reasons:
 * @self: a health reading
 *
 * The three reasons in plain words, tripped or not, in flag order: the
 * last touch, the overdue invoices, the ticket queue. Each names the
 * figure and the threshold it was judged against.
 *
 * Returns: (transfer none) (element-type utf8): three strings
 */
GPtrArray *
venture_customer_health_get_reasons(VentureCustomerHealth *self);

/**
 * venture_customer_health_thresholds:
 * @context: the wiring
 * @organization_id: the organisation
 * @out_thresholds: (out): the thresholds
 *
 * Reads the organisation's thresholds, defaults applied.
 */
void
venture_customer_health_thresholds(
	VentureContext		*context,
	gint64			 organization_id,
	VentureHealthThresholds	*out_thresholds
);

/**
 * venture_customer_health_for_company:
 * @context: the wiring
 * @company: the company
 * @as_of: (nullable): the date to read health at; %NULL means now
 * @error: (out) (optional): return location for a #GError
 *
 * Reads one company's health.
 *
 * Returns: (transfer full) (nullable): the reading
 */
VentureCustomerHealth *
venture_customer_health_for_company(
	VentureContext	 *context,
	VentureEntity	 *company,
	GDateTime	 *as_of,
	GError		**error
);

/**
 * venture_customer_health_compute:
 * @context: the wiring
 * @organization_id: the organisation, or 0 for the default
 * @as_of: (nullable): the date to read health at; %NULL means now
 * @error: (out) (optional): return location for a #GError
 *
 * Reads the health of every customer company in the organisation --
 * companies of kind `customer`, active or not -- worst band first, then
 * by name. Suppliers, prospects and the rest are not customers.
 *
 * Returns: (transfer full) (element-type VentureCustomerHealth) (nullable):
 *   the readings
 */
GPtrArray *
venture_customer_health_compute(
	VentureContext	 *context,
	gint64		  organization_id,
	GDateTime	 *as_of,
	GError		**error
);

/**
 * venture_customer_health_sweep:
 * @context: the wiring
 * @organization_id: the organisation; required
 * @as_of: (nullable): the date to judge health at; %NULL means now
 * @limit: at most this many red companies are visited; zero means 100,
 *   and 1000 is the ceiling
 * @actor: (nullable): who is asking, for the audit trail
 * @error: (out) (optional): return location for a #GError
 *
 * For each red company with no planned "check in: <company>" activity,
 * creates one for its account owner, due at @as_of, with the three
 * reasons in its body. A company that already has one open is left alone,
 * so running the sweep twice creates nothing the second time. Bounded and
 * synchronous: there is no background thread. One transaction: a save
 * refused part-way through rolls back every check-in the sweep made.
 *
 * Refused with %VENTURE_ERROR_VALIDATION when the `customer_health` or
 * `activities` module is off, and %VENTURE_ERROR_INVALID_ARGUMENT without
 * an organisation.
 *
 * Returns: the number of activities created, or -1 on failure
 */
gint
venture_customer_health_sweep(
	VentureContext		 *context,
	gint64			  organization_id,
	GDateTime		 *as_of,
	guint			  limit,
	const VentureActor	 *actor,
	GError			**error
);

/**
 * venture_customer_health_append_block:
 * @context: the wiring
 * @content: the page being built
 * @record: the record whose page it is
 *
 * On a company's page, appends a card with the band and its three reasons.
 * Anything but a customer company, or the module off, appends nothing.
 */
void
venture_customer_health_append_block(
	VentureContext	*context,
	GString		*content,
	VentureEntity	*record
);

/**
 * venture_customer_health_register_reports:
 * @registry: the report registry
 *
 * Registers `customer_health`.
 */
void
venture_customer_health_register_reports(VentureReportRegistry *registry);

G_END_DECLS

#endif /* VENTURE_CUSTOMER_HEALTH_H */
