/* SPDX-License-Identifier: AGPL-3.0-or-later */

/* A subscription's state as its customer would say it. */
static const gchar *
venture_portal_subscription_state(gint status, gboolean ending)
{
	if (ending && status < 3)
		return "Ends at renewal";
	switch (status)
	{
	case 0: return "On a free trial";
	case 1: return "Active";
	case 2: return "Payment overdue";
	case 3: return "Paused";
	case 4: return "Cancelled";
	default: return "Ended";
	}
}

/* "Starter, $30.00 a month" -- a price under its plan's name. */
static void
venture_portal_append_price(VentureDatabase *database, GString *html, VentureEntity *price)
{
	g_autoptr(VentureEntity) plan = NULL;
	g_autofree gchar *plan_name = NULL, *price_name = venture_entity_get_display_name(price);
	gint64 plan_id = 0;
	g_object_get(price, "plan-id", &plan_id, NULL);
	plan = venture_database_get(database, VENTURE_TYPE_PLAN, plan_id, NULL);
	if (plan != NULL)
		g_object_get(plan, "name", &plan_name, NULL);
	venture_html_escape_append(html, plan_name != NULL ? plan_name : "Plan");
	g_string_append(html, ", ");
	venture_html_escape_append(html, price_name);
}

/*
 * The customer's subscriptions: what they are on, when it renews and for
 * how much, the invoices it has issued, and the two things they may do --
 * switch price or cancel at renewal. Every figure comes from the billing
 * service, the same numbers the operator's page and the notices give.
 */
