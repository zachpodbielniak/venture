/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_ACCOUNTING_APPROVAL_SERVICE_H
#define VENTURE_ACCOUNTING_APPROVAL_SERVICE_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
/**
 * venture_accounting_approval_check_write:
 * @database: database owning the operation
 * @record: candidate record
 * @removal: whether removing the record
 * @error: (out) (optional): rejection
 *
 * Consumes an exact-record service permit. Generic writers cannot create,
 * edit or delete approval evidence.
 * Returns: whether the write is authorized
 */
gboolean venture_accounting_approval_check_write(VentureDatabase *database, VentureEntity *record, gboolean removal, GError **error);
/**
 * venture_accounting_approval_allow:
 * @database: database owning the operation
 * @action: post or pay
 * @entity: proposed financial record
 * @details: (nullable) (element-type VentureEntity): ordered journal lines or allocations
 * @actor: (nullable): initiating actor; NULL identifies internal derived work
 * @approval: (out) (transfer full) (nullable): operation-owned pending approval
 * @error: (out) (optional): rejection, including a newly recorded proposal
 *
 * Matches the complete serialized proposal. Call before beginning the financial
 * transaction so a newly recorded proposal survives rejection. A successful
 * caller must consume the returned record inside its financial transaction.
 * Returns: whether the operation may proceed
 */
gboolean venture_accounting_approval_allow(VentureDatabase *database, const gchar *action, VentureEntity *entity, GPtrArray *details, const VentureActor *actor, VentureEntity **approval, GError **error);
/**
 * venture_accounting_approval_consume:
 * @database: database owning the active financial transaction
 * @approval: (nullable): approval returned for this exact operation
 * @actor: (nullable): approving actor
 * @error: (out) (optional): persistence or concurrency failure
 *
 * Marks approval applied. The caller must roll back the financial transaction
 * on failure. No global pending-approval state is retained.
 * Returns: whether approval evidence was saved
 */
gboolean venture_accounting_approval_consume(VentureDatabase *database, VentureEntity *approval, const VentureActor *actor, GError **error);
G_END_DECLS
#endif
