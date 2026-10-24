/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Tag operations over the one tags field (src/core/venture-tags.h). The
 * stored field is free text people type, so reading is forgiving -- any
 * spacing, any capitals -- and only the tag actions rewrite it, as "a, b". */
#include "venture.h"
#include <string.h>

static gchar *
tag_key(const gchar *tag)
{
	g_autofree gchar *copy = g_strdup(tag != NULL ? tag : "");
	g_strstrip(copy);
	return g_utf8_casefold(copy, -1);
}

gchar **
venture_tags_parse(const gchar *tags)
{
	g_autoptr(GPtrArray) result = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GHashTable) seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	g_auto(GStrv) parts = g_strsplit(tags != NULL ? tags : "", ",", -1);
	guint i;
	for (i = 0; parts[i] != NULL; i++)
	{
		gchar *key;
		g_strstrip(parts[i]);
		if (*parts[i] == '\0')
			continue;
		key = tag_key(parts[i]);
		if (g_hash_table_contains(seen, key))
		{
			g_free(key);
			continue;
		}
		g_hash_table_add(seen, key);
		g_ptr_array_add(result, g_strdup(parts[i]));
	}
	g_ptr_array_set_free_func(result, NULL);
	g_ptr_array_add(result, NULL);
	return (gchar **)g_ptr_array_free(g_steal_pointer(&result), FALSE);
}

gboolean
venture_tags_has(const gchar *tags, const gchar *tag)
{
	g_auto(GStrv) parsed = venture_tags_parse(tags);
	g_autofree gchar *wanted = tag_key(tag);
	guint i;
	if (*wanted == '\0')
		return FALSE;
	for (i = 0; parsed[i] != NULL; i++)
	{
		g_autofree gchar *key = tag_key(parsed[i]);
		if (g_strcmp0(key, wanted) == 0)
			return TRUE;
	}
	return FALSE;
}

gboolean
venture_tags_valid(const gchar *tag)
{
	g_autofree gchar *copy = g_strdup(tag != NULL ? tag : "");
	g_strstrip(copy);
	return *copy != '\0' && strlen(copy) <= VENTURE_TAG_MAX_LENGTH && strpbrk(copy, ",\r\n") == NULL &&
		g_utf8_validate(copy, -1, NULL);
}

static gchar *
tags_join(GPtrArray *tags)
{
	g_ptr_array_add(tags, NULL);
	return g_strjoinv(", ", (gchar **)tags->pdata);
}

gchar *
venture_tags_add(const gchar *tags, const gchar *tag)
{
	g_auto(GStrv) parsed = venture_tags_parse(tags);
	g_autoptr(GPtrArray) result = g_ptr_array_new();
	g_autofree gchar *trimmed = g_strstrip(g_strdup(tag != NULL ? tag : ""));
	guint i;
	for (i = 0; parsed[i] != NULL; i++)
		g_ptr_array_add(result, parsed[i]);
	if (*trimmed != '\0' && !venture_tags_has(tags, trimmed))
		g_ptr_array_add(result, trimmed);
	return tags_join(result);
}

gchar *
venture_tags_remove(const gchar *tags, const gchar *tag)
{
	g_auto(GStrv) parsed = venture_tags_parse(tags);
	g_autoptr(GPtrArray) result = g_ptr_array_new();
	g_autofree gchar *unwanted = tag_key(tag);
	guint i;
	for (i = 0; parsed[i] != NULL; i++)
	{
		g_autofree gchar *key = tag_key(parsed[i]);
		if (g_strcmp0(key, unwanted) != 0)
			g_ptr_array_add(result, parsed[i]);
	}
	return tags_join(result);
}

