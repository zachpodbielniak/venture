/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * The money calendar page and its feed. Month, week and agenda views are
 * plain server-rendered HTML over the same expanded events the report and
 * the CLI read; the filter form is hx-boosted so a change re-renders the
 * page in place. Nothing drags: a due date is a fact on a record, changed
 * on that record.
 */

typedef struct
{
	VentureDateRange	*range;
	GDateTime		*today;
	GPtrArray		*events;
	GHashTable		*day_nets;
	GHashTable		*week_nets;
	GHashTable		*by_day;
	const gchar		*view;
	const gchar		*period;
	const gchar		*kind;
	const gchar		*currency;
	gint64			 company_id;
	gint64			 venture_id;
} MoneyCalendarPage;

static void
money_calendar_page_clear(MoneyCalendarPage *page)
{
	g_clear_pointer(&page->range, venture_date_range_free);
	g_clear_pointer(&page->today, g_date_time_unref);
	g_clear_pointer(&page->events, g_ptr_array_unref);
	g_clear_pointer(&page->day_nets, g_hash_table_unref);
	g_clear_pointer(&page->week_nets, g_hash_table_unref);
	g_clear_pointer(&page->by_day, g_hash_table_unref);
}

/* Reads the query, expands and filters. Returns an error response or NULL. */
static HtmxResponse *
money_calendar_page_load(VentureWebServer *self, HtmxRequest *request, MoneyCalendarPage *page)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) all = NULL;
	const gchar *as_of = htmx_request_get_query_param(request, "as_of");
	const gchar *company = htmx_request_get_query_param(request, "company_id");
	const gchar *venture = htmx_request_get_query_param(request, "venture_id");
	gint64 organization_id = venture_web_active_organization(self, request);
	guint i;
	memset(page, 0, sizeof(*page));
	page->view = htmx_request_get_query_param(request, "view");
	page->period = htmx_request_get_query_param(request, "period");
	page->kind = htmx_request_get_query_param(request, "kind");
	page->currency = htmx_request_get_query_param(request, "currency");
	page->company_id = venture_string_is_empty(company) ? 0 : g_ascii_strtoll(company, NULL, 10);
	page->venture_id = venture_string_is_empty(venture) ? 0 : g_ascii_strtoll(venture, NULL, 10);
	if (venture_string_is_empty(page->view))
		page->view = "month";
	if (venture_string_is_empty(page->period))
		page->period = g_strcmp0(page->view, "week") == 0 ? "this_week" : "this_month";
	if (organization_id == 0)
		organization_id = venture_context_get_default_organization_id(self->context);
	page->range = venture_context_parse_period(self->context, page->period, &error);
	if (page->range == NULL)
		return venture_web_error_response(error);
	page->today = venture_string_is_empty(as_of) ? venture_time_now() : venture_time_from_string(as_of, &error);
	if (page->today == NULL)
		return venture_web_error_response(error);
	all = venture_money_calendar_events(self->context, organization_id, page->range, page->today, &error);
	if (all == NULL)
		return venture_web_error_response(error);
	page->events = venture_money_calendar_filter(all, page->kind, page->company_id, page->venture_id, page->currency);
	page->day_nets = venture_money_calendar_nets(page->events, FALSE);
	page->week_nets = venture_money_calendar_nets(page->events, TRUE);
	page->by_day = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_ptr_array_unref);
	for (i = 0; i < page->events->len; i++)
	{
		VentureMoneyCalendarEvent *event = g_ptr_array_index(page->events, i);
		GPtrArray *bucket = g_hash_table_lookup(page->by_day, venture_money_calendar_event_get_day(event));
		if (bucket == NULL)
		{
			bucket = g_ptr_array_new();
			g_hash_table_insert(page->by_day, g_strdup(venture_money_calendar_event_get_day(event)), bucket);
		}
		g_ptr_array_add(bucket, event);
	}
	return NULL;
}

