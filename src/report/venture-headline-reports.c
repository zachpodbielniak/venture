/*
 * venture-headline-reports.c - CAC, churn, LTV, LTV:CAC and cohorts
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The numbers a business is run on, from records the other modules already
 * keep: the cash sales the settlement service derives from receipts and
 * refunds, campaign spend and flagged expense and bill lines, recurring
 * schedules and subscriptions, worklog hours.
 *
 * Every amount is a VentureMoney carried in the organisation's book
 * currency; anything in another currency is counted and reported rather
 * than converted. Every ratio is integer arithmetic in basis points, rounded
 * once and half to even, and only becomes a fraction at the moment it is
 * rendered. Every division has its zero named and refused with "n/a" rather
 * than performed.
 *
 * Everything is read through a VentureHeadlineSnapshot: fetched once, then
 * derived in memory for every period a caller asks about, so the home page
 * reads two periods of four reports from one set of queries.
 */

#include "venture.h"
#include "report/venture-headline-private.h"

#include <string.h>

/* An IN list is bound one parameter per value; keeping each statement to a
 * few hundred stays far inside every backend's parameter limit. */
#define HEADLINE_IN_CHUNK (400)

/* How far apart receipts are expected, in days, for each known cadence.
 * Each is the longest such interval -- a 31-day month, a leap year -- so a
 * customer paying exactly on time is never read as late. */
#define HEADLINE_GAP_DAY (1)
#define HEADLINE_GAP_WEEK (7)
#define HEADLINE_GAP_MONTH (31)
#define HEADLINE_GAP_YEAR (366)

/* The trailing window every projection is measured over, in months. */
#define HEADLINE_WINDOW_MONTHS (12)

/* --- Shared plumbing ------------------------------------------------------ */

/* One dated, signed cash movement for a customer: a cash sale derived from
 * an applied receipt is positive, one derived from a refund negative. The
 * invoice is the one the money settled, which is what decides the period a
 * receipt pays for. */
typedef struct
{
	GDateTime	*date;
	VentureMoney	*amount;
	gint64		 invoice_id;
} HeadlineCash;

static void
headline_cash_free(gpointer data)
{
	HeadlineCash *cash = data;

	g_clear_pointer(&cash->date, g_date_time_unref);
	g_clear_pointer(&cash->amount, venture_money_free);
	g_free(cash);
}

static gint
headline_cash_compare(
	gconstpointer	a,
	gconstpointer	b
){
	const HeadlineCash *left = *(HeadlineCash *const *)a;
	const HeadlineCash *right = *(HeadlineCash *const *)b;

	return g_date_time_compare(left->date, right->date);
}

/* One flagged expense or vendor bill line, reduced to what the reports ask
 * of it. */
typedef struct
{
	GDateTime	*date;
	VentureMoney	*amount;
	gint64		 campaign_id;
	gboolean	 acquisition;
	gboolean	 cost_of_revenue;
	gboolean	 bill_line;
} HeadlineSpend;

static void
headline_spend_free(gpointer data)
{
	HeadlineSpend *spend = data;

	g_clear_pointer(&spend->date, g_date_time_unref);
	g_clear_pointer(&spend->amount, venture_money_free);
	g_free(spend);
}

/* One outreach campaign's own spend figure and the span it ran over. */
typedef struct
{
	gint64		 id;
	gchar		*name;
	VentureMoney	*spend;
	GDateTime	*started;
	GDateTime	*ended;
} HeadlineCampaign;

static void
headline_campaign_free(gpointer data)
{
	HeadlineCampaign *campaign = data;

	g_free(campaign->name);
	g_clear_pointer(&campaign->spend, venture_money_free);
	g_clear_pointer(&campaign->started, g_date_time_unref);
	g_clear_pointer(&campaign->ended, g_date_time_unref);
	g_free(campaign);
}

/* The lead a company was converted from: where the customer came from. */
typedef struct
{
	gchar		*source;
	gint64		 campaign_id;
} HeadlineLead;

static void
headline_lead_free(gpointer data)
{
	HeadlineLead *lead = data;

	g_free(lead->source);
	g_free(lead);
}

/*
 * One recurring commitment of a customer, from either source: an invoice
 * schedule, or a subscription's dated status history. A schedule is running
 * from start_at until the earlier of its end_at and its deletion; its
 * paused flag carries no date, so a paused schedule is neither running nor
 * gone at any moment. A subscription's state at a moment is the to-status
 * of its latest event before that moment.
 */
typedef enum
{
	HEADLINE_RECURRING_NONE = 0,
	HEADLINE_RECURRING_PAUSED,
	HEADLINE_RECURRING_RUNNING
} HeadlineRecurringState;

typedef struct
{
	gint64		 company_id;
	gboolean	 subscription;
	gboolean	 paused;
	GDateTime	*started;
	GDateTime	*ended;
	GDateTime	*deleted;
	GPtrArray	*events;
} HeadlineRecurring;

static void
headline_recurring_free(gpointer data)
{
	HeadlineRecurring *recurring = data;

	g_clear_pointer(&recurring->started, g_date_time_unref);
	g_clear_pointer(&recurring->ended, g_date_time_unref);
	g_clear_pointer(&recurring->deleted, g_date_time_unref);
	g_clear_pointer(&recurring->events, g_ptr_array_unref);
	g_free(recurring);
}

/* A subscription event reduced to when it happened and what it left. */
typedef struct
{
	GDateTime	*at;
	gint		 status;
} HeadlineStatusEvent;

static void
headline_status_event_free(gpointer data)
{
	HeadlineStatusEvent *event = data;

	g_clear_pointer(&event->at, g_date_time_unref);
	g_free(event);
}

struct _VentureHeadlineSnapshot
{
	VentureContext	*context;
	JsonObject	*options;
	gint64		 organization_id;
	gint64		 venture_id;
	gint64		 days;
	GDateTime	*now;
	GDateTime	*cutoff;
	gchar		*currency;

	gboolean	 settings_loaded;
	gint64		 minimum_months;
	gint64		 activity_days;
	VentureMoney	*hourly_rate;

	GHashTable	*cash;
	guint		 cash_skipped;
	GHashTable	*cadence;
	GHashTable	*companies;

	GPtrArray	*spend;
	guint		 spend_skipped;
	GHashTable	*linked_campaigns;
	GHashTable	*campaigns;
	GHashTable	*leads;

	GPtrArray	*recurring;
	gboolean	 billing_history;

	GPtrArray	*schedule_rows;
	GPtrArray	*event_rows;
	GPtrArray	*price_rows;

	GHashTable	*ticket_company;
};

static gint64 *
headline_key(gint64 id)
{
	gint64 *key;

	key = g_new(gint64, 1);
	*key = id;

	return key;
}

static gint64
headline_int(
	gpointer	 object,
	const gchar	*property
){
	gint64 value = 0;

	g_object_get(object, property, &value, NULL);

	return value;
}

/* A table keyed by 64-bit ids. Casting an id through a pointer-sized int
 * truncates it on a platform where gint is 32 bits, and two customers then
 * share one history. */
static GHashTable *
headline_id_table(GDestroyNotify value_free)
{
	return g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free,
	                             value_free);
}

JsonObject *
venture_headline_snapshot_get_options(VentureHeadlineSnapshot *snapshot)
{
	g_return_val_if_fail(NULL != snapshot, NULL);

	return snapshot->options;
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

VentureHeadlineSnapshot *
venture_headline_snapshot_new(
	VentureContext	 *context,
	JsonObject	 *options,
	GError		**error
){
	VentureHeadlineSnapshot *snapshot;
	g_autoptr(GDateTime) as_of = NULL;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);

	if ((NULL != options) && json_object_has_member(options, "as_of"))
	{
		as_of = venture_period_report_as_of(options, error);

		if (NULL == as_of)
			return NULL;
	}

	snapshot = g_new0(VentureHeadlineSnapshot, 1);
	snapshot->context = g_object_ref(context);
	snapshot->organization_id = (NULL != options)
		? venture_json_object_get_int(options, "organization_id", 0) : 0;

	if (0 == snapshot->organization_id)
		snapshot->organization_id =
			venture_context_get_default_organization_id(context);

	snapshot->venture_id = (NULL != options)
		? venture_json_object_get_int(options, "venture_id", 0) : 0;
	snapshot->days = (NULL != options)
		? venture_json_object_get_int(options, "days", 0) : 0;
	snapshot->now = venture_time_now();
	snapshot->currency = headline_currency(context, snapshot->organization_id);

	/* as_of names the last instant that counts; the cutoff is the first
	 * that does not, so every comparison here stays half-open. */
	if (NULL != as_of)
		snapshot->cutoff = g_date_time_add(as_of, 1);

	/* What every report run on the snapshot's behalf is handed, so the
	 * P&L and the billing reports answer about exactly the same rows. */
	snapshot->options = json_object_new();
	json_object_set_int_member(snapshot->options, "organization_id",
	                           snapshot->organization_id);
	json_object_set_string_member(snapshot->options, "currency",
	                              snapshot->currency);

	if (0 != snapshot->venture_id)
		json_object_set_int_member(snapshot->options, "venture_id",
		                           snapshot->venture_id);

	if (NULL != as_of)
		json_object_set_string_member(snapshot->options, "as_of",
			venture_json_object_get_string(options, "as_of", ""));

	return snapshot;
}

void
venture_headline_snapshot_free(VentureHeadlineSnapshot *snapshot)
{
	if (NULL == snapshot)
		return;

	g_clear_object(&snapshot->context);
	g_clear_pointer(&snapshot->options, json_object_unref);
	g_clear_pointer(&snapshot->now, g_date_time_unref);
	g_clear_pointer(&snapshot->cutoff, g_date_time_unref);
	g_clear_pointer(&snapshot->currency, g_free);
	g_clear_pointer(&snapshot->hourly_rate, venture_money_free);
	g_clear_pointer(&snapshot->cash, g_hash_table_unref);
	g_clear_pointer(&snapshot->cadence, g_hash_table_unref);
	g_clear_pointer(&snapshot->companies, g_hash_table_unref);
	g_clear_pointer(&snapshot->spend, g_ptr_array_unref);
	g_clear_pointer(&snapshot->linked_campaigns, g_hash_table_unref);
	g_clear_pointer(&snapshot->campaigns, g_hash_table_unref);
	g_clear_pointer(&snapshot->leads, g_hash_table_unref);
	g_clear_pointer(&snapshot->recurring, g_ptr_array_unref);
	g_clear_pointer(&snapshot->schedule_rows, g_ptr_array_unref);
	g_clear_pointer(&snapshot->event_rows, g_ptr_array_unref);
	g_clear_pointer(&snapshot->price_rows, g_ptr_array_unref);
	g_clear_pointer(&snapshot->ticket_company, g_hash_table_unref);
	g_free(snapshot);
}

/*
 * A query scoped the way every headline read is: the organisation, no page
 * limit, id order. @scoped applies the as_of cutoff to soft deletion, the
 * same visibility rule the P&L uses, so a report run "as of" a date does
 * not lose a row deleted after it.
 */
static VentureQuery *
headline_query(
	VentureHeadlineSnapshot	 *snapshot,
	GType			  entity_type,
	gboolean		  include_deleted,
	gboolean		  scoped,
	GError			**error
){
	g_autoptr(VentureQuery) query = NULL;

	query = venture_query_new(entity_type);
	venture_query_set_organization(query, snapshot->organization_id);
	venture_query_set_limit(query, 0);
	venture_query_set_include_deleted(query, include_deleted);

	if (!venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, error))
		return NULL;

	if (scoped && !venture_period_report_scope(query, snapshot->options, error))
		return NULL;

	return g_steal_pointer(&query);
}

static GPtrArray *
headline_find(
	VentureHeadlineSnapshot	 *snapshot,
	VentureQuery		 *query,
	GError			**error
){
	return venture_database_find(
		venture_context_get_database(snapshot->context), query, error);
}

/* Every row of a type whose integer @field is positive: the rows that
 * name a sale, an invoice, a campaign. */
static GPtrArray *
headline_fetch_positive(
	VentureHeadlineSnapshot	 *snapshot,
	GType			  entity_type,
	const gchar		 *field,
	gboolean		  include_deleted,
	gboolean		  scoped,
	GError			**error
){
	g_autoptr(VentureQuery) query = NULL;

	query = headline_query(snapshot, entity_type, include_deleted, scoped,
	                       error);

	if ((NULL == query) ||
	    !venture_query_add_filter_int(query, field, VENTURE_FILTER_OP_GT, 0,
	                                  error))
		return NULL;

	return headline_find(snapshot, query, error);
}

/* Every row of a type whose string or boolean @field equals @value. */
static GPtrArray *
headline_fetch_equal(
	VentureHeadlineSnapshot	 *snapshot,
	GType			  entity_type,
	const gchar		 *field,
	const gchar		 *value,
	gboolean		  include_deleted,
	gboolean		  scoped,
	GError			**error
){
	g_autoptr(VentureQuery) query = NULL;

	query = headline_query(snapshot, entity_type, include_deleted, scoped,
	                       error);

	if ((NULL == query) ||
	    !venture_query_add_filter_string(query, field, VENTURE_FILTER_OP_EQ,
	                                     value, error))
		return NULL;

	return headline_find(snapshot, query, error);
}

static GPtrArray *
headline_fetch_all(
	VentureHeadlineSnapshot	 *snapshot,
	GType			  entity_type,
	gboolean		  include_deleted,
	gboolean		  scoped,
	GError			**error
){
	g_autoptr(VentureQuery) query = NULL;

	query = headline_query(snapshot, entity_type, include_deleted, scoped,
	                       error);

	if (NULL == query)
		return NULL;

	return headline_find(snapshot, query, error);
}

/*
 * The rows of a type with the given ids, in batches: one statement per few
 * hundred ids rather than one per id, which is the difference between a
 * page that reads ten queries and one that reads ten thousand.
 */
static GPtrArray *
headline_fetch_ids(
	VentureHeadlineSnapshot	 *snapshot,
	GType			  entity_type,
	GHashTable		 *ids,
	gboolean		  include_deleted,
	gboolean		  scoped,
	GError			**error
){
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GPtrArray) values = NULL;
	GHashTableIter iter;
	gpointer key;
	guint remaining;

	rows = g_ptr_array_new_with_free_func(g_object_unref);
	values = g_ptr_array_new_with_free_func(g_free);
	remaining = g_hash_table_size(ids);
	g_hash_table_iter_init(&iter, ids);

	while (g_hash_table_iter_next(&iter, &key, NULL))
	{
		g_ptr_array_add(values, g_strdup_printf("%" G_GINT64_FORMAT,
		                                        *(gint64 *)key));
		remaining--;

		if ((values->len < HEADLINE_IN_CHUNK) && (remaining > 0))
			continue;

		{
			g_autoptr(VentureQuery) query = NULL;
			g_autoptr(GPtrArray) batch = NULL;
			guint i;

			query = headline_query(snapshot, entity_type,
			                       include_deleted, scoped, error);

			if ((NULL == query) ||
			    !venture_query_add_filter(query, "id",
			                              VENTURE_FILTER_OP_IN, values,
			                              error))
				return NULL;

			batch = headline_find(snapshot, query, error);

			if (NULL == batch)
				return NULL;

			for (i = 0; i < batch->len; i++)
				g_ptr_array_add(rows,
					g_object_ref(g_ptr_array_index(batch, i)));

			g_ptr_array_set_size(values, 0);
		}
	}

	return g_steal_pointer(&rows);
}

/* Adds an id to a set of ids, ignoring the zero that means "none". */
static void
headline_id_add(
	GHashTable	*set,
	gint64		 id
){
	if ((0 != id) && !g_hash_table_contains(set, &id))
		g_hash_table_add(set, headline_key(id));
}

/*
 * Adds an amount into a total kept in one currency, counting rather than
 * hiding the ones that cannot join: a different currency, or overflow.
 * Returns whether the amount made it in, so a caller that counts something
 * alongside the money -- a customer-month -- counts only what was summed.
 */
