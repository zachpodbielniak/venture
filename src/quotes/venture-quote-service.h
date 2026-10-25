/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_QUOTE_SERVICE_H
#define VENTURE_QUOTE_SERVICE_H
G_BEGIN_DECLS
#define VENTURE_TYPE_QUOTE_SERVICE (venture_quote_service_get_type())
G_DECLARE_FINAL_TYPE(VentureQuoteService, venture_quote_service, VENTURE, QUOTE_SERVICE, GObject)
/**
 * venture_database_get_quote_service:
 * @database: the owner
 * Returns: (transfer none): its quote service
 */
VentureQuoteService *venture_database_get_quote_service(VentureDatabase *database);
/**
 * venture_quote_service_execute:
 * @self: the service
 * @request: an unsaved quote_action with the expected quote version
 * @method: manual or web
 * @ip: (nullable): the public caller's address
 * @actor: (nullable): audit attribution
 * @error: (out) (optional): refusal
 * Returns: TRUE if the entire action committed
 */
gboolean venture_quote_service_execute(VentureQuoteService *self, VentureEntity *request,
	const gchar *method, const gchar *ip, const VentureActor *actor, GError **error);
/**
 * venture_quote_service_sweep:
 * @self: the service
 * @organization_id: exact organization
 * @now: expiry cutoff
 * @error: (out) (optional): refusal
 * Returns: number expired, or -1 on failure
 */
gint venture_quote_service_sweep(VentureQuoteService *self, gint64 organization_id,
	GDateTime *now, GError **error);
/**
 * venture_quotes_save_hook: (skip)
 * @database: storage
 * @record: proposed write
 * @actor: (nullable): attribution
 * @handled: (out): whether the service performed the save
 * @error: (out) (optional): refusal
 * Returns: TRUE if allowed
 */
gboolean venture_quotes_save_hook(VentureDatabase *database, VentureEntity *record,
	const VentureActor *actor, gboolean *handled, GError **error);
/**
 * venture_quotes_check_removal: (skip)
 * @database: storage
 * @record: removal target
 * @error: (out) (optional): refusal
 * Returns: TRUE if history is preserved
 */
gboolean venture_quotes_check_removal(VentureDatabase *database, VentureEntity *record, GError **error);
/**
 * venture_quotes_register_reports:
 * @registry: report registry
 * Registers the issued-cohort quote report.
 */
void venture_quotes_register_reports(VentureReportRegistry *registry);
/**
 * venture_quotes_remove_hook: (skip)
 * @database: storage
 * @record: target
 * @operation: 0 delete, 1 restore, 2 purge
 * @actor: (nullable): attribution
 * @handled: (out): whether the removal was performed
 * @error: (out) (optional): refusal
 * Returns: TRUE on success
 */
gboolean venture_quotes_remove_hook(VentureDatabase *database, VentureEntity *record,
	guint operation, const VentureActor *actor, gboolean *handled, GError **error);
/**
 * venture_quotes_settings_register: (skip)
 * Registers the organization's built-in discount policy.
 */
void venture_quotes_settings_register(void);
/**
 * venture_quotes_bind_context: (skip)
 * @context: wiring, held weakly by its quote service
 */
void venture_quotes_bind_context(VentureContext *context);
/**
 * venture_quote_service_decide_discount: (skip)
 * @self: quote service
 * @request: staged action
 * @proposer: authenticated proposing user id
 * @approve: whether to send
 * @reason: (nullable): mandatory rejection reason
 * @actor: audit attribution
 * @error: (out) (optional): refusal
 * Returns: TRUE if the decision committed
 */
gboolean venture_quote_service_decide_discount(VentureQuoteService *self,
	VentureEntity *request, gint64 proposer, gboolean approve, const gchar *reason,
	const VentureActor *actor, GError **error);
/**
 * venture_quote_service_discount_pending: (skip)
 * @self: service
 * @request: staged discount action
 * @actor: (nullable): audit attribution
 * @error: (out) (optional): refusal
 * Returns: TRUE after history and inbox notifications commit
 */
gboolean venture_quote_service_discount_pending(VentureQuoteService *self,
	VentureEntity *request, const VentureActor *actor, GError **error);
G_END_DECLS
#endif
