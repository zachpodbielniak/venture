/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* A browser view, never a process-wide module switch or an access grant. */
#define VENTURE_WEB_VENTURE_COOKIE "venture_view"

static gchar *venture_web_read_cookie(HtmxRequest *request, const gchar *name);

/* Append later household destinations here; business navigation is separate. */
static const VentureWebNavLink venture_household_nav[] = {
	{ "/e/sale", "Income", VENTURE_ICON("<path d=\"M4 17l6-6 4 4 6-10\"/>"), "Money", "sales" },
	{ "/e/expense", "Spending", VENTURE_ICON("<path d=\"M4 7l6 6 4-4 6 10\"/>"), NULL, "finance" },
	{ "/e/account", "Accounts", VENTURE_ICON("<path d=\"M3 21h18M5 10v11M12 10v11M19 10v11M3 8l9-5 9 5z\"/>"), NULL, "finance" },
	{ "/bankfeed", "Bank feeds", VENTURE_ICON("<path d=\"M4 7h16M4 12h16M4 17h10\"/>"), NULL, "bankfeed" },
	{ "/e/vendor_bill", "Bills", VENTURE_ICON("<path d=\"M5 3h14v18H5zM8 8h8M8 13h8\"/>"), "Bills", "payables" },
	{ "/payables", "Pay bills", VENTURE_ICON("<path d=\"M4 12h16M14 6l6 6-6 6\"/>"), NULL, "payables" },
	{ "/budgets", "Budgets", VENTURE_ICON("<path d=\"M4 4h16v16H4zM8 8h8M8 12h6\"/>"), "Budgets", "budgets" },
	{ "/e/goal", "Goals", VENTURE_ICON("<circle cx=\"12\" cy=\"12\" r=\"8\"/><circle cx=\"12\" cy=\"12\" r=\"3\"/>"), "Goals", "goals" },
	{ "/e/document", "Documents", VENTURE_ICON("<path d=\"M5 3h14v18H5zM8 8h8M8 13h8\"/>"), "Documents", "core" },
	{ "/reports", "Reports", VENTURE_ICON("<path d=\"M4 20h16M7 20V12M12 20V4M17 20V8\"/>"), "Reports", "core" },
	{ "/e/bank_account", "Bank accounts", VENTURE_ICON("<path d=\"M4 6h16v14H4zM4 10h16\"/>"), "Money", "banking" },
	{ "/e/bank_statement", "Statements", VENTURE_ICON("<path d=\"M5 3h14v18H5z\"/>"), NULL, "banking" },
	{ "/e/bank_transaction", "Statement lines", VENTURE_ICON("<path d=\"M4 7h16M4 12h16M4 17h10\"/>"), NULL, "banking" },
	{ "/e/bank_rule", "Category rules", VENTURE_ICON("<path d=\"M4 7h16M4 12h16M4 17h10\"/>"), NULL, "banking" },
	{ "/e/bank_transfer", "Transfers", VENTURE_ICON("<path d=\"M4 8h16l-4-4M20 16H4l4 4\"/>"), NULL, "banking" },
	{ "/money/calendar", "Due calendar", VENTURE_ICON("<path d=\"M4 5h16v16H4zM4 10h16\"/>"), "Bills", "money_calendar" },
	{ "/reports/household_subscriptions", "Subscriptions", VENTURE_ICON("<path d=\"M4 7h16M4 12h16M4 17h10\"/>"), NULL, "money_calendar" },
	{ "/e/recurring_schedule", "Repeating bills", VENTURE_ICON("<path d=\"M4 7h16M4 12h16M4 17h10\"/>"), NULL, "recurring" },
	{ "/reports/net_worth", "Net worth", VENTURE_ICON("<path d=\"M4 20h16M7 20V12M12 20V4M17 20V8\"/>"), "Reports", "statements" },
	{ "/reports/debt_payoff", "Debt payoff", VENTURE_ICON("<path d=\"M4 7l6 6 4-4 6 10\"/>"), NULL, "statements" },
	{ "/reports/savings_goals", "Savings progress", VENTURE_ICON("<circle cx=\"12\" cy=\"12\" r=\"8\"/>"), "Goals", "goals" },
	{ "/e/household_share", "Settle up", VENTURE_ICON("<path d=\"M4 8h16l-4-4M20 16H4l4 4\"/>"), "Money", "banking" },
	{ "/reports/household_tax_year?period=last_year", "Year-end for taxes", VENTURE_ICON("<path d=\"M5 3h14v18H5zM8 8h8M8 13h5\"/>"), "Reports", "finance" },
	{ "/reports/household_warranties", "Warranties", VENTURE_ICON("<path d=\"M12 3l8 4v5c0 5-3.5 8-8 9-4.5-1-8-4-8-9V7z\"/>"), "Documents", "core" },
	{ NULL, NULL, NULL, NULL, NULL }
};

