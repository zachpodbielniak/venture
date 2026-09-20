/*
 * Sales scope and delivery use the existing quote, ticket and invoice services.
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
	gint64 deal;
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
accepted_contract(Fixture *f, const gchar *quote_number, const gchar *mode)
{
	g_autoptr(VentureEntity) quote = VENTURE_ENTITY(venture_quote_new());
	g_autoptr(VentureEntity) line = VENTURE_ENTITY(venture_quote_line_new());
	g_autoptr(VentureEntity) action = VENTURE_ENTITY(venture_quote_action_new());
	g_autoptr(GError) error = NULL;
	VentureActor actor;
	actor_init(&actor);
	venture_entity_set_organization_id(quote, f->org);
	g_object_set(quote, "number", quote_number, "company-id", f->company, "currency", "USD",
		"billing-mode", mode, "deal-id", f->deal, NULL);
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
	{
		gint64 id = venture_entity_get_id(quote);
		g_clear_object(&quote);
		quote = venture_database_get(f->db, VENTURE_TYPE_QUOTE, id, NULL);
	}
	action = VENTURE_ENTITY(venture_quote_action_new());
	venture_entity_set_organization_id(action, f->org);
	g_object_set(action, "quote-id", venture_entity_get_id(quote), "action", "accept",
		"expected-version", venture_entity_get_version(quote), "accepted-by", "Alex Buyer", NULL);
	g_assert_true(venture_quote_service_execute(venture_database_get_quote_service(f->db),
		action, "manual", NULL, &actor, &error));
	g_assert_no_error(error);
	return venture_database_get(f->db, VENTURE_TYPE_QUOTE, venture_entity_get_id(quote), NULL);
}

static VentureEntity *
accepted_quote(Fixture *f, const gchar *quote_number)
{
	return accepted_contract(f, quote_number, "progress");
}

static VentureEntity *
perform(Fixture *f, VentureEntity *subject, const gchar *name, const gchar *json, GError **error)
{
	g_autoptr(JsonNode) node = json_from_string(json, error);
	g_autoptr(GHashTable) params = NULL;
	if (node == NULL) return NULL;
	params = venture_action_parameters_from_json(node, error);
	if (params == NULL) return NULL;
	return venture_action_registry_perform(venture_database_get_action_registry(f->db),
		venture_entity_get_entity_name(subject), venture_entity_get_id(subject), name,
		params, NULL, VENTURE_USER_ROLE_OWNER, error);
}

/* Replaying a sales handoff must select the same project and retained scope,
 * while delivery creates ordinary tickets and invoices only accepted work. */
static void
test_quote_delivery(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) quote = accepted_quote(f, "Q-1");
	g_autoptr(VentureEntity) project = NULL;
	g_autoptr(VentureEntity) replay = NULL;
	g_autoptr(GError) error = NULL;
	gint64 source = 0;
	(void)data;
	project = perform(f, quote, "handoff", "{\"name\":\"Website\",\"owner\":\"biller\",\"scope\":\"Launch approved design\"}", &error);
	g_assert_no_error(error);
	g_assert_nonnull(project);
	g_assert_cmpstr(venture_entity_get_entity_name(project), ==, "client_project");
	g_object_get(project, "quote-id", &source, NULL);
	g_assert_cmpint(source, ==, venture_entity_get_id(quote));
	replay = perform(f, quote, "handoff", "{\"name\":\"Website\",\"owner\":\"biller\",\"scope\":\"Launch approved design\"}", &error);
	g_assert_no_error(error);
	g_assert_cmpint(venture_entity_get_id(replay), ==, venture_entity_get_id(project));
	g_clear_object(&replay);
	replay = perform(f, quote, "handoff", "{\"name\":\"Other\",\"owner\":\"biller\",\"scope\":\"Changed contract\"}", &error);
	g_assert_null(replay);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
	g_clear_error(&error);
	g_object_set(project, "owner", "forged", NULL);
	g_assert_false(venture_database_save(f->db, project, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
}

static void
assert_profitability(Fixture *f, gint64 billed, gint64 unbilled)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureReportResult) report = venture_report_generate(
		venture_report_registry_lookup(venture_context_get_report_registry(f->context), "project_margin"),
		f->context, NULL, NULL, &error);
	g_autoptr(JsonNode) node = NULL;
	JsonObject *row;
	g_assert_no_error(error);
	g_assert_nonnull(report);
	node = venture_report_result_to_json(report);
	row = json_array_get_object_element(json_object_get_array_member(json_node_get_object(node), "rows"), 0);
	g_assert_cmpint(json_object_get_int_member(json_object_get_object_member(row, "billed"), "amount"), ==, billed);
	g_assert_cmpint(json_object_get_int_member(json_object_get_object_member(row, "unbilled"), "amount"), ==, unbilled);
}

