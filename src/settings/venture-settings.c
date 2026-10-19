/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * venture-settings.c - The settings registry (docs/settings.org)
 *
 * Nothing is stored here. A registration names the record and the field
 * that already hold a setting; reading converts that field to text, and
 * writing validates the text, puts it in the field and saves the record
 * with venture_database_save(), whose audit entry is the one entry a
 * change makes. The assistant's path stages the same record instead, so
 * the change it proposes is byte for byte the change a person makes on
 * the screen.
 */
#include "venture.h"

#include <string.h>

static GPtrArray *registry;
/* Saves by the settings API in progress on this thread. The database is
 * only ever written from the main thread, so the count is never shared. */
static GPrivate writing_depth;
static GMutex registry_lock;

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

static gboolean refuse(GError **error, gint code, const gchar *message)
{
	g_set_error_literal(error, VENTURE_ERROR, code, message);
	return FALSE;
}

/* --- The registry ------------------------------------------------------------ */

static void add_locked(const VentureSettingInfo *info)
{
	guint i;
	if (registry == NULL)
		registry = g_ptr_array_new();
	for (i = 0; i < registry->len; i++) {
		const VentureSettingInfo *known = g_ptr_array_index(registry, i);
		if (known == info || g_strcmp0(known->key, info->key) == 0)
			return;
	}
	g_ptr_array_add(registry, (gpointer)info);
}
/* Each feature registers its own settings; this is the one place that asks
 * them to, the way the mail preview asks for its kinds. Workflows appear on
 * the screen in this order. */
static void ensure_registered(void)
{
	static gsize done;
	if (g_once_init_enter(&done)) {
		venture_mail_preview_settings_register();
		venture_email_design_settings_register();
		venture_marketing_settings_register();
		venture_lead_ai_score_settings_register();
		venture_telephony_settings_register();
		venture_inbox_settings_register();
		venture_ai_messages_settings_register();
		g_once_init_leave(&done, 1);
	}
}
void venture_settings_register(const VentureSettingInfo *info)
{
	g_return_if_fail(info != NULL && info->key != NULL && info->field != NULL);
	g_mutex_lock(&registry_lock);
	add_locked(info);
	g_mutex_unlock(&registry_lock);
}
GPtrArray *venture_settings_registered(void)
{
	GPtrArray *copy = g_ptr_array_new();
	guint i;
	ensure_registered();
	g_mutex_lock(&registry_lock);
	for (i = 0; registry != NULL && i < registry->len; i++)
		g_ptr_array_add(copy, g_ptr_array_index(registry, i));
	g_mutex_unlock(&registry_lock);
	return copy;
}
const VentureSettingInfo *venture_settings_lookup(const gchar *key, gint64 *member)
{
	const VentureSettingInfo *found = NULL;
	guint i;
	if (member != NULL) *member = 0;
	if (key == NULL) return NULL;
	ensure_registered();
	g_mutex_lock(&registry_lock);
	for (i = 0; registry != NULL && i < registry->len && found == NULL; i++) {
		const VentureSettingInfo *info = g_ptr_array_index(registry, i);
		gsize length = strlen(info->key);
		if (g_strcmp0(info->key, key) == 0)
			found = info;
		else if (info->members != NULL && strncmp(key, info->key, length) == 0 && key[length] == '.') {
			gint64 id = 0;
			if (g_ascii_string_to_signed(key + length + 1, 10, 1, G_MAXINT64, &id, NULL)) {
				found = info;
				if (member != NULL) *member = id;
			}
		}
	}
	g_mutex_unlock(&registry_lock);
	return found;
}

/* --- Values as text -------------------------------------------------------------- */

static gboolean parse_switch(const gchar *text, gboolean *out)
{
	static const gchar *const yes[] = { "true", "on", "yes", "1", NULL }, *const no[] = { "false", "off", "no", "0", NULL };
	guint i;
	for (i = 0; yes[i]; i++) if (g_ascii_strcasecmp(text, yes[i]) == 0) { *out = TRUE; return TRUE; }
	for (i = 0; no[i]; i++) if (g_ascii_strcasecmp(text, no[i]) == 0) { *out = FALSE; return TRUE; }
	return FALSE;
}

