/*
 * venture-web-company-tree.inc - A company's parent account and branches
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Included by venture-web-server.c. The figures come from
 * venture_company_tree_rollup(); this file only lays them out, so the
 * page and the API cannot disagree about what a branch adds.
 */

/* A link to one company, by name. */
static void
company_tree_web_link(GString *html, VentureEntity *company)
{
	g_autofree gchar *name = venture_entity_get_display_name(company);

	g_string_append_printf(html, "<a href=\"/e/company/%" G_GINT64_FORMAT "\">",
	                       venture_entity_get_id(company));
	venture_html_escape_append(html, name);
	g_string_append(html, "</a>");
}

static void
company_tree_web_money(GString *html, const VentureMoney *money)
{
	g_autofree gchar *text = venture_money_to_display_string(money, TRUE);

	venture_html_escape_append(html, text);
}

/* One figure: an amount with its count, or a count alone. */
static void
company_tree_web_cell(GString *html, guint count, const VentureMoney *money)
{
	g_string_append(html, "<td>");
	if (NULL == money)
		g_string_append_printf(html, "%u", count);
	else
	{
		company_tree_web_money(html, money);
		if (count > 0)
			g_string_append_printf(html, " (%u)", count);
	}
	g_string_append(html, "</td>");
}

/* One row: the company alone, then with every branch beneath it. */
static void
company_tree_web_row(GString *html, const gchar *label, guint alone_count, const VentureMoney *alone_money,
	guint all_count, const VentureMoney *all_money)
{
	g_string_append_printf(html, "<tr><th scope=\"row\">%s</th>", label);
	company_tree_web_cell(html, alone_count, alone_money);
	company_tree_web_cell(html, all_count, all_money);
	g_string_append(html, "</tr>");
}

/*
 * The hierarchy card on a company's page: the parent it is part of, the
 * branches beneath it, and -- when it has branches -- what it is worth
 * alone and including them. A company with neither parent nor branches
 * gets no card; most customers are one company, and an empty card on
 * every one of them is noise.
 */
static void
venture_web_append_company_tree(VentureWebServer *self, GString *html, VentureEntity *company)
{
	VentureDatabase *database = venture_context_get_database(self->context);
	g_autoptr(VentureEntity) parent = NULL;
	g_autoptr(GPtrArray) children = NULL;
	VentureCompanyRollup alone = { 0 }, all = { 0 };
	gint64 parent_id = 0;
	gboolean with_branches;
	guint i;

	if (!VENTURE_IS_COMPANY(company) || venture_entity_get_id(company) <= 0)
		return;

	g_object_get(company, "parent-id", &parent_id, NULL);
	if (parent_id > 0)
		parent = venture_database_get(database, VENTURE_TYPE_COMPANY, parent_id, NULL);
	children = venture_company_tree_children(database, venture_entity_get_id(company), NULL);
	with_branches = (NULL != children) && (children->len > 0);

	if ((NULL == parent) && !with_branches)
		return;

	g_string_append_printf(html, "<section class=\"card company-tree\"><div class=\"card-head\"><h2>%s</h2></div>"
		"<div class=\"card-body\">", with_branches ? "Branches" : "Parent account");

	if (NULL != parent)
	{
		g_string_append(html, "<p>Part of ");
		company_tree_web_link(html, parent);
		g_string_append(html, "</p>");
	}

	if (!with_branches)
	{
		g_string_append(html, "</div></section>");
		return;
	}

	g_string_append(html, "<ul class=\"related\">");
	for (i = 0; i < children->len; i++)
	{
		g_string_append(html, "<li>");
		company_tree_web_link(html, g_ptr_array_index(children, i));
		g_string_append(html, "</li>");
	}
	g_string_append(html, "</ul>");

	if (venture_company_tree_rollup(database, venture_entity_get_id(company), FALSE, &alone, NULL) &&
	    venture_company_tree_rollup(database, venture_entity_get_id(company), TRUE, &all, NULL))
	{
		g_string_append(html, "<table class=\"data\"><thead><tr><th></th><th>This company</th>"
			"<th>Including branches</th></tr></thead><tbody>");
		company_tree_web_row(html, "Open deals", alone.open_deals, alone.open_deal_value,
			all.open_deals, all.open_deal_value);
		company_tree_web_row(html, "Won revenue", alone.won_deals, alone.won_revenue,
			all.won_deals, all.won_revenue);
		company_tree_web_row(html, "Open invoices", alone.open_invoices, NULL,
			all.open_invoices, NULL);
		company_tree_web_row(html, "Balance due", 0, alone.balance_due, 0, all.balance_due);
		company_tree_web_row(html, "Open tickets", alone.open_tickets, NULL, all.open_tickets, NULL);
		g_string_append(html, "</tbody></table>");
		if (all.other_currency > 0)
			g_string_append_printf(html, "<p class=\"hint\">%u deal%s in another currency %s counted but not "
				"added to the totals.</p>", all.other_currency, all.other_currency == 1 ? "" : "s",
				all.other_currency == 1 ? "is" : "are");
	}
	venture_company_rollup_clear(&alone);
	venture_company_rollup_clear(&all);

	g_string_append(html, "</div></section>");
}