static gboolean
headline_accumulate(
	VentureMoney		**total,
	const VentureMoney	 *amount,
	guint			 *inout_skipped
){
	VentureMoney *next;

	if (NULL == amount)
		return FALSE;

	next = venture_money_add(*total, amount, NULL);

	if (NULL == next)
	{
		if (NULL != inout_skipped)
			(*inout_skipped)++;
		return FALSE;
	}

	venture_money_free(*total);
	*total = next;

	return TRUE;
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

/* Half-open membership, with a missing bound meaning unbounded. */
static gboolean
headline_between(
	GDateTime	*when,
	GDateTime	*start,
	GDateTime	*end
){
	if (NULL == when)
		return FALSE;

	if ((NULL != start) && (g_date_time_compare(when, start) < 0))
		return FALSE;

	if ((NULL != end) && (g_date_time_compare(when, end) >= 0))
		return FALSE;

	return TRUE;
}

static gboolean
headline_within(
	GDateTime		*when,
	const VentureDateRange	*period
){
	if (NULL == period)
		return NULL != when;

	return headline_between(when, venture_date_range_get_start(period),
	                        venture_date_range_get_end(period));
}

/* The earlier of two instants, either of which may be missing. */
static GDateTime *
headline_earliest(
	GDateTime	*a,
	GDateTime	*b
){
	if (NULL == a)
		return (NULL != b) ? g_date_time_ref(b) : NULL;

	if ((NULL == b) || (g_date_time_compare(a, b) <= 0))
		return g_date_time_ref(a);

	return g_date_time_ref(b);
}

/* Now, or the as_of cutoff when that is earlier: nothing after it has
 * happened as far as the report is concerned. */
static GDateTime *
headline_now(VentureHeadlineSnapshot *snapshot)
{
	return headline_earliest(snapshot->now, snapshot->cutoff);
}

/*
 * The end a trailing window is measured back from: the period's end, now,
 * or the as_of cutoff, whichever is earliest -- the same anchor the
 * receivables aging uses, and for the same reason. A period still in
 * progress has an end that has not happened yet.
 */
static GDateTime *
headline_anchor(
	VentureHeadlineSnapshot	*snapshot,
	const VentureDateRange	*period
){
	g_autoptr(GDateTime) now = NULL;
	GDateTime *end;

	now = headline_now(snapshot);
	end = (NULL != period) ? venture_date_range_get_end(period) : NULL;

	return headline_earliest(end, now);
}

/*
 * @numerator over @denominator in basis points, rounded once, half to
 * even: two customers of three is 6,667, not the 6,666 that truncation
 * gives. Rounding through money arithmetic keeps the one rounding rule this
 * codebase has. The caller has already refused a zero denominator; an
 * overflow answers FALSE.
 */
static gboolean
headline_scaled(
	gint64	 numerator,
	gint64	 scale,
	gint64	 denominator,
	gint64	*out_value
){
	g_autoptr(VentureMoney) count = NULL;
	g_autoptr(VentureMoney) scaled = NULL;

	count = venture_money_new(numerator, "XXX", 0);
	scaled = venture_money_multiply_rational(count, scale, denominator, NULL);

	if (NULL == scaled)
		return FALSE;

	*out_value = venture_money_get_amount(scaled);

	return TRUE;
}

static gint64
headline_basis_points(
	gint64	numerator,
	gint64	denominator
){
	gint64 value = 0;

	if (!headline_scaled(numerator, 10000, denominator, &value))
		return (numerator > 0) ? G_MAXINT64 : 0;

	return value;
}

/* Renders basis points as the fraction a ratio metric or a percent column
 * expects. This is the only place a ratio becomes a double, and it is at
 * the presentation boundary, after every computation is done. */
static gdouble
headline_render_bps(gint64 basis_points)
{
	return (gdouble)basis_points / 10000.0;
}

/* --- Settings ------------------------------------------------------------- */

static void
headline_load_settings(VentureHeadlineSnapshot *snapshot)
{
	g_autoptr(VentureEntity) setting = NULL;
	gint64 minimum = 0;
	gint64 days = 0;

	if (snapshot->settings_loaded)
		return;

	snapshot->settings_loaded = TRUE;
	setting = venture_headline_setting_find(
		venture_context_get_database(snapshot->context),
		snapshot->organization_id);

	if (NULL != setting)
		g_object_get(setting, "minimum-customer-months", &minimum,
		             "activity-days", &days, "hourly-rate",
		             &snapshot->hourly_rate, NULL);

	snapshot->minimum_months = (minimum > 0) ? minimum : 12;
	snapshot->activity_days = (days > 0) ? days : 90;
}

/* The quiet threshold in days: the option, else the organisation's
 * setting, else ninety. */
static gint64
headline_days(VentureHeadlineSnapshot *snapshot)
{
	headline_load_settings(snapshot);

	if (snapshot->days > 0)
		return snapshot->days;

	return snapshot->activity_days;
}

/* --- Companies ----------------------------------------------------------- */

/* What the reports show of a company: its name, and its own source for a
 * customer with no lead behind it. */
typedef struct
{
	gchar		*name;
	gchar		*source;
} HeadlineCompany;

static void
headline_company_free(gpointer data)
{
	HeadlineCompany *company = data;

	g_free(company->name);
	g_free(company->source);
	g_free(company);
}

/* Loads the companies in @ids that are not already known, in one batched
 * read rather than one read per row of the report. */
static gboolean
headline_load_companies(
	VentureHeadlineSnapshot	 *snapshot,
	GHashTable		 *ids,
	GError			**error
){
	g_autoptr(GHashTable) missing = NULL;
	g_autoptr(GPtrArray) companies = NULL;
	GHashTableIter iter;
	gpointer key;
	guint i;

	if (NULL == snapshot->companies)
		snapshot->companies = headline_id_table(headline_company_free);

	missing = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	g_hash_table_iter_init(&iter, ids);

	while (g_hash_table_iter_next(&iter, &key, NULL))
		if (!g_hash_table_contains(snapshot->companies, key))
			headline_id_add(missing, *(gint64 *)key);

	if (0 == g_hash_table_size(missing))
		return TRUE;

	companies = headline_fetch_ids(snapshot, VENTURE_TYPE_COMPANY, missing,
	                               TRUE, FALSE, error);

	if (NULL == companies)
		return FALSE;

	for (i = 0; i < companies->len; i++)
	{
		VentureEntity *record = g_ptr_array_index(companies, i);
		HeadlineCompany *company;

		company = g_new0(HeadlineCompany, 1);
		g_object_get(record, "name", &company->name, "source",
		             &company->source, NULL);
		g_hash_table_replace(snapshot->companies,
			headline_key(venture_entity_get_id(record)), company);
	}

	return TRUE;
}

static HeadlineCompany *
headline_company(
	VentureHeadlineSnapshot	*snapshot,
	gint64			 company_id
){
	return (NULL != snapshot->companies)
		? g_hash_table_lookup(snapshot->companies, &company_id) : NULL;
}

static gchar *
headline_company_name(
	VentureHeadlineSnapshot	*snapshot,
	gint64			 company_id
){
	HeadlineCompany *company;

	company = headline_company(snapshot, company_id);

	if ((NULL != company) && !venture_string_is_empty(company->name))
		return g_strdup(company->name);

	return g_strdup_printf("Company #%" G_GINT64_FORMAT, company_id);
}

/* --- Shared rows ----------------------------------------------------------- */

/*
 * Rows two parts of the snapshot both read -- receipt cadence and recurring
 * churn -- are fetched once and kept: every invoice schedule and every plan
 * price and subscription event of the organisation, deleted ones included,
 * because a deletion or a superseded price is still history.
 */
static GPtrArray *
headline_cached_rows(
	VentureHeadlineSnapshot	 *snapshot,
	GPtrArray		**slot,
	GType			  entity_type,
	GError			**error
){
	if (NULL == *slot)
		*slot = headline_fetch_all(snapshot, entity_type, TRUE, FALSE, error);

	return *slot;
}

/* --- Customer cash -------------------------------------------------------- */

/*
 * Every customer's cash history, keyed by company id.
 *
 * Cash is decided by the settlement service, not reconstructed here. It
 * derives a cash sale whenever money a customer paid is applied to one of
 * their invoices -- a receipt applied at once, or a deposit or overpayment
 * applied later -- and a negative cash adjustment whenever an applied
 * receipt is refunded. It derives nothing for a credit note or a write-off
 * applied to an invoice, nothing for money received but not yet applied,
 * and nothing for the refund of such unapplied money. So an allocation or a
 * refund is cash exactly when it names a sale, and the amount is that
 * sale's gross, which is the book-currency value for a foreign invoice.
 *
 * Worked through: a $500 invoice paid with $600 and $100 of the excess
 * refunded is $500 of cash; a $6,000 retainer applied to twelve $500
 * invoices is twelve $500 receipts on the dates they were applied.
 */
static gboolean
headline_load_cash(
	VentureHeadlineSnapshot	 *snapshot,
	GError			**error
){
	g_autoptr(GHashTable) cash = NULL;
	g_autoptr(GHashTable) sale_ids = NULL;
	g_autoptr(GHashTable) invoice_ids = NULL;
	g_autoptr(GHashTable) sales_by_id = NULL;
	g_autoptr(GHashTable) invoice_company = NULL;
	g_autoptr(GHashTable) allocation_invoice = NULL;
	g_autoptr(GPtrArray) allocations = NULL;
	g_autoptr(GPtrArray) refunds = NULL;
	g_autoptr(GPtrArray) sales = NULL;
	g_autoptr(GPtrArray) invoices = NULL;
	GHashTableIter iter;
	gpointer value;
	guint i;

	if (NULL != snapshot->cash)
		return TRUE;

	allocations = headline_fetch_positive(snapshot,
		VENTURE_TYPE_PAYMENT_ALLOCATION, "sale-id", FALSE, TRUE, error);

	if (NULL == allocations)
		return FALSE;

	refunds = headline_fetch_positive(snapshot, VENTURE_TYPE_REFUND,
	                                  "sale-id", FALSE, TRUE, error);

	if (NULL == refunds)
		return FALSE;

	sale_ids = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	invoice_ids = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free,
	                                    NULL);
	allocation_invoice = headline_id_table(g_free);

	for (i = 0; i < allocations->len; i++)
	{
		VentureEntity *allocation = g_ptr_array_index(allocations, i);

		headline_id_add(sale_ids, headline_int(allocation, "sale-id"));
		headline_id_add(invoice_ids, headline_int(allocation, "invoice-id"));
		g_hash_table_replace(allocation_invoice,
			headline_key(venture_entity_get_id(allocation)),
			headline_key(headline_int(allocation, "invoice-id")));
	}

	for (i = 0; i < refunds->len; i++)
		headline_id_add(sale_ids,
			headline_int(g_ptr_array_index(refunds, i), "sale-id"));

	sales = headline_fetch_ids(snapshot, VENTURE_TYPE_SALE, sale_ids, FALSE,
	                           TRUE, error);

	if (NULL == sales)
		return FALSE;

	/* An invoice deleted after it was paid does not unpay it: the company
	 * is read from deleted invoices too. */
	invoices = headline_fetch_ids(snapshot, VENTURE_TYPE_INVOICE, invoice_ids,
	                              TRUE, FALSE, error);

	if (NULL == invoices)
		return FALSE;

	sales_by_id = headline_id_table(NULL);

	for (i = 0; i < sales->len; i++)
		g_hash_table_replace(sales_by_id,
			headline_key(venture_entity_get_id(g_ptr_array_index(sales, i))),
			g_ptr_array_index(sales, i));

	invoice_company = headline_id_table(g_free);

	for (i = 0; i < invoices->len; i++)
	{
		VentureEntity *invoice = g_ptr_array_index(invoices, i);

		g_hash_table_replace(invoice_company,
			headline_key(venture_entity_get_id(invoice)),
			headline_key(headline_int(invoice, "company-id")));
	}

	cash = headline_id_table((GDestroyNotify)g_ptr_array_unref);

	for (i = 0; i < allocations->len + refunds->len; i++)
	{
		g_autoptr(GDateTime) date = NULL;
		g_autoptr(VentureMoney) gross = NULL;
		VentureEntity *record;
		VentureEntity *sale;
		HeadlineCash *movement;
		GPtrArray *history;
		gint64 sale_id;
		gint64 company_id;
		gint64 invoice_id;
		gint64 *found;
		gboolean refund;

		refund = (i >= allocations->len);
		record = refund ? g_ptr_array_index(refunds, i - allocations->len)
		                : g_ptr_array_index(allocations, i);
		sale_id = headline_int(record, "sale-id");
		sale = g_hash_table_lookup(sales_by_id, &sale_id);

		/* A sale deleted since -- a rolled-back cutover -- is cash the
		 * books no longer claim. */
		if (NULL == sale)
			continue;

		if ((0 != snapshot->venture_id) &&
		    (headline_int(sale, "venture-id") != snapshot->venture_id))
			continue;

		g_object_get(sale, "occurred-at", &date, "gross", &gross, NULL);

		if ((NULL == date) || (NULL == gross))
			continue;

		if ((NULL != snapshot->cutoff) &&
		    (g_date_time_compare(date, snapshot->cutoff) >= 0))
			continue;

		if (refund)
		{
			gint64 allocation_id;

			company_id = headline_int(record, "customer-id");
			allocation_id = headline_int(record, "allocation-id");
			found = g_hash_table_lookup(allocation_invoice, &allocation_id);
			invoice_id = (NULL != found) ? *found : 0;
		}
		else
		{
			invoice_id = headline_int(record, "invoice-id");
			found = g_hash_table_lookup(invoice_company, &invoice_id);
			company_id = (NULL != found) ? *found : 0;
		}

		if (0 == company_id)
			continue;

		/* A sale in another currency is counted, never converted. */
		if (0 != g_ascii_strcasecmp(venture_money_get_currency(gross),
		                            snapshot->currency))
		{
			snapshot->cash_skipped++;
			continue;
		}

		history = g_hash_table_lookup(cash, &company_id);

		if (NULL == history)
		{
			history = g_ptr_array_new_with_free_func(headline_cash_free);
			g_hash_table_replace(cash, headline_key(company_id), history);
		}

		movement = g_new0(HeadlineCash, 1);
		movement->date = g_steal_pointer(&date);
		movement->amount = g_steal_pointer(&gross);
		movement->invoice_id = invoice_id;
		g_ptr_array_add(history, movement);
	}

	g_hash_table_iter_init(&iter, cash);

	while (g_hash_table_iter_next(&iter, NULL, &value))
		g_ptr_array_sort(value, headline_cash_compare);

	snapshot->cash = g_steal_pointer(&cash);

	return TRUE;
}

/*
 * How far apart the receipts an invoice belongs to are meant to be, in
 * days, keyed by invoice id: a billing subscription's price interval (month
 * or year), else the recurring schedule that generated the invoice
 * (monthly, weekly, daily, yearly). An invoice that is in neither has no
 * known cadence. Billing wins when both claim an invoice, because its
 * events are dated and its terms immutable.
 */
static gboolean
headline_load_cadence(
	VentureHeadlineSnapshot	 *snapshot,
	GError			**error
){
	g_autoptr(GHashTable) cadence = NULL;
	guint i;

	if (NULL != snapshot->cadence)
		return TRUE;

	cadence = headline_id_table(NULL);

	if (venture_context_module_enabled(snapshot->context, "billing"))
	{
		GPtrArray *events;
		GPtrArray *prices;
		g_autoptr(GHashTable) interval = NULL;

		events = headline_cached_rows(snapshot, &snapshot->event_rows,
			VENTURE_TYPE_SUBSCRIPTION_EVENT, error);

		if (NULL == events)
			return FALSE;

		prices = headline_cached_rows(snapshot, &snapshot->price_rows,
			VENTURE_TYPE_PLAN_PRICE, error);

		if (NULL == prices)
			return FALSE;

		interval = headline_id_table(NULL);

		for (i = 0; i < prices->len; i++)
		{
			VentureEntity *price = g_ptr_array_index(prices, i);
			gint kind = 0;

			g_object_get(price, "interval", &kind, NULL);
			g_hash_table_replace(interval,
				headline_key(venture_entity_get_id(price)),
				GINT_TO_POINTER((1 == kind) ? HEADLINE_GAP_YEAR
				                            : HEADLINE_GAP_MONTH));
		}

		for (i = 0; i < events->len; i++)
		{
			VentureEntity *event = g_ptr_array_index(events, i);
			gint64 price_id;
			gpointer gap;

			if (headline_int(event, "invoice-id") <= 0)
				continue;

			price_id = headline_int(event, "to-plan-price-id");

			if (0 == price_id)
				price_id = headline_int(event, "from-plan-price-id");

			gap = g_hash_table_lookup(interval, &price_id);

			if (NULL != gap)
				g_hash_table_replace(cadence,
					headline_key(headline_int(event, "invoice-id")), gap);
		}
	}

	if (venture_context_module_enabled(snapshot->context, "recurring"))
	{
		g_autoptr(GPtrArray) occurrences = NULL;
		GPtrArray *schedules;
		g_autoptr(GHashTable) frequency = NULL;

		occurrences = headline_fetch_equal(snapshot,
			VENTURE_TYPE_RECURRING_OCCURRENCE, "document-type", "invoice",
			TRUE, FALSE, error);

		if (NULL == occurrences)
			return FALSE;

		schedules = headline_cached_rows(snapshot, &snapshot->schedule_rows,
			VENTURE_TYPE_RECURRING_SCHEDULE, error);

		if (NULL == schedules)
			return FALSE;

		frequency = headline_id_table(NULL);

		for (i = 0; i < schedules->len; i++)
		{
			VentureEntity *schedule = g_ptr_array_index(schedules, i);
			gint kind = 0;
			gint gap;

			g_object_get(schedule, "frequency", &kind, NULL);
			gap = (3 == kind) ? HEADLINE_GAP_YEAR
				: ((1 == kind) ? HEADLINE_GAP_WEEK
				: ((2 == kind) ? HEADLINE_GAP_DAY : HEADLINE_GAP_MONTH));
			g_hash_table_replace(frequency,
				headline_key(venture_entity_get_id(schedule)),
				GINT_TO_POINTER(gap));
		}

		for (i = 0; i < occurrences->len; i++)
		{
			VentureEntity *occurrence = g_ptr_array_index(occurrences, i);
			gint64 schedule_id;
			gint64 invoice_id;
			gpointer gap;

			schedule_id = headline_int(occurrence, "schedule-id");
			invoice_id = headline_int(occurrence, "document-id");
			gap = g_hash_table_lookup(frequency, &schedule_id);

			if ((NULL != gap) && (0 != invoice_id) &&
			    !g_hash_table_contains(cadence, &invoice_id))
				g_hash_table_replace(cadence, headline_key(invoice_id), gap);
		}
	}

	snapshot->cadence = g_steal_pointer(&cadence);

	return TRUE;
}

