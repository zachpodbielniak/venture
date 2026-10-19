/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * The owner's late-fee pages: the settings and schedules (edited through
 * the generated record forms; this page only reads them), the approval
 * queue, an invoice's fee timeline, and the fee panel on the invoice
 * page. Every write is a form that posts to a route calling the late-fee
 * service, the same audited path the REST actions and the assistant use.
 * Owner or administrator only: a fee is a charge on a customer.
 */

static HtmxResponse *
late_fees_gate(VentureWebServer *self, HtmxRequest *request, VentureAuthPrincipal **out)
{
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GError) error = NULL;
	HtmxResponse *gate = venture_web_ui_require_session(self, request);
	if (gate != NULL)
		return gate;
	principal = venture_auth_authenticate(self->auth, request);
	if (!venture_web_hosted_auth_require(self, request, principal, VENTURE_USER_ROLE_ADMIN, &error))
		return venture_web_error_response(error);
	gate = venture_web_require_module_ui(self, request, "late_fees");
	if (gate != NULL)
		return gate;
	if (out != NULL)
		*out = g_steal_pointer(&principal);
	return NULL;
}

static gint64
late_fees_org(VentureWebServer *self, HtmxRequest *request)
{
	gint64 org = venture_web_active_organization(self, request);
	return org != 0 ? org : venture_context_get_default_organization_id(self->context);
}

static gchar *
late_fees_text(VentureEntity *e, const gchar *field)
{
	gchar *value = NULL;
	g_object_get(e, field, &value, NULL);
	return value;
}

static gint64
late_fees_number(VentureEntity *e, const gchar *field)
{
	gint64 value = 0;
	g_object_get(e, field, &value, NULL);
	return value;
}

static gboolean
late_fees_flag(VentureEntity *e, const gchar *field)
{
	gboolean value = FALSE;
	g_object_get(e, field, &value, NULL);
	return value;
}

static void
late_fees_append_money(GString *html, VentureEntity *e, const gchar *field)
{
	g_autoptr(VentureMoney) m = NULL;
	g_object_get(e, field, &m, NULL);
	if (m != NULL)
	{
		g_autofree gchar *shown = venture_money_to_display_string(m, TRUE);
		venture_html_escape_append(html, shown);
	}
	else
		g_string_append(html, "\xe2\x80\x94");
}

static void
late_fees_append_date(GString *html, VentureEntity *e, const gchar *field)
{
	GDateTime *at = NULL;
	g_object_get(e, field, &at, NULL);
	if (at != NULL)
	{
		g_autofree gchar *day = g_date_time_format(at, "%Y-%m-%d");
		g_string_append(html, day);
		g_date_time_unref(at);
	}
	else
		g_string_append(html, "\xe2\x80\x94");
}

static const gchar *
late_fees_status_label(const gchar *status)
{
	if (g_strcmp0(status, "scheduled") == 0) return "announced";
	if (g_strcmp0(status, "awaiting_notice") == 0) return "waiting for notice";
	if (g_strcmp0(status, "proposed") == 0) return "awaiting approval";
	if (g_strcmp0(status, "skipped_condition") == 0) return "condition not met";
	if (g_strcmp0(status, "before_start") == 0) return "before late fees began";
	return status != NULL ? status : "";
}

static void
late_fees_append_badge(GString *html, const gchar *status)
{
	const gchar *tone = "badge-muted";
	if (g_strcmp0(status, "applied") == 0 || g_strcmp0(status, "due") == 0)
		tone = "badge-warn";
	else if (g_strcmp0(status, "waived") == 0 || g_strcmp0(status, "upcoming") == 0 || g_strcmp0(status, "scheduled") == 0)
		tone = "badge-ok";
	g_string_append_printf(html, "<span class=\"badge %s\">", tone);
	venture_html_escape_append(html, late_fees_status_label(status));
	g_string_append(html, "</span>");
}

