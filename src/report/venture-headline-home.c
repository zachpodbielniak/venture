/*
 * venture-headline-home.c - The five cards
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * One card per question Ben runs the business on: the P&L with cash in the
 * bank beside the booked figures, CAC, churn, LTV:CAC and the support load.
 * Each is a headline figure read from its report, the same report run for
 * the period before so the card can say which way it moved, and a link to
 * the report for the detail. Computed once here as JSON, then rendered by
 * the page, so what the browser shows and what the API says are one pass.
 */

#include "venture.h"

/* --- Settings ------------------------------------------------------------- */

VentureEntity *
venture_headline_setting_find(
	VentureDatabase	*database,
	gint64		 organization_id
){
	g_autoptr(VentureQuery) query = NULL;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);

	query = venture_query_new(VENTURE_TYPE_HEADLINE_SETTING);
	venture_query_set_organization(query, organization_id);
	venture_query_set_limit(query, 1);

	if (!venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL))
		return NULL;

	return venture_database_find_one(database, query, NULL);
}

gboolean
venture_headline_home_enabled(
	VentureDatabase	*database,
	gint64		 organization_id
){
	g_autoptr(VentureEntity) setting = NULL;
	gboolean classic = FALSE;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), FALSE);

	setting = venture_headline_setting_find(database, organization_id);

	if (NULL != setting)
		g_object_get(setting, "classic-home", &classic, NULL);

	return !classic;
}

/* --- Cards ---------------------------------------------------------------- */

static VentureReportResult *
headline_run(
	VentureContext		 *context,
	const gchar		 *name,
	gint64			  organization_id,
	VentureDateRange	 *period
){
	g_autoptr(JsonObject) options = NULL;
	VentureReport *report;

	report = venture_report_registry_lookup(
		venture_context_get_report_registry(context), name);

	if (NULL == report)
		return NULL;

	options = json_object_new();
	json_object_set_int_member(options, "organization_id", organization_id);

	return venture_report_generate(report, context, period, options, NULL);
}

static VentureMetric *
headline_metric(
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
		VentureMetric *metric;

		metric = g_ptr_array_index(metrics, i);

		if (0 == g_strcmp0(venture_metric_get_key(metric), key))
			return metric;
	}

	return NULL;
}

/*
 * Which way a figure moved: 1 up, -1 down, 0 flat or unknowable. Money is
 * compared as money; a number as a number; a text metric ("n/a") has no
 * direction, and neither does a comparison across currencies.
 */
static gint
headline_trend(
	VentureMetric	*current,
	VentureMetric	*previous
){
	const VentureMoney *now;
	const VentureMoney *then;

	if ((NULL == current) || (NULL == previous))
		return 0;

	now = venture_metric_get_money(current);
	then = venture_metric_get_money(previous);

	if ((NULL != now) && (NULL != then))
	{
		if (0 != g_ascii_strcasecmp(venture_money_get_currency(now),
		                            venture_money_get_currency(then)))
			return 0;

		return venture_money_compare(now, then);
	}

	if ((NULL != now) || (NULL != then))
		return 0;

	if ((NULL != venture_metric_get_text(current)) ||
	    (NULL != venture_metric_get_text(previous)))
		return 0;

	if (venture_metric_get_number(current) > venture_metric_get_number(previous))
		return 1;

	if (venture_metric_get_number(current) < venture_metric_get_number(previous))
		return -1;

	return 0;
}

static void
headline_add_line(
	JsonArray	*lines,
	const gchar	*label,
	VentureMetric	*metric,
	const gchar	*fallback
){
	g_autofree gchar *value = NULL;
	JsonObject *line;

	value = (NULL != metric) ? venture_metric_format_value(metric)
	                         : g_strdup(fallback);
	line = json_object_new();
	json_object_set_string_member(line, "label", label);
	json_object_set_string_member(line, "value", value);
	json_array_add_object_element(lines, line);
}

static void
headline_add_card(
	JsonArray	*cards,
	const gchar	*key,
	const gchar	*label,
	VentureMetric	*current,
	VentureMetric	*previous,
	gboolean	 higher_is_better,
	const gchar	*link,
	JsonArray	*lines
){
	g_autofree gchar *value = NULL;
	JsonObject *card;

	value = (NULL != current) ? venture_metric_format_value(current)
	                          : g_strdup("n/a");
	card = json_object_new();
	json_object_set_string_member(card, "key", key);
	json_object_set_string_member(card, "label", label);
	json_object_set_string_member(card, "value", value);
	json_object_set_int_member(card, "trend", headline_trend(current, previous));
	json_object_set_boolean_member(card, "higher_is_better", higher_is_better);
	json_object_set_string_member(card, "link", link);
	json_object_set_array_member(card, "lines", lines);
	json_array_add_object_element(cards, card);
}

