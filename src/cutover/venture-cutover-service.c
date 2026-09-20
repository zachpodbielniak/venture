/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
#include <errno.h>
#include <string.h>

struct _VentureCutoverService
{
	GObject parent_instance;
	VentureDatabase *database;
	VentureEntity *writing;
};
G_DEFINE_FINAL_TYPE(VentureCutoverService, venture_cutover_service, G_TYPE_OBJECT)

static void
get_property(GObject *object, guint id, GValue *value, GParamSpec *spec)
{
	if (id == 1)
		g_value_set_object(value, VENTURE_CUTOVER_SERVICE(object)->database);
	else
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
}

static void
set_property(GObject *object, guint id, const GValue *value, GParamSpec *spec)
{
	VentureCutoverService *self = VENTURE_CUTOVER_SERVICE(object);
	if (id == 1)
	{
		self->database = g_value_get_object(value);
		if (self->database != NULL)
			g_object_add_weak_pointer(G_OBJECT(self->database), (gpointer *)&self->database);
	}
	else
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
}

static void
finalize(GObject *object)
{
	VentureCutoverService *self = VENTURE_CUTOVER_SERVICE(object);
	if (self->database != NULL)
		g_object_remove_weak_pointer(G_OBJECT(self->database), (gpointer *)&self->database);
	G_OBJECT_CLASS(venture_cutover_service_parent_class)->finalize(object);
}

static void
venture_cutover_service_class_init(VentureCutoverServiceClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	object_class->get_property = get_property;
	object_class->set_property = set_property;
	object_class->finalize = finalize;
	g_object_class_install_property(object_class, 1,
		g_param_spec_object("database", "Database", "Owning database", VENTURE_TYPE_DATABASE,
			G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
}

static void
venture_cutover_service_init(VentureCutoverService *self)
{
	(void)self;
}

VentureCutoverService *
venture_cutover_service_get(VentureDatabase *database)
{
	VentureCutoverService *self;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	self = g_object_get_data(G_OBJECT(database), "venture-cutover-service");
	if (self == NULL)
	{
		self = g_object_new(VENTURE_TYPE_CUTOVER_SERVICE, "database", database, NULL);
		g_object_set_data_full(G_OBJECT(database), "venture-cutover-service", self, g_object_unref);
	}
	return self;
}

static gboolean
refuse(GError **error, const gchar *message)
{
	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "VentureCutoverService: %s", message);
	return FALSE;
}

gboolean
venture_cutover_check_write(VentureDatabase *database, VentureEntity *record, gboolean removal, GError **error)
{
	const gchar *name;
	VentureCutoverService *self;
	(void)removal;
	if (record == NULL || database == NULL)
		return TRUE;
	name = venture_entity_get_entity_name(record);
	if (g_strcmp0(name, "accounting_cutover") != 0 && g_strcmp0(name, "accounting_cutover_row") != 0)
		return TRUE;
	self = venture_cutover_service_get(database);
	if (self->writing == record)
	{
		self->writing = NULL;
		return TRUE;
	}
	return refuse(error, "cutover batches must be changed through VentureCutoverService");
}

static gboolean
save_owned(VentureCutoverService *self, VentureEntity *record, const VentureActor *actor, GError **error)
{
	gboolean ok;
	self->writing = record;
	ok = venture_database_save(self->database, record, actor, error);
	self->writing = NULL;
	return ok;
}

static JsonObject *
payload_of(VentureEntity *cutover)
{
	g_autofree gchar *text = NULL;
	g_autoptr(JsonParser) parser = json_parser_new();
	g_object_get(cutover, "payload", &text, NULL);
	if (text == NULL || !json_parser_load_from_data(parser, text, -1, NULL) ||
		!JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser)))
		return NULL;
	return json_object_ref(json_node_get_object(json_parser_get_root(parser)));
}

