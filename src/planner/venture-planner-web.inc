/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * venture-planner-web.inc - The calendar page, its JSON and feed twins,
 * and an event's own block on its record page
 *
 * Included by venture-web-server.c. Nothing here computes a figure: the
 * span, the items, which day a multi-day item covers, the run-sheet's
 * running times and the answers counted all come from core
 * (venture-calendar-view.c, venture-planner.c). This file only draws
 * them, so /calendar, /api/v1/calendar and /calendar.ics are one answer
 * in three shapes.
 *
 * The query names -- view, date, from, to, kind, venture_id, assignee,
 * done -- are read here and never handed to venture_query_apply_query_string(),
 * so they cannot be mistaken for field filters on a list page; and
 * /api/v1/calendar is registered ahead of /api/v1/:type, which would
 * otherwise read "calendar" as a record type.
 */

/* --- Reading the question ---------------------------------------------------- */

typedef struct
{
	VentureCalendarQuery	 query;
	gint64			 organization_id;
	GPtrArray		*kinds;		/* gchar*, NULL-terminated for the query */
	GDateTime		*from;
	GDateTime		*to;
	GDateTime		*now;
	gchar			*username;
	const gchar		*view;
	gboolean		 mine;
} PlannerCalendarAsk;

static void
planner_calendar_ask_clear(PlannerCalendarAsk *ask)
{
	g_clear_pointer(&ask->kinds, g_ptr_array_unref);
	g_clear_pointer(&ask->from, g_date_time_unref);
	g_clear_pointer(&ask->to, g_date_time_unref);
	g_clear_pointer(&ask->now, g_date_time_unref);
	g_clear_pointer(&ask->username, g_free);
}

/* Every `kind` in the query string, each of which may itself be a comma
 * list: checkboxes send the name once per box, a script sends a list. */
static GPtrArray *
planner_calendar_kinds(HtmxRequest *request)
{
	g_auto(GStrv) pairs = NULL;
	GPtrArray *kinds;
	const gchar *raw;
	guint i;

	kinds = g_ptr_array_new_with_free_func(g_free);
	raw = htmx_request_get_query(request);

	if (venture_string_is_empty(raw))
		return kinds;

	pairs = g_strsplit(raw, "&", -1);

	for (i = 0; NULL != pairs[i]; i++)
	{
		g_autofree gchar *value = NULL;
		g_auto(GStrv) names = NULL;
		guint j;

		if (!g_str_has_prefix(pairs[i], "kind="))
			continue;

		value = g_uri_unescape_string(pairs[i] + strlen("kind="), NULL);

		if (NULL == value)
			continue;

		g_strdelimit(value, "+", ' ');
		names = g_strsplit(value, ",", -1);

		for (j = 0; NULL != names[j]; j++)
		{
			g_strstrip(names[j]);

			if (('\0' != names[j][0]) &&
			    !g_ptr_array_find_with_equal_func(kinds, names[j], g_str_equal, NULL))
				g_ptr_array_add(kinds, g_strdup(names[j]));
		}
	}

	return kinds;
}

/* Reads the request into a calendar query. Returns an error response, or
 * NULL when @ask is filled. */
