/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
static const VentureFieldDecl portal_fields[] = {
	VENTURE_FIELD_REF("company-id", "Customer", NULL, "company",
		VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("token", "Access token", "256-bit invitation; omitted from generic output",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_SENSITIVE | VENTURE_COLUMN_FLAG_UNIQUE |
		VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("email", "Invitation email", NULL, VENTURE_FIELD_KIND_STRING,
		VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD("revoked", "Revoked", "Access ends without deleting financial history",
		VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_INDEXED)
};
VENTURE_DEFINE_ENTITY(VentureCustomerPortalAccess, venture_customer_portal_access, portal_fields)
