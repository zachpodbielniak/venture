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

static gint
tax_year_descending(gconstpointer a, gconstpointer b)
{
	return *(const gint *)b - *(const gint *)a;
}

/* The owner's own suppliers, by name. Filtered in C on the role, not on
 * the kind column: a company can be a customer and a supplier at once. */
static void
tax_vendor_options(VentureWebServer *self, GString *html, gint64 organization)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_COMPANY);
	g_autoptr(GPtrArray) rows = NULL;
	guint i;
	venture_query_set_limit(query, 0);
	venture_query_set_organization(query, organization);
	venture_query_add_order(query, "name", VENTURE_SORT_ASCENDING, NULL);
	rows = venture_database_find(venture_context_get_database(self->context), query, NULL);
	for (i = 0; rows != NULL && i < rows->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(rows, i);
		g_autofree gchar *name = NULL;
		if (venture_entity_is_deleted(row) || !venture_company_is_supplier(row))
			continue;
		name = venture_entity_get_display_name(row);
		g_string_append_printf(html, "<option value=\"%" G_GINT64_FORMAT "\">", venture_entity_get_id(row));
		venture_html_escape_append(html, name);
		g_string_append(html, "</option>");
	}
}

static GPtrArray *
tax_fiscal_years(VentureWebServer *self, gint64 organization)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_FISCAL_YEAR);
	venture_query_set_limit(query, 0);
	venture_query_set_organization(query, organization);
	venture_query_add_order(query, "start-at", VENTURE_SORT_DESCENDING, NULL);
	return venture_database_find(venture_context_get_database(self->context), query, NULL);
}

/* The owner's fiscal periods, by name, grouped under their fiscal year. */
static void
tax_period_options(VentureWebServer *self, GString *html, GPtrArray *years, gint64 organization)
{
	guint i, j;
	for (i = 0; years != NULL && i < years->len; i++)
	{
		VentureEntity *year = g_ptr_array_index(years, i);
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_FISCAL_PERIOD);
		g_autoptr(GPtrArray) periods = NULL;
		g_autofree gchar *year_name = venture_entity_get_display_name(year);
		venture_query_set_limit(query, 0);
		venture_query_set_organization(query, organization);
		venture_query_add_order(query, "start-at", VENTURE_SORT_ASCENDING, NULL);
		if (!venture_query_add_filter_int(query, "fiscal-year-id", VENTURE_FILTER_OP_EQ,
				venture_entity_get_id(year), NULL))
			continue;
		periods = venture_database_find(venture_context_get_database(self->context), query, NULL);
		if (periods == NULL || periods->len == 0)
			continue;
		g_string_append(html, "<optgroup label=\"");
		venture_html_escape_append(html, year_name);
		g_string_append(html, "\">");
		for (j = 0; j < periods->len; j++)
		{
			VentureEntity *period = g_ptr_array_index(periods, j);
			g_autofree gchar *name = venture_entity_get_display_name(period);
			g_string_append_printf(html, "<option value=\"%" G_GINT64_FORMAT "\">", venture_entity_get_id(period));
			venture_html_escape_append(html, name);
			g_string_append(html, "</option>");
		}
		g_string_append(html, "</optgroup>");
	}
}

/* Calendar years the owner's fiscal years cover, newest first; this year
 * and last when none is set up. A 1099-NEC is filed by calendar year. */
static void
tax_year_options(GString *html, GPtrArray *years)
{
	g_autoptr(GArray) seen = g_array_new(FALSE, FALSE, sizeof(gint));
	guint i, j;
	for (i = 0; years != NULL && i < years->len; i++)
	{
		g_autoptr(GDateTime) start = NULL;
		g_autoptr(GDateTime) end = NULL;
		gint first, last, y;
		g_object_get(g_ptr_array_index(years, i), "start-at", &start, "end-at", &end, NULL);
		if (start == NULL)
			continue;
		first = g_date_time_get_year(start);
		/* The end is exclusive: a year ending on 1 January covers no day of it. */
		last = first;
		if (end != NULL)
		{
			g_autoptr(GDateTime) day_before = g_date_time_add_seconds(end, -1);
			last = g_date_time_get_year(day_before);
		}
		for (y = last; y >= first; y--)
		{
			gboolean dup = FALSE;
			for (j = 0; j < seen->len; j++)
				dup = dup || g_array_index(seen, gint, j) == y;
			if (!dup)
				g_array_append_val(seen, y);
		}
	}
	if (seen->len == 0)
	{
		g_autoptr(GDateTime) now = g_date_time_new_now_utc();
		gint y = g_date_time_get_year(now);
		g_array_append_val(seen, y);
		y--;
		g_array_append_val(seen, y);
	}
	g_array_sort(seen, tax_year_descending);
	for (j = 0; j < seen->len; j++)
		g_string_append_printf(html, "<option value=\"%d\">%d</option>",
			g_array_index(seen, gint, j), g_array_index(seen, gint, j));
}

