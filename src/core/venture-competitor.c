/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

static gint64
number(VentureEntity *row, const gchar *field)
{
	gint64 n = 0;
	g_object_get(row, field, &n, NULL);
	return n;
}

static gboolean
live_reference(VentureDatabase *db, GType type, gint64 id, gint64 org, GError **error)
{
	g_autoptr(VentureEntity) row = venture_database_get(db, type, id, error);
	if (NULL == row)
		return FALSE;
	if (venture_entity_is_deleted(row) || venture_entity_get_organization_id(row) != org)
	{
		venture_set_error_validation(error, "competitor", "Choose a live record in this organization");
		return FALSE;
	}
	return TRUE;
}

static gboolean
validate(VentureDatabase *db, VentureEntity *row, VentureEntity *previous,
	gpointer unused, GError **error)
{
	gint64 org = venture_entity_get_organization_id(row);
	gint64 rival;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) others = NULL;
	guint i;
	(void)unused;
	(void)previous;
	if (venture_entity_is_deleted(row))
		return TRUE;
	if (VENTURE_IS_DEAL(row))
	{
		rival = number(row, "lost-to-competitor-id");
		return rival == 0 || live_reference(db, VENTURE_TYPE_COMPETITOR, rival, org, error);
	}
	rival = number(row, "competitor-id");
	if (!live_reference(db, VENTURE_TYPE_COMPETITOR, rival, org, error) ||
		!live_reference(db, VENTURE_TYPE_DEAL, number(row, "deal-id"), org, error))
		return FALSE;
	query = venture_query_new(VENTURE_TYPE_DEAL_COMPETITOR);
	venture_query_set_organization(query, org);
	venture_query_set_limit(query, 0);
	venture_query_add_filter_int(query, "deal-id", VENTURE_FILTER_OP_EQ, number(row, "deal-id"), NULL);
	venture_query_add_filter_int(query, "competitor-id", VENTURE_FILTER_OP_EQ, rival, NULL);
	others = venture_database_find(db, query, error);
	if (NULL == others)
		return FALSE;
	for (i = 0; i < others->len; i++)
	{
		if (venture_entity_get_id(g_ptr_array_index(others, i)) != venture_entity_get_id(row))
		{
			venture_set_error_validation(error, "competitor_id", "This competitor is already linked to this deal");
			return FALSE;
		}
	}
	return TRUE;
}

void
venture_competitor_install(VentureContext *context)
{
	VentureDatabase *db = venture_context_get_database(context);
	if (g_object_get_data(G_OBJECT(db), "venture-competitors-installed"))
		return;
	g_object_set_data(G_OBJECT(db), "venture-competitors-installed", GINT_TO_POINTER(1));
	venture_database_add_save_validator(db, VENTURE_TYPE_DEAL_COMPETITOR, validate, NULL, NULL);
	venture_database_add_save_validator(db, VENTURE_TYPE_DEAL, validate, NULL, NULL);
}

typedef struct
{
	gint64 faced, won, lost;
	VentureMoney *value;
} Totals;

static void
free_totals(gpointer data)
{
	Totals *totals = data;
	venture_money_free(totals->value);
	g_free(totals);
}

static Totals *
totals_for(GHashTable *currencies, const gchar *currency)
{
	Totals *totals = g_hash_table_lookup(currencies, currency);
	if (NULL == totals)
	{
		totals = g_new0(Totals, 1);
		totals->value = venture_money_new_zero(currency);
		g_hash_table_insert(currencies, g_strdup(currency), totals);
	}
	return totals;
}

static void
add_id(GHashTable *set, gint64 id)
{
	gint64 *key = g_new(gint64, 1);
	*key = id;
	g_hash_table_add(set, key);
}

/* Filter dates before applying the row bound, so narrowing a large
 * organization's reporting period really does reduce the work. */
static GPtrArray *
deals_in_stage(VentureDatabase *db, gint64 org, gint stage,
	VentureDateRange *period, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_DEAL);
	venture_query_set_organization(query, org);
	venture_query_set_limit(query, venture_aggregate_get_max_rows() + 1);
	venture_query_add_filter_int(query, "stage", VENTURE_FILTER_OP_EQ, stage, NULL);
	if (NULL != period && !venture_query_set_date_range(query,
		stage >= VENTURE_DEAL_STAGE_WON ? "closed-at" : "created-at", period, error))
		return NULL;
	return venture_database_find(db, query, error);
}

