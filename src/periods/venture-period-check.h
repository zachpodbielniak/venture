/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_PERIOD_CHECK_H
#define VENTURE_PERIOD_CHECK_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
#define VENTURE_TYPE_PERIOD_CHECK (venture_period_check_get_type())
G_DECLARE_INTERFACE(VenturePeriodCheck, venture_period_check, VENTURE, PERIOD_CHECK, GObject)
/**
 * VenturePeriodCheckInterface:
 * @parent_iface: the parent interface
 * @get_name: a stable checklist label
 * @run: checks a period inside the closing transaction
 */
struct _VenturePeriodCheckInterface
{
	GTypeInterface parent_iface;
	const gchar *(*get_name)(VenturePeriodCheck *self);
	gboolean (*run)(VenturePeriodCheck *self, VentureDatabase *database,
		VentureEntity *period, GError **error);
};
/**
 * venture_period_check_get_name:
 * @self: the check
 * Returns: (transfer none): its checklist label
 */
const gchar *venture_period_check_get_name(VenturePeriodCheck *self);
/**
 * venture_period_check_run:
 * @self: the check
 * @database: the locked repository
 * @period: the fiscal period
 * @error: (out) (optional): failure details
 * Returns: %TRUE if the check passes
 */
gboolean venture_period_check_run(VenturePeriodCheck *self, VentureDatabase *database,
	VentureEntity *period, GError **error);

#define VENTURE_TYPE_PERIOD_CHECKLIST (venture_period_checklist_get_type())
G_DECLARE_FINAL_TYPE(VenturePeriodChecklist, venture_period_checklist, VENTURE, PERIOD_CHECKLIST, GObject)
/**
 * venture_period_checklist_new:
 * Returns: (transfer full): a checklist with ledger balance and draft invoice checks
 */
VenturePeriodChecklist *venture_period_checklist_new(void);
/**
 * venture_period_checklist_add:
 * @self: the checklist registry
 * @check: (transfer full): the implementation to append
 */
void venture_period_checklist_add(VenturePeriodChecklist *self, VenturePeriodCheck *check);
/**
 * venture_period_checklist_run:
 * @self: the checklist
 * @database: the locked repository
 * @period: the period to close
 * @error: (out) (optional): all failed checks, with their names and reasons
 *
 * Emits collect-checks (RUN_LAST), then runs every collected implementation
 * in order. A plugin can add a check that vetoes the close without replacing
 * the shipped checks; one failure never hides another.
 * Returns: %TRUE if every check passes
 */
gboolean venture_period_checklist_run(VenturePeriodChecklist *self, VentureDatabase *database,
	VentureEntity *period, GError **error);
G_END_DECLS
#endif
