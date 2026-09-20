/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

typedef struct
{
	VentureMoney *cost;
	VentureMoney *accumulated;
} Category;

static void
category_free(gpointer data)
{
	Category *category = data;
	venture_money_free(category->cost);
	venture_money_free(category->accumulated);
	g_free(category);
}

static gboolean
add_amount(VentureMoney **total, const VentureMoney *amount, GError **error)
{
	VentureMoney *next = venture_money_add(*total, amount, error);
	if (next == NULL)
		return FALSE;
	venture_money_free(*total);
	*total = next;
	return TRUE;
}

static VentureMoney *
posted_amount(VentureDatabase *db, GType type, const gchar *field, gint64 id,
	gint64 org, GDateTime *cutoff, const VentureMoney *basis, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(type);
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(VentureMoney) total = venture_money_new(0, basis->currency, basis->exponent);
	guint i;
	venture_query_set_organization(query, org);
	venture_query_set_include_deleted(query, TRUE);
	venture_query_set_limit(query, 0);
	if (!venture_query_add_filter_int(query, field, VENTURE_FILTER_OP_EQ, id, error) ||
		!venture_query_add_filter_int(query, "state", VENTURE_FILTER_OP_EQ, VENTURE_SCHEDULE_STATE_POSTED, error))
		return NULL;
	rows = venture_database_find(db, query, error);
	if (rows == NULL)
		return NULL;
	for (i = 0; i < rows->len; i++)
	{
		g_autoptr(VentureEntity) journal = NULL;
		g_autoptr(GDateTime) date = NULL;
		g_autoptr(VentureMoney) amount = NULL;
		gint64 journal_id;
		g_object_get(g_ptr_array_index(rows, i), "journal-id", &journal_id, "amount", &amount, NULL);
		journal = venture_database_get(db, VENTURE_TYPE_JOURNAL, journal_id, error);
		if (journal == NULL)
			return NULL;
		g_object_get(journal, "occurred-at", &date, NULL);
		if (date != NULL && g_date_time_compare(date, cutoff) <= 0 && !add_amount(&total, amount, error))
			return NULL;
	}
	return g_steal_pointer(&total);
}

static gboolean
report_row(VentureReportResult *result, const gchar *label, const gchar *category,
	const VentureMoney *cost, const VentureMoney *accumulated, GError **error)
{
	g_autoptr(VentureMoney) balance = venture_money_subtract(cost, accumulated, error);
	if (balance == NULL)
		return FALSE;
	venture_report_result_begin_row(result);
	venture_report_result_set_text(result, "name", label);
	venture_report_result_set_text(result, "category", category);
	venture_report_result_set_money(result, "cost", cost);
	venture_report_result_set_money(result, "released", accumulated);
	venture_report_result_set_money(result, "balance", balance);
	return TRUE;
}

