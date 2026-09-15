/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

static const VentureFieldDecl custom_field_fields[] = {
	VENTURE_FIELD("record-type", "Record type", "Registered entity name this field attaches to",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_NAME("name", "Name", "Machine name stored on custom_field_value rows"),
	VENTURE_FIELD("kind", "Kind", "string, text, integer, boolean, enum, money, date or datetime",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("required", "Required", "Saves of that record type need a value",
		VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("options", "Options", "JSON; enum choices are a string array",
		VENTURE_FIELD_KIND_JSON, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("field-key", "Field key", "record-type:name, unique inside the organization",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION | VENTURE_COLUMN_FLAG_NOT_NULL)
};
VENTURE_DEFINE_ENTITY(VentureAccountingCustomField, venture_accounting_custom_field, custom_field_fields)

static const VentureFieldDecl layout_fields[] = {
	VENTURE_FIELD("record-type", "Record type", NULL, VENTURE_FIELD_KIND_STRING,
		VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("field-order", "Field order", "JSON array of field names",
		VENTURE_FIELD_KIND_JSON, VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("layout-key", "Layout key", "One layout per record type in an organization",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION | VENTURE_COLUMN_FLAG_NOT_NULL)
};
VENTURE_DEFINE_ENTITY(VentureAccountingLayout, venture_accounting_layout, layout_fields)

static const VentureFieldDecl value_fields[] = {
	VENTURE_FIELD("record-type", "Record type", NULL, VENTURE_FIELD_KIND_STRING,
		VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("record-id", "Record", "Owning row in record-type",
		VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("name", "Field name", NULL, VENTURE_FIELD_KIND_STRING,
		VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("value", "Value", "Private derived index; edit attributes on the owning record",
		VENTURE_FIELD_KIND_TEXT, VENTURE_COLUMN_FLAG_SENSITIVE),
	VENTURE_FIELD("value-key", "Value key", "record-type:id:name, unique inside the organization",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION | VENTURE_COLUMN_FLAG_NOT_NULL)
};
VENTURE_DEFINE_ENTITY(VentureCustomFieldValue, venture_custom_field_value, value_fields)
