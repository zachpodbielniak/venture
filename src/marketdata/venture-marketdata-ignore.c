/*
 * venture-marketdata-ignore.c - The accounts an organization ignores
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A person keeps characters they never play: one parked on a realm for a
 * name, a bank alt on a server they have since left. A push source sends
 * them anyway, and without a way to say so they count -- in the Accounts
 * totals, in the needs-attention list, and in "My characters", which then
 * prices deals on a realm nobody will visit. An account_ignore record
 * names one character (its account key) or one realm (every account on
 * it), and every answer that reads accounts asks here which to leave out.
 *
 * Matching is by text, case folded, rather than by store row: a source
 * that is rebuilt keeps its keys, and a realm is named by its group (its
 * name, "Medivh") or its venue key (its slug, "medivh"), whichever the
 * person had to hand.
 */

#include "venture.h"
#include "marketdata/venture-marketdata-private.h"

#ifdef VENTURE_HAVE_SQLITE

/* More than anyone would ignore; the bound only keeps a read finite. */
#define MD_IGNORE_MAX (1000)

struct _VentureMarketdataIgnores
{
	GHashTable	*characters;	/* folded account key -> record id */
	GHashTable	*realms;	/* folded realm name or key -> record id */
	JsonArray	*records;	/* {id, kind, key, name}, by name */
};

static gchar *
md_ignore_fold(const gchar *text)
{
	g_autofree gchar *trimmed = g_strstrip(g_strdup((NULL != text) ? text : ""));

	return g_utf8_casefold(trimmed, -1);
}

static gint64
md_ignore_lookup(
	GHashTable	*table,
	const gchar	*text
){
	g_autofree gchar *folded = NULL;

	if (venture_string_is_empty(text))
		return 0;

	folded = md_ignore_fold(text);

	return (gint64)GPOINTER_TO_SIZE(g_hash_table_lookup(table, folded));
}

VentureMarketdataIgnores *
venture_marketdata_ignores_load(
	VentureContext	*context,
	gint64		 organization_id
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	VentureMarketdataIgnores *ignores;
	guint i;

	ignores = g_new0(VentureMarketdataIgnores, 1);
	ignores->characters = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	ignores->realms = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	ignores->records = json_array_new();

	if ((NULL == context) || (organization_id <= 0))
		return ignores;

	query = venture_query_new(VENTURE_TYPE_ACCOUNT_IGNORE);
	venture_query_set_organization(query, organization_id);
	venture_query_add_order(query, "name", VENTURE_SORT_ASCENDING, NULL);
	venture_query_set_limit(query, MD_IGNORE_MAX);
	rows = venture_database_find(venture_context_get_database(context), query, NULL);

	for (i = 0; (NULL != rows) && (i < rows->len); i++)
	{
		VentureEntity *record = g_ptr_array_index(rows, i);
		g_autofree gchar *kind = NULL;
		g_autofree gchar *key = NULL;
		g_autofree gchar *name = NULL;
		JsonObject *object;
		GHashTable *table;

		g_object_get(record, "kind", &kind, "key", &key, "name", &name, NULL);

		if (venture_string_is_empty(key))
			continue;

		if (0 == g_strcmp0(kind, "character"))
			table = ignores->characters;
		else if (0 == g_strcmp0(kind, "realm"))
			table = ignores->realms;
		else
			continue;

		g_hash_table_insert(table, md_ignore_fold(key),
		                    GSIZE_TO_POINTER((gsize)venture_entity_get_id(record)));
		object = json_object_new();
		json_object_set_int_member(object, "id", venture_entity_get_id(record));
		json_object_set_string_member(object, "kind", kind);
		json_object_set_string_member(object, "key", key);
		json_object_set_string_member(object, "name", venture_string_is_empty(name) ? key : name);
		json_array_add_object_element(ignores->records, object);
	}

	return ignores;
}

void
venture_marketdata_ignores_free(VentureMarketdataIgnores *ignores)
{
	if (NULL == ignores)
		return;

	g_hash_table_unref(ignores->characters);
	g_hash_table_unref(ignores->realms);
	json_array_unref(ignores->records);
	g_free(ignores);
}

JsonArray *
venture_marketdata_ignores_json(const VentureMarketdataIgnores *ignores)
{
	return json_array_ref(ignores->records);
}

gint64
venture_marketdata_ignores_character(
	const VentureMarketdataIgnores	*ignores,
	const gchar			*account_key
){
	return (NULL != ignores) ? md_ignore_lookup(ignores->characters, account_key) : 0;
}

gint64
venture_marketdata_ignores_realm(
	const VentureMarketdataIgnores	*ignores,
	const gchar			*group_key,
	const gchar			*venue_key
){
	gint64 id;

	if ((NULL == ignores) || (0 == g_hash_table_size(ignores->realms)))
		return 0;

	id = md_ignore_lookup(ignores->realms, group_key);

	return (0 != id) ? id : md_ignore_lookup(ignores->realms, venue_key);
}

gint64
venture_marketdata_ignores_match(
	const VentureMarketdataIgnores	 *ignores,
	const VentureSeriesAccountRow	 *account,
	const gchar			**out_kind
){
	gint64 id;

	if (NULL != out_kind)
		*out_kind = NULL;

	if ((NULL == ignores) || (NULL == account))
		return 0;

	id = venture_marketdata_ignores_character(ignores, account->key);

	if (0 != id)
	{
		if (NULL != out_kind)
			*out_kind = "character";
		return id;
	}

	id = venture_marketdata_ignores_realm(ignores, account->group_key, account->venue_key);

	if ((0 != id) && (NULL != out_kind))
		*out_kind = "realm";

	return id;
}

gchar **
venture_marketdata_ignores_split(
	const VentureMarketdataIgnores	*ignores,
	GPtrArray			*accounts,
	gboolean			 keep
){
	GPtrArray *keys;
	guint i;

	keys = g_ptr_array_new();

	for (i = 0; (NULL != accounts) && (i < accounts->len); )
	{
		VentureSeriesAccountRow *account = g_ptr_array_index(accounts, i);

		if (0 == venture_marketdata_ignores_match(ignores, account, NULL))
		{
			i++;
			continue;
		}

		g_ptr_array_add(keys, g_strdup(account->key));

		if (keep)
			i++;
		else
			g_ptr_array_remove_index(accounts, i);
	}

	g_ptr_array_add(keys, NULL);

	return (gchar **)g_ptr_array_free(keys, FALSE);
}

#endif /* VENTURE_HAVE_SQLITE */