static void
money_calendar_append_event(GString *html, VentureMoneyCalendarEvent *event)
{
	g_autofree gchar *amount = venture_money_to_display_string(venture_money_calendar_event_get_amount(event), TRUE);
	g_string_append_printf(html, "<li class=\"cal-event cal-%s cal-%s%s\"><a href=\"/e/",
		venture_money_calendar_event_get_kind(event), venture_money_calendar_event_get_direction(event),
		venture_money_calendar_event_get_overdue(event) ? " cal-overdue" : "");
	venture_html_escape_append(html, venture_money_calendar_event_get_record_type(event));
	g_string_append_printf(html, "/%" G_GINT64_FORMAT "\"><span class=\"cal-amount\">%s",
		venture_money_calendar_event_get_record_id(event), g_strcmp0(venture_money_calendar_event_get_direction(event), "out") == 0 ? "-" : "+");
	venture_html_escape_append(html, amount);
	g_string_append(html, "</span> ");
	venture_html_escape_append(html, venture_money_calendar_event_get_title(event));
	if (venture_money_calendar_event_get_overdue(event))
		g_string_append(html, " <span class=\"badge\">overdue</span>");
	g_string_append(html, "</a></li>");
}

static void
money_calendar_append_day_events(GString *html, MoneyCalendarPage *page, const gchar *day)
{
	GPtrArray *bucket = g_hash_table_lookup(page->by_day, day);
	g_autofree gchar *net = venture_money_calendar_nets_format(page->day_nets, day);
	guint i;
	g_string_append(html, "<ul class=\"cal-events\">");
	if (bucket != NULL)
		for (i = 0; i < bucket->len; i++)
			money_calendar_append_event(html, g_ptr_array_index(bucket, i));
	g_string_append(html, "</ul>");
	if (net[0] != '\0')
	{
		g_string_append(html, "<div class=\"cal-net muted\">net ");
		venture_html_escape_append(html, net);
		g_string_append(html, "</div>");
	}
}

/* The Monday on or before a day. */
static GDateTime *
money_calendar_monday(GDateTime *day)
{
	return g_date_time_add_days(day, 1 - g_date_time_get_day_of_week(day));
}

/* Month and week views share one grid: rows of seven days from the Monday
 * before the range start to the Sunday after its last day, with the ISO
 * week's net closing each row. */
static void
money_calendar_append_grid(GString *html, MoneyCalendarPage *page, const gchar *css_class)
{
	static const gchar *const names[] = { "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun" };
	GDateTime *start = venture_date_range_get_start(page->range);
	GDateTime *end = venture_date_range_get_end(page->range);
	g_autoptr(GDateTime) cursor = money_calendar_monday(start);
	g_autoptr(GDateTime) last = g_date_time_add_days(end, -1);
	g_autofree gchar *last_day = g_date_time_format(last, "%Y-%m-%d");
	g_autofree gchar *first_day = g_date_time_format(start, "%Y-%m-%d");
	g_autofree gchar *today = g_date_time_format(page->today, "%Y-%m-%d");
	guint i, rows = 0;
	g_string_append_printf(html, "<table class=\"%s\"><thead><tr>", css_class);
	for (i = 0; i < 7; i++)
		g_string_append_printf(html, "<th>%s</th>", names[i]);
	g_string_append(html, "<th>Week</th></tr></thead><tbody>");
	while (rows < 8)
	{
		g_autofree gchar *week = g_strdup_printf("%d-W%02d", g_date_time_get_week_numbering_year(cursor), g_date_time_get_week_of_year(cursor));
		g_autofree gchar *week_net = venture_money_calendar_nets_format(page->week_nets, week);
		g_string_append(html, "<tr>");
		for (i = 0; i < 7; i++)
		{
			g_autofree gchar *day = g_date_time_format(cursor, "%Y-%m-%d");
			gboolean inside = g_strcmp0(day, first_day) >= 0 && g_strcmp0(day, last_day) <= 0;
			GDateTime *next = g_date_time_add_days(cursor, 1);
			g_string_append_printf(html, "<td class=\"cal-day%s%s\" data-day=\"%s\"><div class=\"cal-daynum\">%d</div>",
				inside ? "" : " cal-outside", g_strcmp0(day, today) == 0 ? " cal-today" : "", day, g_date_time_get_day_of_month(cursor));
			if (inside)
				money_calendar_append_day_events(html, page, day);
			g_string_append(html, "</td>");
			g_date_time_unref(cursor);
			cursor = next;
		}
		g_string_append(html, "<td class=\"cal-weeknet\"><div class=\"muted\">");
		venture_html_escape_append(html, week);
		g_string_append(html, "</div>");
		venture_html_escape_append(html, week_net);
		g_string_append(html, "</td></tr>");
		rows++;
		{
			g_autofree gchar *day = g_date_time_format(cursor, "%Y-%m-%d");
			if (g_strcmp0(day, last_day) > 0)
				break;
		}
	}
	g_string_append(html, "</tbody></table>");
}

