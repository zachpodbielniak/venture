/*
 * venture-schema.c - Deriving the database schema from record types
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "venture.h"

#include <string.h>

/*
 * Walks a record type's persistent properties, calling @callback for each.
 * Every function here needs the same walk, and doing it in one place is what
 * keeps the CREATE, the INSERT and the SELECT agreeing on which columns
 * exist and in what order.
 */
typedef void (*VentureSchemaPropertyFunc) (
	VentureEntityClass	*klass,
	GParamSpec		*pspec,
	const gchar		*column,
	VentureColumnFlags	 flags,
	gpointer		 user_data
);

static void
venture_schema_foreach_property(
	GType				 entity_type,
	VentureSchemaPropertyFunc	 callback,
	gpointer			 user_data
){
	g_autoptr(VentureEntityClass) klass = NULL;
	g_autofree GParamSpec **properties = NULL;
	guint n_properties;
	guint i;

	klass = g_type_class_ref(entity_type);
	properties = venture_entity_class_list_persistent_properties(klass,
	                                                             &n_properties);

	for (i = 0; i < n_properties; i++)
	{
		g_autofree gchar *column = NULL;
		VentureColumnFlags flags;

		column = venture_entity_property_to_column(properties[i]->name);
		flags = venture_entity_class_get_column_flags(klass,
		                                              properties[i]->name);

		callback(klass, properties[i], column, flags, user_data);
	}
}

const gchar *
venture_schema_column_type(
	GType		value_type,
	OrmDialectType	dialect
){
	if (G_TYPE_BOOLEAN == value_type)
	{
		/* SQLite has no boolean; an integer 0 or 1 is the convention
		 * and is what its own docs recommend. */
		return (ORM_DIALECT_SQLITE == dialect) ? "INTEGER" : "BOOLEAN";
	}

	if ((G_TYPE_INT64 == value_type) || (G_TYPE_INT == value_type) ||
	    (G_TYPE_UINT == value_type) || (G_TYPE_UINT64 == value_type))
	{
		return (ORM_DIALECT_SQLITE == dialect) ? "INTEGER" : "BIGINT";
	}

	if ((G_TYPE_DOUBLE == value_type) || (G_TYPE_FLOAT == value_type))
		return (ORM_DIALECT_SQLITE == dialect) ? "REAL" : "DOUBLE PRECISION";

	/* Timestamps, enumerations, string lists and everything else are
	 * text. Timestamps in particular are ISO 8601 UTC, which sorts and
	 * compares correctly as a string on both backends without any of the
	 * timezone behaviour a native timestamp column brings. */
	return "TEXT";
}

/* --- Column enumeration -------------------------------------------------- */

static void
venture_schema_collect_column(
	VentureEntityClass	*klass,
	GParamSpec		*pspec,
	const gchar		*column,
	VentureColumnFlags	 flags,
	gpointer		 user_data
){
	GPtrArray *columns;

	columns = user_data;

	if (VENTURE_TYPE_MONEY == pspec->value_type)
	{
		g_ptr_array_add(columns, g_strconcat(column,
			VENTURE_SCHEMA_MONEY_AMOUNT_SUFFIX, NULL));
		g_ptr_array_add(columns, g_strconcat(column,
			VENTURE_SCHEMA_MONEY_CURRENCY_SUFFIX, NULL));
		g_ptr_array_add(columns, g_strconcat(column,
			VENTURE_SCHEMA_MONEY_EXPONENT_SUFFIX, NULL));
		return;
	}

	g_ptr_array_add(columns, g_strdup(column));
}

gchar **
venture_schema_get_columns(GType entity_type)
{
	g_autoptr(GPtrArray) columns = NULL;

	g_return_val_if_fail(g_type_is_a(entity_type, VENTURE_TYPE_ENTITY), NULL);

	columns = g_ptr_array_new();
	venture_schema_foreach_property(entity_type,
	                                venture_schema_collect_column, columns);
	g_ptr_array_add(columns, NULL);

	return (gchar **)g_ptr_array_free(g_steal_pointer(&columns), FALSE);
}