/* Jurisdictions the owner's tax rates already name, as suggestions. */
static void
tax_jurisdiction_options(VentureWebServer *self, GString *html, gint64 organization)
{
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GHashTable) seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	guint i;
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "tax_code") == G_TYPE_INVALID)
		return;
	query = venture_query_new(VENTURE_TYPE_TAX_CODE);
	venture_query_set_limit(query, 0);
	venture_query_set_organization(query, organization);
	rows = venture_database_find(venture_context_get_database(self->context), query, NULL);
	for (i = 0; rows != NULL && i < rows->len; i++)
	{
		g_autofree gchar *place = NULL;
		g_object_get(g_ptr_array_index(rows, i), "jurisdiction", &place, NULL);
		if (venture_string_is_empty(place) || g_hash_table_contains(seen, place))
			continue;
		g_string_append(html, "<option value=\"");
		venture_html_escape_append(html, place);
		g_string_append(html, "\">");
		g_hash_table_add(seen, g_steal_pointer(&place));
	}
}

static void
tax_preview_row(GString *html, const gchar *label, const gchar *value)
{
	g_string_append(html, "<tr><th scope=\"row\">");
	venture_html_escape_append(html, label);
	g_string_append(html, "</th><td>");
	venture_html_escape_append(html, value != NULL && *value != '\0' ? value : "None");
	g_string_append(html, "</td></tr>");
}

static gchar *
tax_entity_name(VentureWebServer *self, GType type, gint64 id)
{
	g_autoptr(VentureEntity) entity = venture_database_get(venture_context_get_database(self->context), type, id, NULL);
	return entity != NULL ? venture_entity_get_display_name(entity) : NULL;
}

static gchar *
tax_money_text(VentureEntity *record, const gchar *property)
{
	g_autoptr(VentureMoney) money = NULL;
	g_object_get(record, property, &money, NULL);
	return money != NULL ? venture_money_to_display_string(money, TRUE) : NULL;
}

static gchar *
tax_day_text(GDateTime *when)
{
	return when != NULL ? g_date_time_format(when, "%-d %B %Y") : NULL;
}

static void
tax_hidden(GString *html, const gchar *name, const gchar *value)
{
	g_string_append_printf(html, "<input type=\"hidden\" name=\"%s\" value=\"", name);
	venture_html_escape_append(html, value != NULL ? value : "");
	g_string_append(html, "\">");
}

static gint64
tax_form_int(HtmxRequest *request, const gchar *name)
{
	const gchar *value = htmx_request_get_form_value(request, name);
	if (value == NULL)
		value = htmx_request_get_query_param(request, name);
	return value != NULL ? g_ascii_strtoll(value, NULL, 10) : 0;
}

static const gchar *
tax_form_text(HtmxRequest *request, const gchar *name)
{
	const gchar *value = htmx_request_get_form_value(request, name);
	return value != NULL ? value : htmx_request_get_query_param(request, name);
}

/* What preparing would make, before anything is saved: the owner sees
 * the supplier or period by name, and the totals, and then prepares. */