static HtmxResponse *
planner_calendar_read(
	VentureWebServer	*self,
	HtmxRequest		*request,
	PlannerCalendarAsk	*ask
){
	g_autoptr(GError) error = NULL;
	const gchar *venture;
	const gchar *assignee;
	const gchar *done;

	memset(ask, 0, sizeof(*ask));
	venture_calendar_query_init(&ask->query);

	ask->view = htmx_request_get_query_param(request, "view");

	if (venture_string_is_empty(ask->view))
		ask->view = "month";

	if (!venture_web_request_organization(self, request, NULL, &ask->organization_id, &error))
		return venture_web_error_response(error);

	if (!venture_calendar_view_range(self->context, ask->view,
	                                 htmx_request_get_query_param(request, "date"),
	                                 htmx_request_get_query_param(request, "from"),
	                                 htmx_request_get_query_param(request, "to"),
	                                 &ask->from, &ask->to, &error))
		return venture_web_error_response(error);

	venture = htmx_request_get_query_param(request, "venture_id");

	if (!venture_string_is_empty(venture))
	{
		gchar *end = NULL;

		ask->query.venture_id = g_ascii_strtoll(venture, &end, 10);

		if ((NULL == end) || ('\0' != *end) || (ask->query.venture_id < 0))
		{
			g_set_error(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			            "venture_id \"%s\" is not a record id", venture);
			return venture_web_error_response(error);
		}
	}

	/* Only "me": naming somebody else's calendar is a question about a
	 * colleague, and the lists already answer it under their own rules. */
	assignee = htmx_request_get_query_param(request, "assignee");

	if (!venture_string_is_empty(assignee))
	{
		g_autoptr(VentureAuthPrincipal) principal = NULL;

		if (0 != g_strcmp0(assignee, "me"))
		{
			g_set_error(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			            "assignee takes only \"me\", not \"%s\"", assignee);
			return venture_web_error_response(error);
		}

		principal = venture_auth_authenticate(self->auth, request);

		if ((NULL == principal) || ((0 == principal->user_id) &&
		                            venture_string_is_empty(principal->name)))
		{
			g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			                    "assignee=me needs a signed-in user");
			return venture_web_error_response(error);
		}

		/* A token acts as "API token #N", never a person: it owns nothing. */
		if (0 == principal->user_id)
		{
			g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			                    "assignee=me needs a user; this token belongs to none");
			return venture_web_error_response(error);
		}

		ask->mine = TRUE;
		ask->username = g_strdup(principal->name);
		ask->query.assignee_user_id = principal->user_id;
		ask->query.assignee_username = ask->username;
	}

	done = htmx_request_get_query_param(request, "done");
	ask->query.include_done = (0 == g_strcmp0(done, "1")) || (0 == g_strcmp0(done, "true"));

	ask->kinds = planner_calendar_kinds(request);

	if (ask->kinds->len > 0)
	{
		g_ptr_array_add(ask->kinds, NULL);
		ask->query.kinds = (gchar **)ask->kinds->pdata;
	}

	ask->now = venture_time_now();
	ask->query.from = ask->from;
	ask->query.to = ask->to;
	ask->query.now = ask->now;
	ask->query.organization_ids = &ask->organization_id;
	ask->query.n_organizations = 1;

	return NULL;
}

/* --- Drawing it ---------------------------------------------------------------- */

/* The query string that keeps the filters across prev, next and the
 * feed link: kinds, venture, assignee, done. */
static gchar *
planner_calendar_filters(const PlannerCalendarAsk *ask)
{
	GString *text;
	guint i;

	text = g_string_new(NULL);

	for (i = 0; (NULL != ask->query.kinds) && (NULL != ask->query.kinds[i]); i++)
	{
		g_string_append(text, "&kind=");
		g_string_append_uri_escaped(text, ask->query.kinds[i], NULL, FALSE);
	}

	if (0 != ask->query.venture_id)
		g_string_append_printf(text, "&venture_id=%" G_GINT64_FORMAT, ask->query.venture_id);

	if (ask->mine)
		g_string_append(text, "&assignee=me");

	if (ask->query.include_done)
		g_string_append(text, "&done=1");

	return g_string_free(text, FALSE);
}

static gboolean
planner_calendar_kind_shown(
	const PlannerCalendarAsk	*ask,
	const gchar			*name
){
	if (NULL == ask->query.kinds)
		return TRUE;

	return g_strv_contains((const gchar *const *)ask->query.kinds, name);
}

/* One item: a link to its record, its time when it has one, its kind
 * and state as classes the stylesheets colour. */
