/*
 * venture-database.h - Storage and record persistence
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * #VentureDatabase is both the connection and the repository. Keeping them
 * together is deliberate: every write here has to do several things
 * atomically -- validate, stamp, bump the concurrency version, write, audit
 * -- and splitting the connection away from the operations that need it
 * would only create the opportunity to do some of that without the rest.
 *
 * What it guarantees:
 *
 *   - a record is validated before it is written, never after
 *   - a save carrying a stale version fails rather than overwriting someone
 *     else's change
 *   - deletion is soft; nothing that touched a tax filing is ever destroyed
 *   - every mutation produces an audit record naming who caused it
 *   - a set of ledger lines that does not balance is refused as a unit
 *
 * Access is serialised with a mutex. SQLite admits one writer at a time
 * regardless, and for a single-operator system the simplicity is worth far
 * more than the concurrency a pool would buy.
 */

#ifndef VENTURE_DATABASE_H
#define VENTURE_DATABASE_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>
#include <orm.h>

G_BEGIN_DECLS

#define VENTURE_TYPE_DATABASE (venture_database_get_type())

G_DECLARE_FINAL_TYPE(VentureDatabase, venture_database, VENTURE, DATABASE, GObject)

/**
 * VentureActor:
 * @kind: who is acting
 * @name: (nullable): a human-readable label, such as a username or model id
 * @prompt: (nullable): for an AI-driven change, the instruction that caused
 *   it
 * @request_id: (nullable): correlates the change with a request. A change
 *   applied from the confirmation queue carries the confirmation's
 *   identifier here, which is what tells a staged-then-approved write apart
 *   from a direct one -- a direct write never sets it
 * @approved_by: (nullable): for a change that went through the confirmation
 *   queue, who let it through. @name stays whoever asked, so the trail
 *   records both parties rather than collapsing them into one
 *
 * Who is responsible for a mutation. Passed to every write so the audit
 * trail can answer "did I do that, or did the AI?" months later.
 */
typedef struct
{
	VentureActorKind	 kind;
	const gchar		*name;
	const gchar		*prompt;
	const gchar		*request_id;
	const gchar		*approved_by;
} VentureActor;

/**
 * venture_database_new:
 * @uri: a `sqlite://` or `postgres://` connection URI
 * @error: (out) (optional): return location for a #GError
 *
 * Opens a database, creating a SQLite file if it does not exist.
 *
 * Returns: (transfer full) (nullable): the database, or %NULL on error
 */
VentureDatabase *
venture_database_new(
	const gchar	 *uri,
	GError		**error
);

/**
 * venture_database_build_uri:
 * @config: the configuration
 *
 * Builds the connection URI a #VentureDatabase would actually open for
 * @config: a relative SQLite path resolved against the state directory, and
 * the password from the environment variable named by
 * `database.password_env` spliced in.
 *
 * The result carries the credential, which is the point -- it is what gets
 * handed to the driver. Never log it or show it: pass it through
 * venture_string_redact_uri() first.
 *
 * Returns: (transfer full): the effective URI
 */
gchar *
venture_database_build_uri(VentureConfig *config);

/**
 * venture_database_new_for_config:
 * @config: the configuration to read the URI and options from
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer full) (nullable): the database, or %NULL on error
 */
VentureDatabase *
venture_database_new_for_config(
	VentureConfig	 *config,
	GError		**error
);

/**
 * venture_database_get_backend:
 * @self: a #VentureDatabase
 *
 * Returns: which storage engine is in use
 */
VentureDatabaseBackend
venture_database_get_backend(VentureDatabase *self);

/**
 * venture_database_get_connection:
 * @self: a #VentureDatabase
 *
 * Retrieves the underlying connection, for the rare operation this API does
 * not cover. Callers must hold the database lock while using it; prefer
 * venture_database_execute() and venture_database_query(), which do.
 *
 * Returns: (transfer none): the connection
 */
OrmConnection *
venture_database_get_connection(VentureDatabase *self);

/**
 * venture_database_migrate:
 * @self: a #VentureDatabase
 * @registry: the record types to create tables for
 * @error: (out) (optional): return location for a #GError
 *
 * Creates or updates every table, then seeds the rows a fresh install needs
 * to be usable: a default organisation, a standard chart of accounts and a
 * starting set of tax categories.
 *
 * Returns: %TRUE on success
 */
gboolean
venture_database_migrate(
	VentureDatabase		 *self,
	VentureEntityRegistry	 *registry,
	GError			**error
);

/**
 * venture_database_get_schema_version:
 * @self: a #VentureDatabase
 *
 * Returns: the schema version recorded in the database, or 0 if none
 */
