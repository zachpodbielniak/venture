/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
static const VentureFieldDecl progress_fields[] = {
	VENTURE_FIELD_REF("quote-id", "Quote", "Accepted contract", "quote",
		VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_REF("invoice-id", "Invoice", NULL, "invoice", VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("kind", "Kind", "progress, retainer or retention",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("percent", "Percent billed", "Of the original agreement, 0..100",
		VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_MONEY("amount", "Amount billed", "Exact billed slice"),
	VENTURE_FIELD("billed-at", "Billed at", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_INDEXED)
};
VENTURE_DEFINE_ENTITY(VentureProgressBilling, venture_progress_billing, progress_fields)
static const VentureFieldDecl retainer_fields[] = {
	VENTURE_FIELD_REF("company-id", "Customer", NULL, "company",
		VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_REF("liability-account-id", "Liability account", "Unearned / retainer liability",
		"account", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_MONEY("amount", "Collected", "Advance received"),
	VENTURE_FIELD_MONEY("remaining", "Remaining liability", "Unreleased balance"),
	VENTURE_FIELD("collected-at", "Collected at", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("released-at", "Released at", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VentureCustomerRetainer, venture_customer_retainer, retainer_fields)
static const VentureFieldDecl retention_fields[] = {
	VENTURE_FIELD_REF("quote-id", "Quote", NULL, "quote",
		VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_REF("liability-account-id", "Liability account", NULL, "account",
		VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_MONEY("amount", "Held", "Retention withheld"),
	VENTURE_FIELD_MONEY("remaining", "Remaining held", NULL),
	VENTURE_FIELD("held-at", "Held at", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("released-at", "Released at", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VentureContractRetention, venture_contract_retention, retention_fields)
