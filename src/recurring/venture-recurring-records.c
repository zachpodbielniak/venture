/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

GType
venture_recurring_kind_get_type(void)
{
	static gsize type = 0;
	static const GEnumValue values[] = {
		{ 0, "VENTURE_RECURRING_KIND_INVOICE", "invoice" },
		{ 1, "VENTURE_RECURRING_KIND_BILL", "bill" },
		{ 2, "VENTURE_RECURRING_KIND_EXPENSE", "expense" },
		{ 3, "VENTURE_RECURRING_KIND_JOURNAL", "journal" },
		{ 0, NULL, NULL }
	};
	if (g_once_init_enter(&type))
	{
		GType registered = g_enum_register_static("VentureRecurringKind", values);
		g_once_init_leave(&type, registered);
	}
	return (GType)type;
}

GType
venture_recurring_frequency_get_type(void)
{
	static gsize type = 0;
	static const GEnumValue values[] = {
		{ 0, "VENTURE_RECURRING_FREQUENCY_MONTHLY", "monthly" },
		{ 1, "VENTURE_RECURRING_FREQUENCY_WEEKLY", "weekly" },
		{ 2, "VENTURE_RECURRING_FREQUENCY_DAILY", "daily" },
		{ 3, "VENTURE_RECURRING_FREQUENCY_YEARLY", "yearly" },
		{ 0, NULL, NULL }
	};
	if (g_once_init_enter(&type))
	{
		GType registered = g_enum_register_static("VentureRecurringFrequency", values);
		g_once_init_leave(&type, registered);
	}
	return (GType)type;
}

GType
venture_recurring_occurrence_status_get_type(void)
{
	static gsize type = 0;
	static const GEnumValue values[] = {
		{ 0, "VENTURE_RECURRING_OCCURRENCE_GENERATED", "generated" },
		{ 1, "VENTURE_RECURRING_OCCURRENCE_SKIPPED_CLOSED", "skipped_closed" },
		{ 2, "VENTURE_RECURRING_OCCURRENCE_FAILED", "failed" },
		{ 0, NULL, NULL }
	};
	if (g_once_init_enter(&type))
	{
		GType registered = g_enum_register_static("VentureRecurringOccurrenceStatus", values);
		g_once_init_leave(&type, registered);
	}
	return (GType)type;
}

GType
venture_collection_step_action_get_type(void)
{
	static gsize type = 0;
	static const GEnumValue values[] = {
		{ 0, "VENTURE_COLLECTION_STEP_REMINDER", "reminder" },
		{ 1, "VENTURE_COLLECTION_STEP_STATEMENT", "statement" },
		{ 2, "VENTURE_COLLECTION_STEP_ESCALATE", "escalate" },
		{ 0, NULL, NULL }
	};
	if (g_once_init_enter(&type))
	{
		GType registered = g_enum_register_static("VentureCollectionStepAction", values);
		g_once_init_leave(&type, registered);
	}
	return (GType)type;
}

GType
venture_collection_case_status_get_type(void)
{
	static gsize type = 0;
	static const GEnumValue values[] = {
		{ 0, "VENTURE_COLLECTION_CASE_OPEN", "open" },
		{ 1, "VENTURE_COLLECTION_CASE_HELD", "held" },
		{ 2, "VENTURE_COLLECTION_CASE_DISPUTED", "disputed" },
		{ 3, "VENTURE_COLLECTION_CASE_CLOSED", "closed" },
		{ 0, NULL, NULL }
	};
	if (g_once_init_enter(&type))
	{
		GType registered = g_enum_register_static("VentureCollectionCaseStatus", values);
		g_once_init_leave(&type, registered);
	}
	return (GType)type;
}

GType
venture_financial_batch_kind_get_type(void)
{
	static gsize type = 0;
	static const GEnumValue values[] = {
		{ 0, "VENTURE_FINANCIAL_BATCH_INVOICE", "invoice" },
		{ 1, "VENTURE_FINANCIAL_BATCH_EXPENSE", "expense" },
		{ 0, NULL, NULL }
	};
	if (g_once_init_enter(&type))
	{
		GType registered = g_enum_register_static("VentureFinancialBatchKind", values);
		g_once_init_leave(&type, registered);
	}
	return (GType)type;
}

