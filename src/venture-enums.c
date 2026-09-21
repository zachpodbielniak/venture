/*
 * venture-enums.c - GType registration and helpers for VENTURE enumerations
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Each enumeration is registered lazily and exactly once via
 * g_once_init_enter(), so the types are safe to first touch from any thread.
 * The values arrays live at file scope rather than inside the accessor so
 * the registration macro stays readable; the arrays are const and are never
 * copied by GLib, which requires them to outlive the type, and file-scope
 * statics trivially do.
 */

#include "venture.h"

#include <string.h>

/*
 * VENTURE_DEFINE_ENUM_TYPE:
 * @func: the accessor function name, e.g. venture_task_status_get_type
 * @name: the registered GType name, e.g. "VentureTaskStatus"
 * @array: a file-scope GEnumValue array terminated by a zeroed entry
 *
 * Emits a thread-safe, register-once GType accessor for an enumeration.
 */
#define VENTURE_DEFINE_ENUM_TYPE(func, name, array)                            \
GType                                                                          \
func(void)                                                                     \
{                                                                              \
	static gsize venture_enum_type_id = 0;                                 \
                                                                               \
	if (g_once_init_enter(&venture_enum_type_id))                          \
	{                                                                      \
		GType venture_registered;                                      \
                                                                               \
		venture_registered = g_enum_register_static(name, array);      \
		g_once_init_leave(&venture_enum_type_id, venture_registered);  \
	}                                                                      \
                                                                               \
	return (GType)venture_enum_type_id;                                    \
}

/*
 * VENTURE_DEFINE_FLAGS_TYPE:
 *
 * As above, but for a GFlags type backed by a GFlagsValue array.
 */
#define VENTURE_DEFINE_FLAGS_TYPE(func, name, array)                           \
GType                                                                          \
func(void)                                                                     \
{                                                                              \
	static gsize venture_flags_type_id = 0;                                \
                                                                               \
	if (g_once_init_enter(&venture_flags_type_id))                         \
	{                                                                      \
		GType venture_registered;                                      \
                                                                               \
		venture_registered = g_flags_register_static(name, array);     \
		g_once_init_leave(&venture_flags_type_id, venture_registered); \
	}                                                                      \
                                                                               \
	return (GType)venture_flags_type_id;                                   \
}

/* Shorthand so the tables below stay one line per value. */
#define VE(sym, nick) { sym, #sym, nick }
#define VE_END { 0, NULL, NULL }

/* --- Organisation -------------------------------------------------------- */

static const GEnumValue venture_organization_kind_values[] = {
	VE(VENTURE_ORGANIZATION_KIND_PERSONAL,        "personal"),
	VE(VENTURE_ORGANIZATION_KIND_SOLE_PROPRIETOR, "sole_proprietor"),
	VE(VENTURE_ORGANIZATION_KIND_LLC,             "llc"),
	VE(VENTURE_ORGANIZATION_KIND_S_CORP,          "s_corp"),
	VE(VENTURE_ORGANIZATION_KIND_C_CORP,          "c_corp"),
	VE(VENTURE_ORGANIZATION_KIND_PARTNERSHIP,     "partnership"),
	VE(VENTURE_ORGANIZATION_KIND_NONPROFIT,       "nonprofit"),
	VE(VENTURE_ORGANIZATION_KIND_TRUST,           "trust"),
	VE_END
};

VENTURE_DEFINE_ENUM_TYPE(venture_organization_kind_get_type,
                         "VentureOrganizationKind",
                         venture_organization_kind_values)

/* --- Venture lifecycle --------------------------------------------------- */

static const GEnumValue venture_venture_status_values[] = {
	VE(VENTURE_VENTURE_STATUS_IDEA,          "idea"),
	VE(VENTURE_VENTURE_STATUS_PLANNING,      "planning"),
	VE(VENTURE_VENTURE_STATUS_BUILDING,      "building"),
	VE(VENTURE_VENTURE_STATUS_ACTIVE,        "active"),
	VE(VENTURE_VENTURE_STATUS_PAUSED,        "paused"),
	VE(VENTURE_VENTURE_STATUS_WINDING_DOWN,  "winding_down"),
	VE(VENTURE_VENTURE_STATUS_ARCHIVED,      "archived"),
	VE_END
};

VENTURE_DEFINE_ENUM_TYPE(venture_venture_status_get_type,
                         "VentureVentureStatus",
                         venture_venture_status_values)

/* --- Accounting ---------------------------------------------------------- */

static const GEnumValue venture_account_kind_values[] = {
	VE(VENTURE_ACCOUNT_KIND_ASSET,     "asset"),
	VE(VENTURE_ACCOUNT_KIND_LIABILITY, "liability"),
	VE(VENTURE_ACCOUNT_KIND_EQUITY,    "equity"),
	VE(VENTURE_ACCOUNT_KIND_INCOME,    "income"),
	VE(VENTURE_ACCOUNT_KIND_EXPENSE,   "expense"),
	VE_END
};

VENTURE_DEFINE_ENUM_TYPE(venture_account_kind_get_type,
                         "VentureAccountKind",
                         venture_account_kind_values)

static const GEnumValue venture_ledger_side_values[] = {
	VE(VENTURE_LEDGER_SIDE_DEBIT,  "debit"),
	VE(VENTURE_LEDGER_SIDE_CREDIT, "credit"),
	VE_END
};

VENTURE_DEFINE_ENUM_TYPE(venture_ledger_side_get_type,
                         "VentureLedgerSide",
                         venture_ledger_side_values)

static const GEnumValue venture_deductibility_values[] = {
	VE(VENTURE_DEDUCTIBILITY_NONE,    "none"),
	VE(VENTURE_DEDUCTIBILITY_FULL,    "full"),
	VE(VENTURE_DEDUCTIBILITY_PARTIAL, "partial"),
	VE(VENTURE_DEDUCTIBILITY_CAPITAL, "capital"),
	VE(VENTURE_DEDUCTIBILITY_REVIEW,  "review"),
	VE_END
};

VENTURE_DEFINE_ENUM_TYPE(venture_deductibility_get_type,
                         "VentureDeductibility",
                         venture_deductibility_values)

/* --- Inventory ----------------------------------------------------------- */

static const GEnumValue venture_inventory_txn_kind_values[] = {
	VE(VENTURE_INVENTORY_TXN_KIND_PURCHASE,   "purchase"),
	VE(VENTURE_INVENTORY_TXN_KIND_PRODUCTION, "production"),
	VE(VENTURE_INVENTORY_TXN_KIND_SALE,       "sale"),
	VE(VENTURE_INVENTORY_TXN_KIND_RETURN,     "return"),
	VE(VENTURE_INVENTORY_TXN_KIND_ADJUSTMENT, "adjustment"),
	VE(VENTURE_INVENTORY_TXN_KIND_WRITE_OFF,  "write_off"),
	VE(VENTURE_INVENTORY_TXN_KIND_TRANSFER,   "transfer"),
	VE_END
};