static void
test_milestone(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) quote = accepted_quote(f, "Q-1");
	g_autoptr(VentureEntity) project = NULL;
	g_autoptr(VentureEntity) deliverable = NULL;
	g_autoptr(VentureEntity) replay = NULL;
	g_autoptr(VentureEntity) ticket = NULL;
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(VentureEntity) scope = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureMoney) balance = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *params = NULL;
	gint64 ticket_id = 0;
	(void)data;
	project = perform(f, quote, "handoff", "{\"name\":\"Website\",\"owner\":\"biller\",\"scope\":\"Launch\"}", &error);
	g_assert_no_error(error);
	query = venture_query_new(VENTURE_TYPE_PROJECT_SCOPE);
	venture_query_set_organization(query, f->org);
	venture_query_add_filter_int(query, "project-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(project), NULL);
	scope = venture_database_find_one(f->db, query, &error);
	g_assert_no_error(error);
	g_assert_nonnull(scope);
	params = g_strdup_printf("{\"key\":\"design\",\"title\":\"Deliver design\",\"scope_id\":%" G_GINT64_FORMAT ",\"amount\":\"400 USD\",\"due\":\"2026-10-01\"}", venture_entity_get_id(scope));
	deliverable = perform(f, project, "plan_work", params, &error);
	g_assert_no_error(error);
	g_assert_nonnull(deliverable);
	g_object_get(deliverable, "ticket-id", &ticket_id, NULL);
	ticket = venture_database_get(f->db, VENTURE_TYPE_TICKET, ticket_id, &error);
	g_assert_no_error(error);
	g_assert_nonnull(ticket);
	replay = perform(f, project, "plan_work", params, &error);
	g_assert_no_error(error);
	g_assert_cmpint(venture_entity_get_id(replay), ==, venture_entity_get_id(deliverable));
	g_clear_object(&replay);
	replay = perform(f, deliverable, "accept", "{\"evidence\":\"Approved design\"}", &error);
	g_assert_null(replay);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	g_object_set(ticket, "status", VENTURE_TICKET_STATUS_DONE, NULL);
	save(f, ticket);
	replay = perform(f, deliverable, "accept", "{\"evidence\":\"Approved design\"}", &error);
	g_assert_no_error(error);
	g_assert_nonnull(replay);
	assert_profitability(f, 0, 40000);
	if (data != NULL)
	{
		g_autoptr(VentureEntity) other_quote = accepted_quote(f, "Q-unrelated");
		g_autoptr(VentureEntity) other_invoice = NULL;
		g_autofree gchar *link_params = NULL;
		other_invoice = venture_progress_service_invoice(venture_progress_service_get(f->db), VENTURE_QUOTE(other_quote), 40, NULL, NULL, &error);
		g_assert_no_error(error);
		link_params = g_strdup_printf("{\"invoice_id\":%" G_GINT64_FORMAT "}", venture_entity_get_id(other_invoice));
		g_clear_object(&replay);
		replay = perform(f, deliverable, "link_invoice", link_params, &error);
		g_assert_null(replay);
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
		g_clear_error(&error);
		g_clear_pointer(&link_params, g_free);
		invoice = venture_progress_service_invoice(venture_progress_service_get(f->db), VENTURE_QUOTE(quote), 40, NULL, NULL, &error);
		g_assert_no_error(error);
		g_assert_nonnull(invoice);
		link_params = g_strdup_printf("{\"invoice_id\":%" G_GINT64_FORMAT "}", venture_entity_get_id(invoice));
		g_clear_object(&replay);
		replay = perform(f, deliverable, "link_invoice", link_params, &error);
		g_assert_no_error(error);
		g_assert_nonnull(replay);
		g_assert_cmpint(venture_entity_get_id(replay), ==, venture_entity_get_id(invoice));
	}
	else invoice = perform(f, deliverable, "bill", "{}", &error);
	g_assert_no_error(error);
	g_assert_nonnull(invoice);
	balance = venture_settlement_service_invoice_balance(venture_settlement_service_get(f->db), venture_entity_get_id(invoice), NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpint(venture_money_get_amount(balance), ==, 40000);
	g_clear_object(&replay);
	replay = perform(f, deliverable, "bill", "{}", &error);
	g_assert_no_error(error);
	g_assert_cmpint(venture_entity_get_id(replay), ==, venture_entity_get_id(invoice));
	assert_profitability(f, 40000, 0);
	/* Delivery creates an ordinary receivable: collecting it must neither
	 * bill the project again nor change the retained billed allocation. */
	{
		g_autoptr(GDateTime) paid = g_date_time_new_now_utc();
		g_assert_true(venture_settlement_service_settle_invoice(venture_settlement_service_get(f->db),
			venture_entity_get_id(invoice), paid, NULL, &error));
		g_assert_no_error(error);
		g_clear_pointer(&balance, venture_money_free);
		balance = venture_settlement_service_invoice_balance(venture_settlement_service_get(f->db),
			venture_entity_get_id(invoice), NULL, &error);
		g_assert_no_error(error);
		g_assert_cmpint(venture_money_get_amount(balance), ==, 0);
		assert_profitability(f, 40000, 0);
	}
	g_clear_object(&replay);
	replay = perform(f, project, "manage", "{\"owner\":\"biller\",\"status\":\"completed\",\"reason\":\"Customer accepted all work\"}", &error);
	g_assert_no_error(error);
	g_assert_nonnull(replay);
}

/* An additive accepted quote is retained once, rather than editing the old
 * agreement or adding its price again when a request is retried. */
static void
test_change(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) original = accepted_quote(f, "Q-original");
	g_autoptr(VentureEntity) extra = accepted_quote(f, "Q-change");
	g_autoptr(VentureEntity) project = NULL;
	g_autoptr(VentureEntity) result = NULL;
	g_autoptr(VentureMoney) budget = NULL;
	g_autofree gchar *params = NULL;
	g_autoptr(GError) error = NULL;
	(void)data;
	project = perform(f, original, "handoff", "{\"name\":\"Website\",\"owner\":\"biller\",\"scope\":\"Launch\"}", &error);
	g_assert_no_error(error);
	params = g_strdup_printf("{\"quote_id\":%" G_GINT64_FORMAT ",\"scope\":\"Add reporting module\"}", venture_entity_get_id(extra));
	result = perform(f, project, "change_scope", params, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	g_clear_object(&result);
	result = perform(f, project, "change_scope", params, &error);
	g_assert_no_error(error);
	g_clear_object(&result);
	result = venture_database_get(f->db, VENTURE_TYPE_CLIENT_PROJECT, venture_entity_get_id(project), &error);
	g_assert_no_error(error);
	g_object_get(result, "budget", &budget, NULL);
	g_assert_cmpint(venture_money_get_amount(budget), ==, 200000);
}

/* The quote's own progress action must contribute the same billed evidence
 * as the project's milestone action. */
static void
test_direct_progress(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) quote = accepted_quote(f, "Q-direct");
	g_autoptr(VentureEntity) project = NULL;
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(GError) error = NULL;
	(void)data;
	invoice = venture_progress_service_invoice(venture_progress_service_get(f->db), VENTURE_QUOTE(quote), 25, NULL, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(invoice);
	project = perform(f, quote, "handoff", "{\"name\":\"Website\",\"owner\":\"biller\",\"scope\":\"Launch\"}", &error);
	g_assert_no_error(error);
	assert_profitability(f, 25000, 0);
	g_clear_object(&invoice);
	invoice = venture_progress_service_invoice(venture_progress_service_get(f->db), VENTURE_QUOTE(quote), 25, NULL, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(invoice);
	assert_profitability(f, 50000, 0);
}

static void
test_full_invoice(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) quote = accepted_contract(f, "Q-full", "full");
	g_autoptr(VentureEntity) project = NULL;
	g_autoptr(VentureEntity) result = NULL;
	g_autoptr(GDateTime) date = venture_time_now();
	g_autoptr(GError) error = NULL;
	(void)data;
	project = perform(f, quote, "handoff", "{\"name\":\"Website\",\"owner\":\"biller\",\"scope\":\"Launch\"}", &error);
	g_assert_no_error(error);
	assert_profitability(f, 100000, 0);
	result = venture_project_service_bill(venture_project_service_get(f->db), venture_entity_get_id(project), date, NULL, &error);
	g_assert_null(result);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
}

static void
test_deal_identity(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureMoney) value = venture_money_new_for_currency(100000, "USD");
	g_autoptr(VentureEntity) deal = g_object_new(VENTURE_TYPE_DEAL, "organization-id", f->org,
		"name", "Won engagement", "company-id", f->company, "stage", VENTURE_DEAL_STAGE_WON, "value", value, NULL);
	g_autoptr(VentureEntity) project = NULL;
	g_autoptr(VentureEntity) quote = NULL;
	g_autoptr(VentureEntity) other = NULL;
	g_autoptr(GError) error = NULL;
	(void)data;
	save(f, deal);
	f->deal = venture_entity_get_id(deal);
	project = perform(f, deal, "handoff", "{\"name\":\"Website\",\"owner\":\"biller\",\"scope\":\"Launch\"}", &error);
	g_assert_no_error(error);
	g_assert_nonnull(project);
	quote = accepted_quote(f, "Q-same-deal");
	other = perform(f, quote, "handoff", "{\"name\":\"Duplicate\",\"owner\":\"biller\",\"scope\":\"Launch\"}", &error);
	g_assert_null(other);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
}

