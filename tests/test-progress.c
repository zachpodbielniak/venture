/*
 * test-progress.c - Progress invoicing, retainers and retention release.
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <venture.h>
#include <string.h>
#include "venture-test-util.h"
#include "venture-test-accounting.h"

typedef struct
{
	VentureDatabase *db;
	VentureConfig *config;
	VentureContext *context;
	gint64 org;
	gint64 company;
} Fixture;

static void
save(Fixture *f, VentureEntity *record)
{
	g_autoptr(GError) error = NULL;
	g_assert_true(venture_database_save(f->db, record, NULL, &error));
	g_assert_no_error(error);
}

static void
actor_init(VentureActor *actor)
{
	actor->kind = VENTURE_ACTOR_KIND_USER;
	actor->name = "biller";
	actor->prompt = NULL;
	actor->request_id = NULL;
	actor->approved_by = NULL;
}

static gint64
account_id(Fixture *f, const gchar *code)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ACCOUNT);
	g_autoptr(VentureEntity) row = NULL;
	venture_query_add_filter_string(query, "code", VENTURE_FILTER_OP_EQ, code, NULL);
	row = venture_database_find_one(f->db, query, NULL);
	g_assert_nonnull(row);
	return venture_entity_get_id(row);
}

static void
setup(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureCompany) company = NULL;
	(void)data;
	f->config = venture_config_new();
	f->db = venture_test_accounting_database(&error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->db);
	f->org = venture_context_get_default_organization_id(f->context);
	company = venture_company_new();
	g_object_set(company, "name", "Client", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(company), f->org);
	save(f, VENTURE_ENTITY(company));
	f->company = venture_entity_get_id(VENTURE_ENTITY(company));
}

static void
teardown(Fixture *f, gconstpointer data)
{
	(void)data;
	g_clear_object(&f->context);
	venture_test_accounting_database_cleanup(f->db);
	g_clear_object(&f->db);
	g_clear_object(&f->config);
}

static VentureEntity *
accepted_quote(Fixture *f)
{
	g_autoptr(VentureEntity) quote = VENTURE_ENTITY(venture_quote_new());
	g_autoptr(VentureEntity) line = VENTURE_ENTITY(venture_quote_line_new());
	g_autoptr(VentureEntity) action = VENTURE_ENTITY(venture_quote_action_new());
	g_autoptr(GError) error = NULL;
	VentureActor actor;
	actor_init(&actor);
	venture_entity_set_organization_id(quote, f->org);
	g_object_set(quote, "number", "Q-1", "company-id", f->company, "currency", "USD",
		"billing-mode", "progress", NULL);
	save(f, quote);
	venture_entity_set_organization_id(line, f->org);
	g_object_set(line, "quote-id", venture_entity_get_id(quote), "description", "Contract",
		"quantity", (gint64)1, NULL);
	g_assert_true(venture_entity_set_field_from_string(line, "unit-price", "1000 USD", NULL));
	save(f, line);
	{
		gint64 qid = venture_entity_get_id(quote);
		g_object_unref(quote);
		quote = venture_database_get(f->db, VENTURE_TYPE_QUOTE, qid, NULL);
	}
	venture_entity_set_organization_id(action, f->org);
	g_object_set(action, "quote-id", venture_entity_get_id(quote), "action", "send",
		"expected-version", venture_entity_get_version(quote), NULL);
	if (!venture_quote_service_execute(venture_database_get_quote_service(f->db),
		action, "manual", NULL, &actor, &error))
		g_error("send: %s", error ? error->message : "nil");
	g_assert_no_error(error);
	g_clear_object(&action);
	quote = venture_database_get(f->db, VENTURE_TYPE_QUOTE, venture_entity_get_id(quote), NULL);
	action = VENTURE_ENTITY(venture_quote_action_new());
	venture_entity_set_organization_id(action, f->org);
	g_object_set(action, "quote-id", venture_entity_get_id(quote), "action", "accept",
		"expected-version", venture_entity_get_version(quote), "accepted-by", "Alex Buyer", NULL);
	g_assert_true(venture_quote_service_execute(venture_database_get_quote_service(f->db),
		action, "manual", NULL, &actor, &error));
	g_assert_no_error(error);
	return venture_database_get(f->db, VENTURE_TYPE_QUOTE, venture_entity_get_id(quote), NULL);
}

static void
test_progress_invoice_remaining(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) quote = accepted_quote(f);
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureMoney) remaining = NULL;
	g_autoptr(VentureEntity) first = NULL;
	g_autoptr(VentureEntity) over = NULL;
	g_autoptr(VentureMoney) total = NULL;
	VentureActor actor;
	gint64 invoice_id = 0;
	(void)data;
	actor_init(&actor);
	g_object_get(quote, "invoice-id", &invoice_id, "total", &total, NULL);
	g_assert_cmpint(invoice_id, ==, 0);
	g_assert_cmpint(venture_money_get_amount(total), ==, 100000);
	remaining = venture_progress_service_remaining(venture_progress_service_get(f->db),
		VENTURE_QUOTE(quote), &error);
	g_assert_cmpint(venture_money_get_amount(remaining), ==, 100000);
	first = venture_progress_service_invoice(venture_progress_service_get(f->db),
		VENTURE_QUOTE(quote), 40, NULL, &actor, &error);
	g_assert_no_error(error);
	g_assert_nonnull(first);
	g_clear_pointer(&remaining, venture_money_free);
	remaining = venture_progress_service_remaining(venture_progress_service_get(f->db),
		VENTURE_QUOTE(quote), &error);
	g_assert_cmpint(venture_money_get_amount(remaining), ==, 60000);
	over = venture_progress_service_invoice(venture_progress_service_get(f->db),
		VENTURE_QUOTE(quote), 70, NULL, &actor, &error);
	g_assert_null(over);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	/* Two 40% instalments need distinct invoice numbers and leave 20% unbilled. */
	over = venture_progress_service_invoice(venture_progress_service_get(f->db),
		VENTURE_QUOTE(quote), 40, NULL, &actor, &error);
	g_assert_no_error(error);
	g_assert_nonnull(over);
	g_clear_pointer(&remaining, venture_money_free);
	remaining = venture_progress_service_remaining(venture_progress_service_get(f->db), VENTURE_QUOTE(quote), &error);
	g_assert_no_error(error);
	g_assert_cmpint(venture_money_get_amount(remaining), ==, 20000);
}