static void
planner_calendar_append_item(
	VentureWebServer		*self,
	GString				*html,
	const VentureCalendarItem	*item,
	gboolean			 show_time
){
	g_string_append(html, "<li class=\"cal-item cal-kind-");
	venture_html_escape_append(html, item->kind);

	if (NULL != item->status)
	{
		g_string_append(html, " cal-status-");
		venture_html_escape_append(html, item->status);
	}

	if (item->overdue)
		g_string_append(html, " cal-overdue");

	if (item->all_day)
		g_string_append(html, " cal-all-day");

	g_string_append(html, "\">");

	if (NULL != item->url)
	{
		g_string_append(html, "<a href=\"");
		venture_html_escape_append(html, item->url);
		g_string_append(html, "\">");
	}

	if (show_time && !item->all_day)
	{
		g_autoptr(GDateTime) local = g_date_time_to_timezone(item->start,
			venture_context_get_timezone(self->context));
		g_autofree gchar *clock = g_date_time_format(local, "%H:%M");

		g_string_append(html, "<span class=\"cal-time\">");
		venture_html_escape_append(html, clock);
		g_string_append(html, "</span> ");
	}

	venture_html_escape_append(html, item->title);

	if (NULL != item->url)
		g_string_append(html, "</a>");

	if (!venture_string_is_empty(item->detail))
	{
		g_string_append(html, " <span class=\"cal-detail muted\">");
		venture_html_escape_append(html, item->detail);
		g_string_append(html, "</span>");
	}

	if (item->overdue)
		g_string_append(html, " <span class=\"badge\">overdue</span>");

	g_string_append(html, "</li>");
}

/* Month and week share one grid: rows of seven days, Monday first, from
 * the Monday on or before the span's first day to the Sunday after its
 * last. Each day lists every item covering it, a trip on each of its
 * days. */
static void
planner_calendar_append_grid(
	VentureWebServer	*self,
	GString			*html,
	PlannerCalendarAsk	*ask,
	VentureCalendarResult	*result
){
	static const gchar *const names[] = { "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun" };
	GTimeZone *timezone;
	g_autoptr(GDateTime) cursor = NULL;
	g_autoptr(GDateTime) last = NULL;
	g_autoptr(GDateTime) today = NULL;
	g_autofree gchar *first_day = NULL;
	g_autofree gchar *last_day = NULL;
	g_autofree gchar *today_day = NULL;
	gboolean week;
	guint rows;
	guint i;

	timezone = venture_context_get_timezone(self->context);
	week = (0 == g_strcmp0(ask->view, "week"));
	cursor = g_date_time_add_days(ask->from, 1 - g_date_time_get_day_of_week(ask->from));
	last = g_date_time_add_days(ask->to, -1);
	first_day = venture_time_to_date_string(ask->from, NULL);
	last_day = venture_time_to_date_string(last, NULL);
	today = venture_time_today(timezone);
	today_day = venture_time_to_date_string(today, NULL);

	/* A calendar: its rows are weeks in order and its columns the days
	 * of one, so a sort would draw a different, wrong month. */
	g_string_append_printf(html, "<div class=\"table-wrap\"><table class=\"cal-grid %s\" data-no-sort>"
	                             "<thead><tr>", week ? "cal-week" : "cal-month");

	for (i = 0; i < 7; i++)
		g_string_append_printf(html, "<th scope=\"col\">%s</th>", names[i]);

	g_string_append(html, "</tr></thead><tbody>");

	for (rows = 0; rows < 7; rows++)
	{
		g_string_append(html, "<tr>");

		for (i = 0; i < 7; i++)
		{
			g_autofree gchar *day = venture_time_to_date_string(cursor, NULL);
			gboolean inside = (g_strcmp0(day, first_day) >= 0) && (g_strcmp0(day, last_day) <= 0);
			GDateTime *next;
			guint k;

			g_string_append_printf(html, "<td class=\"cal-day%s%s\" data-day=\"%s\">"
			                             "<div class=\"cal-daynum\">%d</div>",
			                       inside ? "" : " cal-outside",
			                       (0 == g_strcmp0(day, today_day)) ? " cal-today" : "",
			                       day, g_date_time_get_day_of_month(cursor));

			if (inside)
			{
				g_string_append(html, "<ul class=\"cal-items\">");

				for (k = 0; k < result->items->len; k++)
				{
					VentureCalendarItem *item = g_ptr_array_index(result->items, k);

					if (venture_calendar_item_covers_day(item, day, timezone))
						planner_calendar_append_item(self, html, item, TRUE);
				}

				g_string_append(html, "</ul>");
			}

			g_string_append(html, "</td>");
			next = g_date_time_add_days(cursor, 1);
			g_date_time_unref(cursor);
			cursor = next;
		}

		g_string_append(html, "</tr>");

		{
			g_autofree gchar *day = venture_time_to_date_string(cursor, NULL);

			if (g_strcmp0(day, last_day) > 0)
				break;
		}
	}

	g_string_append(html, "</tbody></table></div>");
}

