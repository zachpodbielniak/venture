/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * The customer account card: where we stand with a customer, first on
 * their page. What they owe and how much of it is late, the invoices
 * still open with what is left on each, what they paid lately, and the
 * last thing done and the next one planned. Each figure comes from the
 * same settlement service the invoices and statements use, so the card
 * cannot disagree with them.
 *
 * Included into venture-web-server.c.
 */

#define CUSTOMER_ACCOUNT_PAYMENTS 5

/* venture-customer-credits-web.inc: the credits section of the card. */
static void customer_credits_append_card(VentureWebServer *self, GString *figures, GString *body,
	VentureEntity *company);

/* The day as the record lists show it, so the card and the list agree. */
static gchar *
customer_account_day(VentureWebServer *self, GDateTime *when)
{
	(void)self;
	return (NULL == when) ? g_strdup("") : g_date_time_format(when, "%Y-%m-%d");
}

/* "$100.00", or "$100.00, €20.00" for a customer billed in two. */
static gchar *
customer_account_totals_text(GPtrArray *totals)
{
	GString *text = g_string_new(NULL);
	guint i;

	for (i = 0; i < totals->len; i++)
	{
		g_autofree gchar *one = venture_money_to_display_string(g_ptr_array_index(totals, i), TRUE);

		if (venture_money_is_zero(g_ptr_array_index(totals, i)))
			continue;
		g_string_append_printf(text, "%s%s", text->len ? ", " : "", one);
	}
	return g_string_free(text, FALSE);
}

static GPtrArray *
customer_account_find(VentureWebServer *self, GType type, VentureEntity *company,
	const gchar *reference, const gchar *order, VentureSortDirection direction, guint limit)
{
	g_autoptr(VentureQuery) query = venture_query_new(type);

	venture_query_set_organization(query, venture_entity_get_organization_id(company));
	venture_query_add_filter_int(query, reference, VENTURE_FILTER_OP_EQ, venture_entity_get_id(company), NULL);
	venture_query_add_order(query, order, direction, NULL);
	venture_query_set_limit(query, limit);
	return venture_database_find(venture_context_get_database(self->context), query, NULL);
}

static void
customer_account_append_invoices(VentureWebServer *self, GString *html, VentureEntity *company,
	GPtrArray *owed, GPtrArray *late)
{
	VentureSettlementService *settlement;
	g_autoptr(GPtrArray) invoices = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(GString) rows = g_string_new(NULL);
	guint i;

	settlement = venture_settlement_service_get(venture_context_get_database(self->context));
	invoices = customer_account_find(self, VENTURE_TYPE_INVOICE, company, "company-id",
		"due-at", VENTURE_SORT_ASCENDING, 0);
	for (i = 0; (NULL != invoices) && (i < invoices->len); i++)
	{
		VentureEntity *invoice = g_ptr_array_index(invoices, i);
		g_autoptr(VentureMoney) balance = NULL;
		g_autoptr(GDateTime) due = NULL;
		g_autofree gchar *number = NULL;
		g_autofree gchar *left = NULL;
		g_autofree gchar *day = NULL;
		gint status = VENTURE_INVOICE_STATUS_DRAFT;
		gboolean overdue;

		g_object_get(invoice, "status", &status, "due-at", &due, "number", &number, NULL);
		if ((VENTURE_INVOICE_STATUS_SENT != status) && (VENTURE_INVOICE_STATUS_PARTIALLY_PAID != status))
			continue;
		balance = venture_settlement_service_invoice_balance(settlement,
			venture_entity_get_id(invoice), NULL, NULL);
		if ((NULL == balance) || venture_money_is_zero(balance) || venture_money_is_negative(balance))
			continue;
		overdue = (NULL != due) && (g_date_time_compare(due, now) < 0);
		venture_money_totals_add(owed, balance, NULL);
		if (overdue)
			venture_money_totals_add(late, balance, NULL);
		left = venture_money_to_display_string(balance, TRUE);
		day = customer_account_day(self, due);
		g_string_append_printf(rows, "<tr><td class=\"row-title\"><a href=\"/e/invoice/%" G_GINT64_FORMAT "\">",
			venture_entity_get_id(invoice));
		venture_html_escape_append(rows, number);
		g_string_append_printf(rows, "</a></td><td%s>%s%s</td><td class=\"num\">%s</td></tr>",
			overdue ? " class=\"negative\"" : "", day, overdue ? " (late)" : "", left);
	}
	if (0 == rows->len)
		return;
	g_string_append(html, "<h3>Open invoices</h3><div class=\"table-wrap\"><table class=\"data\">"
		"<thead><tr><th>Invoice</th><th>Due</th><th class=\"num\">Left to pay</th></tr></thead><tbody>");
	g_string_append_len(html, rows->str, rows->len);
	g_string_append(html, "</tbody></table></div>");
}

