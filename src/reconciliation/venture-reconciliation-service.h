/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_RECONCILIATION_SERVICE_H
#define VENTURE_RECONCILIATION_SERVICE_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
#define VENTURE_TYPE_RECONCILIATION_SERVICE (venture_reconciliation_service_get_type())
G_DECLARE_FINAL_TYPE(VentureReconciliationService, venture_reconciliation_service,
	VENTURE, RECONCILIATION_SERVICE, GObject)
/**
 * venture_reconciliation_service_new:
 * @context: owner, held weakly
 *
 * Returns: (transfer full): service
 */
VentureReconciliationService *venture_reconciliation_service_new(VentureContext *context);
/**
 * venture_context_get_reconciliation_registry:
 * @context: application context
 *
 * Returns: (transfer none) (nullable): matcher registry, NULL when the module is off
 */
VentureReconciliationRegistry *venture_context_get_reconciliation_registry(VentureContext *context);
/**
 * venture_context_get_reconciliation_service:
 * @context: application context
 *
 * Returns: (transfer none) (nullable): shared service, NULL when the module is off
 */
VentureReconciliationService *venture_context_get_reconciliation_service(VentureContext *context);
/**
 * venture_reconciliation_service_suggest:
 * @self: service
 * @type: registered bank-line type
 * @id: saved bank-line ID
 * @matcher: (nullable): one matcher, NULL for all
 * @threshold: stage scores strictly above this value, 0 through 100
 * @actor: origin of the proposal
 * @via: caller surface
 * @error: (out) (optional): error location
 *
 * Returns: (transfer full) (nullable): ranked suggestions and staged confirmations
 *
 * Never saves records. With no bank_match type, returns suggestions only.
 */
JsonNode *venture_reconciliation_service_suggest(VentureReconciliationService *self,
	const gchar *type, gint64 id, const gchar *matcher, gint threshold,
	const VentureActor *actor, const gchar *via, GError **error);
/**
 * venture_reconciliation_service_request:
 * @self: service
 * @input: type, id, optional matcher and threshold
 * @actor: origin
 * @via: caller surface
 * @error: (out) (optional): error location
 *
 * Returns: (transfer full) (nullable): suggestions and confirmations
 */
JsonNode *venture_reconciliation_service_request(VentureReconciliationService *self,
	JsonObject *input, const VentureActor *actor, const gchar *via, GError **error);
G_END_DECLS
#endif
