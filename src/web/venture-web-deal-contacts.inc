/*
 * venture-web-deal-contacts.inc - The buying committee on a deal's page
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Included by venture-web-server.c. The rules -- one primary, one row a
 * person, a live deal and contact -- are the save validator's in
 * src/core/venture-deal-contact.c; this file only lays the rows out and
 * hands the two forms to the database, so a refusal here is the same one
 * the API gives.
 */

/* A link to a record by its name. */
static void
deal_contacts_web_link(GString *html, const gchar *type, VentureEntity *record)
{
	g_autofree gchar *name = venture_entity_get_display_name(record);

	g_string_append_printf(html, "<a href=\"/e/%s/%" G_GINT64_FORMAT "\">", type,
	                       venture_entity_get_id(record));
	venture_html_escape_append(html, name);
	g_string_append(html, "</a>");
}

static VentureDealContactRole
deal_contacts_web_role(VentureEntity *row)
{
	VentureDealContactRole role = VENTURE_DEAL_CONTACT_ROLE_OTHER;

	g_object_get(row, "role", &role, NULL);

	return role;
}

/* The add form: a person in the business, what they are, and whether
 * they are the one mail about the deal goes to. */
static void
deal_contacts_web_add_form(VentureWebServer *self, GString *html, VentureEntity *deal)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_CONTACT);
	g_autoptr(GPtrArray) people = NULL;
	GEnumClass *roles;
	guint i;

	venture_query_set_organization(query, venture_entity_get_organization_id(deal));
	venture_query_set_limit(query, 500);
	venture_query_add_order(query, "name", VENTURE_SORT_ASCENDING, NULL);
	people = venture_database_find(venture_context_get_database(self->context), query, NULL);

	g_string_append_printf(html, "<form method=\"post\" action=\"/deals/%" G_GINT64_FORMAT "/contacts\" "
		"class=\"inline-form deal-contact-add\"><label>Person <select name=\"contact_id\" required>"
		"<option value=\"\">Choose…</option>", venture_entity_get_id(deal));
	for (i = 0; (NULL != people) && (i < people->len); i++)
	{
		VentureEntity *person = g_ptr_array_index(people, i);
		g_autofree gchar *name = venture_entity_get_display_name(person);

		g_string_append_printf(html, "<option value=\"%" G_GINT64_FORMAT "\">", venture_entity_get_id(person));
		venture_html_escape_append(html, name);
		g_string_append(html, "</option>");
	}
	g_string_append(html, "</select></label> <label>Role <select name=\"role\">");
	roles = g_type_class_ref(VENTURE_TYPE_DEAL_CONTACT_ROLE);
	for (i = 0; i < roles->n_values; i++)
		g_string_append_printf(html, "<option value=\"%s\">%s</option>", roles->values[i].value_nick,
			venture_deal_contact_role_label(roles->values[i].value));
	g_type_class_unref(roles);
	g_string_append(html, "</select></label> <label><input type=\"checkbox\" name=\"primary\" value=\"1\"> Primary</label> "
		"<label>Note <input type=\"text\" name=\"note\" maxlength=\"500\"></label> "
		"<button class=\"btn btn-sm\" type=\"submit\">Add to deal</button></form>");
}