/* The expected gap in days for a receipt on @invoice_id, or 0 when its
 * cadence is not known. */
static gint64
headline_cadence_gap(
	VentureHeadlineSnapshot	*snapshot,
	gint64			 invoice_id
){
	gpointer gap;

	if ((NULL == snapshot->cadence) || (0 == invoice_id))
		return 0;

	gap = g_hash_table_lookup(snapshot->cadence, &invoice_id);

	return (NULL != gap) ? GPOINTER_TO_INT(gap) : 0;
}

/* How many months one receipt pays for: twelve on a yearly cadence, else
 * the month it was received in. */
static gint
headline_cadence_months(
	VentureHeadlineSnapshot	*snapshot,
	gint64			 invoice_id
){
	return (HEADLINE_GAP_YEAR == headline_cadence_gap(snapshot, invoice_id))
		? 12 : 1;
}

/* --- Customer activity ---------------------------------------------------- */

/*
 * One paying customer as seen from an anchor, derived from their positive
 * cash movements before it.
 *
 * - first, last: their first and latest positive receipt.
 * - gap: how far apart receipts are expected, in days. The known cadence of
 *   the latest receipt's invoice when there is one; else the median gap
 *   between their receipts on distinct days (the upper median for an even
 *   count); else 31, one month, for a customer who has paid once.
 * - threshold: how long they may be quiet before they count as churned:
 *   max(days, ceil(1.5 x gap)) when the gap is evidence (a known cadence
 *   or two receipts), else days. An annual customer is therefore not
 *   "quiet" nine months after paying for a year, and a customer who pays
 *   every sixty days is not churned on day ninety-one.
 * - covered_until: last + gap, the end of the time their latest receipt
 *   paid for.
 * - churned_at: last + threshold, when that is before the anchor; else
 *   they have not churned.
 */
typedef struct
{
	gint64		 company_id;
	GDateTime	*first;
	GDateTime	*last;
	GDateTime	*covered_until;
	GDateTime	*churned_at;
	gboolean	 paid_in_window;
} HeadlineActivity;

static void
headline_activity_free(gpointer data)
{
	HeadlineActivity *activity = data;

	g_clear_pointer(&activity->first, g_date_time_unref);
	g_clear_pointer(&activity->last, g_date_time_unref);
	g_clear_pointer(&activity->covered_until, g_date_time_unref);
	g_clear_pointer(&activity->churned_at, g_date_time_unref);
	g_free(activity);
}

static gint
headline_activity_compare(
	gconstpointer	a,
	gconstpointer	b
){
	const HeadlineActivity *left = *(HeadlineActivity *const *)a;
	const HeadlineActivity *right = *(HeadlineActivity *const *)b;

	return (left->company_id < right->company_id) ? -1
		: ((left->company_id > right->company_id) ? 1 : 0);
}

static gint
headline_gap_compare(
	gconstpointer	a,
	gconstpointer	b
){
	gint64 left = *(const gint64 *)a;
	gint64 right = *(const gint64 *)b;

	return (left < right) ? -1 : ((left > right) ? 1 : 0);
}

static GPtrArray *
headline_activities(
	VentureHeadlineSnapshot	 *snapshot,
	GDateTime		 *anchor,
	GError			**error
){
	g_autoptr(GPtrArray) activities = NULL;
	g_autoptr(GDateTime) window_start = NULL;
	GHashTableIter iter;
	gpointer key;
	gpointer value;
	gint64 days;

	if (!headline_load_cash(snapshot, error) ||
	    !headline_load_cadence(snapshot, error))
		return NULL;

	days = headline_days(snapshot);

	/* g_date_time_add_days takes a gint; a settings row can hold more. */
	if (days > G_MAXINT)
		days = G_MAXINT;

	activities = g_ptr_array_new_with_free_func(headline_activity_free);
	window_start = g_date_time_add_months(anchor, -HEADLINE_WINDOW_MONTHS);
	g_hash_table_iter_init(&iter, snapshot->cash);

	while (g_hash_table_iter_next(&iter, &key, &value))
	{
		g_autoptr(GArray) gaps = NULL;
		g_autoptr(GDateTime) churned_at = NULL;
		GPtrArray *history = value;
		HeadlineActivity *activity;
		HeadlineCash *latest = NULL;
		GDateTime *first = NULL;
		gint64 gap;
		gint64 threshold;
		gboolean evidence;
		gboolean in_window = FALSE;
		guint i;

		gaps = g_array_new(FALSE, FALSE, sizeof(gint64));

		for (i = 0; i < history->len; i++)
		{
			HeadlineCash *movement = g_ptr_array_index(history, i);

			if (g_date_time_compare(movement->date, anchor) >= 0)
				break;

			if (venture_money_get_amount(movement->amount) <= 0)
				continue;

			if (NULL == first)
				first = movement->date;

			if (NULL != latest)
			{
				gint64 apart;

				apart = g_date_time_difference(movement->date,
				                               latest->date) / G_TIME_SPAN_DAY;

				/* Two receipts on one day are one payment split
				 * across invoices, not a rhythm. */
				if (apart >= 1)
					g_array_append_val(gaps, apart);
			}

			if (g_date_time_compare(movement->date, window_start) >= 0)
				in_window = TRUE;

			latest = movement;
		}

		if (NULL == latest)
			continue;

		gap = headline_cadence_gap(snapshot, latest->invoice_id);
		evidence = TRUE;

		if (0 == gap)
		{
			if (gaps->len > 0)
			{
				g_array_sort(gaps, headline_gap_compare);
				gap = g_array_index(gaps, gint64, gaps->len / 2);
			}
			else
			{
				gap = HEADLINE_GAP_MONTH;
				evidence = FALSE;
			}
		}

		threshold = days;

		if (evidence && (((3 * gap) + 1) / 2 > threshold))
			threshold = ((3 * gap) + 1) / 2;

		if (threshold > G_MAXINT)
			threshold = G_MAXINT;

		activity = g_new0(HeadlineActivity, 1);
		activity->company_id = *(gint64 *)key;
		activity->first = g_date_time_ref(first);
		activity->last = g_date_time_ref(latest->date);
		activity->covered_until = g_date_time_add_days(latest->date, (gint)gap);
		activity->paid_in_window = in_window;
		churned_at = g_date_time_add_days(latest->date, (gint)threshold);

		if ((NULL != churned_at) && (g_date_time_compare(churned_at, anchor) < 0))
			activity->churned_at = g_steal_pointer(&churned_at);

		g_ptr_array_add(activities, activity);
	}

	/* Hash order is not an order: the rows a report lists must come out
	 * the same way on every run. */
	g_ptr_array_sort(activities, headline_activity_compare);

	return g_steal_pointer(&activities);
}

/*
 * Monthly activity churn over the twelve months before the anchor.
 *
 * - events: customers whose churned_at falls inside the window.
 * - customer_months: customer-months at risk. The window is cut into
 *   twelve months counted back from the anchor; a customer is at risk in a
 *   month when, at its first instant, they had already paid (first is
 *   before it) and were still paid up (covered_until is after it). A
 *   customer who joined during a month is at risk from the next one.
 * - rate: events / customer_months, the probability a paying customer is
 *   lost in a month.
 *
 * Worked through: a hundred monthly customers, two leaving and two joining
 * every month, give 24 events over 1,200 customer-months: 2.00%. Dividing
 * the customers who went quiet by everyone who paid in the year and then
 * by twelve read the same business as 1.23%, and every lifetime projected
 * from it as 63% too long.
 */
typedef struct
{
	gint64		 events;
	gint64		 customer_months;
	gint64		 active_customers;
} HeadlineChurnFigures;

static void
headline_activity_churn(
	GPtrArray		*activities,
	GDateTime		*anchor,
	HeadlineChurnFigures	*figures
){
	g_autoptr(GDateTime) window_start = NULL;
	gint month;
	guint i;

	figures->events = 0;
	figures->customer_months = 0;
	figures->active_customers = 0;
	window_start = g_date_time_add_months(anchor, -HEADLINE_WINDOW_MONTHS);

	for (i = 0; i < activities->len; i++)
	{
		HeadlineActivity *activity = g_ptr_array_index(activities, i);

		if (activity->paid_in_window)
			figures->active_customers++;

		if ((NULL != activity->churned_at) &&
		    (g_date_time_compare(activity->churned_at, window_start) >= 0))
			figures->events++;
	}

	for (month = 1; month <= HEADLINE_WINDOW_MONTHS; month++)
	{
		g_autoptr(GDateTime) opens = NULL;

		opens = g_date_time_add_months(anchor, -month);

		for (i = 0; i < activities->len; i++)
		{
			HeadlineActivity *activity = g_ptr_array_index(activities, i);

			if ((g_date_time_compare(activity->first, opens) < 0) &&
			    (g_date_time_compare(activity->covered_until, opens) > 0))
				figures->customer_months++;
		}
	}
}

/* --- Revenue per customer-month ------------------------------------------ */

/*
 * Cash revenue over the twelve months before the anchor, spread over the
 * months each receipt pays for, and the customer-months it was earned in.
 *
 * The window is the same twelve months counted back from the anchor that
 * churn uses. A receipt whose invoice has a yearly cadence is split evenly
 * (VentureMoney allocation, so the parts sum to it) across the twelve months
 * starting with the one it was received in; every other receipt belongs to
 * the month it was received in. A refund is spread like the receipt it
 * refunds. Only the parts that land inside the window count, so an annual
 * receipt from eight months before the window still contributes its last
 * four months.
 *
 * A customer-month is one customer in one of those months with a positive
 * part of a receipt that made it into the total -- an amount skipped as
 * foreign currency adds no customer-month either, or the average would be
 * divided by months whose money was never summed.
 *
 * Worked through: twelve annual customers at $1,200, one renewing each
 * month, is $14,400 over 144 customer-months: $100. Counting each receipt
 * only in its own month made it $1,200.
 */
static gboolean
headline_spread_revenue(
	VentureHeadlineSnapshot	 *snapshot,
	GDateTime		 *anchor,
	VentureMoney		**out_revenue,
	gint64			 *out_customer_months,
	guint			 *inout_skipped,
	GError			**error
){
	g_autoptr(VentureMoney) revenue = NULL;
	g_autoptr(GHashTable) months = NULL;
	GDateTime *bounds[(2 * HEADLINE_WINDOW_MONTHS) + 1];
	GHashTableIter iter;
	gpointer key;
	gpointer value;
	guint b;

	if (!headline_load_cash(snapshot, error) ||
	    !headline_load_cadence(snapshot, error))
		return FALSE;

	/* bounds[k] is k months before the anchor; month k of the window is
	 * [bounds[k + 1], bounds[k]). Twice the window is kept so an annual
	 * receipt received before the window can still be placed. */
	for (b = 0; b < G_N_ELEMENTS(bounds); b++)
		bounds[b] = g_date_time_add_months(anchor, -(gint)b);

	revenue = venture_money_new_zero(snapshot->currency);
	months = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	g_hash_table_iter_init(&iter, snapshot->cash);

	while (g_hash_table_iter_next(&iter, &key, &value))
	{
		GPtrArray *history = value;
		guint i;

		for (i = 0; i < history->len; i++)
		{
			g_autoptr(GPtrArray) parts = NULL;
			HeadlineCash *movement = g_ptr_array_index(history, i);
			gint bucket = -1;
			gint spread;
			gint j;

			if (g_date_time_compare(movement->date, anchor) >= 0)
				break;

			for (b = 0; b + 1 < G_N_ELEMENTS(bounds); b++)
			{
				if (g_date_time_compare(movement->date, bounds[b + 1]) >= 0)
				{
					bucket = (gint)b;
					break;
				}
			}

			if (bucket < 0)
				continue;

			spread = headline_cadence_months(snapshot, movement->invoice_id);
			parts = venture_money_allocate_evenly(movement->amount,
			                                      (gsize)spread, NULL);

			if (NULL == parts)
			{
				(*inout_skipped)++;
				continue;
			}

			/* Part j pays for the month j after the receipt's, which
			 * is j buckets nearer the anchor. */
			for (j = 0; j < spread; j++)
			{
				const VentureMoney *part = g_ptr_array_index(parts, j);
				gint target = bucket - j;

				if ((target < 0) || (target >= HEADLINE_WINDOW_MONTHS))
					continue;

				if (!headline_accumulate(&revenue, part, inout_skipped))
					continue;

				if (venture_money_get_amount(part) > 0)
					g_hash_table_add(months,
						g_strdup_printf("%" G_GINT64_FORMAT ":%d",
						                *(gint64 *)key, target));
			}
		}
	}

	for (b = 0; b < G_N_ELEMENTS(bounds); b++)
		g_clear_pointer(&bounds[b], g_date_time_unref);

	*out_revenue = g_steal_pointer(&revenue);
	*out_customer_months = (gint64)g_hash_table_size(months);

	return TRUE;
}

/* --- Acquisition and cost-of-revenue spend -------------------------------- */

/* A cash projection of a paid bill line (external id bill_line:...) or of a
 * later bill payment or refund (bill_cash:...). The bill line itself is
 * already counted at its bill date; counting its projection too would count
 * the same money twice, once when billed and again when paid. */
static gboolean
headline_bill_projection(const gchar *external_id)
{
	return (NULL != external_id) &&
	       (g_str_has_prefix(external_id, "bill_line:") ||
	        g_str_has_prefix(external_id, "bill_cash:"));
}

static void
headline_spend_add_expenses(
	VentureHeadlineSnapshot	*snapshot,
	GPtrArray		*expenses,
	GHashTable		*seen
){
	guint i;

	for (i = 0; i < expenses->len; i++)
	{
		g_autofree gchar *external = NULL;
		VentureEntity *expense = g_ptr_array_index(expenses, i);
		HeadlineSpend *spend;
		gint64 id;

		id = venture_entity_get_id(expense);

		if (g_hash_table_contains(seen, &id))
			continue;

		headline_id_add(seen, id);
		g_object_get(expense, "external-id", &external, NULL);

		if (headline_bill_projection(external))
			continue;

		if ((0 != snapshot->venture_id) &&
		    (headline_int(expense, "venture-id") != snapshot->venture_id))
			continue;

		spend = g_new0(HeadlineSpend, 1);
		g_object_get(expense, "occurred-at", &spend->date, "amount",
		             &spend->amount, "acquisition", &spend->acquisition,
		             "cost-of-revenue", &spend->cost_of_revenue, NULL);
		spend->campaign_id = headline_int(expense, "campaign-id");

		if ((NULL == spend->date) || (NULL == spend->amount))
		{
			headline_spend_free(spend);
			continue;
		}

		g_ptr_array_add(snapshot->spend, spend);
	}
}

/*
 * What one bill line cost: its net plus any tax that cannot be recovered.
 * A recoverable tax (the tax code's recoverable flag, e.g. input VAT) comes
 * back from the tax authority and was never a cost; a non-recoverable one,
 * or tax with no code, is part of what the line cost. This is the split the
 * payables service posts to the ledger.
 */
static VentureMoney *
headline_bill_line_cost(
	VentureEntity	*line,
	GHashTable	*recoverable
){
	g_autoptr(VentureMoney) amount = NULL;
	g_autoptr(VentureMoney) tax = NULL;
	gint64 tax_code_id;

	amount = venture_vendor_bill_line_get_amount(VENTURE_VENDOR_BILL_LINE(line),
	                                             NULL);

	if (NULL == amount)
		return NULL;

	g_object_get(line, "tax-amount", &tax, NULL);
	tax_code_id = headline_int(line, "tax-code-id");

	if ((NULL != tax) && !venture_money_is_zero(tax) && (0 != tax_code_id) &&
	    g_hash_table_contains(recoverable, &tax_code_id))
		return venture_money_subtract(amount, tax, NULL);

	return g_steal_pointer(&amount);
}