VENTURE_DEFINE_ENUM_TYPE(venture_inventory_txn_kind_get_type,
                         "VentureInventoryTxnKind",
                         venture_inventory_txn_kind_values)

/* --- CRM ----------------------------------------------------------------- */

static const GEnumValue venture_deal_stage_values[] = {
	VE(VENTURE_DEAL_STAGE_LEAD,        "lead"),
	VE(VENTURE_DEAL_STAGE_QUALIFIED,   "qualified"),
	VE(VENTURE_DEAL_STAGE_PROPOSAL,    "proposal"),
	VE(VENTURE_DEAL_STAGE_NEGOTIATION, "negotiation"),
	VE(VENTURE_DEAL_STAGE_WON,         "won"),
	VE(VENTURE_DEAL_STAGE_LOST,        "lost"),
	VE_END
};

VENTURE_DEFINE_ENUM_TYPE(venture_deal_stage_get_type,
                         "VentureDealStage",
                         venture_deal_stage_values)

static const GEnumValue venture_interaction_kind_values[] = {
	VE(VENTURE_INTERACTION_KIND_NOTE,     "note"),
	VE(VENTURE_INTERACTION_KIND_EMAIL,    "email"),
	VE(VENTURE_INTERACTION_KIND_CALL,     "call"),
	VE(VENTURE_INTERACTION_KIND_MEETING,  "meeting"),
	VE(VENTURE_INTERACTION_KIND_MESSAGE,  "message"),
	VE(VENTURE_INTERACTION_KIND_PURCHASE, "purchase"),
	VE(VENTURE_INTERACTION_KIND_SUPPORT,  "support"),
	VE(VENTURE_INTERACTION_KIND_OUTREACH, "outreach"),
	VE_END
};

VENTURE_DEFINE_ENUM_TYPE(venture_interaction_kind_get_type,
                         "VentureInteractionKind",
                         venture_interaction_kind_values)

/* --- Work tracking ------------------------------------------------------- */

static const GEnumValue venture_ticket_status_values[] = {
	VE(VENTURE_TICKET_STATUS_TRIAGE,      "triage"),
	VE(VENTURE_TICKET_STATUS_TODO,        "todo"),
	VE(VENTURE_TICKET_STATUS_IN_PROGRESS, "in_progress"),
	VE(VENTURE_TICKET_STATUS_BLOCKED,     "blocked"),
	VE(VENTURE_TICKET_STATUS_REVIEW,      "review"),
	VE(VENTURE_TICKET_STATUS_DONE,        "done"),
	VE(VENTURE_TICKET_STATUS_CANCELLED,   "cancelled"),
	VE_END
};

VENTURE_DEFINE_ENUM_TYPE(venture_ticket_status_get_type,
                         "VentureTicketStatus",
                         venture_ticket_status_values)

static const GEnumValue venture_ticket_kind_values[] = {
	VE(VENTURE_TICKET_KIND_INTERNAL, "internal"),
	VE(VENTURE_TICKET_KIND_EXTERNAL, "external"),
	VE_END
};

VENTURE_DEFINE_ENUM_TYPE(venture_ticket_kind_get_type,
                         "VentureTicketKind",
                         venture_ticket_kind_values)

static const GEnumValue venture_issue_type_values[] = {
	VE(VENTURE_ISSUE_TYPE_TASK,     "task"),
	VE(VENTURE_ISSUE_TYPE_SUBTASK,  "subtask"),
	VE(VENTURE_ISSUE_TYPE_STORY,    "story"),
	VE(VENTURE_ISSUE_TYPE_EPIC,     "epic"),
	VE(VENTURE_ISSUE_TYPE_BUG,      "bug"),
	VE(VENTURE_ISSUE_TYPE_RESEARCH, "research"),
	VE_END
};

VENTURE_DEFINE_ENUM_TYPE(venture_issue_type_get_type,
                         "VentureIssueType",
                         venture_issue_type_values)

static const GEnumValue venture_kb_format_values[] = {
	VE(VENTURE_KB_FORMAT_ORG,      "org"),
	VE(VENTURE_KB_FORMAT_MARKDOWN, "markdown"),
	VE(VENTURE_KB_FORMAT_TEXT,     "text"),
	VE(VENTURE_KB_FORMAT_HTML,     "html"),
	VE(VENTURE_KB_FORMAT_PDF,      "pdf"),
	VE(VENTURE_KB_FORMAT_DOCX,     "docx"),
	VE(VENTURE_KB_FORMAT_OTHER,    "other"),
	VE_END
};

VENTURE_DEFINE_ENUM_TYPE(venture_kb_format_get_type,
                         "VentureKbFormat",
                         venture_kb_format_values)

static const GEnumValue venture_kb_article_status_values[] = {
	VE(VENTURE_KB_ARTICLE_STATUS_PUBLISHED, "published"),
	VE(VENTURE_KB_ARTICLE_STATUS_DRAFT,     "draft"),
	VE(VENTURE_KB_ARTICLE_STATUS_ARCHIVED,  "archived"),
	VE_END
};

VENTURE_DEFINE_ENUM_TYPE(venture_kb_article_status_get_type,
                         "VentureKbArticleStatus",
                         venture_kb_article_status_values)

static const GEnumValue venture_forge_kind_values[] = {
	VE(VENTURE_FORGE_KIND_FORGEJO, "forgejo"),
	VE(VENTURE_FORGE_KIND_GITEA,   "gitea"),
	VE(VENTURE_FORGE_KIND_GITHUB,  "github"),
	VE(VENTURE_FORGE_KIND_GITLAB,  "gitlab"),
	VE_END
};

VENTURE_DEFINE_ENUM_TYPE(venture_forge_kind_get_type,
                         "VentureForgeKind",
                         venture_forge_kind_values)

static const GEnumValue venture_forge_runner_values[] = {
	VE(VENTURE_FORGE_RUNNER_AGENT, "agent"),
	VE(VENTURE_FORGE_RUNNER_CLI,   "cli"),
	VE_END
};

VENTURE_DEFINE_ENUM_TYPE(venture_forge_runner_get_type,
                         "VentureForgeRunner",
                         venture_forge_runner_values)

static const GEnumValue venture_forge_run_outcome_values[] = {
	VE(VENTURE_FORGE_RUN_OUTCOME_DRAFT_PR,     "draft_pr"),
	VE(VENTURE_FORGE_RUN_OUTCOME_PUSH_BRANCH,  "push_branch"),
	VE(VENTURE_FORGE_RUN_OUTCOME_LOCAL_BRANCH, "local_branch"),
	VE(VENTURE_FORGE_RUN_OUTCOME_NONE,         "none"),
	VE_END
};

