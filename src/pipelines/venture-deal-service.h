/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_DEAL_SERVICE_H
#define VENTURE_DEAL_SERVICE_H
G_BEGIN_DECLS
#define VENTURE_TYPE_DEAL_SERVICE (venture_deal_service_get_type())
G_DECLARE_FINAL_TYPE(VentureDealService, venture_deal_service, VENTURE, DEAL_SERVICE, GObject)
/**
 * venture_database_get_deal_service:
 * @database: owning database
 * Returns: (transfer none): the canonical transition service
 */
VentureDealService *venture_database_get_deal_service(VentureDatabase *database);
/**
 * venture_deal_service_move_stage:
 * @self: the service
 * @deal: persisted opportunity, at its current version
 * @stage_id: destination stage
 * @note: (nullable): transition explanation
 * @actor: (nullable): responsible user
 * @error: (out) (optional): refusal
 *
 * Atomically saves the transition and history. Inputs remain unchanged.
 * Returns: (transfer full) (nullable): the updated deal
 */
VentureDeal *venture_deal_service_move_stage(VentureDealService *self, VentureDeal *deal,
	gint64 stage_id, const gchar *note, const VentureActor *actor, GError **error);
/**
 * venture_deal_service_ensure_default:
 * @self: the service
 * @organization_id: owning organization
 * @error: (out) (optional): refusal
 * Returns: the default pipeline id, or zero on failure
 */
gint64 venture_deal_service_ensure_default(VentureDealService *self, gint64 organization_id, GError **error);
/**
 * venture_deal_service_line_total:
 * @line: a deal line
 * @error: (out) (optional): invalid quantity, price or discount
 *
 * Quantity times unit price, less the discount in basis points, rounded
 * once half to even. Money arithmetic refuses overflow.
 * Returns: (transfer full) (nullable): the line's extended value
 */
VentureMoney *venture_deal_service_line_total(VentureDealLine *line, GError **error);
/**
 * venture_deal_service_create_quote:
 * @self: the service
 * @deal: persisted opportunity with at least one line
 * @actor: (nullable): responsible user
 * @error: (out) (optional): refusal
 *
 * Copies the deal's lines into a draft quote linked to the deal, in one
 * transaction. A rerun revises the deal's latest quote through
 * #VentureQuoteService instead of creating a second quote.
 * Returns: (transfer full) (nullable): the new draft quote
 */
VentureQuote *venture_deal_service_create_quote(VentureDealService *self, VentureDeal *deal,
	const VentureActor *actor, GError **error);
/**
 * venture_deal_service_import_history:
 * @self: the deal service
 * @entry: a new historical entry with its source timestamp and author
 * @actor: (nullable): the migration's audit actor
 * @error: (out) (optional): validation or persistence error
 *
 * Appends historical evidence without replaying a transition, changing the
 * current deal, or booking a historical win again. Both stages must belong
 * to the deal's pipeline and organization. The saved entry is immutable.
 *
 * Returns: TRUE on success
 */
gboolean venture_deal_service_import_history(VentureDealService *self,
	VentureDealStageEntry *entry, const VentureActor *actor, GError **error);

/**
 * venture_deal_service_move_stage_at:
 * @self: canonical deal service
 * @deal: persisted opportunity at its expected version
 * @stage_id: destination stage
 * @at: (nullable): original event date; NULL uses now
 * @note: (nullable): source explanation
 * @actor: (nullable): audit actor
 * @error: (out) (optional): validation error
 *
 * Applies the ordinary transition with its original date, including the
 * immutable sales booking. Inputs remain unchanged.
 * Returns: (transfer full) (nullable): updated opportunity
 */
VentureDeal *venture_deal_service_move_stage_at(VentureDealService *self, VentureDeal *deal,
	gint64 stage_id, GDateTime *at, const gchar *note, const VentureActor *actor, GError **error);
/**
 * venture_deal_service_create_at:
 * @self: canonical service
 * @deal: unsaved opportunity
 * @at: (nullable): original initial-stage date; NULL uses now
 * @actor: (nullable): audit actor
 * @error: (out) (optional): validation error
 *
 * Creates an opportunity through normal admission validation with original
 * initial-stage evidence, without inventing a transition at import time.
 * Returns: TRUE on success
 */
gboolean venture_deal_service_create_at(VentureDealService *self, VentureDeal *deal,
 GDateTime *at, const VentureActor *actor, GError **error);
G_END_DECLS
#endif
