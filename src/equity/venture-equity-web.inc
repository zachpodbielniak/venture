/* SPDX-License-Identifier: AGPL-3.0-or-later */
static HtmxResponse *
equity_post(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) row = NULL;
	g_autoptr(VentureMoney) amount = NULL;
	g_autoptr(VentureEntity) organization = NULL;
	g_autofree gchar *currency = NULL;
	gint64 organization_id;
	VentureActor actor;
	HtmxResponse *gate;
	g_autoptr(JsonNode) body = NULL;
	const gchar *kind_nick = NULL;
	const gchar *amount_text = NULL;
	const gchar *memo = NULL;
	gint kind = VENTURE_EQUITY_KIND_CONTRIBUTION;
	gboolean api = g_str_has_prefix(htmx_request_get_path(request), "/api/");
	(void)params;
	gate = api ? venture_web_api_require(self, request, VENTURE_USER_ROLE_EDITOR)
		: venture_web_ui_require_session(self, request);
	if (gate != NULL)
		return gate;
	gate = api ? venture_web_require_module_api(self, "equity")
		: venture_web_require_module_ui(self, request, "equity");
	if (gate != NULL)
		return gate;
	principal = venture_auth_authenticate(self->auth, request);
	if (!venture_web_hosted_auth_require(self, request, principal, VENTURE_USER_ROLE_EDITOR, &error))
		return venture_web_error_response(error);
	venture_auth_to_actor(principal, &actor);
	/* Authenticate before reading untrusted fields. JSON-GLib's typed
	 * accessors assert on missing or wrongly typed members. */
	if (api)
	{
		const gchar *names[] = { "kind", "amount", "memo" };
		const gchar *values[] = { NULL, NULL, NULL };
		JsonObject *object;
		guint i;
		body = htmx_request_get_json(request, &error);
		if (body == NULL || !JSON_NODE_HOLDS_OBJECT(body))
		{
			g_clear_error(&error);
			g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Expected a JSON object");
			return venture_web_error_response(error);
		}
		object = json_node_get_object(body);
		for (i = 0; i < G_N_ELEMENTS(names); i++)
		{
			JsonNode *value = json_object_get_member(object, names[i]);
			if (value == NULL || JSON_NODE_HOLDS_NULL(value))
				continue;
			if (!JSON_NODE_HOLDS_VALUE(value) || json_node_get_value_type(value) != G_TYPE_STRING)
			{
				g_set_error(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "%s must be a string", names[i]);
				return venture_web_error_response(error);
			}
			values[i] = json_node_get_string(value);
		}
		kind_nick = values[0];
		amount_text = values[1];
		memo = values[2];
	}
	else
	{
		kind_nick = htmx_request_get_form_value(request, "kind");
		amount_text = htmx_request_get_form_value(request, "amount");
		memo = htmx_request_get_form_value(request, "memo");
	}
	if (kind_nick != NULL && !venture_enum_from_nick(VENTURE_TYPE_EQUITY_KIND, kind_nick, &kind))
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Unknown equity kind");
		return venture_web_error_response(error);
	}
	organization_id = venture_web_active_organization(self, request);
	organization = venture_database_get(venture_context_get_database(self->context),
		VENTURE_TYPE_ORGANIZATION, organization_id, &error);
	if (organization == NULL)
		return venture_web_error_response(error);
	g_object_get(organization, "default-currency", &currency, NULL);
	if (amount_text == NULL || (amount = venture_money_from_string(amount_text, currency, &error)) == NULL)
	{
		if (error == NULL)
			g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Amount is required");
		return venture_web_error_response(error);
	}
	/* Resolve the execution date inside the service after approval; a fresh
	 * timestamp here would change the proposal on every browser retry. */
	row = venture_capital_service_post(venture_capital_service_get(venture_context_get_database(self->context)),
		organization_id, kind, amount, NULL, memo, 0, 0, &actor, &error);
	if (row == NULL)
		return venture_web_error_response(error);
	if (api)
		return venture_web_json_response(venture_serializable_to_json(VENTURE_SERIALIZABLE(row), FALSE), 201);
	{
		g_autofree gchar *path = g_strdup_printf("/e/equity_transaction/%" G_GINT64_FORMAT, venture_entity_get_id(row));
		return venture_web_redirect_to(path);
	}
}

static HtmxResponse *
equity_page(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	HtmxResponse *gate = venture_web_ui_require_session(self, request);
	(void)params;
	if (gate != NULL)
		return gate;
	gate = venture_web_require_module_ui(self, request, "equity");
	if (gate != NULL)
		return gate;
	return venture_web_html_response(venture_web_page(self, request, "/equity", "Owner equity",
		"<div class=\"page-head\"><h1>Owner equity</h1>"
		"<p class=\"muted\">Guided contributions, draws, loans and cash transfers. "
		"Expense accounts are refused.</p></div>"
		"<section class=\"card\"><form method=\"post\" action=\"/equity/post\">"
		"<label>Kind<select name=\"kind\">"
		"<option value=\"contribution\">Owner contribution</option>"
		"<option value=\"draw\">Owner draw</option>"
		"<option value=\"loan_proceed\">Loan proceed</option>"
		"<option value=\"loan_payment\">Loan payment</option>"
		"<option value=\"transfer\">Transfer</option></select></label>"
		"<label>Amount<input name=\"amount\" required></label>"
		"<label>Memo<input name=\"memo\"></label>"
		"<button class=\"btn btn-primary\" type=\"submit\">Post</button></form></section>"), 200);
}

static void
venture_equity_web_register(HtmxRouter *router, VentureWebServer *self)
{
	(void)router;
	venture_web_server_add_classified_route(self, HTMX_METHOD_GET, "/equity", VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, equity_page, self);
	venture_web_server_add_classified_route(self, HTMX_METHOD_POST, "/equity/post", VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, equity_post, self);
	venture_web_server_add_classified_route(self, HTMX_METHOD_POST, "/api/v1/equity/post", VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, equity_post, self);
}
