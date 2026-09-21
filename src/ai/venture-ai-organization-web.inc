/* SPDX-License-Identifier: AGPL-3.0-or-later */
static GPtrArray *venture_web_ai_organizations(VentureWebServer *self)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ORGANIZATION);
	venture_query_set_limit(query, 0);
	venture_query_add_filter_int(query, "active", VENTURE_FILTER_OP_EQ, TRUE, NULL);
	return venture_database_find(venture_context_get_database(self->context), query, NULL);
}
static gint64 venture_web_ai_organization(VentureWebServer *self, const gchar *value, GError **error)
{
	g_autoptr(GPtrArray) organizations = venture_web_ai_organizations(self);
	gint64 selected = 0;
	guint i;
	if (!venture_string_is_empty(value) && !g_ascii_string_to_signed(value, 10, 1, G_MAXINT64, &selected, NULL)) goto invalid;
	if (!selected && organizations && organizations->len == 1)
		selected = venture_entity_get_id(g_ptr_array_index(organizations, 0));
	for (i = 0; organizations && i < organizations->len; i++)
		if (venture_entity_get_id(g_ptr_array_index(organizations, i)) == selected) return selected;
invalid:
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED, "Select an organization you may access before starting a conversation");
	return 0;
}
static void venture_web_ai_append_organization(VentureWebServer *self, GString *html, const gchar *id, const gchar *label)
{
	g_autoptr(GPtrArray) organizations = venture_web_ai_organizations(self);
	guint i;
	g_string_append(html, "<label>"); venture_html_escape_append(html, label);
	g_string_append(html, "<select id=\""); venture_html_escape_append(html, id);
	g_string_append(html, "\" name=\"organization_id\">");
	if (!organizations || organizations->len != 1) g_string_append(html, "<option value=\"\">Select an organization</option>");
	for (i = 0; organizations && i < organizations->len; i++) {
		VentureEntity *organization = g_ptr_array_index(organizations, i);
		g_autofree gchar *name = venture_entity_get_display_name(organization);
		g_string_append_printf(html, "<option value=\"%" G_GINT64_FORMAT "\">", venture_entity_get_id(organization));
		venture_html_escape_append(html, name); g_string_append(html, "</option>");
	}
	g_string_append(html, "</select></label>");
	if (!strcmp(id, "chat-organization")) g_string_append(html, "<small>Resumed conversations keep their original organization.</small>");
}
