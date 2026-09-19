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
	VENTURE_FIELD("schedule", "Schedule", "Five numeric or * cron fields, or daily; interpreted in UTC",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("saved-report-ids", "Saved reports", "Comma-separated saved_report ids",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("last-run-at", "Last run", "When run_due last dispatched this pack",
		VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("last-output", "Last output", "JSON results from the last successful scheduled run",
		VENTURE_FIELD_KIND_JSON, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("recipients", "Recipients", "Comma-separated addresses the output is mailed to",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("deliver", "Deliver", "none, or email to mail each run's output through the outbox",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("last-delivered-at", "Last delivered", "When the last output was handed to the mail queue",
		VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("last-delivery-message-id", "Last Message-ID", "Message-ID of the queued mail",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("last-delivery-mail-id", "Last mail row", "The mail_message row the output was queued as",
		VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("last-delivery-error", "Last delivery error", "Why the last delivery was refused; empty when it was queued")
};
VENTURE_DEFINE_ENTITY(VentureReportPack, venture_report_pack, pack_fields)
static const VentureFieldDecl dimension_fields[] = {
	VENTURE_FIELD("code", "Code", "Stable key stored on journals",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION),
	VENTURE_FIELD_NAME("name", "Name", "Department, location, project or class")
};
VENTURE_DEFINE_ENTITY(VentureAccountingDimension, venture_accounting_dimension, dimension_fields)
