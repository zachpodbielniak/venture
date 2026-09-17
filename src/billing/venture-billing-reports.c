/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
#include "billing/venture-billing-reports.h"

static gint64
id_field(VentureEntity *e, const gchar *field)
{
	gint64 id;
	g_object_get(e, field, &id, NULL);
	return id;
}

static gboolean
add(VentureMoney **total, const VentureMoney *amount, GError **error)
{
	VentureMoney *next = venture_money_add(*total, amount, error);
	if (next == NULL)
		return FALSE;
	venture_money_free(*total);
	*total = next;
	return TRUE;
}

static gint64
organization(VentureContext *context, JsonObject *options)
{
	return options != NULL && json_object_has_member(options, "organization_id") ?
		venture_json_object_get_int(options, "organization_id", 0) : venture_context_get_default_organization_id(context);
}

static const gchar *
currency(JsonObject *options)
{
	return options != NULL ? venture_json_object_get_string(options, "currency", venture_money_get_default_currency()) : venture_money_get_default_currency();
}

static GDateTime *
cutoff(VentureDateRange *period, JsonObject *options, GError **error)
{
	g_autoptr(GDateTime) end = period != NULL && venture_date_range_get_end(period) != NULL ?
		g_date_time_ref(venture_date_range_get_end(period)) : venture_time_now();
	if (options != NULL && json_object_has_member(options, "as_of"))
	{
		g_autoptr(GDateTime) asof = venture_period_report_as_of(options, error);
		g_autoptr(GDateTime) exclusive = NULL;
		if (asof == NULL)
			return NULL;
		exclusive = g_date_time_add(asof, 1);
		if (g_date_time_compare(exclusive, end) < 0)
			return g_steal_pointer(&exclusive);
	}
	return g_steal_pointer(&end);
}

static GPtrArray *
events(VentureContext *context, JsonObject *options, GDateTime *end, GError **error)
{
	g_autoptr(VentureQuery) q = venture_query_new(VENTURE_TYPE_SUBSCRIPTION_EVENT);
	g_autofree gchar *text = g_date_time_format_iso8601(end);
	venture_query_set_organization(q, organization(context, options));
	venture_query_set_limit(q, 0);
	venture_query_set_include_deleted(q, TRUE);
	if (!venture_query_add_filter_string(q, "at", VENTURE_FILTER_OP_LT, text, error) ||
		!venture_query_add_order(q, "at", VENTURE_SORT_ASCENDING, error) ||
		!venture_query_add_order(q, "id", VENTURE_SORT_ASCENDING, error))
		return NULL;
	return venture_database_find(venture_context_get_database(context), q, error);
}

static void
remember(GHashTable *table, gint64 id, VentureEntity *event)
{
	gint64 *key = g_new(gint64, 1);
	*key = id;
	g_hash_table_replace(table, key, event);
}

static gboolean
company_totals(VentureContext *context, GHashTable *latest, GHashTable *totals, const gchar *code, GError **error);