static void
customer_account_append_payments(VentureWebServer *self, GString *html, VentureEntity *company)
{
	g_autoptr(GPtrArray) payments = NULL;
	guint i;

	payments = customer_account_find(self, VENTURE_TYPE_PAYMENT, company, "customer-id",
		"date", VENTURE_SORT_DESCENDING, CUSTOMER_ACCOUNT_PAYMENTS);
	if ((NULL == payments) || (0 == payments->len))
		return;
	g_string_append(html, "<h3>Recent payments</h3><div class=\"table-wrap\"><table class=\"data\">"
		"<thead><tr><th>Date</th><th>Method</th><th class=\"num\">Amount</th></tr></thead><tbody>");
	for (i = 0; i < payments->len; i++)
	{
		VentureEntity *payment = g_ptr_array_index(payments, i);
		g_autoptr(VentureMoney) amount = NULL;
		g_autoptr(GDateTime) date = NULL;
		g_autofree gchar *method = NULL;
		g_autofree gchar *day = NULL;
		g_autofree gchar *shown = NULL;

		g_object_get(payment, "amount", &amount, "date", &date, "method", &method, NULL);
		day = customer_account_day(self, date);
		shown = (NULL != amount) ? venture_money_to_display_string(amount, TRUE) : g_strdup("");
		g_string_append_printf(html, "<tr><td class=\"row-title\"><a href=\"/e/payment/%" G_GINT64_FORMAT "\">%s</a></td><td>",
			venture_entity_get_id(payment), day);
		venture_html_escape_append(html, method);
		g_string_append_printf(html, "</td><td class=\"num\">%s</td></tr>", shown);
	}
	g_string_append(html, "</tbody></table></div>");
}

/* The last activity done for them and the next one planned. */
static void
customer_account_append_activity(VentureWebServer *self, GString *html, VentureEntity *company)
{
	g_autoptr(GPtrArray) activities = NULL;
	VentureEntity *last = NULL;
	VentureEntity *next = NULL;
	guint i;

	if (!venture_web_module_enabled(self, "activities"))
		return;
	activities = customer_account_find(self, VENTURE_TYPE_ACTIVITY, company, "company-id",
		"due-at", VENTURE_SORT_ASCENDING, 0);
	for (i = 0; (NULL != activities) && (i < activities->len); i++)
	{
		VentureEntity *activity = g_ptr_array_index(activities, i);
		gint status = VENTURE_ACTIVITY_STATUS_PLANNED;

		g_object_get(activity, "status", &status, NULL);
		if (VENTURE_ACTIVITY_STATUS_DONE == status)
			last = activity;
		else if ((NULL == next) && (VENTURE_ACTIVITY_STATUS_CANCELLED != status))
			next = activity;
	}
	if ((NULL == last) && (NULL == next))
		return;
	g_string_append(html, "<dl class=\"account-activity\">");
	if (NULL != last)
	{
		g_autofree gchar *subject = NULL;
		g_autoptr(GDateTime) when = NULL;
		g_autofree gchar *day = NULL;

		g_object_get(last, "subject", &subject, "completed-at", &when, NULL);
		day = customer_account_day(self, when);
		g_string_append_printf(html, "<dt>Last</dt><dd><a href=\"/e/activity/%" G_GINT64_FORMAT "\">",
			venture_entity_get_id(last));
		venture_html_escape_append(html, subject);
		g_string_append_printf(html, "</a> <span class=\"muted\">%s</span></dd>", day);
	}
	if (NULL != next)
	{
		g_autofree gchar *subject = NULL;
		g_autoptr(GDateTime) when = NULL;
		g_autofree gchar *day = NULL;

		g_object_get(next, "subject", &subject, "due-at", &when, NULL);
		day = customer_account_day(self, when);
		g_string_append_printf(html, "<dt>Next</dt><dd><a href=\"/e/activity/%" G_GINT64_FORMAT "\">",
			venture_entity_get_id(next));
		venture_html_escape_append(html, subject);
		g_string_append_printf(html, "</a> <span class=\"muted\">%s</span></dd>", day);
	}
	g_string_append(html, "</dl>");
}

static void
venture_customer_account_web_append_block(VentureWebServer *self, GString *html, VentureEntity *record)
{
	g_autoptr(GPtrArray) owed = NULL;
	g_autoptr(GPtrArray) late = NULL;
	g_autoptr(GString) body = NULL;
	g_autoptr(GString) credits = NULL;
	g_autofree gchar *owes = NULL;
	g_autofree gchar *overdue = NULL;
	gint64 id = venture_entity_get_id(record);
	g_autofree gchar *new_invoice = NULL;

	if (!VENTURE_IS_COMPANY(record) || (id <= 0) || !venture_company_is_customer(record) ||
	    !venture_entity_registry_is_type_enabled(venture_entity_registry_get_default(), "invoice"))
		return;

	owed = venture_money_totals_new();
	late = venture_money_totals_new();
	body = g_string_new(NULL);
	credits = g_string_new(NULL);
	customer_account_append_invoices(self, body, record, owed, late);
	customer_credits_append_card(self, credits, body, record);
	customer_account_append_payments(self, body, record);
	customer_account_append_activity(self, body, record);
	owes = customer_account_totals_text(owed);
	overdue = customer_account_totals_text(late);
	new_invoice = g_strdup_printf("/invoices/compose?company_id=%" G_GINT64_FORMAT, id);

	g_string_append(html, "<section class=\"card customer-account\"><div class=\"card-head\"><h2>Account</h2>"
		"<a class=\"btn btn-sm\" href=\"");
	venture_html_escape_append(html, new_invoice);
	g_string_append(html, "\">New invoice</a></div><div class=\"card-body\"><dl class=\"account-figures\">");
	g_string_append_printf(html, "<dt>Owes</dt><dd>%s</dd>", ('\0' != owes[0]) ? owes : "Nothing");
	if ('\0' != overdue[0])
		g_string_append_printf(html, "<dt>Overdue</dt><dd class=\"negative\">%s</dd>", overdue);
	g_string_append_len(html, credits->str, credits->len);
	g_string_append(html, "</dl>");
	g_string_append_len(html, body->str, body->len);
	g_string_append(html, "</div></section>");
}
