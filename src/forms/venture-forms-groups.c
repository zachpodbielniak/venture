/*
 * venture-forms-groups.c - Bounded repeating rows in frozen questionnaires
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "venture-forms-private.h"
#include <string.h>

/* The bracket syntax is reserved for generated row names, never authored keys. */
gboolean
venture_forms_key_valid(const gchar *key)
{
	const gchar *cursor;
	if (venture_string_is_empty(key) || strlen(key) > VENTURE_FORMS_KEY_MAX || !g_ascii_islower(key[0]))
		return FALSE;
	for (cursor = key; *cursor != '\0'; cursor++)
		if (!g_ascii_islower(*cursor) && !g_ascii_isdigit(*cursor) && *cursor != '_') return FALSE;
	return TRUE;
}

gboolean
venture_forms_validate_group(VentureDatabase *database, VentureEntity *entity,
	VentureEntity *previous, gpointer data, GError **error)
{
	g_autoptr(VentureEntity) form = NULL;
	g_autofree gchar *key = venture_forms_get_string(entity, "key");
	gint64 form_id = venture_forms_get_int(entity, "form-id");
	gint64 minimum = venture_forms_get_int(entity, "min-rows");
	gint64 maximum = venture_forms_get_int(entity, "max-rows");
	(void)data;
	if (maximum == 0) maximum = 10;
	if (!venture_forms_key_valid(key) || minimum < 0 || maximum < 1 || maximum > 50 || minimum > maximum)
	{
		venture_set_error_validation(error, "Repeat group", "use a stable lowercase key and row bounds from 0 to 50 (maximum 0 means 10)");
		return FALSE;
	}
	if (previous != NULL)
	{
		g_autofree gchar *before = venture_forms_get_string(previous, "key");
		if (g_strcmp0(before, key) != 0 || venture_forms_get_int(previous, "form-id") != form_id)
		{
			venture_set_error_validation(error, "Repeat group", "its key and form cannot change; existing answers use that identity");
			return FALSE;
		}
	}
	form = venture_database_get(database, VENTURE_TYPE_FORM, form_id, NULL);
	if (form == NULL || venture_entity_get_organization_id(form) != venture_entity_get_organization_id(entity))
	{
		venture_set_error_validation(error, "Repeat group", "the form must exist in this organization");
		return FALSE;
	}
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_FORM_FIELD);
		g_autoptr(GPtrArray) fields = NULL;
		venture_query_set_organization(query, venture_entity_get_organization_id(entity));
		venture_query_add_filter_int(query, "form-id", VENTURE_FILTER_OP_EQ, form_id, NULL);
		venture_query_add_filter_string(query, "key", VENTURE_FILTER_OP_EQ, key, NULL);
		venture_query_set_include_deleted(query, TRUE);
		venture_query_set_limit(query, 1);
		fields = venture_database_find(database, query, error);
		if (fields == NULL) return FALSE;
		if (fields->len > 0)
		{
			venture_set_error_validation(error, "Repeat group", "its key must not also name a question");
			return FALSE;
		}
	}
	return TRUE;
}

gboolean
venture_forms_groups_check(GPtrArray *fields, GError **error)
{
	g_autoptr(GHashTable) seen = g_hash_table_new(g_str_hash, g_str_equal);
	guint i = 0, parameters = 8;
	while (i < fields->len)
	{
		VentureFormsField *field = g_ptr_array_index(fields, i);
		guint j;
		if (venture_string_is_empty(field->group_key)) { parameters++; i++; continue; }
		if (g_hash_table_contains(seen, field->group_key) || venture_forms_definition_find(fields, field->group_key) != NULL) goto invalid;
		g_hash_table_add(seen, field->group_key);
		for (j = i; j < fields->len; j++)
		{
			VentureFormsField *member = g_ptr_array_index(fields, j);
			if (g_strcmp0(member->group_key, field->group_key) != 0) break;
			if (member->group_min != field->group_min || member->group_max != field->group_max ||
			    g_strcmp0(member->group_label, field->group_label) != 0 ||
			    member->kind == VENTURE_FORM_FIELD_PAGE_BREAK || !venture_string_is_empty(member->maps_to) || member->marketing_consent)
				goto invalid;
		}
		parameters += (j - i + 1) * field->group_max;
		i = j;
	}
	if (g_hash_table_size(seen) > 0 && parameters > 500)
	{
		venture_set_error_validation(error, "Repeat groups", "reduce row maxima or questions so a full response uses at most 500 input names");
		return FALSE;
	}
	return TRUE;
invalid:
	venture_set_error_validation(error, "Repeat group", "keep its questions together on one page, without page breaks, lead mappings or marketing permission");
	return FALSE;
}

