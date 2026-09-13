/* SPDX-License-Identifier: AGPL-3.0-or-later */
static void
venture_bank_append_actions(GString *content, VentureEntity *record)
{
	const gchar *name = venture_entity_get_entity_name(record);
	const gchar *options;
	if (!strcmp(name, "bank_account")) options = "<option>import</option>";
	else if (!strcmp(name, "bank_statement")) options = "<option>auto</option><option>reconcile</option>";
	else if (!strcmp(name, "bank_transaction")) options = "<option>match</option><option>unmatch</option><option>exclude</option><option>create</option>";
	else return;
	g_string_append_printf(content,
		"<section class=\"card\"><h2>Banking actions</h2>"
		"<form method=\"post\" action=\"/banking/%" G_GINT64_FORMAT "/action\">"
		"<label>Action<select name=\"action\">%s</select></label>"
		"<label>Parameters (JSON)<textarea name=\"parameters\">{}</textarea></label>"
		"<p>Match: parts with type, id and optional signed amount. Exclude: reason. "
		"Create: type expense or receipt, with customer_id for a receipt.</p>"
		"<button class=\"btn\" type=\"submit\">Apply</button></form></section>",
		venture_entity_get_id(record), options);
}