/* The agenda: the items in order, under the day each starts on. */
static void
planner_calendar_append_agenda(
	VentureWebServer	*self,
	GString			*html,
	VentureCalendarResult	*result
){
	const gchar *current = NULL;
	guint i;

	g_string_append(html, "<div class=\"cal-agenda\">");

	if (0 == result->items->len)
		g_string_append(html, "<p class=\"muted\">Nothing dated in this span.</p>");

	for (i = 0; i < result->items->len; i++)
	{
		VentureCalendarItem *item = g_ptr_array_index(result->items, i);

		if (0 != g_strcmp0(item->day, current))
		{
			if (NULL != current)
				g_string_append(html, "</ul></section>");

			g_string_append(html, "<section class=\"card cal-agenda-day\"><div class=\"card-head\"><h2>");
			venture_html_escape_append(html, item->day);
			g_string_append(html, "</h2></div><ul class=\"cal-items\">");
			current = item->day;
		}

		planner_calendar_append_item(self, html, item, TRUE);
	}

	if (NULL != current)
		g_string_append(html, "</ul></section>");

	g_string_append(html, "</div>");
}

static void
planner_calendar_append_option(
	GString		*html,
	const gchar	*value,
	const gchar	*label,
	const gchar	*selected
){
	g_string_append(html, "<option value=\"");
	venture_html_escape_append(html, value);
	g_string_append(html, (0 == g_strcmp0(value, selected)) ? "\" selected>" : "\">");
	venture_html_escape_append(html, label);
	g_string_append(html, "</option>");
}

/* The filters: a GET form, so a filtered calendar is a link. */
static void
planner_calendar_append_filters(
	VentureWebServer	*self,
	GString			*html,
	PlannerCalendarAsk	*ask
){
	g_auto(GStrv) kinds = NULL;
	g_autofree gchar *date = NULL;
	guint i;

	kinds = venture_calendar_sources_available(self->context);
	date = venture_time_to_date_string(ask->from, NULL);

	g_string_append(html, "<form class=\"filters cal-filters\" method=\"get\" action=\"/calendar\">"
	                      "<label>View <select name=\"view\">");
	planner_calendar_append_option(html, "month", "Month", ask->view);
	planner_calendar_append_option(html, "week", "Week", ask->view);
	planner_calendar_append_option(html, "agenda", "Agenda", ask->view);
	g_string_append(html, "</select></label> <label>From <input type=\"date\" name=\"date\" value=\"");
	venture_html_escape_append(html, date);
	g_string_append(html, "\"></label> <fieldset class=\"cal-kinds\"><legend>Show</legend>");

	for (i = 0; NULL != kinds[i]; i++)
	{
		g_string_append(html, "<label class=\"cal-kind-choice cal-kind-");
		venture_html_escape_append(html, kinds[i]);
		g_string_append(html, "\"><input type=\"checkbox\" name=\"kind\" value=\"");
		venture_html_escape_append(html, kinds[i]);
		g_string_append(html, "\"");

		/* Nothing chosen means everything; draw that as every box ticked
		 * only when the person narrowed it, so an untouched form sends
		 * no kinds and keeps meaning "all". */
		if ((NULL != ask->query.kinds) && planner_calendar_kind_shown(ask, kinds[i]))
			g_string_append(html, " checked");

		g_string_append(html, "> ");
		venture_html_escape_append(html, venture_calendar_sources_get_label(self->context, kinds[i]));
		g_string_append(html, "</label> ");
	}

	g_string_append(html, "</fieldset> <label>Venture id <input name=\"venture_id\" type=\"number\" min=\"0\" value=\"");

	if (0 != ask->query.venture_id)
		g_string_append_printf(html, "%" G_GINT64_FORMAT, ask->query.venture_id);

	g_string_append_printf(html, "\"></label> <label><input type=\"checkbox\" name=\"assignee\" value=\"me\"%s> Only mine</label>"
	                             " <label><input type=\"checkbox\" name=\"done\" value=\"1\"%s> Include finished</label>"
	                             " <button class=\"btn btn-primary\">Show</button></form>",
	                       ask->mine ? " checked" : "", ask->query.include_done ? " checked" : "");
}

