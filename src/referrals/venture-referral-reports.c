/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

/*
 * Referrals made in the period, by who made them. Conversion is the share
 * of a referrer's referrals that became customers. A reward is counted
 * with the referral it was for, so the period of a row is the period the
 * customer was sent in, not the day the referrer was paid.
 *
 * A referrer rewarded in two currencies gets a row per currency, with the
 * referral counts repeated on each: the counts belong to the referrer and
 * the money to the currency, and nothing here adds across two.
 */

typedef struct {
	gint64 company;
	gint64 contact;
	gint64 counts[4];
	gint64 rewards;
	GPtrArray *values;
} Referrer;

static void
referrer_free(gpointer data)
{
	Referrer *referrer = data;
	g_ptr_array_unref(referrer->values);
	g_free(referrer);
}

static gint64
number(VentureEntity *e, const gchar *name)
{
	gint64 value = 0;
	g_object_get(e, name, &value, NULL);
	return value;
}

static gint
choice(VentureEntity *e, const gchar *name)
{
	gint value = 0;
	g_object_get(e, name, &value, NULL);
	return value;
}

static GPtrArray *
bounded(VentureDatabase *database, VentureQuery *query, const gchar *what, GError **error)
{
	g_autoptr(GPtrArray) rows = NULL;
	venture_query_set_limit(query, (guint)venture_aggregate_get_max_rows() + 1);
	if (!venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, error))
		return NULL;
	rows = venture_database_find(database, query, error);
	if (rows == NULL)
		return NULL;
	/* Never total a truncated set. */
	if (rows->len > (guint)venture_aggregate_get_max_rows())
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			"More than %d %s match; choose a shorter period", venture_aggregate_get_max_rows(), what);
		return NULL;
	}
	return g_steal_pointer(&rows);
}

static gboolean
within(VentureQuery *query, VentureDateRange *period, GError **error)
{
	GDateTime *start = period != NULL ? venture_date_range_get_start(period) : NULL;
	GDateTime *end = period != NULL ? venture_date_range_get_end(period) : NULL;
	if (start != NULL)
	{
		g_autofree gchar *bound = venture_time_to_string(start);
		if (!venture_query_add_filter_string(query, "created-at", VENTURE_FILTER_OP_GTE, bound, error))
			return FALSE;
	}
	if (end != NULL)
	{
		g_autofree gchar *bound = venture_time_to_string(end);
		if (!venture_query_add_filter_string(query, "created-at", VENTURE_FILTER_OP_LT, bound, error))
			return FALSE;
	}
	return TRUE;
}

static gchar *
referrer_name(VentureDatabase *database, Referrer *referrer)
{
	g_autoptr(VentureEntity) row = NULL;
	if (referrer->company != 0)
		row = venture_database_get(database, VENTURE_TYPE_COMPANY, referrer->company, NULL);
	else
		row = venture_database_get(database, VENTURE_TYPE_CONTACT, referrer->contact, NULL);
	return row != NULL ? venture_entity_get_display_name(row) : g_strdup("Unknown referrer");
}

