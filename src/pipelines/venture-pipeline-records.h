/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_PIPELINE_RECORDS_H
#define VENTURE_PIPELINE_RECORDS_H
G_BEGIN_DECLS
#define VENTURE_TYPE_PIPELINE (venture_pipeline_get_type())
VENTURE_DECLARE_ENTITY(VenturePipeline, venture_pipeline, PIPELINE)
#define VENTURE_TYPE_PIPELINE_STAGE (venture_pipeline_stage_get_type())
VENTURE_DECLARE_ENTITY(VenturePipelineStage, venture_pipeline_stage, PIPELINE_STAGE)
#define VENTURE_TYPE_DEAL_STAGE_ENTRY (venture_deal_stage_entry_get_type())
VENTURE_DECLARE_ENTITY(VentureDealStageEntry, venture_deal_stage_entry, DEAL_STAGE_ENTRY)
#define VENTURE_TYPE_LOSS_REASON (venture_loss_reason_get_type())
VENTURE_DECLARE_ENTITY(VentureLossReason, venture_loss_reason, LOSS_REASON)
/**
 * venture_pipeline_new:
 * Returns: (transfer full): a sales process
 */
/**
 * venture_pipeline_stage_new:
 * Returns: (transfer full): a configurable process stage
 */
/**
 * venture_deal_stage_entry_new:
 * Returns: (transfer full): stage history, writable only by VentureDealService
 */
/**
 * venture_loss_reason_new:
 * Returns: (transfer full): a reason for losing a deal
 */
G_END_DECLS
#endif
