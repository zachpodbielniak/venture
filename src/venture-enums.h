/*
 * venture-enums.h - Enumerations and flags used across VENTURE
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Every enum here is registered with the GLib type system so it can be used
 * as a GObject property, stored in a GValue, serialised by nick, and
 * introspected. The nick of each value is its stable wire format: it is what
 * appears in the REST API, in YAML configuration, in the database and in
 * arguments the AI passes to a tool call. Renaming a nick is a breaking
 * change; adding a value is not.
 *
 * Note deliberately absent from this file: the kind of a venture, the
 * category of an expense and the type of a document are NOT enums. Those are
 * registry-backed string identifiers so that a plugin or a YAML file can
 * introduce a new one without recompiling. See #VentureVentureTypeRegistry.
 */

#ifndef VENTURE_ENUMS_H
#define VENTURE_ENUMS_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>

G_BEGIN_DECLS

/**
 * VentureOrganizationKind:
 * @VENTURE_ORGANIZATION_KIND_PERSONAL: a personal, non-business entity, used
 *   to track household finances alongside the businesses
 * @VENTURE_ORGANIZATION_KIND_SOLE_PROPRIETOR: an unincorporated business
 * @VENTURE_ORGANIZATION_KIND_LLC: a limited liability company
 * @VENTURE_ORGANIZATION_KIND_S_CORP: an S corporation
 * @VENTURE_ORGANIZATION_KIND_C_CORP: a C corporation
 * @VENTURE_ORGANIZATION_KIND_PARTNERSHIP: a partnership
 * @VENTURE_ORGANIZATION_KIND_NONPROFIT: a non-profit organisation
 * @VENTURE_ORGANIZATION_KIND_TRUST: a trust or estate
 *
 * The legal form of an organisation. This drives which tax treatment the
 * reporting layer applies, and keeps personal bookkeeping separated from
 * business bookkeeping in the same database.
 */
typedef enum
{
	VENTURE_ORGANIZATION_KIND_PERSONAL = 0,
	VENTURE_ORGANIZATION_KIND_SOLE_PROPRIETOR,
	VENTURE_ORGANIZATION_KIND_LLC,
	VENTURE_ORGANIZATION_KIND_S_CORP,
	VENTURE_ORGANIZATION_KIND_C_CORP,
	VENTURE_ORGANIZATION_KIND_PARTNERSHIP,
	VENTURE_ORGANIZATION_KIND_NONPROFIT,
	VENTURE_ORGANIZATION_KIND_TRUST
} VentureOrganizationKind;

/**
 * VentureVentureStatus:
 * @VENTURE_VENTURE_STATUS_IDEA: not started; still just a notion
 * @VENTURE_VENTURE_STATUS_PLANNING: being scoped and researched
 * @VENTURE_VENTURE_STATUS_BUILDING: under construction, not yet earning
 * @VENTURE_VENTURE_STATUS_ACTIVE: live and operating
 * @VENTURE_VENTURE_STATUS_PAUSED: temporarily dormant, intended to resume
 * @VENTURE_VENTURE_STATUS_WINDING_DOWN: being retired
 * @VENTURE_VENTURE_STATUS_ARCHIVED: finished; retained for history only
 *
 * The lifecycle stage of a venture. Reporting excludes archived ventures
 * from portfolio roll-ups by default but never deletes their history, since
 * their expenses still matter at tax time.
 */
typedef enum
{
	VENTURE_VENTURE_STATUS_IDEA = 0,
	VENTURE_VENTURE_STATUS_PLANNING,
	VENTURE_VENTURE_STATUS_BUILDING,
	VENTURE_VENTURE_STATUS_ACTIVE,
	VENTURE_VENTURE_STATUS_PAUSED,
	VENTURE_VENTURE_STATUS_WINDING_DOWN,
	VENTURE_VENTURE_STATUS_ARCHIVED
} VentureVentureStatus;

/**
 * VentureAccountKind:
 * @VENTURE_ACCOUNT_KIND_ASSET: something owned
 * @VENTURE_ACCOUNT_KIND_LIABILITY: something owed
 * @VENTURE_ACCOUNT_KIND_EQUITY: owner's residual interest
 * @VENTURE_ACCOUNT_KIND_INCOME: revenue
 * @VENTURE_ACCOUNT_KIND_EXPENSE: cost incurred
 *
 * The five classes of the chart of accounts. Assets and expenses increase on
 * the debit side; liabilities, equity and income increase on the credit side.
 * venture_account_kind_is_debit_normal() encodes that rule.
 */
typedef enum
{
	VENTURE_ACCOUNT_KIND_ASSET = 0,
	VENTURE_ACCOUNT_KIND_LIABILITY,
	VENTURE_ACCOUNT_KIND_EQUITY,
	VENTURE_ACCOUNT_KIND_INCOME,
	VENTURE_ACCOUNT_KIND_EXPENSE
} VentureAccountKind;

/**
 * VentureLedgerSide:
 * @VENTURE_LEDGER_SIDE_DEBIT: the left side of the entry
 * @VENTURE_LEDGER_SIDE_CREDIT: the right side of the entry
 *
 * Which side of a double-entry transaction a ledger line sits on.
 */
typedef enum
{
	VENTURE_LEDGER_SIDE_DEBIT = 0,
	VENTURE_LEDGER_SIDE_CREDIT
} VentureLedgerSide;

/**
 * VentureInventoryTxnKind:
 * @VENTURE_INVENTORY_TXN_KIND_PURCHASE: stock bought in
 * @VENTURE_INVENTORY_TXN_KIND_PRODUCTION: stock made rather than bought
 * @VENTURE_INVENTORY_TXN_KIND_SALE: stock sold, reducing quantity on hand
 * @VENTURE_INVENTORY_TXN_KIND_RETURN: stock returned by a customer
 * @VENTURE_INVENTORY_TXN_KIND_ADJUSTMENT: a correction after a stock count
 * @VENTURE_INVENTORY_TXN_KIND_WRITE_OFF: stock lost, damaged or expired
 * @VENTURE_INVENTORY_TXN_KIND_TRANSFER: stock moved between locations
 *
 * The reason an inventory quantity changed. Quantity on hand is never stored
 * as a mutable field; it is always the sum of the signed transactions, so
 * the stock level is auditable and can be recomputed at any past date.
 */
typedef enum
{
	VENTURE_INVENTORY_TXN_KIND_PURCHASE = 0,
	VENTURE_INVENTORY_TXN_KIND_PRODUCTION,
	VENTURE_INVENTORY_TXN_KIND_SALE,
	VENTURE_INVENTORY_TXN_KIND_RETURN,
	VENTURE_INVENTORY_TXN_KIND_ADJUSTMENT,
	VENTURE_INVENTORY_TXN_KIND_WRITE_OFF,
	VENTURE_INVENTORY_TXN_KIND_TRANSFER
} VentureInventoryTxnKind;

/**
 * VentureDealStage:
 * @VENTURE_DEAL_STAGE_LEAD: an unqualified opportunity
 * @VENTURE_DEAL_STAGE_QUALIFIED: confirmed as worth pursuing
 * @VENTURE_DEAL_STAGE_PROPOSAL: a proposal or quote is out
 * @VENTURE_DEAL_STAGE_NEGOTIATION: terms are being agreed
 * @VENTURE_DEAL_STAGE_WON: closed successfully
 * @VENTURE_DEAL_STAGE_LOST: closed unsuccessfully
 *
 * The CRM pipeline stage of a deal. Won and lost are terminal, and the
 * pipeline report treats every other stage as open.
 */
typedef enum
{
	VENTURE_DEAL_STAGE_LEAD = 0,
	VENTURE_DEAL_STAGE_QUALIFIED,
	VENTURE_DEAL_STAGE_PROPOSAL,
	VENTURE_DEAL_STAGE_NEGOTIATION,
	VENTURE_DEAL_STAGE_WON,
	VENTURE_DEAL_STAGE_LOST
} VentureDealStage;

/**
 * VentureInteractionKind:
 * @VENTURE_INTERACTION_KIND_NOTE: a free-form observation
 * @VENTURE_INTERACTION_KIND_EMAIL: an email exchanged
 * @VENTURE_INTERACTION_KIND_CALL: a phone or video call
 * @VENTURE_INTERACTION_KIND_MEETING: a scheduled meeting
 * @VENTURE_INTERACTION_KIND_MESSAGE: a chat or DM
 * @VENTURE_INTERACTION_KIND_PURCHASE: the contact bought something
 * @VENTURE_INTERACTION_KIND_SUPPORT: a support request
 * @VENTURE_INTERACTION_KIND_OUTREACH: an outbound campaign touch
 *
 * The kind of contact touchpoint recorded on the CRM timeline.
 */
typedef enum
{
	VENTURE_INTERACTION_KIND_NOTE = 0,
	VENTURE_INTERACTION_KIND_EMAIL,
	VENTURE_INTERACTION_KIND_CALL,
	VENTURE_INTERACTION_KIND_MEETING,
	VENTURE_INTERACTION_KIND_MESSAGE,
	VENTURE_INTERACTION_KIND_PURCHASE,
	VENTURE_INTERACTION_KIND_SUPPORT,
	VENTURE_INTERACTION_KIND_OUTREACH
} VentureInteractionKind;

/**
 * VentureTaskStatus:
 * @VENTURE_TICKET_STATUS_TRIAGE: arrived, not yet sorted
 * @VENTURE_TICKET_STATUS_TODO: accepted, not started
 * @VENTURE_TICKET_STATUS_IN_PROGRESS: being worked on
 * @VENTURE_TICKET_STATUS_BLOCKED: waiting on somebody else
 * @VENTURE_TICKET_STATUS_REVIEW: done, awaiting a check
 * @VENTURE_TICKET_STATUS_DONE: finished
 * @VENTURE_TICKET_STATUS_CANCELLED: abandoned or rejected
 *
 * The state of a ticket, internal or external.
 *
 * These deliberately mirror the org-mode workflow keywords so that work
 * round-trips cleanly to and from an org file, and they are the columns of
 * the board. Triage exists because an external ticket arrives before anybody
 * has decided whether it is real; blocked means waiting on somebody else,
 * which for a support ticket is usually the person who raised it.
 */
