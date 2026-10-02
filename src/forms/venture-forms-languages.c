/*
 * venture-forms-languages.c - Versioned, plain-text form translations
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "venture-forms-private.h"
#include <string.h>

/* This is a text catalog, not gettext or executable formatting supplied by
 * an author. A translation can never change a stable key, choice id or rule. */
typedef struct { const gchar *key; const gchar *text; } FormsMessage;
static const FormsMessage messages[] = {
	{ "row_marker", "This row marker is not valid." },
	{ "group_heading", "A group heading does not accept an answer." },
	{ "page_heading", "A page heading does not accept an answer." },
	{ "unknown_question", "This form has no such question." },
	{ "email_required", "An email address is required for this form." },
	{ "duplicate_email", "A response for this email address has already been received." },
	{ "optin_permission", "Explicit marketing permission is required to confirm this signup." },
	{ "optin_email", "A signup email address is required." },
	{ "personal_inbox", "Use the inbox addressed by this personal signup link." },
	{ "current_page", "Only questions on the current page may be answered." },
	{ "row_numbers", "Use consecutive row numbers within the group limit." },
	{ "row_consecutive", "Rows must be consecutive, starting at zero." },
	{ "rows_excess", "Too many rows for this group." },
	{ "rows_minimum", "Add the minimum number of rows for this group." },
	{ "rows_maximum", "The maximum number of rows has been reached." },
	{ "rows_keep", "Keep the minimum number of rows for this group." },
	{ "row_operation", "This row operation does not belong to the current page." },
	{ "required", "This question needs an answer." },
	{ "email", "Enter an email address, like name@example.com." },
	{ "phone", "Enter a phone number using digits, spaces, + - ( ) and dots." },
	{ "url", "Enter a web address starting with https://." },
	{ "date", "Enter a real date as YYYY-MM-DD." },
	{ "number", "Enter a number." },
	{ "choice", "Choose from the options given." },
	{ "multiple", "Choose at least one." },
	{ "one_answer", "Give one answer only." },
	{ "checkbox", "Tick the box or leave it empty." },
	{ "consent", "This box must be ticked." },
	{ "single_line", "Keep this answer to one line." },
	{ "pattern", "This answer is not in the expected format." },
	{ "max_length", "This answer is too long." },
	{ "min_length", "This answer is too short." },
	{ "minimum", "This number is below the minimum." },
	{ "maximum", "This number is above the maximum." },
	{ "rating", "Choose a whole number in the scale." },
	{ "required_note", "Questions marked * are required." },
	{ "errors", "Please check these answers." },
	{ "next", "Next" }, { "back", "Back" }, { "send", "Send" },
	{ "page", "Page" }, { "of", "of" },
	{ "privacy", "Privacy notice" },
	{ "yes", "Yes" }, { "no", "No" }, { "given", "Given" },
	{ "add_row", "Add another" }, { "remove_row", "Remove" },
	{ "success", "Thank you. Your response has been received." },
	{ "inbox_title", "Check your inbox" },
	{ "inbox", "Open the confirmation link and press Confirm signup. If you already requested one, please allow a minute before trying again." },
	{ "confirm", "Confirm signup" },
	{ "confirm_intro", "Confirm that you want to submit these answers and receive the marketing messages you agreed to." },
	{ "optin_subject", "Confirm your signup" },
	{ "optin_intro", "Confirm your signup by opening this link and pressing Confirm signup:" },
	{ "optin_expiry", "This link expires 24 hours after the original request. If you did not request this signup, ignore this message." },
	{ "mail_subject", "We received your response" }
};

gboolean
venture_forms_language_valid(const gchar *language)
{
	guint part = 0, parts = 0;
	const gchar *p;
	if (venture_string_is_empty(language) || strlen(language) > 35) return FALSE;
	for (p = language; *p != '\0'; p++)
	{
		if (*p == '-')
		{
			if (part == 0 || part > 8 || (parts == 0 && part < 2)) return FALSE;
			parts++; part = 0;
		}
		else
		{
			if (!(parts == 0 ? g_ascii_isalpha(*p) : g_ascii_isalnum(*p))) return FALSE;
			part++;
		}
	}
	return part > 0 && part <= 8 && (parts > 0 || part >= 2);
}