gboolean venture_settings_validate(const VentureSettingInfo *info, const gchar *value, gchar **normalised,
	GError **error)
{
	g_autofree gchar *text = g_strstrip(g_strdup(value != NULL ? value : ""));
	g_autofree gchar *result = NULL;
	if (normalised != NULL) *normalised = NULL;
	g_return_val_if_fail(info != NULL, FALSE);
	if (!g_utf8_validate(text, -1, NULL) || strlen(text) > 2000) {
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "%s: at most 2000 characters of text", info->label);
		return FALSE;
	}
	switch (info->type) {
	case VENTURE_SETTING_BOOLEAN: {
		gboolean on = FALSE;
		if (!parse_switch(text, &on)) {
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "%s is on or off (true or false), not \"%s\"", info->label, text);
			return FALSE;
		}
		result = g_strdup(on ? "true" : "false");
		break;
	}
	case VENTURE_SETTING_INTEGER: {
		gint64 number = 0;
		if (!g_ascii_string_to_signed(text, 10, G_MININT64, G_MAXINT64, &number, NULL)) {
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "%s is a whole number, not \"%s\"", info->label, text);
			return FALSE;
		}
		if (number < info->minimum || number > info->maximum) {
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "%s is %" G_GINT64_FORMAT " to %" G_GINT64_FORMAT,
				info->label, info->minimum, info->maximum);
			return FALSE;
		}
		result = g_strdup_printf("%" G_GINT64_FORMAT, number);
		break;
	}
	case VENTURE_SETTING_CHOICE: {
		g_auto(GStrv) choices = g_strsplit(info->choices != NULL ? info->choices : "", ",", -1);
		guint i;
		for (i = 0; choices[i] && result == NULL; i++)
			if (g_ascii_strcasecmp(choices[i], text) == 0)
				result = g_strdup(choices[i]);
		if (result == NULL) {
			g_autofree gchar *list = g_strjoinv(", ", choices);
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "%s is one of %s, not \"%s\"", info->label, list, text);
			return FALSE;
		}
		break;
	}
	case VENTURE_SETTING_TEXT:
	default:
		if (strchr(text, '\n') != NULL || strchr(text, '\r') != NULL) {
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "%s is one line", info->label);
			return FALSE;
		}
		result = g_strdup(text);
		break;
	}
	if (info->check != NULL && !info->check(result, error))
		return FALSE;
	if (normalised != NULL) *normalised = g_steal_pointer(&result);
	return TRUE;
}

/* The field's value as text: what venture_settings_validate() would make
 * of it. A missing string is the empty string. */
static gchar *field_text(VentureEntity *record, const gchar *field)
{
	GParamSpec *spec = g_object_class_find_property(G_OBJECT_GET_CLASS(record), field);
	GValue value = G_VALUE_INIT;
	gchar *text;
	g_return_val_if_fail(spec != NULL, g_strdup(""));
	g_value_init(&value, spec->value_type);
	g_object_get_property(G_OBJECT(record), field, &value);
	if (G_VALUE_HOLDS_BOOLEAN(&value)) text = g_strdup(g_value_get_boolean(&value) ? "true" : "false");
	else if (G_VALUE_HOLDS_INT64(&value)) text = g_strdup_printf("%" G_GINT64_FORMAT, g_value_get_int64(&value));
	else if (G_VALUE_HOLDS_INT(&value)) text = g_strdup_printf("%d", g_value_get_int(&value));
	else if (G_VALUE_HOLDS_STRING(&value)) text = g_strdup(g_value_get_string(&value) != NULL ? g_value_get_string(&value) : "");
	else if (G_VALUE_HOLDS_ENUM(&value)) {
		GEnumValue *nick = g_enum_get_value(G_ENUM_CLASS(g_type_class_peek(spec->value_type)), g_value_get_enum(&value));
		text = g_strdup(nick != NULL ? nick->value_nick : "");
	} else text = g_strdup("");
	g_value_unset(&value);
	return text;
}
static gboolean field_set(VentureEntity *record, const gchar *field, const gchar *text, GError **error)
{
	GParamSpec *spec = g_object_class_find_property(G_OBJECT_GET_CLASS(record), field);
	if (spec == NULL)
		return refuse(error, VENTURE_ERROR_FAILED, "The setting's field does not exist");
	if (spec->value_type == G_TYPE_BOOLEAN) {
		gboolean on = FALSE;
		parse_switch(text, &on);
		g_object_set(record, field, on, NULL);
	} else if (spec->value_type == G_TYPE_INT64) {
		g_object_set(record, field, (gint64)g_ascii_strtoll(text, NULL, 10), NULL);
	} else if (spec->value_type == G_TYPE_INT) {
		g_object_set(record, field, (gint)g_ascii_strtoll(text, NULL, 10), NULL);
	} else if (spec->value_type == G_TYPE_STRING) {
		/* Empty is stored as nothing, the way a field never set reads, so
		 * a reset and a fresh install hold the same and every change from
		 * the default has the same "from". */
		g_object_set(record, field, (text != NULL && *text) ? text : NULL, NULL);
	} else if (G_TYPE_IS_ENUM(spec->value_type)) {
		GEnumValue *nick = g_enum_get_value_by_nick(G_ENUM_CLASS(g_type_class_peek(spec->value_type)), text);
		if (nick == NULL)
			return refuse(error, VENTURE_ERROR_VALIDATION, "That value is not one this field holds");
		g_object_set(record, field, nick->value, NULL);
	} else
		return refuse(error, VENTURE_ERROR_FAILED, "The setting's field cannot hold text");
	return TRUE;
}

