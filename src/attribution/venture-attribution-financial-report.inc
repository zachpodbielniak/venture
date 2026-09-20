/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* A calendar midnight carries a day, not an assertion that this financial
 * event happened before a same-day conversion. Retained creation evidence
 * must also put the event after acquisition; old rows cannot gain attribution
 * merely because their calendar date happens to match a later conversion. */
static gboolean financial_calendar_binding(VentureEntity *event, GDateTime *bound, GDateTime *date)
{
	g_autoptr(GDateTime) calendar = NULL, acquired = NULL;
	GDateTime *recorded = venture_entity_get_created_at(event);
	if (!bound || !date || !recorded || g_date_time_compare(recorded, bound) < 0) return FALSE;
	calendar = g_date_time_to_utc(date); acquired = g_date_time_to_utc(bound);
	return g_date_time_get_hour(calendar) == 0 && g_date_time_get_minute(calendar) == 0 &&
		g_date_time_get_second(calendar) == 0 && g_date_time_get_microsecond(calendar) == 0 &&
		g_date_time_get_year(calendar) == g_date_time_get_year(acquired) &&
		g_date_time_get_month(calendar) == g_date_time_get_month(acquired) &&
		g_date_time_get_day_of_month(calendar) == g_date_time_get_day_of_month(acquired);
}
static AttributionSource *financial_source(AttributionReport *report, gint64 invoice_id, gint64 company_id,
	gint64 deal_id, GDateTime *date, VentureEntity *event)
{
	VentureEntity *binding = NULL, *company = array_row(report->companies, company_id);
	g_autoptr(GDateTime) bound = NULL;
	guint i;
	if (!deal_id && invoice_id) {
		for (i = 0; i < report->quotes->len; i++) {
			VentureEntity *quote = g_ptr_array_index(report->quotes, i);
			gint64 candidate = report_number(quote, "deal-id");
			if (report_number(quote, "invoice-id") != invoice_id || !candidate) continue;
			if (deal_id && candidate != deal_id) return report_source(report, AMBIGUOUS_BINDING, NULL, NULL);
			deal_id = candidate;
		}
	}
	if (deal_id) binding = g_hash_table_lookup(report->deal_bindings, &deal_id);
	else binding = g_hash_table_lookup(report->company_bindings, &company_id);
	if (binding && (gpointer)binding != AMBIGUOUS_BINDING) {
		g_object_get(binding, "bound-at", &bound, NULL);
		if ((!bound || !date || g_date_time_compare(bound, date) > 0) && !financial_calendar_binding(event, bound, date))
			return report_source(report, NULL, NULL, "Acquisition was not bound when this financial event occurred");
	}
	return report_source(report, binding, company, deal_id ? "Explicit quote/deal has no retained acquisition; current CRM source only" : NULL);
}
static gboolean report_financial(AttributionReport *report, GError **error)
{
	g_autoptr(GPtrArray) deals = NULL, events = NULL, allocations = NULL, refunds = NULL, sales = NULL, invoices = NULL;
	g_autoptr(GHashTable) seen_sales = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	g_autoptr(GHashTable) first_cash = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	g_autoptr(VentureHeadlineSnapshot) headline = NULL;
	g_autoptr(JsonObject) scope = json_object_new();
	const gchar *book;
	GHashTableIter iterator;
	gpointer key, value;
	guint i;
	deals = report_rows(report, VENTURE_TYPE_DEAL, FALSE, error); if (!deals) return FALSE;
	for (i = 0; i < deals->len; i++) {
		VentureEntity *deal = g_ptr_array_index(deals, i);
		g_autoptr(GDateTime) date = NULL;
		g_autoptr(VentureMoney) amount = NULL;
		g_autoptr(AttributionSource) source = NULL;
		gint stage;
		g_object_get(deal, "stage", &stage, "closed-at", &date, "value", &amount, NULL);
		if (stage != VENTURE_DEAL_STAGE_WON || !report_within(report, date)) continue;
		source = financial_source(report, 0, report_number(deal, "company-id"), venture_entity_get_id(deal), date, deal);
		if (!report_fact(report, "won_deal_value", deal, source, amount, "Current won deal value at closed-at; management pipeline measure, not booked revenue", error)) return FALSE;
	}
	events = report_rows(report, VENTURE_TYPE_INVOICE_EVENT, TRUE, error); if (!events) return FALSE;
	for (i = 0; i < events->len; i++) {
		VentureEntity *event = g_ptr_array_index(events, i);
		g_autofree gchar *kind = report_string(event, "kind");
		g_autoptr(VentureMoney) amount = NULL;
		g_autoptr(GDateTime) date = NULL;
		g_autoptr(AttributionSource) source = NULL;
		if ((g_strcmp0(kind, "issue") && g_strcmp0(kind, "void")) || !row_within(report, event, "date")) continue;
		g_object_get(event, "net-amount", &amount, "date", &date, NULL);
		if (amount && !strcmp(kind, "void")) {
			VentureMoney *negative = venture_money_multiply_rational(amount, -1, 1, error);
			if (!negative) return FALSE;
			venture_money_free(amount); amount = negative;
		}
		source = financial_source(report, report_number(event, "invoice-id"), report_number(event, "customer-id"), 0, date, event);
		if (!report_fact(report, amount ? "invoiced_net" : "invoiced_net_unknown", event, source, amount,
			"Frozen invoice issue/void event date; net after discount, excluding tax and shipping; missing legacy evidence is unknown", error)) return FALSE;
	}
	allocations = report_rows(report, VENTURE_TYPE_PAYMENT_ALLOCATION, FALSE, error); if (!allocations) return FALSE;
	refunds = report_rows(report, VENTURE_TYPE_REFUND, FALSE, error); if (!refunds) return FALSE;
	sales = report_rows(report, VENTURE_TYPE_SALE, FALSE, error); if (!sales) return FALSE;
	invoices = report_rows(report, VENTURE_TYPE_INVOICE, TRUE, error); if (!invoices) return FALSE;
	json_object_set_int_member(scope, "organization_id", report->organization);
	headline = venture_headline_snapshot_new(report->context, scope, error); if (!headline) return FALSE;
	book = venture_headline_snapshot_get_currency(headline);
	/* Reuse the settlement service's own sale identities and values, just as
	 * the headline cash reader does. Never turn a payment balance into cash. */
	for (i = 0; i < allocations->len + refunds->len; i++) {
		gboolean refund = i >= allocations->len;
		VentureEntity *record = refund ? g_ptr_array_index(refunds, i - allocations->len) : g_ptr_array_index(allocations, i);
		gint64 sale_id = report_number(record, "sale-id"), invoice_id, company_id, *id;
		VentureEntity *sale = array_row(sales, sale_id), *invoice;
		g_autoptr(GDateTime) date = NULL;
		g_autoptr(VentureMoney) amount = NULL;
		g_autoptr(AttributionSource) source = NULL;
		if (!sale || g_hash_table_contains(seen_sales, &sale_id)) continue;
		id = g_new(gint64, 1); *id = sale_id; g_hash_table_add(seen_sales, id);
		g_object_get(sale, "occurred-at", &date, "gross", &amount, NULL);
		if (!date || !amount) continue;
		if (refund) {
			VentureEntity *allocation = array_row(allocations, report_number(record, "allocation-id"));
			invoice_id = allocation ? report_number(allocation, "invoice-id") : 0;
			company_id = report_number(record, "customer-id");
		} else {
			invoice_id = report_number(record, "invoice-id"); invoice = array_row(invoices, invoice_id);
			company_id = invoice ? report_number(invoice, "company-id") : 0;
		}
		if (!company_id) continue;
		if (venture_money_get_amount(amount) > 0 && !g_ascii_strcasecmp(venture_money_get_currency(amount), book)) {
			VentureEntity *prior = g_hash_table_lookup(first_cash, &company_id);
			g_autoptr(GDateTime) earlier = NULL;
			if (prior) g_object_get(prior, "occurred-at", &earlier, NULL);
			if (!earlier || g_date_time_compare(date, earlier) < 0) {
				id = g_new(gint64, 1); *id = company_id; g_hash_table_replace(first_cash, id, sale);
			}
		}
		if (!report_within(report, date)) continue;
		source = financial_source(report, invoice_id, company_id, 0, date, sale);
		if (!report_fact(report, refund ? "cash_refunds" : "cash_receipts", sale, source, amount,
			"Settlement-derived sale occurrence date; applied receipts or negative refund adjustment; one sale counted once", error)) return FALSE;
	}
	g_hash_table_iter_init(&iterator, first_cash);
	while (g_hash_table_iter_next(&iterator, &key, &value)) {
		VentureEntity *sale = value;
		gint64 company = *(gint64 *)key;
		g_autoptr(GDateTime) date = NULL;
		g_autoptr(AttributionSource) source = NULL;
		g_object_get(sale, "occurred-at", &date, NULL);
		if (!report_within(report, date)) continue;
		source = financial_source(report, 0, company, 0, date, sale);
		if (!report_fact(report, "customers", sale, source, NULL,
			"Company's first positive applied cash in book currency; same denominator and period as CAC", error)) return FALSE;
	}
	return TRUE;
}
