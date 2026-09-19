/*
 * venture-headline-home.c - The headline cards
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * One card per question Ben runs the business on: the P&L with cash in the
 * bank beside the booked figures, recurring revenue when billing is in use,
 * CAC, churn, LTV:CAC and the support load. Each is a headline figure read
 * from its report, the same report run for the comparison period so the
 * card can say which way it moved, a definition, and a link to the report
 * for the detail.
 *
 * Every card is written as JSON, HTML and CSV from the one loop that
 * computes it, so what the browser shows, what the API says and what a
 * spreadsheet keeps cannot disagree. The headline reports are read through
 * one VentureHeadlineSnapshot, so both periods of every card come from one
 * set of queries.
 */

#include "venture.h"
#include "report/venture-headline-private.h"

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

/* --- The render ----------------------------------------------------------- */

/* Everything one render writes into, card by card. */
typedef struct
{
	VentureContext		*context;
	VentureHeadlineSnapshot	*snapshot;
	VentureDateRange	*period;
	VentureDateRange	*previous;
	const gchar		*period_text;
	gboolean		 restricted;
	JsonArray		*cards;
	GString			*html;
	GString			*csv;
} HeadlineRender;

/* A result, or the error that stopped it; each card keeps its own. */
typedef struct
{
	VentureReportResult	*current;
	VentureReportResult	*previous;
	GError			*error;
} HeadlinePair;

static void
headline_pair_clear(HeadlinePair *pair)
{
	g_clear_object(&pair->current);
	g_clear_object(&pair->previous);
	g_clear_error(&pair->error);
}

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(HeadlinePair, headline_pair_clear)

static VentureReportResult *
headline_run(
	HeadlineRender		 *render,
	const gchar		 *name,
	VentureDateRange	 *period,
	GError			**error
){
	VentureReport *report;

	report = venture_report_registry_lookup(
		venture_context_get_report_registry(render->context), name);

	if (NULL == report)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		            "The %s report is not available", name);
		return NULL;
	}

	return venture_report_generate(report, render->context, period,
		venture_headline_snapshot_get_options(render->snapshot), error);
}

/* A registry report for the period and the comparison period. A report
 * that fails for the comparison period costs only the arrow. */
