/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_SETUP_RECORDS_H
#define VENTURE_SETUP_RECORDS_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
#define VENTURE_TYPE_ACCOUNTING_SETUP (venture_accounting_setup_get_type())
VENTURE_DECLARE_ENTITY(VentureAccountingSetup, venture_accounting_setup, ACCOUNTING_SETUP)
#define VENTURE_TYPE_ACCOUNTING_CONTROL_MAP (venture_accounting_control_map_get_type())
VENTURE_DECLARE_ENTITY(VentureAccountingControlMap, venture_accounting_control_map, ACCOUNTING_CONTROL_MAP)
G_END_DECLS
#endif
