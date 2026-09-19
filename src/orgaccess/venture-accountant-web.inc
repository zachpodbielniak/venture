/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * The Books page: what an outside accountant lands on and the only thing
 * their sidebar offers. Statements, the reports the books already answer,
 * and the year-end pack as a download -- live for a fiscal year, or the
 * retained output of a scheduled pack.
 */

/* The accountant's whole navigation. The section and module are the same
 * as the accounting home so the sidebar reads as one place, not a stub. */
static const VentureWebNavLink venture_accountant_nav_links[] = {
	{ "/books", "Books", VENTURE_ICON("<path d=\"M4 4h16v16H4zM8 8h8M8 12h8M8 16h5\"/>"), "Accounting", "statements" },
	{ NULL, NULL, NULL, NULL, NULL }
};

/*
 * Which sidebar a request gets. Everybody but an accountant-only user sees
 * the ordinary one; an accountant sees Books alone, because the lists behind
 * every other entry would be empty for them and a sidebar of empty rooms
 * reads as a broken installation rather than a scoped one.
 */
static const VentureWebNavLink *
venture_accountant_web_navigation(VentureWebServer *self, HtmxRequest *request)
{
	g_autoptr(VentureAuthPrincipal) principal = venture_auth_authenticate(self->auth, request);
	if (venture_web_module_enabled(self, "statements") &&
		venture_accountant_role_only(venture_context_get_database(self->context), principal))
		return venture_accountant_nav_links;
	return venture_web_navigation();
}

/* The inbox is personal, but it is not the books: an accountant's sidebar
 * carries Books alone, so the inbox link goes with the rest. */
static void
venture_accountant_web_append_inbox_nav(VentureWebServer *self, HtmxRequest *request, GString *html, const gchar *active)
{
	if (venture_accountant_web_navigation(self, request) == venture_accountant_nav_links)
		return;
	venture_web_append_inbox_nav(self, request, html, active);
}

static HtmxResponse *
venture_accountant_web_books(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	VentureWebServer *self = user_data;
	g_autoptr(GString) content = NULL;
	g_autoptr(GPtrArray) reports = NULL;
	g_autoptr(GPtrArray) packs = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autofree gchar *fy = NULL;
	HtmxResponse *redirect;
	guint i;
	(void)params;
	redirect = venture_web_ui_require_session(self, request);
	if (redirect != NULL)
		return redirect;
	redirect = venture_web_require_module_ui(self, request, "statements");
	if (redirect != NULL)
		return redirect;
	{
		g_autoptr(GDateTime) now = venture_time_now();
		fy = g_strdup_printf("fy_%d", g_date_time_get_year(now));
	}
	content = g_string_new("<div class=\"page-head\"><div class=\"page-title\"><h1>Books</h1>"
		"<span class=\"subtitle\">Statements, reports and the year-end pack for the selected entity</span>"
		"</div></div>");

	/* The pack first: it is what an accountant came for. */
	g_string_append(content, "<section class=\"card\"><div class=\"card-body\"><h2>Year-end pack</h2>"
		"<p class=\"muted\">Trial balance, P&amp;L, balance sheet, cash flow, AR and AP aging, general ledger, "
		"sales-tax and 1099 summaries and the fixed-asset register, as one zip of CSVs with an index.</p>"
		"<form method=\"get\" action=\"/books/year-end-pack.zip\" class=\"inline-form\">"
		"<label>Fiscal year<input name=\"period\" value=\"");
	venture_html_escape_append(content, fy);
	g_string_append(content, "\" placeholder=\"fy_2025\"></label> "
		"<label>Basis<select name=\"basis\"><option value=\"accrual\">Accrual</option>"
		"<option value=\"cash\">Cash</option></select></label> "
		"<button class=\"btn btn-primary\" type=\"submit\">Download pack</button></form>");
	query = venture_query_new(VENTURE_TYPE_REPORT_PACK);
	venture_query_set_limit(query, 0);
	venture_web_scope_to_active_organization(self, request, query);
	packs = venture_database_find(venture_context_get_database(self->context), query, NULL);
	if (packs != NULL && packs->len > 0)
	{
		g_string_append(content, "<h3>Scheduled packs</h3><table class=\"table\"><thead><tr><th>Pack</th>"
			"<th>Last run</th><th></th></tr></thead><tbody>");
		for (i = 0; i < packs->len; i++)
		{
			VentureEntity *pack = g_ptr_array_index(packs, i);
			g_autofree gchar *name = NULL;
			g_autoptr(GDateTime) last = NULL;
			g_autofree gchar *when = NULL;
			g_object_get(pack, "name", &name, "last-run-at", &last, NULL);
			when = last != NULL ? venture_time_to_string(last) : g_strdup("never");
			g_string_append(content, "<tr><td>");
			venture_html_escape_append(content, name);
			g_string_append(content, "</td><td>");
			venture_html_escape_append(content, when);
			g_string_append_printf(content, "</td><td><a class=\"btn btn-sm\" href=\"/books/packs/%" G_GINT64_FORMAT
				"/year-end-pack.zip\">Retained zip</a></td></tr>", venture_entity_get_id(pack));
		}
		g_string_append(content, "</tbody></table>");
	}
	g_string_append(content, "</div></section>");

	venture_statements_append_index(self->context, content);

	reports = venture_report_registry_list(venture_context_get_report_registry(self->context));
	g_string_append(content, "<h2>Reports</h2><div class=\"grid cols-2\">");
	for (i = 0; i < reports->len; i++)
	{
		VentureReport *report = g_ptr_array_index(reports, i);
		if (venture_statements_owns_report(self->context, venture_report_get_name(report)))
			continue;
		g_string_append(content, "<div class=\"card\"><div class=\"card-body\"><h3>");
		venture_html_escape_append(content, venture_report_get_title(report));
		g_string_append(content, "</h3><p class=\"muted\">");
		venture_html_escape_append(content, venture_report_get_description(report));
		g_string_append_printf(content, "</p><a class=\"btn btn-primary btn-sm\" href=\"/reports/%s\">Open</a> "
			"<a class=\"btn btn-sm\" href=\"/api/v1/reports/%s?format=csv\">CSV</a></div></div>",
			venture_report_get_name(report), venture_report_get_name(report));
	}
	g_string_append(content, "</div>");
	return venture_web_html_response(venture_web_page(self, request, "/books", "Books", content->str), 200);
}

