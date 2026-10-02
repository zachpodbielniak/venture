/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture-forms-private.h"
#include <string.h>

#define FORMS_PIPE_MAX 32768

static void
pipe_append(GString *out, const gchar *value)
{
	gsize length, room;
	if (value == NULL || out->len >= FORMS_PIPE_MAX) return;
	room = FORMS_PIPE_MAX - out->len;
	length = strlen(value);
	if (length > room)
	{
		length = room;
		while (length > 0 && (((guchar)value[length] & 0xc0) == 0x80)) length--;
	}
	g_string_append_len(out, value, length);
}

/* A closed grammar keeps stored text declarative. Braces can be escaped,
 * but there is no expression parser or evaluation context to smuggle code. */
static GPtrArray *
pipe_tokens(const gchar *text, GError **error)
{
	GPtrArray *tokens = g_ptr_array_new_with_free_func(g_free);
	const gchar *at = text != NULL ? text : "";
	while (*at != '\0')
	{
		const gchar *end;
		g_autofree gchar *key = NULL;
		gchar *suffix;
		guint i;
		if ((*at == '{' && at[1] == '{') || (*at == '}' && at[1] == '}')) { at += 2; continue; }
		if (*at == '}') goto invalid;
		if (*at != '{') { at++; continue; }
		end = strchr(at + 1, '}');
		if (end == NULL || end - at > VENTURE_FORMS_KEY_MAX + 7 || end == at + 1) goto invalid;
		key = g_strndup(at + 1, end - at - 1);
		suffix = g_str_has_suffix(key, ".count") ? key + strlen(key) - 6 : NULL;
		if (suffix != NULL) *suffix = '\0';
		if (key[0] < 'a' || key[0] > 'z') goto invalid;
		for (i = 1; key[i] != '\0'; i++)
			if (!(g_ascii_islower(key[i]) || g_ascii_isdigit(key[i]) || key[i] == '_')) goto invalid;
		if (suffix != NULL) *suffix = '.';
		g_ptr_array_add(tokens, g_steal_pointer(&key)); at = end + 1;
	}
	return tokens;
invalid:
	venture_set_error_validation(error, "Answer placeholder", "use {field_key} or {group.count}; escape literal braces as {{ and }}");
	g_ptr_array_unref(tokens);
	return NULL;
}

static const VentureFormsField *
pipe_source(GPtrArray *fields, const gchar *key, const VentureFormsField *target)
{
	guint i;
	if (fields == NULL) return NULL;
	for (i = 0; i < fields->len; i++)
	{
		const VentureFormsField *field = g_ptr_array_index(fields, i);
		if (field->group_boundary != VENTURE_FORMS_GROUP_NONE) continue;
		if (g_strcmp0(field->key, key) != 0 && g_strcmp0(field->base_key, key) != 0) continue;
		if (field->group_key != NULL && (target == NULL || g_strcmp0(target->group_key, field->group_key) != 0 ||
		    (field->base_key != NULL && target->row_index != field->row_index))) continue;
		return field;
	}
	return NULL;
}

static gboolean
pipe_precedes(GPtrArray *fields, const VentureFormsField *source, const VentureFormsField *target)
{
	guint i;
	if (target == NULL) return TRUE;
	for (i = 0; i < fields->len; i++)
	{
		const VentureFormsField *field = g_ptr_array_index(fields, i);
		if (g_strcmp0(field->key, target->key) == 0) return FALSE;
		if (field == source) return TRUE;
	}
	return FALSE;
}

static gint
pipe_record_order(gconstpointer a, gconstpointer b)
{
	VentureEntity *left = *(VentureEntity *const *)a, *right = *(VentureEntity *const *)b;
	gint64 lp = venture_forms_get_int(left, "position"), rp = venture_forms_get_int(right, "position");
	gint64 li = venture_entity_get_id(left), ri = venture_entity_get_id(right);
	if (lp != rp) return lp < rp ? -1 : 1;
	if (li == 0) li = G_MAXINT64;
	if (ri == 0) ri = G_MAXINT64;
	return li == ri ? 0 : (li < ri ? -1 : 1);
}