static gboolean
forms_groups_load(VentureDatabase *database, VentureEntity *form, GPtrArray *fields, gboolean check_layout, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_FORM_GROUP);
	g_autoptr(GPtrArray) groups = NULL;
	g_autoptr(GHashTable) by_id = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	g_autofree gchar *confirmation = venture_forms_get_string(form, "confirmation-field");
	g_autofree gchar *unique_email = venture_forms_get_string(form, "unique-email-field");
	guint i;
	venture_query_set_organization(query, venture_entity_get_organization_id(form));
	venture_query_add_filter_int(query, "form-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(form), NULL);
	venture_query_set_limit(query, 501);
	groups = venture_database_find(database, query, error);
	if (groups == NULL) return FALSE;
	if (groups->len > 500)
	{
		venture_set_error_validation(error, "Repeat groups", "at most 500 groups per form");
		return FALSE;
	}
	for (i = 0; i < groups->len; i++)
	{
		VentureEntity *group = g_ptr_array_index(groups, i);
		gint64 *id = g_new(gint64, 1);
		*id = venture_entity_get_id(group);
		g_hash_table_insert(by_id, id, group);
	}
	for (i = 0; i < fields->len; i++)
	{
		VentureFormsField *field = g_ptr_array_index(fields, i);
		VentureEntity *group;
		if (field->group_id == 0) continue;
		group = g_hash_table_lookup(by_id, &field->group_id);
		if (group == NULL)
		{
			venture_set_error_validation(error, "Repeat group", "a question names a missing group on this form");
			return FALSE;
		}
		field->group_key = venture_forms_get_string(group, "key");
		field->group_label = venture_forms_get_string(group, "label");
		field->group_min = (guint)venture_forms_get_int(group, "min-rows");
		field->group_max = (guint)venture_forms_get_int(group, "max-rows");
		if (field->group_max == 0) field->group_max = 10;
		if (g_strcmp0(field->key, confirmation) == 0 || g_strcmp0(field->key, unique_email) == 0)
		{
			venture_set_error_validation(error, "Repeat group", "confirmation and per-email limits need a non-repeated email question");
			return FALSE;
		}
	}
	return !check_layout || venture_forms_groups_check(fields, error);
}

gboolean
venture_forms_groups_load(VentureDatabase *database, VentureEntity *form, GPtrArray *fields, GError **error)
{
	return forms_groups_load(database, form, fields, TRUE, error);
}

gboolean
venture_forms_groups_load_metadata(VentureDatabase *database, VentureEntity *form, GPtrArray *fields, GError **error)
{
	return forms_groups_load(database, form, fields, FALSE, error);
}

static void
group_error(JsonObject *errors, const gchar *key, const gchar *message)
{
	if (errors != NULL) json_object_set_string_member(errors, key, message);
}

static guint
group_count(const VentureFormsField *field, JsonObject *values, gboolean rendering, JsonObject *errors)
{
	g_autofree gchar *prefix = g_strdup_printf("%s[", field->group_key);
	JsonObjectIter iter;
	const gchar *name;
	guint count = 0;
	guint64 present = 0;
	if (values != NULL)
	{
		json_object_iter_init(&iter, values);
		while (json_object_iter_next(&iter, &name, NULL))
		{
			const gchar *start;
			gchar *end = NULL;
			guint64 row;
			if (!g_str_has_prefix(name, prefix)) continue;
			start = name + strlen(prefix);
			row = g_ascii_strtoull(start, &end, 10);
			if (!g_ascii_isdigit(*start) || end == start || *end != ']' || end[1] != '[' || row >= 50)
			{
				group_error(errors, field->group_key, "Use consecutive row numbers within the group limit.");
				continue;
			}
			present |= G_GUINT64_CONSTANT(1) << row;
			count = MAX(count, (guint)row + 1);
		}
	}
	if (count > 0 && present != ((G_GUINT64_CONSTANT(1) << count) - 1))
		group_error(errors, field->group_key, "Rows must be consecutive, starting at zero.");
	if (count > field->group_max)
		group_error(errors, field->group_key, "Too many rows for this group.");
	count = MIN(count, field->group_max);
	return rendering ? MAX(count, field->group_min) : count;
}

