/*
 * test-venture-household-yearend.c - household year-end export and warranties
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The export for whoever does the household's taxes, and warranty
 * reminders, both obeying the private-account contract: a member's
 * unshared personal account and everything derived from it stays private
 * to that member until the owner shares the account with the household.
 */

#include <venture.h>
#include <string.h>

typedef struct
{
	VentureDatabase *db;
	VentureContext *context;
	VentureConfig *config;
	gint64 org, owner, other;
	gint64 shared_account, private_account;
} Fixture;

static void
save(Fixture *f, VentureEntity *entity)
{
	g_autoptr(GError) error = NULL;
	gboolean saved = venture_database_save(f->db, entity, NULL, &error);
	g_assert_no_error(error);
	g_assert_true(saved);
}

static gint64
member(Fixture *f, const gchar *name)
{
	g_autoptr(VentureEntity) user = g_object_new(VENTURE_TYPE_USER, "username", name, "active", TRUE, NULL);
	g_autoptr(VentureEntity) membership = NULL;
	save(f, user);
	membership = g_object_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP, "organization-id", f->org,
		"user-id", venture_entity_get_id(user), "active", TRUE, "role", VENTURE_ORGANIZATION_ROLE_OWNER, NULL);
	save(f, membership);
	return venture_entity_get_id(user);
}

static gint64
account(Fixture *f, const gchar *code, gint64 owner)
{
	g_autoptr(VentureEntity) row = g_object_new(VENTURE_TYPE_ACCOUNT, "organization-id", f->org,
		"code", code, "name", code, "kind", VENTURE_ACCOUNT_KIND_ASSET, "active", TRUE, "private-owner-id", owner, NULL);
	save(f, row);
	return venture_entity_get_id(row);
}

static gint64
purchase(Fixture *f, const gchar *what, gint64 cents, gint year, gint64 cash, VentureDeductibility treatment)
{
	g_autoptr(VentureMoney) amount = venture_money_new(cents, "USD", 2);
	g_autoptr(GDateTime) when = g_date_time_new_utc(year, 3, 14, 12, 0, 0);
	g_autoptr(VentureEntity) row = g_object_new(VENTURE_TYPE_EXPENSE, "organization-id", f->org,
		"description", what, "vendor", "Corner shop", "amount", amount, "occurred-at", when,
		"category", "Medical", "cash-account-id", cash, "deductibility", treatment, NULL);
	save(f, row);
	return venture_entity_get_id(row);
}

static gint64
attach(Fixture *f, gint64 expense, const gchar *title, const gchar *kind, GDateTime *warranty, gint64 remind, gint64 owner)
{
	g_autoptr(VentureEntity) row = g_object_new(VENTURE_TYPE_DOCUMENT, "organization-id", f->org,
		"title", title, "kind", kind, "expense-id", expense, "private-owner-id", owner, NULL);
	if (warranty != NULL)
		g_object_set(row, "warranty-until", warranty, "warranty-reminder-user-id", remind, NULL);
	save(f, row);
	return venture_entity_get_id(row);
}

static void
setup(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	(void)unused;
	f->config = venture_config_new();
	f->db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->db);
	f->org = venture_context_get_default_organization_id(f->context);
	f->owner = member(f, "household-owner");
	f->other = member(f, "household-partner");
	f->shared_account = account(f, "joint-checking", 0);
	f->private_account = account(f, "owner-card", f->owner);
}

static void
teardown(Fixture *f, gconstpointer unused)
{
	(void)unused;
	g_clear_object(&f->context);
	g_clear_object(&f->db);
	g_clear_object(&f->config);
}