gboolean
venture_forms_pipe_validate_text(const gchar *text, GPtrArray *fields, const VentureFormsField *target, GError **error)
{
	g_autoptr(GPtrArray) tokens = pipe_tokens(text, error);
	guint i;
	if (tokens == NULL) return FALSE;
	for (i = 0; i < tokens->len; i++)
	{
		const gchar *key = g_ptr_array_index(tokens, i);
		if (g_str_has_suffix(key, ".count"))
		{
			g_autofree gchar *group = g_strndup(key, strlen(key) - 6);
			gboolean found = FALSE;
			guint j;
			for (j = 0; j < fields->len; j++)
			{
				const VentureFormsField *member = g_ptr_array_index(fields, j);
				if (g_strcmp0(member->group_key, group) != 0) continue;
				found = TRUE;
				if (target != NULL && g_strcmp0(target->group_key, group) != 0 && !pipe_precedes(fields, member, target)) goto invalid;
			}
			if (!found) goto invalid;
		}
		else
		{
			const VentureFormsField *source = pipe_source(fields, key, target);
			if (source == NULL || source->sensitive || source->kind == VENTURE_FORM_FIELD_PAGE_BREAK ||
			    (target != NULL && !pipe_precedes(fields, source, target))) goto invalid;
		}
	}
	return TRUE;
invalid:
	venture_set_error_validation(error, "Answer placeholder", "references must name earlier non-sensitive questions, a same-row question, or an earlier repeat group's .count");
	return FALSE;
}

gboolean
venture_forms_piping_definition(GPtrArray *fields, const gchar *success, GError **error)
{
	guint i;
	for (i = 0; i < fields->len; i++)
	{
		const VentureFormsField *field = g_ptr_array_index(fields, i);
		if (!venture_forms_pipe_validate_text(field->label, fields, field, error) ||
		    !venture_forms_pipe_validate_text(field->help, fields, field, error)) return FALSE;
	}
	return venture_forms_pipe_validate_text(success, fields, NULL, error);
}

static gboolean
pipe_catalog_has_templates(JsonObject *object, guint depth)
{
	JsonObjectIter iter;
	const gchar *key;
	JsonNode *node;
	if (object == NULL || depth > 3) return FALSE;
	json_object_iter_init(&iter, object);
	while (json_object_iter_next(&iter, &key, &node))
	{
		(void)key;
		if (JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING &&
		    strpbrk(json_node_get_string(node), "{}") != NULL) return TRUE;
		if (JSON_NODE_HOLDS_OBJECT(node) && pipe_catalog_has_templates(json_node_get_object(node), depth + 1)) return TRUE;
	}
	return FALSE;
}

/* Validate the edited draft as a whole: changing a source's order or
 * sensitivity must not strand a template which was valid when it was saved. */
