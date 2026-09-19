/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_YEAR_END_PACK_H
#define VENTURE_YEAR_END_PACK_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
/**
 * VENTURE_YEAR_END_PACK_REPORT:
 *
 * The registry name of the year-end pack report.
 */
#define VENTURE_YEAR_END_PACK_REPORT "year_end_pack"
/**
 * venture_year_end_pack_register_reports:
 * @registry: the report registry
 *
 * Registers =year_end_pack=: one result whose rows are the lines of every
 * CSV in the pack plus =index.txt=, so the scheduled-report path retains
 * the whole pack as ordinary report output.
 */
void venture_year_end_pack_register_reports(VentureReportRegistry *registry);
/**
 * venture_year_end_pack_zip:
 * @result: a =year_end_pack= result
 * @error: (out) (optional): return location for failure
 *
 * Returns: (transfer full) (nullable): the pack as a zip of CSV files and
 *   =index.txt=, or %NULL without libarchive
 */
GBytes *venture_year_end_pack_zip(VentureReportResult *result, GError **error);
/**
 * venture_year_end_pack_zip_from_output:
 * @output: a =report_pack.last_output= JSON array of retained results
 * @error: (out) (optional): return location for failure
 *
 * Rebuilds the zip from exactly the bytes a scheduled run retained. A pack
 * whose output holds no year-end result is refused as not found.
 *
 * Returns: (transfer full) (nullable): the zip, or %NULL
 */
GBytes *venture_year_end_pack_zip_from_output(const gchar *output, GError **error);
G_END_DECLS
#endif
