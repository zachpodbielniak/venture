/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <string.h>

/* At most this many recipient ids in one IN filter. */
#define REPORT_CHUNK 500

typedef struct {
	GHashTable *people;
	GHashTable *machines;
} LinkTally;

static void tally_free(gpointer data)
{
	LinkTally *tally = data;
	g_hash_table_unref(tally->people);
	g_hash_table_unref(tally->machines);
	g_free(tally);
}
static void link_free(gpointer data)
{
	VentureMarketingLinkCount *link = data;
	g_free(link->destination);
	g_free(link);
}
static void arm_free(gpointer data)
{
	VentureMarketingArmCount *arm = data;
	g_free(arm->arm);
	g_free(arm->subject);
	g_free(arm);
}
void venture_marketing_send_report_free(VentureMarketingSendReport *report)
{
	if (!report) return;
	g_free(report->name);
	g_free(report->winner);
	g_clear_pointer(&report->arms, g_ptr_array_unref);
	g_clear_pointer(&report->links, g_ptr_array_unref);
	g_free(report);
}
static gboolean stamped(VentureEntity *row, const gchar *field)
{
	g_autoptr(GDateTime) at = NULL;
	g_object_get(row, field, &at, NULL);
	return at != NULL;
}
/* A destination as the click map shows it: scheme, host and path. The
 * query and fragment can carry a personal token or a merged field. */
static gchar *public_destination(const gchar *url)
{
	g_autoptr(GUri) uri = url ? g_uri_parse(url, G_URI_FLAGS_NONE, NULL) : NULL;
	if (!uri || !g_uri_get_host(uri)) return g_strdup("(unreadable link)");
	return g_strdup_printf("%s://%s%s", g_uri_get_scheme(uri), g_uri_get_host(uri), g_uri_get_path(uri));
}
static gboolean tally_events(VentureDatabase *database, gint64 org, GPtrArray *ids, GHashTable *links,
	GHashTable *soft, GError **error)
{
	guint start;
	for (start = 0; start < ids->len; start += REPORT_CHUNK) {
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_MARKETING_EVENT);
		g_autoptr(GPtrArray) chunk = g_ptr_array_new_with_free_func(g_free);
		g_autoptr(GPtrArray) kinds = g_ptr_array_new_with_free_func(g_free);
		g_autoptr(GPtrArray) rows = NULL;
		guint i;
		for (i = start; i < ids->len && i < start + REPORT_CHUNK; i++)
			g_ptr_array_add(chunk, g_strdup_printf("%" G_GINT64_FORMAT, *(gint64 *)g_ptr_array_index(ids, i)));
		g_ptr_array_add(kinds, g_strdup("click"));
		g_ptr_array_add(kinds, g_strdup("temporary_bounce"));
		venture_query_set_organization(query, org);
		venture_query_set_limit(query, 0);
		if (!venture_query_add_filter(query, "recipient-id", VENTURE_FILTER_OP_IN, chunk, error) ||
			!venture_query_add_filter(query, "kind", VENTURE_FILTER_OP_IN, kinds, error)) return FALSE;
		rows = venture_database_find(database, query, error);
		if (!rows) return FALSE;
		for (i = 0; i < rows->len; i++) {
			VentureEntity *event = g_ptr_array_index(rows, i);
			g_autofree gchar *kind = NULL, *machine = NULL;
			gint64 recipient = 0, position = 0;
			LinkTally *tally;
			g_object_get(event, "kind", &kind, "machine-reason", &machine, "recipient-id", &recipient, "link-position", &position, NULL);
			if (!g_strcmp0(kind, "temporary_bounce")) {
				g_hash_table_add(soft, g_memdup2(&recipient, sizeof recipient));
				continue;
			}
			if (position < 1) continue;
			tally = g_hash_table_lookup(links, &position);
			if (!tally) {
				tally = g_new0(LinkTally, 1);
				tally->people = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
				tally->machines = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
				g_hash_table_insert(links, g_memdup2(&position, sizeof position), tally);
			}
			g_hash_table_add(machine && *machine ? tally->machines : tally->people, g_memdup2(&recipient, sizeof recipient));
		}
	}
	return TRUE;
}
static gint compare_links(gconstpointer a, gconstpointer b)
{
	const VentureMarketingLinkCount *left = *(VentureMarketingLinkCount *const *)a, *right = *(VentureMarketingLinkCount *const *)b;
	return left->position < right->position ? -1 : left->position > right->position;
}
/* Every number for one send, from its own evidence: the outbox state for
 * SMTP outcomes, the recipient's first-evidence stamps for the rest, and
 * events for the click map and temporary bounces. */