JsonObject *
venture_forms_catalog(GPtrArray *fields)
{
	return fields != NULL && fields->len > 0 ? ((VentureFormsField *)g_ptr_array_index(fields, 0))->catalog : NULL;
}

void
venture_forms_catalog_restore(GPtrArray *fields, JsonObject *catalog)
{
	guint i;
	for (i = 0; i < fields->len; i++)
	{
		VentureFormsField *field = g_ptr_array_index(fields, i);
		g_clear_pointer(&field->catalog, json_object_unref);
		field->catalog = json_object_ref(catalog);
	}
}

static JsonObject *
forms_catalog_base(VentureEntity *form, GPtrArray *fields)
{
	JsonObject *base = json_object_new();
	const gchar *properties[] = { "title", "description", "submit-label", "success-message", "confirmation-subject", "confirmation-message" };
	guint i, j;
	for (i = 0; i < G_N_ELEMENTS(properties); i++)
	{
		g_autofree gchar *value = venture_forms_get_string(form, properties[i]);
		g_autofree gchar *key = g_strdup_printf("form.%s", properties[i]);
		g_strdelimit(key, "-", '_');
		json_object_set_string_member(base, key, value != NULL ? value : "");
	}
	for (i = 0; i < G_N_ELEMENTS(messages); i++)
	{
		g_autofree gchar *key = g_strdup_printf("message.%s", messages[i].key);
		json_object_set_string_member(base, key, messages[i].text);
	}
	for (i = 0; i < fields->len; i++)
	{
		VentureFormsField *field = g_ptr_array_index(fields, i);
		const gchar *names[] = { "label", "help", "placeholder" };
		const gchar *values[] = { field->label, field->help, field->placeholder };
		for (j = 0; j < G_N_ELEMENTS(names); j++)
		{
			g_autofree gchar *key = g_strdup_printf("field.%s.%s", field->key, names[j]);
			json_object_set_string_member(base, key, values[j] != NULL ? values[j] : "");
		}
		for (j = 0; field->choices != NULL && j < field->choices->len; j++)
		{
			VentureFormsChoice *choice = g_ptr_array_index(field->choices, j);
			g_autofree gchar *key = g_strdup_printf("choice.%s.%s", field->key, choice->id);
			json_object_set_string_member(base, key, choice->label);
		}
		if (field->group_key != NULL)
		{
			g_autofree gchar *key = g_strdup_printf("group.%s.label", field->group_key);
			json_object_set_string_member(base, key, field->group_label);
		}
	}
	return base;
}

static const gchar *
forms_catalog_text(JsonObject *catalog, const gchar *language, const gchar *key, const gchar *fallback)
{
	JsonObject *base, *languages, *selected;
	const gchar *value;
	if (catalog == NULL) return fallback;
	base = json_object_get_object_member(catalog, "base");
	languages = json_object_get_object_member(catalog, "languages");
	selected = language != NULL && json_object_has_member(languages, language) ? json_object_get_object_member(languages, language) : NULL;
	value = selected != NULL ? json_object_get_string_member_with_default(selected, key, NULL) : NULL;
	if (venture_string_is_empty(value) && !g_strv_contains((const gchar *const[]) {
		"message.max_length", "message.min_length", "message.minimum", "message.maximum", "message.rating", NULL }, key))
		value = json_object_get_string_member_with_default(base, key, NULL);
	return venture_string_is_empty(value) ? fallback : value;
}

const gchar *
venture_forms_field_text(const VentureFormsField *field, const gchar *key, const gchar *fallback)
{
	return field != NULL ? forms_catalog_text(field->catalog, field->language, key, fallback) : fallback;
}

