/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

/* One migration of a whole set of books: the source backup as submitted,
 * the state it reached and the evidence each step appended. */
static const VentureFieldDecl books_import_fields[] = {
	VENTURE_FIELD_NAME("source", "Source", "zoho_books"),
	VENTURE_FIELD_NAME("state", "State", "preview, imported, reconciled, active, rolled_back"),
	VENTURE_FIELD("start", "Start", "The source's first day: opening balances enter here",
		VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("export-date", "Export date", "The last day the backup covers",
		VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("manifest", "Manifest", "Settings and the backup's CSV text"),
	VENTURE_FIELD_TEXT("report", "Report", "Preview, reconcile and rollback evidence, appended"),
	VENTURE_FIELD_REF("cutover-id", "Opening cutover", "The cutover batch holding the day-one balances",
		"accounting_cutover", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("activated-at", "Activated at", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("rolled-back-at", "Rolled back at", "The day every posting of the batch was unwound",
		VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VentureBooksImport, venture_books_import, books_import_fields)

/* One source document, or one record it became. A rerun finds its rows and
 * skips them; reconcile reads the records they name. */
static const VentureFieldDecl books_import_row_fields[] = {
	VENTURE_FIELD_REF("import-id", "Import", NULL, "books_import", VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_NAME("source-object", "Source object", "invoice, customer_payment, bill, expense, ..."),
	VENTURE_FIELD_NAME("source-id", "Source ID", "Stable identifier in the source backup"),
	VENTURE_FIELD_NAME("status", "Status", "preview, imported, matched, exception, skipped, rolled_back"),
	VENTURE_FIELD("record-type", "Record type", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("record-id", "Record id", NULL, VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("period", "Period", "Source month, YYYY-MM", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_MONEY("amount", "Amount", "Source document total"),
	VENTURE_FIELD("created", "Created", "This import made the record", VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("exception", "Exception", "Row-level problem")
};
VENTURE_DEFINE_ENTITY(VentureBooksImportRow, venture_books_import_row, books_import_row_fields)
