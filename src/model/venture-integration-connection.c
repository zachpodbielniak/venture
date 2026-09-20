/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

static const VentureFieldDecl fields[] = {
	VENTURE_FIELD_NAME("provider", "Provider", "Stable integration identifier"),
	VENTURE_FIELD("account-id", "Account", "Safe provider account identity", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("environment", "Environment", "test or live", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("enabled", "Enabled", "Disabled bindings remain as historical evidence", VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("credential-revision", "Credential revision", "Changes on rotation even when credentials are the only change", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("sealed-settings", "Credentials", "Authenticated ciphertext; write-only through settings", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_SENSITIVE)
};
VENTURE_DEFINE_ENTITY(VentureIntegrationConnection, venture_integration_connection, fields)
