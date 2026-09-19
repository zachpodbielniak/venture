/*
 * Customer health over HTTP: the sweep, and the filter row on the report
 * page. The band and reasons on a company's page come from
 * venture_customer_health_append_block(); the report itself is an ordinary
 * registry report and needs nothing here.
 */

/* POST /api/v1/customers/health/sweep - one check-in per red company. */
static HtmxResponse *
venture_web_api_customer_health_sweep(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GDateTime) as_of = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) node = NULL;
	JsonObject *object;
	HtmxResponse *gate;
	VentureActor actor;
	const gchar *requested;
	const gchar *organization;
	gint64 organization_id;
	guint limit = 0;
	gint created;

	(void)params;

	gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_EDITOR);

	if (NULL != gate)
		return gate;

	gate = venture_web_require_module_api(self, "customer_health");

	if (NULL != gate)
		return gate;

	principal = venture_auth_authenticate(self->auth, request);
	requested = htmx_request_get_query_param(request, "as_of");

	if (!venture_string_is_empty(requested))
	{
		g_autoptr(JsonObject) options = json_object_new();

		json_object_set_string_member(options, "as_of", requested);
		as_of = venture_period_report_as_of(options, &error);

		if (NULL == as_of)
			return venture_web_error_response(error);
	}

	requested = htmx_request_get_query_param(request, "limit");

	if (!venture_string_is_empty(requested))
		limit = (guint)g_ascii_strtoull(requested, NULL, 10);

	organization = htmx_request_get_query_param(request, "organization_id");
	organization_id = venture_string_is_empty(organization)
		? venture_web_active_organization(self, request)
		: g_ascii_strtoll(organization, NULL, 10);

	if (0 == organization_id)
		organization_id = venture_context_get_default_organization_id(self->context);

	/* Judged in the organisation it names: the sweep reads every customer
	 * there and writes a next action for each red one, so the caller must
	 * hold a working role in that organisation, not merely a global one. */
	{
		static const gint roles[] = {
			VENTURE_ORGANIZATION_ROLE_OWNER,
			VENTURE_ORGANIZATION_ROLE_ADMIN,
			VENTURE_ORGANIZATION_ROLE_EDITOR,
			VENTURE_ORGANIZATION_ROLE_FINANCE,
			VENTURE_ORGANIZATION_ROLE_SALES
		};

		if (!venture_access_policy_has_organization_role(
			venture_database_get_access_policy(venture_context_get_database(self->context)),
			principal, organization_id, roles, G_N_ELEMENTS(roles)))
		{
			g_set_error(&error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED,
			            "The customer health sweep needs a working role in "
			            "organization %" G_GINT64_FORMAT, organization_id);
			return venture_web_error_response(error);
		}
	}

	venture_auth_to_actor(principal, &actor);
	created = venture_customer_health_sweep(self->context, organization_id, as_of,
	                                        limit, &actor, &error);

	if (created < 0)
		return venture_web_error_response(error);

	object = json_object_new();
	json_object_set_int_member(object, "created", created);
	node = json_node_new(JSON_NODE_OBJECT);
	json_node_take_object(node, object);

	return venture_web_json_response(node, 200);
}

/*
 * The filter row above the customer health report: band, owner and sort,
 * as a plain GET form so the URL it produces is the one the churn card
 * links to and the one a bookmark keeps.
 */
static void
venture_web_customer_health_controls(
	HtmxRequest	*request,
	VentureReport	*report,
	GString		*content
){
	static const gchar *const bands[] = { "", "red", "amber", "green", NULL };
	static const gchar *const sorts[] = {
		"", "company", "owner", "band", "-days_since_touch", "-overdue_days",
		"-open_tickets", "-revenue_12m", NULL
	};
	const gchar *band;
	const gchar *owner;
	const gchar *sort;
	const gchar *period;
	const gchar *as_of;
	const gchar *organization;
	guint i;

	if (0 != g_strcmp0(venture_report_get_name(report), "customer_health"))
		return;

	band = htmx_request_get_query_param(request, "band");
	owner = htmx_request_get_query_param(request, "owner");
	sort = htmx_request_get_query_param(request, "sort");
	period = htmx_request_get_query_param(request, "period");
	as_of = htmx_request_get_query_param(request, "as_of");
	organization = htmx_request_get_query_param(request, "organization_id");

	g_string_append(content, "<form method=\"get\" class=\"form-grid customer-health-filters\">");

	if (!venture_string_is_empty(period))
	{
		g_string_append(content, "<input type=\"hidden\" name=\"period\" value=\"");
		venture_html_escape_append(content, period);
		g_string_append(content, "\">");
	}

	if (!venture_string_is_empty(as_of))
	{
		g_string_append(content, "<input type=\"hidden\" name=\"as_of\" value=\"");
		venture_html_escape_append(content, as_of);
		g_string_append(content, "\">");
	}

	if (!venture_string_is_empty(organization))
	{
		g_string_append(content, "<input type=\"hidden\" name=\"organization_id\" value=\"");
		venture_html_escape_append(content, organization);
		g_string_append(content, "\">");
	}

	g_string_append(content, "<label>Band<select name=\"band\">");

	for (i = 0; NULL != bands[i]; i++)
	{
		g_string_append_printf(content, "<option value=\"%s\"%s>%s</option>", bands[i],
		                       (0 == g_strcmp0(bands[i], (NULL != band) ? band : ""))
		                               ? " selected" : "",
		                       ('\0' == bands[i][0]) ? "all" : bands[i]);
	}

	g_string_append(content, "</select></label><label>Owner<input name=\"owner\" value=\"");
	venture_html_escape_append(content, (NULL != owner) ? owner : "");
	g_string_append(content, "\" placeholder=\"username\"></label><label>Sort<select name=\"sort\">");

	for (i = 0; NULL != sorts[i]; i++)
	{
		g_string_append_printf(content, "<option value=\"%s\"%s>%s</option>", sorts[i],
		                       (0 == g_strcmp0(sorts[i], (NULL != sort) ? sort : ""))
		                               ? " selected" : "",
		                       ('\0' == sorts[i][0]) ? "worst first" : sorts[i]);
	}

	g_string_append(content, "</select></label>"
	                         "<button class=\"btn\" type=\"submit\">Filter</button></form>");
}