static void
headline_run_pair(
	HeadlineRender	*render,
	const gchar	*name,
	HeadlinePair	*pair
){
	pair->current = headline_run(render, name, render->period, &pair->error);

	if ((NULL != pair->current) && (NULL != render->previous))
		pair->previous = headline_run(render, name, render->previous, NULL);
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
		VentureMetric *metric = g_ptr_array_index(metrics, i);

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

/* A rate from basis points held in a count metric, for display. */
static VentureMetric *
headline_bps_ratio(
	VentureMetric	*count,
	const gchar	*key,
	const gchar	*label
){
	if ((NULL == count) || (NULL != venture_metric_get_text(count)))
		return NULL;

	return venture_metric_new_ratio(key, label,
		venture_metric_get_number(count) / 10000.0);
}

/* The report page for a card, carrying the question the card answered: the
 * period as it was asked and the scope. */
static gchar *
headline_link(
	HeadlineRender	*render,
	const gchar	*report
){
	JsonObject *options;
	GString *link;
	const gchar *as_of;
	gint64 venture_id;

	options = venture_headline_snapshot_get_options(render->snapshot);
	link = g_string_new("/reports/");
	g_string_append_uri_escaped(link, report, NULL, FALSE);
	g_string_append(link, "?period=");
	g_string_append_uri_escaped(link, render->period_text, NULL, FALSE);
	g_string_append_printf(link, "&organization_id=%" G_GINT64_FORMAT,
		venture_json_object_get_int(options, "organization_id", 0));
	venture_id = venture_json_object_get_int(options, "venture_id", 0);

	if (0 != venture_id)
		g_string_append_printf(link, "&venture_id=%" G_GINT64_FORMAT,
		                       venture_id);

	as_of = venture_json_object_get_string(options, "as_of", NULL);

	if (NULL != as_of)
	{
		g_string_append(link, "&as_of=");
		g_string_append_uri_escaped(link, as_of, NULL, FALSE);
	}

	return g_string_free(link, FALSE);
}

static void
headline_csv_row(
	GString		*csv,
	const gchar	*card,
	const gchar	*label,
	const gchar	*value,
	const gchar	*trend,
	const gchar	*state
){
	const gchar *fields[5];
	guint i;

	fields[0] = card;
	fields[1] = label;
	fields[2] = value;
	fields[3] = trend;
	fields[4] = state;

	for (i = 0; i < G_N_ELEMENTS(fields); i++)
	{
		g_autofree gchar *escaped = NULL;

		escaped = venture_csv_escape((NULL != fields[i]) ? fields[i] : "");

		if (i > 0)
			g_string_append_c(csv, ',');

		g_string_append(csv, escaped);
	}

	g_string_append_c(csv, '\n');
}

/* One line under a card's figure. A missing metric is "n/a" -- never "0",
 * which would claim a count that was never taken. A line with a link is a
 * number that is also a list: the page renders its value as the link. */
typedef struct
{
	const gchar	*label;
	VentureMetric	*metric;
	const gchar	*link;
} HeadlineLine;

/* A further report a card offers beside its own: a label and the report
 * name, linked with the same period and scope as the card. */
typedef struct
{
	const gchar	*label;
	const gchar	*report;
} HeadlineCut;

/*
 * Writes one card to all three renderings at once.
 *
 * @error, when set, makes the card an error card: the figure is n/a, the
 * message is shown in its place and logged with g_warning, and the rest of
 * the page carries on. A restricted render writes the card with no figures
 * at all. @cuts, when there are any, are further reports the card links to
 * beside its own, carried in the JSON as `links` (label/href pairs) and
 * rendered as buttons after the Report one.
 */
static void
headline_add_card(
	HeadlineRender		*render,
	const gchar		*key,
	const gchar		*label,
	const gchar		*report,
	const gchar		*definition,
	VentureMetric		*current,
	VentureMetric		*previous,
	gboolean		 higher_is_better,
	const HeadlineLine	*lines,
	gsize			 n_lines,
	const HeadlineCut	*cuts,
	gsize			 n_cuts,
	const GError		*error
){
	g_autofree gchar *link = NULL;
	g_autofree gchar *value = NULL;
	g_autofree gchar *trend_text = NULL;
	JsonObject *card;
	JsonArray *json_lines;
	const gchar *state;
	const gchar *arrow;
	const gchar *tone;
	gint trend;
	gsize i;

	link = headline_link(render, report);
	state = render->restricted ? "restricted" : ((NULL != error) ? "error" : "ok");
	trend = (NULL == error) ? headline_trend(current, previous) : 0;

	if (render->restricted)
	{
		value = g_strdup("restricted");
		trend = 0;
	}
	else if ((NULL != error) || (NULL == current))
	{
		value = g_strdup("n/a");
	}
	else
	{
		value = venture_metric_format_value(current);
	}

	if (NULL != error)
		g_warning("Headline card %s could not be computed: %s", key,
		          error->message);

	trend_text = g_strdup_printf("%d", trend);

	card = json_object_new();
	json_object_set_string_member(card, "key", key);
	json_object_set_string_member(card, "label", label);
	json_object_set_string_member(card, "value", value);
	json_object_set_int_member(card, "trend", trend);
	json_object_set_boolean_member(card, "higher_is_better", higher_is_better);
	json_object_set_string_member(card, "link", link);
	json_object_set_string_member(card, "definition", definition);
	json_object_set_string_member(card, "state", state);

	if (NULL != error)
		json_object_set_string_member(card, "error", error->message);
	else
		json_object_set_null_member(card, "error");

	arrow = (trend > 0) ? "\xe2\x86\x91" : ((trend < 0) ? "\xe2\x86\x93" : "\xe2\x86\x92");
	tone = (0 == trend) ? "flat"
		: (((trend > 0) == higher_is_better) ? "up" : "down");

	g_string_append(render->html,
		"<div class=\"card widget headline-card\" data-card=\"");
	venture_html_escape_append(render->html, key);
	g_string_append_printf(render->html, "\" data-state=\"%s\"><h3>", state);
	venture_html_escape_append(render->html, label);
	g_string_append(render->html,
		"</h3><div class=\"widget-figure\"><span class=\"figure\">");
	venture_html_escape_append(render->html, value);
	g_string_append(render->html, "</span>");

	if (!render->restricted)
		g_string_append_printf(render->html,
			"<span class=\"stat-delta %s\" title=\"against the period before\">%s</span>",
			tone, arrow);

	g_string_append(render->html, "</div>");
	headline_csv_row(render->csv, key, label, value, trend_text, state);

	json_lines = json_array_new();

	if (render->restricted)
	{
		g_string_append(render->html,
			"<p class=\"muted\">Organisation totals need the owner, admin "
			"or finance role in this organisation.</p>");
	}
	else
	{
		g_string_append(render->html, "<ul class=\"headline-lines\">");

		for (i = 0; i < n_lines; i++)
		{
			g_autofree gchar *line_value = NULL;
			JsonObject *line;

			line_value = ((NULL == error) && (NULL != lines[i].metric))
				? venture_metric_format_value(lines[i].metric)
				: g_strdup("n/a");
			line = json_object_new();
			json_object_set_string_member(line, "label", lines[i].label);
			json_object_set_string_member(line, "value", line_value);

			if (NULL != lines[i].link)
				json_object_set_string_member(line, "link", lines[i].link);

			json_array_add_object_element(json_lines, line);

			g_string_append(render->html, "<li><span class=\"muted\">");
			venture_html_escape_append(render->html, lines[i].label);
			g_string_append(render->html, "</span> ");

			if (NULL != lines[i].link)
			{
				g_string_append(render->html, "<a href=\"");
				venture_html_escape_append(render->html, lines[i].link);
				g_string_append(render->html, "\">");
				venture_html_escape_append(render->html, line_value);
				g_string_append(render->html, "</a>");
			}
			else
				venture_html_escape_append(render->html, line_value);

			g_string_append(render->html, "</li>");
			headline_csv_row(render->csv, key, lines[i].label, line_value,
			                 "", state);
		}

		g_string_append(render->html, "</ul>");
	}

	if (NULL != error)
	{
		g_string_append(render->html, "<div class=\"notice negative\">");
		venture_html_escape_append(render->html, error->message);
		g_string_append(render->html, "</div>");
	}

	g_string_append(render->html, "<p class=\"muted headline-definition\">");
	venture_html_escape_append(render->html, definition);
	g_string_append(render->html, "</p><p><a class=\"btn\" href=\"");
	venture_html_escape_append(render->html, link);
	g_string_append(render->html, "\">Report</a>");

	if (n_cuts > 0)
	{
		JsonArray *json_links;

		json_links = json_array_new();

		for (i = 0; i < n_cuts; i++)
		{
			g_autofree gchar *href = NULL;
			JsonObject *json_link;

			href = headline_link(render, cuts[i].report);
			json_link = json_object_new();
			json_object_set_string_member(json_link, "label", cuts[i].label);
			json_object_set_string_member(json_link, "href", href);
			json_array_add_object_element(json_links, json_link);

			g_string_append(render->html, " <a class=\"btn headline-cut\" href=\"");
			venture_html_escape_append(render->html, href);
			g_string_append(render->html, "\">");
			venture_html_escape_append(render->html, cuts[i].label);
			g_string_append(render->html, "</a>");
		}

		json_object_set_array_member(card, "links", json_links);
	}

	g_string_append(render->html, "</p></div>");

	json_object_set_array_member(card, "lines", json_lines);
	json_array_add_object_element(render->cards, card);
}

/*
 * Cash in the bank: the last statement balance of every bank account, in
 * the first account's currency. Beside the booked P&L because "what is
 * actually in the account" is the question the P&L does not answer.
 */
VentureMetric *
venture_headline_bank_cash(
	VentureContext	*context,
	gint64		 organization_id
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) accounts = NULL;
	g_autoptr(VentureMoney) total = NULL;
	VentureMetric *metric;
	guint skipped;
	guint i;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);

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