gchar *
venture_schema_quote_identifier(const gchar *identifier)
{
	g_autoptr(GString) quoted = NULL;
	const gchar *cursor;

	g_return_val_if_fail(NULL != identifier, NULL);

	quoted = g_string_new("\"");

	for (cursor = identifier; '\0' != *cursor; cursor++)
	{
		/* Doubling an embedded quote is the standard escape and is
		 * accepted by both backends. Identifiers here are derived from
		 * C property names so they cannot actually contain one, but a
		 * plugin could name a field anything. */
		if ('"' == *cursor)
			g_string_append(quoted, "\"\"");
		else
			g_string_append_c(quoted, *cursor);
	}

	g_string_append_c(quoted, '"');

	return g_string_free(g_steal_pointer(&quoted), FALSE);
}

/* --- CREATE TABLE -------------------------------------------------------- */

typedef struct
{
	GString		*sql;
	OrmDialectType	 dialect;
	gboolean	 first;
} VentureSchemaCreateContext;

/*
 * Appends one column definition, or three for a money property.
 */
static void
venture_schema_append_column(
	VentureEntityClass	*klass,
	GParamSpec		*pspec,
	const gchar		*column,
	VentureColumnFlags	 flags,
	gpointer		 user_data
){
	VentureSchemaCreateContext *context;
	g_autofree gchar *quoted = NULL;

	context = user_data;

	if (!context->first)
		g_string_append(context->sql, ",\n");

	context->first = FALSE;

	/* The primary key is spelled differently on each backend, and getting
	 * it wrong means either no autoincrement or a syntax error. */
	if (0 != (flags & VENTURE_COLUMN_FLAG_PRIMARY_KEY))
	{
		quoted = venture_schema_quote_identifier(column);

		if (ORM_DIALECT_SQLITE == context->dialect)
		{
			g_string_append_printf(context->sql,
				"  %s INTEGER PRIMARY KEY AUTOINCREMENT", quoted);
		}
		else
		{
			g_string_append_printf(context->sql,
				"  %s BIGSERIAL PRIMARY KEY", quoted);
		}

		return;
	}

	if (VENTURE_TYPE_MONEY == pspec->value_type)
	{
		g_autofree gchar *amount = NULL;
		g_autofree gchar *currency = NULL;
		g_autofree gchar *exponent = NULL;
		g_autofree gchar *amount_column = g_strconcat(column, VENTURE_SCHEMA_MONEY_AMOUNT_SUFFIX, NULL);
		g_autofree gchar *currency_column = g_strconcat(column, VENTURE_SCHEMA_MONEY_CURRENCY_SUFFIX, NULL);
		g_autofree gchar *exponent_column = g_strconcat(column, VENTURE_SCHEMA_MONEY_EXPONENT_SUFFIX, NULL);

		/* Quoting allocates a new string; it does not consume its input.
		 * Keep both allocations scoped across repeated schema reconciliation. */
		amount = venture_schema_quote_identifier(amount_column);
		currency = venture_schema_quote_identifier(currency_column);
		exponent = venture_schema_quote_identifier(exponent_column);

		g_string_append_printf(context->sql,
			"  %s %s,\n  %s TEXT,\n  %s %s",
			amount,
			venture_schema_column_type(G_TYPE_INT64, context->dialect),
			currency,
			exponent,
			venture_schema_column_type(G_TYPE_INT64, context->dialect));

		return;
	}

	quoted = venture_schema_quote_identifier(column);

	g_string_append_printf(context->sql, "  %s %s", quoted,
		venture_schema_column_type(pspec->value_type, context->dialect));

	if (0 != (flags & VENTURE_COLUMN_FLAG_NOT_NULL))
		g_string_append(context->sql, " NOT NULL");

	if (0 != (flags & VENTURE_COLUMN_FLAG_UNIQUE))
		g_string_append(context->sql, " UNIQUE");
}

gchar *
venture_schema_get_create_table_sql(
	GType		entity_type,
	OrmDialectType	dialect
){
	g_autoptr(VentureEntity) prototype = NULL;
	g_autoptr(GString) sql = NULL;
	g_autofree gchar *table = NULL;
	VentureSchemaCreateContext context;

	g_return_val_if_fail(g_type_is_a(entity_type, VENTURE_TYPE_ENTITY), NULL);

	prototype = g_object_new(entity_type, NULL);
	table = venture_schema_quote_identifier(
		venture_entity_get_table_name(prototype));

	sql = g_string_new(NULL);
	g_string_append_printf(sql, "CREATE TABLE IF NOT EXISTS %s (\n", table);

	context.sql = sql;
	context.dialect = dialect;
	context.first = TRUE;

	venture_schema_foreach_property(entity_type,
	                                venture_schema_append_column, &context);

	g_string_append(sql, "\n)");

	return g_string_free(g_steal_pointer(&sql), FALSE);
}