const gchar *
venture_forms_text(GPtrArray *fields, const gchar *key, const gchar *fallback)
{
	return fields != NULL && fields->len > 0 ? venture_forms_field_text(g_ptr_array_index(fields, 0), key, fallback) : fallback;
}

const gchar *
venture_forms_language(GPtrArray *fields)
{
	VentureFormsField *field = fields != NULL && fields->len > 0 ? g_ptr_array_index(fields, 0) : NULL;
	if (field != NULL && field->language != NULL) return field->language;
	return field != NULL && field->catalog != NULL ? json_object_get_string_member_with_default(field->catalog, "default", "en") : "en";
}

static gchar *
forms_language_match(JsonObject *catalog, const gchar *language)
{
	g_autofree gchar *candidate = NULL;
	JsonObject *languages;
	const gchar *fallback;
	if (catalog == NULL || !venture_forms_language_valid(language)) return NULL;
	candidate = g_ascii_strdown(language, -1);
	languages = json_object_get_object_member(catalog, "languages");
	fallback = json_object_get_string_member_with_default(catalog, "default", "en");
	while (TRUE)
	{
		gchar *dash;
		if (g_strcmp0(candidate, fallback) == 0 || json_object_has_member(languages, candidate)) return g_steal_pointer(&candidate);
		dash = strrchr(candidate, '-');
		if (dash == NULL) return NULL;
		*dash = '\0';
	}
}

gchar *
venture_forms_language_choose(GPtrArray *fields, const gchar *explicit_language, const gchar *accept)
{
	JsonObject *catalog = venture_forms_catalog(fields);
	g_autofree gchar *selected = forms_language_match(catalog, explicit_language);
	gdouble best = 0;
	guint i;
	if (selected != NULL) return g_steal_pointer(&selected);
	if (accept != NULL && strlen(accept) <= 4096)
	{
		g_auto(GStrv) ranges = g_strsplit(accept, ",", 33);
		for (i = 0; ranges[i] != NULL && i < 32; i++)
		{
			g_auto(GStrv) parts = g_strsplit(ranges[i], ";", 3);
			gdouble quality = 1;
			g_autofree gchar *matched = NULL;
			if (parts[1] != NULL)
			{
				gchar *end = NULL, *weight = g_strstrip(parts[1]);
				if (!g_str_has_prefix(weight, "q=") || parts[2] != NULL) continue;
				quality = g_ascii_strtod(weight + 2, &end);
				if (end == weight + 2 || *end != '\0' || !(quality >= 0 && quality <= 1)) continue;
			}
			if (quality <= best) continue;
			matched = forms_language_match(catalog, g_strstrip(parts[0]));
			if (matched != NULL) { g_free(selected); selected = g_steal_pointer(&matched); best = quality; }
		}
	}
	if (selected == NULL) selected = g_strdup(catalog != NULL ? json_object_get_string_member_with_default(catalog, "default", "en") : "en");
	return g_steal_pointer(&selected);
}

void
venture_forms_localize(GPtrArray *fields, const gchar *language)
{
	g_autofree gchar *selected = venture_forms_language_choose(fields, language, NULL);
	guint i, j;
	for (i = 0; i < fields->len; i++)
	{
		VentureFormsField *field = g_ptr_array_index(fields, i);
		const gchar *base_key = field->base_key != NULL ? field->base_key : field->key;
		g_free(field->language); field->language = g_strdup(selected);
#define LOCALIZE(member) G_STMT_START { \
	g_autofree gchar *key = g_strdup_printf("field.%s." #member, base_key); \
	gchar *text = g_strdup(venture_forms_field_text(field, key, field->member)); \
	g_free(field->member); field->member = text; } G_STMT_END
		LOCALIZE(label); LOCALIZE(help); LOCALIZE(placeholder);
#undef LOCALIZE
		for (j = 0; field->choices != NULL && j < field->choices->len; j++)
		{
			VentureFormsChoice *choice = g_ptr_array_index(field->choices, j);
			g_autofree gchar *key = g_strdup_printf("choice.%s.%s", base_key, choice->id);
			gchar *text = g_strdup(venture_forms_field_text(field, key, choice->label));
			g_free(choice->label); choice->label = text;
		}
		if (field->group_key != NULL)
		{
			g_autofree gchar *key = g_strdup_printf("group.%s.label", field->group_key);
			gchar *text = g_strdup(venture_forms_field_text(field, key, field->group_label));
			g_free(field->group_label); field->group_label = text;
		}
	}
}

