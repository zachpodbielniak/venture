/* Billing action adapters, included after the shared API staging helpers. */
static HtmxResponse *
venture_billing_web_action(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = venture_auth_authenticate(self->auth, request);
	g_autoptr(VentureEntity) instruction = g_object_new(VENTURE_TYPE_BILLING_REQUEST, NULL);
	g_autoptr(VentureEntity) sub = NULL;
	g_autoptr(JsonNode) body = NULL;
	g_autoptr(JsonNode) result = NULL;
	g_autoptr(GDateTime) now = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *id_text = g_hash_table_lookup(params, "id");
	const gchar *verb = g_hash_table_lookup(params, "action");
	const gchar *form_action = htmx_request_get_form_value(request, "billing_action");
	gboolean ui = g_str_has_prefix(htmx_request_get_path(request), "/billing/");
	gboolean stage = FALSE;
	VentureActor actor;
	HtmxResponse *gate;
	gint64 id = 0;
	gint64 org;
	if (ui)
	{
		gate = venture_web_ui_require_session(self, request);
		if (gate != NULL)
			return gate;
	}
	if (!venture_web_hosted_auth_require(self, request, principal, VENTURE_USER_ROLE_EDITOR, &error))
		return venture_web_error_response(error);
	gate = venture_web_require_module_api(self, "billing");
	if (gate != NULL)
		return gate;
	if (ui)
	{
		static const gchar *const fields[] = { "at", "plan-price-id", "seats", "at-period-end", "company-id", "contact-id",
			"discount-id", "discount-code", "skip-trial" };
		guint i;
		verb = form_action;
		for (i = 0; i < G_N_ELEMENTS(fields); i++)
		{
			const gchar *value = htmx_request_get_form_value(request, fields[i]);
			if (value != NULL && *value != '\0' && !venture_entity_set_field_from_string(instruction, fields[i], value, &error))
				return venture_web_error_response(error);
		}
	}
	else
	{
		body = htmx_request_get_json(request, &error);
		if (body == NULL || !JSON_NODE_HOLDS_OBJECT(body))
		{
			g_clear_error(&error);
			g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Expected a billing action JSON object");
			return venture_web_error_response(error);
		}
		if (!venture_serializable_from_json(VENTURE_SERIALIZABLE(instruction), body, &error) ||
			!venture_web_stage_requested(request, &stage, &error))
			return venture_web_error_response(error);
	}
	if (verb == NULL)
		verb = "start";
	if (id_text != NULL)
	{
		gchar *end;
		id = g_ascii_strtoll(id_text, &end, 10);
		if (*end != '\0' || id <= 0)
		{
			g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Invalid subscription ID");
			return venture_web_error_response(error);
		}
		sub = venture_database_get(venture_context_get_database(self->context), VENTURE_TYPE_CUSTOMER_SUBSCRIPTION, id, &error);
		if (sub == NULL)
			return venture_web_error_response(error);
		org = venture_entity_get_organization_id(sub);
		g_object_set(instruction, "expected-version", venture_entity_get_version(sub), NULL);
	}
	else
	{
		org = venture_entity_get_organization_id(instruction);
		/* A subscription started from the page belongs to the business
		 * the page is showing. */
		if (org == 0 && ui)
			org = venture_web_active_organization(self, request);
		if (org == 0)
			org = venture_context_get_default_organization_id(self->context);
	}
	/* Route identity wins over body fields; result and identity fields are
	 * never accepted as authorization to modify an existing request. */
	g_object_set(instruction, "id", (gint64)0, "version", (gint64)0,
		"organization-id", org, "subscription-id", id, "action", verb,
		"invoice-id", (gint64)0, "processed", (gint64)0, NULL);
	g_object_get(instruction, "at", &now, NULL);
	if (now == NULL)
	{
		now = venture_time_now();
		g_object_set(instruction, "at", now, NULL);
	}
	if (stage)
		return venture_web_api_stage(self, instruction, NULL, principal, VENTURE_AUDIT_ACTION_CREATE);
	venture_auth_to_actor(principal, &actor);
	if (!venture_billing_service_execute(venture_billing_service_get(venture_context_get_database(self->context)),
		VENTURE_BILLING_REQUEST(instruction), &actor, &error))
		return venture_web_error_response(error);
	if (ui)
	{
		g_autofree gchar *path = NULL;
		/* A start has no id until the service made one. */
		if (id == 0)
			g_object_get(instruction, "subscription-id", &id, NULL);
		path = g_strdup_printf("/e/customer_subscription/%" G_GINT64_FORMAT, id);
		HtmxResponse *response = htmx_response_new();
		htmx_response_set_status(response, 303);
		htmx_response_add_header(response, "Location", path);
		return response;
	}
	result = venture_serializable_to_json(VENTURE_SERIALIZABLE(instruction), FALSE);
	return venture_web_json_response(result, 200);
}