VENTURE_DEFINE_ENUM_TYPE(venture_forge_run_outcome_get_type,
                         "VentureForgeRunOutcome",
                         venture_forge_run_outcome_values)

static const GEnumValue venture_forge_trigger_values[] = {
	VE(VENTURE_FORGE_TRIGGER_MANUAL,         "manual"),
	VE(VENTURE_FORGE_TRIGGER_ON_CREATE,      "on_create"),
	VE(VENTURE_FORGE_TRIGGER_ON_TODO,        "on_todo"),
	VE(VENTURE_FORGE_TRIGGER_ON_IN_PROGRESS, "on_in_progress"),
	VE_END
};

VENTURE_DEFINE_ENUM_TYPE(venture_forge_trigger_get_type,
                         "VentureForgeTrigger",
                         venture_forge_trigger_values)

static const GEnumValue venture_forge_run_state_values[] = {
	VE(VENTURE_FORGE_RUN_STATE_QUEUED,      "queued"),
	VE(VENTURE_FORGE_RUN_STATE_RUNNING,     "running"),
	VE(VENTURE_FORGE_RUN_STATE_SUCCEEDED,   "succeeded"),
	VE(VENTURE_FORGE_RUN_STATE_FAILED,      "failed"),
	VE(VENTURE_FORGE_RUN_STATE_CANCELLED,   "cancelled"),
	VE(VENTURE_FORGE_RUN_STATE_REFUSED,     "refused"),
	VE(VENTURE_FORGE_RUN_STATE_INTERRUPTED, "interrupted"),
	VE_END
};

VENTURE_DEFINE_ENUM_TYPE(venture_forge_run_state_get_type,
                         "VentureForgeRunState",
                         venture_forge_run_state_values)

static const GEnumValue venture_agent_session_state_values[] = {
	VE(VENTURE_AGENT_SESSION_STATE_IDLE,    "idle"),
	VE(VENTURE_AGENT_SESSION_STATE_WORKING, "working"),
	VE(VENTURE_AGENT_SESSION_STATE_CLOSED,  "closed"),
	VE(VENTURE_AGENT_SESSION_STATE_FAILED,  "failed"),
	VE_END
};

VENTURE_DEFINE_ENUM_TYPE(venture_agent_session_state_get_type,
                         "VentureAgentSessionState",
                         venture_agent_session_state_values)

static const GEnumValue venture_forge_link_origin_values[] = {
	VE(VENTURE_FORGE_LINK_ORIGIN_FORGE,   "forge"),
	VE(VENTURE_FORGE_LINK_ORIGIN_VENTURE, "venture"),
	VE_END
};

VENTURE_DEFINE_ENUM_TYPE(venture_forge_link_origin_get_type,
                         "VentureForgeLinkOrigin",
                         venture_forge_link_origin_values)

static const GEnumValue venture_invoice_status_values[] = {
	VE(VENTURE_INVOICE_STATUS_DRAFT, "draft"),
	VE(VENTURE_INVOICE_STATUS_SENT,  "sent"),
	VE(VENTURE_INVOICE_STATUS_PAID,  "paid"),
	VE(VENTURE_INVOICE_STATUS_VOID,  "void"),
	VE(VENTURE_INVOICE_STATUS_PARTIALLY_PAID, "partially_paid"),
	VE_END
};

VENTURE_DEFINE_ENUM_TYPE(venture_invoice_status_get_type,
                         "VentureInvoiceStatus",
                         venture_invoice_status_values)

static const GEnumValue venture_chat_role_values[] = {
	VE(VENTURE_CHAT_ROLE_USER,      "user"),
	VE(VENTURE_CHAT_ROLE_ASSISTANT, "assistant"),
	VE_END
};

VENTURE_DEFINE_ENUM_TYPE(venture_chat_role_get_type,
                         "VentureChatRole",
                         venture_chat_role_values)

static const GEnumValue venture_company_kind_values[] = {
	VE(VENTURE_COMPANY_KIND_CUSTOMER, "customer"),
	VE(VENTURE_COMPANY_KIND_PROSPECT, "prospect"),
	VE(VENTURE_COMPANY_KIND_SUPPLIER, "supplier"),
	VE(VENTURE_COMPANY_KIND_PLATFORM, "platform"),
	VE(VENTURE_COMPANY_KIND_PARTNER,  "partner"),
	VE(VENTURE_COMPANY_KIND_OTHER,    "other"),
	VE_END
};

VENTURE_DEFINE_ENUM_TYPE(venture_company_kind_get_type,
                         "VentureCompanyKind",
                         venture_company_kind_values)

static const GEnumValue venture_priority_values[] = {
	VE(VENTURE_PRIORITY_LOW,    "low"),
	VE(VENTURE_PRIORITY_NORMAL, "normal"),
	VE(VENTURE_PRIORITY_HIGH,   "high"),
	VE(VENTURE_PRIORITY_URGENT, "urgent"),
	VE_END
};

VENTURE_DEFINE_ENUM_TYPE(venture_priority_get_type,
                         "VenturePriority",
                         venture_priority_values)

static const GEnumValue venture_idea_status_values[] = {
	VE(VENTURE_IDEA_STATUS_CAPTURED,    "captured"),
	VE(VENTURE_IDEA_STATUS_RESEARCHING, "researching"),
	VE(VENTURE_IDEA_STATUS_VALIDATED,   "validated"),
	VE(VENTURE_IDEA_STATUS_REJECTED,    "rejected"),
	VE(VENTURE_IDEA_STATUS_PROMOTED,    "promoted"),
	VE_END
};

VENTURE_DEFINE_ENUM_TYPE(venture_idea_status_get_type,
                         "VentureIdeaStatus",
                         venture_idea_status_values)

/* --- Marketing and publishing -------------------------------------------- */

static const GEnumValue venture_campaign_status_values[] = {
	VE(VENTURE_CAMPAIGN_STATUS_DRAFT,     "draft"),
	VE(VENTURE_CAMPAIGN_STATUS_SCHEDULED, "scheduled"),
	VE(VENTURE_CAMPAIGN_STATUS_RUNNING,   "running"),
	VE(VENTURE_CAMPAIGN_STATUS_PAUSED,    "paused"),
	VE(VENTURE_CAMPAIGN_STATUS_COMPLETED, "completed"),
	VE(VENTURE_CAMPAIGN_STATUS_CANCELLED, "cancelled"),
	VE_END
};

VENTURE_DEFINE_ENUM_TYPE(venture_campaign_status_get_type,
                         "VentureCampaignStatus",
                         venture_campaign_status_values)

