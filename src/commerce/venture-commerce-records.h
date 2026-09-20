/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_COMMERCE_RECORDS_H
#define VENTURE_COMMERCE_RECORDS_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
#define VENTURE_TYPE_COMMERCE_IMPORT_LINK (venture_commerce_import_link_get_type())
G_DECLARE_FINAL_TYPE(VentureCommerceImportLink, venture_commerce_import_link, VENTURE, COMMERCE_IMPORT_LINK, VentureEntity)
/**
 * venture_commerce_import_link_new:
 * Returns: (transfer full): empty account identity evidence
 */
VentureCommerceImportLink *venture_commerce_import_link_new(void);
G_END_DECLS
#endif