static GPtrArray *
late_fees_find(VentureWebServer *self, GType type, gint64 org, const gchar *field, const gchar *value, gint64 id, guint limit)
{
	g_autoptr(VentureQuery) query = venture_query_new(type);
	venture_query_set_organization(query, org);
	if (field != NULL && value != NULL)
		venture_query_add_filter_string(query, field, VENTURE_FILTER_OP_EQ, value, NULL);
	else if (field != NULL)
		venture_query_add_filter_int(query, field, VENTURE_FILTER_OP_EQ, id, NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	venture_query_set_limit(query, limit);
	return venture_database_find(venture_context_get_database(self->context), query, NULL);
}

/* A tier as a person reads it: "Day 15, once: $25.00 flat". */
static void
late_fees_append_tiers(GString *html, VentureEntity *schedule)
{
	g_autofree gchar *json = late_fees_text(schedule, "tiers");
	g_autoptr(JsonNode) root = venture_string_is_empty(json) ? NULL : json_from_string(json, NULL);
	JsonArray *tiers;
	guint i;
	if (root == NULL || !JSON_NODE_HOLDS_ARRAY(root) || json_array_get_length(json_node_get_array(root)) == 0)
	{
		g_string_append(html, "<p class=\"muted\">No tiers yet.</p>");
		return;
	}
	tiers = json_node_get_array(root);
	g_string_append(html, "<div class=\"table-wrap\"><table class=\"data\"><thead><tr><th>#</th><th>When</th><th>Charge</th>"
		"<th>Cap</th><th>Rounding</th><th>Only if</th></tr></thead><tbody>");
	for (i = 0; i < json_array_get_length(tiers); i++)
	{
		JsonObject *t = json_array_get_object_element(tiers, i);
		const gchar *type = json_object_get_string_member_with_default(t, "type", "flat");
		gint64 day = json_object_get_int_member_with_default(t, "day", -1);
		gint64 every = json_object_get_int_member_with_default(t, "every", 0);
		gint64 after = json_object_get_int_member_with_default(t, "after", -1);
		g_autofree gchar *when_text = NULL;
		g_autofree gchar *charge = NULL;
		g_autofree gchar *percent = NULL;
		JsonNode *pnode = json_object_get_member(t, "percent");
		if (pnode != NULL && JSON_NODE_HOLDS_VALUE(pnode))
			percent = json_node_get_value_type(pnode) == G_TYPE_STRING ? g_strdup(json_node_get_string(pnode)) :
				g_strdup_printf("%" G_GINT64_FORMAT, json_node_get_int(pnode));
		if (day < 0 && after >= 0)
			day = after + every;
		when_text = every > 0 ? g_strdup_printf("Day %" G_GINT64_FORMAT ", then every %" G_GINT64_FORMAT " days", day, every) :
			g_strdup_printf("Day %" G_GINT64_FORMAT ", once", day);
		if (g_strcmp0(type, "flat") == 0)
			charge = g_strdup_printf("%s flat", json_object_get_string_member_with_default(t, "amount", ""));
		else if (g_strcmp0(type, "percent_original") == 0)
			charge = g_strdup_printf("%s%% of the original invoice", percent ? percent : "?");
		else if (g_strcmp0(type, "markup") == 0)
			charge = g_strdup_printf("%s%% added to everything outstanding", percent ? percent : "?");
		else
			charge = g_strdup_printf("%s%% of the open balance", percent ? percent : "?");
		g_string_append_printf(html, "<tr><td>%u</td><td>", i + 1);
		venture_html_escape_append(html, when_text);
		g_string_append(html, "</td><td>");
		venture_html_escape_append(html, charge);
		g_string_append(html, "</td><td>");
		venture_html_escape_append(html, json_object_get_string_member_with_default(t, "cap", "\xe2\x80\x94"));
		g_string_append(html, "</td><td>");
		venture_html_escape_append(html, json_object_get_string_member_with_default(t, "round", "half_even"));
		g_string_append(html, "</td><td>");
		if (json_object_has_member(t, "conditions") && JSON_NODE_HOLDS_OBJECT(json_object_get_member(t, "conditions")))
		{
			g_autofree gchar *c = venture_json_to_string(json_object_get_member(t, "conditions"), FALSE);
			g_string_append(html, "<code>");
			venture_html_escape_append(html, c);
			g_string_append(html, "</code>");
		}
		else
			g_string_append(html, "always");
		g_string_append(html, "</td></tr>");
	}
	g_string_append(html, "</tbody></table></div>");
}

static void
late_fees_append_pause_form(GString *html, const gchar *scope, gint64 id, gboolean paused, const gchar *back)
{
	g_string_append(html, "<form method=\"post\" action=\"/late-fees/pause\" class=\"inline-form\">");
	g_string_append_printf(html, "<input type=\"hidden\" name=\"scope\" value=\"%s\"><input type=\"hidden\" name=\"id\" value=\"%" G_GINT64_FORMAT "\">",
		scope, id);
	g_string_append(html, "<input type=\"hidden\" name=\"return\" value=\"");
	venture_html_escape_append(html, back);
	g_string_append(html, "\">");
	if (paused)
		g_string_append(html, "<input type=\"hidden\" name=\"resume\" value=\"1\"><button class=\"btn\" type=\"submit\">Resume late fees</button>");
	else
		g_string_append(html, "<label>Until <input type=\"date\" name=\"until\" aria-label=\"Paused until (empty is indefinitely)\"></label>"
			"<label>Reason <input name=\"reason\" placeholder=\"Why\"></label>"
			"<button class=\"btn\" type=\"submit\">Pause late fees</button>");
	g_string_append(html, "</form>");
}

static HtmxResponse *
late_fees_page(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	HtmxResponse *gate = late_fees_gate(self, request, NULL);
	gint64 org;
	g_autoptr(GString) html = NULL;
	g_autoptr(GPtrArray) settings = NULL;
	g_autoptr(GPtrArray) schedules = NULL;
	g_autoptr(GPtrArray) proposed = NULL;
	g_autoptr(GPtrArray) waiting = NULL;
	g_autoptr(GPtrArray) paused_companies = NULL;
	g_autoptr(GPtrArray) paused_invoices = NULL;
	VentureEntity *setting;
	gint64 default_id = 0;
	guint i;
	(void)params;
	if (gate != NULL)
		return gate;
	org = late_fees_org(self, request);
	settings = late_fees_find(self, VENTURE_TYPE_LATE_FEE_SETTING, org, NULL, NULL, 0, 1);
	setting = settings != NULL && settings->len > 0 ? g_ptr_array_index(settings, 0) : NULL;
	schedules = late_fees_find(self, VENTURE_TYPE_LATE_FEE_SCHEDULE, org, NULL, NULL, 0, 200);
	proposed = late_fees_find(self, VENTURE_TYPE_LATE_FEE, org, "status", "proposed", 0, 1000);
	waiting = late_fees_find(self, VENTURE_TYPE_LATE_FEE, org, "status", "awaiting_notice", 0, 1000);
	paused_companies = late_fees_find(self, VENTURE_TYPE_COMPANY, org, "late-fees-paused", "true", 0, 200);
	paused_invoices = late_fees_find(self, VENTURE_TYPE_INVOICE, org, "late-fees-paused", "true", 0, 200);
	if (setting != NULL)
		default_id = late_fees_number(setting, "default-schedule-id");
	html = g_string_new("<div class=\"late-fees\"><div class=\"page-head\"><div class=\"page-title\"><h1>Late fees</h1><span class=\"subtitle\">");
	if (setting == NULL || !late_fees_flag(setting, "enabled"))
		g_string_append(html, "Off");
	else if (late_fees_flag(setting, "auto-apply"))
		g_string_append(html, "On, applied automatically");
	else
		g_string_append(html, "On, each fee waits for your approval");
	g_string_append(html, "</span></div><div class=\"page-actions\">");
	if (setting != NULL)
		g_string_append_printf(html, "<a class=\"btn\" href=\"/e/late_fee_setting/%" G_GINT64_FORMAT "/edit\">Settings</a> ",
			venture_entity_get_id(setting));
	else
		g_string_append(html, "<a class=\"btn btn-primary\" href=\"/e/late_fee_setting/new\">Set up late fees</a> ");
	g_string_append_printf(html, "<a class=\"btn%s\" href=\"/late-fees/queue\">Approval queue (%u)</a> <a class=\"btn\" href=\"/reports/late_fees\">Report</a></div></div>",
		proposed != NULL && proposed->len > 0 ? " btn-primary" : "", proposed != NULL ? proposed->len : 0);
	if (setting == NULL || !late_fees_flag(setting, "enabled"))
		g_string_append(html, "<div class=\"notice\">Late fees are off. Nothing is charged, proposed or mentioned to a customer until you switch them on in Settings.</div>");
	/* Pauses first: where fees would apply, a pause is the thing to see. */
	g_string_append(html, "<div class=\"grid cols-2\"><div class=\"card\"><div class=\"card-head\"><h2>Organization</h2></div><div class=\"card-body\">");
	g_string_append(html, "<div class=\"stat-row\">");
	g_string_append_printf(html, "<div class=\"stat\"><span class=\"stat-value\">%u</span><span class=\"stat-label\">Awaiting approval</span></div>",
		proposed != NULL ? proposed->len : 0);
	g_string_append_printf(html, "<div class=\"stat\"><span class=\"stat-value\">%u</span><span class=\"stat-label\">Waiting for notice</span></div>",
		waiting != NULL ? waiting->len : 0);
	g_string_append_printf(html, "<div class=\"stat\"><span class=\"stat-value\">%u</span><span class=\"stat-label\">Schedules</span></div></div>",
		schedules != NULL ? schedules->len : 0);
	if (setting != NULL && late_fees_flag(setting, "paused"))
	{
		g_autofree gchar *reason = late_fees_text(setting, "pause-reason");
		g_string_append(html, "<p class=\"notice\" role=\"status\"><span><strong>Paused for the whole organization</strong> until ");
		late_fees_append_date(html, setting, "paused-until");
		if (!venture_string_is_empty(reason))
		{
			g_string_append(html, ": ");
			venture_html_escape_append(html, reason);
		}
		g_string_append(html, ". Nothing is added and reminders carry no fee wording.</span></p>");
	}
	late_fees_append_pause_form(html, "organization", org, setting != NULL && late_fees_flag(setting, "paused"), "/late-fees");
	g_string_append(html, "</div></div><div class=\"card\"><div class=\"card-head\"><h2>Paused customers and invoices</h2></div><div class=\"card-body\">");
	if ((paused_companies == NULL || paused_companies->len == 0) && (paused_invoices == NULL || paused_invoices->len == 0))
		g_string_append(html, "<p class=\"muted\">None. Pause one from its page or ask the assistant to.</p>");
	else
	{
		g_string_append(html, "<ul class=\"plain\">");
		for (i = 0; paused_companies != NULL && i < paused_companies->len; i++)
		{
			VentureEntity *c = g_ptr_array_index(paused_companies, i);
			g_autofree gchar *name = late_fees_text(c, "name");
			g_string_append_printf(html, "<li><a href=\"/e/company/%" G_GINT64_FORMAT "\">", venture_entity_get_id(c));
			venture_html_escape_append(html, name);
			g_string_append(html, "</a> (customer) until ");
			late_fees_append_date(html, c, "late-fees-paused-until");
			g_string_append(html, "</li>");
		}
		for (i = 0; paused_invoices != NULL && i < paused_invoices->len; i++)
		{
			VentureEntity *inv = g_ptr_array_index(paused_invoices, i);
			g_autofree gchar *number = late_fees_text(inv, "number");
			g_string_append_printf(html, "<li><a href=\"/e/invoice/%" G_GINT64_FORMAT "\">", venture_entity_get_id(inv));
			venture_html_escape_append(html, number);
			g_string_append(html, "</a> (invoice) until ");
			late_fees_append_date(html, inv, "late-fees-paused-until");
			g_string_append(html, "</li>");
		}
		g_string_append(html, "</ul>");
	}
	g_string_append(html, "</div></div></div>");
	/* Schedules */
	g_string_append(html, "<div class=\"card\"><div class=\"card-head\"><h2>Schedules</h2>"
		"<a class=\"btn btn-sm\" href=\"/e/late_fee_schedule/new\">New schedule</a></div><div class=\"card-body\">");
	if (schedules == NULL || schedules->len == 0)
		g_string_append(html, "<p class=\"muted\">No schedules. A schedule is an ordered list of tiers: what to charge, on which day late, and the caps.</p>");
	for (i = 0; schedules != NULL && i < schedules->len; i++)
	{
		VentureEntity *s = g_ptr_array_index(schedules, i);
		g_autofree gchar *name = late_fees_text(s, "name");
		g_autofree gchar *percent = late_fees_text(s, "max-percent");
		g_autofree gchar *types = late_fees_text(s, "invoice-types");
		g_string_append(html, "<section class=\"late-fee-schedule\"><h3>");
		venture_html_escape_append(html, name);
		if (venture_entity_get_id(s) == default_id)
			g_string_append(html, " <span class=\"badge badge-ok\">default</span>");
		g_string_append_printf(html, " <a class=\"btn btn-sm\" href=\"/e/late_fee_schedule/%" G_GINT64_FORMAT "/edit\">Edit</a></h3><p class=\"muted\">Grace %"
			G_GINT64_FORMAT " days. Caps: ", venture_entity_get_id(s), late_fees_number(s, "grace-days"));
		if (!venture_string_is_empty(percent))
		{
			venture_html_escape_append(html, percent);
			g_string_append(html, "% of the original invoice; ");
		}
		g_string_append(html, "at most ");
		late_fees_append_money(html, s, "max-total");
		g_string_append(html, " in total.");
		if (!venture_string_is_empty(types))
		{
			g_string_append(html, " Governs invoice types: ");
			venture_html_escape_append(html, types);
			g_string_append(html, ".");
		}
		g_string_append(html, "</p>");
		late_fees_append_tiers(html, s);
		g_string_append(html, "</section>");
	}
	g_string_append(html, "</div></div><p class=\"muted\">Late-fee limits vary by jurisdiction. The schedule supports a cap per tier, a cap as a percent of the "
		"original invoice and an absolute maximum; set them for your location. No fee is added without a reminder that stated it, or your "
		"decision to skip that notice.</p></div>");
	return venture_web_html_response(venture_web_page(self, request, "/late-fees", "Late fees", html->str), 200);
}

static HtmxResponse *
late_fees_queue_page(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	HtmxResponse *gate = late_fees_gate(self, request, NULL);
	VentureDatabase *db = venture_context_get_database(self->context);
	g_autoptr(GString) html = NULL;
	static const gchar *const states[] = { "proposed", "awaiting_notice", NULL };
	const gchar *done = htmx_request_get_query_param(request, "done");
	guint s, i, shown = 0;
	gint64 org;
	(void)params;
	if (gate != NULL)
		return gate;
	org = late_fees_org(self, request);
	html = g_string_new("<div class=\"late-fees\"><div class=\"page-head\"><div class=\"page-title\"><h1>Late fee approvals</h1>"
		"<span class=\"subtitle\">Nothing here is on any balance yet</span></div><div class=\"page-actions\"><a class=\"btn\" href=\"/late-fees\">Late fees</a></div></div>");
	if (!venture_string_is_empty(done))
	{
		g_string_append(html, "<div class=\"notice\" role=\"status\">");
		venture_html_escape_append(html, done);
		g_string_append(html, "</div>");
	}
	g_string_append(html, "<form method=\"post\" action=\"/late-fees/decide\"><div class=\"card\"><div class=\"card-body\"><div class=\"table-wrap\"><table class=\"data stack-cards\">"
		"<thead><tr><th><span class=\"sr-only\">Select</span></th><th>Invoice</th><th>Customer</th><th>Due on</th><th>Amount</th><th>Basis</th><th>State</th><th>Decide</th></tr></thead><tbody>");
	for (s = 0; states[s] != NULL; s++)
	{
		g_autoptr(GPtrArray) rows = late_fees_find(self, VENTURE_TYPE_LATE_FEE, org, "status", states[s], 0, 500);
		for (i = 0; rows != NULL && i < rows->len; i++)
		{
			VentureEntity *fee = g_ptr_array_index(rows, i);
			gint64 id = venture_entity_get_id(fee);
			g_autoptr(VentureEntity) invoice = venture_database_get(db, VENTURE_TYPE_INVOICE, late_fees_number(fee, "invoice-id"), NULL);
			g_autoptr(VentureEntity) company = venture_database_get(db, VENTURE_TYPE_COMPANY, late_fees_number(fee, "company-id"), NULL);
			g_autofree gchar *number = invoice != NULL ? late_fees_text(invoice, "number") : NULL;
			g_autofree gchar *name = company != NULL ? late_fees_text(company, "name") : NULL;
			g_autofree gchar *basis = late_fees_text(fee, "basis");
			shown++;
			g_string_append_printf(html, "<tr><td><input type=\"checkbox\" name=\"ids\" value=\"%" G_GINT64_FORMAT "\" aria-label=\"Select fee %" G_GINT64_FORMAT "\"%s></td>"
				"<td class=\"row-head\"><a href=\"/e/invoice/%" G_GINT64_FORMAT "\">", id, id, s == 0 ? "" : " disabled", late_fees_number(fee, "invoice-id"));
			venture_html_escape_append(html, number ? number : "");
			g_string_append(html, "</a></td><td data-label=\"Customer\">");
			venture_html_escape_append(html, name ? name : "");
			g_string_append(html, "</td><td data-label=\"Due on\">");
			late_fees_append_date(html, fee, "due-on");
			g_string_append(html, "</td><td class=\"num\" data-label=\"Amount\">");
			late_fees_append_money(html, fee, "amount");
			g_string_append(html, "</td><td data-label=\"Basis\">");
			venture_html_escape_append(html, basis ? basis : "");
			g_string_append(html, "</td><td data-label=\"State\">");
			late_fees_append_badge(html, states[s]);
			g_string_append(html, "</td><td class=\"actions\">");
			if (s == 0)
				g_string_append_printf(html, "<button class=\"btn btn-sm btn-primary\" formaction=\"/late-fees/%" G_GINT64_FORMAT "/approve\">Approve</button> "
					"<button class=\"btn btn-sm\" formaction=\"/late-fees/%" G_GINT64_FORMAT "/skip\">Skip</button>", id, id);
			else
				g_string_append_printf(html, "<button class=\"btn btn-sm\" formaction=\"/late-fees/%" G_GINT64_FORMAT "/skip-notice\" "
					"title=\"No reminder stated this fee in advance\">Proceed without notice</button> "
					"<button class=\"btn btn-sm\" formaction=\"/late-fees/%" G_GINT64_FORMAT "/skip\">Skip</button>", id, id);
			g_string_append(html, "</td></tr>");
		}
	}
	if (shown == 0)
		g_string_append(html, "<tr><td colspan=\"8\" class=\"muted\">Nothing waits for a decision.</td></tr>");
	g_string_append(html, "</tbody></table></div></div><div class=\"card-body invoice-actions\">"
		"<button class=\"btn btn-primary\" name=\"decision\" value=\"approve\" type=\"submit\">Approve selected</button> "
		"<button class=\"btn\" name=\"decision\" value=\"skip\" type=\"submit\">Skip selected</button> "
		"<label>Reason for skipping <input name=\"reason\" placeholder=\"Optional\"></label></div></div></form>"
		"<p class=\"muted\">Approving adds the fee as its own adjustment on the invoice; the invoice's lines never change. "
		"A fee waits for notice when no reminder stated it before its day.</p></div>");
	return venture_web_html_response(venture_web_page(self, request, "/late-fees", "Late fee approvals", html->str), 200);
}

/* Every value of @name in an urlencoded body: checkboxes repeat. */
static GPtrArray *
late_fees_form_all(HtmxRequest *request, const gchar *name)
{
	GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
	const gchar *body = htmx_request_get_body(request);
	g_auto(GStrv) pairs = body != NULL ? g_strsplit(body, "&", -1) : NULL;
	guint i;
	for (i = 0; pairs != NULL && pairs[i] != NULL; i++)
	{
		gchar *eq = strchr(pairs[i], '=');
		g_autofree gchar *key = NULL;
		if (eq == NULL)
			continue;
		key = g_uri_unescape_segment(pairs[i], eq, NULL);
		if (g_strcmp0(key, name) == 0)
		{
			g_autofree gchar *plus = g_strdup(eq + 1);
			g_strdelimit(plus, "+", ' ');
			g_ptr_array_add(out, g_uri_unescape_string(plus, NULL));
		}
	}
	return out;
}

static HtmxResponse *
late_fees_back(HtmxRequest *request, const gchar *fallback, const gchar *message)
{
	const gchar *back = htmx_request_get_form_value(request, "return");
	g_autofree gchar *encoded = message != NULL ? g_uri_escape_string(message, NULL, FALSE) : NULL;
	g_autofree gchar *target = NULL;
	/* Only a path of this site: never a link out. */
	if (back == NULL || back[0] != '/' || back[1] == '/' || strchr(back, '\\') != NULL)
		back = fallback;
	target = encoded != NULL ? g_strdup_printf("%s%sdone=%s", back, strchr(back, '?') ? "&" : "?", encoded) : g_strdup(back);
	return venture_web_redirect_to(target);
}

static HtmxResponse *
late_fees_decide(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	HtmxResponse *gate = late_fees_gate(self, request, &principal);
	g_autoptr(GPtrArray) values = NULL;
	g_autoptr(GArray) ids = g_array_new(FALSE, FALSE, sizeof(gint64));
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) answer = NULL;
	g_autofree gchar *message = NULL;
	VentureActor actor;
	guint i;
	(void)params;
	if (gate != NULL)
		return gate;
	values = late_fees_form_all(request, "ids");
	for (i = 0; i < values->len; i++)
	{
		gint64 id = g_ascii_strtoll(g_ptr_array_index(values, i), NULL, 10);
		if (id > 0)
			g_array_append_val(ids, id);
	}
	if (ids->len == 0)
		return late_fees_back(request, "/late-fees/queue", "Select at least one fee");
	venture_auth_to_actor(principal, &actor);
	answer = venture_late_fee_service_decide(venture_late_fee_service_get(venture_context_get_database(self->context)),
		late_fees_org(self, request), (const gint64 *)(gpointer)ids->data, ids->len,
		g_strcmp0(htmx_request_get_form_value(request, "decision"), "skip") != 0,
		htmx_request_get_form_value(request, "reason"), &actor, &error);
	if (answer == NULL)
		return venture_web_error_response(error);
	message = g_strdup_printf("%" G_GINT64_FORMAT " decided, %" G_GINT64_FORMAT " refused",
		json_object_get_int_member(json_node_get_object(answer), "decided"),
		json_object_get_int_member(json_node_get_object(answer), "failed"));
	return late_fees_back(request, "/late-fees/queue", message);
}

