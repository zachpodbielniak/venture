/*
 * venture-headline-reports.c - CAC, churn, LTV and LTV:CAC
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The four numbers a business is run on, from records the other modules
 * already keep: campaign spend and acquisition-flagged expense lines, paid
 * invoices and the cash applied to them, recurring schedules, worklog hours.
 * Every amount is a VentureMoney carried in the organisation's book
 * currency; anything in another currency is counted and reported rather
 * than converted. Every ratio is integer arithmetic in basis points and
 * only becomes a fraction at the moment it is rendered. Every division has
 * its zero named and refused with "n/a" rather than performed.
 */

#include "venture.h"
#include "report/venture-headline-private.h"

#include <string.h>

/* --- Shared plumbing ------------------------------------------------------ */

/* One dated, signed cash movement for a customer: an applied receipt is
 * positive, a refund negative. */
typedef struct
{
	GDateTime	*date;
	VentureMoney	*amount;
} HeadlineCash;

static void
headline_cash_free(gpointer data)
{
	HeadlineCash *cash = data;

	g_clear_pointer(&cash->date, g_date_time_unref);
	g_clear_pointer(&cash->amount, venture_money_free);
	g_free(cash);
}

static gint64
headline_organization(
	VentureContext	*context,
	JsonObject	*options
){
	gint64 organization_id;

	organization_id = (NULL != options)
		? venture_json_object_get_int(options, "organization_id", 0) : 0;

	if (0 == organization_id)
		organization_id = venture_context_get_default_organization_id(context);

	return organization_id;
}

/*
 * The currency every headline total is carried in: the organisation's book
 * currency, or the process default when it has none. Anchoring on the book
 * currency rather than on whichever record sorted first keeps a stray
 * foreign receipt from deciding the currency of the whole report.
 */
static gchar *
headline_currency(
	VentureContext	*context,
	gint64		 organization_id
){
	g_autoptr(VentureEntity) organization = NULL;
	g_autofree gchar *currency = NULL;

	organization = venture_database_get(venture_context_get_database(context),
	                                    VENTURE_TYPE_ORGANIZATION,
	                                    organization_id, NULL);

	if (NULL != organization)
		g_object_get(organization, "default-currency", &currency, NULL);

	if (venture_string_is_empty(currency))
		return g_strdup(venture_money_get_default_currency());

	return g_ascii_strup(currency, -1);
}

/*
 * Adds an amount into a total kept in one currency, counting rather than
 * hiding the ones that cannot join: a different currency, or overflow.
 */
static void
headline_accumulate(
	VentureMoney		**total,
	const VentureMoney	 *amount,
	guint			 *inout_skipped
){
	VentureMoney *next;

	if (NULL == amount)
		return;

	next = venture_money_add(*total, amount, NULL);

	if (NULL == next)
	{
		if (NULL != inout_skipped)
			(*inout_skipped)++;
		return;
	}

	venture_money_free(*total);
	*total = next;
}

static GPtrArray *
headline_fetch(
	VentureContext		 *context,
	GType			  entity_type,
	gint64			  organization_id,
	const gchar		 *date_field,
	VentureDateRange	 *period,
	gboolean		  include_deleted,
	GError			**error
){
	g_autoptr(VentureQuery) query = NULL;

	query = venture_query_new(entity_type);
	venture_query_set_organization(query, organization_id);
	venture_query_set_limit(query, 0);
	venture_query_set_include_deleted(query, include_deleted);

	if (!venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, error))
		return NULL;

	if ((NULL != period) && (NULL != date_field) &&
	    !venture_query_set_date_range(query, date_field, period, error))
		return NULL;

	return venture_database_find(venture_context_get_database(context),
	                             query, error);
}

static void
headline_flag_skipped(
	VentureReportResult	*result,
	guint			 skipped,
	const gchar		*currency
){
	g_autofree gchar *note = NULL;

	if (0 == skipped)
		return;

	note = g_strdup_printf(
		"%u amount%s could not be included -- not in %s, the book "
		"currency these figures are carried in, or arithmetic that would "
		"overflow. Cross-currency totals are refused rather than guessed.",
		skipped, (1 == skipped) ? "" : "s", currency);

	venture_report_result_append_note(result, note);
}

static gboolean
headline_within(
	GDateTime		*when,
	const VentureDateRange	*period
){
	GDateTime *start;
	GDateTime *end;

	if (NULL == when)
		return FALSE;

	if (NULL == period)
		return TRUE;

	start = venture_date_range_get_start(period);
	end = venture_date_range_get_end(period);

	if ((NULL != start) && (g_date_time_compare(when, start) < 0))
		return FALSE;

	if ((NULL != end) && (g_date_time_compare(when, end) >= 0))
		return FALSE;

	return TRUE;
}

/*
 * The end a trailing window is measured back from: the period's end, or
 * now, whichever is earlier -- the same anchor the receivables aging uses,
 * and for the same reason. A period still in progress has an end that has
 * not happened yet.
 */
static GDateTime *
headline_anchor(const VentureDateRange *period)
{
	g_autoptr(GDateTime) now = NULL;
	GDateTime *end;

	now = venture_time_now();
	end = (NULL != period) ? venture_date_range_get_end(period) : NULL;

	if ((NULL == end) || (g_date_time_compare(end, now) > 0))
		return g_steal_pointer(&now);

	return g_date_time_ref(end);
}

/* Basis points of @numerator over @denominator, in integers. The caller
 * has already refused a zero denominator. */
static gint64
headline_basis_points(
	gint64	numerator,
	gint64	denominator
){
	return (numerator * 10000) / denominator;
}

/* Renders basis points as the fraction a ratio metric or a percent column
 * expects. This is the only place a ratio becomes a double, and it is at
 * the presentation boundary, after every computation is done. */
static gdouble
headline_render_bps(gint64 basis_points)
{
	return (gdouble)basis_points / 10000.0;
}

static gchar *
headline_company_name(
	VentureContext	*context,
	gint64		 company_id
){
	g_autoptr(VentureEntity) company = NULL;
	gchar *name = NULL;

	company = venture_database_get(venture_context_get_database(context),
	                               VENTURE_TYPE_COMPANY, company_id, NULL);

	if (NULL != company)
		g_object_get(company, "name", &name, NULL);

	if (venture_string_is_empty(name))
	{
		g_free(name);
		name = g_strdup_printf("Company #%" G_GINT64_FORMAT, company_id);
	}

	return name;
}

/* --- Settings ------------------------------------------------------------- */

static gint64
headline_setting_int(
	VentureContext	*context,
	gint64		 organization_id,
	const gchar	*property,
	gint64		 fallback
){
	g_autoptr(VentureEntity) setting = NULL;
	gint64 value = 0;

	setting = venture_headline_setting_find(
		venture_context_get_database(context), organization_id);

	if (NULL != setting)
		g_object_get(setting, property, &value, NULL);

	return (value > 0) ? value : fallback;
}

static VentureMoney *
headline_setting_rate(
	VentureContext	*context,
	gint64		 organization_id
){
	g_autoptr(VentureEntity) setting = NULL;
	VentureMoney *rate = NULL;

	setting = venture_headline_setting_find(
		venture_context_get_database(context), organization_id);

	if (NULL != setting)
		g_object_get(setting, "hourly-rate", &rate, NULL);

	return rate;
}