static const GEnumValue venture_post_status_values[] = {
	VE(VENTURE_POST_STATUS_DRAFT,     "draft"),
	VE(VENTURE_POST_STATUS_REVIEW,    "review"),
	VE(VENTURE_POST_STATUS_SCHEDULED, "scheduled"),
	VE(VENTURE_POST_STATUS_PUBLISHED, "published"),
	VE(VENTURE_POST_STATUS_ARCHIVED,  "archived"),
	VE_END
};

VENTURE_DEFINE_ENUM_TYPE(venture_post_status_get_type,
                         "VenturePostStatus",
                         venture_post_status_values)

static const GEnumValue venture_subscriber_status_values[] = {
	VE(VENTURE_SUBSCRIBER_STATUS_PENDING,      "pending"),
	VE(VENTURE_SUBSCRIBER_STATUS_ACTIVE,       "active"),
	VE(VENTURE_SUBSCRIBER_STATUS_UNSUBSCRIBED, "unsubscribed"),
	VE(VENTURE_SUBSCRIBER_STATUS_BOUNCED,      "bounced"),
	VE(VENTURE_SUBSCRIBER_STATUS_COMPLAINED,   "complained"),
	VE_END
};

VENTURE_DEFINE_ENUM_TYPE(venture_subscriber_status_get_type,
                         "VentureSubscriberStatus",
                         venture_subscriber_status_values)

/* --- Provenance and auditing --------------------------------------------- */

static const GEnumValue venture_actor_kind_values[] = {
	VE(VENTURE_ACTOR_KIND_SYSTEM,     "system"),
	VE(VENTURE_ACTOR_KIND_USER,       "user"),
	VE(VENTURE_ACTOR_KIND_AI,         "ai"),
	VE(VENTURE_ACTOR_KIND_AUTOMATION, "automation"),
	VE(VENTURE_ACTOR_KIND_PLUGIN,     "plugin"),
	VE(VENTURE_ACTOR_KIND_IMPORT,     "import"),
	VE_END
};

VENTURE_DEFINE_ENUM_TYPE(venture_actor_kind_get_type,
                         "VentureActorKind",
                         venture_actor_kind_values)

static const GEnumValue venture_audit_action_values[] = {
	VE(VENTURE_AUDIT_ACTION_CREATE,     "create"),
	VE(VENTURE_AUDIT_ACTION_UPDATE,     "update"),
	VE(VENTURE_AUDIT_ACTION_DELETE,     "delete"),
	VE(VENTURE_AUDIT_ACTION_LOGIN,      "login"),
	VE(VENTURE_AUDIT_ACTION_LOGOUT,     "logout"),
	VE(VENTURE_AUDIT_ACTION_TOOL_CALL,  "tool_call"),
	VE(VENTURE_AUDIT_ACTION_CONFIRM,    "confirm"),
	VE(VENTURE_AUDIT_ACTION_REJECT,     "reject"),
	VE(VENTURE_AUDIT_ACTION_AUTOMATION, "automation"),
	VE(VENTURE_AUDIT_ACTION_EXPORT,     "export"),
	VE_END
};

VENTURE_DEFINE_ENUM_TYPE(venture_audit_action_get_type,
                         "VentureAuditAction",
                         venture_audit_action_values)

/* --- AI ------------------------------------------------------------------ */

static const GEnumValue venture_ai_policy_values[] = {
	VE(VENTURE_AI_POLICY_READ_ONLY,      "read_only"),
	VE(VENTURE_AI_POLICY_CONFIRM_WRITES, "confirm_writes"),
	VE(VENTURE_AI_POLICY_AUTONOMOUS,     "autonomous"),
	VE_END
};

VENTURE_DEFINE_ENUM_TYPE(venture_ai_policy_get_type,
                         "VentureAiPolicy",
                         venture_ai_policy_values)

static const GEnumValue venture_confirmation_state_values[] = {
	VE(VENTURE_CONFIRMATION_STATE_PENDING,  "pending"),
	VE(VENTURE_CONFIRMATION_STATE_APPROVED, "approved"),
	VE(VENTURE_CONFIRMATION_STATE_REJECTED, "rejected"),
	VE(VENTURE_CONFIRMATION_STATE_EXPIRED,  "expired"),
	VE(VENTURE_CONFIRMATION_STATE_FAILED,   "failed"),
	VE_END
};

VENTURE_DEFINE_ENUM_TYPE(venture_confirmation_state_get_type,
                         "VentureConfirmationState",
                         venture_confirmation_state_values)

/* --- Access control ------------------------------------------------------ */

static const GEnumValue venture_user_role_values[] = {
	VE(VENTURE_USER_ROLE_OWNER,   "owner"),
	VE(VENTURE_USER_ROLE_ADMIN,   "admin"),
	VE(VENTURE_USER_ROLE_EDITOR,  "editor"),
	VE(VENTURE_USER_ROLE_VIEWER,  "viewer"),
	VE(VENTURE_USER_ROLE_SERVICE, "service"),
	VE_END
};

VENTURE_DEFINE_ENUM_TYPE(venture_user_role_get_type,
                         "VentureUserRole",
                         venture_user_role_values)

/* --- Declarative schema and querying ------------------------------------- */

static const GEnumValue venture_field_kind_values[] = {
	VE(VENTURE_FIELD_KIND_STRING,    "string"),
	VE(VENTURE_FIELD_KIND_TEXT,      "text"),
	VE(VENTURE_FIELD_KIND_INTEGER,   "integer"),
	VE(VENTURE_FIELD_KIND_DOUBLE,    "double"),
	VE(VENTURE_FIELD_KIND_MONEY,     "money"),
	VE(VENTURE_FIELD_KIND_BOOLEAN,   "boolean"),
	VE(VENTURE_FIELD_KIND_DATE,      "date"),
	VE(VENTURE_FIELD_KIND_DATETIME,  "datetime"),
	VE(VENTURE_FIELD_KIND_ENUM,      "enum"),
	VE(VENTURE_FIELD_KIND_REFERENCE, "reference"),
	VE(VENTURE_FIELD_KIND_JSON,      "json"),
	VE_END
};

VENTURE_DEFINE_ENUM_TYPE(venture_field_kind_get_type,
                         "VentureFieldKind",
                         venture_field_kind_values)

static const GEnumValue venture_aggregate_values[] = {
	VE(VENTURE_AGGREGATE_NONE,           "none"),
	VE(VENTURE_AGGREGATE_SUM,            "sum"),
	VE(VENTURE_AGGREGATE_AVG,            "avg"),
	VE(VENTURE_AGGREGATE_MIN,            "min"),
	VE(VENTURE_AGGREGATE_MAX,            "max"),
	VE(VENTURE_AGGREGATE_COUNT,          "count"),
	VE(VENTURE_AGGREGATE_COUNT_DISTINCT, "count_distinct"),
	VE_END
};

VENTURE_DEFINE_ENUM_TYPE(venture_aggregate_get_type,
                         "VentureAggregate",
                         venture_aggregate_values)

