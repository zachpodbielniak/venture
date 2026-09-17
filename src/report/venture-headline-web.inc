/*
 * The headline home page and its API. Rendered for "/" when the
 * organisation has not asked for the classic dashboard; the cards themselves
 * come from venture_headline_home_render(), which writes their JSON, HTML
 * and CSV in one pass, so this is only the page around them.
 */

/*
 * Whether the viewer may see organisation totals: a global owner or
 * administrator, or a member holding the owner, admin or finance role in
 * the organisation. Anybody else would be shown totals of only the rows
 * the record policy lets them read, presented as the organisation's
 * numbers -- a smaller, wrong figure that looks exactly like the right one.
 */
static gboolean
venture_web_headline_may_see_totals(
	VentureWebServer	*self,
	HtmxRequest		*request,
	gint64			 organization_id
){
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	static const gint roles[] = {
		VENTURE_ORGANIZATION_ROLE_OWNER,
		VENTURE_ORGANIZATION_ROLE_ADMIN,
		VENTURE_ORGANIZATION_ROLE_FINANCE
	};

	principal = venture_auth_authenticate(self->auth, request);

	return venture_access_policy_has_organization_role(
		venture_database_get_access_policy(
			venture_context_get_database(self->context)),
		principal, organization_id, roles, G_N_ELEMENTS(roles));
}

/*
 * The cards for one request: the organisation (the query parameter, else the
 * active one, else the default), venture_id and as_of when given, and the
 * period (a bad one falls back to this_month and says so in @out_notice).
 *
 * The totals are computed with the record policy set aside, once the role
 * check has decided the viewer may see them: filtering row by row is both
 * the wrong answer for a total and a membership query per row.
 */
static gboolean
venture_web_headline_render(
	VentureWebServer	 *self,
	HtmxRequest		 *request,
	JsonNode		**out_cards,
	gchar			**out_html,
	gchar			**out_csv,
	VentureDateRange	**out_period,
	gchar			**out_period_text,
	gchar			**out_notice,
	gint64			 *out_organization_id,
	GError			**error
){
	g_autoptr(VentureDateRange) period = NULL;
	g_autoptr(JsonObject) options = NULL;
	g_autoptr(VentureAccessScope) scope = NULL;
	g_autoptr(GError) period_error = NULL;
	const gchar *requested;
	const gchar *organization;
	const gchar *venture;
	const gchar *as_of;
	gint64 organization_id;
	gboolean may_see;

	requested = htmx_request_get_query_param(request, "period");
	organization = htmx_request_get_query_param(request, "organization_id");
	venture = htmx_request_get_query_param(request, "venture_id");
	as_of = htmx_request_get_query_param(request, "as_of");

	organization_id = venture_string_is_empty(organization)
		? venture_web_active_organization(self, request)
		: g_ascii_strtoll(organization, NULL, 10);

	if (organization_id <= 0)
		organization_id = venture_context_get_default_organization_id(self->context);

	if (venture_string_is_empty(requested))
		requested = "this_month";

	period = venture_context_parse_period(self->context, requested,
	                                      &period_error);

	if (NULL == period)
	{
		/* A mistyped period is a notice on the page, not a silent jump
		 * back to the old dashboard. */
		*out_notice = g_strdup_printf("%s Showing this month instead.",
		                              period_error->message);
		requested = "this_month";
		period = venture_context_parse_period(self->context, requested, error);

		if (NULL == period)
			return FALSE;
	}

	options = json_object_new();
	json_object_set_int_member(options, "organization_id", organization_id);

	if (!venture_string_is_empty(venture))
		json_object_set_int_member(options, "venture_id",
		                           g_ascii_strtoll(venture, NULL, 10));

	if (!venture_string_is_empty(as_of))
		json_object_set_string_member(options, "as_of", as_of);

	may_see = venture_web_headline_may_see_totals(self, request,
	                                              organization_id);

	if (may_see)
		scope = venture_access_policy_enter(venture_database_get_access_policy(
			venture_context_get_database(self->context)), NULL);

	if (!venture_headline_home_render(self->context, options, period,
	                                  requested, NULL, may_see, out_cards,
	                                  out_html, out_csv, error))
		return FALSE;

	*out_period = g_steal_pointer(&period);
	*out_period_text = g_strdup(requested);
	*out_organization_id = organization_id;

	return TRUE;
}

