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
		"<option>import</option><option>reconcile</option><option>activate</option>"
		"<option>rollback_preflight</option><option>rollback</option>"
		"</select></label>"
		"<button class=\"btn\" type=\"submit\">Apply</button></form>"
		"<form method=\"post\" action=\"/cutover/csv\" enctype=\"multipart/form-data\">"
		"<p>Or preview a new batch from CSV exports, one file per section. Templates: "
		"<a href=\"/cutover/template/customers\">customers</a>, <a href=\"/cutover/template/vendors\">vendors</a>, "
		"<a href=\"/cutover/template/chart\">chart</a>, <a href=\"/cutover/template/account_map\">account_map</a>, "
		"<a href=\"/cutover/template/open_ar\">open_ar</a>, <a href=\"/cutover/template/open_ap\">open_ap</a>, "
		"<a href=\"/cutover/template/credits\">credits</a>, <a href=\"/cutover/template/bank_balances\">bank_balances</a>, "
		"<a href=\"/cutover/template/assets\">assets</a>, <a href=\"/cutover/template/trial_balance\">trial_balance</a>.</p>"
		"<label>Source<select name=\"source\"><option>zoho_books</option><option>quickbooks</option>"
		"<option>xero</option><option>generic</option></select></label>"
		"<label>Cutoff<input name=\"cutoff\" required placeholder=\"2026-01-01\"></label>"
		"<label>Currency<input name=\"currency\" placeholder=\"USD\"></label>"
		"<label>Customers<input type=\"file\" name=\"customers\" accept=\".csv,text/csv\"></label>"
		"<label>Vendors<input type=\"file\" name=\"vendors\" accept=\".csv,text/csv\"></label>"
		"<label>Chart<input type=\"file\" name=\"chart\" accept=\".csv,text/csv\"></label>"
		"<label>Account map<input type=\"file\" name=\"account_map\" accept=\".csv,text/csv\"></label>"
		"<label>Open AR<input type=\"file\" name=\"open_ar\" accept=\".csv,text/csv\"></label>"
		"<label>Open AP<input type=\"file\" name=\"open_ap\" accept=\".csv,text/csv\"></label>"
		"<label>Credits<input type=\"file\" name=\"credits\" accept=\".csv,text/csv\"></label>"
		"<label>Bank balances<input type=\"file\" name=\"bank_balances\" accept=\".csv,text/csv\"></label>"
		"<label>Assets<input type=\"file\" name=\"assets\" accept=\".csv,text/csv\"></label>"
		"<label>Trial balance<input type=\"file\" name=\"trial_balance\" accept=\".csv,text/csv\"></label>"
		"<button class=\"btn\" type=\"submit\">Preview from CSV</button></form></section>",
		venture_entity_get_id(record));
}
