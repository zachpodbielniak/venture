/*
 * venture-forms-rules.c - Closed, forward-only questionnaire conditions
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "venture-forms-private.h"
#include <math.h>
#include <string.h>

static const gchar *
rule_string(JsonObject *object, const gchar *key)
{
	JsonNode *node = json_object_get_member(object, key);
	return node != NULL && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING ?
		json_node_get_string(node) : NULL;
}

static gint
rule_field_index(GPtrArray *fields, const gchar *key, guint *page)
{
	guint i, current = 0;
	for (i = 0; i < fields->len; i++)
	{
		VentureFormsField *field = g_ptr_array_index(fields, i);
		if (field->kind == VENTURE_FORM_FIELD_PAGE_BREAK) current++;
		if (g_strcmp0(field->key, key) == 0)
		{
			if (page != NULL) *page = current;
			return (gint)i;
		}
	}
	return -1;
}

static JsonObject *
rule_from_record(VentureEntity *record, GPtrArray *fields, GError **error)
{
	g_autofree gchar *text = venture_forms_get_string(record, "conditions");
	g_autofree gchar *target = venture_forms_get_string(record, "target-key");
	g_autoptr(JsonNode) conditions = text != NULL ? json_from_string(text, NULL) : NULL;
	g_autoptr(JsonObject) rule = json_object_new();
	gint action;
	gint target_index = -1;
	guint target_page = 0, source_page = 0, i;
	JsonArray *array;
	g_object_get(record, "action", &action, NULL);
	if (conditions == NULL || !JSON_NODE_HOLDS_ARRAY(conditions)) goto invalid;
	array = json_node_get_array(conditions);
	if (json_array_get_length(array) == 0 || json_array_get_length(array) > 16) goto invalid;
	if (action != VENTURE_FORM_RULE_END)
	{
		target_index = rule_field_index(fields, target, &target_page);
		if (target_index < 0) goto invalid;
		if ((((VentureFormsField *)g_ptr_array_index(fields, target_index))->kind == VENTURE_FORM_FIELD_PAGE_BREAK) !=
		    (action == VENTURE_FORM_RULE_JUMP)) goto invalid;
	}
	else if (!venture_string_is_empty(target)) goto invalid;
	for (i = 0; i < json_array_get_length(array); i++)
	{
		JsonNode *node = json_array_get_element(array, i);
		JsonObject *condition;
		const gchar *key, *op, *value;
		gint index;
		guint page;
		if (!JSON_NODE_HOLDS_OBJECT(node)) goto invalid;
		condition = json_node_get_object(node);
		key = rule_string(condition, "field");
		op = rule_string(condition, "operator");
		value = rule_string(condition, "value");
		index = rule_field_index(fields, key, &page);
		if (index < 0 || ((VentureFormsField *)g_ptr_array_index(fields, index))->kind == VENTURE_FORM_FIELD_PAGE_BREAK) goto invalid;
		/* Navigation waits until every condition's question has been asked. */
		source_page = MAX(source_page, page);
		if (action != VENTURE_FORM_RULE_END && index >= target_index) goto invalid;
		if (g_strcmp0(op, "is_empty") != 0)
		{
			if (value == NULL || strlen(value) > 4096) goto invalid;
			if (g_strcmp0(op, "equals") != 0 && g_strcmp0(op, "not_equals") != 0 &&
			    g_strcmp0(op, "contains") != 0 && g_strcmp0(op, "any_of") != 0 &&
			    g_strcmp0(op, "greater_than") != 0 && g_strcmp0(op, "less_than") != 0) goto invalid;
			if (g_strcmp0(op, "greater_than") == 0 || g_strcmp0(op, "less_than") == 0)
			{
				gchar *end = NULL;
				gdouble number = g_ascii_strtod(value, &end);
				if (end == value || *end != '\0' || !isfinite(number)) goto invalid;
			}
		}
	}
	if (action == VENTURE_FORM_RULE_JUMP && target_page <= source_page) goto invalid;
	json_object_set_int_member(rule, "action", action);
	json_object_set_string_member(rule, "target", target != NULL ? target : "");
	json_object_set_int_member(rule, "from_page", source_page);
	json_object_set_int_member(rule, "to_page", target_page);
	json_object_set_boolean_member(rule, "any", venture_forms_get_bool(record, "any-condition"));
	json_object_set_array_member(rule, "conditions", json_array_ref(array));
	return g_steal_pointer(&rule);
invalid:
	venture_set_error_validation(error, "Rule", "use 1–16 conditions on known earlier questions, supported operators, and a forward target (empty for end)");
	return NULL;
}

/* Published definitions can outlive the process that wrote them. Validate
 * their closed vocabulary again before any typed JSON access or traversal. */
