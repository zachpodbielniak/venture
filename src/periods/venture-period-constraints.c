/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

#include <string.h>

static OrmResult *
query_table(OrmConnection *connection, const gchar *sql,
	const gchar *table, GError **error)
{
	GList *params = NULL;
	OrmResult *result;

	params = g_list_append(params, orm_value_new_string(table));
	result = orm_connection_query_with_params(connection, sql, params, error);
	g_list_free_full(params, (GDestroyNotify)orm_value_free);
	return result;
}

static GHashTable *
scoped_columns(GType type)
{
	g_autoptr(VentureEntityClass) klass = NULL;
	g_autofree GParamSpec **properties = NULL;
	GHashTable *columns;
	guint n;
	guint i;

	columns = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	klass = g_type_class_ref(type);
	properties = venture_entity_class_list_persistent_properties(klass, &n);
	for (i = 0; i < n; i++)
	{
		if (0 != (venture_entity_class_get_column_flags(klass, properties[i]->name) &
			VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION))
			g_hash_table_add(columns, venture_entity_property_to_column(properties[i]->name));
	}
	return columns;
}

/* Read all schema objects before changing the schema; a live SQLite cursor
 * over sqlite_master can otherwise hold the table being replaced. */
static gboolean
sqlite_rebuild(OrmConnection *connection, const gchar *table,
	GHashTable *columns, GHashTable *obsolete, GError **error)
{
	g_autoptr(OrmResult) result = NULL;
	g_autoptr(GPtrArray) extras = NULL;
	g_autofree gchar *definition = NULL;
	g_autofree gchar *quoted = NULL;
	g_autofree gchar *temporary = NULL;
	g_autofree gchar *quoted_temporary = NULL;
	g_autofree gchar *sql = NULL;
	g_autoptr(GRegex) table_pattern = NULL;
	g_autoptr(GString) copy_columns = NULL;
	GHashTableIter iter;
	gpointer key;
	gint64 sequence = 0;
	guint i;

	quoted = venture_schema_quote_identifier(table);
	temporary = g_strconcat("venture_periods_migrate_", table, NULL);
	quoted_temporary = venture_schema_quote_identifier(temporary);
	result = query_table(connection,
		"SELECT sql FROM sqlite_master WHERE type = 'table' AND name = ?", table, error);
	if (NULL == result)
		return FALSE;
	if (orm_result_next(result))
		definition = g_strdup(orm_row_get_string(orm_result_get_row(result), 0));
	g_clear_object(&result);
	if (NULL == definition)
		return FALSE;

	/* The old generated schema used inline UNIQUE. Preserve all other
	 * declarations verbatim, including columns added by older plugins. */
	g_hash_table_iter_init(&iter, columns);
	while (g_hash_table_iter_next(&iter, &key, NULL))
	{
		g_autofree gchar *field = NULL;
		g_autofree gchar *escaped = NULL;
		g_autofree gchar *pattern = NULL;
		g_autoptr(GRegex) regex = NULL;
		gchar *next;

		field = venture_schema_quote_identifier(key);
		escaped = g_regex_escape_string(field, -1);
		pattern = g_strdup_printf("(%s[ \\t]+[^,\\n]*?)\\bUNIQUE\\b", escaped);
		regex = g_regex_new(pattern, G_REGEX_CASELESS, 0, error);
		if (NULL == regex)
			return FALSE;
		next = g_regex_replace(regex, definition, -1, 0, "\\1", 0, error);
		if (NULL == next)
			return FALSE;
		g_free(definition);
		definition = next;
	}
	table_pattern = g_regex_new(
		"^CREATE TABLE (?:IF NOT EXISTS )?(?:\"(?:[^\"]|\"\")*\"|[^ (]+)",
		G_REGEX_CASELESS, 0, error);
	if (NULL == table_pattern)
		return FALSE;
	sql = g_strdup_printf("CREATE TABLE %s", quoted_temporary);
	{
		gchar *next;
		next = g_regex_replace_literal(table_pattern, definition, -1, 0, sql, 0, error);
		if (NULL == next)
			return FALSE;
		g_free(definition);
		definition = next;
	}
	g_clear_pointer(&sql, g_free);

	extras = g_ptr_array_new_with_free_func(g_free);
	result = query_table(connection,
		"SELECT name, sql FROM sqlite_master WHERE tbl_name = ? "
		"AND type IN ('index', 'trigger') AND sql IS NOT NULL", table, error);
	if (NULL == result)
		return FALSE;
	while (orm_result_next(result))
	{
		OrmRow *row = orm_result_get_row(result);
		if (!g_hash_table_contains(obsolete, orm_row_get_string(row, 0)))
			g_ptr_array_add(extras, g_strdup(orm_row_get_string(row, 1)));
	}
	g_clear_object(&result);
	result = query_table(connection, "SELECT seq FROM sqlite_sequence WHERE name = ?",
		table, error);
	if (NULL == result)
		return FALSE;
	if (orm_result_next(result))
		sequence = orm_row_get_integer(orm_result_get_row(result), 0);
	g_clear_object(&result);

	sql = g_strdup_printf("PRAGMA table_info(%s)", quoted);
	result = orm_connection_query(connection, sql, error);
	if (NULL == result)
		return FALSE;
	copy_columns = g_string_new(NULL);
	while (orm_result_next(result))
	{
		g_autofree gchar *column = NULL;
		column = venture_schema_quote_identifier(orm_row_get_string(orm_result_get_row(result), 1));
		if (copy_columns->len > 0)
			g_string_append(copy_columns, ", ");
		g_string_append(copy_columns, column);
	}
	g_clear_object(&result);
	g_clear_pointer(&sql, g_free);
	if (!orm_connection_execute(connection, definition, error))
		return FALSE;
	sql = g_strdup_printf("INSERT INTO %s (%s) SELECT %s FROM %s",
		quoted_temporary, copy_columns->str, copy_columns->str, quoted);
	if (!orm_connection_execute(connection, sql, error))
		return FALSE;
	g_clear_pointer(&sql, g_free);
	sql = g_strdup_printf("DROP TABLE %s", quoted);
	if (!orm_connection_execute(connection, sql, error))
		return FALSE;
	g_clear_pointer(&sql, g_free);
	sql = g_strdup_printf("ALTER TABLE %s RENAME TO %s", quoted_temporary, quoted);
	if (!orm_connection_execute(connection, sql, error))
		return FALSE;
	g_clear_pointer(&sql, g_free);
	sql = g_strdup_printf("UPDATE sqlite_sequence SET seq = MAX(seq, %" G_GINT64_FORMAT
		") WHERE name = ?", sequence);
	{
		GList *params = NULL;
		gboolean ok;
		params = g_list_append(params, orm_value_new_string(table));
		ok = orm_connection_execute_with_params(connection, sql, params, error);
		g_list_free_full(params, (GDestroyNotify)orm_value_free);
		if (!ok)
			return FALSE;
	}
	for (i = 0; i < extras->len; i++)
		if (!orm_connection_execute(connection, g_ptr_array_index(extras, i), error))
			return FALSE;
	return TRUE;
}

