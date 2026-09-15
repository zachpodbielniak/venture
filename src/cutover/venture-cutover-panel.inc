/* SPDX-License-Identifier: AGPL-3.0-or-later */
static void
venture_cutover_append_actions(GString *content, VentureEntity *record)
{
	const gchar *name = venture_entity_get_entity_name(record);
	if (strcmp(name, "accounting_cutover") != 0)
		return;
	g_string_append_printf(content,
		"<section class=\"card\"><h2>Cutover</h2>"
		"<form method=\"post\" action=\"/cutover/preview\">"
		"<label>Mapped source JSON<textarea name=\"payload\" required placeholder='{\"source\":\"zoho_books\",\"cutoff\":\"2026-01-01\"}'></textarea></label>"
		"<button class=\"btn btn-primary\" type=\"submit\">Preview</button></form>"
		"<form method=\"post\" action=\"/cutover/%" G_GINT64_FORMAT "/action\">"
		"<label>Action<select name=\"action\">"
		"<option>import</option><option>reconcile</option><option>activate</option><option>rollback</option>"
		"</select></label>"
		"<button class=\"btn\" type=\"submit\">Apply</button></form></section>",
		venture_entity_get_id(record));
}
