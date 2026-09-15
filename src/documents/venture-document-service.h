/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_DOCUMENT_SERVICE_H
#define VENTURE_DOCUMENT_SERVICE_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
#define VENTURE_TYPE_DOCUMENT_SERVICE (venture_document_service_get_type())
G_DECLARE_FINAL_TYPE(VentureDocumentService, venture_document_service, VENTURE, DOCUMENT_SERVICE, GObject)
VentureDocumentService *venture_document_service_get(VentureDatabase *database);
VentureEntity *venture_document_service_compose_invoice(VentureDocumentService *self,
	gint64 organization_id, JsonObject *spec, const VentureActor *actor, GError **error);
VentureEntity *venture_document_service_compose_quote(VentureDocumentService *self,
	gint64 organization_id, JsonObject *spec, const VentureActor *actor, GError **error);
G_END_DECLS
#endif