static gboolean
refuse_scope(VentureDatabase *db, VentureEntity *row, VentureEntity *previous, gpointer data, GError **error)
{
	gboolean *fail = data;
	(void)db;
	(void)row;
	(void)previous;
	if (!*fail) return TRUE;
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Injected scope refusal");
	return FALSE;
}

static void
test_atomic_handoff(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) quote = accepted_quote(f, "Q-rollback");
	g_autoptr(VentureEntity) project = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_CLIENT_PROJECT);
	g_autoptr(GError) error = NULL;
	gboolean *fail = g_new(gboolean, 1);
	(void)data;
	*fail = TRUE;
	venture_database_add_save_validator(f->db, VENTURE_TYPE_PROJECT_SCOPE, refuse_scope, fail, g_free);
	project = perform(f, quote, "handoff", "{\"name\":\"Website\",\"owner\":\"biller\",\"scope\":\"Launch\"}", &error);
	g_assert_null(project);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_cmpstr(error->message, ==, "Injected scope refusal");
	g_clear_error(&error);
	g_assert_cmpint(venture_database_count(f->db, query, &error), ==, 0);
	g_assert_no_error(error);
	*fail = FALSE;
	project = perform(f, quote, "handoff", "{\"name\":\"Website\",\"owner\":\"biller\",\"scope\":\"Launch\"}", &error);
	g_assert_no_error(error);
	g_assert_nonnull(project);
	g_assert_cmpint(venture_database_count(f->db, query, &error), ==, 1);
	g_assert_no_error(error);
	/* Clearing submitted sales links cannot turn retained evidence into an
	 * ordinary editable project, nor bypass delete/restore/purge guards. */
	g_object_set(project, "quote-id", (gint64)0, "deal-id", (gint64)0, NULL);
	g_assert_false(venture_database_save(f->db, project, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	g_assert_false(venture_database_delete(f->db, project, NULL, &error));
	g_clear_error(&error);
	g_assert_false(venture_database_restore(f->db, project, NULL, &error));
	g_clear_error(&error);
	g_assert_false(venture_database_purge(f->db, project, NULL, &error));
	g_clear_error(&error);
	{
		g_autoptr(VentureEntity) forged = g_object_new(VENTURE_TYPE_PROJECT_SCOPE,
			"organization-id", f->org, "project-id", venture_entity_get_id(project), "source-key", "forged", NULL);
		g_assert_false(venture_database_save(f->db, forged, NULL, &error));
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	}
}

static void
test_staged_decision(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) quote = accepted_quote(f, "Q-stage");
	g_autoptr(VentureEntity) project = NULL;
	g_autoptr(VentureEntity) changed = NULL;
	g_autoptr(JsonNode) node = json_from_string("{\"owner\":\"biller\",\"status\":\"paused\",\"reason\":\"Await client\"}", NULL);
	g_autoptr(GHashTable) params = venture_action_parameters_from_json(node, NULL);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *id = NULL;
	VentureAction *action = venture_action_registry_lookup(venture_database_get_action_registry(f->db), "client_project", "manage");
	VentureConfirmationStore *store = venture_context_get_confirmations(f->context);
	VentureConfirmation *confirmation;
	(void)data;
	project = perform(f, quote, "handoff", "{\"name\":\"Website\",\"owner\":\"biller\",\"scope\":\"Launch\"}", &error);
	g_assert_no_error(error);
	confirmation = venture_confirmation_store_stage_action(store, action, project, params,
		NULL, VENTURE_USER_ROLE_OWNER, "test", &error);
	g_assert_no_error(error);
	g_assert_nonnull(confirmation);
	id = g_strdup(venture_confirmation_get_id(confirmation));
	changed = perform(f, project, "manage", "{\"owner\":\"biller\",\"status\":\"active\",\"reason\":\"Proceed with delivery\"}", &error);
	g_assert_no_error(error);
	g_assert_nonnull(changed);
	g_assert_false(venture_confirmation_store_approve_as(store, id, "owner", VENTURE_USER_ROLE_OWNER, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
}

static void
test_module_boundary(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) quote = accepted_quote(f, "Q-modules");
	g_autoptr(VentureEntity) project = NULL;
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(GError) error = NULL;
	(void)data;
	project = perform(f, quote, "handoff", "{\"name\":\"Website\",\"owner\":\"biller\",\"scope\":\"Launch\"}", &error);
	g_assert_no_error(error);
	venture_config_set_module_enabled(f->config, "projects", FALSE);
	invoice = venture_progress_service_invoice(venture_progress_service_get(f->db), VENTURE_QUOTE(quote), 25, NULL, NULL, &error);
	g_assert_null(invoice);
	g_assert_nonnull(error);
	g_clear_error(&error);
	venture_config_set_module_enabled(f->config, "projects", TRUE);
	assert_profitability(f, 0, 0);
	invoice = venture_progress_service_invoice(venture_progress_service_get(f->db), VENTURE_QUOTE(quote), 25, NULL, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(invoice);
	assert_profitability(f, 25000, 0);
}

static void
test_permissions(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) quote = accepted_quote(f, "Q-access");
	g_autoptr(VentureEntity) project = NULL;
	g_autoptr(VentureEntity) user = g_object_new(VENTURE_TYPE_USER,
		"username", "delivery-finance", "active", TRUE, "role", VENTURE_USER_ROLE_EDITOR, NULL);
	g_autoptr(VentureEntity) member = NULL;
	g_autoptr(VentureAccessScope) scope = NULL;
	g_autoptr(GError) error = NULL;
	VentureAuthPrincipal principal;
	(void)data;
	save(f, user);
	member = g_object_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP, "user-id", venture_entity_get_id(user),
		"organization-id", f->org, "role", VENTURE_ORGANIZATION_ROLE_FINANCE, "active", TRUE, NULL);
	save(f, member);
	principal.authenticated = TRUE;
	principal.user_id = venture_entity_get_id(user);
	principal.token_id = 0;
	principal.role = VENTURE_USER_ROLE_EDITOR;
	principal.name = NULL;
	scope = venture_access_policy_enter(venture_database_get_access_policy(f->db), &principal);
	project = perform(f, quote, "handoff", "{\"name\":\"Website\",\"owner\":\"delivery-finance\",\"scope\":\"Launch\"}", &error);
	g_assert_no_error(error);
	g_assert_nonnull(project);
	g_clear_object(&scope);
	g_object_set(member, "active", FALSE, NULL);
	save(f, member);
	scope = venture_access_policy_enter(venture_database_get_access_policy(f->db), &principal);
	g_clear_object(&project);
	project = perform(f, quote, "handoff", "{\"name\":\"Website\",\"owner\":\"delivery-finance\",\"scope\":\"Launch\"}", &error);
	g_assert_null(project);
	g_assert_nonnull(error);
}

