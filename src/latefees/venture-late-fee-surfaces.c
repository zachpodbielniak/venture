/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
#include "latefees/venture-late-fee-private.h"

/*
 * What people, scripts and the assistant reach: the actions (REST action
 * routes, venturectl act, the generated assistant tools, all staged the
 * same way), the late_fees report and the timeline entries. Every write
 * goes through the engine's audited path.
 */

static gboolean
refuse(GError **error, const gchar *message)
{
	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Late fees: %s", message);
	return FALSE;
}

static gint64
number(VentureEntity *e, const gchar *field)
{
	gint64 value = 0;
	g_object_get(e, field, &value, NULL);
	return value;
}

static gchar *
text(VentureEntity *e, const gchar *field)
{
	gchar *value = NULL;
	g_object_get(e, field, &value, NULL);
	return value;
}

static GDateTime *
when(VentureEntity *e, const gchar *field)
{
	GDateTime *value = NULL;
	g_object_get(e, field, &value, NULL);
	return value;
}

static gboolean
module_on(GError **error)
{
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "late_fee") == G_TYPE_INVALID)
		return refuse(error, "the late_fees module is disabled");
	return TRUE;
}

/* --- Parameters ----------------------------------------------------------- */

static const gchar *
param_string(GHashTable *params, const gchar *name)
{
	JsonNode *node = params ? g_hash_table_lookup(params, name) : NULL;
	if (node == NULL || !JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_STRING)
		return NULL;
	return json_node_get_string(node);
}

static GDateTime *
param_date(GHashTable *params, const gchar *name, GError **error)
{
	const gchar *value = param_string(params, name);
	if (venture_string_is_empty(value))
		return NULL;
	return venture_time_from_string(value, error);
}

/* The engine copies the saved row back into the object it was handed. */
static VentureEntity *
reloaded(VentureEntity *entity)
{
	return g_object_ref(entity);
}

/* --- Fee actions ---------------------------------------------------------- */

static gboolean
fee_allowed(VentureAction *action, VentureEntity *entity, const VentureActor *actor, GError **error)
{
	(void)action;
	(void)actor;
	if (!module_on(error))
		return FALSE;
	if (!VENTURE_IS_LATE_FEE(entity) || venture_entity_get_id(entity) <= 0)
		return refuse(error, "a saved late_fee is required");
	return TRUE;
}

static VentureEntity *
approve_invoke(VentureAction *action, VentureEntity *entity, GHashTable *params, const VentureActor *actor, GError **error)
{
	(void)params;
	if (!venture_late_fee_service_approve(venture_action_get_data(action), entity, NULL, actor, error))
		return NULL;
	return reloaded(entity);
}

static VentureEntity *
skip_invoke(VentureAction *action, VentureEntity *entity, GHashTable *params, const VentureActor *actor, GError **error)
{
	if (!venture_late_fee_service_skip(venture_action_get_data(action), entity, param_string(params, "reason"), actor, error))
		return NULL;
	return reloaded(entity);
}

static VentureEntity *
skip_notice_invoke(VentureAction *action, VentureEntity *entity, GHashTable *params, const VentureActor *actor, GError **error)
{
	if (!venture_late_fee_service_skip_notice(venture_action_get_data(action), entity, param_string(params, "reason"), actor, error))
		return NULL;
	return reloaded(entity);
}

static VentureEntity *
waive_invoke(VentureAction *action, VentureEntity *entity, GHashTable *params, const VentureActor *actor, GError **error)
{
	if (!venture_late_fee_service_waive(venture_action_get_data(action), entity, param_string(params, "reason"),
		param_string(params, "note"), NULL, actor, error))
		return NULL;
	return reloaded(entity);
}

static gboolean
decide_allowed(VentureAction *action, VentureEntity *entity, const VentureActor *actor, GError **error)
{
	(void)action;
	(void)entity;
	(void)actor;
	return module_on(error);
}