static VentureReportResult *
referrals_report(VentureContext *context, VentureDateRange *period, JsonObject *options, GError **error)
{
	VentureDatabase *database = venture_context_get_database(context);
	gint64 org = options != NULL ? venture_json_object_get_int(options, "organization_id", 0) : 0;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureQuery) paid = NULL;
	g_autoptr(GPtrArray) referrals = NULL;
	g_autoptr(GPtrArray) rewards = NULL;
	g_autoptr(GPtrArray) referrers = g_ptr_array_new_with_free_func(referrer_free);
	g_autoptr(GHashTable) by_key = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	g_autoptr(GHashTable) by_referral = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	g_autoptr(GPtrArray) all_values = venture_money_totals_new();
	g_autoptr(VentureReportResult) result = venture_report_result_new("Referrals", period);
	g_autofree gchar *book = NULL;
	gint64 total = 0, won = 0, given = 0;
	guint i, j;
	if (org == 0)
		org = venture_context_get_default_organization_id(context);
	book = venture_database_get_book_currency(database, org);
	query = venture_query_new(VENTURE_TYPE_REFERRAL);
	venture_query_set_organization(query, org);
	if (!within(query, period, error))
		return NULL;
	referrals = bounded(database, query, "referrals", error);
	if (referrals == NULL)
		return NULL;
	for (i = 0; i < referrals->len; i++)
	{
		VentureEntity *referral = g_ptr_array_index(referrals, i);
		gint64 company = number(referral, "referrer-company-id");
		gint64 contact = company != 0 ? 0 : number(referral, "referrer-contact-id");
		g_autofree gchar *key = g_strdup_printf("%" G_GINT64_FORMAT ":%" G_GINT64_FORMAT, company, contact);
		Referrer *referrer = g_hash_table_lookup(by_key, key);
		gint status = choice(referral, "status");
		if (referrer == NULL)
		{
			referrer = g_new0(Referrer, 1);
			referrer->company = company;
			referrer->contact = contact;
			referrer->values = venture_money_totals_new();
			g_ptr_array_add(referrers, referrer);
			g_hash_table_insert(by_key, g_steal_pointer(&key), referrer);
		}
		if (status >= 0 && status < (gint)G_N_ELEMENTS(referrer->counts))
			referrer->counts[status]++;
		total++;
		if (status == VENTURE_REFERRAL_WON)
			won++;
		{
			gint64 id = venture_entity_get_id(referral);
			g_hash_table_insert(by_referral, g_memdup2(&id, sizeof id), referrer);
		}
	}
	paid = venture_query_new(VENTURE_TYPE_REFERRAL_REWARD);
	venture_query_set_organization(paid, org);
	if (!venture_query_add_filter_string(paid, "status", VENTURE_FILTER_OP_EQ, "applied", error))
		return NULL;
	rewards = bounded(database, paid, "rewards", error);
	if (rewards == NULL)
		return NULL;
	for (i = 0; i < rewards->len; i++)
	{
		VentureEntity *reward = g_ptr_array_index(rewards, i);
		gint64 referral_id = number(reward, "referral-id");
		Referrer *referrer = g_hash_table_lookup(by_referral, &referral_id);
		g_autoptr(VentureMoney) amount = NULL;
		if (referrer == NULL)
			continue;
		g_object_get(reward, "amount", &amount, NULL);
		referrer->rewards++;
		given++;
		if (!venture_money_totals_add(referrer->values, amount, error) ||
		    !venture_money_totals_add(all_values, amount, error))
			return NULL;
	}
	venture_report_result_add_column(result, "referrer", "Referrer", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "referrals", "Referrals", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "pending", "Pending", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "qualified", "Qualified", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "won", "Won", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "lost", "Lost", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "conversion_rate", "Conversion", VENTURE_REPORT_COLUMN_PERCENT);
	venture_report_result_add_column(result, "rewards", "Rewards given", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "reward_value", "Reward value", VENTURE_REPORT_COLUMN_MONEY);
	for (i = 0; i < referrers->len; i++)
	{
		Referrer *referrer = g_ptr_array_index(referrers, i);
		g_autofree gchar *name = referrer_name(database, referrer);
		gint64 count = referrer->counts[0] + referrer->counts[1] + referrer->counts[2] + referrer->counts[3];
		guint rows = MAX(referrer->values->len, 1);
		venture_money_totals_sort(referrer->values, book);
		for (j = 0; j < rows; j++)
		{
			venture_report_result_begin_row(result);
			venture_report_result_set_text(result, "referrer", name);
			venture_report_result_set_number(result, "referrals", count);
			venture_report_result_set_number(result, "pending", referrer->counts[VENTURE_REFERRAL_PENDING]);
			venture_report_result_set_number(result, "qualified", referrer->counts[VENTURE_REFERRAL_QUALIFIED]);
			venture_report_result_set_number(result, "won", referrer->counts[VENTURE_REFERRAL_WON]);
			venture_report_result_set_number(result, "lost", referrer->counts[VENTURE_REFERRAL_LOST]);
			venture_report_result_set_number(result, "conversion_rate", 100.0 * referrer->counts[VENTURE_REFERRAL_WON] / count);
			venture_report_result_set_number(result, "rewards", referrer->rewards);
			if (j < referrer->values->len)
				venture_report_result_set_money(result, "reward_value", g_ptr_array_index(referrer->values, j));
		}
	}
	venture_report_result_add_metric(result, venture_metric_new_count("referrals", "Referrals", total));
	venture_report_result_add_metric(result, venture_metric_new_count("won", "Customers won", won));
	if (total > 0)
		venture_report_result_add_metric(result, venture_metric_new_ratio("conversion_rate", "Conversion", (gdouble)won / total));
	venture_report_result_add_metric(result, venture_metric_new_count("rewards", "Rewards given", given));
	/* The book currency keeps the plain key; every other one says its code. */
	venture_money_totals_sort(all_values, book);
	for (i = 0; i < all_values->len; i++)
	{
		const VentureMoney *value = g_ptr_array_index(all_values, i);
		const gchar *currency = venture_money_get_currency(value);
		g_autofree gchar *key = g_strcmp0(currency, book) == 0 ? g_strdup("reward_value")
			: g_strdup_printf("reward_value_%s", currency);
		g_autofree gchar *label = g_strdup_printf("Reward value (%s)", currency);
		venture_report_result_add_metric(result, venture_metric_new_money(key, label, value));
	}
	return g_steal_pointer(&result);
}

void
venture_referrals_register_reports(VentureReportRegistry *registry)
{
	venture_report_registry_add(registry, VENTURE_REPORT(venture_func_report_new_classified(VENTURE_DATA_CLASS_TENANT,
		"referrals", "Referrals",
		"Referrals made in the period by referrer: how many became customers and the rewards given for them",
		referrals_report)));
}
