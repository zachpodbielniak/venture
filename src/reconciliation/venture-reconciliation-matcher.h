/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_RECONCILIATION_MATCHER_H
#define VENTURE_RECONCILIATION_MATCHER_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS

/**
 * VentureMatchKind:
 * @VENTURE_MATCH_NONE: no match
 * @VENTURE_MATCH_EXACT: same amount
 * @VENTURE_MATCH_PARTIAL: approximate amount
 */
typedef enum {
	VENTURE_MATCH_NONE,
	VENTURE_MATCH_EXACT,
	VENTURE_MATCH_PARTIAL
} VentureMatchKind;
#define VENTURE_TYPE_MATCH_KIND (venture_match_kind_get_type())
/** venture_match_kind_get_type:
 * Returns: the exact/partial/none enum type
 */
GType venture_match_kind_get_type(void) G_GNUC_CONST;

#define VENTURE_TYPE_MATCH_SUGGESTION (venture_match_suggestion_get_type())
G_DECLARE_FINAL_TYPE(VentureMatchSuggestion, venture_match_suggestion, VENTURE, MATCH_SUGGESTION, GObject)
/** venture_match_suggestion_new:
 * @candidate: proposed book record
 * @confidence: score from 0 through 100
 * @rationale: explanation
 * @kind: match classification
 * Returns: (transfer full): an immutable suggestion
 */
VentureMatchSuggestion *venture_match_suggestion_new(VentureEntity *candidate,
	gint confidence, const gchar *rationale, VentureMatchKind kind);

#define VENTURE_TYPE_RECONCILIATION_MATCHER (venture_reconciliation_matcher_get_type())
G_DECLARE_INTERFACE(VentureReconciliationMatcher, venture_reconciliation_matcher,
	VENTURE, RECONCILIATION_MATCHER, GObject)
/** VentureReconciliationMatcherInterface:
 * @parent_iface: parent interface
 * @suggest: propose matches without writing anything
 * @suggest_async: optional asynchronous implementation
 * @suggest_finish: finish the asynchronous implementation
 */
struct _VentureReconciliationMatcherInterface {
	GTypeInterface parent_iface;
	GPtrArray *(*suggest)(VentureReconciliationMatcher *, VentureDatabase *,
		VentureEntity *, GPtrArray *, GCancellable *, GError **);
	void (*suggest_async)(VentureReconciliationMatcher *, VentureDatabase *,
		VentureEntity *, GPtrArray *, GCancellable *, GAsyncReadyCallback, gpointer);
	GPtrArray *(*suggest_finish)(VentureReconciliationMatcher *, GAsyncResult *, GError **);
};
/** venture_reconciliation_matcher_suggest:
 * @self: implementation
 * @db: repository
 * @transaction: bank line as an ordinary entity
 * @candidates: (element-type VentureEntity): book records
 * @cancellable: (nullable): cancellation
 * @error: (out) (optional): error location
 * Returns: (transfer full) (element-type VentureMatchSuggestion) (nullable): proposals
 */
GPtrArray *venture_reconciliation_matcher_suggest(VentureReconciliationMatcher *self,
	VentureDatabase *db, VentureEntity *transaction, GPtrArray *candidates,
	GCancellable *cancellable, GError **error);
/** venture_reconciliation_matcher_suggest_async:
 * @self: implementation
 * @db: repository
 * @transaction: bank line
 * @candidates: (element-type VentureEntity): book records
 * @cancellable: (nullable): cancellation
 * @callback: (scope async): completion callback
 * @user_data: callback data
 *
 * The default runs on the calling main context, never a database worker.
 */
void venture_reconciliation_matcher_suggest_async(VentureReconciliationMatcher *self,
	VentureDatabase *db, VentureEntity *transaction, GPtrArray *candidates,
	GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data);
/** venture_reconciliation_matcher_suggest_finish:
 * @self: implementation
 * @result: asynchronous result
 * @error: (out) (optional): error location
 * Returns: (transfer full) (element-type VentureMatchSuggestion) (nullable): proposals
 */
GPtrArray *venture_reconciliation_matcher_suggest_finish(VentureReconciliationMatcher *self,
	GAsyncResult *result, GError **error);

#define VENTURE_TYPE_EXACT_MATCHER (venture_exact_matcher_get_type())
G_DECLARE_FINAL_TYPE(VentureExactMatcher, venture_exact_matcher, VENTURE, EXACT_MATCHER, GObject)
/** venture_exact_matcher_new:
 * Returns: (transfer full): the deterministic matcher
 */
VentureExactMatcher *venture_exact_matcher_new(void);

#define VENTURE_TYPE_RECONCILIATION_REGISTRY (venture_reconciliation_registry_get_type())
G_DECLARE_FINAL_TYPE(VentureReconciliationRegistry, venture_reconciliation_registry,
	VENTURE, RECONCILIATION_REGISTRY, GObject)
/** venture_reconciliation_registry_new:
 * Returns: (transfer full): an empty registry
 */
VentureReconciliationRegistry *venture_reconciliation_registry_new(void);
/** venture_reconciliation_registry_add:
 * @self: registry
 * @matcher: (transfer full): implementation replacing its name
 */
void venture_reconciliation_registry_add(VentureReconciliationRegistry *self,
	VentureReconciliationMatcher *matcher);
/** venture_reconciliation_registry_lookup:
 * @self: registry
 * @name: implementation name
 * Returns: (transfer none) (nullable): implementation
 */
VentureReconciliationMatcher *venture_reconciliation_registry_lookup(
	VentureReconciliationRegistry *self, const gchar *name);
/** venture_reconciliation_registry_remove:
 * @self: registry
 * @name: implementation name
 * Returns: whether the implementation existed
 */
gboolean venture_reconciliation_registry_remove(VentureReconciliationRegistry *self, const gchar *name);
/** venture_reconciliation_registry_list:
 * @self: registry
 * Returns: (transfer container) (element-type VentureReconciliationMatcher): implementations by name
 */
GPtrArray *venture_reconciliation_registry_list(VentureReconciliationRegistry *self);
/** venture_reconciliation_registry_suggest_all:
 * @self: registry
 * @db: repository
 * @transaction: bank line
 * @candidates: (element-type VentureEntity): book records
 * @cancellable: (nullable): cancellation
 * @error: (out) (optional): error location
 * Returns: (transfer full) (element-type VentureMatchSuggestion) (nullable): highest score per candidate, descending
 */
GPtrArray *venture_reconciliation_registry_suggest_all(VentureReconciliationRegistry *self,
	VentureDatabase *db, VentureEntity *transaction, GPtrArray *candidates,
	GCancellable *cancellable, GError **error);
G_END_DECLS
#endif