static gboolean
add_row(VentureCutoverService *self, gint64 cutover_id, gint64 org, const gchar *source_id,
	const gchar *source_type, const gchar *status, const gchar *exception, const gchar *record_type,
	gint64 record_id, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureAccountingCutoverRow) row = venture_accounting_cutover_row_new();
	g_object_set(row, "cutover-id", cutover_id, "source-id", source_id, "source-type", source_type,
		"status", status, "exception", exception, "record-type", record_type, "record-id", record_id, NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(row), org);
	return save_owned(self, VENTURE_ENTITY(row), actor, error);
}

static gboolean
cutover_rows(VentureCutoverService *self, gint64 cutover_id, GPtrArray **rows, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ACCOUNTING_CUTOVER_ROW);
	venture_query_set_limit(query, 0);
	if (!venture_query_add_filter_int(query, "cutover-id", VENTURE_FILTER_OP_EQ, cutover_id, error))
		return FALSE;
	*rows = venture_database_find(self->database, query, error);
	return *rows != NULL;
}

static gboolean
array_nonempty(JsonObject *payload, const gchar *name)
{
	JsonNode *node = json_object_has_member(payload, name) ? json_object_get_member(payload, name) : NULL;
	return node != NULL && JSON_NODE_HOLDS_ARRAY(node) && json_array_get_length(json_node_get_array(node)) > 0;
}

/* Opening bills, credits and assets are refused only when the modules that
 * own them are switched off; a payload naming them must not silently lose
 * them. */
static gboolean
refuse_unimported_sections(JsonObject *payload, GError **error)
{
	VentureEntityRegistry *registry = venture_entity_registry_get_default();
	if (array_nonempty(payload, "open_ar") && (!venture_entity_registry_is_type_enabled(registry, "invoice") ||
		!venture_entity_registry_is_type_enabled(registry, "payment")))
		return refuse(error, "open_ar requires the invoicing and receivables modules; enable them or list the invoices under unsupported");
	if (array_nonempty(payload, "open_ap") && !venture_entity_registry_is_type_enabled(registry, "vendor_bill"))
		return refuse(error, "open_ap requires the payables module; enable it or list the bills under unsupported");
	if (array_nonempty(payload, "credits"))
	{
		JsonArray *credits = json_object_get_array_member(payload, "credits");
		guint i;
		gboolean need_vendor = FALSE, need_customer = FALSE;
		for (i = 0; i < json_array_get_length(credits); i++)
		{
			JsonNode *node = json_array_get_element(credits, i);
			JsonObject *row;
			JsonNode *kind;
			if (!JSON_NODE_HOLDS_OBJECT(node))
				continue;
			row = json_node_get_object(node);
			kind = json_object_has_member(row, "kind") ? json_object_get_member(row, "kind") : NULL;
			if ((kind != NULL && JSON_NODE_HOLDS_VALUE(kind) && json_node_get_value_type(kind) == G_TYPE_STRING &&
				g_strcmp0(json_node_get_string(kind), "vendor") == 0) ||
				(kind == NULL && json_object_has_member(row, "vendor_source_id")))
				need_vendor = TRUE;
			else
				need_customer = TRUE;
		}
		if (need_vendor && !venture_entity_registry_is_type_enabled(registry, "vendor_credit"))
			return refuse(error, "vendor credits require the payables module; enable it or list the credits under unsupported");
		if (need_customer && !venture_entity_registry_is_type_enabled(registry, "customer_credit"))
			return refuse(error, "customer credits require the receivables module; enable it or list the credits under unsupported");
	}
	if (array_nonempty(payload, "assets") && !venture_entity_registry_is_type_enabled(registry, "fixed_asset"))
		return refuse(error, "assets require the assets module; enable it or list the assets under unsupported");
	if (array_nonempty(payload, "inventory") && (!venture_entity_registry_is_type_enabled(registry, "inventory_item") ||
		!venture_entity_registry_is_type_enabled(registry, "inventory_cost_layer")))
		return refuse(error, "inventory requires the goods module; enable it or list the stock under unsupported");
	return TRUE;
}

#include "venture-cutover-payload.inc"
#include "venture-cutover-csv.inc"
#include "venture-cutover-openings.inc"
#include "venture-cutover-reconcile.inc"
#include "venture-cutover-rollback.inc"

