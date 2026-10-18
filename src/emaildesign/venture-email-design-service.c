/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Stubs: the interface the tests are written against, before the service. */
#include <venture.h>

static void todo(GError **error)
{
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_FAILED, "Email design is not implemented yet");
}
VentureEmailLook *venture_email_look_new_default(const gchar *name)
{
	VentureEmailLook *look = g_new0(VentureEmailLook, 1);
	look->name = g_strdup(name);
	return look;
}
void venture_email_look_free(VentureEmailLook *look)
{
	if (look == NULL) return;
	g_free(look->name); g_free(look->logo_url); g_free(look->primary); g_free(look->accent);
	g_free(look->text); g_free(look->background); g_free(look->card); g_free(look->dark_primary);
	g_free(look->dark_text); g_free(look->dark_background); g_free(look->dark_card); g_free(look->font);
	g_free(look->website); g_free(look->address); g_free(look);
}
const gchar *venture_email_font_stack(const gchar *font) { (void)font; return NULL; }
gboolean venture_email_design_validate(const gchar *json, GError **error) { (void)json; todo(error); return FALSE; }
gboolean venture_email_design_render(const gchar *json, const VentureEmailLook *look, const VentureEmailResolver *resolver,
	const gchar *title, gboolean template_syntax, gchar **html, gchar **text, GError **error)
{
	(void)json; (void)look; (void)resolver; (void)title; (void)template_syntax;
	*html = NULL; *text = NULL; todo(error); return FALSE;
}
const gchar *venture_hosted_image_sniff(GBytes *data, gint *width, gint *height, const gchar **extension)
{
	(void)data; (void)width; (void)height; (void)extension; return NULL;
}
VentureEntity *venture_hosted_image_store(VentureContext *context, gint64 organization_id, const gchar *filename,
	GBytes *data, const gchar *alt, const VentureActor *actor, GError **error)
{
	(void)context; (void)organization_id; (void)filename; (void)data; (void)alt; (void)actor; todo(error); return NULL;
}
gchar *venture_hosted_image_url(VentureContext *context, VentureEntity *image) { (void)context; (void)image; return g_strdup(""); }
GBytes *venture_hosted_image_read(VentureContext *context, gint64 organization_id, const gchar *file,
	const gchar **content_type, GError **error)
{
	(void)context; (void)organization_id; (void)file; *content_type = NULL; todo(error); return NULL;
}
VentureEmailLook *venture_email_look_load(VentureContext *context, gint64 organization_id)
{
	(void)context; (void)organization_id; return venture_email_look_new_default(NULL);
}
VentureEntity *venture_email_design_save(VentureContext *context, VentureEntity *template, const gchar *design,
	const gchar *source, const gchar *note, const VentureActor *actor, GError **error)
{
	(void)context; (void)template; (void)design; (void)source; (void)note; (void)actor; todo(error); return NULL;
}
VentureEntity *venture_email_design_undo(VentureContext *context, gint64 organization_id, gint64 template_id,
	const VentureActor *actor, GError **error)
{
	(void)context; (void)organization_id; (void)template_id; (void)actor; todo(error); return NULL;
}
void venture_email_design_result_free(VentureEmailDesignResult *result)
{
	if (result == NULL) return;
	g_clear_object(&result->template); g_clear_object(&result->version);
	g_clear_pointer(&result->stripped, g_ptr_array_unref); g_free(result->ask); g_free(result);
}
VentureEmailDesignResult *venture_email_design_from_brief(VentureContext *context, gint64 organization_id,
	gint64 template_id, const gchar *name, const gchar *brief, const VentureActor *actor, GError **error)
{
	(void)context; (void)organization_id; (void)template_id; (void)name; (void)brief; (void)actor; todo(error); return NULL;
}
VentureEmailDesignResult *venture_email_design_edit(VentureContext *context, gint64 organization_id,
	gint64 template_id, const gchar *instruction, const VentureActor *actor, GError **error)
{
	(void)context; (void)organization_id; (void)template_id; (void)instruction; (void)actor; todo(error); return NULL;
}
gchar *venture_email_design_ground(const gchar *design, const gchar *const *sources, GPtrArray *stripped, GError **error)
{
	(void)design; (void)sources; (void)stripped; todo(error); return NULL;
}
gboolean venture_email_design_dismiss_logo_ask(VentureContext *context, gint64 organization_id,
	const VentureActor *actor, GError **error)
{
	(void)context; (void)organization_id; (void)actor; todo(error); return FALSE;
}
void venture_email_design_install(VentureContext *context) { (void)context; }
void venture_email_design_preview_register(void) { }