static HtmxResponse *
late_fees_fee_post(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	HtmxResponse *gate = late_fees_gate(self, request, &principal);
	VentureDatabase *db = venture_context_get_database(self->context);
	VentureLateFeeService *service;
	g_autoptr(VentureEntity) fee = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *verb = g_hash_table_lookup(params, "verb");
	const gchar *reason = htmx_request_get_form_value(request, "reason");
	VentureActor actor;
	gboolean ok;
	if (gate != NULL)
		return gate;
	service = venture_late_fee_service_get(db);
	fee = venture_database_get(db, VENTURE_TYPE_LATE_FEE, g_ascii_strtoll(g_hash_table_lookup(params, "id"), NULL, 10), &error);
	if (fee == NULL || venture_entity_get_organization_id(fee) != late_fees_org(self, request))
	{
		if (error == NULL)
			g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "No such late fee");
		return venture_web_error_response(error);
	}
	venture_auth_to_actor(principal, &actor);
	if (g_strcmp0(verb, "approve") == 0)
		ok = venture_late_fee_service_approve(service, fee, NULL, &actor, &error);
	else if (g_strcmp0(verb, "skip") == 0)
		ok = venture_late_fee_service_skip(service, fee, reason, &actor, &error);
	else if (g_strcmp0(verb, "skip-notice") == 0)
		ok = venture_late_fee_service_skip_notice(service, fee, reason, &actor, &error);
	else if (g_strcmp0(verb, "waive") == 0)
		ok = venture_late_fee_service_waive(service, fee, reason, htmx_request_get_form_value(request, "note"), NULL, &actor, &error);
	else
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "No such late-fee action");
		ok = FALSE;
	}
	if (!ok)
		return venture_web_error_response(error);
	return late_fees_back(request, "/late-fees/queue", g_strcmp0(verb, "waive") == 0 ? "Late fee waived" : "Decided");
}