static VentureReportResult *
register_report(VentureContext *context, VentureDateRange *period, JsonObject *options,
	gboolean assets, GError **error)
{
	VentureDatabase *db = venture_context_get_database(context);
	g_autoptr(VentureReportResult) result = venture_report_result_new(assets ? "Fixed assets" : "Deferrals", period);
	g_autoptr(VentureQuery) query = venture_query_new(assets ? VENTURE_TYPE_FIXED_ASSET : VENTURE_TYPE_DEFERRAL);
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GDateTime) cutoff = NULL;
	g_autoptr(GHashTable) categories = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, category_free);
	gint64 org = options != NULL ? venture_json_object_get_int(options, "organization_id", 0) : 0;
	const gchar *currency = options != NULL ? venture_json_object_get_string(options, "currency", "") : "";
	guint i;
	if (org == 0)
		org = venture_context_get_default_organization_id(context);
	cutoff = options != NULL && json_object_has_member(options, "as_of") ? venture_period_report_as_of(options, error) : venture_time_now();
	if (cutoff == NULL)
		return NULL;
	if (period != NULL && venture_date_range_get_end(period) != NULL &&
		g_date_time_compare(cutoff, venture_date_range_get_end(period)) >= 0)
	{
		g_clear_pointer(&cutoff, g_date_time_unref);
		cutoff = g_date_time_add(venture_date_range_get_end(period), -1);
	}
	venture_query_set_organization(query, org);
	venture_query_set_include_deleted(query, TRUE);
	venture_query_set_limit(query, 0);
	rows = venture_database_find(db, query, error);
	if (rows == NULL)
		return NULL;
	venture_report_result_add_column(result, "name", assets ? "Asset" : "Deferral", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "category", "Category", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "cost", assets ? "Cost" : "Total", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "released", assets ? "Accumulated depreciation" : "Released", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "balance", assets ? "Net book value" : "Remaining", VENTURE_REPORT_COLUMN_MONEY);
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(rows, i);
		g_autoptr(VentureMoney) cost = NULL;
		g_autoptr(VentureMoney) released = NULL;
		g_autoptr(GDateTime) acquired = NULL;
		g_autoptr(GDateTime) disposed = NULL;
		g_autofree gchar *label = NULL;
		g_autofree gchar *category = NULL;
		g_autofree gchar *key = NULL;
		Category *aggregate;
		/* Drafts may be discarded before placement; retained uniqueness rows
		 * must not contribute assets that the operator explicitly removed. */
		if (assets && venture_entity_is_deleted(row))
		{
			gint state;
			g_object_get(row, "status", &state, NULL);
			if (state == VENTURE_ASSET_STATUS_DRAFT)
				continue;
		}
		g_object_get(row, assets ? "cost" : "total", &cost, assets ? "acquired-at" : "start", &acquired,
			assets ? "name" : "description", &label, NULL);
		if (cost == NULL || acquired == NULL || g_date_time_compare(acquired, cutoff) > 0 ||
			(!venture_string_is_empty(currency) && !g_str_equal(currency, cost->currency)))
			continue;
		if (assets)
			g_object_get(row, "category", &category, "disposed-at", &disposed, NULL);
		released = posted_amount(db, assets ? VENTURE_TYPE_DEPRECIATION_ENTRY : VENTURE_TYPE_DEFERRAL_ENTRY,
			assets ? "asset-id" : "deferral-id", venture_entity_get_id(row), org, cutoff, cost, error);
		if (released == NULL)
			return NULL;
		if (disposed != NULL && g_date_time_compare(disposed, cutoff) <= 0)
		{
			cost->amount = 0;
			released->amount = 0;
		}
		if (!report_row(result, label, category != NULL ? category : "", cost, released, error))
			return NULL;
		if (!assets)
			continue;
		key = g_strdup_printf("%s [%s]", category != NULL ? category : "Uncategorized", cost->currency);
		aggregate = g_hash_table_lookup(categories, key);
		if (aggregate == NULL)
		{
			aggregate = g_new0(Category, 1);
			aggregate->cost = venture_money_new(0, cost->currency, cost->exponent);
			aggregate->accumulated = venture_money_new(0, cost->currency, cost->exponent);
			g_hash_table_insert(categories, g_strdup(key), aggregate);
		}
		if (!add_amount(&aggregate->cost, cost, error) || !add_amount(&aggregate->accumulated, released, error))
			return NULL;
	}
	if (assets)
	{
		GHashTableIter iter;
		gpointer key, value;
		g_hash_table_iter_init(&iter, categories);
		while (g_hash_table_iter_next(&iter, &key, &value))
		{
			Category *category = value;
			if (!report_row(result, "Category total", key, category->cost, category->accumulated, error))
				return NULL;
		}
	}
	venture_report_result_append_note(result, "Book accounting only. Currencies remain separate. Balances use posted journal dates; disposal removes carrying value on its effective date.");
	return g_steal_pointer(&result);
}

static VentureReportResult *
assets_report(VentureContext *context, VentureDateRange *period, JsonObject *options, GError **error)
{
	return register_report(context, period, options, TRUE, error);
}

static VentureReportResult *
deferrals_report(VentureContext *context, VentureDateRange *period, JsonObject *options, GError **error)
{
	return register_report(context, period, options, FALSE, error);
}

void
venture_assets_register_reports(VentureReportRegistry *registry)
{
	VentureReport *assets = VENTURE_REPORT(venture_func_report_new_classified(VENTURE_DATA_CLASS_TENANT, "fixed_assets", "Fixed assets", "Historical asset and category carrying values.", assets_report));
	VentureReport *deferrals = VENTURE_REPORT(venture_func_report_new_classified(VENTURE_DATA_CLASS_TENANT, "deferrals", "Deferrals", "Historical remaining deferral balances.", deferrals_report));
	g_object_set(assets, "financial", TRUE, NULL);
	g_object_set(deferrals, "financial", TRUE, NULL);
	venture_report_registry_add(registry, assets);
	venture_report_registry_add(registry, deferrals);
}
