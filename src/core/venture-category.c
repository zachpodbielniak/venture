/*
 * venture-category.c - Category trees and location trees
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Two record types nest through a parent-id naming their own type, and the
 * schema -- free of foreign keys by design -- cannot stop a loop. So the
 * rules live here, as save validators every writer passes through: the
 * form, the API, an approved staged change, the assistant, a plugin. The
 * reads (path, ancestor, subtree) are here too, because a path that is
 * computed has to be computed in one place or two pages will disagree.
 */

#include "venture.h"

#include <string.h>

/* The key the installed marker hangs off the database under. */
#define VENTURE_CATEGORY_STATE_KEY "venture-category-state"

/* --- Reading a tree ---------------------------------------------------------- */

/*
 * The parent of one node. A property read rather than a column, so the
 * same walk serves categories, locations and any later type whose
 * parent-id names itself.
 */
static gint64
venture_category_parent_of(VentureEntity *node)
{
	gint64 parent_id;

	parent_id = 0;
	g_object_get(node, "parent-id", &parent_id, NULL);

	return parent_id;
}

/* One node, or NULL with @error set naming what could not be read. */
static VentureEntity *
venture_category_load(
	VentureDatabase	 *database,
	GType		  entity_type,
	gint64		  id,
	GError		**error
){
	g_autoptr(GError) local_error = NULL;
	g_autoptr(VentureEntity) prototype = NULL;
	VentureEntity *node;

	node = venture_database_get(database, entity_type, id, &local_error);

	if (NULL != node)
		return node;

	if (NULL != local_error)
	{
		g_propagate_error(error, g_steal_pointer(&local_error));
		return NULL;
	}

	prototype = g_object_new(entity_type, NULL);
	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
	            "%s #%" G_GINT64_FORMAT " does not exist",
	            venture_entity_get_entity_name(prototype), id);
	return NULL;
}

/*
 * The node and its ancestors, nearest first, as loaded entities. Stops at
 * the top, at an ancestor that cannot be read, at a node seen before (a
 * loop written behind the validator's back) or at the depth bound -- a
 * walk that ends is worth more than one that is exact about a broken tree.
 */
static GPtrArray *
venture_category_chain(
	VentureDatabase	 *database,
	GType		  entity_type,
	gint64		  id,
	GError		**error
){
	g_autoptr(GPtrArray) chain = NULL;
	g_autoptr(GHashTable) seen = NULL;
	VentureEntity *node;
	gint64 cursor;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	g_return_val_if_fail(g_type_is_a(entity_type, VENTURE_TYPE_ENTITY), NULL);

	node = venture_category_load(database, entity_type, id, error);

	if (NULL == node)
		return NULL;

	chain = g_ptr_array_new_with_free_func(g_object_unref);
	seen = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	g_ptr_array_add(chain, node);
	g_hash_table_add(seen, g_memdup2(&id, sizeof(id)));

	cursor = venture_category_parent_of(node);

	while ((0 != cursor) && (chain->len < VENTURE_CATEGORY_MAX_DEPTH) &&
	       !g_hash_table_contains(seen, &cursor))
	{
		node = venture_database_get(database, entity_type, cursor, NULL);

		if (NULL == node)
			break;

		g_ptr_array_add(chain, node);
		g_hash_table_add(seen, g_memdup2(&cursor, sizeof(cursor)));
		cursor = venture_category_parent_of(node);
	}

	return g_steal_pointer(&chain);
}

gchar *
venture_category_path(
	VentureDatabase	 *database,
	GType		  entity_type,
	gint64		  id,
	GError		**error
){
	g_autoptr(GPtrArray) chain = NULL;
	GString *path;
	guint i;

	chain = venture_category_chain(database, entity_type, id, error);

	if (NULL == chain)
		return NULL;

	path = g_string_new(NULL);

	/* The chain is nearest first; a path reads from the top down. */
	for (i = chain->len; i > 0; i--)
	{
		g_autofree gchar *name = NULL;

		g_object_get(g_ptr_array_index(chain, i - 1), "name", &name, NULL);

		if (path->len > 0)
			g_string_append(path, VENTURE_CATEGORY_PATH_SEPARATOR);

		g_string_append(path, (NULL != name) ? name : "");
	}

	return g_string_free(path, FALSE);
}

gint64
venture_category_ancestor_at_depth(
	VentureDatabase	 *database,
	GType		  entity_type,
	gint64		  id,
	guint		  depth,
	GError		**error
){
	g_autoptr(GPtrArray) chain = NULL;

	chain = venture_category_chain(database, entity_type, id, error);

	if (NULL == chain)
		return 0;

	/* chain[len - 1] is the top (depth 0); a node shallower than the
	 * depth asked for is its own group. */
	if (depth >= chain->len)
		return venture_entity_get_id(g_ptr_array_index(chain, 0));

	return venture_entity_get_id(g_ptr_array_index(chain, chain->len - 1 - depth));
}