gchar **
venture_tags_added(const gchar *before, const gchar *after)
{
	g_auto(GStrv) parsed = venture_tags_parse(after);
	g_autoptr(GPtrArray) result = g_ptr_array_new();
	guint i;
	for (i = 0; parsed[i] != NULL; i++)
		if (!venture_tags_has(before, parsed[i]))
			g_ptr_array_add(result, g_strdup(parsed[i]));
	g_ptr_array_add(result, NULL);
	return (gchar **)g_ptr_array_free(g_steal_pointer(&result), FALSE);
}

/* --- The actions --------------------------------------------------------- */

static gboolean
tag_allowed(VentureAction *action, VentureEntity *entity, const VentureActor *actor, GError **error)
{
	(void)action; (void)entity; (void)actor; (void)error;
	return TRUE;
}

/* An ordinary save of the tags field, so the audit, the tag-added
 * triggers and every validator see exactly what a form edit would. */
static VentureEntity *
tag_invoke(VentureAction *action, VentureEntity *entity, GHashTable *params,
	const VentureActor *actor, GError **error)
{
	VentureDatabase *database = venture_action_get_data(action);
	JsonNode *node = g_hash_table_lookup(params, "tag");
	const gchar *tag = node != NULL && JSON_NODE_HOLDS_VALUE(node) ? json_node_get_string(node) : NULL;
	g_autofree gchar *name = NULL, *tags = NULL, *changed = NULL;
	g_autoptr(VentureEntity) fresh = NULL;
	g_object_get(action, "name", &name, NULL);
	if (!venture_tags_valid(tag))
	{
		venture_set_error_validation(error, "tag", "A tag is up to %d characters with no comma or line break",
			VENTURE_TAG_MAX_LENGTH);
		return NULL;
	}
	fresh = venture_database_get(database, G_OBJECT_TYPE(entity), venture_entity_get_id(entity), error);
	if (fresh == NULL)
		return NULL;
	g_object_get(fresh, "tags", &tags, NULL);
	changed = g_strcmp0(name, "add_tag") == 0 ? venture_tags_add(tags, tag) : venture_tags_remove(tags, tag);
	g_object_set(fresh, "tags", changed, NULL);
	if (!venture_database_save(database, fresh, actor, error))
		return NULL;
	return g_steal_pointer(&fresh);
}

void
venture_tags_actions_register(VentureDatabase *database)
{
	static const struct { const gchar *type; const gchar *name; const gchar *label; const gchar *description; } actions[] = {
		{ "contact", "add_tag", "Add tag", "Add one tag; a tag already there, in any capitals, is left alone" },
		{ "contact", "remove_tag", "Remove tag", "Remove one tag, in any capitals" },
		{ "company", "add_tag", "Add tag", "Add one tag; a tag already there, in any capitals, is left alone" },
		{ "company", "remove_tag", "Remove tag", "Remove one tag, in any capitals" }
	};
	guint i;
	g_return_if_fail(VENTURE_IS_DATABASE(database));
	for (i = 0; i < G_N_ELEMENTS(actions); i++)
	{
		g_autoptr(GPtrArray) parameters = g_ptr_array_new_with_free_func((GDestroyNotify)venture_field_spec_free);
		g_autoptr(VentureAction) action = NULL;
		g_autoptr(GError) error = NULL;
		VentureFieldSpec *field = venture_field_spec_new("tag", "Tag", VENTURE_FIELD_KIND_STRING);
		field->required = TRUE;
		field->max_length = 128;
		g_ptr_array_add(parameters, field);
		action = g_object_new(VENTURE_TYPE_ACTION, "data-class", VENTURE_DATA_CLASS_TENANT,
			"type-name", actions[i].type, "name", actions[i].name, "label", actions[i].label,
			"description", actions[i].description, "parameters", parameters, "stageable", FALSE,
			"service-transaction", FALSE, "roles", VENTURE_USER_ROLE_EDITOR, NULL);
		if (!venture_action_registry_register(venture_database_get_action_registry(database), action,
			tag_allowed, tag_invoke, database, NULL, &error))
			g_error("Tag action registration: %s", error->message);
	}
}