static VentureReportResult *
recurring(VentureContext *context, VentureDateRange *period, JsonObject *options, GError **error)
{
	static const gchar *const keys[] = { "new", "expansion", "contraction", "churn", "reactivation" };
	g_autoptr(VentureReportResult) result = venture_report_result_new("Monthly recurring revenue", period);
	g_autoptr(GDateTime) end = cutoff(period, options, error);
	g_autoptr(GPtrArray) history = NULL;
	g_autoptr(GHashTable) latest = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	g_autoptr(GHashTable) seen = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	g_autoptr(GHashTable) plans = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, (GDestroyNotify)venture_money_free);
	g_autoptr(GPtrArray) movements = g_ptr_array_new_with_free_func((GDestroyNotify)venture_money_free);
	g_autoptr(VentureMoney) total = venture_money_new_zero(currency(options));
	g_autoptr(VentureMoney) arr = NULL;
	GHashTableIter iter;
	gpointer key;
	gpointer value;
	GDateTime *start = period != NULL ? venture_date_range_get_start(period) : NULL;
	guint i;
	if (end == NULL)
		return NULL;
	history = events(context, options, end, error);
	if (history == NULL)
		return NULL;
	for (i = 0; i < G_N_ELEMENTS(keys); i++)
		g_ptr_array_add(movements, venture_money_new_zero(currency(options)));
	for (i = 0; i < history->len; i++)
	{
		VentureEntity *event = g_ptr_array_index(history, i);
		g_autoptr(VentureMoney) from = NULL;
		g_autoptr(VentureMoney) to = NULL;
		g_autoptr(VentureMoney) delta = NULL;
		g_autoptr(VentureMoney) positive = NULL;
		g_autoptr(GDateTime) at = NULL;
		gint64 id = id_field(event, "subscription-id");
		gint bucket = -1;
		g_object_get(event, "from-mrr", &from, "to-mrr", &to, "at", &at, NULL);
		if (from == NULL || to == NULL || g_strcmp0(venture_money_get_currency(to), currency(options)) != 0)
			continue;
		remember(latest, id, event);
		if (start == NULL || g_date_time_compare(at, start) >= 0)
		{
			delta = venture_money_subtract(to, from, error);
			if (delta == NULL)
				return NULL;
			if (venture_money_get_amount(from) == 0 && venture_money_get_amount(to) > 0)
				bucket = g_hash_table_contains(seen, &id) ? 4 : 0;
			else if (venture_money_get_amount(to) == 0 && venture_money_get_amount(from) > 0)
				bucket = 3;
			else if (venture_money_get_amount(delta) > 0)
				bucket = 1;
			else if (venture_money_get_amount(delta) < 0)
				bucket = 2;
			if (bucket >= 0)
			{
				VentureMoney *movement = g_ptr_array_index(movements, bucket);
				positive = venture_money_multiply_rational(delta, venture_money_get_amount(delta) < 0 ? -1 : 1, 1, error);
				if (positive == NULL || !add(&movement, positive, error))
					return NULL;
				g_ptr_array_index(movements, bucket) = movement;
			}
		}
		if (venture_money_get_amount(to) > 0)
			remember(seen, id, event);
	}
	g_hash_table_iter_init(&iter, latest);
	while (g_hash_table_iter_next(&iter, &key, &value))
	{
		VentureEntity *event = value;
		g_autoptr(VentureEntity) price = NULL;
		g_autoptr(VentureMoney) amount = NULL;
		VentureMoney *sum;
		gint64 plan_id;
		gint64 *owned;
		price = venture_database_get(venture_context_get_database(context), VENTURE_TYPE_PLAN_PRICE, id_field(event, "to-plan-price-id"), error);
		if (price == NULL)
			return NULL;
		plan_id = id_field(price, "plan-id");
		g_object_get(event, "to-mrr", &amount, NULL);
		if (!add(&total, amount, error))
			return NULL;
		sum = g_hash_table_lookup(plans, &plan_id);
		if (sum != NULL)
			sum = venture_money_add(sum, amount, error);
		else
			sum = venture_money_copy(amount);
		if (sum == NULL)
			return NULL;
		owned = g_new(gint64, 1);
		*owned = plan_id;
		g_hash_table_replace(plans, owned, sum);
	}
	venture_report_result_add_column(result, "plan", "Plan", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "mrr", "MRR", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "arr", "ARR", VENTURE_REPORT_COLUMN_MONEY);
	g_hash_table_iter_init(&iter, plans);
	while (g_hash_table_iter_next(&iter, &key, &value))
	{
		g_autoptr(VentureEntity) plan = venture_database_get(venture_context_get_database(context), VENTURE_TYPE_PLAN, *(gint64 *)key, error);
		g_autofree gchar *name = NULL;
		g_autoptr(VentureMoney) annual = venture_money_multiply_rational(value, 12, 1, error);
		if (plan == NULL || annual == NULL)
			return NULL;
		g_object_get(plan, "name", &name, NULL);
		venture_report_result_begin_row(result);
		venture_report_result_set_text(result, "plan", name);
		venture_report_result_set_money(result, "mrr", value);
		venture_report_result_set_money(result, "arr", annual);
	}
	arr = venture_money_multiply_rational(total, 12, 1, error);
	if (arr == NULL)
		return NULL;
	venture_report_result_add_metric(result, venture_metric_new_money("mrr", "MRR", total));
	venture_report_result_add_metric(result, venture_metric_new_money("arr", "ARR", arr));
	for (i = 0; i < G_N_ELEMENTS(keys); i++)
		venture_report_result_add_metric(result, venture_metric_new_money(keys[i], keys[i], g_ptr_array_index(movements, i)));
	/* Paying customers are companies, not subscriptions: two seats on two
	 * plans are one account, and ARPA is revenue per account. */
	{
		g_autoptr(GHashTable) companies = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, (GDestroyNotify)venture_money_free);
		gint64 customers = 0;
		if (!company_totals(context, latest, companies, currency(options), error))
			return NULL;
		g_hash_table_iter_init(&iter, companies);
		while (g_hash_table_iter_next(&iter, &key, &value))
			if (venture_money_get_amount(value) > 0)
				customers++;
		venture_report_result_add_metric(result, venture_metric_new_count("customers", "Paying customers", customers));
		if (customers > 0)
		{
			g_autoptr(VentureMoney) arpa = venture_money_multiply_rational(total, 1, customers, error);
			if (arpa == NULL)
				return NULL;
			venture_report_result_add_metric(result, venture_metric_new_money("arpa", "ARPA", arpa));
		}
	}
	/* Quick ratio: MRR gained over MRR lost within the period, in exact
	 * hundredths rounded half to even. No loss is no denominator, so no
	 * metric, the same rule as the churn rates. */
	{
		const VentureMoney *gained[] = { g_ptr_array_index(movements, 0), g_ptr_array_index(movements, 1), g_ptr_array_index(movements, 4) };
		const VentureMoney *lost[] = { g_ptr_array_index(movements, 2), g_ptr_array_index(movements, 3) };
		gint64 up = 0;
		gint64 down = 0;
		for (i = 0; i < G_N_ELEMENTS(gained); i++)
			up += venture_money_get_amount(gained[i]);
		for (i = 0; i < G_N_ELEMENTS(lost); i++)
			down += venture_money_get_amount(lost[i]);
		if (down > 0)
		{
			g_autoptr(VentureMoney) numerator = venture_money_new(up, "XXX", 0);
			g_autoptr(VentureMoney) hundredths = venture_money_multiply_rational(numerator, 100, down, error);
			if (hundredths == NULL)
				return NULL;
			venture_report_result_add_metric(result, venture_metric_new_number("quick_ratio", "Quick ratio",
				(gdouble)venture_money_get_amount(hundredths) / 100.0));
		}
	}
	venture_report_result_set_note(result, "MRR is contracted active or past_due recurring revenue immediately before the exclusive period end; trials, pauses and cancellations contribute zero. Annual terms are divided by 12 with half-even rounding; ARR is 12 times rounded MRR. Events freeze money at their effective date. New is the first positive MRR, reactivation a later return from zero, expansion/contraction changes between positive amounts, and churn a fall to zero (including pause). Movements are positive magnitudes within the period. Paying customers are companies whose combined MRR is positive at the endpoint; ARPA is MRR over them, half-even. Quick ratio is (new + expansion + reactivation) / (contraction + churn) within the period, to hundredths half-even, absent when nothing was lost. One organization and requested currency only; other currencies are excluded. This is neither cash nor recognized revenue.");
	return g_steal_pointer(&result);
}

