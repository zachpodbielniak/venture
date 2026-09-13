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
G_END_DECLS
#endif