static gchar *
venture_web_render_headline_home(
	VentureWebServer	*self,
	HtmxRequest		*request
){
	g_autoptr(VentureDateRange) period = NULL;
	g_autoptr(GString) content = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *cards = NULL;
	g_autofree gchar *period_text = NULL;
	g_autofree gchar *notice = NULL;
	g_autoptr(GString) csv_link = NULL;
	gint64 organization_id;
	gsize i;

	if (!venture_web_module_enabled(self, "headline"))
		return NULL;

	organization_id = venture_web_active_organization(self, request);

	if (0 == organization_id)
		organization_id = venture_context_get_default_organization_id(self->context);

	if (!venture_headline_home_enabled(venture_context_get_database(self->context),
	                                   organization_id))
		return NULL;

	if (!venture_web_headline_render(self, request, NULL, &cards, NULL, &period,
	                                 &period_text, &notice, &organization_id,
	                                 &error))
	{
		g_autoptr(GString) body = g_string_new("<div class=\"notice negative\">");

		venture_html_escape_append(body, error->message);
		g_string_append(body, "</div>");

		return venture_web_page(self, request, "/", "Home", body->str);
	}

	content = g_string_new("<div class=\"page-head\"><div class=\"page-title\">"
	                       "<h1>The headline numbers</h1><span class=\"subtitle\">");
	venture_html_escape_append(content, venture_date_range_get_label(period));
	g_string_append(content, ", against the period before</span></div>"
	                         "<div class=\"page-actions\"><div class=\"btn-group\">");

	for (i = 0; NULL != venture_web_report_periods[i]; i++)
	{
		g_string_append_printf(content,
			"<a class=\"btn btn-sm%s\" href=\"/?period=%s\">%s</a>",
			(0 == g_strcmp0(period_text, venture_web_report_periods[i]))
				? " active" : "",
			venture_web_report_periods[i], venture_web_report_periods[i]);
	}

	csv_link = g_string_new("/api/v1/headline?format=csv&period=");
	g_string_append_uri_escaped(csv_link, period_text, NULL, FALSE);
	g_string_append_printf(csv_link, "&organization_id=%" G_GINT64_FORMAT,
	                       organization_id);

	g_string_append(content, "</div> <a class=\"btn btn-sm\" href=\"");
	venture_html_escape_append(content, csv_link->str);
	g_string_append(content, "\">Export CSV</a> "
	                         "<a class=\"btn\" href=\"/overview\">Old dashboard</a> "
	                         "<a class=\"btn\" href=\"/reports\">All reports</a>"
	                         "</div></div>");

	if (NULL != notice)
	{
		g_string_append(content, "<div class=\"notice negative\">");
		venture_html_escape_append(content, notice);
		g_string_append(content, "</div>");
	}

	g_string_append(content, "<div class=\"headline-cards widget-grid\">");
	g_string_append(content, cards);
	g_string_append(content, "</div>");

	return venture_web_page(self, request, "/", "Home", content->str);
}

/* GET /api/v1/headline - the same cards as JSON, or ?format=csv. */
static HtmxResponse *
venture_web_api_headline(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureDateRange) period = NULL;
	g_autoptr(JsonNode) cards = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *csv = NULL;
	g_autofree gchar *period_text = NULL;
	g_autofree gchar *notice = NULL;
	HtmxResponse *denied;
	gint64 organization_id;
	gboolean as_csv;

	(void)params;

	denied = venture_web_api_require(self, request, VENTURE_USER_ROLE_VIEWER);

	if (NULL != denied)
		return denied;

	if (!venture_web_module_enabled(self, "headline"))
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		                    "The headline module is off");
		return venture_web_error_response(error);
	}

	/* An API caller asked for something specific; a period that does not
	 * parse is refused rather than quietly answered about another one. */
	{
		const gchar *requested = htmx_request_get_query_param(request, "period");
		g_autoptr(VentureDateRange) check = NULL;

		if (!venture_string_is_empty(requested))
		{
			check = venture_context_parse_period(self->context, requested,
			                                     &error);

			if (NULL == check)
				return venture_web_error_response(error);
		}
	}

	as_csv = (0 == g_strcmp0(htmx_request_get_query_param(request, "format"),
	                         "csv"));

	if (!venture_web_headline_render(self, request, &cards, NULL,
	                                 as_csv ? &csv : NULL, &period,
	                                 &period_text, &notice, &organization_id,
	                                 &error))
		return venture_web_error_response(error);

	if (as_csv)
	{
		HtmxResponse *response;

		response = htmx_response_new_with_content(csv);
		htmx_response_set_content_type(response, "text/csv; charset=utf-8");

		return response;
	}

	return venture_web_json_response(cards, 200);
}