static VentureMetric *
headline_bank_cash(HeadlineRender *render)
{
	return venture_headline_bank_cash(render->context,
		venture_json_object_get_int(
			venture_headline_snapshot_get_options(render->snapshot),
			"organization_id", 0));
}

/*
 * The support queue as it stands: tickets open now, how many of them have
 * missed a service level, and -- for the arrow -- how many were open when
 * the period began: raised before it and not resolved before it. The
 * support report counts by when a ticket was raised, which is the right
 * question for "how did March go" and the wrong one for "what is waiting".
 */
static gboolean
headline_support_queue(
	HeadlineRender	 *render,
	gint64		 *out_open,
	gint64		 *out_breached,
	gint64		 *out_open_at_start,
	GError		**error
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) tickets = NULL;
	JsonObject *options;
	GDateTime *start;
	gint64 venture_id;
	guint i;

	*out_open = 0;
	*out_breached = 0;
	*out_open_at_start = 0;

	options = venture_headline_snapshot_get_options(render->snapshot);
	query = venture_query_new(VENTURE_TYPE_TICKET);
	venture_query_set_organization(query,
		venture_json_object_get_int(options, "organization_id", 0));
	venture_query_set_limit(query, 0);
	venture_id = venture_json_object_get_int(options, "venture_id", 0);

	if ((0 != venture_id) &&
	    !venture_query_add_filter_int(query, "venture-id", VENTURE_FILTER_OP_EQ,
	                                  venture_id, error))
		return FALSE;

	tickets = venture_database_find(venture_context_get_database(render->context),
	                                query, error);

	if (NULL == tickets)
		return FALSE;

	start = venture_date_range_get_start(render->period);

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

	return TRUE;
}

