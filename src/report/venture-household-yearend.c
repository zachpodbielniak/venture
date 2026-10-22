/*
 * venture-household-yearend.c - the household's year end and warranties
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Two reports and one reminder sweep for a household:
 *
 * - household_tax_year: every purchase in the period tagged deductible,
 *   partly deductible, capital or "ask your tax preparer", with the
 *   receipts attached to it; the CSV is the export for whoever does the
 *   taxes.
 * - household_warranties: every document with a warranty end date, and
 *   how long is left.
 * - venture_household_warranty_remind(): an inbox reminder once per
 *   warranty end date, inside the last WARRANTY_LEAD_DAYS days.
 *
 * Privacy is not decided here. Every read runs under the caller's access
 * scope, so a member's unshared personal account, its purchases and its
 * receipts are absent from another member's export and list, and present
 * once the owner shares the account (see docs/household.org).
 */

#include "venture.h"

#include <string.h>

#define WARRANTY_LEAD_DAYS 30

static gint64
org_option(VentureContext *context, JsonObject *options)
{
	gint64 org = options != NULL ? venture_json_object_get_int(options, "organization_id", 0) : 0;
	return org > 0 ? org : venture_context_get_default_organization_id(context);
}

static gchar *
day_text(GDateTime *when)
{
	return when != NULL ? g_date_time_format(when, "%Y-%m-%d") : NULL;
}

static GDateTime *
date_field(VentureEntity *entity, const gchar *name)
{
	GDateTime *value = NULL;
	g_object_get(entity, name, &value, NULL);
	return value;
}

static gint64
int_field(VentureEntity *entity, const gchar *name)
{
	gint64 value = 0;
	g_object_get(entity, name, &value, NULL);
	return value;
}

/* Household words for each tax treatment; NULL for "not for tax". */
static gchar *
treatment_text(VentureEntity *expense)
{
	VentureDeductibility treatment = VENTURE_DEDUCTIBILITY_NONE;
	gint64 percent = 0;
	g_object_get(expense, "deductibility", &treatment, "business-use-percent", &percent, NULL);
	switch (treatment)
	{
	case VENTURE_DEDUCTIBILITY_FULL: return g_strdup("Deductible");
	case VENTURE_DEDUCTIBILITY_PARTIAL:
		return g_strdup_printf("Partly deductible (%" G_GINT64_FORMAT "%%)", percent);
	case VENTURE_DEDUCTIBILITY_CAPITAL: return g_strdup("Spread over several years");
	case VENTURE_DEDUCTIBILITY_REVIEW: return g_strdup("Ask your tax preparer");
	case VENTURE_DEDUCTIBILITY_NONE:
	default: return NULL;
	}
}

/* Titles of the documents attached to a purchase that the caller may see. */
static gchar *
receipt_titles(VentureDatabase *db, VentureEntity *expense, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_DOCUMENT);
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GString) titles = g_string_new(NULL);
	guint i;
	venture_query_set_organization(query, venture_entity_get_organization_id(expense));
	if (!venture_query_add_filter_int(query, "expense-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(expense), error))
		return NULL;
	venture_query_add_order(query, "title", VENTURE_SORT_ASCENDING, NULL);
	venture_query_set_limit(query, 0);
	rows = venture_database_find(db, query, error);
	if (rows == NULL)
		return NULL;
	for (i = 0; i < rows->len; i++)
	{
		g_autofree gchar *title = NULL;
		g_object_get(g_ptr_array_index(rows, i), "title", &title, NULL);
		if (titles->len > 0) g_string_append(titles, "; ");
		g_string_append(titles, title != NULL ? title : "(untitled)");
	}
	return g_string_free(g_steal_pointer(&titles), FALSE);
}

static VentureReportResult *
household_tax_year(VentureContext *context, VentureDateRange *period, JsonObject *options, GError **error)
{
	VentureDatabase *db = venture_context_get_database(context);
	g_autoptr(VentureReportResult) result = venture_report_result_new("Year-end for taxes", period);
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_EXPENSE);
	g_autoptr(GPtrArray) expenses = NULL;
	guint i, rows = 0, missing = 0;

	venture_query_set_organization(query, org_option(context, options));
	if (period != NULL && !venture_query_set_date_range(query, "occurred-at", period, error))
		return NULL;
	if (!venture_query_add_filter_int(query, "deductibility", VENTURE_FILTER_OP_NE, VENTURE_DEDUCTIBILITY_NONE, error))
		return NULL;
	venture_query_add_order(query, "occurred-at", VENTURE_SORT_ASCENDING, NULL);
	venture_query_set_limit(query, 0);
	expenses = venture_database_find(db, query, error);
	if (expenses == NULL)
		return NULL;

	venture_report_result_add_column(result, "date", "Date", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "description", "What", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "paid_to", "Paid to", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "category", "Category", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "treatment", "Tax", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "amount", "Amount", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "deductible", "Deductible", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "receipts", "Receipts", VENTURE_REPORT_COLUMN_TEXT);

	for (i = 0; i < expenses->len; i++)
	{
		VentureEntity *expense = g_ptr_array_index(expenses, i);
		g_autofree gchar *treatment = treatment_text(expense);
		g_autofree gchar *description = NULL, *vendor = NULL, *category = NULL, *day = NULL, *receipts = NULL;
		g_autoptr(VentureMoney) amount = NULL, deductible = NULL;
		g_autoptr(GDateTime) when = date_field(expense, "occurred-at");
		if (treatment == NULL)
			continue;
		receipts = receipt_titles(db, expense, error);
		if (receipts == NULL)
			return NULL;
		g_object_get(expense, "description", &description, "vendor", &vendor, "category", &category,
			"amount", &amount, NULL);
		deductible = venture_expense_get_deductible_amount(VENTURE_EXPENSE(expense), NULL);
		day = day_text(when);
		venture_report_result_begin_row(result);
		venture_report_result_set_text(result, "date", day != NULL ? day : "");
		venture_report_result_set_text(result, "description", description != NULL ? description : "");
		venture_report_result_set_text(result, "paid_to", vendor != NULL ? vendor : "");
		venture_report_result_set_text(result, "category", venture_string_is_empty(category) ? "(no category)" : category);
		venture_report_result_set_text(result, "treatment", treatment);
		venture_report_result_set_money(result, "amount", amount);
		venture_report_result_set_money(result, "deductible", deductible);
		venture_report_result_set_text(result, "receipts", *receipts != '\0' ? receipts : "No receipt attached");
		if (*receipts == '\0') missing++;
		rows++;
	}
	venture_report_result_add_metric(result, venture_metric_new_number("items", "Purchases", (gdouble)rows));
	venture_report_result_add_metric(result, venture_metric_new_number("missing_receipts", "Without a receipt", (gdouble)missing));
	venture_report_result_set_note(result,
		"Purchases tagged for tax in this period, for whoever does your taxes. "
		"\"Ask your tax preparer\" purchases count as zero until someone decides. "
		"Purchases from another member's personal account appear only once that member shares the account with the household.");
	return g_steal_pointer(&result);
}