/* Re-read under the caller's access scope: cookies can be forged, and a
 * venture can be deleted, moved or retyped between two page requests. */
static VentureEntity *
venture_web_view_venture(VentureWebServer *self, HtmxRequest *request)
{
	g_autofree gchar *value = venture_web_read_cookie(request, VENTURE_WEB_VENTURE_COOKIE);
	g_autoptr(VentureEntity) record = NULL;
	gint64 id;
	gint64 organization = venture_web_active_organization(self, request);

	if (organization <= 0 || value == NULL ||
		!g_ascii_string_to_signed(value, 10, 1, G_MAXINT64, &id, NULL))
		return NULL;
	record = venture_database_get(venture_context_get_database(self->context), VENTURE_TYPE_VENTURE, id, NULL);
	if (record == NULL || venture_entity_is_deleted(record) ||
		venture_entity_get_organization_id(record) != organization)
		return NULL;
	return g_steal_pointer(&record);
}

static gboolean
venture_web_household_view(VentureWebServer *self, HtmxRequest *request)
{
	g_autoptr(VentureEntity) record = venture_web_view_venture(self, request);
	g_autofree gchar *kind = NULL;

	/* The accountant's restricted Books view takes precedence. */
	if (record == NULL || venture_accountant_web_navigation(self, request) != venture_web_navigation())
		return FALSE;
	g_object_get(record, "venture-type", &kind, NULL);
	return g_strcmp0(kind, "household") == 0;
}

static const gchar *
venture_web_household_home(VentureWebServer *self)
{
	gsize i;
	if (venture_web_module_enabled(self, "finance"))
		return "/e/expense";
	for (i = 0; venture_household_nav[i].path != NULL; i++)
	{
		if (venture_web_module_enabled(self, venture_household_nav[i].module))
			return venture_household_nav[i].path;
	}
	return "/reports";
}

static const VentureWebNavLink *
venture_web_household_type_link(const gchar *name)
{
	gsize i;
	for (i = 0; venture_household_nav[i].path != NULL; i++)
	{
		const gchar *path = venture_household_nav[i].path;
		if (g_str_has_prefix(path, "/e/") && g_strcmp0(path + 3, name) == 0)
			return &venture_household_nav[i];
	}
	return NULL;
}

static void
venture_web_append_household_nav(VentureWebServer *self, GString *html, const gchar *active)
{
	g_autoptr(GHashTable) sections = g_hash_table_new(g_str_hash, g_str_equal);
	gsize i;
	/* Children append destinations; display a section only once, retaining
	 * its original order even when later children add links to Money. */
	for (i = 0; venture_household_nav[i].path != NULL; i++)
	{
		const gchar *section = venture_household_nav[i].section;
		const gchar *current = NULL;
		gboolean shown = FALSE;
		gsize j;
		if (section == NULL || g_hash_table_contains(sections, section)) continue;
		g_hash_table_add(sections, (gpointer)section);
		for (j = 0; venture_household_nav[j].path != NULL; j++)
		{
			const VentureWebNavLink *link = &venture_household_nav[j];
			if (link->section != NULL) current = link->section;
			if (g_strcmp0(current, section) || !venture_web_module_enabled(self, link->module)) continue;
			if (!shown)
			{
				venture_web_append_nav_heading(html, section);
				shown = TRUE;
			}
			venture_web_append_nav_item(html, link, active);
		}
	}
}

static void
venture_web_append_venture_picker(VentureWebServer *self, HtmxRequest *request, GString *html)
{
	g_autoptr(VentureEntity) active = venture_web_view_venture(self, request);
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_VENTURE);
	g_autoptr(GPtrArray) ventures = NULL;
	gint64 organization = venture_web_active_organization(self, request);
	guint i;
	gboolean has_household = FALSE;

	if (organization <= 0) return;
	venture_query_add_filter_int(query, "organization-id", VENTURE_FILTER_OP_EQ, organization, NULL);
	venture_query_add_order(query, "name", VENTURE_SORT_ASCENDING, NULL);
	venture_query_set_limit(query, 0);
	ventures = venture_database_find(venture_context_get_database(self->context), query, NULL);
	if (ventures == NULL || ventures->len == 0) return;
	/* A business-only entity keeps exactly its ordinary selector. */
	for (i = 0; i < ventures->len; i++)
	{
		g_autofree gchar *kind = NULL;
		g_object_get(g_ptr_array_index(ventures, i), "venture-type", &kind, NULL);
		if (g_strcmp0(kind, "household") == 0)
		{
			has_household = TRUE;
			break;
		}
	}
	if (!has_household) return;

	g_string_append(html, "<div class=\"entity-picker\"><div class=\"entity-picker-label\">Venture view</div>"
		"<details class=\"entity-menu\"><summary>");
	if (active != NULL)
	{
		g_autofree gchar *name = venture_entity_get_display_name(active);
		venture_html_escape_append(html, name);
	}
	else g_string_append(html, "Full view");
	g_string_append(html, "</summary><div class=\"entity-menu-list\">"
		"<a class=\"entity-option\" href=\"/view/venture/all\">Full view</a>");
	for (i = 0; i < ventures->len; i++)
	{
		VentureEntity *record = g_ptr_array_index(ventures, i);
		g_autofree gchar *name = venture_entity_get_display_name(record);
		g_string_append_printf(html, "<a class=\"entity-option\" href=\"/view/venture/%" G_GINT64_FORMAT "\">", venture_entity_get_id(record));
		venture_html_escape_append(html, name);
		g_string_append(html, "</a>");
	}
	g_string_append(html, "</div></details></div>");
}