/* The problems belonging to one source row, joined for its exception. */
static gchar *
row_exception(Plan *plan, const gchar *section, gint position, gboolean *has_error)
{
	g_autoptr(GString) text = g_string_new(NULL);
	guint i;
	*has_error = FALSE;
	for (i = 0; i < plan->problems->len; i++)
	{
		Problem *problem = g_ptr_array_index(plan->problems, i);
		if (g_strcmp0(problem->section, section) != 0 || problem->position != position)
			continue;
		if (!problem->warning)
			*has_error = TRUE;
		g_string_append_printf(text, "%s%s: %s", text->len > 0 ? "; " : "",
			problem->warning ? "warning" : "error", problem->message);
	}
	return text->len > 0 ? g_string_free(g_steal_pointer(&text), FALSE) : NULL;
}

/* Every write-free check a payload can be given before import. */
static Plan *
plan_validate(VentureCutoverService *self, gint64 org, JsonObject *payload, GError **error)
{
	Plan *plan = plan_build(self, org, payload, error);
	if (plan == NULL)
		return NULL;
	plan_check_database(plan);
	plan_check_credit_caps(plan);
	plan_predict(plan);
	return plan;
}

static void
append_plan_report(Plan *plan, GString *report)
{
	static const gchar *const sections[] = { "open_ar", "open_ap", "credits", "bank_balances", "assets", "trial_balance", "inventory" };
	GPtrArray *rows[7];
	guint i, errors = plan_error_count(plan);
	g_autofree gchar *cutoff = g_date_time_format_iso8601(plan->cutoff);
	rows[0] = plan->open_ar;
	rows[1] = plan->open_ap;
	rows[2] = plan->credits;
	rows[3] = plan->bank;
	rows[4] = plan->assets;
	rows[5] = plan->trial_balance;
	rows[6] = plan->inventory;
	g_string_append_printf(report, "Source %s, cutoff %s: opening balances as of the end of the day before.\n",
		plan->source, cutoff);
	g_string_append(report, "Rows:");
	for (i = 0; i < G_N_ELEMENTS(sections); i++)
		g_string_append_printf(report, " %s %u%s", sections[i], rows[i]->len, i + 1 < G_N_ELEMENTS(sections) ? "," : "\n");
	if (plan->problems->len == 0)
	{
		g_string_append(report, "No problems found.\n");
		return;
	}
	g_string_append_printf(report, "Problems: %u error%s, %u warning%s%s\n", errors, errors == 1 ? "" : "s",
		plan->problems->len - errors, plan->problems->len - errors == 1 ? "" : "s",
		errors > 0 ? " (import is refused until the errors are fixed)" : "");
	for (i = 0; i < plan->problems->len; i++)
	{
		Problem *problem = g_ptr_array_index(plan->problems, i);
		g_autofree gchar *line = problem_format(problem);
		g_string_append_printf(report, "- %s %s\n", problem->warning ? "WARNING" : "ERROR", line);
	}
}

