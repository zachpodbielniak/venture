/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

/* One migration batch: the manifest as submitted, its state and the
 * preview report. Written only by VentureCrmImportService. */
static const VentureFieldDecl crm_import_fields[] = {
	VENTURE_FIELD_NAME("source", "Source", "hubspot, zoho_crm or salesforce"),
	VENTURE_FIELD_NAME("state", "State", "preview, imported, active, rolled_back"),
	VENTURE_FIELD_TEXT("manifest", "Manifest", "Mapped JSON manifest with the CSV text inlined"),
	VENTURE_FIELD_TEXT("report", "Report", "Preview counts, unsupported objects and exceptions"),
	VENTURE_FIELD("activated-at", "Activated at", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VentureCrmImport, venture_crm_import, crm_import_fields)

/* One source row: which export it came from, the record it became or
 * matched, and whether this batch created it, so rollback removes exactly
 * what the import created and a rerun finds what it already did. */
static const VentureFieldDecl crm_import_row_fields[] = {
	VENTURE_FIELD_REF("import-id", "Import", NULL, "crm_import", VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_NAME("source-object", "Source object", "company, contact, deal, note or task"),
	VENTURE_FIELD_NAME("source-id", "Source ID", "Stable identifier from the source CRM"),
	VENTURE_FIELD("record-type", "Record type", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("record-id", "Record id", NULL, VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_NAME("status", "Status", "preview, imported, matched, exception, skipped, rolled_back"),
	VENTURE_FIELD_TEXT("exception", "Exception", "Conflict or unresolved relation recorded on the row"),
	VENTURE_FIELD("created", "Created", "This batch created the record", VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VentureCrmImportRow, venture_crm_import_row, crm_import_row_fields)
