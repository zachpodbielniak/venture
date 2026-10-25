/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_COMPETITOR_H
#define VENTURE_COMPETITOR_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
/**
 * venture_competitor_install:
 * @context: application context
 *
 * Installs organization and duplicate-link validation once per database.
 */
void venture_competitor_install(VentureContext *context);
/**
 * venture_competitor_register_reports:
 * @registry: report registry
 *
 * Registers the currency-separated competitor win/loss report.
 */
void venture_competitor_register_reports(VentureReportRegistry *registry);
G_END_DECLS
#endif
