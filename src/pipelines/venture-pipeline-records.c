/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

static GType
pipeline_kind_get_type(void)
{
	static gsize type_id;
	static const GEnumValue values[] = {
		{ 0, "PIPELINE_SALES", "sales" }, { 1, "PIPELINE_RENEWAL", "renewal" },
		{ 2, "PIPELINE_PARTNERSHIP", "partnership" }, { 3, "PIPELINE_CUSTOM", "custom" },
		{ 0, NULL, NULL }
	};
	if (g_once_init_enter(&type_id))
		g_once_init_leave(&type_id, g_enum_register_static("VenturePipelineKind", values));
	return type_id;
}

static GType
pipeline_stage_kind_get_type(void)
{
	static gsize type_id;
	static const GEnumValue values[] = {
		{ 0, "PIPELINE_STAGE_OPEN", "open" }, { 1, "PIPELINE_STAGE_WON", "won" },
		{ 2, "PIPELINE_STAGE_LOST", "lost" }, { 0, NULL, NULL }
	};
	if (g_once_init_enter(&type_id))
		g_once_init_leave(&type_id, g_enum_register_static("VenturePipelineStageKind", values));
	return type_id;
}

static const VentureFieldDecl pipeline_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", NULL),
	VENTURE_FIELD_ENUM("kind", "Kind", NULL, pipeline_kind_get_type, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("default", "Default", "Default process for this organization", VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("active", "Active", NULL, VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VenturePipeline, venture_pipeline, pipeline_fields)

static const VentureFieldDecl stage_fields[] = {
	VENTURE_FIELD_REF("pipeline-id", "Pipeline", NULL, "pipeline", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_NAME("name", "Name", NULL),
	VENTURE_FIELD("position", "Position", "Ascending process order", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("probability", "Probability", "Default percentage, 0-100", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_ENUM("kind", "Kind", NULL, pipeline_stage_kind_get_type, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("required-fields", "Required fields", "Comma-separated deal field names", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("rotting-days", "Rotting days", "Zero disables the overdue clock", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VenturePipelineStage, venture_pipeline_stage, stage_fields)

static const VentureFieldDecl entry_fields[] = {
	VENTURE_FIELD_REF("deal-id", "Deal", NULL, "deal", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_REF("from-stage", "From stage", NULL, "pipeline_stage", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("to-stage", "To stage", NULL, "pipeline_stage", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("entered-at", "Entered at", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("by", "By user", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("note", "Note", NULL)
};
VENTURE_DEFINE_ENTITY(VentureDealStageEntry, venture_deal_stage_entry, entry_fields)

static const VentureFieldDecl reason_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", NULL),
	VENTURE_FIELD("active", "Active", NULL, VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VentureLossReason, venture_loss_reason, reason_fields)

static const VentureFieldDecl deal_line_fields[] = {
	VENTURE_FIELD_REF("deal-id", "Deal", NULL, "deal", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_REF("product-id", "Product", NULL, "product", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("description", "Description", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("quantity", "Quantity", "Whole units", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_MONEY("unit-price", "Unit price", NULL),
	VENTURE_FIELD("discount-bp", "Discount (basis points)", "Integer basis points, 0-10000; 1250 is 12.5%", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("position", "Position", NULL, VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VentureDealLine, venture_deal_line, deal_line_fields)