GArray *
venture_category_descendants(
	VentureDatabase	 *database,
	GType		  entity_type,
	gint64		  id,
	gboolean	  include_self,
	GError		**error
){
	g_autoptr(GArray) found = NULL;
	g_autoptr(GHashTable) seen = NULL;
	g_autoptr(GPtrArray) frontier = NULL;
	guint level;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	g_return_val_if_fail(g_type_is_a(entity_type, VENTURE_TYPE_ENTITY), NULL);

	found = g_array_new(FALSE, FALSE, sizeof(gint64));
	seen = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	g_hash_table_add(seen, g_memdup2(&id, sizeof(id)));

	if (include_self)
		g_array_append_val(found, id);

	frontier = g_ptr_array_new_with_free_func(g_free);
	g_ptr_array_add(frontier, g_strdup_printf("%" G_GINT64_FORMAT, id));

	/*
	 * One query per level, never one per node: a level's children are
	 * everything whose parent is in the level above. Bounded like the walk
	 * up, and a node seen before is not followed again, so a loop in the
	 * table ends the search instead of repeating it.
	 */
	for (level = 0; (level < VENTURE_CATEGORY_MAX_DEPTH) && (frontier->len > 0); level++)
	{
		g_autoptr(VentureQuery) query = NULL;
		g_autoptr(GPtrArray) rows = NULL;
		g_autoptr(GPtrArray) next = NULL;
		guint i;

		query = venture_query_new(entity_type);
		venture_query_set_limit(query, 0);

		if (!venture_query_add_filter(query, "parent-id", VENTURE_FILTER_OP_IN,
		                              frontier, error))
			return NULL;

		rows = venture_database_find(database, query, error);

		if (NULL == rows)
			return NULL;

		next = g_ptr_array_new_with_free_func(g_free);

		for (i = 0; i < rows->len; i++)
		{
			gint64 child;

			child = venture_entity_get_id(g_ptr_array_index(rows, i));

			if (g_hash_table_contains(seen, &child))
				continue;

			g_hash_table_add(seen, g_memdup2(&child, sizeof(child)));
			g_array_append_val(found, child);
			g_ptr_array_add(next, g_strdup_printf("%" G_GINT64_FORMAT, child));
		}

		g_clear_pointer(&frontier, g_ptr_array_unref);
		frontier = g_steal_pointer(&next);
	}

	return g_steal_pointer(&found);
}

/* --- The save validators ------------------------------------------------------ */

/* "" and NULL are the same absence of a type. */
static const gchar *
venture_category_or_empty(const gchar *text)
{
	return (NULL != text) ? text : "";
}

/*
 * The rules every node of a tree meets, whichever type it is: its parent
 * exists, is in the same organization, is not itself or any node beneath
 * it, and the node is no deeper than the bound. Read under a trusted
 * scope: a loop is a loop whoever is saving, and a check that a caller's
 * access could blind would let one through.
 */
