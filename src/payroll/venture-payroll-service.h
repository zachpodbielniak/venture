/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_PAYROLL_SERVICE_H
#define VENTURE_PAYROLL_SERVICE_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
#define VENTURE_TYPE_PAYROLL_SERVICE (venture_payroll_service_get_type())
G_DECLARE_FINAL_TYPE(VenturePayrollService, venture_payroll_service, VENTURE, PAYROLL_SERVICE, GObject)
/**
 * venture_payroll_service_get:
 * @database: database owning the records
 *
 * Returns the per-database service. The database owns this reference.
 *
 * Returns: (transfer none): borrowed result
 */
VenturePayrollService *venture_payroll_service_get(VentureDatabase *database);
/**
 * venture_payroll_service_import_json:
 * @self: the service or registry instance
 * @organization_id: target legal entity ID
 * @payload: import payload
 * @actor: (nullable): audit actor; NULL for internal service work
 * @error: (out) (optional): return location for an error
 *
 * Returns: (transfer full) (nullable): owned result
 */
VentureEntity *venture_payroll_service_import_json(VenturePayrollService *self, gint64 organization_id,
	JsonObject *payload, const VentureActor *actor, GError **error);
/**
 * venture_payroll_service_import_csv:
 * @self: the service or registry instance
 * @organization_id: target legal entity ID
 * @run_key: import idempotency key
 * @period_start: start of the payroll period
 * @period_end: end of the payroll period
 * @currency: ISO currency code
 * @csv: CSV import text
 * @actor: (nullable): audit actor; NULL for internal service work
 * @error: (out) (optional): return location for an error
 *
 * Returns: (transfer full) (nullable): owned result
 */
VentureEntity *venture_payroll_service_import_csv(VenturePayrollService *self, gint64 organization_id,
	const gchar *run_key, const gchar *period_start, const gchar *period_end, const gchar *currency,
	const gchar *csv, const VentureActor *actor, GError **error);
/**
 * venture_payroll_service_disburse:
 * @self: the service or registry instance
 * @run: payroll run
 * @kind: operation or field kind
 * @actor: (nullable): audit actor; NULL for internal service work
 * @error: (out) (optional): return location for an error
 *
 * Returns: TRUE on success, FALSE on failure
 */
gboolean venture_payroll_service_disburse(VenturePayrollService *self, VentureEntity *run,
	const gchar *kind, const VentureActor *actor, GError **error);
/**
 * venture_payroll_service_reverse:
 * @self: the service or registry instance
 * @run: payroll run
 * @actor: (nullable): audit actor; NULL for internal service work
 * @error: (out) (optional): return location for an error
 *
 * Returns: TRUE on success, FALSE on failure
 */
gboolean venture_payroll_service_reverse(VenturePayrollService *self, VentureEntity *run,
	const VentureActor *actor, GError **error);
/**
 * venture_payroll_save_hook:
 * @database: database owning the records
 * @record: candidate record
 * @actor: (nullable): audit actor; NULL for internal service work
 * @handled: (out): whether the service performed the write
 * @error: (out) (optional): return location for an error
 *
 * Routes generic writes through the subsystem operation when required.
 *
 * Returns: TRUE on success, FALSE on failure
 */
gboolean venture_payroll_save_hook(VentureDatabase *database, VentureEntity *record,
	const VentureActor *actor, gboolean *handled, GError **error);
/**
 * venture_payroll_check_write:
 * @database: database owning the records
 * @record: candidate record
 * @removal: whether this is a removal operation
 * @error: (out) (optional): return location for an error
 *
 * Checks service ownership and lifecycle restrictions before a generic write.
 *
 * Returns: TRUE on success, FALSE on failure
 */
gboolean venture_payroll_check_write(VentureDatabase *database, VentureEntity *record,
	gboolean removal, GError **error);
/**
 * venture_payroll_register_reports:
 * @registry: registry receiving the registrations
 */
void venture_payroll_register_reports(VentureReportRegistry *registry);
G_END_DECLS
#endif
