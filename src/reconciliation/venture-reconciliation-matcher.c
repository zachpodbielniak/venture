/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

GType
venture_match_kind_get_type(void)
{
	static gsize type;
	if (g_once_init_enter(&type))
	{
		static const GEnumValue values[] = {
			{ VENTURE_MATCH_NONE, "VENTURE_MATCH_NONE", "none" },
			{ VENTURE_MATCH_EXACT, "VENTURE_MATCH_EXACT", "exact" },
			{ VENTURE_MATCH_PARTIAL, "VENTURE_MATCH_PARTIAL", "partial" },
			{ 0, NULL, NULL }
		};
		GType registered = g_enum_register_static("VentureMatchKind", values);
		g_once_init_leave(&type, registered);
	}
	return (GType)type;
}

struct _VentureMatchSuggestion {
	GObject parent_instance;
	VentureEntity *candidate;
	gint confidence;
	gchar *rationale;
	VentureMatchKind kind;
};
G_DEFINE_FINAL_TYPE(VentureMatchSuggestion, venture_match_suggestion, G_TYPE_OBJECT)

static void
suggestion_get(GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
	VentureMatchSuggestion *self = VENTURE_MATCH_SUGGESTION(object);
	switch (id) {
	case 1: g_value_set_object(value, self->candidate); break;
	case 2: g_value_set_int(value, self->confidence); break;
	case 3: g_value_set_string(value, self->rationale); break;
	case 4: g_value_set_enum(value, self->kind); break;
	default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
	}
}
static void
suggestion_set(GObject *object, guint id, const GValue *value, GParamSpec *pspec)
{
	VentureMatchSuggestion *self = VENTURE_MATCH_SUGGESTION(object);
	switch (id) {
	case 1: g_set_object(&self->candidate, g_value_get_object(value)); break;
	case 2: self->confidence = g_value_get_int(value); break;
	case 3: g_free(self->rationale); self->rationale = g_value_dup_string(value); break;
	case 4: self->kind = g_value_get_enum(value); break;
	default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
	}
}
static void
suggestion_finalize(GObject *object)
{
	VentureMatchSuggestion *self = VENTURE_MATCH_SUGGESTION(object);
	g_clear_object(&self->candidate);
	g_free(self->rationale);
	G_OBJECT_CLASS(venture_match_suggestion_parent_class)->finalize(object);
}
static void
venture_match_suggestion_class_init(VentureMatchSuggestionClass *klass)
{
	GObjectClass *oc = G_OBJECT_CLASS(klass);
	GParamFlags flags = G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS;
	oc->get_property = suggestion_get;
	oc->set_property = suggestion_set;
	oc->finalize = suggestion_finalize;
	g_object_class_install_property(oc, 1, g_param_spec_object("candidate", "Candidate", "Proposed book record", VENTURE_TYPE_ENTITY, flags));
	g_object_class_install_property(oc, 2, g_param_spec_int("confidence", "Confidence", "Score", 0, 100, 0, flags));
	g_object_class_install_property(oc, 3, g_param_spec_string("rationale", "Rationale", "Reason", "", flags));
	g_object_class_install_property(oc, 4, g_param_spec_enum("kind", "Kind", "Classification", VENTURE_TYPE_MATCH_KIND, VENTURE_MATCH_NONE, flags));
}
static void
venture_match_suggestion_init(VentureMatchSuggestion *self)
{
	(void)self;
}
VentureMatchSuggestion *
venture_match_suggestion_new(VentureEntity *candidate, gint confidence,
	const gchar *rationale, VentureMatchKind kind)
{
	return g_object_new(VENTURE_TYPE_MATCH_SUGGESTION, "candidate", candidate,
		"confidence", CLAMP(confidence, 0, 100), "rationale", rationale, "kind", kind, NULL);
}