/* --- The cards ------------------------------------------------------------ */

static void
headline_card_pnl(HeadlineRender *render)
{
	/* The cuts of the P&L: the same figures by customer, by vendor, as a
	 * run-rate and as a cash outlook, from the pnl_cuts module. */
	static const HeadlineCut cuts[] = {
		{ "By customer", "revenue_by_customer" },
		{ "By vendor", "spend_by_vendor" },
		{ "Recurring", "recurring_costs" },
		{ "Cash outlook", "cash_outlook" }
	};
	g_auto(HeadlinePair) pnl = { NULL, NULL, NULL };
	g_autoptr(VentureMetric) cash = NULL;
	HeadlineLine lines[3] = { { NULL, NULL, NULL } };
	gsize n_cuts;

	if (!render->restricted)
	{
		headline_run_pair(render, "pnl", &pnl);
		cash = headline_bank_cash(render);
	}

	lines[0].label = "Revenue";
	lines[0].metric = headline_metric(pnl.current, "revenue");
	lines[1].label = "Expenses";
	lines[1].metric = headline_metric(pnl.current, "expenses");
	lines[2].label = "Bank cash";
	lines[2].metric = cash;
	n_cuts = venture_context_module_enabled(render->context, "pnl_cuts")
		? G_N_ELEMENTS(cuts) : 0;

	headline_add_card(render, "pnl", "Profit and loss", "pnl",
		"Profit is net revenue (sales less refunds, platform fees and "
		"shipping cost) minus every expense dated in the period. Bank cash "
		"is the sum of every bank account's last imported statement "
		"balance.",
		headline_metric(pnl.current, "profit"),
		headline_metric(pnl.previous, "profit"), TRUE,
		lines, G_N_ELEMENTS(lines), cuts, n_cuts, pnl.error);
}