static VentureMarketingSendReport *build_report(VentureDatabase *database, gint64 org, VentureEntity *send, GError **error)
{
	g_autoptr(VentureMarketingSendReport) report = g_new0(VentureMarketingSendReport, 1);
	g_autoptr(VentureQuery) audience = venture_query_new(VENTURE_TYPE_MARKETING_RECIPIENT);
	g_autoptr(GPtrArray) recipients = NULL, ids = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GHashTable) links = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, tally_free);
	g_autoptr(GHashTable) soft = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	g_autofree gchar *destinations = NULL;
	GHashTableIter iter;
	gpointer key, value;
	guint j;
	report->send_id = venture_entity_get_id(send);
	report->links = g_ptr_array_new_with_free_func(link_free);
	report->arms = g_ptr_array_new_with_free_func(arm_free);
	g_object_get(send, "name", &report->name, "eligible-count", &report->eligible, "excluded-count", &report->excluded, NULL);
	venture_query_set_organization(audience, org); venture_query_set_limit(audience, 0);
	venture_query_add_filter_int(audience, "send-id", VENTURE_FILTER_OP_EQ, report->send_id, NULL);
	venture_query_add_order(audience, "id", VENTURE_SORT_ASCENDING, NULL);
	recipients = venture_database_find(database, audience, error);
	if (!recipients) return NULL;
	for (j = 0; j < recipients->len; j++) {
		VentureEntity *recipient = g_ptr_array_index(recipients, j);
		g_autofree gchar *state = NULL;
		gint64 mail_id = 0, id = venture_entity_get_id(recipient);
		g_object_get(recipient, "state", &state, "mail-id", &mail_id, NULL);
		if (mail_id) {
			g_autoptr(VentureEntity) mail = venture_database_get(database, VENTURE_TYPE_MAIL_MESSAGE, mail_id, error);
			if (!mail) return NULL;
			if (venture_entity_get_organization_id(mail) != org) { venture_set_error_validation(error, "marketing", "Outbox evidence scope mismatch"); return NULL; }
			g_clear_pointer(&state, g_free); g_object_get(mail, "state", &state, NULL);
		}
		if (!g_strcmp0(state, "sent")) report->accepted++;
		else if (!g_strcmp0(state, "eligible") || !g_strcmp0(state, "queued") || !g_strcmp0(state, "sending") || !g_strcmp0(state, "failed")) report->pending++;
		else if (!g_strcmp0(state, "uncertain")) report->uncertain++;
		else if (!g_strcmp0(state, "dead")) report->failed++;
		else if (!g_strcmp0(state, "cancelled")) report->cancelled++;
		if (stamped(recipient, "bounced-at")) report->bounced++;
		if (stamped(recipient, "opened-at")) report->opened++;
		else if (stamped(recipient, "machine-opened-at")) report->machine_opened++;
		if (stamped(recipient, "clicked-at")) report->clicked++;
		if (stamped(recipient, "unsubscribed-at")) report->unsubscribed++;
		if (stamped(recipient, "delivered-at")) report->delivered++;
		if (stamped(recipient, "replied-at")) report->replied++;
		if (stamped(recipient, "complained-at")) report->complained++;
		if (!destinations) {
			g_autofree gchar *frozen = NULL;
			g_object_get(recipient, "links", &frozen, NULL);
			if (frozen && *frozen && strcmp(frozen, "[]")) destinations = g_steal_pointer(&frozen);
		}
		if (mail_id) g_ptr_array_add(ids, g_memdup2(&id, sizeof id));
	}
	{
		/* An A/B test's arms: each counted from its own recipients. */
		gint64 percent = 0;
		g_object_get(send, "test-percent", &percent, "winner", &report->winner, NULL);
		if (report->winner && !*report->winner) g_clear_pointer(&report->winner, g_free);
		if (percent > 0) {
			const gchar *names[] = { "a", "b", "rest" };
			guint k;
			for (k = 0; k < G_N_ELEMENTS(names); k++) {
				VentureMarketingArmCount *arm = g_new0(VentureMarketingArmCount, 1);
				arm->arm = g_strdup(names[k]);
				g_ptr_array_add(report->arms, arm);
			}
			for (j = 0; j < recipients->len; j++) {
				VentureEntity *recipient = g_ptr_array_index(recipients, j);
				g_autofree gchar *variant = NULL, *state = NULL;
				VentureMarketingArmCount *arm = NULL;
				gint64 mail_id = 0;
				g_object_get(recipient, "variant", &variant, "mail-id", &mail_id, NULL);
				for (k = 0; k < report->arms->len; k++)
					if (!g_strcmp0(variant, ((VentureMarketingArmCount *)g_ptr_array_index(report->arms, k))->arm))
						arm = g_ptr_array_index(report->arms, k);
				if (!arm) continue;
				arm->recipients++;
				if (!arm->subject) g_object_get(recipient, "subject", &arm->subject, NULL);
				if (mail_id) {
					g_autoptr(VentureEntity) mail = venture_database_get(database, VENTURE_TYPE_MAIL_MESSAGE, mail_id, NULL);
					if (mail) g_object_get(mail, "state", &state, NULL);
					if (!g_strcmp0(state, "sent")) arm->accepted++;
				}
				if (stamped(recipient, "opened-at")) arm->opened++;
				if (stamped(recipient, "clicked-at")) arm->clicked++;
			}
		}
	}
	if (!tally_events(database, org, ids, links, soft, error)) return NULL;
	report->soft_bounced = g_hash_table_size(soft);
	{
		g_autoptr(JsonNode) parsed = destinations ? json_from_string(destinations, NULL) : NULL;
		JsonArray *array = parsed && JSON_NODE_HOLDS_ARRAY(parsed) ? json_node_get_array(parsed) : NULL;
		guint n = array ? json_array_get_length(array) : 0;
		/* Every frozen link is on the map, clicked or not: a link nobody
		 * clicked is an answer too. */
		for (j = 0; j < n && j < 1000; j++) {
			VentureMarketingLinkCount *link = g_new0(VentureMarketingLinkCount, 1);
			gint64 position = j + 1;
			LinkTally *tally = g_hash_table_lookup(links, &position);
			link->position = position;
			link->destination = public_destination(json_array_get_string_element(array, j));
			if (tally) {
				GHashTableIter machines;
				link->clicks = g_hash_table_size(tally->people);
				g_hash_table_iter_init(&machines, tally->machines);
				while (g_hash_table_iter_next(&machines, &key, NULL))
					if (!g_hash_table_contains(tally->people, key)) link->machine_clicks++;
				g_hash_table_remove(links, &position);
			}
			g_ptr_array_add(report->links, link);
		}
		g_hash_table_iter_init(&iter, links);
		while (g_hash_table_iter_next(&iter, &key, &value)) {
			VentureMarketingLinkCount *link = g_new0(VentureMarketingLinkCount, 1);
			LinkTally *tally = value;
			link->position = *(gint64 *)key;
			link->destination = g_strdup("(link no longer in the message)");
			link->clicks = g_hash_table_size(tally->people);
			link->machine_clicks = g_hash_table_size(tally->machines);
			g_ptr_array_add(report->links, link);
		}
		g_ptr_array_sort(report->links, compare_links);
	}
	return g_steal_pointer(&report);
}
VentureMarketingSendReport *venture_marketing_send_report(VentureDatabase *database,
	gint64 organization_id, gint64 send_id, GError **error)
{
	g_autoptr(VentureEntity) send = NULL;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	send = venture_database_get(database, VENTURE_TYPE_MARKETING_SEND, send_id, error);
	if (!send) return NULL;
	if (venture_entity_get_organization_id(send) != organization_id) {
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Campaign send not found");
		return NULL;
	}
	return build_report(database, organization_id, send, error);
}
JsonNode *venture_marketing_send_report_to_json(const VentureMarketingSendReport *report)
{
	g_autoptr(JsonBuilder) builder = json_builder_new();
	const struct { const gchar *name; gint64 value; } numbers[] = {
		{ "send_id", report->send_id }, { "eligible", report->eligible }, { "excluded", report->excluded },
		{ "accepted", report->accepted }, { "pending", report->pending }, { "uncertain", report->uncertain },
		{ "failed", report->failed }, { "cancelled", report->cancelled }, { "delivered", report->delivered },
		{ "opened", report->opened }, { "machine_opened", report->machine_opened }, { "clicked", report->clicked },
		{ "replied", report->replied }, { "unsubscribed", report->unsubscribed }, { "bounced", report->bounced },
		{ "soft_bounced", report->soft_bounced }, { "complained", report->complained }
	};
	guint i;
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "name");
	json_builder_add_string_value(builder, report->name ? report->name : "");
	for (i = 0; i < G_N_ELEMENTS(numbers); i++) {
		json_builder_set_member_name(builder, numbers[i].name);
		json_builder_add_int_value(builder, numbers[i].value);
	}
	json_builder_set_member_name(builder, "links");
	json_builder_begin_array(builder);
	for (i = 0; i < report->links->len; i++) {
		const VentureMarketingLinkCount *link = g_ptr_array_index(report->links, i);
		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "position"); json_builder_add_int_value(builder, link->position);
		json_builder_set_member_name(builder, "destination"); json_builder_add_string_value(builder, link->destination);
		json_builder_set_member_name(builder, "clicks"); json_builder_add_int_value(builder, link->clicks);
		json_builder_set_member_name(builder, "machine_clicks"); json_builder_add_int_value(builder, link->machine_clicks);
		json_builder_end_object(builder);
	}
	json_builder_end_array(builder);
	json_builder_set_member_name(builder, "winner");
	if (report->winner) json_builder_add_string_value(builder, report->winner);
	else json_builder_add_null_value(builder);
	json_builder_set_member_name(builder, "arms");
	json_builder_begin_array(builder);
	for (i = 0; report->arms && i < report->arms->len; i++) {
		const VentureMarketingArmCount *arm = g_ptr_array_index(report->arms, i);
		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "arm"); json_builder_add_string_value(builder, arm->arm);
		json_builder_set_member_name(builder, "subject"); json_builder_add_string_value(builder, arm->subject ? arm->subject : "");
		json_builder_set_member_name(builder, "recipients"); json_builder_add_int_value(builder, arm->recipients);
		json_builder_set_member_name(builder, "accepted"); json_builder_add_int_value(builder, arm->accepted);
		json_builder_set_member_name(builder, "opened"); json_builder_add_int_value(builder, arm->opened);
		json_builder_set_member_name(builder, "clicked"); json_builder_add_int_value(builder, arm->clicked);
		json_builder_end_object(builder);
	}
	json_builder_end_array(builder);
	json_builder_end_object(builder);
	return json_builder_get_root(builder);
}

