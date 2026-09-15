/*
 * test-approvals.c - Durable post/pay SoD: a second actor is required.
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <venture.h>
#include <string.h>
#include "venture-test-util.h"

typedef struct
{
	VentureDatabase *db;
	VentureConfig *config;
	VentureContext *context;
	gint64 org;
} Fixture;

static void
setup(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	(void)data;
	f->config = venture_config_new();
	f->db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->db);
	f->org = venture_context_get_default_organization_id(f->context);
}

static void
teardown(Fixture *f, gconstpointer data)
{
	(void)data;
	g_clear_object(&f->context);
	g_clear_object(&f->db);
	g_clear_object(&f->config);
}

static void
fill_actor(VentureActor *actor, const gchar *name)
{
	actor->kind = VENTURE_ACTOR_KIND_USER;
	actor->name = name;
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
	return venture_entity_get_id(row);
}

static VentureJournal *
draft_journal(Fixture *f)
{
	g_autoptr(VentureJournal) journal = venture_journal_new();
	g_autoptr(VentureJournalLine) debit = venture_journal_line_new();
	g_autoptr(VentureJournalLine) credit = venture_journal_line_new();
	g_autoptr(GDateTime) when = g_date_time_new_from_iso8601("2026-08-10T00:00:00Z", NULL);
	g_autoptr(GPtrArray) lines = g_ptr_array_new_with_free_func(g_object_unref);
	g_autoptr(GError) error = NULL;
	g_object_set(journal, "source-type", "organization", "source-id", f->org,
		"occurred-at", when, "currency", "USD", "organization-id", f->org,
		"state", VENTURE_JOURNAL_DRAFT, NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(journal), NULL, &error));
	g_object_set(debit, "journal-id", venture_entity_get_id(VENTURE_ENTITY(journal)),
		"account-id", account_id(f, "1000"), "side", VENTURE_LEDGER_SIDE_DEBIT,
		"organization-id", f->org, NULL);
	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(debit), "amount", "10 USD", NULL));
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(debit), NULL, &error));
	g_object_set(credit, "journal-id", venture_entity_get_id(VENTURE_ENTITY(journal)),
		"account-id", account_id(f, "4000"), "side", VENTURE_LEDGER_SIDE_CREDIT,
		"organization-id", f->org, NULL);
	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(credit), "amount", "10 USD", NULL));
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(credit), NULL, &error));
	(void)lines;
	return g_steal_pointer(&journal);
}

static void
test_post_requires_second_actor(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureAccountingApprovalRule) rule = venture_accounting_approval_rule_new();
	g_autoptr(VentureJournal) journal = NULL;
	g_autoptr(VentureJournal) unrelated = NULL;
	g_autoptr(VentureJournal) posted = NULL;
	VentureActor alice, bob;
	(void)data;
	fill_actor(&alice, "alice");
	fill_actor(&bob, "bob");
	venture_entity_set_organization_id(VENTURE_ENTITY(rule), f->org);
	g_object_set(rule, "action", "post", "require-second-actor", TRUE, NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(rule), NULL, &error));
	journal = draft_journal(f);
	posted = venture_posting_service_post(venture_database_get_posting_service(f->db),
		journal, NULL, NULL, &alice, &error);
	g_assert_null(posted);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_assert_nonnull(strstr(error->message, "second actor"));
	g_clear_error(&error);
	posted = venture_posting_service_post(venture_database_get_posting_service(f->db),
		journal, NULL, NULL, &alice, &error);
	g_assert_null(posted);
	g_clear_error(&error);
	/* Identical amounts do not authorize a different saved draft. */
	unrelated = draft_journal(f);
	posted = venture_posting_service_post(venture_database_get_posting_service(f->db),
		unrelated, NULL, NULL, &bob, &error);
	g_assert_null(posted);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_clear_error(&error);
	posted = venture_posting_service_post(venture_database_get_posting_service(f->db),
		journal, NULL, NULL, &bob, &error);
	g_assert_no_error(error);
	g_assert_nonnull(posted);
}

static void
test_pay_requires_second_actor(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureAccountingApprovalRule) rule = venture_accounting_approval_rule_new();
	g_autoptr(VentureCompany) customer = venture_company_new();
	g_autoptr(VentureInvoice) invoice = venture_invoice_new();
	g_autoptr(VentureInvoiceLine) line = venture_invoice_line_new();
	g_autoptr(GDateTime) now = venture_time_now();
	VentureActor alice, bob;
	(void)data;
	fill_actor(&alice, "alice");
	fill_actor(&bob, "bob");
	venture_entity_set_organization_id(VENTURE_ENTITY(rule), f->org);
	g_object_set(rule, "action", "pay", "require-second-actor", TRUE, NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(rule), NULL, &error));
	g_object_set(customer, "name", "Payer", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(customer), f->org);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(customer), NULL, &error));
	venture_entity_set_organization_id(VENTURE_ENTITY(invoice), f->org);
	g_object_set(invoice, "number", "INV-SOD", "company-id",
		venture_entity_get_id(VENTURE_ENTITY(customer)), NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(invoice), NULL, &error));
	venture_entity_set_organization_id(VENTURE_ENTITY(line), f->org);
	g_object_set(line, "invoice-id", venture_entity_get_id(VENTURE_ENTITY(invoice)),
		"description", "Work", "quantity", 1.0, NULL);
	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(line), "unit-price", "10 USD", NULL));
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(line), NULL, &error));
	g_assert_true(venture_settlement_service_transition(venture_settlement_service_get(f->db),
		invoice, "sent", now, NULL, &error));
	g_assert_false(venture_settlement_service_settle_invoice(venture_settlement_service_get(f->db),
		venture_entity_get_id(VENTURE_ENTITY(invoice)), now, &alice, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_clear_error(&error);
	g_assert_true(venture_settlement_service_settle_invoice(venture_settlement_service_get(f->db),
		venture_entity_get_id(VENTURE_ENTITY(invoice)), now, &bob, &error));
	g_assert_no_error(error);
}

static gboolean
reject_applied(VentureDatabase *db, VentureEntity *record, VentureEntity *previous,
	gpointer data, GError **error)
{
	g_autofree gchar *state = NULL;
	(void)db;
	(void)previous;
	(void)data;
	g_object_get(record, "state", &state, NULL);
	if (g_strcmp0(state, "applied") != 0)
		return TRUE;
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "injected approval failure");
	return FALSE;
}