VentureEntity *
venture_cutover_service_preview(VentureCutoverService *self, gint64 organization_id,
	JsonObject *payload, const VentureActor *actor, GError **error)
{
	static const struct { const gchar *section; const gchar *type; } kinds[] = {
		{ "open_ar", "open_ar" }, { "open_ap", "open_ap" }, { "credits", "credit" },
		{ "bank_balances", "bank" }, { "assets", "asset" }, { "trial_balance", "trial_balance" },
		{ "inventory", "inventory" }
	};
	g_autoptr(VentureAccountingCutover) cutover = NULL;
	g_autoptr(Plan) plan = NULL;
	g_autofree gchar *payload_text = NULL;
	g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);
	g_autoptr(GString) report = g_string_new("Cutover preview\n");
	JsonNode *unsupported;
	guint k, i;
	if (organization_id <= 0)
	{
		refuse(error, "a cutover needs one legal entity; choose an organization first");
		return NULL;
	}
	plan = plan_validate(self, organization_id, payload, error);
	if (plan == NULL)
		return NULL;
	json_node_set_object(node, json_object_ref(payload));
	payload_text = venture_json_to_string(node, FALSE);
	append_plan_report(plan, report);
	unsupported = member(payload, "unsupported");
	if (unsupported != NULL && JSON_NODE_HOLDS_ARRAY(unsupported))
	{
		JsonArray *list = json_node_get_array(unsupported);
		g_string_append(report, "Unsupported source fields:\n");
		for (i = 0; i < json_array_get_length(list); i++)
		{
			JsonNode *item = json_array_get_element(list, i);
			if (JSON_NODE_HOLDS_VALUE(item) && json_node_get_value_type(item) == G_TYPE_STRING)
				g_string_append_printf(report, "- %s\n", json_node_get_string(item));
		}
	}
	if (!venture_database_begin(self->database, error))
		return NULL;
	cutover = venture_accounting_cutover_new();
	g_object_set(cutover, "source", plan->source, "cutoff", plan->cutoff, "state", "preview",
		"payload", payload_text, "reconciliation-report", report->str, NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(cutover), organization_id);
	if (!save_owned(self, VENTURE_ENTITY(cutover), actor, error))
		goto fail;
	/* One row per source row, carrying its located problems, so the batch
	 * page lists exactly which rows need fixing. */
	for (k = 0; k < G_N_ELEMENTS(kinds); k++)
	{
		JsonArray *array = plan_section(plan, kinds[k].section);
		for (i = 0; array != NULL && i < json_array_get_length(array); i++)
		{
			JsonNode *element = json_array_get_element(array, i);
			g_autofree gchar *exception = NULL;
			g_autofree gchar *source_id = NULL;
			gboolean has_error;
			exception = row_exception(plan, kinds[k].section, (gint)i, &has_error);
			if (JSON_NODE_HOLDS_OBJECT(element))
			{
				JsonNode *id = member(json_node_get_object(element), g_strcmp0(kinds[k].type, "trial_balance") == 0
					? "account_code" : "source_id");
				if (id != NULL && JSON_NODE_HOLDS_VALUE(id) && json_node_get_value_type(id) == G_TYPE_STRING)
					source_id = g_strdup(json_node_get_string(id));
			}
			/* A row without a usable id is still listed, by position. */
			if (source_id == NULL || *source_id == '\0')
			{
				g_free(source_id);
				source_id = g_strdup_printf("%s[%u]", kinds[k].section, i);
			}
			if (!add_row(self, venture_entity_get_id(VENTURE_ENTITY(cutover)), organization_id, source_id,
				kinds[k].type, has_error ? "exception" : "preview", exception, NULL, 0, actor, error))
				goto fail;
		}
	}
	if (!venture_database_commit(self->database, error))
		return NULL;
	return VENTURE_ENTITY(g_steal_pointer(&cutover));
fail:
	venture_database_rollback(self->database);
	return NULL;
}

static Plan *
plan_of(VentureCutoverService *self, VentureEntity *cutover, GError **error)
{
	g_autoptr(JsonObject) payload = payload_of(cutover);
	g_autoptr(Plan) plan = NULL;
	if (payload == NULL)
	{
		refuse(error, "cutover payload is missing");
		return NULL;
	}
	plan = plan_build(self, venture_entity_get_organization_id(cutover), payload, error);
	if (plan == NULL)
		return NULL;
	plan->cutover_id = venture_entity_get_id(cutover);
	return g_steal_pointer(&plan);
}