static void
headline_card_mrr(HeadlineRender *render)
{
	g_auto(HeadlinePair) mrr = { NULL, NULL, NULL };
	g_auto(HeadlinePair) churn = { NULL, NULL, NULL };
	g_autoptr(VentureMetric) nrr = NULL;
	HeadlineLine lines[3] = { { NULL, NULL, NULL } };

	if (!render->restricted)
	{
		headline_run_pair(render, "mrr", &mrr);

		if (NULL == mrr.error)
			churn.current = headline_run(render, "churn", render->period,
			                             &churn.error);

		nrr = headline_bps_ratio(headline_metric(churn.current, "nrr_bps"),
		                         "nrr", "Net revenue retention");
	}

	lines[0].label = "ARR";
	lines[0].metric = headline_metric(mrr.current, "arr");
	lines[1].label = "Quick ratio";
	lines[1].metric = headline_metric(mrr.current, "quick_ratio");
	lines[2].label = "Net revenue retention";
	lines[2].metric = nrr;

	headline_add_card(render, "mrr", "Recurring revenue", "mrr",
		"Contracted monthly recurring revenue from active and past-due "
		"subscriptions at the period's end; ARR is twelve times MRR. The "
		"quick ratio is MRR gained (new, expansion, reactivation) over MRR "
		"lost (contraction, churn) in the period. Net revenue retention is "
		"what the subscribers at the period's start pay at its end, "
		"expansion included, over what they paid at its start.",
		headline_metric(mrr.current, "mrr"),
		headline_metric(mrr.previous, "mrr"), TRUE,
		lines, G_N_ELEMENTS(lines), NULL, 0,
		(NULL != mrr.error) ? mrr.error : churn.error);
}

static void
headline_card_cac(
	HeadlineRender		*render,
	VentureReportResult	*cac,
	VentureReportResult	*cac_before,
	const GError		*error
){
	HeadlineLine lines[2] = { { NULL, NULL, NULL } };

	lines[0].label = "Spend";
	lines[0].metric = headline_metric(cac, "spend");
	lines[1].label = "New customers";
	lines[1].metric = headline_metric(cac, "new_customers");

	headline_add_card(render, "cac", "Customer acquisition cost", "cac",
		"Acquisition spend in the period -- acquisition-flagged expenses "
		"and bill lines, and campaign spend pro-rated over each campaign -- "
		"divided by new customers: companies whose first cash receipt fell "
		"in the period.",
		headline_metric(cac, "cac"), headline_metric(cac_before, "cac"),
		FALSE, lines, G_N_ELEMENTS(lines), NULL, 0, error);
}

/*
 * Churn. With billing in use it is subscription logo churn: companies with
 * positive MRR at the period's start and none at its end. Otherwise it is
 * monthly activity churn from the customer_churn report.
 */