gboolean
venture_forms_validate_piping(VentureDatabase *database, VentureEntity *entity, GError **error)
{
	g_autoptr(VentureEntity) form = VENTURE_IS_FORM(entity) ? g_object_ref(entity) :
		venture_database_get(database, VENTURE_TYPE_FORM, venture_forms_get_int(entity, "form-id"), error);
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GPtrArray) fields = g_ptr_array_new_with_free_func(venture_forms_field_free);
	g_autofree gchar *success = NULL;
	gboolean candidate = VENTURE_IS_FORM_FIELD(entity), replaced = FALSE;
	guint i;
	if (form == NULL) return FALSE;
	rows = venture_forms_fields(database, form, error);
	if (rows == NULL) return FALSE;
	if (candidate)
	{
		for (i = 0; i < rows->len; i++)
		{
			VentureEntity *row = g_ptr_array_index(rows, i);
			if (venture_entity_get_id(row) != venture_entity_get_id(entity)) continue;
			g_object_unref(row); rows->pdata[i] = g_object_ref(entity); replaced = TRUE;
		}
		if (!replaced) g_ptr_array_add(rows, g_object_ref(entity));
	}
	g_ptr_array_sort(rows, pipe_record_order);
	for (i = 0; i < rows->len; i++) g_ptr_array_add(fields, venture_forms_field_from_record(g_ptr_array_index(rows, i)));

	success = venture_forms_get_string(form, "success-message");
	if (!venture_forms_translations_load(database, form, fields, error)) return FALSE;
	/* Layout constraints belong to publish. Only templates need a draft's
	 * group metadata while the author is still assembling its questions. */
	if (!pipe_catalog_has_templates(venture_forms_catalog(fields), 0)) return TRUE;
	if (!venture_forms_groups_load_metadata(database, form, fields, error)) return FALSE;
	return venture_forms_piping_definition(fields, success, error) &&
		venture_forms_translations_check(fields, error);
}

gchar *
venture_forms_pipe_answer(const VentureFormsField *field, JsonNode *answer)
{
	g_autoptr(GString) text = g_string_new(NULL);
	JsonArray *array = answer != NULL && JSON_NODE_HOLDS_ARRAY(answer) ? json_node_get_array(answer) : NULL;
	guint i, length = array != NULL ? json_array_get_length(array) : 1;
	if (answer == NULL || field->sensitive) return g_strdup("");
	if (field->kind == VENTURE_FORM_FIELD_CONSENT) return g_strdup(venture_forms_field_text(field, "message.given", "Given"));
	for (i = 0; i < length; i++)
	{
		JsonNode *value = array != NULL ? json_array_get_element(array, i) : answer;
		g_autofree gchar *number = NULL;
		gchar buffer[G_ASCII_DTOSTR_BUF_SIZE];
		const gchar *shown = NULL;
		guint j;
		if (!JSON_NODE_HOLDS_VALUE(value)) continue;
		if (json_node_get_value_type(value) == G_TYPE_STRING) shown = json_node_get_string(value);
		else if (json_node_get_value_type(value) == G_TYPE_BOOLEAN) shown = venture_forms_field_text(field, json_node_get_boolean(value) ? "message.yes" : "message.no", json_node_get_boolean(value) ? "Yes" : "No");
		else if (json_node_get_value_type(value) == G_TYPE_DOUBLE) shown = g_ascii_dtostr(buffer, sizeof(buffer), json_node_get_double(value));
		else { number = json_to_string(value, FALSE); shown = number; }
		if (venture_forms_kind_has_choices(field->kind))
			for (j = 0; field->choices != NULL && j < field->choices->len; j++)
			{
				const VentureFormsChoice *choice = g_ptr_array_index(field->choices, j);
				if (g_strcmp0(choice->id, shown) == 0) { shown = choice->label; break; }
			}
		if (text->len > 0) pipe_append(text, ", ");
		pipe_append(text, shown);
	}
	return g_string_free(g_steal_pointer(&text), FALSE);
}

/* Group sentinels come from the expansion's validated, bounded shape, not
 * from an arbitrary scalar supplied under a count-looking browser name. */
void
venture_forms_pipe_counts(GPtrArray *fields, JsonObject *raw, JsonObject *values)
{
	guint i;
	for (i = 0; i < fields->len; i++)
	{
		const VentureFormsField *field = g_ptr_array_index(fields, i);
		g_autofree gchar *key = NULL, *count = NULL;
		guint rows = 0;
		if (field->group_key == NULL) continue;
		key = g_strdup_printf("%s.count", field->group_key);
		if (json_object_has_member(values, key)) continue;
		if (raw != NULL && json_object_has_member(raw, field->group_key))
		{
			JsonNode *node = json_object_get_member(raw, field->group_key);
			if (JSON_NODE_HOLDS_ARRAY(node)) rows = json_array_get_length(json_node_get_array(node));
		}
		else rows = field->row_count;
		count = g_strdup_printf("%u", MIN(rows, field->group_max));
		json_object_set_string_member(values, key, count);
	}
}

