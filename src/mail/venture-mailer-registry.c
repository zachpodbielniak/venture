/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
struct _VentureMailerRegistry { GObject parent_instance; GHashTable *mailers; };
G_DEFINE_FINAL_TYPE(VentureMailerRegistry, venture_mailer_registry, G_TYPE_OBJECT)
static void registry_finalize(GObject *object)
{
	g_hash_table_unref(VENTURE_MAILER_REGISTRY(object)->mailers);
	G_OBJECT_CLASS(venture_mailer_registry_parent_class)->finalize(object);
}
static void venture_mailer_registry_class_init(VentureMailerRegistryClass *klass) { G_OBJECT_CLASS(klass)->finalize = registry_finalize; }
static void venture_mailer_registry_init(VentureMailerRegistry *self) { self->mailers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref); }
VentureMailerRegistry *venture_mailer_registry_new(void) { return g_object_new(VENTURE_TYPE_MAILER_REGISTRY, NULL); }
void venture_mailer_registry_add(VentureMailerRegistry *self, const gchar *name, VentureMailer *mailer)
{
	g_return_if_fail(name && *name && VENTURE_IS_MAILER(mailer));
	g_hash_table_replace(self->mailers, g_strdup(name), g_object_ref(mailer));
}
VentureMailer *venture_mailer_registry_lookup(VentureMailerRegistry *self, const gchar *name) { return g_hash_table_lookup(self->mailers, name); }
