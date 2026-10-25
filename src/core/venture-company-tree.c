/*
 * venture-company-tree.c - Parent accounts and branch rollups
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The loop rule is venture_category_check_tree_node(), the one definition
 * of a loop for every self-nesting tree, and the walk down is
 * venture_category_descendants(). What is particular to companies is the
 * rollup: a customer's open deals, won revenue, balance due and open
 * tickets, read from the rows every time so the parent never carries a
 * total that goes stale when a branch's invoice is paid.
 */

#include "venture.h"

#include <string.h>

#define VENTURE_COMPANY_TREE_STATE_KEY "venture-company-tree-installed"

/* Company ids are matched in batches of this many, so an IN list stays well
 * under every backend's bound on bound parameters. */
#define COMPANY_TREE_IN_BATCH (500)

/* ==========================================================================
 * The rule
 * ========================================================================== */

static gint64
company_tree_parent(VentureEntity *company)
{
	gint64 parent_id;

	parent_id = 0;
	g_object_get(company, "parent-id", &parent_id, NULL);

	return parent_id;
}

/*
 * Judged only when the parent is written, the rule every reference
 * follows: a company stored under a parent that was later deleted can
 * still be renamed.
 */
static gboolean
venture_company_tree_validate(
	VentureDatabase	 *database,
	VentureEntity	 *entity,
	VentureEntity	 *previous,
	gpointer	  user_data,
	GError		**error
){
	(void)user_data;

	if ((NULL != previous) &&
	    (company_tree_parent(previous) == company_tree_parent(entity)))
		return TRUE;

	return venture_category_check_tree_node(database, entity, error);
}

void
venture_company_tree_install(VentureContext *context)
{
	VentureDatabase *database;

	g_return_if_fail(VENTURE_IS_CONTEXT(context));

	database = venture_context_get_database(context);

	/* The tests build several contexts over one database; validators are
	 * per database, so the second one must add nothing. */
	if (NULL != g_object_get_data(G_OBJECT(database),
	                              VENTURE_COMPANY_TREE_STATE_KEY))
		return;

	g_object_set_data(G_OBJECT(database), VENTURE_COMPANY_TREE_STATE_KEY,
	                  GINT_TO_POINTER(1));

	venture_database_add_save_validator(database, VENTURE_TYPE_COMPANY,
	                                    venture_company_tree_validate, NULL,
	                                    NULL);
}

/* ==========================================================================
 * The walk and the rollup
 * ========================================================================== */

GPtrArray *
venture_company_tree_children(
	VentureDatabase	 *database,
	gint64		  company_id,
	GError		**error
){
	g_autoptr(VentureQuery) query = NULL;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);

	query = venture_query_new(VENTURE_TYPE_COMPANY);
	venture_query_set_limit(query, 0);
	venture_query_add_order(query, "name", VENTURE_SORT_ASCENDING, NULL);

	if (!venture_query_add_filter_int(query, "parent-id", VENTURE_FILTER_OP_EQ,
	                                  company_id, error))
		return NULL;

	return venture_database_find(database, query, error);
}

/* Adds @amount to *@total when it is in the total's currency; one in
 * another currency is counted in @other instead of converted at a rate
 * nobody chose. */
static gboolean
company_tree_add(
	VentureMoney		**total,
	const VentureMoney	 *amount,
	guint			 *other,
	GError			**error
){
	VentureMoney *sum;

	if (NULL == amount)
		return TRUE;

	if (0 != g_strcmp0(venture_money_get_currency(amount),
	                   venture_money_get_currency(*total)))
	{
		if (NULL != other)
			(*other)++;
		return TRUE;
	}

	sum = venture_money_add(*total, amount, error);

	if (NULL == sum)
		return FALSE;

	venture_money_free(*total);
	*total = sum;

	return TRUE;
}

/* Every row of @type whose company-id is one of @ids. */
static GPtrArray *
company_tree_rows(
	VentureDatabase	 *database,
	GType		  type,
	GArray		 *ids,
	GError		**error
){
	g_autoptr(GPtrArray) all = NULL;
	guint offset;

	all = g_ptr_array_new_with_free_func(g_object_unref);

	for (offset = 0; offset < ids->len; offset += COMPANY_TREE_IN_BATCH)
	{
		g_autoptr(VentureQuery) query = NULL;
		g_autoptr(GPtrArray) values = NULL;
		g_autoptr(GPtrArray) rows = NULL;
		guint i;

		values = g_ptr_array_new_with_free_func(g_free);

		for (i = offset; (i < ids->len) && (i < offset + COMPANY_TREE_IN_BATCH); i++)
			g_ptr_array_add(values, g_strdup_printf("%" G_GINT64_FORMAT,
				g_array_index(ids, gint64, i)));

		query = venture_query_new(type);
		venture_query_set_limit(query, 0);

		if (!venture_query_add_filter(query, "company-id", VENTURE_FILTER_OP_IN,
		                              values, error))
			return NULL;

		rows = venture_database_find(database, query, error);

		if (NULL == rows)
			return NULL;

		for (i = 0; i < rows->len; i++)
			g_ptr_array_add(all, g_object_ref(g_ptr_array_index(rows, i)));
	}

	return g_steal_pointer(&all);
}