static HtmxResponse *
late_fees_pause_post(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	HtmxResponse *gate = late_fees_gate(self, request, &principal);
	VentureDatabase *db = venture_context_get_database(self->context);
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) until = NULL;
	const gchar *scope = htmx_request_get_form_value(request, "scope");
	const gchar *until_text = htmx_request_get_form_value(request, "until");
	const gchar *id_text = htmx_request_get_form_value(request, "id");
	gint64 org;
	gint64 id = id_text != NULL ? g_ascii_strtoll(id_text, NULL, 10) : 0;
	VentureActor actor;
	gboolean ok;
	(void)params;
	if (gate != NULL)
		return gate;
	org = late_fees_org(self, request);
	/* The pause is judged in the organization the page is showing. */
	if (g_strcmp0(scope, "organization") == 0)
		id = org;
	else
	{
		g_autoptr(VentureEntity) row = venture_database_get(db, g_strcmp0(scope, "invoice") == 0 ? VENTURE_TYPE_INVOICE : VENTURE_TYPE_COMPANY, id, NULL);
		if (row == NULL || venture_entity_get_organization_id(row) != org)
		{
			g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "No such record");
			return venture_web_error_response(error);
		}
	}
	if (!venture_string_is_empty(until_text) && (until = venture_time_from_string(until_text, &error)) == NULL)
		return venture_web_error_response(error);
	venture_auth_to_actor(principal, &actor);
	if (g_strcmp0(htmx_request_get_form_value(request, "resume"), "1") == 0)
		ok = venture_late_fee_service_resume(venture_late_fee_service_get(db), scope, id, &actor, &error);
	else
		ok = venture_late_fee_service_pause(venture_late_fee_service_get(db), scope, id, until,
			htmx_request_get_form_value(request, "reason"), &actor, &error);
	if (!ok)
		return venture_web_error_response(error);
	return late_fees_back(request, "/late-fees", NULL);
}