static HtmxResponse *
venture_accountant_web_zip(GBytes *zip, const gchar *filename)
{
	g_autofree gchar *disposition = g_strdup_printf("attachment; filename=\"%s\"", filename);
	HtmxResponse *response = htmx_response_new();
	htmx_response_set_status(response, 200);
	htmx_response_set_bytes(response, zip);
	htmx_response_set_content_type(response, "application/zip");
	htmx_response_add_header(response, "Content-Disposition", disposition);
	return response;
}

/* GET /books/year-end-pack.zip?period=fy_2025 -- the pack, generated now
 * for the selected entity under the caller's own read authority. */
static HtmxResponse *
venture_accountant_web_pack_live(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	VentureWebServer *self = user_data;
	g_autoptr(VentureDateRange) period = NULL;
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(JsonObject) options = json_object_new();
	g_autoptr(GBytes) zip = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *filename = NULL;
	VentureReport *report;
	HtmxResponse *gate;
	const gchar *requested = htmx_request_get_query_param(request, "period");
	const gchar *basis = htmx_request_get_query_param(request, "basis");
	gint64 org;
	(void)params;
	gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_VIEWER);
	if (gate != NULL)
		return gate;
	gate = venture_web_require_module_api(self, "statements");
	if (gate != NULL)
		return gate;
	report = venture_report_registry_lookup(venture_context_get_report_registry(self->context), VENTURE_YEAR_END_PACK_REPORT);
	if (report == NULL)
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "The year-end pack report is not available");
		return venture_web_error_response(error);
	}
	period = venture_context_parse_period(self->context, venture_string_is_empty(requested) ? "fy" : requested, &error);
	if (period == NULL)
		return venture_web_error_response(error);
	org = venture_web_active_organization(self, request);
	if (org > 0)
		json_object_set_int_member(options, "organization_id", org);
	if (!venture_string_is_empty(basis))
		json_object_set_string_member(options, "basis", basis);
	result = venture_report_generate(report, self->context, period, options, &error);
	if (result == NULL)
		return venture_web_error_response(error);
	zip = venture_year_end_pack_zip(result, &error);
	if (zip == NULL)
		return venture_web_error_response(error);
	filename = g_strdup_printf("year-end-pack-%s.zip", venture_string_is_empty(requested) ? "fy" : requested);
	return venture_accountant_web_zip(zip, filename);
}

/* GET /books/packs/:id/year-end-pack.zip -- the retained output of a scheduled pack, as
 * the zip it would have been on the day it ran. */
static HtmxResponse *
venture_accountant_web_pack_retained(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	VentureWebServer *self = user_data;
	g_autoptr(VentureEntity) pack = NULL;
	g_autoptr(GBytes) zip = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *output = NULL;
	g_autofree gchar *filename = NULL;
	HtmxResponse *gate;
	const gchar *id = g_hash_table_lookup(params, "id");
	gint64 pack_id = id != NULL ? g_ascii_strtoll(id, NULL, 10) : 0;
	gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_VIEWER);
	if (gate != NULL)
		return gate;
	gate = venture_web_require_module_api(self, "statements");
	if (gate != NULL)
		return gate;
	/* The repository read runs under the request scope, so a pack outside
	 * the caller's organisations is not found rather than forbidden. */
	pack = pack_id > 0 ? venture_database_get(venture_context_get_database(self->context), VENTURE_TYPE_REPORT_PACK, pack_id, &error) : NULL;
	if (pack == NULL || venture_entity_is_deleted(pack))
	{
		g_clear_error(&error);
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "No such report pack");
		return venture_web_error_response(error);
	}
	g_object_get(pack, "last-output", &output, NULL);
	zip = venture_year_end_pack_zip_from_output(output, &error);
	if (zip == NULL)
		return venture_web_error_response(error);
	filename = g_strdup_printf("year-end-pack-%" G_GINT64_FORMAT ".zip", pack_id);
	return venture_accountant_web_zip(zip, filename);
}

static void
venture_accountant_web_register(HtmxRouter *router, VentureWebServer *self)
{
	htmx_router_get(router, "/books", venture_accountant_web_books, self);
	htmx_router_get(router, "/books/year-end-pack.zip", venture_accountant_web_pack_live, self);
	htmx_router_get(router, "/books/packs/:id/year-end-pack.zip", venture_accountant_web_pack_retained, self);
}
