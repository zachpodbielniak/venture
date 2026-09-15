/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_DOCUMENT_SERVICE_H
#define VENTURE_DOCUMENT_SERVICE_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
#define VENTURE_TYPE_DOCUMENT_SERVICE (venture_document_service_get_type())
G_DECLARE_FINAL_TYPE(VentureDocumentService, venture_document_service, VENTURE, DOCUMENT_SERVICE, GObject)
/**
 * venture_document_service_get:
 * @database: database owning the records
 *
 * Returns the per-database service. The database owns this reference.
 *
 * Returns: (transfer none): borrowed result
 */
VentureDocumentService *venture_document_service_get(VentureDatabase *database);
/**
 * venture_document_service_compose_invoice:
 * @self: the service or registry instance
 * @organization_id: target legal entity ID
 * @spec: document specification as a JSON object
 * @actor: (nullable): audit actor; NULL for internal service work
 * @error: (out) (optional): return location for an error
 *
 * Returns: (transfer full) (nullable): owned result
 */
VentureEntity *venture_document_service_compose_invoice(VentureDocumentService *self,
	gint64 organization_id, JsonObject *spec, const VentureActor *actor, GError **error);
/**
 * venture_document_service_compose_quote:
 * @self: the service or registry instance
 * @organization_id: target legal entity ID
 * @spec: document specification as a JSON object
 * @actor: (nullable): audit actor; NULL for internal service work
 * @error: (out) (optional): return location for an error
 *
 * Returns: (transfer full) (nullable): owned result
 */
VentureEntity *venture_document_service_compose_quote(VentureDocumentService *self,
	gint64 organization_id, JsonObject *spec, const VentureActor *actor, GError **error);
G_END_DECLS
#endif
