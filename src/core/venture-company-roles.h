/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_COMPANY_ROLES_H
#define VENTURE_COMPANY_ROLES_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS

/*
 * A company is a customer, a supplier, or both. Its relationship ("kind")
 * says what it mainly is; the two flags say every way you deal with it, so
 * a company you sell to and buy from is in both lists.
 */

/**
 * venture_company_is_customer:
 * @company: a #VentureCompany
 *
 * Returns: %TRUE when they buy from you: the customer flag, or a
 *   relationship of customer on a row saved before the flag existed
 */
gboolean venture_company_is_customer(VentureEntity *company);

/**
 * venture_company_is_supplier:
 * @company: a #VentureCompany
 *
 * Returns: %TRUE when you buy from them: the supplier flag, or a
 *   relationship of supplier on a row saved before the flag existed
 */
gboolean venture_company_is_supplier(VentureEntity *company);

/**
 * venture_company_roles_install:
 * @database: the repository
 *
 * Registers the save rule that keeps the flags in step with the
 * relationship: a company saved as a customer is flagged a customer, and
 * one saved as a supplier is flagged a supplier. Clearing a flag is the
 * owner's to do; changing the relationship never clears one. Idempotent.
 */
void venture_company_roles_install(VentureDatabase *database);

G_END_DECLS
#endif
