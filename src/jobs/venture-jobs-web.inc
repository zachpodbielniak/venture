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

/* Who is on a job, by name, the lead marked. */
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
		gboolean lead = FALSE;
		g_autoptr(VentureEntity) user = NULL;
		g_autofree gchar *username = NULL, *display = NULL;
		g_object_get(g_ptr_array_index(rows, i), "user-id", &user_id, "lead", &lead, NULL);
		user = venture_database_get(database, VENTURE_TYPE_USER, user_id, NULL);
		if (user != NULL)
			g_object_get(user, "username", &username, "display-name", &display, NULL);
		if (i > 0)
			g_string_append(html, ", ");
		venture_html_escape_append(html, !venture_string_is_empty(display) ? display
			: (username != NULL ? username : "a former user"));
		if (lead)
			g_string_append(html, " (lead)");
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

/* What is at a property: one line per piece of equipment. */
static void
jobs_web_append_equipment(VentureWebServer *self, GString *html, gint64 organization, gint64 property)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_EQUIPMENT);
	g_autoptr(GPtrArray) rows = NULL;
	guint i;
	venture_query_set_limit(query, 50);
	venture_query_set_organization(query, organization);
	venture_query_add_filter_int(query, "property-id", VENTURE_FILTER_OP_EQ, property, NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	rows = venture_database_find(venture_context_get_database(self->context), query, NULL);
	if (rows == NULL || rows->len == 0)
		return;
	g_string_append(html, "<h3>Equipment there</h3><ul class=\"job-equipment\">");
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(rows, i);
		g_autofree gchar *name = NULL, *make = NULL, *model = NULL, *serial = NULL;
		gint64 year = 0;
		g_object_get(row, "name", &name, "make", &make, "model", &model, "serial", &serial,
			"installed-year", &year, NULL);
		g_string_append_printf(html, "<li><a href=\"/e/equipment/%" G_GINT64_FORMAT "\">", venture_entity_get_id(row));
		venture_html_escape_append(html, name);
		g_string_append(html, "</a>");
		if (!venture_string_is_empty(make) || !venture_string_is_empty(model))
		{
			g_string_append(html, " \xc2\xb7 ");
			venture_html_escape_append(html, make != NULL ? make : "");
			if (!venture_string_is_empty(make) && !venture_string_is_empty(model))
				g_string_append_c(html, ' ');
			venture_html_escape_append(html, model != NULL ? model : "");
		}
		if (year > 0)
			g_string_append_printf(html, " \xc2\xb7 put in %" G_GINT64_FORMAT, year);
		if (!venture_string_is_empty(serial))
		{
			g_string_append(html, " \xc2\xb7 serial ");
			venture_html_escape_append(html, serial);
		}
		g_string_append(html, "</li>");
	}
	g_string_append(html, "</ul>");
}

/* A job's page: who is on it, how to get in and what is there -- what a
 * technician reads in the van. Sending someone is the Actions card's
 * "Send a technician". */
static void
jobs_web_append_job_block(VentureWebServer *self, GString *html, VentureEntity *job)
{
	VentureDatabase *database = venture_context_get_database(self->context);
	g_autoptr(VentureEntity) property = NULL;
	g_autofree gchar *access = NULL;
	gint64 property_id = 0;
	g_object_get(job, "property-id", &property_id, NULL);
	if (property_id > 0)
		property = venture_database_get(database, VENTURE_TYPE_PROPERTY, property_id, NULL);
	if (property != NULL && !venture_entity_is_deleted(property))
		g_object_get(property, "access-notes", &access, NULL);
	g_string_append(html, "<section class=\"card job-visit\"><div class=\"card-head\"><h2>The visit</h2></div><div class=\"card-body\">"
		"<h3>Technicians</h3>");
	jobs_web_append_technicians(self, html, job);
	if (!venture_string_is_empty(access))
	{
		g_string_append(html, "<h3>Getting in</h3><p>");
		venture_html_escape_append(html, access);
		g_string_append(html, "</p>");
	}
	if (property != NULL && !venture_entity_is_deleted(property))
		jobs_web_append_equipment(self, html, venture_entity_get_organization_id(job), property_id);
	g_string_append(html, "</div></section>");
}