/* --- Customer cash -------------------------------------------------------- */

/*
 * Every customer's cash history: applied receipts (allocations funded by a
 * payment, not by a credit note) as positive movements, refunds as negative
 * ones, keyed by company id. This is "paid revenue" throughout: what the
 * customer actually handed over, net of what was handed back.
 */
static GHashTable *
headline_cash_by_customer(
	VentureContext	 *context,
	gint64		  organization_id,
	GError		**error
){
	g_autoptr(GHashTable) cash = NULL;
	g_autoptr(GHashTable) invoice_company = NULL;
	g_autoptr(GPtrArray) invoices = NULL;
	g_autoptr(GPtrArray) allocations = NULL;
	g_autoptr(GPtrArray) refunds = NULL;
	guint i;

	invoices = headline_fetch(context, VENTURE_TYPE_INVOICE, organization_id,
	                          NULL, NULL, TRUE, error);

	if (NULL == invoices)
		return NULL;

	allocations = headline_fetch(context, VENTURE_TYPE_PAYMENT_ALLOCATION,
	                             organization_id, NULL, NULL, TRUE, error);

	if (NULL == allocations)
		return NULL;

	refunds = headline_fetch(context, VENTURE_TYPE_REFUND, organization_id,
	                         NULL, NULL, TRUE, error);

	if (NULL == refunds)
		return NULL;

	invoice_company = g_hash_table_new(g_direct_hash, g_direct_equal);

	for (i = 0; i < invoices->len; i++)
	{
		VentureEntity *invoice;
		gint64 company_id = 0;

		invoice = g_ptr_array_index(invoices, i);
		g_object_get(invoice, "company-id", &company_id, NULL);
		g_hash_table_insert(invoice_company,
			GINT_TO_POINTER((gint)venture_entity_get_id(invoice)),
			GINT_TO_POINTER((gint)company_id));
	}

	cash = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL,
	                             (GDestroyNotify)g_ptr_array_unref);

	for (i = 0; i < allocations->len + refunds->len; i++)
	{
		g_autoptr(GDateTime) date = NULL;
		g_autoptr(VentureMoney) amount = NULL;
		VentureEntity *record;
		HeadlineCash *movement;
		GPtrArray *history;
		gint64 company_id = 0;
		gboolean refund;

		refund = (i >= allocations->len);
		record = refund ? g_ptr_array_index(refunds, i - allocations->len)
		                : g_ptr_array_index(allocations, i);

		if (refund)
		{
			g_object_get(record, "customer-id", &company_id, "date", &date,
			             "amount", &amount, NULL);
		}
		else
		{
			gint64 payment_id = 0;
			gint64 invoice_id = 0;

			g_object_get(record, "payment-id", &payment_id,
			             "invoice-id", &invoice_id, "date", &date,
			             "amount", &amount, NULL);

			/* A credit note applied to an invoice is not cash the
			 * customer paid; only a receipt is. */
			if (0 == payment_id)
				continue;

			company_id = GPOINTER_TO_INT(g_hash_table_lookup(
				invoice_company, GINT_TO_POINTER((gint)invoice_id)));
		}

		if ((0 == company_id) || (NULL == date) || (NULL == amount))
			continue;

		history = g_hash_table_lookup(cash, GINT_TO_POINTER((gint)company_id));

		if (NULL == history)
		{
			history = g_ptr_array_new_with_free_func(headline_cash_free);
			g_hash_table_insert(cash, GINT_TO_POINTER((gint)company_id),
			                    history);
		}

		movement = g_new0(HeadlineCash, 1);
		movement->date = g_date_time_ref(date);
		movement->amount = refund ? venture_money_negate(amount)
		                          : venture_money_copy(amount);
		g_ptr_array_add(history, movement);
	}

	return g_steal_pointer(&cash);
}

/*
 * When each company first had an invoice reach paid, keyed by company id.
 * A company's first paid invoice is the moment it became a customer.
 */
static GHashTable *
headline_first_paid(
	VentureContext	 *context,
	gint64		  organization_id,
	GError		**error
){
	g_autoptr(GHashTable) first = NULL;
	g_autoptr(GPtrArray) invoices = NULL;
	guint i;

	invoices = headline_fetch(context, VENTURE_TYPE_INVOICE, organization_id,
	                          NULL, NULL, TRUE, error);

	if (NULL == invoices)
		return NULL;

	first = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL,
	                              (GDestroyNotify)g_date_time_unref);

	for (i = 0; i < invoices->len; i++)
	{
		g_autoptr(GDateTime) paid_at = NULL;
		VentureEntity *invoice;
		GDateTime *known;
		VentureInvoiceStatus status;
		gint64 company_id = 0;

		invoice = g_ptr_array_index(invoices, i);
		g_object_get(invoice, "status", &status, "company-id", &company_id,
		             "paid-at", &paid_at, NULL);

		if ((VENTURE_INVOICE_STATUS_PAID != status) || (0 == company_id) ||
		    (NULL == paid_at))
			continue;

		known = g_hash_table_lookup(first, GINT_TO_POINTER((gint)company_id));

		if ((NULL == known) || (g_date_time_compare(paid_at, known) < 0))
			g_hash_table_insert(first, GINT_TO_POINTER((gint)company_id),
			                    g_date_time_ref(paid_at));
	}

	return g_steal_pointer(&first);
}

/* ==========================================================================
 * CAC
 * ========================================================================== */

typedef struct
{
	gchar		*source;
	gint64		 campaign_id;
	gchar		*campaign;
	gint64		 customers;
	VentureMoney	*spend;
} CacGroup;

static void
cac_group_free(gpointer data)
{
	CacGroup *group = data;

	g_free(group->source);
	g_free(group->campaign);
	g_clear_pointer(&group->spend, venture_money_free);
	g_free(group);
}

/*
 * Acquisition spend in the period: campaign spend for campaigns that started
 * in it, expenses flagged acquisition dated in it, and acquisition-flagged
 * lines of bills dated in it that are not drafts or void.
 */