static void
venture_portal_append_subscriptions(VentureWebServer *self, GString *html, VentureEntity *access, const gchar *token)
{
	VentureDatabase *database = venture_context_get_database(self->context);
	VenturePortalService *portal = venture_portal_service_get(database);
	g_autoptr(GPtrArray) subs = venture_portal_service_subscriptions(portal, VENTURE_CUSTOMER_PORTAL_ACCESS(access), NULL);
	GTimeZone *zone = venture_context_get_timezone(self->context);
	guint i;
	if (subs == NULL || subs->len == 0)
		return;
	g_string_append(html, "<h1>Your subscriptions</h1>");
	for (i = 0; i < subs->len; i++)
	{
		VentureEntity *sub = g_ptr_array_index(subs, i);
		g_autoptr(VentureEntity) price = NULL, pending = NULL;
		g_autoptr(GDateTime) renews = NULL;
		g_autoptr(VentureMoney) next = NULL;
		g_autoptr(GPtrArray) offered = NULL;
		g_autoptr(VentureQuery) events_query = NULL;
		g_autoptr(GPtrArray) events = NULL;
		g_autofree gchar *day = NULL;
		gint64 id = venture_entity_get_id(sub), price_id = 0, pending_id = 0, seats = 1;
		gint status = 0;
		gboolean ending = FALSE, per_seat = FALSE, live;
		guint j, shown = 0;
		g_object_get(sub, "plan-price-id", &price_id, "pending-plan-price-id", &pending_id, "seats", &seats,
			"status", &status, "cancel-at-period-end", &ending, "current-period-end", &renews, NULL);
		price = venture_database_get(database, VENTURE_TYPE_PLAN_PRICE, price_id, NULL);
		if (pending_id > 0)
			pending = venture_database_get(database, VENTURE_TYPE_PLAN_PRICE, pending_id, NULL);
		day = renews != NULL ? venture_time_to_date_string(renews, zone) : NULL;
		live = status < 3 && !ending;
		g_string_append(html, "<section><h2>");
		if (price != NULL)
		{
			venture_portal_append_price(database, html, price);
			g_object_get(price, "per-seat", &per_seat, NULL);
		}
		if (per_seat)
			g_string_append_printf(html, " \xc3\x97 %" G_GINT64_FORMAT " seat%s", seats, seats == 1 ? "" : "s");
		g_string_append(html, "</h2><p>");
		g_string_append(html, venture_portal_subscription_state(status, ending));
		g_string_append(html, ".</p>");
		next = venture_billing_service_next_invoice(venture_billing_service_get(database),
			VENTURE_CUSTOMER_SUBSCRIPTION(sub), NULL);
		if (next != NULL && day != NULL)
		{
			g_autofree gchar *amount = venture_money_to_display_string(next, TRUE);
			g_string_append(html, "<p>Next renewal: ");
			venture_html_escape_append(html, day);
			g_string_append(html, ", for ");
			venture_html_escape_append(html, amount);
			g_string_append(html, ".</p>");
		}
		else if (ending && day != NULL)
		{
			g_string_append(html, "<p>Ends on ");
			venture_html_escape_append(html, day);
			g_string_append(html, ". No further base charges; final usage, if any, is invoiced when it ends.</p>");
		}
		if (pending != NULL)
		{
			g_string_append(html, "<p>Switches to ");
			venture_portal_append_price(database, html, pending);
			g_string_append(html, " at renewal.</p>");
		}

		/* The invoices it has issued, read from its events. */
		events_query = venture_query_new(VENTURE_TYPE_SUBSCRIPTION_EVENT);
		venture_query_set_limit(events_query, 0);
		venture_query_add_filter_int(events_query, "subscription-id", VENTURE_FILTER_OP_EQ, id, NULL);
		venture_query_add_order(events_query, "id", VENTURE_SORT_DESCENDING, NULL);
		events = venture_database_find(database, events_query, NULL);
		for (j = 0; events != NULL && j < events->len; j++)
		{
			g_autoptr(VentureEntity) invoice = NULL;
			g_autoptr(VentureMoney) balance = NULL;
			g_autofree gchar *number = NULL, *owed = NULL;
			gint64 invoice_id = 0;
			g_object_get(g_ptr_array_index(events, j), "invoice-id", &invoice_id, NULL);
			if (invoice_id == 0)
				g_object_get(g_ptr_array_index(events, j), "final-invoice-id", &invoice_id, NULL);
			if (invoice_id == 0)
				continue;
			invoice = venture_database_get(database, VENTURE_TYPE_INVOICE, invoice_id, NULL);
			if (invoice == NULL)
				continue;
			if (shown++ == 0)
				g_string_append(html, "<table><tr><th>Invoice</th><th>Still owed</th></tr>");
			g_object_get(invoice, "number", &number, NULL);
			balance = venture_settlement_service_invoice_balance(venture_settlement_service_get(database), invoice_id, NULL, NULL);
			owed = balance != NULL ? venture_money_to_display_string(balance, TRUE) : g_strdup("");
			g_string_append(html, "<tr><td>");
			venture_html_escape_append(html, number != NULL ? number : "");
			g_string_append(html, "</td><td>");
			venture_html_escape_append(html, owed);
			g_string_append(html, "</td></tr>");
		}
		if (shown > 0)
			g_string_append(html, "</table>");

		if (!live)
		{
			g_string_append(html, "</section>");
			continue;
		}
		offered = venture_portal_service_offered_prices(portal, VENTURE_CUSTOMER_PORTAL_ACCESS(access), id, NULL);
		if (offered != NULL && offered->len > 0)
		{
			g_string_append(html, "<form method=\"post\" action=\"/portal/");
			venture_html_escape_append(html, token);
			g_string_append_printf(html, "/subscriptions/%" G_GINT64_FORMAT "\">"
				"<input type=\"hidden\" name=\"action\" value=\"change\">"
				"<label>Switch to <select name=\"plan_price_id\" required>", id);
			for (j = 0; j < offered->len; j++)
			{
				VentureEntity *choice = g_ptr_array_index(offered, j);
				g_string_append_printf(html, "<option value=\"%" G_GINT64_FORMAT "\">", venture_entity_get_id(choice));
				venture_portal_append_price(database, html, choice);
				g_string_append(html, "</option>");
			}
			g_string_append(html, "</select></label> <label><input type=\"radio\" name=\"when\" value=\"renewal\" checked> At renewal");
			if (day != NULL)
			{
				g_string_append(html, " on ");
				venture_html_escape_append(html, day);
			}
			g_string_append(html, "</label> <label><input type=\"radio\" name=\"when\" value=\"now\"> Now; the difference for "
				"the rest of this period goes on your next invoice</label> <button>Switch plan</button></form>");
		}
		g_string_append(html, "<form method=\"post\" action=\"/portal/");
		venture_html_escape_append(html, token);
		g_string_append_printf(html, "/subscriptions/%" G_GINT64_FORMAT "\">"
			"<input type=\"hidden\" name=\"action\" value=\"cancel\">"
			"<button>Cancel at renewal</button> You keep it until then; no further base charges. Final usage, if any, is invoiced when it ends.</form></section>", id);
	}
}

