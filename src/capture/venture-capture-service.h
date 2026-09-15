/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_CAPTURE_SERVICE_H
#define VENTURE_CAPTURE_SERVICE_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
#define VENTURE_TYPE_CAPTURE_SERVICE (venture_capture_service_get_type())
G_DECLARE_FINAL_TYPE(VentureCaptureService, venture_capture_service, VENTURE, CAPTURE_SERVICE, GObject)

/**
 * venture_capture_service_get:
 * @database: owning repository
 * Returns: (transfer none): its capture inbox service
 */
VentureCaptureService *venture_capture_service_get(VentureDatabase *database);

/**
 * venture_capture_service_ingest:
 * @self: the service
 * @kind: receipt or supplier_invoice
 * @title: a label for the inbox row
 * @source: (nullable): how it arrived
 * @document_id: filed document, or 0
 * @vendor: (nullable): printed vendor name
 * @amount: (nullable): captured total
 * @occurred_at: (nullable): document date
 * @notes: (nullable): operator notes
 * @actor: (nullable): audit actor
 * @error: (out) (optional): failure
 * Returns: (transfer full) (nullable): the inbox row
 */
VentureEntity *venture_capture_service_ingest(VentureCaptureService *self, const gchar *kind,
	const gchar *title, const gchar *source, gint64 document_id, const gchar *vendor,
	const VentureMoney *amount, GDateTime *occurred_at, const gchar *notes,
	const VentureActor *actor, GError **error);

/**
 * venture_capture_service_ingest_for_organization:
 * @self: the service
 * @organization_id: owning organization
 * @kind: receipt or supplier_invoice
 * @title: inbox label
 * @source: (nullable): source description
 * @document_id: filed document, or zero
 * @vendor: (nullable): printed vendor
 * @amount: (nullable): captured total
 * @occurred_at: (nullable): document date
 * @notes: (nullable): operator notes
 * @actor: (nullable): audit actor
 * @error: (out) (optional): failure
 * Returns: (transfer full) (nullable): the scoped inbox row
 */
VentureEntity *venture_capture_service_ingest_for_organization(VentureCaptureService *self, gint64 organization_id,
	const gchar *kind, const gchar *title, const gchar *source, gint64 document_id, const gchar *vendor,
	const VentureMoney *amount, GDateTime *occurred_at, const gchar *notes,
	const VentureActor *actor, GError **error);

/**
 * venture_capture_service_convert:
 * @self: the service
 * @item: an inbox row
 * @as: expense or vendor_bill
 * @options: (nullable): company_id, category, quantity
 * @actor: (nullable): audit actor
 * @error: (out) (optional): failure
 *
 * Creates the destination through the ordinary expense or vendor-bill save,
 * never by writing settlement tables directly.
 * Returns: (transfer full) (nullable): the created expense or bill
 */
VentureEntity *venture_capture_service_convert(VentureCaptureService *self, VentureEntity *item,
	const gchar *as, JsonObject *options, const VentureActor *actor, GError **error);

/**
 * venture_capture_service_reject:
 * @self: the service
 * @item: an inbox row
 * @reason: (nullable): why it was rejected
 * @actor: (nullable): audit actor
 * @error: (out) (optional): failure
 * Returns: %TRUE on success
 */
gboolean venture_capture_service_reject(VentureCaptureService *self, VentureEntity *item,
	const gchar *reason, const VentureActor *actor, GError **error);

/**
 * venture_capture_save_hook: (skip)
 */
gboolean venture_capture_save_hook(VentureDatabase *database, VentureEntity *record,
	const VentureActor *actor, gboolean *handled, GError **error);

G_END_DECLS
#endif
