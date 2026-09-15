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
 *
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
 *
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
 *
 * Returns: whether approval evidence was saved
 */
gboolean venture_accounting_approval_consume(VentureDatabase *database, VentureEntity *approval, const VentureActor *actor, GError **error);
/**
 * VentureAccountingOperation: (skip)
 * A lexical, thread-bound approval and transaction for one business command.
 * This opaque C scope has no registered boxed type and must be finished or
 * freed synchronously on its owning thread; garbage-collected ownership cannot
 * preserve its transaction and lock lifetime.
 */
typedef struct _VentureAccountingOperation VentureAccountingOperation;
/**
 * venture_accounting_operation_begin: (skip)
 * @database: database owning the operation
 * @operation: stable service and command name
 * @subject: (nullable): full proposed source record
 * @details: (nullable) (element-type VentureEntity): ordered proposed child records
 * @arguments: (transfer none) (nullable): exact command arguments; consumes a floating reference
 * @organization: legal entity, or zero for an explicitly all-organization command
 * @actor: (nullable): initiating actor
 * @error: (out) (optional): rejection or storage failure
 *
 * Native C API returning a lexical scope; see #VentureAccountingOperation.
 *
 * Binds consent to the complete command and stable database state before any
 * business writes. A rejection commits only the pending proposal. Authorized
 * work owns a serializable transaction; nested services share its authority.
 *
 * Returns: (transfer full) (nullable): lexical operation scope
 */
VentureAccountingOperation *venture_accounting_operation_begin(VentureDatabase *database,
	const gchar *operation, VentureEntity *subject, GPtrArray *details,
	GVariant *arguments, gint64 organization, const VentureActor *actor, GError **error);
/**
 * venture_accounting_operation_finish: (skip)
 * @operation: lexical operation scope
 * @error: (out) (optional): failure
 *
 * Native C scope operation; the opaque scope must remain on its owning thread.
 * Consumes consent in the same transaction as the business changes.
 *
 * Returns: whether the operation committed successfully
 */
gboolean venture_accounting_operation_finish(VentureAccountingOperation *operation, GError **error);
/**
 * venture_accounting_operation_free: (skip)
 * @operation: (transfer full) (nullable): lexical operation scope
 *
 * Native C scope cleanup; must run synchronously on the originating thread.
 * Rolls back an unfinished operation, preserving its previously committed consent.
 */
void venture_accounting_operation_free(VentureAccountingOperation *operation);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(VentureAccountingOperation, venture_accounting_operation_free)
/**
 * venture_accounting_operation_is_financial:
 * @record: proposed record
 *
 * Returns: whether generic writes can initiate accounting work
 */
gboolean venture_accounting_operation_is_financial(VentureEntity *record);
/**
 * venture_accounting_operation_suspend:
 * @database: database emitting callbacks
 *
 * Prevents synchronous event subscribers inheriting a user's financial consent.
 */
void venture_accounting_operation_suspend(VentureDatabase *database);
/**
 * venture_accounting_operation_resume:
 * @database: database that finished emitting callbacks
 *
 * Balances venture_accounting_operation_suspend().
 */
void venture_accounting_operation_resume(VentureDatabase *database);
/**
 * venture_accounting_operation_is_approved:
 * @database: owning database
 *
 * Returns: whether an approved whole operation is active
 */
gboolean venture_accounting_operation_is_approved(VentureDatabase *database);
/**
 * venture_accounting_operation_guard_write:
 * @database: database owning a candidate write
 * @record: candidate record
 * @actor: (nullable): writer
 * @error: (out) (optional): rejection
 *
 * Prevents a callback or another principal altering snapshotted inputs under
 * an active approval. Ordinary draft edits outside a scope remain permitted.
 *
 * Returns: whether the active scope permits this write
 */
gboolean venture_accounting_operation_guard_write(VentureDatabase *database, VentureEntity *record,
	const VentureActor *actor, GError **error);
G_END_DECLS
#endif
