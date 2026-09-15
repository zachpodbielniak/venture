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

static void
venture_backup_web_register(HtmxRouter *router, VentureWebServer *self)
{
	htmx_router_post(router, "/backup/export", venture_backup_web_export, self);
	htmx_router_post(router, "/api/v1/accounting_backups/export", venture_backup_web_export, self);
}
