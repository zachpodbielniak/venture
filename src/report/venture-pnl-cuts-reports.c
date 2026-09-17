/*
 * venture-pnl-cuts-reports.c - The cuts of the P&L the business is run on
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Money in and money out, sliced the way the questions arrive: revenue by
 * customer and by where the customer came from; spend by vendor and by
 * category, with the acquisition share visible; the recurring costs rolled
 * up to a monthly run-rate; and the next few weeks of cash, from what is
 * due to be paid and due to be collected, starting from what the bank says
 * is there. Every figure is read from records the other modules already
 * keep -- invoice events, receipts, refunds, credit notes, approved bills,
 * expenses, schedules, statement balances -- and none is invented: an
 * undated document is listed as undated, a schedule that cannot be priced
 * is listed unpriced, an amount in another currency is counted and noted.
 */

#include "venture.h"
#include "report/venture-pnl-cuts-private.h"

#include <string.h>

/* --- Shared plumbing ------------------------------------------------------ */

static gint64
cuts_organization(
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

/* The organisation's book currency, or the process default: the currency
 * every total here is carried in. */
static gchar *
cuts_currency(
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

static GPtrArray *
cuts_fetch(
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

/* Adds into a total that already has a currency, counting what cannot
 * join rather than hiding it. */
static void
cuts_accumulate(
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

static void
cuts_flag_skipped(
	VentureReportResult	*result,
	guint			 skipped,
	const gchar		*currency
){
	g_autofree gchar *note = NULL;

	if (0 == skipped)
		return;

	note = g_strdup_printf(
		"%u amount%s could not be included in the totals -- not in %s, the "
		"book currency they are carried in, or arithmetic that would "
		"overflow. Rows keep their own currency; cross-currency totals are "
		"refused rather than guessed.",
		skipped, (1 == skipped) ? "" : "s", currency);

	venture_report_result_append_note(result, note);
}

static gchar *
cuts_company_name(
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

/* A calendar day in the context's timezone, the way periods are cut. */
static gchar *
cuts_day(
	VentureContext	*context,
	GDateTime	*when
){
	if (NULL == when)
		return g_strdup("");

	return venture_time_to_date_string(when,
		venture_context_get_timezone(context));
}

/*
 * The `by` option (or `group_by`, which every other report spells it as),
 * checked against what the report can group on. An unknown grouping is
 * refused: a report that quietly fell back to the default would answer a
 * different question from the one asked.
 */
static const gchar *
cuts_grouping(
	JsonObject		 *options,
	const gchar *const	 *allowed,
	GError			**error
){
	const gchar *by = NULL;
	guint i;

	if (NULL != options)
	{
		by = venture_json_object_get_string(options, "by", NULL);

		if (venture_string_is_empty(by))
			by = venture_json_object_get_string(options, "group_by", NULL);
	}

	if (venture_string_is_empty(by))
		return allowed[0];

	for (i = 0; NULL != allowed[i]; i++)
	{
		if (0 == g_ascii_strcasecmp(by, allowed[i]))
			return allowed[i];
	}

	{
		g_autofree gchar *choices = NULL;

		choices = g_strjoinv(" or ", (gchar **)allowed);
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "\"%s\" is not a grouping this report offers; by=%s",
		            by, choices);
	}

	return NULL;
}

/* --- Groups: one row per label per currency ------------------------------- */

#define CUTS_SLOTS 5

typedef struct
{
	gchar		*key;
	gchar		*label;
	gchar		*detail;
	gchar		*currency;
	VentureMoney	*slot[CUTS_SLOTS];
} CutsGroup;

static void
cuts_group_free(gpointer data)
{
	CutsGroup *group = data;
	guint i;

	g_free(group->key);
	g_free(group->label);
	g_free(group->detail);
	g_free(group->currency);

	for (i = 0; i < CUTS_SLOTS; i++)
		g_clear_pointer(&group->slot[i], venture_money_free);

	g_free(group);
}

/* Finds or starts the group for @key in @currency. Grouping by currency as
 * well as by label is what keeps a customer who pays in two currencies from
 * having them added together. */
static CutsGroup *
cuts_group_get(
	GHashTable	*groups,
	GPtrArray	*order,
	const gchar	*key,
	const gchar	*label,
	const gchar	*currency
){
	g_autofree gchar *composite = NULL;
	CutsGroup *group;
	guint i;

	composite = g_strdup_printf("%s\n%s", key, currency);
	group = g_hash_table_lookup(groups, composite);

	if (NULL != group)
		return group;

	group = g_new0(CutsGroup, 1);
	group->key = g_strdup(key);
	group->label = g_strdup(label);
	group->currency = g_strdup(currency);

	for (i = 0; i < CUTS_SLOTS; i++)
		group->slot[i] = venture_money_new_zero(currency);

	g_hash_table_insert(groups, g_steal_pointer(&composite), group);
	g_ptr_array_add(order, group);

	return group;
}

static void
cuts_group_add(
	CutsGroup		*group,
	guint			 slot,
	const VentureMoney	*amount
){
	cuts_accumulate(&group->slot[slot], amount, NULL);
}

/* What a sort is ordered by: the book currency, whose rows come first
 * because they are the ones the totals are made of, and the slot that is
 * compared. Passed to the comparator, not kept anywhere. */
typedef struct
{
	const gchar	*currency;
	guint		 slot;
} CutsSortKey;

/* Descending on the sort slot; rows in the book currency before the rest. */
static gint
cuts_group_compare(
	gconstpointer	a,
	gconstpointer	b,
	gpointer	user_data
){
	const CutsGroup *left = *(CutsGroup *const *)a;
	const CutsGroup *right = *(CutsGroup *const *)b;
	const CutsSortKey *key = user_data;
	gboolean left_book;
	gboolean right_book;
	gint64 l;
	gint64 r;

	left_book = (0 == g_strcmp0(left->currency, key->currency));
	right_book = (0 == g_strcmp0(right->currency, key->currency));

	if (left_book != right_book)
		return left_book ? -1 : 1;

	l = venture_money_get_amount(left->slot[key->slot]);
	r = venture_money_get_amount(right->slot[key->slot]);

	if (l != r)
		return (l > r) ? -1 : 1;

	return g_strcmp0(left->label, right->label);
}

static void
cuts_groups_sort(
	GPtrArray	*order,
	const gchar	*currency,
	guint		 slot
){
	CutsSortKey key;

	key.currency = currency;
	key.slot = slot;
	g_ptr_array_sort_with_data(order, cuts_group_compare, &key);
}

/* --- Attribution: where a customer came from ------------------------------ */

/* The source of the lead that converted into each company, keyed by
 * company id; the first converted lead wins, as CAC counts it. */
static GHashTable *
cuts_lead_sources(
	VentureContext	 *context,
	gint64		  organization_id,
	GError		**error
){
	g_autoptr(GHashTable) sources = NULL;
	g_autoptr(GPtrArray) leads = NULL;
	guint i;

	sources = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);

	if (!venture_context_module_enabled(context, "leads"))
		return g_steal_pointer(&sources);

	leads = cuts_fetch(context, VENTURE_TYPE_LEAD, organization_id, NULL,
	                   NULL, FALSE, error);

	if (NULL == leads)
		return NULL;

	for (i = 0; i < leads->len; i++)
	{
		g_autofree gchar *source = NULL;
		VentureEntity *lead;
		VentureLeadStatus status;
		gint64 company_id = 0;

		lead = g_ptr_array_index(leads, i);
		g_object_get(lead, "status", &status, "converted-company-id",
		             &company_id, "source", &source, NULL);

		if ((VENTURE_LEAD_CONVERTED != status) || (0 == company_id) ||
		    venture_string_is_empty(source) ||
		    g_hash_table_contains(sources, GINT_TO_POINTER((gint)company_id)))
			continue;

		g_hash_table_insert(sources, GINT_TO_POINTER((gint)company_id),
		                    g_steal_pointer(&source));
	}

	return g_steal_pointer(&sources);
}

/* The lead's source, else the company's own, else unattributed. */
static gchar *
cuts_company_source(
	VentureContext	*context,
	GHashTable	*lead_sources,
	gint64		 company_id
){
	g_autoptr(VentureEntity) company = NULL;
	const gchar *known;
	gchar *source = NULL;

	known = g_hash_table_lookup(lead_sources, GINT_TO_POINTER((gint)company_id));

	if (NULL != known)
		return g_strdup(known);

	company = venture_database_get(venture_context_get_database(context),
	                               VENTURE_TYPE_COMPANY, company_id, NULL);

	if (NULL != company)
		g_object_get(company, "source", &source, NULL);

	if (venture_string_is_empty(source))
	{
		g_free(source);
		source = g_strdup("(unattributed)");
	}

	return source;
}

/* ==========================================================================
 * Revenue by customer
 * ========================================================================== */

typedef enum
{
	REVENUE_PAID = 0,
	REVENUE_ISSUED = 1
} RevenueSlot;

/*
 * Adds one movement to the group the company belongs to, under the
 * grouping asked for. Company names and sources are looked up once each.
 */
static void
revenue_add(
	VentureContext		*context,
	GHashTable		*groups,
	GPtrArray		*order,
	GHashTable		*lead_sources,
	GHashTable		*customers,
	gboolean		 by_source,
	gint64			 company_id,
	RevenueSlot		 slot,
	const VentureMoney	*amount
){
	g_autofree gchar *name = NULL;
	g_autofree gchar *source = NULL;
	g_autofree gchar *key = NULL;
	CutsGroup *group;

	if ((0 == company_id) || (NULL == amount))
		return;

	g_hash_table_add(customers, GINT_TO_POINTER((gint)company_id));
	source = cuts_company_source(context, lead_sources, company_id);

	if (by_source)
	{
		group = cuts_group_get(groups, order, source, source,
		                       venture_money_get_currency(amount));
	}
	else
	{
		name = cuts_company_name(context, company_id);
		key = g_strdup_printf("%" G_GINT64_FORMAT, company_id);
		group = cuts_group_get(groups, order, key, name,
		                       venture_money_get_currency(amount));

		if (NULL == group->detail)
			group->detail = g_steal_pointer(&source);
	}

	cuts_group_add(group, slot, amount);
}

static VentureReportResult *
venture_report_revenue_by_customer(
	VentureContext		 *context,
	VentureDateRange	 *period,
	JsonObject		 *options,
	GError			**error
){
	static const gchar *const groupings[] = { "customer", "source", NULL };
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(GHashTable) groups = NULL;
	g_autoptr(GPtrArray) order = NULL;
	g_autoptr(GHashTable) lead_sources = NULL;
	g_autoptr(GHashTable) customers = NULL;
	g_autoptr(GHashTable) invoice_company = NULL;
	g_autoptr(GHashTable) void_invoices = NULL;
	g_autoptr(GPtrArray) invoices = NULL;
	g_autoptr(GPtrArray) events = NULL;
	g_autoptr(GPtrArray) credits = NULL;
	g_autoptr(GPtrArray) allocations = NULL;
	g_autoptr(GPtrArray) refunds = NULL;
	g_autoptr(VentureMoney) paid_total = NULL;
	g_autoptr(VentureMoney) issued_total = NULL;
	g_autofree gchar *currency = NULL;
	const gchar *by;
	gint64 organization_id;
	gboolean by_source;
	guint skipped;
	guint i;

	by = cuts_grouping(options, groupings, error);

	if (NULL == by)
		return NULL;

	by_source = (0 == g_strcmp0(by, "source"));
	organization_id = cuts_organization(context, options);
	currency = cuts_currency(context, organization_id);

	/* Which company each invoice bills, and which were voided: a voided
	 * invoice was never revenue, whenever it was issued. Deleted invoices
	 * still map their receipts. */
	invoices = cuts_fetch(context, VENTURE_TYPE_INVOICE, organization_id,
	                      NULL, NULL, TRUE, error);

	if (NULL == invoices)
		return NULL;

	invoice_company = g_hash_table_new(g_direct_hash, g_direct_equal);
	void_invoices = g_hash_table_new(g_direct_hash, g_direct_equal);

	for (i = 0; i < invoices->len; i++)
	{
		VentureEntity *invoice;
		VentureInvoiceStatus status;
		gint64 company_id = 0;

		invoice = g_ptr_array_index(invoices, i);
		g_object_get(invoice, "company-id", &company_id, "status", &status, NULL);
		g_hash_table_insert(invoice_company,
			GINT_TO_POINTER((gint)venture_entity_get_id(invoice)),
			GINT_TO_POINTER((gint)company_id));

		if (VENTURE_INVOICE_STATUS_VOID == status)
			g_hash_table_add(void_invoices,
				GINT_TO_POINTER((gint)venture_entity_get_id(invoice)));
	}

	events = cuts_fetch(context, VENTURE_TYPE_INVOICE_EVENT, organization_id,
	                    "date", period, FALSE, error);

	if (NULL == events)
		return NULL;

	credits = cuts_fetch(context, VENTURE_TYPE_CUSTOMER_CREDIT, organization_id,
	                     "date", period, FALSE, error);

	if (NULL == credits)
		return NULL;

	allocations = cuts_fetch(context, VENTURE_TYPE_PAYMENT_ALLOCATION,
	                         organization_id, "date", period, FALSE, error);

	if (NULL == allocations)
		return NULL;

	refunds = cuts_fetch(context, VENTURE_TYPE_REFUND, organization_id,
	                     "date", period, FALSE, error);

	if (NULL == refunds)
		return NULL;

	lead_sources = cuts_lead_sources(context, organization_id, error);

	if (NULL == lead_sources)
		return NULL;

	groups = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                               cuts_group_free);
	order = g_ptr_array_new();
	customers = g_hash_table_new(g_direct_hash, g_direct_equal);

	/* Issued: the amount frozen on each issue event in the period. */
	for (i = 0; i < events->len; i++)
	{
		g_autofree gchar *kind = NULL;
		g_autoptr(VentureMoney) amount = NULL;
		VentureEntity *event;
		gint64 invoice_id = 0;

		event = g_ptr_array_index(events, i);
		g_object_get(event, "kind", &kind, "invoice-id", &invoice_id,
		             "amount", &amount, NULL);

		if ((0 != g_strcmp0(kind, "issue")) ||
		    g_hash_table_contains(void_invoices, GINT_TO_POINTER((gint)invoice_id)))
			continue;

		revenue_add(context, groups, order, lead_sources, customers, by_source,
			GPOINTER_TO_INT(g_hash_table_lookup(invoice_company,
				GINT_TO_POINTER((gint)invoice_id))),
			REVENUE_ISSUED, amount);
	}

	/* Less credit notes and write-offs: issued and then given back or
	 * given up, without cash moving. Deposits and overpayments are cash
	 * and are already in the receipts. */
	for (i = 0; i < credits->len; i++)
	{
		g_autofree gchar *kind = NULL;
		g_autoptr(VentureMoney) amount = NULL;
		g_autoptr(VentureMoney) negated = NULL;
		VentureEntity *credit;
		gint64 company_id = 0;

		credit = g_ptr_array_index(credits, i);
		g_object_get(credit, "kind", &kind, "customer-id", &company_id,
		             "amount", &amount, NULL);

		if ((0 != g_strcmp0(kind, "credit_note")) &&
		    (0 != g_strcmp0(kind, "write_off")))
			continue;

		if (NULL != amount)
			negated = venture_money_negate(amount);

		revenue_add(context, groups, order, lead_sources, customers, by_source,
		            company_id, REVENUE_ISSUED, negated);
	}

	/* Paid: receipts applied in the period, less refunds. A credit applied
	 * to an invoice is not cash the customer handed over. */
	for (i = 0; i < allocations->len; i++)
	{
		g_autoptr(VentureMoney) amount = NULL;
		VentureEntity *allocation;
		gint64 payment_id = 0;
		gint64 invoice_id = 0;

		allocation = g_ptr_array_index(allocations, i);
		g_object_get(allocation, "payment-id", &payment_id, "invoice-id",
		             &invoice_id, "amount", &amount, NULL);

		if (0 == payment_id)
			continue;

		revenue_add(context, groups, order, lead_sources, customers, by_source,
			GPOINTER_TO_INT(g_hash_table_lookup(invoice_company,
				GINT_TO_POINTER((gint)invoice_id))),
			REVENUE_PAID, amount);
	}

	for (i = 0; i < refunds->len; i++)
	{
		g_autoptr(VentureMoney) amount = NULL;
		g_autoptr(VentureMoney) negated = NULL;
		VentureEntity *refund;
		gint64 company_id = 0;

		refund = g_ptr_array_index(refunds, i);
		g_object_get(refund, "customer-id", &company_id, "amount", &amount, NULL);

		if (NULL != amount)
			negated = venture_money_negate(amount);

		revenue_add(context, groups, order, lead_sources, customers, by_source,
		            company_id, REVENUE_PAID, negated);
	}

	cuts_groups_sort(order, currency, REVENUE_PAID);

	paid_total = venture_money_new_zero(currency);
	issued_total = venture_money_new_zero(currency);
	skipped = 0;

	result = venture_report_result_new(by_source ? "Revenue by lead source"
	                                             : "Revenue by customer", period);

	venture_report_result_add_column(result, by_source ? "source" : "customer",
	                                 by_source ? "Source" : "Customer",
	                                 VENTURE_REPORT_COLUMN_TEXT);

	if (!by_source)
		venture_report_result_add_column(result, "source", "Source",
		                                 VENTURE_REPORT_COLUMN_TEXT);

	venture_report_result_add_column(result, "currency", "Currency",
	                                 VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "paid", "Paid",
	                                 VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "issued", "Issued",
	                                 VENTURE_REPORT_COLUMN_MONEY);

	for (i = 0; i < order->len; i++)
	{
		CutsGroup *group;

		group = g_ptr_array_index(order, i);

		venture_report_result_begin_row(result);
		venture_report_result_set_text(result, by_source ? "source" : "customer",
		                               group->label);

		if (!by_source)
			venture_report_result_set_text(result, "source",
				(NULL != group->detail) ? group->detail : "(unattributed)");

		venture_report_result_set_text(result, "currency", group->currency);
		venture_report_result_set_money(result, "paid", group->slot[REVENUE_PAID]);
		venture_report_result_set_money(result, "issued", group->slot[REVENUE_ISSUED]);

		if (0 != g_strcmp0(group->currency, currency))
		{
			skipped++;
			continue;
		}

		cuts_accumulate(&paid_total, group->slot[REVENUE_PAID], &skipped);
		cuts_accumulate(&issued_total, group->slot[REVENUE_ISSUED], &skipped);
	}

	venture_report_result_add_metric(result,
		venture_metric_new_money("paid", "Paid revenue", paid_total));
	venture_report_result_add_metric(result,
		venture_metric_new_money("issued", "Issued revenue", issued_total));
	venture_report_result_add_metric(result,
		venture_metric_new_count("customers", "Customers",
		                         (gint64)g_hash_table_size(customers)));

	if (!venture_context_module_enabled(context, "leads"))
		venture_report_result_append_note(result,
			"The leads module is off, so sources come from each company's "
			"own source field rather than from the lead that converted.");

	cuts_flag_skipped(result, skipped, currency);

	return g_steal_pointer(&result);
}

/* ==========================================================================
 * Spend by vendor
 * ========================================================================== */

typedef enum
{
	SPEND_BILLS = 0,
	SPEND_EXPENSES = 1,
	SPEND_ACQUISITION = 2,
	SPEND_OTHER = 3,
	SPEND_TOTAL = 4
} SpendSlot;

static void
spend_add(
	GHashTable		*groups,
	GPtrArray		*order,
	const gchar		*label,
	SpendSlot		 source,
	gboolean		 acquisition,
	const VentureMoney	*amount
){
	CutsGroup *group;

	if (NULL == amount)
		return;

	group = cuts_group_get(groups, order, label, label,
	                       venture_money_get_currency(amount));
	cuts_group_add(group, source, amount);
	cuts_group_add(group, acquisition ? SPEND_ACQUISITION : SPEND_OTHER, amount);
	cuts_group_add(group, SPEND_TOTAL, amount);
}

static const gchar *
spend_label(
	const gchar	*text,
	const gchar	*fallback
){
	return venture_string_is_empty(text) ? fallback : text;
}

static VentureReportResult *
venture_report_spend_by_vendor(
	VentureContext		 *context,
	VentureDateRange	 *period,
	JsonObject		 *options,
	GError			**error
){
	static const gchar *const groupings[] = { "vendor", "category", NULL };
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(GHashTable) groups = NULL;
	g_autoptr(GPtrArray) order = NULL;
	g_autoptr(GPtrArray) expenses = NULL;
	g_autoptr(VentureMoney) spend_total = NULL;
	g_autoptr(VentureMoney) bills_total = NULL;
	g_autoptr(VentureMoney) expenses_total = NULL;
	g_autoptr(VentureMoney) acquisition_total = NULL;
	g_autoptr(VentureMoney) other_total = NULL;
	g_autofree gchar *currency = NULL;
	const gchar *by;
	gint64 organization_id;
	gboolean by_category;
	guint skipped;
	guint i;

	by = cuts_grouping(options, groupings, error);

	if (NULL == by)
		return NULL;

	by_category = (0 == g_strcmp0(by, "category"));
	organization_id = cuts_organization(context, options);
	currency = cuts_currency(context, organization_id);

	groups = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                               cuts_group_free);
	order = g_ptr_array_new();

	/* Expenses dated in the period, whoever was paid. */
	expenses = cuts_fetch(context, VENTURE_TYPE_EXPENSE, organization_id,
	                      "occurred-at", period, FALSE, error);

	if (NULL == expenses)
		return NULL;

	for (i = 0; i < expenses->len; i++)
	{
		g_autofree gchar *vendor = NULL;
		g_autofree gchar *category = NULL;
		g_autoptr(VentureMoney) amount = NULL;
		gboolean acquisition = FALSE;

		g_object_get(g_ptr_array_index(expenses, i), "vendor", &vendor,
		             "category", &category, "acquisition", &acquisition,
		             "amount", &amount, NULL);

		spend_add(groups, order,
		          by_category ? spend_label(category, "(uncategorised)")
		                      : spend_label(vendor, "(no vendor)"),
		          SPEND_EXPENSES, acquisition, amount);
	}

	/* Lines of bills dated in the period that are past draft and not
	 * void: approved, part paid or paid. */
	if (venture_context_module_enabled(context, "payables"))
	{
		g_autoptr(GPtrArray) bills = NULL;
		g_autoptr(GPtrArray) lines = NULL;
		g_autoptr(GHashTable) vendor_of_bill = NULL;
		g_autoptr(GHashTable) vendor_names = NULL;

		bills = cuts_fetch(context, VENTURE_TYPE_VENDOR_BILL, organization_id,
		                   "bill-date", period, FALSE, error);

		if (NULL == bills)
			return NULL;

		vendor_of_bill = g_hash_table_new(g_direct_hash, g_direct_equal);
		vendor_names = g_hash_table_new_full(g_direct_hash, g_direct_equal,
		                                     NULL, g_free);

		for (i = 0; i < bills->len; i++)
		{
			g_autofree gchar *status = NULL;
			VentureEntity *bill;
			gint64 vendor_id = 0;

			bill = g_ptr_array_index(bills, i);
			g_object_get(bill, "status", &status, "company-id", &vendor_id, NULL);

			if ((0 == g_strcmp0(status, "draft")) ||
			    (0 == g_strcmp0(status, "void")))
				continue;

			g_hash_table_insert(vendor_of_bill,
				GINT_TO_POINTER((gint)venture_entity_get_id(bill)),
				GINT_TO_POINTER((gint)vendor_id));

			if (!g_hash_table_contains(vendor_names,
			                           GINT_TO_POINTER((gint)vendor_id)))
				g_hash_table_insert(vendor_names,
					GINT_TO_POINTER((gint)vendor_id),
					cuts_company_name(context, vendor_id));
		}

		lines = cuts_fetch(context, VENTURE_TYPE_VENDOR_BILL_LINE,
		                   organization_id, NULL, NULL, FALSE, error);

		if (NULL == lines)
			return NULL;

		for (i = 0; i < lines->len; i++)
		{
			g_autofree gchar *category = NULL;
			g_autoptr(VentureMoney) amount = NULL;
			VentureEntity *line;
			gpointer vendor_id;
			gboolean acquisition = FALSE;
			gint64 bill_id = 0;

			line = g_ptr_array_index(lines, i);
			g_object_get(line, "bill-id", &bill_id, "category", &category,
			             "acquisition", &acquisition, NULL);

			if (!g_hash_table_lookup_extended(vendor_of_bill,
			                                  GINT_TO_POINTER((gint)bill_id),
			                                  NULL, &vendor_id))
				continue;

			amount = venture_vendor_bill_line_get_amount(
				VENTURE_VENDOR_BILL_LINE(line), NULL);

			spend_add(groups, order,
			          by_category ? spend_label(category, "(uncategorised)")
			                      : g_hash_table_lookup(vendor_names, vendor_id),
			          SPEND_BILLS, acquisition, amount);
		}
	}

	cuts_groups_sort(order, currency, SPEND_TOTAL);

	spend_total = venture_money_new_zero(currency);
	bills_total = venture_money_new_zero(currency);
	expenses_total = venture_money_new_zero(currency);
	acquisition_total = venture_money_new_zero(currency);
	other_total = venture_money_new_zero(currency);
	skipped = 0;

	result = venture_report_result_new(by_category ? "Spend by category"
	                                               : "Spend by vendor", period);

	venture_report_result_add_column(result, by_category ? "category" : "vendor",
	                                 by_category ? "Category" : "Vendor",
	                                 VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "currency", "Currency",
	                                 VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "bills", "Bills",
	                                 VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "expenses", "Expenses",
	                                 VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "total", "Total",
	                                 VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "acquisition", "Acquisition",
	                                 VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "other", "Other",
	                                 VENTURE_REPORT_COLUMN_MONEY);

	for (i = 0; i < order->len; i++)
	{
		CutsGroup *group;

		group = g_ptr_array_index(order, i);

		venture_report_result_begin_row(result);
		venture_report_result_set_text(result, by_category ? "category" : "vendor",
		                               group->label);
		venture_report_result_set_text(result, "currency", group->currency);
		venture_report_result_set_money(result, "bills", group->slot[SPEND_BILLS]);
		venture_report_result_set_money(result, "expenses", group->slot[SPEND_EXPENSES]);
		venture_report_result_set_money(result, "total", group->slot[SPEND_TOTAL]);
		venture_report_result_set_money(result, "acquisition", group->slot[SPEND_ACQUISITION]);
		venture_report_result_set_money(result, "other", group->slot[SPEND_OTHER]);

		if (0 != g_strcmp0(group->currency, currency))
		{
			skipped++;
			continue;
		}

		cuts_accumulate(&spend_total, group->slot[SPEND_TOTAL], &skipped);
		cuts_accumulate(&bills_total, group->slot[SPEND_BILLS], &skipped);
		cuts_accumulate(&expenses_total, group->slot[SPEND_EXPENSES], &skipped);
		cuts_accumulate(&acquisition_total, group->slot[SPEND_ACQUISITION], &skipped);
		cuts_accumulate(&other_total, group->slot[SPEND_OTHER], &skipped);
	}

	{
		VentureMetric *metric;

		metric = venture_metric_new_money("spend", "Spend", spend_total);
		venture_metric_set_higher_is_better(metric, FALSE);
		venture_report_result_add_metric(result, metric);
		metric = venture_metric_new_money("bills", "From bills", bills_total);
		venture_metric_set_higher_is_better(metric, FALSE);
		venture_report_result_add_metric(result, metric);
		metric = venture_metric_new_money("expenses", "From expenses", expenses_total);
		venture_metric_set_higher_is_better(metric, FALSE);
		venture_report_result_add_metric(result, metric);
		metric = venture_metric_new_money("acquisition", "Acquisition", acquisition_total);
		venture_metric_set_higher_is_better(metric, FALSE);
		venture_report_result_add_metric(result, metric);
		metric = venture_metric_new_money("other", "Other", other_total);
		venture_metric_set_higher_is_better(metric, FALSE);
		venture_report_result_add_metric(result, metric);
	}

	if (!venture_context_module_enabled(context, "payables"))
		venture_report_result_append_note(result,
			"The payables module is off, so only expenses are counted; "
			"no vendor bills are.");

	cuts_flag_skipped(result, skipped, currency);

	return g_steal_pointer(&result);
}

/* ==========================================================================
 * Recurring costs
 * ========================================================================== */

typedef struct
{
	gchar		*name;
	gchar		*kind;
	gchar		*vendor;
	gchar		*frequency;
	gchar		*next_due;
	VentureMoney	*amount;
	VentureMoney	*monthly;
} RecurringRow;

static void
recurring_row_free(gpointer data)
{
	RecurringRow *row = data;

	g_free(row->name);
	g_free(row->kind);
	g_free(row->vendor);
	g_free(row->frequency);
	g_free(row->next_due);
	g_clear_pointer(&row->amount, venture_money_free);
	g_clear_pointer(&row->monthly, venture_money_free);
	g_free(row);
}

/* The recurring module registers its kinds and frequencies as GEnums by
 * nick; read the values back from the type rather than assuming them. */
static gint
recurring_enum_value(
	GType		 enum_type,
	const gchar	*nick
){
	GEnumClass *klass;
	GEnumValue *member;
	gint value;

	klass = g_type_class_ref(enum_type);
	member = g_enum_get_value_by_nick(klass, nick);
	value = (NULL != member) ? member->value : -1;
	g_type_class_unref(klass);

	return value;
}

static gchar *
recurring_nick(
	GType	enum_type,
	gint	value
){
	GEnumClass *klass;
	GEnumValue *member;
	gchar *nick;

	klass = g_type_class_ref(enum_type);
	member = g_enum_get_value(klass, value);
	nick = g_strdup((NULL != member) ? member->value_nick : "?");
	g_type_class_unref(klass);

	return nick;
}

/*
 * What one occurrence of a schedule costs, read from its template the way
 * the recurring service would read it -- into an unsaved record of the
 * document's own type, so the same fields and the same arithmetic apply --
 * and who it goes to. NULL when the template does not carry a price.
 */
static VentureMoney *
recurring_price(
	VentureContext	 *context,
	VentureEntity	 *schedule,
	gboolean	  is_bill,
	gchar		**out_vendor
){
	g_autoptr(JsonParser) parser = NULL;
	g_autofree gchar *text = NULL;
	JsonNode *root;
	JsonObject *object;

	*out_vendor = NULL;
	g_object_get(schedule, "template", &text, NULL);

	if (venture_string_is_empty(text))
		return NULL;

	parser = json_parser_new();

	if (!json_parser_load_from_data(parser, text, -1, NULL))
		return NULL;

	root = json_parser_get_root(parser);

	if ((NULL == root) || !JSON_NODE_HOLDS_OBJECT(root))
		return NULL;

	object = json_node_get_object(root);

	if (!is_bill)
	{
		g_autoptr(VentureEntity) expense = NULL;
		VentureMoney *amount = NULL;

		expense = g_object_new(VENTURE_TYPE_EXPENSE, NULL);

		if (!venture_serializable_from_json(VENTURE_SERIALIZABLE(expense), root, NULL))
			return NULL;

		g_object_get(expense, "amount", &amount, "vendor", out_vendor, NULL);

		if (venture_string_is_empty(*out_vendor))
		{
			g_free(*out_vendor);
			*out_vendor = g_strdup("(no vendor)");
		}

		return amount;
	}
	else
	{
		g_autoptr(VentureMoney) total = NULL;
		JsonNode *lines_node;
		JsonArray *lines;
		gint64 company_id;
		guint i;

		company_id = venture_json_object_get_int(object, "company_id", 0);
		*out_vendor = (0 != company_id) ? cuts_company_name(context, company_id)
		                                : g_strdup("(no vendor)");

		lines_node = json_object_get_member(object, "lines");

		if ((NULL == lines_node) || !JSON_NODE_HOLDS_ARRAY(lines_node))
			return NULL;

		lines = json_node_get_array(lines_node);

		for (i = 0; i < json_array_get_length(lines); i++)
		{
			g_autoptr(VentureEntity) line = NULL;
			g_autoptr(VentureMoney) amount = NULL;
			JsonNode *node;

			node = json_array_get_element(lines, i);

			if (!JSON_NODE_HOLDS_OBJECT(node))
				return NULL;

			line = g_object_new(VENTURE_TYPE_VENDOR_BILL_LINE, NULL);

			if (!venture_serializable_from_json(VENTURE_SERIALIZABLE(line), node, NULL))
				return NULL;

			amount = venture_vendor_bill_line_get_amount(
				VENTURE_VENDOR_BILL_LINE(line), NULL);

			if (NULL == amount)
				return NULL;

			if (NULL == total)
			{
				total = g_steal_pointer(&amount);
				continue;
			}

			/* Lines of one bill in two currencies is not a bill that
			 * can be priced. */
			{
				VentureMoney *next;

				next = venture_money_add(total, amount, NULL);

				if (NULL == next)
					return NULL;

				venture_money_free(total);
				total = next;
			}
		}

		return g_steal_pointer(&total);
	}
}

/* The calendar day a schedule fires on, in its own timezone -- the day the
 * recurring service will generate the document on. */
static gchar *
recurring_day(
	VentureEntity	*schedule,
	GDateTime	*when
){
	g_autofree gchar *name = NULL;
	g_autoptr(GTimeZone) zone = NULL;

	if (NULL == when)
		return g_strdup("");

	g_object_get(schedule, "timezone", &name, NULL);
	zone = g_time_zone_new_identifier(venture_string_is_empty(name) ? "UTC" : name);

	if (NULL == zone)
		zone = g_time_zone_new_utc();

	return venture_time_to_date_string(when, zone);
}

/* One occurrence scaled to a month: 52 weeks and 365 days to the year,
 * twelve months to it, all as exact rationals rounded once. */
static VentureMoney *
recurring_monthly(
	const VentureMoney	*amount,
	const gchar		*frequency
){
	if (0 == g_strcmp0(frequency, "weekly"))
		return venture_money_multiply_rational(amount, 52, 12, NULL);

	if (0 == g_strcmp0(frequency, "daily"))
		return venture_money_multiply_rational(amount, 365, 12, NULL);

	if (0 == g_strcmp0(frequency, "yearly"))
		return venture_money_multiply_rational(amount, 1, 12, NULL);

	return venture_money_copy(amount);
}

static gint
recurring_row_compare(
	gconstpointer	a,
	gconstpointer	b,
	gpointer	user_data
){
	const RecurringRow *left = *(RecurringRow *const *)a;
	const RecurringRow *right = *(RecurringRow *const *)b;
	const gchar *currency = user_data;
	gboolean left_book;
	gboolean right_book;

	/* Unpriced rows last; book currency first; then largest first. */
	if ((NULL == left->monthly) != (NULL == right->monthly))
		return (NULL == left->monthly) ? 1 : -1;

	if (NULL == left->monthly)
		return g_strcmp0(left->name, right->name);

	left_book = (0 == g_strcmp0(venture_money_get_currency(left->monthly),
	                            currency));
	right_book = (0 == g_strcmp0(venture_money_get_currency(right->monthly),
	                             currency));

	if (left_book != right_book)
		return left_book ? -1 : 1;

	if (venture_money_get_amount(left->monthly) !=
	    venture_money_get_amount(right->monthly))
		return (venture_money_get_amount(left->monthly) >
		        venture_money_get_amount(right->monthly)) ? -1 : 1;

	return g_strcmp0(left->name, right->name);
}

static VentureReportResult *
venture_report_recurring_costs(
	VentureContext		 *context,
	VentureDateRange	 *period,
	JsonObject		 *options,
	GError			**error
){
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(GPtrArray) schedules = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GDateTime) now = NULL;
	g_autoptr(VentureMoney) monthly_total = NULL;
	g_autoptr(VentureMoney) annual_total = NULL;
	g_autofree gchar *currency = NULL;
	gint64 organization_id;
	gint64 excluded;
	gint64 unpriced;
	gint kind_bill;
	gint kind_expense;
	guint skipped;
	guint i;

	organization_id = cuts_organization(context, options);
	currency = cuts_currency(context, organization_id);
	now = venture_time_now();
	rows = g_ptr_array_new_with_free_func(recurring_row_free);
	kind_bill = recurring_enum_value(venture_recurring_kind_get_type(), "bill");
	kind_expense = recurring_enum_value(venture_recurring_kind_get_type(), "expense");
	excluded = 0;
	unpriced = 0;
	skipped = 0;

	if (venture_context_module_enabled(context, "recurring"))
	{
		/* Deleted schedules are fetched so the cancelled ones can be
		 * counted as excluded rather than silently absent. */
		schedules = cuts_fetch(context, VENTURE_TYPE_RECURRING_SCHEDULE,
		                       organization_id, NULL, NULL, TRUE, error);

		if (NULL == schedules)
			return NULL;
	}

	for (i = 0; (NULL != schedules) && (i < schedules->len); i++)
	{
		g_autoptr(GDateTime) end_at = NULL;
		g_autoptr(GDateTime) next_run = NULL;
		VentureEntity *schedule;
		RecurringRow *row;
		gint kind = 0;
		gint frequency = 0;
		gboolean paused = FALSE;

		schedule = g_ptr_array_index(schedules, i);
		g_object_get(schedule, "kind", &kind, "frequency", &frequency,
		             "paused", &paused, "end-at", &end_at,
		             "next-run-at", &next_run, NULL);

		/* Invoices are revenue and journals are bookkeeping; neither is
		 * a cost. */
		if ((kind_bill != kind) && (kind_expense != kind))
			continue;

		/* Cancelled (deleted), paused, or run its course. */
		if (venture_entity_is_deleted(schedule) || paused ||
		    ((NULL != end_at) && (g_date_time_compare(end_at, now) < 0)))
		{
			excluded++;
			continue;
		}

		row = g_new0(RecurringRow, 1);
		g_object_get(schedule, "name", &row->name, NULL);
		row->kind = recurring_nick(venture_recurring_kind_get_type(), kind);
		row->frequency = recurring_nick(venture_recurring_frequency_get_type(),
		                                frequency);
		row->next_due = recurring_day(schedule, next_run);
		row->amount = recurring_price(context, schedule, kind_bill == kind,
		                              &row->vendor);

		if (NULL != row->amount)
			row->monthly = recurring_monthly(row->amount, row->frequency);

		if (NULL == row->monthly)
			unpriced++;

		g_ptr_array_add(rows, row);
	}

	g_ptr_array_sort_with_data(rows, recurring_row_compare, currency);

	monthly_total = venture_money_new_zero(currency);

	result = venture_report_result_new("Recurring costs", period);

	venture_report_result_add_column(result, "schedule", "Schedule",
	                                 VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "kind", "Document",
	                                 VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "vendor", "Vendor",
	                                 VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "frequency", "Every",
	                                 VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "amount", "Per occurrence",
	                                 VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "monthly", "Per month",
	                                 VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "next_due", "Next due",
	                                 VENTURE_REPORT_COLUMN_DATE);

	for (i = 0; i < rows->len; i++)
	{
		RecurringRow *row;

		row = g_ptr_array_index(rows, i);

		venture_report_result_begin_row(result);
		venture_report_result_set_text(result, "schedule", row->name);
		venture_report_result_set_text(result, "kind", row->kind);
		venture_report_result_set_text(result, "vendor",
			(NULL != row->vendor) ? row->vendor : "(no vendor)");
		venture_report_result_set_text(result, "frequency", row->frequency);
		venture_report_result_set_text(result, "next_due", row->next_due);

		if (NULL != row->amount)
			venture_report_result_set_money(result, "amount", row->amount);

		if (NULL == row->monthly)
			continue;

		venture_report_result_set_money(result, "monthly", row->monthly);

		if (0 != g_strcmp0(venture_money_get_currency(row->monthly), currency))
		{
			skipped++;
			continue;
		}

		cuts_accumulate(&monthly_total, row->monthly, &skipped);
	}

	annual_total = venture_money_multiply_int(monthly_total, 12, NULL);

	{
		VentureMetric *metric;

		metric = venture_metric_new_money("monthly", "Monthly run-rate", monthly_total);
		venture_metric_set_higher_is_better(metric, FALSE);
		venture_report_result_add_metric(result, metric);

		if (NULL != annual_total)
		{
			metric = venture_metric_new_money("annual", "Annual run-rate", annual_total);
			venture_metric_set_higher_is_better(metric, FALSE);
		}
		else
		{
			metric = venture_metric_new_text("annual", "Annual run-rate", "n/a");
		}

		venture_report_result_add_metric(result, metric);
	}

	venture_report_result_add_metric(result,
		venture_metric_new_count("schedules", "Active schedules",
		                         (gint64)rows->len));
	venture_report_result_add_metric(result,
		venture_metric_new_count("excluded", "Cancelled, paused or ended",
		                         excluded));

	if (!venture_context_module_enabled(context, "recurring"))
		venture_report_result_append_note(result,
			"The recurring module is off, so there are no schedules to "
			"roll up; the run-rate is empty, not zero.");

	if (unpriced > 0)
	{
		g_autofree gchar *note = NULL;

		note = g_strdup_printf(
			"%" G_GINT64_FORMAT " schedule%s could not be priced from "
			"%s template -- no amount, or no lines with a unit price -- "
			"and %s listed without a figure rather than counted as zero.",
			unpriced, (1 == unpriced) ? "" : "s",
			(1 == unpriced) ? "its" : "their", (1 == unpriced) ? "is" : "are");
		venture_report_result_append_note(result, note);
	}

	cuts_flag_skipped(result, skipped, currency);

	return g_steal_pointer(&result);
}