/* Several at once: "ids" is a comma-separated list, each its own decision. */
static VentureEntity *
decide_invoke(VentureAction *action, VentureEntity *entity, GHashTable *params, const VentureActor *actor, GError **error)
{
	VentureLateFeeService *self = venture_action_get_data(action);
	const gchar *list = param_string(params, "ids");
	g_auto(GStrv) parts = NULL;
	g_autoptr(GArray) ids = g_array_new(FALSE, FALSE, sizeof(gint64));
	g_autoptr(JsonNode) answer = NULL;
	g_autofree gchar *serialized = NULL;
	gint64 org = entity != NULL ? venture_entity_get_organization_id(entity) : 0;
	VentureEntity *result;
	guint i;
	if (venture_string_is_empty(list))
	{
		refuse(error, "ids names the fees, comma separated");
		return NULL;
	}
	parts = g_strsplit(list, ",", -1);
	for (i = 0; parts[i] != NULL; i++)
	{
		gchar *end = NULL;
		gint64 id;
		g_strstrip(parts[i]);
		if (parts[i][0] == '\0')
			continue;
		id = g_ascii_strtoll(parts[i], &end, 10);
		if (end == parts[i] || *end != '\0' || id <= 0)
		{
			refuse(error, "ids are late_fee ids, comma separated");
			return NULL;
		}
		g_array_append_val(ids, id);
	}
	if (org <= 0 && ids->len > 0)
	{
		g_autoptr(VentureEntity) first = venture_database_get(venture_late_fee_service_get_database(self), VENTURE_TYPE_LATE_FEE,
			g_array_index(ids, gint64, 0), NULL);
		org = first != NULL ? venture_entity_get_organization_id(first) : 0;
	}
	answer = venture_late_fee_service_decide(self, org, (const gint64 *)(gpointer)ids->data, ids->len,
		g_strcmp0(param_string(params, "decision"), "skip") != 0, param_string(params, "reason"), actor, error);
	if (answer == NULL)
		return NULL;
	/* A type-level action answers with a record; the answer rides on an
	 * unsaved fee's note, which the fee validator refuses to store. */
	serialized = venture_json_to_string(answer, FALSE);
	result = g_object_new(VENTURE_TYPE_LATE_FEE, NULL);
	venture_entity_set_organization_id(result, org);
	g_object_set(result, "note", serialized, "status", "decided", NULL);
	return result;
}

/* --- Pause actions -------------------------------------------------------- */

static const gchar *
scope_of(VentureEntity *entity, gint64 *id)
{
	if (VENTURE_IS_INVOICE(entity))
	{
		*id = venture_entity_get_id(entity);
		return "invoice";
	}
	if (VENTURE_IS_COMPANY(entity))
	{
		*id = venture_entity_get_id(entity);
		return "company";
	}
	*id = venture_entity_get_organization_id(entity);
	return "organization";
}

static gboolean
pause_allowed(VentureAction *action, VentureEntity *entity, const VentureActor *actor, GError **error)
{
	(void)action;
	(void)actor;
	if (!module_on(error))
		return FALSE;
	if (entity == NULL || venture_entity_get_id(entity) <= 0)
		return refuse(error, "a saved record is required");
	return TRUE;
}

static VentureEntity *
reread(VentureLateFeeService *self, VentureEntity *entity, GError **error)
{
	return venture_database_get(venture_late_fee_service_get_database(self), G_OBJECT_TYPE(entity), venture_entity_get_id(entity), error);
}

static VentureEntity *
pause_invoke(VentureAction *action, VentureEntity *entity, GHashTable *params, const VentureActor *actor, GError **error)
{
	VentureLateFeeService *self = venture_action_get_data(action);
	g_autoptr(GError) local = NULL;
	g_autoptr(GDateTime) until = param_date(params, "until", &local);
	const gchar *scope;
	gint64 id = 0;
	if (local != NULL)
	{
		g_propagate_error(error, g_steal_pointer(&local));
		return NULL;
	}
	scope = scope_of(entity, &id);
	if (!venture_late_fee_service_pause(self, scope, id, until, param_string(params, "reason"), actor, error))
		return NULL;
	return reread(self, entity, error);
}

static VentureEntity *
resume_invoke(VentureAction *action, VentureEntity *entity, GHashTable *params, const VentureActor *actor, GError **error)
{
	VentureLateFeeService *self = venture_action_get_data(action);
	const gchar *scope;
	gint64 id = 0;
	(void)params;
	scope = scope_of(entity, &id);
	if (!venture_late_fee_service_resume(self, scope, id, actor, error))
		return NULL;
	return reread(self, entity, error);
}

/* --- Preview action ------------------------------------------------------- */

static gboolean
preview_allowed(VentureAction *action, VentureEntity *entity, const VentureActor *actor, GError **error)
{
	(void)action;
	(void)actor;
	if (!module_on(error))
		return FALSE;
	if (!VENTURE_IS_INVOICE(entity) || venture_entity_get_id(entity) <= 0)
		return refuse(error, "a saved invoice is required");
	return TRUE;
}

