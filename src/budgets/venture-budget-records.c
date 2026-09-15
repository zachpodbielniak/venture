/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

static const VentureFieldDecl budget_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", "Plan name for this legal entity"),
	VENTURE_FIELD("period", "Period", "YYYY or YYYY-MM this plan covers",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("currency", "Currency", "Uppercase book currency of the plan amounts",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("dimension", "Dimension", "Optional default department, location or class",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("status", "Status", "draft or active",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED)
};
VENTURE_DEFINE_ENTITY(VentureBudget, venture_budget, budget_fields)

static const VentureFieldDecl budget_line_fields[] = {
	VENTURE_FIELD_REF("budget-id", "Budget", NULL, "budget",
		VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_REF("account-id", "Account", NULL, "account",
		VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("period", "Period", "YYYY-MM this line covers; defaults to the parent plan",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_MONEY("amount", "Amount", "Planned amount in the plan currency"),
	VENTURE_FIELD("dimension", "Dimension", "Optional department, location, project or class",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED)
};
VENTURE_DEFINE_ENTITY(VentureBudgetLine, venture_budget_line, budget_line_fields)
