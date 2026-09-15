/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_PORTAL_RECORDS_H
#define VENTURE_PORTAL_RECORDS_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
#define VENTURE_TYPE_CUSTOMER_PORTAL_ACCESS (venture_customer_portal_access_get_type())
VENTURE_DECLARE_ENTITY(VentureCustomerPortalAccess, venture_customer_portal_access, CUSTOMER_PORTAL_ACCESS)
G_END_DECLS
#endif