static gboolean
headline_spend_add_bill_lines(
	VentureHeadlineSnapshot	 *snapshot,
	GError			**error
){
	g_autoptr(GPtrArray) lines = NULL;
	g_autoptr(GHashTable) seen = NULL;
	g_autoptr(GHashTable) bill_ids = NULL;
	g_autoptr(GHashTable) code_ids = NULL;
	g_autoptr(GHashTable) bills_by_id = NULL;
	g_autoptr(GHashTable) recoverable = NULL;
	g_autoptr(GPtrArray) bills = NULL;
	g_autoptr(GPtrArray) codes = NULL;
	static const gchar *const flags[] = { "acquisition", "cost-of-revenue" };
	guint i;

	lines = g_ptr_array_new_with_free_func(g_object_unref);
	seen = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);

	/* Three narrow reads rather than every bill line ever written: the
	 * flagged ones and the ones that name a campaign. */
	for (i = 0; i <= G_N_ELEMENTS(flags); i++)
	{
		g_autoptr(GPtrArray) batch = NULL;
		guint j;

		batch = (i < G_N_ELEMENTS(flags))
			? headline_fetch_equal(snapshot, VENTURE_TYPE_VENDOR_BILL_LINE,
			                       flags[i], "true", FALSE, TRUE, error)
			: headline_fetch_positive(snapshot,
			                          VENTURE_TYPE_VENDOR_BILL_LINE,
			                          "campaign-id", FALSE, TRUE, error);

		if (NULL == batch)
			return FALSE;

		for (j = 0; j < batch->len; j++)
		{
			VentureEntity *line = g_ptr_array_index(batch, j);
			gint64 id = venture_entity_get_id(line);

			if (g_hash_table_contains(seen, &id))
				continue;

			headline_id_add(seen, id);
			g_ptr_array_add(lines, g_object_ref(line));
		}
	}

	bill_ids = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	code_ids = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);

	for (i = 0; i < lines->len; i++)
	{
		headline_id_add(bill_ids,
			headline_int(g_ptr_array_index(lines, i), "bill-id"));
		headline_id_add(code_ids,
			headline_int(g_ptr_array_index(lines, i), "tax-code-id"));
	}

	bills = headline_fetch_ids(snapshot, VENTURE_TYPE_VENDOR_BILL, bill_ids,
	                           FALSE, TRUE, error);

	if (NULL == bills)
		return FALSE;

	codes = headline_fetch_ids(snapshot, VENTURE_TYPE_TAX_CODE, code_ids,
	                           TRUE, FALSE, error);

	if (NULL == codes)
		return FALSE;

	bills_by_id = headline_id_table(NULL);

	for (i = 0; i < bills->len; i++)
		g_hash_table_replace(bills_by_id,
			headline_key(venture_entity_get_id(g_ptr_array_index(bills, i))),
			g_ptr_array_index(bills, i));

	recoverable = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free,
	                                    NULL);

	for (i = 0; i < codes->len; i++)
	{
		gboolean flag = FALSE;

		g_object_get(g_ptr_array_index(codes, i), "recoverable", &flag, NULL);

		if (flag)
			headline_id_add(recoverable,
				venture_entity_get_id(g_ptr_array_index(codes, i)));
	}

	for (i = 0; i < lines->len; i++)
	{
		g_autofree gchar *status = NULL;
		VentureEntity *line = g_ptr_array_index(lines, i);
		VentureEntity *bill;
		HeadlineSpend *spend;
		gint64 bill_id;

		bill_id = headline_int(line, "bill-id");
		bill = g_hash_table_lookup(bills_by_id, &bill_id);

		if (NULL == bill)
			continue;

		/* A draft is not yet owed and a void bill never was. */
		g_object_get(bill, "status", &status, NULL);

		if ((0 == g_strcmp0(status, "draft")) ||
		    (0 == g_strcmp0(status, "void")))
			continue;

		if ((0 != snapshot->venture_id) &&
		    (headline_int(bill, "venture-id") != snapshot->venture_id))
			continue;

		spend = g_new0(HeadlineSpend, 1);
		g_object_get(bill, "bill-date", &spend->date, NULL);
		g_object_get(line, "acquisition", &spend->acquisition,
		             "cost-of-revenue", &spend->cost_of_revenue, NULL);
		spend->campaign_id = headline_int(line, "campaign-id");
		spend->bill_line = TRUE;
		spend->amount = headline_bill_line_cost(line, recoverable);

		if (NULL == spend->date)
		{
			headline_spend_free(spend);
			continue;
		}

		if (NULL == spend->amount)
		{
			/* An invalid quantity or price is an amount the report
			 * could not include, and it says so. */
			snapshot->spend_skipped++;
			headline_spend_free(spend);
			continue;
		}

		g_ptr_array_add(snapshot->spend, spend);
	}

	return TRUE;
}

/*
 * Flagged spend: every living expense or vendor bill line flagged
 * acquisition or cost of revenue, or naming a campaign; campaigns, with the
 * ids of the ones some row names; converted leads by company.
 */
static gboolean
headline_load_spend(
	VentureHeadlineSnapshot	 *snapshot,
	GError			**error
){
	g_autoptr(GHashTable) seen = NULL;
	static const gchar *const flags[] = { "acquisition", "cost-of-revenue" };
	guint i;

	if (NULL != snapshot->spend)
		return TRUE;

	snapshot->spend = g_ptr_array_new_with_free_func(headline_spend_free);
	snapshot->linked_campaigns = g_hash_table_new_full(g_int64_hash,
	                                                   g_int64_equal, g_free,
	                                                   NULL);
	snapshot->campaigns = headline_id_table(headline_campaign_free);
	snapshot->leads = headline_id_table(headline_lead_free);
	seen = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);

	for (i = 0; i <= G_N_ELEMENTS(flags); i++)
	{
		g_autoptr(GPtrArray) expenses = NULL;

		expenses = (i < G_N_ELEMENTS(flags))
			? headline_fetch_equal(snapshot, VENTURE_TYPE_EXPENSE, flags[i],
			                       "true", FALSE, TRUE, error)
			: headline_fetch_positive(snapshot, VENTURE_TYPE_EXPENSE,
			                          "campaign-id", FALSE, TRUE, error);

		if (NULL == expenses)
			return FALSE;

		headline_spend_add_expenses(snapshot, expenses, seen);
	}

	if (venture_context_module_enabled(snapshot->context, "payables") &&
	    !headline_spend_add_bill_lines(snapshot, error))
		return FALSE;

	for (i = 0; i < snapshot->spend->len; i++)
	{
		HeadlineSpend *spend = g_ptr_array_index(snapshot->spend, i);

		headline_id_add(snapshot->linked_campaigns, spend->campaign_id);
	}

	if (venture_context_module_enabled(snapshot->context, "outreach"))
	{
		g_autoptr(GPtrArray) campaigns = NULL;

		campaigns = headline_fetch_all(snapshot, VENTURE_TYPE_CAMPAIGN, FALSE,
		                               TRUE, error);

		if (NULL == campaigns)
			return FALSE;

		for (i = 0; i < campaigns->len; i++)
		{
			VentureEntity *record = g_ptr_array_index(campaigns, i);
			HeadlineCampaign *campaign;

			if ((0 != snapshot->venture_id) &&
			    (headline_int(record, "venture-id") != snapshot->venture_id))
				continue;

			campaign = g_new0(HeadlineCampaign, 1);
			campaign->id = venture_entity_get_id(record);
			g_object_get(record, "name", &campaign->name, "spend",
			             &campaign->spend, "started-at", &campaign->started,
			             "ended-at", &campaign->ended, NULL);
			g_hash_table_replace(snapshot->campaigns,
			                     headline_key(campaign->id), campaign);
		}
	}

	{
		g_autoptr(GPtrArray) leads = NULL;

		leads = headline_fetch_positive(snapshot, VENTURE_TYPE_LEAD,
		                                "converted-company-id", FALSE, TRUE,
		                                error);

		if (NULL == leads)
			return FALSE;

		for (i = 0; i < leads->len; i++)
		{
			VentureEntity *record = g_ptr_array_index(leads, i);
			VentureLeadStatus status;
			HeadlineLead *lead;
			gint64 company_id;

			g_object_get(record, "status", &status, NULL);
			company_id = headline_int(record, "converted-company-id");

			/* The first converted lead in id order is the one the
			 * company came from. */
			if ((VENTURE_LEAD_CONVERTED != status) ||
			    g_hash_table_contains(snapshot->leads, &company_id))
				continue;

			if ((0 != snapshot->venture_id) &&
			    (headline_int(record, "venture-id") != snapshot->venture_id))
				continue;

			lead = g_new0(HeadlineLead, 1);
			g_object_get(record, "source", &lead->source, NULL);
			lead->campaign_id = headline_int(record, "campaign-id");
			g_hash_table_replace(snapshot->leads, headline_key(company_id),
			                     lead);
		}
	}

	return TRUE;
}

/*
 * The part of a campaign's own spend figure that belongs to @period.
 *
 * A campaign ran over [started_at, ended_at), or [started_at, now) while it
 * has no end, and its spend is spread evenly across that time: the share for
 * a period is spend x overlap / span, split with VentureMoney allocation so
 * the shares of consecutive periods sum back to the spend. A campaign that
 * ended where it started, or has not started yet, is a single moment and
 * belongs whole to the period containing its start. A campaign still running
 * is re-spread every day, so its earlier months' shares shrink as it goes
 * on -- the honest consequence of not knowing yet how long it will run.
 * Returns %NULL for a campaign without spend or a start.
 */
static VentureMoney *
headline_campaign_share(
	const HeadlineCampaign	*campaign,
	const VentureDateRange	*period,
	GDateTime		*now
){
	g_autoptr(GDateTime) lower = NULL;
	g_autoptr(GDateTime) upper = NULL;
	g_autoptr(GPtrArray) shares = NULL;
	GDateTime *ends;
	GDateTime *start;
	GDateTime *end;
	gint64 ratios[2];
	gint64 total;
	gint64 overlap;

	if ((NULL == campaign->spend) || (NULL == campaign->started))
		return NULL;

	ends = (NULL != campaign->ended) ? campaign->ended : now;
	start = (NULL != period) ? venture_date_range_get_start(period) : NULL;
	end = (NULL != period) ? venture_date_range_get_end(period) : NULL;
	total = g_date_time_difference(ends, campaign->started) / G_TIME_SPAN_SECOND;

	if (total <= 0)
		return headline_within(campaign->started, period)
			? venture_money_copy(campaign->spend)
			: venture_money_new_zero(venture_money_get_currency(campaign->spend));

	lower = ((NULL != start) && (g_date_time_compare(start, campaign->started) > 0))
		? g_date_time_ref(start) : g_date_time_ref(campaign->started);
	upper = ((NULL != end) && (g_date_time_compare(end, ends) < 0))
		? g_date_time_ref(end) : g_date_time_ref(ends);
	overlap = g_date_time_difference(upper, lower) / G_TIME_SPAN_SECOND;

	if (overlap <= 0)
		return venture_money_new_zero(venture_money_get_currency(campaign->spend));

	if (overlap >= total)
		return venture_money_copy(campaign->spend);

	ratios[0] = overlap;
	ratios[1] = total - overlap;
	shares = venture_money_allocate(campaign->spend, ratios, 2, NULL);

	if (NULL == shares)
		return NULL;

	return venture_money_copy(g_ptr_array_index(shares, 0));
}

/* ==========================================================================
 * CAC
 * ========================================================================== */

/* One row of the breakdown: the new customers from one campaign and source,
 * and that row's share of the campaign's spend. */
typedef struct
{
	gint64		 campaign_id;
	gchar		*source;
	gint64		 customers;
	VentureMoney	*spend;
} CacRow;

static void
cac_row_free(gpointer data)
{
	CacRow *row = data;

	g_free(row->source);
	g_clear_pointer(&row->spend, venture_money_free);
	g_free(row);
}

/* Campaigns in id order, then the customers no campaign brought; sources
 * alphabetically within each, so the table reads the same on every run. */
static gint
cac_row_compare(
	gconstpointer	a,
	gconstpointer	b
){
	const CacRow *left = *(CacRow *const *)a;
	const CacRow *right = *(CacRow *const *)b;

	if (left->campaign_id != right->campaign_id)
	{
		if (0 == left->campaign_id)
			return 1;

		if (0 == right->campaign_id)
			return -1;

		return (left->campaign_id < right->campaign_id) ? -1 : 1;
	}

	return g_strcmp0(left->source, right->source);
}

/* The earliest positive cash movement: the moment a company became a paying
 * customer. NULL for a company that has only ever had money handed back. */
static GDateTime *
headline_first_cash(GPtrArray *history)
{
	guint i;

	for (i = 0; i < history->len; i++)
	{
		HeadlineCash *movement = g_ptr_array_index(history, i);

		if (venture_money_get_amount(movement->amount) > 0)
			return movement->date;
	}

	return NULL;
}

/*
 * Customer acquisition cost for @period.
 *
 * Acquisition spend is, for rows dated in the period:
 * - every expense or vendor bill line flagged acquisition or naming a
 *   campaign (a row paid for a campaign is acquisition spend whether or not
 *   its own flag is on). Bill lines count at their bill's date, net plus
 *   non-recoverable tax, drafts and voids excluded; the cash expenses the
 *   payables service projects from a paid bill line are not counted again.
 * - each campaign's own spend figure, pro-rated over the time it ran, but
 *   only for a campaign that no expense or bill line names: once its costs
 *   are recorded as rows, the figure is a summary of the same money.
 *
 * A new customer is a company whose earliest cash receipt (see
 * headline_load_cash) falls in the period: the moment money arrived, not
 * when an invoice was marked paid, which a write-off also does and a refund
 * undoes. A company paying in instalments is new at its first instalment.
 *
 * CAC = spend / new customers, one rational multiplication rounded half to
 * even.
 */
