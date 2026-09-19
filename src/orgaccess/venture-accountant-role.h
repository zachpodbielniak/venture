/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_ACCOUNTANT_ROLE_H
#define VENTURE_ACCOUNTANT_ROLE_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
/**
 * venture_accountant_role_readable:
 * @database: repository
 * @entity: candidate record
 *
 * The books, as an outside accountant sees them: every type a finance
 * module owns except payroll, the organization row for the picker, the
 * companies that invoices and bills name, and documents attached to a
 * financial record. Everything else does not exist for this role.
 *
 * Returns: whether an accountant membership may read or export @entity
 */
gboolean venture_accountant_role_readable(VentureDatabase *database, VentureEntity *entity);
/**
 * venture_accountant_role_refuse_write:
 * @action: the refused operation
 * @error: (out) (optional): return location for the refusal
 *
 * Returns: %FALSE, with a permission error that names the role
 */
gboolean venture_accountant_role_refuse_write(const gchar *action, GError **error);
/**
 * venture_accountant_role_only:
 * @database: repository
 * @actor: (nullable): authenticated principal
 *
 * Returns: whether @actor holds at least one active membership and every
 *   one of them is the accountant role, with no global bypass
 */
gboolean venture_accountant_role_only(VentureDatabase *database, const VentureAuthPrincipal *actor);
G_END_DECLS
#endif