static void
money_calendar_append_agenda(GString *html, MoneyCalendarPage *page)
{
	const gchar *current_week = NULL;
	const gchar *current_day = NULL;
	guint i;
	g_string_append(html, "<div class=\"cal-agenda\">");
	if (page->events->len == 0)
		g_string_append(html, "<p class=\"muted\">Nothing dated in this span.</p>");
	for (i = 0; i < page->events->len; i++)
	{
		VentureMoneyCalendarEvent *event = g_ptr_array_index(page->events, i);
		const gchar *week = venture_money_calendar_event_get_week(event);
		const gchar *day = venture_money_calendar_event_get_day(event);
		if (g_strcmp0(week, current_week) != 0)
		{
			g_autofree gchar *net = venture_money_calendar_nets_format(page->week_nets, week);
			if (current_day != NULL)
				g_string_append(html, "</ul></section>");
			current_day = NULL;
			g_string_append(html, "<h2 class=\"cal-weekhead\">");
			venture_html_escape_append(html, week);
			g_string_append(html, " <span class=\"muted\">week net ");
			venture_html_escape_append(html, net);
			g_string_append(html, "</span></h2>");
			current_week = week;
		}
		if (g_strcmp0(day, current_day) != 0)
		{
			g_autofree gchar *net = venture_money_calendar_nets_format(page->day_nets, day);
			if (current_day != NULL)
				g_string_append(html, "</ul></section>");
			g_string_append(html, "<section class=\"card cal-agenda-day\"><h3>");
			venture_html_escape_append(html, day);
			g_string_append(html, " <span class=\"muted\">net ");
			venture_html_escape_append(html, net);
			g_string_append(html, "</span></h3><ul class=\"cal-events\">");
			current_day = day;
		}
		money_calendar_append_event(html, event);
	}
	if (current_day != NULL)
		g_string_append(html, "</ul></section>");
	g_string_append(html, "</div>");
}

static void
money_calendar_append_option(GString *html, const gchar *value, const gchar *label, const gchar *selected)
{
	g_string_append(html, "<option value=\"");
	venture_html_escape_append(html, value);
	g_string_append(html, g_strcmp0(value, selected) == 0 ? "\" selected>" : "\">");
	venture_html_escape_append(html, label);
	g_string_append(html, "</option>");
}

static void
money_calendar_append_filters(GString *html, MoneyCalendarPage *page)
{
	g_autoptr(GPtrArray) kinds = venture_money_calendar_expander_kinds(venture_money_calendar_expander_get_default());
	guint i;
	g_string_append(html, "<form class=\"filters cal-filters\" method=\"get\" action=\"/money/calendar\" hx-boost=\"true\">"
		"<label>View <select name=\"view\">");
	money_calendar_append_option(html, "month", "Month", page->view);
	money_calendar_append_option(html, "week", "Week", page->view);
	money_calendar_append_option(html, "agenda", "Agenda", page->view);
	g_string_append(html, "</select></label> <label>Period <input name=\"period\" value=\"");
	venture_html_escape_append(html, page->period);
	g_string_append(html, "\" placeholder=\"2026-03 or 2026-03-02..2026-03-08\"></label> <label>Kind <select name=\"kind\">");
	money_calendar_append_option(html, "", "All kinds", page->kind ? page->kind : "");
	for (i = 0; i < kinds->len; i++)
		money_calendar_append_option(html, g_ptr_array_index(kinds, i), g_ptr_array_index(kinds, i), page->kind ? page->kind : "");
	g_string_append(html, "</select></label> <label>Vendor / customer id <input name=\"company_id\" type=\"number\" min=\"0\" value=\"");
	if (page->company_id > 0)
		g_string_append_printf(html, "%" G_GINT64_FORMAT, page->company_id);
	g_string_append(html, "\"></label>");
	g_string_append(html, " <label>Venture id <input name=\"venture_id\" type=\"number\" min=\"0\" value=\"");
	if (page->venture_id > 0)
		g_string_append_printf(html, "%" G_GINT64_FORMAT, page->venture_id);
	g_string_append(html, "\"></label> <label>Currency <input name=\"currency\" size=\"4\" value=\"");
	venture_html_escape_append(html, page->currency ? page->currency : "");
	g_string_append(html, "\"></label> <button class=\"btn btn-primary\">Show</button></form>");
}

