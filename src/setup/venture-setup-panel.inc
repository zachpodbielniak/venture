/* SPDX-License-Identifier: AGPL-3.0-or-later */
static void
venture_setup_append_actions(GString *content, VentureEntity *record)
{
	const gchar *name = venture_entity_get_entity_name(record);
	g_autofree gchar *checklist = NULL;
	if (strcmp(name, "accounting_setup") != 0)
		return;
	g_object_get(record, "checklist", &checklist, NULL);
	g_string_append_printf(content,
		"<section class=\"card\"><h2>Accounting setup</h2>"
		"<p>Resumable checklist for the legal entity, book currency, fiscal year, "
		"basis, tax profile, chart, banks, openings and control accounts.</p>"
		"<form method=\"post\" action=\"/setup/preview\">"
		"<label>Legal name<input name=\"legal_name\" required></label>"
		"<label>Book currency<input name=\"book_currency\" value=\"USD\" required></label>"
		"<label>Fiscal year start<input name=\"fiscal_year_start\" value=\"2026-01-01\" required></label>"
		"<label>Basis<select name=\"basis\"><option>accrual</option><option>cash</option></select></label>"
		"<label>Tax profile<input name=\"tax_profile\" value=\"standard\"></label>"
		"<label>Chart template<input name=\"chart_template\" value=\"standard\"></label>"
		"<label>Bank name<input name=\"bank_name\" value=\"Operating account\"></label>"
		"<label>Opening cash<input name=\"opening_cash\" value=\"0 USD\"></label>"
		"<button class=\"btn btn-primary\" type=\"submit\">Preview</button></form>"
		"<form method=\"post\" action=\"/setup/%" G_GINT64_FORMAT "/action\">"
		"<input type=\"hidden\" name=\"action\" value=\"complete\">"
		"<button class=\"btn\" type=\"submit\">Complete setup</button></form>",
		venture_entity_get_id(record));
	if (checklist != NULL && checklist[0] != '\0')
	{
		g_string_append(content, "<pre class=\"muted\">");
		venture_html_escape_append(content, checklist);
		g_string_append(content, "</pre>");
	}
	g_string_append(content, "</section>");
}