/* --- Indexes ------------------------------------------------------------- */

typedef struct
{
	GPtrArray	*statements;
	const gchar	*table;
} VentureSchemaIndexContext;

static void
venture_schema_append_index(
	VentureEntityClass	*klass,
	GParamSpec		*pspec,
	const gchar		*column,
	VentureColumnFlags	 flags,
	gpointer		 user_data
){
	VentureSchemaIndexContext *context;
	g_autofree gchar *quoted_table = NULL;
	g_autofree gchar *quoted_column = NULL;
	g_autofree gchar *index_name = NULL;
	g_autofree gchar *target = NULL;

	context = user_data;

	if (0 != (flags & VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION))
	{
		quoted_table = venture_schema_quote_identifier(context->table);
		quoted_column = venture_schema_quote_identifier(column);
		index_name = g_strdup_printf("uq_%s_organization_%s", context->table, column);
		g_ptr_array_add(context->statements,
			g_strdup_printf("CREATE UNIQUE INDEX IF NOT EXISTS %s ON %s "
				"(\"organization_id\", %s) WHERE %s IS NOT NULL AND %s <> ''",
				index_name, quoted_table, quoted_column, quoted_column, quoted_column));
		return;
	}

	if (0 == (flags & VENTURE_COLUMN_FLAG_INDEXED))
		return;

	/* A unique constraint already creates an index on both backends, so
	 * adding a second one would just cost writes. */
	if (0 != (flags & VENTURE_COLUMN_FLAG_UNIQUE))
		return;

	/* Money expands to three columns; the amount is the one worth
	 * indexing, since nobody looks a record up by its currency. */
	target = (VENTURE_TYPE_MONEY == pspec->value_type)
		? g_strconcat(column, VENTURE_SCHEMA_MONEY_AMOUNT_SUFFIX, NULL)
		: g_strdup(column);

	quoted_table = venture_schema_quote_identifier(context->table);
	quoted_column = venture_schema_quote_identifier(target);
	index_name = g_strdup_printf("idx_%s_%s", context->table, target);

	g_ptr_array_add(context->statements,
		g_strdup_printf("CREATE INDEX IF NOT EXISTS %s ON %s (%s)",
		                index_name, quoted_table, quoted_column));
}

gchar **
venture_schema_get_create_index_sql(
	GType		entity_type,
	OrmDialectType	dialect
){
	g_autoptr(VentureEntity) prototype = NULL;
	g_autoptr(GPtrArray) statements = NULL;
	VentureSchemaIndexContext context;

	g_return_val_if_fail(g_type_is_a(entity_type, VENTURE_TYPE_ENTITY), NULL);

	(void)dialect;

	prototype = g_object_new(entity_type, NULL);
	statements = g_ptr_array_new();

	context.statements = statements;
	context.table = venture_entity_get_table_name(prototype);

	venture_schema_foreach_property(entity_type,
	                                venture_schema_append_index, &context);

	g_ptr_array_add(statements, NULL);

	return (gchar **)g_ptr_array_free(g_steal_pointer(&statements), FALSE);
}

/* --- Applying the schema ------------------------------------------------- */

GHashTable *
venture_schema_get_existing_columns(
	OrmConnection	 *connection,
	const gchar	 *table_name,
	GError		**error
){
	g_autoptr(OrmResult) result = NULL;
	g_autoptr(GHashTable) columns = NULL;
	g_autofree gchar *sql = NULL;
	OrmDialectType dialect;

	g_return_val_if_fail(NULL != connection, NULL);
	g_return_val_if_fail(NULL != table_name, NULL);

	dialect = orm_engine_get_dialect_type(orm_connection_get_engine(connection));

	if (ORM_DIALECT_SQLITE == dialect)
	{
		sql = g_strdup_printf("PRAGMA table_info(\"%s\")", table_name);
	}
	else
	{
		sql = g_strdup_printf(
			"SELECT column_name FROM information_schema.columns "
			"WHERE table_name = '%s'", table_name);
	}

	result = orm_connection_query(connection, sql, error);

	if (NULL == result)
		return NULL;

	columns = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

	while (orm_result_next(result))
	{
		OrmRow *row;
		const gchar *name;

		row = orm_result_get_row(result);

		/* PRAGMA table_info returns cid, name, type, ...; the
		 * information_schema query selects the name alone. */
		name = orm_row_get_string(row, (ORM_DIALECT_SQLITE == dialect) ? 1 : 0);

		if (NULL != name)
			g_hash_table_add(columns, g_strdup(name));
	}

	return g_steal_pointer(&columns);
}

