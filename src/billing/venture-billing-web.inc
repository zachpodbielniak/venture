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
	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_EDITOR, &error))
		return venture_web_error_response(error);
	gate = venture_web_require_module_api(self, "billing");
	if (gate != NULL)
		return gate;
	if (ui)
	{
		static const gchar *const fields[] = { "at", "plan-price-id", "seats", "at-period-end" };
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
		g_autofree gchar *path = g_strdup_printf("/e/customer_subscription/%" G_GINT64_FORMAT, id);
		HtmxResponse *response = htmx_response_new();
		htmx_response_set_status(response, 303);
		htmx_response_add_header(response, "Location", path);
		return response;
	}
	result = venture_serializable_to_json(VENTURE_SERIALIZABLE(instruction), FALSE);
	return venture_web_json_response(result, 200);
}

static void
venture_billing_web_buttons(VentureWebServer *self, GString *html, VentureEntity *record, VentureAuthPrincipal *principal)
{
	static const gchar *const actions[] = { "renew", "pause", "resume", "cancel" };
	guint i;
	gint64 id;
	if (!VENTURE_IS_CUSTOMER_SUBSCRIPTION(record) ||
		!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_EDITOR, NULL))
		return;
	id = venture_entity_get_id(record);
	g_string_append(html, "<section class=\"card billing-action\"><h2>Subscription actions</h2>");
	for (i = 0; i < G_N_ELEMENTS(actions); i++)
	{
		g_string_append_printf(html, "<form method=\"post\" action=\"/billing/subscriptions/%" G_GINT64_FORMAT "/action\"><input type=\"hidden\" name=\"billing_action\" value=\"%s\"><button class=\"btn\" type=\"submit\">%s</button></form>", id, actions[i], actions[i]);
	}
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_PLAN_PRICE);
		g_autoptr(GPtrArray) prices = NULL;
		venture_query_set_organization(query, venture_entity_get_organization_id(record));
		venture_query_set_limit(query, 0);
		prices = venture_database_find(venture_context_get_database(self->context), query, NULL);
		g_string_append_printf(html, "<form method=\"post\" action=\"/billing/subscriptions/%" G_GINT64_FORMAT "/action\"><input type=\"hidden\" name=\"billing_action\" value=\"change\"><label>New plan <select name=\"plan-price-id\" required>", id);
		for (i = 0; prices != NULL && i < prices->len; i++)
		{
			VentureEntity *price = g_ptr_array_index(prices, i);
			g_autoptr(VentureEntity) plan = NULL;
			g_autoptr(VentureMoney) amount = NULL;
			g_autofree gchar *name = NULL;
			g_autofree gchar *formatted = NULL;
			gint64 plan_id;
			gint interval;
			gboolean active;
			gboolean per_seat;
			g_object_get(price, "plan-id", &plan_id, "amount", &amount, "interval", &interval, "active", &active, "per-seat", &per_seat, NULL);
			if (!active || amount == NULL)
				continue;
			plan = venture_database_get(venture_context_get_database(self->context), VENTURE_TYPE_PLAN, plan_id, NULL);
			if (plan == NULL)
				continue;
			g_object_get(plan, "name", &name, NULL);
			formatted = venture_money_to_string(amount);
			g_string_append_printf(html, "<option value=\"%" G_GINT64_FORMAT "\">", venture_entity_get_id(price));
			venture_html_escape_append(html, name);
			g_string_append(html, " — ");
			venture_html_escape_append(html, formatted);
			g_string_append_printf(html, " / %s%s</option>", interval == 1 ? "year" : "month", per_seat ? " / seat" : "");
		}
		g_string_append(html, "</select></label><label><input name=\"at-period-end\" type=\"checkbox\" value=\"true\">At period end</label><button class=\"btn\" type=\"submit\">Change plan</button></form></section>");
	}
}
