/* SPDX-License-Identifier: AGPL-3.0-or-later */
static gint64
tax_org(VentureWebServer *self, HtmxRequest *request)
{
	const gchar *value = htmx_request_get_query_param(request, "organization_id");
	gint64 organization = value ? g_ascii_strtoll(value, NULL, 10) : venture_web_active_organization(self, request);
	return organization > 0 ? organization : venture_context_get_default_organization_id(self->context);
}

static HtmxResponse *
tax_filing_api(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) body = NULL;
	g_autoptr(JsonNode) output = NULL;
	g_autoptr(VentureEntity) filing = NULL;
	VentureTaxFilingService *service;
	VentureActor actor;
	const gchar *action = g_hash_table_lookup(params, "action");
	const gchar *path = htmx_request_get_path(request);
	HtmxResponse *gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_EDITOR);
	if (gate != NULL)
		return gate;
	gate = venture_web_require_module_api(self, "tax_filing");
	if (gate != NULL)
		return gate;
	principal = venture_auth_authenticate(self->auth, request);
	venture_auth_to_actor(principal, &actor);
	service = venture_tax_filing_service_get(venture_context_get_database(self->context));
	body = htmx_request_get_json(request, NULL);
	if (action == NULL && g_str_has_suffix(path, "/prepare"))
		action = "prepare";
	if (g_strcmp0(action, "prepare") == 0)
	{
		JsonObject *object = body != NULL && JSON_NODE_HOLDS_OBJECT(body) ? json_node_get_object(body) : NULL;
		gint64 period_id = object ? venture_json_object_get_int(object, "fiscal_period_id", 0) : 0;
		const gchar *country = object ? venture_json_object_get_string(object, "country", NULL) : NULL;
		const gchar *jurisdiction = object ? venture_json_object_get_string(object, "jurisdiction", NULL) : NULL;
		if (country == NULL)
			country = htmx_request_get_form_value(request, "country");
		if (jurisdiction == NULL)
			jurisdiction = htmx_request_get_form_value(request, "jurisdiction");
		if (period_id == 0 && htmx_request_get_form_value(request, "fiscal_period_id") != NULL)
			period_id = g_ascii_strtoll(htmx_request_get_form_value(request, "fiscal_period_id"), NULL, 10);
		filing = venture_tax_filing_service_prepare(service, tax_org(self, request),
			country, jurisdiction, period_id, NULL, NULL, &actor, &error);
		if (filing == NULL)
			return venture_web_error_response(error);
		output = venture_serializable_to_json(VENTURE_SERIALIZABLE(filing), FALSE);
		return venture_web_json_response(output, 201);
	}
	filing = venture_database_get(venture_context_get_database(self->context),
		VENTURE_TYPE_TAX_FILING, g_ascii_strtoll(g_hash_table_lookup(params, "id"), NULL, 10), &error);
	if (filing == NULL)
		return venture_web_error_response(error);
	if (g_strcmp0(action, "review") == 0)
	{
		if (!venture_tax_filing_service_review(service, filing, &actor, &error))
			return venture_web_error_response(error);
	}
	else if (g_strcmp0(action, "submit") == 0)
	{
		if (!venture_tax_filing_service_submit(service, filing, &actor, &error))
			return venture_web_error_response(error);
	}
	else if (g_strcmp0(action, "acknowledge") == 0)
	{
		const gchar *ack = NULL;
		if (body != NULL && JSON_NODE_HOLDS_OBJECT(body))
			ack = venture_json_object_get_string(json_node_get_object(body), "acknowledgment_id", NULL);
		if (!venture_tax_filing_service_acknowledge(service, filing, ack, &actor, &error))
			return venture_web_error_response(error);
	}
	else if (g_strcmp0(action, "amend") == 0)
	{
		g_autoptr(VentureEntity) next = venture_tax_filing_service_amend(service, filing, &actor, &error);
		if (next == NULL)
			return venture_web_error_response(error);
		output = venture_serializable_to_json(VENTURE_SERIALIZABLE(next), FALSE);
		return venture_web_json_response(output, 201);
	}
	else
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT, "Unknown tax filing action");
		return venture_web_error_response(error);
	}
	output = venture_serializable_to_json(VENTURE_SERIALIZABLE(filing), FALSE);
	return venture_web_json_response(output, 200);
}

