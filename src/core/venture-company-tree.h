/*
 * venture-company-tree.h - Parent accounts and branch rollups
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A customer company may be a branch of another: a franchise location of
 * its group, a school of its district. `company.parent-id` names the
 * parent. This file holds the save rule (no loop, same organization, the
 * same bound as every other tree), the list of a parent's branches, and
 * the rollup the company page shows for the company alone and including
 * its branches. The figures are read from the rows every time; nothing is
 * stored on the parent. See docs/data-model.org.
 */

#ifndef VENTURE_COMPANY_TREE_H
#define VENTURE_COMPANY_TREE_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>

G_BEGIN_DECLS

/**
 * VentureCompanyRollup:
 * @companies: how many companies were counted (1 for the company alone)
 * @open_deals: deals neither won nor lost
 * @open_deal_value: their value, in the book currency
 * @won_deals: deals won
 * @won_revenue: their value, in the book currency
 * @open_invoices: invoices sent and not yet paid or void
 * @balance_due: what those companies owe, in the book currency
 * @open_tickets: tickets neither done nor cancelled
 * @other_currency: deals counted whose value is in another currency, and
 *   so left out of the sums rather than added at a made-up rate
 *
 * What a customer is worth to you. Clear with
 * venture_company_rollup_clear().
 */
typedef struct
{
	guint		 companies;
	guint		 open_deals;
	VentureMoney	*open_deal_value;
	guint		 won_deals;
	VentureMoney	*won_revenue;
	guint		 open_invoices;
	VentureMoney	*balance_due;
	guint		 open_tickets;
	guint		 other_currency;
} VentureCompanyRollup;

/**
 * venture_company_tree_install:
 * @context: application context
 *
 * Adds the save rule for a company's `parent-id`: the parent exists, is
 * in the same organization, and is not the company itself or any branch
 * beneath it. Calling it again for the same database adds nothing.
 */
void
venture_company_tree_install(VentureContext *context);

/**
 * venture_company_tree_children:
 * @database: the database to read
 * @company_id: the parent account
 * @error: (out) (optional): return location for a #GError
 *
 * The live companies whose parent is @company_id, by name.
 *
 * Returns: (transfer full) (element-type VentureEntity) (nullable): the
 *   branches, or %NULL with @error set
 */
GPtrArray *
venture_company_tree_children(
	VentureDatabase	 *database,
	gint64		  company_id,
	GError		**error
);

/**
 * venture_company_tree_rollup:
 * @database: the database to read
 * @company_id: the company
 * @include_branches: count every branch beneath it too
 * @out: (out caller-allocates): the figures
 * @error: (out) (optional): return location for a #GError
 *
 * Counts the company's open deals, won revenue, open invoices and balance
 * due, and open tickets; with @include_branches, its whole subtree's.
 *
 * Returns: %TRUE with @out filled, or %FALSE with @error set
 */
gboolean
venture_company_tree_rollup(
	VentureDatabase		 *database,
	gint64			  company_id,
	gboolean		  include_branches,
	VentureCompanyRollup	 *out,
	GError			**error
);

/**
 * venture_company_rollup_clear:
 * @rollup: figures filled by venture_company_tree_rollup()
 *
 * Frees the amounts and zeroes the counts.
 */
void
venture_company_rollup_clear(VentureCompanyRollup *rollup);

G_END_DECLS

#endif /* VENTURE_COMPANY_TREE_H */