/* Runs a report as one member, through the same policy a request meets. */
static VentureReportResult *
run_as(Fixture *f, gint64 user, const gchar *name, const gchar *period_text)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureDateRange) period = venture_date_range_parse(period_text, NULL, 1, &error);
	VentureReport *report = venture_report_registry_lookup(venture_context_get_report_registry(f->context), name);
	VentureAuthPrincipal actor = { 0 };
	g_autoptr(VentureAccessScope) scope = NULL;
	VentureReportResult *result;
	g_assert_no_error(error);
	g_assert_nonnull(report);
	actor.authenticated = TRUE;
	actor.role = VENTURE_USER_ROLE_EDITOR;
	actor.user_id = user;
	scope = venture_access_policy_enter(venture_database_get_access_policy(f->db), &actor);
	result = venture_report_generate(report, f->context, period, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	return result;
}

static gboolean
csv_has(VentureReportResult *result, const gchar *text)
{
	g_autofree gchar *csv = venture_report_result_render(result, VENTURE_OUTPUT_FORMAT_CSV);
	return strstr(csv, text) != NULL;
}

static void
share_private_account(Fixture *f, gboolean shared)
{
	g_autoptr(VentureEntity) row = venture_database_get(f->db, VENTURE_TYPE_ACCOUNT, f->private_account, NULL);
	VentureAuthPrincipal actor = { 0 };
	g_autoptr(VentureAccessScope) scope = NULL;
	actor.authenticated = TRUE;
	actor.role = VENTURE_USER_ROLE_EDITOR;
	actor.user_id = f->owner;
	scope = venture_access_policy_enter(venture_database_get_access_policy(f->db), &actor);
	g_object_set(row, "household-shared", shared, NULL);
	save(f, row);
}

/* The year's deductible and to-check purchases, with their receipts; the
 * owner's unshared card stays out of the partner's copy until shared. */
static void
test_tax_year(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureReportResult) mine = NULL, theirs = NULL, shared = NULL;
	gint64 dentist, glasses;
	(void)unused;
	dentist = purchase(f, "Dentist", 12000, 2026, f->shared_account, VENTURE_DEDUCTIBILITY_FULL);
	attach(f, dentist, "Dentist receipt", "receipt", NULL, 0, 0);
	purchase(f, "Groceries", 5000, 2026, f->shared_account, VENTURE_DEDUCTIBILITY_NONE);
	glasses = purchase(f, "Glasses", 3000, 2026, f->private_account, VENTURE_DEDUCTIBILITY_REVIEW);
	attach(f, glasses, "Glasses receipt", "receipt", NULL, 0, f->owner);
	purchase(f, "Last year's dentist", 9000, 2025, f->shared_account, VENTURE_DEDUCTIBILITY_FULL);

	mine = run_as(f, f->owner, "household_tax_year", "2026");
	g_assert_cmpuint(venture_report_result_get_row_count(mine), ==, 2);
	g_assert_true(csv_has(mine, "Dentist receipt"));
	g_assert_true(csv_has(mine, "Glasses receipt"));
	g_assert_true(csv_has(mine, "Ask your tax preparer"));
	g_assert_false(csv_has(mine, "Groceries"));
	g_assert_false(csv_has(mine, "Last year"));

	theirs = run_as(f, f->other, "household_tax_year", "2026");
	g_assert_cmpuint(venture_report_result_get_row_count(theirs), ==, 1);
	g_assert_true(csv_has(theirs, "Dentist"));
	g_assert_false(csv_has(theirs, "Glasses"));

	share_private_account(f, TRUE);
	shared = run_as(f, f->other, "household_tax_year", "2026");
	g_assert_cmpuint(venture_report_result_get_row_count(shared), ==, 2);
	g_assert_true(csv_has(shared, "Glasses receipt"));
}

/* Reminders come once, inside the lead time, only to someone who may read
 * the warranty; the list follows the same privacy as the export. */