/* A deal's people with their roles, and the form to add one. */
static void
deal_contacts_web_deal(VentureWebServer *self, GString *html, VentureEntity *deal)
{
	VentureDatabase *database = venture_context_get_database(self->context);
	g_autoptr(GPtrArray) roles = venture_deal_contact_list(database, "deal-id", venture_entity_get_id(deal), NULL);
	guint i;

	g_string_append(html, "<section class=\"card deal-contacts\"><div class=\"card-head\"><h2>Buying committee</h2></div>"
		"<div class=\"card-body\">");
	if ((NULL == roles) || (0 == roles->len))
		g_string_append(html, "<p class=\"muted\">Nobody is on this deal yet. Add who decides, who pays and who "
			"argues for you.</p>");
	else
	{
		g_string_append(html, "<table class=\"data\"><thead><tr><th>Person</th><th>Role</th><th>Note</th><th></th>"
			"</tr></thead><tbody>");
		for (i = 0; i < roles->len; i++)
		{
			VentureEntity *row = g_ptr_array_index(roles, i);
			g_autoptr(VentureEntity) person = NULL;
			g_autofree gchar *note = NULL;
			gint64 contact_id = 0;
			gboolean primary = FALSE;

			g_object_get(row, "contact-id", &contact_id, "primary", &primary, "note", &note, NULL);
			person = venture_database_get(database, VENTURE_TYPE_CONTACT, contact_id, NULL);
			g_string_append(html, "<tr><td>");
			if (NULL != person)
				deal_contacts_web_link(html, "contact", person);
			if (primary)
				g_string_append(html, " <span class=\"badge\">Primary</span>");
			g_string_append_printf(html, "</td><td>%s</td><td>",
				venture_deal_contact_role_label(deal_contacts_web_role(row)));
			if (NULL != note)
				venture_html_escape_append(html, note);
			g_string_append_printf(html, "</td><td><form method=\"post\" action=\"/deals/%" G_GINT64_FORMAT
				"/contacts/%" G_GINT64_FORMAT "/remove\"><button class=\"btn btn-sm btn-ghost\" type=\"submit\">"
				"Remove</button></form></td></tr>", venture_entity_get_id(deal), venture_entity_get_id(row));
		}
		g_string_append(html, "</tbody></table>");
	}
	deal_contacts_web_add_form(self, html, deal);
	g_string_append(html, "</div></section>");
}

/* The deals a person is on, and as what. Nothing when they are on none. */
static void
deal_contacts_web_contact(VentureWebServer *self, GString *html, VentureEntity *contact)
{
	VentureDatabase *database = venture_context_get_database(self->context);
	g_autoptr(GPtrArray) roles = venture_deal_contact_list(database, "contact-id", venture_entity_get_id(contact), NULL);
	guint i;

	if ((NULL == roles) || (0 == roles->len))
		return;

	g_string_append(html, "<section class=\"card deal-contacts\"><div class=\"card-head\"><h2>On deals</h2></div>"
		"<div class=\"card-body\"><ul class=\"related\">");
	for (i = 0; i < roles->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(roles, i);
		g_autoptr(VentureEntity) deal = NULL;
		gint64 deal_id = 0;
		gboolean primary = FALSE;

		g_object_get(row, "deal-id", &deal_id, "primary", &primary, NULL);
		deal = venture_database_get(database, VENTURE_TYPE_DEAL, deal_id, NULL);
		if ((NULL == deal) || venture_entity_is_deleted(deal))
			continue;
		g_string_append(html, "<li>");
		deal_contacts_web_link(html, "deal", deal);
		g_string_append_printf(html, " <span class=\"muted\">%s%s</span></li>",
			venture_deal_contact_role_label(deal_contacts_web_role(row)), primary ? ", primary contact" : "");
	}
	g_string_append(html, "</ul></div></section>");
}

static void
venture_web_append_deal_contacts(VentureWebServer *self, GString *html, VentureEntity *record)
{
	if (venture_entity_get_id(record) <= 0 || venture_entity_is_deleted(record) ||
	    !venture_web_module_enabled(self, "crm"))
		return;

	if (VENTURE_IS_DEAL(record))
		deal_contacts_web_deal(self, html, record);
	else if (VENTURE_IS_CONTACT(record))
		deal_contacts_web_contact(self, html, record);
}

/* Both forms: an editor, signed in, with the deal in reach. */
static HtmxResponse *
deal_contacts_web_gate(VentureWebServer *self, HtmxRequest *request, GHashTable *params,
	VentureEntity **deal, VentureActor *actor)
{
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GError) error = NULL;
	HtmxResponse *gate;

	gate = venture_web_ui_require_session(self, request);
	if (NULL == gate)
		gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_EDITOR);
	if (NULL == gate)
		gate = venture_web_require_module_api(self, "crm");
	if (NULL != gate)
		return gate;

	*deal = venture_database_get(venture_context_get_database(self->context), VENTURE_TYPE_DEAL,
		g_ascii_strtoll(g_hash_table_lookup(params, "id"), NULL, 10), &error);
	if ((NULL == *deal) || venture_entity_is_deleted(*deal))
	{
		g_clear_object(deal);
		g_clear_error(&error);
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Deal not found");
		return venture_web_error_response(error);
	}

	principal = venture_auth_authenticate(self->auth, request);
	venture_auth_to_actor(principal, actor);

	return NULL;
}