gboolean
venture_forms_rules_restore(GPtrArray *fields, JsonNode *node, GError **error)
{
	g_autoptr(JsonArray) rules = json_array_new();
	JsonArray *array;
	guint i;
	if (node == NULL) return TRUE;
	if (!JSON_NODE_HOLDS_ARRAY(node)) goto invalid;
	array = json_node_get_array(node);
	if (json_array_get_length(array) > 256) goto invalid;
	for (i = 0; i < json_array_get_length(array); i++)
	{
		JsonNode *item = json_array_get_element(array, i), *action_node, *any_node, *conditions;
		JsonObject *object;
		g_autoptr(VentureEntity) record = VENTURE_ENTITY(venture_form_rule_new());
		g_autofree gchar *text = NULL;
		JsonObject *rule;
		gint64 action;
		if (!JSON_NODE_HOLDS_OBJECT(item)) goto invalid;
		object = json_node_get_object(item);
		action_node = json_object_get_member(object, "action");
		any_node = json_object_get_member(object, "any");
		conditions = json_object_get_member(object, "conditions");
		if (action_node == NULL || !JSON_NODE_HOLDS_VALUE(action_node) || json_node_get_value_type(action_node) != G_TYPE_INT64 ||
		    any_node == NULL || !JSON_NODE_HOLDS_VALUE(any_node) || json_node_get_value_type(any_node) != G_TYPE_BOOLEAN ||
		    conditions == NULL || !JSON_NODE_HOLDS_ARRAY(conditions) || rule_string(object, "target") == NULL) goto invalid;
		action = json_node_get_int(action_node);
		if (action < VENTURE_FORM_RULE_SHOW || action > VENTURE_FORM_RULE_END) goto invalid;
		text = json_to_string(conditions, FALSE);
		g_object_set(record, "action", (gint)action, "target-key", rule_string(object, "target"),
			"conditions", text, "any-condition", json_node_get_boolean(any_node), NULL);
		rule = rule_from_record(record, fields, error);
		if (rule == NULL) return FALSE;
		json_array_add_object_element(rules, rule);
	}
	for (i = 0; i < fields->len; i++)
		((VentureFormsField *)g_ptr_array_index(fields, i))->rules = json_array_ref(rules);
	return TRUE;
invalid:
	venture_set_error_validation(error, "Rules", "the published rule definition cannot be read");
	return FALSE;
}

gboolean
venture_forms_validate_rule(VentureDatabase *database, VentureEntity *entity,
	VentureEntity *previous, gpointer data, GError **error)
{
	g_autoptr(VentureEntity) form = venture_database_get(database, VENTURE_TYPE_FORM,
		venture_forms_get_int(entity, "form-id"), error);
	g_autoptr(GPtrArray) fields = NULL;
	g_autoptr(JsonObject) rule = NULL;
	(void)previous; (void)data;
	if (form == NULL) return FALSE;
	if (venture_entity_get_organization_id(form) != venture_entity_get_organization_id(entity))
	{
		venture_set_error_validation(error, "Rule", "the form must belong to this organization");
		return FALSE;
	}
	/* Read questions without loading rules again, avoiding validation recursion. */
	g_object_set_data(G_OBJECT(form), "venture-forms-loading-rule", GINT_TO_POINTER(1));
	fields = venture_forms_definition_from_records(database, form, error);
	if (fields == NULL) return FALSE;
	rule = rule_from_record(entity, fields, error);
	return rule != NULL;
}