static void
headline_card_churn(
	HeadlineRender	*render,
	gboolean	 billing
){
	g_autoptr(VentureReportResult) activity = NULL;
	g_autoptr(VentureReportResult) activity_before = NULL;
	g_autoptr(VentureReportResult) health = NULL;
	g_autoptr(GError) error = NULL;
	g_auto(HeadlinePair) subscriptions = { NULL, NULL, NULL };
	g_autoptr(VentureMetric) logo = NULL;
	g_autoptr(VentureMetric) logo_before = NULL;
	g_autoptr(VentureMetric) revenue = NULL;
	g_autofree gchar *at_risk_link = NULL;
	HeadlineLine lines[3] = { { NULL, NULL, NULL } };
	gsize n_lines = 2;

	if (!render->restricted)
	{
		activity = venture_headline_snapshot_churn(render->snapshot,
		                                           render->period, &error);

		if ((NULL != activity) && (NULL != render->previous))
			activity_before = venture_headline_snapshot_churn(render->snapshot,
				render->previous, NULL);
	}

	/* Who is about to leave: the red count from the customer health
	 * report, as of the period's end, linking to the report filtered to
	 * red so the number is a list. Only with the customer_health module
	 * on; the card is otherwise exactly as it was. A health report that
	 * fails costs the line, not the card. */
	if (!render->restricted &&
	    venture_context_module_enabled(render->context, "customer_health"))
	{
		g_autoptr(GError) health_error = NULL;
		g_autofree gchar *report_link = NULL;

		health = headline_run(render, "customer_health", render->period,
		                      &health_error);

		if (NULL == health)
			g_warning("Headline churn card could not count customers at "
			          "risk: %s", health_error->message);

		report_link = headline_link(render, "customer_health");
		at_risk_link = g_strconcat(report_link, "&band=red", NULL);
		lines[2].label = "At risk";
		lines[2].metric = headline_metric(health, "red");
		lines[2].link = at_risk_link;
		n_lines = 3;
	}

	if (billing && !render->restricted && (NULL == error))
	{
		headline_run_pair(render, "churn", &subscriptions);
		logo = headline_bps_ratio(headline_metric(subscriptions.current,
		                                          "logo_churn_bps"),
		                          "logo_churn", "Logo churn");
		logo_before = headline_bps_ratio(headline_metric(subscriptions.previous,
		                                                 "logo_churn_bps"),
		                                 "logo_churn", "Logo churn");
		revenue = headline_bps_ratio(headline_metric(subscriptions.current,
		                                             "revenue_churn_bps"),
		                             "revenue_churn", "Revenue churn");
	}

	if (billing)
	{
		lines[0].label = "Revenue churn";
		lines[0].metric = revenue;
		lines[1].label = "Monthly activity churn";
		lines[1].metric = headline_metric(activity, "activity_churn");

		headline_add_card(render, "churn", "Churn", "churn",
			"Logo churn: companies paying for a subscription when the "
			"period opened and paying for none at its end, over those "
			"paying at its start. Revenue churn: the MRR those companies "
			"lost, contraction included, over their opening MRR.",
			logo, logo_before, FALSE, lines, n_lines, NULL, 0,
			(NULL != error) ? error : subscriptions.error);
		return;
	}

	lines[0].label = "Recurring churn";
	lines[0].metric = headline_metric(activity, "recurring_churn");
	lines[1].label = "Gone quiet";
	lines[1].metric = headline_metric(activity, "inactive_customers");

	headline_add_card(render, "churn", "Churn", "customer_churn",
		"Monthly activity churn: customers whose last cash receipt plus "
		"their quiet threshold (activity days, 90 by default, or one and a "
		"half times their own payment rhythm when longer) fell in the "
		"trailing twelve months, over customer-months at risk. Recurring "
		"churn counts customers whose invoice schedules all stopped in the "
		"period; a pause is not churn.",
		headline_metric(activity, "activity_churn"),
		headline_metric(activity_before, "activity_churn"), FALSE,
		lines, n_lines, NULL, 0, error);
}

static void
headline_card_ltv_cac(
	HeadlineRender		*render,
	VentureReportResult	*ratio,
	VentureReportResult	*ratio_before,
	const GError		*error
){
	HeadlineLine lines[4] = { { NULL, NULL, NULL } };

	lines[0].label = "Projected LTV";
	lines[0].metric = headline_metric(ratio, "projected_ltv");
	lines[1].label = "CAC";
	lines[1].metric = headline_metric(ratio, "cac");
	lines[2].label = "ARPA";
	lines[2].metric = headline_metric(ratio, "arpa");
	lines[3].label = "CAC payback (months)";
	lines[3].metric = headline_metric(ratio, "payback_months");

	headline_add_card(render, "ltv_cac", "LTV to CAC", "ltv_cac",
		"Projected lifetime value -- revenue per customer-month times gross "
		"margin over monthly activity churn, across the trailing twelve "
		"months -- divided by CAC. Gross margin counts only spend flagged "
		"cost of revenue and priced support hours. CAC payback is CAC over "
		"ARPA times gross margin, in months.",
		headline_metric(ratio, "ltv_cac"),
		headline_metric(ratio_before, "ltv_cac"), TRUE,
		lines, G_N_ELEMENTS(lines), NULL, 0, error);
}

/*
 * The card's number is what supporting customers cost in the period, read
 * through venture_support_rollup_cost() -- the same function a company's
 * page reads its line from, so the two cannot disagree. The queue (open
 * now, past a service level) and the tickets raised are the lines under
 * it.
 */