static gboolean
cac_spend(
	VentureContext		 *context,
	gint64			  organization_id,
	VentureDateRange	 *period,
	const gchar		 *currency,
	VentureMoney		**out_campaign_spend,
	VentureMoney		**out_expense_spend,
	GHashTable		 *campaign_spend_by_id,
	guint			 *inout_skipped,
	GError			**error
){
	g_autoptr(GPtrArray) campaigns = NULL;
	g_autoptr(GPtrArray) expenses = NULL;
	g_autoptr(VentureMoney) from_campaigns = NULL;
	g_autoptr(VentureMoney) from_expenses = NULL;
	guint i;

	from_campaigns = venture_money_new_zero(currency);
	from_expenses = venture_money_new_zero(currency);

	if (venture_context_module_enabled(context, "outreach"))
	{
		campaigns = headline_fetch(context, VENTURE_TYPE_CAMPAIGN,
		                           organization_id, "started-at", period,
		                           FALSE, error);

		if (NULL == campaigns)
			return FALSE;

		for (i = 0; i < campaigns->len; i++)
		{
			g_autoptr(VentureMoney) spend = NULL;
			VentureEntity *campaign;

			campaign = g_ptr_array_index(campaigns, i);
			g_object_get(campaign, "spend", &spend, NULL);

			if (NULL == spend)
				continue;

			headline_accumulate(&from_campaigns, spend, inout_skipped);
			g_hash_table_insert(campaign_spend_by_id,
				GINT_TO_POINTER((gint)venture_entity_get_id(campaign)),
				g_steal_pointer(&spend));
		}
	}

	expenses = headline_fetch(context, VENTURE_TYPE_EXPENSE, organization_id,
	                          "occurred-at", period, FALSE, error);

	if (NULL == expenses)
		return FALSE;

	for (i = 0; i < expenses->len; i++)
	{
		g_autoptr(VentureMoney) amount = NULL;
		gboolean acquisition = FALSE;

		g_object_get(g_ptr_array_index(expenses, i), "acquisition",
		             &acquisition, "amount", &amount, NULL);

		if (acquisition)
			headline_accumulate(&from_expenses, amount, inout_skipped);
	}

	if (venture_context_module_enabled(context, "payables"))
	{
		g_autoptr(GPtrArray) lines = NULL;
		g_autoptr(GHashTable) bills_in_period = NULL;
		g_autoptr(GPtrArray) bills = NULL;

		bills = headline_fetch(context, VENTURE_TYPE_VENDOR_BILL,
		                       organization_id, "bill-date", period, FALSE,
		                       error);

		if (NULL == bills)
			return FALSE;

		bills_in_period = g_hash_table_new(g_direct_hash, g_direct_equal);

		for (i = 0; i < bills->len; i++)
		{
			g_autofree gchar *status = NULL;
			VentureEntity *bill;

			bill = g_ptr_array_index(bills, i);
			g_object_get(bill, "status", &status, NULL);

			if ((0 == g_strcmp0(status, "draft")) ||
			    (0 == g_strcmp0(status, "void")))
				continue;

			g_hash_table_add(bills_in_period,
				GINT_TO_POINTER((gint)venture_entity_get_id(bill)));
		}

		lines = headline_fetch(context, VENTURE_TYPE_VENDOR_BILL_LINE,
		                       organization_id, NULL, NULL, FALSE, error);

		if (NULL == lines)
			return FALSE;

		for (i = 0; i < lines->len; i++)
		{
			g_autoptr(VentureMoney) amount = NULL;
			VentureEntity *line;
			gboolean acquisition = FALSE;
			gint64 bill_id = 0;

			line = g_ptr_array_index(lines, i);
			g_object_get(line, "acquisition", &acquisition, "bill-id",
			             &bill_id, NULL);

			if (!acquisition ||
			    !g_hash_table_contains(bills_in_period,
			                           GINT_TO_POINTER((gint)bill_id)))
				continue;

			amount = venture_vendor_bill_line_get_amount(
				VENTURE_VENDOR_BILL_LINE(line), NULL);

			if (NULL == amount)
			{
				(*inout_skipped)++;
				continue;
			}

			headline_accumulate(&from_expenses, amount, inout_skipped);
		}
	}

	*out_campaign_spend = g_steal_pointer(&from_campaigns);
	*out_expense_spend = g_steal_pointer(&from_expenses);

	return TRUE;
}

static VentureReportResult *
venture_report_cac(
	VentureContext		 *context,
	VentureDateRange	 *period,
	JsonObject		 *options,
	GError			**error
){
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(GHashTable) first_paid = NULL;
	g_autoptr(GHashTable) campaign_spend = NULL;
	g_autoptr(GHashTable) lead_by_company = NULL;
	g_autoptr(GPtrArray) leads = NULL;
	g_autoptr(GPtrArray) groups = NULL;
	g_autoptr(VentureMoney) from_campaigns = NULL;
	g_autoptr(VentureMoney) from_expenses = NULL;
	g_autoptr(VentureMoney) spend = NULL;
	g_autoptr(VentureMoney) cac = NULL;
	g_autofree gchar *currency = NULL;
	GHashTableIter iter;
	gpointer key;
	gpointer value;
	gint64 organization_id;
	gint64 new_customers;
	guint skipped;
	guint i;

	organization_id = headline_organization(context, options);
	currency = headline_currency(context, organization_id);
	skipped = 0;

	campaign_spend = g_hash_table_new_full(g_direct_hash, g_direct_equal,
	                                       NULL,
	                                       (GDestroyNotify)venture_money_free);

	if (!cac_spend(context, organization_id, period, currency,
	               &from_campaigns, &from_expenses, campaign_spend, &skipped,
	               error))
		return NULL;

	spend = venture_money_add(from_campaigns, from_expenses, NULL);

	if (NULL == spend)
		spend = venture_money_copy(from_expenses);

	first_paid = headline_first_paid(context, organization_id, error);

	if (NULL == first_paid)
		return NULL;

	/* The converted lead behind each new customer, for the breakdown. */
	leads = headline_fetch(context, VENTURE_TYPE_LEAD, organization_id, NULL,
	                       NULL, FALSE, error);

	if (NULL == leads)
		return NULL;

	lead_by_company = g_hash_table_new(g_direct_hash, g_direct_equal);

	for (i = 0; i < leads->len; i++)
	{
		VentureEntity *lead;
		VentureLeadStatus status;
		gint64 company_id = 0;

		lead = g_ptr_array_index(leads, i);
		g_object_get(lead, "status", &status, "converted-company-id",
		             &company_id, NULL);

		if ((VENTURE_LEAD_CONVERTED == status) && (0 != company_id) &&
		    !g_hash_table_contains(lead_by_company,
		                           GINT_TO_POINTER((gint)company_id)))
			g_hash_table_insert(lead_by_company,
			                    GINT_TO_POINTER((gint)company_id), lead);
	}

	groups = g_ptr_array_new_with_free_func(cac_group_free);
	new_customers = 0;

	g_hash_table_iter_init(&iter, first_paid);

	while (g_hash_table_iter_next(&iter, &key, &value))
	{
		g_autofree gchar *source = NULL;
		g_autoptr(VentureEntity) company = NULL;
		VentureEntity *lead;
		CacGroup *group;
		gint64 campaign_id = 0;
		guint j;

		if (!headline_within(value, period))
			continue;

		new_customers++;
		lead = g_hash_table_lookup(lead_by_company, key);

		if (NULL != lead)
		{
			g_object_get(lead, "source", &source, "campaign-id",
			             &campaign_id, NULL);
		}
		else
		{
			/* No lead behind it: the company's own source, if it
			 * carries one, is the next best attribution. */
			company = venture_database_get(
				venture_context_get_database(context),
				VENTURE_TYPE_COMPANY, GPOINTER_TO_INT(key), NULL);

			if (NULL != company)
				g_object_get(company, "source", &source, NULL);
		}

		if (venture_string_is_empty(source))
		{
			g_free(source);
			source = g_strdup("(unattributed)");
		}

		group = NULL;

		for (j = 0; j < groups->len; j++)
		{
			CacGroup *candidate;

			candidate = g_ptr_array_index(groups, j);

			if ((candidate->campaign_id == campaign_id) &&
			    (0 == g_strcmp0(candidate->source, source)))
			{
				group = candidate;
				break;
			}
		}

		if (NULL == group)
		{
			group = g_new0(CacGroup, 1);
			group->source = g_strdup(source);
			group->campaign_id = campaign_id;

			if (0 != campaign_id)
			{
				g_autoptr(VentureEntity) campaign = NULL;
				VentureMoney *known;

				campaign = venture_database_get(
					venture_context_get_database(context),
					VENTURE_TYPE_CAMPAIGN, campaign_id, NULL);

				if (NULL != campaign)
					g_object_get(campaign, "name", &group->campaign, NULL);

				known = g_hash_table_lookup(campaign_spend,
					GINT_TO_POINTER((gint)campaign_id));

				if (NULL != known)
					group->spend = venture_money_copy(known);
			}

			g_ptr_array_add(groups, group);
		}

		group->customers++;
	}

	result = venture_report_result_new("Customer acquisition cost", period);

	venture_report_result_add_metric(result,
		venture_metric_new_money("spend", "Acquisition spend", spend));
	venture_report_result_add_metric(result,
		venture_metric_new_money("campaign_spend", "Campaign spend",
		                         from_campaigns));
	venture_report_result_add_metric(result,
		venture_metric_new_money("expense_spend", "Flagged expense spend",
		                         from_expenses));
	venture_report_result_add_metric(result,
		venture_metric_new_count("new_customers", "New customers",
		                         new_customers));

	if (new_customers > 0)
	{
		VentureMetric *metric;

		cac = venture_money_multiply_rational(spend, 1, new_customers, NULL);
		metric = venture_metric_new_money("cac", "CAC", cac);
		venture_metric_set_higher_is_better(metric, FALSE);
		venture_report_result_add_metric(result, metric);
	}
	else
	{
		venture_report_result_add_metric(result,
			venture_metric_new_text("cac", "CAC", "n/a"));
		venture_report_result_append_note(result,
			"No company had its first paid invoice in this period, so "
			"there is nothing to divide the spend by. CAC is n/a, not "
			"zero.");
	}

	venture_report_result_add_column(result, "source", "Source",
	                                 VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "campaign", "Campaign",
	                                 VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "new_customers", "New customers",
	                                 VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "spend", "Campaign spend",
	                                 VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "cac", "CAC",
	                                 VENTURE_REPORT_COLUMN_MONEY);

	for (i = 0; i < groups->len; i++)
	{
		CacGroup *group;

		group = g_ptr_array_index(groups, i);

		venture_report_result_begin_row(result);
		venture_report_result_set_text(result, "source", group->source);
		venture_report_result_set_text(result, "campaign",
			(NULL != group->campaign) ? group->campaign : "");
		venture_report_result_set_number(result, "new_customers",
		                                 (gdouble)group->customers);

		if (NULL != group->spend)
		{
			g_autoptr(VentureMoney) group_cac = NULL;

			group_cac = venture_money_multiply_rational(group->spend, 1,
			                                            group->customers,
			                                            NULL);
			venture_report_result_set_money(result, "spend", group->spend);
			venture_report_result_set_money(result, "cac", group_cac);
		}
	}

	if (!venture_context_module_enabled(context, "outreach"))
		venture_report_result_append_note(result,
			"The outreach module is off, so no campaign spend is counted; "
			"only flagged expense and bill lines are.");

	headline_flag_skipped(result, skipped, currency);

	return g_steal_pointer(&result);
}

