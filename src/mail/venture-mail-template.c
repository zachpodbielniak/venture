/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <string.h>
static gchar *render(const gchar *format, JsonObject *values, gboolean html, GError **error)
{
	GString *out = g_string_new(NULL);
	const gchar *p = format ? format : "";
	while (*p) {
		const gchar *end;
		g_autofree gchar *key = NULL;
		g_autofree gchar *value = NULL;
		JsonNode *node;
		if (*p != '{') { g_string_append_c(out, *p++); continue; }
		end = strchr(p + 1, '}');
		if (!end) goto invalid;
		key = g_strndup(p + 1, end - p - 1);
		g_strdelimit(key, "-", '_');
		node = json_object_get_member(values, key);
		if (!node) goto invalid;
		if (JSON_NODE_HOLDS_NULL(node)) value = g_strdup("");
		else if (json_node_get_value_type(node) == G_TYPE_STRING) value = g_strdup(json_node_get_string(node));
		else value = json_to_string(node, FALSE);
		if (html) {
			g_autofree gchar *escaped = g_markup_escape_text(value, -1);
			g_string_append(out, escaped);
		} else g_string_append(out, value);
		p = end + 1;
	}
	return g_string_free(out, FALSE);
invalid:
	g_string_free(out, TRUE);
	venture_set_error_validation(error, "template", "Unknown, sensitive or malformed placeholder");
	return NULL;
}
VentureMailMessage *venture_mail_template_render(VentureMailTemplate *self, VentureEntity *record, GError **error)
{
	g_autoptr(JsonNode) values = NULL;
	g_autoptr(VentureMailMessage) message = venture_mail_message_new();
	const gchar *fields[] = { "subject", "text-body", "html-body" };
	guint i;
	if (venture_entity_get_organization_id(VENTURE_ENTITY(self)) != venture_entity_get_organization_id(record)) {
		venture_set_error_validation(error, "template", "Template and record must belong to the same organization");
		return NULL;
	}
	values = venture_serializable_to_json(VENTURE_SERIALIZABLE(record), FALSE);
	for (i = 0; i < G_N_ELEMENTS(fields); i++) {
		g_autofree gchar *format = NULL;
		g_autofree gchar *result = NULL;
		g_object_get(self, fields[i], &format, NULL);
		result = render(format, json_node_get_object(values), i == 2, error);
		if (!result) return NULL;
		g_object_set(message, fields[i], result, NULL);
	}
	venture_entity_set_organization_id(VENTURE_ENTITY(message), venture_entity_get_organization_id(record));
	return g_steal_pointer(&message);
}