typedef enum
{
	VENTURE_TICKET_STATUS_TRIAGE = 0,
	VENTURE_TICKET_STATUS_TODO,
	VENTURE_TICKET_STATUS_IN_PROGRESS,
	VENTURE_TICKET_STATUS_BLOCKED,
	VENTURE_TICKET_STATUS_REVIEW,
	VENTURE_TICKET_STATUS_DONE,
	VENTURE_TICKET_STATUS_CANCELLED
} VentureTicketStatus;

/**
 * VentureTicketKind:
 * @VENTURE_TICKET_KIND_INTERNAL: your own work -- a project task
 * @VENTURE_TICKET_KIND_EXTERNAL: somebody else's problem -- a support request
 *
 * What a ticket is for.
 *
 * One type covers both because the work is the same shape: something to do,
 * in a state, assigned to somebody, with a thread of comments. The
 * difference is who raised it and who may see the replies, and both are
 * fields rather than separate systems to keep in step.
 */
typedef enum
{
	VENTURE_TICKET_KIND_INTERNAL = 0,
	VENTURE_TICKET_KIND_EXTERNAL
} VentureTicketKind;

/**
 * VentureIssueType:
 * @VENTURE_ISSUE_TYPE_TASK: a unit of work somebody can just do
 * @VENTURE_ISSUE_TYPE_SUBTASK: a piece of a task, split out to be shared
 * @VENTURE_ISSUE_TYPE_STORY: a change described by what it gives somebody
 * @VENTURE_ISSUE_TYPE_EPIC: a body of work that has to be broken up first
 * @VENTURE_ISSUE_TYPE_BUG: something that already exists and is wrong
 * @VENTURE_ISSUE_TYPE_RESEARCH: a question to answer, with no code expected
 *
 * What shape of work a ticket is.
 *
 * A different question from #VentureTicketKind, and the two are deliberately
 * separate fields. Kind says whose problem it is -- your own project work or
 * somebody else's support request -- which is what decides who may read the
 * replies. This says how big the piece is and therefore how it should be
 * approached, which is what the forge rules key on: a bug wants a
 * reproduction and a regression test, an epic wants breaking up and no code
 * at all. An external bug is both, and common.
 *
 * %VENTURE_ISSUE_TYPE_TASK is zero deliberately. This column is added to a
 * table that already has rows; those rows get NULL, and a NULL enum column
 * reads back as the property default, which is always the enum's zero value.
 * Every ticket written before this field existed therefore becomes a task,
 * which is what they were. Reordering this enumeration later is safe for
 * storage -- values persist by nick -- but it silently retypes every
 * historical ticket, so the first value is a one-time decision.
 */
typedef enum
{
	VENTURE_ISSUE_TYPE_TASK = 0,
	VENTURE_ISSUE_TYPE_SUBTASK,
	VENTURE_ISSUE_TYPE_STORY,
	VENTURE_ISSUE_TYPE_EPIC,
	VENTURE_ISSUE_TYPE_BUG,
	VENTURE_ISSUE_TYPE_RESEARCH
} VentureIssueType;

/**
 * VentureForgeKind:
 * @VENTURE_FORGE_KIND_FORGEJO: Forgejo
 * @VENTURE_FORGE_KIND_GITEA: Gitea, which speaks the same API
 * @VENTURE_FORGE_KIND_GITHUB: GitHub
 * @VENTURE_FORGE_KIND_GITLAB: GitLab
 *
 * Which software a forge runs.
 *
 * Forgejo and Gitea share API v1 and are served by one client. The other two
 * are declared so the form can express what an operator actually has, and the
 * client factory refuses them with %VENTURE_ERROR_NOT_SUPPORTED -- a clearer
 * answer than a 404 from a GitHub URL addressed as though it were Forgejo.
 */
typedef enum
{
	VENTURE_FORGE_KIND_FORGEJO = 0,
	VENTURE_FORGE_KIND_GITEA,
	VENTURE_FORGE_KIND_GITHUB,
	VENTURE_FORGE_KIND_GITLAB
} VentureForgeKind;

/**
 * VentureForgeRunner:
 * @VENTURE_FORGE_RUNNER_AGENT: an agent inside this process
 * @VENTURE_FORGE_RUNNER_CLI: a coding CLI in a checkout
 *
 * What actually does the work.
 *
 * The agent runner needs no checkout and no subprocess, so it works
 * everywhere including inside the shipped container image, which is why it is
 * zero. It reaches the filesystem only through VENTURE's own confined file
 * tools and has no shell, so it can edit a tree but cannot run its tests.
 * The CLI runner spawns claude-code, opencode or grok-build in a workspace
 * and can do both, at the cost of needing that binary present.
 */
typedef enum
{
	VENTURE_FORGE_RUNNER_AGENT = 0,
	VENTURE_FORGE_RUNNER_CLI
} VentureForgeRunner;

/**
 * VentureForgeRunOutcome:
 * @VENTURE_FORGE_RUN_OUTCOME_DRAFT_PR: push the branch and open a draft
 * @VENTURE_FORGE_RUN_OUTCOME_PUSH_BRANCH: push the branch and stop
 * @VENTURE_FORGE_RUN_OUTCOME_LOCAL_BRANCH: commit locally and stop
 * @VENTURE_FORGE_RUN_OUTCOME_NONE: change nothing anywhere
 *
 * How far a successful run goes.
 *
 * Unlike #VentureInvoiceStatus this order is not a progression -- it runs
 * from most to least, because the first value is what a new rule gets and
 * what the form offers first, and the decision was that a run should end at
 * a draft pull request. Nothing here ever merges: a draft is a place to look
 * at the work, which is the point.
 */
typedef enum
{
	VENTURE_FORGE_RUN_OUTCOME_DRAFT_PR = 0,
	VENTURE_FORGE_RUN_OUTCOME_PUSH_BRANCH,
	VENTURE_FORGE_RUN_OUTCOME_LOCAL_BRANCH,
	VENTURE_FORGE_RUN_OUTCOME_NONE
} VentureForgeRunOutcome;

/**
 * VentureForgeTrigger:
 * @VENTURE_FORGE_TRIGGER_MANUAL: only when somebody presses the button
 * @VENTURE_FORGE_TRIGGER_ON_CREATE: as soon as the ticket exists
 * @VENTURE_FORGE_TRIGGER_ON_TODO: when it is accepted off triage
 * @VENTURE_FORGE_TRIGGER_ON_IN_PROGRESS: when somebody starts it
 *
 * What starts a run.
 *
 * Manual is zero: a rule created by hand does nothing at all until somebody
 * asks it to. That is the right default for the one feature here that spends
 * money without being watched.
 */
typedef enum
{
	VENTURE_FORGE_TRIGGER_MANUAL = 0,
	VENTURE_FORGE_TRIGGER_ON_CREATE,
	VENTURE_FORGE_TRIGGER_ON_TODO,
	VENTURE_FORGE_TRIGGER_ON_IN_PROGRESS
} VentureForgeTrigger;

/**
 * VentureAgentSessionState:
 * @VENTURE_AGENT_SESSION_STATE_IDLE: open, waiting for something to do
 * @VENTURE_AGENT_SESSION_STATE_WORKING: a turn is in flight
 * @VENTURE_AGENT_SESSION_STATE_CLOSED: ended deliberately
 * @VENTURE_AGENT_SESSION_STATE_FAILED: the provider or the workspace gave
 *   way, and the session cannot be continued
 *
 * Where an agent-harness session stands.
 *
 * Closed and failed are both terminal and are kept apart for the reason
 * refused and failed are kept apart on a run: one is a decision and the
 * other is a fault, and a list that shows them the same way cannot answer
 * "what went wrong yesterday".
 */
typedef enum
{
	VENTURE_AGENT_SESSION_STATE_IDLE = 0,
	VENTURE_AGENT_SESSION_STATE_WORKING,
	VENTURE_AGENT_SESSION_STATE_CLOSED,
	VENTURE_AGENT_SESSION_STATE_FAILED
} VentureAgentSessionState;

/**
 * VentureForgeRunState:
 * @VENTURE_FORGE_RUN_STATE_QUEUED: accepted, waiting for a slot
 * @VENTURE_FORGE_RUN_STATE_RUNNING: in progress
 * @VENTURE_FORGE_RUN_STATE_SUCCEEDED: finished, and did what it said
 * @VENTURE_FORGE_RUN_STATE_FAILED: tried and could not
 * @VENTURE_FORGE_RUN_STATE_CANCELLED: stopped by a person
 * @VENTURE_FORGE_RUN_STATE_REFUSED: never started
 * @VENTURE_FORGE_RUN_STATE_INTERRUPTED: the server stopped mid-run
 *
 * Where a run stands.
 *
 * Refused is distinct from failed on purpose: refused means a daily limit or
 * an approval gate stopped it before anything ran, failed means a runner
 * tried and could not. Collapsing the two makes "why did nothing happen"
 * unanswerable. Interrupted is likewise not failed -- nobody observed the
 * outcome, so nothing may be concluded from it and nothing is retried
 * automatically.
 */
typedef enum
{
	VENTURE_FORGE_RUN_STATE_QUEUED = 0,
	VENTURE_FORGE_RUN_STATE_RUNNING,
	VENTURE_FORGE_RUN_STATE_SUCCEEDED,
	VENTURE_FORGE_RUN_STATE_FAILED,
	VENTURE_FORGE_RUN_STATE_CANCELLED,
	VENTURE_FORGE_RUN_STATE_REFUSED,
	VENTURE_FORGE_RUN_STATE_INTERRUPTED
} VentureForgeRunState;

