/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

static const VentureFieldDecl project_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", NULL),
	VENTURE_FIELD_REF("venture-id", "Venture", NULL, "venture", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("customer-id", "Customer", NULL, "company", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("currency", "Currency", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("billing-kind", "Billing kind", "time, cost or fixed", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_MONEY("budget", "Budget", NULL),
	VENTURE_FIELD_MONEY("retainer", "Retainer", NULL)
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