static VentureFormsField *
group_field_copy(const VentureFormsField *field)
{
	VentureFormsField *copy = g_new0(VentureFormsField, 1);
	*copy = *field;
#define COPY_STRING(member) copy->member = g_strdup(field->member)
	COPY_STRING(key); COPY_STRING(label); COPY_STRING(help); COPY_STRING(placeholder);
	COPY_STRING(pattern); COPY_STRING(default_value); COPY_STRING(maps_to); COPY_STRING(autocomplete);
	COPY_STRING(booking_name_field); COPY_STRING(booking_email_field);
	copy->booking_slots = field->booking_slots != NULL ? json_node_ref(field->booking_slots) : NULL;
	COPY_STRING(scoring); COPY_STRING(contact_field); COPY_STRING(group_key); COPY_STRING(group_label); COPY_STRING(base_key);
#undef COPY_STRING
	copy->choices = field->choices != NULL ? g_ptr_array_ref(field->choices) : NULL;
	copy->rules = field->rules != NULL ? json_array_ref(field->rules) : NULL;
	copy->quiz = field->quiz != NULL ? json_object_ref(field->quiz) : NULL;
	copy->payment = field->payment != NULL ? json_object_ref(field->payment) : NULL;
	copy->catalog = field->catalog != NULL ? json_object_ref(field->catalog) : NULL;
	copy->language = g_strdup(field->language);
	return copy;
}

static JsonArray *
group_row_rules(GPtrArray *fields, const VentureFormsField *group, guint row)
{
	JsonArray *rules = json_array_new();
	guint i;
	for (i = 0; group->rules != NULL && i < json_array_get_length(group->rules); i++)
	{
		JsonNode *node = json_array_get_element(group->rules, i);
		JsonObject *rule = json_node_get_object(node);
		const gchar *target = json_object_get_string_member(rule, "target");
		const VentureFormsField *target_field = venture_forms_definition_find(fields, target);
		g_autofree gchar *text = NULL, *indexed = NULL;
		JsonNode *copy;
		JsonArray *conditions;
		guint j;
		if (target_field == NULL || g_strcmp0(target_field->group_key, group->group_key) != 0) continue;
		text = json_to_string(node, FALSE);
		copy = json_from_string(text, NULL);
		rule = json_node_get_object(copy);
		indexed = g_strdup_printf("%s[%u][%s]", group->group_key, row, target);
		json_object_set_string_member(rule, "target", indexed);
		conditions = json_object_get_array_member(rule, "conditions");
		for (j = 0; j < json_array_get_length(conditions); j++)
		{
			JsonObject *condition = json_array_get_object_element(conditions, j);
			const gchar *key = json_object_get_string_member(condition, "field");
			const VentureFormsField *source = venture_forms_definition_find(fields, key);
			if (source != NULL && g_strcmp0(source->group_key, group->group_key) == 0)
			{
				g_autofree gchar *name = g_strdup_printf("%s[%u][%s]", group->group_key, row, key);
				json_object_set_string_member(condition, "field", name);
			}
		}
		json_array_add_element(rules, copy);
	}
	return rules;
}

static VentureFormsField *
group_marker(const VentureFormsField *field, guint boundary, guint row, guint count)
{
	VentureFormsField *marker = group_field_copy(field);
	marker->kind = VENTURE_FORM_FIELD_HIDDEN;
	marker->required = FALSE;
	marker->sensitive = FALSE;
	marker->group_boundary = boundary;
	g_free(marker->label); marker->label = g_strdup(field->group_label);
	marker->row_index = row;
	marker->row_count = count;
	g_free(marker->key);
	if (boundary == VENTURE_FORMS_GROUP_ROW_START)
		marker->key = g_strdup_printf("%s[%u][_row]", field->group_key, row);
	else if (boundary == VENTURE_FORMS_GROUP_ROW_END)
		marker->key = g_strdup_printf("%s[%u][_end]", field->group_key, row);
	else marker->key = g_strdup(field->group_key);
	return marker;
}