/* ==========================================================================
 * Cash outlook
 * ========================================================================== */

#define CASH_OUTLOOK_DEFAULT_WEEKS	8
#define CASH_OUTLOOK_MAX_WEEKS		104

typedef struct
{
	gchar		*label;
	GDateTime	*from;
	GDateTime	*to;		/* exclusive */
	VentureMoney	*cash_in;
	VentureMoney	*cash_out;
	gboolean	 counted;	/* inside the horizon: feeds the totals */
} CashBucket;

static void
cash_bucket_free(gpointer data)
{
	CashBucket *bucket = data;

	g_free(bucket->label);
	g_clear_pointer(&bucket->from, g_date_time_unref);
	g_clear_pointer(&bucket->to, g_date_time_unref);
	g_clear_pointer(&bucket->cash_in, venture_money_free);
	g_clear_pointer(&bucket->cash_out, venture_money_free);
	g_free(bucket);
}

static CashBucket *
cash_bucket_new(
	const gchar	*label,
	GDateTime	*from,
	GDateTime	*to,
	const gchar	*currency,
	gboolean	 counted
){
	CashBucket *bucket;

	bucket = g_new0(CashBucket, 1);
	bucket->label = g_strdup(label);
	bucket->from = (NULL != from) ? g_date_time_ref(from) : NULL;
	bucket->to = (NULL != to) ? g_date_time_ref(to) : NULL;
	bucket->cash_in = venture_money_new_zero(currency);
	bucket->cash_out = venture_money_new_zero(currency);
	bucket->counted = counted;

	return bucket;
}