typedef struct
{
	OrmConnection	*connection;
	GHashTable	*existing;
	const gchar	*table;
	OrmDialectType	 dialect;
	GError		*error;
} VentureSchemaAlterContext;

/*
 * Adds a column the record type has gained since the table was created.
 *
 * Only additions are automatic. Dropping or retyping a column both destroy
 * data, and doing either silently on startup because someone renamed a field
 * would be indefensible -- so those stay a deliberate, manual migration.
 */
static void
venture_schema_add_missing_column(
	OrmConnection		*connection,
	const gchar		*table,
	const gchar		*column,
	const gchar		*sql_type,
	GHashTable		*existing,
	GError			**error
){
	g_autofree gchar *quoted_table = NULL;
	g_autofree gchar *quoted_column = NULL;
	g_autofree gchar *sql = NULL;

	if (g_hash_table_contains(existing, column))
		return;

	quoted_table = venture_schema_quote_identifier(table);
	quoted_column = venture_schema_quote_identifier(column);
	sql = g_strdup_printf("ALTER TABLE %s ADD COLUMN %s %s",
	                      quoted_table, quoted_column, sql_type);

	g_debug("venture_schema: %s", sql);

	orm_connection_execute(connection, sql, error);
}

static void
venture_schema_alter_property(
	VentureEntityClass	*klass,
	GParamSpec		*pspec,
	const gchar		*column,
	VentureColumnFlags	 flags,
	gpointer		 user_data
){
	VentureSchemaAlterContext *context;

	context = user_data;

	/* Stop at the first failure rather than piling up errors. */
	if (NULL != context->error)
		return;

	if (0 != (flags & VENTURE_COLUMN_FLAG_PRIMARY_KEY))
		return;

	if (VENTURE_TYPE_MONEY == pspec->value_type)
	{
		g_autofree gchar *amount = NULL;
		g_autofree gchar *currency = NULL;
		g_autofree gchar *exponent = NULL;
		const gchar *integer_type;

		integer_type = venture_schema_column_type(G_TYPE_INT64,
		                                          context->dialect);

		amount = g_strconcat(column, VENTURE_SCHEMA_MONEY_AMOUNT_SUFFIX, NULL);
		currency = g_strconcat(column, VENTURE_SCHEMA_MONEY_CURRENCY_SUFFIX, NULL);
		exponent = g_strconcat(column, VENTURE_SCHEMA_MONEY_EXPONENT_SUFFIX, NULL);

		venture_schema_add_missing_column(context->connection, context->table,
		                                  amount, integer_type,
		                                  context->existing, &context->error);
		venture_schema_add_missing_column(context->connection, context->table,
		                                  currency, "TEXT",
		                                  context->existing, &context->error);
		venture_schema_add_missing_column(context->connection, context->table,
		                                  exponent, integer_type,
		                                  context->existing, &context->error);
		return;
	}

	/* A NOT NULL column cannot simply be added to a table with rows in
	 * it, so added columns are always nullable. The application-level
	 * validation still enforces the constraint on write. */
	venture_schema_add_missing_column(context->connection, context->table,
	                                  column,
	                                  venture_schema_column_type(pspec->value_type,
	                                                             context->dialect),
	                                  context->existing, &context->error);
}

