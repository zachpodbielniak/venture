/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

gchar **venture_tags_parse(const gchar *tags) { (void)tags; return NULL; }
gboolean venture_tags_has(const gchar *tags, const gchar *tag) { (void)tags; (void)tag; return FALSE; }
gboolean venture_tags_valid(const gchar *tag) { (void)tag; return FALSE; }
gchar *venture_tags_add(const gchar *tags, const gchar *tag) { (void)tags; (void)tag; return NULL; }
gchar *venture_tags_remove(const gchar *tags, const gchar *tag) { (void)tags; (void)tag; return NULL; }
gchar **venture_tags_added(const gchar *before, const gchar *after) { (void)before; (void)after; return NULL; }
void venture_tags_actions_register(VentureDatabase *database) { (void)database; }