gboolean
venture_company_tree_rollup(
	VentureDatabase		 *database,
	gint64			  company_id,
	gboolean		  include_branches,
	VentureCompanyRollup	 *out,
	GError			**error
){
	g_autoptr(VentureEntity) company = NULL;
	g_autoptr(GArray) ids = NULL;
	g_autoptr(GPtrArray) deals = NULL;
	g_autoptr(GPtrArray) invoices = NULL;
	g_autoptr(GPtrArray) tickets = NULL;
	g_autofree gchar *currency = NULL;
	gint64 organization_id;
	guint i;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), FALSE);
	g_return_val_if_fail(NULL != out, FALSE);

	memset(out, 0, sizeof(*out));

	company = venture_database_get(database, VENTURE_TYPE_COMPANY, company_id, error);

	if (NULL == company)
		return FALSE;

	organization_id = venture_entity_get_organization_id(company);

	if (include_branches)
	{
		ids = venture_category_descendants(database, VENTURE_TYPE_COMPANY,
		                                   company_id, TRUE, error);
		if (NULL == ids)
			return FALSE;
	}
	else
	{
		ids = g_array_new(FALSE, FALSE, sizeof(gint64));
		g_array_append_val(ids, company_id);
	}

	currency = venture_database_get_book_currency(database, organization_id);
	out->companies = ids->len;
	out->open_deal_value = venture_money_new_zero(currency);
	out->won_revenue = venture_money_new_zero(currency);
	out->balance_due = venture_money_new_zero(currency);

	deals = company_tree_rows(database, VENTURE_TYPE_DEAL, ids, error);

	if (NULL == deals)
		goto fail;

	for (i = 0; i < deals->len; i++)
	{
		g_autoptr(VentureMoney) value = NULL;
		VentureDealStage stage;

		g_object_get(g_ptr_array_index(deals, i), "stage", &stage, "value", &value, NULL);

		if (VENTURE_DEAL_STAGE_LOST == stage)
			continue;

		if (VENTURE_DEAL_STAGE_WON == stage)
		{
			out->won_deals++;
			if (!company_tree_add(&out->won_revenue, value, &out->other_currency, error))
				goto fail;
		}
		else
		{
			out->open_deals++;
			if (!company_tree_add(&out->open_deal_value, value, &out->other_currency, error))
				goto fail;
		}
	}

	invoices = company_tree_rows(database, VENTURE_TYPE_INVOICE, ids, error);

	if (NULL == invoices)
		goto fail;

	for (i = 0; i < invoices->len; i++)
	{
		VentureInvoiceStatus status;

		g_object_get(g_ptr_array_index(invoices, i), "status", &status, NULL);

		if ((VENTURE_INVOICE_STATUS_SENT == status) ||
		    (VENTURE_INVOICE_STATUS_PARTIALLY_PAID == status))
			out->open_invoices++;
	}

	/* What each company owes, from the receivables ledger rather than the
	 * invoices' own figures, so a payment on account is counted. */
	for (i = 0; i < ids->len; i++)
	{
		g_autoptr(VentureMoney) owed = NULL;

		owed = venture_settlement_service_customer_balance(
			venture_settlement_service_get(database), organization_id,
			g_array_index(ids, gint64, i), NULL, currency, error);

		if (NULL == owed)
			goto fail;

		if (!company_tree_add(&out->balance_due, owed, NULL, error))
			goto fail;
	}

	tickets = company_tree_rows(database, VENTURE_TYPE_TICKET, ids, error);

	if (NULL == tickets)
		goto fail;

	for (i = 0; i < tickets->len; i++)
	{
		VentureTicketStatus status;

		g_object_get(g_ptr_array_index(tickets, i), "status", &status, NULL);

		if ((VENTURE_TICKET_STATUS_DONE != status) &&
		    (VENTURE_TICKET_STATUS_CANCELLED != status))
			out->open_tickets++;
	}

	return TRUE;

fail:
	venture_company_rollup_clear(out);
	return FALSE;
}

void
venture_company_rollup_clear(VentureCompanyRollup *rollup)
{
	g_return_if_fail(NULL != rollup);

	g_clear_pointer(&rollup->open_deal_value, venture_money_free);
	g_clear_pointer(&rollup->won_revenue, venture_money_free);
	g_clear_pointer(&rollup->balance_due, venture_money_free);
	memset(rollup, 0, sizeof(*rollup));
}