static GPtrArray *
warranty_documents(VentureDatabase *db, gint64 org, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_DOCUMENT);
	g_autoptr(GPtrArray) rows = NULL;
	GPtrArray *warranties;
	guint i;
	if (org > 0)
		venture_query_set_organization(query, org);
	venture_query_add_order(query, "warranty-until", VENTURE_SORT_ASCENDING, NULL);
	venture_query_set_limit(query, 0);
	rows = venture_database_find(db, query, error);
	if (rows == NULL)
		return NULL;
	warranties = g_ptr_array_new_with_free_func(g_object_unref);
	for (i = 0; i < rows->len; i++)
	{
		g_autoptr(GDateTime) until = date_field(g_ptr_array_index(rows, i), "warranty-until");
		if (until != NULL)
			g_ptr_array_add(warranties, g_object_ref(g_ptr_array_index(rows, i)));
	}
	return warranties;
}

/* Whole calendar days from @from to @to; negative once @to has passed. */
static gint
days_between(GDateTime *from, GDateTime *to)
{
	GDate a, b;
	g_date_clear(&a, 1);
	g_date_clear(&b, 1);
	g_date_set_dmy(&a, g_date_time_get_day_of_month(from), g_date_time_get_month(from), g_date_time_get_year(from));
	g_date_set_dmy(&b, g_date_time_get_day_of_month(to), g_date_time_get_month(to), g_date_time_get_year(to));
	return g_date_days_between(&a, &b);
}

static VentureReportResult *
household_warranties(VentureContext *context, VentureDateRange *period, JsonObject *options, GError **error)
{
	VentureDatabase *db = venture_context_get_database(context);
	g_autoptr(VentureReportResult) result = venture_report_result_new("Warranties", period);
	g_autoptr(GPtrArray) rows = warranty_documents(db, org_option(context, options), error);
	g_autoptr(GDateTime) now = venture_time_now();
	guint i;
	if (rows == NULL)
		return NULL;
	venture_report_result_add_column(result, "title", "Warranty", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "purchase", "Purchase", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "ends", "Ends", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "status", "Status", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "record", "Record", VENTURE_REPORT_COLUMN_TEXT);
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *document = g_ptr_array_index(rows, i);
		g_autoptr(GDateTime) until = date_field(document, "warranty-until");
		g_autoptr(VentureEntity) expense = NULL;
		g_autofree gchar *title = NULL, *ends = day_text(until), *status = NULL, *what = NULL, *record = NULL;
		gint64 expense_id = int_field(document, "expense-id");
		gint left = days_between(now, until);
		g_object_get(document, "title", &title, NULL);
		/* Read under the caller's scope: a purchase they cannot see
		 * leaves the column empty rather than naming it. */
		if (expense_id > 0)
			expense = venture_database_get(db, VENTURE_TYPE_EXPENSE, expense_id, NULL);
		if (expense != NULL)
			g_object_get(expense, "description", &what, NULL);
		if (left < 0) status = g_strdup("Ended");
		else if (left == 0) status = g_strdup("Ends today");
		else status = g_strdup_printf("%d day%s left", left, left == 1 ? "" : "s");
		record = g_strdup_printf("document:%" G_GINT64_FORMAT, venture_entity_get_id(document));
		venture_report_result_begin_row(result);
		venture_report_result_set_text(result, "title", title != NULL ? title : "(untitled)");
		venture_report_result_set_text(result, "purchase", what != NULL ? what : "");
		venture_report_result_set_text(result, "ends", ends);
		venture_report_result_set_text(result, "status", status);
		venture_report_result_set_text(result, "record", record);
	}
	venture_report_result_set_note(result,
		"Every document with a warranty end date, soonest first, whatever the period. Set the reminder "
		"person on the document to get an inbox reminder in the last 30 days.");
	return g_steal_pointer(&result);
}

