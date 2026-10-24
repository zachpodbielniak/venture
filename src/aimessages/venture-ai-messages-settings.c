/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * venture-ai-messages-settings.c - AI on messages settings
 * (docs/ai-messages.org, docs/settings.org)
 *
 * The switch and business hours are fields on the organization; each
 * auto-reply rule's auto-send is on the rule.
 */
#include "venture.h"

static gboolean hours(const gchar *value, GError **error)
{
	g_autoptr(GTimeZone) zone = g_time_zone_new_utc();
	g_autoptr(GDateTime) now = g_date_time_new_now_utc();
	gboolean open = FALSE;
	if (*value == '\0')
		return TRUE;
	return venture_ai_messages_business_hours_open(value, zone, now, &open, error);
}
static GPtrArray *rules(VentureContext *context, gint64 organization_id, GError **error)
{
	VentureDatabase *database = venture_context_get_database(context);
	g_autoptr(VentureAccessScope) internal = venture_access_policy_enter(venture_database_get_access_policy(database), NULL);
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_AI_MESSAGE_RULE);
	g_autoptr(GPtrArray) rows = NULL;
	GPtrArray *members;
	guint i;
	venture_query_set_organization(query, organization_id);
	venture_query_add_order(query, "position", VENTURE_SORT_ASCENDING, NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	rows = venture_database_find(database, query, error);
	if (rows == NULL)
		return NULL;
	members = g_ptr_array_new_with_free_func((GDestroyNotify)venture_setting_member_free);
	for (i = 0; i < rows->len; i++) {
		VentureEntity *row = g_ptr_array_index(rows, i);
		g_autofree gchar *name = NULL;
		g_object_get(row, "name", &name, NULL);
		g_ptr_array_add(members, venture_setting_member_new(venture_entity_get_id(row), name));
	}
	return members;
}
static VentureEntity *rule(VentureContext *context, gint64 organization_id, gint64 member, gboolean for_write, GError **error)
{
	VentureDatabase *database = venture_context_get_database(context);
	g_autoptr(VentureAccessScope) internal = venture_access_policy_enter(venture_database_get_access_policy(database), NULL);
	VentureEntity *row = venture_database_get(database, VENTURE_TYPE_AI_MESSAGE_RULE, member, NULL);
	(void)for_write; (void)error;
	if (row != NULL && (venture_entity_is_deleted(row) || venture_entity_get_organization_id(row) != organization_id))
		g_clear_object(&row);
	return row;
}

static const VentureSettingInfo messages[] = {
	{ "ai-messages.off", "AI on messages", "ai_messages", "Switch AI messages off",
	  "The one switch: while it is on, nothing is read, drafted, summarised or sent by the AI, and no rule answers.",
	  VENTURE_SETTING_BOOLEAN, "false", 0, 0, NULL, VENTURE_SETTING_PERMISSION_ADMIN,
	  "organization: ai-messages-off", "ai-messages-off", NULL, NULL, NULL, NULL },
	{ "ai-messages.business-hours", "AI on messages", "ai_messages", "Business hours",
	  "When you are open, in the organization's time zone, for example Mon-Fri 09:00-17:00; Sat 10:00-14:00. "
	  "After-hours rules answer outside them; empty means no after-hours replies.",
	  VENTURE_SETTING_TEXT, "", 0, 0, NULL, VENTURE_SETTING_PERMISSION_ADMIN,
	  "organization: business-hours", "business-hours", "", NULL, NULL, hours },
	{ "ai-messages.auto-send", "AI on messages", "ai_messages", "Send without asking",
	  "This auto-reply rule sends on its own instead of leaving a draft for a person to approve. Off by default.",
	  VENTURE_SETTING_BOOLEAN, "false", 0, 0, NULL, VENTURE_SETTING_PERMISSION_ADMIN,
	  "ai_message_rule (each): auto-send", "auto-send", NULL, rule, rules, NULL }
};

void venture_ai_messages_settings_register(void)
{
	gsize i;
	for (i = 0; i < G_N_ELEMENTS(messages); i++)
		venture_settings_register(&messages[i]);
}