/* --- Reading ----------------------------------------------------------------- */

static gboolean module_on(VentureContext *context, const VentureSettingInfo *info)
{
	return info->module == NULL || venture_context_module_enabled(context, info->module);
}
static VentureEntity *organization_record(VentureContext *context, gint64 organization_id, GError **error)
{
	g_autoptr(VentureAccessScope) internal = venture_access_policy_enter(
		venture_database_get_access_policy(venture_context_get_database(context)), NULL);
	VentureEntity *org = venture_database_get(venture_context_get_database(context), VENTURE_TYPE_ORGANIZATION,
		organization_id, error);
	if (org != NULL && venture_entity_is_deleted(org))
		g_clear_object(&org);
	if (org == NULL && (error == NULL || *error == NULL))
		refuse(error, VENTURE_ERROR_NOT_FOUND, "Organization not found");
	return org;
}
static VentureEntity *locate(VentureContext *context, const VentureSettingInfo *info, gint64 organization_id,
	gint64 member, gboolean for_write, GError **error)
{
	if (info->locate != NULL)
		return info->locate(context, organization_id, member, for_write, error);
	return organization_record(context, organization_id, error);
}
static const VentureSettingInfo *resolve(VentureContext *context, const gchar *key, gint64 *member, GError **error)
{
	const VentureSettingInfo *info = venture_settings_lookup(key, member);
	if (info == NULL || (info->members != NULL && *member == 0)) {
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "There is no setting called %s", key != NULL ? key : "");
		return NULL;
	}
	if (!module_on(context, info)) {
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "%s belongs to a module that is switched off", key);
		return NULL;
	}
	return info;
}
static VentureSettingValue *value_from(const VentureSettingInfo *info, gint64 member, const gchar *member_label,
	VentureEntity *record)
{
	VentureSettingValue *value = g_new0(VentureSettingValue, 1);
	value->info = info;
	value->member = member;
	value->key = member > 0 ? g_strdup_printf("%s.%" G_GINT64_FORMAT, info->key, member) : g_strdup(info->key);
	value->member_label = g_strdup(member_label);
	value->stored = record != NULL ? field_text(record, info->field) : NULL;
	if (value->stored == NULL || (info->empty_value != NULL && g_strcmp0(value->stored, info->empty_value) == 0))
		value->value = g_strdup(info->default_value);
	else
		value->value = g_strdup(value->stored);
	value->is_default = g_strcmp0(value->value, info->default_value) == 0;
	return value;
}
static gchar *member_label(VentureContext *context, const VentureSettingInfo *info, gint64 organization_id, gint64 member)
{
	g_autoptr(GPtrArray) members = NULL;
	guint i;
	if (member == 0 || info->members == NULL) return NULL;
	members = info->members(context, organization_id, NULL);
	for (i = 0; members != NULL && i < members->len; i++) {
		VentureSettingMember *one = g_ptr_array_index(members, i);
		if (one->id == member) return g_strdup(one->label);
	}
	return NULL;
}
static VentureSettingValue *read_one(VentureContext *context, const VentureSettingInfo *info, gint64 organization_id,
	gint64 member, GError **error)
{
	g_autoptr(VentureEntity) record = NULL;
	g_autoptr(GError) local = NULL;
	g_autofree gchar *label = NULL;
	record = locate(context, info, organization_id, member, FALSE, &local);
	if (local != NULL) {
		g_propagate_error(error, g_steal_pointer(&local));
		return NULL;
	}
	if (member > 0 && record == NULL) {
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "There is no setting called %s.%" G_GINT64_FORMAT, info->key, member);
		return NULL;
	}
	label = member_label(context, info, organization_id, member);
	return value_from(info, member, label, record);
}

