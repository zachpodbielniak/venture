/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * A customer's credits, where the owner already is: on the customer's
 * account card and on each of their open invoices, each with an Apply
 * button. Applying is one payment allocation from the credit to the
 * invoice, saved through the settlement service like any other, so the
 * credit's remaining amount, the invoice's balance and its status move
 * together in one transaction.
 *
 * Included into venture-web-server.c, after the account card.
 */

/* The customer's credits with something left on them, oldest first. */
static GPtrArray *
customer_credits_unused(VentureWebServer *self, VentureEntity *company)
{
	g_autoptr(GPtrArray) all = NULL;
	GPtrArray *unused = g_ptr_array_new_with_free_func(g_object_unref);
	guint i;

	all = customer_account_find(self, VENTURE_TYPE_CUSTOMER_CREDIT, company, "customer-id",
		"date", VENTURE_SORT_ASCENDING, 0);
	for (i = 0; (NULL != all) && (i < all->len); i++)
	{
		VentureEntity *credit = g_ptr_array_index(all, i);
		g_autoptr(VentureMoney) left = NULL;

		g_object_get(credit, "remaining", &left, NULL);
		if ((NULL != left) && !venture_money_is_zero(left) && !venture_money_is_negative(left))
			g_ptr_array_add(unused, g_object_ref(credit));
	}
	return unused;
}

/* "50.00": the amount box's value, in the credit's own currency. */
static gchar *
customer_credits_amount_text(const VentureMoney *amount)
{
	g_autofree gchar *text = venture_money_to_string(amount);
	gchar *space = strchr(text, ' ');

	return (NULL != space) ? g_strndup(text, space - text) : g_strdup(text);
}

/* The smaller of what the credit holds and what the invoice owes. */
static VentureMoney *
customer_credits_offer(VentureWebServer *self, VentureEntity *credit, VentureEntity *invoice)
{
	g_autoptr(VentureMoney) left = NULL;
	g_autoptr(VentureMoney) owed = NULL;

	g_object_get(credit, "remaining", &left, NULL);
	owed = venture_settlement_service_invoice_balance(
		venture_settlement_service_get(venture_context_get_database(self->context)),
		venture_entity_get_id(invoice), NULL, NULL);
	if ((NULL == left) || (NULL == owed) ||
	    g_strcmp0(venture_money_get_currency(left), venture_money_get_currency(owed)) != 0)
		return NULL;
	return venture_money_copy((venture_money_compare(left, owed) <= 0) ? left : owed);
}

/* The customer's invoices that still owe something, due soonest first. */
static GPtrArray *
customer_credits_open_invoices(VentureWebServer *self, VentureEntity *company)
{
	g_autoptr(GPtrArray) all = NULL;
	GPtrArray *open = g_ptr_array_new_with_free_func(g_object_unref);
	guint i;

	all = customer_account_find(self, VENTURE_TYPE_INVOICE, company, "company-id",
		"due-at", VENTURE_SORT_ASCENDING, 0);
	for (i = 0; (NULL != all) && (i < all->len); i++)
	{
		VentureEntity *invoice = g_ptr_array_index(all, i);
		gint status = VENTURE_INVOICE_STATUS_DRAFT;

		g_object_get(invoice, "status", &status, NULL);
		if ((VENTURE_INVOICE_STATUS_SENT == status) || (VENTURE_INVOICE_STATUS_PARTIALLY_PAID == status))
			g_ptr_array_add(open, g_object_ref(invoice));
	}
	return open;
}

static void
customer_credits_append_form_start(GString *html, VentureEntity *credit, const gchar *back)
{
	g_string_append_printf(html, "<form class=\"inline-form\" method=\"post\" action=\"/customer-credits/apply\">"
		"<input type=\"hidden\" name=\"credit_id\" value=\"%" G_GINT64_FORMAT "\">"
		"<input type=\"hidden\" name=\"back\" value=\"", venture_entity_get_id(credit));
	venture_html_escape_append(html, back);
	g_string_append(html, "\">");
}

static void
customer_credits_append_amount(GString *html, const VentureMoney *offer)
{
	g_autofree gchar *value = (NULL != offer) ? customer_credits_amount_text(offer) : g_strdup("");

	g_string_append_printf(html, "<input type=\"text\" inputmode=\"decimal\" name=\"amount\" value=\"%s\" "
		"size=\"8\" aria-label=\"Amount to apply\"> <button class=\"btn btn-sm\" type=\"submit\">Apply</button></form>",
		value);
}

/* On the account card: the credit total beside what is owed, and each
 * credit with a form to apply it to one of the open invoices. */
