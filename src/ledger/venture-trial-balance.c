/*
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "venture.h"
#include "ledger/venture-ledger-private.h"

static gboolean
accumulate(VentureMoney **sum, const VentureMoney *amount, GError **error)
{
	VentureMoney *total = venture_money_add(*sum, amount, error);

	if (NULL == total)
		return FALSE;
	venture_money_free(*sum);
	*sum = total;
	return TRUE;
}

static VentureReportResult *
trial_balance(VentureContext *context, VentureDateRange *period, JsonObject *options, GError **error)
{
	VentureDatabase *db = venture_context_get_database(context);
	VenturePostingService *service = venture_context_get_posting_service(context);
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) journals = NULL;
	g_autoptr(GPtrArray) accounts = NULL;
	g_autoptr(GHashTable) currencies = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(GDateTime) as_of = NULL;
	g_autoptr(GList) names = NULL;
	GList *currency_node;
	const gchar *requested_currency;
	gint64 org;
	guint i;

	if (NULL == service)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED, "The ledger module is disabled");
		return NULL;
	}
	org = options != NULL ? venture_json_object_get_int(options, "organization_id", 0) : 0;
	if (org == 0)
		org = venture_context_get_default_organization_id(context);
	requested_currency = options != NULL ? venture_json_object_get_string(options, "currency", NULL) : NULL;
	as_of = period != NULL ? g_date_time_add(venture_date_range_get_end(period), -1) : venture_time_now();
	if (!venture_database_begin(db, error))
		return NULL;
	query = venture_query_new(VENTURE_TYPE_JOURNAL);
	venture_query_set_limit(query, 0);
	venture_query_set_organization(query, org);
	venture_query_set_include_deleted(query, TRUE);
	journals = venture_database_find(db, query, error);
	if (NULL == journals)
		goto fail;
	for (i = 0; i < journals->len; i++)
	{
		g_autofree gchar *currency = NULL;
		g_autoptr(GDateTime) when = NULL;
		VentureJournalState state;

		g_object_get(g_ptr_array_index(journals, i), "currency", &currency,
			"state", &state, "occurred-at", &when, NULL);
		if (state == VENTURE_JOURNAL_DRAFT || NULL == when ||
			g_date_time_compare(when, as_of) > 0 || NULL == currency)
			continue;
		if (NULL == requested_currency || g_str_equal(requested_currency, currency))
			g_hash_table_add(currencies, g_steal_pointer(&currency));
	}
	if (NULL != requested_currency)
		g_hash_table_add(currencies, g_strdup(requested_currency));
	g_clear_object(&query);
	query = venture_query_new(VENTURE_TYPE_ACCOUNT);
	venture_query_set_limit(query, 0);
	venture_query_set_include_deleted(query, TRUE);
	venture_query_set_organization(query, org);
	venture_query_add_order(query, "code", VENTURE_SORT_ASCENDING, NULL);
	accounts = venture_database_find(db, query, error);
	if (NULL == accounts)
		goto fail;
	result = venture_report_result_new("Trial balance", period);
	venture_report_result_add_column(result, "code", "Account", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "name", "Name", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "currency", "Currency", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "debit", "Debit", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "credit", "Credit", VENTURE_REPORT_COLUMN_MONEY);
	names = g_list_sort(g_hash_table_get_keys(currencies), (GCompareFunc)g_strcmp0);
	for (currency_node = names; currency_node != NULL; currency_node = currency_node->next)
	{
		const gchar *currency = currency_node->data;
		g_autoptr(VentureMoney) debits = venture_money_new_zero(currency);
		g_autoptr(VentureMoney) credits = venture_money_new_zero(currency);
		g_autoptr(VentureMoney) zero = venture_money_new_zero(currency);
		g_autoptr(VentureMoney) difference = NULL;
		g_autofree gchar *debit_key = g_strdup_printf("debits_%s", currency);
		g_autofree gchar *credit_key = g_strdup_printf("credits_%s", currency);
		g_autofree gchar *difference_key = g_strdup_printf("difference_%s", currency);

		for (i = 0; i < accounts->len; i++)
		{
			VentureEntity *account = g_ptr_array_index(accounts, i);
			g_autoptr(VentureMoney) balance = venture_posting_service_account_balance(
				service, venture_entity_get_id(account), org, currency, as_of, error);
			g_autoptr(VentureMoney) magnitude = NULL;
			g_autofree gchar *code = NULL;
			g_autofree gchar *name = NULL;
			gboolean debit;

			if (NULL == balance)
				goto fail;
			if (balance->amount == 0)
				continue;
			if (balance->amount == G_MININT64)
			{
				g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_BALANCE, "Trial balance magnitude overflows");
				goto fail;
			}
			debit = balance->amount > 0;
			magnitude = venture_money_abs(balance);
			if (!accumulate(debit ? &debits : &credits, magnitude, error))
				goto fail;
			g_object_get(account, "code", &code, "name", &name, NULL);
			venture_report_result_begin_row(result);
			venture_report_result_set_text(result, "code", code);
			venture_report_result_set_text(result, "name", name);
			venture_report_result_set_text(result, "currency", currency);
			venture_report_result_set_money(result, "debit", debit ? magnitude : zero);
			venture_report_result_set_money(result, "credit", debit ? zero : magnitude);
		}
		difference = venture_money_subtract(debits, credits, error);
		if (NULL == difference)
			goto fail;
		if (difference->amount != 0)
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_BALANCE, "The trial balance does not balance");
			goto fail;
		}
		venture_report_result_add_metric(result, venture_metric_new_money(debit_key, "Debits", debits));
		venture_report_result_add_metric(result, venture_metric_new_money(credit_key, "Credits", credits));
		venture_report_result_add_metric(result, venture_metric_new_money(difference_key, "Difference", difference));
	}
	if (!venture_database_commit(db, error))
		return NULL;
	return g_steal_pointer(&result);
fail:
	venture_database_rollback(db);
	return NULL;
}

void
venture_ledger_register_report(VentureReportRegistry *registry)
{
	venture_report_registry_add(registry, VENTURE_REPORT(venture_func_report_new(
		"trial_balance", "Trial balance",
		"Posted account balances as of the period end, for one legal entity, separately per book currency",
		trial_balance)));
}
