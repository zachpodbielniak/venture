/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

GType
venture_project_delivery_status_get_type(void)
{
	static gsize type;
	static const GEnumValue values[] = {
		{ VENTURE_PROJECT_DELIVERY_ACTIVE, "VENTURE_PROJECT_DELIVERY_ACTIVE", "active" },
		{ VENTURE_PROJECT_DELIVERY_PAUSED, "VENTURE_PROJECT_DELIVERY_PAUSED", "paused" },
		{ VENTURE_PROJECT_DELIVERY_COMPLETED, "VENTURE_PROJECT_DELIVERY_COMPLETED", "completed" },
		{ VENTURE_PROJECT_DELIVERY_CANCELLED, "VENTURE_PROJECT_DELIVERY_CANCELLED", "cancelled" },
		{ 0, NULL, NULL }
	};
	if (g_once_init_enter(&type))
	{
		GType registered = g_enum_register_static("VentureProjectDeliveryStatus", values);
		g_once_init_leave(&type, registered);
	}
	return type;
}

static const VentureFieldDecl project_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", NULL),
	VENTURE_FIELD_REF("venture-id", "Venture", NULL, "venture", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("customer-id", "Customer", NULL, "company", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("currency", "Currency", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("billing-kind", "Billing kind", "time, cost or fixed", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_MONEY("budget", "Budget", NULL),
	VENTURE_FIELD_MONEY("retainer", "Retainer", NULL),
	VENTURE_FIELD("owner", "Owner", "Responsible username", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_ASSIGNED_USERNAME),
	VENTURE_FIELD_ENUM("delivery-status", "Delivery status", "Independent of invoice/payment state", venture_project_delivery_status_get_type, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("delivery-note", "Latest delivery decision", "Prior decisions remain in the audit history"),
	VENTURE_FIELD_REF("quote-id", "Original agreement", "Set by sales handoff", "quote", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("deal-id", "Original deal", "Set by sales handoff", "deal", VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VentureClientProject, venture_client_project, project_fields)

static const VentureFieldDecl rate_fields[] = {
	VENTURE_FIELD_REF("project-id", "Project", NULL, "client_project", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_NAME("role", "Role", NULL),
	VENTURE_FIELD_MONEY("billing-rate", "Billing rate per hour", NULL),
	VENTURE_FIELD_MONEY("cost-rate", "Staff cost per hour", NULL)
};
VENTURE_DEFINE_ENTITY(VentureProjectRate, venture_project_rate, rate_fields)

static const VentureFieldDecl time_fields[] = {
	VENTURE_FIELD_REF("project-id", "Project", NULL, "client_project", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_REF("rate-id", "Rate", NULL, "project_rate", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("worklog-id", "Worklog", NULL, "worklog", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("minutes", "Minutes", NULL, VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("occurred-at", "When", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("approved", "Approved", NULL, VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_MONEY("amount", "Billable amount", NULL),
	VENTURE_FIELD_MONEY("actual-cost", "Frozen labour cost", "Unknown for historical approvals without cost evidence")
};
VENTURE_DEFINE_ENTITY(VentureProjectTime, venture_project_time, time_fields)

static const VentureFieldDecl cost_fields[] = {
	VENTURE_FIELD_REF("project-id", "Project", NULL, "client_project", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_REF("expense-id", "Expense", NULL, "expense", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_MONEY("amount", "Amount", NULL),
	VENTURE_FIELD("billable", "Billable", NULL, VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("occurred-at", "When", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VentureProjectCost, venture_project_cost, cost_fields)

static const VentureFieldDecl billing_fields[] = {
	VENTURE_FIELD_REF("project-id", "Project", NULL, "client_project", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("source-type", "Source type", "project_time or project_cost", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("source-id", "Source id", NULL, VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_REF("invoice-id", "Invoice", NULL, "invoice", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_REF("invoice-line-id", "Invoice line", NULL, "invoice_line", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_MONEY("amount", "Billed amount", NULL)
};
VENTURE_DEFINE_ENTITY(VentureProjectBilling, venture_project_billing, billing_fields)

/* Scope is retained contract evidence, not a second quotation engine. */
static const VentureFieldDecl scope_fields[] = {
	VENTURE_FIELD_REF("project-id", "Project", NULL, "client_project", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("source-key", "Sales identity", "Unique quote or deal handoff", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION),
	VENTURE_FIELD_REF("quote-id", "Accepted quote", NULL, "quote", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("deal-id", "Won deal", NULL, "deal", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("scope", "Agreed scope", "Frozen at handoff or change approval"),
	VENTURE_FIELD("request-digest", "Request digest", "Detects changed replay parameters", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("owner", "Owner at agreement", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_MONEY("amount", "Agreed value", NULL),
	VENTURE_FIELD("agreed-at", "Recorded at", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VentureProjectScope, venture_project_scope, scope_fields)

static const VentureFieldDecl deliverable_fields[] = {
	VENTURE_FIELD_REF("project-id", "Project", NULL, "client_project", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_REF("scope-id", "Agreement", NULL, "project_scope", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_REF("ticket-id", "Planned work", "Work status and time remain on the ticket", "ticket", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("request-key", "Planning identity", "Project UUID plus caller's stable key", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION),
	VENTURE_FIELD("request-digest", "Request digest", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_MONEY("amount", "Fixed billing slice", "Use time approval for time-and-materials projects"),
	VENTURE_FIELD("accepted-at", "Accepted at", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("accepted-by", "Accepted by", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("acceptance", "Acceptance evidence", NULL),
	VENTURE_FIELD_REF("invoice-id", "Invoice", "Set by verified billing action", "invoice", VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VentureProjectDeliverable, venture_project_deliverable, deliverable_fields)