/* ==========================================================================
 * Churn
 * ========================================================================== */

typedef struct
{
	gint64		 active_at_start;
	gint64		 churned;
	gint64		 active_12m;
	gint64		 inactive;
} ChurnFigures;

/*
 * Activity churn over a trailing window: customers with paid revenue in the
 * twelve months before the anchor, split into those who paid within the
 * last @days and those who did not. The rows behind the numbers go on the
 * result when one is given.
 */
static gboolean
churn_activity(
	VentureContext		 *context,
	gint64			  organization_id,
	VentureDateRange	 *period,
	gint64			  days,
	ChurnFigures		 *figures,
	VentureReportResult	 *result,
	GError			**error
){
	g_autoptr(GHashTable) cash = NULL;
	g_autoptr(GDateTime) anchor = NULL;
	g_autoptr(GDateTime) window_start = NULL;
	g_autoptr(GDateTime) cutoff = NULL;
	GHashTableIter iter;
	gpointer key;
	gpointer value;

	cash = headline_cash_by_customer(context, organization_id, error);

	if (NULL == cash)
		return FALSE;

	anchor = headline_anchor(period);
	window_start = g_date_time_add_months(anchor, -12);
	cutoff = g_date_time_add_days(anchor, -(gint)days);

	g_hash_table_iter_init(&iter, cash);

	while (g_hash_table_iter_next(&iter, &key, &value))
	{
		GPtrArray *history = value;
		GDateTime *last = NULL;
		gboolean in_window = FALSE;
		guint i;

		for (i = 0; i < history->len; i++)
		{
			HeadlineCash *movement;

			movement = g_ptr_array_index(history, i);

			if (venture_money_is_negative(movement->amount))
				continue;

			if ((g_date_time_compare(movement->date, window_start) < 0) ||
			    (g_date_time_compare(movement->date, anchor) >= 0))
				continue;

			in_window = TRUE;

			if ((NULL == last) ||
			    (g_date_time_compare(movement->date, last) > 0))
				last = movement->date;
		}

		if (!in_window)
			continue;

		figures->active_12m++;

		if (g_date_time_compare(last, cutoff) < 0)
		{
			figures->inactive++;

			if (NULL != result)
			{
				g_autofree gchar *name = NULL;
				g_autofree gchar *when = NULL;

				name = headline_company_name(context,
				                             GPOINTER_TO_INT(key));
				when = venture_time_to_date_string(last, NULL);
				venture_report_result_begin_row(result);
				venture_report_result_set_text(result, "kind", "activity");
				venture_report_result_set_text(result, "customer", name);
				venture_report_result_set_text(result, "detail",
					"no paid revenue since");
				venture_report_result_set_text(result, "date", when);
			}
		}
	}

	return TRUE;
}

