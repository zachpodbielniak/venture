/*
 * venture-arbitrage-web.inc - A trade's own block on its record page
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Included by venture-web-server.c before the record detail renderer,
 * which calls venture_web_append_arbitrage_block() in the group of
 * type-specific blocks that come before the generic panels. Everything it
 * shows comes from venture_arbitrage_trade_summary(), the same arithmetic
 * the arbitrage_performance report runs, so the page and a report row
 * cannot disagree. The trade's actions (close, reopen, abandon) are the
 * generic action card; a leg's Execute is offered here beside the leg,
 * posting to the same generated endpoint the leg's own page uses.
 */

/* A money member of a summary object, displayed; an em dash for none. */
static void
venture_web_arbitrage_money(
	GString		*html,
	JsonObject	*object,
	const gchar	*member
){
	g_autoptr(VentureMoney) money = NULL;
	g_autofree gchar *text = NULL;
	JsonNode *node;

	node = json_object_has_member(object, member) ? json_object_get_member(object, member) : NULL;

	if ((NULL != node) && JSON_NODE_HOLDS_OBJECT(node))
		money = venture_money_from_json(node, NULL, NULL);

	if (NULL == money)
	{
		g_string_append(html, "<span class=\"muted\">\xe2\x80\x94</span>");
		return;
	}

	text = venture_money_to_display_string(money, TRUE);
	venture_html_escape_append(html, text);
}

/* A record's name for a link, read as the viewer: one they may not see is
 * not named. */
static void
venture_web_arbitrage_link(
	VentureWebServer	*self,
	GString			*html,
	GType			 type,
	const gchar		*type_name,
	gint64			 id
){
	g_autoptr(VentureEntity) target = NULL;
	g_autofree gchar *label = NULL;

	if (id <= 0)
	{
		g_string_append(html, "<span class=\"muted\">\xe2\x80\x94</span>");
		return;
	}

	target = venture_database_get(venture_context_get_database(self->context), type, id, NULL);

	if (NULL == target)
	{
		g_string_append(html, "<span class=\"muted\">\xe2\x80\x94</span>");
		return;
	}

	label = venture_entity_get_display_name(target);
	g_string_append_printf(html, "<a href=\"/e/%s/%" G_GINT64_FORMAT "\">", type_name, id);
	venture_html_escape_append(html, label);
	g_string_append(html, "</a>");
}