VentureReportResult *
venture_headline_snapshot_cac(
	VentureHeadlineSnapshot	 *snapshot,
	VentureDateRange	 *period,
	GError			**error
){
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(GHashTable) campaign_spend = NULL;
	g_autoptr(GHashTable) rows_by_key = NULL;
	g_autoptr(GHashTable) unattributed = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(VentureMoney) from_campaigns = NULL;
	g_autoptr(VentureMoney) from_expenses = NULL;
	g_autoptr(VentureMoney) spend = NULL;
	g_autoptr(GDateTime) now = NULL;
	GHashTableIter iter;
	gpointer key;
	gpointer value;
	gint64 new_customers;
	guint skipped;
	guint i;

	g_return_val_if_fail(NULL != snapshot, NULL);

	if (!headline_load_cash(snapshot, error) ||
	    !headline_load_spend(snapshot, error))
		return NULL;

	now = headline_now(snapshot);
	skipped = snapshot->spend_skipped;
	from_campaigns = venture_money_new_zero(snapshot->currency);
	from_expenses = venture_money_new_zero(snapshot->currency);
	campaign_spend = headline_id_table((GDestroyNotify)venture_money_free);

	/* A campaign's own spend figure, for the campaigns no row names. */
	g_hash_table_iter_init(&iter, snapshot->campaigns);

	while (g_hash_table_iter_next(&iter, &key, &value))
	{
		g_autoptr(VentureMoney) share = NULL;
		HeadlineCampaign *campaign = value;

		if (g_hash_table_contains(snapshot->linked_campaigns, key))
			continue;

		share = headline_campaign_share(campaign, period, now);

		if (NULL == share)
			continue;

		if (headline_accumulate(&from_campaigns, share, &skipped))
			g_hash_table_replace(campaign_spend, headline_key(campaign->id),
			                     g_steal_pointer(&share));
	}

	/* The rows: a campaign's recorded costs, and other flagged spend. */
	for (i = 0; i < snapshot->spend->len; i++)
	{
		HeadlineSpend *row = g_ptr_array_index(snapshot->spend, i);
		HeadlineCampaign *campaign;

		if ((!row->acquisition && (0 == row->campaign_id)) ||
		    !headline_within(row->date, period))
			continue;

		campaign = (0 != row->campaign_id)
			? g_hash_table_lookup(snapshot->campaigns, &row->campaign_id)
			: NULL;

		if (NULL == campaign)
		{
			headline_accumulate(&from_expenses, row->amount, &skipped);
			continue;
		}

		if (headline_accumulate(&from_campaigns, row->amount, &skipped))
		{
			VentureMoney *known;

			known = g_hash_table_lookup(campaign_spend, &row->campaign_id);

			if (NULL == known)
				g_hash_table_replace(campaign_spend,
					headline_key(row->campaign_id),
					venture_money_copy(row->amount));
			else
			{
				VentureMoney *sum = venture_money_add(known, row->amount, NULL);

				if (NULL != sum)
					g_hash_table_replace(campaign_spend,
						headline_key(row->campaign_id), sum);
			}
		}
	}

	spend = venture_money_add(from_campaigns, from_expenses, NULL);

	if (NULL == spend)
	{
		skipped++;
		spend = venture_money_copy(from_expenses);
	}

	/* New customers, and which lead each came from. */
	rows = g_ptr_array_new_with_free_func(cac_row_free);
	rows_by_key = g_hash_table_new(g_str_hash, g_str_equal);
	unattributed = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free,
	                                     NULL);
	new_customers = 0;
	g_hash_table_iter_init(&iter, snapshot->cash);

	while (g_hash_table_iter_next(&iter, &key, &value))
	{
		if (!headline_within(headline_first_cash(value), period))
			continue;

		new_customers++;

		if (!g_hash_table_contains(snapshot->leads, key))
			headline_id_add(unattributed, *(gint64 *)key);
	}

	/* A company with no lead behind it is attributed to its own source,
	 * read for all of them at once. */
	if (!headline_load_companies(snapshot, unattributed, error))
		return NULL;

	{
		g_autoptr(GPtrArray) keys = g_ptr_array_new_with_free_func(g_free);

		g_hash_table_iter_init(&iter, snapshot->cash);

		while (g_hash_table_iter_next(&iter, &key, &value))
		{
			g_autofree gchar *row_key = NULL;
			HeadlineLead *lead;
			HeadlineCompany *company;
			const gchar *source = NULL;
			gint64 campaign_id = 0;
			CacRow *row;

			if (!headline_within(headline_first_cash(value), period))
				continue;

			lead = g_hash_table_lookup(snapshot->leads, key);
			company = headline_company(snapshot, *(gint64 *)key);

			if (NULL != lead)
			{
				source = lead->source;
				campaign_id = lead->campaign_id;
			}
			else if (NULL != company)
			{
				source = company->source;
			}

			if (venture_string_is_empty(source))
				source = "(unattributed)";

			row_key = g_strdup_printf("%" G_GINT64_FORMAT ":%s", campaign_id,
			                          source);
			row = g_hash_table_lookup(rows_by_key, row_key);

			if (NULL == row)
			{
				row = g_new0(CacRow, 1);
				row->campaign_id = campaign_id;
				row->source = g_strdup(source);
				g_ptr_array_add(rows, row);
				g_hash_table_insert(rows_by_key, row_key, row);
				g_ptr_array_add(keys, g_steal_pointer(&row_key));
			}

			row->customers++;
		}

		g_ptr_array_sort(rows, cac_row_compare);
	}

	/*
	 * A campaign's spend in the period is split across its source rows in
	 * proportion to the customers each brought, by VentureMoney allocation,
	 * so the rows sum to the campaign's spend. Giving every row the whole
	 * campaign spend counted it once per source.
	 */
	for (i = 0; i < rows->len; )
	{
		CacRow *first = g_ptr_array_index(rows, i);
		const VentureMoney *total;
		guint end;
		guint j;

		for (end = i; end < rows->len; end++)
			if (((CacRow *)g_ptr_array_index(rows, end))->campaign_id !=
			    first->campaign_id)
				break;

		total = (0 != first->campaign_id)
			? g_hash_table_lookup(campaign_spend, &first->campaign_id) : NULL;

		if (NULL != total)
		{
			g_autofree gint64 *ratios = NULL;
			g_autoptr(GPtrArray) shares = NULL;

			ratios = g_new0(gint64, end - i);

			for (j = i; j < end; j++)
				ratios[j - i] = ((CacRow *)g_ptr_array_index(rows, j))->customers;

			shares = venture_money_allocate(total, ratios, end - i, NULL);

			for (j = i; (NULL != shares) && (j < end); j++)
				((CacRow *)g_ptr_array_index(rows, j))->spend =
					venture_money_copy(g_ptr_array_index(shares, j - i));
		}

		i = end;
	}

	result = venture_report_result_new("Customer acquisition cost", period);

	venture_report_result_add_metric(result,
		venture_metric_new_money("spend", "Acquisition spend", spend));
	venture_report_result_add_metric(result,
		venture_metric_new_money("campaign_spend", "Campaign spend",
		                         from_campaigns));
	venture_report_result_add_metric(result,
		venture_metric_new_money("expense_spend", "Other flagged spend",
		                         from_expenses));
	venture_report_result_add_metric(result,
		venture_metric_new_count("new_customers", "New customers",
		                         new_customers));

	if (new_customers > 0)
	{
		g_autoptr(VentureMoney) cac = NULL;
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
			"No company received its first cash receipt in this period, so "
			"there is nothing to divide the spend by. CAC is n/a, not "
			"zero.");
	}

	venture_report_result_add_column(result, "source", "Source",
	                                 VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "campaign", "Campaign",
	                                 VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "new_customers", "New customers",
	                                 VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "spend", "Campaign spend share",
	                                 VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "cac", "CAC",
	                                 VENTURE_REPORT_COLUMN_MONEY);

	for (i = 0; i < rows->len; i++)
	{
		g_autofree gchar *campaign_name = NULL;
		CacRow *row = g_ptr_array_index(rows, i);
		HeadlineCampaign *campaign;

		campaign = (0 != row->campaign_id)
			? g_hash_table_lookup(snapshot->campaigns, &row->campaign_id)
			: NULL;

		if ((NULL != campaign) && !venture_string_is_empty(campaign->name))
			campaign_name = g_strdup(campaign->name);
		else if (0 != row->campaign_id)
			campaign_name = g_strdup_printf("Campaign #%" G_GINT64_FORMAT,
			                                row->campaign_id);

		venture_report_result_begin_row(result);
		venture_report_result_set_text(result, "source", row->source);
		venture_report_result_set_text(result, "campaign",
			(NULL != campaign_name) ? campaign_name : "");
		venture_report_result_set_number(result, "new_customers",
		                                 (gdouble)row->customers);

		if (NULL != row->spend)
		{
			g_autoptr(VentureMoney) row_cac = NULL;

			row_cac = venture_money_multiply_rational(row->spend, 1,
			                                          row->customers, NULL);
			venture_report_result_set_money(result, "spend", row->spend);
			venture_report_result_set_money(result, "cac", row_cac);
		}
	}

	venture_report_result_append_note(result,
		"New customers are companies whose first cash receipt fell in the "
		"period. Spend is acquisition-flagged expenses and bill lines dated "
		"in the period (bill lines net of recoverable tax), plus each "
		"campaign's spend pro-rated over the time it ran, for campaigns no "
		"expense or bill line names.");

	if (!venture_context_module_enabled(snapshot->context, "outreach"))
		venture_report_result_append_note(result,
			"The outreach module is off, so no campaign spend is counted; "
			"only flagged expense and bill lines are.");

	headline_flag_skipped(result, skipped, snapshot->currency);

	return g_steal_pointer(&result);
}

static VentureReportResult *
venture_report_cac(
	VentureContext		 *context,
	VentureDateRange	 *period,
	JsonObject		 *options,
	GError			**error
){
	g_autoptr(VentureHeadlineSnapshot) snapshot = NULL;

	snapshot = venture_headline_snapshot_new(context, options, error);

	if (NULL == snapshot)
		return NULL;

	return venture_headline_snapshot_cac(snapshot, period, error);
}

/* ==========================================================================
 * Churn
 * ========================================================================== */

/*
 * Every recurring commitment, from invoice schedules (the recurring module)
 * and from billing subscriptions (the billing module), with the customer it
 * belongs to. Deleted schedules are read too: a deletion is how a schedule
 * is cancelled, and when.
 */
static gboolean
headline_load_recurring(
	VentureHeadlineSnapshot	 *snapshot,
	GError			**error
){
	guint i;

	if (NULL != snapshot->recurring)
		return TRUE;

	snapshot->recurring = g_ptr_array_new_with_free_func(headline_recurring_free);

	if (venture_context_module_enabled(snapshot->context, "recurring"))
	{
		GPtrArray *schedules;

		schedules = headline_cached_rows(snapshot, &snapshot->schedule_rows,
			VENTURE_TYPE_RECURRING_SCHEDULE, error);

		if (NULL == schedules)
			return FALSE;

		for (i = 0; i < schedules->len; i++)
		{
			g_autofree gchar *template_text = NULL;
			g_autoptr(JsonNode) template_node = NULL;
			VentureEntity *schedule = g_ptr_array_index(schedules, i);
			HeadlineRecurring *recurring;
			GDateTime *deleted;
			JsonObject *template_object = NULL;
			gint kind = 0;

			/* Enum value 0 is the invoice kind: the only schedule that
			 * is a customer's commitment rather than a cost. */
			g_object_get(schedule, "kind", &kind, "template", &template_text,
			             NULL);

			if (0 != kind)
				continue;

			if (!venture_string_is_empty(template_text))
				template_node = venture_json_parse(template_text, NULL);

			if ((NULL != template_node) && JSON_NODE_HOLDS_OBJECT(template_node))
				template_object = json_node_get_object(template_node);

			if ((NULL == template_object) ||
			    (0 == venture_json_object_get_int(template_object,
			                                      "company_id", 0)))
				continue;

			if ((0 != snapshot->venture_id) &&
			    (venture_json_object_get_int(template_object, "venture_id", 0)
			     != snapshot->venture_id))
				continue;

			recurring = g_new0(HeadlineRecurring, 1);
			recurring->company_id = venture_json_object_get_int(template_object,
			                                                    "company_id", 0);
			g_object_get(schedule, "start-at", &recurring->started, "end-at",
			             &recurring->ended, "paused", &recurring->paused, NULL);
			deleted = venture_entity_get_deleted_at(schedule);

			/* A deletion after the as_of cutoff had not happened yet. */
			if ((NULL != deleted) &&
			    ((NULL == snapshot->cutoff) ||
			     (g_date_time_compare(deleted, snapshot->cutoff) < 0)))
				recurring->deleted = g_date_time_ref(deleted);

			g_ptr_array_add(snapshot->recurring, recurring);
		}
	}

	if (venture_context_module_enabled(snapshot->context, "billing"))
	{
		g_autoptr(GPtrArray) subscriptions = NULL;
		GPtrArray *events;
		g_autoptr(GHashTable) by_subscription = NULL;
		g_autoptr(GHashTable) price_venture = NULL;

		subscriptions = headline_fetch_all(snapshot,
			VENTURE_TYPE_CUSTOMER_SUBSCRIPTION, TRUE, FALSE, error);

		if (NULL == subscriptions)
			return FALSE;

		events = headline_cached_rows(snapshot, &snapshot->event_rows,
			VENTURE_TYPE_SUBSCRIPTION_EVENT, error);

		if (NULL == events)
			return FALSE;

		/* A subscription belongs to the venture of its price's plan. */
		if (0 != snapshot->venture_id)
		{
			GPtrArray *prices;
			g_autoptr(GPtrArray) plans = NULL;
			g_autoptr(GHashTable) plan_venture = NULL;

			prices = headline_cached_rows(snapshot, &snapshot->price_rows,
				VENTURE_TYPE_PLAN_PRICE, error);
			plans = (NULL != prices)
				? headline_fetch_all(snapshot, VENTURE_TYPE_PLAN, TRUE, FALSE,
				                     error)
				: NULL;

			if (NULL == plans)
				return FALSE;

			plan_venture = headline_id_table(g_free);
			price_venture = headline_id_table(g_free);

			for (i = 0; i < plans->len; i++)
				g_hash_table_replace(plan_venture,
					headline_key(venture_entity_get_id(g_ptr_array_index(plans, i))),
					headline_key(headline_int(g_ptr_array_index(plans, i),
					                          "venture-id")));

			for (i = 0; i < prices->len; i++)
			{
				gint64 plan_id;
				gint64 *venture;

				plan_id = headline_int(g_ptr_array_index(prices, i), "plan-id");
				venture = g_hash_table_lookup(plan_venture, &plan_id);
				g_hash_table_replace(price_venture,
					headline_key(venture_entity_get_id(g_ptr_array_index(prices, i))),
					headline_key((NULL != venture) ? *venture : 0));
			}
		}

		by_subscription = headline_id_table(NULL);

		for (i = 0; i < subscriptions->len; i++)
		{
			VentureEntity *subscription = g_ptr_array_index(subscriptions, i);
			HeadlineRecurring *recurring;

			if (NULL != price_venture)
			{
				gint64 price_id;
				gint64 *venture;

				price_id = headline_int(subscription, "plan-price-id");
				venture = g_hash_table_lookup(price_venture, &price_id);

				if ((NULL == venture) || (*venture != snapshot->venture_id))
					continue;
			}

			recurring = g_new0(HeadlineRecurring, 1);
			recurring->company_id = headline_int(subscription, "company-id");
			recurring->subscription = TRUE;
			recurring->events = g_ptr_array_new_with_free_func(
				headline_status_event_free);
			g_ptr_array_add(snapshot->recurring, recurring);
			g_hash_table_replace(by_subscription,
				headline_key(venture_entity_get_id(subscription)), recurring);
		}

		for (i = 0; i < events->len; i++)
		{
			VentureEntity *event = g_ptr_array_index(events, i);
			HeadlineRecurring *recurring;
			HeadlineStatusEvent *status;
			gint64 subscription_id;

			subscription_id = headline_int(event, "subscription-id");
			recurring = g_hash_table_lookup(by_subscription, &subscription_id);

			if (NULL == recurring)
				continue;

			status = g_new0(HeadlineStatusEvent, 1);
			g_object_get(event, "at", &status->at, "to-status", &status->status,
			             NULL);

			if ((NULL == status->at) ||
			    ((NULL != snapshot->cutoff) &&
			     (g_date_time_compare(status->at, snapshot->cutoff) >= 0)))
			{
				headline_status_event_free(status);
				continue;
			}

			g_ptr_array_add(recurring->events, status);
			snapshot->billing_history = TRUE;
		}
	}

	return TRUE;
}

gboolean
venture_headline_snapshot_billing_in_use(
	VentureHeadlineSnapshot	 *snapshot,
	gboolean		 *out_in_use,
	GError			**error
){
	g_return_val_if_fail(NULL != snapshot, FALSE);
	g_return_val_if_fail(NULL != out_in_use, FALSE);

	*out_in_use = FALSE;

	if (!venture_context_module_enabled(snapshot->context, "billing"))
		return TRUE;

	if (!headline_load_recurring(snapshot, error))
		return FALSE;

	*out_in_use = snapshot->billing_history;

	return TRUE;
}

/* A billing subscription status by its nick. The billing module registers
 * the enumeration without C names, so the nick is the stable spelling. */
static gint
headline_billing_status(const gchar *nick)
{
	gint value = -1;

	if (!venture_enum_from_nick(venture_billing_status_get_type(), nick, &value))
		return -1;

	return value;
}

/*
 * The state of one commitment at @when, and when and how it ended when it
 * had. A schedule runs from start_at until the earlier of end_at and its
 * deletion; paused, it is paused whatever the date, because the flag has
 * none. A subscription is what its latest event before @when left it:
 * active and past_due run, trialing and paused are paused, cancelled and
 * expired are gone.
 */
static HeadlineRecurringState
headline_recurring_state(
	const HeadlineRecurring	 *recurring,
	GDateTime		 *when,
	GDateTime		**out_ended,
	const gchar		**out_how
){
	*out_ended = NULL;
	*out_how = NULL;

	if (recurring->subscription)
	{
		HeadlineStatusEvent *latest = NULL;
		guint i;

		for (i = 0; i < recurring->events->len; i++)
		{
			HeadlineStatusEvent *event = g_ptr_array_index(recurring->events, i);

			if (g_date_time_compare(event->at, when) >= 0)
				continue;

			if ((NULL == latest) || (g_date_time_compare(event->at, latest->at) >= 0))
				latest = event;
		}

		if (NULL == latest)
			return HEADLINE_RECURRING_NONE;

		if ((headline_billing_status("active") == latest->status) ||
		    (headline_billing_status("past_due") == latest->status))
			return HEADLINE_RECURRING_RUNNING;

		if ((headline_billing_status("trialing") == latest->status) ||
		    (headline_billing_status("paused") == latest->status))
			return HEADLINE_RECURRING_PAUSED;

		*out_ended = latest->at;
		*out_how = (headline_billing_status("expired") == latest->status)
			? "expired" : "cancelled";

		return HEADLINE_RECURRING_NONE;
	}

	if ((NULL == recurring->started) ||
	    (g_date_time_compare(recurring->started, when) >= 0))
		return HEADLINE_RECURRING_NONE;

	if ((NULL != recurring->deleted) &&
	    (g_date_time_compare(recurring->deleted, when) < 0))
	{
		*out_ended = recurring->deleted;
		*out_how = "cancelled";
		return HEADLINE_RECURRING_NONE;
	}

	if ((NULL != recurring->ended) &&
	    (g_date_time_compare(recurring->ended, when) < 0))
	{
		*out_ended = recurring->ended;
		*out_how = "lapsed";
		return HEADLINE_RECURRING_NONE;
	}

	return recurring->paused ? HEADLINE_RECURRING_PAUSED
	                         : HEADLINE_RECURRING_RUNNING;
}