static VentureReportResult *
venture_report_churn(
	VentureContext		 *context,
	VentureDateRange	 *period,
	JsonObject		 *options,
	GError			**error
){
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(GPtrArray) schedules = NULL;
	g_autofree gchar *days_note = NULL;
	ChurnFigures figures = { 0, 0, 0, 0 };
	GDateTime *start;
	gint64 organization_id;
	gint64 days;
	guint i;

	organization_id = headline_organization(context, options);
	days = (NULL != options) ? venture_json_object_get_int(options, "days", 0)
	                         : 0;

	if (days <= 0)
		days = headline_setting_int(context, organization_id,
		                            "activity-days", 90);

	result = venture_report_result_new("Churn", period);

	venture_report_result_add_column(result, "kind", "Kind",
	                                 VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "customer", "Customer",
	                                 VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "detail", "What happened",
	                                 VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "date", "Date",
	                                 VENTURE_REPORT_COLUMN_TEXT);

	/*
	 * Recurring churn: invoice schedules that were running when the period
	 * opened and were cancelled (deleted) or lapsed (reached their end)
	 * inside it, over the ones running when it opened.
	 */
	start = (NULL != period) ? venture_date_range_get_start(period) : NULL;

	if (venture_context_module_enabled(context, "recurring") && (NULL != start))
	{
		schedules = headline_fetch(context, VENTURE_TYPE_RECURRING_SCHEDULE,
		                           organization_id, NULL, NULL, TRUE, error);

		if (NULL == schedules)
			return NULL;

		for (i = 0; i < schedules->len; i++)
		{
			g_autoptr(GDateTime) started = NULL;
			g_autoptr(GDateTime) ends = NULL;
			g_autofree gchar *template_text = NULL;
			g_autoptr(JsonNode) template_node = NULL;
			VentureEntity *schedule;
			gint kind = 0;
			GDateTime *deleted;
			const gchar *what = NULL;
			GDateTime *when = NULL;

			schedule = g_ptr_array_index(schedules, i);
			g_object_get(schedule, "kind", &kind, "start-at", &started,
			             "end-at", &ends, NULL);
			deleted = venture_entity_get_deleted_at(schedule);

			/* Enum value 0 is the invoice kind: the only schedule that
			 * is a customer subscription rather than a cost. */
			if ((0 != kind) ||
			    (NULL == started) ||
			    (g_date_time_compare(started, start) >= 0))
				continue;

			if ((NULL != deleted) && (g_date_time_compare(deleted, start) < 0))
				continue;

			if ((NULL != ends) && (g_date_time_compare(ends, start) < 0))
				continue;

			figures.active_at_start++;

			if ((NULL != deleted) && headline_within(deleted, period))
			{
				what = "cancelled";
				when = deleted;
			}
			else if ((NULL != ends) && headline_within(ends, period))
			{
				what = "lapsed";
				when = ends;
			}

			if (NULL == what)
				continue;

			figures.churned++;

			{
				g_autofree gchar *name = NULL;
				g_autofree gchar *date = NULL;
				g_autofree gchar *customer = NULL;
				gint64 company_id = 0;

				g_object_get(schedule, "name", &name, "template",
				             &template_text, NULL);

				if (!venture_string_is_empty(template_text))
					template_node = venture_json_parse(template_text, NULL);

				if ((NULL != template_node) &&
				    JSON_NODE_HOLDS_OBJECT(template_node))
					company_id = venture_json_object_get_int(
						json_node_get_object(template_node),
						"company_id", 0);

				customer = (0 != company_id)
					? headline_company_name(context, company_id)
					: g_strdup(name);
				date = venture_time_to_date_string(when, NULL);
				venture_report_result_begin_row(result);
				venture_report_result_set_text(result, "kind", "recurring");
				venture_report_result_set_text(result, "customer", customer);
				venture_report_result_set_text(result, "detail", what);
				venture_report_result_set_text(result, "date", date);
			}
		}
	}

	if (!churn_activity(context, organization_id, period, days, &figures,
	                    result, error))
		return NULL;

	venture_report_result_add_metric(result,
		venture_metric_new_count("active_at_start",
		                         "Schedules active at start",
		                         figures.active_at_start));
	venture_report_result_add_metric(result,
		venture_metric_new_count("churned", "Schedules cancelled or lapsed",
		                         figures.churned));

	if (figures.active_at_start > 0)
	{
		VentureMetric *metric;

		metric = venture_metric_new_ratio("recurring_churn", "Recurring churn",
			headline_render_bps(headline_basis_points(
				figures.churned, figures.active_at_start)));
		venture_metric_set_higher_is_better(metric, FALSE);
		venture_report_result_add_metric(result, metric);
	}
	else
	{
		venture_report_result_add_metric(result,
			venture_metric_new_text("recurring_churn", "Recurring churn",
			                        "n/a"));
		venture_report_result_append_note(result,
			venture_context_module_enabled(context, "recurring")
			? "No invoice schedule was running when the period opened, so "
			  "recurring churn has no denominator and is n/a."
			: "The recurring module is off, so recurring churn is n/a.");
	}

	venture_report_result_add_metric(result,
		venture_metric_new_count("active_customers",
		                         "Customers paid in trailing 12 months",
		                         figures.active_12m));
	venture_report_result_add_metric(result,
		venture_metric_new_count("inactive_customers", "Of which gone quiet",
		                         figures.inactive));

	if (figures.active_12m > 0)
	{
		VentureMetric *metric;

		metric = venture_metric_new_ratio("activity_churn", "Activity churn",
			headline_render_bps(headline_basis_points(
				figures.inactive, figures.active_12m)));
		venture_metric_set_higher_is_better(metric, FALSE);
		venture_report_result_add_metric(result, metric);
	}
	else
	{
		venture_report_result_add_metric(result,
			venture_metric_new_text("activity_churn", "Activity churn",
			                        "n/a"));
		venture_report_result_append_note(result,
			"No customer paid anything in the trailing twelve months, so "
			"activity churn has no denominator and is n/a.");
	}

	venture_report_result_add_metric(result,
		venture_metric_new_count("days", "Quiet days threshold", days));

	days_note = g_strdup_printf(
		"Activity churn counts customers with paid revenue in the twelve "
		"months before the period's end and none in its last %"
		G_GINT64_FORMAT " days. Pass days=N to change the threshold.",
		days);
	venture_report_result_append_note(result, days_note);

	return g_steal_pointer(&result);
}

/* ==========================================================================
 * LTV
 * ========================================================================== */

typedef struct
{
	gint64		 company_id;
	VentureMoney	*realised;
	VentureMoney	*support_cost;
} LtvCustomer;

static void
ltv_customer_free(gpointer data)
{
	LtvCustomer *customer = data;

	g_clear_pointer(&customer->realised, venture_money_free);
	g_clear_pointer(&customer->support_cost, venture_money_free);
	g_free(customer);
}

static gint
ltv_customer_compare(
	gconstpointer	a,
	gconstpointer	b
){
	const LtvCustomer *left = *(LtvCustomer *const *)a;
	const LtvCustomer *right = *(LtvCustomer *const *)b;

	gint order;

	/* Largest first; ties by id so the order is stable. */
	order = venture_money_compare(right->realised, left->realised);

	if (0 != order)
		return order;

	return (left->company_id < right->company_id) ? -1
		: ((left->company_id > right->company_id) ? 1 : 0);
}

/*
 * Support cost per customer: worklog hours on tickets that name a company,
 * priced at the configured hourly rate. Hours are stored as a double by the
 * ticket module; they are rounded once to hundredths of an hour and from
 * then on the arithmetic is exact money times a rational.
 */
