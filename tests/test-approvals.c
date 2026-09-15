/*
 * test-approvals.c - Durable post/pay SoD: a second actor is required.
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
} Fixture;

static void
setup(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	(void)data;
	f->config = venture_config_new();
	f->db = venture_test_accounting_database(&error);
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
	venture_test_accounting_database_cleanup(f->db);
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
	unrelated = draft_journal(f);
	posted = venture_posting_service_post(venture_database_get_posting_service(f->db),
		journal, NULL, NULL, &alice, &error);
	g_assert_null(posted);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_assert_nonnull(strstr(error->message, "second"));
	g_clear_error(&error);
	posted = venture_posting_service_post(venture_database_get_posting_service(f->db),
		journal, NULL, NULL, &alice, &error);
	g_assert_null(posted);
	g_clear_error(&error);
	/* Identical amounts do not authorize a different saved draft. */
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

/* A batch has no persisted document identity when proposed. The outer request
 * must persist consent without leaking generated rows, and cover every input. */
static gint64
approval_count(Fixture *f, GType type, const gchar *state)
{
	g_autoptr(VentureQuery) query = venture_query_new(type);
	g_autoptr(GError) error = NULL;
	gint64 count;
	venture_query_set_organization(query, f->org);
	if (state != NULL)
		g_assert_true(venture_query_add_filter_string(query, "state", VENTURE_FILTER_OP_EQ, state, &error));
	count = venture_database_count(f->db, query, &error);
	g_assert_no_error(error);
	return count;
}

static void
enable_operation_rule(Fixture *f)
{
	g_autoptr(VentureAccountingApprovalRule) rule = venture_accounting_approval_rule_new();
	g_autoptr(GError) error = NULL;
	g_object_set(rule, "organization-id", f->org, "action", "post", "require-second-actor", TRUE, NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(rule), NULL, &error));
	g_assert_no_error(error);
}

static void
test_generated_batch(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) result = NULL;
	VentureActor alice, bob;
	const gchar *original = "[{\"description\":\"Supplies\",\"amount\":\"10 USD\",\"occurred_at\":\"2026-08-10\",\"external_id\":\"batch-consent\"}]";
	const gchar *changed = "[{\"description\":\"Supplies\",\"amount\":\"20 USD\",\"occurred_at\":\"2026-08-10\",\"external_id\":\"batch-consent\"}]";
	(void)data;
	fill_actor(&alice, "alice"); fill_actor(&bob, "bob");
	enable_operation_rule(f);
	result = venture_recurring_service_batch(venture_recurring_service_get(f->db),
		"expense", "json", original, FALSE, FALSE, &alice, &error);
	g_assert_null(result);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_clear_error(&error);
	g_assert_cmpint(approval_count(f, VENTURE_TYPE_EXPENSE, NULL), ==, 0);
	g_assert_cmpint(approval_count(f, VENTURE_TYPE_ACCOUNTING_APPROVAL, "pending"), ==, 1);
	result = venture_recurring_service_batch(venture_recurring_service_get(f->db),
		"expense", "json", changed, FALSE, FALSE, &bob, &error);
	g_assert_null(result);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_clear_error(&error);
	g_assert_cmpint(approval_count(f, VENTURE_TYPE_EXPENSE, NULL), ==, 0);
	g_assert_cmpint(approval_count(f, VENTURE_TYPE_ACCOUNTING_APPROVAL, "pending"), ==, 2);
	result = venture_recurring_service_batch(venture_recurring_service_get(f->db),
		"expense", "json", original, FALSE, FALSE, &bob, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	g_assert_cmpint(approval_count(f, VENTURE_TYPE_EXPENSE, NULL), ==, 1);
	g_assert_cmpint(approval_count(f, VENTURE_TYPE_ACCOUNTING_APPROVAL, "applied"), ==, 1);
}

/* A changed financial configuration invalidates the old consent even when
 * command arguments are identical; a failed final approval save is atomic. */
