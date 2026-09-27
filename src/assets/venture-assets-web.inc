/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Included in the web translation unit to reuse its authentication policy. */
static HtmxResponse *
venture_web_asset_action(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	VentureDatabase *db = venture_context_get_database(self->context);
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) asset = NULL;
	g_autoptr(VentureEntity) original = NULL;
	g_autoptr(JsonNode) body = NULL;
	g_autoptr(JsonNode) result = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *destination = NULL;
	const gchar *operation = g_hash_table_lookup(params, "operation");
	gboolean api = g_str_has_prefix(htmx_request_get_path(request), "/api/");
	gboolean stage = FALSE;
	gint64 id = g_ascii_strtoll(g_hash_table_lookup(params, "id"), NULL, 10);
	HtmxResponse *gate;
	VentureActor actor;
	gate = api ? venture_web_api_require(self, request, VENTURE_USER_ROLE_EDITOR) : venture_web_ui_require_session(self, request);
	if (gate != NULL)
		return gate;
	gate = api ? venture_web_require_module_api(self, "assets") : venture_web_require_module_ui(self, request, "assets");
	if (gate != NULL)
		return gate;
	principal = venture_auth_authenticate(self->auth, request);
	if (!venture_web_hosted_auth_require(self, request, principal, VENTURE_USER_ROLE_EDITOR, &error))
		return venture_web_error_response(error);
	if (g_strcmp0(operation, "place-in-service") == 0)
		operation = "place";
	if (g_strcmp0(operation, "place") != 0 && g_strcmp0(operation, "dispose") != 0 && g_strcmp0(operation, "write-off") != 0)
	{
		venture_set_error_validation(&error, "operation", "Use place-in-service, dispose or write-off");
		return venture_web_error_response(error);
	}
	asset = venture_database_get(db, VENTURE_TYPE_FIXED_ASSET, id, &error);
	if (asset == NULL || venture_entity_is_deleted(asset))
	{
		if (error == NULL)
			g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Fixed asset was not found");
		return venture_web_error_response(error);
	}
	original = venture_database_get(db, VENTURE_TYPE_FIXED_ASSET, id, &error);
	if (original == NULL)
	{
		if (error == NULL)
			g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Fixed asset was not found");
		return venture_web_error_response(error);
	}
	if (api)
	{
		body = htmx_request_get_json(request, &error);
		if (body == NULL || !JSON_NODE_HOLDS_OBJECT(body))
		{
			if (error == NULL)
				venture_set_error_validation(&error, "body", "Expected a JSON object");
			return venture_web_error_response(error);
		}
		{
			g_autoptr(JsonNode) fields = venture_json_parse("{}", NULL);
			const gchar *names[] = { "in_service_at", "disposed_at", "disposal_proceeds" };
			guint i;
			for (i = 0; i < G_N_ELEMENTS(names); i++)
				if (json_object_has_member(json_node_get_object(body), names[i]))
					json_object_set_member(json_node_get_object(fields), names[i],
						json_node_copy(json_object_get_member(json_node_get_object(body), names[i])));
			if (!venture_serializable_from_json(VENTURE_SERIALIZABLE(asset), fields, &error))
				return venture_web_error_response(error);
		}
	}
	else
	{
		const gchar *proceeds = htmx_request_get_form_value(request, "disposal_proceeds");
		if (!venture_string_is_empty(proceeds) && !venture_entity_set_field_from_string(asset, "disposal-proceeds", proceeds, &error))
			return venture_web_error_response(error);
	}
	g_object_set(asset, "operation", operation, NULL);
	if (!venture_confirmation_parse_stage_flag(htmx_request_get_query_param(request, "stage"), &stage, &error))
		return venture_web_error_response(error);
	venture_auth_to_actor(principal, &actor);
	if (stage)
	{
		VentureConfirmation *confirmation = venture_confirmation_store_stage(venture_context_get_confirmations(self->context),
			VENTURE_AUDIT_ACTION_UPDATE, asset, original, &actor, "rest-api", &error);
		if (confirmation == NULL)
			return venture_web_error_response(error);
		result = venture_confirmation_to_json(confirmation);
		return venture_web_json_response(result, 202);
	}
	if (!venture_database_save(db, asset, &actor, &error))
		return venture_web_error_response(error);
	g_clear_object(&asset);
	asset = venture_database_get(db, VENTURE_TYPE_FIXED_ASSET, id, &error);
	if (asset == NULL || venture_entity_is_deleted(asset))
	{
		if (error == NULL)
			g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Fixed asset was not found");
		return venture_web_error_response(error);
	}
	if (api)
	{
		result = venture_serializable_to_json(VENTURE_SERIALIZABLE(asset), FALSE);
		return venture_web_json_response(result, 200);
	}
	destination = g_strdup_printf("/e/fixed_asset/%" G_GINT64_FORMAT, id);
	return venture_web_redirect_to(destination);
}