static gboolean
sqlite_migrate(OrmConnection *connection, const gchar *table,
	GHashTable *columns, GError **error)
{
	g_autoptr(OrmResult) result = NULL;
	g_autoptr(GHashTable) obsolete = NULL;
	g_autoptr(GPtrArray) indexes = NULL;
	g_autofree gchar *quoted = NULL;
	g_autofree gchar *sql = NULL;
	gboolean rebuild = FALSE;
	guint i;

	quoted = venture_schema_quote_identifier(table);
	sql = g_strdup_printf("PRAGMA index_list(%s)", quoted);
	result = orm_connection_query(connection, sql, error);
	if (NULL == result)
		return FALSE;
	indexes = g_ptr_array_new_with_free_func(g_free);
	obsolete = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	while (orm_result_next(result))
	{
		OrmRow *row = orm_result_get_row(result);
		if (0 != orm_row_get_integer(row, 2))
			g_ptr_array_add(indexes, g_strdup(orm_row_get_string(row, 1)));
	}
	g_clear_object(&result);
	for (i = 0; i < indexes->len; i++)
	{
		const gchar *index = g_ptr_array_index(indexes, i);
		g_autofree gchar *quoted_index = NULL;
		gboolean scoped = FALSE;
		guint n = 0;

		quoted_index = venture_schema_quote_identifier(index);
		g_clear_pointer(&sql, g_free);
		sql = g_strdup_printf("PRAGMA index_info(%s)", quoted_index);
		result = orm_connection_query(connection, sql, error);
		if (NULL == result)
			return FALSE;
		while (orm_result_next(result))
		{
			const gchar *column = orm_row_get_string(orm_result_get_row(result), 2);
			n++;
			scoped = (NULL != column) && g_hash_table_contains(columns, column);
		}
		g_clear_object(&result);
		if ((1 == n) && scoped)
		{
			g_hash_table_add(obsolete, g_strdup(index));
			if (g_str_has_prefix(index, "sqlite_autoindex_"))
				rebuild = TRUE;
		}
	}
	if (rebuild)
		return sqlite_rebuild(connection, table, columns, obsolete, error);
	{
		GHashTableIter iter;
		gpointer key;
		g_hash_table_iter_init(&iter, obsolete);
		while (g_hash_table_iter_next(&iter, &key, NULL))
		{
			g_autofree gchar *index = venture_schema_quote_identifier(key);
			g_clear_pointer(&sql, g_free);
			sql = g_strdup_printf("DROP INDEX %s", index);
			if (!orm_connection_execute(connection, sql, error))
				return FALSE;
		}
	}
	return TRUE;
}