/* How many of @type name the customer, and a link to them. */
static void
jobs_web_append_work_row(VentureWebServer *self, GString *html, VentureEntity *company, GType type,
	const gchar *type_name, const gchar *new_path, const gchar *new_label)
{
	g_autoptr(VentureQuery) query = NULL;
	g_autofree gchar *plural = NULL;
	gint64 count;
	if (!venture_entity_registry_is_type_enabled(venture_entity_registry_get_default(), type_name))
		return;
	query = venture_query_new(type);
	venture_query_set_organization(query, venture_entity_get_organization_id(company));
	venture_query_add_filter_int(query, "company-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(company), NULL);
	count = venture_database_count(venture_context_get_database(self->context), query, NULL);
	plural = venture_web_type_label(type, TRUE);
	g_string_append_printf(html, "<li><a href=\"/e/%s?company_id=%" G_GINT64_FORMAT "\">", type_name,
		venture_entity_get_id(company));
	venture_html_escape_append(html, plural);
	g_string_append_printf(html, "</a> <span class=\"count\">%" G_GINT64_FORMAT "</span> "
		"<a class=\"btn btn-sm btn-ghost\" href=\"", count);
	venture_html_escape_append(html, new_path);
	g_string_append(html, "\">");
	venture_html_escape_append(html, new_label);
	g_string_append(html, "</a></li>");
}

/* A customer's page leads to the work done for them: their places, jobs,
 * quotes and invoices, each with the way to start one. */
static void
jobs_web_append_customer_work(VentureWebServer *self, GString *html, VentureEntity *company)
{
	gint64 id = venture_entity_get_id(company);
	g_autofree gchar *new_property = g_strdup_printf("/e/property/new?company_id=%" G_GINT64_FORMAT, id);
	g_autofree gchar *new_job = g_strdup_printf("/e/job/new?company_id=%" G_GINT64_FORMAT, id);
	g_autofree gchar *new_quote = g_strdup_printf("/quotes/compose?company_id=%" G_GINT64_FORMAT, id);
	g_autofree gchar *new_invoice = g_strdup_printf("/invoices/compose?company_id=%" G_GINT64_FORMAT, id);
	g_string_append(html, "<section class=\"card customer-work\"><div class=\"card-head\"><h2>Work</h2></div>"
		"<div class=\"card-body\"><ul class=\"related\">");
	jobs_web_append_work_row(self, html, company, VENTURE_TYPE_PROPERTY, "property", new_property, "New property");
	jobs_web_append_work_row(self, html, company, VENTURE_TYPE_JOB, "job", new_job, "New job");
	jobs_web_append_work_row(self, html, company, VENTURE_TYPE_QUOTE, "quote", new_quote, "New quote");
	jobs_web_append_work_row(self, html, company, VENTURE_TYPE_INVOICE, "invoice", new_invoice, "New invoice");
	g_string_append(html, "</ul></div></section>");
}

/* Today's jobs in the business's zone, earliest first: one person's, or
 * everyone's when @user is 0. Cancelled jobs are left out. */
static GPtrArray *
jobs_web_today(VentureWebServer *self, gint64 organization, gint64 user)
{
	GTimeZone *zone = venture_context_get_timezone(self->context);
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(GDateTime) local = g_date_time_to_timezone(now, zone);
	g_autoptr(GDateTime) day = g_date_time_new(zone, g_date_time_get_year(local),
		g_date_time_get_month(local), g_date_time_get_day_of_month(local), 0, 0, 0);
	g_autoptr(GDateTime) next = g_date_time_add_days(day, 1);
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_JOB);
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GHashTable) assigned = NULL;
	GPtrArray *today = g_ptr_array_new_with_free_func(g_object_unref);
	guint i;
	venture_query_set_limit(query, 0);
	if (organization > 0)
		venture_query_set_organization(query, organization);
	{
		/* Stored times compare as their UTC text, as the calendar's do. */
		g_autofree gchar *from_text = venture_time_to_string(day);
		g_autofree gchar *to_text = venture_time_to_string(next);
		venture_query_add_filter_string(query, "window-start", VENTURE_FILTER_OP_GTE, from_text, NULL);
		venture_query_add_filter_string(query, "window-start", VENTURE_FILTER_OP_LT, to_text, NULL);
	}
	rows = venture_database_find(venture_context_get_database(self->context), query, NULL);
	if (user > 0)
		assigned = jobs_web_assigned(self, organization, user);
	for (i = 0; rows != NULL && i < rows->len; i++)
	{
		VentureEntity *job = g_ptr_array_index(rows, i);
		gint64 id = venture_entity_get_id(job);
		gint status = 0;
		g_object_get(job, "status", &status, NULL);
		if (status == VENTURE_JOB_CANCELLED)
			continue;
		if (assigned != NULL && !g_hash_table_contains(assigned, &id))
			continue;
		g_ptr_array_add(today, g_object_ref(job));
	}
	g_ptr_array_sort(today, jobs_web_by_window);
	return today;
}

