/* SPDX-License-Identifier: AGPL-3.0-or-later */
static void
venture_bank_append_actions(GString *content, VentureEntity *record)
{
	const gchar *name = venture_entity_get_entity_name(record);
	const gchar *options;
	guint i;
	if (!strcmp(name, "bank_account")) options = "<option>import</option>";
	else if (!strcmp(name, "bank_statement")) options = "<option>auto</option><option>reconcile</option>";
	else if (!strcmp(name, "bank_transaction")) options = "<option>match</option><option>unmatch</option><option>exclude</option><option>create</option>";
	else return;
	g_string_append_printf(content,
		"<section class=\"card\"><h2>Banking actions</h2>"
		"<form method=\"post\" action=\"/banking/%" G_GINT64_FORMAT "/action\">"
		"<label>Action<select name=\"action\">%s</select></label>", venture_entity_get_id(record), options);
	if (!strcmp(name, "bank_account"))
	{
		g_string_append(content,
			"<label>Format<select name=\"format\"><option>csv</option><option>ofx</option><option>qfx</option></select></label>"
			"<label>Statement contents<textarea name=\"data\" required></textarea></label>"
			"<label>Period start<input type=\"date\" name=\"period_start\" required></label>"
			"<label>Period end<input type=\"date\" name=\"period_end\" required></label>"
			"<label>Opening balance<input name=\"opening_balance\" required></label>"
			"<label>Closing balance<input name=\"closing_balance\" required></label>");
	}
	else if (!strcmp(name, "bank_statement"))
		g_string_append(content, "<label>Reconciliation state<select name=\"state\">"
			"<option value=\"reconciled\">Finalize</option><option value=\"open\">Save open review</option></select></label>");
	else
	{
		g_string_append(content,
			"<label>Exclusion reason<input name=\"reason\"></label>"
			"<label>Create<select name=\"type\"><option value=\"expense\">Expense</option>"
			"<option value=\"receipt\">Receipt</option></select></label>"
			"<label>Customer ID for receipt<input type=\"number\" name=\"customer_id\" min=\"1\"></label>"
			"<p>For matching, select the existing record and its signed amount. Add rows for a split.</p>");
		for (i = 0; i < 3; i++)
			g_string_append_printf(content,
				"<fieldset><legend>Match %u</legend><label>Record type<select name=\"record_type_%u\">"
				"<option value=\"expense\">Expense</option><option value=\"payment\">Payment</option>"
				"<option value=\"sale\">Sale</option><option value=\"vendor_bill_payment\">Vendor bill payment</option>"
				"<option value=\"bill_payment\">Bill payment</option></select></label>"
				"<label>Record ID<input type=\"number\" name=\"record_id_%u\" min=\"1\"></label>"
				"<label>Amount (optional)<input name=\"match_amount_%u\"></label></fieldset>", i + 1, i, i, i);
	}
	g_string_append(content, "<button class=\"btn\" type=\"submit\">Apply</button></form></section>");
}