static VentureReportResult *
report(VentureContext *context, VentureDateRange *period, JsonObject *options, GError **error)
{
	VentureDatabase *db = venture_context_get_database(context);
	gint64 org = options ? venture_json_object_get_int(options, "organization_id", 0) : 0;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_COMPETITOR);
	g_autoptr(GPtrArray) competitors = NULL;
	g_autoptr(GHashTable) groups = g_hash_table_new_full(g_int64_hash, g_int64_equal,
		g_free, (GDestroyNotify)g_hash_table_unref);
	g_autoptr(VentureReportResult) result = venture_report_result_new("Competitor win/loss", period);
	g_autofree gchar *book = NULL;
	guint i, visited = 0;
	gint stage;
	if (org == 0)
		org = venture_context_get_default_organization_id(context);
	venture_query_set_organization(query, org);
	venture_query_set_limit(query, 0);
	venture_query_add_order(query, "name", VENTURE_SORT_ASCENDING, NULL);
	competitors = venture_database_find(db, query, error);
	if (NULL == competitors)
		return NULL;
	book = venture_posting_service_book_currency(venture_database_get_posting_service(db), org, error);
	if (NULL == book)
		return NULL;
	for (i = 0; i < competitors->len; i++)
	{
		gint64 *key = g_new(gint64, 1);
		*key = venture_entity_get_id(g_ptr_array_index(competitors, i));
		g_hash_table_insert(groups, key, g_hash_table_new_full(g_str_hash, g_str_equal, g_free, free_totals));
	}
	for (stage = VENTURE_DEAL_STAGE_LEAD; stage <= VENTURE_DEAL_STAGE_LOST; stage++)
	{
		g_autoptr(GPtrArray) deals = deals_in_stage(db, org, stage, period, error);
		if (NULL == deals)
			return NULL;
		visited += deals->len;
		if (visited > (guint)venture_aggregate_get_max_rows())
		{
			venture_set_error_validation(error, "period", "Too many deals; narrow the reporting period");
			return NULL;
		}
		for (i = 0; i < deals->len; i++)
		{
			VentureEntity *deal = g_ptr_array_index(deals, i);
			g_autoptr(VentureQuery) links_query = venture_query_new(VENTURE_TYPE_DEAL_COMPETITOR);
			g_autoptr(GPtrArray) links = NULL;
			g_autoptr(GHashTable) rivals = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
			g_autoptr(VentureMoney) value = NULL;
			gint64 winner = number(deal, "lost-to-competitor-id");
			GHashTableIter iter;
			gpointer key;
			guint j;
			venture_query_set_organization(links_query, org);
			venture_query_set_limit(links_query, 0);
			venture_query_add_filter_int(links_query, "deal-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(deal), NULL);
			links = venture_database_find(db, links_query, error);
			if (NULL == links)
				return NULL;
			for (j = 0; j < links->len; j++)
				add_id(rivals, number(g_ptr_array_index(links, j), "competitor-id"));
			/* An explicitly named winner is evidence even without a link. */
			if (stage == VENTURE_DEAL_STAGE_LOST && winner > 0)
				add_id(rivals, winner);
			g_object_get(deal, "value", &value, NULL);
			g_hash_table_iter_init(&iter, rivals);
			while (g_hash_table_iter_next(&iter, &key, NULL))
			{
				GHashTable *currencies = g_hash_table_lookup(groups, key);
				Totals *totals;
				if (NULL == currencies)
					continue;
				totals = totals_for(currencies, value ? venture_money_get_currency(value) : book);
				totals->faced++;
				if (stage == VENTURE_DEAL_STAGE_WON)
					totals->won++;
				if (stage == VENTURE_DEAL_STAGE_LOST && *(gint64 *)key == winner)
				{
					totals->lost++;
					if (value)
					{
						VentureMoney *sum = venture_money_add(totals->value, value, error);
						if (NULL == sum)
							return NULL;
						venture_money_free(totals->value);
						totals->value = sum;
					}
				}
			}
		}
	}
	venture_report_result_add_column(result, "competitor", "Competitor", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "currency", "Currency", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "faced", "Deals faced", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "won", "Won", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "lost", "Lost to", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "win_rate", "Win rate (%)", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "value_lost", "Value lost", VENTURE_REPORT_COLUMN_MONEY);
	for (i = 0; i < competitors->len; i++)
	{
		VentureEntity *competitor = g_ptr_array_index(competitors, i);
		gint64 id = venture_entity_get_id(competitor);
		GHashTable *currencies = g_hash_table_lookup(groups, &id);
		g_autofree gchar *name = venture_entity_get_display_name(competitor);
		GHashTableIter iter;
		gpointer currency, data;
		if (g_hash_table_size(currencies) == 0)
			totals_for(currencies, book);
		g_hash_table_iter_init(&iter, currencies);
		while (g_hash_table_iter_next(&iter, &currency, &data))
		{
			Totals *totals = data;
			venture_report_result_begin_row(result);
			venture_report_result_set_text(result, "competitor", name);
			venture_report_result_set_text(result, "currency", currency);
			venture_report_result_set_number(result, "faced", totals->faced);
			venture_report_result_set_number(result, "won", totals->won);
			venture_report_result_set_number(result, "lost", totals->lost);
			venture_report_result_set_number(result, "win_rate", totals->won + totals->lost > 0 ?
				100.0 * totals->won / (totals->won + totals->lost) : 0);
			venture_report_result_set_money(result, "value_lost", totals->value);
		}
	}
	return g_steal_pointer(&result);
}

void
venture_competitor_register_reports(VentureReportRegistry *registry)
{
	venture_report_registry_add(registry, VENTURE_REPORT(venture_func_report_new_classified(
		VENTURE_DATA_CLASS_TENANT, "competitor_win_loss", "Competitor win/loss",
		"Opportunities faced, won and lost to each competitor, by currency", report)));
}
