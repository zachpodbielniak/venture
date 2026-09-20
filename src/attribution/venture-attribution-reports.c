/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <string.h>
#include "report/venture-headline-private.h"

typedef struct {
	gchar *source, *reason;
	gint64 campaign;
} AttributionSource;
static void source_clear(AttributionSource *source)
{
	g_free(source->source); g_free(source->reason); g_free(source);
}
G_DEFINE_AUTOPTR_CLEANUP_FUNC(AttributionSource, source_clear)
typedef struct {
	gchar *measure, *source, *reason, *basis, *currency;
	gint64 campaign, count;
	GString *records;
	VentureMoney *amount;
} AttributionBucket;
static void bucket_free(gpointer data)
{
	AttributionBucket *bucket = data;
	g_free(bucket->measure); g_free(bucket->source); g_free(bucket->reason); g_free(bucket->basis); g_free(bucket->currency);
	g_string_free(bucket->records, TRUE); venture_money_free(bucket->amount); g_free(bucket);
}
typedef struct {
	VentureContext *context;
	VentureDatabase *database;
	VentureReportResult *result;
	VentureDateRange *period;
	gint64 organization;
	gboolean last, details;
	GHashTable *buckets, *company_bindings, *lead_bindings, *deal_bindings;
	GPtrArray *bindings, *companies, *quotes;
} AttributionReport;
#define AMBIGUOUS_BINDING ((gpointer)1)
static gint64 report_number(VentureEntity *row, const gchar *field)
{
	gint64 value = 0; g_object_get(row, field, &value, NULL); return value;
}
static gchar *report_string(VentureEntity *row, const gchar *field)
{
	gchar *value = NULL; g_object_get(row, field, &value, NULL); return value;
}
static gboolean report_within(AttributionReport *report, GDateTime *date)
{
	return date && (!report->period || venture_date_range_contains(report->period, date));
}
static gboolean row_within(AttributionReport *report, VentureEntity *row, const gchar *field)
{
	g_autoptr(GDateTime) date = NULL; g_object_get(row, field, &date, NULL); return report_within(report, date);
}
static GPtrArray *report_rows(AttributionReport *report, GType type, gboolean deleted, GError **error)
{
	g_autoptr(VentureEntity) example = g_object_new(type, NULL);
	g_autoptr(VentureQuery) query = NULL;
	if (!venture_entity_registry_lookup(venture_entity_registry_get_default(), venture_entity_get_entity_name(example)))
		return g_ptr_array_new_with_free_func(g_object_unref);
	query = venture_query_new(type); venture_query_set_organization(query, report->organization);
	venture_query_set_include_deleted(query, deleted); venture_query_set_limit(query, 0);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	return venture_database_find(report->database, query, error);
}
static VentureEntity *array_row(GPtrArray *rows, gint64 id)
{
	guint i;
	for (i = 0; rows && i < rows->len; i++) {
		VentureEntity *row = g_ptr_array_index(rows, i); if (venture_entity_get_id(row) == id) return row;
	}
	return NULL;
}
static void binding_index(GHashTable *index, gint64 id, VentureEntity *binding)
{
	gint64 *key;
	gpointer previous;
	if (id <= 0) return;
	previous = g_hash_table_lookup(index, &id);
	if (previous == binding) return;
	key = g_new(gint64, 1); *key = id;
	g_hash_table_replace(index, key, previous ? AMBIGUOUS_BINDING : binding);
}
static AttributionSource *report_source(AttributionReport *report, VentureEntity *evidence, VentureEntity *fallback,
	const gchar *reason)
{
	AttributionSource *source = g_new0(AttributionSource, 1);
	if ((gpointer)evidence == AMBIGUOUS_BINDING) {
		source->source = g_strdup("unknown"); source->reason = g_strdup("Ambiguous acquisition after repeated conversion or CRM merge"); return source;
	}
	if (evidence) {
		source->source = report_string(evidence, report->last ? "last-source" : "first-source");
		source->campaign = report_number(evidence, report->last ? "last-campaign-id" : "first-campaign-id");
		source->reason = report_string(evidence, report->last ? "last-evidence" : "first-evidence");
	} else if (fallback && g_object_class_find_property(G_OBJECT_GET_CLASS(fallback), "source")) {
		source->source = report_string(fallback, "source");
		if (g_object_class_find_property(G_OBJECT_GET_CLASS(fallback), "campaign-id")) source->campaign = report_number(fallback, "campaign-id");
		source->reason = g_strdup(reason ? reason : "Current CRM source only; no retained anonymous touch evidence");
	} else source->reason = g_strdup(reason ? reason : "No retained acquisition evidence");
	if (venture_string_is_empty(source->source)) { g_free(source->source); source->source = g_strdup("unknown"); }
	return source;
}
static void render_bucket(AttributionReport *report, AttributionBucket *bucket)
{
	venture_report_result_begin_row(report->result);
	venture_report_result_set_text(report->result, "model", report->last ? "last" : "first");
	venture_report_result_set_text(report->result, "measure", bucket->measure);
	venture_report_result_set_text(report->result, "source", bucket->source);
	venture_report_result_set_number(report->result, "campaign_id", bucket->campaign);
	venture_report_result_set_text(report->result, "currency", bucket->currency);
	venture_report_result_set_number(report->result, "count", bucket->count);
	venture_report_result_set_money(report->result, "amount", bucket->amount);
	venture_report_result_set_text(report->result, "source_records", bucket->records->str);
	venture_report_result_set_text(report->result, "period_basis", bucket->basis);
	venture_report_result_set_text(report->result, "evidence", bucket->reason);
}
static gboolean report_fact(AttributionReport *report, const gchar *measure, VentureEntity *record,
	AttributionSource *source, const VentureMoney *amount, const gchar *basis, GError **error)
{
	g_autofree gchar *key = NULL, *reference = NULL;
	const gchar *currency = amount ? venture_money_get_currency(amount) : "";
	AttributionBucket *bucket;
	/* Include the evidence category in the group: declared CRM sources must
	 * not become indistinguishable from retained consented observations. */
	key = g_strdup_printf("%s/%zu:%s/%" G_GINT64_FORMAT "/%s/%u/%s", measure, strlen(source->source), source->source,
		source->campaign, currency, amount ? venture_money_get_exponent(amount) : 0, source->reason ? source->reason : "");
	reference = g_strdup_printf("%s:%" G_GINT64_FORMAT, venture_entity_get_entity_name(record), venture_entity_get_id(record));
	bucket = g_hash_table_lookup(report->buckets, key);
	if (!bucket) {
		bucket = g_new0(AttributionBucket, 1); bucket->measure = g_strdup(measure); bucket->source = g_strdup(source->source);
		bucket->reason = g_strdup(source->reason); bucket->campaign = source->campaign; bucket->currency = g_strdup(currency);
		bucket->basis = g_strdup(basis); bucket->records = g_string_new(NULL);
		g_hash_table_insert(report->buckets, g_steal_pointer(&key), bucket);
	}
	bucket->count++;
	if (amount) {
		VentureMoney *sum = bucket->amount ? venture_money_add(bucket->amount, amount, error) : venture_money_copy(amount);
		if (!sum) return FALSE;
		venture_money_free(bucket->amount); bucket->amount = sum;
	}
	if (bucket->records->len < 8192) {
		if (bucket->records->len) g_string_append(bucket->records, ", ");
		g_string_append(bucket->records, reference);
		if (bucket->records->len >= 8192) g_string_append(bucket->records, " (additional records: use details=true)");
	}
	if (report->details) {
		AttributionBucket detail = *bucket;
		g_autoptr(GString) ref = g_string_new(reference);
		detail.count = 1; detail.amount = (VentureMoney *)amount; detail.records = ref; render_bucket(report, &detail);
	}
	return TRUE;
}
static gboolean report_acquisition(AttributionReport *report, GError **error)
{
	g_autoptr(GPtrArray) submissions = report_rows(report, VENTURE_TYPE_ATTRIBUTION_SUBMISSION, TRUE, error), leads = NULL, touches = NULL;
	g_autoptr(GHashTable) visitor_touches = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	g_autoptr(GHashTable) submitting_visitors = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	g_autoptr(GDateTime) now = venture_time_now();
	GDateTime *cutoff = report->period ? venture_date_range_get_end(report->period) : NULL;
	gint64 captured_leads = 0, converted = 0;
	GHashTableIter iterator;
	gpointer value;
	guint i;
	if (!submissions) return FALSE;
	leads = report_rows(report, VENTURE_TYPE_LEAD, TRUE, error); if (!leads) return FALSE;
	touches = report_rows(report, VENTURE_TYPE_ATTRIBUTION_TOUCH, TRUE, error); if (!touches) return FALSE;
	for (i = 0; i < touches->len; i++) {
		VentureEntity *touch = g_ptr_array_index(touches, i), *previous;
		gint64 visitor = report_number(touch, "visitor-id"), *key;
		g_autoptr(GDateTime) date = NULL, old = NULL;
		if (!visitor || !row_within(report, touch, "occurred-at")) continue;
		previous = g_hash_table_lookup(visitor_touches, &visitor);
		g_object_get(touch, "occurred-at", &date, NULL);
		if (previous) {
			gint order;
			g_object_get(previous, "occurred-at", &old, NULL); order = g_date_time_compare(date, old);
			if ((!report->last && order >= 0) || (report->last && order < 0)) continue;
		}
		key = g_new(gint64, 1); *key = visitor; g_hash_table_replace(visitor_touches, key, touch);
	}
	g_hash_table_iter_init(&iterator, visitor_touches);
	while (g_hash_table_iter_next(&iterator, NULL, &value)) {
		VentureEntity *touch = value;
		g_autoptr(AttributionSource) source = report_source(report, NULL, touch, "Consented visitor with a retained observation in this period; visitor is not proof of a unique person");
		if (!report_fact(report, "visitors", touch, source, NULL, "First/last retained observation in the period per visitor; redacted linkage excluded", error)) return FALSE;
	}
	for (i = 0; i < submissions->len; i++) {
		VentureEntity *submission = g_ptr_array_index(submissions, i);
		g_autoptr(AttributionSource) source = report_source(report, submission, NULL, NULL);
		if (row_within(report, submission, "submitted-at")) {
			gint64 visitor = report_number(submission, "visitor-id");
			if (visitor && g_hash_table_contains(visitor_touches, &visitor)) {
				gint64 *key = g_new(gint64, 1); *key = visitor; g_hash_table_add(submitting_visitors, key);
			}
			if (!report_fact(report, "submissions", submission, source, NULL, "Accepted form receipt time; exact retries counted once", error)) return FALSE;
		}
	}
	for (i = 0; i < leads->len; i++) {
		VentureEntity *lead = g_ptr_array_index(leads, i);
		gint64 id = venture_entity_get_id(lead);
		VentureEntity *binding = g_hash_table_lookup(report->lead_bindings, &id);
		g_autoptr(AttributionSource) source = report_source(report, binding, lead, NULL);
		if (row_within(report, lead, "created-at") && !report_fact(report, "leads", lead, source, NULL, "Lead creation time; repeated submissions do not create another lead", error)) return FALSE;
	}
	for (i = 0; i < report->bindings->len; i++) {
		VentureEntity *binding = g_ptr_array_index(report->bindings, i);
		g_autoptr(AttributionSource) source = report_source(report, binding, NULL, NULL);
		if (row_within(report, binding, "captured-at")) {
			g_autoptr(GDateTime) bound = NULL;
			captured_leads++; g_object_get(binding, "bound-at", &bound, NULL);
			if (bound && g_date_time_compare(bound, cutoff ? cutoff : now) < 0) converted++;
		}
		if (row_within(report, binding, "bound-at") && !report_fact(report, "converted_leads", binding, source, NULL, "Canonical conversion time for retained acquisition bindings", error)) return FALSE;
	}
	venture_report_result_add_metric(report->result, venture_metric_new_count("observed_visitors", "Observed consenting visitors", g_hash_table_size(visitor_touches)));
	venture_report_result_add_metric(report->result, venture_metric_new_count("submitting_observed_visitors", "Observed visitors submitting in period", g_hash_table_size(submitting_visitors)));
	venture_report_result_add_metric(report->result, g_hash_table_size(visitor_touches) ?
		venture_metric_new_ratio("visitor_submission_rate", "Observed visitor to form rate", (gdouble)g_hash_table_size(submitting_visitors) / g_hash_table_size(visitor_touches)) :
		venture_metric_new_text("visitor_submission_rate", "Observed visitor to form rate", "n/a"));
	venture_report_result_add_metric(report->result, venture_metric_new_count("captured_leads", "Captured lead cohort", captured_leads));
	venture_report_result_add_metric(report->result, venture_metric_new_count("captured_leads_converted", "Captured cohort converted by period end", converted));
	venture_report_result_add_metric(report->result, captured_leads ?
		venture_metric_new_ratio("captured_lead_conversion_rate", "Captured lead conversion rate", (gdouble)converted / captured_leads) :
		venture_metric_new_text("captured_lead_conversion_rate", "Captured lead conversion rate", "n/a"));
	return TRUE;
}
#include "venture-attribution-financial-report.inc"
#include "venture-attribution-report-reconcile.inc"