static HtmxResponse *
tax_filing_preview_ui(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	VentureTaxFilingService *service;
	g_autoptr(GString) content = g_string_new(NULL);
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) draft = NULL;
	g_autofree gchar *business = NULL;
	const gchar *kind = tax_form_text(request, "kind");
	gint64 organization = tax_org(self, request);
	HtmxResponse *gate = venture_web_ui_require_session(self, request);
	(void)params;
	if (gate != NULL)
		return gate;
	gate = venture_web_require_module_ui(self, request, "tax_filing");
	if (gate != NULL)
		return gate;
	service = venture_tax_filing_service_get(venture_context_get_database(self->context));
	business = tax_entity_name(self, VENTURE_TYPE_ORGANIZATION, organization);
	g_string_append(content, "<div class=\"page-head\"><h1>Check before you prepare</h1></div>"
		"<div class=\"card\"><div class=\"card-head\"><h2>What will be prepared</h2></div><table><tbody>");
	tax_preview_row(content, "Business", business);
	if (g_strcmp0(kind, "1099") == 0)
	{
		gint64 vendor = tax_form_int(request, "vendor_id");
		gint year = (gint)tax_form_int(request, "year");
		g_autofree gchar *vendor_name = NULL;
		g_autofree gchar *paid = NULL;
		g_autofree gchar *year_text = g_strdup_printf("%d", year);
		g_autofree gchar *vendor_text = g_strdup_printf("%" G_GINT64_FORMAT, vendor);
		draft = venture_tax_filing_service_preview_1099(service, organization, vendor, year, &error);
		if (draft == NULL)
			return venture_web_error_response(error);
		vendor_name = tax_entity_name(self, VENTURE_TYPE_COMPANY, vendor);
		paid = tax_money_text(draft, "amount");
		tax_preview_row(content, "Form", "1099-NEC");
		tax_preview_row(content, "Supplier", vendor_name);
		tax_preview_row(content, "Calendar year", year_text);
		tax_preview_row(content, "Paid in the year", paid);
		g_string_append(content, "</tbody></table>");
		if (venture_entity_get_id(draft) > 0)
		{
			g_string_append_printf(content, "<p>This form is already prepared. "
				"<a href=\"/e/contractor_tax_pack/%" G_GINT64_FORMAT "\">Open it</a>.</p>",
				venture_entity_get_id(draft));
			g_string_append(content, "<p><a href=\"/tax-filings\">Back to tax filings</a></p></div>");
		}
		else
		{
			g_string_append(content, "<form method=\"post\" action=\"/tax-filings/prepare\" class=\"stack\">");
			tax_hidden(content, "kind", "1099");
			tax_hidden(content, "vendor_id", vendor_text);
			tax_hidden(content, "year", year_text);
			g_string_append(content, "<button type=\"submit\">Prepare 1099-NEC</button> "
				"<a href=\"/tax-filings\">Change</a></form></div>");
		}
	}
	else
	{
		const gchar *country = tax_form_text(request, "country");
		const gchar *jurisdiction = tax_form_text(request, "jurisdiction");
		gint64 period = tax_form_int(request, "fiscal_period_id");
		g_autofree gchar *period_name = NULL;
		g_autofree gchar *period_text = g_strdup_printf("%" G_GINT64_FORMAT, period);
		g_autofree gchar *from = NULL;
		g_autofree gchar *until = NULL;
		g_autofree gchar *dates = NULL;
		g_autofree gchar *taxable = NULL;
		g_autofree gchar *tax = NULL;
		g_autoptr(GDateTime) start = NULL;
		g_autoptr(GDateTime) end = NULL;
		g_autoptr(GDateTime) last = NULL;
		draft = venture_tax_filing_service_preview(service, organization, country, jurisdiction, period, &error);
		if (draft == NULL)
			return venture_web_error_response(error);
		period_name = tax_entity_name(self, VENTURE_TYPE_FISCAL_PERIOD, period);
		g_object_get(draft, "period-start", &start, "period-end", &end, NULL);
		/* The stored end is exclusive; a person reads the last day. */
		last = end != NULL ? g_date_time_add_days(end, -1) : NULL;
		from = tax_day_text(start);
		until = tax_day_text(last);
		dates = g_strdup_printf("%s to %s", from != NULL ? from : "?", until != NULL ? until : "?");
		taxable = tax_money_text(draft, "taxable");
		tax = tax_money_text(draft, "tax");
		tax_preview_row(content, "Return", "Sales tax");
		tax_preview_row(content, "Jurisdiction",
			!venture_string_is_empty(jurisdiction) ? jurisdiction : country);
		tax_preview_row(content, "Fiscal period", period_name);
		tax_preview_row(content, "Dates", dates);
		tax_preview_row(content, "Taxable sales", taxable);
		tax_preview_row(content, "Tax", tax);
		g_string_append(content, "</tbody></table>"
			"<form method=\"post\" action=\"/tax-filings/prepare\" class=\"stack\">");
		tax_hidden(content, "kind", "sales");
		tax_hidden(content, "country", country);
		tax_hidden(content, "jurisdiction", jurisdiction);
		tax_hidden(content, "fiscal_period_id", period_text);
		g_string_append(content, "<button type=\"submit\">Prepare return</button> "
			"<a href=\"/tax-filings\">Change</a></form></div>");
	}
	return venture_web_html_response(venture_web_page(self, request, "/tax-filings", "Check before you prepare",
		content->str), 200);
}

/* Prepares what the preview showed and opens it. The service refuses a
 * supplier or period of another business, whatever the form carried. */
