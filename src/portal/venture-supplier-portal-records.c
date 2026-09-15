/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
static const VentureFieldDecl supplier_portal_fields[] = {
	VENTURE_FIELD_REF("company-id", "Supplier", "A vendor company; isolation is per token", "company",
		VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("token", "Access token", "256-bit invitation; omitted from generic output",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_SENSITIVE | VENTURE_COLUMN_FLAG_UNIQUE |
		VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("email", "Invitation email", NULL, VENTURE_FIELD_KIND_STRING,
		VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD("revoked", "Revoked", "Access ends without deleting payable history",
		VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_INDEXED)
};
VENTURE_DEFINE_ENTITY(VentureSupplierPortalAccess, venture_supplier_portal_access, supplier_portal_fields)
