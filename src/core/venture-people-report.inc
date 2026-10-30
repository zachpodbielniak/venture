/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Current contacts and customer companies without a matching person. */

static gchar *
people_email_key(const gchar *email)
{
	g_autofree gchar *trimmed = NULL;

	if (email == NULL)
		return NULL;
	trimmed = g_strdup(email);
	g_strstrip(trimmed);
	if (*trimmed == '\0')
		return NULL;
	return g_ascii_strdown(trimmed, -1);
}

static void
people_add_row(VentureReportResult *result, VentureEntity *entity,
	const gchar *type, const gchar *source)
{
	g_autofree gchar *name = NULL;
	g_autofree gchar *email = NULL;
	g_autofree gchar *phone = NULL;

	g_object_get(entity, "name", &name, "email", &email, "phone", &phone, NULL);
	venture_report_result_begin_row(result);
	venture_report_result_set_integer(result, "id", venture_entity_get_id(entity));
	venture_report_result_set_text(result, "type", type);
	venture_report_result_set_text(result, "name", name != NULL ? name : "");
	venture_report_result_set_text(result, "email", email != NULL ? email : "");
	venture_report_result_set_text(result, "phone", phone != NULL ? phone : "");
	venture_report_result_set_text(result, "source", source);
}

static VentureReportResult *
people_report(VentureContext *context, VentureDateRange *period,
	JsonObject *options, GError **error)
{
	VentureDatabase *database = venture_context_get_database(context);
	g_autoptr(VentureQuery) contacts_query = NULL;
	g_autoptr(VentureQuery) companies_query = NULL;
	g_autoptr(GPtrArray) contacts = NULL;
	g_autoptr(GPtrArray) companies = NULL;
	g_autoptr(GHashTable) linked = NULL;
	g_autoptr(GHashTable) emails = NULL;
	g_autoptr(VentureReportResult) result = NULL;
	gint64 organization;
	guint i;

	(void)period;
	organization = options != NULL ? venture_json_object_get_int(options, "organization_id", 0) : 0;
	if (organization == 0)
		organization = venture_context_get_default_organization_id(context);
	contacts_query = venture_query_new(VENTURE_TYPE_CONTACT);
	venture_query_set_organization(contacts_query, organization);
	venture_query_set_limit(contacts_query, 0);
	if (!venture_query_add_order(contacts_query, "id", VENTURE_SORT_ASCENDING, error))
		return NULL;
	contacts = venture_database_find(database, contacts_query, error);
	if (contacts == NULL)
		return NULL;
	companies_query = venture_query_new(VENTURE_TYPE_COMPANY);
	venture_query_set_organization(companies_query, organization);
	venture_query_set_limit(companies_query, 0);
	if (!venture_query_add_order(companies_query, "id", VENTURE_SORT_ASCENDING, error))
		return NULL;
	companies = venture_database_find(database, companies_query, error);
	if (companies == NULL)
		return NULL;
	linked = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	emails = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	result = venture_report_result_new("People", NULL);
	venture_report_result_add_column(result, "id", "ID", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "type", "Type", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "name", "Name", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "email", "Email", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "phone", "Phone", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "source", "Source", VENTURE_REPORT_COLUMN_TEXT);
	for (i = 0; i < contacts->len; i++)
	{
		VentureEntity *contact = g_ptr_array_index(contacts, i);
		g_autofree gchar *email = NULL;
		g_autofree gchar *key = NULL;
		gint64 company_id = 0;

		g_object_get(contact, "company-id", &company_id, "email", &email, NULL);
		if (company_id > 0)
		{
			gint64 *id = g_new(gint64, 1);
			*id = company_id;
			g_hash_table_add(linked, id);
		}
		key = people_email_key(email);
		if (key != NULL)
			g_hash_table_add(emails, g_steal_pointer(&key));
		people_add_row(result, contact, "contact", "contact");
	}
	for (i = 0; i < companies->len; i++)
	{
		VentureEntity *company = g_ptr_array_index(companies, i);
		g_autofree gchar *email = NULL;
		g_autofree gchar *key = NULL;
		gint kind;
		gint64 id;

		g_object_get(company, "kind", &kind, "email", &email, NULL);
		if (kind != VENTURE_COMPANY_KIND_CUSTOMER)
			continue;
		id = venture_entity_get_id(company);
		key = people_email_key(email);
		if (g_hash_table_contains(linked, &id) ||
			(key != NULL && g_hash_table_contains(emails, key)))
			continue;
		people_add_row(result, company, "company", "customer");
	}
	return g_steal_pointer(&result);
}