gboolean
venture_schema_create_table(
	OrmConnection	 *connection,
	GType		  entity_type,
	GError		**error
){
	g_autoptr(VentureEntity) prototype = NULL;
	g_autofree gchar *create_sql = NULL;
	g_auto(GStrv) index_sql = NULL;
	g_autoptr(GHashTable) existing = NULL;
	VentureSchemaAlterContext context;
	OrmDialectType dialect;
	const gchar *table;
	gsize i;

	g_return_val_if_fail(NULL != connection, FALSE);
	g_return_val_if_fail(g_type_is_a(entity_type, VENTURE_TYPE_ENTITY), FALSE);

	dialect = orm_engine_get_dialect_type(orm_connection_get_engine(connection));
	prototype = g_object_new(entity_type, NULL);
	table = venture_entity_get_table_name(prototype);

	create_sql = venture_schema_get_create_table_sql(entity_type, dialect);

	if (!orm_connection_execute(connection, create_sql, error))
	{
		g_prefix_error(error, "Creating table %s: ", table);
		return FALSE;
	}

	/* Add whatever the record type has gained since the table was made. */
	existing = venture_schema_get_existing_columns(connection, table, error);

	if (NULL == existing)
		return FALSE;

	context.connection = connection;
	context.existing = existing;
	context.table = table;
	context.dialect = dialect;
	context.error = NULL;

	venture_schema_foreach_property(entity_type,
	                                venture_schema_alter_property, &context);

	if (NULL != context.error)
	{
		g_propagate_prefixed_error(error, context.error,
		                           "Updating table %s: ", table);
		return FALSE;
	}

	index_sql = venture_schema_get_create_index_sql(entity_type, dialect);

	if (!venture_period_constraints_migrate(connection, entity_type, error))
		return FALSE;

	for (i = 0; NULL != index_sql[i]; i++)
	{
		if (!orm_connection_execute(connection, index_sql[i], error))
		{
			g_prefix_error(error, "Creating index on %s: ", table);
			return FALSE;
		}
	}

	return TRUE;
}

gboolean
venture_schema_create_all(
	OrmConnection		 *connection,
	VentureEntityRegistry	 *registry,
	GError			**error
){
	g_autofree GType *types = NULL;
	guint n_types;
	guint i;

	g_return_val_if_fail(NULL != connection, FALSE);
	g_return_val_if_fail(VENTURE_IS_ENTITY_REGISTRY(registry), FALSE);

	types = venture_entity_registry_list_types(registry, &n_types);

	for (i = 0; i < n_types; i++)
	{
		if (!venture_schema_create_table(connection, types[i], error))
			return FALSE;
	}

	return TRUE;
}

/* --- Binding ------------------------------------------------------------- */

typedef struct
{
	VentureEntity	*entity;
	GPtrArray	*columns;
	GList		*values;
	gboolean	 include_identity;
} VentureSchemaBindContext;

/*
 * Converts one property value to the OrmValue (or three) that represent it.
 */
