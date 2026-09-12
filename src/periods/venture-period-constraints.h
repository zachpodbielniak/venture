/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_PERIOD_CONSTRAINTS_H
#define VENTURE_PERIOD_CONSTRAINTS_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

G_BEGIN_DECLS

/**
 * venture_period_constraints_migrate:
 * @connection: the locked schema connection
 * @entity_type: the record type being migrated
 * @error: (out) (optional): failure details
 *
 * Replaces legacy global uniqueness with metadata-derived organization
 * indexes. SQLite retains the original table definition, indexes, triggers
 * and autoincrement watermark; PostgreSQL drops only the legacy constraint.
 * The replacement and new indexes are one savepoint and preserve every row.
 *
 * Returns: %TRUE on success
 */
gboolean venture_period_constraints_migrate(OrmConnection *connection,
	GType entity_type, GError **error);

G_END_DECLS
#endif
