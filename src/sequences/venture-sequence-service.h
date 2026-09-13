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
 * venture_sequences_check_removal: (skip)
 * @record: record to delete, restore or purge
 * @error: (out) (optional): error
 * Returns: TRUE if removal preserves sequence evidence
 */
gboolean venture_sequences_check_removal(VentureEntity *record, GError **error);
/**
 * venture_sequence_service_run_due:
 * @self: the service
 * @organization_id: exactly one organization
 * @as_of: inclusive execution cutoff
 * @actor: (nullable): audit actor
 * @error: (out) (optional): error
 * Returns: steps processed, or -1 on a transactional failure
 */
gint venture_sequence_service_run_due(VentureSequenceService *self, gint64 organization_id,
	GDateTime *as_of, const VentureActor *actor, GError **error);
/**
 * venture_sequence_service_transition:
 * @self: the service
 * @organization_id: exactly one organization
 * @enrollment_id: target enrollment
 * @action: pause, resume, exit or goal
 * @reason: (nullable): operator explanation
 * @actor: (nullable): audit actor
 * @error: (out) (optional): error
 * Returns: TRUE if the transition commits
 */
gboolean venture_sequence_service_transition(VentureSequenceService *self, gint64 organization_id,
	gint64 enrollment_id, const gchar *action, const gchar *reason,
	const VentureActor *actor, GError **error);
/**
 * venture_sequences_register_reports:
 * @registry: the report registry
 *
 * Adds sequence cohort performance and failed delivery reports.
 */
void venture_sequences_register_reports(VentureReportRegistry *registry);
G_END_DECLS
#endif