/*
 * Cash in the bank: the last statement balance of every bank account, in
 * the first account's currency. Beside the booked P&L because "what is
 * actually in the account" is the question the P&L does not answer.
 */
static VentureMetric *
headline_bank_cash(
	VentureContext	*context,
	gint64		 organization_id
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) accounts = NULL;
	g_autoptr(VentureMoney) total = NULL;
	VentureMetric *metric;
	guint skipped;
	guint i;

	if (!venture_context_module_enabled(context, "banking"))
		return venture_metric_new_text("cash", "Bank cash", "n/a");

	query = venture_query_new(VENTURE_TYPE_BANK_ACCOUNT);
	venture_query_set_organization(query, organization_id);
	venture_query_set_limit(query, 0);
	accounts = venture_database_find(venture_context_get_database(context),
	                                 query, NULL);

	if (NULL == accounts)
		return venture_metric_new_text("cash", "Bank cash", "n/a");

	skipped = 0;

	for (i = 0; i < accounts->len; i++)
	{
		g_autoptr(VentureMoney) balance = NULL;
		VentureMoney *sum;

		g_object_get(g_ptr_array_index(accounts, i), "last-statement-balance",
		             &balance, NULL);

		if (NULL == balance)
			continue;

		if (NULL == total)
		{
			total = venture_money_copy(balance);
			continue;
		}

		sum = venture_money_add(total, balance, NULL);

		if (NULL == sum)
		{
			skipped++;
			continue;
		}

		venture_money_free(total);
		total = sum;
	}

	if (NULL == total)
		return venture_metric_new_text("cash", "Bank cash", "no statements");

	metric = venture_metric_new_money("cash", "Bank cash", total);

	if (skipped > 0)
		venture_metric_set_note(metric, "some accounts are in another currency");

	return metric;
}

/*
 * The support queue as it stands: tickets open now, how many of them have
 * missed a service level, and -- for the arrow -- how many were open when
 * the period began: raised before it and not resolved before it. The
 * support report counts by when a ticket was raised, which is the right
 * question for "how did March go" and the wrong one for "what is waiting".
 */
static void
headline_support_queue(
	VentureContext		*context,
	gint64			 organization_id,
	VentureDateRange	*period,
	gint64			*out_open,
	gint64			*out_breached,
	gint64			*out_open_at_start
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) tickets = NULL;
	GDateTime *start;
	guint i;

	*out_open = 0;
	*out_breached = 0;
	*out_open_at_start = 0;

	if (!venture_context_module_enabled(context, "tickets"))
		return;

	query = venture_query_new(VENTURE_TYPE_TICKET);
	venture_query_set_organization(query, organization_id);
	venture_query_set_limit(query, 0);
	tickets = venture_database_find(venture_context_get_database(context),
	                                query, NULL);

	if (NULL == tickets)
		return;

	start = venture_date_range_get_start(period);

	for (i = 0; i < tickets->len; i++)
	{
		g_autoptr(GDateTime) resolved_at = NULL;
		VentureEntity *ticket;
		GDateTime *created;
		VentureTicketStatus status;
		gboolean breached = FALSE;
		gboolean open;

		ticket = g_ptr_array_index(tickets, i);
		g_object_get(ticket, "status", &status, "sla-breached", &breached,
		             "resolved-at", &resolved_at, NULL);
		created = venture_entity_get_created_at(ticket);
		open = (VENTURE_TICKET_STATUS_DONE != status) &&
		       (VENTURE_TICKET_STATUS_CANCELLED != status);

		if (open)
		{
			(*out_open)++;

			if (breached)
				(*out_breached)++;
		}

		if ((NULL != start) && (NULL != created) &&
		    (g_date_time_compare(created, start) < 0) &&
		    ((NULL == resolved_at) ||
		     (g_date_time_compare(resolved_at, start) >= 0)))
			(*out_open_at_start)++;
	}
}

