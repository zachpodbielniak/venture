/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_EMAIL_DESIGN_PRIVATE_H
#define VENTURE_EMAIL_DESIGN_PRIVATE_H
#include <venture.h>
G_BEGIN_DECLS
/* What the sources allow a design to say: their numbers and whether
 * they offer anything free. */
typedef struct {
	GHashTable *numbers;
	gboolean free_ok;
} VentureEmailGrounds;
void venture_email_grounds_init(VentureEmailGrounds *grounds, const gchar *const *sources);
void venture_email_grounds_clear(VentureEmailGrounds *grounds);
/* The first claim in @text the grounds do not allow, or NULL. */
gchar *venture_email_grounds_claim(const VentureEmailGrounds *grounds, const gchar *text);
/* Every http or https address in @text, into @into. */
void venture_email_collect_urls(GHashTable *into, const gchar *text);
/* Every address a design links to or loads, into @into. */
void venture_email_design_urls(const gchar *design, GHashTable *into);
/* Every hosted image id a design names, into @into (gint64 keys). */
void venture_email_design_image_ids(const gchar *design, GHashTable *into);
/* The database's context, set by venture_email_design_install(). */
VentureContext *venture_email_design_context(VentureDatabase *database);
/* Owner or administrator of @organization_id, when a person is acting. */
gboolean venture_email_design_require_manager(VentureContext *context, gint64 organization_id, GError **error);
/* The hosted image validator; called by venture_email_design_install(). */
void venture_hosted_image_install(VentureDatabase *database);
/* Fills the dark-mode colours a brand left empty and lowercases colours. */
void venture_email_look_finish(VentureEmailLook *look);
/* The filing flag the service sets while it writes rows only it may write. */
#define VENTURE_EMAIL_DESIGN_FILING "venture-email-design-filing"
/* Asks for a logo the first time an organization without one gets a
 * design; NULL when it has one, or was asked, or said no. */
gchar *venture_email_design_logo_ask(VentureContext *context, gint64 organization_id, const VentureActor *actor);
G_END_DECLS
#endif