GPtrArray *
venture_forms_expand_groups(GPtrArray *fields, JsonObject *values, gboolean rendering, JsonObject *errors)
{
	GPtrArray *expanded = g_ptr_array_new_with_free_func(venture_forms_field_free);
	guint i = 0;
	while (i < fields->len)
	{
		VentureFormsField *field = g_ptr_array_index(fields, i);
		guint end, row, count;
		if (venture_string_is_empty(field->group_key))
		{
			g_ptr_array_add(expanded, group_field_copy(field)); i++; continue;
		}
		count = group_count(field, values, rendering, errors);
		for (end = i + 1; end < fields->len; end++)
			if (g_strcmp0(((VentureFormsField *)g_ptr_array_index(fields, end))->group_key, field->group_key) != 0) break;
		g_ptr_array_add(expanded, group_marker(field, VENTURE_FORMS_GROUP_START, 0, count));
		for (row = 0; row < count; row++)
		{
			g_autoptr(JsonArray) rules = group_row_rules(fields, field, row);
			guint j;
			g_ptr_array_add(expanded, group_marker(field, VENTURE_FORMS_GROUP_ROW_START, row, count));
			for (j = i; j < end; j++)
			{
				VentureFormsField *source = g_ptr_array_index(fields, j);
				VentureFormsField *copy = group_field_copy(source);
				g_free(copy->base_key); copy->base_key = g_strdup(source->key);
				g_free(copy->key); copy->key = g_strdup_printf("%s[%u][%s]", source->group_key, row, source->key);
				copy->row_index = row; copy->row_count = count;
				g_clear_pointer(&copy->rules, json_array_unref); copy->rules = json_array_ref(rules);
				g_ptr_array_add(expanded, copy);
			}
			g_ptr_array_add(expanded, group_marker(field, VENTURE_FORMS_GROUP_ROW_END, row, count));
		}
		g_ptr_array_add(expanded, group_marker(field, VENTURE_FORMS_GROUP_END, 0, count));
		i = end;
	}
	return expanded;
}

void
venture_forms_groups_validate(GPtrArray *fields, GHashTable *not_shown, JsonObject *errors)
{
	guint i;
	for (i = 0; i < fields->len; i++)
	{
		VentureFormsField *field = g_ptr_array_index(fields, i);
		if (field->group_boundary == VENTURE_FORMS_GROUP_START &&
		    (not_shown == NULL || !g_hash_table_contains(not_shown, field->key)) && field->row_count < field->group_min)
			group_error(errors, field->group_key, "Add the minimum number of rows for this group.");
	}
}

static void
group_clear_values(JsonObject *values, const gchar *group)
{
	g_autofree gchar *prefix = g_strdup_printf("%s[", group);
	GList *names = json_object_get_members(values), *item;
	for (item = names; item != NULL; item = item->next)
		if (g_str_has_prefix(item->data, prefix)) json_object_remove_member(values, item->data);
	g_list_free(names);
}

JsonObject *
venture_forms_groups_overlay(GPtrArray *fields, guint page, JsonObject *previous, JsonObject *posted)
{
	JsonObject *values = json_object_new();
	g_autoptr(GPtrArray) selected = venture_forms_page_fields(fields, page);
	JsonObjectIter iter;
	const gchar *name;
	JsonNode *node;
	guint i;
	json_object_iter_init(&iter, previous);
	while (json_object_iter_next(&iter, &name, &node)) json_object_set_member(values, name, json_node_copy(node));
	for (i = 0; i < selected->len; i++)
	{
		VentureFormsField *field = g_ptr_array_index(selected, i);
		if (!venture_string_is_empty(field->group_key)) group_clear_values(values, field->group_key);
		else json_object_remove_member(values, field->key);
	}
	json_object_iter_init(&iter, posted);
	while (json_object_iter_next(&iter, &name, &node)) json_object_set_member(values, name, json_node_copy(node));
	return values;
}

void
venture_forms_groups_clear_page(GPtrArray *fields, guint page, JsonObject *values)
{
	g_autoptr(GPtrArray) selected = venture_forms_page_fields(fields, page);
	guint i;
	for (i = 0; i < selected->len; i++)
	{
		VentureFormsField *field = g_ptr_array_index(selected, i);
		if (!venture_string_is_empty(field->group_key)) group_clear_values(values, field->group_key);
	}
}

