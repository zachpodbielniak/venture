/* SPDX-License-Identifier: AGPL-3.0-or-later */
static void
venture_bank_append_actions(GString *content, VentureEntity *record)
{
	const gchar *name = venture_entity_get_entity_name(record);
	const gchar *options;
	guint i;
	if (!strcmp(name, "bank_account")) options = "<option>import</option><option>map</option><option>inbox</option><option>bulk</option><option>transfer</option>";
	else if (!strcmp(name, "bank_statement")) options = "<option>auto</option><option>reconcile</option><option>reopen</option>";
	else if (!strcmp(name, "bank_transaction")) options = "<option>match</option><option>unmatch</option><option>exclude</option><option>create</option><option>reverse</option><option>correct</option>";
	else if (!strcmp(name, "bank_rule")) options = "<option>preview</option><option>enable</option>";
	else return;
	g_string_append_printf(content,
		"<section class=\"card\"><h2>Banking actions</h2>"
		"<form method=\"post\" action=\"/banking/%" G_GINT64_FORMAT "/action\">"
		"<label>Action<select name=\"action\">%s</select></label>", venture_entity_get_id(record), options);
	if (!strcmp(name, "bank_account"))
	{
		g_string_append(content,
			"<label>Format<select name=\"format\"><option>csv</option><option>ofx</option><option>qfx</option></select></label>"
			"<label>Statement contents<textarea name=\"data\"></textarea></label>"
			"<label>Period start<input type=\"date\" name=\"period_start\"></label>"
			"<label>Period end<input type=\"date\" name=\"period_end\"></label>"
			"<label>Opening balance<input name=\"opening_balance\"></label>"
			"<label>Closing balance<input name=\"closing_balance\"></label>"
			"<p>CSV mapping. Leave amount empty when the file uses debit and credit columns.</p>"
			"<label>Date column<input name=\"date_column\"></label>"
			"<label>Amount column<input name=\"amount_column\"></label>"
			"<label>Debit column<input name=\"debit_column\"></label>"
			"<label>Credit column<input name=\"credit_column\"></label>"
			"<label>Description column<input name=\"description_column\"></label>"
			"<label>Reference column<input name=\"reference_column\"></label>"
			"<label>External id column<input name=\"external_id_column\"></label>"
			"<label>Locale<input name=\"locale\"></label>"
			"<label><input type=\"checkbox\" name=\"apply\" value=\"true\"> Save mapping</label>"
			"<label>Transfer to bank ID<input type=\"number\" name=\"counterparty_bank_account_id\" min=\"1\"></label>"
			"<label>Transfer amount<input name=\"amount\"></label>"
			"<label>Transfer date<input type=\"date\" name=\"date\"></label>");
	}
	else if (!strcmp(name, "bank_statement"))
		g_string_append(content, "<label>Reconciliation state<select name=\"state\">"
			"<option value=\"reconciled\">Finalize</option><option value=\"open\">Save open review</option></select></label>"
			"<label>Reopen reason<input name=\"reason\"></label>");
	else if (!strcmp(name, "bank_transaction"))
	{
		g_string_append(content,
			"<label>Exclusion reason<input name=\"reason\"></label>"
			"<label>Create<select name=\"type\"><option value=\"expense\">Expense</option>"
			"<option value=\"receipt\">Receipt</option></select></label>"
			"<label>Category<input name=\"category\"></label>"
			"<label>Customer ID for receipt<input type=\"number\" name=\"customer_id\" min=\"1\"></label>"
			"<label>Candidate search<input name=\"search\" placeholder=\"description or reference\"></label>"
			"<p>For matching, select the existing record and its signed amount. Add rows for a split, refund, journal or explicit fee adjustment. REST and CLI accept any number of parts.</p>");
		for (i = 0; i < 12; i++)
			g_string_append_printf(content,
				"<fieldset><legend>Match %u</legend><label>Record type<select name=\"record_type_%u\">"
				"<option value=\"expense\">Expense</option><option value=\"payment\">Payment</option>"
				"<option value=\"sale\">Sale</option><option value=\"refund\">Refund</option>"
				"<option value=\"journal\">Journal</option><option value=\"processor_payout\">Processor payout</option>"
				"<option value=\"adjustment\">Adjustment</option>"
				"<option value=\"vendor_bill_payment\">Vendor bill payment</option>"
				"<option value=\"bill_payment\">Bill payment</option></select></label>"
				"<label>Record ID<input type=\"number\" name=\"record_id_%u\" min=\"1\"></label>"
				"<label>Amount (optional)<input name=\"match_amount_%u\"></label></fieldset>", i + 1, i, i, i);
	}
	g_string_append(content, "<button class=\"btn\" type=\"submit\">Apply</button></form></section>");
}
