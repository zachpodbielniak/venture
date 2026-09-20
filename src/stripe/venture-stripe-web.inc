/* SPDX-License-Identifier: AGPL-3.0-or-later */
static HtmxResponse *
venture_web_stripe_settings(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	static const gint roles[] = { VENTURE_ORGANIZATION_ROLE_OWNER, VENTURE_ORGANIZATION_ROLE_ADMIN };
	static const gchar *const fields[] = { "secret_key", "publishable_key", "webhook_secret", "api_version", "success_url", "cancel_url", "environment" };
	static const gchar *const labels[] = { "Secret API key", "Publishable API key", "Webhook signing secret", "API version", "Success URL", "Cancel URL", "Environment" };
	VentureWebServer *self = user_data;
	VentureDatabase *database = venture_context_get_database(self->context);
	VentureIntegrationService *integrations = venture_integration_service_get(database);
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureIntegrationConnection) connection = NULL;
	g_autoptr(VentureEntity) organization = NULL;
	g_autoptr(GString) content = g_string_new("<h1>Stripe settings</h1>");
	g_autoptr(GError) error = NULL;
	g_autofree gchar *account = NULL;
	g_autofree gchar *environment = NULL;
	gint64 id = g_ascii_strtoll(g_hash_table_lookup(params, "id"), NULL, 10);
	gint64 version = 0;
	guint i;
	HtmxResponse *gate = venture_web_ui_require_session(self, request);
	VentureActor actor;
	if (gate) return gate;
	gate = venture_web_require_module_ui(self, request, "stripe");
	if (gate) return gate;
	principal = venture_auth_authenticate(self->auth, request);
	if (!venture_access_policy_has_organization_role(venture_database_get_access_policy(database),
		principal, id, roles, G_N_ELEMENTS(roles)))
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED, "Organization integration administration is required");
		return venture_web_error_response(error);
	}
	organization = venture_database_get(database, VENTURE_TYPE_ORGANIZATION, id, &error);
	if (!organization) return venture_web_error_response(error);
	venture_auth_to_actor(principal, &actor);
	if (htmx_request_get_method(request) == HTMX_METHOD_POST)
	{
		const gchar *operation = htmx_request_get_form_value(request, "operation");
		const gchar *version_text = htmx_request_get_form_value(request, "version");
		const gchar *connection_text = htmx_request_get_form_value(request, "connection_id");
		gint64 expected_connection = connection_text ? g_ascii_strtoll(connection_text, NULL, 10) : 0;
		version = version_text ? g_ascii_strtoll(version_text, NULL, 10) : 0;
		if (!g_strcmp0(operation, "configure"))
		{
			g_autoptr(JsonNode) settings = json_node_new(JSON_NODE_OBJECT);
			JsonObject *object = json_object_new();
			json_node_take_object(settings, object);
			json_object_set_boolean_member(object, "ach_enabled",
				!g_strcmp0(htmx_request_get_form_value(request, "ach_enabled"), "on"));
			for (i = 0; i < G_N_ELEMENTS(fields); i++)
			{
				const gchar *value = htmx_request_get_form_value(request, fields[i]);
				json_object_set_string_member(object, fields[i], value ? value : "");
			}
			connection = venture_stripe_settings_configure(database, id, settings, version, expected_connection, NULL, &actor, &error);
		}
		else if (!g_strcmp0(operation, "test"))
		{
			if (venture_stripe_settings_test(database, id, NULL, &error))
				g_string_append(content, "<p>Stripe verified the configured account.</p>");
		}
		else if (!g_strcmp0(operation, "disconnect"))
		{
			if (venture_integration_service_disable(integrations, id,
				expected_connection, version, &actor, &error))
				g_string_append(content, "<p>Disconnected. Historical payment callbacks retain their original account binding.</p>");
		}
		else
			g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Choose a Stripe settings operation");
		if (error)
		{
			g_string_append(content, "<p role=\"alert\">");
			venture_html_escape_append(content, error->message);
			g_string_append(content, "</p>");
		}
		g_clear_object(&connection);
	}
	connection = venture_integration_service_find(integrations, id, "stripe", NULL);
	version = connection ? venture_entity_get_version(VENTURE_ENTITY(connection)) : 0;
	if (connection)
	{
		g_object_get(connection, "account-id", &account, "environment", &environment, NULL);
		g_string_append(content, "<p>Connected account: ");
		venture_html_escape_append(content, account);
		g_string_append(content, " · ");
		venture_html_escape_append(content, environment);
		g_string_append_printf(content, "</p><p>Webhook path: <code>/webhooks/stripe/%" G_GINT64_FORMAT "</code></p>", venture_entity_get_id(VENTURE_ENTITY(connection)));
	}
	else g_string_append(content, "<p>No Stripe account is connected to this organization.</p>");
	g_string_append(content, "<p>Credentials are write-only. Supply all values when connecting or rotating. To change account or environment, disconnect first.</p><form method=\"post\">");
	g_string_append_printf(content, "<input type=\"hidden\" name=\"version\" value=\"%" G_GINT64_FORMAT "\">", version);
	g_string_append_printf(content, "<input type=\"hidden\" name=\"connection_id\" value=\"%" G_GINT64_FORMAT "\">", connection ? venture_entity_get_id(VENTURE_ENTITY(connection)) : 0);
	for (i = 0; i < G_N_ELEMENTS(fields); i++)
	{
		g_string_append_printf(content, "<label>%s<input name=\"%s\" type=\"%s\" autocomplete=\"off\"></label>", labels[i], fields[i], i < 3 ? "password" : "text");
	}
	g_string_append(content, "<label><input type=\"checkbox\" name=\"ach_enabled\">Offer ACH debit alongside cards for USD invoices of at least $0.50</label><p>Enable ACH Direct Debit in Stripe first. Supply this choice again when rotating credentials. Bank verification and mandate acceptance take place on Stripe Checkout.</p><p>Environment: test or live. Return URLs: HTTPS. API version: your pinned Stripe API version.</p><button name=\"operation\" value=\"configure\">Connect or rotate</button>");
	if (connection) g_string_append(content, "<button name=\"operation\" value=\"test\">Test connection</button><button name=\"operation\" value=\"disconnect\">Disconnect</button>");
	g_string_append(content, "</form>");
	return venture_web_html_response(venture_web_page(self, request, "/organizations", "Stripe settings", content->str), error ? 400 : 200);
}

