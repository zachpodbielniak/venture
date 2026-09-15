/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

static const VentureFieldDecl capture_fields[] = {
	VENTURE_FIELD_NAME("title", "Title", "What was captured"),
	VENTURE_FIELD_NAME("kind", "Kind", "receipt or supplier_invoice"),
	VENTURE_FIELD_NAME("status", "Status", "inbox, converted or rejected; VentureCaptureService owns conversion"),
	VENTURE_FIELD("source", "Source", "upload, email, scan, or an integration name",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_REF("document-id", "Document", "The filed original", "document", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("vendor", "Vendor", "As printed on the document",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD_MONEY("amount", "Amount", NULL),
	VENTURE_FIELD("occurred-at", "Date", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_TEXT("notes", "Notes", NULL),
	VENTURE_FIELD("result-type", "Result type", "expense or vendor_bill after conversion",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("result-id", "Result", NULL, VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("rejection-reason", "Rejection", NULL)
};
VENTURE_DEFINE_ENTITY(VentureCaptureItem, venture_capture_item, capture_fields)