G_DEFINE_INTERFACE(VentureReconciliationMatcher, venture_reconciliation_matcher, G_TYPE_OBJECT)
static void
venture_reconciliation_matcher_default_init(VentureReconciliationMatcherInterface *iface)
{
	/** VentureReconciliationMatcher:name:
	 * Stable implementation key used by the registry and CLI.
	 */
	g_object_interface_install_property(iface, g_param_spec_string("name", "Name",
		"Registry key", NULL, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));
}
GPtrArray *
venture_reconciliation_matcher_suggest(VentureReconciliationMatcher *self, VentureDatabase *db,
	VentureEntity *transaction, GPtrArray *candidates, GCancellable *cancellable, GError **error)
{
	g_return_val_if_fail(VENTURE_IS_RECONCILIATION_MATCHER(self), NULL);
	g_return_val_if_fail(VENTURE_IS_DATABASE(db), NULL);
	g_return_val_if_fail(VENTURE_IS_ENTITY(transaction), NULL);
	g_return_val_if_fail(NULL != candidates, NULL);
	if (g_cancellable_set_error_if_cancelled(cancellable, error))
		return NULL;
	if (NULL == VENTURE_RECONCILIATION_MATCHER_GET_IFACE(self)->suggest)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_UNSUPPORTED, "Matcher has no synchronous implementation");
		return NULL;
	}
	return VENTURE_RECONCILIATION_MATCHER_GET_IFACE(self)->suggest(self, db, transaction, candidates, cancellable, error);
}

typedef struct {
	VentureDatabase *db;
	VentureEntity *transaction;
	GPtrArray *candidates;
} SuggestTask;
static void
suggest_task_free(gpointer data)
{
	SuggestTask *task = data;
	g_object_unref(task->db);
	g_object_unref(task->transaction);
	g_ptr_array_unref(task->candidates);
	g_free(task);
}
static gboolean
suggest_idle(gpointer data)
{
	GTask *task = data;
	SuggestTask *args = g_task_get_task_data(task);
	g_autoptr(GError) error = NULL;
	GPtrArray *result = venture_reconciliation_matcher_suggest(g_task_get_source_object(task),
		args->db, args->transaction, args->candidates, g_task_get_cancellable(task), &error);
	if (result != NULL)
		g_task_return_pointer(task, result, (GDestroyNotify)g_ptr_array_unref);
	else
		g_task_return_error(task, g_steal_pointer(&error));
	return G_SOURCE_REMOVE;
}
void
venture_reconciliation_matcher_suggest_async(VentureReconciliationMatcher *self, VentureDatabase *db,
	VentureEntity *transaction, GPtrArray *candidates, GCancellable *cancellable,
	GAsyncReadyCallback callback, gpointer user_data)
{
	GTask *task;
	SuggestTask *args;
	GSource *source;
	guint i;
	if (VENTURE_RECONCILIATION_MATCHER_GET_IFACE(self)->suggest_async != NULL)
	{
		VENTURE_RECONCILIATION_MATCHER_GET_IFACE(self)->suggest_async(self, db, transaction, candidates, cancellable, callback, user_data);
		return;
	}
	task = g_task_new(self, cancellable, callback, user_data);
	g_task_set_source_tag(task, venture_reconciliation_matcher_suggest_async);
	args = g_new0(SuggestTask, 1);
	args->db = g_object_ref(db);
	args->transaction = g_object_ref(transaction);
	args->candidates = g_ptr_array_new_with_free_func(g_object_unref);
	for (i = 0; i < candidates->len; i++)
		g_ptr_array_add(args->candidates, g_object_ref(g_ptr_array_index(candidates, i)));
	g_task_set_task_data(task, args, suggest_task_free);
	source = g_idle_source_new();
	g_source_set_callback(source, suggest_idle, task, g_object_unref);
	g_source_attach(source, g_task_get_context(task));
	g_source_unref(source);
}
GPtrArray *
venture_reconciliation_matcher_suggest_finish(VentureReconciliationMatcher *self, GAsyncResult *result, GError **error)
{
	if (g_async_result_is_tagged(result, venture_reconciliation_matcher_suggest_async))
	{
		g_return_val_if_fail(g_task_is_valid(result, self), NULL);
		return g_task_propagate_pointer(G_TASK(result), error);
	}
	return VENTURE_RECONCILIATION_MATCHER_GET_IFACE(self)->suggest_finish(self, result, error);
}
