/* SPDX-License-Identifier: AGPL-3.0-or-later */
static HtmxResponse *
equity_post(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) row = NULL;
	g_autoptr(VentureMoney) amount = NULL;
	g_autoptr(GDateTime) when = NULL;
	VentureActor actor;
	HtmxResponse *gate;
	g_autoptr(JsonNode) body = htmx_request_get_json(request, NULL);
	const gchar *kind_nick = htmx_request_get_form_value(request, "kind");
	const gchar *amount_text = htmx_request_get_form_value(request, "amount");
	const gchar *memo = htmx_request_get_form_value(request, "memo");
	gint kind = VENTURE_EQUITY_KIND_CONTRIBUTION;
	gboolean api = g_str_has_prefix(htmx_request_get_path(request), "/api/");
	(void)params;
	if (body != NULL && JSON_NODE_HOLDS_OBJECT(body))
	{
		JsonObject *object = json_node_get_object(body);
		if (kind_nick == NULL)
			kind_nick = json_object_get_string_member(object, "kind");
		if (amount_text == NULL)
			amount_text = json_object_get_string_member(object, "amount");
		if (memo == NULL)
			memo = json_object_get_string_member(object, "memo");
	}
	gate = api ? venture_web_api_require(self, request, VENTURE_USER_ROLE_EDITOR)
		: venture_web_ui_require_session(self, request);
	if (gate != NULL)
		return gate;
	gate = api ? venture_web_require_module_api(self, "equity")
		: venture_web_require_module_ui(self, request, "equity");
	if (gate != NULL)
		return gate;
	principal = venture_auth_authenticate(self->auth, request);
	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_EDITOR, &error))
		return venture_web_error_response(error);
	venture_auth_to_actor(principal, &actor);
	if (kind_nick != NULL)
		venture_enum_from_nick(VENTURE_TYPE_EQUITY_KIND, kind_nick, &kind);
	if (amount_text == NULL || (amount = venture_money_from_string(amount_text, "USD", &error)) == NULL)
	{
		if (error == NULL)
			g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Amount is required");
		return venture_web_error_response(error);
	}
	when = venture_time_now();
	row = venture_capital_service_post(venture_capital_service_get(venture_context_get_database(self->context)),
		venture_context_get_default_organization_id(self->context), kind, amount, when, memo, 0, 0, &actor, &error);
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
	htmx_router_get(router, "/equity", equity_page, self);
	htmx_router_post(router, "/equity/post", equity_post, self);
	htmx_router_post(router, "/api/v1/equity/post", equity_post, self);
}