/*
 * GET /billing/subscriptions/new -- put a customer on a plan. The page says
 * what a subscription is for, so nobody reaches for it when they meant to
 * repeat an invoice.
 */
static HtmxResponse *
venture_billing_web_new(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	VentureWebServer *self = user_data;
	g_autoptr(GString) html = g_string_new(NULL);
	g_autoptr(GPtrArray) unpriced = g_ptr_array_new_with_free_func(g_object_unref);
	HtmxResponse *gate;
	gint64 organization;
	guint i;
	(void)params;
	gate = venture_web_ui_require_session(self, request);
	if (gate != NULL)
		return gate;
	gate = venture_web_require_module_ui(self, request, "billing");
	if (gate != NULL)
		return gate;
	organization = venture_web_active_organization(self, request);
	g_string_append(html, "<form method=\"post\" action=\"/billing/subscriptions/start\">"
		"<input type=\"hidden\" name=\"billing_action\" value=\"start\">"
		"<div class=\"page-head\"><div class=\"page-title\">");
	venture_web_append_crumbs(html, VENTURE_TYPE_CUSTOMER_SUBSCRIPTION, "customer_subscription", TRUE);
	g_string_append(html, "<h1>New subscription</h1><span class=\"subtitle\">Put a customer on a plan. "
		"VENTURE invoices each period, handles seats and trials, and works out the difference when the plan "
		"changes part-way through a period.</span></div><div class=\"page-actions\">"
		"<a class=\"btn btn-ghost\" href=\"/e/customer_subscription\">Cancel</a>"
		"<button class=\"btn btn-primary\" type=\"submit\">Start subscription</button></div></div>"
		"<div class=\"doc-layout\"><section class=\"card\"><div class=\"card-body\"><div class=\"doc-parties\">");
	document_append_parties(self, html, organization);
	g_string_append(html, "<label class=\"doc-field\"><span class=\"field-label\">Seats</span>"
		"<input type=\"number\" name=\"seats\" value=\"1\" min=\"1\" step=\"1\"></label></div>"
		"<label class=\"doc-field\"><span class=\"field-label\">Plan<span class=\"required\">*</span></span>"
		"<select name=\"plan-price-id\" required><option value=\"\">Choose a plan\xe2\x80\xa6</option>");
	venture_plan_web_append_price_options(self, html, organization, 0, 0, unpriced);
	g_string_append(html, "</select><span class=\"field-help\">Every offered price, under its plan. "
		"<a href=\"/plans/new\">Make a new plan</a>.</span></label>"
		"<div class=\"doc-parties\"><label class=\"doc-field\"><span class=\"field-label\">Discount</span>"
		"<select name=\"discount-id\" data-plan-discounts><option value=\"\">No discount</option>");
	venture_plan_web_append_discount_options(self, html, organization);
	/* A code sits beside the offers because that is how a customer names
	 * one: they quote it, and nobody should have to find which offer it is. */
	g_string_append(html, "</select><span class=\"field-help\">The chosen plan's offers.</span></label>"
		"<label class=\"doc-field\"><span class=\"field-label\">Have a code?</span>"
		"<input type=\"text\" name=\"discount-code\" autocomplete=\"off\" spellcheck=\"false\">"
		"<span class=\"field-help\">A code the customer quoted, in any case. Use it or the list, not both.</span></label></div>"
		"<label class=\"checkbox\" data-skip-trial hidden><input type=\"checkbox\" name=\"skip-trial\" value=\"true\"> "
		"<span data-skip-trial-label>Skip the free trial and send the first invoice now</span></label>"
		"<p class=\"field-help\" data-first-bill>The first invoice is issued the moment the subscription starts, "
		"unless the price has a free trial.</p>");
	/* A plan that is offered but has no price cannot be sold; say so
	 * where somebody is looking for it. */
	for (i = 0; i < unpriced->len; i++)
	{
		VentureEntity *plan = g_ptr_array_index(unpriced, i);
		g_autofree gchar *name = venture_entity_get_display_name(plan);
		g_string_append(html, "<p class=\"field-help\"><strong>");
		venture_html_escape_append(html, name);
		g_string_append_printf(html, "</strong> has no price yet \xe2\x80\x94 "
			"<a href=\"/e/plan/%" G_GINT64_FORMAT "\">add one</a> to offer it.</p>", venture_entity_get_id(plan));
	}
	g_string_append(html, "</div></section>"
		"<aside class=\"doc-aside\"><section class=\"card\"><div class=\"card-body explainer\">"
		"<h3>Subscription or repeating invoice?</h3>"
		"<p><strong>A subscription</strong> is a customer on a plan: a price a month, a quarter or a year, maybe per seat, "
		"maybe a free trial. Change the plan mid-month and the difference for the days left is added to the next "
		"invoice, or credited.</p><p><strong>A repeating invoice</strong> sends the same lines on a schedule "
		"\xe2\x80\x94 rent, a retainer, hosting. Editing it changes the invoices still to come; nothing is "
		"prorated. <a href=\"/invoices/compose?repeat=1\">Repeat an invoice instead</a>.</p>"
		"</div></section></aside></div></form>"
		/* Choosing a customer narrows the plans to their venture's and
		 * the shared ones; the service refuses anything else anyway. */
		"<script>(function(){var c=document.querySelector('[data-doc-company]'),"
		"p=document.querySelector('select[name=plan-price-id]');if(!c||!p)return;"
		"function narrow(){var o=c.selectedOptions[0],v=o&&o.dataset.venture||'0';"
		"Array.prototype.forEach.call(p.options,function(x){if(!x.value)return;"
		"var mine=v==='0'||x.dataset.venture==='0'||x.dataset.venture===v;x.hidden=!mine;x.disabled=!mine;});"
		"if(p.selectedOptions[0]&&p.selectedOptions[0].hidden){p.value='';}"
		"p.dispatchEvent(new Event('change',{bubbles:true}));}"
		"c.addEventListener('change',narrow);narrow();"
		/* A plan's discounts, and the trial choice when its price has one. */
		"var d=document.querySelector('[data-plan-discounts]'),t=document.querySelector('[data-skip-trial]'),"
		"b=document.querySelector('[data-first-bill]');"
		"function plan(){var o=p.selectedOptions[0],id=o&&o.value?o.dataset.plan:'',days=o&&o.value?+o.dataset.trial:0;"
		"if(d){Array.prototype.forEach.call(d.options,function(x){if(!x.value)return;var mine=x.dataset.plan===id;"
		"x.hidden=!mine;x.disabled=!mine;});if(d.selectedOptions[0]&&d.selectedOptions[0].hidden)d.value='';"
		"d.dispatchEvent(new Event('change',{bubbles:true}));}"
		"if(t){t.hidden=!(days>0);t.querySelector('[data-skip-trial-label]').textContent="
		"'Skip the '+days+'-day free trial and send the first invoice now';}"
		"if(b)b.textContent=days>0?'On the free trial, the first invoice is issued when the trial ends, after '+days+' days.'"
		":'The first invoice is issued the moment the subscription starts.';}"
		"p.addEventListener('change',plan);if(t)t.addEventListener('change',function(){var x=t.querySelector('input');"
		"if(b&&x.checked)b.textContent='The first invoice is issued the moment the subscription starts.';else plan();});plan();"
		"})();</script>");
	return venture_web_html_response(venture_web_page(self, request, "/e/customer_subscription",
		"New subscription", html->str), 200);
}