/* One line per job: when, what, for whom. */
static void
jobs_web_append_today_list(VentureWebServer *self, GString *html, GPtrArray *jobs, guint limit)
{
	VentureDatabase *database = venture_context_get_database(self->context);
	guint i;
	g_string_append(html, "<ul class=\"related\">");
	for (i = 0; i < jobs->len && i < limit; i++)
	{
		VentureEntity *job = g_ptr_array_index(jobs, i);
		g_autoptr(GDateTime) start = NULL;
		g_autoptr(VentureEntity) company = NULL;
		g_autofree gchar *title = NULL, *clock = NULL, *who = NULL;
		gint64 company_id = 0;
		g_object_get(job, "title", &title, "window-start", &start, "company-id", &company_id, NULL);
		clock = jobs_web_clock(self, start);
		company = venture_database_get(database, VENTURE_TYPE_COMPANY, company_id, NULL);
		who = company != NULL ? venture_entity_get_display_name(company) : NULL;
		g_string_append(html, "<li><strong>");
		venture_html_escape_append(html, clock);
		g_string_append_printf(html, "</strong> <a href=\"/e/job/%" G_GINT64_FORMAT "\">", venture_entity_get_id(job));
		venture_html_escape_append(html, title);
		g_string_append(html, "</a>");
		if (who != NULL)
		{
			g_string_append(html, " \xc2\xb7 ");
			venture_html_escape_append(html, who);
		}
		g_string_append(html, "</li>");
	}
	if (jobs->len > limit)
		g_string_append_printf(html, "<li class=\"muted\">and %u more</li>", jobs->len - limit);
	g_string_append(html, "</ul>");
}

/* My day's jobs: the signed-in person's, or everyone's for the local
 * owner of an install without sign-in, who is nobody's technician. */
static void
venture_jobs_web_append_today(VentureWebServer *self, GString *html, HtmxRequest *request,
	VentureAuthPrincipal *principal)
{
	g_autoptr(GPtrArray) jobs = NULL;
	gint64 user = principal != NULL ? principal->user_id : 0;
	if (!venture_web_module_enabled(self, "jobs"))
		return;
	jobs = jobs_web_today(self, venture_web_active_organization(self, request), user);
	g_string_append(html, "<div class=\"card\"><div class=\"card-head\"><h2>Jobs today</h2>"
		"<a class=\"btn btn-sm\" href=\"/jobs/day\">Your day</a></div><div class=\"card-body\">");
	if (jobs->len == 0)
		g_string_append(html, "<p class=\"muted\">No jobs booked for today.</p>");
	else
		jobs_web_append_today_list(self, html, jobs, 20);
	g_string_append(html, "</div></div>");
}

/* How many rows of @type in the business match one status. */
static gint64
jobs_web_count_status(VentureWebServer *self, GType type, gint64 organization, const gchar *status)
{
	g_autoptr(VentureQuery) query = venture_query_new(type);
	if (organization > 0)
		venture_query_set_organization(query, organization);
	venture_query_add_filter_string(query, "status", VENTURE_FILTER_OP_EQ, status, NULL);
	return venture_database_count(venture_context_get_database(self->context), query, NULL);
}

/* Issued invoices past their due date: what a chase list starts from. */
static gint64
jobs_web_count_overdue(VentureWebServer *self, gint64 organization)
{
	static const gchar *const open[] = { "sent", "partially_paid", NULL };
	g_autoptr(GDateTime) now = venture_time_now();
	g_autofree gchar *now_text = venture_time_to_string(now);
	gint64 count = 0;
	guint i;
	for (i = 0; open[i] != NULL; i++)
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_INVOICE);
		if (organization > 0)
			venture_query_set_organization(query, organization);
		venture_query_add_filter_string(query, "status", VENTURE_FILTER_OP_EQ, open[i], NULL);
		venture_query_add_filter_string(query, "due-at", VENTURE_FILTER_OP_LT, now_text, NULL);
		count += venture_database_count(venture_context_get_database(self->context), query, NULL);
	}
	return count;
}

/* The home page's first card for a business that does jobs: what is on
 * today, which quotes wait for an answer and who owes money -- the
 * questions an owner opens the app with, before the accounts. */