/**
 * VentureForgeLinkOrigin:
 * @VENTURE_FORGE_LINK_ORIGIN_FORGE: the issue was raised upstream
 * @VENTURE_FORGE_LINK_ORIGIN_VENTURE: VENTURE filed it
 *
 * Which side raised the issue behind a link.
 *
 * Half of the webhook loop guard. %VENTURE_FORGE_LINK_ORIGIN_FORGE is zero
 * rather than the other way round, and the choice is load-bearing: if a bug
 * ever leaves origin unset on a row VENTURE created, a zero meaning VENTURE
 * would make the loop guard suppress a real upstream ticket, which is silent
 * data loss. A zero meaning FORGE makes VENTURE re-import its own issue
 * instead -- noisy, visible, and fixable.
 */
typedef enum
{
	VENTURE_FORGE_LINK_ORIGIN_FORGE = 0,
	VENTURE_FORGE_LINK_ORIGIN_VENTURE
} VentureForgeLinkOrigin;

/**
 * VentureInvoiceStatus:
 * @VENTURE_INVOICE_STATUS_DRAFT: being written; not yet a claim on anybody
 * @VENTURE_INVOICE_STATUS_SENT: issued to the customer and awaiting payment
 * @VENTURE_INVOICE_STATUS_PAID: settled; the revenue is real
 * @VENTURE_INVOICE_STATUS_VOID: cancelled without payment
 *
 * Where an invoice stands.
 *
 * Overdue is deliberately not a status: it is a fact derived from due-at
 * and the clock, and a stored copy of a derivable fact is a copy that goes
 * stale. The UI computes it at render time.
 */
typedef enum
{
	VENTURE_INVOICE_STATUS_DRAFT = 0,
	VENTURE_INVOICE_STATUS_SENT,
	VENTURE_INVOICE_STATUS_PAID,
	VENTURE_INVOICE_STATUS_VOID
} VentureInvoiceStatus;

/**
 * VentureChatRole:
 * @VENTURE_CHAT_ROLE_USER: the operator typed it
 * @VENTURE_CHAT_ROLE_ASSISTANT: the model replied with it
 *
 * Who said a line in an AI conversation.
 *
 * An enum rather than a free string because the transcript is replayed to
 * the model when a conversation resumes, and a misspelled role would turn a
 * stored reply into something the provider rejects -- days after it was
 * written, in a thread that used to work.
 */
typedef enum
{
	VENTURE_CHAT_ROLE_USER = 0,
	VENTURE_CHAT_ROLE_ASSISTANT
} VentureChatRole;

/**
 * VentureCompanyKind:
 * @VENTURE_COMPANY_KIND_CUSTOMER: buys from you
 * @VENTURE_COMPANY_KIND_PROSPECT: might buy from you
 * @VENTURE_COMPANY_KIND_SUPPLIER: you buy from them
 * @VENTURE_COMPANY_KIND_PLATFORM: a marketplace or storefront you sell on
 * @VENTURE_COMPANY_KIND_PARTNER: you work with them
 * @VENTURE_COMPANY_KIND_OTHER: none of the above
 *
 * What an outside business is to you.
 *
 * Distinct from #VentureOrganizationKind, which is the legal form of one of
 * *your* entities. A company here is somebody you deal with; an
 * organisation is somebody you are.
 */
typedef enum
{
	VENTURE_COMPANY_KIND_CUSTOMER = 0,
	VENTURE_COMPANY_KIND_PROSPECT,
	VENTURE_COMPANY_KIND_SUPPLIER,
	VENTURE_COMPANY_KIND_PLATFORM,
	VENTURE_COMPANY_KIND_PARTNER,
	VENTURE_COMPANY_KIND_OTHER
} VentureCompanyKind;

/**
 * VenturePriority:
 * @VENTURE_PRIORITY_LOW: can slip indefinitely
 * @VENTURE_PRIORITY_NORMAL: the default
 * @VENTURE_PRIORITY_HIGH: should be done soon
 * @VENTURE_PRIORITY_URGENT: should be done now
 *
 * A coarse priority shared by tasks, ideas and deals.
 */
typedef enum
{
	VENTURE_PRIORITY_LOW = 0,
	VENTURE_PRIORITY_NORMAL,
	VENTURE_PRIORITY_HIGH,
	VENTURE_PRIORITY_URGENT
} VenturePriority;

/**
 * VentureIdeaStatus:
 * @VENTURE_IDEA_STATUS_CAPTURED: written down, not yet examined
 * @VENTURE_IDEA_STATUS_RESEARCHING: being investigated
 * @VENTURE_IDEA_STATUS_VALIDATED: research supports pursuing it
 * @VENTURE_IDEA_STATUS_REJECTED: examined and declined
 * @VENTURE_IDEA_STATUS_PROMOTED: turned into a real venture
 *
 * Where an idea sits in the explore-and-decide pipeline. Promoting an idea
 * creates a #VentureVenture and links the two, so the reasoning that led to
 * a venture stays attached to it.
 */
typedef enum
{
	VENTURE_IDEA_STATUS_CAPTURED = 0,
	VENTURE_IDEA_STATUS_RESEARCHING,
	VENTURE_IDEA_STATUS_VALIDATED,
	VENTURE_IDEA_STATUS_REJECTED,
	VENTURE_IDEA_STATUS_PROMOTED
} VentureIdeaStatus;

/**
 * VentureCampaignStatus:
 * @VENTURE_CAMPAIGN_STATUS_DRAFT: being written
 * @VENTURE_CAMPAIGN_STATUS_SCHEDULED: queued to start
 * @VENTURE_CAMPAIGN_STATUS_RUNNING: currently live
 * @VENTURE_CAMPAIGN_STATUS_PAUSED: halted, resumable
 * @VENTURE_CAMPAIGN_STATUS_COMPLETED: finished
 * @VENTURE_CAMPAIGN_STATUS_CANCELLED: abandoned before completion
 *
 * The state of a marketing campaign, whether that is an ad buy, a book
 * promotion, a newsletter blast or an outreach push.
 */
typedef enum
{
	VENTURE_CAMPAIGN_STATUS_DRAFT = 0,
	VENTURE_CAMPAIGN_STATUS_SCHEDULED,
	VENTURE_CAMPAIGN_STATUS_RUNNING,
	VENTURE_CAMPAIGN_STATUS_PAUSED,
	VENTURE_CAMPAIGN_STATUS_COMPLETED,
	VENTURE_CAMPAIGN_STATUS_CANCELLED
} VentureCampaignStatus;

/**
 * VenturePostStatus:
 * @VENTURE_POST_STATUS_DRAFT: not published
 * @VENTURE_POST_STATUS_REVIEW: awaiting a final pass
 * @VENTURE_POST_STATUS_SCHEDULED: queued for a future publish date
 * @VENTURE_POST_STATUS_PUBLISHED: live
 * @VENTURE_POST_STATUS_ARCHIVED: unpublished after the fact
 *
 * The publication state of a blog post or newsletter issue.
 */
typedef enum
{
	VENTURE_POST_STATUS_DRAFT = 0,
	VENTURE_POST_STATUS_REVIEW,
	VENTURE_POST_STATUS_SCHEDULED,
	VENTURE_POST_STATUS_PUBLISHED,
	VENTURE_POST_STATUS_ARCHIVED
} VenturePostStatus;

/**
 * VentureSubscriberStatus:
 * @VENTURE_SUBSCRIBER_STATUS_PENDING: signed up, not yet confirmed
 * @VENTURE_SUBSCRIBER_STATUS_ACTIVE: confirmed and receiving mail
 * @VENTURE_SUBSCRIBER_STATUS_UNSUBSCRIBED: opted out
 * @VENTURE_SUBSCRIBER_STATUS_BOUNCED: mail to this address is failing
 * @VENTURE_SUBSCRIBER_STATUS_COMPLAINED: reported the mail as spam
 *
 * The delivery state of a newsletter subscriber. Anything other than
 * %VENTURE_SUBSCRIBER_STATUS_ACTIVE is excluded from a send.
 */
typedef enum
{
	VENTURE_SUBSCRIBER_STATUS_PENDING = 0,
	VENTURE_SUBSCRIBER_STATUS_ACTIVE,
	VENTURE_SUBSCRIBER_STATUS_UNSUBSCRIBED,
	VENTURE_SUBSCRIBER_STATUS_BOUNCED,
	VENTURE_SUBSCRIBER_STATUS_COMPLAINED
} VentureSubscriberStatus;

/**
 * VentureDeductibility:
 * @VENTURE_DEDUCTIBILITY_NONE: not deductible at all
 * @VENTURE_DEDUCTIBILITY_FULL: fully deductible
 * @VENTURE_DEDUCTIBILITY_PARTIAL: deductible at the category's percentage
 * @VENTURE_DEDUCTIBILITY_CAPITAL: capitalised and depreciated, not expensed
 * @VENTURE_DEDUCTIBILITY_REVIEW: flagged for a human or accountant to decide
 *
 * How an expense is treated for tax. %VENTURE_DEDUCTIBILITY_REVIEW is the
 * safe default for anything imported or classified automatically, so nothing
 * silently claims a deduction that was never confirmed.
 */
typedef enum
{
	VENTURE_DEDUCTIBILITY_NONE = 0,
	VENTURE_DEDUCTIBILITY_FULL,
	VENTURE_DEDUCTIBILITY_PARTIAL,
	VENTURE_DEDUCTIBILITY_CAPITAL,
	VENTURE_DEDUCTIBILITY_REVIEW
} VentureDeductibility;

