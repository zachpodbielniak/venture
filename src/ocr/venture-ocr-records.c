/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
static const VentureFieldDecl job_fields[] = {
	VENTURE_FIELD_NAME("title", "Title", "Original document title"),
	VENTURE_FIELD_REF("document-id", "Document", "Unmodified original attachment", "document", VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_NAME("state", "State", "pending, running, succeeded, failed or cancelled; service owned"),
	VENTURE_FIELD("source-hash", "Source checksum", "SHA256 of original bytes", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_NAME("language", "Language", "Installed Tesseract languages joined with +"),
	VENTURE_FIELD("engine", "Engine", "Local engine and version", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("pages", "Pages", "Total document pages", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("completed-pages", "Completed pages", "Durable page cursor", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("attempts", "Attempts", "Page operations attempted", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("text", "Extracted text", "Retained output awaiting or applied through review"),
	VENTURE_FIELD_TEXT("error", "Error", "Last bounded operation failure"),
	VENTURE_FIELD("extracted-at", "Extracted at", "Last successful extraction", VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("applied-hash", "Applied checksum", "Detects later document corrections", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("reviewed", "Reviewed", "Explicit review protects later edits", VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VentureOcrJob, venture_ocr_job, job_fields)
static const VentureFieldDecl batch_fields[] = {
	VENTURE_FIELD_NAME("title", "Title", "Capture inbox extraction batch"),
	VENTURE_FIELD_NAME("state", "State", "pending, running, succeeded or cancelled; service owned"),
	VENTURE_FIELD_NAME("language", "Language", "Installed Tesseract languages joined with +"),
	VENTURE_FIELD_TEXT("capture-ids", "Capture identities", "Frozen bounded inbox selection"),
	VENTURE_FIELD("cursor", "Completed documents", "Durable inbox cursor", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("total", "Documents", "Frozen inbox count", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("failed", "Failed documents", "Failures retained on each document job", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("current-job-id", "Current job", "One page is processed per step", "ocr_job", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("force", "Force extraction", "Re-extract unchanged sources without replacing corrections", VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("error", "Last error", "A failed document does not discard the rest")
};
VENTURE_DEFINE_ENTITY(VentureOcrBatch, venture_ocr_batch, batch_fields)