static VentureEntity *
preview_invoke(VentureAction *action, VentureEntity *entity, GHashTable *params, const VentureActor *actor, GError **error)
{
	VentureLateFeeService *self = venture_action_get_data(action);
	g_autoptr(GError) local = NULL;
	g_autoptr(GDateTime) as_of = param_date(params, "as_of", &local);
	g_autoptr(JsonNode) preview = NULL;
	g_autofree gchar *serialized = NULL;
	VentureEntity *result;
	(void)actor;
	if (local != NULL)
	{
		g_propagate_error(error, g_steal_pointer(&local));
		return NULL;
	}
	preview = venture_late_fee_service_preview(self, venture_entity_get_id(entity), as_of, error);
	if (preview == NULL)
		return NULL;
	serialized = venture_json_to_string(preview, FALSE);
	result = g_object_new(VENTURE_TYPE_LATE_FEE, NULL);
	venture_entity_set_organization_id(result, venture_entity_get_organization_id(entity));
	g_object_set(result, "invoice-id", venture_entity_get_id(entity), "status", "preview", "note", serialized, NULL);
	return result;
}

/* --- Registration --------------------------------------------------------- */

static void
add_param(GPtrArray *parameters, const gchar *name, const gchar *label, VentureFieldKind kind, gboolean required)
{
	VentureFieldSpec *spec = venture_field_spec_new(name, label, kind);
	spec->required = required;
	g_ptr_array_add(parameters, spec);
}

static void
register_action(VentureActionRegistry *registry, const gchar *type_name, const gchar *name, const gchar *label,
	const gchar *description, GPtrArray *parameters, gboolean stageable, gboolean type_level,
	VentureActionAllowed allowed, VentureActionInvoke invoke, VentureLateFeeService *self)
{
	g_autoptr(VentureAction) action = NULL;
	g_autoptr(GError) error = NULL;
	action = g_object_new(VENTURE_TYPE_ACTION, "data-class", VENTURE_DATA_CLASS_TENANT, "type-name", type_name, "name", name,
		"label", label, "description", description, "parameters", parameters, "stageable", stageable,
		"type-level", type_level, "service-transaction", TRUE, "roles", VENTURE_USER_ROLE_EDITOR, NULL);
	if (!venture_action_registry_register(registry, action, allowed, invoke, self, NULL, &error))
		g_error("Late-fee action registration: %s", error->message);
	g_ptr_array_set_size(parameters, 0);
}

void
venture_late_fee_actions_register(VentureDatabase *database)
{
	VentureLateFeeService *self = venture_late_fee_service_get(database);
	VentureActionRegistry *registry;
	g_autoptr(GPtrArray) p = g_ptr_array_new_with_free_func((GDestroyNotify)venture_field_spec_free);
	static const gchar *const paused[] = { "invoice", "company", "late_fee_setting", NULL };
	guint i;
	if (!venture_late_fee_service_claim_actions(self))
		return;
	registry = venture_database_get_action_registry(database);
	register_action(registry, "late_fee", "approve", "Approve late fee",
		"Add this proposed fee to the invoice as a separate adjustment through settlement", p, TRUE, FALSE,
		fee_allowed, approve_invoke, self);
	add_param(p, "reason", "Reason", VENTURE_FIELD_KIND_STRING, FALSE);
	register_action(registry, "late_fee", "skip", "Skip late fee",
		"Decide not to charge this occurrence; nothing reaches the balance", p, TRUE, FALSE,
		fee_allowed, skip_invoke, self);
	add_param(p, "reason", "Reason", VENTURE_FIELD_KIND_STRING, FALSE);
	register_action(registry, "late_fee", "skip_notice", "Proceed without notice",
		"Let this fee proceed although no reminder stated it in advance", p, TRUE, FALSE,
		fee_allowed, skip_notice_invoke, self);
	add_param(p, "reason", "Reason", VENTURE_FIELD_KIND_STRING, TRUE);
	add_param(p, "note", "Note to the customer", VENTURE_FIELD_KIND_TEXT, FALSE);
	register_action(registry, "late_fee", "waive", "Waive late fee",
		"Reverse an applied fee with a waiver credit; the balance, statement and reports follow", p, TRUE, FALSE,
		fee_allowed, waive_invoke, self);
	add_param(p, "organization_id", "Organization", VENTURE_FIELD_KIND_INTEGER, FALSE);
	add_param(p, "ids", "Late fee ids", VENTURE_FIELD_KIND_STRING, TRUE);
	add_param(p, "decision", "approve or skip", VENTURE_FIELD_KIND_STRING, FALSE);
	add_param(p, "reason", "Reason", VENTURE_FIELD_KIND_STRING, FALSE);
	register_action(registry, "late_fee", "decide", "Approve or skip several",
		"Approve (or, with decision=skip, skip) several proposed fees, each its own decision", p, TRUE, TRUE,
		decide_allowed, decide_invoke, self);
	for (i = 0; paused[i] != NULL; i++)
	{
		add_param(p, "until", "Until", VENTURE_FIELD_KIND_DATETIME, FALSE);
		add_param(p, "reason", "Reason", VENTURE_FIELD_KIND_STRING, FALSE);
		register_action(registry, paused[i], "pause_late_fees", "Pause late fees",
			"Pause late fees until a date (the first day they may apply again), or indefinitely without one", p, TRUE, FALSE,
			pause_allowed, pause_invoke, self);
		register_action(registry, paused[i], "resume_late_fees", "Resume late fees",
			"End a late-fee pause", p, TRUE, FALSE, pause_allowed, resume_invoke, self);
	}
	add_param(p, "as_of", "As of", VENTURE_FIELD_KIND_DATETIME, FALSE);
	/* Not stageable: it writes nothing. */
	register_action(registry, "invoice", "late_fee_preview", "Late fee preview",
		"What the invoice's schedule would charge, as a full timeline; writes nothing", p, FALSE, FALSE,
		preview_allowed, preview_invoke, self);
}