static GHashTable *
ltv_support_cost(
	VentureContext		 *context,
	gint64			  organization_id,
	const VentureDateRange	 *window,
	const VentureMoney	 *rate,
	guint			 *inout_skipped,
	GError			**error
){
	g_autoptr(GHashTable) cost = NULL;
	g_autoptr(GHashTable) ticket_company = NULL;
	g_autoptr(GPtrArray) tickets = NULL;
	g_autoptr(GPtrArray) worklogs = NULL;
	guint i;

	cost = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL,
	                             (GDestroyNotify)venture_money_free);

	if ((NULL == rate) || venture_money_is_zero(rate) ||
	    !venture_context_module_enabled(context, "tickets"))
		return g_steal_pointer(&cost);

	tickets = headline_fetch(context, VENTURE_TYPE_TICKET, organization_id,
	                         NULL, NULL, TRUE, error);

	if (NULL == tickets)
		return NULL;

	ticket_company = g_hash_table_new(g_direct_hash, g_direct_equal);

	for (i = 0; i < tickets->len; i++)
	{
		VentureEntity *ticket;
		gint64 company_id = 0;

		ticket = g_ptr_array_index(tickets, i);
		g_object_get(ticket, "company-id", &company_id, NULL);

		if (0 != company_id)
			g_hash_table_insert(ticket_company,
				GINT_TO_POINTER((gint)venture_entity_get_id(ticket)),
				GINT_TO_POINTER((gint)company_id));
	}

	worklogs = headline_fetch(context, VENTURE_TYPE_WORKLOG, organization_id,
	                          "occurred-at", (VentureDateRange *)window,
	                          FALSE, error);

	if (NULL == worklogs)
		return NULL;

	for (i = 0; i < worklogs->len; i++)
	{
		g_autoptr(VentureMoney) line = NULL;
		VentureMoney *total;
		gdouble hours = 0.0;
		gint64 hundredths;
		gint64 ticket_id = 0;
		gint64 company_id;

		g_object_get(g_ptr_array_index(worklogs, i), "ticket-id",
		             &ticket_id, "hours", &hours, NULL);
		company_id = GPOINTER_TO_INT(g_hash_table_lookup(
			ticket_company, GINT_TO_POINTER((gint)ticket_id)));

		if ((0 == company_id) || (hours <= 0.0))
			continue;

		hundredths = (gint64)(hours * 100.0 + 0.5);
		line = venture_money_multiply_rational(rate, hundredths, 100, NULL);

		if (NULL == line)
		{
			(*inout_skipped)++;
			continue;
		}

		total = g_hash_table_lookup(cost, GINT_TO_POINTER((gint)company_id));

		if (NULL == total)
		{
			g_hash_table_insert(cost, GINT_TO_POINTER((gint)company_id),
			                    g_steal_pointer(&line));
			continue;
		}

		headline_accumulate(&total, line, inout_skipped);
		g_hash_table_steal(cost, GINT_TO_POINTER((gint)company_id));
		g_hash_table_insert(cost, GINT_TO_POINTER((gint)company_id), total);
	}

	return g_steal_pointer(&cost);
}

/*
 * The P&L over @window, read from the pnl report itself so the margin here
 * is the margin the P&L page shows, not a second opinion.
 */
static gboolean
ltv_pnl(
	VentureContext		 *context,
	gint64			  organization_id,
	VentureDateRange	 *window,
	VentureMoney		**out_revenue,
	VentureMoney		**out_expenses,
	GError			**error
){
	g_autoptr(VentureReportResult) pnl = NULL;
	g_autoptr(JsonObject) options = NULL;
	VentureReport *report;
	GPtrArray *metrics;
	guint i;

	report = venture_report_registry_lookup(
		venture_context_get_report_registry(context), "pnl");

	if (NULL == report)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		                    "The pnl report is not available, so gross "
		                    "margin cannot be read");
		return FALSE;
	}

	options = json_object_new();
	json_object_set_int_member(options, "organization_id", organization_id);
	pnl = venture_report_generate(report, context, window, options, error);

	if (NULL == pnl)
		return FALSE;

	metrics = venture_report_result_get_metrics(pnl);

	for (i = 0; i < metrics->len; i++)
	{
		VentureMetric *metric;
		const gchar *key;

		metric = g_ptr_array_index(metrics, i);
		key = venture_metric_get_key(metric);

		if ((0 == g_strcmp0(key, "revenue")) &&
		    (NULL != venture_metric_get_money(metric)))
			*out_revenue = venture_money_copy(venture_metric_get_money(metric));
		else if ((0 == g_strcmp0(key, "expenses")) &&
		         (NULL != venture_metric_get_money(metric)))
			*out_expenses = venture_money_copy(venture_metric_get_money(metric));
	}

	return TRUE;
}

