/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_RECURRING_RECORDS_H
#define VENTURE_RECURRING_RECORDS_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
GType venture_recurring_kind_get_type(void) G_GNUC_CONST;
GType venture_recurring_frequency_get_type(void) G_GNUC_CONST;
GType venture_recurring_occurrence_status_get_type(void) G_GNUC_CONST;
GType venture_collection_step_action_get_type(void) G_GNUC_CONST;
GType venture_collection_case_status_get_type(void) G_GNUC_CONST;
GType venture_financial_batch_kind_get_type(void) G_GNUC_CONST;
#define VENTURE_TYPE_RECURRING_SCHEDULE (venture_recurring_schedule_get_type())
VENTURE_DECLARE_ENTITY(VentureRecurringSchedule, venture_recurring_schedule, RECURRING_SCHEDULE)
#define VENTURE_TYPE_RECURRING_OCCURRENCE (venture_recurring_occurrence_get_type())
VENTURE_DECLARE_ENTITY(VentureRecurringOccurrence, venture_recurring_occurrence, RECURRING_OCCURRENCE)
#define VENTURE_TYPE_COLLECTION_POLICY (venture_collection_policy_get_type())
VENTURE_DECLARE_ENTITY(VentureCollectionPolicy, venture_collection_policy, COLLECTION_POLICY)
#define VENTURE_TYPE_COLLECTION_STEP (venture_collection_step_get_type())
VENTURE_DECLARE_ENTITY(VentureCollectionStep, venture_collection_step, COLLECTION_STEP)
#define VENTURE_TYPE_COLLECTION_CASE (venture_collection_case_get_type())
VENTURE_DECLARE_ENTITY(VentureCollectionCase, venture_collection_case, COLLECTION_CASE)
#define VENTURE_TYPE_COLLECTION_NOTICE (venture_collection_notice_get_type())
VENTURE_DECLARE_ENTITY(VentureCollectionNotice, venture_collection_notice, COLLECTION_NOTICE)
#define VENTURE_TYPE_FINANCIAL_BATCH (venture_financial_batch_get_type())
VENTURE_DECLARE_ENTITY(VentureFinancialBatch, venture_financial_batch, FINANCIAL_BATCH)
G_END_DECLS
#endif
