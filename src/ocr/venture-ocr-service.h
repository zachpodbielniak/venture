/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_OCR_SERVICE_H
#define VENTURE_OCR_SERVICE_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
#define VENTURE_TYPE_OCR_SERVICE (venture_ocr_service_get_type())
G_DECLARE_FINAL_TYPE(VentureOcrService, venture_ocr_service, VENTURE, OCR_SERVICE, GObject)
/**
 * venture_ocr_service_get:
 * @database: owning repository
 * Returns: (transfer none): per-repository service
 */
VentureOcrService *venture_ocr_service_get(VentureDatabase *database);
/**
 * venture_ocr_service_configure:
 * @self: service
 * @config: live configuration, including attachment root and language
 */
void venture_ocr_service_configure(VentureOcrService *self, VentureConfig *config);
/**
 * venture_ocr_service_enabled:
 * @self: service
 * Returns: whether its configured optional OCR module is enabled
 */
gboolean venture_ocr_service_enabled(VentureOcrService *self);
/**
 * venture_ocr_service_set_engine:
 * @self: service
 * @engine: (nullable): substitute provider, NULL restores the local provider
 *
 * Install before starting work. The service retains the provider.
 */
void venture_ocr_service_set_engine(VentureOcrService *self, VentureOcrEngine *engine);
/**
 * venture_ocr_service_queue:
 * @self: service
 * @document: saved document with expected organization
 * @language: (nullable): language, NULL uses configuration
 * @force: re-extract unchanged bytes, preserving reviewed corrections
 * @actor: (nullable): attribution
 * @error: (out) (optional): validation or access failure
 * Returns: (transfer full) (nullable): durable job, possibly an unchanged existing job
 */
VentureEntity *venture_ocr_service_queue(VentureOcrService *self, VentureEntity *document,
	const gchar *language, gboolean force, const VentureActor *actor, GError **error);
/**
 * venture_ocr_service_step:
 * @self: service
 * @job: durable job with expected version
 * @cancellable: (nullable): cancellation of the active bounded operation
 * @actor: (nullable): attribution
 * @error: (out) (optional): repository or scope failure
 *
 * Processes at most one page on the calling thread. Engine failures are
 * persisted as failed jobs and returned successfully; inspect state/error.
 * Returns: (transfer full) (nullable): updated job
 */
VentureEntity *venture_ocr_service_step(VentureOcrService *self, VentureEntity *job,
	GCancellable *cancellable, const VentureActor *actor, GError **error);
/**
 * venture_ocr_service_control:
 * @self: service
 * @job: saved job
 * @operation: retry, cancel or review
 * @reviewed_text: (nullable): explicit corrected text for review, NULL uses output
 * @actor: (nullable): attribution
 * @error: (out) (optional): failure
 * Returns: (transfer full) (nullable): updated job
 */
VentureEntity *venture_ocr_service_control(VentureOcrService *self, VentureEntity *job,
	const gchar *operation, const gchar *reviewed_text, const VentureActor *actor, GError **error);
/**
 * venture_ocr_service_batch:
 * @self: service
 * @organization: exact organization
 * @after_id: exclusive capture cursor from an earlier batch
 * @limit: maximum inbox rows, from 1 to 25
 * @language: (nullable): language override
 * @force: re-extract unchanged sources
 * @actor: (nullable): attribution
 * @error: (out) (optional): failure
 * Returns: (transfer full) (nullable): frozen durable batch
 */
VentureEntity *venture_ocr_service_batch(VentureOcrService *self, gint64 organization,
	gint64 after_id, guint limit, const gchar *language, gboolean force, const VentureActor *actor, GError **error);
/**
 * venture_ocr_service_batch_step:
 * @self: service
 * @batch: saved batch with expected version
 * @cancel: cancel the batch and its current job
 * @actor: (nullable): attribution
 * @error: (out) (optional): failure
 * Returns: (transfer full) (nullable): batch after at most one page of work
 */
VentureEntity *venture_ocr_service_batch_step(VentureOcrService *self, VentureEntity *batch,
	gboolean cancel, const VentureActor *actor, GError **error);
/**
 * venture_ocr_actions_register:
 * @database: owning repository
 * Registers the shared generic action surface.
 */
void venture_ocr_actions_register(VentureDatabase *database);
G_END_DECLS
#endif