/* Previous, today and next: the same view a span earlier or later. */
static void
planner_calendar_append_steps(
	GString			*html,
	PlannerCalendarAsk	*ask
){
	g_autoptr(GDateTime) before = NULL;
	g_autoptr(GDateTime) after = NULL;
	g_autofree gchar *filters = NULL;
	g_autofree gchar *prev_day = NULL;
	g_autofree gchar *next_day = NULL;

	if (0 == g_strcmp0(ask->view, "month"))
		before = g_date_time_add_months(ask->from, -1);
	else if (0 == g_strcmp0(ask->view, "week"))
		before = g_date_time_add_days(ask->from, -7);
	else
		before = g_date_time_add_days(ask->from, -30);

	after = g_date_time_ref(ask->to);
	filters = planner_calendar_filters(ask);
	prev_day = venture_time_to_date_string(before, NULL);
	next_day = venture_time_to_date_string(after, NULL);

	g_string_append(html, "<nav class=\"cal-steps\" aria-label=\"Calendar pages\">");
	g_string_append(html, "<a class=\"btn btn-sm\" href=\"/calendar?view=");
	venture_html_escape_append(html, ask->view);
	g_string_append(html, "&amp;date=");
	venture_html_escape_append(html, prev_day);
	venture_html_escape_append(html, filters);
	g_string_append(html, "\" aria-label=\"Previous\">Previous</a> ");
	g_string_append(html, "<a class=\"btn btn-sm\" href=\"/calendar?view=");
	venture_html_escape_append(html, ask->view);
	venture_html_escape_append(html, filters);
	g_string_append(html, "\">Today</a> ");
	g_string_append(html, "<a class=\"btn btn-sm\" href=\"/calendar?view=");
	venture_html_escape_append(html, ask->view);
	g_string_append(html, "&amp;date=");
	venture_html_escape_append(html, next_day);
	venture_html_escape_append(html, filters);
	g_string_append(html, "\" aria-label=\"Next\">Next</a></nav>");
}

static HtmxResponse *
planner_calendar_page(
	VentureWebServer	*self,
	HtmxRequest		*request,
	PlannerCalendarAsk	*ask,
	VentureCalendarResult	*result
){
	g_autoptr(GString) content = NULL;
	g_autofree gchar *filters = NULL;
	g_autofree gchar *first = NULL;
	g_autoptr(GDateTime) last = NULL;
	g_autofree gchar *last_text = NULL;
	guint i;

	filters = planner_calendar_filters(ask);
	first = venture_time_to_date_string(ask->from, NULL);
	last = g_date_time_add_days(ask->to, -1);
	last_text = venture_time_to_date_string(last, NULL);

	content = g_string_new("<div class=\"planner-calendar\"><div class=\"page-head\"><div class=\"page-title\">"
	                       "<h1>Calendar</h1><span class=\"subtitle\">");
	venture_html_escape_append(content, first);
	g_string_append(content, " to ");
	venture_html_escape_append(content, last_text);
	g_string_append(content, "</span></div><div class=\"page-actions\"><a class=\"btn\" href=\"/calendar.ics?from=");
	venture_html_escape_append(content, first);
	g_string_append(content, "&amp;to=");
	venture_html_escape_append(content, last_text);
	venture_html_escape_append(content, filters);
	g_string_append(content, "\">Subscribe (.ics)</a> <a class=\"btn btn-primary\" href=\"/e/event/new\">New event</a></div></div>");

	planner_calendar_append_steps(content, ask);
	planner_calendar_append_filters(self, content, ask);

	/* What was left out, said where it is read: a calendar missing a
	 * kind without saying so would look complete. */
	for (i = 0; i < result->notes->len; i++)
	{
		g_string_append(content, "<div class=\"notice info cal-note\" role=\"status\">");
		venture_html_escape_append(content, g_ptr_array_index(result->notes, i));
		g_string_append(content, "</div>");
	}

	if (0 == g_strcmp0(ask->view, "agenda"))
		planner_calendar_append_agenda(self, content, result);
	else
		planner_calendar_append_grid(self, content, ask, result);

	g_string_append(content, "<p class=\"muted\">Every dated record in one place. Dates change on the "
	                         "record, not here; a repeating event is one record.</p></div>");

	return venture_web_html_response(venture_web_page(self, request, "/calendar", "Calendar",
	                                                  content->str), 200);
}

