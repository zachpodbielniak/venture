/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
static const VentureFieldDecl fields[] = {
	VENTURE_FIELD_NAME("title", "Provider", "Explicitly linked sign-in provider"),
	VENTURE_FIELD_REF("user-id", "Local user", "Locally authorized account; never provisioned by a claim", "user", VENTURE_COLUMN_FLAG_INDEXED | VENTURE_COLUMN_FLAG_PERSONAL_OWNER),
	VENTURE_FIELD_REF("connection-id", "Provider connection", "Organization-owned OIDC client", "integration_connection", VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("issuer", "Issuer", "Exact trusted issuer", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("subject", "Subject", "Verified opaque subject; never an email match", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_SENSITIVE),
	VENTURE_FIELD("identity-key", "Identity key", "Organization/issuer/subject uniqueness", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_UNIQUE | VENTURE_COLUMN_FLAG_SENSITIVE),
	VENTURE_FIELD("active", "Active", "Unlinking immediately revokes this sign-in route", VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VentureOidcIdentity, venture_oidc_identity, fields)
