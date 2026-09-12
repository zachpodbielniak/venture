/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_PERIOD_SERVICE_H
#define VENTURE_PERIOD_SERVICE_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS

#define VENTURE_TYPE_PERIOD_SERVICE (venture_period_service_get_type())
G_DECLARE_FINAL_TYPE(VenturePeriodService, venture_period_service, VENTURE, PERIOD_SERVICE, GObject)

/**
 * venture_period_service_get:
 * @database: the owning database
 * Returns: (transfer none): its fiscal calendar service
 */
VenturePeriodService *venture_period_service_get(VentureDatabase *database);

/**
 * venture_period_service_get_checklist:
 * @self: the service
 * Returns: (transfer none): the extensible close checklist
 */
VenturePeriodChecklist *venture_period_service_get_checklist(VenturePeriodService *self);

/**
 * venture_period_service_install:
 * @context: the application context
 *
 * Connects its report registry to the repository's closing service.
 */
void venture_period_service_install(VentureContext *context);

/**
 * venture_period_service_generate:
 * @self: the service
 * @organization_id: the legal entity
 * @name: the fiscal year's label
 * @start: the inclusive first day
 * @length: monthly or quarterly
 * @actor: (nullable): the actor for the audit trail
 * @error: (out) (optional): failure details
 *
 * Creates a year and its complete calendar atomically. Generic fiscal_year
 * creation uses exactly this same service through the repository hook.
 * Returns: (transfer full) (nullable): the persisted year
 */
VentureFiscalYear *venture_period_service_generate(VenturePeriodService *self,
	gint64 organization_id, const gchar *name, GDateTime *start,
	VenturePeriodLength length, const VentureActor *actor, GError **error);

/**
 * venture_periods_save:
 * @database: the repository
 * @entity: the proposed record
 * @actor: (nullable): the actor
 * @handled: (out): whether this was a fiscal record
 * @error: (out) (optional): failure details
 *
 * Repository hook for atomic fiscal operations; every surface reaches it.
 * Returns: %TRUE on success, or when @handled is %FALSE
 */
gboolean venture_periods_save(VentureDatabase *database, VentureEntity *entity,
	const VentureActor *actor, gboolean *handled, GError **error);

/**
 * venture_periods_check_removal:
 * @database: the repository
 * @entity: the record being deleted or restored
 * @error: (out) (optional): refusal details
 * Returns: %TRUE if the operation may proceed
 */
gboolean venture_periods_check_removal(VentureDatabase *database,
	VentureEntity *entity, GError **error);

G_END_DECLS
#endif
