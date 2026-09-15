/* SPDX-License-Identifier: AGPL-3.0-or-later */
static HtmxResponse *
venture_custom_fields_web(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	VentureWebServer *self = user_data;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GPtrArray) fields = NULL;
	g_autoptr(GPtrArray) layouts = NULL;
	g_autoptr(VentureQuery) field_query = NULL;
	g_autoptr(VentureQuery) layout_query = NULL;
	g_autoptr(GString) content = g_string_new(NULL);
	HtmxResponse *gate;
	VentureActor actor;
	gint64 org;
	guint i;
	(void)params;
	gate = venture_web_ui_require_session(self, request);
	if (gate != NULL)
		return gate;
	gate = venture_web_require_module_ui(self, request, "custom_fields");
	if (gate != NULL)
		return gate;
	principal = venture_auth_authenticate(self->auth, request);
	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_EDITOR, &error))
		return venture_web_error_response(error);
	venture_auth_to_actor(principal, &actor);
	org = venture_web_active_organization(self, request);
	if (htmx_request_get_method(request) == HTMX_METHOD_POST)
	{
		const gchar *action = htmx_request_get_form_value(request, "action");
		const gchar *record_type = htmx_request_get_form_value(request, "record_type");
		if (g_strcmp0(action, "layout") == 0)
		{
			const gchar *order = htmx_request_get_form_value(request, "field_order");
			g_autoptr(VentureEntity) layout = venture_custom_fields_service_set_layout(
				venture_custom_fields_service_get(venture_context_get_database(self->context)),
				org, record_type, order, &actor, &error);
			if (layout == NULL)
				return venture_web_error_response(error);
		}
		else
		{
			const gchar *name = htmx_request_get_form_value(request, "name");
			const gchar *kind = htmx_request_get_form_value(request, "kind");
			const gchar *required = htmx_request_get_form_value(request, "required");
			const gchar *options = htmx_request_get_form_value(request, "options");
			g_autoptr(VentureEntity) field = venture_custom_fields_service_define(
				venture_custom_fields_service_get(venture_context_get_database(self->context)),
				org, record_type, name, kind, g_strcmp0(required, "1") == 0 || g_strcmp0(required, "true") == 0,
				options, &actor, &error);
			if (field == NULL)
				return venture_web_error_response(error);
		}
		return venture_web_redirect_to("/settings/fields");
	}
	field_query = venture_query_new(VENTURE_TYPE_ACCOUNTING_CUSTOM_FIELD);
	venture_query_set_organization(field_query, org);
	venture_query_set_limit(field_query, 0);
	fields = venture_database_find(venture_context_get_database(self->context), field_query, &error);
	if (fields == NULL)
		return venture_web_error_response(error);
	layout_query = venture_query_new(VENTURE_TYPE_ACCOUNTING_LAYOUT);
	venture_query_set_organization(layout_query, org);
	venture_query_set_limit(layout_query, 0);
	layouts = venture_database_find(venture_context_get_database(self->context), layout_query, NULL);
	g_string_append(content, "<h1>Custom fields</h1>"
		"<p>Attach fields to any record type without writing C. Required values are checked on save.</p>"
		"<h2>Defined fields</h2><table><thead><tr><th>Record</th><th>Name</th><th>Kind</th><th>Required</th></tr></thead><tbody>");
	for (i = 0; i < fields->len; i++)
	{
		VentureEntity *field = g_ptr_array_index(fields, i);
		g_autofree gchar *record_type = NULL;
		g_autofree gchar *name = NULL;
		g_autofree gchar *kind = NULL;
		gboolean required = FALSE;
		g_object_get(field, "record-type", &record_type, "name", &name, "kind", &kind, "required", &required, NULL);
		g_string_append(content, "<tr><td>");
		venture_html_escape_append(content, record_type);
		g_string_append(content, "</td><td>");
		venture_html_escape_append(content, name);
		g_string_append(content, "</td><td>");
		venture_html_escape_append(content, kind);
		g_string_append(content, required ? "</td><td>yes</td></tr>" : "</td><td>no</td></tr>");
	}
	g_string_append(content, "</tbody></table>"
		"<h2>Add a field</h2>"
		"<form method=\"post\">"
		"<input type=\"hidden\" name=\"action\" value=\"field\">"
		"<label>Record type <input name=\"record_type\" required></label> "
		"<label>Name <input name=\"name\" required></label> "
		"<label>Kind <input name=\"kind\" value=\"string\" required></label> "
		"<label>Required <input type=\"checkbox\" name=\"required\" value=\"1\"></label> "
		"<label>Options JSON <input name=\"options\" value=\"[]\"></label> "
		"<button>Save field</button></form>"
		"<h2>Layouts</h2>");
	if (layouts != NULL)
	{
		g_string_append(content, "<table><thead><tr><th>Record</th><th>Order</th></tr></thead><tbody>");
		for (i = 0; i < layouts->len; i++)
		{
			VentureEntity *layout = g_ptr_array_index(layouts, i);
			g_autofree gchar *record_type = NULL;
			g_autofree gchar *order = NULL;
			g_object_get(layout, "record-type", &record_type, "field-order", &order, NULL);
			g_string_append(content, "<tr><td>");
			venture_html_escape_append(content, record_type);
			g_string_append(content, "</td><td><code>");
			venture_html_escape_append(content, order);
			g_string_append(content, "</code></td></tr>");
		}
		g_string_append(content, "</tbody></table>");
	}
	g_string_append(content, "<form method=\"post\">"
		"<input type=\"hidden\" name=\"action\" value=\"layout\">"
		"<label>Record type <input name=\"record_type\" required></label> "
		"<label>Field order JSON <input name=\"field_order\" value=\"[]\" required></label> "
		"<button>Save layout</button></form>");
	return venture_web_html_response(venture_web_page(self, request, "/settings/fields", "Custom fields", content->str), 200);
}

static void
venture_custom_fields_web_register(HtmxRouter *router, VentureWebServer *self)
{
	htmx_router_get(router, "/settings/fields", venture_custom_fields_web, self);
	htmx_router_post(router, "/settings/fields", venture_custom_fields_web, self);
}