/* Approval evidence and its financial effect must commit together. */
static void
test_atomic_approval(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureAccountingApprovalRule) rule = venture_accounting_approval_rule_new();
	g_autoptr(VentureJournal) journal = NULL;
	g_autoptr(VentureJournal) posted = NULL;
	g_autoptr(VentureEntity) stored = NULL;
	VentureActor alice, bob;
	gint state;
	(void)data;
	fill_actor(&alice, "alice");
	fill_actor(&bob, "bob");
	g_object_set(rule, "organization-id", f->org, "action", "post", "require-second-actor", TRUE, NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(rule), NULL, &error));
	journal = draft_journal(f);
	posted = venture_posting_service_post(venture_database_get_posting_service(f->db),
		journal, NULL, NULL, &alice, &error);
	g_assert_null(posted);
	g_clear_error(&error);
	venture_database_add_save_validator(f->db, VENTURE_TYPE_ACCOUNTING_APPROVAL, reject_applied, NULL, NULL);
	posted = venture_posting_service_post(venture_database_get_posting_service(f->db),
		journal, NULL, NULL, &bob, &error);
	g_assert_null(posted);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	stored = venture_database_get(f->db, VENTURE_TYPE_JOURNAL,
		venture_entity_get_id(VENTURE_ENTITY(journal)), &error);
	g_assert_no_error(error);
	g_object_get(stored, "state", &state, NULL);
	g_assert_cmpint(state, ==, VENTURE_JOURNAL_DRAFT);
}

/* Switching token labels cannot turn one authenticated account into two
 * people. Omitting the audit actor also must not discard request authority. */
static void
test_account_identity(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureAccountingApprovalRule) rule = venture_accounting_approval_rule_new();
	g_autoptr(VentureJournal) journal = NULL;
	g_autoptr(VentureEntity) approval = NULL;
	g_autoptr(VentureAccessScope) scope = NULL;
	VentureAuthPrincipal principal;
	VentureActor actor;
	gchar first_name[] = "first-token";
	gchar second_name[] = "second-token";
	(void)data;
	g_object_set(rule, "organization-id", f->org, "action", "post", "require-second-actor", TRUE, NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(rule), NULL, &error));
	journal = draft_journal(f);
	principal.authenticated = TRUE;
	principal.user_id = 101;
	principal.token_id = 1;
	principal.name = first_name;
	principal.role = VENTURE_USER_ROLE_OWNER;
	venture_auth_to_actor(&principal, &actor);
	scope = venture_access_policy_enter(venture_database_get_access_policy(f->db), &principal);
	g_assert_false(venture_accounting_approval_allow(f->db, "post", VENTURE_ENTITY(journal), NULL, &actor, &approval, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_clear_error(&error);
	g_clear_object(&scope);
	principal.token_id = 2;
	principal.name = second_name;
	venture_auth_to_actor(&principal, &actor);
	scope = venture_access_policy_enter(venture_database_get_access_policy(f->db), &principal);
	g_assert_false(venture_accounting_approval_allow(f->db, "post", VENTURE_ENTITY(journal), NULL, &actor, &approval, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_clear_error(&error);
	g_assert_false(venture_accounting_approval_allow(f->db, "post", VENTURE_ENTITY(journal), NULL, NULL, &approval, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_clear_error(&error);
	g_clear_object(&scope);
	principal.user_id = 102;
	scope = venture_access_policy_enter(venture_database_get_access_policy(f->db), &principal);
	g_assert_true(venture_accounting_approval_allow(f->db, "post", VENTURE_ENTITY(journal), NULL, &actor, &approval, &error));
	g_assert_no_error(error);
	g_assert_nonnull(approval);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/approvals/post-second-actor", Fixture, NULL, setup, test_post_requires_second_actor, teardown);
	g_test_add("/approvals/pay-second-actor", Fixture, NULL, setup, test_pay_requires_second_actor, teardown);
	g_test_add("/approvals/atomic", Fixture, NULL, setup, test_atomic_approval, teardown);
	g_test_add("/approvals/account-identity", Fixture, NULL, setup, test_account_identity, teardown);
	return g_test_run();
}