gchar *
venture_forms_pipe_text(const gchar *text, GPtrArray *fields, const VentureFormsField *target, JsonObject *values)
{
	g_autoptr(GString) out = g_string_new(NULL);
	const gchar *at = text != NULL ? text : "";
	while (*at != '\0' && out->len < FORMS_PIPE_MAX)
	{
		if ((*at == '{' && at[1] == '{') || (*at == '}' && at[1] == '}'))
		{
			g_string_append_c(out, *at); at += 2;
		}
		else if (*at == '{')
		{
			const gchar *end = strchr(at + 1, '}');
			g_autofree gchar *key = NULL, *scoped = NULL;
			const gchar *lookup;
			const VentureFormsField *source;
			if (end == NULL) break;
			key = g_strndup(at + 1, end - at - 1);
			source = pipe_source(fields, key, target);
			lookup = source != NULL ? source->key : key;
			if (source != NULL && source->group_key != NULL && source->base_key == NULL && target != NULL)
			{
				scoped = g_strdup_printf("%s[%u][%s]", source->group_key, target->row_index, source->key);
				lookup = scoped;
			}
			if (values != NULL && (g_str_has_suffix(key, ".count") || (source != NULL && !source->sensitive && (target == NULL || pipe_precedes(fields, source, target)))))
				pipe_append(out, json_object_get_string_member_with_default(values, lookup, ""));
			at = end + 1;
		}
		else
		{
			const gchar *next = g_utf8_next_char(at);
			if (out->len + (next - at) > FORMS_PIPE_MAX) break;
			g_string_append_len(out, at, next - at); at = next;
		}
	}
	return g_string_free(g_steal_pointer(&out), FALSE);
}

JsonObject *
venture_forms_pipe_stored_values(GPtrArray *fields, JsonObject *answers)
{
	JsonObject *values = json_object_new();
	guint i;
	for (i = 0; i < fields->len; i++)
	{
		const VentureFormsField *field = g_ptr_array_index(fields, i);
		JsonArray *rows = NULL;
		guint row, length = 1;
		if (field->sensitive) continue;
		if (field->group_key != NULL)
		{
			JsonNode *group = json_object_get_member(answers, field->group_key);
			if (group == NULL || !JSON_NODE_HOLDS_ARRAY(group)) continue;
			rows = json_node_get_array(group); length = json_array_get_length(rows);
		}
		for (row = 0; row < length; row++)
		{
			JsonObject *object = answers;
			g_autofree gchar *key = NULL, *value = NULL;
			if (rows != NULL)
			{
				JsonNode *item = json_array_get_element(rows, row);
				if (!JSON_NODE_HOLDS_OBJECT(item)) continue;
				object = json_node_get_object(item);
				key = g_strdup_printf("%s[%u][%s]", field->group_key, row, field->key);
			}
			value = venture_forms_pipe_answer(field, json_object_get_member(object, field->key));
			json_object_set_string_member(values, key != NULL ? key : field->key, value);
		}
	}
	venture_forms_pipe_counts(fields, answers, values);
	return values;
}