JsonNode *
venture_headline_home_cards(
	VentureContext		 *context,
	gint64			  organization_id,
	VentureDateRange	 *period,
	GError			**error
){
	g_autoptr(VentureDateRange) previous = NULL;
	g_autoptr(VentureReportResult) pnl = NULL;
	g_autoptr(VentureReportResult) pnl_before = NULL;
	g_autoptr(VentureReportResult) cac = NULL;
	g_autoptr(VentureReportResult) cac_before = NULL;
	g_autoptr(VentureReportResult) churn = NULL;
	g_autoptr(VentureReportResult) churn_before = NULL;
	g_autoptr(VentureReportResult) ratio = NULL;
	g_autoptr(VentureReportResult) ratio_before = NULL;
	g_autoptr(VentureReportResult) support = NULL;
	g_autoptr(VentureMetric) cash = NULL;
	g_autoptr(VentureMetric) open_now = NULL;
	g_autoptr(VentureMetric) open_before = NULL;
	g_autoptr(VentureMetric) breaches = NULL;
	gint64 open_count;
	gint64 breached_count;
	gint64 open_at_start;
	JsonArray *cards;
	JsonArray *lines;
	JsonNode *node;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);
	g_return_val_if_fail(NULL != period, NULL);

	if (0 == organization_id)
		organization_id = venture_context_get_default_organization_id(context);

	previous = venture_date_range_previous_period(period);

	pnl = headline_run(context, "pnl", organization_id, period);
	cac = headline_run(context, "cac", organization_id, period);
	churn = headline_run(context, "churn", organization_id, period);
	ratio = headline_run(context, "ltv_cac", organization_id, period);
	support = headline_run(context, "support", organization_id, period);

	/* All-time has no period before it; the arrows are then flat. */
	if (NULL != previous)
	{
		pnl_before = headline_run(context, "pnl", organization_id, previous);
		cac_before = headline_run(context, "cac", organization_id, previous);
		churn_before = headline_run(context, "churn", organization_id, previous);
		ratio_before = headline_run(context, "ltv_cac", organization_id, previous);
	}

	if ((NULL == pnl) && (NULL == cac) && (NULL == churn) &&
	    (NULL == ratio) && (NULL == support))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		                    "None of the headline reports is available; is "
		                    "the headline module on?");
		return NULL;
	}

	cards = json_array_new();

	/* 1. Money: the P&L, with the bank beside the books. */
	cash = headline_bank_cash(context, organization_id);
	lines = json_array_new();
	headline_add_line(lines, "Revenue", headline_metric(pnl, "revenue"), "n/a");
	headline_add_line(lines, "Expenses", headline_metric(pnl, "expenses"), "n/a");
	headline_add_line(lines, "Bank cash", cash, "n/a");
	headline_add_card(cards, "pnl", "Profit and loss",
	                  headline_metric(pnl, "profit"),
	                  headline_metric(pnl_before, "profit"), TRUE,
	                  "/reports/pnl", lines);

	/* 2. What a customer costs. */
	lines = json_array_new();
	headline_add_line(lines, "Spend", headline_metric(cac, "spend"), "n/a");
	headline_add_line(lines, "New customers",
	                  headline_metric(cac, "new_customers"), "0");
	headline_add_card(cards, "cac", "Customer acquisition cost",
	                  headline_metric(cac, "cac"),
	                  headline_metric(cac_before, "cac"), FALSE,
	                  "/reports/cac", lines);

	/* 3. Who is leaving. */
	lines = json_array_new();
	headline_add_line(lines, "Recurring churn",
	                  headline_metric(churn, "recurring_churn"), "n/a");
	headline_add_line(lines, "Gone quiet",
	                  headline_metric(churn, "inactive_customers"), "0");
	headline_add_card(cards, "churn", "Churn",
	                  headline_metric(churn, "activity_churn"),
	                  headline_metric(churn_before, "activity_churn"), FALSE,
	                  "/reports/churn", lines);

	/* 4. Whether a customer is worth acquiring. */
	lines = json_array_new();
	headline_add_line(lines, "Projected LTV",
	                  headline_metric(ratio, "projected_ltv"), "n/a");
	headline_add_line(lines, "CAC", headline_metric(ratio, "cac"), "n/a");
	headline_add_card(cards, "ltv_cac", "LTV to CAC",
	                  headline_metric(ratio, "ltv_cac"),
	                  headline_metric(ratio_before, "ltv_cac"), TRUE,
	                  "/reports/ltv_cac", lines);

	/* 5. How customers are being looked after: the queue as it stands. */
	headline_support_queue(context, organization_id, period, &open_count,
	                       &breached_count, &open_at_start);
	open_now = venture_metric_new_count("open", "Open tickets", open_count);
	open_before = venture_metric_new_count("open", "Open tickets", open_at_start);
	breaches = venture_metric_new_count("breaches", "Service levels missed",
	                                    breached_count);
	lines = json_array_new();
	headline_add_line(lines, "Service levels missed", breaches, "0");
	headline_add_line(lines, "Raised in period",
	                  headline_metric(support, "tickets"), "0");
	headline_add_card(cards, "support", "Support", open_now, open_before, FALSE,
	                  "/reports/support", lines);

	node = json_node_new(JSON_NODE_ARRAY);
	json_node_take_array(node, cards);

	return node;
}