static gboolean
venture_cutover_service_import_impl(VentureCutoverService *self, VentureAccountingCutover *cutover,
	const VentureActor *actor, GError **error)
{
	g_autoptr(Plan) plan = NULL;
	g_autoptr(PartyIndex) parties = NULL;
	g_autofree gchar *state = NULL;
	Import import;
	g_object_get(cutover, "state", &state, NULL);
	if (g_strcmp0(state, "preview") != 0 && g_strcmp0(state, "imported") != 0)
		return refuse(error, "import requires a previewed batch");
	plan = plan_of(self, VENTURE_ENTITY(cutover), error);
	if (plan == NULL)
		return FALSE;
	/* Validate everything before the first write: a refusal names every bad
	 * row at once and leaves nothing behind. */
	plan_check_database(plan);
	if (!plan_refuse_errors(plan, error))
		return FALSE;
	if (!venture_database_begin(self->database, error))
		return FALSE;
	parties = party_index_build(self, plan->org, error);
	if (parties == NULL)
		goto fail;
	import.plan = plan;
	import.cutover = VENTURE_ENTITY(cutover);
	import.cutover_id = venture_entity_get_id(VENTURE_ENTITY(cutover));
	import.parties = parties;
	import.actor = actor;
	import.clearing = clearing_account(plan, TRUE, actor, error);
	if (import.clearing == 0 ||
		!import_chart(&import, error) ||
		!import_open_ar(&import, error) ||
		!import_open_ap(&import, error) ||
		!import_credits(&import, error) ||
		!import_bank(&import, error) ||
		!import_assets(&import, error) ||
		!import_inventory(&import, error) ||
		!import_trial_balance(&import, error))
		goto fail;
	g_object_set(cutover, "state", "imported", NULL);
	if (!save_owned(self, VENTURE_ENTITY(cutover), actor, error) ||
		!venture_database_commit(self->database, error))
		goto fail;
	return TRUE;
fail:
	venture_database_rollback(self->database);
	return FALSE;
}

static gboolean
append_report(VentureCutoverService *self, VentureEntity *cutover, const gchar *state, GString *block,
	const VentureActor *actor, GError **error)
{
	g_autofree gchar *report = NULL;
	g_autofree gchar *combined = NULL;
	g_object_get(cutover, "reconciliation-report", &report, NULL);
	combined = g_strconcat(report != NULL ? report : "", block->str, NULL);
	g_object_set(cutover, "reconciliation-report", combined, NULL);
	if (state != NULL)
		g_object_set(cutover, "state", state, NULL);
	return save_owned(self, cutover, actor, error);
}

/* Runs reconcile, appends its evidence either way and sets @passed. */
static gboolean
reconcile_and_record(VentureCutoverService *self, VentureEntity *cutover, const gchar *passed_state,
	gboolean *passed, gchar **summary, const VentureActor *actor, GError **error)
{
	g_autoptr(Plan) plan = plan_of(self, cutover, error);
	g_autoptr(GString) block = g_string_new(NULL);
	if (plan == NULL || !reconcile_run(self, cutover, plan, block, passed, summary, error))
		return FALSE;
	return append_report(self, cutover, *passed ? passed_state : NULL, block, actor, error);
}

gboolean
venture_cutover_service_reconcile(VentureCutoverService *self, VentureAccountingCutover *cutover,
	const VentureActor *actor, GError **error)
{
	g_autofree gchar *state = NULL;
	g_autofree gchar *summary = NULL;
	gboolean passed = FALSE;
	g_object_get(cutover, "state", &state, NULL);
	if (g_strcmp0(state, "imported") != 0 && g_strcmp0(state, "reconciled") != 0)
		return refuse(error, "reconcile after import");
	/* A failure is evidence too: it is appended to the report and saved
	 * before the refusal, and a reconciled batch that drifted falls back to
	 * imported so it cannot be activated on stale figures. */
	if (!reconcile_and_record(self, VENTURE_ENTITY(cutover), "reconciled", &passed, &summary, actor, error))
		return FALSE;
	if (!passed)
	{
		if (g_strcmp0(state, "reconciled") == 0)
		{
			g_object_set(cutover, "state", "imported", NULL);
			if (!save_owned(self, VENTURE_ENTITY(cutover), actor, error))
				return FALSE;
		}
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"VentureCutoverService: reconcile failed: %s", summary);
		return FALSE;
	}
	return TRUE;
}