static void
headline_card_support(HeadlineRender *render)
{
	g_autoptr(VentureReportResult) support = NULL;
	g_autoptr(VentureMetric) open_now = NULL;
	g_autoptr(VentureMetric) breaches = NULL;
	g_autoptr(VentureMetric) cost = NULL;
	g_autoptr(VentureMetric) cost_before = NULL;
	g_autoptr(GError) error = NULL;
	HeadlineLine lines[3] = { { NULL, NULL, NULL } };
	gint64 open_count = 0;
	gint64 breached_count = 0;
	gint64 open_at_start = 0;

	if (!render->restricted &&
	    venture_context_module_enabled(render->context, "tickets"))
	{
		if (headline_support_queue(render, &open_count, &breached_count,
		                           &open_at_start, &error))
		{
			open_now = venture_metric_new_count("open", "Open tickets",
			                                    open_count);
			breaches = venture_metric_new_count("breaches",
			                                    "Service levels missed",
			                                    breached_count);
			support = headline_run(render, "support", render->period, &error);
		}

		if (NULL == error)
		{
			cost = venture_support_rollup_cost(render->context,
				venture_headline_snapshot_get_options(render->snapshot),
				render->period, &error);

			if ((NULL != cost) && (NULL != render->previous))
				cost_before = venture_support_rollup_cost(render->context,
					venture_headline_snapshot_get_options(render->snapshot),
					render->previous, NULL);
		}
	}

	/* With the tickets module off every figure is n/a: nobody counted
	 * tickets, which is not the same as there being none. */
	lines[0].label = "Open tickets";
	lines[0].metric = open_now;
	lines[1].label = "Service levels missed";
	lines[1].metric = breaches;
	lines[2].label = "Raised in period";
	lines[2].metric = headline_metric(support, "tickets");

	headline_add_card(render, "support", "Support", "support_rollup",
		"What supporting customers cost in the period: agent minutes logged "
		"on the tickets raised in it at the support hourly rate, plus the "
		"per-ticket rate for tickets with no minutes, from the support "
		"rollup. Beneath it, tickets open now, the ones among them past a "
		"service level, and tickets raised in the period.",
		cost, cost_before, FALSE, lines, G_N_ELEMENTS(lines), NULL, 0, error);
}

/* A range as the text that parses back to it: "all" when unbounded, else
 * YYYY-MM-DD..YYYY-MM-DD, inclusive, in the range's own timezone. */
static gchar *
headline_period_text(VentureDateRange *period)
{
	g_autoptr(GDateTime) last = NULL;
	g_autoptr(GDateTime) local_last = NULL;
	g_autofree gchar *first = NULL;
	g_autofree gchar *final = NULL;
	GDateTime *start;
	GDateTime *end;

	start = venture_date_range_get_start(period);
	end = venture_date_range_get_end(period);

	if ((NULL == start) || (NULL == end))
		return g_strdup("all");

	last = g_date_time_add(end, -1);
	local_last = g_date_time_to_timezone(last, g_date_time_get_timezone(start));
	first = g_date_time_format(start, "%Y-%m-%d");
	final = g_date_time_format(local_last, "%Y-%m-%d");

	return g_strdup_printf("%s..%s", first, final);
}

