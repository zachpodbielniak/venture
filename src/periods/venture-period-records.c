/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

GType
venture_period_state_get_type(void)
{
	static gsize type_id = 0;
	if (g_once_init_enter(&type_id))
	{
		static const GEnumValue values[] = {
			{ VENTURE_PERIOD_OPEN, "VENTURE_PERIOD_OPEN", "open" },
			{ VENTURE_PERIOD_CLOSED, "VENTURE_PERIOD_CLOSED", "closed" },
			{ VENTURE_PERIOD_LOCKED, "VENTURE_PERIOD_LOCKED", "locked" },
			{ 0, NULL, NULL }
		};
		GType id = g_enum_register_static("VenturePeriodState", values);
		g_once_init_leave(&type_id, id);
	}
	return type_id;
}

GType
venture_period_length_get_type(void)
{
	static gsize type_id = 0;
	if (g_once_init_enter(&type_id))
	{
		static const GEnumValue values[] = {
			{ VENTURE_PERIOD_MONTHLY, "VENTURE_PERIOD_MONTHLY", "monthly" },
			{ VENTURE_PERIOD_QUARTERLY, "VENTURE_PERIOD_QUARTERLY", "quarterly" },
			{ 0, NULL, NULL }
		};
		GType id = g_enum_register_static("VenturePeriodLength", values);
		g_once_init_leave(&type_id, id);
	}
	return type_id;
}

#define PERIOD_FIELDS \
	VENTURE_FIELD_NAME("name", "Name", NULL), \
	VENTURE_FIELD("start-at", "Start", "Inclusive boundary", VENTURE_FIELD_KIND_DATE, VENTURE_COLUMN_FLAG_NOT_NULL), \
	VENTURE_FIELD("end-at", "End", "Exclusive boundary", VENTURE_FIELD_KIND_DATE, VENTURE_COLUMN_FLAG_NOT_NULL), \
	VENTURE_FIELD_ENUM("state", "State", NULL, venture_period_state_get_type, VENTURE_COLUMN_FLAG_INDEXED), \
	VENTURE_FIELD("closed-by", "Closed by", "Set from the actor at close", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE), \
	VENTURE_FIELD("closed-at", "Closed at", "Set at close", VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE)

static const VentureFieldDecl fiscal_year_fields[] = {
	PERIOD_FIELDS,
	VENTURE_FIELD_ENUM("period-length", "Period length", NULL,
		venture_period_length_get_type, VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VentureFiscalYear, venture_fiscal_year, fiscal_year_fields)

static const VentureFieldDecl fiscal_period_fields[] = {
	PERIOD_FIELDS,
	VENTURE_FIELD_REF("fiscal-year-id", "Fiscal year", NULL,
		"fiscal_year", VENTURE_COLUMN_FLAG_NOT_NULL)
};
VENTURE_DEFINE_ENTITY(VentureFiscalPeriod, venture_fiscal_period, fiscal_period_fields)
#undef PERIOD_FIELDS

static const VentureFieldDecl report_snapshot_fields[] = {
	VENTURE_FIELD_REF("fiscal-period-id", "Fiscal period", NULL,
		"fiscal_period", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("report", "Report", NULL, VENTURE_FIELD_KIND_STRING,
		VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("as-of", "As of", NULL, VENTURE_FIELD_KIND_DATETIME,
		VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("closed-by", "Closed by", NULL, VENTURE_FIELD_KIND_STRING,
		VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("totals", "Totals", "Exact report metrics at close",
		VENTURE_FIELD_KIND_JSON, VENTURE_COLUMN_FLAG_NOT_NULL)
};
VENTURE_DEFINE_ENTITY(VentureReportSnapshot, venture_report_snapshot, report_snapshot_fields)

void
venture_period_records_register_constraints(void)
{
	g_autoptr(VentureEntityClass) klass = NULL;
	VentureColumnFlags flags;

	klass = g_type_class_ref(VENTURE_TYPE_ACCOUNT);
	flags = venture_entity_class_get_column_flags(klass, "code");
	venture_entity_class_set_column_flags(klass, "code",
		(flags & ~VENTURE_COLUMN_FLAG_UNIQUE) |
		VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION);
}
