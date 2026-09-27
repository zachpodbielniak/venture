/*
 * venture-category.h - Category trees and location trees
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A `category` is one node of a tree any record type can be grouped by; a
 * `location` is one node of a tree of places stock can be. Both nest
 * through `parent-id`, and both are held to the same rules at the save: no
 * loop, no parent in another organization, and no deeper than
 * %VENTURE_CATEGORY_MAX_DEPTH. A category is also held to its tree's
 * record type (`applies-to`).
 *
 * The path a person reads ("Materials / Herbs") is computed here from the
 * parents, never stored: renaming a parent renames every path beneath it
 * without touching a row.
 */

#ifndef VENTURE_CATEGORY_H
#define VENTURE_CATEGORY_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>

G_BEGIN_DECLS

/**
 * VENTURE_CATEGORY_MAX_DEPTH:
 *
 * The most levels a category or location tree may have, counting the top
 * one. A walk up the parents stops here, so a loop written behind the
 * validator's back (by hand-written SQL, say) ends a walk instead of
 * hanging it; and a tree deeper than this is not a grouping anybody reads.
 */
#define VENTURE_CATEGORY_MAX_DEPTH (32)

/**
 * VENTURE_CATEGORY_PATH_SEPARATOR:
 *
 * What joins the levels of a computed path.
 */
#define VENTURE_CATEGORY_PATH_SEPARATOR " / "

/**
 * venture_category_install:
 * @context: the wiring
 *
 * Installs the save validators for `category` and `location` (loops,
 * cross-organization parents, depth, `applies-to`), and the one that holds
 * every record's category references to the tree's `applies-to`. Called
 * once by the context; a second context over the same database installs
 * nothing twice.
 */
void
venture_category_install(VentureContext *context);

/**
 * venture_category_path:
 * @database: the database to read
 * @entity_type: %VENTURE_TYPE_CATEGORY or %VENTURE_TYPE_LOCATION (any
 *   type whose `parent-id` names its own type)
 * @id: the node whose path is wanted
 * @error: (out) (optional): return location for a #GError
 *
 * Builds the node's path from the top of its tree down, joined by
 * %VENTURE_CATEGORY_PATH_SEPARATOR: "Materials / Herbs". Computed on every
 * call from the current names, so it is never stale. A soft-deleted
 * ancestor still names its level -- the child still hangs from it.
 *
 * Returns: (transfer full) (nullable): the path, or %NULL with @error set
 *   when @id cannot be read
 */
gchar *
venture_category_path(
	VentureDatabase	 *database,
	GType		  entity_type,
	gint64		  id,
	GError		**error
);

/**
 * venture_category_ancestor_at_depth:
 * @database: the database to read
 * @entity_type: %VENTURE_TYPE_CATEGORY or %VENTURE_TYPE_LOCATION
 * @id: the node to start from
 * @depth: the level wanted, 0 being the top of the tree
 * @error: (out) (optional): return location for a #GError
 *
 * The node's ancestor at @depth: grouping "Materials / Herbs / Rare" at
 * depth 0 gives "Materials", at depth 1 "Herbs". A node shallower than
 * @depth is its own answer, so a product filed directly under "Materials"
 * still lands in the "Materials" group at depth 1 rather than nowhere.
 *
 * Returns: the ancestor's id, or 0 with @error set when @id cannot be read
 */
gint64
venture_category_ancestor_at_depth(
	VentureDatabase	 *database,
	GType		  entity_type,
	gint64		  id,
	guint		  depth,
	GError		**error
);

/**
 * venture_category_descendants:
 * @database: the database to read
 * @entity_type: %VENTURE_TYPE_CATEGORY or %VENTURE_TYPE_LOCATION
 * @id: the node whose subtree is wanted
 * @include_self: whether @id itself is the first element
 * @error: (out) (optional): return location for a #GError
 *
 * Every live node beneath @id, breadth first, at most
 * %VENTURE_CATEGORY_MAX_DEPTH levels down. What a filter "in Materials,
 * at any depth" needs: products whose `category-id` is in this list.
 *
 * Returns: (transfer full) (element-type gint64) (nullable): the ids, or
 *   %NULL with @error set
 */
GArray *
venture_category_descendants(
	VentureDatabase	 *database,
	GType		  entity_type,
	gint64		  id,
	gboolean	  include_self,
	GError		**error
);

/**
 * venture_category_check_applies_to:
 * @database: the database to read
 * @category_id: the category a record is about to point at
 * @entity_name: the registered name of the record's type
 * @error: (out) (optional): return location for a #GError
 *
 * Refuses a category whose `applies-to` names a different record type: a
 * product filed under an expense head is a grouping no report of either
 * will show. A category with no `applies-to` fits anything. Built-in
 * reference fields are checked by the validator this file installs;
 * custom reference fields call this themselves.
 *
 * Returns: %TRUE when @category_id may be used by @entity_name
 */
gboolean
venture_category_check_applies_to(
	VentureDatabase	 *database,
	gint64		  category_id,
	const gchar	 *entity_name,
	GError		**error
);

/**
 * venture_category_check_tree_node:
 * @database: the database to read
 * @entity: the node about to be saved; its type must have a `parent-id`
 *   reference naming its own type, and a `name`
 * @error: (out) (optional): return location for a #GError
 *
 * The rules every node of a self-referencing tree meets, whatever the
 * type: its parent is not itself or anything beneath it, is in the same
 * organization, and the tree is no deeper than
 * %VENTURE_CATEGORY_MAX_DEPTH. Categories and locations are checked with
 * it by the validators this file installs; any other type that nests
 * through its own `parent-id` -- a goal's sub-goals -- calls it from its
 * own save validator, so there is one definition of a loop. The walk
 * reads under a trusted scope: a loop is a loop whoever is saving.
 *
 * Returns: %TRUE when the node may be saved where it says it sits
 */
gboolean
venture_category_check_tree_node(
	VentureDatabase	 *database,
	VentureEntity	 *entity,
	GError		**error
);

G_END_DECLS

#endif /* VENTURE_CATEGORY_H */