static HtmxResponse *
deal_contacts_web_back(VentureEntity *deal)
{
	g_autofree gchar *destination = g_strdup_printf("/e/deal/%" G_GINT64_FORMAT, venture_entity_get_id(deal));

	return venture_web_redirect_to(destination);
}

/* POST /deals/:id/contacts */
static HtmxResponse *
venture_web_deal_contacts_add(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	VentureWebServer *self = user_data;
	g_autoptr(VentureEntity) deal = NULL;
	g_autoptr(VentureEntity) row = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *role_text = htmx_request_get_form_value(request, "role");
	const gchar *contact_text = htmx_request_get_form_value(request, "contact_id");
	const gchar *note = htmx_request_get_form_value(request, "note");
	GEnumClass *roles;
	GEnumValue *role = NULL;
	VentureActor actor;
	HtmxResponse *gate;

	gate = deal_contacts_web_gate(self, request, params, &deal, &actor);
	if (NULL != gate)
		return gate;

	roles = g_type_class_ref(VENTURE_TYPE_DEAL_CONTACT_ROLE);
	role = g_enum_get_value_by_nick(roles, venture_string_is_empty(role_text) ? "other" : role_text);
	g_type_class_unref(roles);
	if (NULL == role)
	{
		venture_set_error_validation(&error, "role", "Choose one of the listed roles");
		return venture_web_error_response(error);
	}

	row = VENTURE_ENTITY(venture_deal_contact_new());
	g_object_set(row, "organization-id", venture_entity_get_organization_id(deal),
		"deal-id", venture_entity_get_id(deal),
		"contact-id", (NULL != contact_text) ? g_ascii_strtoll(contact_text, NULL, 10) : (gint64)0,
		"role", role->value,
		"primary", 0 == g_strcmp0(htmx_request_get_form_value(request, "primary"), "1"),
		"note", venture_string_is_empty(note) ? NULL : note, NULL);

	if (!venture_database_save(venture_context_get_database(self->context), row, &actor, &error))
		return venture_web_error_response(error);

	return deal_contacts_web_back(deal);
}

/* POST /deals/:id/contacts/:link/remove */
static HtmxResponse *
venture_web_deal_contacts_remove(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	VentureWebServer *self = user_data;
	VentureDatabase *database = venture_context_get_database(self->context);
	g_autoptr(VentureEntity) deal = NULL;
	g_autoptr(VentureEntity) row = NULL;
	g_autoptr(GError) error = NULL;
	VentureActor actor;
	HtmxResponse *gate;
	gint64 deal_id = 0;

	gate = deal_contacts_web_gate(self, request, params, &deal, &actor);
	if (NULL != gate)
		return gate;

	row = venture_database_get(database, VENTURE_TYPE_DEAL_CONTACT,
		g_ascii_strtoll(g_hash_table_lookup(params, "link"), NULL, 10), NULL);
	if (NULL != row)
		g_object_get(row, "deal-id", &deal_id, NULL);
	/* The role must be on the deal in the address, or a form for one deal
	 * could remove somebody from another. */
	if ((NULL == row) || (deal_id != venture_entity_get_id(deal)))
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "That person is not on this deal");
		return venture_web_error_response(error);
	}

	if (!venture_database_delete(database, row, &actor, &error))
		return venture_web_error_response(error);

	return deal_contacts_web_back(deal);
}

static void
venture_deal_contacts_pages_register(VentureWebServer *self)
{
	venture_web_server_add_classified_route(self, HTMX_METHOD_POST, "/deals/:id/contacts",
		VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, venture_web_deal_contacts_add, self);
	venture_web_server_add_classified_route(self, HTMX_METHOD_POST, "/deals/:id/contacts/:link/remove",
		VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, venture_web_deal_contacts_remove, self);
}
