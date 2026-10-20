/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Included by the web server: the technician's day. */

/* "08:00", in the business's zone. */
static gchar *
jobs_web_clock(VentureWebServer *self, GDateTime *when)
{
	g_autoptr(GDateTime) local = g_date_time_to_timezone(when, venture_context_get_timezone(self->context));
	return g_date_time_format(local, "%H:%M");
}

static gint
jobs_web_by_window(gconstpointer a, gconstpointer b)
{
	VentureEntity *left = *(VentureEntity *const *)a;
	VentureEntity *right = *(VentureEntity *const *)b;
	g_autoptr(GDateTime) first = NULL;
	g_autoptr(GDateTime) second = NULL;
	gint order;
	g_object_get(left, "window-start", &first, NULL);
	g_object_get(right, "window-start", &second, NULL);
	order = g_date_time_compare(first, second);
	return order != 0 ? order : (venture_entity_get_id(left) < venture_entity_get_id(right) ? -1 : 1);
}

/* The jobs one technician is on, as a set of job ids; NULL for everyone. */
static GHashTable *
jobs_web_assigned(VentureWebServer *self, gint64 organization, gint64 user)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_JOB_TECHNICIAN);
	g_autoptr(GPtrArray) rows = NULL;
	GHashTable *jobs = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	guint i;
	venture_query_set_limit(query, 0);
	if (organization > 0)
		venture_query_set_organization(query, organization);
	venture_query_add_filter_int(query, "user-id", VENTURE_FILTER_OP_EQ, user, NULL);
	rows = venture_database_find(venture_context_get_database(self->context), query, NULL);
	for (i = 0; rows != NULL && i < rows->len; i++)
	{
		gint64 *id = g_new(gint64, 1);
		g_object_get(g_ptr_array_index(rows, i), "job-id", id, NULL);
		g_hash_table_add(jobs, id);
	}
	return jobs;
}

/* Who is on a job, by name. */
static void
jobs_web_append_technicians(VentureWebServer *self, GString *html, VentureEntity *job)
{
	VentureDatabase *database = venture_context_get_database(self->context);
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_JOB_TECHNICIAN);
	g_autoptr(GPtrArray) rows = NULL;
	guint i;
	venture_query_set_limit(query, 0);
	venture_query_set_organization(query, venture_entity_get_organization_id(job));
	venture_query_add_filter_int(query, "job-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(job), NULL);
	rows = venture_database_find(database, query, NULL);
	if (rows == NULL || rows->len == 0)
	{
		g_string_append(html, "<p class=\"muted small\">Nobody is on this job yet.</p>");
		return;
	}
	g_string_append(html, "<p class=\"small\">");
	for (i = 0; i < rows->len; i++)
	{
		gint64 user_id = 0;
		g_autoptr(VentureEntity) user = NULL;
		g_autofree gchar *username = NULL, *display = NULL;
		g_object_get(g_ptr_array_index(rows, i), "user-id", &user_id, NULL);
		user = venture_database_get(database, VENTURE_TYPE_USER, user_id, NULL);
		if (user != NULL)
			g_object_get(user, "username", &username, "display-name", &display, NULL);
		if (i > 0)
			g_string_append(html, ", ");
		venture_html_escape_append(html, !venture_string_is_empty(display) ? display
			: (username != NULL ? username : "a former user"));
	}
	g_string_append(html, "</p>");
}

/* One stop on the day: when, what, who, where, how to get in, and the way
 * to call or get directions with one thumb. */