static const GEnumValue venture_filter_op_values[] = {
	VE(VENTURE_FILTER_OP_EQ,       "eq"),
	VE(VENTURE_FILTER_OP_NE,       "ne"),
	VE(VENTURE_FILTER_OP_LT,       "lt"),
	VE(VENTURE_FILTER_OP_LTE,      "lte"),
	VE(VENTURE_FILTER_OP_GT,       "gt"),
	VE(VENTURE_FILTER_OP_GTE,      "gte"),
	VE(VENTURE_FILTER_OP_LIKE,     "like"),
	VE(VENTURE_FILTER_OP_ILIKE,    "ilike"),
	VE(VENTURE_FILTER_OP_IN,       "in"),
	VE(VENTURE_FILTER_OP_NOT_IN,   "not_in"),
	VE(VENTURE_FILTER_OP_IS_NULL,  "is_null"),
	VE(VENTURE_FILTER_OP_NOT_NULL, "not_null"),
	VE(VENTURE_FILTER_OP_BETWEEN,  "between"),
	VE_END
};

VENTURE_DEFINE_ENUM_TYPE(venture_filter_op_get_type,
                         "VentureFilterOp",
                         venture_filter_op_values)

static const GEnumValue venture_sort_direction_values[] = {
	VE(VENTURE_SORT_ASCENDING,  "asc"),
	VE(VENTURE_SORT_DESCENDING, "desc"),
	VE_END
};

VENTURE_DEFINE_ENUM_TYPE(venture_sort_direction_get_type,
                         "VentureSortDirection",
                         venture_sort_direction_values)

/* --- Output and infrastructure ------------------------------------------- */

static const GEnumValue venture_output_format_values[] = {
	VE(VENTURE_OUTPUT_FORMAT_TABLE, "table"),
	VE(VENTURE_OUTPUT_FORMAT_JSON,  "json"),
	VE(VENTURE_OUTPUT_FORMAT_YAML,  "yaml"),
	VE(VENTURE_OUTPUT_FORMAT_CSV,   "csv"),
	VE(VENTURE_OUTPUT_FORMAT_ORG,   "org"),
	VE(VENTURE_OUTPUT_FORMAT_HTML,  "html"),
	VE(VENTURE_OUTPUT_FORMAT_TEXT,  "text"),
	VE_END
};

VENTURE_DEFINE_ENUM_TYPE(venture_output_format_get_type,
                         "VentureOutputFormat",
                         venture_output_format_values)

static const GEnumValue venture_database_backend_values[] = {
	VE(VENTURE_DATABASE_BACKEND_SQLITE,   "sqlite"),
	VE(VENTURE_DATABASE_BACKEND_POSTGRES, "postgres"),
	VE_END
};

VENTURE_DEFINE_ENUM_TYPE(venture_database_backend_get_type,
                         "VentureDatabaseBackend",
                         venture_database_backend_values)

static const GEnumValue venture_plugin_kind_values[] = {
	VE(VENTURE_PLUGIN_KIND_NATIVE,      "native"),
	VE(VENTURE_PLUGIN_KIND_CRISPY,      "crispy"),
	VE(VENTURE_PLUGIN_KIND_DECLARATIVE, "declarative"),
	VE_END
};

static const GEnumValue venture_milestone_status_values[] = {
	VE(VENTURE_MILESTONE_STATUS_PLANNED,   "planned"),
	VE(VENTURE_MILESTONE_STATUS_ACTIVE,    "active"),
	VE(VENTURE_MILESTONE_STATUS_COMPLETED, "completed"),
	VE(VENTURE_MILESTONE_STATUS_CANCELLED, "cancelled"),
	VE_END
};

VENTURE_DEFINE_ENUM_TYPE(venture_milestone_status_get_type,
                         "VentureMilestoneStatus",
                         venture_milestone_status_values)

static const GEnumValue venture_release_status_values[] = {
	VE(VENTURE_RELEASE_STATUS_PLANNED,     "planned"),
	VE(VENTURE_RELEASE_STATUS_IN_PROGRESS, "in_progress"),
	VE(VENTURE_RELEASE_STATUS_RELEASED,    "released"),
	VE(VENTURE_RELEASE_STATUS_YANKED,      "yanked"),
	VE_END
};

VENTURE_DEFINE_ENUM_TYPE(venture_release_status_get_type,
                         "VentureReleaseStatus",
                         venture_release_status_values)

static const GEnumValue venture_build_status_values[] = {
	VE(VENTURE_BUILD_STATUS_QUEUED,    "queued"),
	VE(VENTURE_BUILD_STATUS_RUNNING,   "running"),
	VE(VENTURE_BUILD_STATUS_SUCCEEDED, "succeeded"),
	VE(VENTURE_BUILD_STATUS_FAILED,    "failed"),
	VE(VENTURE_BUILD_STATUS_CANCELLED, "cancelled"),
	VE_END
};

VENTURE_DEFINE_ENUM_TYPE(venture_build_status_get_type,
                         "VentureBuildStatus",
                         venture_build_status_values)

static const GEnumValue venture_build_trigger_values[] = {
	VE(VENTURE_BUILD_TRIGGER_MANUAL,  "manual"),
	VE(VENTURE_BUILD_TRIGGER_WEBHOOK, "webhook"),
	VE(VENTURE_BUILD_TRIGGER_RULE,    "rule"),
	VE(VENTURE_BUILD_TRIGGER_RUN,     "run"),
	VE_END
};

VENTURE_DEFINE_ENUM_TYPE(venture_build_trigger_get_type,
                         "VentureBuildTrigger",
                         venture_build_trigger_values)

static const GEnumValue venture_environment_kind_values[] = {
	VE(VENTURE_ENVIRONMENT_KIND_DEVELOPMENT, "development"),
	VE(VENTURE_ENVIRONMENT_KIND_STAGING,     "staging"),
	VE(VENTURE_ENVIRONMENT_KIND_PRODUCTION,  "production"),
	VE(VENTURE_ENVIRONMENT_KIND_OTHER,       "other"),
	VE_END
};

VENTURE_DEFINE_ENUM_TYPE(venture_environment_kind_get_type,
                         "VentureEnvironmentKind",
                         venture_environment_kind_values)

static const GEnumValue venture_deployment_status_values[] = {
	VE(VENTURE_DEPLOYMENT_STATUS_PENDING,     "pending"),
	VE(VENTURE_DEPLOYMENT_STATUS_IN_PROGRESS, "in_progress"),
	VE(VENTURE_DEPLOYMENT_STATUS_SUCCEEDED,   "succeeded"),
	VE(VENTURE_DEPLOYMENT_STATUS_FAILED,      "failed"),
	VE(VENTURE_DEPLOYMENT_STATUS_ROLLED_BACK, "rolled_back"),
	VE_END
};