static const VentureFieldDecl schedule_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", "Reusable schedule"),
	VENTURE_FIELD_ENUM("kind", "Document", NULL, venture_recurring_kind_get_type, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_ENUM("frequency", "Frequency", NULL, venture_recurring_frequency_get_type, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("timezone", "Timezone", "IANA timezone used for calendar days", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("start-at", "Start", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("end-at", "End", "Inclusive last occurrence date", VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("next-run-at", "Next run", "UTC instant of the next occurrence", VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("cycle-index", "Cycle", "Occurrences already considered from start-at", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("auto-post", "Auto post", "Issue, approve or post instead of leaving a draft", VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("paused", "Paused", NULL, VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("template", "Template", "JSON document payload; later edits apply only to future occurrences", VENTURE_FIELD_KIND_JSON, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("last-generated-at", "Last generated", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("last-error", "Last error", "Visible failure from the latest attempt")
};
VENTURE_DEFINE_ENTITY(VentureRecurringSchedule, venture_recurring_schedule, schedule_fields)

static const VentureFieldDecl occurrence_fields[] = {
	VENTURE_FIELD_REF("schedule-id", "Schedule", NULL, "recurring_schedule", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("occurrence-key", "Occurrence key", "Durable schedule and calendar-day identity", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION | VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_ENUM("status", "Status", NULL, venture_recurring_occurrence_status_get_type, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("document-type", "Document type", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("document-id", "Document", NULL, VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("generated-at", "Generated at", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("schedule-version", "Schedule version", "Version of the template used for this occurrence", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("error", "Error", NULL)
};
VENTURE_DEFINE_ENTITY(VentureRecurringOccurrence, venture_recurring_occurrence, occurrence_fields)

static const VentureFieldDecl policy_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", NULL),
	VENTURE_FIELD("active", "Active", NULL, VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("timezone", "Timezone", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("statement-day", "Statement day", "1-28; zero disables the monthly statement", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VentureCollectionPolicy, venture_collection_policy, policy_fields)

static const VentureFieldDecl step_fields[] = {
	VENTURE_FIELD_REF("policy-id", "Policy", NULL, "collection_policy", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("day-offset", "Day offset", "Negative is before due; zero is the due date", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_ENUM("action", "Action", NULL, venture_collection_step_action_get_type, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("template-name", "Mail template", "Organization mail_template name", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("active", "Active", NULL, VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VentureCollectionStep, venture_collection_step, step_fields)

static const VentureFieldDecl case_fields[] = {
	VENTURE_FIELD_REF("invoice-id", "Invoice", NULL, "invoice", VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION),
	VENTURE_FIELD_REF("policy-id", "Policy", NULL, "collection_policy", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("owner", "Owner", "Operator responsible for follow-up", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED | VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD("promised-at", "Promised payment", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("dispute-notes", "Dispute notes", NULL),
	VENTURE_FIELD_ENUM("status", "Status", NULL, venture_collection_case_status_get_type, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("last-notice-at", "Last notice", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VentureCollectionCase, venture_collection_case, case_fields)

static const VentureFieldDecl notice_fields[] = {
	VENTURE_FIELD_REF("invoice-id", "Invoice", NULL, "invoice", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("policy-id", "Policy", NULL, "collection_policy", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("step-id", "Step", NULL, "collection_step", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("case-id", "Case", NULL, "collection_case", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("customer-id", "Customer", NULL, "company", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("occurrence-key", "Occurrence key", "Idempotent reminder or statement identity", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION | VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_ENUM("action", "Action", NULL, venture_collection_step_action_get_type, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_REF("mail-message-id", "Mail message", NULL, "mail_message", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("queued-at", "Queued at", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("last-error", "Last error", "Retained delivery or eligibility failure")
};
VENTURE_DEFINE_ENTITY(VentureCollectionNotice, venture_collection_notice, notice_fields)

static const VentureFieldDecl batch_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", "Reusable mapping and payload"),
	VENTURE_FIELD_ENUM("kind", "Kind", NULL, venture_financial_batch_kind_get_type, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("format", "Format", "json or csv", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("payload", "Payload", "CSV or JSON documents"),
	VENTURE_FIELD("auto-post", "Auto post", "Issue invoices after every document validates", VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("last-result", "Last result", "Counts and per-document identities from the last apply", VENTURE_FIELD_KIND_JSON, VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VentureFinancialBatch, venture_financial_batch, batch_fields)