static void
jobs_web_append_stop(VentureWebServer *self, GString *html, VentureEntity *job)
{
	VentureDatabase *database = venture_context_get_database(self->context);
	g_autoptr(GDateTime) start = NULL, end = NULL;
	g_autoptr(VentureEntity) company = NULL, contact = NULL, property = NULL;
	g_autofree gchar *title = NULL, *number = NULL, *address = NULL, *from = NULL, *to = NULL;
	g_autofree gchar *phone = NULL, *who = NULL, *access = NULL, *problem = NULL;
	gint64 company_id = 0, contact_id = 0, property_id = 0;
	gint status = 0;
	g_object_get(job, "title", &title, "number", &number, "window-start", &start, "window-end", &end,
		"company-id", &company_id, "contact-id", &contact_id, "property-id", &property_id,
		"service-address", &address, "status", &status, "problem", &problem, NULL);
	company = venture_database_get(database, VENTURE_TYPE_COMPANY, company_id, NULL);
	if (contact_id > 0)
		contact = venture_database_get(database, VENTURE_TYPE_CONTACT, contact_id, NULL);
	if (property_id > 0)
		property = venture_database_get(database, VENTURE_TYPE_PROPERTY, property_id, NULL);
	if (contact != NULL)
		g_object_get(contact, "phone", &phone, NULL);
	if (venture_string_is_empty(phone) && company != NULL)
	{
		g_clear_pointer(&phone, g_free);
		g_object_get(company, "phone", &phone, NULL);
	}
	if (property != NULL)
		g_object_get(property, "access-notes", &access, NULL);
	who = company != NULL ? venture_entity_get_display_name(company) : g_strdup("");
	from = start != NULL ? jobs_web_clock(self, start) : NULL;
	to = end != NULL ? jobs_web_clock(self, end) : NULL;

	g_string_append(html, "<li class=\"card job-stop\"><div class=\"card-body\"><p><strong class=\"job-window\">");
	venture_html_escape_append(html, from != NULL ? from : "");
	if (to != NULL)
	{
		g_string_append(html, "\xe2\x80\x93");
		venture_html_escape_append(html, to);
	}
	g_string_append(html, "</strong> <span class=\"status\">");
	{
		g_autofree gchar *label = venture_web_label_from_name(venture_enum_to_nick(venture_job_status_get_type(), status));
		venture_html_escape_append(html, label);
	}
	g_string_append_printf(html, "</span></p><h3><a href=\"/e/job/%" G_GINT64_FORMAT "\">", venture_entity_get_id(job));
	venture_html_escape_append(html, title);
	g_string_append(html, "</a></h3><p>");
	venture_html_escape_append(html, who);
	if (!venture_string_is_empty(phone))
	{
		/* tel: takes the digits, with a leading + kept. */
		g_autoptr(GString) dial = g_string_new(NULL);
		const gchar *p;
		for (p = phone; *p != '\0'; p++)
			if (g_ascii_isdigit(*p) || (*p == '+' && dial->len == 0))
				g_string_append_c(dial, *p);
		g_string_append(html, " \xc2\xb7 <a href=\"tel:");
		venture_html_escape_append(html, dial->str);
		g_string_append(html, "\">");
		venture_html_escape_append(html, phone);
		g_string_append(html, "</a>");
	}
	g_string_append(html, "</p>");
	if (!venture_string_is_empty(address))
	{
		g_autofree gchar *query = g_uri_escape_string(address, NULL, FALSE);
		g_string_append(html, "<p class=\"job-address\">");
		venture_html_escape_append(html, address);
		g_string_append_printf(html, " <a class=\"btn btn-sm\" href=\"geo:0,0?q=%s\">Directions</a></p>", query);
	}
	if (!venture_string_is_empty(access))
	{
		g_string_append(html, "<p class=\"small\"><strong>Getting in:</strong> ");
		venture_html_escape_append(html, access);
		g_string_append(html, "</p>");
	}
	if (!venture_string_is_empty(problem))
	{
		g_string_append(html, "<p class=\"small\">");
		venture_html_escape_append(html, problem);
		g_string_append(html, "</p>");
	}
	jobs_web_append_technicians(self, html, job);
	g_string_append(html, "</div></li>");
}

/*
 * GET /jobs/day?date=YYYY-MM-DD&tech=ID|all -- a technician's day: the jobs
 * they are on whose arrival window starts that day in the business's zone,
 * earliest first, each with its window, customer, phone, address and how to
 * get in. Without tech it is the signed-in person's day; tech=all is
 * everyone's. Cancelled jobs are left out.
 */