static HtmxResponse *
venture_web_assets_run(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(JsonNode) body = NULL;
	g_autoptr(JsonNode) result = NULL;
	g_autoptr(GError) error = NULL;
	HtmxResponse *gate;
	JsonObject *object;
	VentureActor actor;
	gint64 org;
	gint count;
	gboolean dry_run;
	(void)params;
	gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_EDITOR);
	if (gate != NULL)
		return gate;
	gate = venture_web_require_module_api(self, "assets");
	if (gate != NULL)
		return gate;
	body = htmx_request_get_json(request, &error);
	if (body == NULL || !JSON_NODE_HOLDS_OBJECT(body))
	{
		if (error == NULL)
			venture_set_error_validation(&error, "body", "Expected period, optional organization_id and dry_run");
		return venture_web_error_response(error);
	}
	object = json_node_get_object(body);
	org = venture_context_get_default_organization_id(self->context);
	dry_run = FALSE;
	if (json_object_has_member(object, "organization_id"))
	{
		GValue value = G_VALUE_INIT;
		gboolean ok = venture_json_value_from_node(json_object_get_member(object, "organization_id"), G_TYPE_INT64, &value, &error);
		if (ok)
			org = g_value_get_int64(&value);
		g_value_unset(&value);
		if (!ok)
			return venture_web_error_response(error);
	}
	if (json_object_has_member(object, "dry_run"))
	{
		GValue value = G_VALUE_INIT;
		gboolean ok = venture_json_value_from_node(json_object_get_member(object, "dry_run"), G_TYPE_BOOLEAN, &value, &error);
		if (ok)
			dry_run = g_value_get_boolean(&value);
		g_value_unset(&value);
		if (!ok)
			return venture_web_error_response(error);
	}
	{
		gboolean stage = FALSE;
		if (!venture_confirmation_parse_stage_flag(htmx_request_get_query_param(request, "stage"), &stage, &error))
			return venture_web_error_response(error);
		if (stage)
		{
			venture_set_error_validation(&error, "stage", "Stage a fixed_asset operation=run-period:YYYY-MM to approve a sweep");
			return venture_web_error_response(error);
		}
	}
	principal = venture_auth_authenticate(self->auth, request);
	venture_auth_to_actor(principal, &actor);
	count = venture_asset_service_run_period(venture_asset_service_get(venture_context_get_database(self->context)),
		venture_json_object_get_string(object, "period", ""), org, dry_run, &actor, &error);
	if (count < 0)
		return venture_web_error_response(error);
	result = venture_json_parse("{}", NULL);
	json_object_set_int_member(json_node_get_object(result), "entries", count);
	json_object_set_boolean_member(json_node_get_object(result), "dry_run", dry_run);
	return venture_web_json_response(result, 200);
}

/*
 * An asset's page leads with where it is in its life, in money: what it
 * cost, what it is worth on the books today, what the next month takes
 * off and when it is used up. Then the one or two things you can do,
 * each saying what it does.
 */