static void
venture_jobs_web_append_home(VentureWebServer *self, GString *html, HtmxRequest *request)
{
	g_autoptr(GPtrArray) jobs = NULL;
	gint64 organization = venture_web_active_organization(self, request);
	if (!venture_web_module_enabled(self, "jobs"))
		return;
	if (organization <= 0)
		organization = venture_context_get_default_organization_id(self->context);
	jobs = jobs_web_today(self, organization, 0);
	g_string_append(html, "<section class=\"card home-today\"><div class=\"card-head\"><h2>Today</h2>"
		"<a class=\"btn btn-sm\" href=\"/jobs/day?tech=all\">The day</a></div><div class=\"card-body\">");
	if (jobs->len == 0)
		g_string_append(html, "<p class=\"muted\">No jobs booked for today.</p>");
	else
		jobs_web_append_today_list(self, html, jobs, 8);
	if (venture_web_module_enabled(self, "quotes"))
		g_string_append_printf(html, "<p><a href=\"/e/quote?status=sent\">Quotes waiting for an answer</a> "
			"<span class=\"count\">%" G_GINT64_FORMAT "</span></p>",
			jobs_web_count_status(self, VENTURE_TYPE_QUOTE, organization, "sent"));
	if (venture_web_module_enabled(self, "receivables"))
		g_string_append_printf(html, "<p><a href=\"/reports/receivables\">Overdue invoices</a> "
			"<span class=\"count\">%" G_GINT64_FORMAT "</span></p>",
			jobs_web_count_overdue(self, organization));
	g_string_append(html, "</div></section>");
}

/* A new property opened from a customer starts at the customer's address:
 * for most homeowners it is the same place, and typing it twice is how the
 * two drift apart. */
static void
venture_jobs_web_seed_new(VentureWebServer *self, HtmxRequest *request, VentureEntity *record)
{
	g_autoptr(VentureEntity) company = NULL;
	g_autofree gchar *address = NULL, *theirs = NULL;
	gint64 company_id = 0;
	gint64 organization = venture_web_active_organization(self, request);
	if (!venture_web_module_enabled(self, "jobs") || !VENTURE_IS_PROPERTY(record))
		return;
	g_object_get(record, "company-id", &company_id, "address", &address, NULL);
	if (company_id <= 0 || !venture_string_is_empty(address))
		return;
	if (organization <= 0)
		organization = venture_context_get_default_organization_id(self->context);
	company = venture_database_get(venture_context_get_database(self->context), VENTURE_TYPE_COMPANY, company_id, NULL);
	/* Only a customer of the business on screen. */
	if (company == NULL || venture_entity_get_organization_id(company) != organization)
		return;
	g_object_get(company, "address", &theirs, NULL);
	if (!venture_string_is_empty(theirs))
		g_object_set(record, "address", theirs, NULL);
}

