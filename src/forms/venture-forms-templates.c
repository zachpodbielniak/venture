/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture-forms-private.h"
#include <yaml-glib.h>
#include <yaml.h>
#include <string.h>

#define FORM_PORTABLE_BYTES (1024 * 1024)
#define FORM_PORTABLE_ROWS 2000

/* Only this ownership boundary is form-specific. Fields and wire values
 * come from the same metadata and serializer as every other record surface. */
typedef struct { const gchar *key; GType (*type)(void); } FormsPart;
static const FormsPart parts[] = {
	{ "groups", venture_form_group_get_type },
	{ "fields", venture_form_field_get_type },
	{ "rules", venture_form_rule_get_type },
	{ "translations", venture_form_translation_get_type },
	{ "result_bands", venture_form_result_band_get_type },
	{ "prices", venture_form_price_get_type }
};

static gboolean portable_fail(GError **error, const gchar *message)
{
	venture_set_error_validation(error, "Form definition", "%s", message);
	return FALSE;
}

static gboolean portable_field(VentureFieldSpec *spec)
{
	static const gchar *const machinery[] = { "id", "uuid", "organization_id", "created_at", "updated_at", "deleted_at", "version", "attributes", "form_id", "group_id", "state", "slug", "public_token", "published_version_id", "published_number", NULL };
	g_autofree gchar *wire = venture_entity_property_to_column(spec->name);
	return !(spec->flags & (VENTURE_COLUMN_FLAG_SENSITIVE | VENTURE_COLUMN_FLAG_TRANSIENT)) && !g_strv_contains(machinery, wire);
}

static JsonNode *portable_record(VentureDatabase *database, VentureEntity *entity, GError **error)
{
	g_autoptr(JsonNode) wire = venture_serializable_to_json(VENTURE_SERIALIZABLE(entity), FALSE);
	g_autoptr(JsonObject) output = json_object_new();
	g_autoptr(GPtrArray) specs = venture_entity_get_field_specs(entity);
	guint i;
	for (i = 0; i < specs->len; i++)
	{
		VentureFieldSpec *spec = g_ptr_array_index(specs, i);
		g_autofree gchar *key = venture_entity_property_to_column(spec->name);
		JsonNode *value = json_object_get_member(json_node_get_object(wire), key);
		if (!portable_field(spec) || value == NULL) continue;
		if (spec->kind == VENTURE_FIELD_KIND_REFERENCE)
		{
			gint64 id = json_node_get_int(value);
			if (id > 0)
			{
				g_autofree gchar *binding = g_strdup_printf("%s:%" G_GINT64_FORMAT, spec->reference_type, id);
				JsonObject *reference = json_object_new();
				json_object_set_string_member(reference, "binding", binding);
				json_object_set_object_member(output, key, reference);
			}
			continue;
		}
		/* A sensitive question's old prefilled text must not become an
		 * export side channel even if it predates the current validator. */
		if (VENTURE_IS_FORM_FIELD(entity) && venture_forms_get_bool(entity, "sensitive") && g_str_equal(key, "default_value")) continue;
		json_object_set_member(output, key, json_node_copy(value));
	}
	if (VENTURE_IS_FORM_FIELD(entity) && venture_forms_get_int(entity, "group-id") > 0)
	{
		g_autoptr(VentureEntity) group = venture_database_get(database, VENTURE_TYPE_FORM_GROUP, venture_forms_get_int(entity, "group-id"), error);
		g_autofree gchar *key = group != NULL ? venture_forms_get_string(group, "key") : NULL;
		if (group == NULL) return NULL;
		json_object_set_string_member(output, "group", key);
	}
	{
		JsonNode *node = json_node_new(JSON_NODE_OBJECT);
		json_node_take_object(node, g_steal_pointer(&output)); return node;
	}
}

