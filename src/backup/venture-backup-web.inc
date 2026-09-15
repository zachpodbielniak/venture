/* SPDX-License-Identifier: AGPL-3.0-or-later */
static HtmxResponse *
venture_backup_web_export(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) result = NULL;
	g_autoptr(GError) error = NULL;
	VentureActor actor;
	HtmxResponse *gate;
	const gchar *format = htmx_request_get_form_value(request, "format");
	(void)params;
	gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_ADMIN);
	if (gate != NULL)
		return gate;
	principal = venture_auth_authenticate(self->auth, request);
	venture_auth_to_actor(principal, &actor);
	result = venture_backup_service_export(venture_backup_service_get(venture_context_get_database(self->context)),
		venture_context_get_default_organization_id(self->context), format ? format : "json", &actor, &error);
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
	g_string_append(content, "<section class=\"card\"><h2>Accounting backup</h2>"
		"<form method=\"post\" action=\"/backup/export\"><label>Format<select name=\"format\"><option>json</option><option>csv</option></select></label>"
		"<button class=\"btn btn-primary\" type=\"submit\">Export pack</button></form></section>");
}

static void
venture_backup_web_register(HtmxRouter *router, VentureWebServer *self)
{
	htmx_router_post(router, "/backup/export", venture_backup_web_export, self);
	htmx_router_post(router, "/api/v1/accounting_backups/export", venture_backup_web_export, self);
}
