/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
#include <string.h>

static const VentureFieldDecl rule_fields[] = {
	VENTURE_FIELD("action", "Action", "post or pay", VENTURE_FIELD_KIND_STRING,
		VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("require-second-actor", "Require second actor",
		"The proposer cannot apply post or pay", VENTURE_FIELD_KIND_BOOLEAN,
		VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VentureAccountingApprovalRule, venture_accounting_approval_rule, rule_fields)

static const VentureFieldDecl approval_fields[] = {
	VENTURE_FIELD("action", "Action", "post or pay", VENTURE_FIELD_KIND_STRING,
		VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("record-type", "Record type", NULL, VENTURE_FIELD_KIND_STRING,
		VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("record-id", "Record id", NULL, VENTURE_FIELD_KIND_INTEGER,
		VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("proposer", "Proposer", "Actor who requested the action",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("approver", "Approver", "Second actor who applied it",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("state", "State", "pending or applied", VENTURE_FIELD_KIND_STRING,
		VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("proposal-digest", "Proposal digest",
		"SHA-256 of the intended write so an approval cannot move to another document",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED)
};
VENTURE_DEFINE_ENTITY(VentureAccountingApproval, venture_accounting_approval, approval_fields)