static gboolean
company_totals(VentureContext *context, GHashTable *latest, GHashTable *totals, const gchar *code, GError **error)
{
	GHashTableIter iter;
	gpointer key;
	gpointer value;
	g_hash_table_iter_init(&iter, latest);
	while (g_hash_table_iter_next(&iter, &key, &value))
	{
		g_autoptr(VentureEntity) sub = venture_database_get(venture_context_get_database(context), VENTURE_TYPE_CUSTOMER_SUBSCRIPTION, *(gint64 *)key, error);
		g_autoptr(VentureMoney) amount = NULL;
		VentureMoney *sum;
		gint64 company;
		gint64 *owned;
		if (sub == NULL)
			return FALSE;
		g_object_get(value, "to-mrr", &amount, NULL);
		if (amount == NULL || g_strcmp0(venture_money_get_currency(amount), code) != 0)
			continue;
		company = id_field(sub, "company-id");
		sum = g_hash_table_lookup(totals, &company);
		sum = sum != NULL ? venture_money_add(sum, amount, error) : venture_money_copy(amount);
		if (sum == NULL)
			return FALSE;
		owned = g_new(gint64, 1);
		*owned = company;
		g_hash_table_replace(totals, owned, sum);
	}
	return TRUE;
}

static VentureReportResult *
churn(VentureContext *context, VentureDateRange *period, JsonObject *options, GError **error)
{
	g_autoptr(VentureReportResult) result = venture_report_result_new("Subscription churn", period);
	g_autoptr(GDateTime) end = cutoff(period, options, error);
	g_autoptr(GPtrArray) history = NULL;
	g_autoptr(GHashTable) opening = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	g_autoptr(GHashTable) closing = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	g_autoptr(GHashTable) companies_start = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, (GDestroyNotify)venture_money_free);
	g_autoptr(GHashTable) companies_end = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, (GDestroyNotify)venture_money_free);
	g_autoptr(VentureMoney) initial = venture_money_new_zero(currency(options));
	g_autoptr(VentureMoney) lost = venture_money_new_zero(currency(options));
	g_autoptr(VentureMoney) gained = venture_money_new_zero(currency(options));
	GDateTime *start = period != NULL ? venture_date_range_get_start(period) : NULL;
	GHashTableIter iter;
	gpointer key;
	gpointer value;
	gint64 logos = 0;
	gint64 lost_logos = 0;
	guint i;
	if (end == NULL)
		return NULL;
	history = events(context, options, end, error);
	if (history == NULL)
		return NULL;
	for (i = 0; i < history->len; i++)
	{
		VentureEntity *event = g_ptr_array_index(history, i);
		g_autoptr(GDateTime) at = NULL;
		g_object_get(event, "at", &at, NULL);
		remember(closing, id_field(event, "subscription-id"), event);
		if (start != NULL && g_date_time_compare(at, start) < 0)
			remember(opening, id_field(event, "subscription-id"), event);
	}
	if (!company_totals(context, opening, companies_start, currency(options), error) ||
		!company_totals(context, closing, companies_end, currency(options), error))
		return NULL;
	g_hash_table_iter_init(&iter, companies_start);
	while (g_hash_table_iter_next(&iter, &key, &value))
	{
		VentureMoney *close = g_hash_table_lookup(companies_end, key);
		g_autoptr(VentureMoney) zero = venture_money_new_zero(currency(options));
		g_autoptr(VentureMoney) loss = NULL;
		if (venture_money_get_amount(value) <= 0)
			continue;
		logos++;
		if (close == NULL || venture_money_is_zero(close))
			lost_logos++;
		loss = venture_money_subtract(value, close != NULL ? close : zero, error);
		if (loss == NULL || !add(&initial, value, error))
			return NULL;
		if (venture_money_get_amount(loss) > 0 && !add(&lost, loss, error))
			return NULL;
		/* Net retention also credits an opening company's growth; a
		 * company that arrived during the period is new business, not
		 * retention, so only the opening cohort's gains count. */
		if (venture_money_get_amount(loss) < 0)
		{
			g_autoptr(VentureMoney) gain = venture_money_negate(loss);
			if (gain == NULL || !add(&gained, gain, error))
				return NULL;
		}
	}
	venture_report_result_add_metric(result, venture_metric_new_count("opening_logos", "Opening customers", logos));
	venture_report_result_add_metric(result, venture_metric_new_count("lost_logos", "Lost customers", lost_logos));
	venture_report_result_add_metric(result, venture_metric_new_money("opening_mrr", "Opening MRR", initial));
	venture_report_result_add_metric(result, venture_metric_new_money("lost_mrr", "Lost MRR", lost));
	if (logos > 0)
	{
		g_autoptr(VentureMoney) numerator = venture_money_new(lost_logos, "XXX", 0);
		g_autoptr(VentureMoney) rate = venture_money_multiply_rational(numerator, 10000, logos, error);
		if (rate == NULL)
			return NULL;
		venture_report_result_add_metric(result, venture_metric_new_count("logo_churn_bps", "Logo churn (basis points)", venture_money_get_amount(rate)));
	}
	if (venture_money_get_amount(initial) > 0)
	{
		g_autoptr(VentureMoney) rate = venture_money_multiply_rational(lost, 10000, venture_money_get_amount(initial), error);
		if (rate == NULL)
			return NULL;
		venture_report_result_add_metric(result, venture_metric_new_count("revenue_churn_bps", "Revenue churn (basis points)", venture_money_get_amount(rate)));
		/* Gross retention is exactly what revenue churn did not take, so
		 * it is derived from the same rounded figure and the two always
		 * sum to 10,000. Net retention adds the cohort's expansion. */
		venture_report_result_add_metric(result, venture_metric_new_count("grr_bps", "Gross revenue retention (basis points)",
			10000 - venture_money_get_amount(rate)));
		{
			g_autoptr(VentureMoney) kept = venture_money_subtract(initial, lost, error);
			g_autoptr(VentureMoney) net = NULL;
			g_autoptr(VentureMoney) numerator = NULL;
			g_autoptr(VentureMoney) nrr = NULL;
			if (kept == NULL)
				return NULL;
			net = venture_money_add(kept, gained, error);
			if (net == NULL)
				return NULL;
			numerator = venture_money_new(venture_money_get_amount(net), "XXX", 0);
			nrr = venture_money_multiply_rational(numerator, 10000, venture_money_get_amount(initial), error);
			if (nrr == NULL)
				return NULL;
			venture_report_result_add_metric(result, venture_metric_new_count("nrr_bps", "Net revenue retention (basis points)",
				venture_money_get_amount(nrr)));
		}
	}
	venture_report_result_add_metric(result, venture_metric_new_money("expansion_mrr", "Opening cohort expansion", gained));
	venture_report_result_set_note(result, "Opening cohort is companies with positive contracted MRR before period start. Logo churn is lost_logos / opening_logos; lost logos have zero closing MRR across all subscriptions. Revenue churn is lost_mrr / opening_mrr, summing positive opening-minus-closing losses per opening company (including contractions, excluding new customers and gains). Gross revenue retention is 10,000 minus revenue churn in basis points; net revenue retention is (opening_mrr - lost_mrr + expansion_mrr) / opening_mrr, where expansion_mrr sums positive closing-minus-opening gains of opening companies only. Zero denominators mean undefined churn and retention. Both boundaries use dated events, one organization and currency. Pauses count as loss; past_due remains contracted MRR.");
	return g_steal_pointer(&result);
}