static void
late_fees_append_waive_form(GString *html, gint64 fee_id, gint64 invoice_id)
{
	g_string_append_printf(html, "<details class=\"late-fee-waive\"><summary class=\"btn btn-sm\">Waive</summary>"
		"<form method=\"post\" action=\"/late-fees/%" G_GINT64_FORMAT "/waive\" class=\"waive-form\">"
		"<input type=\"hidden\" name=\"return\" value=\"/e/invoice/%" G_GINT64_FORMAT "\">"
		"<label>Reason <input name=\"reason\" required placeholder=\"Goodwill, error, agreement\"></label>"
		"<label>Note to the customer <input name=\"note\" placeholder=\"Optional; sent by email\"></label>"
		"<button class=\"btn btn-sm\" type=\"submit\">Waive this fee</button></form></details>", fee_id, invoice_id);
}

/* The fee panel on an invoice's page: what was charged, with its waive
 * action, what waits, and the pause in force. */
static void
venture_late_fees_web_append_invoice(VentureWebServer *self, GString *html, VentureEntity *invoice)
{
	VentureDatabase *db = venture_context_get_database(self->context);
	g_autoptr(GPtrArray) fees = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	g_autofree gchar *paused = NULL;
	g_autofree gchar *next = NULL;
	const gchar *scope = NULL;
	gint64 id = venture_entity_get_id(invoice);
	guint i;
	if (!venture_context_module_enabled(self->context, "late_fees"))
		return;
	fees = late_fees_find(self, VENTURE_TYPE_LATE_FEE, venture_entity_get_organization_id(invoice), "invoice-id", NULL, id, 200);
	paused = venture_late_fee_service_pause_state(venture_late_fee_service_get(db), invoice, now);
	scope = venture_late_fee_service_pause_scope(venture_late_fee_service_get(db), invoice, now);
	next = venture_late_fee_service_notice_text(venture_late_fee_service_get(db), invoice, now);
	g_string_append_printf(html, "<div class=\"card late-fee-panel\"><div class=\"card-head\"><h2>Late fees</h2>"
		"<a class=\"btn btn-sm\" href=\"/invoices/%" G_GINT64_FORMAT "/late-fees\">Preview timeline</a></div><div class=\"card-body\">", id);
	if (paused != NULL)
	{
		/* One child, so the notice's flex row cannot split the sentence. */
		g_string_append(html, "<p class=\"notice\" role=\"status\"><span><strong>");
		venture_html_escape_append(html, paused);
		g_string_append(html, "</strong>. Nothing is added and reminders carry no fee wording.");
		if (g_strcmp0(scope, "company") == 0)
			g_string_append_printf(html, " <a href=\"/e/company/%" G_GINT64_FORMAT "\">Change it on the customer's page.</a>",
				late_fees_number(invoice, "company-id"));
		else if (g_strcmp0(scope, "organization") == 0)
			g_string_append(html, " <a href=\"/late-fees\">Change it on the Late fees page.</a>");
		g_string_append(html, "</span></p>");
	}
	else if (!g_str_equal(next, ""))
	{
		g_string_append(html, "<p>Next: ");
		venture_html_escape_append(html, next);
		g_string_append(html, "</p>");
	}
	if (fees == NULL || fees->len == 0)
		g_string_append(html, "<p class=\"muted\">No late fees on this invoice.</p>");
	else
	{
		g_string_append(html, "<div class=\"table-wrap\"><table class=\"data stack-cards\"><thead><tr><th>Due on</th><th>Amount</th><th>State</th><th>Basis</th><th>Action</th></tr></thead><tbody>");
		for (i = 0; i < fees->len; i++)
		{
			VentureEntity *fee = g_ptr_array_index(fees, i);
			g_autofree gchar *status = late_fees_text(fee, "status");
			g_autofree gchar *basis = late_fees_text(fee, "basis");
			g_autofree gchar *reason = late_fees_text(fee, "reason");
			g_string_append(html, "<tr><td data-label=\"Due on\">");
			late_fees_append_date(html, fee, "due-on");
			g_string_append(html, "</td><td class=\"num\" data-label=\"Amount\">");
			late_fees_append_money(html, fee, "amount");
			g_string_append(html, "</td><td data-label=\"State\">");
			late_fees_append_badge(html, status);
			if (!venture_string_is_empty(reason) && (g_strcmp0(status, "waived") == 0 || g_strcmp0(status, "skipped") == 0 ||
				g_strcmp0(status, "cancelled") == 0))
			{
				g_string_append(html, " <span class=\"muted\">");
				venture_html_escape_append(html, reason);
				g_string_append(html, "</span>");
			}
			g_string_append(html, "</td><td data-label=\"Basis\">");
			venture_html_escape_append(html, basis ? basis : "");
			g_string_append(html, "</td><td>");
			if (g_strcmp0(status, "applied") == 0)
				late_fees_append_waive_form(html, venture_entity_get_id(fee), id);
			else if (g_strcmp0(status, "proposed") == 0 || g_strcmp0(status, "awaiting_notice") == 0)
				g_string_append(html, "<a href=\"/late-fees/queue\">Decide</a>");
			g_string_append(html, "</td></tr>");
		}
		g_string_append(html, "</tbody></table></div>");
	}
	/* Only the control that means something: resume a pause of this
	 * invoice, pause an unpaused one; under a customer's or the
	 * organization's pause an invoice pause would change nothing. */
	if (scope == NULL || g_strcmp0(scope, "invoice") == 0)
	{
		g_autofree gchar *back = g_strdup_printf("/e/invoice/%" G_GINT64_FORMAT, id);
		late_fees_append_pause_form(html, "invoice", id, scope != NULL, back);
	}
	g_string_append(html, "</div></div>");
}