/* A customer lost from the recurring cohort: how, and when, the last of the
 * commitments that were running at the start ended. */
typedef struct
{
	const gchar	*how;
	GDateTime	*ended;
} HeadlineLoss;

static void
headline_loss_free(gpointer data)
{
	HeadlineLoss *loss = data;

	g_clear_pointer(&loss->ended, g_date_time_unref);
	g_free(loss);
}

/* A customer's recurring standing: the best state any of their commitments
 * is in, and how the last one that had been running at the start ended. */
typedef struct
{
	HeadlineRecurringState	 opening;
	HeadlineRecurringState	 closing;
	GDateTime		*ended;
	const gchar		*how;
} HeadlineRecurringCustomer;

/*
 * Recurring churn, counted in customers rather than in schedules.
 *
 * A customer is recurring at an instant when any invoice schedule or
 * subscription of theirs is running then. The cohort is the customers
 * recurring at the period's start; one is lost when none of their
 * commitments is running or paused at the anchor. Cancelling one of three
 * schedules is therefore not a third of a customer lost, a plan change that
 * replaces one subscription with another is not churn, and a pause is not
 * churn either -- a paused customer has not left.
 */
static gboolean
headline_recurring_churn(
	VentureHeadlineSnapshot	 *snapshot,
	const VentureDateRange	 *period,
	GDateTime		 *anchor,
	gint64			 *out_opening,
	GHashTable		 *out_lost,
	GError			**error
){
	g_autoptr(GHashTable) customers = NULL;
	GDateTime *start;
	GHashTableIter iter;
	gpointer key;
	gpointer value;
	guint i;

	*out_opening = 0;
	start = (NULL != period) ? venture_date_range_get_start(period) : NULL;

	if (!headline_load_recurring(snapshot, error))
		return FALSE;

	if (NULL == start)
		return TRUE;

	customers = headline_id_table(g_free);

	for (i = 0; i < snapshot->recurring->len; i++)
	{
		HeadlineRecurring *recurring = g_ptr_array_index(snapshot->recurring, i);
		HeadlineRecurringCustomer *customer;
		HeadlineRecurringState opening;
		HeadlineRecurringState closing;
		GDateTime *ended;
		GDateTime *unused_ended;
		const gchar *how;
		const gchar *unused_how;

		customer = g_hash_table_lookup(customers, &recurring->company_id);

		if (NULL == customer)
		{
			customer = g_new0(HeadlineRecurringCustomer, 1);
			g_hash_table_replace(customers, headline_key(recurring->company_id),
			                     customer);
		}

		opening = headline_recurring_state(recurring, start, &unused_ended,
		                                   &unused_how);
		closing = headline_recurring_state(recurring, anchor, &ended, &how);

		customer->opening = MAX(customer->opening, opening);
		customer->closing = MAX(customer->closing, closing);

		if ((HEADLINE_RECURRING_RUNNING == opening) && (NULL != ended) &&
		    ((NULL == customer->ended) ||
		     (g_date_time_compare(ended, customer->ended) > 0)))
		{
			customer->ended = ended;
			customer->how = how;
		}
	}

	g_hash_table_iter_init(&iter, customers);

	while (g_hash_table_iter_next(&iter, &key, &value))
	{
		HeadlineRecurringCustomer *customer = value;

		if (HEADLINE_RECURRING_RUNNING != customer->opening)
			continue;

		(*out_opening)++;

		if (HEADLINE_RECURRING_NONE == customer->closing)
		{
			HeadlineLoss *loss;

			loss = g_new0(HeadlineLoss, 1);
			loss->how = (NULL != customer->how) ? customer->how : "cancelled";
			loss->ended = (NULL != customer->ended)
				? g_date_time_ref(customer->ended) : NULL;
			g_hash_table_replace(out_lost, headline_key(*(gint64 *)key), loss);
		}
	}

	return TRUE;
}

VentureReportResult *
venture_headline_snapshot_churn(
	VentureHeadlineSnapshot	 *snapshot,
	VentureDateRange	 *period,
	GError			**error
){
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(GDateTime) anchor = NULL;
	g_autoptr(GPtrArray) activities = NULL;
	g_autoptr(GHashTable) lost = NULL;
	g_autoptr(GHashTable) named = NULL;
	g_autofree gchar *days_note = NULL;
	HeadlineChurnFigures figures;
	GHashTableIter iter;
	gpointer key;
	gint64 opening;
	gint64 days;
	guint i;

	g_return_val_if_fail(NULL != snapshot, NULL);

	anchor = headline_anchor(snapshot, period);
	days = headline_days(snapshot);
	lost = headline_id_table(headline_loss_free);

	if (!headline_recurring_churn(snapshot, period, anchor, &opening, lost,
	                              error))
		return NULL;

	activities = headline_activities(snapshot, anchor, error);

	if (NULL == activities)
		return NULL;

	headline_activity_churn(activities, anchor, &figures);

	/* Every name the table needs, in one read. */
	named = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	g_hash_table_iter_init(&iter, lost);

	while (g_hash_table_iter_next(&iter, &key, NULL))
		headline_id_add(named, *(gint64 *)key);

	for (i = 0; i < activities->len; i++)
	{
		HeadlineActivity *activity = g_ptr_array_index(activities, i);

		if (NULL != activity->churned_at)
			headline_id_add(named, activity->company_id);
	}

	if (!headline_load_companies(snapshot, named, error))
		return NULL;

	result = venture_report_result_new(
		"Customer churn (activity and recurring schedules)", period);

	venture_report_result_add_column(result, "kind", "Kind",
	                                 VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "customer", "Customer",
	                                 VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "detail", "What happened",
	                                 VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "date", "Date",
	                                 VENTURE_REPORT_COLUMN_TEXT);

	{
		g_autoptr(GArray) order = NULL;

		/* Hash order is not an order; a table that shuffles between
		 * runs cannot be compared with last month's. */
		order = g_array_new(FALSE, FALSE, sizeof(gint64));
		g_hash_table_iter_init(&iter, lost);

		while (g_hash_table_iter_next(&iter, &key, NULL))
			g_array_append_val(order, *(gint64 *)key);

		g_array_sort(order, headline_gap_compare);

		for (i = 0; i < order->len; i++)
		{
			g_autofree gchar *name = NULL;
			g_autofree gchar *date = NULL;
			HeadlineLoss *loss;
			gint64 company_id;

			company_id = g_array_index(order, gint64, i);
			loss = g_hash_table_lookup(lost, &company_id);
			name = headline_company_name(snapshot, company_id);
			date = (NULL != loss->ended)
				? venture_time_to_date_string(loss->ended, NULL)
				: g_strdup("");
			venture_report_result_begin_row(result);
			venture_report_result_set_text(result, "kind", "recurring");
			venture_report_result_set_text(result, "customer", name);
			venture_report_result_set_text(result, "detail", loss->how);
			venture_report_result_set_text(result, "date", date);
		}
	}

	for (i = 0; i < activities->len; i++)
	{
		g_autofree gchar *name = NULL;
		g_autofree gchar *when = NULL;
		g_autoptr(GDateTime) window_start = NULL;
		HeadlineActivity *activity = g_ptr_array_index(activities, i);

		window_start = g_date_time_add_months(anchor, -HEADLINE_WINDOW_MONTHS);

		if ((NULL == activity->churned_at) ||
		    (g_date_time_compare(activity->churned_at, window_start) < 0))
			continue;

		name = headline_company_name(snapshot, activity->company_id);
		when = venture_time_to_date_string(activity->last, NULL);
		venture_report_result_begin_row(result);
		venture_report_result_set_text(result, "kind", "activity");
		venture_report_result_set_text(result, "customer", name);
		venture_report_result_set_text(result, "detail",
		                               "no paid revenue since");
		venture_report_result_set_text(result, "date", when);
	}

	venture_report_result_add_metric(result,
		venture_metric_new_count("active_at_start",
		                         "Recurring customers at start", opening));
	venture_report_result_add_metric(result,
		venture_metric_new_count("churned", "Recurring customers lost",
		                         (gint64)g_hash_table_size(lost)));

	if (opening > 0)
	{
		VentureMetric *metric;

		metric = venture_metric_new_ratio("recurring_churn", "Recurring churn",
			headline_render_bps(headline_basis_points(
				(gint64)g_hash_table_size(lost), opening)));
		venture_metric_set_higher_is_better(metric, FALSE);
		venture_report_result_add_metric(result, metric);
	}
	else
	{
		venture_report_result_add_metric(result,
			venture_metric_new_text("recurring_churn", "Recurring churn",
			                        "n/a"));
		venture_report_result_append_note(result,
			(venture_context_module_enabled(snapshot->context, "recurring") ||
			 venture_context_module_enabled(snapshot->context, "billing"))
			? "No customer had an invoice schedule or subscription running "
			  "when the period opened, so recurring churn has no "
			  "denominator and is n/a."
			: "The recurring and billing modules are off, so recurring "
			  "churn is n/a.");
	}

	venture_report_result_add_metric(result,
		venture_metric_new_count("active_customers",
		                         "Customers paid in trailing 12 months",
		                         figures.active_customers));
	venture_report_result_add_metric(result,
		venture_metric_new_count("inactive_customers",
		                         "Gone quiet in trailing 12 months",
		                         figures.events));
	venture_report_result_add_metric(result,
		venture_metric_new_count("customer_months",
		                         "Customer-months at risk",
		                         figures.customer_months));

	if (figures.customer_months > 0)
	{
		VentureMetric *metric;
		gint64 bps;

		bps = headline_basis_points(figures.events, figures.customer_months);
		metric = venture_metric_new_ratio("activity_churn",
		                                  "Monthly activity churn",
		                                  headline_render_bps(bps));
		venture_metric_set_higher_is_better(metric, FALSE);
		venture_report_result_add_metric(result, metric);
		metric = venture_metric_new_count("activity_churn_bps",
		                                  "Monthly activity churn (basis points)",
		                                  bps);
		venture_metric_set_higher_is_better(metric, FALSE);
		venture_report_result_add_metric(result, metric);
	}
	else
	{
		venture_report_result_add_metric(result,
			venture_metric_new_text("activity_churn", "Monthly activity churn",
			                        "n/a"));
		venture_report_result_append_note(result,
			"No paying customer was at risk in any month of the trailing "
			"twelve, so activity churn has no denominator and is n/a.");
	}

	venture_report_result_add_metric(result,
		venture_metric_new_count("days", "Quiet days threshold", days));

	venture_report_result_append_note(result,
		"Recurring churn: customers with an invoice schedule or subscription "
		"running when the period opened, and none running or paused at its "
		"end (or now), over those running at the start. Pauses are not "
		"churn; replacing one schedule or plan with another is not churn.");

	days_note = g_strdup_printf(
		"Monthly activity churn: customers whose last cash receipt plus "
		"their quiet threshold fell in the twelve months before the "
		"period's end (or now), over customer-months at risk in those "
		"months -- a customer is at risk in a month when they had paid "
		"before it opened and their latest receipt still covered its "
		"opening. The quiet threshold is %" G_GINT64_FORMAT " days, or one "
		"and a half times the customer's own rhythm when that is longer: "
		"their subscription or schedule interval, else the median gap "
		"between their receipts. Pass days=N to change it.", days);
	venture_report_result_append_note(result, days_note);

	return g_steal_pointer(&result);
}

static VentureReportResult *
venture_report_churn(
	VentureContext		 *context,
	VentureDateRange	 *period,
	JsonObject		 *options,
	GError			**error
){
	g_autoptr(VentureHeadlineSnapshot) snapshot = NULL;

	snapshot = venture_headline_snapshot_new(context, options, error);

	if (NULL == snapshot)
		return NULL;

	return venture_headline_snapshot_churn(snapshot, period, error);
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
 * Support cost per customer over @window: worklog hours on tickets that
 * name a company, priced at the configured hourly rate. Hours are stored as
 * a double by the ticket module; they are rounded once to hundredths of an
 * hour and from then on the arithmetic is exact money times a rational.
 */
static gboolean
ltv_support_cost(
	VentureHeadlineSnapshot	 *snapshot,
	const VentureDateRange	 *window,
	GHashTable		 *out_cost,
	VentureMoney		**out_total,
	guint			 *inout_skipped,
	GError			**error
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) worklogs = NULL;
	guint i;

	headline_load_settings(snapshot);
	*out_total = venture_money_new_zero(snapshot->currency);

	if ((NULL == snapshot->hourly_rate) ||
	    venture_money_is_zero(snapshot->hourly_rate) ||
	    !venture_context_module_enabled(snapshot->context, "tickets"))
		return TRUE;

	if (NULL == snapshot->ticket_company)
	{
		g_autoptr(GPtrArray) tickets = NULL;

		tickets = headline_fetch_positive(snapshot, VENTURE_TYPE_TICKET,
		                                  "company-id", TRUE, FALSE, error);

		if (NULL == tickets)
			return FALSE;

		snapshot->ticket_company = headline_id_table(g_free);

		for (i = 0; i < tickets->len; i++)
		{
			VentureEntity *ticket = g_ptr_array_index(tickets, i);

			if ((0 != snapshot->venture_id) &&
			    (headline_int(ticket, "venture-id") != snapshot->venture_id))
				continue;

			g_hash_table_replace(snapshot->ticket_company,
				headline_key(venture_entity_get_id(ticket)),
				headline_key(headline_int(ticket, "company-id")));
		}
	}

	query = headline_query(snapshot, VENTURE_TYPE_WORKLOG, FALSE, TRUE, error);

	if ((NULL == query) ||
	    !venture_query_set_date_range(query, "occurred-at",
	                                  (VentureDateRange *)window, error))
		return FALSE;

	worklogs = headline_find(snapshot, query, error);

	if (NULL == worklogs)
		return FALSE;

	for (i = 0; i < worklogs->len; i++)
	{
		g_autoptr(VentureMoney) line = NULL;
		VentureMoney *total;
		gdouble hours = 0.0;
		gint64 hundredths;
		gint64 ticket_id;
		gint64 *company_id;

		g_object_get(g_ptr_array_index(worklogs, i), "hours", &hours, NULL);
		ticket_id = headline_int(g_ptr_array_index(worklogs, i), "ticket-id");
		company_id = g_hash_table_lookup(snapshot->ticket_company, &ticket_id);

		if ((NULL == company_id) || (hours <= 0.0))
			continue;

		hundredths = (gint64)(hours * 100.0 + 0.5);
		line = venture_money_multiply_rational(snapshot->hourly_rate,
		                                       hundredths, 100, NULL);

		if ((NULL == line) || !headline_accumulate(out_total, line, inout_skipped))
		{
			if (NULL == line)
				(*inout_skipped)++;
			continue;
		}

		total = g_hash_table_lookup(out_cost, company_id);

		if (NULL == total)
		{
			g_hash_table_replace(out_cost, headline_key(*company_id),
			                     g_steal_pointer(&line));
			continue;
		}

		{
			VentureMoney *sum = venture_money_add(total, line, NULL);

			if (NULL != sum)
				g_hash_table_replace(out_cost, headline_key(*company_id), sum);
		}
	}

	return TRUE;
}

/*
 * Gross margin over @window, in basis points.
 *
 * margin = (revenue - cost of revenue) / revenue, where revenue is the pnl
 * report's net revenue for the window (scoped exactly as this report is:
 * organisation, venture, as_of) and cost of revenue is every expense and
 * vendor bill line flagged cost of revenue and dated in the window, plus the
 * attributed support cost. A row flagged acquisition, or naming a campaign,
 * is never cost of revenue: acquisition spend is what CAC divides, and
 * counting it here too made LTV:CAC charge for it twice. Nothing else on the
 * P&L -- salaries, rent, advertising -- is cost of revenue unless flagged.
 *
 * The margin is unknown ("n/a", with a note) when the P&L has no revenue in
 * the book currency, or revenue is zero or negative, or the margin itself is
 * zero or negative: a lifetime value projected from a negative margin is a
 * negative number that reads as a precise loss per customer rather than as
 * the absence of an answer.
 */
typedef struct
{
	VentureMoney	*revenue;
	VentureMoney	*cost;
	gint64		 flagged_rows;
	gint64		 margin_bps;
	gboolean	 known;
	gboolean	 computed;
	gchar		*reason;
} LtvMargin;

static void
ltv_margin_clear(LtvMargin *margin)
{
	g_clear_pointer(&margin->revenue, venture_money_free);
	g_clear_pointer(&margin->cost, venture_money_free);
	g_clear_pointer(&margin->reason, g_free);
}

