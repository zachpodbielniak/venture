/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_MAILER_REGISTRY_H
#define VENTURE_MAILER_REGISTRY_H
#include "mail/venture-mailer.h"
#define VENTURE_TYPE_MAILER_REGISTRY (venture_mailer_registry_get_type())
G_DECLARE_FINAL_TYPE(VentureMailerRegistry, venture_mailer_registry, VENTURE, MAILER_REGISTRY, GObject)
/**
 * venture_mailer_registry_new:
 * Returns: (transfer full): an empty implementation registry
 */
VentureMailerRegistry *venture_mailer_registry_new(void);
/**
 * venture_mailer_registry_add:
 * @self: registry
 * @name: implementation name
 * @mailer: (transfer none): implementation to retain, replacing the name
 */
void venture_mailer_registry_add(VentureMailerRegistry *self, const gchar *name, VentureMailer *mailer);
/**
 * venture_mailer_registry_lookup:
 * @self: registry
 * @name: implementation name
 * Returns: (transfer none) (nullable): registered implementation
 */
VentureMailer *venture_mailer_registry_lookup(VentureMailerRegistry *self, const gchar *name);
#endif