static HtmxResponse *
money_calendar_page(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	MoneyCalendarPage page;
	HtmxResponse *gate = venture_web_ui_require_session(self, request);
	g_autoptr(GString) content = NULL;
	g_autofree gchar *feed = NULL;
	(void)params;
	if (gate != NULL)
		return gate;
	gate = venture_web_require_module_ui(self, request, "money_calendar");
	if (gate != NULL)
		return gate;
	gate = money_calendar_page_load(self, request, &page);
	if (gate != NULL)
	{
		money_calendar_page_clear(&page);
		return gate;
	}
	content = g_string_new("<div class=\"money-calendar\"><div class=\"page-head\"><div class=\"page-title\"><h1>Money calendar</h1><span class=\"subtitle\">");
	venture_html_escape_append(content, venture_date_range_get_label(page.range));
	g_string_append(content, "</span></div><div class=\"page-actions\">");
	feed = g_strdup_printf("/money/calendar.ics?period=%s", page.period);
	g_string_append(content, "<a class=\"btn\" href=\"");
	venture_html_escape_append(content, feed);
	g_string_append(content, "\">Subscribe (.ics)</a> <a class=\"btn\" href=\"/reports\">All reports</a></div></div>");
	money_calendar_append_filters(content, &page);
	if (g_strcmp0(page.view, "agenda") == 0)
		money_calendar_append_agenda(content, &page);
	else
		money_calendar_append_grid(content, &page, g_strcmp0(page.view, "week") == 0 ? "cal-week" : "cal-month");
	g_string_append(content, "<p class=\"muted\">Nets are per currency and never converted. Reminders show the invoice balance but do not move cash. "
		"Overdue bills and invoices are carried to today and flagged. Dates change on the record, not here.</p></div>");
	money_calendar_page_clear(&page);
	return venture_web_html_response(venture_web_page(self, request, "/money/calendar", "Money calendar", content->str), 200);
}

static HtmxResponse *
money_calendar_feed(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	MoneyCalendarPage page;
	g_autofree gchar *calendar = NULL;
	g_autoptr(GBytes) bytes = NULL;
	HtmxResponse *response;
	HtmxResponse *gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_VIEWER);
	(void)params;
	if (gate != NULL)
		return gate;
	gate = venture_web_require_module_api(self, "money_calendar");
	if (gate != NULL)
		return gate;
	gate = money_calendar_page_load(self, request, &page);
	if (gate != NULL)
	{
		money_calendar_page_clear(&page);
		return gate;
	}
	calendar = venture_money_calendar_ics(page.events);
	money_calendar_page_clear(&page);
	bytes = g_bytes_new(calendar, strlen(calendar));
	response = htmx_response_new();
	htmx_response_set_status(response, 200);
	htmx_response_set_bytes(response, bytes);
	htmx_response_set_content_type(response, "text/calendar; charset=utf-8");
	return response;
}

static void
venture_money_calendar_web_register(HtmxRouter *router, VentureWebServer *self)
{
	(void)router;
	venture_web_server_add_classified_route(self, HTMX_METHOD_GET, "/money/calendar", VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, money_calendar_page, self);
	venture_web_server_add_classified_route(self, HTMX_METHOD_GET, "/money/calendar.ics", VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, money_calendar_feed, self);
}
