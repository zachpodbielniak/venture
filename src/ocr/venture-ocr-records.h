/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_OCR_RECORDS_H
#define VENTURE_OCR_RECORDS_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
#define VENTURE_TYPE_OCR_JOB (venture_ocr_job_get_type())
G_DECLARE_FINAL_TYPE(VentureOcrJob, venture_ocr_job, VENTURE, OCR_JOB, VentureEntity)
#define VENTURE_TYPE_OCR_BATCH (venture_ocr_batch_get_type())
G_DECLARE_FINAL_TYPE(VentureOcrBatch, venture_ocr_batch, VENTURE, OCR_BATCH, VentureEntity)
/**
 * venture_ocr_job_new:
 * Returns: (transfer full): an empty job; use the OCR service to queue it
 */
VentureOcrJob *venture_ocr_job_new(void);
/**
 * venture_ocr_batch_new:
 * Returns: (transfer full): an empty batch; use the OCR service to queue it
 */
VentureOcrBatch *venture_ocr_batch_new(void);
G_END_DECLS
#endif