/*
 * The bucket a due date falls in. The buckets are laid out overdue, week
 * 1..N, later, undated -- in that order -- so the index is found by
 * walking the dated ones.
 */
static CashBucket *
cash_bucket_for(
	GPtrArray	*buckets,
	GDateTime	*due
){
	guint i;

	if (NULL == due)
		return g_ptr_array_index(buckets, buckets->len - 1);

	for (i = 0; i < buckets->len - 1; i++)
	{
		CashBucket *bucket;

		bucket = g_ptr_array_index(buckets, i);

		if ((NULL == bucket->to) || (g_date_time_compare(due, bucket->to) < 0))
			return bucket;
	}

	return g_ptr_array_index(buckets, buckets->len - 2);
}

static void
cash_bucket_add(
	CashBucket		*bucket,
	gboolean		 inbound,
	const VentureMoney	*amount,
	guint			*inout_skipped
){
	cuts_accumulate(inbound ? &bucket->cash_in : &bucket->cash_out, amount,
	                inout_skipped);
}

static VentureReportResult *
venture_report_cash_outlook(
	VentureContext		 *context,
	VentureDateRange	 *period,
	JsonObject		 *options,
	GError			**error
){
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(GPtrArray) buckets = NULL;
	g_autoptr(GDateTime) start = NULL;
	g_autoptr(GDateTime) horizon = NULL;
	g_autoptr(VentureMetric) opening = NULL;
	g_autoptr(VentureMoney) balance = NULL;
	g_autoptr(VentureMoney) in_total = NULL;
	g_autoptr(VentureMoney) out_total = NULL;
	g_autofree gchar *currency = NULL;
	g_autofree gchar *title = NULL;
	const VentureMoney *bank;
	gint64 organization_id;
	gint64 weeks;
	gint64 undated;
	guint skipped;
	guint i;

	weeks = (NULL != options)
		? venture_json_object_get_int(options, "weeks", CASH_OUTLOOK_DEFAULT_WEEKS)
		: CASH_OUTLOOK_DEFAULT_WEEKS;

	if ((weeks < 1) || (weeks > CASH_OUTLOOK_MAX_WEEKS))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "weeks must be between 1 and %d; %" G_GINT64_FORMAT
		            " is not a horizon", CASH_OUTLOOK_MAX_WEEKS, weeks);
		return NULL;
	}

	organization_id = cuts_organization(context, options);
	currency = cuts_currency(context, organization_id);

	/* The outlook starts where the period does: today for "today" or
	 * "this_week", the first of the month for "2026-04". All-time has no
	 * start, so it starts now. */
	if ((NULL != period) && (NULL != venture_date_range_get_start(period)))
	{
		start = g_date_time_ref(venture_date_range_get_start(period));
	}
	else
	{
		g_autoptr(GDateTime) now = NULL;
		g_autoptr(GDateTime) local = NULL;

		now = venture_time_now();
		local = g_date_time_to_timezone(now, venture_context_get_timezone(context));
		start = g_date_time_new(venture_context_get_timezone(context),
		                        g_date_time_get_year(local),
		                        g_date_time_get_month(local),
		                        g_date_time_get_day_of_month(local), 0, 0, 0);
	}

	horizon = g_date_time_add_days(start, 7 * (gint)weeks);

	buckets = g_ptr_array_new_with_free_func(cash_bucket_free);
	g_ptr_array_add(buckets, cash_bucket_new("Overdue", NULL, start, currency, TRUE));

	for (i = 0; i < (guint)weeks; i++)
	{
		g_autoptr(GDateTime) from = NULL;
		g_autoptr(GDateTime) to = NULL;
		g_autofree gchar *label = NULL;

		from = g_date_time_add_days(start, 7 * (gint)i);
		to = g_date_time_add_days(from, 7);
		label = g_strdup_printf("Week %u", i + 1);
		g_ptr_array_add(buckets, cash_bucket_new(label, from, to, currency, TRUE));
	}

	g_ptr_array_add(buckets, cash_bucket_new("Later", horizon, NULL, currency, FALSE));
	g_ptr_array_add(buckets, cash_bucket_new("Undated", NULL, NULL, currency, FALSE));

	skipped = 0;
	undated = 0;

	/* Cash out: what is still owed on every approved bill, by its due
	 * date. Drafts are not commitments and voids are not anything. */
	if (venture_context_module_enabled(context, "payables"))
	{
		g_autoptr(GPtrArray) bills = NULL;
		VenturePayablesService *payables;

		bills = cuts_fetch(context, VENTURE_TYPE_VENDOR_BILL, organization_id,
		                   NULL, NULL, FALSE, error);

		if (NULL == bills)
			return NULL;

		payables = venture_database_get_payables_service(
			venture_context_get_database(context));

		for (i = 0; i < bills->len; i++)
		{
			g_autofree gchar *status = NULL;
			g_autoptr(GDateTime) due = NULL;
			g_autoptr(VentureMoney) owed = NULL;
			VentureEntity *bill;

			bill = g_ptr_array_index(bills, i);
			g_object_get(bill, "status", &status, "due-date", &due, NULL);

			if ((0 != g_strcmp0(status, "approved")) &&
			    (0 != g_strcmp0(status, "partially_paid")))
				continue;

			owed = venture_payables_service_bill_balance(payables,
				venture_entity_get_id(bill), NULL, error);

			if (NULL == owed)
				return NULL;

			if (venture_money_is_zero(owed) || venture_money_is_negative(owed))
				continue;

			if (NULL == due)
				undated++;

			cash_bucket_add(cash_bucket_for(buckets, due), FALSE, owed, &skipped);
		}
	}

	/* Cash in: what is still owed on every issued invoice, by its due
	 * date. Paid and void ones are settled history. */
	{
		g_autoptr(GPtrArray) invoices = NULL;
		VentureSettlementService *settlement;

		invoices = cuts_fetch(context, VENTURE_TYPE_INVOICE, organization_id,
		                      NULL, NULL, FALSE, error);

		if (NULL == invoices)
			return NULL;

		settlement = venture_settlement_service_get(
			venture_context_get_database(context));

		for (i = 0; i < invoices->len; i++)
		{
			g_autoptr(GDateTime) due = NULL;
			g_autoptr(VentureMoney) owed = NULL;
			VentureEntity *invoice;
			VentureInvoiceStatus status;

			invoice = g_ptr_array_index(invoices, i);
			g_object_get(invoice, "status", &status, "due-at", &due, NULL);

			if ((VENTURE_INVOICE_STATUS_SENT != status) &&
			    (VENTURE_INVOICE_STATUS_PARTIALLY_PAID != status))
				continue;

			owed = venture_settlement_service_invoice_balance(settlement,
				venture_entity_get_id(invoice), NULL, error);

			if (NULL == owed)
				return NULL;

			if (venture_money_is_zero(owed) || venture_money_is_negative(owed))
				continue;

			if (NULL == due)
				undated++;

			cash_bucket_add(cash_bucket_for(buckets, due), TRUE, owed, &skipped);
		}
	}

	/* The opening figure is what the bank last said, and only when it
	 * is in the currency the flows are in; a running balance that mixed
	 * the two would be the confidently wrong number this report exists
	 * not to produce. */
	opening = venture_headline_bank_cash(context, organization_id);
	bank = venture_metric_get_money(opening);

	if ((NULL != bank) &&
	    (0 == g_ascii_strcasecmp(venture_money_get_currency(bank), currency)))
		balance = venture_money_copy(bank);

	in_total = venture_money_new_zero(currency);
	out_total = venture_money_new_zero(currency);

	title = g_strdup_printf("Cash outlook, %" G_GINT64_FORMAT " week%s",
	                        weeks, (1 == weeks) ? "" : "s");
	result = venture_report_result_new(title, period);

	venture_report_result_add_column(result, "bucket", "When",
	                                 VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "from", "From",
	                                 VENTURE_REPORT_COLUMN_DATE);
	venture_report_result_add_column(result, "to", "To",
	                                 VENTURE_REPORT_COLUMN_DATE);
	venture_report_result_add_column(result, "cash_in", "Cash in",
	                                 VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "cash_out", "Cash out",
	                                 VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "net", "Net",
	                                 VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "balance", "Balance",
	                                 VENTURE_REPORT_COLUMN_MONEY);

	for (i = 0; i < buckets->len; i++)
	{
		g_autoptr(VentureMoney) net = NULL;
		g_autoptr(GDateTime) last_day = NULL;
		g_autofree gchar *from = NULL;
		g_autofree gchar *to = NULL;
		CashBucket *bucket;

		bucket = g_ptr_array_index(buckets, i);
		net = venture_money_subtract(bucket->cash_in, bucket->cash_out, NULL);

		/* "To" is the last day inside the bucket, as people read it. */
		if (NULL != bucket->to)
			last_day = g_date_time_add_days(bucket->to, -1);

		from = cuts_day(context, bucket->from);
		to = cuts_day(context, last_day);

		venture_report_result_begin_row(result);
		venture_report_result_set_text(result, "bucket", bucket->label);
		venture_report_result_set_text(result, "from", from);
		venture_report_result_set_text(result, "to", to);
		venture_report_result_set_money(result, "cash_in", bucket->cash_in);
		venture_report_result_set_money(result, "cash_out", bucket->cash_out);

		if (NULL != net)
			venture_report_result_set_money(result, "net", net);

		if (!bucket->counted)
			continue;

		cuts_accumulate(&in_total, bucket->cash_in, &skipped);
		cuts_accumulate(&out_total, bucket->cash_out, &skipped);

		if ((NULL != balance) && (NULL != net))
		{
			VentureMoney *next;

			next = venture_money_add(balance, net, NULL);

			if (NULL != next)
			{
				venture_money_free(balance);
				balance = next;
			}

			venture_report_result_set_money(result, "balance", balance);
		}
	}

	if (NULL != bank)
		venture_report_result_add_metric(result,
			venture_metric_new_money("opening", "Opening bank cash", bank));
	else
		venture_report_result_add_metric(result,
			venture_metric_new_text("opening", "Opening bank cash",
				venture_metric_get_text(opening)));
	venture_report_result_add_metric(result,
		venture_metric_new_money("cash_in", "Expected in", in_total));

	{
		VentureMetric *metric;

		metric = venture_metric_new_money("cash_out", "Expected out", out_total);
		venture_metric_set_higher_is_better(metric, FALSE);
		venture_report_result_add_metric(result, metric);
	}

	if (NULL != balance)
		venture_report_result_add_metric(result,
			venture_metric_new_money("closing", "Projected balance", balance));
	else
		venture_report_result_add_metric(result,
			venture_metric_new_text("closing", "Projected balance", "n/a"));

	venture_report_result_add_metric(result,
		venture_metric_new_count("weeks", "Weeks", weeks));

	if (NULL == bank)
		venture_report_result_append_note(result,
			"There is no bank statement balance to start from, so the "
			"flows are shown without a running balance; import a "
			"statement and the balance appears.");
	else if (NULL == balance)
		venture_report_result_append_note(result,
			"The bank balance is in a different currency from the book, "
			"so no running balance is projected from it.");

	if (undated > 0)
	{
		g_autofree gchar *note = NULL;

		note = g_strdup_printf(
			"%" G_GINT64_FORMAT " open document%s ha%s no due date and %s "
			"listed as undated: nothing is assumed about when %s will be "
			"paid, and %s not in the projected balance.",
			undated, (1 == undated) ? "" : "s", (1 == undated) ? "s" : "ve",
			(1 == undated) ? "is" : "are", (1 == undated) ? "it" : "they",
			(1 == undated) ? "it is" : "they are");
		venture_report_result_append_note(result, note);
	}

	if (!venture_context_module_enabled(context, "payables"))
		venture_report_result_append_note(result,
			"The payables module is off, so no bills are counted as cash "
			"out.");

	cuts_flag_skipped(result, skipped, currency);

	return g_steal_pointer(&result);
}

