/*
 * venture-support-rollup.h - Support cost and ticket volume per customer
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The support rollup answers "how do I support existing customers" with a
 * number per customer: tickets raised, closed and still open, service
 * levels missed, how long the first reply and the resolution took, what the
 * customer made of it, and what the support cost. The cost is the one
 * figure the home page's Support card and a company's page both show, and
 * both read it through venture_support_rollup_cost() so they cannot
 * disagree.
 */

#ifndef VENTURE_SUPPORT_ROLLUP_H
#define VENTURE_SUPPORT_ROLLUP_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/**
 * venture_support_rollup_register_reports:
 * @registry: the report registry
 *
 * Registers the `support_rollup` report: one row per company (or per
 * product with `group_by=product`) for the tickets raised in the period.
 */
void
venture_support_rollup_register_reports(VentureReportRegistry *registry);

/**
 * venture_support_rollup_cost:
 * @context: the wiring
 * @options: (nullable): the scope: `organization_id` (0 or absent for the
 *   default), and optionally `company`, `venture_id` and `as_of`
 * @period: the period the tickets were raised in
 * @error: (out) (optional): a refused option or a failed read
 *
 * The support cost of the tickets raised in @period, for the organisation
 * or for one company when @options names one. This is the `support_cost`
 * metric of the `support_rollup` report, so the Support card on the home
 * page, the line on a company's page and the report's own total are the
 * same number: a money metric, or a text metric reading `n/a` when no rate
 * is configured.
 *
 * Returns: (transfer full) (nullable): the metric, or %NULL with @error set
 */
VentureMetric *
venture_support_rollup_cost(
	VentureContext		 *context,
	JsonObject		 *options,
	VentureDateRange	 *period,
	GError			**error
);

G_END_DECLS

#endif /* VENTURE_SUPPORT_ROLLUP_H */