static void
test_operation_snapshot(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureAccountingOperation) operation = NULL;
	g_autoptr(VentureEntity) account = NULL;
	VentureActor alice, bob;
	(void)data;
	fill_actor(&alice, "alice"); fill_actor(&bob, "bob");
	enable_operation_rule(f);
	operation = venture_accounting_operation_begin(f->db, "test.operation", NULL, NULL,
		g_variant_new("(s)", "input"), f->org, &alice, &error);
	g_assert_null(operation);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_clear_error(&error);
	account = venture_database_get(f->db, VENTURE_TYPE_ACCOUNT, account_id(f, "4000"), &error);
	g_assert_no_error(error);
	g_object_set(account, "name", "Revised income configuration", NULL);
	g_assert_true(venture_database_save(f->db, account, NULL, &error));
	g_assert_no_error(error);
	operation = venture_accounting_operation_begin(f->db, "test.operation", NULL, NULL,
		g_variant_new("(s)", "input"), f->org, &bob, &error);
	g_assert_null(operation);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_clear_error(&error);
	g_assert_cmpint(approval_count(f, VENTURE_TYPE_ACCOUNTING_APPROVAL, "pending"), ==, 2);
	operation = venture_accounting_operation_begin(f->db, "test.operation", NULL, NULL,
		g_variant_new("(s)", "input"), f->org, &alice, &error);
	g_assert_no_error(error);
	g_assert_nonnull(operation);
	g_object_set(account, "name", "Must roll back", NULL);
	g_assert_true(venture_database_save(f->db, account, &alice, &error));
	g_assert_no_error(error);
	venture_database_add_save_validator(f->db, VENTURE_TYPE_ACCOUNTING_APPROVAL, reject_applied, NULL, NULL);
	g_assert_false(venture_accounting_operation_finish(operation, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	g_clear_pointer(&operation, venture_accounting_operation_free);
	g_clear_object(&account);
	account = venture_database_get(f->db, VENTURE_TYPE_ACCOUNT, account_id(f, "4000"), &error);
	g_assert_no_error(error);
	{
		g_autofree gchar *name = NULL;
		g_object_get(account, "name", &name, NULL);
		g_assert_cmpstr(name, ==, "Revised income configuration");
	}
	g_assert_cmpint(approval_count(f, VENTURE_TYPE_ACCOUNTING_APPROVAL, "applied"), ==, 0);
}

/* Consent is tied to accounts, not token labels, and may only be consumed
 * once even when the command itself leaves the business snapshot unchanged. */
static void
test_operation_identity_replay(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureAccountingOperation) operation = NULL;
	g_autoptr(VentureAccountingOperation) nested = NULL;
	g_autoptr(VentureAccessScope) access = NULL;
	VentureAuthPrincipal principal;
	gchar first_name[] = "token-one";
	gchar second_name[] = "token-two";
	(void)data;
	enable_operation_rule(f);
	principal.authenticated = TRUE; principal.user_id = 101; principal.token_id = 1;
	principal.name = first_name; principal.role = VENTURE_USER_ROLE_OWNER;
	access = venture_access_policy_enter(venture_database_get_access_policy(f->db), &principal);
	operation = venture_accounting_operation_begin(f->db, "test.identity", NULL, NULL, NULL, f->org, NULL, &error);
	g_assert_null(operation);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_clear_error(&error); g_clear_object(&access);
	principal.token_id = 2; principal.name = second_name;
	access = venture_access_policy_enter(venture_database_get_access_policy(f->db), &principal);
	operation = venture_accounting_operation_begin(f->db, "test.identity", NULL, NULL, NULL, f->org, NULL, &error);
	g_assert_null(operation);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_clear_error(&error); g_clear_object(&access);
	principal.user_id = 102;
	access = venture_access_policy_enter(venture_database_get_access_policy(f->db), &principal);
	operation = venture_accounting_operation_begin(f->db, "test.identity", NULL, NULL, NULL, f->org, NULL, &error);
	g_assert_no_error(error); g_assert_nonnull(operation);
	/* A child cannot turn consent for one organization into authority for another. */
	nested = venture_accounting_operation_begin(f->db, "test.child", NULL, NULL, NULL, f->org + 1, NULL, &error);
	g_assert_null(nested);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_clear_error(&error); g_clear_object(&access);
	principal.user_id = 103;
	access = venture_access_policy_enter(venture_database_get_access_policy(f->db), &principal);
	nested = venture_accounting_operation_begin(f->db, "test.child", NULL, NULL, NULL, f->org, NULL, &error);
	g_assert_null(nested);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_clear_error(&error); g_clear_object(&access);
	principal.user_id = 102;
	access = venture_access_policy_enter(venture_database_get_access_policy(f->db), &principal);
	g_assert_true(venture_accounting_operation_finish(operation, &error));
	g_assert_no_error(error);
	g_clear_pointer(&operation, venture_accounting_operation_free);
	operation = venture_accounting_operation_begin(f->db, "test.identity", NULL, NULL, NULL, f->org, NULL, &error);
	g_assert_null(operation);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_clear_error(&error);
	g_assert_cmpint(approval_count(f, VENTURE_TYPE_ACCOUNTING_APPROVAL, "applied"), ==, 1);
	g_assert_cmpint(approval_count(f, VENTURE_TYPE_ACCOUNTING_APPROVAL, "pending"), ==, 1);
}

