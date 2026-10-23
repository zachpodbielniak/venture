/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* The computed steps as a list a person reads: done or still to do, the
 * step, and what was found. Text that does not parse is shown as it is. */
static void
venture_setup_append_checklist(GString *content, const gchar *checklist)
{
	g_autoptr(JsonParser) parser = json_parser_new();
	JsonNode *root;
	JsonArray *steps;
	guint i;
	if (!json_parser_load_from_data(parser, checklist, -1, NULL) ||
	    (root = json_parser_get_root(parser)) == NULL || !JSON_NODE_HOLDS_ARRAY(root))
	{
		g_string_append(content, "<pre class=\"muted\">");
		venture_html_escape_append(content, checklist);
		g_string_append(content, "</pre>");
		return;
	}
	steps = json_node_get_array(root);
	g_string_append(content, "<ul class=\"setup-checklist\">");
	for (i = 0; i < json_array_get_length(steps); i++)
	{
		JsonObject *step = json_array_get_object_element(steps, i);
		const gchar *label, *detail;
		gboolean done;
		if (step == NULL)
			continue;
		label = json_object_get_string_member_with_default(step, "label", "");
		detail = json_object_get_string_member_with_default(step, "detail", "");
		done = json_object_get_boolean_member_with_default(step, "done", FALSE);
		g_string_append_printf(content, "<li class=\"%s\"><span aria-label=\"%s\">%s</span> <strong>",
			done ? "done" : "todo", done ? "Done" : "To do", done ? "\xe2\x9c\x93" : "\xe2\x97\x8b");
		venture_html_escape_append(content, label);
		g_string_append(content, "</strong>");
		if (detail[0] != '\0')
		{
			g_string_append(content, " \xc2\xb7 ");
			venture_html_escape_append(content, detail);
		}
		g_string_append(content, "</li>");
	}
	g_string_append(content, "</ul>");
}

static void
venture_setup_append_actions(GString *content, VentureEntity *record, VentureDatabase *database)
{
	const gchar *name = venture_entity_get_entity_name(record);
	g_autofree gchar *checklist = NULL;
	g_autofree gchar *book = NULL;
	g_autofree gchar *book_html = NULL;
	if (strcmp(name, "accounting_setup") != 0)
		return;
	g_object_get(record, "checklist", &checklist, NULL);
	/* Offered as the organization already keeps it, not as "USD": a euro
	 * organization accepting the form's default would re-denominate its
	 * books. */
	book = venture_database_get_book_currency(database, venture_entity_get_organization_id(record));
	book_html = g_markup_escape_text(book, -1);
	g_string_append_printf(content,
		"<section class=\"card\"><h2>Accounting setup</h2>"
		"<p>Resumable checklist for the legal entity, book currency, fiscal year, "
		"basis, tax profile, chart, banks, openings and control accounts.</p>"
		"<form method=\"post\" action=\"/setup/preview\">"
		"<label>Legal name<input name=\"legal_name\" required></label>"
		"<label>Book currency<input name=\"book_currency\" value=\"%s\" required></label>"
		"<label>Fiscal year start<input name=\"fiscal_year_start\" value=\"2026-01-01\" required></label>"
		"<label>Basis<select name=\"basis\"><option>accrual</option><option>cash</option></select></label>"
		"<label>Tax profile<input name=\"tax_profile\" value=\"standard\"></label>"
		"<label>Chart template<input name=\"chart_template\" value=\"standard\"></label>"
		"<label>Bank name<input name=\"bank_name\" value=\"Operating account\"></label>"
		"<label>Opening cash<input name=\"opening_cash\" value=\"0 %s\"></label>"
		"<button class=\"btn btn-primary\" type=\"submit\">Preview</button></form>"
		"<form method=\"post\" action=\"/setup/%" G_GINT64_FORMAT "/action\">"
		"<input type=\"hidden\" name=\"action\" value=\"complete\">"
		"<button class=\"btn\" type=\"submit\">Complete setup</button></form>",
		book_html, book_html, venture_entity_get_id(record));
	if (checklist != NULL && checklist[0] != '\0')
		venture_setup_append_checklist(content, checklist);
	g_string_append(content, "</section>");
}
