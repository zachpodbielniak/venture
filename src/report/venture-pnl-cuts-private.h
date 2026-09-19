/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_PNL_CUTS_PRIVATE_H
#define VENTURE_PNL_CUTS_PRIVATE_H
#include "venture.h"

/**
 * venture_pnl_cuts_register_reports:
 * @registry: the report registry
 *
 * Registers the four cuts of the P&L: revenue_by_customer, spend_by_vendor,
 * recurring_costs and cash_outlook.
 */
void venture_pnl_cuts_register_reports(VentureReportRegistry *registry);
#endif
