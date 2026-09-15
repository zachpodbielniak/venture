/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

static gboolean
setup_before_save(VentureEntity *entity, GError **error)
{
	if (venture_entity_get_organization_id(entity) <= 0)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"Accounting setup belongs to one legal entity");
		return FALSE;
	}
	g_object_set(entity, "setup-key", "default", NULL);
	return TRUE;
}

static const VentureFieldDecl setup_fields[] = {
	VENTURE_FIELD("setup-key", "Setup", "One checklist per legal entity", VENTURE_FIELD_KIND_STRING,
		VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION),
	VENTURE_FIELD_NAME("state", "State", "preview or complete"),
	VENTURE_FIELD("legal-name", "Legal name", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD("book-currency", "Book currency", "ISO 4217 book currency", VENTURE_FIELD_KIND_STRING,
		VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("basis", "Basis", "accrual or cash", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("tax-profile", "Tax profile", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("chart-template", "Chart template", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("bank-name", "Operating bank", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_MONEY("opening-cash", "Opening cash", "Posted against retained earnings at the fiscal year start"),
	VENTURE_FIELD("fiscal-year-start", "Fiscal year start", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("period-length", "Period length", "monthly or quarterly", VENTURE_FIELD_KIND_STRING,
		VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("payload", "Payload", "Resumable setup answers as JSON"),
	VENTURE_FIELD_TEXT("checklist", "Checklist", "Computed setup steps")
};
VENTURE_DEFINE_ENTITY_WITH_CODE(VentureAccountingSetup, venture_accounting_setup, setup_fields,
	VENTURE_ENTITY_CLASS(klass)->before_save = setup_before_save;)

static gboolean
map_before_save(VentureEntity *entity, GError **error)
{
	static const gchar *const classes[] = {
		"cash", "receivables", "payables", "tax", "deferred", "retained_earnings",
		"clearing", "inventory", "income", "expense", NULL
	};
	g_autofree gchar *classification = NULL;
	g_autofree gchar *subject_type = NULL;
	g_autoptr(GDateTime) effective = NULL;
	g_autofree gchar *when = NULL;
	gint64 account_id = 0;
	gint64 subject_id = 0;
	gsize i;
	gboolean known = FALSE;
	if (venture_entity_get_organization_id(entity) <= 0)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"A control-account mapping belongs to one legal entity");
		return FALSE;
	}
	g_object_get(entity, "classification", &classification, "account-id", &account_id,
		"subject-type", &subject_type, "subject-id", &subject_id, "effective-from", &effective, NULL);
	if (classification == NULL || classification[0] == '\0')
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"Map a semantic classification such as cash or accounts receivable, not a chart code");
		return FALSE;
	}
	for (i = 0; classes[i] != NULL; i++)
		if (g_str_equal(classification, classes[i]))
			known = TRUE;
	if (!known)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"Unknown control-account classification '%s'", classification);
		return FALSE;
	}
	if (account_id <= 0)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"A control-account mapping must name an account");
		return FALSE;
	}
	if (subject_type == NULL || subject_type[0] == '\0')
		g_object_set(entity, "subject-type", "organization", NULL);
	if (effective != NULL)
		when = g_date_time_format(effective, "%Y-%m-%dT%H:%M:%SZ");
	{
		g_autofree gchar *key = g_strdup_printf("%s|%s|%" G_GINT64_FORMAT "|%s",
			classification,
			subject_type != NULL && subject_type[0] != '\0' ? subject_type : "organization",
			subject_id, when != NULL ? when : "");
		g_object_set(entity, "map-key", key, NULL);
	}
	return TRUE;
}

static const VentureFieldDecl map_fields[] = {
	VENTURE_FIELD("map-key", "Map key", "Classification, subject and effective date", VENTURE_FIELD_KIND_STRING,
		VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION),
	VENTURE_FIELD_NAME("classification", "Classification", "Semantic role: cash, receivables, payables, tax, retained_earnings, clearing, inventory, income, expense"),
	VENTURE_FIELD_REF("account-id", "Account", NULL, "account", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("subject-type", "Subject type", "organization, bank_account or product", VENTURE_FIELD_KIND_STRING,
		VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("subject-id", "Subject", "Bank or item override; zero is the organization default",
		VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("effective-from", "Effective from", "Dated restatement; empty means from the first books",
		VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY_WITH_CODE(VentureAccountingControlMap, venture_accounting_control_map, map_fields,
	VENTURE_ENTITY_CLASS(klass)->before_save = map_before_save;)