/* A date as its day, in the configured zone. */
static gchar *
venture_billing_web_day(VentureWebServer *self, GDateTime *when)
{
	return when != NULL ? venture_time_to_date_string(when, venture_context_get_timezone(self->context)) : NULL;
}

/*
 * The invoices a subscription has issued, newest first: what it has
 * billed is the question after what it will bill. Read from its events,
 * which record the invoice each start and renewal issued.
 */
static void
venture_billing_web_invoices(VentureWebServer *self, GString *html, VentureEntity *record)
{
	VentureDatabase *database = venture_context_get_database(self->context);
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_SUBSCRIPTION_EVENT);
	g_autoptr(GPtrArray) events = NULL;
	guint i, shown = 0;

	venture_query_set_limit(query, 0);
	venture_query_add_filter_int(query, "subscription-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(record), NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_DESCENDING, NULL);
	events = venture_database_find(database, query, NULL);
	for (i = 0; events != NULL && i < events->len; i++)
	{
		g_autoptr(VentureEntity) invoice = NULL;
		g_autoptr(VentureMoney) balance = NULL;
		g_autofree gchar *name = NULL, *issued_text = NULL, *balance_text = NULL;
		g_autoptr(GDateTime) issued = NULL;
		gint64 invoice_id = 0;

		g_object_get(g_ptr_array_index(events, i), "invoice-id", &invoice_id, NULL);
		if (invoice_id == 0)
			continue;
		invoice = venture_database_get(database, VENTURE_TYPE_INVOICE, invoice_id, NULL);
		if (invoice == NULL)
			continue;
		if (shown == 0)
			g_string_append(html, "<section class=\"card\"><div class=\"card-head\"><h2>Invoices</h2></div>"
				"<div class=\"card-body\"><table class=\"line-items\"><thead><tr><th>Invoice</th><th>Issued</th>"
				"<th class=\"num\">Still owed</th></tr></thead><tbody>");
		name = venture_entity_get_display_name(invoice);
		g_object_get(invoice, "issued-at", &issued, NULL);
		issued_text = venture_billing_web_day(self, issued);
		balance = venture_settlement_service_invoice_balance(venture_settlement_service_get(database), invoice_id, NULL, NULL);
		balance_text = balance != NULL ? venture_money_to_display_string(balance, TRUE) : g_strdup("\xe2\x80\x94");
		g_string_append_printf(html, "<tr><td><a href=\"/e/invoice/%" G_GINT64_FORMAT "\">", invoice_id);
		venture_html_escape_append(html, name);
		g_string_append(html, "</a></td><td>");
		venture_html_escape_append(html, issued_text != NULL ? issued_text : "");
		g_string_append(html, "</td><td class=\"num\">");
		venture_html_escape_append(html, balance_text);
		g_string_append(html, "</td></tr>");
		shown++;
	}
	if (shown > 0)
		g_string_append(html, "</tbody></table></div></section>");
}