static void
customer_credits_append_card(VentureWebServer *self, GString *figures, GString *body, VentureEntity *company)
{
	g_autoptr(GPtrArray) credits = customer_credits_unused(self, company);
	g_autoptr(GPtrArray) invoices = NULL;
	g_autoptr(GPtrArray) totals = venture_money_totals_new();
	g_autofree gchar *total = NULL;
	g_autofree gchar *back = g_strdup_printf("/e/company/%" G_GINT64_FORMAT, venture_entity_get_id(company));
	guint i, j;

	if (0 == credits->len)
		return;
	invoices = customer_credits_open_invoices(self, company);
	g_string_append(body, "<h3>Credits</h3><div class=\"table-wrap\"><table class=\"data\">"
		"<thead><tr><th>Credit</th><th class=\"num\">Left</th><th>Apply to</th></tr></thead><tbody>");
	for (i = 0; i < credits->len; i++)
	{
		VentureEntity *credit = g_ptr_array_index(credits, i);
		g_autoptr(VentureMoney) left = NULL;
		g_autoptr(VentureMoney) offer = NULL;
		g_autofree gchar *shown = NULL;
		g_autofree gchar *name = venture_entity_get_display_name(credit);

		g_object_get(credit, "remaining", &left, NULL);
		venture_money_totals_add(totals, left, NULL);
		shown = venture_money_to_display_string(left, TRUE);
		g_string_append_printf(body, "<tr><td class=\"row-title\"><a href=\"/e/customer_credit/%" G_GINT64_FORMAT "\">",
			venture_entity_get_id(credit));
		venture_html_escape_append(body, name);
		g_string_append_printf(body, "</a></td><td class=\"num\">%s</td><td>", shown);
		if (0 == invoices->len)
		{
			g_string_append(body, "<span class=\"muted\">No open invoice</span></td></tr>");
			continue;
		}
		customer_credits_append_form_start(body, credit, back);
		g_string_append(body, "<select name=\"invoice_id\" aria-label=\"Invoice\">");
		for (j = 0; j < invoices->len; j++)
		{
			VentureEntity *invoice = g_ptr_array_index(invoices, j);
			g_autofree gchar *number = NULL;

			g_object_get(invoice, "number", &number, NULL);
			g_string_append_printf(body, "<option value=\"%" G_GINT64_FORMAT "\"%s>",
				venture_entity_get_id(invoice), (0 == j) ? " selected" : "");
			venture_html_escape_append(body, number);
			g_string_append(body, "</option>");
		}
		g_string_append(body, "</select> ");
		offer = customer_credits_offer(self, credit, g_ptr_array_index(invoices, 0));
		customer_credits_append_amount(body, offer);
		g_string_append(body, "</td></tr>");
	}
	g_string_append(body, "</tbody></table></div>");
	total = customer_account_totals_text(totals);
	g_string_append_printf(figures, "<dt>Credit</dt><dd>%s</dd>", total);
}

/* On an open invoice: the customer's credits, each ready to apply. */
static void
venture_customer_credits_web_append_invoice(VentureWebServer *self, GString *html, VentureEntity *invoice)
{
	g_autoptr(VentureEntity) company = NULL;
	g_autoptr(GPtrArray) credits = NULL;
	g_autofree gchar *back = NULL;
	gint64 company_id = 0;
	gint status = VENTURE_INVOICE_STATUS_DRAFT;
	guint i;

	if (!VENTURE_IS_INVOICE(invoice) || (venture_entity_get_id(invoice) <= 0))
		return;
	g_object_get(invoice, "status", &status, "company-id", &company_id, NULL);
	if (((VENTURE_INVOICE_STATUS_SENT != status) && (VENTURE_INVOICE_STATUS_PARTIALLY_PAID != status)) ||
	    (company_id <= 0))
		return;
	company = venture_database_get(venture_context_get_database(self->context), VENTURE_TYPE_COMPANY, company_id, NULL);
	if (NULL == company)
		return;
	credits = customer_credits_unused(self, company);
	if (0 == credits->len)
		return;
	back = g_strdup_printf("/e/invoice/%" G_GINT64_FORMAT, venture_entity_get_id(invoice));
	g_string_append(html, "<section class=\"card customer-account invoice-credits\"><div class=\"card-head\">"
		"<h2>Credit to apply</h2></div><div class=\"card-body\"><div class=\"table-wrap\"><table class=\"data\">"
		"<thead><tr><th>Credit</th><th class=\"num\">Left</th><th>Apply</th></tr></thead><tbody>");
	for (i = 0; i < credits->len; i++)
	{
		VentureEntity *credit = g_ptr_array_index(credits, i);
		g_autoptr(VentureMoney) left = NULL;
		g_autoptr(VentureMoney) offer = customer_credits_offer(self, credit, invoice);
		g_autofree gchar *shown = NULL;
		g_autofree gchar *name = venture_entity_get_display_name(credit);

		if (NULL == offer)
			continue;
		g_object_get(credit, "remaining", &left, NULL);
		shown = venture_money_to_display_string(left, TRUE);
		g_string_append_printf(html, "<tr><td class=\"row-title\"><a href=\"/e/customer_credit/%" G_GINT64_FORMAT "\">",
			venture_entity_get_id(credit));
		venture_html_escape_append(html, name);
		g_string_append_printf(html, "</a></td><td class=\"num\">%s</td><td>", shown);
		customer_credits_append_form_start(html, credit, back);
		g_string_append_printf(html, "<input type=\"hidden\" name=\"invoice_id\" value=\"%" G_GINT64_FORMAT "\">",
			venture_entity_get_id(invoice));
		customer_credits_append_amount(html, offer);
		g_string_append(html, "</td></tr>");
	}
	g_string_append(html, "</tbody></table></div></div></section>");
}