static HtmxResponse *
tax_filing_prepare_ui(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	VentureTaxFilingService *service;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) made = NULL;
	g_autofree gchar *path = NULL;
	VentureActor actor;
	gint64 organization = tax_org(self, request);
	HtmxResponse *gate = venture_web_ui_require_session(self, request);
	(void)params;
	if (gate != NULL)
		return gate;
	gate = venture_web_require_module_ui(self, request, "tax_filing");
	if (gate != NULL)
		return gate;
	principal = venture_auth_authenticate(self->auth, request);
	if (!venture_web_hosted_auth_require(self, request, principal, VENTURE_USER_ROLE_EDITOR, &error))
		return venture_web_error_response(error);
	venture_auth_to_actor(principal, &actor);
	service = venture_tax_filing_service_get(venture_context_get_database(self->context));
	if (g_strcmp0(tax_form_text(request, "kind"), "1099") == 0)
	{
		made = venture_tax_filing_service_prepare_1099(service, organization,
			tax_form_int(request, "vendor_id"), (gint)tax_form_int(request, "year"), &actor, &error);
		if (made == NULL)
			return venture_web_error_response(error);
		path = g_strdup_printf("/e/contractor_tax_pack/%" G_GINT64_FORMAT, venture_entity_get_id(made));
	}
	else
	{
		made = venture_tax_filing_service_prepare(service, organization,
			tax_form_text(request, "country"), tax_form_text(request, "jurisdiction"),
			tax_form_int(request, "fiscal_period_id"), NULL, NULL, &actor, &error);
		if (made == NULL)
			return venture_web_error_response(error);
		path = g_strdup_printf("/e/tax_filing/%" G_GINT64_FORMAT, venture_entity_get_id(made));
	}
	return venture_web_redirect_to(path);
}

static HtmxResponse *
tax_filings_ui(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	g_autoptr(GString) content = g_string_new("<div class=\"page-head\"><h1>Tax filings</h1></div>");
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) years = NULL;
	gint64 organization;
	guint i;
	HtmxResponse *gate = venture_web_ui_require_session(self, request);
	(void)params;
	if (gate != NULL)
		return gate;
	gate = venture_web_require_module_ui(self, request, "tax_filing");
	if (gate != NULL)
		return gate;
	organization = tax_org(self, request);
	years = tax_fiscal_years(self, organization);
	/* Every choice is the owner's own record, by name: nobody types an id. */
	g_string_append(content,
		"<div class=\"card\"><div class=\"card-head\"><h2>Prepare a sales-tax return</h2></div>"
		"<form method=\"get\" action=\"/tax-filings/preview\" class=\"stack\">"
		"<input type=\"hidden\" name=\"kind\" value=\"sales\">"
		"<label>Country <select name=\"country\" required>");
	{
		g_autoptr(GPtrArray) adapters = venture_tax_filing_adapter_registry_list(
			venture_tax_filing_service_get_adapters(venture_tax_filing_service_get(
				venture_context_get_database(self->context))));
		guint k;
		for (k = 0; adapters != NULL && k < adapters->len; k++)
		{
			const gchar *country = venture_tax_filing_adapter_get_name(g_ptr_array_index(adapters, k));
			g_string_append(content, "<option value=\"");
			venture_html_escape_append(content, country);
			g_string_append(content, "\">");
			venture_html_escape_append(content, g_strcmp0(country, "US") == 0 ? "United States" : country);
			g_string_append(content, "</option>");
		}
	}
	g_string_append(content, "</select></label>"
		"<label>State or region <input name=\"jurisdiction\" list=\"tax-jurisdictions\" placeholder=\"US-NY\"></label>"
		"<datalist id=\"tax-jurisdictions\">");
	tax_jurisdiction_options(self, content, organization);
	g_string_append(content, "</datalist>"
		"<label>Fiscal period <select name=\"fiscal_period_id\" required>"
		"<option value=\"\">Pick a period\xe2\x80\xa6</option>");
	tax_period_options(self, content, years, organization);
	g_string_append(content, "</select></label>"
		"<span class=\"field-help\">No period listed? <a href=\"/e/fiscal_year/new\">Set up a fiscal year</a>.</span>"
		"<button type=\"submit\">Preview</button></form></div>");
	g_string_append(content,
		"<div class=\"card\"><div class=\"card-head\"><h2>Prepare 1099-NEC</h2></div>"
		"<form method=\"get\" action=\"/tax-filings/preview\" class=\"stack\">"
		"<input type=\"hidden\" name=\"kind\" value=\"1099\">"
		"<label>Supplier <select name=\"vendor_id\" required>"
		"<option value=\"\">Who you paid\xe2\x80\xa6</option>");
	tax_vendor_options(self, content, organization);
	g_string_append(content, "</select></label>"
		"<span class=\"field-help\">Not listed? <a href=\"/e/company/new\">Add them</a> "
		"as a company whose relationship is Supplier.</span>"
		"<label>Calendar year <select name=\"year\" required>");
	tax_year_options(content, years);
	g_string_append(content, "</select></label>"
		"<button type=\"submit\">Preview</button></form></div>");
	query = venture_query_new(VENTURE_TYPE_TAX_FILING);
	venture_query_set_organization(query, organization);
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