/**
 * VentureActorKind:
 * @VENTURE_ACTOR_KIND_SYSTEM: VENTURE itself, e.g. a migration
 * @VENTURE_ACTOR_KIND_USER: a human acting through the UI or the CLI
 * @VENTURE_ACTOR_KIND_AI: an AI tool call
 * @VENTURE_ACTOR_KIND_AUTOMATION: a podomation binding
 * @VENTURE_ACTOR_KIND_PLUGIN: a loaded plugin
 * @VENTURE_ACTOR_KIND_IMPORT: a bulk import
 *
 * Who caused a change. Every mutation records this, which is what makes it
 * possible to answer "did I do that, or did the AI?" months later.
 */
typedef enum
{
	VENTURE_ACTOR_KIND_SYSTEM = 0,
	VENTURE_ACTOR_KIND_USER,
	VENTURE_ACTOR_KIND_AI,
	VENTURE_ACTOR_KIND_AUTOMATION,
	VENTURE_ACTOR_KIND_PLUGIN,
	VENTURE_ACTOR_KIND_IMPORT
} VentureActorKind;

/**
 * VentureAuditAction:
 * @VENTURE_AUDIT_ACTION_CREATE: a record was created
 * @VENTURE_AUDIT_ACTION_UPDATE: a record was modified
 * @VENTURE_AUDIT_ACTION_DELETE: a record was deleted
 * @VENTURE_AUDIT_ACTION_LOGIN: a session was opened
 * @VENTURE_AUDIT_ACTION_LOGOUT: a session was closed
 * @VENTURE_AUDIT_ACTION_TOOL_CALL: an AI tool was invoked
 * @VENTURE_AUDIT_ACTION_CONFIRM: a pending AI write was approved
 * @VENTURE_AUDIT_ACTION_REJECT: a pending AI write was declined
 * @VENTURE_AUDIT_ACTION_AUTOMATION: an automation binding fired
 * @VENTURE_AUDIT_ACTION_EXPORT: data was exported out of the system
 *
 * What an audit entry records.
 */
typedef enum
{
	VENTURE_AUDIT_ACTION_CREATE = 0,
	VENTURE_AUDIT_ACTION_UPDATE,
	VENTURE_AUDIT_ACTION_DELETE,
	VENTURE_AUDIT_ACTION_LOGIN,
	VENTURE_AUDIT_ACTION_LOGOUT,
	VENTURE_AUDIT_ACTION_TOOL_CALL,
	VENTURE_AUDIT_ACTION_CONFIRM,
	VENTURE_AUDIT_ACTION_REJECT,
	VENTURE_AUDIT_ACTION_AUTOMATION,
	VENTURE_AUDIT_ACTION_EXPORT
} VentureAuditAction;

/**
 * VentureAiPolicy:
 * @VENTURE_AI_POLICY_READ_ONLY: the AI may query but never mutate
 * @VENTURE_AI_POLICY_CONFIRM_WRITES: mutations are staged and must be
 *   approved by a human before they are applied
 * @VENTURE_AI_POLICY_AUTONOMOUS: mutations apply immediately
 *
 * How much authority AI tool calls have. The default is
 * %VENTURE_AI_POLICY_CONFIRM_WRITES: reads run unattended, and anything that
 * would change a record is turned into a #VentureConfirmation carrying a
 * diff for approval. Every applied mutation is audited regardless of policy.
 */
typedef enum
{
	VENTURE_AI_POLICY_READ_ONLY = 0,
	VENTURE_AI_POLICY_CONFIRM_WRITES,
	VENTURE_AI_POLICY_AUTONOMOUS
} VentureAiPolicy;

/**
 * VentureConfirmationState:
 * @VENTURE_CONFIRMATION_STATE_PENDING: awaiting a decision
 * @VENTURE_CONFIRMATION_STATE_APPROVED: approved and applied
 * @VENTURE_CONFIRMATION_STATE_REJECTED: declined by the operator
 * @VENTURE_CONFIRMATION_STATE_EXPIRED: timed out without a decision
 * @VENTURE_CONFIRMATION_STATE_FAILED: approved but the write then failed
 *
 * The lifecycle of a staged change, whichever surface proposed it.
 */
typedef enum
{
	VENTURE_CONFIRMATION_STATE_PENDING = 0,
	VENTURE_CONFIRMATION_STATE_APPROVED,
	VENTURE_CONFIRMATION_STATE_REJECTED,
	VENTURE_CONFIRMATION_STATE_EXPIRED,
	VENTURE_CONFIRMATION_STATE_FAILED
} VentureConfirmationState;

/**
 * VentureUserRole:
 * @VENTURE_USER_ROLE_OWNER: full control, including user management
 * @VENTURE_USER_ROLE_ADMIN: full control over data and settings
 * @VENTURE_USER_ROLE_EDITOR: may read and write business data
 * @VENTURE_USER_ROLE_VIEWER: read-only
 * @VENTURE_USER_ROLE_SERVICE: a non-human integration account
 *
 * The role attached to a user or an API token. VENTURE ships as a
 * single-operator system, but the roles exist from the start so that adding
 * a second person later is a configuration change rather than a migration.
 */
typedef enum
{
	VENTURE_USER_ROLE_OWNER = 0,
	VENTURE_USER_ROLE_ADMIN,
	VENTURE_USER_ROLE_EDITOR,
	VENTURE_USER_ROLE_VIEWER,
	VENTURE_USER_ROLE_SERVICE
} VentureUserRole;

/**
 * VentureKbFormat:
 * @VENTURE_KB_FORMAT_ORG: Emacs Org mode, the preferred source format
 * @VENTURE_KB_FORMAT_MARKDOWN: Markdown
 * @VENTURE_KB_FORMAT_TEXT: plain text
 * @VENTURE_KB_FORMAT_HTML: HTML
 * @VENTURE_KB_FORMAT_PDF: text extracted from a PDF
 * @VENTURE_KB_FORMAT_DOCX: text extracted from an OOXML document
 * @VENTURE_KB_FORMAT_OTHER: stored, but its text could not be read
 *
 * What an article's body came from.
 *
 * This records the *source* format, not a rendering instruction. An article
 * imported from a PDF keeps %VENTURE_KB_FORMAT_PDF even though its body is
 * now plain text, because the difference matters when the file changes on
 * disk and has to be extracted again -- and because text recovered from a
 * PDF is worth trusting less than text somebody wrote.
 *
 * %VENTURE_KB_FORMAT_ORG is zero: it is the preferred format, so an article
 * created without saying is treated as the thing most of them are.
 */
typedef enum
{
	VENTURE_KB_FORMAT_ORG = 0,
	VENTURE_KB_FORMAT_MARKDOWN,
	VENTURE_KB_FORMAT_TEXT,
	VENTURE_KB_FORMAT_HTML,
	VENTURE_KB_FORMAT_PDF,
	VENTURE_KB_FORMAT_DOCX,
	VENTURE_KB_FORMAT_OTHER
} VentureKbFormat;

/**
 * VentureKbArticleStatus:
 * @VENTURE_KB_ARTICLE_STATUS_PUBLISHED: searchable and offered to the AI
 * @VENTURE_KB_ARTICLE_STATUS_DRAFT: kept, but withheld from search
 * @VENTURE_KB_ARTICLE_STATUS_ARCHIVED: superseded; withheld from search
 *
 * Whether an article counts as knowledge yet.
 *
 * %VENTURE_KB_ARTICLE_STATUS_PUBLISHED is zero because an imported or synced
 * file is knowledge the moment it arrives -- making import land in a draft
 * state would mean every sync needed a second, manual step before the AI
 * could see any of it. Draft is the deliberate act, not the default.
 *
 * Only published articles are searched. A draft still embeds, so promoting
 * one costs nothing.
 */
typedef enum
{
	VENTURE_KB_ARTICLE_STATUS_PUBLISHED = 0,
	VENTURE_KB_ARTICLE_STATUS_DRAFT,
	VENTURE_KB_ARTICLE_STATUS_ARCHIVED
} VentureKbArticleStatus;

/**
 * VentureFieldKind:
 * @VENTURE_FIELD_KIND_STRING: a short single-line string
 * @VENTURE_FIELD_KIND_TEXT: a long multi-line string
 * @VENTURE_FIELD_KIND_INTEGER: a signed 64-bit integer
 * @VENTURE_FIELD_KIND_DOUBLE: a double-precision float
 * @VENTURE_FIELD_KIND_MONEY: a #VentureMoney amount
 * @VENTURE_FIELD_KIND_BOOLEAN: a boolean
 * @VENTURE_FIELD_KIND_DATE: a calendar date
 * @VENTURE_FIELD_KIND_DATETIME: an instant in time
 * @VENTURE_FIELD_KIND_ENUM: one of a fixed set of string choices
 * @VENTURE_FIELD_KIND_REFERENCE: a foreign key to another entity
 * @VENTURE_FIELD_KIND_JSON: an arbitrary JSON blob
 *
 * The type of a field on a declaratively-defined venture type. This is the
 * vocabulary a YAML venture-type definition uses, and it maps onto both a
 * GType for the property and a SQL type for the column.
 */
typedef enum
{
	VENTURE_FIELD_KIND_STRING = 0,
	VENTURE_FIELD_KIND_TEXT,
	VENTURE_FIELD_KIND_INTEGER,
	VENTURE_FIELD_KIND_DOUBLE,
	VENTURE_FIELD_KIND_MONEY,
	VENTURE_FIELD_KIND_BOOLEAN,
	VENTURE_FIELD_KIND_DATE,
	VENTURE_FIELD_KIND_DATETIME,
	VENTURE_FIELD_KIND_ENUM,
	VENTURE_FIELD_KIND_REFERENCE,
	VENTURE_FIELD_KIND_JSON
} VentureFieldKind;