/* One billing model per project. A deal handoff bills approved time, so a
 * priced deliverable on it could never be invoiced (there is no quote for
 * the progress service) and would inflate =unbilled= forever; a manually
 * labelled "fixed" project has no quote either, so a change order or a
 * time-and-materials invoice on it would charge both contract and labour. */
static void
test_billing_model(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureMoney) value = venture_money_new_for_currency(100000, "USD");
	g_autoptr(VentureEntity) deal = g_object_new(VENTURE_TYPE_DEAL, "organization-id", f->org,
		"name", "Hourly engagement", "company-id", f->company, "stage", VENTURE_DEAL_STAGE_WON, "value", value, NULL);
	g_autoptr(VentureEntity) project = NULL;
	g_autoptr(VentureEntity) scope = NULL;
	g_autoptr(VentureEntity) deliverable = NULL;
	g_autoptr(VentureEntity) ticket = NULL;
	g_autoptr(VentureEntity) result = NULL;
	g_autoptr(VentureEntity) manual = NULL;
	g_autoptr(VentureEntity) quote = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_PROJECT_SCOPE);
	g_autoptr(GDateTime) date = venture_time_now();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *params = NULL;
	g_autofree gchar *sql = NULL;
	gint64 ticket_id = 0;
	(void)data;
	save(f, deal);
	f->deal = venture_entity_get_id(deal);
	project = perform(f, deal, "handoff", "{\"name\":\"Hourly\",\"owner\":\"biller\",\"scope\":\"Ongoing work\"}", &error);
	g_assert_no_error(error);
	g_assert_nonnull(project);
	venture_query_set_organization(query, f->org);
	venture_query_add_filter_int(query, "project-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(project), NULL);
	scope = venture_database_find_one(f->db, query, &error);
	g_assert_no_error(error);
	g_assert_nonnull(scope);
	params = g_strdup_printf("{\"key\":\"design\",\"title\":\"Design\",\"scope_id\":%" G_GINT64_FORMAT ",\"amount\":\"400 USD\",\"due\":\"2026-10-01\"}", venture_entity_get_id(scope));
	deliverable = perform(f, project, "plan_work", params, &error);
	g_assert_null(deliverable);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_nonnull(strstr(error->message, "time-and-materials"));
	g_clear_error(&error);
	g_clear_pointer(&params, g_free);
	/* Unpriced planning stays available: the ticket tracks the work and
	 * approved time bills it. */
	params = g_strdup_printf("{\"key\":\"design\",\"title\":\"Design\",\"scope_id\":%" G_GINT64_FORMAT ",\"due\":\"2026-10-01\"}", venture_entity_get_id(scope));
	deliverable = perform(f, project, "plan_work", params, &error);
	g_assert_no_error(error);
	g_assert_nonnull(deliverable);
	g_object_get(deliverable, "ticket-id", &ticket_id, NULL);
	ticket = venture_database_get(f->db, VENTURE_TYPE_TICKET, ticket_id, &error);
	g_assert_no_error(error);
	g_object_set(ticket, "status", VENTURE_TICKET_STATUS_DONE, NULL);
	save(f, ticket);
	result = perform(f, deliverable, "accept", "{\"evidence\":\"Approved\"}", &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	g_clear_object(&result);
	/* A slice priced before the gate existed is retained evidence, but it is
	 * not receivable revenue on a time-and-materials project. */
	sql = g_strdup_printf("UPDATE %s SET amount_amount = 40000, amount_currency = 'USD', amount_exponent = 2 WHERE id = %" G_GINT64_FORMAT,
		venture_entity_get_table_name(deliverable), venture_entity_get_id(deliverable));
	g_assert_true(venture_database_execute(f->db, sql, NULL, &error));
	g_assert_no_error(error);
	assert_profitability(f, 0, 0);
	/* Time-and-materials billing itself is still open on the deal project;
	 * only the absence of approved work stops it here. */
	result = venture_project_service_bill(venture_project_service_get(f->db), venture_entity_get_id(project), date, NULL, &error);
	g_assert_null(result);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_nonnull(strstr(error->message, "no approved unbilled time or costs"));
	g_clear_error(&error);
	f->deal = 0;
	quote = accepted_quote(f, "Q-manual-change");
	manual = g_object_new(VENTURE_TYPE_CLIENT_PROJECT, "organization-id", f->org, "name", "Hand-made fixed",
		"owner", "biller", "customer-id", f->company, "currency", "USD", "budget", value,
		"billing-kind", "fixed", "delivery-status", VENTURE_PROJECT_DELIVERY_ACTIVE, NULL);
	save(f, manual);
	g_clear_pointer(&params, g_free);
	params = g_strdup_printf("{\"quote_id\":%" G_GINT64_FORMAT ",\"scope\":\"Add reporting\"}", venture_entity_get_id(quote));
	result = perform(f, manual, "change_scope", params, &error);
	g_assert_null(result);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_nonnull(strstr(error->message, "quote handoff"));
	g_clear_error(&error);
	result = venture_project_service_bill(venture_project_service_get(f->db), venture_entity_get_id(manual), date, NULL, &error);
	g_assert_null(result);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_nonnull(strstr(error->message, "fixed-price projects bill accepted delivery"));
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	venture_entity_registry_register_builtins(venture_entity_registry_get_default());
	g_test_add("/project-delivery/quote", Fixture, NULL, setup, test_quote_delivery, teardown);
	g_test_add("/project-delivery/milestone", Fixture, NULL, setup, test_milestone, teardown);
	g_test_add("/project-delivery/link-invoice", Fixture, GINT_TO_POINTER(1), setup, test_milestone, teardown);
	g_test_add("/project-delivery/change", Fixture, NULL, setup, test_change, teardown);
	g_test_add("/project-delivery/direct-progress", Fixture, NULL, setup, test_direct_progress, teardown);
	g_test_add("/project-delivery/full-invoice", Fixture, NULL, setup, test_full_invoice, teardown);
	g_test_add("/project-delivery/atomic-handoff", Fixture, NULL, setup, test_atomic_handoff, teardown);
	g_test_add("/project-delivery/module-boundary", Fixture, NULL, setup, test_module_boundary, teardown);
	g_test_add("/project-delivery/permissions", Fixture, NULL, setup, test_permissions, teardown);
	g_test_add("/project-delivery/deal-identity", Fixture, NULL, setup, test_deal_identity, teardown);
	g_test_add("/project-delivery/staged-decision", Fixture, NULL, setup, test_staged_decision, teardown);
	g_test_add("/project-delivery/billing-model", Fixture, NULL, setup, test_billing_model, teardown);
	return g_test_run();
}
