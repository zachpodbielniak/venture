/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_CLOSE_SERVICE_H
#define VENTURE_CLOSE_SERVICE_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS

#define VENTURE_TYPE_CLOSE_SERVICE (venture_close_service_get_type())
G_DECLARE_FINAL_TYPE(VentureCloseService, venture_close_service, VENTURE, CLOSE_SERVICE, GObject)

/**
 * venture_close_service_get:
 * @database: the owning repository
 * Returns: (transfer none): its close workspace service
 */
VentureCloseService *venture_close_service_get(VentureDatabase *database);

/**
 * venture_close_service_install:
 * @context: the application context
 *
 * Registers subledger close checks on the period checklist.
 */
void venture_close_service_install(VentureContext *context);

/**
 * venture_close_service_open:
 * @self: the service
 * @period_id: a fiscal period
 * @currency: (nullable): book currency, default USD
 * @actor: (nullable): the audit actor
 * @error: (out) (optional): failure
 *
 * Creates the workspace and its checklist tasks in one transaction.
 * Returns: (transfer full) (nullable): the persisted workspace
 */
VentureEntity *venture_close_service_open(VentureCloseService *self, gint64 period_id,
	const gchar *currency, const VentureActor *actor, GError **error);

/**
 * venture_close_service_run_checks:
 * @self: the service
 * @workspace: a persisted workspace
 * @actor: (nullable): the audit actor
 * @error: (out) (optional): failure
 *
 * Recomputes bank, AR/AP, suspense, tax, depreciation, deferral and TB
 * tie-outs and writes discrepancies for unexplained differences.
 * Returns: %TRUE if every check is clean or already explained
 */
gboolean venture_close_service_run_checks(VentureCloseService *self, VentureEntity *workspace,
	const VentureActor *actor, GError **error);

/**
 * venture_close_service_complete_task:
 * @self: the service
 * @task: a persisted task
 * @waive: mark waived rather than done
 * @notes: (nullable): workpaper notes
 * @actor: (nullable): the audit actor
 * @error: (out) (optional): failure
 * Returns: %TRUE on success
 */
gboolean venture_close_service_complete_task(VentureCloseService *self, VentureEntity *task,
	gboolean waive, const gchar *notes, const VentureActor *actor, GError **error);

/**
 * venture_close_service_explain:
 * @self: the service
 * @discrepancy: a persisted discrepancy
 * @explanation: why the difference exists
 * @correction_type: (nullable): record type of a correcting document
 * @correction_id: correcting record, or 0
 * @actor: (nullable): the audit actor
 * @error: (out) (optional): failure
 * Returns: %TRUE on success
 */
gboolean venture_close_service_explain(VentureCloseService *self, VentureEntity *discrepancy,
	const gchar *explanation, const gchar *correction_type, gint64 correction_id,
	const VentureActor *actor, GError **error);

/**
 * venture_close_service_add_workpaper:
 * @self: the service
 * @workspace_id: the workspace
 * @task_id: optional task, or 0
 * @title: the workpaper title
 * @body: (nullable): notes
 * @document_id: optional filed document, or 0
 * @actor: (nullable): the audit actor
 * @error: (out) (optional): failure
 * Returns: (transfer full) (nullable): the persisted workpaper
 */
VentureEntity *venture_close_service_add_workpaper(VentureCloseService *self, gint64 workspace_id,
	gint64 task_id, const gchar *title, const gchar *body, gint64 document_id,
	const VentureActor *actor, GError **error);

/**
 * venture_close_service_sign:
 * @self: the service
 * @workspace: a persisted workspace
 * @role: preparer or reviewer
 * @actor: the named signer
 * @error: (out) (optional): failure
 * Returns: %TRUE on success
 */
gboolean venture_close_service_sign(VentureCloseService *self, VentureEntity *workspace,
	const gchar *role, const VentureActor *actor, GError **error);

/**
 * venture_close_service_complete:
 * @self: the service
 * @workspace: a persisted workspace
 * @actor: the closer
 * @error: (out) (optional): failure
 *
 * Requires both signoffs, a balanced TB, tied subledgers and no open
 * discrepancies, then closes the fiscal period with its report pack.
 * Returns: %TRUE on success
 */
gboolean venture_close_service_complete(VentureCloseService *self, VentureEntity *workspace,
	const VentureActor *actor, GError **error);

/**
 * venture_close_service_reopen:
 * @self: the service
 * @workspace: a completed or signed workspace
 * @actor: an actor holding periods.reopen
 * @error: (out) (optional): failure
 * Returns: %TRUE on success
 */
gboolean venture_close_service_reopen(VentureCloseService *self, VentureEntity *workspace,
	const VentureActor *actor, GError **error);

/**
 * venture_close_service_pack:
 * @self: the service
 * @workspace: a persisted workspace
 * @error: (out) (optional): failure
 * Returns: (transfer full) (nullable): the report pack as JSON
 */
JsonNode *venture_close_service_pack(VentureCloseService *self, VentureEntity *workspace,
	GError **error);

/**
 * venture_close_save_hook: (skip)
 * Returns: %TRUE if allowed or completed
 */
gboolean venture_close_save_hook(VentureDatabase *database, VentureEntity *record,
	const VentureActor *actor, gboolean *handled, GError **error);

/**
 * venture_close_register_reports:
 * @registry: the report registry
 */
void venture_close_register_reports(VentureReportRegistry *registry);

/**
 * venture_close_actions_register:
 * @database: repository owning the action registry
 *
 * Registers metadata-driven finance actions backed by the close service.
 * Each operation owns its transaction and cannot be staged.
 */
void venture_close_actions_register(VentureDatabase *database);

G_END_DECLS
#endif
