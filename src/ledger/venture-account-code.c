/*
 * venture-account-code.c - a taken account code says who has it
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * An account's code is unique in its organization. A code already in use
 * is refused naming the account that has it, instead of surfacing as a
 * database constraint error. A household makes few accounts by hand: bank
 * accounts make their own ledger account (see the banking service).
 */

#include "venture.h"
#include "ledger/venture-ledger-private.h"

static VentureEntity *
account_with_code(VentureDatabase *db, gint64 org, const gchar *code)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ACCOUNT);
	g_autoptr(VentureAccessScope) internal = venture_access_policy_enter(venture_database_get_access_policy(db), NULL);
	venture_query_set_organization(query, org);
	venture_query_add_filter_string(query, "code", VENTURE_FILTER_OP_EQ, code, NULL);
	venture_query_set_include_deleted(query, TRUE);
	return venture_database_find_one(db, query, NULL);
}

static gboolean
validate_account_code(VentureDatabase *database, VentureEntity *entity, VentureEntity *previous, gpointer data, GError **error)
{
	g_autofree gchar *code = NULL;
	g_autoptr(VentureEntity) holder = NULL;
	gint64 org = venture_entity_get_organization_id(entity);
	(void)previous; (void)data;
	g_object_get(entity, "code", &code, NULL);
	if (code == NULL || *code == '\0')
		return TRUE;
	holder = account_with_code(database, org, code);
	if (holder != NULL && venture_entity_get_id(holder) != venture_entity_get_id(entity))
	{
		g_autofree gchar *name = NULL;
		g_object_get(holder, "name", &name, NULL);
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"Code %s is already used by the account \"%s\"%s; choose another or leave it empty",
			code, name != NULL ? name : "(unnamed)", venture_entity_is_deleted(holder) ? " (deleted)" : "");
		return FALSE;
	}
	return TRUE;
}

void
venture_account_code_install(VentureDatabase *database)
{
	g_return_if_fail(VENTURE_IS_DATABASE(database));
	venture_database_add_save_validator(database, VENTURE_TYPE_ACCOUNT, validate_account_code, NULL, NULL);
}