static HtmxResponse *
late_fees_invoice_page(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	HtmxResponse *gate = late_fees_gate(self, request, NULL);
	VentureDatabase *db = venture_context_get_database(self->context);
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) preview = NULL;
	g_autoptr(GDateTime) as_of = NULL;
	g_autoptr(GString) html = NULL;
	g_autofree gchar *number = NULL;
	g_autofree gchar *title = NULL;
	const gchar *as_of_text = htmx_request_get_query_param(request, "as_of");
	JsonObject *p;
	JsonArray *entries;
	gint64 id;
	guint i;
	if (gate != NULL)
		return gate;
	id = g_ascii_strtoll(g_hash_table_lookup(params, "id"), NULL, 10);
	invoice = venture_database_get(db, VENTURE_TYPE_INVOICE, id, &error);
	if (invoice == NULL || venture_entity_get_organization_id(invoice) != late_fees_org(self, request))
	{
		if (error == NULL)
			g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "No such invoice");
		return venture_web_error_response(error);
	}
	if (!venture_string_is_empty(as_of_text) && (as_of = venture_time_from_string(as_of_text, &error)) == NULL)
		return venture_web_error_response(error);
	preview = venture_late_fee_service_preview(venture_late_fee_service_get(db), id, as_of, &error);
	if (preview == NULL)
		return venture_web_error_response(error);
	p = json_node_get_object(preview);
	entries = json_object_get_array_member(p, "entries");
	number = late_fees_text(invoice, "number");
	title = g_strdup_printf("Late fees: %s", number ? number : "");
	html = g_string_new("<div class=\"late-fees\"><div class=\"page-head\"><div class=\"page-title\"><h1>");
	venture_html_escape_append(html, title);
	g_string_append(html, "</h1><span class=\"subtitle\">What the schedule would charge, before anything is applied</span></div>");
	g_string_append_printf(html, "<div class=\"page-actions\"><a class=\"btn\" href=\"/e/invoice/%" G_GINT64_FORMAT "\">Invoice</a></div></div>", id);
	if (!json_object_get_boolean_member(p, "enabled"))
		g_string_append(html, "<div class=\"notice\">Late fees are off for this organization: nothing below is charged.</div>");
	if (json_object_get_boolean_member(p, "paused"))
	{
		g_string_append(html, "<div class=\"notice\" role=\"status\"><span><strong>Paused</strong> for the ");
		venture_html_escape_append(html, json_object_get_string_member(p, "pause_scope"));
		g_string_append(html, ", until ");
		venture_html_escape_append(html, venture_string_is_empty(json_object_get_string_member_with_default(p, "paused_until", "")) ?
			"further notice" : json_object_get_string_member(p, "paused_until"));
		g_string_append(html, ". Nothing below is added while it holds.</span></div>");
	}
	if (json_object_get_boolean_member(p, "exempt"))
		g_string_append(html, "<div class=\"notice\">This invoice or its customer is exempt from late fees.</div>");
	g_string_append(html, "<div class=\"stat-row\"><div class=\"stat\"><span class=\"stat-value\">");
	venture_html_escape_append(html, json_object_get_string_member_with_default(p, "schedule_name", ""));
	g_string_append(html, "</span><span class=\"stat-label\">Schedule</span></div><div class=\"stat\"><span class=\"stat-value\">");
	g_string_append_printf(html, "%" G_GINT64_FORMAT, json_object_get_int_member_with_default(p, "days_late", 0));
	g_string_append(html, "</span><span class=\"stat-label\">Days late</span></div><div class=\"stat\"><span class=\"stat-value\">");
	venture_html_escape_append(html, json_object_get_string_member_with_default(p, "original", ""));
	g_string_append(html, "</span><span class=\"stat-label\">Original invoice</span></div><div class=\"stat\"><span class=\"stat-value\">");
	venture_html_escape_append(html, json_object_get_string_member_with_default(p, "charged", ""));
	g_string_append(html, "</span><span class=\"stat-label\">Fees charged</span></div></div>");
	g_string_append(html, "<div class=\"card\"><div class=\"card-head\"><h2>Timeline</h2></div><div class=\"card-body\"><div class=\"table-wrap\"><table class=\"data late-fee-timeline stack-cards\">"
		"<thead><tr><th>Day</th><th>Date</th><th>Tier</th><th>Charge</th><th>Amount</th><th>State</th><th>Notice</th></tr></thead><tbody>");
	for (i = 0; entries != NULL && i < json_array_get_length(entries) && i < 60; i++)
	{
		JsonObject *e = json_array_get_object_element(entries, i);
		const gchar *status = json_object_get_string_member_with_default(e, "status", "");
		g_string_append_printf(html, "<tr><td class=\"num\" data-label=\"Day\">%" G_GINT64_FORMAT "</td><td data-label=\"Date\">", json_object_get_int_member(e, "day"));
		venture_html_escape_append(html, json_object_get_string_member_with_default(e, "date", ""));
		g_string_append_printf(html, "</td><td data-label=\"Tier\">%" G_GINT64_FORMAT "%s</td><td data-label=\"Charge\">", json_object_get_int_member(e, "tier"),
			json_object_get_boolean_member_with_default(e, "recurring", FALSE) ? " (recurring)" : "");
		venture_html_escape_append(html, json_object_get_string_member_with_default(e, "basis",
			json_object_get_string_member_with_default(e, "type", "")));
		g_string_append(html, "</td><td class=\"num\" data-label=\"Amount\">");
		venture_html_escape_append(html, json_object_get_string_member_with_default(e, "display", "\xe2\x80\x94"));
		g_string_append(html, "</td><td data-label=\"State\">");
		late_fees_append_badge(html, status);
		if (json_object_has_member(e, "reason"))
		{
			g_string_append(html, " <span class=\"muted\">");
			venture_html_escape_append(html, json_object_get_string_member(e, "reason"));
			g_string_append(html, "</span>");
		}
		g_string_append(html, "</td><td data-label=\"Notice\">");
		venture_html_escape_append(html, json_object_get_string_member_with_default(e, "notice", ""));
		g_string_append(html, "</td></tr>");
	}
	if (entries == NULL || json_array_get_length(entries) == 0)
		g_string_append(html, "<tr><td colspan=\"7\" class=\"muted\">No schedule governs this invoice.</td></tr>");
	g_string_append(html, "</tbody></table></div></div></div><p class=\"muted\">Amounts assume the balance stays as it is today; a percent tier "
		"is recomputed on its day and never exceeds what the reminder told the customer.</p>");
	venture_late_fees_web_append_invoice(self, html, invoice);
	g_string_append(html, "</div>");
	return venture_web_html_response(venture_web_page(self, request, "/late-fees", title, html->str), 200);
}

static void
venture_late_fees_web_register(VentureWebServer *self)
{
	venture_web_server_add_classified_route(self, HTMX_METHOD_GET, "/late-fees", VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, late_fees_page, self);
	venture_web_server_add_classified_route(self, HTMX_METHOD_GET, "/late-fees/queue", VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, late_fees_queue_page, self);
	venture_web_server_add_classified_route(self, HTMX_METHOD_POST, "/late-fees/decide", VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, late_fees_decide, self);
	venture_web_server_add_classified_route(self, HTMX_METHOD_POST, "/late-fees/pause", VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, late_fees_pause_post, self);
	venture_web_server_add_classified_route(self, HTMX_METHOD_POST, "/late-fees/:id/:verb", VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, late_fees_fee_post, self);
	venture_web_server_add_classified_route(self, HTMX_METHOD_GET, "/invoices/:id/late-fees", VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, late_fees_invoice_page, self);
}