static VentureReportResult *
due(VentureContext *context, VentureDateRange *period, JsonObject *options, GError **error)
{
	g_autoptr(VentureReportResult) result = venture_report_result_new("Subscriptions due", period);
	g_autoptr(GDateTime) at = cutoff(period, options, error);
	g_autoptr(GDateTime) until = NULL;
	g_autoptr(VentureQuery) q = venture_query_new(VENTURE_TYPE_CUSTOMER_SUBSCRIPTION);
	g_autoptr(GPtrArray) subs = NULL;
	gint64 days = options != NULL ? venture_json_object_get_int(options, "days", 30) : 30;
	guint i;
	if (at == NULL)
		return NULL;
	if (days < 0 || days > 3660)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "days must be between 0 and 3660");
		return NULL;
	}
	until = g_date_time_add_days(at, (gint)days);
	venture_query_set_organization(q, organization(context, options));
	venture_query_set_limit(q, 0);
	subs = venture_database_find(venture_context_get_database(context), q, error);
	if (subs == NULL)
		return NULL;
	venture_report_result_add_column(result, "subscription", "Subscription", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "due", "Renewal date", VENTURE_REPORT_COLUMN_DATE);
	for (i = 0; i < subs->len; i++)
	{
		VentureEntity *sub = g_ptr_array_index(subs, i);
		g_autoptr(GDateTime) end = NULL;
		g_autofree gchar *label = NULL;
		g_autofree gchar *date = NULL;
		gint state;
		gboolean cancelled;
		g_object_get(sub, "status", &state, "current-period-end", &end, "cancel-at-period-end", &cancelled, NULL);
		if (cancelled || (state != 0 && state != 1) || end == NULL ||
			g_date_time_compare(end, at) < 0 || g_date_time_compare(end, until) > 0)
			continue;
		label = g_strdup_printf("customer_subscription/%" G_GINT64_FORMAT, venture_entity_get_id(sub));
		date = g_date_time_format(end, "%F");
		venture_report_result_begin_row(result);
		venture_report_result_set_text(result, "subscription", label);
		venture_report_result_set_text(result, "due", date);
	}
	venture_report_result_set_note(result, "Operational renewals from the period's exclusive end (or as_of cutoff) through the next days, default 30, inclusive. Uses current active/trialing subscription terms; scheduled cancellations are excluded. One organization only. This forward-looking queue does not reconstruct historical schedules.");
	return g_steal_pointer(&result);
}

