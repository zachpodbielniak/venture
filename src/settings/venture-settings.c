/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * venture-settings.c - The settings registry (docs/settings.org)
 */
#include "venture.h"

#include <string.h>

VentureSettingMember *venture_setting_member_new(gint64 id, const gchar *label)
{
	VentureSettingMember *member = g_new0(VentureSettingMember, 1);
	member->id = id;
	member->label = g_strdup(label);
	return member;
}
void venture_setting_member_free(VentureSettingMember *member)
{
	if (member == NULL) return;
	g_free(member->label);
	g_free(member);
}
void venture_setting_value_free(VentureSettingValue *value)
{
	if (value == NULL) return;
	g_free(value->key);
	g_free(value->member_label);
	g_free(value->value);
	g_free(value->stored);
	g_free(value);
}
void venture_settings_register(const VentureSettingInfo *info) { (void)info; }
GPtrArray *venture_settings_registered(void) { return g_ptr_array_new(); }
const VentureSettingInfo *venture_settings_lookup(const gchar *key, gint64 *member) { (void)key; if (member) *member = 0; return NULL; }
gboolean venture_settings_validate(const VentureSettingInfo *info, const gchar *value, gchar **normalised, GError **error)
{
	(void)info; (void)value; if (normalised) *normalised = NULL;
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_UNSUPPORTED, "not yet");
	return FALSE;
}
GPtrArray *venture_settings_list(VentureContext *context, gint64 organization_id, GError **error)
{
	(void)context; (void)organization_id; (void)error;
	return g_ptr_array_new_with_free_func((GDestroyNotify)venture_setting_value_free);
}
VentureSettingValue *venture_settings_get(VentureContext *context, gint64 organization_id, const gchar *key, GError **error)
{
	(void)context; (void)organization_id; (void)key;
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_UNSUPPORTED, "not yet");
	return NULL;
}
gboolean venture_settings_require_manager(VentureDatabase *database, const VentureSettingInfo *info, gint64 organization_id, GError **error)
{
	(void)database; (void)info; (void)organization_id;
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_UNSUPPORTED, "not yet");
	return FALSE;
}
VentureSettingValue *venture_settings_set(VentureContext *context, gint64 organization_id, const gchar *key,
	const gchar *value, const VentureActor *actor, GError **error)
{
	(void)context; (void)organization_id; (void)key; (void)value; (void)actor;
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_UNSUPPORTED, "not yet");
	return NULL;
}
VentureSettingValue *venture_settings_reset(VentureContext *context, gint64 organization_id, const gchar *key,
	const VentureActor *actor, GError **error)
{
	(void)context; (void)organization_id; (void)key; (void)actor;
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_UNSUPPORTED, "not yet");
	return NULL;
}
VentureConfirmation *venture_settings_stage(VentureContext *context, gint64 organization_id, const gchar *key,
	const gchar *value, const VentureActor *origin, GError **error)
{
	(void)context; (void)organization_id; (void)key; (void)value; (void)origin;
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_UNSUPPORTED, "not yet");
	return NULL;
}
gboolean venture_settings_check_approval(VentureDatabase *database, VentureEntity *staged, const gchar *via, GError **error)
{
	(void)database; (void)staged; (void)via; (void)error;
	return TRUE;
}
JsonNode *venture_setting_value_to_json(const VentureSettingValue *value)
{
	(void)value;
	return json_node_new(JSON_NODE_NULL);
}
gchar *venture_settings_value_from_json(JsonNode *node) { (void)node; return NULL; }
