/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_EQUITY_SERVICE_H
#define VENTURE_EQUITY_SERVICE_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
#define VENTURE_TYPE_CAPITAL_SERVICE (venture_capital_service_get_type())
G_DECLARE_FINAL_TYPE(VentureCapitalService, venture_capital_service, VENTURE, CAPITAL_SERVICE, GObject)
/**
 * venture_capital_service_get:
 * @database: database owning the records
 *
 * Returns the per-database service. The database owns this reference.
 *
 * Returns: (transfer none): borrowed result
 */
VentureCapitalService *venture_capital_service_get(VentureDatabase *database);
/**
 * venture_equity_check_write:
 * @database: database owning the records
 * @record: candidate record
 * @removal: whether this is a removal operation
 * @error: (out) (optional): return location for an error
 *
 * Checks service ownership and lifecycle restrictions before a generic write.
 *
 * Returns: TRUE on success, FALSE on failure
 */
gboolean venture_equity_check_write(VentureDatabase *database, VentureEntity *record,
	gboolean removal, GError **error);
/**
 * venture_capital_service_post:
 * @self: the service or registry instance
 * @organization_id: target legal entity ID
 * @kind: operation or field kind
 * @amount: amount with an explicit currency
 * @when: effective timestamp
 * @memo: operator memo
 * @debit_account_id: debit account id
 * @credit_account_id: credit account id
 * @actor: (nullable): audit actor; NULL for internal service work
 * @error: (out) (optional): return location for an error
 *
 * Returns: (transfer full) (nullable): owned result
 */
VentureEntity *venture_capital_service_post(VentureCapitalService *self, gint64 organization_id,
	VentureEquityKind kind, const VentureMoney *amount, GDateTime *when, const gchar *memo,
	gint64 debit_account_id, gint64 credit_account_id, const VentureActor *actor, GError **error);
G_END_DECLS
#endif
