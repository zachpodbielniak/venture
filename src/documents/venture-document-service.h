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
/**
 * VENTURE_DOCUMENT_EXTRACT_MAX_PAGES:
 *
 * The most pages venture_document_extract_text() reads from one PDF.
 */
#define VENTURE_DOCUMENT_EXTRACT_MAX_PAGES (2000)
/**
 * VENTURE_DOCUMENT_EXTRACT_MAX_BYTES:
 *
 * Once this much text has been extracted from a PDF, no further page is read.
 */
#define VENTURE_DOCUMENT_EXTRACT_MAX_BYTES (4 * 1024 * 1024)
/**
 * venture_document_extract_text:
 * @content_type: (nullable): the declared MIME type
 * @filename: (nullable): the original file name
 * @data: the file's bytes
 *
 * Pulls readable text out of a filed document: PDFs through poppler when
 * the build has it, HTML through venture_document_html_to_text(), and
 * anything that announces itself as text -- including JSON, CSV and YAML,
 * which is what exports and invoices actually arrive as -- as UTF-8, read
 * as windows-1252 when it is not valid UTF-8. The upload route and inbound
 * mail both call this, so a receipt yields the same text either way. A PDF
 * is read up to %VENTURE_DOCUMENT_EXTRACT_MAX_PAGES pages and roughly
 * %VENTURE_DOCUMENT_EXTRACT_MAX_BYTES of text, because it may be a
 * stranger's attachment parsed on the main loop.
 *
 * Returns: (transfer full) (nullable): the text, or %NULL when there is none
 */
gchar *venture_document_extract_text(const gchar *content_type, const gchar *filename, GBytes *data);
/**
 * venture_document_html_to_text:
 * @html: markup, not necessarily well formed
 * @length: bytes of @html, or -1 when NUL-terminated
 *
 * Strips tags, scripts, styles and comments, turns block boundaries into
 * line breaks and decodes the common and numeric character references. A
 * scanner rather than a regex, because the input is somebody else's markup:
 * an unbalanced tag must degrade to text, not to a loop.
 *
 * Returns: (transfer full): valid UTF-8 text, possibly empty
 */
gchar *venture_document_html_to_text(const gchar *html, gssize length);
G_END_DECLS
#endif
