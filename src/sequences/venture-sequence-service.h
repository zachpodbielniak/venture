/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_SEQUENCE_SERVICE_H
#define VENTURE_SEQUENCE_SERVICE_H
G_BEGIN_DECLS
#define VENTURE_TYPE_SEQUENCE_SERVICE (venture_sequence_service_get_type())
G_DECLARE_FINAL_TYPE(VentureSequenceService, venture_sequence_service, VENTURE, SEQUENCE_SERVICE, GObject)
/**
 * venture_sequence_service_get:
 * @database: the owning database
 * Returns: (transfer none): its single sequence service
 */
VentureSequenceService *venture_sequence_service_get(VentureDatabase *database);
/**
 * venture_sequence_service_enroll:
 * @self: the service
 * @enrollment: an unsaved enrollment with sequence, contact and optional context
 * @actor: (nullable): audit actor
 * @error: (out) (optional): error
 * Returns: TRUE when enrollment and scheduling commit together
 */
gboolean venture_sequence_service_enroll(VentureSequenceService *self,
	VentureEntity *enrollment, const VentureActor *actor, GError **error);
/**
 * venture_sequences_save_hook: (skip)
 * @database: the database
 * @record: proposed save
 * @actor: (nullable): audit actor
 * @handled: (out): whether the service performed the write
 * @error: (out) (optional): error
 * Returns: TRUE if the save is allowed or complete
 */
gboolean venture_sequences_save_hook(VentureDatabase *database, VentureEntity *record,
	const VentureActor *actor, gboolean *handled, GError **error);
/**
 * venture_sequences_check_removal: (skip)
 * @record: record to delete, restore or purge
 * @error: (out) (optional): error
 * Returns: TRUE if removal preserves sequence evidence
 */
gboolean venture_sequences_check_removal(VentureEntity *record, GError **error);
G_END_DECLS
#endif