typedef struct
{
	gint64 org;
	guint attempts;
} CallbackAttempt;

/* Signal callbacks are separate work, even if the writer has been approved. */
static void
try_borrow_consent(VentureDatabase *db, VentureEntity *entity, gboolean created, gpointer data)
{
	CallbackAttempt *attempt = data;
	g_autoptr(VentureAccountingOperation) operation = NULL;
	g_autoptr(GError) error = NULL;
	(void)created;
	if (!VENTURE_IS_ACCOUNT(entity)) return;
	attempt->attempts++;
	operation = venture_accounting_operation_begin(db, "test.callback", NULL, NULL, NULL,
		attempt->org, NULL, &error);
	g_assert_null(operation);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
}

static void
test_operation_callback(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureAccountingOperation) operation = NULL;
	g_autoptr(VentureEntity) account = NULL;
	VentureActor alice, bob;
	CallbackAttempt attempt;
	gulong handler;
	(void)data;
	fill_actor(&alice, "alice"); fill_actor(&bob, "bob");
	enable_operation_rule(f);
	operation = venture_accounting_operation_begin(f->db, "test.callback-owner", NULL, NULL,
		NULL, f->org, &alice, &error);
	g_assert_null(operation); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_clear_error(&error);
	operation = venture_accounting_operation_begin(f->db, "test.callback-owner", NULL, NULL,
		NULL, f->org, &bob, &error);
	g_assert_no_error(error); g_assert_nonnull(operation);
	account = venture_database_get(f->db, VENTURE_TYPE_ACCOUNT, account_id(f, "4000"), &error);
	g_assert_no_error(error);
	attempt.org = f->org; attempt.attempts = 0;
	handler = g_signal_connect(f->db, "entity-saved", G_CALLBACK(try_borrow_consent), &attempt);
	g_object_set(account, "name", "Approved account change", NULL);
	g_assert_true(venture_database_save(f->db, account, &bob, &error));
	g_assert_no_error(error);
	g_signal_handler_disconnect(f->db, handler);
	g_assert_cmpuint(attempt.attempts, ==, 1);
	g_assert_true(venture_accounting_operation_finish(operation, &error));
	g_assert_no_error(error);
}

/* Old or future-dated consent must never apply, even when the full proposal
 * still matches. Age only fixture evidence through SQL; public writes are refused. */
static void
test_operation_expiry(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureAccountingOperation) operation = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ACCOUNTING_APPROVAL);
	g_autoptr(VentureEntity) pending = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(GDateTime) shifted = g_date_time_add_hours(now, GPOINTER_TO_INT(data));
	g_autofree gchar *timestamp = g_date_time_format_iso8601(shifted);
	g_autofree gchar *sql = NULL;
	VentureActor alice, bob;
	fill_actor(&alice, "alice"); fill_actor(&bob, "bob");
	enable_operation_rule(f);
	operation = venture_accounting_operation_begin(f->db, "test.expiry", NULL, NULL, NULL, f->org, &alice, &error);
	g_assert_null(operation);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_clear_error(&error);
	venture_query_set_organization(query, f->org);
	pending = venture_database_find_one(f->db, query, &error);
	g_assert_no_error(error); g_assert_nonnull(pending);
	sql = g_strdup_printf("UPDATE %s SET created_at='%s' WHERE id=%" G_GINT64_FORMAT,
		venture_entity_get_table_name(pending), timestamp, venture_entity_get_id(pending));
	g_assert_true(venture_database_execute(f->db, sql, NULL, &error));
	g_assert_no_error(error);
	operation = venture_accounting_operation_begin(f->db, "test.expiry", NULL, NULL, NULL, f->org, &bob, &error);
	g_assert_null(operation);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_clear_error(&error);
	g_assert_cmpint(approval_count(f, VENTURE_TYPE_ACCOUNTING_APPROVAL, "pending"), ==, 2);
	g_assert_cmpint(approval_count(f, VENTURE_TYPE_ACCOUNTING_APPROVAL, "applied"), ==, 0);
	operation = venture_accounting_operation_begin(f->db, "test.expiry", NULL, NULL, NULL, f->org, &alice, &error);
	g_assert_no_error(error); g_assert_nonnull(operation);
	g_assert_true(venture_accounting_operation_finish(operation, &error));
	g_assert_no_error(error);
	g_assert_cmpint(approval_count(f, VENTURE_TYPE_ACCOUNTING_APPROVAL, "applied"), ==, 1);
}

/* Post/pay consent protects financial effects, not ordinary bookkeeping
 * preparation. Draft and account edits remain usable by one authorized actor. */