static VentureReportResult *
venture_report_ltv(
	VentureContext		 *context,
	VentureDateRange	 *period,
	JsonObject		 *options,
	GError			**error
){
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(GHashTable) cash = NULL;
	g_autoptr(GHashTable) support = NULL;
	g_autoptr(GHashTable) customer_months = NULL;
	g_autoptr(GPtrArray) customers = NULL;
	g_autoptr(VentureMoney) rate = NULL;
	g_autoptr(VentureMoney) total = NULL;
	g_autoptr(VentureMoney) mean = NULL;
	g_autoptr(VentureMoney) median = NULL;
	g_autoptr(VentureMoney) revenue_12m = NULL;
	g_autoptr(VentureMoney) pnl_revenue = NULL;
	g_autoptr(VentureMoney) pnl_expenses = NULL;
	g_autoptr(VentureMoney) support_total = NULL;
	g_autoptr(VentureMoney) arpu = NULL;
	g_autoptr(VentureMoney) projected = NULL;
	g_autoptr(VentureDateRange) window = NULL;
	g_autoptr(GDateTime) anchor = NULL;
	g_autoptr(GDateTime) window_start = NULL;
	g_autofree gchar *currency = NULL;
	ChurnFigures churn = { 0, 0, 0, 0 };
	GHashTableIter iter;
	gpointer key;
	gpointer value;
	gint64 organization_id;
	gint64 minimum_months;
	gint64 days;
	gint64 months;
	gint64 margin_bps;
	gboolean margin_known;
	const gchar *withheld;
	guint skipped;
	guint i;

	organization_id = headline_organization(context, options);
	currency = headline_currency(context, organization_id);
	minimum_months = headline_setting_int(context, organization_id,
	                                      "minimum-customer-months", 12);
	days = (NULL != options) ? venture_json_object_get_int(options, "days", 0)
	                         : 0;

	if (days <= 0)
		days = headline_setting_int(context, organization_id,
		                            "activity-days", 90);

	rate = headline_setting_rate(context, organization_id);
	skipped = 0;

	cash = headline_cash_by_customer(context, organization_id, error);

	if (NULL == cash)
		return NULL;

	anchor = headline_anchor(period);
	window_start = g_date_time_add_months(anchor, -12);
	window = venture_date_range_new_labelled(window_start, anchor,
	                                         "Trailing twelve months");

	support = ltv_support_cost(context, organization_id, window, rate,
	                           &skipped, error);

	if (NULL == support)
		return NULL;

	/* Realised: each customer's lifetime cash net of refunds. And, for the
	 * projection, the trailing year's revenue and the distinct
	 * customer-months it was earned across. */
	customers = g_ptr_array_new_with_free_func(ltv_customer_free);
	customer_months = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                        NULL);
	total = venture_money_new_zero(currency);
	revenue_12m = venture_money_new_zero(currency);
	support_total = venture_money_new_zero(currency);

	g_hash_table_iter_init(&iter, cash);

	while (g_hash_table_iter_next(&iter, &key, &value))
	{
		GPtrArray *history = value;
		LtvCustomer *customer;
		VentureMoney *cost;

		customer = g_new0(LtvCustomer, 1);
		customer->company_id = GPOINTER_TO_INT(key);
		customer->realised = venture_money_new_zero(currency);

		for (i = 0; i < history->len; i++)
		{
			HeadlineCash *movement;

			movement = g_ptr_array_index(history, i);
			headline_accumulate(&customer->realised, movement->amount,
			                    &skipped);

			if ((g_date_time_compare(movement->date, window_start) >= 0) &&
			    (g_date_time_compare(movement->date, anchor) < 0))
			{
				headline_accumulate(&revenue_12m, movement->amount, NULL);

				if (!venture_money_is_negative(movement->amount))
					g_hash_table_add(customer_months,
						g_strdup_printf("%d:%04d-%02d",
							GPOINTER_TO_INT(key),
							g_date_time_get_year(movement->date),
							g_date_time_get_month(movement->date)));
			}
		}

		cost = g_hash_table_lookup(support, key);

		if (NULL != cost)
		{
			customer->support_cost = venture_money_copy(cost);
			headline_accumulate(&support_total, cost, &skipped);
		}

		headline_accumulate(&total, customer->realised, &skipped);
		g_ptr_array_add(customers, customer);
	}

	g_ptr_array_sort(customers, ltv_customer_compare);

	if (customers->len > 0)
	{
		LtvCustomer *middle;

		mean = venture_money_multiply_rational(total, 1, customers->len, NULL);
		middle = g_ptr_array_index(customers, customers->len / 2);

		if (0 == (customers->len % 2))
		{
			g_autoptr(VentureMoney) pair = NULL;
			LtvCustomer *upper;

			upper = g_ptr_array_index(customers, customers->len / 2 - 1);
			pair = venture_money_add(middle->realised, upper->realised, NULL);
			median = (NULL != pair)
				? venture_money_multiply_rational(pair, 1, 2, NULL)
				: venture_money_copy(middle->realised);
		}
		else
		{
			median = venture_money_copy(middle->realised);
		}
	}

	if (NULL == mean)
		mean = venture_money_new_zero(currency);

	if (NULL == median)
		median = venture_money_new_zero(currency);

	/* Projected: ARPU per customer-month x gross margin / monthly churn. */
	if (!ltv_pnl(context, organization_id, window, &pnl_revenue,
	             &pnl_expenses, error))
		return NULL;

	if (!churn_activity(context, organization_id, period, days, &churn, NULL,
	                    error))
		return NULL;

	months = (gint64)g_hash_table_size(customer_months);
	margin_known = FALSE;
	margin_bps = 0;
	withheld = NULL;

	if ((NULL != pnl_revenue) && !venture_money_is_zero(pnl_revenue) &&
	    (NULL != pnl_expenses))
	{
		g_autoptr(VentureMoney) costs = NULL;
		g_autoptr(VentureMoney) gross = NULL;

		costs = venture_money_add(pnl_expenses, support_total, NULL);

		if (NULL == costs)
			costs = venture_money_copy(pnl_expenses);

		gross = venture_money_subtract(pnl_revenue, costs, NULL);

		if (NULL != gross)
		{
			margin_bps = headline_basis_points(
				venture_money_get_amount(gross),
				venture_money_get_amount(pnl_revenue));
			margin_known = TRUE;
		}
	}

	if (months < minimum_months)
		withheld = "insufficient data";
	else if (!margin_known)
		withheld = "n/a";
	else if (0 == churn.inactive)
		withheld = "n/a";

	if (NULL == withheld)
	{
		g_autoptr(VentureMoney) margin_share = NULL;

		arpu = venture_money_multiply_rational(revenue_12m, 1, months, NULL);
		margin_share = (NULL != arpu)
			? venture_money_multiply_percent(arpu, margin_bps, NULL) : NULL;

		/* Monthly churn is inactive / (active x 12); dividing by it is
		 * multiplying by active x 12 / inactive, one exact rational. */
		projected = (NULL != margin_share)
			? venture_money_multiply_rational(margin_share,
			                                  churn.active_12m * 12,
			                                  churn.inactive, NULL)
			: NULL;

		if (NULL == projected)
			withheld = "n/a";
	}

	result = venture_report_result_new("Customer lifetime value", period);

	venture_report_result_add_metric(result,
		venture_metric_new_count("customers", "Paying customers",
		                         (gint64)customers->len));
	venture_report_result_add_metric(result,
		venture_metric_new_money("realised_total", "Realised, all customers",
		                         total));
	venture_report_result_add_metric(result,
		venture_metric_new_money("realised_mean", "Realised, mean", mean));
	venture_report_result_add_metric(result,
		venture_metric_new_money("realised_median", "Realised, median",
		                         median));

	if (NULL == withheld)
	{
		venture_report_result_add_metric(result,
			venture_metric_new_money("projected", "Projected LTV",
			                         projected));
		venture_report_result_add_metric(result,
			venture_metric_new_money("arpu", "Revenue per customer-month",
			                         arpu));
	}
	else
	{
		g_autofree gchar *note = NULL;

		venture_report_result_add_metric(result,
			venture_metric_new_text("projected", "Projected LTV", withheld));

		if (months < minimum_months)
			note = g_strdup_printf(
				"Projected LTV is withheld: %" G_GINT64_FORMAT
				" customer-month%s of paid revenue in the trailing twelve "
				"months is below the minimum of %" G_GINT64_FORMAT
				". Change it in the organisation's headline settings.",
				months, (1 == months) ? "" : "s", minimum_months);
		else if (!margin_known)
			note = g_strdup(
				"Projected LTV is n/a: the trailing twelve months have no "
				"net revenue on the P&L, or revenue and expenses are in "
				"different currencies, so there is no gross margin.");
		else
			note = g_strdup(
				"Projected LTV is n/a: no customer has gone quiet in the "
				"trailing twelve months, so monthly activity churn is zero "
				"and lifetime would be unbounded. That is good news, not a "
				"number.");

		venture_report_result_append_note(result, note);
	}

	if (margin_known)
		venture_report_result_add_metric(result,
			venture_metric_new_ratio("gross_margin", "Gross margin",
			                         headline_render_bps(margin_bps)));
	else
		venture_report_result_add_metric(result,
			venture_metric_new_text("gross_margin", "Gross margin", "n/a"));

	if (churn.active_12m > 0)
	{
		VentureMetric *metric;

		metric = venture_metric_new_ratio("monthly_churn",
			"Monthly activity churn",
			headline_render_bps(headline_basis_points(
				churn.inactive, churn.active_12m * 12)));
		venture_metric_set_higher_is_better(metric, FALSE);
		venture_report_result_add_metric(result, metric);
	}
	else
	{
		venture_report_result_add_metric(result,
			venture_metric_new_text("monthly_churn", "Monthly activity churn",
			                        "n/a"));
	}

	venture_report_result_add_metric(result,
		venture_metric_new_count("customer_months", "Customer-months",
		                         months));
	venture_report_result_add_metric(result,
		venture_metric_new_money("support_cost", "Attributed support cost",
		                         support_total));

	venture_report_result_add_column(result, "customer", "Customer",
	                                 VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "realised", "Realised",
	                                 VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "support_cost", "Support cost",
	                                 VENTURE_REPORT_COLUMN_MONEY);

	for (i = 0; (i < customers->len) && (i < 10); i++)
	{
		g_autofree gchar *name = NULL;
		LtvCustomer *customer;

		customer = g_ptr_array_index(customers, i);
		name = headline_company_name(context, customer->company_id);

		venture_report_result_begin_row(result);
		venture_report_result_set_text(result, "customer", name);
		venture_report_result_set_money(result, "realised", customer->realised);

		if (NULL != customer->support_cost)
			venture_report_result_set_money(result, "support_cost",
			                                customer->support_cost);
	}

	if ((NULL == rate) || venture_money_is_zero(rate))
		venture_report_result_append_note(result,
			"No worklog hourly rate is configured, so support hours cost "
			"nothing in the gross margin. Set hourly_rate in the "
			"organisation's headline settings to attribute them.");

	headline_flag_skipped(result, skipped, currency);

	return g_steal_pointer(&result);
}