VENTURE_DEFINE_ENUM_TYPE(venture_deployment_status_get_type,
                         "VentureDeploymentStatus",
                         venture_deployment_status_values)

static const GEnumValue venture_incident_severity_values[] = {
	VE(VENTURE_INCIDENT_SEVERITY_SEV3, "sev3"),
	VE(VENTURE_INCIDENT_SEVERITY_SEV1, "sev1"),
	VE(VENTURE_INCIDENT_SEVERITY_SEV2, "sev2"),
	VE(VENTURE_INCIDENT_SEVERITY_SEV4, "sev4"),
	VE_END
};

VENTURE_DEFINE_ENUM_TYPE(venture_incident_severity_get_type,
                         "VentureIncidentSeverity",
                         venture_incident_severity_values)

static const GEnumValue venture_incident_status_values[] = {
	VE(VENTURE_INCIDENT_STATUS_OPEN,       "open"),
	VE(VENTURE_INCIDENT_STATUS_MITIGATED,  "mitigated"),
	VE(VENTURE_INCIDENT_STATUS_RESOLVED,   "resolved"),
	VE(VENTURE_INCIDENT_STATUS_POSTMORTEM, "postmortem"),
	VE_END
};

VENTURE_DEFINE_ENUM_TYPE(venture_incident_status_get_type,
                         "VentureIncidentStatus",
                         venture_incident_status_values)

static const GEnumValue venture_dashboard_purpose_values[] = {
	VE(VENTURE_DASHBOARD_PURPOSE_OVERVIEW,  "overview"),
	VE(VENTURE_DASHBOARD_PURPOSE_REPORTING, "reporting"),
	VE(VENTURE_DASHBOARD_PURPOSE_WORK,      "work"),
	VE_END
};

VENTURE_DEFINE_ENUM_TYPE(venture_dashboard_purpose_get_type,
                         "VentureDashboardPurpose",
                         venture_dashboard_purpose_values)

static const GEnumValue venture_dashboard_layout_values[] = {
	VE(VENTURE_DASHBOARD_LAYOUT_THREE_COLUMNS, "three_columns"),
	VE(VENTURE_DASHBOARD_LAYOUT_TWO_COLUMNS,   "two_columns"),
	VE(VENTURE_DASHBOARD_LAYOUT_FOUR_COLUMNS,  "four_columns"),
	VE(VENTURE_DASHBOARD_LAYOUT_ONE_COLUMN,    "one_column"),
	VE_END
};

VENTURE_DEFINE_ENUM_TYPE(venture_dashboard_layout_get_type,
                         "VentureDashboardLayout",
                         venture_dashboard_layout_values)

guint
venture_dashboard_layout_get_columns(VentureDashboardLayout layout)
{
	switch (layout)
	{
	case VENTURE_DASHBOARD_LAYOUT_TWO_COLUMNS:  return 2;
	case VENTURE_DASHBOARD_LAYOUT_FOUR_COLUMNS: return 4;
	case VENTURE_DASHBOARD_LAYOUT_ONE_COLUMN:   return 1;
	case VENTURE_DASHBOARD_LAYOUT_THREE_COLUMNS:
	default:
		return 3;
	}
}

static const GEnumValue venture_widget_span_values[] = {
	VE(VENTURE_WIDGET_SPAN_NORMAL, "normal"),
	VE(VENTURE_WIDGET_SPAN_WIDE,   "wide"),
	VE(VENTURE_WIDGET_SPAN_FULL,   "full"),
	VE_END
};

VENTURE_DEFINE_ENUM_TYPE(venture_widget_span_get_type,
                         "VentureWidgetSpan",
                         venture_widget_span_values)

static const GEnumValue venture_link_kind_values[] = {
	VE(VENTURE_LINK_KIND_RELATED,       "related"),
	VE(VENTURE_LINK_KIND_BLOCKS,        "blocks"),
	VE(VENTURE_LINK_KIND_BLOCKED_BY,    "blocked_by"),
	VE(VENTURE_LINK_KIND_DEPENDS_ON,    "depends_on"),
	VE(VENTURE_LINK_KIND_REQUIRED_BY,   "required_by"),
	VE(VENTURE_LINK_KIND_PARENT_OF,     "parent_of"),
	VE(VENTURE_LINK_KIND_CHILD_OF,      "child_of"),
	VE(VENTURE_LINK_KIND_DUPLICATES,    "duplicates"),
	VE(VENTURE_LINK_KIND_CAUSES,        "causes"),
	VE(VENTURE_LINK_KIND_CAUSED_BY,     "caused_by"),
	VE(VENTURE_LINK_KIND_PRODUCES,      "produces"),
	VE(VENTURE_LINK_KIND_PRODUCED_BY,   "produced_by"),
	VE(VENTURE_LINK_KIND_REFERENCES,    "references"),
	VE(VENTURE_LINK_KIND_REFERENCED_BY, "referenced_by"),
	VE(VENTURE_LINK_KIND_SUPERSEDES,    "supersedes"),
	VE(VENTURE_LINK_KIND_SUPERSEDED_BY, "superseded_by"),
	VE_END
};

VENTURE_DEFINE_ENUM_TYPE(venture_link_kind_get_type,
                         "VentureLinkKind",
                         venture_link_kind_values)

VentureLinkKind
venture_link_kind_inverse(VentureLinkKind kind)
{
	switch (kind)
	{
	case VENTURE_LINK_KIND_BLOCKS:        return VENTURE_LINK_KIND_BLOCKED_BY;
	case VENTURE_LINK_KIND_BLOCKED_BY:    return VENTURE_LINK_KIND_BLOCKS;
	case VENTURE_LINK_KIND_DEPENDS_ON:    return VENTURE_LINK_KIND_REQUIRED_BY;
	case VENTURE_LINK_KIND_REQUIRED_BY:   return VENTURE_LINK_KIND_DEPENDS_ON;
	case VENTURE_LINK_KIND_PARENT_OF:     return VENTURE_LINK_KIND_CHILD_OF;
	case VENTURE_LINK_KIND_CHILD_OF:      return VENTURE_LINK_KIND_PARENT_OF;
	case VENTURE_LINK_KIND_CAUSES:        return VENTURE_LINK_KIND_CAUSED_BY;
	case VENTURE_LINK_KIND_CAUSED_BY:     return VENTURE_LINK_KIND_CAUSES;
	case VENTURE_LINK_KIND_PRODUCES:      return VENTURE_LINK_KIND_PRODUCED_BY;
	case VENTURE_LINK_KIND_PRODUCED_BY:   return VENTURE_LINK_KIND_PRODUCES;
	case VENTURE_LINK_KIND_REFERENCES:    return VENTURE_LINK_KIND_REFERENCED_BY;
	case VENTURE_LINK_KIND_REFERENCED_BY: return VENTURE_LINK_KIND_REFERENCES;
	case VENTURE_LINK_KIND_SUPERSEDES:    return VENTURE_LINK_KIND_SUPERSEDED_BY;
	case VENTURE_LINK_KIND_SUPERSEDED_BY: return VENTURE_LINK_KIND_SUPERSEDES;

	/* Related and duplicates read the same from either end. */
	case VENTURE_LINK_KIND_RELATED:
	case VENTURE_LINK_KIND_DUPLICATES:
	default:
		return kind;
	}
}