gboolean
venture_forms_group_move(GPtrArray *fields, const gchar *move, JsonObject *values, JsonObject *errors)
{
	g_auto(GStrv) parts = NULL;
	VentureFormsField *group = NULL;
	guint i;
	gboolean add;
	if (venture_string_is_empty(move) || g_strcmp0(move, "next") == 0 || g_strcmp0(move, "back") == 0) return FALSE;
	parts = g_strsplit(move, ":", 4);
	add = g_strcmp0(parts[0], "add") == 0;
	if ((!add && g_strcmp0(parts[0], "remove") != 0) || g_strv_length(parts) != (add ? 2u : 3u)) goto invalid;
	for (i = 0; i < fields->len; i++)
	{
		VentureFormsField *field = g_ptr_array_index(fields, i);
		if (field->group_boundary == VENTURE_FORMS_GROUP_START && g_strcmp0(field->group_key, parts[1]) == 0)
		{ group = field; break; }
	}
	if (group == NULL) goto invalid;
	if (json_object_get_size(errors) > 0) return TRUE;
	if (add)
	{
		g_autofree gchar *sentinel = NULL;
		if (group->row_count >= group->group_max)
		{
			group_error(errors, group->group_key, "The maximum number of rows has been reached.");
			return TRUE;
		}
		sentinel = g_strdup_printf("%s[%u][_row]", group->group_key, group->row_count);
		json_object_set_string_member(values, sentinel, "1");
	}
	else
	{
		gchar *end = NULL;
		guint64 row = g_ascii_strtoull(parts[2], &end, 10);
		g_autofree gchar *prefix = g_strdup_printf("%s[", group->group_key);
		g_autoptr(JsonObject) shifted = json_object_new();
		JsonObjectIter iter;
		const gchar *name;
		JsonNode *node;
		if (!g_ascii_isdigit(parts[2][0]) || end == parts[2] || *end != '\0' || row >= group->row_count) goto invalid;
		if (group->row_count <= group->group_min)
		{
			group_error(errors, group->group_key, "Keep the minimum number of rows for this group.");
			return TRUE;
		}
		json_object_iter_init(&iter, values);
		while (json_object_iter_next(&iter, &name, &node))
		{
			guint64 index;
			gchar *tail = NULL;
			g_autofree gchar *renamed = NULL;
			if (!g_str_has_prefix(name, prefix)) continue;
			index = g_ascii_strtoull(name + strlen(prefix), &tail, 10);
			if (index == row) continue;
			renamed = g_strdup_printf("%s[%u%s", group->group_key, (guint)(index > row ? index - 1 : index), tail);
			json_object_set_member(shifted, renamed, json_node_copy(node));
		}
		group_clear_values(values, group->group_key);
		json_object_iter_init(&iter, shifted);
		while (json_object_iter_next(&iter, &name, &node)) json_object_set_member(values, name, json_node_copy(node));
	}
	return TRUE;
invalid:
	group_error(errors, "_form", "This row operation does not belong to the current page.");
	return TRUE;
}

void
venture_forms_groups_fold(GPtrArray *fields, JsonObject *answers, GHashTable *not_shown, gboolean secret)
{
	guint i;
	for (i = 0; i < fields->len; i++)
	{
		VentureFormsField *field = g_ptr_array_index(fields, i);
		JsonArray *rows;
		guint row;
		if (field->group_boundary != VENTURE_FORMS_GROUP_START ||
		    (not_shown != NULL && g_hash_table_contains(not_shown, field->key))) continue;
		if (secret)
		{
			g_autofree gchar *prefix = g_strdup_printf("%s[", field->group_key);
			JsonObjectIter iter;
			const gchar *name;
			gboolean any = FALSE;
			json_object_iter_init(&iter, answers);
			while (json_object_iter_next(&iter, &name, NULL))
				if (g_str_has_prefix(name, prefix)) any = TRUE;
			if (!any) continue;
		}
		rows = json_array_new();
		for (row = 0; row < field->row_count; row++) json_array_add_object_element(rows, json_object_new());
		json_object_set_array_member(answers, field->group_key, rows);
	}
	for (i = 0; i < fields->len; i++)
	{
		VentureFormsField *field = g_ptr_array_index(fields, i);
		JsonNode *value, *group;
		JsonArray *rows;
		if (field->group_boundary != VENTURE_FORMS_GROUP_NONE || venture_string_is_empty(field->group_key)) continue;
		value = json_object_get_member(answers, field->key);
		group = json_object_get_member(answers, field->group_key);
		if (value == NULL || group == NULL || !JSON_NODE_HOLDS_ARRAY(group)) continue;
		rows = json_node_get_array(group);
		if (field->row_index >= json_array_get_length(rows)) continue;
		json_object_set_member(json_array_get_object_element(rows, field->row_index), field->base_key, json_node_copy(value));
		json_object_remove_member(answers, field->key);
	}
}

