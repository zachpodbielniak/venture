/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
GType
venture_asset_method_get_type(void)
{
	static gsize type_id = 0;
	if (g_once_init_enter(&type_id))
	{
		static const GEnumValue values[] = {
			{ VENTURE_ASSET_METHOD_STRAIGHT_LINE, "VENTURE_ASSET_METHOD_STRAIGHT_LINE", "straight_line" },
			{ VENTURE_ASSET_METHOD_DECLINING_BALANCE, "VENTURE_ASSET_METHOD_DECLINING_BALANCE", "declining_balance" },
			{ VENTURE_ASSET_METHOD_NONE, "VENTURE_ASSET_METHOD_NONE", "none" },
			{ 0, NULL, NULL }
		};
		GType id = g_enum_register_static("VentureAssetMethod", values);
		g_once_init_leave(&type_id, id);
	}
	return type_id;
}
GType
venture_asset_status_get_type(void)
{
	static gsize type_id = 0;
	if (g_once_init_enter(&type_id))
	{
		static const GEnumValue values[] = {
			{ VENTURE_ASSET_STATUS_DRAFT, "VENTURE_ASSET_STATUS_DRAFT", "draft" },
			{ VENTURE_ASSET_STATUS_IN_SERVICE, "VENTURE_ASSET_STATUS_IN_SERVICE", "in_service" },
			{ VENTURE_ASSET_STATUS_DISPOSED, "VENTURE_ASSET_STATUS_DISPOSED", "disposed" },
			{ VENTURE_ASSET_STATUS_WRITTEN_OFF, "VENTURE_ASSET_STATUS_WRITTEN_OFF", "written_off" },
			{ 0, NULL, NULL }
		};
		GType id = g_enum_register_static("VentureAssetStatus", values);
		g_once_init_leave(&type_id, id);
	}
	return type_id;
}
GType
venture_schedule_state_get_type(void)
{
	static gsize type_id = 0;
	if (g_once_init_enter(&type_id))
	{
		static const GEnumValue values[] = {
			{ VENTURE_SCHEDULE_STATE_SCHEDULED, "VENTURE_SCHEDULE_STATE_SCHEDULED", "scheduled" },
			{ VENTURE_SCHEDULE_STATE_POSTED, "VENTURE_SCHEDULE_STATE_POSTED", "posted" },
			{ VENTURE_SCHEDULE_STATE_SKIPPED, "VENTURE_SCHEDULE_STATE_SKIPPED", "skipped" },
			{ 0, NULL, NULL }
		};
		GType id = g_enum_register_static("VentureScheduleState", values);
		g_once_init_leave(&type_id, id);
	}
	return type_id;
}
GType
venture_deferral_kind_get_type(void)
{
	static gsize type_id = 0;
	if (g_once_init_enter(&type_id))
	{
		static const GEnumValue values[] = {
			{ VENTURE_DEFERRAL_KIND_PREPAYMENT, "VENTURE_DEFERRAL_KIND_PREPAYMENT", "prepayment" },
			{ VENTURE_DEFERRAL_KIND_ACCRUAL, "VENTURE_DEFERRAL_KIND_ACCRUAL", "accrual" },
			{ 0, NULL, NULL }
		};
		GType id = g_enum_register_static("VentureDeferralKind", values);
		g_once_init_leave(&type_id, id);
	}
	return type_id;
}
GType
venture_deferral_status_get_type(void)
{
	static gsize type_id = 0;
	if (g_once_init_enter(&type_id))
	{
		static const GEnumValue values[] = {
			{ VENTURE_DEFERRAL_STATUS_ACTIVE, "VENTURE_DEFERRAL_STATUS_ACTIVE", "active" },
			{ VENTURE_DEFERRAL_STATUS_COMPLETE, "VENTURE_DEFERRAL_STATUS_COMPLETE", "complete" },
			{ 0, NULL, NULL }
		};
		GType id = g_enum_register_static("VentureDeferralStatus", values);
		g_once_init_leave(&type_id, id);
	}
	return type_id;
}
static const VentureFieldDecl fixed_asset_fields[] = {
	VENTURE_FIELD_REF("proceeds-account-id", "Disposal cash account", NULL, "account", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("gain-loss-account-id", "Disposal gain/loss account", "Defaults to depreciation expense for write-offs", "account", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("schedule-note", "Schedule note", "Service explains a move to an open period", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("operation", "Operation", "Stage place, dispose or write-off through VentureAssetService", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_TRANSIENT),
	VENTURE_FIELD_NAME("name", "Name", NULL),
	VENTURE_FIELD("tag", "Tag", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION),
	VENTURE_FIELD_REF("venture-id", "Venture Id", NULL, "venture", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("category", "Category", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("acquired-at", "Acquired At", NULL, VENTURE_FIELD_KIND_DATE, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_MONEY("cost", "Cost", NULL),
	VENTURE_FIELD_MONEY("salvage-value", "Salvage Value", NULL),
	VENTURE_FIELD("useful-life-months", "Useful Life Months", NULL, VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_ENUM("method", "Method", NULL, venture_asset_method_get_type, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("asset-account-id", "Asset Account Id", NULL, "account", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("accumulated-depreciation-account-id", "Accumulated Depreciation Account Id", NULL, "account", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("depreciation-expense-account-id", "Depreciation Expense Account Id", NULL, "account", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_ENUM("status", "Status", NULL, venture_asset_status_get_type, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("in-service-at", "In Service At", NULL, VENTURE_FIELD_KIND_DATE, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("disposed-at", "Disposed At", NULL, VENTURE_FIELD_KIND_DATE, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_MONEY("disposal-proceeds", "Disposal Proceeds", NULL),
	VENTURE_FIELD_REF("source-expense-id", "Source Expense Id", NULL, "expense", VENTURE_COLUMN_FLAG_NONE),
};
VENTURE_DEFINE_ENTITY(VentureFixedAsset, venture_fixed_asset, fixed_asset_fields)
static const VentureFieldDecl depreciation_entry_fields[] = {
	VENTURE_FIELD_REF("asset-id", "Asset Id", NULL, "fixed_asset", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("period", "Period", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_MONEY("amount", "Amount", NULL),
	VENTURE_FIELD_REF("journal-id", "Journal Id", NULL, "journal", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_ENUM("state", "State", NULL, venture_schedule_state_get_type, VENTURE_COLUMN_FLAG_NONE),
};
VENTURE_DEFINE_ENTITY(VentureDepreciationEntry, venture_depreciation_entry, depreciation_entry_fields)
static const VentureFieldDecl deferral_fields[] = {
	VENTURE_FIELD_ENUM("kind", "Kind", NULL, venture_deferral_kind_get_type, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_NAME("description", "Description", NULL),
	VENTURE_FIELD_MONEY("total", "Total", NULL),
	VENTURE_FIELD("start", "Start", NULL, VENTURE_FIELD_KIND_DATE, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("months", "Months", NULL, VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("source-account-id", "Source Account Id", NULL, "account", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("target-account-id", "Target Account Id", NULL, "account", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("source-expense-id", "Source Expense Id", NULL, "expense", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("source-invoice-id", "Source Invoice Id", NULL, "invoice", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_ENUM("status", "Status", NULL, venture_deferral_status_get_type, VENTURE_COLUMN_FLAG_NONE),
};
VENTURE_DEFINE_ENTITY(VentureDeferral, venture_deferral, deferral_fields)
static const VentureFieldDecl deferral_entry_fields[] = {
	VENTURE_FIELD_REF("deferral-id", "Deferral Id", NULL, "deferral", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("period", "Period", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_MONEY("amount", "Amount", NULL),
	VENTURE_FIELD_REF("journal-id", "Journal Id", NULL, "journal", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_ENUM("state", "State", NULL, venture_schedule_state_get_type, VENTURE_COLUMN_FLAG_NONE),
};
VENTURE_DEFINE_ENTITY(VentureDeferralEntry, venture_deferral_entry, deferral_entry_fields)