gboolean
venture_link_kind_is_symmetric(VentureLinkKind kind)
{
	return venture_link_kind_inverse(kind) == kind;
}

const gchar *
venture_link_kind_to_label(VentureLinkKind kind)
{
	switch (kind)
	{
	case VENTURE_LINK_KIND_RELATED:       return "related to";
	case VENTURE_LINK_KIND_BLOCKS:        return "blocks";
	case VENTURE_LINK_KIND_BLOCKED_BY:    return "blocked by";
	case VENTURE_LINK_KIND_DEPENDS_ON:    return "depends on";
	case VENTURE_LINK_KIND_REQUIRED_BY:   return "required by";
	case VENTURE_LINK_KIND_PARENT_OF:     return "parent of";
	case VENTURE_LINK_KIND_CHILD_OF:      return "child of";
	case VENTURE_LINK_KIND_DUPLICATES:    return "duplicates";
	case VENTURE_LINK_KIND_CAUSES:        return "causes";
	case VENTURE_LINK_KIND_CAUSED_BY:     return "caused by";
	case VENTURE_LINK_KIND_PRODUCES:      return "produces";
	case VENTURE_LINK_KIND_PRODUCED_BY:   return "produced by";
	case VENTURE_LINK_KIND_REFERENCES:    return "references";
	case VENTURE_LINK_KIND_REFERENCED_BY: return "referenced by";
	case VENTURE_LINK_KIND_SUPERSEDES:    return "supersedes";
	case VENTURE_LINK_KIND_SUPERSEDED_BY: return "superseded by";
	default:                              return "linked to";
	}
}

VENTURE_DEFINE_ENUM_TYPE(venture_plugin_kind_get_type,
                         "VenturePluginKind",
                         venture_plugin_kind_values)

static const GFlagsValue venture_column_flags_values[] = {
	VE(VENTURE_COLUMN_FLAG_NONE,        "none"),
	VE(VENTURE_COLUMN_FLAG_PRIMARY_KEY, "primary_key"),
	VE(VENTURE_COLUMN_FLAG_UNIQUE,      "unique"),
	VE(VENTURE_COLUMN_FLAG_INDEXED,     "indexed"),
	VE(VENTURE_COLUMN_FLAG_NOT_NULL,    "not_null"),
	VE(VENTURE_COLUMN_FLAG_IMMUTABLE,   "immutable"),
	VE(VENTURE_COLUMN_FLAG_SENSITIVE,   "sensitive"),
	VE(VENTURE_COLUMN_FLAG_SEARCHABLE,  "searchable"),
	VE(VENTURE_COLUMN_FLAG_TRANSIENT,   "transient"),
	VE(VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION, "unique_organization"),
	VE(VENTURE_COLUMN_FLAG_PERSONAL_OWNER, "personal_owner"),
	VE(VENTURE_COLUMN_FLAG_ASSIGNED_USERNAME, "assigned_username"),
	VE(VENTURE_COLUMN_FLAG_OPTIONAL_PERSONAL_OWNER, "optional_personal_owner"),
	VE(VENTURE_COLUMN_FLAG_HOST_RESOURCE, "host_resource"),
	VE(VENTURE_COLUMN_FLAG_RETAIN_REFERENCE, "retain_reference"),
	VE_END
};

VENTURE_DEFINE_FLAGS_TYPE(venture_column_flags_get_type,
                          "VentureColumnFlags",
                          venture_column_flags_values)

/* --- Nick conversion ----------------------------------------------------- */

const gchar *
venture_enum_to_nick(
	GType	enum_type,
	gint	value
){
	g_autoptr(GEnumClass) enum_class = NULL;
	GEnumValue *enum_value;

	g_return_val_if_fail(G_TYPE_IS_ENUM(enum_type), NULL);

	/* Referencing the class keeps the values array alive for the lookup;
	 * the nick we return points into that array, which is file-scope
	 * static data and therefore outlives the class ref. */
	enum_class = g_type_class_ref(enum_type);
	enum_value = g_enum_get_value(enum_class, value);

	if (NULL == enum_value)
		return NULL;

	return enum_value->value_nick;
}

gboolean
venture_enum_from_nick(
	GType		 enum_type,
	const gchar	*nick,
	gint		*out_value
){
	g_autoptr(GEnumClass) enum_class = NULL;
	g_autofree gchar *normalised = NULL;
	guint i;

	g_return_val_if_fail(G_TYPE_IS_ENUM(enum_type), FALSE);
	g_return_val_if_fail(NULL != nick, FALSE);
	g_return_val_if_fail(NULL != out_value, FALSE);

	enum_class = g_type_class_ref(enum_type);

	/* Match case-insensitively and treat '-' and '_' as equivalent. The
	 * strings compared here come from JSON bodies, YAML files, CLI
	 * arguments and AI tool calls, and every one of those sources has a
	 * different idea of how to spell a multi-word identifier. Being
	 * lenient here is much better than rejecting a request over a dash. */
	normalised = g_ascii_strdown(nick, -1);
	g_strdelimit(normalised, "-", '_');

	for (i = 0; i < enum_class->n_values; i++)
	{
		g_autofree gchar *candidate_nick = NULL;
		g_autofree gchar *candidate_name = NULL;

		candidate_nick = g_ascii_strdown(enum_class->values[i].value_nick, -1);
		g_strdelimit(candidate_nick, "-", '_');

		if (0 == g_strcmp0(candidate_nick, normalised))
		{
			*out_value = enum_class->values[i].value;
			return TRUE;
		}

		/* Also accept the full C identifier, so a caller that read
		 * VENTURE_TASK_STATUS_DONE out of the documentation gets what
		 * it expects rather than an error. */
		candidate_name = g_ascii_strdown(enum_class->values[i].value_name, -1);

		if (0 == g_strcmp0(candidate_name, normalised))
		{
			*out_value = enum_class->values[i].value;
			return TRUE;
		}
	}

	return FALSE;
}

gchar **
venture_enum_list_nicks(GType enum_type)
{
	g_autoptr(GEnumClass) enum_class = NULL;
	g_autoptr(GPtrArray) nicks = NULL;
	guint i;

	g_return_val_if_fail(G_TYPE_IS_ENUM(enum_type), NULL);

	enum_class = g_type_class_ref(enum_type);
	nicks = g_ptr_array_new();

	for (i = 0; i < enum_class->n_values; i++)
		g_ptr_array_add(nicks, g_strdup(enum_class->values[i].value_nick));

	g_ptr_array_add(nicks, NULL);

	return (gchar **)g_ptr_array_free(g_steal_pointer(&nicks), FALSE);
}

