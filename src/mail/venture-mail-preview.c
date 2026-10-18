/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Red-phase stub: the API exists, nothing is implemented yet. */
#include <venture.h>
G_DEFINE_INTERFACE(VentureMailLinkChecker, venture_mail_link_checker, G_TYPE_OBJECT)
static void venture_mail_link_checker_default_init(VentureMailLinkCheckerInterface *iface) { (void)iface; }
gboolean venture_mail_link_checker_check(VentureMailLinkChecker *self, const gchar *url, GCancellable *cancellable, GError **error)
{ (void)self; (void)url; (void)cancellable; (void)error; return TRUE; }
struct _VentureHttpLinkChecker { GObject parent_instance; };
G_DEFINE_FINAL_TYPE(VentureHttpLinkChecker, venture_http_link_checker, G_TYPE_OBJECT)
static void venture_http_link_checker_class_init(VentureHttpLinkCheckerClass *klass) { (void)klass; }
static void venture_http_link_checker_init(VentureHttpLinkChecker *self) { (void)self; }
VentureHttpLinkChecker *venture_http_link_checker_new(void) { return g_object_new(VENTURE_TYPE_HTTP_LINK_CHECKER, NULL); }
void venture_mail_preview_free(VentureMailPreview *preview) { if (preview) g_free(preview); }
const VentureMailCheck *venture_mail_preview_get_check(const VentureMailPreview *preview, const gchar *id) { (void)preview; (void)id; return NULL; }
void venture_mail_visible_bodies(VentureMailMessage *message, gchar **text, gchar **html) { (void)message; *text = g_strdup(""); *html = NULL; }
VentureMailPreview *venture_mail_preview_render(VentureMailMessage *message, const gchar *kind, gboolean bulk, VentureMailLinkChecker *checker, GCancellable *cancellable)
{ (void)message; (void)kind; (void)bulk; (void)checker; (void)cancellable; return g_new0(VentureMailPreview, 1); }
void venture_mail_preview_register(const VentureMailPreviewKind *kind) { (void)kind; }
const VentureMailPreviewKind *venture_mail_preview_lookup(const gchar *id) { (void)id; return NULL; }
GPtrArray *venture_mail_preview_kinds(void) { return g_ptr_array_new(); }
VentureMailPreview *venture_mail_preview_build(VentureContext *context, const gchar *kind, gint64 organization_id, gint64 source_id, GDateTime *now, VentureMailLinkChecker *checker, GError **error)
{ (void)context; (void)kind; (void)organization_id; (void)source_id; (void)now; (void)checker; g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Not implemented"); return NULL; }
VentureMailPreview *venture_mail_preview_message(VentureContext *context, gint64 organization_id, gint64 message_id, VentureMailLinkChecker *checker, GError **error)
{ (void)context; (void)organization_id; (void)message_id; (void)checker; g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Not implemented"); return NULL; }
JsonNode *venture_mail_preview_to_json(const VentureMailPreview *preview) { (void)preview; return json_node_new(JSON_NODE_NULL); }
gchar *venture_mail_preview_actor_email(VentureDatabase *database, const VentureActor *actor, GError **error) { (void)database; (void)actor; (void)error; return NULL; }
VentureMailMessage *venture_mail_preview_enqueue_test(VentureDatabase *database, VentureMailMessage *message, const gchar *key, const VentureActor *actor, GError **error)
{ (void)database; (void)message; (void)key; (void)actor; g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Not implemented"); return NULL; }
VentureMailMessage *venture_mail_preview_send_test(VentureContext *context, const gchar *kind, gint64 organization_id, gint64 source_id, const gchar *token, const VentureActor *actor, GError **error)
{ (void)context; (void)kind; (void)organization_id; (void)source_id; (void)token; (void)actor; g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Not implemented"); return NULL; }
