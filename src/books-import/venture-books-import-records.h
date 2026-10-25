/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_BOOKS_IMPORT_RECORDS_H
#define VENTURE_BOOKS_IMPORT_RECORDS_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
#define VENTURE_TYPE_BOOKS_IMPORT (venture_books_import_get_type())
VENTURE_DECLARE_ENTITY(VentureBooksImport, venture_books_import, BOOKS_IMPORT)
#define VENTURE_TYPE_BOOKS_IMPORT_ROW (venture_books_import_row_get_type())
VENTURE_DECLARE_ENTITY(VentureBooksImportRow, venture_books_import_row, BOOKS_IMPORT_ROW)
G_END_DECLS
#endif
