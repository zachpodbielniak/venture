/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_PERIOD_RECORDS_H
#define VENTURE_PERIOD_RECORDS_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

G_BEGIN_DECLS

/**
 * venture_period_records_register_constraints:
 *
 * Applies organization-scoped numbering metadata after builtin registration.
 * Account's field table remains owned by the journal module.
 */
void venture_period_records_register_constraints(void);

G_END_DECLS
#endif