gboolean
venture_forms_translations_load(VentureDatabase *database, VentureEntity *form, GPtrArray *fields, GError **error)
{
	g_autoptr(JsonObject) catalog = json_object_new();
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_FORM_TRANSLATION);
	g_autoptr(GPtrArray) rows = NULL;
	g_autofree gchar *language = venture_forms_get_string(form, "default-language");
	JsonObject *languages = json_object_new();
	guint i;
	json_object_set_string_member(catalog, "default", venture_string_is_empty(language) ? "en" : language);
	json_object_set_object_member(catalog, "base", forms_catalog_base(form, fields));
	json_object_set_object_member(catalog, "languages", languages);
	venture_query_set_organization(query, venture_entity_get_organization_id(form));
	venture_query_add_filter_int(query, "form-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(form), NULL);
	venture_query_set_limit(query, 2001);
	venture_query_add_order(query, "language", VENTURE_SORT_ASCENDING, NULL);
	venture_query_add_order(query, "text-key", VENTURE_SORT_ASCENDING, NULL);
	rows = venture_database_find(database, query, error);
	if (rows == NULL) return FALSE;
	if (rows->len > 2000) { venture_set_error_validation(error, "Translations", "at most 2000 translated texts per form"); return FALSE; }
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(rows, i);
		g_autofree gchar *lang = venture_forms_get_string(row, "language");
		g_autofree gchar *key = venture_forms_get_string(row, "text-key");
		g_autofree gchar *text = venture_forms_get_string(row, "text");
		JsonObject *translations;
		if (venture_string_is_empty(lang) || venture_string_is_empty(key)) continue;
		if (!json_object_has_member(languages, lang)) json_object_set_object_member(languages, lang, json_object_new());
		translations = json_object_get_object_member(languages, lang);
		json_object_set_string_member(translations, key, text != NULL ? text : "");
	}
	{
		g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);
		g_autofree gchar *text = NULL;
		json_node_set_object(node, catalog); text = json_to_string(node, FALSE);
		if (strlen(text) > 1024 * 1024)
		{ venture_set_error_validation(error, "Translations", "the complete catalog must fit within 1 MiB"); return FALSE; }
	}
	json_object_set_int_member(catalog, "record_count", rows->len);
	venture_forms_catalog_restore(fields, catalog);
	return TRUE;
}

/* Missing text is an advisory, never a failed save. Only bounded counts and
 * language tags enter diagnostics, not author-controlled wording. */
