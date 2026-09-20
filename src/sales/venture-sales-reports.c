/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

typedef struct {
	gint64 quota_id, recipient_id;
	gboolean team;
	gchar *name, *currency;
	GDateTime *start, *end;
	VentureMoney *target, *actual, *open, *committed;
	gint64 bookings, unvalued;
} SalesRow;
static void row_free(gpointer data)
{
	SalesRow *row = data;
	g_free(row->name); g_free(row->currency);
	g_clear_pointer(&row->start, g_date_time_unref); g_clear_pointer(&row->end, g_date_time_unref);
	g_clear_pointer(&row->target, venture_money_free); g_clear_pointer(&row->actual, venture_money_free);
	g_clear_pointer(&row->open, venture_money_free); g_clear_pointer(&row->committed, venture_money_free); g_free(row);
}
static gint64 number(VentureEntity *entity, const gchar *field)
{
	gint64 value = 0;
	GParamSpec *spec;
	if (!entity) return 0;
	spec = g_object_class_find_property(G_OBJECT_GET_CLASS(entity), field);
	if (G_TYPE_IS_ENUM(G_PARAM_SPEC_VALUE_TYPE(spec))) {
		gint enumeration = 0; g_object_get(entity, field, &enumeration, NULL); return enumeration;
	}
	g_object_get(entity, field, &value, NULL); return value;
}
static gboolean within(GDateTime *at, GDateTime *start, GDateTime *end)
{ return at && (!start || g_date_time_compare(at, start) >= 0) && (!end || g_date_time_compare(at, end) < 0); }
static gboolean overlaps(GDateTime *start, GDateTime *end, VentureDateRange *period)
{
	GDateTime *from = venture_date_range_get_start(period), *until = venture_date_range_get_end(period);
	return (!from || !end || g_date_time_compare(end, from) > 0) && (!until || !start || g_date_time_compare(start, until) < 0);
}
static gboolean add(VentureMoney **total, const VentureMoney *value, GError **error)
{
	VentureMoney *next;
	if (!value) return TRUE;
	if (!*total) { *total = venture_money_copy(value); return TRUE; }
	next = venture_money_add(*total, value, error); if (!next) return FALSE;
	venture_money_free(*total); *total = next; return TRUE;
}
static GPtrArray *fetch(VentureDatabase *db, GType type, gint64 org, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(type);
	venture_query_set_organization(query, org); venture_query_set_limit(query, 0);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	return venture_database_find(db, query, error);
}
static gboolean matches(SalesRow *row, gboolean team, gint64 recipient, const gchar *currency)
{ return row->team == team && row->recipient_id == recipient && !g_strcmp0(row->currency, currency); }
static SalesRow *unplanned(GPtrArray *rows, gboolean team, gint64 recipient, const gchar *name,
	const gchar *currency, VentureDateRange *period)
{
	SalesRow *row;
	guint i;
	for (i = 0; i < rows->len; i++) {
		row = g_ptr_array_index(rows, i);
		if (row->quota_id == 0 && matches(row, team, recipient, currency)) return row;
	}
	row = g_new0(SalesRow, 1); row->team = team; row->recipient_id = recipient;
	row->name = g_strdup(recipient == 0 || venture_string_is_empty(name) ? "Unassigned or unresolved" : name); row->currency = g_strdup(currency);
	if (venture_date_range_get_start(period)) row->start = g_date_time_ref(venture_date_range_get_start(period));
	if (venture_date_range_get_end(period)) row->end = g_date_time_ref(venture_date_range_get_end(period));
	g_ptr_array_add(rows, row); return row;
}
static gint row_compare(gconstpointer a, gconstpointer b)
{
	const SalesRow *aa = *(SalesRow *const *)a, *bb = *(SalesRow *const *)b;
	gint order;
	if (aa->team != bb->team) return aa->team ? 1 : -1;
	if (aa->recipient_id != bb->recipient_id) return aa->recipient_id < bb->recipient_id ? -1 : 1;
	order = g_strcmp0(aa->currency, bb->currency); if (order) return order;
	if (aa->start && bb->start) { order = g_date_time_compare(aa->start, bb->start); if (order) return order; }
	return aa->quota_id < bb->quota_id ? -1 : aa->quota_id > bb->quota_id ? 1 : 0;
}
static VentureReportResult *attainment(VentureContext *context, VentureDateRange *period, JsonObject *options, GError **error)
{
	VentureDatabase *db = venture_context_get_database(context);
	gint64 org = options ? venture_json_object_get_int(options, "organization_id", venture_context_get_default_organization_id(context)) : venture_context_get_default_organization_id(context);
	g_autoptr(VentureEntity) organization = NULL;
	g_autoptr(GPtrArray) quotas = NULL, credits = NULL, deals = NULL;
	g_autoptr(GPtrArray) rows = g_ptr_array_new_with_free_func(row_free);
	g_autoptr(VentureReportResult) result = venture_report_result_new("Sales quota attainment", period);
	guint i;
	gint64 uncaptured = 0;
	organization = venture_database_get(db, VENTURE_TYPE_ORGANIZATION, org, error);
	if (!organization || venture_entity_is_deleted(organization)) return NULL;
	quotas = fetch(db, VENTURE_TYPE_SALES_QUOTA, org, error); if (!quotas) return NULL;
	credits = fetch(db, VENTURE_TYPE_SALES_CREDIT, org, error); if (!credits) return NULL;
	deals = fetch(db, VENTURE_TYPE_DEAL, org, error); if (!deals) return NULL;
	for (i = 0; i < quotas->len; i++) {
		VentureEntity *quota = g_ptr_array_index(quotas, i);
		g_autoptr(GDateTime) start = NULL, end = NULL;
		g_autoptr(VentureMoney) target = NULL;
		SalesRow *row;
		g_object_get(quota, "starts-at", &start, "ends-at", &end, "target", &target, NULL);
		if (!target || !overlaps(start, end, period)) continue;
		row = g_new0(SalesRow, 1); row->quota_id = venture_entity_get_id(quota);
		row->team = number(quota, "team-id") > 0; row->recipient_id = number(quota, row->team ? "team-id" : "owner-user-id");
		g_object_get(quota, "name", &row->name, NULL); row->currency = g_strdup(venture_money_get_currency(target));
		row->target = g_steal_pointer(&target); row->start = g_steal_pointer(&start); row->end = g_steal_pointer(&end);
		g_ptr_array_add(rows, row);
	}
	for (i = 0; i < credits->len; i++) {
		VentureEntity *credit = g_ptr_array_index(credits, i);
		g_autoptr(VentureMoney) value = NULL;
		g_autoptr(GDateTime) at = NULL;
		g_autofree gchar *rep_name = NULL, *team_name = NULL, *kind = NULL;
		gint sign;
		guint scope;
		g_object_get(credit, "value", &value, "credited-at", &at, "rep-name", &rep_name, "team-name", &team_name, "kind", &kind, NULL);
		sign = !g_strcmp0(kind, "reversal") ? -1 : 1;
		for (scope = 0; scope < 2; scope++) {
			gint64 recipient = number(credit, scope ? "team-id" : "owner-user-id");
			const gchar *currency = value ? venture_money_get_currency(value) : "";
			gboolean covered = FALSE;
			guint j;
			if (scope && recipient == 0) continue;
			for (j = 0; j < rows->len; j++) {
				SalesRow *row = g_ptr_array_index(rows, j);
				if (row->quota_id == 0 || !matches(row, scope != 0, recipient, currency) || !within(at, row->start, row->end)) continue;
				covered = TRUE; row->bookings += sign;
				if (!add(&row->actual, value, error)) return NULL;
			}
			if (!covered && within(at, venture_date_range_get_start(period), venture_date_range_get_end(period))) {
				SalesRow *row = unplanned(rows, scope != 0, recipient, scope ? team_name : rep_name, currency, period);
				row->bookings += sign; if (!value) row->unvalued += sign;
				if (!add(&row->actual, value, error)) return NULL;
			}
		}
	}
	for (i = 0; i < deals->len; i++) {
		VentureEntity *deal = g_ptr_array_index(deals, i), *latest = NULL;
		guint j;
		if (number(deal, "stage") != VENTURE_DEAL_STAGE_WON) continue;
		for (j = 0; j < credits->len; j++) {
			VentureEntity *candidate = g_ptr_array_index(credits, j);
			if (number(candidate, "deal-id") == venture_entity_get_id(deal)) latest = candidate;
		}
		if (!latest) uncaptured++;
		else {
			g_autofree gchar *kind = NULL;
			g_object_get(latest, "kind", &kind, NULL);
			if (g_strcmp0(kind, "booking")) uncaptured++;
		}
	}
	venture_report_result_add_metric(result, venture_metric_new_number("uncaptured_wins", "Won deals without captured booking credit", uncaptured));
	/* Match the existing forecast's current-open definition; it is displayed
	 * beside each complete quota window, never added to achieved bookings. */
	for (i = 0; i < deals->len; i++) {
		VentureEntity *deal = g_ptr_array_index(deals, i);
		g_autoptr(VentureMoney) value = NULL, weighted = NULL;
		g_autofree gchar *owner = NULL;
		gboolean committed = FALSE;
		guint scope;
		if (number(deal, "stage") >= VENTURE_DEAL_STAGE_WON) continue;
		g_object_get(deal, "value", &value, "owner", &owner, "committed", &committed, NULL);
		if (!value) continue;
		weighted = venture_deal_get_weighted_value(VENTURE_DEAL(deal), error); if (!weighted) return NULL;
		for (scope = 0; scope < 2; scope++) {
			gint64 recipient = number(deal, scope ? "team-id" : "owner-user-id");
			gboolean covered = FALSE;
			guint j;
			if (scope && recipient == 0) continue;
			for (j = 0; j < rows->len; j++) {
				SalesRow *row = g_ptr_array_index(rows, j);
				if (!matches(row, scope != 0, recipient, venture_money_get_currency(value))) continue;
				covered = TRUE;
				if (!add(&row->open, weighted, error) || ((committed || number(deal, "probability") >= 90) && !add(&row->committed, value, error))) return NULL;
			}
			if (!covered) {
				SalesRow *row = unplanned(rows, scope != 0, recipient, scope ? "Team without quota" : owner, venture_money_get_currency(value), period);
				if (!add(&row->open, weighted, error) || ((committed || number(deal, "probability") >= 90) && !add(&row->committed, value, error))) return NULL;
			}
		}
	}
	venture_report_result_add_column(result, "name", "Target or credited owner", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "scope", "Scope", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "currency", "Currency", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "target", "Target", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "actual", "Net booked", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "remaining", "Remaining", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "attainment", "Attainment", VENTURE_REPORT_COLUMN_PERCENT);
	venture_report_result_add_column(result, "open_weighted", "Current open weighted", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "committed", "Current committed", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "starts_at", "Period starts", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "ends_at", "Period ends (exclusive)", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "status", "Quota", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "bookings", "Net bookings", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "unvalued_bookings", "Net unvalued bookings", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "quota_id", "Quota ID", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "recipient_id", "Recipient ID", VENTURE_REPORT_COLUMN_TEXT);
	g_ptr_array_sort(rows, row_compare);
	for (i = 0; i < rows->len; i++) {
		SalesRow *row = g_ptr_array_index(rows, i);
		g_autoptr(VentureMoney) zero = *row->currency ? venture_money_new_zero(row->currency) : NULL, remaining = NULL;
		g_autofree gchar *start = row->start ? venture_time_to_date_string(row->start, NULL) : g_strdup("Unbounded");
		g_autofree gchar *end = row->end ? venture_time_to_date_string(row->end, NULL) : g_strdup("Unbounded");
		g_autofree gchar *quota_id = g_strdup_printf("%" G_GINT64_FORMAT, row->quota_id), *recipient_id = g_strdup_printf("%" G_GINT64_FORMAT, row->recipient_id);
		const VentureMoney *actual = row->actual ? row->actual : zero;
		venture_report_result_begin_row(result);
		venture_report_result_set_text(result, "quota_id", quota_id); venture_report_result_set_text(result, "recipient_id", recipient_id);
		venture_report_result_set_text(result, "scope", row->team ? "team" : "rep"); venture_report_result_set_text(result, "name", row->name);
		venture_report_result_set_text(result, "currency", *row->currency ? row->currency : "Unvalued");
		venture_report_result_set_text(result, "starts_at", start); venture_report_result_set_text(result, "ends_at", end);
		venture_report_result_set_text(result, "status", row->quota_id ? "Target set" : "No quota");
		if (actual) venture_report_result_set_money(result, "actual", actual);
		if (row->target) {
			remaining = venture_money_subtract(row->target, actual, error); if (!remaining) return NULL;
			venture_report_result_set_money(result, "target", row->target);
			venture_report_result_set_money(result, "remaining", venture_money_is_negative(remaining) ? zero : remaining);
			/* This ratio is presentation only; every monetary total above is
			 * exact integer arithmetic with checked currency and overflow. */
			venture_report_result_set_number(result, "attainment", venture_money_to_double(actual) / venture_money_to_double(row->target));
		}
		if (row->open) venture_report_result_set_money(result, "open_weighted", row->open);
		if (row->committed) venture_report_result_set_money(result, "committed", row->committed);
		venture_report_result_set_number(result, "bookings", row->bookings); venture_report_result_set_number(result, "unvalued_bookings", row->unvalued);
	}
	return g_steal_pointer(&result);
}
void venture_sales_reports_register(VentureReportRegistry *registry)
{
	g_return_if_fail(VENTURE_IS_REPORT_REGISTRY(registry));
	venture_report_registry_add(registry, VENTURE_REPORT(venture_func_report_new("sales_attainment", "Sales quota attainment",
		"Immutable booked sales by representative/team and currency, independent period targets and current forecast", attainment)));
}