/* --- The late_fees report ------------------------------------------------- */

typedef struct
{
	gchar *currency;
	gint64 applied_count, waived_count;
	VentureMoney *applied, *waived, *collected, *pending;
} Totals;

static void
totals_free(gpointer data)
{
	Totals *t = data;
	g_free(t->currency);
	venture_money_free(t->applied);
	venture_money_free(t->waived);
	venture_money_free(t->collected);
	venture_money_free(t->pending);
	g_free(t);
}

static gboolean
in_period(VentureDateRange *period, GDateTime *at)
{
	GDateTime *start, *end;
	if (period == NULL)
		return TRUE;
	if (at == NULL)
		return FALSE;
	start = venture_date_range_get_start(period);
	end = venture_date_range_get_end(period);
	return (start == NULL || g_date_time_compare(at, start) >= 0) && (end == NULL || g_date_time_compare(at, end) < 0);
}

static gboolean
bump(VentureMoney **total, const VentureMoney *amount, GError **error)
{
	VentureMoney *next = venture_money_add(*total, amount, error);
	if (next == NULL)
		return FALSE;
	venture_money_free(*total);
	*total = next;
	return TRUE;
}

static VentureReportResult *
late_fees_report(VentureContext *context, VentureDateRange *period, JsonObject *options, GError **error)
{
	VentureDatabase *db = venture_context_get_database(context);
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GHashTable) by_currency = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, totals_free);
	g_autoptr(GList) keys = NULL;
	g_autoptr(VentureReportResult) result = NULL;
	gint64 org = options != NULL && json_object_has_member(options, "organization_id") ?
		json_object_get_int_member(options, "organization_id") : venture_context_get_default_organization_id(context);
	GList *k;
	guint i;
	if (!module_on(error))
		return NULL;
	query = venture_query_new(VENTURE_TYPE_LATE_FEE);
	venture_query_set_organization(query, org);
	venture_query_set_include_deleted(query, TRUE);
	/* One past the bound, so a truncated total is refused rather than shown. */
	venture_query_set_limit(query, (guint)venture_aggregate_get_max_rows() + 1);
	if (!venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, error))
		return NULL;
	rows = venture_database_find(db, query, error);
	if (rows == NULL)
		return NULL;
	if (rows->len > (guint)venture_aggregate_get_max_rows())
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			"More than %d late fees match; narrow the period", venture_aggregate_get_max_rows());
		return NULL;
	}
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *fee = g_ptr_array_index(rows, i);
		g_autofree gchar *status = text(fee, "status");
		g_autoptr(VentureMoney) amount = NULL;
		g_autoptr(GDateTime) due_on = when(fee, "due-on");
		g_autoptr(GDateTime) decided = when(fee, "decided-at");
		Totals *t;
		g_object_get(fee, "amount", &amount, NULL);
		if (amount == NULL)
			continue;
		t = g_hash_table_lookup(by_currency, venture_money_get_currency(amount));
		if (t == NULL)
		{
			t = g_new0(Totals, 1);
			t->currency = g_strdup(venture_money_get_currency(amount));
			t->applied = venture_money_new(0, t->currency, venture_money_get_exponent(amount));
			t->waived = venture_money_copy(t->applied);
			t->collected = venture_money_copy(t->applied);
			t->pending = venture_money_copy(t->applied);
			g_hash_table_insert(by_currency, t->currency, t);
		}
		if (g_strcmp0(status, "applied") == 0 || g_strcmp0(status, "waived") == 0)
		{
			/* Applied in the period its adjustment is dated: the due day. */
			if (in_period(period, due_on))
			{
				t->applied_count++;
				if (!bump(&t->applied, amount, error))
					return NULL;
			}
			if (g_strcmp0(status, "waived") == 0 && in_period(period, decided))
			{
				t->waived_count++;
				if (!bump(&t->waived, amount, error))
					return NULL;
			}
			if (g_strcmp0(status, "applied") == 0)
			{
				/* Collected once the invoice carrying it is paid in full. */
				g_autoptr(VentureEntity) invoice = venture_database_get(db, VENTURE_TYPE_INVOICE, number(fee, "invoice-id"), NULL);
				g_autoptr(GDateTime) paid = invoice != NULL ? when(invoice, "paid-at") : NULL;
				if (paid != NULL && in_period(period, paid) && !bump(&t->collected, amount, error))
					return NULL;
			}
		}
		else if ((g_strcmp0(status, "proposed") == 0 || g_strcmp0(status, "awaiting_notice") == 0) && in_period(period, due_on) &&
			!bump(&t->pending, amount, error))
			return NULL;
	}
	result = venture_report_result_new("Late fees", period);
	venture_report_result_add_column(result, "currency", "Currency", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "applied_count", "Fees applied", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "applied", "Applied", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "waived_count", "Fees waived", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "waived", "Waived", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "collected", "Collected", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "pending", "Awaiting approval or notice", VENTURE_REPORT_COLUMN_MONEY);
	keys = g_hash_table_get_keys(by_currency);
	keys = g_list_sort(keys, (GCompareFunc)g_strcmp0);
	for (k = keys; k != NULL; k = k->next)
	{
		Totals *t = g_hash_table_lookup(by_currency, k->data);
		venture_report_result_begin_row(result);
		venture_report_result_set_text(result, "currency", t->currency);
		venture_report_result_set_number(result, "applied_count", (gdouble)t->applied_count);
		venture_report_result_set_money(result, "applied", t->applied);
		venture_report_result_set_number(result, "waived_count", (gdouble)t->waived_count);
		venture_report_result_set_money(result, "waived", t->waived);
		venture_report_result_set_money(result, "collected", t->collected);
		venture_report_result_set_money(result, "pending", t->pending);
	}
	venture_report_result_append_note(result, "Each currency is its own row and is never converted. Late-fee limits vary by jurisdiction; set the schedule caps for your location.");
	return g_steal_pointer(&result);
}

