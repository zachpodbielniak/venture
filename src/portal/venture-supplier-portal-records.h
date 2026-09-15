/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_SUPPLIER_PORTAL_RECORDS_H
#define VENTURE_SUPPLIER_PORTAL_RECORDS_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
#define VENTURE_TYPE_SUPPLIER_PORTAL_ACCESS (venture_supplier_portal_access_get_type())
VENTURE_DECLARE_ENTITY(VentureSupplierPortalAccess, venture_supplier_portal_access, SUPPLIER_PORTAL_ACCESS)
G_END_DECLS
#endif