static VentureReportResult *attribution_report(VentureContext *context, VentureDateRange *period, JsonObject *options, GError **error)
{
	AttributionReport report;
	g_autoptr(VentureReportResult) result = venture_report_result_new("First-party source attribution", period);
	g_autoptr(GHashTable) buckets = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, bucket_free);
	g_autoptr(GHashTable) company_bindings = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	g_autoptr(GHashTable) lead_bindings = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	g_autoptr(GHashTable) deal_bindings = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	g_autoptr(GPtrArray) bindings = NULL, companies = NULL, quotes = NULL;
	g_autoptr(GList) keys = NULL;
	const gchar *model = options ? venture_json_object_get_string(options, "model", "first") : "first";
	const gchar *texts[] = { "model", "measure", "source", "currency", "source_records", "period_basis", "evidence" };
	GList *item;
	guint i;
	if (options) {
		JsonNode *value = json_object_get_member(options, "model");
		if (value && (!JSON_NODE_HOLDS_VALUE(value) || json_node_get_value_type(value) != G_TYPE_STRING)) {
			venture_set_error_validation(error, "model", "Choose first or last attribution"); return NULL;
		}
		value = json_object_get_member(options, "organization_id");
		if (value && (!JSON_NODE_HOLDS_VALUE(value) || json_node_get_value_type(value) != G_TYPE_INT64)) {
			venture_set_error_validation(error, "organization_id", "Organization must be an integer identity"); return NULL;
		}
		value = json_object_get_member(options, "details");
		if (value && (!JSON_NODE_HOLDS_VALUE(value) ||
			(json_node_get_value_type(value) != G_TYPE_BOOLEAN &&
			 (json_node_get_value_type(value) != G_TYPE_STRING ||
			  (g_strcmp0(json_node_get_string(value), "true") && g_strcmp0(json_node_get_string(value), "false")))))) {
			venture_set_error_validation(error, "details", "Details must be true or false"); return NULL;
		}
	}
	if (g_strcmp0(model, "first") && g_strcmp0(model, "last")) { venture_set_error_validation(error, "model", "Choose first or last attribution"); return NULL; }
	if (options && (json_object_has_member(options, "venture_id") || json_object_has_member(options, "as_of"))) {
		venture_set_error_validation(error, "attribution", "Attribution supports organization and period; partial venture/as-of reconstruction is unavailable"); return NULL;
	}
	report.context = context; report.database = venture_context_get_database(context); report.result = result; report.period = period;
	report.organization = options ? venture_json_object_get_int(options, "organization_id", venture_context_get_default_organization_id(context)) : venture_context_get_default_organization_id(context);
	if (report.organization <= 0) { venture_set_error_validation(error, "organization_id", "Choose an exact organization"); return NULL; }
	report.last = !strcmp(model, "last"); report.details = options && venture_json_object_get_bool(options, "details", FALSE);
	report.buckets = buckets; report.company_bindings = company_bindings; report.lead_bindings = lead_bindings; report.deal_bindings = deal_bindings;
	bindings = report_rows(&report, VENTURE_TYPE_ATTRIBUTION_BINDING, TRUE, error); if (!bindings) return NULL;
	companies = report_rows(&report, VENTURE_TYPE_COMPANY, TRUE, error); if (!companies) return NULL;
	quotes = report_rows(&report, VENTURE_TYPE_QUOTE, TRUE, error); if (!quotes) return NULL;
	report.bindings = bindings; report.companies = companies; report.quotes = quotes;
	for (i = 0; i < bindings->len; i++) {
		VentureEntity *binding = g_ptr_array_index(bindings, i), *company;
		gint64 company_id = report_number(binding, "company-id"), survivor;
		binding_index(lead_bindings, report_number(binding, "lead-id"), binding);
		binding_index(deal_bindings, report_number(binding, "deal-id"), binding);
		binding_index(company_bindings, company_id, binding);
		company = array_row(companies, company_id);
		survivor = company ? venture_dedupe_merged_into(report.database, company) : 0;
		if (survivor && array_row(companies, survivor)) binding_index(company_bindings, survivor, binding);
	}
	for (i = 0; i < G_N_ELEMENTS(texts); i++) venture_report_result_add_column(result, texts[i], texts[i], VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "campaign_id", "Campaign ID", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "count", "Distinct source records", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "amount", "Amount in stated currency", VENTURE_REPORT_COLUMN_MONEY);
	if (!report_acquisition(&report, error) || !report_financial(&report, error) || !report_reconcile(&report, options, error)) return NULL;
	if (!report.details) {
		keys = g_hash_table_get_keys(buckets); keys = g_list_sort(g_steal_pointer(&keys), (GCompareFunc)g_strcmp0);
		for (item = keys; item; item = item->next) render_bucket(&report, g_hash_table_lookup(buckets, item->data));
	}
	venture_report_result_set_note(result, "One selected model, one source record per measure. Counts have separate denominators and dates: visitors are observed consenting capabilities; submissions are accepted forms; leads are new CRM rows; converted_leads have retained conversion evidence; customers are first positive applied cash in book currency (CAC basis). Won deal value is current CRM value dated at close; invoiced_net is frozen issued net less voids; cash_receipts and cash_refunds are settlement-derived sales, excluding unapplied money and noncash credits. Currency buckets are never added across currencies. Retained business snapshots survive analytics withdrawal; old CRM source is declared provenance, not proof of consent or a historical touch. details=true lists individual source records. Disabled financial/report modules contribute no rows.");
	return g_steal_pointer(&result);
}
void venture_attribution_register_reports(VentureReportRegistry *registry)
{
	venture_report_registry_add(registry, VENTURE_REPORT(venture_func_report_new_classified(VENTURE_DATA_CLASS_TENANT, "attribution", "Source attribution",
		"Organization-period first/last-touch source/campaign counts and separate source-linked deal, invoice and applied-cash measures", attribution_report)));
}