/* POST /portal/:token/subscriptions/:id -- a customer's own plan change or
 * cancellation. The token is the credential; the service answers NOT_FOUND
 * for a subscription that is not this customer's. */
static HtmxResponse *
venture_portal_subscription_web(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	VentureWebServer *self = user_data;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *location = NULL;
	const gchar *token = g_hash_table_lookup(params, "token");
	const gchar *id_text = g_hash_table_lookup(params, "id");
	const gchar *action = htmx_request_get_form_value(request, "action");
	const gchar *price = htmx_request_get_form_value(request, "plan_price_id");
	const gchar *when = htmx_request_get_form_value(request, "when");
	HtmxResponse *response;
	VentureActor actor;
	gint64 id = 0;
	if (id_text != NULL)
	{
		gchar *end = NULL;
		id = g_ascii_strtoll(id_text, &end, 10);
		if (end == NULL || *end != '\0')
			id = 0;
	}
	actor.kind = VENTURE_ACTOR_KIND_USER;
	actor.name = "customer portal";
	actor.prompt = NULL;
	actor.request_id = NULL;
	actor.approved_by = NULL;
	if (!venture_portal_service_manage_subscription(venture_portal_service_get(venture_context_get_database(self->context)),
		token, id, action, price != NULL ? g_ascii_strtoll(price, NULL, 10) : 0, g_strcmp0(when, "now") == 0, &actor, &error))
	{
		if (g_error_matches(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND))
		{
			response = htmx_response_new_with_content("Not found");
			htmx_response_set_status(response, 404);
			return response;
		}
		return venture_web_error_response(error);
	}
	location = g_strdup_printf("/portal/%s", token);
	response = htmx_response_new();
	htmx_response_set_status(response, 303);
	htmx_response_add_header(response, "Location", location);
	return response;
}
static HtmxResponse *
venture_portal_web(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	VentureWebServer *self = user_data;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) access = NULL;
	g_autoptr(GPtrArray) invoices = NULL;
	g_autoptr(GString) html = g_string_new("<!doctype html><html><head><meta charset=\"utf-8\"><title>Customer portal</title></head><body>");
	const gchar *token = g_hash_table_lookup(params, "token");
	guint i;
	access = venture_portal_service_lookup(venture_portal_service_get(venture_context_get_database(self->context)), token, &error);
	if (access == NULL)
	{
		HtmxResponse *response = htmx_response_new_with_content("Not found");
		htmx_response_set_status(response, 404);
		return response;
	}
	if (htmx_request_get_method(request) == HTMX_METHOD_POST)
	{
		const gchar *invoice = htmx_request_get_form_value(request, "invoice_id");
		g_autoptr(VentureStripeCheckout) checkout = NULL;
		g_autofree gchar *url = NULL;
		HtmxResponse *response;
		VentureActor actor;
		actor.kind = VENTURE_ACTOR_KIND_USER;
		actor.name = "customer portal";
		actor.prompt = NULL;
		actor.request_id = NULL;
		actor.approved_by = NULL;
		checkout = venture_portal_service_checkout(
			venture_portal_service_get(venture_context_get_database(self->context)),
			venture_context_get_stripe_service(self->context), token,
			invoice ? g_ascii_strtoll(invoice, NULL, 10) : 0, &actor, &error);
		if (checkout == NULL)
			return venture_web_error_response(error);
		g_object_get(checkout, "url", &url, NULL);
		response = htmx_response_new();
		htmx_response_set_status(response, 303);
		htmx_response_add_header(response, "Location", url);
		return response;
	}
	invoices = venture_portal_service_invoices(venture_portal_service_get(venture_context_get_database(self->context)),
		VENTURE_CUSTOMER_PORTAL_ACCESS(access), &error);
	if (invoices == NULL)
		return venture_web_error_response(error);
	g_string_append(html, "<h1>Your invoices</h1><table>");
	for (i = 0; i < invoices->len; i++)
	{
		VentureEntity *invoice = g_ptr_array_index(invoices, i);
		g_autofree gchar *number = NULL;
		gint status;
		g_object_get(invoice, "number", &number, "status", &status, NULL);
		g_string_append(html, "<tr><td>");
		venture_html_escape_append(html, number);
		g_string_append(html, "</td><td>");
		if (status == VENTURE_INVOICE_STATUS_SENT || status == VENTURE_INVOICE_STATUS_PARTIALLY_PAID)
			g_string_append_printf(html, "<form method=\"post\"><input type=\"hidden\" name=\"invoice_id\" value=\"%" G_GINT64_FORMAT "\">"
				"<button>Pay with Checkout</button></form>",
				venture_entity_get_id(invoice));
		g_string_append(html, "</td><td>");
		{
			g_autofree gchar *reminders = venture_dunning_portal_summary(venture_context_get_database(self->context), invoice);
			venture_html_escape_append(html, reminders);
		}
		g_string_append(html, "</td></tr>");
	}
	g_string_append(html, "</table>");
	venture_portal_append_subscriptions(self, html, access, token);
	g_string_append(html, "</body></html>");
	return htmx_response_new_with_content(html->str);
}

