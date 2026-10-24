/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * venture-inbox-settings.c - Each web chat's switch as a setting
 * (docs/inbox.org, docs/settings.org)
 */
#include "venture.h"

static GPtrArray *chats(VentureContext *context, gint64 organization_id, GError **error)
{
	VentureDatabase *database = venture_context_get_database(context);
	g_autoptr(VentureAccessScope) internal = venture_access_policy_enter(venture_database_get_access_policy(database), NULL);
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_INBOX_CHAT_WIDGET);
	g_autoptr(GPtrArray) rows = NULL;
	GPtrArray *members;
	guint i;
	venture_query_set_organization(query, organization_id);
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
static VentureEntity *chat(VentureContext *context, gint64 organization_id, gint64 member, gboolean for_write, GError **error)
{
	VentureDatabase *database = venture_context_get_database(context);
	g_autoptr(VentureAccessScope) internal = venture_access_policy_enter(venture_database_get_access_policy(database), NULL);
	VentureEntity *row = venture_database_get(database, VENTURE_TYPE_INBOX_CHAT_WIDGET, member, NULL);
	(void)for_write; (void)error;
	if (row != NULL && (venture_entity_is_deleted(row) || venture_entity_get_organization_id(row) != organization_id))
		g_clear_object(&row);
	return row;
}

static const VentureSettingInfo webchat = {
	"inbox.webchat", "Conversations", "inbox", "Web chat on",
	"Whether this chat on your website takes new messages. Off, its page says the chat is closed; conversations already "
	"started stay in the inbox.",
	VENTURE_SETTING_BOOLEAN, "true", 0, 0, NULL, VENTURE_SETTING_PERMISSION_ADMIN,
	"inbox_chat_widget (each): active", "active", NULL, chat, chats, NULL
};

void venture_inbox_settings_register(void)
{
	venture_settings_register(&webchat);
}
