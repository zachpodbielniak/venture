/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_SEQUENCE_SERVICE_PRIVATE_H
#define VENTURE_SEQUENCE_SERVICE_PRIVATE_H

/* Private dispatch continuation; only the database supplies this callback. */
typedef gboolean (*VentureSequenceSaveContinuation)(VentureDatabase *database,
	VentureEntity *record, const VentureActor *actor, GError **error);
/**
 * venture_sequences_save_hook: (skip)
 * @database: the database
 * @record: proposed save
 * @actor: (nullable): audit actor
 * @save: downstream persistence after already-authorized service hooks
 * @handled: (out): whether the service performed the write
 * @error: (out) (optional): error
 * Returns: TRUE if the save is allowed or complete
 */
gboolean venture_sequences_save_hook(VentureDatabase *database, VentureEntity *record,
	const VentureActor *actor, VentureSequenceSaveContinuation save, gboolean *handled, GError **error);

#endif
