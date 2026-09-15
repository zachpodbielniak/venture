/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

static const VentureFieldDecl workspace_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", "The fiscal period this close covers"),
	VENTURE_FIELD_REF("fiscal-period-id", "Fiscal period", NULL, "fiscal_period",
		VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION),
	VENTURE_FIELD_NAME("status", "Status",
		"preparing, in_review, signed_off, completed or reopened; VentureCloseService owns transitions"),
	VENTURE_FIELD_NAME("currency", "Currency", "Book currency for tie-outs"),
	VENTURE_FIELD("preparer", "Preparer", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("reviewer", "Reviewer", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("tb-balanced", "Trial balance tied", "Set by the close service after the pack",
		VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("subledger-tied", "Subledgers tied", "AR, AP and bank control differences are zero or explained",
		VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("pack-notes", "Report pack", "Snapshot of the close pack")
};
VENTURE_DEFINE_ENTITY(VentureCloseWorkspace, venture_close_workspace, workspace_fields)

static const VentureFieldDecl task_fields[] = {
	VENTURE_FIELD_REF("workspace-id", "Workspace", NULL, "close_workspace", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_NAME("kind", "Check",
		"bank_recon, ar_control, ap_control, suspense, tax, depreciation, deferrals, tb_tieout or subledger_tieout"),
	VENTURE_FIELD_NAME("role", "Role", "preparer or reviewer"),
	VENTURE_FIELD_NAME("status", "Status", "open, done or waived; VentureCloseService owns transitions"),
	VENTURE_FIELD("assignee", "Assignee", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_TEXT("notes", "Notes", "What the check found"),
	VENTURE_FIELD_MONEY("difference", "Difference", "Control minus subledger, or unmatched amount")
};
VENTURE_DEFINE_ENTITY(VentureCloseTask, venture_close_task, task_fields)

static const VentureFieldDecl workpaper_fields[] = {
	VENTURE_FIELD_REF("workspace-id", "Workspace", NULL, "close_workspace", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_REF("task-id", "Task", NULL, "close_task", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_NAME("title", "Title", NULL),
	VENTURE_FIELD_TEXT("body", "Body", NULL),
	VENTURE_FIELD_REF("document-id", "Document", NULL, "document", VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VentureCloseWorkpaper, venture_close_workpaper, workpaper_fields)

static const VentureFieldDecl discrepancy_fields[] = {
	VENTURE_FIELD_REF("workspace-id", "Workspace", NULL, "close_workspace", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_REF("task-id", "Task", NULL, "close_task", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_NAME("kind", "Kind", "The checklist item this difference belongs to"),
	VENTURE_FIELD_NAME("status", "Status", "open, explained or corrected; an explanation is required to leave open"),
	VENTURE_FIELD_TEXT("explanation", "Explanation", "Why the difference exists"),
	VENTURE_FIELD("correction-type", "Correction type", "Record type of the correcting document",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("correction-id", "Correction", "The correcting record",
		VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_MONEY("amount", "Amount", NULL)
};
VENTURE_DEFINE_ENTITY(VentureCloseDiscrepancy, venture_close_discrepancy, discrepancy_fields)

static const VentureFieldDecl signoff_fields[] = {
	VENTURE_FIELD_REF("workspace-id", "Workspace", NULL, "close_workspace", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_NAME("role", "Role", "preparer or reviewer"),
	VENTURE_FIELD("actor", "Actor", "Who signed", VENTURE_FIELD_KIND_STRING,
		VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("signed-at", "Signed at", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("pack-hash", "Pack hash", "Fingerprint of the report pack at signoff",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VentureCloseSignoff, venture_close_signoff, signoff_fields)