static void
test_warranties(Fixture *f, gconstpointer unused)
{
	g_autoptr(GDateTime) now = g_date_time_new_utc(2026, 6, 1, 9, 0, 0);
	g_autoptr(GDateTime) soon = g_date_time_add_days(now, 20);
	g_autoptr(GDateTime) later = g_date_time_add_days(now, 90);
	g_autoptr(GDateTime) past = g_date_time_add_days(now, -5);
	g_autoptr(GDateTime) next_month = g_date_time_add_days(now, 75);
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureReportResult) mine = NULL, theirs = NULL;
	g_autoptr(VentureEntity) stamped = NULL;
	g_autoptr(GDateTime) stamp = NULL;
	gint64 fridge, laptop, phone, kettle, fridge_doc;
	(void)unused;
	fridge = purchase(f, "Fridge", 90000, 2026, f->shared_account, VENTURE_DEDUCTIBILITY_NONE);
	fridge_doc = attach(f, fridge, "Fridge warranty", "warranty", soon, f->other, 0);
	laptop = purchase(f, "Laptop", 120000, 2026, f->private_account, VENTURE_DEDUCTIBILITY_NONE);
	attach(f, laptop, "Laptop warranty", "warranty", soon, f->owner, f->owner);
	phone = purchase(f, "Phone", 60000, 2026, f->shared_account, VENTURE_DEDUCTIBILITY_NONE);
	attach(f, phone, "Phone warranty", "warranty", later, f->owner, 0);
	kettle = purchase(f, "Kettle", 4000, 2026, f->shared_account, VENTURE_DEDUCTIBILITY_NONE);
	attach(f, kettle, "Kettle warranty", "warranty", past, f->owner, 0);

	/* The fridge to the partner and the laptop to its owner: the phone is
	 * outside 30 days and the kettle has already ended. */
	g_assert_cmpint(venture_household_warranty_remind(f->context, f->org, now, &error), ==, 2);
	g_assert_no_error(error);
	g_assert_cmpint(venture_household_warranty_remind(f->context, f->org, now, &error), ==, 0);
	g_assert_no_error(error);
	stamped = venture_database_get(f->db, VENTURE_TYPE_DOCUMENT, fridge_doc, NULL);
	g_object_get(stamped, "warranty-reminded-at", &stamp, NULL);
	g_assert_nonnull(stamp);
	g_assert_cmpint(venture_household_warranty_remind(f->context, f->org, next_month, &error), ==, 1);
	g_assert_no_error(error);

	mine = run_as(f, f->owner, "household_warranties", "2026");
	g_assert_true(csv_has(mine, "Laptop warranty"));
	g_assert_true(csv_has(mine, "Fridge warranty"));
	g_assert_true(csv_has(mine, "Ended"));
	theirs = run_as(f, f->other, "household_warranties", "2026");
	g_assert_true(csv_has(theirs, "Fridge warranty"));
	g_assert_false(csv_has(theirs, "Laptop"));
}

/* A warranty date needs a purchase or a title, and the reminder person
 * must be able to read the document. */
static void
test_warranty_validation(Fixture *f, gconstpointer unused)
{
	g_autoptr(GDateTime) when = g_date_time_new_utc(2027, 1, 1, 0, 0, 0);
	g_autoptr(VentureEntity) row = NULL;
	g_autoptr(GError) error = NULL;
	gint64 laptop;
	(void)unused;
	laptop = purchase(f, "Laptop", 120000, 2026, f->private_account, VENTURE_DEDUCTIBILITY_NONE);
	row = g_object_new(VENTURE_TYPE_DOCUMENT, "organization-id", f->org, "title", "Laptop warranty",
		"kind", "warranty", "expense-id", laptop, "private-owner-id", f->owner,
		"warranty-until", when, "warranty-reminder-user-id", f->other, NULL);
	g_assert_false(venture_database_save(f->db, row, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/household-yearend/tax-year", Fixture, NULL, setup, test_tax_year, teardown);
	g_test_add("/household-yearend/warranties", Fixture, NULL, setup, test_warranties, teardown);
	g_test_add("/household-yearend/warranty-validation", Fixture, NULL, setup, test_warranty_validation, teardown);
	return g_test_run();
}