static gboolean
venture_category_check_node(
	VentureDatabase	 *database,
	VentureEntity	 *entity,
	VentureEntity	 *previous,
	GError		**error
){
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(VentureEntity) parent = NULL;
	g_autoptr(GHashTable) seen = NULL;
	const gchar *type_name;
	GType entity_type;
	gint64 own_id;
	gint64 parent_id;
	gint64 cursor;
	guint levels;

	/* Every rule here is about the row as it will be, not as it was. */
	(void)previous;

	parent_id = venture_category_parent_of(entity);

	if (0 == parent_id)
		return TRUE;

	entity_type = G_OBJECT_TYPE(entity);
	type_name = venture_entity_get_entity_name(entity);
	own_id = venture_entity_get_id(entity);

	if ((0 != own_id) && (parent_id == own_id))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		            "A %s cannot be its own parent", type_name);
		return FALSE;
	}

	internal = venture_access_policy_enter(
		venture_database_get_access_policy(database), NULL);

	parent = venture_database_get(database, entity_type, parent_id, NULL);

	/*
	 * The reference check has already refused a parent written now that
	 * is missing or deleted. A parent the node kept is left alone -- the
	 * same rule every reference follows -- so a node under a since-deleted
	 * parent can still be renamed.
	 */
	if (NULL == parent)
		return TRUE;

	if (venture_entity_get_organization_id(parent) !=
	    venture_entity_get_organization_id(entity))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		            "%s #%" G_GINT64_FORMAT " belongs to another organization "
		            "and cannot be this %s's parent",
		            type_name, parent_id, type_name);
		return FALSE;
	}

	/*
	 * Walk up from the parent. Reaching this node means the parent is
	 * somewhere beneath it, and saving would close a loop that every
	 * path, subtree and report would then walk forever -- or, bounded,
	 * silently get wrong.
	 */
	seen = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	levels = 2;
	cursor = venture_category_parent_of(parent);
	g_hash_table_add(seen, g_memdup2(&parent_id, sizeof(parent_id)));

	while (0 != cursor)
	{
		g_autoptr(VentureEntity) above = NULL;

		if ((0 != own_id) && (cursor == own_id))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			            "%s #%" G_GINT64_FORMAT " is beneath this %s; "
			            "making it the parent would close a loop",
			            type_name, parent_id, type_name);
			return FALSE;
		}

		/* An existing loop above that does not pass through this node is
		 * the table's problem, not this save's: stop rather than spin. */
		if (g_hash_table_contains(seen, &cursor))
			break;

		levels++;

		if (levels > VENTURE_CATEGORY_MAX_DEPTH)
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			            "A %s tree may be at most %d levels deep",
			            type_name, VENTURE_CATEGORY_MAX_DEPTH);
			return FALSE;
		}

		g_hash_table_add(seen, g_memdup2(&cursor, sizeof(cursor)));
		above = venture_database_get(database, entity_type, cursor, NULL);

		if (NULL == above)
			break;

		cursor = venture_category_parent_of(above);
	}

	return TRUE;
}

/* A location is a node and nothing more. */
static gboolean
venture_category_validate_location(
	VentureDatabase	 *database,
	VentureEntity	 *entity,
	VentureEntity	 *previous,
	gpointer	  user_data,
	GError		**error
){
	(void)user_data;

	return venture_category_check_node(database, entity, previous, error);
}

/*
 * A category is a node, plus the record type its tree groups. applies-to
 * is checked against every type ever registered -- a module switched off
 * does not stop its tree being edited -- and a parent must group the same
 * type, or one tree would file products under an expense head.
 */
static gboolean
venture_category_validate_category(
	VentureDatabase	 *database,
	VentureEntity	 *entity,
	VentureEntity	 *previous,
	gpointer	  user_data,
	GError		**error
){
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autofree gchar *applies_to = NULL;
	gint64 parent_id;

	(void)user_data;

	g_object_get(entity, "applies-to", &applies_to, NULL);

	/* Stored as the registry spells it, so "Product " and "product" are
	 * not two trees. */
	if (NULL != applies_to)
	{
		gchar *lowered;

		g_strstrip(applies_to);
		lowered = g_ascii_strdown(applies_to, -1);
		g_free(applies_to);
		applies_to = lowered;
		g_object_set(entity, "applies-to", applies_to, NULL);
	}

	if (('\0' != venture_category_or_empty(applies_to)[0]) &&
	    (G_TYPE_INVALID == venture_entity_registry_lookup_any(
	    	venture_entity_registry_get_default(), applies_to)))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		            "category.applies_to: \"%s\" is not a record type", applies_to);
		return FALSE;
	}

	if (!venture_category_check_node(database, entity, previous, error))
		return FALSE;

	internal = venture_access_policy_enter(
		venture_database_get_access_policy(database), NULL);

	parent_id = venture_category_parent_of(entity);

	if (0 != parent_id)
	{
		g_autoptr(VentureEntity) parent = NULL;
		g_autofree gchar *parent_applies = NULL;

		parent = venture_database_get(database, VENTURE_TYPE_CATEGORY, parent_id, NULL);

		if (NULL != parent)
		{
			g_object_get(parent, "applies-to", &parent_applies, NULL);

			if (0 != g_strcmp0(venture_category_or_empty(parent_applies),
			                   venture_category_or_empty(applies_to)))
			{
				g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
				            "category #%" G_GINT64_FORMAT " groups %s, not %s; "
				            "a category's parent must group the same records",
				            parent_id,
				            ('\0' != venture_category_or_empty(parent_applies)[0])
				            	? parent_applies : "any record type",
				            ('\0' != venture_category_or_empty(applies_to)[0])
				            	? applies_to : "any record type");
				return FALSE;
			}
		}
	}

	/*
	 * Changing what a tree groups, from under its sub-categories, would
	 * leave each of them disagreeing with its parent -- the state the
	 * check above exists to prevent. The children move first.
	 */
	if (NULL != previous)
	{
		g_autofree gchar *old_applies = NULL;

		g_object_get(previous, "applies-to", &old_applies, NULL);

		if (0 != g_strcmp0(venture_category_or_empty(old_applies),
		                   venture_category_or_empty(applies_to)))
		{
			g_autoptr(VentureQuery) query = NULL;
			g_autoptr(GPtrArray) children = NULL;

			query = venture_query_new(VENTURE_TYPE_CATEGORY);
			venture_query_set_limit(query, 1);
			venture_query_add_filter_int(query, "parent-id", VENTURE_FILTER_OP_EQ,
			                             venture_entity_get_id(entity), NULL);
			children = venture_database_find(database, query, error);

			if (NULL == children)
				return FALSE;

			if (children->len > 0)
			{
				g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
				            "category #%" G_GINT64_FORMAT " has sub-categories; "
				            "change what they group first",
				            venture_entity_get_id(entity));
				return FALSE;
			}
		}
	}

	return TRUE;
}

