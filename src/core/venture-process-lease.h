/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_PROCESS_LEASE_H
#define VENTURE_PROCESS_LEASE_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
#define VENTURE_TYPE_PROCESS_LEASE (venture_process_lease_get_type())
G_DECLARE_FINAL_TYPE(VentureProcessLease, venture_process_lease, VENTURE, PROCESS_LEASE, GObject)
/**
 * venture_process_lease_acquire:
 * @database: repository, retained until the lease is released
 * @state_directory: existing application state directory
 * @error: (out) (optional): redacted failure
 *
 * Exclusively leases both state and database for one server or offline
 * maintenance process. PostgreSQL uses a session advisory lock on this
 * repository's connection; SQLite additionally locks its main database inode.
 * An in-memory SQLite repository needs only its state-directory lease.
 * Release on the acquiring thread, after services stop and before closing the
 * repository. This coordinates VENTURE processes, not arbitrary database
 * clients; platform operations must also honor their tenant maintenance lock.
 *
 * Returns: (transfer full) (nullable): lease, or NULL if busy/unsafe/unavailable
 */
VentureProcessLease *venture_process_lease_acquire(VentureDatabase *database,
	const gchar *state_directory, GError **error);
G_END_DECLS
#endif