/*
 * Whether the customer has been told their trial is ending: the question
 * somebody asks before chasing a first invoice nobody expected. Read from
 * the outbox row the renewal sweep queued, which is the only record of it.
 */
static void
venture_billing_web_trial_reminder(VentureWebServer *self, GString *html, VentureEntity *record)
{
	VentureDatabase *database = venture_context_get_database(self->context);
	g_autoptr(VentureEntity) reminder = NULL;
	gint64 days = 0;

	if (!venture_context_module_enabled(self->context, "mail"))
		return;
	reminder = venture_billing_service_trial_reminder(venture_billing_service_get(database),
		VENTURE_CUSTOMER_SUBSCRIPTION(record));
	if (reminder != NULL)
	{
		g_autofree gchar *state = NULL, *to = NULL, *day = NULL;
		g_autoptr(GDateTime) sent = NULL;

		g_object_get(reminder, "state", &state, "to", &to, "sent-at", &sent, NULL);
		day = venture_billing_web_day(self, sent != NULL ? sent : venture_entity_get_created_at(reminder));
		g_string_append(html, "<p>Trial reminder: ");
		g_string_append(html, g_strcmp0(state, "sent") == 0 ? "sent to " : "queued for ");
		venture_html_escape_append(html, to != NULL ? to : "");
		if (day != NULL)
		{
			g_string_append(html, " on ");
			venture_html_escape_append(html, day);
		}
		g_string_append(html, ".</p>");
		return;
	}
	g_object_get(venture_context_get_config(self->context), "billing-trial-reminder-days", &days, NULL);
	if (days > 0)
		g_string_append_printf(html, "<p>Trial reminder: not sent yet. The renewal sweep mails the customer "
			"%" G_GINT64_FORMAT " day%s before the trial ends.</p>", days, days == 1 ? "" : "s");
	else
		g_string_append(html, "<p>Trial reminder: none will be sent; billing.trial_reminder_days is zero.</p>");
}

/*
 * A subscription page answers what the customer will be charged next and
 * when, in a sentence, before offering anything to change. Then the
 * changes, each saying what it does to the next invoice -- the question
 * everybody has about a plan change in the middle of a month.
 */