/**
 * VentureAggregate:
 * @VENTURE_AGGREGATE_NONE: no aggregation; take the raw value
 * @VENTURE_AGGREGATE_SUM: add the values
 * @VENTURE_AGGREGATE_AVG: arithmetic mean
 * @VENTURE_AGGREGATE_MIN: smallest value
 * @VENTURE_AGGREGATE_MAX: largest value
 * @VENTURE_AGGREGATE_COUNT: number of rows
 * @VENTURE_AGGREGATE_COUNT_DISTINCT: number of distinct values
 *
 * How a metric folds many rows into one number.
 */
typedef enum
{
	VENTURE_AGGREGATE_NONE = 0,
	VENTURE_AGGREGATE_SUM,
	VENTURE_AGGREGATE_AVG,
	VENTURE_AGGREGATE_MIN,
	VENTURE_AGGREGATE_MAX,
	VENTURE_AGGREGATE_COUNT,
	VENTURE_AGGREGATE_COUNT_DISTINCT
} VentureAggregate;

/**
 * VentureFilterOp:
 * @VENTURE_FILTER_OP_EQ: equal
 * @VENTURE_FILTER_OP_NE: not equal
 * @VENTURE_FILTER_OP_LT: less than
 * @VENTURE_FILTER_OP_LTE: less than or equal
 * @VENTURE_FILTER_OP_GT: greater than
 * @VENTURE_FILTER_OP_GTE: greater than or equal
 * @VENTURE_FILTER_OP_LIKE: SQL LIKE, case sensitive
 * @VENTURE_FILTER_OP_ILIKE: case-insensitive substring match
 * @VENTURE_FILTER_OP_IN: value is in a list
 * @VENTURE_FILTER_OP_NOT_IN: value is not in a list
 * @VENTURE_FILTER_OP_IS_NULL: value is NULL
 * @VENTURE_FILTER_OP_NOT_NULL: value is not NULL
 * @VENTURE_FILTER_OP_BETWEEN: value falls in an inclusive range
 *
 * Comparison operators available to #VentureQuery. This is the complete set
 * the REST API and the AI query tool accept; anything outside it is rejected
 * rather than interpolated, which is what keeps AI-generated filters from
 * becoming SQL injection.
 */
typedef enum
{
	VENTURE_FILTER_OP_EQ = 0,
	VENTURE_FILTER_OP_NE,
	VENTURE_FILTER_OP_LT,
	VENTURE_FILTER_OP_LTE,
	VENTURE_FILTER_OP_GT,
	VENTURE_FILTER_OP_GTE,
	VENTURE_FILTER_OP_LIKE,
	VENTURE_FILTER_OP_ILIKE,
	VENTURE_FILTER_OP_IN,
	VENTURE_FILTER_OP_NOT_IN,
	VENTURE_FILTER_OP_IS_NULL,
	VENTURE_FILTER_OP_NOT_NULL,
	VENTURE_FILTER_OP_BETWEEN
} VentureFilterOp;

/**
 * VentureSortDirection:
 * @VENTURE_SORT_ASCENDING: smallest first
 * @VENTURE_SORT_DESCENDING: largest first
 *
 * Sort order for a query.
 */
typedef enum
{
	VENTURE_SORT_ASCENDING = 0,
	VENTURE_SORT_DESCENDING
} VentureSortDirection;

/**
 * VentureOutputFormat:
 * @VENTURE_OUTPUT_FORMAT_TABLE: aligned columns for a terminal
 * @VENTURE_OUTPUT_FORMAT_JSON: JSON, the REST API's native form
 * @VENTURE_OUTPUT_FORMAT_YAML: YAML
 * @VENTURE_OUTPUT_FORMAT_CSV: comma-separated values
 * @VENTURE_OUTPUT_FORMAT_ORG: an org-mode table
 * @VENTURE_OUTPUT_FORMAT_HTML: an HTML fragment
 * @VENTURE_OUTPUT_FORMAT_TEXT: plain prose
 *
 * Rendering formats shared by reports and by venturectl. Org-mode is
 * included so that a report can be pasted straight into a notes file.
 */
typedef enum
{
	VENTURE_OUTPUT_FORMAT_TABLE = 0,
	VENTURE_OUTPUT_FORMAT_JSON,
	VENTURE_OUTPUT_FORMAT_YAML,
	VENTURE_OUTPUT_FORMAT_CSV,
	VENTURE_OUTPUT_FORMAT_ORG,
	VENTURE_OUTPUT_FORMAT_HTML,
	VENTURE_OUTPUT_FORMAT_TEXT
} VentureOutputFormat;

/**
 * VentureDatabaseBackend:
 * @VENTURE_DATABASE_BACKEND_SQLITE: an on-disk or in-memory SQLite file
 * @VENTURE_DATABASE_BACKEND_POSTGRES: a PostgreSQL server
 *
 * Which storage engine the database layer is talking to. Selected from the
 * scheme of the connection URI.
 */
typedef enum
{
	VENTURE_DATABASE_BACKEND_SQLITE = 0,
	VENTURE_DATABASE_BACKEND_POSTGRES
} VentureDatabaseBackend;

/**
 * VenturePluginKind:
 * @VENTURE_PLUGIN_KIND_NATIVE: a compiled .so loaded with GModule
 * @VENTURE_PLUGIN_KIND_CRISPY: a C source file compiled on demand by crispy
 * @VENTURE_PLUGIN_KIND_DECLARATIVE: a YAML definition with no code at all
 *
 * How a plugin was supplied. All three end up implementing the same
 * #VenturePlugin interface, so nothing downstream needs to care which is
 * which.
 */
typedef enum
{
	VENTURE_PLUGIN_KIND_NATIVE = 0,
	VENTURE_PLUGIN_KIND_CRISPY,
	VENTURE_PLUGIN_KIND_DECLARATIVE
} VenturePluginKind;

/**
 * VentureColumnFlags:
 * @VENTURE_COLUMN_FLAG_NONE: no special treatment
 * @VENTURE_COLUMN_FLAG_PRIMARY_KEY: the table's primary key
 * @VENTURE_COLUMN_FLAG_UNIQUE: values must be unique
 * @VENTURE_COLUMN_FLAG_INDEXED: create an index on this column
 * @VENTURE_COLUMN_FLAG_NOT_NULL: NULL is not permitted
 * @VENTURE_COLUMN_FLAG_IMMUTABLE: may be set on insert but never updated
 * @VENTURE_COLUMN_FLAG_SENSITIVE: never serialise this to an API response,
 *   a log line or anything the AI can see
 * @VENTURE_COLUMN_FLAG_SEARCHABLE: include in full-text search
 * @VENTURE_COLUMN_FLAG_TRANSIENT: computed at run time; never persisted
 * @VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION: nonempty values are unique within
 *   an organization, including soft-deleted records
 *
 * Per-property persistence hints. These are attached to a GObject property
 * with venture_entity_class_set_column_flags(), which is how a plain
 * GObject subclass becomes a database table without writing any SQL.
 */
typedef enum
{
	VENTURE_COLUMN_FLAG_NONE        = 0,
	VENTURE_COLUMN_FLAG_PRIMARY_KEY = 1 << 0,
	VENTURE_COLUMN_FLAG_UNIQUE      = 1 << 1,
	VENTURE_COLUMN_FLAG_INDEXED     = 1 << 2,
	VENTURE_COLUMN_FLAG_NOT_NULL    = 1 << 3,
	VENTURE_COLUMN_FLAG_IMMUTABLE   = 1 << 4,
	VENTURE_COLUMN_FLAG_SENSITIVE   = 1 << 5,
	VENTURE_COLUMN_FLAG_SEARCHABLE  = 1 << 6,
	VENTURE_COLUMN_FLAG_TRANSIENT   = 1 << 7,
	VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION = 1 << 8
} VentureColumnFlags;

/* --- GType registration -------------------------------------------------- */

#define VENTURE_TYPE_ORGANIZATION_KIND		(venture_organization_kind_get_type())
#define VENTURE_TYPE_VENTURE_STATUS		(venture_venture_status_get_type())
#define VENTURE_TYPE_ACCOUNT_KIND		(venture_account_kind_get_type())
#define VENTURE_TYPE_LEDGER_SIDE		(venture_ledger_side_get_type())
#define VENTURE_TYPE_INVENTORY_TXN_KIND		(venture_inventory_txn_kind_get_type())
#define VENTURE_TYPE_DEAL_STAGE			(venture_deal_stage_get_type())
#define VENTURE_TYPE_INTERACTION_KIND		(venture_interaction_kind_get_type())
#define VENTURE_TYPE_TICKET_STATUS		(venture_ticket_status_get_type())
#define VENTURE_TYPE_TICKET_KIND		(venture_ticket_kind_get_type())
#define VENTURE_TYPE_ISSUE_TYPE			(venture_issue_type_get_type())
#define VENTURE_TYPE_KB_FORMAT			(venture_kb_format_get_type())
#define VENTURE_TYPE_KB_ARTICLE_STATUS		(venture_kb_article_status_get_type())
#define VENTURE_TYPE_FORGE_KIND			(venture_forge_kind_get_type())
#define VENTURE_TYPE_FORGE_RUNNER		(venture_forge_runner_get_type())
#define VENTURE_TYPE_FORGE_RUN_OUTCOME		(venture_forge_run_outcome_get_type())
#define VENTURE_TYPE_FORGE_TRIGGER		(venture_forge_trigger_get_type())
#define VENTURE_TYPE_AGENT_SESSION_STATE	(venture_agent_session_state_get_type())
#define VENTURE_TYPE_FORGE_RUN_STATE		(venture_forge_run_state_get_type())
#define VENTURE_TYPE_FORGE_LINK_ORIGIN		(venture_forge_link_origin_get_type())
#define VENTURE_TYPE_INVOICE_STATUS		(venture_invoice_status_get_type())
#define VENTURE_TYPE_CHAT_ROLE			(venture_chat_role_get_type())
#define VENTURE_TYPE_COMPANY_KIND		(venture_company_kind_get_type())
#define VENTURE_TYPE_PRIORITY			(venture_priority_get_type())
#define VENTURE_TYPE_IDEA_STATUS		(venture_idea_status_get_type())
#define VENTURE_TYPE_CAMPAIGN_STATUS		(venture_campaign_status_get_type())
#define VENTURE_TYPE_POST_STATUS		(venture_post_status_get_type())
#define VENTURE_TYPE_SUBSCRIBER_STATUS		(venture_subscriber_status_get_type())
#define VENTURE_TYPE_DEDUCTIBILITY		(venture_deductibility_get_type())
#define VENTURE_TYPE_ACTOR_KIND			(venture_actor_kind_get_type())
#define VENTURE_TYPE_AUDIT_ACTION		(venture_audit_action_get_type())
#define VENTURE_TYPE_AI_POLICY			(venture_ai_policy_get_type())
#define VENTURE_TYPE_CONFIRMATION_STATE		(venture_confirmation_state_get_type())
#define VENTURE_TYPE_USER_ROLE			(venture_user_role_get_type())
#define VENTURE_TYPE_FIELD_KIND			(venture_field_kind_get_type())
#define VENTURE_TYPE_AGGREGATE			(venture_aggregate_get_type())
#define VENTURE_TYPE_FILTER_OP			(venture_filter_op_get_type())
#define VENTURE_TYPE_SORT_DIRECTION		(venture_sort_direction_get_type())
#define VENTURE_TYPE_OUTPUT_FORMAT		(venture_output_format_get_type())
#define VENTURE_TYPE_DATABASE_BACKEND		(venture_database_backend_get_type())
#define VENTURE_TYPE_PLUGIN_KIND		(venture_plugin_kind_get_type())
#define VENTURE_TYPE_COLUMN_FLAGS		(venture_column_flags_get_type())