GPtrArray *venture_settings_list(VentureContext *context, gint64 organization_id, GError **error)
{
	g_autoptr(GPtrArray) infos = venture_settings_registered();
	g_autoptr(GPtrArray) values = g_ptr_array_new_with_free_func((GDestroyNotify)venture_setting_value_free);
	g_autoptr(VentureEntity) org = NULL;
	guint i, j;
	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);
	org = organization_record(context, organization_id, error);
	if (org == NULL) return NULL;
	for (i = 0; i < infos->len; i++) {
		const VentureSettingInfo *info = g_ptr_array_index(infos, i);
		if (!module_on(context, info)) continue;
		if (info->members == NULL) {
			VentureSettingValue *value = read_one(context, info, organization_id, 0, error);
			if (value == NULL) return NULL;
			g_ptr_array_add(values, value);
			continue;
		}
		{
			g_autoptr(GPtrArray) members = info->members(context, organization_id, error);
			if (members == NULL) return NULL;
			for (j = 0; j < members->len; j++) {
				VentureSettingMember *member = g_ptr_array_index(members, j);
				g_autoptr(VentureEntity) record = locate(context, info, organization_id, member->id, FALSE, NULL);
				if (record != NULL)
					g_ptr_array_add(values, value_from(info, member->id, member->label, record));
			}
		}
	}
	return g_steal_pointer(&values);
}

VentureSettingValue *venture_settings_get(VentureContext *context, gint64 organization_id, const gchar *key,
	GError **error)
{
	const VentureSettingInfo *info;
	g_autoptr(VentureEntity) org = NULL;
	gint64 member = 0;
	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);
	if ((info = resolve(context, key, &member, error)) == NULL) return NULL;
	if ((org = organization_record(context, organization_id, error)) == NULL) return NULL;
	return read_one(context, info, organization_id, member, error);
}

/* --- Who may change it ------------------------------------------------------------ */

gboolean venture_settings_require_manager(VentureDatabase *database, const VentureSettingInfo *info,
	gint64 organization_id, GError **error)
{
	static const gint managers[] = { VENTURE_ORGANIZATION_ROLE_OWNER, VENTURE_ORGANIZATION_ROLE_ADMIN };
	VentureAccessPolicy *policy;
	const VentureAuthPrincipal *actor;
	gsize count;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), FALSE);
	policy = venture_database_get_access_policy(database);
	actor = venture_access_policy_get_actor(policy);
	if (actor == NULL)
		return TRUE;
	count = (info != NULL && info->permission == VENTURE_SETTING_PERMISSION_OWNER) ? 1 : G_N_ELEMENTS(managers);
	if (venture_access_policy_has_organization_role(policy, actor, organization_id, managers, count))
		return TRUE;
	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED, "Changing %s needs %s of the organization",
		info != NULL ? info->label : "a setting", count == 1 ? "an owner" : "an owner or administrator");
	return FALSE;
}

/* --- Changing it ------------------------------------------------------------------ */

/* The one preparation the direct and the staged paths share: everything
 * up to the save. @record is left holding the change; @original, when
 * asked for, is a copy of it as it stood. Returns FALSE with no error when
 * nothing would change. */
static gboolean prepare(VentureContext *context, gint64 organization_id, const gchar *key, const gchar *value,
	gboolean reset, const VentureSettingInfo **out_info, gint64 *out_member, VentureEntity **out_record,
	VentureEntity **out_original, GError **error)
{
	VentureDatabase *database = venture_context_get_database(context);
	const VentureSettingInfo *info;
	g_autoptr(VentureEntity) org = NULL, record = NULL;
	g_autoptr(GError) local = NULL;
	g_autofree gchar *normal = NULL, *now = NULL, *target = NULL;
	gint64 member = 0;
	*out_record = NULL;
	if (out_original != NULL) *out_original = NULL;
	if ((info = resolve(context, key, &member, error)) == NULL) return FALSE;
	if ((org = organization_record(context, organization_id, error)) == NULL) return FALSE;
	if (!reset && !venture_settings_validate(info, value, &normal, error)) return FALSE;
	if (!venture_settings_require_manager(database, info, organization_id, error)) return FALSE;
	record = locate(context, info, organization_id, member, TRUE, &local);
	if (record == NULL) {
		if (local != NULL) g_propagate_error(error, g_steal_pointer(&local));
		else g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "There is no setting called %s", key);
		return FALSE;
	}
	if (venture_entity_get_organization_id(record) != organization_id && !VENTURE_IS_ORGANIZATION(record)) {
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "There is no setting called %s", key);
		return FALSE;
	}
	*out_info = info;
	*out_member = member;
	target = reset ? g_strdup(info->empty_value != NULL ? info->empty_value : info->default_value) : g_steal_pointer(&normal);
	now = field_text(record, info->field);
	/* The value already in force: nothing to write, nothing to audit. */
	if (g_strcmp0(now, target) == 0 && venture_entity_is_persisted(record))
		return FALSE;
	if (!reset && info->empty_value != NULL && g_strcmp0(now, info->empty_value) == 0 &&
		g_strcmp0(target, info->default_value) == 0 && venture_entity_is_persisted(record))
		return FALSE;
	if (out_original != NULL && venture_entity_is_persisted(record)) {
		VentureEntity *copy = g_object_new(G_OBJECT_TYPE(record), NULL);
		venture_entity_copy_properties_from(copy, record, FALSE);
		*out_original = copy;
	}
	if (!field_set(record, info->field, target, error)) {
		if (out_original != NULL) g_clear_object(out_original);
		return FALSE;
	}
	*out_record = g_steal_pointer(&record);
	return TRUE;
}

