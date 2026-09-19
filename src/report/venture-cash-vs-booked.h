/*
 * venture-cash-vs-booked.h - Revenue booked beside cash received
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The first of the five questions -- how much money is coming in -- has two
 * honest answers that drift apart: what was invoiced, and what arrived. This
 * report puts them side by side per period bucket and per currency, and the
 * home card's "booked vs received" line reads the same function, so the
 * card and the report cannot disagree.
 */

#ifndef VENTURE_CASH_VS_BOOKED_H
#define VENTURE_CASH_VS_BOOKED_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/**
 * venture_cash_vs_booked_report:
 * @context: the running context
 * @period: the period to bucket
 * @options: (nullable): organization_id, currency, as_of, bucket (month or
 *   week), customer_id, venture_id; any other member is refused
 * @error: (out) (optional): the refusal
 *
 * Revenue booked (issued invoices less voids, credit notes and write-offs)
 * beside cash received (receipts less refunds) for each bucket of @period
 * and each currency seen, with the gap and the gap accumulated from the
 * period start. The metrics are the period totals in the report currency;
 * "booked_vs_received" is the one-line text the home card shows.
 *
 * Returns: (transfer full) (nullable): the result, or %NULL on refusal
 */
VentureReportResult *
venture_cash_vs_booked_report(
	VentureContext		 *context,
	VentureDateRange	 *period,
	JsonObject		 *options,
	GError			**error
);

/**
 * venture_cash_vs_booked_register_report:
 * @registry: the report registry
 *
 * Registers the report as "cash_vs_booked".
 */
void
venture_cash_vs_booked_register_report(VentureReportRegistry *registry);

G_END_DECLS

#endif /* VENTURE_CASH_VS_BOOKED_H */
