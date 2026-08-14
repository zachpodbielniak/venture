/*
 * venture-schema.h - Deriving the database schema from record types
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * There is no schema file. The tables, columns, types, constraints and
 * indexes are all derived from the GObject properties of the registered
 * record types, which means a new field is a line in a field table and a new
 * record type is a registration -- never a migration written by hand and
 * never a schema that has drifted from the code.
 *
 * Column naming and types
 * -----------------------
 *
 * A property maps to a column of the same name with hyphens turned into
 * underscores, except for money, which expands to three:
 *
 *   gross  ->  gross_amount    BIGINT   exact minor units
 *              gross_currency  TEXT     ISO 4217 code
 *              gross_exponent  SMALLINT minor-unit digits
 *
 * Storing money as an integer plus its currency is what lets the database do
 * the aggregation. A single text column would be tidier to look at and would
 * make `SELECT SUM(...)` impossible, which for a system whose whole purpose
 * is totalling money would be the wrong trade every time.
 *
 * Timestamps are stored as ISO 8601 text in UTC. That sorts correctly as a
 * string, compares correctly between the two backends, and never suffers the
 * timezone surprises that a native timestamp column brings with it.
 * Enumerations are stored by nick, so the values stay readable in a shell
 * and stay stable if the C enum is ever reordered.
 */

#ifndef VENTURE_SCHEMA_H
#define VENTURE_SCHEMA_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>
#include <orm.h>

G_BEGIN_DECLS

/**
 * VENTURE_SCHEMA_MONEY_AMOUNT_SUFFIX:
 *
 * Suffix of the column holding a money field's exact minor-unit amount.
 */
#define VENTURE_SCHEMA_MONEY_AMOUNT_SUFFIX "_amount"

/**
 * VENTURE_SCHEMA_MONEY_CURRENCY_SUFFIX:
 *
 * Suffix of the column holding a money field's ISO 4217 code.
 */
#define VENTURE_SCHEMA_MONEY_CURRENCY_SUFFIX "_currency"

/**
 * VENTURE_SCHEMA_MONEY_EXPONENT_SUFFIX:
 *
 * Suffix of the column holding a money field's minor-unit digit count.
 */
#define VENTURE_SCHEMA_MONEY_EXPONENT_SUFFIX "_exponent"

/**
 * venture_schema_column_type:
 * @value_type: the #GType of a property
 * @dialect: the target SQL dialect
 *
 * Maps a property type onto its SQL column type.
 *
 * Returns: (transfer none): the SQL type name
 */
const gchar *
venture_schema_column_type(
	GType		value_type,
	OrmDialectType	dialect
);

/**
 * venture_schema_get_columns:
 * @entity_type: a #GType deriving from %VENTURE_TYPE_ENTITY
 *
 * Lists the column names for a record type, in declaration order, with money
 * properties already expanded into their three columns.
 *
 * Returns: (transfer full) (array zero-terminated=1): the column names
 */
gchar **
venture_schema_get_columns(GType entity_type);

/**
 * venture_schema_get_create_table_sql:
 * @entity_type: a #GType deriving from %VENTURE_TYPE_ENTITY
 * @dialect: the target SQL dialect
 *
 * Builds the `CREATE TABLE IF NOT EXISTS` statement for a record type.
 *
 * Returns: (transfer full): the statement
 */
gchar *
venture_schema_get_create_table_sql(
	GType		entity_type,
	OrmDialectType	dialect
);

/**
 * venture_schema_get_create_index_sql:
 * @entity_type: a #GType deriving from %VENTURE_TYPE_ENTITY
 * @dialect: the target SQL dialect
 *
 * Builds the index statements implied by the record type's column flags.
 *
 * Returns: (transfer full) (array zero-terminated=1): the statements
 */
gchar **
venture_schema_get_create_index_sql(
	GType		entity_type,
	OrmDialectType	dialect
);

/**
 * venture_schema_create_table:
 * @connection: an open #OrmConnection
 * @entity_type: the record type to create
 * @error: (out) (optional): return location for a #GError
 *
 * Creates the table and its indexes if they do not already exist, then adds
 * any column the record type has gained since the table was made.
 *
 * That last part is what makes adding a field a non-event: the new column
 * appears on next startup, existing rows get NULL, and nothing has to be
 * migrated by hand. Removing or retyping a field is deliberately NOT done
 * automatically, because both destroy data.
 *
 * Returns: %TRUE on success
 */
gboolean
venture_schema_create_table(
	OrmConnection	 *connection,
	GType		  entity_type,
	GError		**error
);

/**
 * venture_schema_create_all:
 * @connection: an open #OrmConnection
 * @registry: the record types to create tables for
 * @error: (out) (optional): return location for a #GError
 *
 * Creates or updates the table for every registered record type.
 *
 * Returns: %TRUE on success
 */
gboolean
venture_schema_create_all(
	OrmConnection		 *connection,
	VentureEntityRegistry	 *registry,
	GError			**error
);

/**
 * venture_schema_get_existing_columns:
 * @connection: an open #OrmConnection
 * @table_name: the table to inspect
 * @error: (out) (optional): return location for a #GError
 *
 * Reads the columns a table currently has. Used to work out what a record
 * type has gained since the table was created.
 *
 * Returns: (transfer full) (element-type utf8) (nullable): a set of column
 *   names, or %NULL on error
 */
GHashTable *
venture_schema_get_existing_columns(
	OrmConnection	 *connection,
	const gchar	 *table_name,
	GError		**error
);

/**
 * venture_schema_quote_identifier:
 * @identifier: a table or column name
 *
 * Quotes an identifier for SQL. Both supported backends accept double
 * quotes, and an embedded quote is doubled.
 *
 * Returns: (transfer full): the quoted identifier
 */
gchar *
venture_schema_quote_identifier(const gchar *identifier);

/**
 * venture_schema_bind_entity:
 * @entity: the record to read
 * @out_columns: (out) (transfer full): the column names bound
 * @out_values: (out) (transfer full) (element-type OrmValue): the values, in
 *   the same order
 * @include_identity: whether to include the primary key
 *
 * Flattens a record into parallel column and value lists ready to be handed
 * to a parameterised INSERT or UPDATE.
 */
void
venture_schema_bind_entity(
	VentureEntity	 *entity,
	GPtrArray	**out_columns,
	GList		**out_values,
	gboolean	  include_identity
);

/**
 * venture_schema_populate_entity:
 * @entity: the record to fill
 * @row: a result row
 *
 * Populates a record from a query result row, reversing
 * venture_schema_bind_entity(). Columns the record type does not have are
 * ignored, so a database that is ahead of the binary still loads.
 */
void
venture_schema_populate_entity(
	VentureEntity	*entity,
	OrmRow		*row
);

G_END_DECLS

#endif /* VENTURE_SCHEMA_H */