static void
venture_web_append_arbitrage_block(
	VentureWebServer	*self,
	GString			*content,
	VentureAuthPrincipal	*principal,
	VentureEntity		*record
){
	g_autoptr(JsonNode) summary = NULL;
	g_autoptr(GError) error = NULL;
	JsonObject *root;
	JsonArray *figures;
	JsonArray *position;
	JsonArray *legs;
	JsonArray *journals;
	gboolean can_execute;
	gboolean over;
	gint status;
	gint64 trade_id;
	guint i;

	trade_id = venture_entity_get_id(record);
	summary = venture_arbitrage_trade_summary(venture_context_get_database(self->context),
	                                          trade_id, &error);

	if (NULL == summary)
	{
		g_string_append(content, "<div class=\"notice negative\" role=\"alert\">");
		venture_html_escape_append(content, error->message);
		g_string_append(content, "</div>");
		return;
	}

	status = 0;
	g_object_get(record, "status", &status, NULL);
	over = (VENTURE_ARBITRAGE_TRADE_STATUS_CLOSED == status) ||
	       (VENTURE_ARBITRAGE_TRADE_STATUS_ABANDONED == status);
	can_execute = !over && venture_web_require_for_type(self, principal, VENTURE_TYPE_ARBITRAGE_LEG,
	                                                   VENTURE_USER_ROLE_EDITOR, NULL);

	root = json_node_get_object(summary);
	figures = json_object_get_array_member(root, "figures");
	position = json_object_get_array_member(root, "position");
	legs = json_object_get_array_member(root, "legs");
	journals = json_object_get_array_member(root, "journals");

	/* --- What it made, per currency --- */

	g_string_append(content, "<div class=\"card arbitrage-figures\"><div class=\"card-head\">"
	                         "<h2>Result</h2></div><div class=\"card-body\">");

	if (0 == json_array_get_length(figures))
		g_string_append(content, "<p class=\"muted\">Nothing executed yet. A leg posts when "
		                         "it is executed.</p>");
	else
	{
		g_string_append(content, "<div class=\"table-wrap\"><table class=\"data\"><thead><tr>"
		                         "<th>Currency</th><th class=\"num\">Out</th>"
		                         "<th class=\"num\">Back</th><th class=\"num\">Into stock</th>"
		                         "<th class=\"num\">Cost of stock out</th>"
		                         "<th class=\"num\">Fees</th><th class=\"num\">Realised</th>"
		                         "</tr></thead><tbody>");

		for (i = 0; i < json_array_get_length(figures); i++)
		{
			JsonObject *row;

			row = json_array_get_object_element(figures, i);
			g_string_append(content, "<tr><td>");
			venture_html_escape_append(content, json_object_get_string_member(row, "currency"));
			g_string_append(content, "</td><td class=\"num\">");
			venture_web_arbitrage_money(content, row, "capital");
			g_string_append(content, "</td><td class=\"num\">");
			venture_web_arbitrage_money(content, row, "returned");
			g_string_append(content, "</td><td class=\"num\">");
			venture_web_arbitrage_money(content, row, "stock_bought");
			g_string_append(content, "</td><td class=\"num\">");
			venture_web_arbitrage_money(content, row, "stock_cost");
			g_string_append(content, "</td><td class=\"num\">");
			venture_web_arbitrage_money(content, row, "fees");
			g_string_append(content, "</td><td class=\"num\">");
			venture_web_arbitrage_money(content, row, "realised");
			g_string_append(content, "</td></tr>");
		}

		g_string_append(content, "</tbody></table></div>");
	}

	/* What is left on the positions account, per book section: the profit
	 * or loss Close will take to gains, or nothing once it has. */
	g_string_append(content, "<p class=\"muted\">On the positions account: ");

	{
		guint shown;

		shown = 0;

		for (i = 0; i < json_array_get_length(position); i++)
		{
			g_autoptr(VentureMoney) money = NULL;
			g_autofree gchar *text = NULL;

			money = venture_money_from_json(json_array_get_element(position, i), NULL, NULL);

			if ((NULL == money) || venture_money_is_zero(money))
				continue;

			text = venture_money_to_display_string(money, TRUE);
			g_string_append(content, (shown++ > 0) ? ", " : "");
			venture_html_escape_append(content, text);
			/* A credit balance is money ahead of what went out; a debit is
			 * money still out -- a loss only once the trade is over. */
			g_string_append(content, venture_money_is_negative(money) ? " ahead" : " still out");
		}

		if (0 == shown)
			g_string_append(content, "nothing");
	}

	g_string_append(content, ".</p>");

	if (json_array_get_length(journals) > 0)
	{
		g_string_append(content, "<p class=\"muted\">Journals:");

		for (i = 0; i < json_array_get_length(journals); i++)
		{
			gint64 journal_id;

			journal_id = json_array_get_int_element(journals, i);
			g_string_append_printf(content,
				" <a href=\"/e/journal/%" G_GINT64_FORMAT "\">#%" G_GINT64_FORMAT "</a>",
				journal_id, journal_id);
		}

		g_string_append(content, "</p>");
	}

	g_string_append(content, "</div></div>");

	/* --- The legs, in the order they were recorded --- */

	g_string_append(content, "<div class=\"card arbitrage-legs\"><div class=\"card-head\"><h2>Legs</h2>");

	if (!over)
		g_string_append_printf(content,
			"<a class=\"btn btn-sm\" href=\"/e/arbitrage_leg/new?trade_id=%" G_GINT64_FORMAT
			"\">New leg</a>", trade_id);

	g_string_append(content, "</div><div class=\"card-body\">");

	if (0 == json_array_get_length(legs))
	{
		g_string_append(content, "<p class=\"muted\">No legs yet. A leg is one movement at "
		                         "one venue: a buy, a sell, a stake, a payout, a fee or a "
		                         "transfer.</p></div></div>");
		return;
	}

	g_string_append(content, "<div class=\"table-wrap\"><table class=\"data\"><thead><tr>"
	                         "<th>When</th><th>Leg</th><th>Venue</th><th>Instrument</th>"
	                         "<th class=\"num\">Units</th><th class=\"num\">Amount</th>"
	                         "<th class=\"num\">Fees</th><th>Status</th><th></th>"
	                         "</tr></thead><tbody>");

	for (i = 0; i < json_array_get_length(legs); i++)
	{
		JsonObject *leg;
		const gchar *kind;
		const gchar *leg_status;
		const gchar *when;
		gint64 leg_id;
		gint64 quantity;

		leg = json_array_get_object_element(legs, i);

		/* A deleted leg's journals are still in the books and in the
		 * figures above; the row says so instead of vanishing. */
		leg_id = json_object_get_int_member_with_default(leg, "id", 0);
		kind = json_object_get_string_member_with_default(leg, "kind", "");
		leg_status = json_object_get_string_member_with_default(leg, "status", "");
		when = json_object_get_string_member_with_default(leg, "occurred_at", NULL);
		quantity = json_object_get_int_member_with_default(leg, "quantity", 0);

		g_string_append(content, "<tr><td>");

		if (!venture_string_is_empty(when))
		{
			g_autoptr(GDateTime) at = venture_time_from_string(when, NULL);
			g_autofree gchar *text = (NULL != at) ? venture_time_to_date_string(at,
				venture_context_get_timezone(self->context)) : NULL;

			venture_html_escape_append(content, (NULL != text) ? text : when);
		}
		else
			g_string_append(content, "<span class=\"muted\">\xe2\x80\x94</span>");

		g_string_append_printf(content, "</td><td><a href=\"/e/arbitrage_leg/%" G_GINT64_FORMAT "\">",
		                       leg_id);
		venture_html_escape_append(content, kind);
		g_string_append(content, "</a>");

		if (json_object_get_boolean_member_with_default(leg, "deleted", FALSE))
			g_string_append(content, " <span class=\"muted\">(deleted)</span>");

		g_string_append(content, "</td><td>");
		venture_web_arbitrage_link(self, content, VENTURE_TYPE_VENUE, "venue",
		                           json_object_get_int_member_with_default(leg, "venue_id", 0));
		g_string_append(content, "</td><td>");
		venture_web_arbitrage_link(self, content, VENTURE_TYPE_INSTRUMENT, "instrument",
		                           json_object_get_int_member_with_default(leg, "instrument_id", 0));
		g_string_append(content, "</td><td class=\"num\">");

		if (quantity > 0)
			g_string_append_printf(content, "%" G_GINT64_FORMAT, quantity);

		g_string_append(content, "</td><td class=\"num\">");
		venture_web_arbitrage_money(content, leg, "amount");
		g_string_append(content, "</td><td class=\"num\">");
		venture_web_arbitrage_money(content, leg, "fees");
		g_string_append_printf(content, "</td><td><span class=\"badge status-badge %s\" data-status=\"",
		                       venture_web_status_tone(leg_status));
		venture_html_escape_append(content, leg_status);
		g_string_append(content, "\">");
		venture_html_escape_append(content, leg_status);
		g_string_append(content, "</span></td><td>");

		if (can_execute && !json_object_get_boolean_member_with_default(leg, "deleted", FALSE) &&
		    ((0 == g_strcmp0(leg_status, "planned")) || (0 == g_strcmp0(leg_status, "failed"))))
			g_string_append_printf(content,
				"<form method=\"post\" action=\"/api/v1/arbitrage_leg/%" G_GINT64_FORMAT
				"/actions/execute\" data-record-action=\"execute\">"
				"<button class=\"btn btn-sm\" type=\"submit\">Execute</button></form>",
				leg_id);

		g_string_append(content, "</td></tr>");
	}

	g_string_append(content, "</tbody></table></div></div></div>");
}
