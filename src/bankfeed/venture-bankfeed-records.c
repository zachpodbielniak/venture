/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

static const VentureFieldDecl bank_connection_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", "Linked account label"),
	VENTURE_FIELD("provider", "Provider", "Registry key such as teller",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("provider-account-id", "Provider account", "The feed's account identifier",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_REF("bank-account-id", "Bank account", "Statement evidence this feed writes",
		"bank_account", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("connection-key", "Connection key", "Derived organization:provider:account identity",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_UNIQUE),
	VENTURE_FIELD("status", "Status", "linked or error",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("last-synced-at", "Last synced", "Service-owned",
		VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("last-imported", "Last imported", "Rows written by the latest sync",
		VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
};
static gboolean
connection_before_save(VentureEntity *entity, GError **error)
{
	g_autofree gchar *provider = NULL, *account = NULL, *key = NULL, *status = NULL;
	gint64 bank_id = 0;

	g_object_get(entity, "provider", &provider, "provider-account-id", &account,
		"bank-account-id", &bank_id, "status", &status, NULL);
	if (venture_string_is_empty(provider) || !g_regex_match_simple("^[a-z][a-z0-9_-]*$", provider, 0, 0) ||
		venture_string_is_empty(account) || bank_id <= 0 || venture_entity_get_organization_id(entity) <= 0)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"A bank connection requires provider, provider account and bank account");
		return FALSE;
	}
	key = g_strdup_printf("%" G_GINT64_FORMAT ":%s:%s", venture_entity_get_organization_id(entity), provider, account);
	g_object_set(entity, "connection-key", key, NULL);
	if (venture_string_is_empty(status))
		g_object_set(entity, "status", "linked", NULL);
	return TRUE;
}
VENTURE_DEFINE_ENTITY_WITH_CODE(VentureBankConnection, venture_bank_connection, bank_connection_fields,
	VENTURE_ENTITY_CLASS(klass)->before_save = connection_before_save;)