static void
venture_web_append_asset_actions(VentureWebServer *self, GString *html, VentureEntity *asset)
{
	g_autoptr(VentureMoney) cost = NULL;
	g_autoptr(VentureMoney) posted = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) entries = NULL;
	g_autofree gchar *next_period = NULL, *last_period = NULL, *next_amount = NULL;
	gint status;
	gint64 id = venture_entity_get_id(asset);
	guint i;

	g_object_get(asset, "status", &status, "cost", &cost, NULL);
	query = venture_query_new(VENTURE_TYPE_DEPRECIATION_ENTRY);
	venture_query_set_limit(query, 0);
	venture_query_add_filter_int(query, "asset-id", VENTURE_FILTER_OP_EQ, id, NULL);
	venture_query_add_order(query, "period", VENTURE_SORT_ASCENDING, NULL);
	entries = venture_database_find(venture_context_get_database(self->context), query, NULL);
	for (i = 0; entries != NULL && i < entries->len; i++)
	{
		VentureEntity *entry = g_ptr_array_index(entries, i);
		g_autoptr(VentureMoney) amount = NULL;
		g_autofree gchar *period = NULL;
		gint state;
		g_object_get(entry, "amount", &amount, "period", &period, "state", &state, NULL);
		if (state == VENTURE_SCHEDULE_STATE_POSTED && amount != NULL)
		{
			if (posted == NULL)
				posted = venture_money_copy(amount);
			else
			{
				VentureMoney *sum = venture_money_add(posted, amount, NULL);
				if (sum != NULL)
				{
					venture_money_free(posted);
					posted = sum;
				}
			}
		}
		else if (state == VENTURE_SCHEDULE_STATE_SCHEDULED && next_period == NULL)
		{
			next_period = g_strdup(period);
			next_amount = amount != NULL ? venture_money_to_display_string(amount, TRUE) : NULL;
		}
		if (state != VENTURE_SCHEDULE_STATE_SKIPPED)
		{
			g_free(last_period);
			last_period = g_strdup(period);
		}
	}

	g_string_append(html, "<section class=\"card next-up\"><div class=\"card-head\"><h2>Where it stands</h2></div><div class=\"card-body\">");
	if (status == VENTURE_ASSET_STATUS_DRAFT)
		g_string_append(html, "<p class=\"next-up-lead\">Not in use yet. Placing it in service writes its whole "
			"depreciation schedule, month by month, into the books.</p>");
	else if (status == VENTURE_ASSET_STATUS_DISPOSED || status == VENTURE_ASSET_STATUS_WRITTEN_OFF)
		g_string_append(html, "<p class=\"next-up-lead\">No longer owned. Its remaining schedule was stopped.</p>");
	else
	{
		g_autofree gchar *cost_text = cost != NULL ? venture_money_to_display_string(cost, TRUE) : NULL;
		g_autoptr(VentureMoney) book = NULL;
		g_autofree gchar *book_text = NULL;
		if (cost != NULL)
			book = posted != NULL ? venture_money_subtract(cost, posted, NULL) : venture_money_copy(cost);
		book_text = book != NULL ? venture_money_to_display_string(book, TRUE) : NULL;
		g_string_append(html, "<div class=\"stat-row asset-stats\">");
		if (cost_text != NULL)
		{
			g_string_append(html, "<div class=\"stat\"><span class=\"stat-label\">Cost</span><span class=\"stat-value\">");
			venture_html_escape_append(html, cost_text);
			g_string_append(html, "</span></div>");
		}
		if (book_text != NULL)
		{
			g_string_append(html, "<div class=\"stat\"><span class=\"stat-label\">On the books today</span><span class=\"stat-value\">");
			venture_html_escape_append(html, book_text);
			g_string_append(html, "</span></div>");
		}
		g_string_append(html, "</div>");
		if (next_period != NULL)
		{
			g_string_append(html, "<p>Next: ");
			venture_html_escape_append(html, next_amount != NULL ? next_amount : "");
			g_string_append(html, " of depreciation in <strong>");
			venture_html_escape_append(html, next_period);
			g_string_append(html, "</strong>. Used up by <strong>");
			venture_html_escape_append(html, last_period != NULL ? last_period : "");
			g_string_append(html, "</strong>.</p>");
		}
		else
			g_string_append(html, "<p>Fully depreciated. It stays on the books at what it is worth at the end until it is sold or written off.</p>");
	}
	g_string_append(html, "</div></section>");

	if (status == VENTURE_ASSET_STATUS_DRAFT)
		g_string_append_printf(html, "<section class=\"card record-actions\"><div class=\"card-head\"><h2>Next step</h2></div><div class=\"card-body\">"
			"<form method=\"post\" action=\"/assets/%" G_GINT64_FORMAT "/place-in-service\"><button class=\"btn btn-primary\">Place in service</button></form>"
			"</div></section>", id);
	else if (status == VENTURE_ASSET_STATUS_IN_SERVICE)
		g_string_append_printf(html, "<section class=\"card record-actions\"><div class=\"card-head\"><h2>If it leaves the business</h2></div><div class=\"card-body\">"
			"<details class=\"record-action\"><summary class=\"btn\">Sold it</summary>"
			"<form method=\"post\" action=\"/assets/%" G_GINT64_FORMAT "/dispose\"><label class=\"doc-field\"><span class=\"field-label\">Sold for</span>"
			"<input name=\"disposal_proceeds\" inputmode=\"decimal\" placeholder=\"0.00\" required></label>"
			"<span class=\"field-help\">The rest of the schedule stops; the gain or loss against what it was worth is posted.</span>"
			"<button class=\"btn btn-primary\">Record the sale</button></form></details>"
			"<form method=\"post\" action=\"/assets/%" G_GINT64_FORMAT "/write-off\"><button class=\"btn\" title=\"Lost, broken or thrown away: what it was still worth becomes a loss\">Write it off</button></form>"
			"</div></section>", id, id);
}