static gboolean
ltv_margin(
	VentureHeadlineSnapshot	 *snapshot,
	VentureDateRange	 *window,
	const VentureMoney	 *support_total,
	LtvMargin		 *margin,
	guint			 *inout_skipped,
	GError			**error
){
	g_autoptr(VentureReportResult) pnl = NULL;
	g_autoptr(VentureMoney) gross = NULL;
	VentureReport *report;
	GPtrArray *metrics;
	guint i;

	if (!headline_load_spend(snapshot, error))
		return FALSE;

	report = venture_report_registry_lookup(
		venture_context_get_report_registry(snapshot->context), "pnl");

	if (NULL == report)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		                    "The pnl report is not available, so gross "
		                    "margin cannot be read");
		return FALSE;
	}

	pnl = venture_report_generate(report, snapshot->context, window,
	                              snapshot->options, error);

	if (NULL == pnl)
		return FALSE;

	metrics = venture_report_result_get_metrics(pnl);

	for (i = 0; i < metrics->len; i++)
	{
		VentureMetric *metric = g_ptr_array_index(metrics, i);

		if ((0 == g_strcmp0(venture_metric_get_key(metric), "revenue")) &&
		    (NULL != venture_metric_get_money(metric)))
			margin->revenue = venture_money_copy(venture_metric_get_money(metric));
	}

	margin->cost = venture_money_copy(support_total);

	for (i = 0; i < snapshot->spend->len; i++)
	{
		HeadlineSpend *row = g_ptr_array_index(snapshot->spend, i);

		if (!row->cost_of_revenue || row->acquisition ||
		    (0 != row->campaign_id) || !headline_within(row->date, window))
			continue;

		margin->flagged_rows++;
		headline_accumulate(&margin->cost, row->amount, inout_skipped);
	}

	if ((NULL == margin->revenue) ||
	    (0 != g_ascii_strcasecmp(venture_money_get_currency(margin->revenue),
	                             snapshot->currency)))
	{
		margin->reason = g_strdup_printf(
			"the trailing twelve months have no P&L revenue in %s, the book "
			"currency", snapshot->currency);
		return TRUE;
	}

	if (venture_money_get_amount(margin->revenue) <= 0)
	{
		margin->reason = g_strdup(
			"the trailing twelve months have no positive net revenue on the "
			"P&L, so there is no margin to take");
		return TRUE;
	}

	gross = venture_money_subtract(margin->revenue, margin->cost, NULL);

	if (NULL == gross)
	{
		(*inout_skipped)++;
		margin->reason = g_strdup(
			"revenue and cost of revenue could not be subtracted");
		return TRUE;
	}

	margin->margin_bps = headline_basis_points(venture_money_get_amount(gross),
	                                           venture_money_get_amount(margin->revenue));
	margin->computed = TRUE;

	if (margin->margin_bps <= 0)
	{
		margin->reason = g_strdup_printf(
			"cost of revenue is at least the trailing twelve months' revenue "
			"(a gross margin of %.2f%%), and a lifetime value projected from "
			"a margin at or below zero is not a number to act on",
			(gdouble)margin->margin_bps / 100.0);
		return TRUE;
	}

	margin->known = TRUE;

	return TRUE;
}

/*
 * Customer lifetime value for the period, realised and projected.
 *
 * Realised: each customer's cash (see headline_load_cash) before the anchor,
 * receipts less refunds, with the mean, median and total, and the ten
 * largest customers as rows.
 *
 * Projected: revenue per customer-month x gross margin / monthly activity
 * churn, all over the twelve months before the anchor:
 * - revenue per customer-month: headline_spread_revenue;
 * - gross margin: ltv_margin;
 * - monthly activity churn: headline_activity_churn. Dividing by it is
 *   multiplying by customer-months at risk / churn events, one exact
 *   rational.
 *
 * Withheld as "insufficient data" below the minimum customer-months, and
 * "n/a" with a note when the margin is unknown or no customer churned.
 */
