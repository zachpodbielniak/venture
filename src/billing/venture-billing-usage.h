/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_BILLING_USAGE_H
#define VENTURE_BILLING_USAGE_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
/**
 * venture_billing_usage_total:
 * @database: the database
 * @organization_id: the subscription's organization
 * @subscription_id: the subscription
 * @from: the period's first instant, counted
 * @until: the period's end, not counted
 * @error: (out) (optional): error location
 *
 * Adds up the usage reported for @subscription_id in [@from, @until). The
 * half-open interval is what counts a report on a boundary exactly once:
 * it belongs to the period it starts.
 *
 * Returns: the units used, or -1 on error
 */
gint64 venture_billing_usage_total(VentureDatabase *database, gint64 organization_id,
	gint64 subscription_id, GDateTime *from, GDateTime *until, GError **error);
/**
 * venture_billing_usage_bill: (skip)
 * @database: the database, inside the billing service's transaction
 * @subscription: the subscription being renewed, still describing the
 *   period that has ended
 * @invoice: the renewal invoice, not yet sent
 * @period_start: the first day of the period the invoice charges for
 * @actor: (nullable): the audit actor
 * @error: (out) (optional): error location
 *
 * Adds the ended period's usage beyond the included units as its own line
 * on @invoice. Does nothing unless @period_start is the subscription's
 * current period end, which is what a renewal bills; a start has no usage
 * to bill and a free trial's is not charged.
 *
 * Returns: TRUE unless the line could not be written
 */
gboolean venture_billing_usage_bill(VentureDatabase *database, VentureEntity *subscription,
	VentureEntity *invoice, GDateTime *period_start, const VentureActor *actor, GError **error);
/**
 * venture_billing_usage_check_save: (skip)
 * @database: the database, lock held
 * @record: a usage record about to be written
 * @error: (out) (optional): error location
 *
 * Returns: TRUE when the report may be written
 */
gboolean venture_billing_usage_check_save(VentureDatabase *database, VentureEntity *record, GError **error);
/**
 * venture_billing_usage_check_removal: (skip)
 * @database: the database
 * @record: a usage record being removed, restored or purged
 * @error: (out) (optional): error location
 *
 * Returns: TRUE unless the record was already billed
 */
gboolean venture_billing_usage_check_removal(VentureDatabase *database, VentureEntity *record, GError **error);
/**
 * venture_billing_price_check_metering: (skip)
 * @price: a plan price about to be written
 * @error: (out) (optional): error location
 *
 * Returns: TRUE when the price is flat, or metered with a unit, a rate in
 *   its own currency and a non-negative number of included units
 */
gboolean venture_billing_price_check_metering(VentureEntity *price, GError **error);
G_END_DECLS
#endif
