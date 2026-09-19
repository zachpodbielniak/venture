/*
 * The support cost line on a company's page. The figure is
 * venture_support_rollup_cost() for this month, the same function the
 * Support card on the home page reads its number from, so the page and the
 * card cannot disagree. Shown only to a viewer who may see organisation
 * totals, by the same rule as the cards.
 */
static void
venture_web_support_rollup_append_company_block(
	VentureWebServer	*self,
	HtmxRequest		*request,
	GString			*content,
	VentureEntity		*record
){
	g_autoptr(VentureDateRange) period = NULL;
	g_autoptr(JsonObject) options = NULL;
	g_autoptr(VentureMetric) cost = NULL;
	g_autoptr(GError) error = NULL;
	gint64 organization_id;

	if ((VENTURE_TYPE_COMPANY != G_OBJECT_TYPE(record)) ||
	    !venture_web_module_enabled(self, "tickets"))
		return;

	organization_id = venture_entity_get_organization_id(record);

	if (!venture_web_headline_may_see_totals(self, request, organization_id))
		return;

	period = venture_context_parse_period(self->context, "this_month", &error);

	if (NULL != period)
	{
		options = json_object_new();
		json_object_set_int_member(options, "organization_id", organization_id);
		json_object_set_int_member(options, "company",
		                           venture_entity_get_id(record));
		cost = venture_support_rollup_cost(self->context, options, period,
		                                   &error);
	}

	g_string_append(content,
		"<section class=\"card\" data-block=\"support-cost\"><h2>Support</h2>"
		"<dl class=\"detail\"><dt>Support cost (this month)</dt><dd>");

	if (NULL != cost)
	{
		g_autofree gchar *value = venture_metric_format_value(cost);

		venture_html_escape_append(content, value);
	}
	else
	{
		g_string_append(content, "n/a");

		if (NULL != error)
		{
			g_string_append(content, " <span class=\"muted\">");
			venture_html_escape_append(content, error->message);
			g_string_append(content, "</span>");
		}
	}

	g_string_append_printf(content,
		"</dd></dl><p><a href=\"/reports/support_rollup?period=this_month"
		"&amp;company=%" G_GINT64_FORMAT "&amp;organization_id=%" G_GINT64_FORMAT
		"\">Support rollup</a></p></section>",
		venture_entity_get_id(record), organization_id);
}