gchar *
venture_forms_success_message(VentureDatabase *database, VentureEntity *form, VentureEntity *response)
{
	g_autofree gchar *template = venture_forms_get_string(form, "success-message");
	g_autoptr(VentureEntity) version = NULL;
	g_autoptr(GPtrArray) fields = NULL;
	g_autoptr(JsonNode) root = NULL;
	g_autoptr(JsonObject) values = json_object_new();

	if (response != NULL)
	{
		g_autofree gchar *text = venture_forms_get_string(response, "answers");
		version = venture_database_get(database, VENTURE_TYPE_FORM_VERSION, venture_forms_get_int(response, "version-id"), NULL);
		root = json_from_string(text, NULL);
	}
	else version = venture_forms_published_version(database, form, NULL);
	fields = venture_forms_definition_for(database, form, version, NULL);
	if (fields == NULL) return g_strdup("");
	{
		g_autofree gchar *language = response != NULL ? venture_forms_get_string(response, "language") : NULL;
		gchar *localized;
		venture_forms_localize(fields, language);
		localized = g_strdup(venture_forms_text(fields, "form.success_message", template));
		g_free(template); template = localized;
		if (venture_string_is_empty(template)) return g_strdup(venture_forms_text(fields, "message.success", "Thank you. Your response has been received."));
	}
	if (root != NULL && JSON_NODE_HOLDS_OBJECT(root))
	{
		g_clear_pointer(&values, json_object_unref);
		values = venture_forms_pipe_stored_values(fields, json_node_get_object(root));
	}

	return venture_forms_pipe_text(template, fields, NULL, values);
}

void
venture_forms_piping_apply(GPtrArray *fields, JsonObject *values)
{
	guint i;
	for (i = 0; i < fields->len; i++)
	{
		VentureFormsField *field = g_ptr_array_index(fields, i);
		gchar *label = venture_forms_pipe_text(field->label, fields, field, values);
		gchar *help = venture_forms_pipe_text(field->help, fields, field, values);
		g_free(field->label); field->label = label;
		g_free(field->help); field->help = help;
	}
}

/* Only values actually referenced on this page leave the private draft.
 * Current-page input descriptions exclude sensitive questions entirely. */
JsonObject *
venture_forms_pipe_context(GPtrArray *selected, GPtrArray *fields, JsonObject *values)
{
	JsonObject *context = json_object_new();
	JsonObject *previous = json_object_new(), *sources = json_object_new();
	guint i;
	for (i = 0; i < selected->len; i++)
	{
		const VentureFormsField *target = g_ptr_array_index(selected, i);
		const gchar *texts[] = { target->label, target->help };
		guint t;
		for (t = 0; t < G_N_ELEMENTS(texts); t++)
		{
			g_autoptr(GPtrArray) tokens = pipe_tokens(texts[t], NULL);
			guint j;
			if (tokens == NULL) continue;
			for (j = 0; j < tokens->len; j++)
			{
				const gchar *key = g_ptr_array_index(tokens, j);
				const VentureFormsField *source = pipe_source(fields, key, target);
				const gchar *lookup = source != NULL ? source->key : key;
				if (source != NULL && (source->sensitive || !pipe_precedes(fields, source, target))) continue;
				if (values != NULL && json_object_has_member(values, lookup))
					json_object_set_string_member(previous, lookup, json_object_get_string_member(values, lookup));
				if (source != NULL)
				{
					JsonObject *info = json_object_new(), *choices = json_object_new();
					guint c;
					json_object_set_string_member(info, "kind", venture_forms_kind_nick(source->kind));
					json_object_set_string_member(info, "yes", venture_forms_field_text(source, "message.yes", "Yes"));
					json_object_set_string_member(info, "no", venture_forms_field_text(source, "message.no", "No"));
					json_object_set_string_member(info, "given", venture_forms_field_text(source, "message.given", "Given"));
					if (source->kind == VENTURE_FORM_FIELD_HIDDEN) json_object_set_string_member(info, "default", source->default_value != NULL ? source->default_value : "");
					for (c = 0; source->choices != NULL && c < source->choices->len; c++)
					{
						const VentureFormsChoice *choice = g_ptr_array_index(source->choices, c);
						json_object_set_string_member(choices, choice->id, choice->label);
					}
					json_object_set_object_member(info, "choices", choices);
					json_object_set_object_member(sources, lookup, info);
				}
			}
		}
	}
	json_object_set_object_member(context, "values", previous);
	json_object_set_object_member(context, "fields", sources);
	return context;
}