static void
venture_schema_bind_property(
	VentureEntityClass	*klass,
	GParamSpec		*pspec,
	const gchar		*column,
	VentureColumnFlags	 flags,
	gpointer		 user_data
){
	VentureSchemaBindContext *context;
	g_auto(GValue) value = G_VALUE_INIT;

	context = user_data;

	/* The primary key is assigned by the database on insert and is the
	 * WHERE clause on update, so it is never one of the bound values. */
	if (!context->include_identity &&
	    (0 != (flags & VENTURE_COLUMN_FLAG_PRIMARY_KEY)))
		return;

	g_value_init(&value, pspec->value_type);
	g_object_get_property(G_OBJECT(context->entity), pspec->name, &value);

	if (VENTURE_TYPE_MONEY == pspec->value_type)
	{
		const VentureMoney *money;

		money = g_value_get_boxed(&value);

		g_ptr_array_add(context->columns,
			g_strconcat(column, VENTURE_SCHEMA_MONEY_AMOUNT_SUFFIX, NULL));
		g_ptr_array_add(context->columns,
			g_strconcat(column, VENTURE_SCHEMA_MONEY_CURRENCY_SUFFIX, NULL));
		g_ptr_array_add(context->columns,
			g_strconcat(column, VENTURE_SCHEMA_MONEY_EXPONENT_SUFFIX, NULL));

		if (NULL == money)
		{
			/* All three columns go NULL together, so a missing
			 * amount is distinguishable from a zero one. */
			context->values = g_list_append(context->values,
			                                orm_value_new_null());
			context->values = g_list_append(context->values,
			                                orm_value_new_null());
			context->values = g_list_append(context->values,
			                                orm_value_new_null());
			return;
		}

		context->values = g_list_append(context->values,
			orm_value_new_integer(venture_money_get_amount(money)));
		context->values = g_list_append(context->values,
			orm_value_new_string(venture_money_get_currency(money)));
		context->values = g_list_append(context->values,
			orm_value_new_integer((gint64)venture_money_get_exponent(money)));

		return;
	}

	g_ptr_array_add(context->columns, g_strdup(column));

	if (G_TYPE_DATE_TIME == pspec->value_type)
	{
		GDateTime *when;
		g_autofree gchar *text = NULL;

		when = g_value_get_boxed(&value);
		text = venture_time_to_string(when);

		context->values = g_list_append(context->values,
			(NULL != text) ? orm_value_new_string(text)
			               : orm_value_new_null());
		return;
	}

	if (G_TYPE_IS_ENUM(pspec->value_type))
	{
		const gchar *nick;

		nick = venture_enum_to_nick(pspec->value_type,
		                            g_value_get_enum(&value));

		context->values = g_list_append(context->values,
			(NULL != nick) ? orm_value_new_string(nick)
			               : orm_value_new_null());
		return;
	}

	if (G_TYPE_STRV == pspec->value_type)
	{
		const gchar * const *items;
		g_autofree gchar *joined = NULL;

		items = g_value_get_boxed(&value);

		if (NULL == items)
		{
			context->values = g_list_append(context->values,
			                                orm_value_new_null());
			return;
		}

		joined = g_strjoinv("\x1f", (gchar **)items);
		context->values = g_list_append(context->values,
			orm_value_new_string(joined));
		return;
	}

	if (G_TYPE_BOOLEAN == pspec->value_type)
	{
		context->values = g_list_append(context->values,
			orm_value_new_boolean(g_value_get_boolean(&value)));
		return;
	}

	if ((G_TYPE_INT64 == pspec->value_type) || (G_TYPE_INT == pspec->value_type))
	{
		gint64 number;

		number = (G_TYPE_INT64 == pspec->value_type)
			? g_value_get_int64(&value)
			: (gint64)g_value_get_int(&value);

		context->values = g_list_append(context->values,
			orm_value_new_integer(number));
		return;
	}

	if (G_TYPE_DOUBLE == pspec->value_type)
	{
		context->values = g_list_append(context->values,
			orm_value_new_float(g_value_get_double(&value)));
		return;
	}

	{
		const gchar *text;

		text = g_value_get_string(&value);

		context->values = g_list_append(context->values,
			(NULL != text) ? orm_value_new_string(text)
			               : orm_value_new_null());
	}
}

void
venture_schema_bind_entity(
	VentureEntity	 *entity,
	GPtrArray	**out_columns,
	GList		**out_values,
	gboolean	  include_identity
){
	VentureSchemaBindContext context;

	g_return_if_fail(VENTURE_IS_ENTITY(entity));
	g_return_if_fail(NULL != out_columns);
	g_return_if_fail(NULL != out_values);

	context.entity = entity;
	context.columns = g_ptr_array_new_with_free_func(g_free);
	context.values = NULL;
	context.include_identity = include_identity;

	venture_schema_foreach_property(G_OBJECT_TYPE(entity),
	                                venture_schema_bind_property, &context);

	*out_columns = context.columns;
	*out_values = context.values;
}

/* --- Populating ---------------------------------------------------------- */

typedef struct
{
	VentureEntity	*entity;
	OrmRow		*row;
} VentureSchemaPopulateContext;