gboolean
venture_forms_has_groups(GPtrArray *fields)
{
	guint i;
	for (i = 0; fields != NULL && i < fields->len; i++)
		if (!venture_string_is_empty(((VentureFormsField *)g_ptr_array_index(fields, i))->group_key)) return TRUE;
	return FALSE;
}

JsonArray *
venture_forms_group_render_rules(GPtrArray *fields)
{
	JsonArray *rules = json_array_new();
	guint i, j;
	for (i = 0; i < fields->len; i++)
	{
		VentureFormsField *field = g_ptr_array_index(fields, i);
		if (field->group_boundary != VENTURE_FORMS_GROUP_NONE) continue;
		for (j = 0; field->rules != NULL && j < json_array_get_length(field->rules); j++)
		{
			JsonNode *node = json_array_get_element(field->rules, j);
			JsonObject *rule = json_node_get_object(node);
			if (json_object_get_int_member(rule, "action") < VENTURE_FORM_RULE_JUMP &&
			    g_strcmp0(json_object_get_string_member(rule, "target"), field->key) == 0)
				json_array_add_element(rules, json_node_copy(node));
		}
	}
	return rules;
}

/* Normalize the two wire encodings before the same field validator sees them.
 * Reject collisions instead of letting member order choose an answer. */
JsonObject *
venture_forms_flatten_json_answers(JsonObject *object, GError **error)
{
	g_autoptr(JsonObject) flat = json_object_new();
	JsonObjectIter iter;
	const gchar *name;
	JsonNode *node;
	json_object_iter_init(&iter, object);
	while (json_object_iter_next(&iter, &name, &node))
	{
		JsonArray *rows = JSON_NODE_HOLDS_ARRAY(node) ? json_node_get_array(node) : NULL;
		guint i;
		if (rows == NULL || json_array_get_length(rows) == 0 ||
		    !JSON_NODE_HOLDS_OBJECT(json_array_get_element(rows, 0)))
		{
			if (json_object_has_member(flat, name)) goto malformed;
			json_object_set_member(flat, name, json_node_copy(node));
			if (json_object_get_size(flat) > 500) goto malformed;
			continue;
		}
		if (!venture_forms_key_valid(name) || json_array_get_length(rows) > 50) goto malformed;
		for (i = 0; i < json_array_get_length(rows); i++)
		{
			JsonNode *row = json_array_get_element(rows, i), *value;
			JsonObjectIter members;
			const gchar *key;
			g_autofree gchar *sentinel = g_strdup_printf("%s[%u][_row]", name, i);
			if (!JSON_NODE_HOLDS_OBJECT(row) || json_object_has_member(object, sentinel) ||
			    json_object_has_member(flat, sentinel)) goto malformed;
			json_object_set_string_member(flat, sentinel, "1");
			json_object_iter_init(&members, json_node_get_object(row));
			while (json_object_iter_next(&members, &key, &value))
			{
				g_autofree gchar *indexed = g_strdup_printf("%s[%u][%s]", name, i, key);
				if (!venture_forms_key_valid(key) || json_object_has_member(object, indexed) ||
				    json_object_has_member(flat, indexed) || JSON_NODE_HOLDS_OBJECT(value)) goto malformed;
				json_object_set_member(flat, indexed, json_node_copy(value));
				if (json_object_get_size(flat) > 500) goto malformed;
			}
			if (json_object_get_size(flat) > 500) goto malformed;
		}
	}
	return g_steal_pointer(&flat);
malformed:
	venture_set_error_validation(error, "answers", "use at most 50 rows, distinct question names and one level of row objects");
	return NULL;
}