static HtmxResponse *
venture_web_ui_switch_venture(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	VentureWebServer *self = user_data;
	HtmxResponse *gate = venture_web_ui_require_session(self, request);
	HtmxResponse *response;
	g_autoptr(VentureEntity) record = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *cookie = NULL;
	g_autofree gchar *kind = NULL;
	const gchar *value = g_hash_table_lookup(params, "id");
	gint64 id = 0;

	if (gate != NULL) return gate;
	if (g_strcmp0(value, "all") != 0)
	{
		if (value == NULL || !g_ascii_string_to_signed(value, 10, 1, G_MAXINT64, &id, NULL))
		{
			g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT, "Choose a venture from the menu");
			return venture_web_error_response(error);
		}
		record = venture_database_get(venture_context_get_database(self->context), VENTURE_TYPE_VENTURE, id, NULL);
		if (record == NULL || venture_entity_is_deleted(record) ||
			venture_entity_get_organization_id(record) != venture_web_active_organization(self, request))
		{
			g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "That venture is not available in this entity");
			return venture_web_error_response(error);
		}
		g_object_get(record, "venture-type", &kind, NULL);
	}
	cookie = g_strdup_printf("%s=%" G_GINT64_FORMAT "; Path=/; HttpOnly; SameSite=Lax; Max-Age=%d",
		VENTURE_WEB_VENTURE_COOKIE, id, 60 * 60 * 24 * 365);
	response = venture_web_redirect_to(g_strcmp0(kind, "household") == 0 ? venture_web_household_home(self) : "/");
	htmx_response_add_header(response, "Set-Cookie", cookie);
	return response;
}

/* A participant picker discloses names, not user records or credentials.
 * Ordinary user CRUD remains installation-owner-only. */
static GPtrArray *
venture_web_household_members(VentureWebServer *self, gint64 organization)
{
	VentureDatabase *database = venture_context_get_database(self->context);
	g_autoptr(VentureEntity) org = NULL;
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) memberships = NULL;
	GPtrArray *result = g_ptr_array_new_with_free_func(g_object_unref);
	guint i;
	if (organization <= 0) return result;
	org = venture_database_get(database, VENTURE_TYPE_ORGANIZATION, organization, NULL);
	if (!org) return result;
	internal = venture_access_policy_enter(venture_database_get_access_policy(database), NULL);
	query = venture_query_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP);
	venture_query_set_organization(query, organization);
	venture_query_set_limit(query, 0);
	venture_query_add_filter_int(query, "active", VENTURE_FILTER_OP_EQ, 1, NULL);
	memberships = venture_database_find(database, query, NULL);
	for (i = 0; memberships && i < memberships->len; i++) {
		VentureEntity *member = g_ptr_array_index(memberships, i);
		g_autoptr(VentureEntity) user = NULL;
		g_autofree gchar *name = NULL;
		gint64 id = 0;
		gboolean active = FALSE;
		g_object_get(member, "user-id", &id, NULL);
		user = venture_database_get(database, VENTURE_TYPE_USER, id, NULL);
		if (!user || venture_entity_is_deleted(user)) continue;
		g_object_get(user, "active", &active, "username", &name, NULL);
		if (active) g_ptr_array_add(result, g_object_new(VENTURE_TYPE_USER, "id", id, "username", name, NULL));
	}
	return result;
}

static gchar *
venture_web_household_member_name(VentureWebServer *self, gint64 organization, gint64 user_id)
{
	g_autoptr(GPtrArray) members = venture_web_household_members(self, organization);
	guint i;
	for (i = 0; i < members->len; i++) {
		VentureEntity *user = g_ptr_array_index(members, i);
		if (venture_entity_get_id(user) == user_id) {
			gchar *name = NULL;
			g_object_get(user, "username", &name, NULL);
			return name;
		}
	}
	return g_strdup("Former household member");
}
