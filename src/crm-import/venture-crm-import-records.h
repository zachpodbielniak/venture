/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_CRM_IMPORT_RECORDS_H
#define VENTURE_CRM_IMPORT_RECORDS_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
#define VENTURE_TYPE_CRM_IMPORT (venture_crm_import_get_type())
VENTURE_DECLARE_ENTITY(VentureCrmImport, venture_crm_import, CRM_IMPORT)
#define VENTURE_TYPE_CRM_IMPORT_ROW (venture_crm_import_row_get_type())
VENTURE_DECLARE_ENTITY(VentureCrmImportRow, venture_crm_import_row, CRM_IMPORT_ROW)
G_END_DECLS
#endif