static void
test_draft_edits(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) account = NULL;
	g_autoptr(VentureInvoice) invoice = venture_invoice_new();
	g_autoptr(VentureJournal) journal = draft_journal(f);
	g_autoptr(VentureJournal) posted = NULL;
	VentureActor alice, bob;
	(void)data;
	fill_actor(&alice, "alice"); fill_actor(&bob, "bob");
	enable_operation_rule(f);
	g_object_set(invoice, "organization-id", f->org, "number", "DRAFT-NO-CONSENT", NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(invoice), &alice, &error));
	g_assert_no_error(error);
	g_object_set(invoice, "notes", "Prepared by the bookkeeper", NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(invoice), &alice, &error));
	g_assert_no_error(error);
	account = venture_database_get(f->db, VENTURE_TYPE_ACCOUNT, account_id(f, "4000"), &error);
	g_assert_no_error(error);
	g_object_set(account, "name", "Updated income label", NULL);
	g_assert_true(venture_database_save(f->db, account, &alice, &error));
	g_assert_no_error(error);
	g_assert_cmpint(approval_count(f, VENTURE_TYPE_ACCOUNTING_APPROVAL, NULL), ==, 0);
	posted = venture_posting_service_post(venture_database_get_posting_service(f->db), journal, NULL, NULL, &alice, &error);
	g_assert_null(posted);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_clear_error(&error);
	g_assert_cmpint(approval_count(f, VENTURE_TYPE_ACCOUNTING_APPROVAL, "pending"), ==, 1);
	posted = venture_posting_service_post(venture_database_get_posting_service(f->db), journal, NULL, NULL, &bob, &error);
	g_assert_no_error(error); g_assert_nonnull(posted);
}

/* A command in an organization without a rule must not suppress another
 * organization's posting rule merely because it already owns a transaction. */
static void
test_unprotected_parent(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureAccountingOperation) outer = NULL;
	g_autoptr(VentureOrganization) other = venture_organization_new();
	g_autoptr(VentureJournal) journal = draft_journal(f);
	g_autoptr(VentureJournal) posted = NULL;
	VentureActor alice;
	(void)data;
	fill_actor(&alice, "alice");
	enable_operation_rule(f);
	g_object_set(other, "name", "Unprotected organization", "slug", "unprotected", NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(other), NULL, &error));
	g_assert_no_error(error);
	outer = venture_accounting_operation_begin(f->db, "test.unprotected", NULL, NULL,
		NULL, venture_entity_get_id(VENTURE_ENTITY(other)), &alice, &error);
	g_assert_no_error(error); g_assert_nonnull(outer);
	posted = venture_posting_service_post(venture_database_get_posting_service(f->db), journal,
		NULL, NULL, &alice, &error);
	g_assert_null(posted);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_clear_error(&error);
	g_clear_pointer(&outer, venture_accounting_operation_free);
	g_assert_cmpint(approval_count(f, VENTURE_TYPE_ACCOUNTING_APPROVAL, NULL), ==, 0);
	posted = venture_posting_service_post(venture_database_get_posting_service(f->db), journal,
		NULL, NULL, &alice, &error);
	g_assert_null(posted);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_clear_error(&error);
	g_assert_cmpint(approval_count(f, VENTURE_TYPE_ACCOUNTING_APPROVAL, "pending"), ==, 1);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/approvals/post-second-actor", Fixture, NULL, setup, test_post_requires_second_actor, teardown);
	g_test_add("/approvals/pay-second-actor", Fixture, NULL, setup, test_pay_requires_second_actor, teardown);
	g_test_add("/approvals/atomic", Fixture, NULL, setup, test_atomic_approval, teardown);
	g_test_add("/approvals/account-identity", Fixture, NULL, setup, test_account_identity, teardown);
	g_test_add("/approvals/generated-batch", Fixture, NULL, setup, test_generated_batch, teardown);
	g_test_add("/approvals/operation-snapshot", Fixture, NULL, setup, test_operation_snapshot, teardown);
	g_test_add("/approvals/operation-identity-replay", Fixture, NULL, setup, test_operation_identity_replay, teardown);
	g_test_add("/approvals/operation-callback", Fixture, NULL, setup, test_operation_callback, teardown);
	g_test_add("/approvals/operation-expired", Fixture, GINT_TO_POINTER(-25), setup, test_operation_expiry, teardown);
	g_test_add("/approvals/operation-future", Fixture, GINT_TO_POINTER(1), setup, test_operation_expiry, teardown);
	g_test_add("/approvals/draft-edits", Fixture, NULL, setup, test_draft_edits, teardown);
	g_test_add("/approvals/unprotected-parent", Fixture, NULL, setup, test_unprotected_parent, teardown);
	return g_test_run();
}