gboolean
venture_category_check_applies_to(
	VentureDatabase	 *database,
	gint64		  category_id,
	const gchar	 *entity_name,
	GError		**error
){
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(VentureEntity) category = NULL;
	g_autofree gchar *applies_to = NULL;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), FALSE);
	g_return_val_if_fail(NULL != entity_name, FALSE);

	internal = venture_access_policy_enter(
		venture_database_get_access_policy(database), NULL);

	category = venture_database_get(database, VENTURE_TYPE_CATEGORY, category_id, NULL);

	if (NULL == category)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		            "category #%" G_GINT64_FORMAT " does not exist", category_id);
		return FALSE;
	}

	g_object_get(category, "applies-to", &applies_to, NULL);

	if (('\0' == venture_category_or_empty(applies_to)[0]) ||
	    (0 == g_strcmp0(applies_to, entity_name)))
		return TRUE;

	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
	            "category #%" G_GINT64_FORMAT " groups %s records, not %s",
	            category_id, applies_to, entity_name);
	return FALSE;
}

/*
 * Every record's references to a category, found from the field table --
 * product.category-id today, a plugin type's field the day it registers --
 * held to the tree's applies-to. Only a reference being written is
 * checked, the rule every reference follows: a product under a category
 * whose tree later changed type can still be edited.
 */
static gboolean
venture_category_validate_references(
	VentureDatabase	 *database,
	VentureEntity	 *entity,
	VentureEntity	 *previous,
	gpointer	  user_data,
	GError		**error
){
	VentureEntityClass *klass;
	g_autofree GParamSpec **properties = NULL;
	guint n_properties;
	guint i;

	(void)user_data;

	/* A category's own parent is held to equality above, not to this. */
	if (VENTURE_IS_CATEGORY(entity))
		return TRUE;

	klass = VENTURE_ENTITY_GET_CLASS(entity);
	properties = venture_entity_class_list_persistent_properties(klass, &n_properties);

	for (i = 0; i < n_properties; i++)
	{
		GParamSpec *pspec;
		gint64 target_id;

		pspec = properties[i];

		if ((0 != g_strcmp0(venture_entity_class_get_reference(klass, pspec->name),
		                    "category")) ||
		    (G_TYPE_INT64 != G_PARAM_SPEC_VALUE_TYPE(pspec)))
			continue;

		target_id = 0;
		g_object_get(entity, pspec->name, &target_id, NULL);

		if (0 == target_id)
			continue;

		if (NULL != previous)
		{
			gint64 previous_id;

			previous_id = 0;
			g_object_get(previous, pspec->name, &previous_id, NULL);

			if (previous_id == target_id)
				continue;
		}

		if (!venture_category_check_applies_to(database, target_id,
		                                       venture_entity_get_entity_name(entity),
		                                       error))
			return FALSE;
	}

	return TRUE;
}

void
venture_category_install(VentureContext *context)
{
	VentureDatabase *database;

	g_return_if_fail(VENTURE_IS_CONTEXT(context));

	database = venture_context_get_database(context);

	/* The tests build several contexts over one database; validators are
	 * per database, so the second one must add nothing. */
	if (NULL != g_object_get_data(G_OBJECT(database), VENTURE_CATEGORY_STATE_KEY))
		return;

	g_object_set_data(G_OBJECT(database), VENTURE_CATEGORY_STATE_KEY,
	                  GINT_TO_POINTER(1));

	venture_database_add_save_validator(database, VENTURE_TYPE_CATEGORY,
	                                    venture_category_validate_category, NULL, NULL);
	venture_database_add_save_validator(database, VENTURE_TYPE_LOCATION,
	                                    venture_category_validate_location, NULL, NULL);
	venture_database_add_save_validator(database, VENTURE_TYPE_ENTITY,
	                                    venture_category_validate_references, NULL, NULL);
}