static gboolean
options_valid(VentureContext *context, JsonObject *options, GError **error)
{
	g_autofree gchar *upper = g_ascii_strup(currency(options), -1);
	if (organization(context, options) <= 0 || !venture_currency_is_valid(currency(options)) ||
		g_strcmp0(upper, currency(options)) != 0)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"Billing reports require one positive organization ID and an uppercase currency code");
		return FALSE;
	}
	return TRUE;
}

static VentureReportResult *
recurring_checked(VentureContext *context, VentureDateRange *period, JsonObject *options, GError **error)
{
	return options_valid(context, options, error) ? recurring(context, period, options, error) : NULL;
}

static VentureReportResult *
churn_checked(VentureContext *context, VentureDateRange *period, JsonObject *options, GError **error)
{
	return options_valid(context, options, error) ? churn(context, period, options, error) : NULL;
}

static VentureReportResult *
due_checked(VentureContext *context, VentureDateRange *period, JsonObject *options, GError **error)
{
	return options_valid(context, options, error) ? due(context, period, options, error) : NULL;
}

void
venture_billing_register_reports(VentureReportRegistry *registry)
{
	venture_report_registry_add(registry, VENTURE_REPORT(venture_func_report_new("mrr", "Recurring revenue", "Contracted MRR, ARR, ARPA, dated movements and the quick ratio", recurring_checked)));
	/* Titled apart from the headline module's customer_churn, which counts
	 * paying customers going quiet: two reports both called "Customer
	 * churn" gave two different answers to what read as one question. */
	venture_report_registry_add(registry, VENTURE_REPORT(venture_func_report_new("churn", "Subscription churn (billing)", "Opening subscription cohort, logo and revenue churn, gross and net revenue retention", churn_checked)));
	venture_report_registry_add(registry, VENTURE_REPORT(venture_func_report_new("subscriptions_due", "Subscriptions due", "Renewals in the next days", due_checked)));
}
