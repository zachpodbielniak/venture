/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>

static VentureReportResult *performance(VentureContext *context, VentureDateRange *period, JsonObject *options, GError **error)
{
	VentureDatabase *database = venture_context_get_database(context);
	gint64 org = options && json_object_has_member(options, "organization_id") ? json_object_get_int_member(options, "organization_id") : venture_context_get_default_organization_id(context);
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_MARKETING_SEND);
	g_autoptr(GPtrArray) sends = NULL;
	g_autoptr(VentureReportResult) result = venture_report_result_new("Marketing send performance", period);
	const gchar *keys[] = { "send_id", "eligible", "excluded", "accepted", "pending", "uncertain", "failed", "cancelled", "hard_bounced", "uniquely_opened", "uniquely_clicked", "unsubscribed" };
	const gchar *labels[] = { "Send ID", "Approved eligible", "Preview exclusions", "SMTP accepted", "Pending", "Uncertain SMTP", "Known failures", "Cancelled", "Hard bounce evidence", "Unique observed opens", "Unique observed clicks", "Unsubscribe observations" };
	guint i, j;
	venture_query_set_organization(query, org); venture_query_set_limit(query, 0); venture_query_set_include_deleted(query, TRUE);
	if (period && !venture_query_set_date_range(query, "approved-at", period, error)) return NULL;
	sends = venture_database_find(database, query, error); if (!sends) return NULL;
	venture_report_result_add_column(result, "send", "Send", VENTURE_REPORT_COLUMN_TEXT);
	for (i = 0; i < G_N_ELEMENTS(keys); i++) venture_report_result_add_column(result, keys[i], labels[i], VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "period_basis", "Period basis", VENTURE_REPORT_COLUMN_TEXT);
	for (i = 0; i < sends->len; i++) {
		VentureEntity *send = g_ptr_array_index(sends, i);
		g_autoptr(VentureQuery) audience = venture_query_new(VENTURE_TYPE_MARKETING_RECIPIENT);
		g_autoptr(GPtrArray) recipients = NULL;
		g_autoptr(GDateTime) approved = NULL;
		g_autofree gchar *name = NULL;
		gint64 counts[G_N_ELEMENTS(keys)] = { 0 };
		g_object_get(send, "approved-at", &approved, "name", &name, "eligible-count", &counts[1], "excluded-count", &counts[2], NULL);
		if (!approved) continue;
		counts[0] = venture_entity_get_id(send);
		venture_query_set_organization(audience, org); venture_query_set_limit(audience, 0);
		venture_query_add_filter_int(audience, "send-id", VENTURE_FILTER_OP_EQ, counts[0], NULL);
		recipients = venture_database_find(database, audience, error); if (!recipients) return NULL;
		for (j = 0; j < recipients->len; j++) {
			VentureEntity *recipient = g_ptr_array_index(recipients, j);
			g_autofree gchar *state = NULL;
			gint64 mail_id;
			const gchar *dates[] = { "bounced-at", "opened-at", "clicked-at", "unsubscribed-at" };
			guint k;
			g_object_get(recipient, "state", &state, "mail-id", &mail_id, NULL);
			if (mail_id) {
				g_autoptr(VentureEntity) mail = venture_database_get(database, VENTURE_TYPE_MAIL_MESSAGE, mail_id, error);
				if (!mail) return NULL;
				if (venture_entity_get_organization_id(mail) != org) { venture_set_error_validation(error, "marketing", "Outbox evidence scope mismatch"); return NULL; }
				g_clear_pointer(&state, g_free); g_object_get(mail, "state", &state, NULL);
			}
			if (!g_strcmp0(state, "sent")) counts[3]++;
			else if (!g_strcmp0(state, "eligible") || !g_strcmp0(state, "queued") || !g_strcmp0(state, "sending") || !g_strcmp0(state, "failed")) counts[4]++;
			else if (!g_strcmp0(state, "uncertain")) counts[5]++;
			else if (!g_strcmp0(state, "dead")) counts[6]++;
			else if (!g_strcmp0(state, "cancelled")) counts[7]++;
			for (k = 0; k < G_N_ELEMENTS(dates); k++) {
				g_autoptr(GDateTime) at = NULL;
				g_object_get(recipient, dates[k], &at, NULL);
				if (at) counts[8 + k]++;
			}
		}
		venture_report_result_begin_row(result); venture_report_result_set_text(result, "send", name);
		for (j = 0; j < G_N_ELEMENTS(keys); j++) venture_report_result_set_number(result, keys[j], counts[j]);
		venture_report_result_set_text(result, "period_basis", "Approval cohort; retained outcomes through report execution, not historical status reconstruction");
	}
	return g_steal_pointer(&result);
}
void venture_marketing_register_reports(VentureReportRegistry *registry)
{
	venture_report_registry_add(registry, VENTURE_REPORT(venture_func_report_new_classified(VENTURE_DATA_CLASS_TENANT, "marketing_performance", "Marketing send performance",
		"Approved-send cohort with SMTP acceptance, uncertainty, hard bounce evidence and unique observed engagement", performance)));
}
