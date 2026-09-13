/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

static gchar *
matcher_name(VentureReconciliationMatcher *matcher)
{
	gchar *name = NULL;
	g_object_get(matcher, "name", &name, NULL);
	return name;
}

struct _VentureReconciliationRegistry
{
	GObject parent_instance;
	GHashTable *rules;
};
G_DEFINE_FINAL_TYPE(VentureReconciliationRegistry, venture_reconciliation_registry, G_TYPE_OBJECT)

static void
venture_reconciliation_registry_finalize(GObject *object)
{
	g_hash_table_unref(VENTURE_RECONCILIATION_REGISTRY(object)->rules);
	G_OBJECT_CLASS(venture_reconciliation_registry_parent_class)->finalize(object);
}
static void
venture_reconciliation_registry_class_init(VentureReconciliationRegistryClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = venture_reconciliation_registry_finalize;
}
static void
venture_reconciliation_registry_init(VentureReconciliationRegistry *self)
{
	self->rules = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
}
VentureReconciliationRegistry *
venture_reconciliation_registry_new(void)
{
	return g_object_new(VENTURE_TYPE_RECONCILIATION_REGISTRY, NULL);
}
void
venture_reconciliation_registry_add(VentureReconciliationRegistry *self, VentureReconciliationMatcher *rule)
{
	g_autofree gchar *name = NULL;

	g_return_if_fail(VENTURE_IS_RECONCILIATION_REGISTRY(self));
	g_return_if_fail(VENTURE_IS_RECONCILIATION_MATCHER(rule));
	name = matcher_name(rule);
	g_return_if_fail(NULL != name && '\0' != *name);
	g_hash_table_replace(self->rules, g_strdup(name), rule);
}
VentureReconciliationMatcher *
venture_reconciliation_registry_lookup(VentureReconciliationRegistry *self, const gchar *name)
{
	g_return_val_if_fail(VENTURE_IS_RECONCILIATION_REGISTRY(self), NULL);
	g_return_val_if_fail(NULL != name, NULL);
	return g_hash_table_lookup(self->rules, name);
}
gboolean
venture_reconciliation_registry_remove(VentureReconciliationRegistry *self, const gchar *name)
{
	g_return_val_if_fail(VENTURE_IS_RECONCILIATION_REGISTRY(self), FALSE);
	return g_hash_table_remove(self->rules, name);
}
static gint
compare_rules(gconstpointer a, gconstpointer b)
{
	g_autofree gchar *an = matcher_name(*(VentureReconciliationMatcher *const *)a);
	g_autofree gchar *bn = matcher_name(*(VentureReconciliationMatcher *const *)b);
	return g_strcmp0(an, bn);
}
GPtrArray *
venture_reconciliation_registry_list(VentureReconciliationRegistry *self)
{
	GPtrArray *result = g_ptr_array_new();
	GHashTableIter iter;
	gpointer value;

	g_hash_table_iter_init(&iter, self->rules);
	while (g_hash_table_iter_next(&iter, NULL, &value))
		g_ptr_array_add(result, value);
	g_ptr_array_sort(result, compare_rules);
	return result;
}

static gint
compare_suggestions(gconstpointer a, gconstpointer b)
{
	gint ac, bc;
	g_object_get(*(GObject *const *)a, "confidence", &ac, NULL);
	g_object_get(*(GObject *const *)b, "confidence", &bc, NULL);
	return bc - ac;
}
GPtrArray *
venture_reconciliation_registry_suggest_all(VentureReconciliationRegistry *self,
	VentureDatabase *db, VentureEntity *transaction, GPtrArray *candidates,
	GCancellable *cancellable, GError **error)
{
	g_autoptr(GPtrArray) matchers = venture_reconciliation_registry_list(self);
	g_autoptr(GPtrArray) all = g_ptr_array_new_with_free_func(g_object_unref);
	g_autoptr(GPtrArray) result = g_ptr_array_new_with_free_func(g_object_unref);
	g_autoptr(GHashTable) seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	guint i, j;
	for (i = 0; i < matchers->len; i++)
	{
		g_autoptr(GPtrArray) suggestions = venture_reconciliation_matcher_suggest(
			g_ptr_array_index(matchers, i), db, transaction, candidates, cancellable, error);
		if (suggestions == NULL) return NULL;
		for (j = 0; j < suggestions->len; j++)
			g_ptr_array_add(all, g_object_ref(g_ptr_array_index(suggestions, j)));
	}
	g_ptr_array_sort(all, compare_suggestions);
	for (i = 0; i < all->len; i++)
	{
		VentureMatchSuggestion *suggestion = g_ptr_array_index(all, i);
		g_autoptr(VentureEntity) candidate = NULL;
		g_autofree gchar *key = NULL;
		gint64 id;
		g_object_get(suggestion, "candidate", &candidate, NULL);
		if (candidate == NULL) continue;
		id = venture_entity_get_id(candidate);
		key = id != 0 ? g_strdup_printf("%s:%" G_GINT64_FORMAT, G_OBJECT_TYPE_NAME(candidate), id)
			: g_strdup_printf("%p", (void *)candidate);
		if (g_hash_table_contains(seen, key)) continue;
		g_hash_table_add(seen, g_steal_pointer(&key));
		g_ptr_array_add(result, g_object_ref(suggestion));
	}
	return g_steal_pointer(&result);
}