static gint64
liability(Fixture *f)
{
	return account_id(f, "2200");
}

static void
test_retainer_then_release(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureMoney) amount = venture_money_new_for_currency(25000, "USD");
	g_autoptr(VentureEntity) retainer = NULL;
	g_autoptr(VentureMoney) remaining = NULL;
	VentureActor actor;
	gint64 liability_id = liability(f);
	(void)data;
	actor_init(&actor);
	retainer = venture_progress_service_collect_retainer(venture_progress_service_get(f->db),
		f->org, f->company, liability_id, amount, &actor, &error);
	g_assert_no_error(error);
	g_object_get(retainer, "remaining", &remaining, NULL);
	g_assert_cmpint(venture_money_get_amount(remaining), ==, 25000);
	g_assert_true(venture_progress_service_release_retainer(venture_progress_service_get(f->db),
		VENTURE_CUSTOMER_RETAINER(retainer), amount, &actor, &error));
	g_assert_no_error(error);
	g_clear_pointer(&remaining, venture_money_free);
	g_object_get(retainer, "remaining", &remaining, NULL);
	g_assert_cmpint(venture_money_get_amount(remaining), ==, 0);
}

static void
test_retention_hold_and_release(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) quote = accepted_quote(f);
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureMoney) amount = venture_money_new_for_currency(10000, "USD");
	g_autoptr(VentureEntity) retention = NULL;
	VentureActor actor;
	gint64 liability_id = liability(f);
	(void)data;
	actor_init(&actor);
	retention = venture_progress_service_hold_retention(venture_progress_service_get(f->db),
		VENTURE_QUOTE(quote), liability_id, amount, &actor, &error);
	g_assert_no_error(error);
	g_assert_true(venture_progress_service_release_retention(venture_progress_service_get(f->db),
		VENTURE_CONTRACT_RETENTION(retention), amount, &actor, &error));
	g_assert_no_error(error);
	{
		g_autoptr(GDateTime) as_of = g_date_time_new_now_utc();
		g_autoptr(VentureMoney) ar = venture_posting_service_account_balance(
			venture_database_get_posting_service(f->db), account_id(f, "1100"),
			f->org, "USD", as_of, &error);
		g_assert_no_error(error);
		g_assert_cmpint(venture_money_get_amount(ar), ==, 0);
	}
}