void
venture_late_fee_register_reports(VentureReportRegistry *registry)
{
	venture_report_registry_add(registry, VENTURE_REPORT(venture_func_report_new_classified(VENTURE_DATA_CLASS_TENANT,
		"late_fees", "Late fees", "Fees applied, waived and collected per period, per currency, and what awaits approval or notice",
		late_fees_report)));
}

/* --- Timeline ------------------------------------------------------------- */

static gchar *
describe(VentureEntity *fee)
{
	g_autofree gchar *status = text(fee, "status");
	g_autofree gchar *reason = text(fee, "reason");
	g_autoptr(VentureMoney) amount = NULL;
	g_autoptr(GDateTime) due_on = when(fee, "due-on");
	g_autofree gchar *shown = NULL;
	g_autofree gchar *day = due_on != NULL ? g_date_time_format(due_on, "%Y-%m-%d") : g_strdup("");
	gint64 tier = number(fee, "tier");
	g_object_get(fee, "amount", &amount, NULL);
	shown = amount != NULL ? venture_money_to_display_string(amount, TRUE) : g_strdup("");
	if (g_strcmp0(status, "applied") == 0)
		return g_strdup_printf("late fee %s applied (tier %" G_GINT64_FORMAT ", %s)", shown, tier, day);
	if (g_strcmp0(status, "proposed") == 0)
		return g_strdup_printf("late fee %s proposed for approval (tier %" G_GINT64_FORMAT ", %s)", shown, tier, day);
	if (g_strcmp0(status, "scheduled") == 0)
		return g_strdup_printf("late fee %s announced for %s", shown, day);
	if (g_strcmp0(status, "awaiting_notice") == 0)
		return g_strdup_printf("late fee %s due %s, waiting for its advance notice", shown, day);
	if (g_strcmp0(status, "waived") == 0)
		return g_strdup_printf("late fee %s waived: %s", shown, reason ? reason : "");
	return g_strdup_printf("late fee for %s %s%s%s", day, status ? status : "", reason ? ": " : "", reason ? reason : "");
}

