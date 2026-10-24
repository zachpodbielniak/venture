/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_TAGS_H
#define VENTURE_TAGS_H
G_BEGIN_DECLS

/* Tags are the one comma-separated tags field contacts and companies
 * already have. These are the operations segments, triggers and the tag
 * actions share, so "has this tag" means one thing everywhere: a whole
 * tag between commas, trimmed, any capitals. */

/**
 * VENTURE_TAG_MAX_LENGTH:
 *
 * The longest tag, in bytes.
 */
#define VENTURE_TAG_MAX_LENGTH 64

/**
 * venture_tags_parse:
 * @tags: (nullable): a tags field
 *
 * Returns: (transfer full) (array zero-terminated=1): the tags in order,
 *   trimmed, empty ones dropped, a repeat (any capitals) dropped after its
 *   first spelling
 */
gchar **venture_tags_parse(const gchar *tags);
/**
 * venture_tags_has:
 * @tags: (nullable): a tags field
 * @tag: one tag
 * Returns: whether @tag is one of @tags, ignoring capitals and spacing
 */
gboolean venture_tags_has(const gchar *tags, const gchar *tag);
/**
 * venture_tags_valid:
 * @tag: (nullable): a proposed tag
 * Returns: whether it is non-empty, at most %VENTURE_TAG_MAX_LENGTH bytes
 *   and free of commas and line breaks
 */
gboolean venture_tags_valid(const gchar *tag);
/**
 * venture_tags_add:
 * @tags: (nullable): a tags field
 * @tag: the tag to add
 * Returns: (transfer full): the field with @tag added at the end unless it
 *   is already there, written "a, b"
 */
gchar *venture_tags_add(const gchar *tags, const gchar *tag);
/**
 * venture_tags_remove:
 * @tags: (nullable): a tags field
 * @tag: the tag to remove, any capitals
 * Returns: (transfer full): the field without @tag, written "a, b"
 */
gchar *venture_tags_remove(const gchar *tags, const gchar *tag);
/**
 * venture_tags_added:
 * @before: (nullable): the stored field
 * @after: (nullable): the field being saved
 * Returns: (transfer full) (array zero-terminated=1): the tags in @after
 *   that @before did not have, in @after's spelling and order
 */
gchar **venture_tags_added(const gchar *before, const gchar *after);
/**
 * venture_tags_actions_register:
 * @database: the action registry's database
 *
 * Adds the add_tag and remove_tag actions on contacts and companies.
 */
void venture_tags_actions_register(VentureDatabase *database);

G_END_DECLS
#endif