/* --- Semantic helpers ---------------------------------------------------- */

gboolean
venture_account_kind_is_debit_normal(VentureAccountKind kind)
{
	/* Assets and expenses grow with debits. Liabilities, equity and
	 * income grow with credits. This single fact is what lets the ledger
	 * turn a signed amount into a correctly-sided entry without the
	 * caller having to think in double-entry terms. */
	switch (kind)
	{
	case VENTURE_ACCOUNT_KIND_ASSET:
	case VENTURE_ACCOUNT_KIND_EXPENSE:
		return TRUE;

	case VENTURE_ACCOUNT_KIND_LIABILITY:
	case VENTURE_ACCOUNT_KIND_EQUITY:
	case VENTURE_ACCOUNT_KIND_INCOME:
	default:
		return FALSE;
	}
}

gboolean
venture_deal_stage_is_closed(VentureDealStage stage)
{
	return (VENTURE_DEAL_STAGE_WON == stage) ||
	       (VENTURE_DEAL_STAGE_LOST == stage);
}

gboolean
venture_venture_status_is_operating(VentureVentureStatus status)
{
	/* Building counts as operating because a venture under construction
	 * is already accruing costs that belong in the portfolio view. */
	switch (status)
	{
	case VENTURE_VENTURE_STATUS_BUILDING:
	case VENTURE_VENTURE_STATUS_ACTIVE:
	case VENTURE_VENTURE_STATUS_WINDING_DOWN:
		return TRUE;

	case VENTURE_VENTURE_STATUS_IDEA:
	case VENTURE_VENTURE_STATUS_PLANNING:
	case VENTURE_VENTURE_STATUS_PAUSED:
	case VENTURE_VENTURE_STATUS_ARCHIVED:
	default:
		return FALSE;
	}
}

guint
venture_filter_op_arity(VentureFilterOp op)
{
	switch (op)
	{
	/* The NULL tests compare against nothing at all. */
	case VENTURE_FILTER_OP_IS_NULL:
	case VENTURE_FILTER_OP_NOT_NULL:
		return 0;

	/* BETWEEN takes an inclusive lower and upper bound. */
	case VENTURE_FILTER_OP_BETWEEN:
		return 2;

	/* Everything else takes exactly one operand. For IN and NOT_IN that
	 * single operand is itself a list, which the query compiler expands
	 * into the right number of placeholders. */
	default:
		return 1;
	}
}

/* --- Workdesk -------------------------------------------------------------- */

static const GEnumValue venture_notification_kind_values[] = {
	VE(VENTURE_NOTIFICATION_KIND_MENTION,  "mention"),
	VE(VENTURE_NOTIFICATION_KIND_ASSIGNED, "assigned"),
	VE(VENTURE_NOTIFICATION_KIND_WATCHED,  "watched"),
	VE(VENTURE_NOTIFICATION_KIND_SLA,      "sla"),
	VE(VENTURE_NOTIFICATION_KIND_BUDGET,   "budget"),
	VE(VENTURE_NOTIFICATION_KIND_RUN,      "run"),
	VE(VENTURE_NOTIFICATION_KIND_SYSTEM,   "system"),
	VE_END
};

VENTURE_DEFINE_ENUM_TYPE(venture_notification_kind_get_type,
                         "VentureNotificationKind",
                         venture_notification_kind_values)

static const GEnumValue venture_sprint_status_values[] = {
	VE(VENTURE_SPRINT_STATUS_PLANNED,   "planned"),
	VE(VENTURE_SPRINT_STATUS_ACTIVE,    "active"),
	VE(VENTURE_SPRINT_STATUS_COMPLETED, "completed"),
	VE_END
};

VENTURE_DEFINE_ENUM_TYPE(venture_sprint_status_get_type,
                         "VentureSprintStatus",
                         venture_sprint_status_values)

static const GEnumValue venture_budget_period_values[] = {
	VE(VENTURE_BUDGET_PERIOD_MONTHLY,  "monthly"),
	VE(VENTURE_BUDGET_PERIOD_WEEKLY,   "weekly"),
	VE(VENTURE_BUDGET_PERIOD_ALL_TIME, "all_time"),
	VE_END
};

VENTURE_DEFINE_ENUM_TYPE(venture_budget_period_get_type,
                         "VentureBudgetPeriod",
                         venture_budget_period_values)

static const GEnumValue venture_sla_state_values[] = {
	VE(VENTURE_SLA_STATE_NONE,     "none"),
	VE(VENTURE_SLA_STATE_OK,       "ok"),
	VE(VENTURE_SLA_STATE_WARNING,  "warning"),
	VE(VENTURE_SLA_STATE_BREACHED, "breached"),
	VE_END
};

VENTURE_DEFINE_ENUM_TYPE(venture_sla_state_get_type,
                         "VentureSlaState",
                         venture_sla_state_values)

/* --- Webhooks, routing and satisfaction ----------------------------------- */

static const GEnumValue venture_delivery_state_values[] = {
	VE(VENTURE_DELIVERY_STATE_PENDING,   "pending"),
	VE(VENTURE_DELIVERY_STATE_SUCCEEDED, "succeeded"),
	VE(VENTURE_DELIVERY_STATE_FAILED,    "failed"),
	VE_END
};

VENTURE_DEFINE_ENUM_TYPE(venture_delivery_state_get_type,
                         "VentureDeliveryState",
                         venture_delivery_state_values)

static const GEnumValue venture_routing_strategy_values[] = {
	VE(VENTURE_ROUTING_STRATEGY_ROUND_ROBIN, "round_robin"),
	VE(VENTURE_ROUTING_STRATEGY_LEAST_BUSY,  "least_busy"),
	VE(VENTURE_ROUTING_STRATEGY_FIRST,       "first"),
	VE_END
};

VENTURE_DEFINE_ENUM_TYPE(venture_routing_strategy_get_type,
                         "VentureRoutingStrategy",
                         venture_routing_strategy_values)

static const GEnumValue venture_satisfaction_values[] = {
	VE(VENTURE_SATISFACTION_UNRATED, "unrated"),
	VE(VENTURE_SATISFACTION_BAD,     "bad"),
	VE(VENTURE_SATISFACTION_NEUTRAL, "neutral"),
	VE(VENTURE_SATISFACTION_GOOD,    "good"),
	VE_END
};

VENTURE_DEFINE_ENUM_TYPE(venture_satisfaction_get_type,
                         "VentureSatisfaction",
                         venture_satisfaction_values)
