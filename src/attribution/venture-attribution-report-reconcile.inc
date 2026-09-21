/* SPDX-License-Identifier: AGPL-3.0-or-later */
static gboolean report_reconcile(AttributionReport *report, JsonObject *options, GError **error)
{
	const gchar *names[] = { "cac", "campaigns" };
	g_autoptr(JsonObject) scope = json_object_new();
	guint i, j;
	json_object_set_int_member(scope, "organization_id", report->organization);
	for (i = 0; i < G_N_ELEMENTS(names); i++) {
		VentureReport *definition = venture_report_registry_lookup(venture_context_get_report_registry(report->context), names[i]);
		g_autoptr(VentureReportResult) existing = NULL;
		GPtrArray *metrics;
		if (!definition) continue;
		existing = venture_report_generate(definition, report->context, report->period, scope, error); if (!existing) return FALSE;
		metrics = venture_report_result_get_metrics(existing);
		for (j = 0; j < metrics->len; j++) {
			VentureMetric *metric = g_ptr_array_index(metrics, j), *copy;
			const VentureMoney *amount = venture_metric_get_money(metric);
			const gchar *text = venture_metric_get_text(metric);
			g_autofree gchar *key = g_strdup_printf("%s_%s", names[i], venture_metric_get_key(metric));
			g_autofree gchar *label = g_strdup_printf("%s report: %s", names[i], venture_metric_get_label(metric));
			if (amount) copy = venture_metric_new_money(key, label, amount);
			else if (text) copy = venture_metric_new_text(key, label, text);
			else copy = venture_metric_new_count(key, label, venture_metric_get_number(metric));
			venture_metric_set_note(copy, i ? "Exact existing campaigns report, same organization/period: campaign-start cohort and declared totals; not issued or collected revenue" :
				"Exact existing CAC report, same organization/period: first applied book-currency cash customers and dated acquisition spend");
			venture_report_result_add_metric(report->result, copy);
			if (!strcmp(names[i], "cac") && !strcmp(venture_metric_get_key(metric), "new_customers")) {
				GHashTableIter iterator;
				gpointer value;
				gint64 customers = 0;
				g_hash_table_iter_init(&iterator, report->buckets);
				while (g_hash_table_iter_next(&iterator, NULL, &value)) {
					AttributionBucket *bucket = value;
					if (!strcmp(bucket->measure, "customers")) customers += bucket->count;
				}
				venture_report_result_add_metric(report->result, venture_metric_new_count("cac_customer_difference", "Attribution customers minus CAC customers",
					customers - venture_metric_get_number(metric)));
			}
		}
	}
	return TRUE;
}