static HtmxResponse *
venture_jobs_web_day(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	VentureWebServer *self = user_data;
	VentureDatabase *database = venture_context_get_database(self->context);
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GString) content = g_string_new(NULL);
	g_autoptr(GDateTime) day = NULL, next = NULL, now = venture_time_now();
	g_autoptr(GHashTable) assigned = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureQuery) people = NULL;
	g_autoptr(GPtrArray) jobs = NULL;
	g_autoptr(GPtrArray) users = NULL;
	g_autoptr(GPtrArray) stops = g_ptr_array_new_with_free_func(g_object_unref);
	g_autofree gchar *day_text = NULL;
	const gchar *date = htmx_request_get_query_param(request, "date");
	const gchar *tech = htmx_request_get_query_param(request, "tech");
	gint64 organization = venture_web_active_organization(self, request);
	gint64 user = 0;
	gboolean everyone;
	HtmxResponse *gate;
	guint i;
	(void)params;

	gate = venture_web_ui_require_session(self, request);
	if (gate != NULL)
		return gate;
	gate = venture_web_require_module_ui(self, request, "jobs");
	if (gate != NULL)
		return gate;
	principal = venture_auth_authenticate(self->auth, request);
	if (!venture_web_require_for_type(self, principal, VENTURE_TYPE_JOB, VENTURE_USER_ROLE_VIEWER, NULL))
	{
		g_autoptr(GError) error = NULL;
		venture_web_require_for_type(self, principal, VENTURE_TYPE_JOB, VENTURE_USER_ROLE_VIEWER, &error);
		return venture_web_error_response(error);
	}
	everyone = g_strcmp0(tech, "all") == 0;
	if (!everyone)
		user = !venture_string_is_empty(tech) ? g_ascii_strtoll(tech, NULL, 10)
			: (principal != NULL ? principal->user_id : 0);

	{
		GTimeZone *zone = venture_context_get_timezone(self->context);
		g_autoptr(GDateTime) local = g_date_time_to_timezone(now, zone);
		gint y = g_date_time_get_year(local), m = g_date_time_get_month(local), d = g_date_time_get_day_of_month(local);
		if (!venture_string_is_empty(date))
			sscanf(date, "%4d-%2d-%2d", &y, &m, &d);
		day = g_date_time_new(zone, y, m, d, 0, 0, 0);
		if (day == NULL)
			day = g_date_time_new(zone, g_date_time_get_year(local), g_date_time_get_month(local),
				g_date_time_get_day_of_month(local), 0, 0, 0);
		next = g_date_time_add_days(day, 1);
		day_text = g_date_time_format(day, "%Y-%m-%d");
	}

	query = venture_query_new(VENTURE_TYPE_JOB);
	venture_query_set_limit(query, 0);
	if (organization > 0)
		venture_query_set_organization(query, organization);
	jobs = venture_database_find(database, query, NULL);
	if (!everyone)
		assigned = jobs_web_assigned(self, organization, user);
	for (i = 0; jobs != NULL && i < jobs->len; i++)
	{
		VentureEntity *job = g_ptr_array_index(jobs, i);
		g_autoptr(GDateTime) start = NULL;
		gint status = 0;
		gint64 id = venture_entity_get_id(job);
		g_object_get(job, "window-start", &start, "status", &status, NULL);
		if (start == NULL || status == VENTURE_JOB_CANCELLED ||
		    g_date_time_compare(start, day) < 0 || g_date_time_compare(start, next) >= 0)
			continue;
		if (assigned != NULL && !g_hash_table_contains(assigned, &id))
			continue;
		g_ptr_array_add(stops, g_object_ref(job));
	}
	g_ptr_array_sort(stops, jobs_web_by_window);

	g_string_append(content, "<div class=\"page-head\"><div class=\"page-title\"><h1>Day</h1><span class=\"subtitle\">");
	venture_html_escape_append(content, day_text);
	g_string_append(content, "</span></div></div><form class=\"card\" method=\"get\" action=\"/jobs/day\"><div class=\"card-body\">"
		"<label class=\"field\"><span class=\"field-label\">Day</span><input type=\"date\" name=\"date\" value=\"");
	venture_html_escape_append(content, day_text);
	g_string_append(content, "\"></label><label class=\"field\"><span class=\"field-label\">Technician</span><select name=\"tech\">");
	g_string_append_printf(content, "<option value=\"all\"%s>Everyone</option>", everyone ? " selected" : "");
	people = venture_query_new(VENTURE_TYPE_USER);
	venture_query_set_limit(people, 0);
	users = venture_database_find(database, people, NULL);
	for (i = 0; users != NULL && i < users->len; i++)
	{
		VentureEntity *person = g_ptr_array_index(users, i);
		g_autofree gchar *username = NULL, *display = NULL;
		gboolean active = FALSE;
		g_object_get(person, "username", &username, "display-name", &display, "active", &active, NULL);
		if (!active)
			continue;
		g_string_append_printf(content, "<option value=\"%" G_GINT64_FORMAT "\"%s>", venture_entity_get_id(person),
			(!everyone && venture_entity_get_id(person) == user) ? " selected" : "");
		venture_html_escape_append(content, !venture_string_is_empty(display) ? display : username);
		g_string_append(content, "</option>");
	}
	g_string_append(content, "</select></label><button class=\"btn btn-primary\" type=\"submit\">Show</button></div></form>");
	if (stops->len == 0)
		g_string_append(content, "<div class=\"empty\"><h3>Nothing booked</h3><p class=\"muted\">Jobs show here once they "
			"have an arrival window on this day and the technician is on them.</p></div>");
	else
	{
		g_string_append(content, "<ol class=\"job-day\">");
		for (i = 0; i < stops->len; i++)
			jobs_web_append_stop(self, content, g_ptr_array_index(stops, i));
		g_string_append(content, "</ol>");
	}
	return venture_web_html_response(venture_web_page(self, request, "/jobs/day", "Day", content->str), 200);
}

static void
venture_jobs_web_register(VentureWebServer *self)
{
	venture_web_server_add_classified_route(self, HTMX_METHOD_GET, "/jobs/day",
		VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, venture_jobs_web_day, self);
}
