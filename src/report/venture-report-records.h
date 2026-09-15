/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_REPORT_RECORDS_H
#define VENTURE_REPORT_RECORDS_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
#define VENTURE_TYPE_SAVED_REPORT (venture_saved_report_get_type())
VENTURE_DECLARE_ENTITY(VentureSavedReport, venture_saved_report, SAVED_REPORT)
#define VENTURE_TYPE_REPORT_PACK (venture_report_pack_get_type())
VENTURE_DECLARE_ENTITY(VentureReportPack, venture_report_pack, REPORT_PACK)
#define VENTURE_TYPE_ACCOUNTING_DIMENSION (venture_accounting_dimension_get_type())
VENTURE_DECLARE_ENTITY(VentureAccountingDimension, venture_accounting_dimension, ACCOUNTING_DIMENSION)
G_END_DECLS
#endif