GType venture_organization_kind_get_type	(void) G_GNUC_CONST;
GType venture_venture_status_get_type		(void) G_GNUC_CONST;
GType venture_account_kind_get_type		(void) G_GNUC_CONST;
GType venture_ledger_side_get_type		(void) G_GNUC_CONST;
GType venture_inventory_txn_kind_get_type	(void) G_GNUC_CONST;
GType venture_deal_stage_get_type		(void) G_GNUC_CONST;
GType venture_interaction_kind_get_type		(void) G_GNUC_CONST;
GType venture_ticket_status_get_type		(void) G_GNUC_CONST;
GType venture_ticket_kind_get_type		(void) G_GNUC_CONST;
GType venture_issue_type_get_type		(void) G_GNUC_CONST;
GType venture_kb_format_get_type		(void) G_GNUC_CONST;
GType venture_kb_article_status_get_type	(void) G_GNUC_CONST;
GType venture_forge_kind_get_type		(void) G_GNUC_CONST;
GType venture_forge_runner_get_type		(void) G_GNUC_CONST;
GType venture_forge_run_outcome_get_type	(void) G_GNUC_CONST;
GType venture_forge_trigger_get_type		(void) G_GNUC_CONST;
GType venture_agent_session_state_get_type	(void) G_GNUC_CONST;
GType venture_forge_run_state_get_type		(void) G_GNUC_CONST;
GType venture_forge_link_origin_get_type	(void) G_GNUC_CONST;
GType venture_invoice_status_get_type		(void) G_GNUC_CONST;
GType venture_chat_role_get_type		(void) G_GNUC_CONST;
GType venture_company_kind_get_type		(void) G_GNUC_CONST;
GType venture_priority_get_type			(void) G_GNUC_CONST;
GType venture_idea_status_get_type		(void) G_GNUC_CONST;
GType venture_campaign_status_get_type		(void) G_GNUC_CONST;
GType venture_post_status_get_type		(void) G_GNUC_CONST;
GType venture_subscriber_status_get_type	(void) G_GNUC_CONST;
GType venture_deductibility_get_type		(void) G_GNUC_CONST;
GType venture_actor_kind_get_type		(void) G_GNUC_CONST;
GType venture_audit_action_get_type		(void) G_GNUC_CONST;
GType venture_ai_policy_get_type		(void) G_GNUC_CONST;
GType venture_confirmation_state_get_type	(void) G_GNUC_CONST;
GType venture_user_role_get_type		(void) G_GNUC_CONST;
GType venture_field_kind_get_type		(void) G_GNUC_CONST;
GType venture_aggregate_get_type		(void) G_GNUC_CONST;
GType venture_filter_op_get_type		(void) G_GNUC_CONST;
GType venture_sort_direction_get_type		(void) G_GNUC_CONST;
GType venture_output_format_get_type		(void) G_GNUC_CONST;
GType venture_database_backend_get_type		(void) G_GNUC_CONST;
GType venture_plugin_kind_get_type		(void) G_GNUC_CONST;
GType venture_column_flags_get_type		(void) G_GNUC_CONST;

/* --- Nick conversion helpers --------------------------------------------- */

/**
 * venture_enum_to_nick:
 * @enum_type: a registered #GEnum type
 * @value: the enumeration value
 *
 * Converts an enumeration value to its stable string nick, which is the form
 * used in JSON, YAML, the database and AI tool arguments.
 *
 * Returns: (transfer none) (nullable): the nick, or %NULL if @value is not a
 *   member of @enum_type
 */
const gchar *
venture_enum_to_nick(
	GType	enum_type,
	gint	value
);

/**
 * venture_enum_from_nick:
 * @enum_type: a registered #GEnum type
 * @nick: the string nick to look up
 * @out_value: (out): return location for the enumeration value
 *
 * Converts a string nick back into an enumeration value. Both the nick and
 * the C identifier name are accepted, and matching is case insensitive, so
 * "in_progress", "IN_PROGRESS" and "VENTURE_TASK_STATUS_IN_PROGRESS" all
 * resolve. This tolerance matters because the AI supplies these strings.
 *
 * Returns: %TRUE if @nick was recognised
 */
gboolean
venture_enum_from_nick(
	GType		 enum_type,
	const gchar	*nick,
	gint		*out_value
);

/**
 * venture_enum_list_nicks:
 * @enum_type: a registered #GEnum type
 *
 * Lists every valid nick of @enum_type. Used to build the enum constraint in
 * an AI tool's parameter schema and the options of a select element in the
 * web UI, so both stay in sync with the C definition automatically.
 *
 * Returns: (transfer full) (array zero-terminated=1): a %NULL-terminated
 *   array of nicks. Free with g_strfreev().
 */
gchar **
venture_enum_list_nicks(GType enum_type);

/**
 * venture_account_kind_is_debit_normal:
 * @kind: an account class
 *
 * Determines whether an account of this class increases on the debit side.
 * Assets and expenses do; liabilities, equity and income do not. The ledger
 * uses this to turn a signed amount into the correct debit or credit line.
 *
 * Returns: %TRUE if the account is debit-normal
 */
gboolean
venture_account_kind_is_debit_normal(VentureAccountKind kind);

/**
 * venture_deal_stage_is_closed:
 * @stage: a pipeline stage
 *
 * Determines whether a deal in this stage is closed, won or lost. Open deals
 * are the ones that count toward pipeline value.
 *
 * Returns: %TRUE if the stage is terminal
 */
gboolean
venture_deal_stage_is_closed(VentureDealStage stage);

/**
 * venture_venture_status_is_operating:
 * @status: a venture lifecycle status
 *
 * Determines whether a venture in this status is considered to be running,
 * and therefore included in portfolio roll-ups by default.
 *
 * Returns: %TRUE if the venture is operating
 */
gboolean
venture_venture_status_is_operating(VentureVentureStatus status);

/**
 * VentureMilestoneStatus:
 * @VENTURE_MILESTONE_STATUS_PLANNED: not started
 * @VENTURE_MILESTONE_STATUS_ACTIVE: being worked
 * @VENTURE_MILESTONE_STATUS_COMPLETED: done
 * @VENTURE_MILESTONE_STATUS_CANCELLED: abandoned
 *
 * Where a milestone stands.
 */
typedef enum
{
	VENTURE_MILESTONE_STATUS_PLANNED = 0,
	VENTURE_MILESTONE_STATUS_ACTIVE,
	VENTURE_MILESTONE_STATUS_COMPLETED,
	VENTURE_MILESTONE_STATUS_CANCELLED
} VentureMilestoneStatus;

#define VENTURE_TYPE_MILESTONE_STATUS (venture_milestone_status_get_type())

GType
venture_milestone_status_get_type(void) G_GNUC_CONST;

/**
 * VentureReleaseStatus:
 * @VENTURE_RELEASE_STATUS_PLANNED: a version with a name and nothing in it yet
 * @VENTURE_RELEASE_STATUS_IN_PROGRESS: being assembled
 * @VENTURE_RELEASE_STATUS_RELEASED: out
 * @VENTURE_RELEASE_STATUS_YANKED: withdrawn after release
 *
 * Where a release stands. Released is recorded with a timestamp, which is
 * what the lead-time report measures to.
 */
typedef enum
{
	VENTURE_RELEASE_STATUS_PLANNED = 0,
	VENTURE_RELEASE_STATUS_IN_PROGRESS,
	VENTURE_RELEASE_STATUS_RELEASED,
	VENTURE_RELEASE_STATUS_YANKED
} VentureReleaseStatus;

#define VENTURE_TYPE_RELEASE_STATUS (venture_release_status_get_type())

GType
venture_release_status_get_type(void) G_GNUC_CONST;

/**
 * VentureBuildStatus:
 * @VENTURE_BUILD_STATUS_QUEUED: accepted by the CI, not started
 * @VENTURE_BUILD_STATUS_RUNNING: in progress
 * @VENTURE_BUILD_STATUS_SUCCEEDED: finished green
 * @VENTURE_BUILD_STATUS_FAILED: finished red
 * @VENTURE_BUILD_STATUS_CANCELLED: stopped before it finished
 *
 * Where a build stands. Queued is the zero value so a row written before
 * the CI reported anything reads as waiting rather than as green.
 */