static gboolean
postgres_migrate(OrmConnection *connection, const gchar *table,
	GHashTable *columns, GError **error)
{
	g_autoptr(OrmResult) result = NULL;
	g_autoptr(GPtrArray) constraints = NULL;
	g_autofree gchar *quoted = NULL;
	guint i;

	result = query_table(connection,
		"SELECT c.conname, a.attname FROM pg_constraint c "
		"JOIN pg_class t ON t.oid = c.conrelid "
		"JOIN pg_namespace n ON n.oid = t.relnamespace "
		"JOIN pg_attribute a ON a.attrelid = t.oid AND a.attnum = c.conkey[1] "
		"WHERE c.contype = 'u' AND cardinality(c.conkey) = 1 "
		"AND n.nspname = current_schema() AND t.relname = $1", table, error);
	if (NULL == result)
		return FALSE;
	constraints = g_ptr_array_new_with_free_func(g_free);
	while (orm_result_next(result))
	{
		OrmRow *row = orm_result_get_row(result);
		if (g_hash_table_contains(columns, orm_row_get_string(row, 1)))
			g_ptr_array_add(constraints, g_strdup(orm_row_get_string(row, 0)));
	}
	g_clear_object(&result);
	quoted = venture_schema_quote_identifier(table);
	for (i = 0; i < constraints->len; i++)
	{
		g_autofree gchar *constraint = NULL;
		g_autofree gchar *sql = NULL;
		constraint = venture_schema_quote_identifier(g_ptr_array_index(constraints, i));
		sql = g_strdup_printf("ALTER TABLE %s DROP CONSTRAINT %s", quoted, constraint);
		if (!orm_connection_execute(connection, sql, error))
			return FALSE;
	}
	return TRUE;
}

gboolean
venture_period_constraints_migrate(OrmConnection *connection,
	GType entity_type, GError **error)
{
	g_autoptr(GHashTable) columns = NULL;
	g_autoptr(VentureEntity) prototype = NULL;
	g_autoptr(GError) local_error = NULL;
	g_autoptr(OrmTransaction) transaction = NULL;
	g_auto(GStrv) indexes = NULL;
	OrmDialectType dialect;
	const gchar *table;
	gboolean ok;
	guint i;

	columns = scoped_columns(entity_type);
	if (0 == g_hash_table_size(columns))
		return TRUE;
	prototype = g_object_new(entity_type, NULL);
	table = venture_entity_get_table_name(prototype);
	dialect = orm_engine_get_dialect_type(orm_connection_get_engine(connection));
	/* A schema migration may itself be inside an operator's transaction. */
	if (!orm_connection_in_transaction(connection))
	{
		transaction = orm_connection_begin_transaction(connection, error);
		if (NULL == transaction)
			return FALSE;
	}
	if (!orm_connection_execute(connection, "SAVEPOINT venture_periods_unique", error))
	{
		if (transaction) orm_transaction_rollback(transaction, NULL);
		return FALSE;
	}
	ok = (ORM_DIALECT_SQLITE == dialect)
		? sqlite_migrate(connection, table, columns, &local_error)
		: postgres_migrate(connection, table, columns, &local_error);
	indexes = venture_schema_get_create_index_sql(entity_type, dialect);
	for (i = 0; ok && (NULL != indexes[i]); i++)
		ok = orm_connection_execute(connection, indexes[i], &local_error);
	if (!ok)
	{
		orm_connection_execute(connection, "ROLLBACK TO SAVEPOINT venture_periods_unique", NULL);
		orm_connection_execute(connection, "RELEASE SAVEPOINT venture_periods_unique", NULL);
		/* Close only the transaction we started; preserve caller ownership. */
		if (transaction) orm_transaction_rollback(transaction, NULL);
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_MIGRATION,
			"Organization uniqueness migration for %s refused; rows are unchanged. "
			"Resolve duplicate nonempty identifiers before retrying: %s", table,
			(NULL != local_error) ? local_error->message : "unsupported legacy constraint");
		return FALSE;
	}
	if (!orm_connection_execute(connection, "RELEASE SAVEPOINT venture_periods_unique", error))
	{
		if (transaction) orm_transaction_rollback(transaction, NULL);
		return FALSE;
	}
	return (NULL == transaction) || orm_transaction_commit(transaction, error);
}