gboolean
venture_headline_home_render(
	VentureContext		 *context,
	JsonObject		 *options,
	VentureDateRange	 *period,
	const gchar		 *period_text,
	GDateTime		 *now,
	gboolean		  may_see_totals,
	JsonNode		**out_cards,
	gchar			**out_html,
	gchar			**out_csv,
	GError			**error
){
	g_autoptr(VentureHeadlineSnapshot) snapshot = NULL;
	g_autoptr(VentureDateRange) previous = NULL;
	g_autoptr(GDateTime) clock = NULL;
	g_autoptr(GString) html = NULL;
	g_autoptr(GString) csv = NULL;
	g_autoptr(VentureReportResult) cac = NULL;
	g_autoptr(VentureReportResult) cac_before = NULL;
	g_autoptr(VentureReportResult) ltv = NULL;
	g_autoptr(VentureReportResult) ratio = NULL;
	g_autoptr(VentureReportResult) ratio_before = NULL;
	g_autoptr(GError) cac_error = NULL;
	g_autoptr(GError) ratio_error = NULL;
	g_autoptr(GError) billing_error = NULL;
	g_autofree gchar *spelled = NULL;
	HeadlineRender render;
	JsonNode *node;
	gboolean billing = FALSE;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), FALSE);
	g_return_val_if_fail(NULL != period, FALSE);
	g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

	snapshot = venture_headline_snapshot_new(context, options, error);

	if (NULL == snapshot)
		return FALSE;

	clock = (NULL != now) ? g_date_time_ref(now) : venture_time_now();
	/* The period a card is compared with is the calendar one: September
	 * against August, and on the third of the month the first two days
	 * and a bit of each. */
	previous = venture_date_range_comparison_period(period, clock);
	html = g_string_new(NULL);
	csv = g_string_new("card,metric,value,trend,state\n");

	render.context = context;
	render.snapshot = snapshot;
	render.period = period;
	render.previous = previous;
	render.period_text = period_text;

	/* A caller with a range and no words for it still gets links that
	 * reopen exactly that range: an explicit inclusive span, which every
	 * period parser reads back. */
	if (venture_string_is_empty(period_text))
	{
		spelled = headline_period_text(period);
		render.period_text = spelled;
	}

	render.restricted = !may_see_totals;
	render.cards = json_array_new();
	render.html = html;
	render.csv = csv;

	if (!render.restricted)
	{
		if (!venture_headline_snapshot_billing_in_use(snapshot, &billing,
		                                              &billing_error))
			g_warning("Headline cards could not tell whether billing is in "
			          "use: %s", billing_error->message);

		/* CAC once per period, and LTV:CAC reads it rather than
		 * computing it again. */
		cac = venture_headline_snapshot_cac(snapshot, period, &cac_error);

		if (NULL != previous)
			cac_before = venture_headline_snapshot_cac(snapshot, previous, NULL);

		ltv = venture_headline_snapshot_ltv(snapshot, period, &ratio_error);

		if ((NULL != ltv) && (NULL != cac))
			ratio = venture_headline_snapshot_ltv_cac(snapshot, period, ltv,
			                                          cac, &ratio_error);
		else if ((NULL == ratio_error) && (NULL != cac_error))
			ratio_error = g_error_copy(cac_error);

		if ((NULL != ratio) && (NULL != previous))
			ratio_before = venture_headline_snapshot_ltv_cac(snapshot, previous,
			                                                 NULL, cac_before,
			                                                 NULL);
	}

	headline_card_pnl(&render);

	if (billing)
		headline_card_mrr(&render);

	headline_card_cac(&render, cac, cac_before, cac_error);
	headline_card_churn(&render, billing);
	headline_card_ltv_cac(&render, ratio, ratio_before, ratio_error);
	headline_card_support(&render);

	node = json_node_new(JSON_NODE_ARRAY);
	json_node_take_array(node, render.cards);

	if (NULL != out_cards)
		*out_cards = node;
	else
		json_node_unref(node);

	if (NULL != out_html)
		*out_html = g_string_free(g_steal_pointer(&html), FALSE);

	if (NULL != out_csv)
		*out_csv = g_string_free(g_steal_pointer(&csv), FALSE);

	return TRUE;
}

JsonNode *
venture_headline_home_cards(
	VentureContext		 *context,
	gint64			  organization_id,
	VentureDateRange	 *period,
	GError			**error
){
	g_autoptr(JsonObject) options = NULL;
	JsonNode *cards = NULL;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);
	g_return_val_if_fail(NULL != period, NULL);
	g_return_val_if_fail(error == NULL || *error == NULL, NULL);

	options = json_object_new();

	if (0 != organization_id)
		json_object_set_int_member(options, "organization_id", organization_id);

	if (!venture_headline_home_render(context, options, period, NULL, NULL,
	                                  TRUE, &cards, NULL, NULL, error))
		return NULL;

	return cards;
}