static void
test_generic_write_refused(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureProgressBilling) row = venture_progress_billing_new();
	g_autoptr(GError) error = NULL;
	(void)data;
	venture_entity_set_organization_id(VENTURE_ENTITY(row), f->org);
	g_object_set(row, "kind", "progress", NULL);
	g_assert_false(venture_database_save(f->db, VENTURE_ENTITY(row), NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
}

static VentureEntity *
retainer_action(Fixture *f, const gchar *type, gint64 id, const gchar *name,
	const gchar *json, GError **error)
{
	g_autoptr(JsonNode) node = venture_json_parse(json, error);
	g_autoptr(GHashTable) parameters = node ? venture_action_parameters_from_json(node, error) : NULL;
	VentureActor actor;
	actor_init(&actor);
	return parameters ? venture_action_registry_perform(venture_database_get_action_registry(f->db),
		type, id, name, parameters, &actor, VENTURE_USER_ROLE_EDITOR, error) : NULL;
}

/* Retainers cannot be created by generic CRUD. These actions must expose
 * actual collection/release and post both partial releases exactly once. */
static void
test_retainer_actions(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) retainer = NULL, result = NULL;
	g_autoptr(VentureMoney) remaining = NULL, cash = NULL, balance = NULL;
	g_autoptr(GDateTime) now = NULL;
	g_autofree gchar *parameters = g_strdup_printf("{\"amount\":\"250 USD\",\"liability_account_id\":%" G_GINT64_FORMAT "}", liability(f));
	VentureAction *action = venture_action_registry_lookup(venture_database_get_action_registry(f->db), "company", "collect_retainer");
	gint64 id;
	(void)unused;
	g_assert_nonnull(action);
	{
		g_autofree gchar *bad = g_strdup_printf("{\"amount\":\"250 USD\",\"liability_account_id\":%" G_GINT64_FORMAT "}", account_id(f, "4000"));
		result = retainer_action(f, "company", f->company, "collect_retainer", bad, &error);
		g_assert_null(result); g_assert_nonnull(error); g_clear_error(&error);
	}
	retainer = retainer_action(f, "company", f->company, "collect_retainer", parameters, &error);
	g_assert_no_error(error); g_assert_nonnull(retainer);
	id = venture_entity_get_id(retainer);
	result = retainer_action(f, "customer_retainer", id, "release", "{\"amount\":\"300 USD\"}", &error);
	g_assert_null(result); g_assert_nonnull(error); g_clear_error(&error);
	result = retainer_action(f, "customer_retainer", id, "release", "{\"amount\":\"100 EUR\"}", &error);
	g_assert_null(result); g_assert_nonnull(error); g_clear_error(&error);
	result = retainer_action(f, "customer_retainer", id, "release", "{\"amount\":\"100 USD\"}", &error);
	g_assert_no_error(error); g_assert_nonnull(result); g_clear_object(&result);
	result = retainer_action(f, "customer_retainer", id, "release", "{\"amount\":\"150 USD\"}", &error);
	g_assert_no_error(error); g_assert_nonnull(result);
	g_object_get(result, "remaining", &remaining, NULL);
	g_assert_cmpint(venture_money_get_amount(remaining), ==, 0); g_clear_object(&result);
	result = retainer_action(f, "customer_retainer", id, "release", "{\"amount\":\"1 USD\"}", &error);
	g_assert_null(result); g_assert_nonnull(error); g_clear_error(&error);
	now = venture_time_now();
	cash = venture_posting_service_account_balance(venture_database_get_posting_service(f->db), account_id(f, "1000"), f->org, "USD", now, &error);
	g_assert_no_error(error); g_assert_cmpint(venture_money_get_amount(cash), ==, 25000);
	balance = venture_posting_service_account_balance(venture_database_get_posting_service(f->db), liability(f), f->org, "USD", now, &error);
	g_assert_no_error(error); g_assert_cmpint(venture_money_get_amount(balance), ==, 0);
}