static VentureSettingValue *change(VentureContext *context, gint64 organization_id, const gchar *key,
	const gchar *value, gboolean reset, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) record = NULL;
	g_autoptr(GError) local = NULL;
	const VentureSettingInfo *info = NULL;
	gint64 member = 0;
	gboolean saved;
	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);
	if (!prepare(context, organization_id, key, value, reset, &info, &member, &record, NULL, &local)) {
		if (local != NULL) {
			g_propagate_error(error, g_steal_pointer(&local));
			return NULL;
		}
		return venture_settings_get(context, organization_id, key, error);
	}
	venture_settings_write_begin();
	saved = venture_database_save(venture_context_get_database(context), record, actor, error);
	venture_settings_write_end();
	if (!saved)
		return NULL;
	return venture_settings_get(context, organization_id, key, error);
}

VentureSettingValue *venture_settings_set(VentureContext *context, gint64 organization_id, const gchar *key,
	const gchar *value, const VentureActor *actor, GError **error)
{
	return change(context, organization_id, key, value, FALSE, actor, error);
}

VentureSettingValue *venture_settings_reset(VentureContext *context, gint64 organization_id, const gchar *key,
	const VentureActor *actor, GError **error)
{
	return change(context, organization_id, key, NULL, TRUE, actor, error);
}

VentureConfirmation *venture_settings_stage(VentureContext *context, gint64 organization_id, const gchar *key,
	const gchar *value, const VentureActor *origin, GError **error)
{
	g_autoptr(VentureEntity) record = NULL, original = NULL;
	g_autoptr(GError) local = NULL;
	g_autofree gchar *via = NULL;
	const VentureSettingInfo *info = NULL;
	gint64 member = 0;
	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);
	if (!prepare(context, organization_id, key, value, value == NULL, &info, &member, &record, &original, &local)) {
		if (local != NULL)
			g_propagate_error(error, g_steal_pointer(&local));
		else
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "%s already is that; nothing to change", key);
		return NULL;
	}
	via = member > 0 ? g_strdup_printf("settings:%s.%" G_GINT64_FORMAT, info->key, member) : g_strdup_printf("settings:%s", info->key);
	return venture_confirmation_store_stage(venture_context_get_confirmations(context),
		original != NULL ? VENTURE_AUDIT_ACTION_UPDATE : VENTURE_AUDIT_ACTION_CREATE, record, original, origin, via, error);
}

gboolean venture_settings_check_approval(VentureDatabase *database, VentureEntity *staged, const gchar *via,
	GError **error)
{
	const VentureSettingInfo *info;
	gint64 organization_id;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), FALSE);
	if (via == NULL || !g_str_has_prefix(via, "settings:"))
		return TRUE;
	info = venture_settings_lookup(via + strlen("settings:"), NULL);
	organization_id = VENTURE_IS_ORGANIZATION(staged) ? venture_entity_get_id(staged) : venture_entity_get_organization_id(staged);
	return venture_settings_require_manager(database, info, organization_id, error);
}

