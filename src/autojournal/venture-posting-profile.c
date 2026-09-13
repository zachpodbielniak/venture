/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

static gboolean
profile_before_save(VentureEntity *entity, GError **error)
{
	if (venture_entity_get_organization_id(entity) <= 0)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"A posting profile requires an organization");
		return FALSE;
	}
	/* One durable key per legal entity, including retained deleted profiles. */
	g_object_set(entity, "profile-key", "default", NULL);
	return TRUE;
}
#define ACCOUNT(field, label) VENTURE_FIELD_REF(field, label, NULL, "account", VENTURE_COLUMN_FLAG_NOT_NULL)
static const VentureFieldDecl profile_fields[] = {
	VENTURE_FIELD("profile-key", "Profile", "One profile per organization", VENTURE_FIELD_KIND_STRING,
		VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION),
	ACCOUNT("sales-account-id", "Sales income"),
	ACCOUNT("fees-account-id", "Platform fees"),
	ACCOUNT("tax-account-id", "Sales tax payable"),
	ACCOUNT("shipping-income-account-id", "Shipping income"),
	ACCOUNT("shipping-expense-account-id", "Shipping expense"),
	ACCOUNT("discounts-account-id", "Discounts"),
	ACCOUNT("refunds-account-id", "Refunds"),
	ACCOUNT("cogs-account-id", "Cost of goods sold"),
	ACCOUNT("inventory-account-id", "Inventory"),
	ACCOUNT("cash-account-id", "Cash or clearing"),
	ACCOUNT("payable-account-id", "Accounts payable"),
	ACCOUNT("default-expense-account-id", "Default expense"),
	VENTURE_FIELD("expense-categories", "Expense categories", "JSON object mapping category codes to account IDs",
		VENTURE_FIELD_KIND_JSON, VENTURE_COLUMN_FLAG_NONE)
};
#undef ACCOUNT
VENTURE_DEFINE_ENTITY_WITH_CODE(VenturePostingProfile, venture_posting_profile, profile_fields,
	VENTURE_ENTITY_CLASS(klass)->before_save = profile_before_save;)
