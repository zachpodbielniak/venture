/* SPDX-License-Identifier: AGPL-3.0-or-later */
static HtmxResponse *
venture_backup_web_export(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) result = NULL;
	g_autoptr(GError) error = NULL;
	VentureActor actor;
	gint64 org;
	const gchar *requested_org = htmx_request_get_form_value(request, "organization_id");
	HtmxResponse *gate;
	const gchar *format = htmx_request_get_form_value(request, "format");
	(void)params;
	gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_ADMIN);
	if (gate != NULL)
		return gate;
	gate = venture_web_require_module_api(self, "backup");
	if (gate != NULL)
		return gate;
	org = requested_org != NULL ? g_ascii_strtoll(requested_org, NULL, 10) :
		venture_web_active_organization(self, request);
	if (org <= 0)
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Choose an organization to export");
		return venture_web_error_response(error);
	}
	principal = venture_auth_authenticate(self->auth, request);
	venture_auth_to_actor(principal, &actor);
	result = venture_backup_service_export(venture_backup_service_get(venture_context_get_database(self->context)),
		org, format ? format : "json", &actor, &error);
	if (result == NULL)
		return venture_web_error_response(error);
	if (htmx_request_get_form_value(request, "format") != NULL)
	{
		g_autofree gchar *path = g_strdup_printf("/e/accounting_backup/%" G_GINT64_FORMAT, venture_entity_get_id(result));
		return venture_web_redirect_to(path);
	}
	return venture_web_json_response(venture_serializable_to_json(VENTURE_SERIALIZABLE(result), FALSE), 200);
}

static void
venture_backup_append_actions(GString *content, VentureEntity *record)
{
	if (!VENTURE_IS_ACCOUNTING_BACKUP(record) && g_strcmp0(venture_entity_get_entity_name(record), "organization") != 0)
		return;
	g_string_append_printf(content, "<section class=\"card\"><h2>Accounting pack</h2>"
		"<form method=\"post\" action=\"/backup/export\"><input type=\"hidden\" name=\"organization_id\" value=\"%" G_GINT64_FORMAT "\">"
		"<label>Format<select name=\"format\"><option>json</option><option>csv</option></select></label>"
		"<button class=\"btn btn-primary\" type=\"submit\">Export pack</button></form></section>",
		VENTURE_IS_ACCOUNTING_BACKUP(record) ? venture_entity_get_organization_id(record) : venture_entity_get_id(record));
}

/* Settings > Backups: every schedule and run of the active organization,
 * with sizes and verification verdicts. Anyone signed in may read it; only
 * an owner or admin sees the buttons, and the POST behind them carries the
 * same gate, so an accountant or a read-only user cannot trigger a run. */
static void
venture_backup_web_append_cell(GString *content, const gchar *value)
{
	g_string_append(content, "<td>");
	venture_html_escape_append(content, value != NULL ? value : "");
	g_string_append(content, "</td>");
}

static void
venture_backup_web_append_time(GString *content, VentureEntity *record, const gchar *field)
{
	g_autoptr(GDateTime) when = NULL;
	g_autofree gchar *shown = NULL;
	g_object_get(record, field, &when, NULL);
	shown = when != NULL ? venture_time_to_display_string(when, NULL) : NULL;
	venture_backup_web_append_cell(content, shown);
}