gint64
venture_database_get_schema_version(VentureDatabase *self);

/* --- Transactions -------------------------------------------------------- */

/**
 * venture_database_begin:
 * @self: a #VentureDatabase
 * @error: (out) (optional): return location for a #GError
 *
 * Begins a transaction, taking the database lock until it ends.
 *
 * Returns: %TRUE on success
 */
gboolean
venture_database_begin(
	VentureDatabase	 *self,
	GError		**error
);

/**
 * venture_database_commit:
 * @self: a #VentureDatabase
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: %TRUE on success
 */
gboolean
venture_database_commit(
	VentureDatabase	 *self,
	GError		**error
);

/**
 * venture_database_rollback:
 * @self: a #VentureDatabase
 *
 * Abandons the transaction and releases the lock.
 */
void
venture_database_rollback(VentureDatabase *self);

/* --- Record operations --------------------------------------------------- */

/**
 * VentureSaveValidator:
 * @database: the database the record is about to be written to
 * @entity: the record, validated and about to be inserted or updated
 * @previous: (nullable): the stored row it replaces, or %NULL on insert
 * @user_data: the data given at registration
 * @error: (out) (optional): return location for a #GError
 *
 * A check that needs the database. venture_entity_validate() sees only the
 * record; a rule like "both ends of this link must exist" has to look
 * rows up, and this is where it does. Runs inside the database lock, so
 * what it checked is what is there when the write lands. It may also fill
 * derived fields, the way before_save does.
 *
 * Returns: %TRUE to proceed with the save
 */
typedef gboolean (*VentureSaveValidator) (
	VentureDatabase	 *database,
	VentureEntity	 *entity,
	VentureEntity	 *previous,
	gpointer	  user_data,
	GError		**error
);

/**
 * venture_database_add_save_validator:
 * @self: a #VentureDatabase
 * @entity_type: the record type to check; subclasses are checked too
 * @validate: the check
 * @user_data: passed to @validate
 * @destroy: (nullable): called on @user_data when the database is finalised
 *
 * Registers a check that runs before every save of @entity_type, from any
 * writer: the web form, the REST API, an approved staged change, the AI, a
 * plugin. This is the mechanism for an invariant that spans rows, which
 * the schema -- deliberately free of foreign-key clauses -- cannot express.
 */
void
venture_database_add_save_validator(
	VentureDatabase		*self,
	GType			 entity_type,
	VentureSaveValidator	 validate,
	gpointer		 user_data,
	GDestroyNotify		 destroy
);

/**
 * venture_database_save:
 * @self: a #VentureDatabase
 * @entity: the record to write
 * @actor: (nullable): who is responsible
 * @error: (out) (optional): return location for a #GError
 *
 * Validates and writes a record, inserting it if it has no primary key and
 * updating it otherwise, then records an audit entry.
 *
 * An update whose in-memory version does not match the stored one fails with
 * %VENTURE_ERROR_CONFLICT rather than overwriting. That is what stops two
 * browser tabs, or a person and an automation, from silently clobbering each
 * other's edits.
 *
 * Returns: %TRUE on success
 */
gboolean
venture_database_save(
	VentureDatabase		 *self,
	VentureEntity		 *entity,
	const VentureActor	 *actor,
	GError			**error
);

/**
 * venture_database_get:
 * @self: a #VentureDatabase
 * @entity_type: the record type
 * @id: the primary key
 * @error: (out) (optional): return location for a #GError
 *
 * Fetches one record by primary key, including a soft-deleted one.
 *
 * Returns: (transfer full) (nullable): the record, or %NULL if absent
 */
VentureEntity *
venture_database_get(
	VentureDatabase	 *self,
	GType		  entity_type,
	gint64		  id,
	GError		**error
);

/**
 * venture_database_get_by_uuid:
 * @self: a #VentureDatabase
 * @entity_type: the record type
 * @uuid: the external identifier
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer full) (nullable): the record, or %NULL if absent
 */
VentureEntity *
venture_database_get_by_uuid(
	VentureDatabase	 *self,
	GType		  entity_type,
	const gchar	 *uuid,
	GError		**error
);

/**
 * venture_database_find_one:
 * @self: a #VentureDatabase
 * @query: the query to run
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer full) (nullable): the first matching record, or %NULL
 */
VentureEntity *
venture_database_find_one(
	VentureDatabase	 *self,
	VentureQuery	 *query,
	GError		**error
);

/**
 * venture_database_find:
 * @self: a #VentureDatabase
 * @query: the query to run
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer full) (element-type VentureEntity) (nullable): the
 *   matching records, or %NULL on error
 */