static void
venture_jobs_web_append_block(VentureWebServer *self, GString *html, VentureEntity *record)
{
	if (!venture_web_module_enabled(self, "jobs") || venture_entity_get_id(record) <= 0)
		return;
	if (VENTURE_IS_JOB(record))
		jobs_web_append_job_block(self, html, record);
	else if (VENTURE_IS_COMPANY(record))
		jobs_web_append_customer_work(self, html, record);
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
	/* This business's people only, as the technician picker offers.
	 * Membership plus people filed here, when organization access is
	 * on; otherwise the user row. */
	if (organization > 0 && venture_web_users_by_membership(self))
		users = venture_web_member_users(self, organization);
	else
	{
		people = venture_query_new(VENTURE_TYPE_USER);
		venture_query_set_limit(people, 0);
		if (organization > 0)
			venture_query_set_organization(people, organization);
		users = venture_database_find(database, people, NULL);
	}
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

static gint
jobs_web_by_next_visit(gconstpointer a, gconstpointer b)
{
	g_autoptr(GDateTime) left = NULL, right = NULL;
	g_object_get(*(VentureEntity *const *)a, "next-visit", &left, NULL);
	g_object_get(*(VentureEntity *const *)b, "next-visit", &right, NULL);
	return g_date_time_compare(left, right);
}

/*
 * GET /jobs/visits -- maintenance visits due in the next 30 days or
 * overdue, soonest first: whose, where, what, and a button that books the
 * job. A plan with no next date, or one that has ended, is not listed.
 */
static HtmxResponse *
venture_jobs_web_visits(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	VentureWebServer *self = user_data;
	VentureDatabase *database = venture_context_get_database(self->context);
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GString) content = g_string_new(NULL);
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_MAINTENANCE_PLAN);
	g_autoptr(GPtrArray) plans = NULL;
	g_autoptr(GPtrArray) due = g_ptr_array_new_with_free_func(g_object_unref);
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(GDateTime) horizon = g_date_time_add_days(now, 30);
	gint64 organization = venture_web_active_organization(self, request);
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
	{
		g_autoptr(GError) error = NULL;
		if (!venture_web_require_for_type(self, principal, VENTURE_TYPE_MAINTENANCE_PLAN, VENTURE_USER_ROLE_VIEWER, &error))
			return venture_web_error_response(error);
	}
	venture_query_set_limit(query, 0);
	if (organization > 0)
		venture_query_set_organization(query, organization);
	plans = venture_database_find(database, query, NULL);
	for (i = 0; plans != NULL && i < plans->len; i++)
	{
		VentureEntity *plan = g_ptr_array_index(plans, i);
		g_autoptr(GDateTime) next = NULL, ended = NULL;
		g_object_get(plan, "next-visit", &next, "ended-at", &ended, NULL);
		if (next == NULL || ended != NULL || g_date_time_compare(next, horizon) > 0)
			continue;
		g_ptr_array_add(due, g_object_ref(plan));
	}
	g_ptr_array_sort(due, jobs_web_by_next_visit);

	g_string_append(content, "<div class=\"page-head\"><div class=\"page-title\"><h1>Visits due</h1>"
		"<span class=\"subtitle\">Maintenance plans due in the next 30 days, or overdue</span></div>"
		"<div class=\"page-actions\"><a class=\"btn\" href=\"/e/maintenance_plan\">All plans</a>"
		"<a class=\"btn btn-primary\" href=\"/e/maintenance_plan/new\">New plan</a></div></div>");
	if (due->len == 0)
	{
		g_string_append(content, "<div class=\"empty\"><h3>Nothing due</h3><p class=\"muted\">A maintenance plan "
			"shows here when its next visit is less than 30 days away.</p></div>");
		return venture_web_html_response(venture_web_page(self, request, "/jobs/visits", "Visits due", content->str), 200);
	}
	g_string_append(content, "<ul class=\"job-visits\">");
	for (i = 0; i < due->len; i++)
	{
		VentureEntity *plan = g_ptr_array_index(due, i);
		g_autoptr(GDateTime) next = NULL;
		g_autoptr(GDateTime) local = NULL;
		g_autoptr(VentureEntity) company = NULL;
		g_autoptr(VentureEntity) property = NULL;
		g_autofree gchar *visit = NULL, *when = NULL, *who = NULL, *address = NULL;
		gint64 company_id = 0, property_id = 0, id = venture_entity_get_id(plan);
		g_object_get(plan, "visit", &visit, "next-visit", &next, "company-id", &company_id,
			"property-id", &property_id, NULL);
		local = g_date_time_to_timezone(next, venture_context_get_timezone(self->context));
		when = g_date_time_format(local, "%-d %B %Y");
		company = venture_database_get(database, VENTURE_TYPE_COMPANY, company_id, NULL);
		who = company != NULL ? venture_entity_get_display_name(company) : NULL;
		if (property_id > 0)
			property = venture_database_get(database, VENTURE_TYPE_PROPERTY, property_id, NULL);
		if (property != NULL)
			g_object_get(property, "address", &address, NULL);
		g_string_append(content, "<li class=\"card\"><div class=\"card-body\"><strong>");
		venture_html_escape_append(content, when);
		g_string_append_printf(content, "</strong> <a href=\"/e/maintenance_plan/%" G_GINT64_FORMAT "\">", id);
		venture_html_escape_append(content, visit);
		g_string_append(content, "</a>");
		if (who != NULL)
		{
			g_string_append(content, " \xc2\xb7 ");
			venture_html_escape_append(content, who);
		}
		if (!venture_string_is_empty(address))
		{
			g_string_append(content, " \xc2\xb7 ");
			venture_html_escape_append(content, address);
		}
		g_string_append_printf(content, "<form method=\"post\" action=\"/api/v1/maintenance_plan/%" G_GINT64_FORMAT
			"/actions/book\"><button class=\"btn btn-primary btn-sm\" type=\"submit\">Book the visit</button></form>"
			"</div></li>", id);
	}
	g_string_append(content, "</ul>");
	return venture_web_html_response(venture_web_page(self, request, "/jobs/visits", "Visits due", content->str), 200);
}

static void
venture_jobs_web_register(VentureWebServer *self)
{
	venture_web_server_add_classified_route(self, HTMX_METHOD_GET, "/jobs/day",
		VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, venture_jobs_web_day, self);
	venture_web_server_add_classified_route(self, HTMX_METHOD_GET, "/jobs/visits",
		VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, venture_jobs_web_visits, self);
}