static void
add_entry(VentureLateFeeTimelineAdd add, GPtrArray *events, GDateTime *at, gint64 id, gint64 invoice_id,
	const gchar *body, const gchar *status)
{
	g_autoptr(JsonBuilder) b = json_builder_new();
	g_autofree gchar *iso = at != NULL ? g_date_time_format_iso8601(at) : NULL;
	json_builder_begin_object(b);
	json_builder_set_member_name(b, "kind");
	json_builder_add_string_value(b, "late_fee");
	json_builder_set_member_name(b, "id");
	json_builder_add_int_value(b, id);
	json_builder_set_member_name(b, "invoice_id");
	json_builder_add_int_value(b, invoice_id);
	json_builder_set_member_name(b, "actor");
	json_builder_add_string_value(b, "late fees");
	json_builder_set_member_name(b, "body");
	json_builder_add_string_value(b, body);
	json_builder_set_member_name(b, "status");
	json_builder_add_string_value(b, status != NULL ? status : "");
	if (iso != NULL)
	{
		json_builder_set_member_name(b, "when");
		json_builder_add_string_value(b, iso);
	}
	json_builder_end_object(b);
	add(events, at, json_builder_get_root(b));
}

void
venture_late_fee_append_timeline(VentureContext *context, const gchar *target_type, gint64 target_id, guint limit,
	VentureLateFeeTimelineAdd add, GPtrArray *events)
{
	VentureDatabase *db;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	gboolean invoice = g_strcmp0(target_type, "invoice") == 0;
	guint i;
	g_return_if_fail(VENTURE_IS_CONTEXT(context));
	if ((!invoice && g_strcmp0(target_type, "company") != 0) || !venture_context_module_enabled(context, "late_fees"))
		return;
	db = venture_context_get_database(context);
	query = venture_query_new(VENTURE_TYPE_LATE_FEE);
	venture_query_add_filter_int(query, invoice ? "invoice-id" : "company-id", VENTURE_FILTER_OP_EQ, target_id, NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_DESCENDING, NULL);
	venture_query_set_limit(query, limit ? limit : 100);
	rows = venture_database_find(db, query, NULL);
	for (i = 0; rows != NULL && i < rows->len; i++)
	{
		VentureEntity *fee = g_ptr_array_index(rows, i);
		g_autofree gchar *body = describe(fee);
		g_autofree gchar *status = text(fee, "status");
		g_autoptr(GDateTime) decided = when(fee, "decided-at");
		GDateTime *at = decided != NULL ? decided : venture_entity_get_updated_at(fee);
		add_entry(add, events, at, venture_entity_get_id(fee), number(fee, "invoice-id"), body, status);
	}
	if (invoice)
	{
		/* What happens next, computed and written nowhere. */
		g_autoptr(VentureEntity) record = venture_database_get(db, VENTURE_TYPE_INVOICE, target_id, NULL);
		g_autoptr(GDateTime) now = venture_time_now();
		g_autofree gchar *paused = record != NULL ? venture_late_fee_service_pause_state(venture_late_fee_service_get(db), record, now) : NULL;
		g_autofree gchar *next = NULL;
		if (paused != NULL)
			add_entry(add, events, now, 0, target_id, paused, "paused");
		else if (record != NULL && !g_str_equal((next = venture_late_fee_service_notice_text(venture_late_fee_service_get(db), record, now)), ""))
		{
			g_autofree gchar *body = g_strdup_printf("next: %c%s", g_ascii_tolower(next[0]), next + 1);
			add_entry(add, events, now, 0, target_id, body, "scheduled");
		}
	}
}