/*
 * Activation re-runs reconcile against the books as they are now, then closes
 * every open fiscal period that ends at or before the cutoff so nothing can
 * later be posted underneath the opening balances. Reconciliation at 9 and
 * activation at 5 are not the same books.
 */
static gboolean
close_periods_through(VentureCutoverService *self, VentureEntity *cutover, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) periods = NULL;
	g_autoptr(GDateTime) cutoff = NULL;
	guint i;
	if (!venture_entity_registry_is_type_enabled(venture_entity_registry_get_default(), "fiscal_period"))
		return TRUE;
	g_object_get(cutover, "cutoff", &cutoff, NULL);
	query = venture_query_new(VENTURE_TYPE_FISCAL_PERIOD);
	venture_query_set_organization(query, venture_entity_get_organization_id(cutover));
	venture_query_set_limit(query, 0);
	if (!venture_query_add_order(query, "start-at", VENTURE_SORT_ASCENDING, error))
		return FALSE;
	periods = venture_database_find(self->database, query, error);
	if (periods == NULL)
		return FALSE;
	for (i = 0; i < periods->len; i++)
	{
		VentureEntity *period = g_ptr_array_index(periods, i);
		g_autoptr(GDateTime) end = NULL;
		gint state;
		g_object_get(period, "end-at", &end, "state", &state, NULL);
		if (state != VENTURE_PERIOD_OPEN || end == NULL || g_date_time_compare(end, cutoff) > 0)
			continue;
		g_object_set(period, "state", VENTURE_PERIOD_CLOSED, NULL);
		if (!venture_database_save(self->database, period, actor, error))
			return FALSE;
	}
	return TRUE;
}

gboolean
venture_cutover_service_activate(VentureCutoverService *self, VentureAccountingCutover *cutover,
	const VentureActor *actor, GError **error)
{
	g_autofree gchar *state = NULL;
	g_autofree gchar *summary = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	gboolean passed = FALSE;
	g_object_get(cutover, "state", &state, NULL);
	if (g_strcmp0(state, "reconciled") != 0)
		return refuse(error, "activate after reconciliation");
	if (!reconcile_and_record(self, VENTURE_ENTITY(cutover), NULL, &passed, &summary, actor, error))
		return FALSE;
	if (!passed)
	{
		g_object_set(cutover, "state", "imported", NULL);
		if (!save_owned(self, VENTURE_ENTITY(cutover), actor, error))
			return FALSE;
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"VentureCutoverService: activation refused, the books no longer reconcile: %s", summary);
		return FALSE;
	}
	if (!venture_database_begin(self->database, error))
		return FALSE;
	if (!close_periods_through(self, VENTURE_ENTITY(cutover), actor, error))
		goto fail;
	g_object_set(cutover, "state", "active", "activated-at", now, NULL);
	if (!save_owned(self, VENTURE_ENTITY(cutover), actor, error) || !venture_database_commit(self->database, error))
		goto fail;
	return TRUE;
fail:
	venture_database_rollback(self->database);
	return FALSE;
}

GPtrArray *
venture_cutover_service_rollback_preflight(VentureCutoverService *self, VentureAccountingCutover *cutover,
	GError **error)
{
	g_autoptr(GPtrArray) blockers = g_ptr_array_new_with_free_func(g_free);
	g_autofree gchar *state = NULL;
	Rollback walk;
	gboolean ok;
	g_return_val_if_fail(VENTURE_IS_CUTOVER_SERVICE(self), NULL);
	g_object_get(cutover, "state", &state, NULL);
	if (g_strcmp0(state, "active") == 0)
	{
		g_ptr_array_add(blockers, g_strdup("the batch is active; an activated cutover is the books and cannot be rolled back"));
		return g_steal_pointer(&blockers);
	}
	if (g_strcmp0(state, "rolled_back") == 0)
		return g_steal_pointer(&blockers);
	memset(&walk, 0, sizeof(walk));
	rollback_prepare(&walk, self, VENTURE_ENTITY(cutover), TRUE, blockers, NULL);
	ok = rollback_walk(&walk, error);
	rollback_clear(&walk);
	return ok ? g_steal_pointer(&blockers) : NULL;
}