GPtrArray *
venture_database_find(
	VentureDatabase	 *self,
	VentureQuery	 *query,
	GError		**error
);

/**
 * venture_database_count:
 * @self: a #VentureDatabase
 * @query: the query to count
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: the number of matching records, or -1 on error
 */
gint64
venture_database_count(
	VentureDatabase	 *self,
	VentureQuery	 *query,
	GError		**error
);

/**
 * venture_database_delete:
 * @self: a #VentureDatabase
 * @entity: the record to remove
 * @actor: (nullable): who is responsible
 * @error: (out) (optional): return location for a #GError
 *
 * Soft deletes a record by stamping its deletion time. The row survives, so
 * a report over a past period still reconstructs correctly and an accidental
 * deletion is recoverable.
 *
 * Returns: %TRUE on success
 */
gboolean
venture_database_delete(
	VentureDatabase		 *self,
	VentureEntity		 *entity,
	const VentureActor	 *actor,
	GError			**error
);

/**
 * venture_database_restore:
 * @self: a #VentureDatabase
 * @entity: the record to bring back
 * @actor: (nullable): who is responsible
 * @error: (out) (optional): return location for a #GError
 *
 * Clears a record's deletion stamp.
 *
 * Returns: %TRUE on success
 */
gboolean
venture_database_restore(
	VentureDatabase		 *self,
	VentureEntity		 *entity,
	const VentureActor	 *actor,
	GError			**error
);

/**
 * venture_database_purge:
 * @self: a #VentureDatabase
 * @entity: the record to destroy
 * @actor: (nullable): who is responsible
 * @error: (out) (optional): return location for a #GError
 *
 * Physically removes a row. Not reachable from the API or the AI tools; it
 * exists for the test suite and for a deliberate operator-run purge of
 * genuinely worthless data.
 *
 * Returns: %TRUE on success
 */
gboolean
venture_database_purge(
	VentureDatabase		 *self,
	VentureEntity		 *entity,
	const VentureActor	 *actor,
	GError			**error
);

/**
 * venture_database_save_ledger_transaction:
 * @self: a #VentureDatabase
 * @entries: (element-type VentureLedgerEntry): the lines of one transaction
 * @actor: (nullable): who is responsible
 * @error: (out) (optional): return location for a #GError
 *
 * Writes a set of ledger lines as one transaction, refusing the whole set
 * with %VENTURE_ERROR_BALANCE if the debits do not equal the credits.
 *
 * A half-posted transaction is worse than none: it leaves the books wrong in
 * a way nothing downstream can detect. This is the only way ledger lines are
 * written.
 *
 * Returns: %TRUE on success
 */
gboolean
venture_database_save_ledger_transaction(
	VentureDatabase		 *self,
	GPtrArray		 *entries,
	const VentureActor	 *actor,
	GError			**error
);

/* --- Aggregation --------------------------------------------------------- */

/**
 * venture_database_sum_money:
 * @self: a #VentureDatabase
 * @query: which records to total
 * @field: the money field to sum
 * @error: (out) (optional): return location for a #GError
 *
 * Totals a money field across matching records, in the database rather than
 * by fetching rows.
 *
 * Records in a different currency from the majority are excluded rather than
 * added, because summing across currencies without a rate would produce a
 * confidently wrong number. The currency actually used is the one most rows
 * carry.
 *
 * Returns: (transfer full) (nullable): the total, or %NULL on error
 */
VentureMoney *
venture_database_sum_money(
	VentureDatabase	 *self,
	VentureQuery	 *query,
	const gchar	 *field,
	GError		**error
);

/**
 * venture_database_execute:
 * @self: a #VentureDatabase
 * @sql: the statement
 * @params: (nullable) (element-type OrmValue): bound parameters
 * @error: (out) (optional): return location for a #GError
 *
 * Runs a statement under the database lock.
 *
 * Returns: %TRUE on success
 */
gboolean
venture_database_execute(
	VentureDatabase	 *self,
	const gchar	 *sql,
	GList		 *params,
	GError		**error
);

/**
 * venture_database_query_raw:
 * @self: a #VentureDatabase
 * @sql: the statement
 * @params: (nullable) (element-type OrmValue): bound parameters
 * @error: (out) (optional): return location for a #GError
 *
 * Runs a query under the database lock. Used by the reporting engine for
 * aggregates that do not map onto #VentureQuery.
 *
 * Returns: (transfer full) (nullable): the result set, or %NULL on error
 */
OrmResult *
venture_database_query_raw(
	VentureDatabase	 *self,
	const gchar	 *sql,
	GList		 *params,
	GError		**error
);

G_END_DECLS

#endif /* VENTURE_DATABASE_H */