/* --- Registration --------------------------------------------------------- */

void
venture_pnl_cuts_register_reports(VentureReportRegistry *registry)
{
	g_return_if_fail(VENTURE_IS_REPORT_REGISTRY(registry));

	venture_report_registry_add(registry, VENTURE_REPORT(venture_func_report_new(
		"revenue_by_customer", "Revenue by customer",
		"Paid revenue (receipts less refunds) and issued revenue (invoices "
		"less credit notes) per customer for the period, largest first, "
		"with the lead source each came from; by=source groups by source",
		venture_report_revenue_by_customer)));
	venture_report_registry_add(registry, VENTURE_REPORT(venture_func_report_new(
		"spend_by_vendor", "Spend by vendor",
		"Approved bills and expenses per vendor for the period, largest "
		"first, with the acquisition-flagged share beside the rest; "
		"by=category groups by expense category",
		venture_report_spend_by_vendor)));
	venture_report_registry_add(registry, VENTURE_REPORT(venture_func_report_new(
		"recurring_costs", "Recurring costs",
		"Every running recurring bill and expense schedule priced from its "
		"template and rolled up to a monthly run-rate, with the next due "
		"date; cancelled, paused and ended schedules excluded",
		venture_report_recurring_costs)));
	venture_report_registry_add(registry, VENTURE_REPORT(venture_func_report_new(
		"cash_outlook", "Cash outlook",
		"The next N weeks (weeks, default 8) of cash out from approved "
		"unpaid bills and cash in from issued unpaid invoices by due date, "
		"in weekly buckets from the bank balance; undated documents are "
		"listed, never placed",
		venture_report_cash_outlook)));
}