static void
venture_stripe_web_settings_link(VentureWebServer *self, GString *content,
	VentureEntity *record, const VentureAuthPrincipal *principal)
{
	static const gint roles[] = { VENTURE_ORGANIZATION_ROLE_OWNER, VENTURE_ORGANIZATION_ROLE_ADMIN };
	gint64 id = venture_entity_get_id(record);
	if (G_OBJECT_TYPE(record) != VENTURE_TYPE_ORGANIZATION ||
		!venture_context_module_enabled(self->context, "stripe")) return;
	if (!venture_access_policy_has_organization_role(venture_database_get_access_policy(
		venture_context_get_database(self->context)), principal, id, roles, G_N_ELEMENTS(roles))) return;
	g_string_append_printf(content, "<p><a class=\"btn\" href=\"/organizations/%" G_GINT64_FORMAT "/settings/stripe\">Stripe settings</a></p>", id);
}

/* A payment capability authenticates itself before any invoice is read. */
static HtmxResponse *
venture_web_stripe_payment_link(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	VentureWebServer *self = user_data;
	VentureDatabase *database = venture_context_get_database(self->context);
	g_autoptr(VentureStripePaymentLink) link = NULL;
	g_autoptr(VentureStripeService) configured = NULL;
	VentureStripeService *service = venture_context_get_stripe_service(self->context);
	g_autoptr(VentureStripeCheckout) checkout = NULL;
	g_autoptr(VentureMoney) balance = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GString) html = g_string_new("<!doctype html><html><head><meta charset=\"utf-8\"><title>Invoice payment</title></head><body><h1>Invoice payment</h1>");
	g_autofree gchar *amount = NULL, *url = NULL, *state = NULL;
	const gchar *token = g_hash_table_lookup(params, "token");
	gint64 invoice_id, connection_id;
	HtmxResponse *response;
	link = venture_stripe_service_resolve_payment_link(database, token, NULL, &error);
	if (!link) goto unavailable;
	g_object_get(link, "invoice-id", &invoice_id, "connection-id", &connection_id, NULL);
	if (htmx_request_get_method(request) == HTMX_METHOD_POST)
	{
		if (!service)
		{
			configured = venture_stripe_service_for_connection(database,
				venture_entity_get_organization_id(VENTURE_ENTITY(link)), connection_id, FALSE, NULL, &error);
			service = configured;
		}
		if (!service) goto unavailable;
		checkout = venture_stripe_service_pay_link(service, token, NULL, &error);
		if (!checkout) goto unavailable;
		g_object_get(checkout, "url", &url, "status", &state, NULL);
		if (!g_strcmp0(state, "open") && venture_web_url_is_web(url))
		{
			response = htmx_response_new();
			htmx_response_set_status(response, 303);
			htmx_response_add_header(response, "Location", url);
			goto headers;
		}
		g_string_append(html, "<p>Payment is pending verification. Do not start another payment.</p>");
	}
	else
	{
		balance = venture_settlement_service_invoice_balance(venture_settlement_service_get(database), invoice_id, NULL, &error);
		if (!balance || venture_money_get_amount(balance) <= 0) goto unavailable;
		amount = venture_money_to_string(balance);
		g_string_append(html, "<p>Outstanding amount: ");
		venture_html_escape_append(html, amount);
		g_string_append(html, "</p><form method=\"post\"><button>Continue to secure payment</button></form>");
	}
	g_string_append(html, "</body></html>");
	response = htmx_response_new_with_content(html->str);
	goto headers;
unavailable:
	response = htmx_response_new_with_content("Payment link is unavailable");
	htmx_response_set_status(response, 404);
headers:
	htmx_response_add_header(response, "Cache-Control", "no-store");
	htmx_response_add_header(response, "Referrer-Policy", "no-referrer");
	return response;
}