/*
 * The one handler behind /calendar, /api/v1/calendar and /calendar.ics:
 * the same question read the same way, answered as a page, as JSON or as
 * a feed. Each door carries its own guard -- a session for the page, the
 * viewer role for the API and the feed, which a calendar client reaches
 * with a token.
 */
static HtmxResponse *
venture_planner_web_calendar(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 data
){
	VentureWebServer *self = data;
	g_autoptr(VentureCalendarResult) result = NULL;
	g_autoptr(GError) error = NULL;
	PlannerCalendarAsk ask;
	HtmxResponse *gate;
	HtmxResponse *response;
	const gchar *path;
	gboolean api;
	gboolean feed;

	(void)params;

	path = htmx_request_get_path(request);
	api = g_str_has_prefix(path, "/api/");
	feed = g_str_has_suffix(path, ".ics");

	if (api || feed)
	{
		gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_VIEWER);

		if (NULL == gate)
			gate = venture_web_require_module_api(self, "planner");
	}
	else
	{
		gate = venture_web_ui_require_session(self, request);

		if (NULL == gate)
			gate = venture_web_require_module_ui(self, request, "planner");
	}

	if (NULL != gate)
		return gate;

	gate = planner_calendar_read(self, request, &ask);

	if (NULL != gate)
	{
		planner_calendar_ask_clear(&ask);
		return gate;
	}

	result = venture_calendar_view_fetch(self->context, &ask.query, &error);

	if (NULL == result)
	{
		planner_calendar_ask_clear(&ask);
		return venture_web_error_response(error);
	}

	if (api)
	{
		g_autoptr(JsonNode) node = venture_calendar_result_to_json(result, &ask.query);

		json_object_set_string_member(json_node_get_object(node), "view", ask.view);
		response = venture_web_json_response(node, 200);
	}
	else if (feed)
	{
		g_autofree gchar *calendar = venture_calendar_result_to_ics(result);
		g_autoptr(GBytes) bytes = g_bytes_new(calendar, strlen(calendar));

		response = htmx_response_new();
		htmx_response_set_status(response, 200);
		htmx_response_set_bytes(response, bytes);
		htmx_response_set_content_type(response, "text/calendar; charset=utf-8");
	}
	else
	{
		response = planner_calendar_page(self, request, &ask, result);
	}

	planner_calendar_ask_clear(&ask);

	return response;
}

/* --- An event's own block ------------------------------------------------------ */

/* A time of day in the configured zone. */
static gchar *
planner_web_clock(
	VentureWebServer	*self,
	GDateTime		*when
){
	g_autoptr(GDateTime) local = g_date_time_to_timezone(when, venture_context_get_timezone(self->context));

	return g_date_time_format(local, "%H:%M");
}

/*
 * The event's run-sheet, who is coming and the work to prepare it, before
 * the generic panels -- which skip the agenda items, attendees and
 * tickets, so nothing is listed twice. The running times, the answers
 * counted and done-of-total all come from venture-planner.c.
 */
