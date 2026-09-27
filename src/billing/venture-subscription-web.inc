/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * venture-subscription-web.inc - Where a subscription came from, what it
 * has used this period, and whether its last invoice is paid.
 */

/* Which saved method an authorization charges, in words. */
static gchar *
venture_subscription_web_method(VentureEntity *authorization)
{
	g_autofree gchar *label = NULL;
	if (authorization != NULL)
		g_object_get(authorization, "method-label", &label, NULL);
	if (label != NULL && *label != '\0')
		return g_steal_pointer(&label);
	/* Authorized before the method was recorded: still true, less specific. */
	return g_strdup("the saved payment method");
}

/*
 * The last invoice's state, in one sentence. With Stripe on, "paid" is
 * told apart from "collected automatically", and an unpaid invoice says
 * whether a saved card will be charged or somebody has to pay it -- which
 * is the question a customer's account manager is asked.
 */
static void
venture_subscription_web_payment(VentureWebServer *self, GString *html, VentureEntity *record)
{
	VentureDatabase *database = venture_context_get_database(self->context);
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_SUBSCRIPTION_EVENT);
	g_autoptr(VentureEntity) last = NULL;
	g_autoptr(VentureEntity) authorization = NULL;
	g_autoptr(VentureEntity) paid_by = NULL;
	g_autoptr(VentureMoney) balance = NULL;
	g_autoptr(GError) missing = NULL;
	gint64 invoice_id = 0;
	gboolean collecting = FALSE;

	venture_query_set_limit(query, 1);
	venture_query_add_filter_int(query, "subscription-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(record), NULL);
	venture_query_add_filter_int(query, "invoice-id", VENTURE_FILTER_OP_GT, 0, NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_DESCENDING, NULL);
	last = venture_database_find_one(database, query, &missing);
	if (last == NULL)
		return;
	g_object_get(last, "invoice-id", &invoice_id, NULL);
	balance = venture_settlement_service_invoice_balance(venture_settlement_service_get(database), invoice_id, NULL, NULL);
	if (balance == NULL)
		return;
	if (venture_web_module_enabled(self, "stripe"))
	{
		g_autoptr(VentureQuery) permissions = venture_query_new(VENTURE_TYPE_STRIPE_AUTHORIZATION);
		g_autoptr(VentureQuery) attempts = venture_query_new(VENTURE_TYPE_STRIPE_CHECKOUT);
		g_autoptr(GPtrArray) rows = NULL;
		guint i;

		venture_query_set_limit(permissions, 0);
		venture_query_add_filter_int(permissions, "subscription-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(record), NULL);
		rows = venture_database_find(database, permissions, NULL);
		for (i = 0; rows != NULL && i < rows->len; i++)
		{
			VentureEntity *candidate = g_ptr_array_index(rows, i);
			g_autofree gchar *status = NULL;
			gboolean enabled = FALSE;
			g_object_get(candidate, "status", &status, "enabled", &enabled, NULL);
			if (enabled && g_strcmp0(status, "active") == 0)
				g_set_object(&authorization, candidate);
		}
		g_clear_pointer(&rows, g_ptr_array_unref);
		venture_query_set_limit(attempts, 0);
		venture_query_add_filter_int(attempts, "invoice-id", VENTURE_FILTER_OP_EQ, invoice_id, NULL);
		rows = venture_database_find(database, attempts, NULL);
		for (i = 0; rows != NULL && i < rows->len; i++)
		{
			VentureEntity *attempt = g_ptr_array_index(rows, i);
			g_autofree gchar *channel = NULL, *status = NULL;
			gint64 payment = 0, permission = 0;
			gboolean active = FALSE;
			g_object_get(attempt, "channel", &channel, "status", &status, "payment-id", &payment,
				"authorization-id", &permission, "active", &active, NULL);
			if (g_strcmp0(channel, "automatic") != 0)
				continue;
			if (payment != 0 && permission != 0)
			{
				g_clear_object(&paid_by);
				paid_by = venture_database_get(database, VENTURE_TYPE_STRIPE_AUTHORIZATION, permission, NULL);
			}
			else if (active && g_strcmp0(status, "processing") == 0)
				collecting = TRUE;
		}
	}
	g_string_append(html, "<p>");
	if (venture_money_get_amount(balance) <= 0)
	{
		if (paid_by != NULL)
		{
			g_autofree gchar *method = venture_subscription_web_method(paid_by);
			g_string_append(html, "Collected automatically from ");
			venture_html_escape_append(html, method);
			g_string_append(html, ".");
		}
		else
			g_string_append(html, "The last invoice is paid.");
	}
	else
	{
		g_autofree gchar *owed = venture_money_to_display_string(balance, TRUE);
		g_string_append(html, "Waiting for payment of <strong>");
		venture_html_escape_append(html, owed);
		g_string_append(html, "</strong>");
		if (authorization != NULL)
		{
			g_autofree gchar *method = venture_subscription_web_method(authorization);
			g_string_append(html, collecting ? " \xe2\x80\x94 collection from " : " \xe2\x80\x94 it will be collected automatically from ");
			venture_html_escape_append(html, method);
			g_string_append(html, collecting ? " is in progress." : ".");
		}
		else
			g_string_append(html, ".");
	}
	g_string_append(html, "</p>");
}

/* The current period's metered use so far, and what it adds up to. */
static void
venture_subscription_web_usage(VentureWebServer *self, GString *html, VentureEntity *record)
{
	VentureDatabase *database = venture_context_get_database(self->context);
	g_autoptr(VentureEntity) price = NULL;
	g_autoptr(GDateTime) start = NULL, end = NULL;
	g_autoptr(VentureMoney) rate = NULL;
	g_autofree gchar *unit = NULL, *used_text = NULL, *included_text = NULL;
	gint64 price_id = 0, used, included = 0;

	g_object_get(record, "plan-price-id", &price_id, "current-period-start", &start, "current-period-end", &end, NULL);
	if (price_id <= 0 || start == NULL || end == NULL)
		return;
	price = venture_database_get(database, VENTURE_TYPE_PLAN_PRICE, price_id, NULL);
	if (price == NULL || !venture_plan_price_is_metered(VENTURE_PLAN_PRICE(price)))
		return;
	used = venture_billing_usage_total(database, venture_entity_get_organization_id(record),
		venture_entity_get_id(record), start, end, NULL);
	if (used < 0)
		return;
	g_object_get(price, "usage-unit", &unit, "unit-amount", &rate, "included-units", &included, NULL);
	used_text = venture_billing_format_count(used);
	included_text = venture_billing_format_count(included);
	g_string_append(html, "<p>Usage this period: <strong>");
	venture_html_escape_append(html, used_text);
	g_string_append_c(html, ' ');
	venture_html_escape_append(html, unit);
	g_string_append(html, "</strong>");
	if (included > 0)
	{
		g_string_append(html, " (");
		venture_html_escape_append(html, included_text);
		g_string_append(html, " included)");
	}
	if (used > included && rate != NULL)
	{
		g_autoptr(VentureMoney) charge = venture_money_multiply_int(rate, used - included, NULL);
		g_autoptr(VentureMoney) rounded = charge != NULL ? venture_money_rescale(charge,
			(guint8)venture_currency_get_exponent(venture_money_get_currency(charge)), NULL) : NULL;
		if (rounded != NULL)
		{
			g_autofree gchar *text = venture_money_to_display_string(rounded, TRUE);
			g_string_append(html, "; ");
			venture_html_escape_append(html, text);
			g_string_append(html, " so far, added to the renewal invoice.");
		}
	}
	else
		g_string_append(html, "; nothing extra so far.");
	g_string_append(html, "</p>");
}

static void
venture_subscription_web_facts(VentureWebServer *self, GString *html, VentureEntity *record)
{
	VentureDatabase *database = venture_context_get_database(self->context);
	g_autoptr(GString) body = g_string_new(NULL);
	gint64 quote_id = 0;

	g_object_get(record, "quote-id", &quote_id, NULL);
	if (quote_id > 0)
	{
		g_autoptr(VentureEntity) quote = venture_database_get(database, VENTURE_TYPE_QUOTE, quote_id, NULL);
		g_autofree gchar *number = NULL;
		if (quote != NULL)
			g_object_get(quote, "number", &number, NULL);
		g_string_append_printf(body, "<p>From quote <a href=\"/e/quote/%" G_GINT64_FORMAT "\">", quote_id);
		venture_html_escape_append(body, number != NULL ? number : "");
		g_string_append(body, "</a>.</p>");
	}
	venture_subscription_web_usage(self, body, record);
	venture_subscription_web_payment(self, body, record);
	if (body->len == 0)
		return;
	g_string_append(html, "<section class=\"card\"><div class=\"card-head\"><h2>Usage and payment</h2></div><div class=\"card-body\">");
	g_string_append(html, body->str);
	g_string_append(html, "</div></section>");
}