static gint64
customer_credits_id(const gchar *text)
{
	return (NULL != text) ? g_ascii_strtoll(text, NULL, 10) : 0;
}

/* Where to go after applying: the page it came from, if it is one of
 * ours, else the invoice. */
static gchar *
customer_credits_back(const gchar *back, gint64 invoice_id)
{
	if ((NULL != back) && ('/' == back[0]) && ('/' != back[1]) && ('\\' != back[1]) &&
	    (NULL == strpbrk(back, "\r\n")))
		return g_strdup(back);
	return g_strdup_printf("/e/invoice/%" G_GINT64_FORMAT, invoice_id);
}

/*
 * POST /customer-credits/apply: credit_id, invoice_id, amount, back.
 * Refused, with nothing applied, when the two belong to different
 * customers or businesses, or the amount is not positive or exceeds what
 * the credit holds or what the invoice owes.
 */
static HtmxResponse *
venture_customer_credits_web_apply(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	VentureWebServer *self = user_data;
	VentureDatabase *database = venture_context_get_database(self->context);
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) credit = NULL;
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(VentureEntity) allocation = NULL;
	g_autoptr(VentureMoney) left = NULL;
	g_autoptr(VentureMoney) owed = NULL;
	g_autoptr(VentureMoney) amount = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *destination = NULL;
	VentureActor actor;
	HtmxResponse *redirect;
	gint64 credit_id, invoice_id, credit_customer = 0, invoice_customer = 0;
	(void)params;

	redirect = venture_web_ui_require_session(self, request);
	if (NULL != redirect)
		return redirect;
	principal = venture_auth_authenticate(self->auth, request);
	if (!venture_web_require_for_type(self, principal, VENTURE_TYPE_PAYMENT_ALLOCATION,
	                                  VENTURE_USER_ROLE_EDITOR, &error))
		return venture_web_error_response(error);
	credit_id = customer_credits_id(htmx_request_get_form_value(request, "credit_id"));
	invoice_id = customer_credits_id(htmx_request_get_form_value(request, "invoice_id"));
	credit = venture_database_get(database, VENTURE_TYPE_CUSTOMER_CREDIT, credit_id, NULL);
	invoice = venture_database_get(database, VENTURE_TYPE_INVOICE, invoice_id, NULL);
	if ((NULL == credit) || (NULL == invoice))
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "No such credit or invoice");
		return venture_web_error_response(error);
	}
	g_object_get(credit, "customer-id", &credit_customer, "remaining", &left, NULL);
	g_object_get(invoice, "company-id", &invoice_customer, NULL);
	if ((credit_customer != invoice_customer) ||
	    (venture_entity_get_organization_id(credit) != venture_entity_get_organization_id(invoice)))
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"A credit applies only to the same customer's invoices");
		return venture_web_error_response(error);
	}
	owed = venture_settlement_service_invoice_balance(venture_settlement_service_get(database), invoice_id, NULL, &error);
	if ((NULL == left) || (NULL == owed))
		return venture_web_error_response(error);
	amount = venture_money_from_string(htmx_request_get_form_value(request, "amount"),
		venture_money_get_currency(left), &error);
	if (NULL == amount)
		return venture_web_error_response(error);
	if (venture_money_is_zero(amount) || venture_money_is_negative(amount) ||
	    (venture_money_compare(amount, left) > 0) || (venture_money_compare(amount, owed) > 0))
	{
		g_clear_error(&error);
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"Apply more than nothing, and no more than the credit holds or the invoice owes");
		return venture_web_error_response(error);
	}
	allocation = g_object_new(VENTURE_TYPE_PAYMENT_ALLOCATION, "credit-id", credit_id,
		"invoice-id", invoice_id, "amount", amount, "date", now, NULL);
	venture_entity_set_organization_id(allocation, venture_entity_get_organization_id(invoice));
	venture_auth_to_actor(principal, &actor);
	if (!venture_database_save(database, allocation, &actor, &error))
		return venture_web_error_response(error);
	destination = customer_credits_back(htmx_request_get_form_value(request, "back"), invoice_id);
	redirect = venture_web_redirect_to(destination);
	htmx_response_set_status(redirect, 303);
	return redirect;
}

static void
venture_customer_credits_web_register(VentureWebServer *self)
{
	venture_web_server_add_classified_route(self, HTMX_METHOD_POST, "/customer-credits/apply",
		VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, venture_customer_credits_web_apply, self);
}