static void
venture_planner_web_append_event_block(
	VentureWebServer	*self,
	GString			*content,
	VentureEntity		*record
){
	g_autoptr(VenturePlannerAgenda) agenda = NULL;
	g_autoptr(GPtrArray) attendees = NULL;
	g_autoptr(GPtrArray) tickets = NULL;
	g_autofree gchar *online = NULL;
	VentureDatabase *database;
	guint counts[VENTURE_PLANNER_RSVP_COUNT];
	guint done;
	guint total;
	gint64 id;
	gint64 organization;
	guint i;

	database = venture_context_get_database(self->context);
	id = venture_entity_get_id(record);
	organization = venture_entity_get_organization_id(record);
	g_object_get(record, "online-url", &online, NULL);

	/* --- Joining online: a link only when it is a web address --- */

	if (venture_web_url_is_web(online))
	{
		g_string_append(content, "<div class=\"card event-online\"><div class=\"card-body\">"
		                         "<a class=\"btn btn-primary\" rel=\"noopener noreferrer\" href=\"");
		venture_html_escape_append(content, online);
		g_string_append(content, "\">Join online</a></div></div>");
	}

	/* --- The run-sheet --- */

	agenda = venture_planner_agenda(database, record, NULL);

	g_string_append(content, "<div class=\"card event-agenda\"><div class=\"card-head\"><h2>Run-sheet");

	if ((NULL != agenda) && (agenda->lines->len > 0))
		g_string_append_printf(content, " <span class=\"muted\">%" G_GINT64_FORMAT " min</span>",
		                       agenda->total_minutes);

	g_string_append_printf(content, "</h2><a class=\"btn btn-sm\" href=\"/e/event_agenda_item/new?event_id=%"
	                                G_GINT64_FORMAT "&amp;organization_id=%" G_GINT64_FORMAT "\">New item</a>"
	                                "</div><div class=\"card-body\">", id, organization);

	if ((NULL == agenda) || (0 == agenda->lines->len))
	{
		g_string_append(content, "<p class=\"muted\">No agenda yet. Each item's start follows "
		                         "from the event's start and the minutes before it.</p>");
	}
	else
	{
		if (agenda->overruns)
			g_string_append(content, "<div class=\"notice warning\" role=\"status\">The run-sheet "
			                         "runs past the event's end.</div>");

		/* The order is the content: a sort would reshuffle the day. */
		g_string_append(content, "<div class=\"table-wrap\"><table class=\"data event-runsheet\" data-no-sort>"
		                         "<thead><tr><th>Starts</th><th>Item</th><th>Presenter</th>"
		                         "<th class=\"num\">Minutes</th><th>Done</th></tr></thead><tbody>");

		for (i = 0; i < agenda->lines->len; i++)
		{
			VenturePlannerAgendaLine *line = g_ptr_array_index(agenda->lines, i);
			g_autofree gchar *title = NULL;
			g_autofree gchar *presenter = NULL;
			gint64 minutes;
			gboolean finished;

			g_object_get(line->item, "title", &title, "presenter", &presenter,
			             "minutes", &minutes, "done", &finished, NULL);

			g_string_append(content, "<tr><td class=\"num\">");

			if (NULL != line->starts)
			{
				g_autofree gchar *clock = planner_web_clock(self, line->starts);

				venture_html_escape_append(content, clock);
			}
			else
			{
				g_string_append_printf(content, "+%" G_GINT64_FORMAT " min", line->offset_minutes);
			}

			g_string_append_printf(content, "</td><td><a href=\"/e/event_agenda_item/%" G_GINT64_FORMAT "\">",
			                       venture_entity_get_id(line->item));
			venture_html_escape_append(content, venture_string_is_empty(title) ? "Item" : title);
			g_string_append(content, "</a></td><td>");
			venture_html_escape_append(content, (NULL != presenter) ? presenter : "");
			g_string_append_printf(content, "</td><td class=\"num\">%" G_GINT64_FORMAT "</td><td>%s</td></tr>",
			                       minutes, finished ? "<span class=\"badge positive\">done</span>" : "");
		}

		g_string_append(content, "</tbody></table></div>");
	}

	g_string_append(content, "</div></div>");

	/* --- Who is coming --- */

	attendees = venture_planner_attendees(database, record, counts, NULL);

	g_string_append(content, "<div class=\"card event-attendees\"><div class=\"card-head\"><h2>Attendees");

	if ((NULL != attendees) && (attendees->len > 0))
		g_string_append_printf(content, " <span class=\"muted\">%u yes, %u maybe, %u no, %u waiting</span>",
		                       counts[VENTURE_ATTENDEE_RSVP_YES], counts[VENTURE_ATTENDEE_RSVP_MAYBE],
		                       counts[VENTURE_ATTENDEE_RSVP_NO], counts[VENTURE_ATTENDEE_RSVP_PENDING]);

	g_string_append_printf(content, "</h2><a class=\"btn btn-sm\" href=\"/e/event_attendee/new?event_id=%"
	                                G_GINT64_FORMAT "&amp;organization_id=%" G_GINT64_FORMAT "\">Invite</a>"
	                                "</div><div class=\"card-body\">", id, organization);

	if ((NULL == attendees) || (0 == attendees->len))
	{
		g_string_append(content, "<p class=\"muted\">Nobody invited yet.</p>");
	}
	else
	{
		g_string_append(content, "<ul class=\"event-people\">");

		for (i = 0; i < attendees->len; i++)
		{
			VentureEntity *attendee = g_ptr_array_index(attendees, i);
			g_autofree gchar *label = venture_entity_get_display_name(attendee);
			gint role = 0;
			gint rsvp = 0;

			g_object_get(attendee, "role", &role, "rsvp", &rsvp, NULL);
			g_string_append_printf(content, "<li><a href=\"/e/event_attendee/%" G_GINT64_FORMAT "\">",
			                       venture_entity_get_id(attendee));
			venture_html_escape_append(content, label);
			g_string_append(content, "</a> <span class=\"badge\">");
			venture_web_append_nick(content, VENTURE_TYPE_ATTENDEE_RSVP, rsvp);
			g_string_append(content, "</span>");

			if (VENTURE_ATTENDEE_ROLE_REQUIRED != role)
			{
				g_string_append(content, " <span class=\"muted\">");
				venture_web_append_nick(content, VENTURE_TYPE_ATTENDEE_ROLE, role);
				g_string_append(content, "</span>");
			}

			g_string_append(content, "</li>");
		}

		g_string_append(content, "</ul>");
	}

	g_string_append(content, "</div></div>");

	/* --- The work to prepare it: ordinary tickets --- */

	if (!venture_web_module_enabled(self, "tickets"))
		return;

	tickets = venture_planner_prep_tickets(database, record, &done, &total, NULL);

	g_string_append(content, "<div class=\"card event-prep\"><div class=\"card-head\"><h2>Preparation");

	if ((NULL != tickets) && (total > 0))
		g_string_append_printf(content, " <span class=\"muted\">%u of %u done</span>", done, total);

	g_string_append_printf(content, "</h2><a class=\"btn btn-sm\" href=\"/e/ticket/new?event_id=%"
	                                G_GINT64_FORMAT "&amp;organization_id=%" G_GINT64_FORMAT "\">New task</a>"
	                                "</div><div class=\"card-body\">", id, organization);

	if ((NULL == tickets) || (0 == tickets->len))
	{
		g_string_append(content, "<p class=\"muted\">No preparation tickets. A ticket naming this "
		                         "event is listed here.</p></div></div>");
		return;
	}

	g_string_append(content, "<ul class=\"event-prep-list\">");

	for (i = 0; i < tickets->len; i++)
	{
		VentureEntity *ticket = g_ptr_array_index(tickets, i);
		g_autofree gchar *title = NULL;
		g_autoptr(GDateTime) due = NULL;
		gint status = 0;

		g_object_get(ticket, "title", &title, "status", &status, "due-at", &due, NULL);
		g_string_append_printf(content, "<li><a href=\"/e/ticket/%" G_GINT64_FORMAT "\">",
		                       venture_entity_get_id(ticket));
		venture_html_escape_append(content, venture_string_is_empty(title) ? "Ticket" : title);
		g_string_append(content, "</a> <span class=\"badge\">");
		venture_web_append_nick(content, VENTURE_TYPE_TICKET_STATUS, status);
		g_string_append(content, "</span>");

		if (NULL != due)
		{
			g_autofree gchar *when = venture_time_to_date_string(due,
				venture_context_get_timezone(self->context));

			g_string_append(content, " <span class=\"muted\">due ");
			venture_html_escape_append(content, when);
			g_string_append(content, "</span>");
		}

		g_string_append(content, "</li>");
	}

	g_string_append(content, "</ul></div></div>");
}

/* --- Routes -------------------------------------------------------------------- */

/*
 * Registered ahead of /api/v1/:type: the router takes the first match,
 * and the generic list route would read "calendar" as a record type and
 * its query string as field filters.
 */
static void
venture_planner_web_register(VentureWebServer *self)
{
	venture_web_server_add_classified_route(self, HTMX_METHOD_GET, "/calendar",
		VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, venture_planner_web_calendar, self);
	venture_web_server_add_classified_route(self, HTMX_METHOD_GET, "/calendar.ics",
		VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, venture_planner_web_calendar, self);
	venture_web_server_add_classified_route(self, HTMX_METHOD_GET, "/api/v1/calendar",
		VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, venture_planner_web_calendar, self);
}