static VentureReportResult *performance(VentureContext *context, VentureDateRange *period, JsonObject *options, GError **error)
{
	VentureDatabase *database = venture_context_get_database(context);
	gint64 org = options && json_object_has_member(options, "organization_id") ? json_object_get_int_member(options, "organization_id") : venture_context_get_default_organization_id(context);
	gint64 only = options && json_object_has_member(options, "send_id") ? json_object_get_int_member(options, "send_id") : 0;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_MARKETING_SEND);
	g_autoptr(GPtrArray) sends = NULL;
	g_autoptr(VentureReportResult) result = venture_report_result_new("Marketing send performance", period);
	/* The first twelve columns keep their keys and meaning; the rest were
	 * added with the deliverability evidence. Opens and clicks judged a
	 * machine's are not in uniquely_opened or uniquely_clicked. */
	const gchar *keys[] = { "send_id", "eligible", "excluded", "accepted", "pending", "uncertain", "failed", "cancelled",
		"hard_bounced", "uniquely_opened", "uniquely_clicked", "unsubscribed",
		"delivered", "machine_opened", "replied", "complained", "temporary_bounced" };
	const gchar *labels[] = { "Send ID", "Approved eligible", "Preview exclusions", "SMTP accepted", "Pending", "Uncertain SMTP", "Known failures", "Cancelled",
		"Hard bounce evidence", "Unique opens (machines aside)", "Unique clicks (machines aside)", "Unsubscribe observations",
		"Provider reported delivered", "Machine-judged opens only", "Replied", "Complaints", "Temporary bounce evidence" };
	guint i, j;
	venture_query_set_organization(query, org); venture_query_set_limit(query, 0); venture_query_set_include_deleted(query, TRUE);
	if (only > 0) venture_query_add_filter_int(query, "id", VENTURE_FILTER_OP_EQ, only, NULL);
	if (period && !venture_query_set_date_range(query, "approved-at", period, error)) return NULL;
	sends = venture_database_find(database, query, error); if (!sends) return NULL;
	venture_report_result_add_column(result, "send", "Send", VENTURE_REPORT_COLUMN_TEXT);
	for (i = 0; i < G_N_ELEMENTS(keys); i++) venture_report_result_add_column(result, keys[i], labels[i], VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "period_basis", "Period basis", VENTURE_REPORT_COLUMN_TEXT);
	for (i = 0; i < sends->len; i++) {
		VentureEntity *send = g_ptr_array_index(sends, i);
		g_autoptr(GDateTime) approved = NULL;
		g_autoptr(VentureMarketingSendReport) report = NULL;
		gint64 counts[G_N_ELEMENTS(keys)];
		g_object_get(send, "approved-at", &approved, NULL);
		if (!approved) continue;
		report = build_report(database, org, send, error);
		if (!report) return NULL;
		counts[0] = report->send_id; counts[1] = report->eligible; counts[2] = report->excluded; counts[3] = report->accepted;
		counts[4] = report->pending; counts[5] = report->uncertain; counts[6] = report->failed; counts[7] = report->cancelled;
		counts[8] = report->bounced; counts[9] = report->opened; counts[10] = report->clicked; counts[11] = report->unsubscribed;
		counts[12] = report->delivered; counts[13] = report->machine_opened; counts[14] = report->replied;
		counts[15] = report->complained; counts[16] = report->soft_bounced;
		venture_report_result_begin_row(result); venture_report_result_set_text(result, "send", report->name);
		for (j = 0; j < G_N_ELEMENTS(keys); j++) venture_report_result_set_number(result, keys[j], counts[j]);
		venture_report_result_set_text(result, "period_basis", "Approval cohort; retained outcomes through report execution, not historical status reconstruction");
	}
	return g_steal_pointer(&result);
}
void venture_marketing_register_reports(VentureReportRegistry *registry)
{
	venture_report_registry_add(registry, VENTURE_REPORT(venture_func_report_new_classified(VENTURE_DATA_CLASS_TENANT, "marketing_performance", "Marketing send performance",
		"Approved-send cohort with SMTP acceptance, provider delivery, bounces, complaints, replies and unique engagement with machine opens counted apart", performance)));
}