gboolean
venture_forms_validate_translation(VentureDatabase *database, VentureEntity *entity,
	VentureEntity *previous, gpointer data, GError **error)
{
	g_autoptr(VentureEntity) form = NULL;
	g_autoptr(GPtrArray) fields = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) duplicates = NULL;
	g_autofree gchar *language = venture_forms_get_string(entity, "language");
	g_autofree gchar *key = venture_forms_get_string(entity, "text-key");
	g_autofree gchar *text = venture_forms_get_string(entity, "text");
	g_autofree gchar *normalized = NULL, *name = NULL;
	JsonObject *catalog, *base, *languages, *selected;
	JsonObjectIter iter;
	const gchar *member;
	JsonNode *value;
	guint i, missing = 0;
	(void)previous; (void)data;
	if (!venture_forms_language_valid(language) || venture_string_is_empty(key) || strlen(key) > 160 ||
	    (text != NULL && (strlen(text) > 32768 || !g_utf8_validate(text, -1, NULL)))) goto invalid;
	normalized = g_ascii_strdown(language, -1);
	form = venture_database_get(database, VENTURE_TYPE_FORM, venture_forms_get_int(entity, "form-id"), error);
	if (form == NULL) return FALSE;
	if (venture_entity_get_organization_id(form) != venture_entity_get_organization_id(entity)) goto invalid;
	fields = venture_forms_definition_from_records(database, form, error);
	if (fields == NULL) return FALSE;
	catalog = venture_forms_catalog(fields);
	if (catalog == NULL) goto invalid;
	base = json_object_get_object_member(catalog, "base");
	if (!json_object_has_member(base, key)) goto invalid;
	if (venture_entity_get_id(entity) == 0 && json_object_get_int_member(catalog, "record_count") >= 2000)
	{ venture_set_error_validation(error, "Translations", "at most 2000 translated texts per form"); return FALSE; }
	name = g_strdup_printf("%s:%s", normalized, key);
	query = venture_query_new(VENTURE_TYPE_FORM_TRANSLATION);
	venture_query_set_organization(query, venture_entity_get_organization_id(form));
	venture_query_add_filter_int(query, "form-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(form), NULL);
	venture_query_add_filter_string(query, "language", VENTURE_FILTER_OP_EQ, normalized, NULL);
	venture_query_add_filter_string(query, "text-key", VENTURE_FILTER_OP_EQ, key, NULL);
	duplicates = venture_database_find(database, query, error);
	if (duplicates == NULL) return FALSE;
	for (i = 0; i < duplicates->len; i++)
		if (venture_entity_get_id(g_ptr_array_index(duplicates, i)) != venture_entity_get_id(entity))
		{ venture_set_error_validation(error, "Translation", "this text already has a translation in that language"); return FALSE; }
	languages = json_object_get_object_member(catalog, "languages");
	if (!json_object_has_member(languages, normalized)) json_object_set_object_member(languages, normalized, json_object_new());
	selected = json_object_get_object_member(languages, normalized);
	json_object_set_string_member(selected, key, text != NULL ? text : "");
	{
		g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);
		g_autofree gchar *encoded = NULL;
		json_node_set_object(node, catalog); encoded = json_to_string(node, FALSE);
		if (strlen(encoded) > 1024 * 1024)
		{ venture_set_error_validation(error, "Translations", "the complete catalog must fit within 1 MiB"); return FALSE; }
	}
	venture_forms_localize(fields, normalized);
	if (!venture_forms_piping_definition(fields, venture_forms_text(fields, "form.success_message", ""), error) ||
	    !venture_forms_piping_definition(fields, venture_forms_text(fields, "form.confirmation_subject", ""), error) ||
	    !venture_forms_piping_definition(fields, venture_forms_text(fields, "form.confirmation_message", ""), error)) return FALSE;
	if ((g_str_has_suffix(key, "subject") || g_strcmp0(key, "message.optin_subject") == 0) &&
	    text != NULL && (strchr(text, '\r') != NULL || strchr(text, '\n') != NULL))
	{ venture_set_error_validation(error, "Translation", "an email subject must be one line"); return FALSE; }
	json_object_iter_init(&iter, base);
	while (json_object_iter_next(&iter, &member, &value))
		if (!venture_string_is_empty(json_node_get_string(value)) &&
		    venture_string_is_empty(json_object_get_string_member_with_default(selected, member, NULL))) missing++;
	if (missing > 0) g_message("Form translation %s uses default wording for %u texts", normalized, missing);
	g_object_set(entity, "name", name, "language", normalized, NULL);
	return TRUE;
invalid:
	venture_set_error_validation(error, "Translation", "use a valid language tag and an existing public text key in this form");
	return FALSE;
}

gchar *
venture_forms_language_from_values(VentureEntity *form, VentureEntity *version, JsonObject *values)
{
	g_autoptr(JsonObject) seed = NULL;
	const gchar *packed;
	if (values == NULL) return NULL;
	packed = json_object_get_string_member_with_default(values, VENTURE_FORMS_PREFILL, NULL);
	seed = venture_forms_prefill_unpack(form, version, packed, NULL);
	return seed != NULL ? g_strdup(json_object_get_string_member_with_default(seed, VENTURE_FORMS_LANGUAGE, NULL)) : NULL;
}

