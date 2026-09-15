/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_CAPTURE_RECORDS_H
#define VENTURE_CAPTURE_RECORDS_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
#define VENTURE_TYPE_CAPTURE_ITEM (venture_capture_item_get_type())
VENTURE_DECLARE_ENTITY(VentureCaptureItem, venture_capture_item, CAPTURE_ITEM)
/**
 * venture_capture_item_new:
 * Returns: (transfer full): an inbox row for a captured receipt or supplier invoice
 */
G_END_DECLS
#endif
