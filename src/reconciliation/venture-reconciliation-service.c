/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
#include "venture-reconciliation-private.h"

/* Optional banking seam, deliberately declared without banking headers. */
extern GPtrArray *venture_bank_transaction_candidates(VentureDatabase *db,
	VentureEntity *transaction, GError **error) __attribute__((weak));

struct _VentureReconciliationService { GObject parent_instance; GWeakRef context; };
G_DEFINE_FINAL_TYPE(VentureReconciliationService, venture_reconciliation_service, G_TYPE_OBJECT)
static void
service_get(GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
	if (id == 1) g_value_take_object(value, g_weak_ref_get(&VENTURE_RECONCILIATION_SERVICE(object)->context));
	else G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
}
static void
service_set(GObject *object, guint id, const GValue *value, GParamSpec *pspec)
{
	if (id == 1) g_weak_ref_set(&VENTURE_RECONCILIATION_SERVICE(object)->context, g_value_get_object(value));
	else G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
}
static void
service_free(GObject *object)
{
	g_weak_ref_clear(&VENTURE_RECONCILIATION_SERVICE(object)->context);
	G_OBJECT_CLASS(venture_reconciliation_service_parent_class)->finalize(object);
}
static void
venture_reconciliation_service_class_init(VentureReconciliationServiceClass *klass)
{
	GObjectClass *oc = G_OBJECT_CLASS(klass);
	oc->get_property = service_get; oc->set_property = service_set; oc->finalize = service_free;
	g_object_class_install_property(oc, 1, g_param_spec_object("context", "Context", "Owner (weak reference)",
		VENTURE_TYPE_CONTEXT, G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
}
static void venture_reconciliation_service_init(VentureReconciliationService *self) { g_weak_ref_init(&self->context, NULL); }
VentureReconciliationService *venture_reconciliation_service_new(VentureContext *context)
{ return g_object_new(VENTURE_TYPE_RECONCILIATION_SERVICE, "context", context, NULL); }

/* Without banking, plugin field metadata supplies a useful candidate seam. */
static GPtrArray *
collect_candidates(VentureContext *context, VentureEntity *transaction, GError **error)
{
	VentureDatabase *db = venture_context_get_database(context);
	g_autoptr(GPtrArray) result = g_ptr_array_new_with_free_func(g_object_unref);
	g_autofree GType *types = NULL;
	guint n, i, j;
	if (venture_bank_transaction_candidates != NULL &&
		g_str_equal(venture_entity_get_entity_name(transaction), "bank_transaction"))
		return venture_bank_transaction_candidates(db, transaction, error);
	types = venture_entity_registry_list_types(venture_context_get_entity_registry(context), &n);
	for (i = 0; i < n; i++)
	{
		g_autoptr(VentureEntity) prototype = g_object_new(types[i], NULL);
		g_autoptr(GPtrArray) specs = venture_entity_get_field_specs(prototype);
		g_autoptr(VentureQuery) query = NULL;
		g_autoptr(GPtrArray) rows = NULL;
		gboolean amount = FALSE, date = FALSE;
		for (j = 0; j < specs->len; j++)
		{
			VentureFieldSpec *spec = g_ptr_array_index(specs, j);
			if (venture_field_spec_get_flags(spec) & VENTURE_COLUMN_FLAG_SENSITIVE) continue;
			if (g_str_equal(venture_field_spec_get_name(spec), "amount") && venture_field_spec_get_kind(spec) == VENTURE_FIELD_KIND_MONEY) amount = TRUE;
			if (g_str_equal(venture_field_spec_get_name(spec), "date") && venture_field_spec_get_kind(spec) == VENTURE_FIELD_KIND_DATE) date = TRUE;
		}
		if (!amount || !date) continue;
		query = venture_query_new(types[i]);
		venture_query_add_filter_int(query, "organization-id", VENTURE_FILTER_OP_EQ, venture_entity_get_organization_id(transaction), NULL);
		venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
		venture_query_set_limit(query, 200);
		rows = venture_database_find(db, query, error);
		if (rows == NULL) return NULL;
		for (j = 0; j < rows->len; j++)
		{
			VentureEntity *candidate = g_ptr_array_index(rows, j);
			if (G_OBJECT_TYPE(candidate) == G_OBJECT_TYPE(transaction) && venture_entity_get_id(candidate) == venture_entity_get_id(transaction)) continue;
			g_ptr_array_add(result, g_object_ref(candidate));
		}
	}
	return g_steal_pointer(&result);
}
static gboolean
match_shape(GType type, GError **error)
{
	g_autoptr(VentureEntity) prototype = g_object_new(type, NULL);
	GObjectClass *klass = G_OBJECT_GET_CLASS(prototype);
	const gchar *names[] = { "transaction-id", "target-type", "target-id", "amount" };
	GType types[4];
	guint i;
	types[0] = G_TYPE_INT64; types[1] = G_TYPE_STRING; types[2] = G_TYPE_INT64; types[3] = VENTURE_TYPE_MONEY;
	for (i = 0; i < G_N_ELEMENTS(names); i++)
	{
		GParamSpec *pspec = g_object_class_find_property(klass, names[i]);
		if (pspec == NULL || G_PARAM_SPEC_VALUE_TYPE(pspec) != types[i] || !(pspec->flags & G_PARAM_WRITABLE))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "bank_match requires writable %s for reconciliation staging", names[i]);
			return FALSE;
		}
	}
	return TRUE;
}
JsonNode *
venture_reconciliation_service_suggest(VentureReconciliationService *self, const gchar *type,
	gint64 id, const gchar *matcher, gint threshold, const VentureActor *actor, const gchar *via, GError **error)
{
	g_autoptr(VentureContext) context = g_weak_ref_get(&self->context);
	g_autoptr(VentureEntity) transaction = NULL;
	g_autoptr(GPtrArray) candidates = NULL;
	g_autoptr(GPtrArray) suggestions = NULL;
	g_autoptr(VentureReconciliationRegistry) selected = NULL;
	g_autoptr(JsonBuilder) builder = json_builder_new();
	VentureReconciliationRegistry *registry;
	VentureDatabase *db;
	GType record_type, match_type;
	guint i;
	if (context == NULL || !venture_context_module_enabled(context, "reconciliation"))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "The reconciliation module is disabled"); return NULL;
	}
	if (type == NULL || id <= 0 || threshold < 0 || threshold > 100)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT, "A type, positive ID and threshold from 0 through 100 are required"); return NULL;
	}
	db = venture_context_get_database(context);
	record_type = venture_entity_registry_lookup(venture_context_get_entity_registry(context), type);
	if (record_type == G_TYPE_INVALID)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Unknown reconciliation transaction type"); return NULL;
	}
	transaction = venture_database_get(db, record_type, id, error);
	if (transaction == NULL || venture_entity_is_deleted(transaction))
	{
		if (error == NULL || *error == NULL)
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "The reconciliation transaction does not exist or was deleted");
		return NULL;
	}
	candidates = collect_candidates(context, transaction, error);
	if (candidates == NULL) return NULL;
	registry = venture_context_get_reconciliation_registry(context);
	if (matcher != NULL)
	{
		VentureReconciliationMatcher *implementation = venture_reconciliation_registry_lookup(registry, matcher);
		if (implementation == NULL)
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Unknown reconciliation matcher: %s", matcher); return NULL;
		}
		selected = venture_reconciliation_registry_new();
		venture_reconciliation_registry_add(selected, g_object_ref(implementation));
		registry = selected;
	}
	suggestions = venture_reconciliation_registry_suggest_all(registry, db, transaction, candidates, NULL, error);
	if (suggestions == NULL) return NULL;
	match_type = venture_entity_registry_lookup(venture_context_get_entity_registry(context), "bank_match");
	if (match_type != G_TYPE_INVALID && !match_shape(match_type, error)) return NULL;
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "suggestions");
	json_builder_begin_array(builder);
	for (i = 0; i < suggestions->len; i++)
	{
		g_autoptr(VentureEntity) candidate = NULL;
		g_autofree gchar *why = NULL;
		gint confidence, kind;
		g_object_get(g_ptr_array_index(suggestions, i), "candidate", &candidate, "confidence", &confidence, "rationale", &why, "kind", &kind, NULL);
		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "type"); json_builder_add_string_value(builder, venture_entity_get_entity_name(candidate));
		json_builder_set_member_name(builder, "id"); json_builder_add_int_value(builder, venture_entity_get_id(candidate));
		json_builder_set_member_name(builder, "confidence"); json_builder_add_int_value(builder, confidence);
		json_builder_set_member_name(builder, "rationale"); json_builder_add_string_value(builder, why != NULL ? why : "");
		json_builder_set_member_name(builder, "kind"); json_builder_add_string_value(builder, kind == VENTURE_MATCH_EXACT ? "exact" : kind == VENTURE_MATCH_PARTIAL ? "partial" : "none");
		if (match_type != G_TYPE_INVALID && confidence > threshold && kind != VENTURE_MATCH_NONE)
		{
			g_autoptr(VentureMoney) amount = venture_reconciliation_dup_field(transaction, "amount", VENTURE_TYPE_MONEY);
			g_autoptr(VentureEntity) match = NULL;
			VentureConfirmation *confirmation;
			if (amount == NULL)
			{
				g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "A transaction amount is required to stage a match"); return NULL;
			}
			match = g_object_new(match_type, "organization-id", venture_entity_get_organization_id(transaction),
				"transaction-id", id, "target-type", venture_entity_get_entity_name(candidate), "target-id", venture_entity_get_id(candidate), "amount", amount, NULL);
			if (!venture_entity_validate(match, error)) return NULL;
			confirmation = venture_confirmation_store_stage(venture_context_get_confirmations(context), VENTURE_AUDIT_ACTION_CREATE, match, NULL, actor, via, error);
			if (confirmation == NULL) return NULL;
			json_builder_set_member_name(builder, "confirmation");
			json_builder_add_value(builder, venture_confirmation_to_json(confirmation));
		}
		json_builder_end_object(builder);
	}
	json_builder_end_array(builder);
	json_builder_set_member_name(builder, "note");
	json_builder_add_string_value(builder, match_type == G_TYPE_INVALID ? "Suggestions only: bank_match is not registered. Nothing applied." : "Matches above the threshold await confirmation. Nothing applied.");
	json_builder_end_object(builder);
	return json_builder_get_root(builder);
}

JsonNode *
venture_reconciliation_service_request(VentureReconciliationService *self, JsonObject *input,
	const VentureActor *actor, const gchar *via, GError **error)
{
	const gchar *names[] = { "type", "id", "matcher", "threshold" };
	GType types[] = { G_TYPE_STRING, G_TYPE_INT64, G_TYPE_STRING, G_TYPE_INT64 };
	guint i;
	gint64 threshold = 80;
	for (i = 0; i < G_N_ELEMENTS(names); i++)
	{
		JsonNode *node = json_object_get_member(input, names[i]);
		if (node == NULL && i >= 2) continue;
		if (node == NULL || !JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != types[i])
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT, "Invalid reconciliation %s", names[i]); return NULL;
		}
	}
	if (json_object_has_member(input, "threshold")) threshold = json_object_get_int_member(input, "threshold");
	if (threshold < 0 || threshold > 100)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT, "Threshold must be between 0 and 100"); return NULL;
	}
	return venture_reconciliation_service_suggest(self, json_object_get_string_member(input, "type"),
		json_object_get_int_member(input, "id"), venture_json_object_get_string(input, "matcher", NULL),
		(gint)threshold, actor, via, error);
}