static HtmxResponse *
contractor_tax_api(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) body = NULL;
	g_autoptr(JsonNode) output = NULL;
	g_autoptr(VentureEntity) pack = NULL;
	VentureTaxFilingService *service;
	VentureActor actor;
	const gchar *action = g_hash_table_lookup(params, "action");
	const gchar *path = htmx_request_get_path(request);
	HtmxResponse *gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_EDITOR);
	if (gate != NULL)
		return gate;
	gate = venture_web_require_module_api(self, "tax_filing");
	if (gate != NULL)
		return gate;
	principal = venture_auth_authenticate(self->auth, request);
	venture_auth_to_actor(principal, &actor);
	service = venture_tax_filing_service_get(venture_context_get_database(self->context));
	body = htmx_request_get_json(request, NULL);
	if (action == NULL && g_str_has_suffix(path, "/prepare"))
		action = "prepare";
	if (action == NULL && g_str_has_suffix(path, "/export"))
		action = "export";
	if (g_strcmp0(action, "prepare") == 0)
	{
		JsonObject *object = body != NULL && JSON_NODE_HOLDS_OBJECT(body) ? json_node_get_object(body) : NULL;
		gint64 vendor_id = object ? venture_json_object_get_int(object, "vendor_id", 0) : 0;
		gint year = (gint)(object ? venture_json_object_get_int(object, "year", 0) : 0);
		if (vendor_id == 0 && htmx_request_get_form_value(request, "vendor_id") != NULL)
			vendor_id = g_ascii_strtoll(htmx_request_get_form_value(request, "vendor_id"), NULL, 10);
		if (year == 0 && htmx_request_get_form_value(request, "year") != NULL)
			year = (gint)g_ascii_strtoll(htmx_request_get_form_value(request, "year"), NULL, 10);
		pack = venture_tax_filing_service_prepare_1099(service, tax_org(self, request),
			vendor_id, year, &actor, &error);
		if (pack == NULL)
			return venture_web_error_response(error);
		output = venture_serializable_to_json(VENTURE_SERIALIZABLE(pack), FALSE);
		return venture_web_json_response(output, 201);
	}
	pack = venture_database_get(venture_context_get_database(self->context),
		VENTURE_TYPE_CONTRACTOR_TAX_PACK, g_ascii_strtoll(g_hash_table_lookup(params, "id"), NULL, 10), &error);
	if (pack == NULL)
		return venture_web_error_response(error);
	if (g_strcmp0(action, "review") == 0)
	{
		if (!venture_tax_filing_service_review_1099(service, pack, &actor, &error))
			return venture_web_error_response(error);
	}
	else if (g_strcmp0(action, "approve") == 0)
	{
		if (!venture_tax_filing_service_approve_1099(service, pack, &actor, &error))
			return venture_web_error_response(error);
	}
	else if (g_strcmp0(action, "export") == 0)
	{
		g_autofree gchar *csv = venture_tax_filing_service_export_1099(service, pack, &actor, &error);
		HtmxResponse *response;
		if (csv == NULL)
			return venture_web_error_response(error);
		response = htmx_response_new_with_content(csv);
		htmx_response_set_content_type(response, "text/csv; charset=utf-8");
		return response;
	}
	else
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT, "Unknown contractor tax action");
		return venture_web_error_response(error);
	}
	output = venture_serializable_to_json(VENTURE_SERIALIZABLE(pack), FALSE);
	return venture_web_json_response(output, 200);
}

static HtmxResponse *
tax_filings_ui(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	g_autoptr(GString) content = g_string_new("<div class=\"page-head\"><h1>Tax filings</h1></div>");
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GError) error = NULL;
	guint i;
	HtmxResponse *gate = venture_web_ui_require_session(self, request);
	(void)params;
	if (gate != NULL)
		return gate;
	gate = venture_web_require_module_ui(self, request, "tax_filing");
	if (gate != NULL)
		return gate;
	g_string_append(content,
		"<div class=\"card\"><div class=\"card-head\"><h2>Prepare a sales-tax return</h2></div>"
		"<form method=\"post\" action=\"/api/v1/tax-filings/prepare\" class=\"stack\">"
		"<label>Country <input name=\"country\" value=\"US\" required></label>"
		"<label>Jurisdiction <input name=\"jurisdiction\" placeholder=\"US-NY\"></label>"
		"<label>Fiscal period id <input name=\"fiscal_period_id\" type=\"number\" required></label>"
		"<button type=\"submit\">Prepare</button></form></div>");
	g_string_append(content,
		"<div class=\"card\"><div class=\"card-head\"><h2>Prepare 1099-NEC</h2></div>"
		"<form method=\"post\" action=\"/api/v1/contractor-tax/prepare\" class=\"stack\">"
		"<label>Vendor id <input name=\"vendor_id\" type=\"number\" required></label>"
		"<label>Year <input name=\"year\" type=\"number\" required></label>"
		"<button type=\"submit\">Prepare</button></form></div>");
	query = venture_query_new(VENTURE_TYPE_TAX_FILING);
	venture_query_set_organization(query, tax_org(self, request));
	rows = venture_database_find(venture_context_get_database(self->context), query, &error);
	if (rows == NULL)
		return venture_web_error_response(error);
	g_string_append(content, "<div class=\"card\"><div class=\"card-head\"><h2>Filing packs</h2></div>"
		"<table><thead><tr><th>Name</th><th>Status</th></tr></thead><tbody>");
	for (i = 0; i < rows->len; i++)
	{
		g_autofree gchar *name = NULL;
		g_autofree gchar *status = NULL;
		g_object_get(g_ptr_array_index(rows, i), "name", &name, "status", &status, NULL);
		g_string_append(content, "<tr><td><a href=\"/e/tax_filing/");
		g_string_append_printf(content, "%" G_GINT64_FORMAT, venture_entity_get_id(g_ptr_array_index(rows, i)));
		g_string_append(content, "\">");
		venture_html_escape_append(content, name);
		g_string_append(content, "</a></td><td>");
		venture_html_escape_append(content, status);
		g_string_append(content, "</td></tr>");
	}
	g_string_append(content, "</tbody></table></div>");
	return venture_web_html_response(venture_web_page(self, request, "/tax-filings", "Tax filings", content->str), 200);
}