/* Both invitation routes use the same private outbox operation. */
static HtmxResponse *
venture_portal_invite_request(VentureWebServer *self, HtmxRequest *request, gboolean supplier)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) body = NULL;
	g_autoptr(JsonNode) output = NULL;
	g_autoptr(VentureEntity) access = NULL;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autofree gchar *base_url = NULL;
	VentureActor actor;
	JsonObject *object;
	JsonNode *company;
	gint64 company_id = 0;
	HtmxResponse *gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_EDITOR);
	if (gate != NULL) return gate;
	gate = venture_web_require_module_api(self, supplier ? "supplier_portal" : "receivables");
	if (gate != NULL) return gate;
	body = htmx_request_get_json(request, &error);
	if (body == NULL || !JSON_NODE_HOLDS_OBJECT(body))
	{
		if (error == NULL)
			g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Expected an invitation object");
		return venture_web_error_response(error);
	}
	object = json_node_get_object(body);
	company = json_object_get_member(object, "company_id");
	if (company != NULL && JSON_NODE_HOLDS_VALUE(company))
	{
		if (json_node_get_value_type(company) == G_TYPE_STRING)
			company_id = g_ascii_strtoll(json_node_get_string(company), NULL, 10);
		else if (json_node_get_value_type(company) == G_TYPE_INT64)
			company_id = json_node_get_int(company);
	}
	principal = venture_auth_authenticate(self->auth, request);
	venture_auth_to_actor(principal, &actor);
	g_object_get(venture_context_get_config(self->context), "server-base-url", &base_url, NULL);
	access = venture_portal_service_send_invitation(venture_portal_service_get(venture_context_get_database(self->context)),
		base_url, supplier, venture_web_active_organization(self, request), company_id,
		venture_json_object_get_string(object, "email", NULL), &actor, &error);
	if (access == NULL) return venture_web_error_response(error);
	output = venture_serializable_to_json(VENTURE_SERIALIZABLE(access), FALSE);
	return venture_web_json_response(output, 201);
}

static HtmxResponse *
venture_customer_portal_invite_api(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	(void)params;
	return venture_portal_invite_request(user_data, request, FALSE);
}

static void
venture_portal_web_register(HtmxRouter *router, VentureWebServer *self)
{
	(void)router;
	venture_web_server_add_classified_route(self, HTMX_METHOD_GET, "/portal/:token", VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, venture_portal_web, self);
	venture_web_server_add_classified_route(self, HTMX_METHOD_POST, "/portal/:token", VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, venture_portal_web, self);
	venture_web_server_add_classified_route(self, HTMX_METHOD_POST, "/portal/:token/subscriptions/:id", VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, venture_portal_subscription_web, self);
	venture_web_server_add_classified_route(self, HTMX_METHOD_POST, "/api/v1/customer_portal/invite", VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, venture_customer_portal_invite_api, self);
}