static gchar *
blockers_text(GPtrArray *blockers)
{
	g_autoptr(GString) text = g_string_new(NULL);
	guint i;
	for (i = 0; i < blockers->len; i++)
		g_string_append_printf(text, "\n- %s", (const gchar *)g_ptr_array_index(blockers, i));
	return g_string_free(g_steal_pointer(&text), FALSE);
}

static gboolean
venture_cutover_service_rollback_impl(VentureCutoverService *self, VentureAccountingCutover *cutover,
	const VentureActor *actor, GError **error)
{
	g_autofree gchar *state = NULL;
	g_autoptr(GPtrArray) blockers = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	Rollback walk;
	guint i;
	g_object_get(cutover, "state", &state, NULL);
	/* Retrying rollback must never reverse the reversal and recreate cash. */
	if (g_strcmp0(state, "rolled_back") == 0)
		return TRUE;
	blockers = venture_cutover_service_rollback_preflight(self, cutover, error);
	if (blockers == NULL)
		return FALSE;
	if (blockers->len > 0)
	{
		g_autofree gchar *text = blockers_text(blockers);
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"VentureCutoverService: rollback is blocked; nothing was changed:%s", text);
		return FALSE;
	}
	if (!venture_database_begin(self->database, error))
		return FALSE;
	memset(&walk, 0, sizeof(walk));
	rollback_prepare(&walk, self, VENTURE_ENTITY(cutover), FALSE, blockers, actor);
	if (!rollback_walk(&walk, error))
	{
		rollback_clear(&walk);
		goto fail;
	}
	rollback_clear(&walk);
	if (!cutover_rows(self, venture_entity_get_id(VENTURE_ENTITY(cutover)), &rows, error))
		goto fail;
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(rows, i);
		g_object_set(row, "status", "rolled_back", NULL);
		if (!save_owned(self, row, actor, error))
			goto fail;
	}
	g_object_set(cutover, "state", "rolled_back", NULL);
	if (!save_owned(self, VENTURE_ENTITY(cutover), actor, error) ||
		!venture_database_commit(self->database, error))
		goto fail;
	return TRUE;
fail:
	venture_database_rollback(self->database);
	return FALSE;
}

static gboolean
cutover_allowed(VentureAction *action, VentureEntity *entity, const VentureActor *actor, GError **error)
{
	(void)action;
	(void)entity;
	(void)actor;
	(void)error;
	return TRUE;
}

static VentureEntity *
record_preflight(VentureCutoverService *service, VentureEntity *entity, const VentureActor *actor, GError **error)
{
	g_autoptr(GPtrArray) blockers = venture_cutover_service_rollback_preflight(service, VENTURE_ACCOUNTING_CUTOVER(entity), error);
	g_autoptr(GString) block = g_string_new(NULL);
	g_autoptr(GDateTime) now = venture_time_now();
	g_autofree gchar *stamp = g_date_time_format_iso8601(now);
	guint i;
	if (blockers == NULL)
		return NULL;
	g_string_append_printf(block, "\n== Rollback preflight %s ==\n", stamp);
	if (blockers->len == 0)
		g_string_append(block, "No blockers: rollback can unwind every imported record.\n");
	for (i = 0; i < blockers->len; i++)
		g_string_append_printf(block, "BLOCKER %s\n", (const gchar *)g_ptr_array_index(blockers, i));
	if (!append_report(service, entity, NULL, block, actor, error))
		return NULL;
	return g_object_ref(entity);
}

