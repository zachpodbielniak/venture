/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

static const VentureFieldDecl fields[] = {
	VENTURE_FIELD_REF("private-owner-id", "Private owner", "Private credential owner; zero is an organization binding", "user", VENTURE_COLUMN_FLAG_OPTIONAL_PERSONAL_OWNER),
	VENTURE_FIELD("provider", "Provider", "Stable integration identifier", VENTURE_FIELD_KIND_STRING,
		VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_SEARCHABLE | VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION),
	VENTURE_FIELD("account-id", "Account", "Safe provider account identity", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("environment", "Environment", "test or live", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("enabled", "Enabled", "Disabled bindings remain as historical evidence", VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("credential-revision", "Credential revision", "Changes on rotation even when credentials are the only change", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("sealed-settings", "Credentials", "Authenticated ciphertext; write-only through settings", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_SENSITIVE)
};
VENTURE_DEFINE_ENTITY_WITH_CODE(VentureIntegrationConnection, venture_integration_connection, fields,
	venture_entity_class_set_field_unique_scope(VENTURE_ENTITY_CLASS(klass), "provider", NULL, "enabled");)