typedef enum
{
	VENTURE_BUILD_STATUS_QUEUED = 0,
	VENTURE_BUILD_STATUS_RUNNING,
	VENTURE_BUILD_STATUS_SUCCEEDED,
	VENTURE_BUILD_STATUS_FAILED,
	VENTURE_BUILD_STATUS_CANCELLED
} VentureBuildStatus;

#define VENTURE_TYPE_BUILD_STATUS (venture_build_status_get_type())

GType
venture_build_status_get_type(void) G_GNUC_CONST;

/**
 * VentureBuildTrigger:
 * @VENTURE_BUILD_TRIGGER_MANUAL: recorded by a person
 * @VENTURE_BUILD_TRIGGER_WEBHOOK: reported by the forge's CI
 * @VENTURE_BUILD_TRIGGER_RULE: started by an automation rule
 * @VENTURE_BUILD_TRIGGER_RUN: produced by an AI coding run
 *
 * What started a build.
 */
typedef enum
{
	VENTURE_BUILD_TRIGGER_MANUAL = 0,
	VENTURE_BUILD_TRIGGER_WEBHOOK,
	VENTURE_BUILD_TRIGGER_RULE,
	VENTURE_BUILD_TRIGGER_RUN
} VentureBuildTrigger;

#define VENTURE_TYPE_BUILD_TRIGGER (venture_build_trigger_get_type())

GType
venture_build_trigger_get_type(void) G_GNUC_CONST;

/**
 * VentureEnvironmentKind:
 * @VENTURE_ENVIRONMENT_KIND_DEVELOPMENT: a developer's own
 * @VENTURE_ENVIRONMENT_KIND_STAGING: the rehearsal
 * @VENTURE_ENVIRONMENT_KIND_PRODUCTION: the one customers use
 * @VENTURE_ENVIRONMENT_KIND_OTHER: anything else
 *
 * What an environment is for.
 */
typedef enum
{
	VENTURE_ENVIRONMENT_KIND_DEVELOPMENT = 0,
	VENTURE_ENVIRONMENT_KIND_STAGING,
	VENTURE_ENVIRONMENT_KIND_PRODUCTION,
	VENTURE_ENVIRONMENT_KIND_OTHER
} VentureEnvironmentKind;

#define VENTURE_TYPE_ENVIRONMENT_KIND (venture_environment_kind_get_type())

GType
venture_environment_kind_get_type(void) G_GNUC_CONST;

/**
 * VentureDeploymentStatus:
 * @VENTURE_DEPLOYMENT_STATUS_PENDING: planned, not started
 * @VENTURE_DEPLOYMENT_STATUS_IN_PROGRESS: rolling out
 * @VENTURE_DEPLOYMENT_STATUS_SUCCEEDED: live
 * @VENTURE_DEPLOYMENT_STATUS_FAILED: did not go live
 * @VENTURE_DEPLOYMENT_STATUS_ROLLED_BACK: was live, then withdrawn
 *
 * Where a deployment stands. The latest succeeded deployment to an
 * environment is what that environment is running.
 */
typedef enum
{
	VENTURE_DEPLOYMENT_STATUS_PENDING = 0,
	VENTURE_DEPLOYMENT_STATUS_IN_PROGRESS,
	VENTURE_DEPLOYMENT_STATUS_SUCCEEDED,
	VENTURE_DEPLOYMENT_STATUS_FAILED,
	VENTURE_DEPLOYMENT_STATUS_ROLLED_BACK
} VentureDeploymentStatus;

#define VENTURE_TYPE_DEPLOYMENT_STATUS (venture_deployment_status_get_type())

GType
venture_deployment_status_get_type(void) G_GNUC_CONST;

/**
 * VentureIncidentSeverity:
 * @VENTURE_INCIDENT_SEVERITY_SEV1: everything is down
 * @VENTURE_INCIDENT_SEVERITY_SEV2: something important is down
 * @VENTURE_INCIDENT_SEVERITY_SEV3: degraded, with a workaround
 * @VENTURE_INCIDENT_SEVERITY_SEV4: cosmetic
 *
 * How bad an incident is. Sev3 is the zero value: most incidents are
 * annoying rather than critical, and a row written without a severity
 * should not read as an outage.
 */
typedef enum
{
	VENTURE_INCIDENT_SEVERITY_SEV3 = 0,
	VENTURE_INCIDENT_SEVERITY_SEV1,
	VENTURE_INCIDENT_SEVERITY_SEV2,
	VENTURE_INCIDENT_SEVERITY_SEV4
} VentureIncidentSeverity;

#define VENTURE_TYPE_INCIDENT_SEVERITY (venture_incident_severity_get_type())

GType
venture_incident_severity_get_type(void) G_GNUC_CONST;

/**
 * VentureIncidentStatus:
 * @VENTURE_INCIDENT_STATUS_OPEN: happening
 * @VENTURE_INCIDENT_STATUS_MITIGATED: contained, not fixed
 * @VENTURE_INCIDENT_STATUS_RESOLVED: fixed
 * @VENTURE_INCIDENT_STATUS_POSTMORTEM: fixed, and being written up
 *
 * Where an incident stands.
 */
typedef enum
{
	VENTURE_INCIDENT_STATUS_OPEN = 0,
	VENTURE_INCIDENT_STATUS_MITIGATED,
	VENTURE_INCIDENT_STATUS_RESOLVED,
	VENTURE_INCIDENT_STATUS_POSTMORTEM
} VentureIncidentStatus;

#define VENTURE_TYPE_INCIDENT_STATUS (venture_incident_status_get_type())

GType
venture_incident_status_get_type(void) G_GNUC_CONST;

/**
 * VentureDashboardPurpose:
 * @VENTURE_DASHBOARD_PURPOSE_OVERVIEW: a general view, the kind a home page is
 * @VENTURE_DASHBOARD_PURPOSE_REPORTING: figures and tables, for reading
 * @VENTURE_DASHBOARD_PURPOSE_WORK: queues and actions, for doing
 *
 * What a dashboard is for. It changes nothing about what a dashboard may
 * hold -- any widget goes on any dashboard -- and is there so a list of
 * twenty views can be sorted, and so a template lands in the right group.
 */
typedef enum
{
	VENTURE_DASHBOARD_PURPOSE_OVERVIEW = 0,
	VENTURE_DASHBOARD_PURPOSE_REPORTING,
	VENTURE_DASHBOARD_PURPOSE_WORK
} VentureDashboardPurpose;

#define VENTURE_TYPE_DASHBOARD_PURPOSE (venture_dashboard_purpose_get_type())

GType
venture_dashboard_purpose_get_type(void) G_GNUC_CONST;

/**
 * VentureDashboardLayout:
 * @VENTURE_DASHBOARD_LAYOUT_THREE_COLUMNS: three across, the default
 * @VENTURE_DASHBOARD_LAYOUT_TWO_COLUMNS: two across
 * @VENTURE_DASHBOARD_LAYOUT_FOUR_COLUMNS: four across, for many small figures
 * @VENTURE_DASHBOARD_LAYOUT_ONE_COLUMN: stacked, for a page that reads top
 *   to bottom
 *
 * How many columns a dashboard's grid has on a wide screen. Narrow screens
 * collapse every layout to one column.
 */
typedef enum
{
	VENTURE_DASHBOARD_LAYOUT_THREE_COLUMNS = 0,
	VENTURE_DASHBOARD_LAYOUT_TWO_COLUMNS,
	VENTURE_DASHBOARD_LAYOUT_FOUR_COLUMNS,
	VENTURE_DASHBOARD_LAYOUT_ONE_COLUMN
} VentureDashboardLayout;

#define VENTURE_TYPE_DASHBOARD_LAYOUT (venture_dashboard_layout_get_type())

GType
venture_dashboard_layout_get_type(void) G_GNUC_CONST;

/**
 * venture_dashboard_layout_get_columns:
 * @layout: a layout
 *
 * Returns: the number of grid columns, 1 to 4
 */
guint
venture_dashboard_layout_get_columns(VentureDashboardLayout layout);

/**
 * VentureWidgetSpan:
 * @VENTURE_WIDGET_SPAN_NORMAL: one grid column
 * @VENTURE_WIDGET_SPAN_WIDE: two grid columns
 * @VENTURE_WIDGET_SPAN_FULL: the whole row
 *
 * How much of a dashboard row a widget takes.
 */
typedef enum
{
	VENTURE_WIDGET_SPAN_NORMAL = 0,
	VENTURE_WIDGET_SPAN_WIDE,
	VENTURE_WIDGET_SPAN_FULL
} VentureWidgetSpan;

#define VENTURE_TYPE_WIDGET_SPAN (venture_widget_span_get_type())

GType
venture_widget_span_get_type(void) G_GNUC_CONST;

/**
 * VentureLinkKind:
 * @VENTURE_LINK_KIND_RELATED: connected, with no direction
 * @VENTURE_LINK_KIND_BLOCKS: the source cannot finish until the target does
 * @VENTURE_LINK_KIND_BLOCKED_BY: the reverse of blocks
 * @VENTURE_LINK_KIND_DEPENDS_ON: the source needs the target
 * @VENTURE_LINK_KIND_REQUIRED_BY: the reverse of depends on
 * @VENTURE_LINK_KIND_PARENT_OF: the target is part of the source
 * @VENTURE_LINK_KIND_CHILD_OF: the reverse of parent of
 * @VENTURE_LINK_KIND_DUPLICATES: the two are the same thing twice
 * @VENTURE_LINK_KIND_CAUSES: the source led to the target
 * @VENTURE_LINK_KIND_CAUSED_BY: the reverse of causes
 * @VENTURE_LINK_KIND_PRODUCES: the target came out of the source
 * @VENTURE_LINK_KIND_PRODUCED_BY: the reverse of produces
 * @VENTURE_LINK_KIND_REFERENCES: the source mentions the target
 * @VENTURE_LINK_KIND_REFERENCED_BY: the reverse of references
 * @VENTURE_LINK_KIND_SUPERSEDES: the source replaces the target
 * @VENTURE_LINK_KIND_SUPERSEDED_BY: the reverse of supersedes
 *
 * What a #VentureRecordLink means, read from its source. Every directed
 * kind has an inverse, and a link is stored once: standing on the target,
 * the same row reads as the inverse. Related and duplicates are their own
 * inverse.
 */