static VentureEntity *
cutover_invoke(VentureAction *action, VentureEntity *entity, GHashTable *params,
	const VentureActor *actor, GError **error)
{
	g_autofree gchar *name = NULL;
	VentureCutoverService *service = venture_action_get_data(action);
	g_object_get(action, "name", &name, NULL);
	if (g_strcmp0(name, "import") == 0)
		return venture_cutover_service_import(service, VENTURE_ACCOUNTING_CUTOVER(entity), actor, error) ? g_object_ref(entity) : NULL;
	if (g_strcmp0(name, "reconcile") == 0)
		return venture_cutover_service_reconcile(service, VENTURE_ACCOUNTING_CUTOVER(entity), actor, error) ? g_object_ref(entity) : NULL;
	if (g_strcmp0(name, "activate") == 0)
		return venture_cutover_service_activate(service, VENTURE_ACCOUNTING_CUTOVER(entity), actor, error) ? g_object_ref(entity) : NULL;
	if (g_strcmp0(name, "rollback_preflight") == 0)
		return record_preflight(service, entity, actor, error);
	if (g_strcmp0(name, "rollback") == 0)
		return venture_cutover_service_rollback(service, VENTURE_ACCOUNTING_CUTOVER(entity), actor, error) ? g_object_ref(entity) : NULL;
	(void)params;
	return NULL;
}

void
venture_cutover_actions_register(VentureDatabase *database)
{
	VentureActionRegistry *registry = venture_database_get_action_registry(database);
	static const gchar *const names[] = { "import", "reconcile", "activate", "rollback_preflight", "rollback" };
	guint i;
	for (i = 0; i < G_N_ELEMENTS(names); i++)
	{
		g_autoptr(VentureAction) action = g_object_new(VENTURE_TYPE_ACTION, "data-class", VENTURE_DATA_CLASS_TENANT, "type-name", "accounting_cutover",
			"name", names[i], "label", names[i], "description", "Cutover batch action",
			"stageable", FALSE, "service-transaction", TRUE, "roles", VENTURE_USER_ROLE_EDITOR, NULL);
		g_autoptr(GError) error = NULL;
		venture_action_registry_register(registry, action, cutover_allowed, cutover_invoke,
			venture_cutover_service_get(database), NULL, &error);
	}
}

/* Bind consent before this operation creates derived rows or enters nested
 * transactions. All generated financial effects share this root proposal. */
gboolean
venture_cutover_service_import(VentureCutoverService *self, VentureAccountingCutover *cutover,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureAccountingOperation) operation = NULL;
	VentureDatabase * db = self->database;
	GVariantBuilder arguments;
	gboolean result;
	if (db == NULL)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Database is unavailable");
		return FALSE;
	}
	g_return_val_if_fail(VENTURE_IS_ENTITY(cutover), FALSE);
	g_variant_builder_init(&arguments, G_VARIANT_TYPE_VARDICT);
	operation = venture_accounting_operation_begin(db, "cutover-import", VENTURE_ENTITY(cutover), NULL,
		g_variant_builder_end(&arguments), venture_entity_get_organization_id(VENTURE_ENTITY(cutover)), actor, error);
	if (operation == NULL)
		return FALSE;
	result = venture_cutover_service_import_impl(self, cutover, actor, error);
	if (!result)
		return FALSE;
	if (!venture_accounting_operation_finish(operation, error))
		return FALSE;
	return result;
}

/* Bind consent before this operation creates derived rows or enters nested
 * transactions. All generated financial effects share this root proposal. */
gboolean
venture_cutover_service_rollback(VentureCutoverService *self, VentureAccountingCutover *cutover,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureAccountingOperation) operation = NULL;
	VentureDatabase * db = self->database;
	GVariantBuilder arguments;
	gboolean result;
	if (db == NULL)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Database is unavailable");
		return FALSE;
	}
	g_return_val_if_fail(VENTURE_IS_ENTITY(cutover), FALSE);
	g_variant_builder_init(&arguments, G_VARIANT_TYPE_VARDICT);
	operation = venture_accounting_operation_begin(db, "cutover-rollback", VENTURE_ENTITY(cutover), NULL,
		g_variant_builder_end(&arguments), venture_entity_get_organization_id(VENTURE_ENTITY(cutover)), actor, error);
	if (operation == NULL)
		return FALSE;
	result = venture_cutover_service_rollback_impl(self, cutover, actor, error);
	if (!result)
		return FALSE;
	if (!venture_accounting_operation_finish(operation, error))
		return FALSE;
	return result;
}
