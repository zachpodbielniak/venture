/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_DATABASE_SNAPSHOT_PRIVATE_H
#define VENTURE_DATABASE_SNAPSHOT_PRIVATE_H
#include "venture.h"
/* Archival persistence only: the caller validates the complete graph and owns
 * a transaction. These never dispatch workflows, automations or provider calls. */
gboolean venture_database_snapshot_insert(VentureDatabase *database, VentureEntity *record, GError **error);
gboolean venture_database_snapshot_update(VentureDatabase *database, VentureEntity *record, gint64 expected_version, GError **error);
#endif