static void
venture_billing_web_buttons(VentureWebServer *self, GString *html, VentureEntity *record, VentureAuthPrincipal *principal)
{
	g_autoptr(GDateTime) period_end = NULL, trial_end = NULL, cancelled = NULL;
	g_autoptr(VentureMoney) adjustment = NULL;
	g_autoptr(VentureEntity) price = NULL, pending = NULL;
	g_autofree gchar *renews = NULL, *trial = NULL, *price_name = NULL, *pending_name = NULL;
	gint64 id, price_id = 0, pending_id = 0, seats = 1;
	gboolean cancel_at_end = FALSE, editor, trialing = FALSE;
	gint status = 0;
	guint i;

	if (!VENTURE_IS_CUSTOMER_SUBSCRIPTION(record))
		return;
	id = venture_entity_get_id(record);
	g_object_get(record, "status", &status, "current-period-end", &period_end, "trial-end", &trial_end,
		"cancelled-at", &cancelled, "cancel-at-period-end", &cancel_at_end, "pending-adjustment", &adjustment,
		"plan-price-id", &price_id, "pending-plan-price-id", &pending_id, "seats", &seats, NULL);
	renews = venture_billing_web_day(self, period_end);
	trial = venture_billing_web_day(self, trial_end);
	if (price_id > 0)
		price = venture_database_get(venture_context_get_database(self->context), VENTURE_TYPE_PLAN_PRICE, price_id, NULL);
	if (pending_id > 0)
		pending = venture_database_get(venture_context_get_database(self->context), VENTURE_TYPE_PLAN_PRICE, pending_id, NULL);
	price_name = price != NULL ? venture_entity_get_display_name(price) : NULL;
	pending_name = pending != NULL ? venture_entity_get_display_name(pending) : NULL;

	g_string_append(html, "<section class=\"card next-up\"><div class=\"card-head\"><h2>What happens next</h2></div>"
		"<div class=\"card-body\"><p class=\"next-up-lead\">");
	if (status == 4 || status == 5)
		g_string_append(html, "This subscription has ended. Nothing more will be invoiced.");
	else if (status == 3)
		g_string_append(html, "Paused. Nothing is invoiced until it is resumed.");
	else if (status == 0 && trial != NULL)
	{
		g_string_append(html, "On a free trial until <strong>");
		venture_html_escape_append(html, trial);
		g_string_append(html, "</strong>. The first invoice is issued when the trial ends.");
		trialing = TRUE;
	}
	else if (cancel_at_end && renews != NULL)
	{
		g_string_append(html, "Ends on <strong>");
		venture_html_escape_append(html, renews);
		g_string_append(html, "</strong>. No further invoice will be issued.");
	}
	else if (renews != NULL)
	{
		g_string_append(html, "Renews on <strong>");
		venture_html_escape_append(html, renews);
		g_string_append(html, "</strong>");
		if (price_name != NULL)
		{
			g_string_append(html, " at ");
			venture_html_escape_append(html, price_name);
			if (seats > 1)
				g_string_append_printf(html, " \xc3\x97 %" G_GINT64_FORMAT " seats", seats);
		}
		g_string_append(html, ".");
	}
	else
		g_string_append(html, "Not started yet.");
	g_string_append(html, "</p>");

	if (trialing)
		venture_billing_web_trial_reminder(self, html, record);
	if (pending_name != NULL)
	{
		g_string_append(html, "<p>Switches to <strong>");
		venture_html_escape_append(html, pending_name);
		g_string_append(html, "</strong> at renewal; until then the current price stands.</p>");
	}
	if (adjustment != NULL && venture_money_get_amount(adjustment) != 0)
	{
		g_autofree gchar *text = venture_money_to_display_string(adjustment, TRUE);
		g_string_append(html, "<p>");
		venture_html_escape_append(html, text);
		g_string_append(html, venture_money_get_amount(adjustment) > 0
			? " is added to the next invoice for a change made part-way through this period.</p>"
			: " is credited against the next invoice for a change made part-way through this period.</p>");
	}
	{
		g_autoptr(VentureEntity) discount = NULL;
		gint64 discount_id = 0, used = 0, periods = 0;

		g_object_get(record, "discount-id", &discount_id, "discount-periods-used", &used, NULL);
		if (discount_id > 0)
			discount = venture_database_get(venture_context_get_database(self->context), VENTURE_TYPE_PLAN_DISCOUNT,
				discount_id, NULL);
		if (discount != NULL)
		{
			g_autofree gchar *label = venture_entity_get_display_name(discount);

			g_object_get(discount, "periods", &periods, NULL);
			g_string_append(html, "<p>Discount: <strong>");
			venture_html_escape_append(html, label);
			g_string_append(html, "</strong>");
			if (periods > 0 && used >= periods)
				g_string_append(html, " \xe2\x80\x94 used up; invoices are at the full price now.");
			else if (periods > 0)
				g_string_append_printf(html, " \xe2\x80\x94 %" G_GINT64_FORMAT " of %" G_GINT64_FORMAT " invoices left.",
					periods - used, periods);
			g_string_append(html, "</p>");
		}
	}
	g_string_append(html, "</div></section>");
	venture_billing_web_invoices(self, html, record);

	editor = venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_EDITOR, NULL);
	if (!editor || status == 4 || status == 5)
		return;

	/* Changing the plan: the one decision that affects money, laid out as
	 * the two choices it is, each saying what it does. */
	{
		g_string_append_printf(html, "<section class=\"card\"><div class=\"card-head\"><h2>Change the plan</h2></div>"
			"<div class=\"card-body\"><form method=\"post\" action=\"/billing/subscriptions/%" G_GINT64_FORMAT "/action\" class=\"plan-change\">"
			"<input type=\"hidden\" name=\"billing_action\" value=\"change\">"
			"<label class=\"doc-field\"><span class=\"field-label\">New plan</span><select name=\"plan-price-id\" required>", id);
		{
			/* Only plans this customer's venture sells, or shared ones. */
			g_autoptr(VentureEntity) customer = NULL;
			gint64 company_id = 0, venture = 0;

			g_object_get(record, "company-id", &company_id, NULL);
			customer = venture_database_get(venture_context_get_database(self->context), VENTURE_TYPE_COMPANY,
				company_id, NULL);
			if (customer != NULL)
				g_object_get(customer, "venture-id", &venture, NULL);
			venture_plan_web_append_price_options(self, html, venture_entity_get_organization_id(record),
				price_id, venture, NULL);
		}
		g_string_append(html, "</select></label><fieldset class=\"choice-cards\"><legend class=\"field-label\">When</legend>"
			"<label class=\"choice-card\"><input type=\"radio\" name=\"at-period-end\" value=\"true\" checked>"
			"<span><strong>At renewal");
		if (renews != NULL)
		{
			g_string_append(html, " on ");
			venture_html_escape_append(html, renews);
		}
		g_string_append(html, "</strong><span class=\"muted\">They keep today's price until then. Nothing extra to bill.</span></span></label>"
			"<label class=\"choice-card\"><input type=\"radio\" name=\"at-period-end\" value=\"false\">"
			"<span><strong>Right now</strong><span class=\"muted\">The difference for the days left in this period is "
			"worked out to the cent and added to the next invoice \xe2\x80\x94 or credited, if the new plan costs less.</span></span></label>"
			"</fieldset><button class=\"btn btn-primary\" type=\"submit\">Change plan</button></form></div></section>");
	}

	/* The rest, in words, and only the ones this state allows. */
	g_string_append(html, "<section class=\"card record-actions\"><div class=\"card-head\"><h2>Subscription</h2></div><div class=\"card-body\">");
	{
		static const struct { const gchar *action; const gchar *label; gint when; } actions[] = {
			{ "renew", "Renew now", 1 }, { "pause", "Pause", 1 }, { "resume", "Resume", 3 }, { "cancel", "Cancel subscription", -1 },
		};
		for (i = 0; i < G_N_ELEMENTS(actions); i++)
		{
			if (actions[i].when >= 0 && actions[i].when != status && !(actions[i].when == 1 && (status == 0 || status == 2)))
				continue;
			g_string_append_printf(html, "<form method=\"post\" action=\"/billing/subscriptions/%" G_GINT64_FORMAT "/action\">"
				"<input type=\"hidden\" name=\"billing_action\" value=\"%s\"><button class=\"btn%s\" type=\"submit\">%s</button></form>",
				id, actions[i].action, g_strcmp0(actions[i].action, "cancel") == 0 ? " btn-danger" : "", actions[i].label);
		}
	}
	g_string_append(html, "</div></section>");
}
