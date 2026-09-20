/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

static const VentureFieldDecl fields[] = {
	VENTURE_FIELD_NAME("name", "Identity", "Retained account-scoped import identity"),
	VENTURE_FIELD("provider", "Provider", "Stable commerce provider namespace", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("account-id", "Account", "Immutable remote account identity", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("environment", "Environment", "Live or test account namespace", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_REF("invoice-id", "Invoice", "Exactly one invoice or customer target", "invoice", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("company-id", "Customer", "Exactly one invoice or customer target", "company", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("remote-id", "Remote identity", "Original provider identity, never guessed from email", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_TEXT("reason", "Reason", "Explicit legacy adoption or import provenance"),
	VENTURE_FIELD_REF("connection-id", "Imported binding", "Historical encrypted binding; rotation does not change replay identity", "integration_connection", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("identity-key", "Identity key", "Organization, provider, account, environment, object kind and remote identity", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_UNIQUE),
	VENTURE_FIELD("target-key", "Target key", "One account claim for each local record", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_UNIQUE)
};
VENTURE_DEFINE_ENTITY(VentureCommerceImportLink, venture_commerce_import_link, fields)