void venture_settings_write_begin(void)
{
	g_private_set(&writing_depth, GINT_TO_POINTER(GPOINTER_TO_INT(g_private_get(&writing_depth)) + 1));
}
void venture_settings_write_end(void)
{
	gint depth = GPOINTER_TO_INT(g_private_get(&writing_depth));
	g_private_set(&writing_depth, GINT_TO_POINTER(depth > 0 ? depth - 1 : 0));
}
gboolean venture_settings_writing(void)
{
	return GPOINTER_TO_INT(g_private_get(&writing_depth)) > 0;
}
gboolean venture_settings_only_change(VentureEntity *previous, VentureEntity *row, const gchar *field)
{
	g_autoptr(JsonNode) diff = NULL;
	g_autofree gchar *wire = NULL;
	JsonObject *object;
	if (previous == NULL || row == NULL || G_OBJECT_TYPE(previous) != G_OBJECT_TYPE(row))
		return FALSE;
	diff = venture_entity_diff(previous, row);
	if (diff == NULL || !JSON_NODE_HOLDS_OBJECT(diff))
		return FALSE;
	object = json_node_get_object(diff);
	wire = g_strdelimit(g_strdup(field), "-", '_');
	return json_object_get_size(object) == 1 && (json_object_has_member(object, wire) || json_object_has_member(object, field));
}

/* --- The wire form ------------------------------------------------------------------ */

static void add_typed(JsonBuilder *builder, const VentureSettingInfo *info, const gchar *text)
{
	gboolean on = FALSE;
	if (text == NULL)
		json_builder_add_null_value(builder);
	else if (info->type == VENTURE_SETTING_BOOLEAN && parse_switch(text, &on))
		json_builder_add_boolean_value(builder, on);
	else if (info->type == VENTURE_SETTING_INTEGER && *text)
		json_builder_add_int_value(builder, g_ascii_strtoll(text, NULL, 10));
	else
		json_builder_add_string_value(builder, text);
}

JsonNode *venture_setting_value_to_json(const VentureSettingValue *value)
{
	static const gchar *const types[] = { "boolean", "integer", "text", "choice" };
	const VentureSettingInfo *info;
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_return_val_if_fail(value != NULL && value->info != NULL, NULL);
	info = value->info;
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "key");
	json_builder_add_string_value(builder, value->key);
	json_builder_set_member_name(builder, "workflow");
	json_builder_add_string_value(builder, info->workflow);
	json_builder_set_member_name(builder, "label");
	json_builder_add_string_value(builder, info->label);
	if (value->member_label != NULL) {
		json_builder_set_member_name(builder, "member");
		json_builder_add_string_value(builder, value->member_label);
	}
	json_builder_set_member_name(builder, "help");
	json_builder_add_string_value(builder, info->help);
	json_builder_set_member_name(builder, "type");
	json_builder_add_string_value(builder, types[info->type]);
	json_builder_set_member_name(builder, "value");
	add_typed(builder, info, value->value);
	json_builder_set_member_name(builder, "default");
	add_typed(builder, info, info->default_value);
	json_builder_set_member_name(builder, "is_default");
	json_builder_add_boolean_value(builder, value->is_default);
	if (info->type == VENTURE_SETTING_INTEGER) {
		json_builder_set_member_name(builder, "minimum");
		json_builder_add_int_value(builder, info->minimum);
		json_builder_set_member_name(builder, "maximum");
		json_builder_add_int_value(builder, info->maximum);
	}
	if (info->type == VENTURE_SETTING_CHOICE && info->choices != NULL) {
		g_auto(GStrv) choices = g_strsplit(info->choices, ",", -1);
		guint i;
		json_builder_set_member_name(builder, "choices");
		json_builder_begin_array(builder);
		for (i = 0; choices[i]; i++) json_builder_add_string_value(builder, choices[i]);
		json_builder_end_array(builder);
	}
	json_builder_set_member_name(builder, "permission");
	json_builder_add_string_value(builder, info->permission == VENTURE_SETTING_PERMISSION_OWNER ? "owner" : "owner or administrator");
	json_builder_set_member_name(builder, "stored_in");
	json_builder_add_string_value(builder, info->stored_in);
	json_builder_end_object(builder);
	return json_builder_get_root(builder);
}

gchar *venture_settings_value_from_json(JsonNode *node)
{
	if (node == NULL || !JSON_NODE_HOLDS_VALUE(node)) return NULL;
	switch (json_node_get_value_type(node)) {
	case G_TYPE_BOOLEAN: return g_strdup(json_node_get_boolean(node) ? "true" : "false");
	case G_TYPE_INT64: return g_strdup_printf("%" G_GINT64_FORMAT, json_node_get_int(node));
	case G_TYPE_STRING: return g_strdup(json_node_get_string(node));
	default: return NULL;
	}
}