void
venture_forms_translation_errors(GPtrArray *fields, JsonObject *errors)
{
	JsonObjectIter iter;
	const gchar *key;
	JsonNode *node;
	guint i;
	json_object_iter_init(&iter, errors);
	while (json_object_iter_next(&iter, &key, &node))
	{
		const gchar *text = json_node_get_string(node);
		(void)key;
		for (i = 0; i < G_N_ELEMENTS(messages); i++)
			if (g_strcmp0(text, messages[i].text) == 0)
			{
				g_autofree gchar *name = g_strdup_printf("message.%s", messages[i].key);
				g_autofree gchar *translated = g_strdup(venture_forms_text(fields, name, text));
				json_node_set_string(node, translated);
				break;
			}
	}
}

GPtrArray *
venture_forms_record_definition(VentureDatabase *database, VentureEntity *form, VentureEntity *record, GError **error)
{
	g_autoptr(VentureEntity) version = NULL;
	g_autoptr(GPtrArray) fields = NULL;
	g_autofree gchar *language = NULL;
	version = venture_database_get(database, VENTURE_TYPE_FORM_VERSION, venture_forms_get_int(record, "version-id"), error);
	if (version == NULL) return NULL;
	fields = venture_forms_definition_for(database, form, version, error);
	if (fields == NULL) return NULL;
	if (VENTURE_IS_FORM_SUBMISSION(record)) language = venture_forms_get_string(record, "language");
	else
	{
		g_autofree gchar *text = venture_forms_get_string(record, "answers");
		g_autoptr(JsonNode) root = venture_string_is_empty(text) ? NULL : json_from_string(text, NULL);
		if (root != NULL && JSON_NODE_HOLDS_OBJECT(root))
			language = venture_forms_language_from_values(form, version, json_node_get_object(root));
	}
	venture_forms_localize(fields, language);
	return g_steal_pointer(&fields);
}

/* Validate translated templates against the prospective draft as well: a
 * source becoming sensitive must not leave an old translated reference usable.
 * Stale destination keys can remain for historical versions and are ignored. */
static gboolean
forms_catalog_check_texts(GPtrArray *fields, JsonObject *texts, GError **error)
{
	JsonObjectIter iter;
	const gchar *key;
	JsonNode *node;
	json_object_iter_init(&iter, texts);
	while (json_object_iter_next(&iter, &key, &node))
	{
		const VentureFormsField *target = NULL;
		if (g_str_has_prefix(key, "field.") && (g_str_has_suffix(key, ".label") || g_str_has_suffix(key, ".help")))
		{
			const gchar *end = strrchr(key, '.');
			g_autofree gchar *name = g_strndup(key + 6, (gsize)(end - key - 6));
			target = venture_forms_definition_find(fields, name);
			if (target == NULL) continue;
		}
		else if (!g_strv_contains((const gchar *const[]) { "form.success_message", "form.confirmation_subject", "form.confirmation_message", NULL }, key)) continue;
		if (!venture_forms_pipe_validate_text(json_node_get_string(node), fields, target, error)) return FALSE;
	}
	return TRUE;
}

gboolean
venture_forms_translations_check(GPtrArray *fields, GError **error)
{
	JsonObject *catalog = venture_forms_catalog(fields);
	JsonObjectIter iter;
	const gchar *language;
	JsonNode *node;
	if (catalog == NULL) return TRUE;
	if (!forms_catalog_check_texts(fields, json_object_get_object_member(catalog, "base"), error)) return FALSE;
	json_object_iter_init(&iter, json_object_get_object_member(catalog, "languages"));
	while (json_object_iter_next(&iter, &language, &node))
	{
		(void)language;
		if (!forms_catalog_check_texts(fields, json_node_get_object(node), error)) return FALSE;
	}
	return TRUE;
}