JsonNode *venture_forms_export_definition(VentureDatabase *database, VentureEntity *form, GError **error)
{
	g_autoptr(JsonObject) object = json_object_new();
	JsonNode *record, *result;
	guint i, j, total = 0;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	g_return_val_if_fail(VENTURE_IS_FORM(form), NULL);
	record = portable_record(database, form, error); if (record == NULL) return NULL;
	json_object_set_string_member(object, "format", "venture-form"); json_object_set_int_member(object, "version", 1);
	json_object_set_member(object, "form", record);
	for (i = 0; i < G_N_ELEMENTS(parts); i++)
	{
		g_autoptr(VentureQuery) query = venture_query_new(parts[i].type());
		g_autoptr(GPtrArray) rows = NULL;
		g_autoptr(JsonArray) array = json_array_new();
		venture_query_set_organization(query, venture_entity_get_organization_id(form));
		venture_query_add_filter_int(query, "form-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(form), NULL);
		venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
		venture_query_set_limit(query, FORM_PORTABLE_ROWS + 1); rows = venture_database_find(database, query, error);
		if (rows == NULL) return NULL;
		total += rows->len;
		if (total > FORM_PORTABLE_ROWS) { portable_fail(error, "too many definition records to export"); return NULL; }
		for (j = 0; j < rows->len; j++)
		{
			record = portable_record(database, g_ptr_array_index(rows, j), error);
			if (record == NULL) return NULL;
			json_array_add_element(array, record);
		}
		json_object_set_array_member(object, parts[i].key, g_steal_pointer(&array));
	}
	result = json_node_new(JSON_NODE_OBJECT); json_node_take_object(result, g_steal_pointer(&object)); return result;
}

gchar *venture_forms_definition_format(JsonNode *definition, const gchar *format, GError **error)
{
	g_autoptr(YamlDocument) document = NULL;
	g_autoptr(YamlGenerator) generator = NULL;
	g_autofree gchar *text = NULL;
	g_return_val_if_fail(definition != NULL, NULL);
	if (g_strcmp0(format, "json") == 0 || format == NULL)
	{
		text = json_to_string(definition, TRUE);
		if (strlen(text) > FORM_PORTABLE_BYTES) { portable_fail(error, "export exceeds the 1 MiB import bound"); return NULL; }
		return g_steal_pointer(&text);
	}
	if (g_strcmp0(format, "yaml") != 0) { portable_fail(error, "format must be json or yaml"); return NULL; }
	document = yaml_document_from_json_node(definition); generator = yaml_generator_new();
	yaml_generator_set_document(generator, document); yaml_generator_set_indent(generator, 2);
	text = yaml_generator_to_data(generator, NULL, error);
	if (text != NULL && strlen(text) > FORM_PORTABLE_BYTES) { portable_fail(error, "export exceeds the 1 MiB import bound"); return NULL; }
	return g_steal_pointer(&text);
}

/* YAML aliases can expand exponentially during JSON conversion. Inspect
 * events before constructing nodes, bounding depth and refusing aliases. */
static JsonNode *portable_parse(const gchar *text, GError **error)
{
	yaml_parser_t parser;
	yaml_event_t event;
	g_autoptr(YamlParser) reader = NULL;
	guint depth = 0, events = 0, documents = 0;
	gboolean done = FALSE, valid = TRUE;
	if (text == NULL || strlen(text) > FORM_PORTABLE_BYTES || !g_utf8_validate(text, -1, NULL)) goto bad;
	if (!yaml_parser_initialize(&parser)) goto bad;
	yaml_parser_set_input_string(&parser, (const unsigned char *)text, strlen(text));
	while (!done && valid)
	{
		if (!yaml_parser_parse(&parser, &event)) { valid = FALSE; break; }
		events++;
		if (event.type == YAML_MAPPING_START_EVENT || event.type == YAML_SEQUENCE_START_EVENT) depth++;
		if (event.type == YAML_MAPPING_END_EVENT || event.type == YAML_SEQUENCE_END_EVENT) depth--;
		if (event.type == YAML_DOCUMENT_START_EVENT) documents++;
		if (event.type == YAML_ALIAS_EVENT || depth > 32 || events > 30000 || documents > 1) valid = FALSE;
		done = event.type == YAML_STREAM_END_EVENT;
		yaml_event_delete(&event);
	}
	yaml_parser_delete(&parser);
	if (!valid || documents != 1) goto bad;
	reader = yaml_parser_new();
	if (!yaml_parser_load_from_data(reader, text, -1, error)) return NULL;
	return yaml_node_to_json_node(yaml_parser_get_root(reader));
bad:
	portable_fail(error, "expected one UTF-8 JSON/YAML document, at most 1 MiB and 32 levels, without aliases"); return NULL;
}

static gboolean portable_value(VentureFieldSpec *spec, JsonNode *value)
{
	GType type = JSON_NODE_HOLDS_VALUE(value) ? json_node_get_value_type(value) : G_TYPE_INVALID;
	if (JSON_NODE_HOLDS_NULL(value)) return TRUE;
	switch (spec->kind)
	{
	case VENTURE_FIELD_KIND_INTEGER: return type == G_TYPE_INT64;
	case VENTURE_FIELD_KIND_DOUBLE: return type == G_TYPE_INT64 || type == G_TYPE_DOUBLE;
	case VENTURE_FIELD_KIND_BOOLEAN: return type == G_TYPE_BOOLEAN;
	case VENTURE_FIELD_KIND_JSON: return TRUE;
	case VENTURE_FIELD_KIND_MONEY:
		if (JSON_NODE_HOLDS_OBJECT(value))
		{
			JsonObject *money = json_node_get_object(value);
			JsonNode *amount = json_object_get_member(money, "amount"), *currency = json_object_get_member(money, "currency"), *exponent = json_object_get_member(money, "exponent");
			return amount != NULL && JSON_NODE_HOLDS_VALUE(amount) && json_node_get_value_type(amount) == G_TYPE_INT64 &&
				currency != NULL && JSON_NODE_HOLDS_VALUE(currency) && json_node_get_value_type(currency) == G_TYPE_STRING &&
				(exponent == NULL || (JSON_NODE_HOLDS_VALUE(exponent) && json_node_get_value_type(exponent) == G_TYPE_INT64));
		}
		return FALSE;
	case VENTURE_FIELD_KIND_ENUM: return type == G_TYPE_STRING && spec->choices != NULL && g_strv_contains((const gchar *const *)spec->choices, json_node_get_string(value));
	default: return type == G_TYPE_STRING;
	}
}

static VentureEntity *portable_import_record(VentureDatabase *database, GType type, JsonNode *node,
	gint64 organization, JsonObject *bindings, GError **error)
{
	g_autoptr(VentureEntity) entity = g_object_new(type, NULL);
	g_autoptr(GPtrArray) specs = venture_entity_get_field_specs(entity);
	g_autoptr(GHashTable) allowed = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	g_autoptr(JsonNode) copy = NULL;
	g_autoptr(GList) members = NULL;
	GList *iter;
	guint i;
	if (!JSON_NODE_HOLDS_OBJECT(node)) goto bad;
	/* JsonNode copies share object storage. Own the outer mapping before
	 * removing portable controls or replacing reference bindings. */
	copy = json_node_new(JSON_NODE_OBJECT); json_node_take_object(copy, json_object_new());
	members = json_object_get_members(json_node_get_object(node));
	for (iter = members; iter != NULL; iter = iter->next)
		json_object_set_member(json_node_get_object(copy), iter->data, json_node_copy(json_object_get_member(json_node_get_object(node), iter->data)));
	for (i = 0; i < specs->len; i++)
	{
		VentureFieldSpec *spec = g_ptr_array_index(specs, i);
		if (portable_field(spec)) g_hash_table_insert(allowed, venture_entity_property_to_column(spec->name), spec);
	}
	for (iter = members; iter != NULL; iter = iter->next)
	{
		const gchar *key = iter->data;
		VentureFieldSpec *spec = g_hash_table_lookup(allowed, key);
		JsonNode *value = json_object_get_member(json_node_get_object(copy), key);
		if (type == VENTURE_TYPE_FORM_FIELD && g_str_equal(key, "group"))
		{
			if (!JSON_NODE_HOLDS_VALUE(value) || json_node_get_value_type(value) != G_TYPE_STRING) goto bad;
			json_object_remove_member(json_node_get_object(copy), key); continue;
		}
		if (spec == NULL) goto bad;
		if (spec->kind == VENTURE_FIELD_KIND_REFERENCE)
		{
			JsonNode *binding, *mapped;
			g_autoptr(VentureEntity) target = NULL;
			GType target_type;
			if (JSON_NODE_HOLDS_NULL(value)) { json_object_remove_member(json_node_get_object(copy), key); continue; }
			if (!JSON_NODE_HOLDS_OBJECT(value) || json_object_get_size(json_node_get_object(value)) != 1) goto bad;
			binding = json_object_get_member(json_node_get_object(value), "binding");
			if (binding == NULL || !JSON_NODE_HOLDS_VALUE(binding) || json_node_get_value_type(binding) != G_TYPE_STRING) goto bad;
			mapped = bindings != NULL ? json_object_get_member(bindings, json_node_get_string(binding)) : NULL;
			if (mapped == NULL || !JSON_NODE_HOLDS_VALUE(mapped) || json_node_get_value_type(mapped) != G_TYPE_INT64 || json_node_get_int(mapped) <= 0) goto bad;
			target_type = venture_entity_registry_lookup(venture_entity_registry_get_default(), spec->reference_type);
			if (target_type == G_TYPE_INVALID) goto bad;
			target = venture_database_get(database, target_type, json_node_get_int(mapped), NULL);
			if (target == NULL || venture_entity_get_organization_id(target) != organization) goto bad;
			json_object_set_int_member(json_node_get_object(copy), key, json_node_get_int(mapped));
		}
		else if (!portable_value(spec, value)) goto bad;
	}
	if (!venture_serializable_from_json(VENTURE_SERIALIZABLE(entity), copy, error)) return NULL;
	venture_entity_set_organization_id(entity, organization);
	return g_steal_pointer(&entity);
bad:
	portable_fail(error, "unknown/forbidden field, invalid value, or missing same-organization reference binding"); return NULL;
}

VentureEntity *venture_forms_import_definition(VentureContext *context, gint64 organization,
	const gchar *text, JsonObject *bindings, const VentureActor *actor, GError **error)
{
	g_autoptr(JsonNode) parsed = NULL;
	g_autoptr(VentureEntity) form = NULL;
	g_autoptr(GPtrArray) rows = g_ptr_array_new_with_free_func(g_object_unref), fields = NULL;
	g_autoptr(GHashTable) groups = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	g_autoptr(GList) members = NULL;
	g_autofree gchar *uuid = NULL, *slug = NULL;
	VentureDatabase *database;
	JsonObject *object;
	JsonNode *form_node, *format, *version;
	GList *iter;
	guint i, j;
	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);
	if (!venture_context_module_enabled(context, "forms")) { portable_fail(error, "forms module is off"); return NULL; }
	database = venture_context_get_database(context);
	if (organization <= 0) organization = venture_context_get_default_organization_id(context);
	parsed = portable_parse(text, error); if (parsed == NULL) return NULL;
	if (!JSON_NODE_HOLDS_OBJECT(parsed)) goto bad;
	object = json_node_get_object(parsed); format = json_object_get_member(object, "format"); version = json_object_get_member(object, "version");
	if (format == NULL || !JSON_NODE_HOLDS_VALUE(format) || json_node_get_value_type(format) != G_TYPE_STRING || g_strcmp0(json_node_get_string(format), "venture-form") != 0 ||
	    version == NULL || !JSON_NODE_HOLDS_VALUE(version) || json_node_get_value_type(version) != G_TYPE_INT64 || json_node_get_int(version) != 1) goto bad;
	members = json_object_get_members(object);
	for (iter = members; iter != NULL; iter = iter->next)
	{
		const gchar *key = iter->data;
		gboolean known = g_str_equal(key, "format") || g_str_equal(key, "version") || g_str_equal(key, "form");
		for (i = 0; i < G_N_ELEMENTS(parts); i++) known |= g_str_equal(key, parts[i].key);
		if (!known) goto bad;
	}
	form_node = json_object_get_member(object, "form"); if (form_node == NULL) goto bad;
	form = portable_import_record(database, VENTURE_TYPE_FORM, form_node, organization, bindings, error); if (form == NULL) return NULL;
	for (i = 0; i < G_N_ELEMENTS(parts); i++)
	{
		JsonNode *node = json_object_get_member(object, parts[i].key);
		JsonArray *array;
		if (node == NULL) continue;
		if (!JSON_NODE_HOLDS_ARRAY(node)) goto bad;
		array = json_node_get_array(node);
		if (json_array_get_length(array) > FORM_PORTABLE_ROWS - rows->len) goto bad;
		for (j = 0; j < json_array_get_length(array); j++)
		{
			JsonNode *row = json_array_get_element(array, j);
			VentureEntity *entity = portable_import_record(database, parts[i].type(), row, organization, bindings, error);
			if (entity == NULL) return NULL;
			if (parts[i].type() == VENTURE_TYPE_FORM_FIELD && json_object_has_member(json_node_get_object(row), "group"))
				g_object_set_data_full(G_OBJECT(entity), "portable-group", g_strdup(json_object_get_string_member(json_node_get_object(row), "group")), g_free);
			g_ptr_array_add(rows, entity);
		}
	}
	/* A new identity avoids tokens and slugs accidentally pointing back at
	 * the source install. Import never publishes or copies any response. */
	uuid = g_uuid_string_random(); slug = g_strdup_printf("import-%.16s", uuid);
	g_object_set(form, "state", VENTURE_FORM_DRAFT, "slug", slug, NULL);
	if (!venture_database_begin(database, error)) return NULL;
	if (!venture_database_save(database, form, actor, error)) goto rollback;
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *entity = g_ptr_array_index(rows, i);
		const gchar *group = g_object_get_data(G_OBJECT(entity), "portable-group");
		g_object_set(entity, "form-id", venture_entity_get_id(form), NULL);
		if (group != NULL)
		{
			gint64 *id = g_hash_table_lookup(groups, group);
			if (id == NULL) { portable_fail(error, "question names a missing repeat group"); goto rollback; }
			g_object_set(entity, "group-id", *id, NULL);
		}
		if (!venture_database_save(database, entity, actor, error)) goto rollback;
		if (VENTURE_IS_FORM_GROUP(entity))
		{
			gint64 *id = g_new(gint64, 1); *id = venture_entity_get_id(entity);
			g_hash_table_insert(groups, venture_forms_get_string(entity, "key"), id);
		}
	}
	fields = venture_forms_definition_from_records(database, form, error);
	if (fields == NULL) goto rollback;
	if (!venture_database_commit(database, error)) return NULL;
	return g_steal_pointer(&form);