static void
venture_schema_populate_property(
	VentureEntityClass	*klass,
	GParamSpec		*pspec,
	const gchar		*column,
	VentureColumnFlags	 flags,
	gpointer		 user_data
){
	VentureSchemaPopulateContext *context;
	g_auto(GValue) value = G_VALUE_INIT;
	gint index;

	context = user_data;

	if (VENTURE_TYPE_MONEY == pspec->value_type)
	{
		g_autofree gchar *amount_column = NULL;
		g_autofree gchar *currency_column = NULL;
		g_autofree gchar *exponent_column = NULL;
		g_autoptr(VentureMoney) money = NULL;
		gint amount_index;
		gint currency_index;
		gint exponent_index;

		amount_column = g_strconcat(column,
			VENTURE_SCHEMA_MONEY_AMOUNT_SUFFIX, NULL);
		currency_column = g_strconcat(column,
			VENTURE_SCHEMA_MONEY_CURRENCY_SUFFIX, NULL);
		exponent_column = g_strconcat(column,
			VENTURE_SCHEMA_MONEY_EXPONENT_SUFFIX, NULL);

		amount_index = orm_row_get_column_index(context->row, amount_column);
		currency_index = orm_row_get_column_index(context->row, currency_column);
		exponent_index = orm_row_get_column_index(context->row, exponent_column);

		if ((amount_index < 0) ||
		    orm_row_is_null(context->row, amount_index))
			return;

		money = venture_money_new(
			orm_row_get_integer(context->row, amount_index),
			(currency_index >= 0)
				? orm_row_get_string(context->row, currency_index)
				: NULL,
			(exponent_index >= 0)
				? (guint8)orm_row_get_integer(context->row, exponent_index)
				: (guint8)(VENTURE_MONEY_MAX_EXPONENT + 1));

		g_object_set(context->entity, pspec->name, money, NULL);

		return;
	}

	index = orm_row_get_column_index(context->row, column);

	/* A column the record type has but the row does not means the
	 * database is behind the binary; leaving the property at its default
	 * is right and lets a read-only older database still load. */
	if (index < 0)
		return;

	if (orm_row_is_null(context->row, index))
		return;

	if (G_TYPE_DATE_TIME == pspec->value_type)
	{
		g_autoptr(GDateTime) when = NULL;
		g_autoptr(GError) local_error = NULL;
		const gchar *text;

		text = orm_row_get_string(context->row, index);

		if (NULL == text)
			return;

		when = venture_time_from_string(text, &local_error);

		if (NULL == when)
		{
			g_warning("Ignoring unreadable timestamp in %s.%s: %s",
			          venture_entity_get_table_name(context->entity),
			          column, local_error->message);
			return;
		}

		g_object_set(context->entity, pspec->name, when, NULL);

		return;
	}

	if (G_TYPE_IS_ENUM(pspec->value_type))
	{
		const gchar *nick;
		gint enum_value;

		nick = orm_row_get_string(context->row, index);

		if ((NULL == nick) ||
		    !venture_enum_from_nick(pspec->value_type, nick, &enum_value))
			return;

		g_value_init(&value, pspec->value_type);
		g_value_set_enum(&value, enum_value);
		g_object_set_property(G_OBJECT(context->entity), pspec->name, &value);

		return;
	}

	if (G_TYPE_STRV == pspec->value_type)
	{
		g_auto(GStrv) items = NULL;
		const gchar *joined;

		joined = orm_row_get_string(context->row, index);

		if (NULL == joined)
			return;

		items = g_strsplit(joined, "\x1f", -1);
		g_object_set(context->entity, pspec->name, items, NULL);

		return;
	}

	g_value_init(&value, pspec->value_type);

	if (G_TYPE_BOOLEAN == pspec->value_type)
	{
		g_value_set_boolean(&value,
			orm_row_get_boolean(context->row, index));
	}
	else if (G_TYPE_INT64 == pspec->value_type)
	{
		g_value_set_int64(&value, orm_row_get_integer(context->row, index));
	}
	else if (G_TYPE_INT == pspec->value_type)
	{
		g_value_set_int(&value,
			(gint)orm_row_get_integer(context->row, index));
	}
	else if (G_TYPE_DOUBLE == pspec->value_type)
	{
		g_value_set_double(&value, orm_row_get_float(context->row, index));
	}
	else
	{
		g_value_set_string(&value, orm_row_get_string(context->row, index));
	}

	g_object_set_property(G_OBJECT(context->entity), pspec->name, &value);
}

void
venture_schema_populate_entity(
	VentureEntity	*entity,
	OrmRow		*row
){
	VentureSchemaPopulateContext context;

	g_return_if_fail(VENTURE_IS_ENTITY(entity));
	g_return_if_fail(NULL != row);

	context.entity = entity;
	context.row = row;

	venture_schema_foreach_property(G_OBJECT_TYPE(entity),
	                                venture_schema_populate_property, &context);

	venture_entity_after_load(entity);
}
