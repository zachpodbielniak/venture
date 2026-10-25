/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * The people at a company, on the company's page: each person's name and
 * what they do there, with their email and phone as links, and a way to
 * add one already filed under the company. Customers and suppliers alike:
 * whom to call is the first question either way.
 *
 * Included into venture-web-server.c.
 */

static void
venture_company_people_web_append_block(VentureWebServer *self, GString *html, VentureEntity *company)
{
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) people = NULL;
	gint64 id = venture_entity_get_id(company);
	guint i;

	if (!VENTURE_IS_COMPANY(company) || (id <= 0) || !venture_web_module_enabled(self, "crm"))
		return;
	query = venture_query_new(VENTURE_TYPE_CONTACT);
	venture_query_set_organization(query, venture_entity_get_organization_id(company));
	venture_query_add_filter_int(query, "company-id", VENTURE_FILTER_OP_EQ, id, NULL);
	venture_query_add_order(query, "name", VENTURE_SORT_ASCENDING, NULL);
	venture_query_set_limit(query, 0);
	people = venture_database_find(venture_context_get_database(self->context), query, NULL);

	g_string_append_printf(html, "<section class=\"card company-people\"><div class=\"card-head\"><h2>People</h2>"
		"<a class=\"btn btn-sm\" href=\"/e/contact/new?company_id=%" G_GINT64_FORMAT "\">Add a person</a></div>"
		"<div class=\"card-body\">", id);
	if ((NULL == people) || (0 == people->len))
	{
		g_string_append(html, "<p class=\"muted\">Nobody here yet.</p></div></section>");
		return;
	}
	g_string_append(html, "<ul class=\"people\">");
	for (i = 0; i < people->len; i++)
	{
		VentureEntity *person = g_ptr_array_index(people, i);
		g_autofree gchar *name = NULL;
		g_autofree gchar *role = NULL;
		g_autofree gchar *email = NULL;
		g_autofree gchar *phone = NULL;

		g_object_get(person, "name", &name, "role", &role, "email", &email, "phone", &phone, NULL);
		g_string_append_printf(html, "<li><a class=\"person\" href=\"/e/contact/%" G_GINT64_FORMAT "\">",
			venture_entity_get_id(person));
		venture_html_escape_append(html, name);
		g_string_append(html, "</a>");
		if (!venture_string_is_empty(role))
		{
			g_string_append(html, " <span class=\"muted\">");
			venture_html_escape_append(html, role);
			g_string_append(html, "</span>");
		}
		if (!venture_string_is_empty(email))
		{
			g_string_append(html, " <a href=\"mailto:");
			venture_html_escape_append(html, email);
			g_string_append(html, "\">");
			venture_html_escape_append(html, email);
			g_string_append(html, "</a>");
		}
		if (!venture_string_is_empty(phone))
		{
			g_string_append(html, " <a href=\"tel:");
			venture_html_escape_append(html, phone);
			g_string_append(html, "\">");
			venture_html_escape_append(html, phone);
			g_string_append(html, "</a>");
		}
		g_string_append(html, "</li>");
	}
	g_string_append(html, "</ul></div></section>");
}