static HtmxResponse *
venture_backup_web_settings(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	VentureWebServer *self = user_data;
	VentureDatabase *database = venture_context_get_database(self->context);
	VentureBackupScheduleService *service = venture_backup_schedule_service_get(database);
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GPtrArray) schedules = NULL, runs = NULL;
	g_autoptr(VentureQuery) schedule_query = NULL, run_query = NULL;
	g_autoptr(GString) content = g_string_new(NULL);
	HtmxResponse *gate;
	VentureActor actor;
	gboolean may_trigger;
	gint64 org;
	guint i;
	(void)params;
	gate = venture_web_ui_require_session(self, request);
	if (gate != NULL)
		return gate;
	gate = venture_web_require_module_ui(self, request, "backup");
	if (gate != NULL)
		return gate;
	principal = venture_auth_authenticate(self->auth, request);
	venture_auth_to_actor(principal, &actor);
	may_trigger = venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_ADMIN, NULL);
	org = venture_web_active_organization(self, request);
	if (htmx_request_get_method(request) == HTMX_METHOD_POST)
	{
		const gchar *action = htmx_request_get_form_value(request, "action");
		const gchar *schedule_id = htmx_request_get_form_value(request, "schedule_id");
		const gchar *run_id = htmx_request_get_form_value(request, "run_id");
		g_autoptr(VentureEntity) subject = NULL;
		g_autoptr(VentureEntity) produced = NULL;
		if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_ADMIN, &error))
			return venture_web_error_response(error);
		if (g_strcmp0(action, "run") == 0)
		{
			subject = venture_database_get(database, VENTURE_TYPE_BACKUP_SCHEDULE,
				schedule_id != NULL ? g_ascii_strtoll(schedule_id, NULL, 10) : 0, &error);
			if (subject == NULL)
				return venture_web_error_response(error);
			/* The page lists only the active organization's schedules, but the
			 * id comes from the form: an admin of one organization must not be
			 * able to snapshot another's by posting its number. */
			if (venture_entity_get_organization_id(subject) != org)
			{
				g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
					"Backup schedule is not in the active organization");
				return venture_web_error_response(error);
			}
			produced = venture_backup_schedule_service_run(service, VENTURE_BACKUP_SCHEDULE(subject), NULL, &actor, &error);
			if (produced == NULL)
				return venture_web_error_response(error);
		}
		else if (g_strcmp0(action, "verify") == 0 || g_strcmp0(action, "drill") == 0)
		{
			subject = venture_database_get(database, VENTURE_TYPE_BACKUP_RUN,
				run_id != NULL ? g_ascii_strtoll(run_id, NULL, 10) : 0, &error);
			if (subject == NULL)
				return venture_web_error_response(error);
			if (venture_entity_get_organization_id(subject) != org)
			{
				g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
					"Backup run is not in the active organization");
				return venture_web_error_response(error);
			}
			if (g_strcmp0(action, "verify") == 0)
			{
				if (!venture_backup_schedule_service_verify(service, VENTURE_BACKUP_RUN(subject), &actor, &error))
					return venture_web_error_response(error);
			}
			else
			{
				produced = venture_backup_schedule_service_restore_drill(service, VENTURE_BACKUP_RUN(subject),
					htmx_request_get_form_value(request, "name"), &actor, &error);
				if (produced == NULL)
					return venture_web_error_response(error);
			}
		}
		else
		{
			g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Choose run, verify or drill");
			return venture_web_error_response(error);
		}
		return venture_web_redirect_to("/settings/backups");
	}
	schedule_query = venture_query_new(VENTURE_TYPE_BACKUP_SCHEDULE);
	venture_query_set_organization(schedule_query, org);
	venture_query_set_limit(schedule_query, 0);
	venture_query_add_order(schedule_query, "id", VENTURE_SORT_ASCENDING, NULL);
	schedules = venture_database_find(database, schedule_query, &error);
	if (schedules == NULL)
		return venture_web_error_response(error);
	run_query = venture_query_new(VENTURE_TYPE_BACKUP_RUN);
	venture_query_set_organization(run_query, org);
	venture_query_set_limit(run_query, 200);
	venture_query_add_order(run_query, "id", VENTURE_SORT_DESCENDING, NULL);
	runs = venture_database_find(database, run_query, &error);
	if (runs == NULL)
		return venture_web_error_response(error);
	g_string_append(content, "<h1>Backups</h1>"
		"<p>Scheduled snapshots of this organization and copies of the whole installation, what they weighed, "
		"and whether each one restored into an empty database and tied. Off-site copying attaches to the path.</p>"
		"<h2>Schedules</h2><table><thead><tr><th>Name</th><th>Scope</th><th>Schedule</th><th>Retention</th>"
		"<th>Verify</th><th>Last run</th><th></th></tr></thead><tbody>");
	for (i = 0; i < schedules->len; i++)
	{
		VentureEntity *schedule = g_ptr_array_index(schedules, i);
		g_autofree gchar *name = NULL, *scope = NULL, *expression = NULL, *retention = NULL;
		gint64 keep = 0;
		gboolean verify = FALSE;
		g_object_get(schedule, "name", &name, "scope", &scope, "schedule", &expression, "retention", &keep, "verify", &verify, NULL);
		retention = keep > 0 ? g_strdup_printf("%" G_GINT64_FORMAT, keep) : g_strdup("default");
		g_string_append(content, "<tr>");
		venture_backup_web_append_cell(content, name);
		venture_backup_web_append_cell(content, scope);
		venture_backup_web_append_cell(content, venture_string_is_empty(expression) ? "manual" : expression);
		venture_backup_web_append_cell(content, retention);
		venture_backup_web_append_cell(content, verify ? "yes" : "no");
		venture_backup_web_append_time(content, schedule, "last-run-at");
		g_string_append(content, "<td>");
		if (may_trigger)
			g_string_append_printf(content, "<form method=\"post\" action=\"/settings/backups\">"
				"<input type=\"hidden\" name=\"action\" value=\"run\">"
				"<input type=\"hidden\" name=\"schedule_id\" value=\"%" G_GINT64_FORMAT "\">"
				"<button class=\"btn\" type=\"submit\">Run now</button></form>", venture_entity_get_id(schedule));
		g_string_append(content, "</td></tr>");
	}
	g_string_append(content, "</tbody></table>"
		"<h2>Runs</h2><table><thead><tr><th>Run</th><th>Kind</th><th>Name</th><th>Scope</th><th>Started</th>"
		"<th>Status</th><th>Size</th><th>SHA-256</th><th>Verified</th><th>Verification</th><th>Message</th><th></th></tr></thead><tbody>");
	for (i = 0; i < runs->len; i++)
	{
		VentureEntity *run = g_ptr_array_index(runs, i);
		g_autofree gchar *kind = NULL, *name = NULL, *scope = NULL, *status = NULL, *sha = NULL, *verification = NULL, *message = NULL;
		g_autofree gchar *id = g_strdup_printf("%" G_GINT64_FORMAT, venture_entity_get_id(run));
		g_autofree gchar *size = NULL;
		gint64 bytes = 0;
		g_object_get(run, "kind", &kind, "name", &name, "scope", &scope, "status", &status, "size", &bytes,
			"sha256", &sha, "verification", &verification, "message", &message, NULL);
		size = g_format_size(bytes);
		g_string_append(content, "<tr>");
		venture_backup_web_append_cell(content, id);
		venture_backup_web_append_cell(content, kind);
		venture_backup_web_append_cell(content, name);
		venture_backup_web_append_cell(content, scope);
		venture_backup_web_append_time(content, run, "started-at");
		venture_backup_web_append_cell(content, status);
		venture_backup_web_append_cell(content, size);
		g_string_append(content, "<td><code>");
		venture_html_escape_append(content, sha != NULL ? sha : "");
		g_string_append(content, "</code></td>");
		venture_backup_web_append_time(content, run, "verified-at");
		venture_backup_web_append_cell(content, venture_string_is_empty(verification) ? "never" : verification);
		venture_backup_web_append_cell(content, message);
		g_string_append(content, "<td>");
		if (may_trigger && g_strcmp0(kind, "backup") == 0 && g_strcmp0(status, "succeeded") == 0)
			g_string_append_printf(content, "<form method=\"post\" action=\"/settings/backups\">"
				"<input type=\"hidden\" name=\"action\" value=\"verify\">"
				"<input type=\"hidden\" name=\"run_id\" value=\"%s\">"
				"<button class=\"btn\" type=\"submit\">Verify</button></form>"
				"<form method=\"post\" action=\"/settings/backups\">"
				"<input type=\"hidden\" name=\"action\" value=\"drill\">"
				"<input type=\"hidden\" name=\"run_id\" value=\"%s\">"
				"<input name=\"name\" placeholder=\"Drill name\">"
				"<button class=\"btn\" type=\"submit\">Restore drill</button></form>", id, id);
		g_string_append(content, "</td></tr>");
	}
	g_string_append(content, "</tbody></table>");
	return venture_web_html_response(venture_web_page(self, request, "/settings/backups", "Backups", content->str), 200);
}

static void
venture_backup_web_register(HtmxRouter *router, VentureWebServer *self)
{
	htmx_router_post(router, "/backup/export", venture_backup_web_export, self);
	htmx_router_post(router, "/api/v1/accounting_backups/export", venture_backup_web_export, self);
	htmx_router_get(router, "/settings/backups", venture_backup_web_settings, self);
	htmx_router_post(router, "/settings/backups", venture_backup_web_settings, self);
}