static HtmxResponse *
venture_web_assets_run_tax(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(JsonNode) body = NULL;
	g_autoptr(JsonNode) result = NULL;
	g_autoptr(GError) error = NULL;
	HtmxResponse *gate;
	JsonObject *object;
	VentureActor actor;
	gint64 org;
	gint count;
	gboolean dry_run;
	(void)params;
	gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_EDITOR);
	if (gate != NULL)
		return gate;
	gate = venture_web_require_module_api(self, "assets");
	if (gate != NULL)
		return gate;
	body = htmx_request_get_json(request, &error);
	if (body == NULL || !JSON_NODE_HOLDS_OBJECT(body))
	{
		if (error == NULL)
			venture_set_error_validation(&error, "body", "Expected period");
		return venture_web_error_response(error);
	}
	object = json_node_get_object(body);
	org = json_object_has_member(object, "organization_id") ?
		venture_json_object_get_int(object, "organization_id", 0) :
		venture_context_get_default_organization_id(self->context);
	dry_run = json_object_has_member(object, "dry_run") &&
		json_object_get_boolean_member(object, "dry_run");
	if (org <= 0)
		org = venture_context_get_default_organization_id(self->context);
	principal = venture_auth_authenticate(self->auth, request);
	venture_auth_to_actor(principal, &actor);
	count = venture_asset_service_run_tax_period(venture_asset_service_get(venture_context_get_database(self->context)),
		venture_json_object_get_string(object, "period", ""), org, dry_run, &actor, &error);
	if (count < 0)
		return venture_web_error_response(error);
	result = venture_json_parse("{}", NULL);
	json_object_set_int_member(json_node_get_object(result), "entries", count);
	json_object_set_boolean_member(json_node_get_object(result), "dry_run", dry_run);
	return venture_web_json_response(result, 200);
}
