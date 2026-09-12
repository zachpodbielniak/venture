/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_PERIOD_GUARD_H
#define VENTURE_PERIOD_GUARD_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS

#define VENTURE_TYPE_PERIOD_GUARD (venture_period_guard_get_type())
G_DECLARE_INTERFACE(VenturePeriodGuard, venture_period_guard, VENTURE, PERIOD_GUARD, GObject)

/**
 * VenturePeriodGuardInterface:
 * @parent_iface: the parent interface
 * @is_postable: checks an organization's accounting date, setting an error on refusal
 *
 * Posting services depend on this interface, regardless of enabled modules.
 */
struct _VenturePeriodGuardInterface
{
	GTypeInterface parent_iface;
	gboolean (*is_postable)(VenturePeriodGuard *self, VentureDatabase *database,
		gint64 organization_id, GDateTime *date, GError **error);
};

/**
 * venture_period_guard_is_postable:
 * @self: a guard or guard registry
 * @database: the repository, held in the caller's posting transaction
 * @organization_id: the legal entity
 * @date: (nullable): the accounting instant
 * @error: (out) (optional): refusal naming the period and state
 *
 * Call inside the transaction that will post. An organization with no fiscal
 * calendar, or the disabled periods module, imposes no default restriction.
 * A configured calendar refuses missing dates and dates outside its coverage.
 * Returns: %TRUE if posting may proceed
 */
gboolean venture_period_guard_is_postable(VenturePeriodGuard *self,
	VentureDatabase *database, gint64 organization_id, GDateTime *date, GError **error);

#define VENTURE_TYPE_DEFAULT_PERIOD_GUARD (venture_default_period_guard_get_type())
G_DECLARE_FINAL_TYPE(VentureDefaultPeriodGuard, venture_default_period_guard,
	VENTURE, DEFAULT_PERIOD_GUARD, GObject)

/**
 * venture_default_period_guard_new:
 * Returns: (transfer full): the built-in calendar guard
 */
VenturePeriodGuard *venture_default_period_guard_new(void);

#define VENTURE_TYPE_PERIOD_GUARD_REGISTRY (venture_period_guard_registry_get_type())
G_DECLARE_FINAL_TYPE(VenturePeriodGuardRegistry, venture_period_guard_registry,
	VENTURE, PERIOD_GUARD_REGISTRY, GObject)

/**
 * venture_period_guard_registry_new:
 * Returns: (transfer full): an empty registry; all registered guards must permit posting
 */
VenturePeriodGuardRegistry *venture_period_guard_registry_new(void);

/**
 * venture_period_guard_registry_add:
 * @self: the registry
 * @guard: (transfer full): a guard, run in registration order
 */
void venture_period_guard_registry_add(VenturePeriodGuardRegistry *self, VenturePeriodGuard *guard);

/**
 * venture_database_get_period_guard:
 * @database: the repository
 * Returns: (transfer none): its registry, including the default guard; available even with periods off
 */
VenturePeriodGuardRegistry *venture_database_get_period_guard(VentureDatabase *database);

/**
 * venture_periods_validate_financial:
 * @database: the locked repository
 * @entity: the proposed financial record
 * @previous: (nullable): its stored version
 * @error: (out) (optional): refusal details
 * Returns: %TRUE if both the old and new accounting dates permit the write
 */
gboolean venture_periods_validate_financial(VentureDatabase *database,
	VentureEntity *entity, VentureEntity *previous, GError **error);
G_END_DECLS
#endif
