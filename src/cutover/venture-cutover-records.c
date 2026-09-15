/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
static const VentureFieldDecl cutover_fields[] = {
	VENTURE_FIELD_NAME("source", "Source", "zoho_books or quickbooks"),
	VENTURE_FIELD("cutoff", "Cutoff", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_NAME("state", "State", "preview, imported, reconciled, active, rolled_back"),
	VENTURE_FIELD_TEXT("payload", "Payload", "Mapped source JSON"),
	VENTURE_FIELD_TEXT("reconciliation-report", "Reconciliation report", "Retained cutover evidence"),
	VENTURE_FIELD("activated-at", "Activated at", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VentureAccountingCutover, venture_accounting_cutover, cutover_fields)
static const VentureFieldDecl row_fields[] = {
	VENTURE_FIELD_REF("cutover-id", "Cutover", NULL, "accounting_cutover", VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_NAME("source-id", "Source ID", "Stable identifier from the source system"),
	VENTURE_FIELD_NAME("source-type", "Source type", NULL),
	VENTURE_FIELD_NAME("status", "Status", "preview, imported, exception, skipped"),
	VENTURE_FIELD_TEXT("exception", "Exception", "Row-level refusal"),
	VENTURE_FIELD("record-type", "Record type", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("record-id", "Record id", NULL, VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VentureAccountingCutoverRow, venture_accounting_cutover_row, row_fields)
