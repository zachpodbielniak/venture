/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
static const VentureFieldDecl saved_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", "Saved report"),
	VENTURE_FIELD("report-name", "Report", "Registry name such as cash_flow",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("period", "Period", "Named period or ISO date range",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("options", "Options", "JSON report options"),
	VENTURE_FIELD("dimension", "Dimension", "Optional journal dimension filter",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED)
};
VENTURE_DEFINE_ENTITY(VentureSavedReport, venture_saved_report, saved_fields)
static const VentureFieldDecl pack_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", "Scheduled pack"),
	VENTURE_FIELD("schedule", "Schedule", "Cron-like delivery cadence",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("saved-report-ids", "Saved reports", "Comma-separated saved_report ids",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VentureReportPack, venture_report_pack, pack_fields)
static const VentureFieldDecl dimension_fields[] = {
	VENTURE_FIELD("code", "Code", "Stable key stored on journals",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION),
	VENTURE_FIELD_NAME("name", "Name", "Department, location, project or class")
};
VENTURE_DEFINE_ENTITY(VentureAccountingDimension, venture_accounting_dimension, dimension_fields)