gboolean
venture_forms_rules_load(VentureDatabase *database, VentureEntity *form, GPtrArray *fields, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_FORM_RULE);
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(JsonArray) rules = json_array_new();
	guint i;
	if (g_object_get_data(G_OBJECT(form), "venture-forms-loading-rule") != NULL) return TRUE;
	venture_query_set_organization(query, venture_entity_get_organization_id(form));
	venture_query_add_filter_int(query, "form-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(form), NULL);
	venture_query_add_order(query, "position", VENTURE_SORT_ASCENDING, NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	venture_query_set_limit(query, 257);
	rows = venture_database_find(database, query, error);
	if (rows == NULL) return FALSE;
	if (rows->len > 256)
	{
		venture_set_error_validation(error, "Rules", "at most 256 rules per version");
		return FALSE;
	}
	for (i = 0; i < rows->len; i++)
	{
		JsonObject *rule = rule_from_record(g_ptr_array_index(rows, i), fields, error);
		if (rule == NULL) return FALSE;
		json_array_add_object_element(rules, rule);
	}
	for (i = 0; i < fields->len; i++)
		((VentureFormsField *)g_ptr_array_index(fields, i))->rules = json_array_ref(rules);
	return TRUE;
}

static gboolean
rule_atom(const gchar *actual, const gchar *op, const gchar *wanted)
{
	if (actual == NULL) actual = "";
	if (g_strcmp0(op, "equals") == 0 || g_strcmp0(op, "not_equals") == 0)
		return g_strcmp0(actual, wanted) == 0;
	if (g_strcmp0(op, "contains") == 0) return wanted != NULL && strstr(actual, wanted) != NULL;
	if (g_strcmp0(op, "any_of") == 0)
	{
		g_auto(GStrv) choices = g_strsplit(wanted, "\n", -1);
		guint i;
		for (i = 0; choices[i] != NULL; i++) if (g_strcmp0(actual, choices[i]) == 0) return TRUE;
		return FALSE;
	}
	if (g_strcmp0(op, "greater_than") == 0 || g_strcmp0(op, "less_than") == 0)
	{
		gchar *end = NULL;
		gdouble number = g_ascii_strtod(actual, &end), limit = g_ascii_strtod(wanted, NULL);
		if (end == actual || *end != '\0' || !isfinite(number)) return FALSE;
		return g_strcmp0(op, "greater_than") == 0 ? number > limit : number < limit;
	}
	return FALSE;
}

gboolean
venture_forms_condition_matches(JsonObject *condition, JsonObject *values)
{
	const gchar *op = rule_string(condition, "operator");
	const gchar *wanted = rule_string(condition, "value");
	JsonNode *node = values != NULL ? json_object_get_member(values, rule_string(condition, "field")) : NULL;
	gboolean match = FALSE;
	if (g_strcmp0(op, "is_empty") == 0)
		return node == NULL || JSON_NODE_HOLDS_NULL(node) ||
			(JSON_NODE_HOLDS_ARRAY(node) && json_array_get_length(json_node_get_array(node)) == 0) ||
			(JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING && *json_node_get_string(node) == '\0');
	if (node != NULL && JSON_NODE_HOLDS_ARRAY(node))
	{
		JsonArray *array = json_node_get_array(node);
		guint i;
		for (i = 0; i < json_array_get_length(array); i++)
		{
			JsonNode *item = json_array_get_element(array, i);
			if (JSON_NODE_HOLDS_VALUE(item) && json_node_get_value_type(item) == G_TYPE_STRING &&
			    rule_atom(json_node_get_string(item), op, wanted)) match = TRUE;
		}
	}
	else if (node != NULL && JSON_NODE_HOLDS_VALUE(node))
	{
		g_autofree gchar *text = json_node_get_value_type(node) == G_TYPE_STRING ?
			g_strdup(json_node_get_string(node)) : json_to_string(node, FALSE);
		match = rule_atom(text, op, wanted);
	}
	else match = rule_atom("", op, wanted);
	return g_strcmp0(op, "not_equals") == 0 ? !match : match;
}

gboolean
venture_forms_rule_matches(JsonObject *rule, JsonObject *values)
{
	JsonArray *conditions = json_object_get_array_member(rule, "conditions");
	gboolean any = json_object_get_boolean_member(rule, "any");
	guint i;
	for (i = 0; i < json_array_get_length(conditions); i++)
	{
		gboolean matches = venture_forms_condition_matches(json_array_get_object_element(conditions, i), values);
		if (any && matches) return TRUE;
		if (!any && !matches) return FALSE;
	}
	return !any;
}

gboolean
venture_forms_field_active(const VentureFormsField *field, JsonObject *values, gboolean *required)
{
	gboolean active = TRUE, must = field->required;
	guint i;
	for (i = 0; field->rules != NULL && i < json_array_get_length(field->rules); i++)
	{
		JsonObject *rule = json_array_get_object_element(field->rules, i);
		gint action = (gint)json_object_get_int_member(rule, "action");
		gboolean matches;
		if (g_strcmp0(rule_string(rule, "target"), field->key) != 0) continue;
		matches = venture_forms_rule_matches(rule, values);
		if (action == VENTURE_FORM_RULE_SHOW && !matches) active = FALSE;
		if (action == VENTURE_FORM_RULE_HIDE && matches) active = FALSE;
		if (action == VENTURE_FORM_RULE_REQUIRE && matches) must = TRUE;
	}
	if (required != NULL) *required = active && must;
	return active;
}

guint
venture_forms_next_page(GPtrArray *fields, guint page, JsonObject *values)
{
	JsonArray *rules = fields->len > 0 ? ((VentureFormsField *)g_ptr_array_index(fields, 0))->rules : NULL;
	guint i;
	for (i = 0; rules != NULL && i < json_array_get_length(rules); i++)
	{
		JsonObject *rule = json_array_get_object_element(rules, i);
		gint action = (gint)json_object_get_int_member(rule, "action");
		if ((guint)json_object_get_int_member(rule, "from_page") != page || !venture_forms_rule_matches(rule, values)) continue;
		if (action == VENTURE_FORM_RULE_JUMP) return (guint)json_object_get_int_member(rule, "to_page");
		if (action == VENTURE_FORM_RULE_END) return venture_forms_page_count(fields);
	}
	return page + 1;
}

void
venture_forms_rules_filter(GPtrArray *fields, JsonObject *values, GHashTable *not_shown)
{
	guint page, next = 0, pages = venture_forms_page_count(fields);
	for (page = 0; page < pages; page++)
	{
		g_autoptr(GPtrArray) questions = venture_forms_page_fields(fields, page);
		guint i;
		for (i = 0; i < questions->len; i++)
		{
			VentureFormsField *field = g_ptr_array_index(questions, i);
			if (page != next || !venture_forms_field_active(field, values, NULL))
			{
				json_object_remove_member(values, field->key);
				if (not_shown != NULL) g_hash_table_add(not_shown, g_strdup(field->key));
			}
		}
		if (page == next) next = venture_forms_next_page(fields, page, values);
	}
}