VentureReportResult *
venture_headline_snapshot_ltv(
	VentureHeadlineSnapshot	 *snapshot,
	VentureDateRange	 *period,
	GError			**error
){
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(GHashTable) support = NULL;
	g_autoptr(GHashTable) top = NULL;
	g_autoptr(GPtrArray) customers = NULL;
	g_autoptr(GPtrArray) activities = NULL;
	g_autoptr(VentureMoney) total = NULL;
	g_autoptr(VentureMoney) mean = NULL;
	g_autoptr(VentureMoney) median = NULL;
	g_autoptr(VentureMoney) revenue_12m = NULL;
	g_autoptr(VentureMoney) support_total = NULL;
	g_autoptr(VentureMoney) arpu = NULL;
	g_autoptr(VentureMoney) projected = NULL;
	g_autoptr(VentureDateRange) window = NULL;
	g_autoptr(GDateTime) anchor = NULL;
	g_autoptr(GDateTime) window_start = NULL;
	HeadlineChurnFigures churn;
	LtvMargin margin = { NULL, NULL, 0, 0, FALSE, FALSE, NULL };
	GHashTableIter iter;
	gpointer key;
	gpointer value;
	gint64 months;
	const gchar *withheld;
	guint skipped;
	guint i;

	g_return_val_if_fail(NULL != snapshot, NULL);

	headline_load_settings(snapshot);

	if (!headline_load_cash(snapshot, error))
		return NULL;

	skipped = snapshot->cash_skipped;
	anchor = headline_anchor(snapshot, period);
	window_start = g_date_time_add_months(anchor, -HEADLINE_WINDOW_MONTHS);
	window = venture_date_range_new_labelled(window_start, anchor,
	                                         "Trailing twelve months");
	support = headline_id_table((GDestroyNotify)venture_money_free);

	if (!ltv_support_cost(snapshot, window, support, &support_total, &skipped,
	                      error))
		return NULL;

	/* Realised: each customer's cash before the anchor, net of refunds. */
	customers = g_ptr_array_new_with_free_func(ltv_customer_free);
	total = venture_money_new_zero(snapshot->currency);
	g_hash_table_iter_init(&iter, snapshot->cash);

	while (g_hash_table_iter_next(&iter, &key, &value))
	{
		GPtrArray *history = value;
		LtvCustomer *customer;
		VentureMoney *cost;
		gboolean any = FALSE;

		customer = g_new0(LtvCustomer, 1);
		customer->company_id = *(gint64 *)key;
		customer->realised = venture_money_new_zero(snapshot->currency);

		for (i = 0; i < history->len; i++)
		{
			HeadlineCash *movement = g_ptr_array_index(history, i);

			if (g_date_time_compare(movement->date, anchor) >= 0)
				break;

			any = TRUE;
			headline_accumulate(&customer->realised, movement->amount,
			                    &skipped);
		}

		if (!any)
		{
			ltv_customer_free(customer);
			continue;
		}

		cost = g_hash_table_lookup(support, key);

		if (NULL != cost)
			customer->support_cost = venture_money_copy(cost);

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
		mean = venture_money_new_zero(snapshot->currency);

	if (NULL == median)
		median = venture_money_new_zero(snapshot->currency);

	/* Projected. */
	if (!headline_spread_revenue(snapshot, anchor, &revenue_12m, &months,
	                             &skipped, error))
		return NULL;

	activities = headline_activities(snapshot, anchor, error);

	if (NULL == activities)
		return NULL;

	headline_activity_churn(activities, anchor, &churn);

	if (!ltv_margin(snapshot, window, support_total, &margin, &skipped, error))
	{
		ltv_margin_clear(&margin);
		return NULL;
	}

	if (months > 0)
		arpu = venture_money_multiply_rational(revenue_12m, 1, months, NULL);

	withheld = NULL;

	if (months < snapshot->minimum_months)
		withheld = "insufficient data";
	else if (!margin.known || (0 == churn.events) ||
	         (0 == churn.customer_months) || (NULL == arpu))
		withheld = "n/a";

	if (NULL == withheld)
	{
		g_autoptr(VentureMoney) margin_share = NULL;

		margin_share = venture_money_multiply_percent(arpu, margin.margin_bps,
		                                              NULL);
		projected = (NULL != margin_share)
			? venture_money_multiply_rational(margin_share,
			                                  churn.customer_months,
			                                  churn.events, NULL)
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
	}
	else
	{
		g_autofree gchar *note = NULL;

		venture_report_result_add_metric(result,
			venture_metric_new_text("projected", "Projected LTV", withheld));

		if (months < snapshot->minimum_months)
			note = g_strdup_printf(
				"Projected LTV is withheld: %" G_GINT64_FORMAT
				" customer-month%s of paid revenue in the trailing twelve "
				"months is below the minimum of %" G_GINT64_FORMAT
				". Change it in the organisation's headline settings.",
				months, (1 == months) ? "" : "s", snapshot->minimum_months);
		else if (!margin.known)
			note = g_strdup_printf("Projected LTV is n/a: %s.",
				(NULL != margin.reason) ? margin.reason
				                        : "there is no gross margin");
		else if (0 == churn.events)
			note = g_strdup(
				"Projected LTV is n/a: no customer went quiet in the "
				"trailing twelve months, so monthly activity churn is zero "
				"and lifetime would be unbounded. That is good news, not a "
				"number.");
		else
			note = g_strdup(
				"Projected LTV is n/a: the projection could not be computed "
				"without overflow.");

		venture_report_result_append_note(result, note);
	}

	if (NULL != arpu)
		venture_report_result_add_metric(result,
			venture_metric_new_money("arpu", "Revenue per customer-month",
			                         arpu));
	else
		venture_report_result_add_metric(result,
			venture_metric_new_text("arpu", "Revenue per customer-month",
			                        "n/a"));

	venture_report_result_add_metric(result,
		venture_metric_new_money("revenue_12m", "Cash revenue, trailing 12 months",
		                         revenue_12m));

	if (margin.known)
	{
		venture_report_result_add_metric(result,
			venture_metric_new_ratio("gross_margin", "Gross margin",
			                         headline_render_bps(margin.margin_bps)));
		venture_report_result_add_metric(result,
			venture_metric_new_count("gross_margin_bps",
			                         "Gross margin (basis points)",
			                         margin.margin_bps));
	}
	else
	{
		venture_report_result_add_metric(result,
			venture_metric_new_text("gross_margin", "Gross margin", "n/a"));
	}

	if (NULL != margin.cost)
	{
		VentureMetric *metric;

		metric = venture_metric_new_money("cost_of_revenue", "Cost of revenue",
		                                  margin.cost);
		venture_metric_set_higher_is_better(metric, FALSE);
		venture_report_result_add_metric(result, metric);
	}

	if (churn.customer_months > 0)
	{
		VentureMetric *metric;
		gint64 bps;

		bps = headline_basis_points(churn.events, churn.customer_months);
		metric = venture_metric_new_ratio("monthly_churn",
			"Monthly activity churn", headline_render_bps(bps));
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
		venture_metric_new_count("churn_events", "Gone quiet in trailing 12 months",
		                         churn.events));
	venture_report_result_add_metric(result,
		venture_metric_new_count("risk_months", "Customer-months at risk",
		                         churn.customer_months));
	venture_report_result_add_metric(result,
		venture_metric_new_count("customer_months", "Customer-months of revenue",
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

	top = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);

	for (i = 0; (i < customers->len) && (i < 10); i++)
		headline_id_add(top,
			((LtvCustomer *)g_ptr_array_index(customers, i))->company_id);

	if (!headline_load_companies(snapshot, top, error))
	{
		ltv_margin_clear(&margin);
		return NULL;
	}

	for (i = 0; (i < customers->len) && (i < 10); i++)
	{
		g_autofree gchar *name = NULL;
		LtvCustomer *customer = g_ptr_array_index(customers, i);

		name = headline_company_name(snapshot, customer->company_id);

		venture_report_result_begin_row(result);
		venture_report_result_set_text(result, "customer", name);
		venture_report_result_set_money(result, "realised", customer->realised);

		if (NULL != customer->support_cost)
			venture_report_result_set_money(result, "support_cost",
			                                customer->support_cost);
	}

	if (margin.computed && (0 == margin.flagged_rows) &&
	    venture_money_is_zero(support_total))
		venture_report_result_append_note(result,
			"Nothing in the trailing twelve months is flagged cost of revenue "
			"and no support hours are priced, so gross margin is 100%. Flag "
			"hosting, fulfilment and similar expenses as cost of revenue for "
			"a real margin.");

	if ((NULL == snapshot->hourly_rate) ||
	    venture_money_is_zero(snapshot->hourly_rate))
		venture_report_result_append_note(result,
			"No worklog hourly rate is configured, so support hours cost "
			"nothing in the gross margin. Set hourly_rate in the "
			"organisation's headline settings to attribute them.");

	headline_flag_skipped(result, skipped, snapshot->currency);
	ltv_margin_clear(&margin);

	return g_steal_pointer(&result);
}

static VentureReportResult *
venture_report_ltv(
	VentureContext		 *context,
	VentureDateRange	 *period,
	JsonObject		 *options,
	GError			**error
){
	g_autoptr(VentureHeadlineSnapshot) snapshot = NULL;

	snapshot = venture_headline_snapshot_new(context, options, error);

	if (NULL == snapshot)
		return NULL;

	return venture_headline_snapshot_ltv(snapshot, period, error);
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

	if (NULL == result)
		return NULL;

	metrics = venture_report_result_get_metrics(result);

	for (i = 0; i < metrics->len; i++)
	{
		VentureMetric *metric = g_ptr_array_index(metrics, i);

		if (0 == g_strcmp0(venture_metric_get_key(metric), key))
			return metric;
	}

	return NULL;
}

/*
 * Average revenue per account, per month. With billing in use it is the
 * billing mrr report's ARPA: contracted MRR at the period's end over the
 * companies with positive MRR. Otherwise it is revenue per customer-month
 * from the ltv report: the trailing twelve months' cash, spread over the
 * months it paid for, per paying customer per month. Returns a new amount,
 * and which of the two it is in @out_source.
 */
static VentureMoney *
headline_arpa(
	VentureHeadlineSnapshot	 *snapshot,
	VentureDateRange	 *period,
	VentureReportResult	 *ltv,
	const gchar		**out_source,
	GError			**error
){
	VentureMetric *arpu;
	gboolean billing = FALSE;

	*out_source = NULL;

	if (!venture_headline_snapshot_billing_in_use(snapshot, &billing, error))
		return NULL;

	if (billing)
	{
		g_autoptr(VentureReportResult) mrr = NULL;
		VentureReport *report;
		VentureMetric *arpa;

		report = venture_report_registry_lookup(
			venture_context_get_report_registry(snapshot->context), "mrr");
		mrr = (NULL != report)
			? venture_report_generate(report, snapshot->context, period,
			                          snapshot->options, error)
			: NULL;

		if ((NULL != report) && (NULL == mrr))
			return NULL;

		arpa = headline_find_metric(mrr, "arpa");

		if ((NULL != arpa) && (NULL != venture_metric_get_money(arpa)))
		{
			*out_source = "billing MRR per paying company";
			return venture_money_copy(venture_metric_get_money(arpa));
		}
	}

	arpu = headline_find_metric(ltv, "arpu");

	if ((NULL != arpu) && (NULL != venture_metric_get_money(arpu)))
	{
		*out_source = "trailing twelve months' cash per customer-month";
		return venture_money_copy(venture_metric_get_money(arpu));
	}

	return NULL;
}

/*
 * LTV:CAC for the period, with ARPA and CAC payback beside it.
 *
 * - ltv_cac: projected LTV / CAC, in hundredths rounded half to even; n/a
 *   whenever either side is, or they are in different currencies.
 * - arpa: see headline_arpa.
 * - payback_months: CAC / (ARPA x gross margin), the months of gross profit
 *   one new customer takes to repay what acquiring them cost, in hundredths
 *   rounded half to even; n/a when CAC, ARPA or the margin is unknown, or
 *   the margin is at or below zero -- a customer who never repays has no
 *   payback period, not a very long one.
 */
VentureReportResult *
venture_headline_snapshot_ltv_cac(
	VentureHeadlineSnapshot	 *snapshot,
	VentureDateRange	 *period,
	VentureReportResult	 *ltv,
	VentureReportResult	 *cac,
	GError			**error
){
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(VentureReportResult) owned_ltv = NULL;
	g_autoptr(VentureReportResult) owned_cac = NULL;
	g_autoptr(VentureMoney) arpa = NULL;
	g_autofree gchar *payback_text = NULL;
	VentureMetric *projected;
	VentureMetric *cost;
	VentureMetric *margin;
	const VentureMoney *ltv_amount;
	const VentureMoney *cac_amount;
	const gchar *reason;
	const gchar *arpa_source;
	gint64 payback = -1;

	g_return_val_if_fail(NULL != snapshot, NULL);

	if (NULL == ltv)
	{
		owned_ltv = venture_headline_snapshot_ltv(snapshot, period, error);

		if (NULL == owned_ltv)
			return NULL;

		ltv = owned_ltv;
	}

	if (NULL == cac)
	{
		owned_cac = venture_headline_snapshot_cac(snapshot, period, error);

		if (NULL == owned_cac)
			return NULL;

		cac = owned_cac;
	}

	arpa = headline_arpa(snapshot, period, ltv, &arpa_source, error);

	if ((NULL == arpa) && (NULL != error) && (NULL != *error))
		return NULL;

	projected = headline_find_metric(ltv, "projected");
	cost = headline_find_metric(cac, "cac");
	margin = headline_find_metric(ltv, "gross_margin_bps");
	ltv_amount = (NULL != projected) ? venture_metric_get_money(projected)
	                                 : NULL;
	cac_amount = (NULL != cost) ? venture_metric_get_money(cost) : NULL;
	reason = NULL;

	result = venture_report_result_new("LTV to CAC", period);

	if (NULL == ltv_amount)
		reason = (NULL != projected) ? venture_metric_get_text(projected)
		                             : "n/a";
	else if ((NULL == cac_amount) || venture_money_is_zero(cac_amount))
		reason = "n/a";
	else if (0 != g_ascii_strcasecmp(venture_money_get_currency(ltv_amount),
	                                 venture_money_get_currency(cac_amount)))
		reason = "n/a";

	if (NULL == reason)
	{
		gint64 hundredths = 0;

		if (headline_scaled(venture_money_get_amount(ltv_amount), 100,
		                    venture_money_get_amount(cac_amount), &hundredths))
			venture_report_result_add_metric(result,
				venture_metric_new_number("ltv_cac", "LTV:CAC",
				                          (gdouble)hundredths / 100.0));
		else
			reason = "n/a";
	}

	if (NULL != reason)
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
				"Projected LTV and CAC are in different currencies, or too "
				"large to divide, so the ratio is refused rather than "
				"guessed.");
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
	{
		VentureMetric *metric;

		metric = venture_metric_new_money("cac", "CAC", cac_amount);
		venture_metric_set_higher_is_better(metric, FALSE);
		venture_report_result_add_metric(result, metric);
	}
	else
	{
		venture_report_result_add_metric(result,
			venture_metric_new_text("cac", "CAC", "n/a"));
	}

	if (NULL != arpa)
		venture_report_result_add_metric(result,
			venture_metric_new_money("arpa", "ARPA", arpa));
	else
		venture_report_result_add_metric(result,
			venture_metric_new_text("arpa", "ARPA", "n/a"));

	/* Payback: CAC minor units x 100 (hundredths of a month) x 10,000
	 * (basis points) over ARPA minor units x margin basis points. */
	if ((NULL != cac_amount) && (NULL != arpa) && (NULL != margin) &&
	    (venture_metric_get_number(margin) > 0.0) &&
	    (venture_money_get_amount(arpa) > 0) &&
	    (0 == g_ascii_strcasecmp(venture_money_get_currency(arpa),
	                             venture_money_get_currency(cac_amount))) &&
	    (venture_money_get_exponent(arpa) ==
	     venture_money_get_exponent(cac_amount)))
	{
		gint64 margin_bps = (gint64)venture_metric_get_number(margin);
		gint64 denominator;

		if (!__builtin_mul_overflow(venture_money_get_amount(arpa), margin_bps,
		                            &denominator) &&
		    !headline_scaled(venture_money_get_amount(cac_amount), 1000000,
		                     denominator, &payback))
			payback = -1;
	}

	if (payback >= 0)
	{
		VentureMetric *metric;

		metric = venture_metric_new_number("payback_months",
		                                   "CAC payback (months)",
		                                   (gdouble)payback / 100.0);
		venture_metric_set_higher_is_better(metric, FALSE);
		venture_report_result_add_metric(result, metric);
		payback_text = venture_metric_format_value(metric);
	}
	else
	{
		venture_report_result_add_metric(result,
			venture_metric_new_text("payback_months", "CAC payback (months)",
			                        "n/a"));
		payback_text = g_strdup("n/a");
		venture_report_result_append_note(result,
			"CAC payback is n/a: it needs a CAC, an ARPA and a positive gross "
			"margin in the same currency.");
	}

	if (NULL != arpa_source)
	{
		g_autofree gchar *note = NULL;

		note = g_strdup_printf("ARPA is %s. CAC payback is CAC / (ARPA x "
		                       "gross margin), in months.", arpa_source);
		venture_report_result_append_note(result, note);
	}

	venture_report_result_add_column(result, "side", "Side",
	                                 VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "value", "Value",
	                                 VENTURE_REPORT_COLUMN_TEXT);

	{
		g_autofree gchar *ltv_text = NULL;
		g_autofree gchar *cac_text = NULL;
		g_autofree gchar *arpa_text = NULL;

		ltv_text = (NULL != projected) ? venture_metric_format_value(projected)
		                               : g_strdup("n/a");
		cac_text = (NULL != cost) ? venture_metric_format_value(cost)
		                          : g_strdup("n/a");
		arpa_text = (NULL != arpa) ? venture_money_to_display_string(arpa, TRUE)
		                           : g_strdup("n/a");

		venture_report_result_begin_row(result);
		venture_report_result_set_text(result, "side", "Projected LTV");
		venture_report_result_set_text(result, "value", ltv_text);
		venture_report_result_begin_row(result);
		venture_report_result_set_text(result, "side", "CAC");
		venture_report_result_set_text(result, "value", cac_text);
		venture_report_result_begin_row(result);
		venture_report_result_set_text(result, "side", "ARPA");
		venture_report_result_set_text(result, "value", arpa_text);
		venture_report_result_begin_row(result);
		venture_report_result_set_text(result, "side", "CAC payback (months)");
		venture_report_result_set_text(result, "value", payback_text);
	}

	return g_steal_pointer(&result);
}

static VentureReportResult *
venture_report_ltv_cac(
	VentureContext		 *context,
	VentureDateRange	 *period,
	JsonObject		 *options,
	GError			**error
){
	g_autoptr(VentureHeadlineSnapshot) snapshot = NULL;

	snapshot = venture_headline_snapshot_new(context, options, error);

	if (NULL == snapshot)
		return NULL;

	return venture_headline_snapshot_ltv_cac(snapshot, period, NULL, NULL,
	                                         error);
}

/* ==========================================================================
 * Cohorts
 * ========================================================================== */

/* The months that can be read from a cohort: its own and the twelve after. */
#define HEADLINE_COHORT_MONTHS (13)

typedef struct
{
	gint		 index;
	gint		 year;
	gint		 month;
	gint64		 size;
	gint64		 active[HEADLINE_COHORT_MONTHS];
} HeadlineCohort;

static gint
headline_cohort_compare(
	gconstpointer	a,
	gconstpointer	b
){
	const HeadlineCohort *left = *(HeadlineCohort *const *)a;
	const HeadlineCohort *right = *(HeadlineCohort *const *)b;

	return left->index - right->index;
}

/*
 * Cohort retention.
 *
 * A cohort is the companies whose first cash receipt fell in the period,
 * grouped by the calendar month of that receipt, read in UTC because a
 * receipt's date is a calendar date stored as midnight UTC (see
 * venture_date_range_parse()): read in New York, 1 March was February.
 * A company is still paying in month k of its cohort (month 0 being the
 * cohort's own) when a positive receipt covers that calendar month: one
 * received in it, or one on a yearly cadence received in the eleven months
 * before it. The share for month k is those companies over the cohort's
 * size. The period only chooses the cohorts; they are followed up to now,
 * or as_of, so last quarter's cohorts show how they have done since. A
 * month that has not started by then is left blank rather than shown as
 * zero: nobody has had the chance to pay in it yet.
 *
 * The month 1, 3, 6 and 12 metrics pool every cohort that has reached that
 * month: the companies still paying over the companies in those cohorts.
 */
static VentureReportResult *
venture_report_cohorts(
	VentureContext		 *context,
	VentureDateRange	 *period,
	JsonObject		 *options,
	GError			**error
){
	g_autoptr(VentureHeadlineSnapshot) snapshot = NULL;
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(GPtrArray) cohorts = NULL;
	g_autoptr(GHashTable) by_month = NULL;
	g_autoptr(GDateTime) anchor = NULL;
	static const gint milestones[] = { 1, 3, 6, 12 };
	g_autoptr(GTimeZone) zone = NULL;
	GHashTableIter iter;
	gpointer value;
	gint64 customers = 0;
	guint i;

	snapshot = venture_headline_snapshot_new(context, options, error);

	if ((NULL == snapshot) || !headline_load_cash(snapshot, error) ||
	    !headline_load_cadence(snapshot, error))
		return NULL;

	zone = g_time_zone_new_utc();
	anchor = headline_now(snapshot);
	cohorts = g_ptr_array_new_with_free_func(g_free);
	by_month = g_hash_table_new(g_int_hash, g_int_equal);
	g_hash_table_iter_init(&iter, snapshot->cash);

	while (g_hash_table_iter_next(&iter, NULL, &value))
	{
		g_autoptr(GDateTime) local_first = NULL;
		GPtrArray *history = value;
		GDateTime *first;
		HeadlineCohort *cohort;
		gboolean seen[HEADLINE_COHORT_MONTHS];
		gint key;
		gint k;
		guint j;

		first = headline_first_cash(history);

		if ((NULL == first) || !headline_within(first, period) ||
		    (g_date_time_compare(first, anchor) >= 0))
			continue;

		local_first = g_date_time_to_timezone(first, zone);
		key = (g_date_time_get_year(local_first) * 12) +
		      (g_date_time_get_month(local_first) - 1);
		cohort = g_hash_table_lookup(by_month, &key);

		if (NULL == cohort)
		{
			cohort = g_new0(HeadlineCohort, 1);
			cohort->index = key;
			cohort->year = g_date_time_get_year(local_first);
			cohort->month = g_date_time_get_month(local_first);
			g_ptr_array_add(cohorts, cohort);
			/* The key lives in the cohort, which the array owns and
			 * which outlives the table. */
			g_hash_table_insert(by_month, &cohort->index, cohort);
		}

		cohort->size++;
		customers++;

		for (k = 0; k < HEADLINE_COHORT_MONTHS; k++)
			seen[k] = FALSE;

		for (j = 0; j < history->len; j++)
		{
			g_autoptr(GDateTime) local = NULL;
			HeadlineCash *movement = g_ptr_array_index(history, j);
			gint index;
			gint spread;

			if (g_date_time_compare(movement->date, anchor) >= 0)
				break;

			if (venture_money_get_amount(movement->amount) <= 0)
				continue;

			local = g_date_time_to_timezone(movement->date, zone);
			index = ((g_date_time_get_year(local) * 12) +
			         (g_date_time_get_month(local) - 1)) - key;
			spread = headline_cadence_months(snapshot, movement->invoice_id);

			for (k = MAX(index, 0); (k < index + spread) &&
			     (k < HEADLINE_COHORT_MONTHS); k++)
				seen[k] = TRUE;
		}

		for (k = 0; k < HEADLINE_COHORT_MONTHS; k++)
			if (seen[k])
				cohort->active[k]++;
	}

	g_ptr_array_sort(cohorts, headline_cohort_compare);
	result = venture_report_result_new("Customer cohorts", period);

	venture_report_result_add_column(result, "cohort", "Cohort",
	                                 VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "customers", "Customers",
	                                 VENTURE_REPORT_COLUMN_NUMBER);

	for (i = 0; i < HEADLINE_COHORT_MONTHS; i++)
	{
		g_autofree gchar *key = g_strdup_printf("m%u", i);
		g_autofree gchar *label = g_strdup_printf("Month %u", i);

		venture_report_result_add_column(result, key, label,
		                                 VENTURE_REPORT_COLUMN_PERCENT);
	}

	for (i = 0; i < cohorts->len; i++)
	{
		g_autofree gchar *label = NULL;
		HeadlineCohort *cohort = g_ptr_array_index(cohorts, i);
		gint k;

		label = g_strdup_printf("%04d-%02d", cohort->year, cohort->month);
		venture_report_result_begin_row(result);
		venture_report_result_set_text(result, "cohort", label);
		venture_report_result_set_number(result, "customers",
		                                 (gdouble)cohort->size);

		for (k = 0; k < HEADLINE_COHORT_MONTHS; k++)
		{
			g_autoptr(GDateTime) opens = NULL;
			g_autoptr(GDateTime) shifted = NULL;
			g_autofree gchar *column = g_strdup_printf("m%d", k);

			opens = g_date_time_new(zone, cohort->year, cohort->month, 1, 0, 0, 0.0);
			shifted = (NULL != opens) ? g_date_time_add_months(opens, k) : NULL;

			/* A month that has not started is blank, not zero. */
			if ((NULL == shifted) || (g_date_time_compare(shifted, anchor) >= 0))
				continue;

			venture_report_result_set_number(result, column,
				headline_render_bps(headline_basis_points(cohort->active[k],
				                                          cohort->size)));
		}
	}

	venture_report_result_add_metric(result,
		venture_metric_new_count("cohorts", "Cohorts", (gint64)cohorts->len));
	venture_report_result_add_metric(result,
		venture_metric_new_count("customers", "New customers", customers));

	for (i = 0; i < G_N_ELEMENTS(milestones); i++)
	{
		g_autofree gchar *key = g_strdup_printf("month_%d", milestones[i]);
		g_autofree gchar *label = g_strdup_printf("Still paying in month %d",
		                                          milestones[i]);
		gint64 reached = 0;
		gint64 active = 0;
		guint j;

		for (j = 0; j < cohorts->len; j++)
		{
			g_autoptr(GDateTime) opens = NULL;
			g_autoptr(GDateTime) shifted = NULL;
			HeadlineCohort *cohort = g_ptr_array_index(cohorts, j);

			opens = g_date_time_new(zone, cohort->year, cohort->month, 1, 0, 0, 0.0);
			shifted = (NULL != opens)
				? g_date_time_add_months(opens, milestones[i]) : NULL;

			if ((NULL == shifted) || (g_date_time_compare(shifted, anchor) >= 0))
				continue;

			reached += cohort->size;
			active += cohort->active[milestones[i]];
		}

		if (reached > 0)
			venture_report_result_add_metric(result,
				venture_metric_new_ratio(key, label,
					headline_render_bps(headline_basis_points(active, reached))));
		else
			venture_report_result_add_metric(result,
				venture_metric_new_text(key, label, "n/a"));
	}

	venture_report_result_append_note(result,
		"A cohort is the companies whose first cash receipt fell in one "
		"calendar month of the period. Month k is the share of them with a "
		"receipt covering the k-th calendar month after it -- received in "
		"it, or on a yearly cadence in the eleven months before -- followed "
		"up to now, or as_of. Months that have not started yet are blank.");

	return g_steal_pointer(&result);
}

/* --- Registration --------------------------------------------------------- */

void
venture_headline_register_reports(VentureReportRegistry *registry)
{
	g_return_if_fail(VENTURE_IS_REPORT_REGISTRY(registry));

	venture_report_registry_add(registry, VENTURE_REPORT(venture_func_report_new(
		"cac", "Customer acquisition cost",
		"Acquisition spend -- acquisition-flagged expense and bill lines, and "
		"campaign spend pro-rated over the campaign -- over companies whose "
		"first cash receipt fell in the period, broken down by the converted "
		"lead's campaign and source",
		venture_report_cac)));
	venture_report_registry_add(registry, VENTURE_REPORT(venture_func_report_new(
		"customer_churn", "Customer churn (activity and recurring schedules)",
		"Recurring churn: customers whose invoice schedules and subscriptions "
		"all stopped in the period, over those with one running at its start. "
		"Monthly activity churn: customers gone quiet in the trailing twelve "
		"months over customer-months at risk (days, default 90). Both list "
		"the customers behind the number",
		venture_report_churn)));
	venture_report_registry_add(registry, VENTURE_REPORT(venture_func_report_new(
		"ltv", "Customer lifetime value",
		"Realised: cash per customer net of refunds, with mean, median and "
		"the top ten. Projected: revenue per customer-month x gross margin / "
		"monthly activity churn, withheld below a minimum of customer-months",
		venture_report_ltv)));
	venture_report_registry_add(registry, VENTURE_REPORT(venture_func_report_new(
		"customer_cohorts", "Customer cohorts",
		"Companies grouped by the month of their first cash receipt, and the "
		"share of each still paying in each of the twelve months after",
		venture_report_cohorts)));
	venture_report_registry_add(registry, VENTURE_REPORT(venture_func_report_new(
		"ltv_cac", "LTV to CAC",
		"Projected LTV over CAC for the same period, with ARPA and CAC "
		"payback; n/a when either side is",
		venture_report_ltv_cac)));
}