rollback:
	venture_database_rollback(database); return NULL;
bad:
	portable_fail(error, "expected venture-form version 1 with form and bounded definition arrays"); return NULL;
}

/* Templates are portable definitions, not a second construction path.
 * Requests intentionally do not promise a booking or charge a card. */
static const struct { const gchar *name; const gchar *definition; } templates[] = {
	{ "contact", "{\"format\":\"venture-form\",\"version\":1,\"form\":{\"name\":\"Contact us\",\"title\":\"Contact us\",\"submit_label\":\"Send\",\"success_message\":\"Thank you. Your response has been received.\",\"retention_days\":90,\"retention_action\":\"purge\"},\"fields\":[{\"key\":\"name\",\"label\":\"Your name\",\"kind\":\"short_text\",\"required\":true,\"autocomplete\":\"name\",\"position\":10},{\"key\":\"email\",\"label\":\"Email address\",\"kind\":\"email\",\"required\":true,\"position\":20},{\"key\":\"message\",\"label\":\"How can we help?\",\"kind\":\"long_text\",\"required\":true,\"position\":30}]}" },
	{ "feedback", "{\"format\":\"venture-form\",\"version\":1,\"form\":{\"name\":\"Share your feedback\",\"title\":\"Share your feedback\",\"submit_label\":\"Send\",\"success_message\":\"Thank you. Your response has been received.\",\"retention_days\":90,\"retention_action\":\"purge\"},\"fields\":[{\"key\":\"rating\",\"label\":\"How was your experience?\",\"kind\":\"rating\",\"required\":true,\"min_value\":1,\"max_value\":5,\"position\":10},{\"key\":\"comments\",\"label\":\"What could we improve?\",\"kind\":\"long_text\",\"position\":20}]}" },
	{ "nps", "{\"format\":\"venture-form\",\"version\":1,\"form\":{\"name\":\"Recommend us\",\"title\":\"Recommend us\",\"submit_label\":\"Send\",\"success_message\":\"Thank you. Your response has been received.\",\"retention_days\":90,\"retention_action\":\"purge\"},\"fields\":[{\"key\":\"recommend\",\"label\":\"How likely are you to recommend us?\",\"kind\":\"rating\",\"required\":true,\"min_value\":0,\"max_value\":10,\"help\":\"0 means not at all likely; 10 means extremely likely.\",\"position\":10},{\"key\":\"reason\",\"label\":\"What is the main reason?\",\"kind\":\"long_text\",\"position\":20}]}" },
	{ "event-signup", "{\"format\":\"venture-form\",\"version\":1,\"form\":{\"name\":\"Event signup\",\"title\":\"Event signup\",\"submit_label\":\"Send\",\"success_message\":\"Thank you. Your response has been received.\",\"retention_days\":90,\"retention_action\":\"purge\"},\"fields\":[{\"key\":\"name\",\"label\":\"Your name\",\"kind\":\"short_text\",\"required\":true,\"autocomplete\":\"name\",\"position\":10},{\"key\":\"email\",\"label\":\"Email address\",\"kind\":\"email\",\"required\":true,\"position\":20},{\"key\":\"access\",\"label\":\"Anything we can do to help you attend?\",\"kind\":\"long_text\",\"sensitive\":true,\"position\":30}]}" },
	{ "job-application", "{\"format\":\"venture-form\",\"version\":1,\"form\":{\"name\":\"Job application\",\"title\":\"Job application\",\"submit_label\":\"Send\",\"success_message\":\"Thank you. Your response has been received.\",\"retention_days\":90,\"retention_action\":\"purge\"},\"fields\":[{\"key\":\"name\",\"label\":\"Your name\",\"kind\":\"short_text\",\"required\":true,\"autocomplete\":\"name\",\"position\":10},{\"key\":\"email\",\"label\":\"Email address\",\"kind\":\"email\",\"required\":true,\"position\":20},{\"key\":\"experience\",\"label\":\"Tell us about your experience\",\"kind\":\"long_text\",\"required\":true,\"position\":30},{\"key\":\"portfolio\",\"label\":\"Portfolio or resume link\",\"kind\":\"url\",\"position\":40}]}" },
	{ "newsletter", "{\"format\":\"venture-form\",\"version\":1,\"form\":{\"name\":\"Newsletter signup\",\"title\":\"Newsletter signup\",\"submit_label\":\"Send\",\"success_message\":\"Thank you. Your response has been received.\",\"retention_days\":90,\"retention_action\":\"purge\",\"double_opt_in\":true,\"optin_email_field\":\"email\",\"description\":\"Confirm your signup using the email we send you.\"},\"fields\":[{\"key\":\"email\",\"label\":\"Email address\",\"kind\":\"email\",\"required\":true,\"position\":10},{\"key\":\"permission\",\"label\":\"I want to receive this newsletter by email. I can unsubscribe at any time.\",\"kind\":\"consent\",\"marketing_consent\":true,\"required\":true,\"position\":20}]}" },
	{ "appointment-request", "{\"format\":\"venture-form\",\"version\":1,\"form\":{\"name\":\"Request an appointment\",\"title\":\"Request an appointment\",\"submit_label\":\"Send\",\"success_message\":\"Thank you. Your response has been received.\",\"retention_days\":90,\"retention_action\":\"purge\",\"description\":\"This is a request, not a confirmed appointment. We will contact you to arrange a time.\"},\"fields\":[{\"key\":\"name\",\"label\":\"Your name\",\"kind\":\"short_text\",\"required\":true,\"autocomplete\":\"name\",\"position\":10},{\"key\":\"email\",\"label\":\"Email address\",\"kind\":\"email\",\"required\":true,\"position\":20},{\"key\":\"preferred_date\",\"label\":\"Preferred date\",\"kind\":\"date\",\"required\":true,\"position\":30},{\"key\":\"details\",\"label\":\"What would you like to discuss?\",\"kind\":\"long_text\",\"position\":40}]}" },
	{ "order", "{\"format\":\"venture-form\",\"version\":1,\"form\":{\"name\":\"Order request\",\"title\":\"Order request\",\"submit_label\":\"Send\",\"success_message\":\"Thank you. Your response has been received.\",\"retention_days\":90,\"retention_action\":\"purge\",\"description\":\"This is an order request. We will confirm availability and payment details before accepting it.\"},\"fields\":[{\"key\":\"name\",\"label\":\"Your name\",\"kind\":\"short_text\",\"required\":true,\"autocomplete\":\"name\",\"position\":10},{\"key\":\"email\",\"label\":\"Email address\",\"kind\":\"email\",\"required\":true,\"position\":20},{\"key\":\"item\",\"label\":\"Requested item\",\"kind\":\"short_text\",\"required\":true,\"position\":30},{\"key\":\"quantity\",\"label\":\"Quantity\",\"kind\":\"number\",\"required\":true,\"min_value\":1,\"max_value\":1000,\"position\":40}]}" },
};

JsonNode *venture_forms_templates(void)
{
	JsonNode *result = json_node_new(JSON_NODE_ARRAY);
	JsonArray *array = json_array_new();
	guint i;
	for (i = 0; i < G_N_ELEMENTS(templates); i++)
	{
		JsonObject *item = json_object_new();
		json_object_set_string_member(item, "name", templates[i].name);
		json_object_set_member(item, "definition", json_from_string(templates[i].definition, NULL));
		json_array_add_object_element(array, item);
	}
	json_node_take_array(result, array); return result;
}