typedef enum
{
	VENTURE_LINK_KIND_RELATED = 0,
	VENTURE_LINK_KIND_BLOCKS,
	VENTURE_LINK_KIND_BLOCKED_BY,
	VENTURE_LINK_KIND_DEPENDS_ON,
	VENTURE_LINK_KIND_REQUIRED_BY,
	VENTURE_LINK_KIND_PARENT_OF,
	VENTURE_LINK_KIND_CHILD_OF,
	VENTURE_LINK_KIND_DUPLICATES,
	VENTURE_LINK_KIND_CAUSES,
	VENTURE_LINK_KIND_CAUSED_BY,
	VENTURE_LINK_KIND_PRODUCES,
	VENTURE_LINK_KIND_PRODUCED_BY,
	VENTURE_LINK_KIND_REFERENCES,
	VENTURE_LINK_KIND_REFERENCED_BY,
	VENTURE_LINK_KIND_SUPERSEDES,
	VENTURE_LINK_KIND_SUPERSEDED_BY
} VentureLinkKind;

#define VENTURE_TYPE_LINK_KIND (venture_link_kind_get_type())

GType
venture_link_kind_get_type(void) G_GNUC_CONST;

/**
 * VentureNotificationKind:
 * @VENTURE_NOTIFICATION_KIND_MENTION: somebody wrote @you in a comment
 * @VENTURE_NOTIFICATION_KIND_ASSIGNED: a ticket was handed to you
 * @VENTURE_NOTIFICATION_KIND_WATCHED: a record you watch changed
 * @VENTURE_NOTIFICATION_KIND_SLA: a ticket is about to miss, or missed,
 *   its service level
 * @VENTURE_NOTIFICATION_KIND_BUDGET: an agent budget crossed its warning
 *   line or ran out
 * @VENTURE_NOTIFICATION_KIND_RUN: a coding run finished, one way or another
 * @VENTURE_NOTIFICATION_KIND_SYSTEM: anything else the install wants to say
 *
 * Why a notification exists. The inbox groups and colours by it, and a
 * filter on it is how "just the mentions" is answered.
 */
typedef enum
{
	VENTURE_NOTIFICATION_KIND_MENTION = 0,
	VENTURE_NOTIFICATION_KIND_ASSIGNED,
	VENTURE_NOTIFICATION_KIND_WATCHED,
	VENTURE_NOTIFICATION_KIND_SLA,
	VENTURE_NOTIFICATION_KIND_BUDGET,
	VENTURE_NOTIFICATION_KIND_RUN,
	VENTURE_NOTIFICATION_KIND_SYSTEM
} VentureNotificationKind;

#define VENTURE_TYPE_NOTIFICATION_KIND (venture_notification_kind_get_type())

GType
venture_notification_kind_get_type(void) G_GNUC_CONST;

/**
 * VentureSprintStatus:
 * @VENTURE_SPRINT_STATUS_PLANNED: not started; tickets can still be moved in
 * @VENTURE_SPRINT_STATUS_ACTIVE: the one being worked
 * @VENTURE_SPRINT_STATUS_COMPLETED: over; what did not finish rolls forward
 *
 * Where a sprint stands. Planned is zero so a sprint made without saying
 * is one that has not begun.
 */
typedef enum
{
	VENTURE_SPRINT_STATUS_PLANNED = 0,
	VENTURE_SPRINT_STATUS_ACTIVE,
	VENTURE_SPRINT_STATUS_COMPLETED
} VentureSprintStatus;

#define VENTURE_TYPE_SPRINT_STATUS (venture_sprint_status_get_type())

GType
venture_sprint_status_get_type(void) G_GNUC_CONST;

/**
 * VentureBudgetPeriod:
 * @VENTURE_BUDGET_PERIOD_MONTHLY: the calendar month, resetting on the 1st
 * @VENTURE_BUDGET_PERIOD_WEEKLY: the ISO week, resetting on Monday
 * @VENTURE_BUDGET_PERIOD_ALL_TIME: never resets; a cap on the whole spend
 *
 * The window an agent budget's limit applies to.
 */
typedef enum
{
	VENTURE_BUDGET_PERIOD_MONTHLY = 0,
	VENTURE_BUDGET_PERIOD_WEEKLY,
	VENTURE_BUDGET_PERIOD_ALL_TIME
} VentureBudgetPeriod;

#define VENTURE_TYPE_BUDGET_PERIOD (venture_budget_period_get_type())

GType
venture_budget_period_get_type(void) G_GNUC_CONST;

/**
 * VentureSlaState:
 * @VENTURE_SLA_STATE_NONE: no policy covers the ticket, or it is closed
 * @VENTURE_SLA_STATE_OK: within the target with room to spare
 * @VENTURE_SLA_STATE_WARNING: inside the last fifth of the target
 * @VENTURE_SLA_STATE_BREACHED: the target has passed
 *
 * How a ticket stands against one of its service-level clocks. Computed
 * from the due time and the moment of asking, never stored -- the stored
 * fact is the due time, which does not change by being looked at.
 */
typedef enum
{
	VENTURE_SLA_STATE_NONE = 0,
	VENTURE_SLA_STATE_OK,
	VENTURE_SLA_STATE_WARNING,
	VENTURE_SLA_STATE_BREACHED
} VentureSlaState;

#define VENTURE_TYPE_SLA_STATE (venture_sla_state_get_type())

GType
venture_sla_state_get_type(void) G_GNUC_CONST;

/**
 * VentureDeliveryState:
 * @VENTURE_DELIVERY_STATE_PENDING: sent, nothing back yet
 * @VENTURE_DELIVERY_STATE_SUCCEEDED: the endpoint answered 2xx
 * @VENTURE_DELIVERY_STATE_FAILED: it answered something else, or nothing
 *
 * How one outbound webhook delivery went.
 */
typedef enum
{
	VENTURE_DELIVERY_STATE_PENDING = 0,
	VENTURE_DELIVERY_STATE_SUCCEEDED,
	VENTURE_DELIVERY_STATE_FAILED
} VentureDeliveryState;

#define VENTURE_TYPE_DELIVERY_STATE (venture_delivery_state_get_type())

GType
venture_delivery_state_get_type(void) G_GNUC_CONST;

/**
 * VentureRoutingStrategy:
 * @VENTURE_ROUTING_STRATEGY_ROUND_ROBIN: each new ticket to the next name
 *   in the list, in turn
 * @VENTURE_ROUTING_STRATEGY_LEAST_BUSY: to whoever holds the fewest open
 *   tickets right now
 * @VENTURE_ROUTING_STRATEGY_FIRST: always the first name; a queue with one
 *   owner
 *
 * How a routing rule picks whom a ticket goes to.
 */
typedef enum
{
	VENTURE_ROUTING_STRATEGY_ROUND_ROBIN = 0,
	VENTURE_ROUTING_STRATEGY_LEAST_BUSY,
	VENTURE_ROUTING_STRATEGY_FIRST
} VentureRoutingStrategy;

#define VENTURE_TYPE_ROUTING_STRATEGY (venture_routing_strategy_get_type())

GType
venture_routing_strategy_get_type(void) G_GNUC_CONST;

/**
 * VentureSatisfaction:
 * @VENTURE_SATISFACTION_UNRATED: nobody has said
 * @VENTURE_SATISFACTION_BAD: not good
 * @VENTURE_SATISFACTION_NEUTRAL: adequate
 * @VENTURE_SATISFACTION_GOOD: good
 *
 * What whoever raised a ticket made of how it went. Unrated is zero
 * deliberately: the column is added to a populated table, and every
 * historical ticket reads back as "nobody has said", which is true.
 */
typedef enum
{
	VENTURE_SATISFACTION_UNRATED = 0,
	VENTURE_SATISFACTION_BAD,
	VENTURE_SATISFACTION_NEUTRAL,
	VENTURE_SATISFACTION_GOOD
} VentureSatisfaction;

#define VENTURE_TYPE_SATISFACTION (venture_satisfaction_get_type())

GType
venture_satisfaction_get_type(void) G_GNUC_CONST;

/**
 * venture_link_kind_inverse:
 * @kind: a link kind
 *
 * The same link read from the other end: blocks becomes blocked by, parent
 * of becomes child of. The symmetric kinds are their own inverse.
 *
 * Returns: the inverse kind
 */
VentureLinkKind
venture_link_kind_inverse(VentureLinkKind kind);

/**
 * venture_link_kind_is_symmetric:
 * @kind: a link kind
 *
 * Returns: %TRUE if the kind reads the same from both ends
 */
gboolean
venture_link_kind_is_symmetric(VentureLinkKind kind);

/**
 * venture_link_kind_to_label:
 * @kind: a link kind
 *
 * Returns: (transfer none): the kind in words, e.g. "blocked by"
 */
const gchar *
venture_link_kind_to_label(VentureLinkKind kind);

/**
 * venture_filter_op_arity:
 * @op: a filter operator
 *
 * Reports how many operands @op takes: 0 for the NULL tests, 2 for BETWEEN,
 * and 1 for everything else. Used to validate a filter before it is compiled
 * into SQL.
 *
 * Returns: the operand count
 */
guint
venture_filter_op_arity(VentureFilterOp op);

G_END_DECLS

#endif /* VENTURE_ENUMS_H */