static void
test_retainer_authority(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) user = g_object_new(VENTURE_TYPE_USER,
		"username", "biller", "role", VENTURE_USER_ROLE_EDITOR, "active", TRUE, NULL);
	g_autoptr(VentureEntity) member = NULL, result = NULL;
	g_autoptr(VentureAccessScope) scope = NULL;
	g_autofree gchar *parameters = g_strdup_printf("{\"amount\":\"1 USD\",\"liability_account_id\":%" G_GINT64_FORMAT "}", liability(f));
	VentureAuthPrincipal principal;
	gboolean stageable = TRUE;
	VentureAction *action = venture_action_registry_lookup(venture_database_get_action_registry(f->db), "company", "collect_retainer");
	(void)unused;
	g_object_get(action, "stageable", &stageable, NULL); g_assert_false(stageable);
	save(f, user);
	member = g_object_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP,
		"organization-id", f->org, "user-id", venture_entity_get_id(user),
		"role", VENTURE_ORGANIZATION_ROLE_EDITOR, "active", TRUE, NULL);
	save(f, member);
	principal.user_id = venture_entity_get_id(user); principal.token_id = 0;
	principal.name = (gchar *)"biller"; principal.role = VENTURE_USER_ROLE_EDITOR; principal.authenticated = TRUE;
	scope = venture_access_policy_enter(venture_database_get_access_policy(f->db), &principal);
	result = retainer_action(f, "company", f->company, "collect_retainer", parameters, &error);
	g_assert_null(result); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED); g_clear_error(&error);
	g_clear_object(&scope);
	g_object_set(member, "role", VENTURE_ORGANIZATION_ROLE_FINANCE, NULL); save(f, member);
	scope = venture_access_policy_enter(venture_database_get_access_policy(f->db), &principal);
	result = retainer_action(f, "company", f->company, "collect_retainer", parameters, &error);
	g_assert_no_error(error); g_assert_nonnull(result); g_clear_object(&result);
	g_clear_object(&scope);
	g_object_set(member, "active", FALSE, NULL); save(f, member);
	scope = venture_access_policy_enter(venture_database_get_access_policy(f->db), &principal);
	result = retainer_action(f, "company", f->company, "collect_retainer", parameters, &error);
	g_assert_null(result); g_assert_nonnull(error); g_clear_error(&error);
	g_clear_object(&scope);
	venture_config_set_module_enabled(f->config, "quotes", FALSE);
	result = retainer_action(f, "company", f->company, "collect_retainer", parameters, &error);
	g_assert_null(result); g_assert_nonnull(error); g_clear_error(&error);
	venture_config_set_module_enabled(f->config, "quotes", TRUE);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/progress/invoice-remaining", Fixture, NULL, setup, test_progress_invoice_remaining, teardown);
	g_test_add("/progress/retainer-authority", Fixture, NULL, setup, test_retainer_authority, teardown);
	g_test_add("/progress/retainer-actions", Fixture, NULL, setup, test_retainer_actions, teardown);
	g_test_add("/progress/retainer", Fixture, NULL, setup, test_retainer_then_release, teardown);
	g_test_add("/progress/retention", Fixture, NULL, setup, test_retention_hold_and_release, teardown);
	g_test_add("/progress/generic-write", Fixture, NULL, setup, test_generic_write_refused, teardown);
	return g_test_run();
}