gint
venture_household_warranty_remind(VentureContext *context, gint64 organization_id, GDateTime *now, GError **error)
{
	VentureDatabase *db;
	g_autoptr(GDateTime) clock = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(VentureAccessScope) internal = NULL;
	guint i;
	gint sent = 0;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), -1);
	db = venture_context_get_database(context);
	clock = now != NULL ? g_date_time_ref(now) : venture_time_now();
	/* The sweep reads every warranty; each reminder is then checked
	 * against what its recipient may read today. */
	internal = venture_access_policy_enter(venture_database_get_access_policy(db), NULL);
	rows = warranty_documents(db, organization_id, error);
	if (rows == NULL)
		return -1;
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *document = g_ptr_array_index(rows, i);
		g_autoptr(GDateTime) until = date_field(document, "warranty-until");
		g_autoptr(GDateTime) told = date_field(document, "warranty-reminded-at");
		g_autofree gchar *until_day = day_text(until), *told_day = day_text(told);
		g_autofree gchar *title = NULL, *heading = NULL, *body = NULL;
		gint64 user = int_field(document, "warranty-reminder-user-id");
		gint left = days_between(clock, until);
		if (user <= 0 || left < 0 || left > WARRANTY_LEAD_DAYS)
			continue;
		if (told_day != NULL && g_strcmp0(told_day, until_day) == 0)
			continue;
		if (!venture_comment_user_can_read(context, user, document))
			continue;
		g_object_get(document, "title", &title, NULL);
		heading = g_strdup_printf("Warranty ends %s", until_day);
		body = g_strdup_printf("%s ends on %s, in %d day%s. Check it before then if anything needs a claim.",
			title != NULL ? title : "A warranty", until_day, left, left == 1 ? "" : "s");
		if (!venture_database_begin(db, error))
			return -1;
		/* The stamp is the end date reminded about: a new end date
		 * reminds again, the same one never twice. */
		g_object_set(document, "warranty-reminded-at", until, NULL);
		if (!venture_notify_send(context, user, VENTURE_NOTIFICATION_KIND_REMINDER, heading, body,
				"document", venture_entity_get_id(document), title, NULL, error) ||
			!venture_database_save(db, document, NULL, error) || !venture_database_commit(db, error))
		{
			venture_database_rollback(db);
			return -1;
		}
		sent++;
	}
	return sent;
}

/* A reminder needs a date, and a private warranty reminds only its owner. */
static gboolean
validate_warranty(VentureDatabase *database, VentureEntity *entity, VentureEntity *previous, gpointer data, GError **error)
{
	g_autoptr(GDateTime) until = date_field(entity, "warranty-until");
	gint64 user = int_field(entity, "warranty-reminder-user-id");
	gint64 owner;
	(void)previous; (void)data;
	if (user < 0 || (user > 0 && until == NULL))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"A warranty reminder needs the date the warranty ends");
		return FALSE;
	}
	if (user == 0)
		return TRUE;
	owner = venture_access_policy_get_personal_owner(venture_database_get_access_policy(database), entity);
	if (owner > 0 && owner != user)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"This warranty belongs to a private purchase; only its owner can be reminded about it");
		return FALSE;
	}
	return TRUE;
}

void
venture_household_yearend_install(VentureDatabase *database)
{
	g_return_if_fail(VENTURE_IS_DATABASE(database));
	venture_database_add_save_validator(database, VENTURE_TYPE_DOCUMENT, validate_warranty, NULL, NULL);
}

void
venture_household_yearend_register_reports(VentureReportRegistry *registry)
{
	g_return_if_fail(VENTURE_IS_REPORT_REGISTRY(registry));
	venture_report_registry_add(registry, VENTURE_REPORT(venture_func_report_new_classified(VENTURE_DATA_CLASS_TENANT,
		"household_tax_year", "Year-end for taxes",
		"Purchases tagged deductible, partly deductible, capital or \"ask your tax preparer\" in the period, "
		"with their receipts; export it as CSV for whoever does the taxes. Options: organization_id",
		household_tax_year)));
	venture_report_registry_add(registry, VENTURE_REPORT(venture_func_report_new_classified(VENTURE_DATA_CLASS_TENANT,
		"household_warranties", "Warranties",
		"Every document with a warranty end date, soonest first, with the days left. Options: organization_id",
		household_warranties)));
}