/* ==========================================================================
 * LTV:CAC
 * ========================================================================== */

static VentureMetric *
headline_find_metric(
	VentureReportResult	*result,
	const gchar		*key
){
	GPtrArray *metrics;
	guint i;

	metrics = venture_report_result_get_metrics(result);

	for (i = 0; i < metrics->len; i++)
	{
		VentureMetric *metric;

		metric = g_ptr_array_index(metrics, i);

		if (0 == g_strcmp0(venture_metric_get_key(metric), key))
			return metric;
	}

	return NULL;
}

static VentureReportResult *
venture_report_ltv_cac(
	VentureContext		 *context,
	VentureDateRange	 *period,
	JsonObject		 *options,
	GError			**error
){
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(VentureReportResult) ltv = NULL;
	g_autoptr(VentureReportResult) cac = NULL;
	VentureMetric *projected;
	VentureMetric *cost;
	const VentureMoney *ltv_amount;
	const VentureMoney *cac_amount;
	const gchar *reason;

	ltv = venture_report_ltv(context, period, options, error);

	if (NULL == ltv)
		return NULL;

	cac = venture_report_cac(context, period, options, error);

	if (NULL == cac)
		return NULL;

	projected = headline_find_metric(ltv, "projected");
	cost = headline_find_metric(cac, "cac");
	ltv_amount = (NULL != projected) ? venture_metric_get_money(projected)
	                                 : NULL;
	cac_amount = (NULL != cost) ? venture_metric_get_money(cost) : NULL;
	reason = NULL;

	result = venture_report_result_new("LTV to CAC", period);

	if (NULL == ltv_amount)
		reason = (NULL != projected) ? venture_metric_get_text(projected)
		                             : "n/a";
	else if (NULL == cac_amount)
		reason = "n/a";
	else if (venture_money_is_zero(cac_amount))
		reason = "n/a";
	else if (0 != g_ascii_strcasecmp(venture_money_get_currency(ltv_amount),
	                                 venture_money_get_currency(cac_amount)))
		reason = "n/a";

	if (NULL == reason)
	{
		gint64 hundredths;

		/* Both sides are in the same currency and exponent, so the ratio
		 * is one integer division of minor units, kept to hundredths. */
		hundredths = (venture_money_get_amount(ltv_amount) * 100)
			/ venture_money_get_amount(cac_amount);
		venture_report_result_add_metric(result,
			venture_metric_new_number("ltv_cac", "LTV:CAC",
			                          (gdouble)hundredths / 100.0));
	}
	else
	{
		venture_report_result_add_metric(result,
			venture_metric_new_text("ltv_cac", "LTV:CAC", reason));

		if (NULL == ltv_amount)
			venture_report_result_append_note(result,
				"Projected LTV is not available for this period, so the "
				"ratio is n/a.");
		else if ((NULL == cac_amount) || venture_money_is_zero(cac_amount))
			venture_report_result_append_note(result,
				"CAC is not available for this period -- no new customers, "
				"or no spend -- so the ratio is n/a.");
		else
			venture_report_result_append_note(result,
				"Projected LTV and CAC are in different currencies, so "
				"the ratio is refused rather than guessed.");
	}

	if (NULL != ltv_amount)
		venture_report_result_add_metric(result,
			venture_metric_new_money("projected_ltv", "Projected LTV",
			                         ltv_amount));
	else
		venture_report_result_add_metric(result,
			venture_metric_new_text("projected_ltv", "Projected LTV",
				(NULL != projected) ? venture_metric_get_text(projected)
				                    : "n/a"));

	if (NULL != cac_amount)
		venture_report_result_add_metric(result,
			venture_metric_new_money("cac", "CAC", cac_amount));
	else
		venture_report_result_add_metric(result,
			venture_metric_new_text("cac", "CAC", "n/a"));

	venture_report_result_add_column(result, "side", "Side",
	                                 VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "value", "Value",
	                                 VENTURE_REPORT_COLUMN_TEXT);

	{
		g_autofree gchar *ltv_text = NULL;
		g_autofree gchar *cac_text = NULL;

		ltv_text = (NULL != projected) ? venture_metric_format_value(projected)
		                               : g_strdup("n/a");
		cac_text = (NULL != cost) ? venture_metric_format_value(cost)
		                          : g_strdup("n/a");

		venture_report_result_begin_row(result);
		venture_report_result_set_text(result, "side", "Projected LTV");
		venture_report_result_set_text(result, "value", ltv_text);
		venture_report_result_begin_row(result);
		venture_report_result_set_text(result, "side", "CAC");
		venture_report_result_set_text(result, "value", cac_text);
	}

	return g_steal_pointer(&result);
}

/* --- Registration --------------------------------------------------------- */

void
venture_headline_register_reports(VentureReportRegistry *registry)
{
	g_return_if_fail(VENTURE_IS_REPORT_REGISTRY(registry));

	venture_report_registry_add(registry, VENTURE_REPORT(venture_func_report_new(
		"cac", "Customer acquisition cost",
		"Acquisition spend -- campaign spend plus acquisition-flagged expense "
		"and bill lines -- over companies whose first paid invoice fell in "
		"the period, broken down by the converted lead's source and campaign",
		venture_report_cac)));
	venture_report_registry_add(registry, VENTURE_REPORT(venture_func_report_new(
		"customer_churn", "Customer churn (recurring and activity)",
		"Recurring churn: invoice schedules cancelled or lapsed in the period "
		"over those running at its start. Activity churn: customers paid in "
		"the trailing twelve months with nothing in the last N days "
		"(days, default 90). Both list the customers behind the number",
		venture_report_churn)));
	venture_report_registry_add(registry, VENTURE_REPORT(venture_func_report_new(
		"ltv", "Customer lifetime value",
		"Realised: lifetime paid revenue per customer net of refunds, with "
		"mean, median and the top ten. Projected: revenue per customer-month "
		"x gross margin / monthly activity churn, withheld below a minimum "
		"of customer-months",
		venture_report_ltv)));
	venture_report_registry_add(registry, VENTURE_REPORT(venture_func_report_new(
		"ltv_cac", "LTV to CAC",
		"Projected LTV over CAC for the same period; n/a when either side is",
		venture_report_ltv_cac)));
}
