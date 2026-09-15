/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

static void
tag_payroll(GType type)
{
	g_type_set_qdata(type, g_quark_from_static_string("venture-access-payroll"),
		GINT_TO_POINTER(TRUE));
}

static const VentureFieldDecl run_fields[] = {
	VENTURE_FIELD("run-key", "Run key", "Unique imported pay-run identity", VENTURE_FIELD_KIND_STRING,
		VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION | VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD("period-start", "Period start", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("period-end", "Period end", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_NAME("currency", "Currency", "Uppercase ISO 4217 code"),
	VENTURE_FIELD_NAME("status", "Status", "imported, disbursed or reversed; VenturePayrollService owns transitions"),
	VENTURE_FIELD("net-disbursed", "Net disbursed", NULL, VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("tax-disbursed", "Tax disbursed", NULL, VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("memo", "Memo", NULL)
};
VENTURE_DEFINE_ENTITY_WITH_CODE(VenturePayrollRun, venture_payroll_run, run_fields,
	tag_payroll(G_TYPE_FROM_CLASS(klass));)

static const VentureFieldDecl line_fields[] = {
	VENTURE_FIELD_REF("run-id", "Pay run", NULL, "payroll_run", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_NAME("employee", "Employee", "Imported employee name"),
	VENTURE_FIELD_MONEY("gross", "Gross", NULL),
	VENTURE_FIELD_MONEY("employer-cost", "Employer cost", NULL),
	VENTURE_FIELD_MONEY("deductions", "Deductions", NULL),
	VENTURE_FIELD_MONEY("net", "Net pay", NULL),
	VENTURE_FIELD_MONEY("liabilities", "Liabilities", "Tax and other amounts still payable")
};
VENTURE_DEFINE_ENTITY_WITH_CODE(VenturePayrollLine, venture_payroll_line, line_fields,
	tag_payroll(G_TYPE_FROM_CLASS(klass));)
