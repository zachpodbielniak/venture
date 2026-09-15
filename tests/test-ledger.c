/*
 * test-ledger.c - Journals are the only authority for account balances
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <venture.h>

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
	f->db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db,
		venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->config = venture_config_new();
	f->context = venture_context_new(f->config, f->db);
	f->org = venture_context_get_default_organization_id(f->context);
}

static void
teardown(Fixture *f, gconstpointer data)
{
	(void)data;
	g_clear_object(&f->context);
	g_clear_object(&f->config);
	g_clear_object(&f->db);
}

static void
test_module(Fixture *f, gconstpointer data)
{
	VentureModule *module;
	const gchar *const *requires;

	(void)data;
	module = venture_module_registry_lookup(
		venture_context_get_modules(f->context), "ledger");
	g_assert_nonnull(module);
	requires = venture_module_get_requires(module);
	g_assert_true(g_strv_contains(requires, "finance"));
}

static void
test_records(Fixture *f, gconstpointer data)
{
	VentureEntityRegistry *registry;

	(void)data;
	registry = venture_context_get_entity_registry(f->context);
	g_assert_cmpuint(venture_entity_registry_lookup(registry, "journal"), !=, 0);
	g_assert_cmpuint(venture_entity_registry_lookup(registry, "journal_line"), !=, 0);
	g_assert_cmpuint(venture_entity_registry_lookup(registry, "exchange_rate"), !=, 0);
}

static void
test_generic_ledger_write(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureLedgerEntry) entry = NULL;
	g_autoptr(VentureMoney) amount = NULL;
	g_autoptr(GError) error = NULL;

	(void)data;
	entry = venture_ledger_entry_new();
	amount = venture_money_new_for_currency(100, "USD");
	g_object_set(entry, "transaction-id", "bypass", "account-id", (gint64)1,
		"amount", amount, NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(entry), f->org);
	/* The generic save is shared by REST, CLI and approved AI writes. */
	g_assert_false(venture_database_save(f->db, VENTURE_ENTITY(entry), NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
}

static GDateTime *
date(const gchar *value)
{
	return g_date_time_new_from_iso8601(value, NULL);
}

static gint64
account(Fixture *f, const gchar *code)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ACCOUNT);
	g_autoptr(VentureEntity) row = NULL;

	venture_query_add_filter_string(query, "code", VENTURE_FILTER_OP_EQ, code, NULL);
	row = venture_database_find_one(f->db, query, NULL);
	g_assert_nonnull(row);
	return venture_entity_get_id(row);
}

static VentureJournal *
header(Fixture *f)
{
	VentureJournal *journal = venture_journal_new();
	g_autoptr(GDateTime) when = date("2026-01-10T00:00:00Z");

	g_object_set(journal, "source-type", "organization", "source-id", f->org,
		"occurred-at", when, "currency", "USD", "organization-id", f->org, NULL);
	return journal;
}

static VentureJournalLine *
line(Fixture *f, const gchar *code, VentureLedgerSide side, gint64 units, const gchar *currency)
{
	VentureJournalLine *row = venture_journal_line_new();
	g_autoptr(VentureMoney) amount = venture_money_new_for_currency(units, currency);

	g_object_set(row, "account-id", account(f, code), "side", side, "amount", amount,
		"organization-id", f->org, NULL);
	return row;
}

static GPtrArray *
lines(Fixture *f, gint64 credit, const gchar *currency)
{
	GPtrArray *rows = g_ptr_array_new_with_free_func(g_object_unref);

	g_ptr_array_add(rows, line(f, "1000", VENTURE_LEDGER_SIDE_DEBIT, 10000, "USD"));
	g_ptr_array_add(rows, line(f, "4000", VENTURE_LEDGER_SIDE_CREDIT, credit, currency));
	return rows;
}

static gint64
count(Fixture *f, GType type)
{
	g_autoptr(VentureQuery) query = venture_query_new(type);

	venture_query_set_include_deleted(query, TRUE);
	return venture_database_count(f->db, query, NULL);
}

static void
test_post_and_lookup(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureJournal) draft = header(f);
	g_autoptr(VentureJournal) posted = NULL;
	g_autoptr(GPtrArray) rows = lines(f, 10000, "USD");
	g_autoptr(GPtrArray) found = NULL;
	g_autoptr(GError) error = NULL;
	VenturePostingService *service = venture_database_get_posting_service(f->db);
	VentureJournalState state;

	(void)data;
	posted = venture_posting_service_post(service, draft, rows, NULL, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(posted);
	g_object_get(posted, "state", &state, NULL);
	g_assert_cmpint(state, ==, VENTURE_JOURNAL_POSTED);
	g_assert_cmpint(count(f, VENTURE_TYPE_JOURNAL_LINE), ==, 2);
	g_assert_cmpint(count(f, VENTURE_TYPE_LEDGER_ENTRY), ==, 2);
	found = venture_posting_service_find_source(service, "organization", f->org, f->org, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(found->len, ==, 1);
	g_assert_cmpint(venture_entity_get_id(VENTURE_ENTITY(draft)), ==, 0);
	g_assert_null(venture_posting_service_post(service, posted, NULL, NULL, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
}

static void
test_refusals(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureJournal) draft = header(f);
	g_autoptr(GPtrArray) rows = lines(f, 10000, "USD");
	g_autoptr(GError) error = NULL;
	VenturePostingService *service = venture_database_get_posting_service(f->db);
	const gchar *which = data;
	g_autoptr(VentureMoney) amount = NULL;

	if (g_str_equal(which, "unbalanced"))
	{
		amount = venture_money_new_for_currency(9999, "USD");
		g_object_set(g_ptr_array_index(rows, 1), "amount", amount, NULL);
	}
	else if (g_str_equal(which, "currency"))
	{
		amount = venture_money_new_for_currency(10000, "EUR");
		g_object_set(g_ptr_array_index(rows, 1), "amount", amount, NULL);
	}
	else if (g_str_equal(which, "organization"))
		g_object_set(draft, "organization-id", (gint64)0, NULL);
	else if (g_str_equal(which, "line-organization"))
		g_object_set(g_ptr_array_index(rows, 1), "organization-id", f->org + 100, NULL);
	else if (g_str_equal(which, "account"))
		g_object_set(g_ptr_array_index(rows, 1), "account-id", (gint64)999999, NULL);
	else if (g_str_equal(which, "source"))
		g_object_set(draft, "source-id", (gint64)999999, NULL);
	else if (g_str_equal(which, "negative"))
	{
		amount = venture_money_new_for_currency(-10000, "USD");
		g_object_set(g_ptr_array_index(rows, 0), "amount", amount, NULL);
	}
	else if (g_str_equal(which, "empty"))
		g_ptr_array_set_size(rows, 0);
	else if (g_str_equal(which, "date"))
		g_object_set(draft, "occurred-at", NULL, NULL);
	else if (g_str_equal(which, "overflow"))
	{
		amount = venture_money_new_for_currency(G_MAXINT64, "USD");
		g_object_set(g_ptr_array_index(rows, 0), "amount", amount, NULL);
		g_ptr_array_add(rows, line(f, "1000", VENTURE_LEDGER_SIDE_DEBIT, 1, "USD"));
	}
	g_assert_null(venture_posting_service_post(service, draft, rows, NULL, NULL, &error));
	g_assert_nonnull(error);
	if (g_str_equal(which, "unbalanced"))
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_BALANCE);
	g_assert_cmpint(count(f, VENTURE_TYPE_JOURNAL), ==, 0);
	g_assert_cmpint(count(f, VENTURE_TYPE_JOURNAL_LINE), ==, 0);
	g_assert_cmpint(count(f, VENTURE_TYPE_LEDGER_ENTRY), ==, 0);
}

static void
test_immutable(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureJournal) draft = header(f);
	g_autoptr(VentureJournal) posted = NULL;
	g_autoptr(GPtrArray) rows = lines(f, 10000, "USD");
	g_autoptr(VentureEntity) target = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *which = data;
	GType type = g_str_has_prefix(which, "line") ? VENTURE_TYPE_JOURNAL_LINE :
		(g_str_has_prefix(which, "projection") ? VENTURE_TYPE_LEDGER_ENTRY : VENTURE_TYPE_JOURNAL);

	posted = venture_posting_service_post(venture_database_get_posting_service(f->db),
		draft, rows, NULL, NULL, &error);
	g_assert_no_error(error);
	query = venture_query_new(type);
	target = venture_database_find_one(f->db, query, &error);
	g_assert_no_error(error);
	g_assert_nonnull(target);
	/* Moving a line to another journal must check its OLD parent too. */
	if (g_str_has_suffix(which, "move"))
	{
		g_clear_object(&draft);
		draft = header(f);
		g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(draft), NULL, &error));
		g_object_set(target, "journal-id", venture_entity_get_id(VENTURE_ENTITY(draft)), NULL);
	}
	else
		g_object_set(target, "memo", "tampered", NULL);
	if (g_str_has_suffix(which, "delete"))
		g_assert_false(venture_database_delete(f->db, target, NULL, &error));
	else if (g_str_has_suffix(which, "purge"))
		g_assert_false(venture_database_purge(f->db, target, NULL, &error));
	else
		g_assert_false(venture_database_save(f->db, target, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
}

static void
test_draft_transition(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureJournal) draft = header(f);
	g_autoptr(VentureJournal) posted = NULL;
	g_autoptr(GPtrArray) rows = lines(f, 10000, "USD");
	g_autoptr(GError) error = NULL;
	guint i;

	(void)data;
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(draft), NULL, &error));
	for (i = 0; i < rows->len; i++)
	{
		g_object_set(g_ptr_array_index(rows, i), "journal-id",
			venture_entity_get_id(VENTURE_ENTITY(draft)), NULL);
		g_assert_true(venture_database_save(f->db, g_ptr_array_index(rows, i), NULL, &error));
	}
	g_object_set(draft, "state", VENTURE_JOURNAL_POSTED, NULL);
	g_assert_false(venture_database_save(f->db, VENTURE_ENTITY(draft), NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_clear_error(&error);
	g_object_set(draft, "state", VENTURE_JOURNAL_DRAFT, NULL);
	posted = venture_posting_service_post(venture_database_get_posting_service(f->db),
		draft, NULL, NULL, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(posted);
}

static GError *
veto(VenturePostingService *service, VentureJournal *journal, GPtrArray *rows, gpointer data)
{
	(void)service;
	(void)journal;
	(void)rows;
	(void)data;
	return g_error_new_literal(VENTURE_ERROR, VENTURE_ERROR_CONFLICT, "approval veto");
}

static GError *
closed_date(VenturePostingService *service, gint64 org, GDateTime *when, gpointer data)
{
	(void)service;
	(void)org;
	(void)when;
	(void)data;
	return g_error_new_literal(VENTURE_ERROR, VENTURE_ERROR_CONFLICT, "date is closed");
}

static void
observed(VenturePostingService *service, VentureJournal *journal, gpointer data)
{
	guint *n = data;
	VentureJournalState state;

	(void)service;
	g_object_get(journal, "state", &state, NULL);
	g_assert_cmpint(state, ==, VENTURE_JOURNAL_POSTED);
	(*n)++;
}

static gboolean
fail_projection(VentureDatabase *db, VentureEntity *entity, VentureEntity *previous,
	gpointer data, GError **error)
{
	(void)db;
	(void)entity;
	(void)previous;
	(void)data;
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_DATABASE, "injected last write failure");
	return FALSE;
}

static void
test_atomic_and_signals(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureJournal) draft = header(f);
	g_autoptr(VentureJournal) posted = NULL;
	g_autoptr(GPtrArray) rows = lines(f, 10000, "USD");
	g_autoptr(GError) error = NULL;
	VenturePostingService *service = venture_database_get_posting_service(f->db);
	const gchar *which = data;
	guint n = 0;
	GSignalQuery signal;

	g_signal_query(g_signal_lookup("posting", VENTURE_TYPE_POSTING_SERVICE), &signal);
	g_assert_true(0 != (signal.signal_flags & G_SIGNAL_RUN_LAST));
	g_signal_connect(service, "posted", G_CALLBACK(observed), &n);
	if (g_str_equal(which, "veto"))
		g_signal_connect(service, "posting", G_CALLBACK(veto), NULL);
	else if (g_str_equal(which, "closed"))
		g_signal_connect(service, "date-postable", G_CALLBACK(closed_date), NULL);
	else if (g_str_equal(which, "storage"))
		venture_database_add_save_validator(f->db, VENTURE_TYPE_LEDGER_ENTRY,
			fail_projection, NULL, NULL);
	g_assert_true(venture_database_begin(f->db, &error));
	posted = venture_posting_service_post(service, draft, rows, NULL, NULL, &error);
	g_assert_cmpuint(n, ==, 0);
	if (g_str_equal(which, "commit"))
	{
		g_assert_no_error(error);
		g_assert_nonnull(posted);
		g_assert_true(venture_database_commit(f->db, &error));
		g_assert_cmpuint(n, ==, 1);
		return;
	}
	if (!g_str_equal(which, "rollback"))
	{
		g_assert_null(posted);
		g_assert_nonnull(error);
	}
	venture_database_rollback(f->db);
	g_assert_cmpuint(n, ==, 0);
	g_assert_cmpint(count(f, VENTURE_TYPE_JOURNAL), ==, 0);
	g_assert_cmpint(count(f, VENTURE_TYPE_JOURNAL_LINE), ==, 0);
	g_assert_cmpint(count(f, VENTURE_TYPE_LEDGER_ENTRY), ==, 0);
	g_assert_cmpint(venture_entity_get_id(VENTURE_ENTITY(draft)), ==, 0);
}

static void
test_reverse_and_balance(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureJournal) draft = header(f);
	g_autoptr(VentureJournal) posted = NULL;
	g_autoptr(VentureJournal) reversal = NULL;
	g_autoptr(GPtrArray) rows = lines(f, 10000, "USD");
	g_autoptr(GPtrArray) found = NULL;
	g_autoptr(GDateTime) before = date("2026-01-09T00:00:00Z");
	g_autoptr(GDateTime) during = date("2026-01-15T00:00:00Z");
	g_autoptr(GDateTime) after = date("2026-02-01T00:00:00Z");
	g_autoptr(VentureMoney) balance = NULL;
	g_autoptr(GError) error = NULL;
	VenturePostingService *service = venture_database_get_posting_service(f->db);
	gint64 original_id;
	gint64 reverses_id;
	g_autoptr(VentureEntity) original = NULL;
	VentureJournalState state;

	(void)data;
	posted = venture_posting_service_post(service, draft, rows, NULL, NULL, &error);
	g_assert_no_error(error);
	original_id = venture_entity_get_id(VENTURE_ENTITY(posted));
	reversal = venture_posting_service_reverse(service, original_id, after, "correction", NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(reversal);
	g_object_get(reversal, "reverses-id", &reverses_id, NULL);
	g_assert_cmpint(reverses_id, ==, original_id);
	original = venture_database_get(f->db, VENTURE_TYPE_JOURNAL, original_id, &error);
	g_object_get(original, "state", &state, NULL);
	g_assert_cmpint(state, ==, VENTURE_JOURNAL_REVERSED);
	balance = venture_posting_service_account_balance(service, account(f, "1000"), f->org, "USD", before, &error);
	g_assert_no_error(error);
	g_assert_cmpint(balance->amount, ==, 0);
	g_clear_pointer(&balance, venture_money_free);
	balance = venture_posting_service_account_balance(service, account(f, "1000"), f->org, "USD", during, &error);
	g_assert_no_error(error);
	g_assert_cmpint(balance->amount, ==, 10000);
	g_clear_pointer(&balance, venture_money_free);
	balance = venture_posting_service_account_balance(service, account(f, "1000"), f->org, "USD", after, &error);
	g_assert_no_error(error);
	g_assert_cmpint(balance->amount, ==, 0);
	g_clear_pointer(&balance, venture_money_free);
	balance = venture_posting_service_account_balance(service, account(f, "1000"), f->org, "EUR", during, &error);
	g_assert_no_error(error);
	g_assert_cmpint(balance->amount, ==, 0);
	found = venture_posting_service_find_source(service, "organization", f->org, f->org, &error);
	g_assert_cmpuint(found->len, ==, 2);
	g_assert_null(venture_posting_service_reverse(service, original_id, after, "twice", NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
}

/* A plugin owns these implementations; neither is known to the service. */
typedef struct _TestRule TestRule;
typedef struct _TestRuleClass TestRuleClass;
struct _TestRule { GObject parent; Fixture *fixture; };
struct _TestRuleClass { GObjectClass parent; };
GType test_rule_get_type(void);
static void rule_iface(VenturePostingRuleInterface *iface);
G_DEFINE_TYPE_WITH_CODE(TestRule, test_rule, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(VENTURE_TYPE_POSTING_RULE, rule_iface))
static void test_rule_init(TestRule *self) { (void)self; }
static void test_rule_class_init(TestRuleClass *klass) { (void)klass; }
static const gchar *rule_name(VenturePostingRule *self) { (void)self; return "organization"; }
static GPtrArray *
rule_build(VenturePostingRule *self, VentureDatabase *db, VentureEntity *source, GError **error)
{
	TestRule *rule = (TestRule *)self;
	GPtrArray *rows = lines(rule->fixture, 7000, "USD");
	g_autoptr(VentureMoney) amount = venture_money_new_for_currency(7000, "USD");

	(void)db;
	(void)source;
	(void)error;
	g_object_set(g_ptr_array_index(rows, 0), "amount", amount, NULL);
	return rows;
}
static void
rule_iface(VenturePostingRuleInterface *iface)
{
	iface->get_name = rule_name;
	iface->build_lines = rule_build;
}

typedef struct _TestExchange TestExchange;
typedef struct _TestExchangeClass TestExchangeClass;
struct _TestExchange { GObject parent; gboolean wrong; };
struct _TestExchangeClass { GObjectClass parent; };
GType test_exchange_get_type(void);
static void exchange_iface(VentureExchangePolicyInterface *iface);
G_DEFINE_TYPE_WITH_CODE(TestExchange, test_exchange, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(VENTURE_TYPE_EXCHANGE_POLICY, exchange_iface))
static void test_exchange_init(TestExchange *self) { (void)self; }
static void test_exchange_class_init(TestExchangeClass *klass) { (void)klass; }
static const gchar *exchange_name(VentureExchangePolicy *self) { (void)self; return "test-rate-set-1"; }
static VentureMoney *
exchange_convert(VentureExchangePolicy *self, const VentureMoney *amount, const gchar *currency,
	GDateTime *when, GError **error)
{
	g_autoptr(VentureMoney) value = venture_money_multiply_rational(amount, 2, 1, error);

	(void)when;
	if (NULL == value)
		return NULL;
	return venture_money_new(value->amount, ((TestExchange *)self)->wrong ? "EUR" : currency, value->exponent);
}
static void
exchange_iface(VentureExchangePolicyInterface *iface)
{
	iface->get_name = exchange_name;
	iface->convert = exchange_convert;
}

static void
test_plugin_rule(Fixture *f, gconstpointer data)
{
	VenturePostingService *service = venture_database_get_posting_service(f->db);
	g_autoptr(VentureEntity) source = venture_database_get(f->db, VENTURE_TYPE_ORGANIZATION, f->org, NULL);
	g_autoptr(VentureJournal) posted = NULL;
	g_autoptr(GError) error = NULL;
	TestRule *rule = g_object_new(test_rule_get_type(), NULL);
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(VentureMoney) balance = NULL;

	(void)data;
	rule->fixture = f;
	venture_posting_rule_registry_add(venture_posting_service_get_rules(service), VENTURE_POSTING_RULE(rule));
	posted = venture_posting_service_post_document(service, "organization", source, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(posted);
	balance = venture_posting_service_account_balance(service, account(f, "1000"), f->org, "USD", now, &error);
	g_assert_no_error(error);
	g_assert_cmpint(balance->amount, ==, 7000);
	g_assert_null(venture_posting_service_post_document(service, "organization", source, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_ALREADY_EXISTS);
}

static void
test_exchange(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureJournal) draft = header(f);
	g_autoptr(VentureJournal) posted = NULL;
	g_autoptr(GPtrArray) rows = lines(f, 5000, "EUR");
	g_autoptr(GError) error = NULL;
	g_autoptr(GObject) policy = g_object_new(test_exchange_get_type(), NULL);
	g_autofree gchar *policy_name = NULL;

	((TestExchange *)policy)->wrong = NULL != data;
	posted = venture_posting_service_post(venture_database_get_posting_service(f->db),
		draft, rows, VENTURE_EXCHANGE_POLICY(policy), NULL, &error);
	if (NULL != data)
	{
		g_assert_null(posted);
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
		return;
	}
	g_assert_no_error(error);
	g_assert_nonnull(posted);
	g_object_get(posted, "exchange-policy", &policy_name, NULL);
	g_assert_cmpstr(policy_name, ==, "test-rate-set-1");
}

/* Second-actor consent cannot certify opaque conversion state or a rate
 * table from a different legal entity than the snapshotted journal. */
static void
test_approved_exchange_boundary(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureJournal) draft = header(f), posted = NULL;
	g_autoptr(GPtrArray) entries = lines(f, 10000, "USD");
	g_autoptr(VentureAccountingApprovalRule) rule = venture_accounting_approval_rule_new();
	g_autoptr(VentureExchangePolicy) policy = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_JOURNAL);
	g_autoptr(GError) error = NULL;
	VentureActor actor;
	if (g_strcmp0(data, "opaque") == 0)
		policy = VENTURE_EXCHANGE_POLICY(g_object_new(test_exchange_get_type(), NULL));
	else
		policy = venture_rate_table_policy_new(f->db, f->org + 1);
	g_object_set(rule, "organization-id", f->org, "action", "post", "require-second-actor", TRUE, NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(rule), NULL, &error));
	g_assert_no_error(error);
	actor.kind = VENTURE_ACTOR_KIND_USER;
	actor.name = "alice";
	actor.prompt = NULL;
	actor.request_id = NULL;
	actor.approved_by = NULL;
	posted = venture_posting_service_post(venture_database_get_posting_service(f->db),
		draft, entries, policy, &actor, &error);
	g_assert_null(posted);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_clear_error(&error);
	actor.name = "bob";
	posted = venture_posting_service_post(venture_database_get_posting_service(f->db),
		draft, entries, policy, &actor, &error);
	g_assert_null(posted);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_assert_nonnull(strstr(error->message, "exchange-rate policy"));
	g_clear_error(&error);
	g_assert_cmpint(venture_database_count(f->db, query, &error), ==, 0);
	g_assert_no_error(error);
}


/* Replacing a callback under the same name must invalidate previously
 * proposed work even when no persisted row or command argument changes. */
static void
test_approval_rule_revision(Fixture *f, gconstpointer data)
{
	VenturePostingService *service = venture_database_get_posting_service(f->db);
	VenturePostingRuleRegistry *registry = venture_posting_service_get_rules(service);
	g_autoptr(VentureJournal) draft = header(f), posted = NULL;
	g_autoptr(GPtrArray) entries = lines(f, 10000, "USD");
	g_autoptr(VentureEntity) rule = VENTURE_ENTITY(venture_accounting_approval_rule_new());
	g_autoptr(GError) error = NULL;
	g_autofree gchar *revision = NULL;
	TestRule *implementation;
	VentureActor actor;
	(void)data;
	g_object_set(rule, "organization-id", f->org, "action", "post", "require-second-actor", TRUE, NULL);
	g_assert_true(venture_database_save(f->db, rule, NULL, &error));
	g_assert_no_error(error);
	implementation = g_object_new(test_rule_get_type(), NULL);
	implementation->fixture = f;
	venture_posting_rule_registry_add(registry, VENTURE_POSTING_RULE(implementation));
	revision = g_strdup(venture_posting_rule_registry_get_revision(registry));
	actor.kind = VENTURE_ACTOR_KIND_USER;
	actor.name = "alice";
	actor.prompt = NULL;
	actor.request_id = NULL;
	actor.approved_by = NULL;
	posted = venture_posting_service_post(service, draft, entries, NULL, &actor, &error);
	g_assert_null(posted);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_clear_error(&error);
	implementation = g_object_new(test_rule_get_type(), NULL);
	implementation->fixture = f;
	venture_posting_rule_registry_add(registry, VENTURE_POSTING_RULE(implementation));
	g_assert_cmpstr(venture_posting_rule_registry_get_revision(registry), !=, revision);
	actor.name = "bob";
	posted = venture_posting_service_post(service, draft, entries, NULL, &actor, &error);
	g_assert_null(posted);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_clear_error(&error);
	/* Bob's attempt is a new proposal under the replacement rule set. */
	actor.name = "alice";
	posted = venture_posting_service_post(service, draft, entries, NULL, &actor, &error);
	g_assert_no_error(error);
	g_assert_nonnull(posted);
}


static void
test_rate_table(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureExchangeRate) rate = venture_exchange_rate_new();
	g_autoptr(GObject) policy = NULL;
	g_autoptr(VentureMoney) euros = venture_money_new_for_currency(10000, "EUR");
	g_autoptr(VentureMoney) valued = NULL;
	g_autoptr(GDateTime) when = date("2026-01-10T00:00:00Z");
	g_autoptr(GError) error = NULL;

	(void)data;
	policy = G_OBJECT(venture_rate_table_policy_new(f->db, f->org));
	valued = venture_exchange_policy_convert(VENTURE_EXCHANGE_POLICY(policy),
		euros, "USD", when, &error);
	g_assert_null(valued);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	g_object_set(rate, "from-currency", "EUR", "to-currency", "USD",
		"rate-numerator", (gint64)110, "rate-denominator", (gint64)100,
		"source", "manual", "reason", "board", "effective-at", when, NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(rate), f->org);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(rate), NULL, &error));
	g_assert_no_error(error);
	valued = venture_exchange_policy_convert(VENTURE_EXCHANGE_POLICY(policy),
		euros, "USD", when, &error);
	g_assert_no_error(error);
	g_assert_cmpint(valued->amount, ==, 11000);
	g_assert_cmpstr(valued->currency, ==, "USD");
	{
		g_autoptr(VentureExchangeRate) yen = venture_exchange_rate_new();
		g_autoptr(VentureMoney) jpy = venture_money_new_for_currency(100, "JPY");
		g_autoptr(VentureMoney) usd = NULL;
		g_object_set(yen, "from-currency", "JPY", "to-currency", "USD",
			"rate-numerator", (gint64)1, "rate-denominator", (gint64)150,
			"source", "manual", "reason", "board", "effective-at", when, NULL);
		venture_entity_set_organization_id(VENTURE_ENTITY(yen), f->org);
		g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(yen), NULL, &error));
		usd = venture_exchange_policy_convert(VENTURE_EXCHANGE_POLICY(policy),
			jpy, "USD", when, &error);
		g_assert_no_error(error);
		g_assert_cmpint(usd->amount, ==, 67);
		g_assert_cmpstr(usd->currency, ==, "USD");
		g_assert_cmpint(usd->exponent, ==, 2);
	}
}

static void
test_source_saves(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureExpense) expense = venture_expense_new();
	g_autoptr(VentureSale) sale = venture_sale_new();
	g_autoptr(VentureVenture) venture = venture_venture_new();
	g_autoptr(VentureMoney) amount = venture_money_new_for_currency(5000, "USD");
	g_autoptr(GDateTime) when = date("2026-01-10T00:00:00Z");
	g_autoptr(GDateTime) as_of = date("2026-02-01T00:00:00Z");
	g_autoptr(GPtrArray) found = NULL;
	g_autoptr(VentureMoney) balance = NULL;
	g_autoptr(GError) error = NULL;
	VenturePostingService *service = venture_database_get_posting_service(f->db);

	(void)data;
	g_object_set(expense, "description", "Materials", "amount", amount,
		"organization-id", f->org, "occurred-at", when, NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(expense), NULL, &error));
	g_assert_no_error(error);
	found = venture_posting_service_find_source(service, "expense",
		venture_entity_get_id(VENTURE_ENTITY(expense)), f->org, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(found->len, ==, 1);
	g_clear_pointer(&found, g_ptr_array_unref);
	g_object_set(expense, "description", "Materials, clarified", NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(expense), NULL, &error));
	g_assert_cmpint(count(f, VENTURE_TYPE_JOURNAL), ==, 1);
	g_clear_pointer(&amount, venture_money_free);
	amount = venture_money_new_for_currency(6000, "USD");
	g_object_set(expense, "amount", amount, NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(expense), NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpint(count(f, VENTURE_TYPE_JOURNAL), ==, 3);
	g_object_set(venture, "name", "Books", "venture-type", "books", "organization-id", f->org, NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(venture), NULL, &error));
	g_object_set(sale, "gross", amount, "venture-id", venture_entity_get_id(VENTURE_ENTITY(venture)),
		"organization-id", f->org, "occurred-at", when, NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(sale), NULL, &error));
	g_assert_no_error(error);
	found = venture_posting_service_find_source(service, "sale", venture_entity_get_id(VENTURE_ENTITY(sale)), f->org, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(found->len, ==, 1);
	balance = venture_posting_service_account_balance(service, account(f, "1000"), f->org, "USD", as_of, &error);
	g_assert_no_error(error);
	g_assert_cmpint(balance->amount, ==, 0);
	/* Deleting a source never removes posted history. */
	g_assert_true(venture_database_delete(f->db, VENTURE_ENTITY(sale), NULL, &error));
	g_assert_cmpint(count(f, VENTURE_TYPE_JOURNAL), ==, 4);
}

/* Draft editing is not posting. Adding an amount crosses the consent
 * boundary, and removing a posted amount must never erase the source. */
static void
test_unvalued_source_approval(Fixture *f, gconstpointer data)
{
	gboolean sale = g_strcmp0(data, "sale") == 0;
	g_autoptr(VentureEntity) source = g_object_new(sale ? VENTURE_TYPE_SALE : VENTURE_TYPE_EXPENSE, NULL);
	g_autoptr(VentureEntity) venture = VENTURE_ENTITY(venture_venture_new());
	g_autoptr(VentureEntity) rule = VENTURE_ENTITY(venture_accounting_approval_rule_new());
	g_autoptr(VentureMoney) amount = venture_money_new_for_currency(1000, "USD");
	g_autoptr(GDateTime) when = date("2026-01-10T00:00:00Z");
	g_autoptr(GError) error = NULL;
	VentureActor actor;
	g_object_set(venture, "name", "Draft business", "venture-type", "books", "organization-id", f->org, NULL);
	g_assert_true(venture_database_save(f->db, venture, NULL, &error));
	g_assert_no_error(error);
	g_object_set(source, "organization-id", f->org, "venture-id", venture_entity_get_id(venture),
		"occurred-at", when, sale ? "channel" : "description", "Draft", NULL);
	g_object_set(rule, "organization-id", f->org, "action", "post", "require-second-actor", TRUE, NULL);
	g_assert_true(venture_database_save(f->db, rule, NULL, &error));
	g_assert_no_error(error);
	actor.kind = VENTURE_ACTOR_KIND_USER;
	actor.name = "alice";
	actor.prompt = NULL;
	actor.request_id = NULL;
	actor.approved_by = NULL;
	g_assert_true(venture_database_save(f->db, source, &actor, &error));
	g_assert_no_error(error);
	g_object_set(source, sale ? "channel" : "description", "Edited draft", NULL);
	g_assert_true(venture_database_save(f->db, source, &actor, &error));
	g_assert_no_error(error);
	g_assert_cmpint(count(f, VENTURE_TYPE_JOURNAL), ==, 0);
	g_assert_cmpint(count(f, VENTURE_TYPE_ACCOUNTING_APPROVAL), ==, 0);
	g_object_set(source, sale ? "gross" : "amount", amount, NULL);
	g_assert_false(venture_database_save(f->db, source, &actor, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_clear_error(&error);
	actor.name = "bob";
	g_assert_true(venture_database_save(f->db, source, &actor, &error));
	g_assert_no_error(error);
	g_assert_cmpint(count(f, VENTURE_TYPE_JOURNAL), ==, 1);
	g_object_set(source, sale ? "gross" : "amount", NULL, NULL);
	g_assert_false(venture_database_save(f->db, source, &actor, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_cmpint(count(f, VENTURE_TYPE_JOURNAL), ==, 1);
}


static void
test_source_veto(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureExpense) expense = venture_expense_new();
	g_autoptr(VentureMoney) amount = venture_money_new_for_currency(1000, "USD");
	g_autoptr(GError) error = NULL;

	(void)data;
	g_signal_connect(venture_database_get_posting_service(f->db), "posting", G_CALLBACK(veto), NULL);
	g_object_set(expense, "description", "Refused", "amount", amount, "organization-id", f->org, NULL);
	g_assert_false(venture_database_save(f->db, VENTURE_ENTITY(expense), NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
	g_assert_cmpint(count(f, VENTURE_TYPE_EXPENSE), ==, 0);
	g_assert_cmpint(count(f, VENTURE_TYPE_JOURNAL), ==, 0);
	g_assert_cmpint(venture_entity_get_id(VENTURE_ENTITY(expense)), ==, 0);
}

static void
test_invoice_rule(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureInvoice) invoice = venture_invoice_new();
	g_autoptr(VentureInvoiceLine) row = venture_invoice_line_new();
	g_autoptr(VentureMoney) price = venture_money_new_for_currency(1234, "USD");
	g_autoptr(VentureJournal) posted = NULL;
	g_autoptr(GError) error = NULL;

	(void)data;
	g_object_set(invoice, "number", "SETTLE-1", "organization-id", f->org,
		"status", VENTURE_INVOICE_STATUS_DRAFT, NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(invoice), NULL, &error));
	g_object_set(row, "invoice-id", venture_entity_get_id(VENTURE_ENTITY(invoice)),
		"description", "Work", "unit-price", price, "quantity", 1.0, "organization-id", f->org, NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(row), NULL, &error));
	posted = venture_posting_service_post_document(venture_database_get_posting_service(f->db),
		"invoice-settlement", VENTURE_ENTITY(invoice), NULL, &error);
	/* Settlement is owned by receivables; a document rule would double post. */
	g_assert_null(posted);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
}

static void
test_trial_and_module_off(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureJournal) draft = header(f);
	g_autoptr(VentureJournal) posted = NULL;
	g_autoptr(GPtrArray) rows = lines(f, 10000, "USD");
	g_autoptr(GDateTime) start = date("2026-02-01T00:00:00Z");
	g_autoptr(GDateTime) end = date("2026-03-01T00:00:00Z");
	g_autoptr(VentureDateRange) period = venture_date_range_new(start, end);
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(JsonNode) json = NULL;
	g_autoptr(GError) error = NULL;
	VentureReportRegistry *registry = venture_context_get_report_registry(f->context);
	VentureReport *report = venture_report_registry_lookup(registry, "trial_balance");

	(void)data;
	g_assert_nonnull(report);
	posted = venture_posting_service_post(venture_database_get_posting_service(f->db), draft, rows, NULL, NULL, &error);
	g_assert_no_error(error);
	/* A balance report includes history before the window's start. */
	result = venture_report_generate(report, f->context, period, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	g_assert_cmpuint(venture_report_result_get_row_count(result), ==, 2);
	{
		GPtrArray *metrics = venture_report_result_get_metrics(result);
		VentureMetric *difference;

		g_assert_cmpuint(metrics->len, ==, 3);
		difference = g_ptr_array_index(metrics, 2);
		g_assert_cmpint(difference->money->amount, ==, 0);
	}
	json = venture_report_result_to_json(result);
	{
		g_autofree gchar *text = venture_json_to_string(json, FALSE);

		g_assert_nonnull(strstr(text, "difference"));
		g_assert_nonnull(strstr(text, "10000"));
	}
	venture_config_set_module_enabled(f->config, "ledger", FALSE);
	g_assert_null(venture_context_get_posting_service(f->context));
	g_assert_null(venture_report_registry_lookup(registry, "trial_balance"));
	g_assert_cmpuint(venture_entity_registry_lookup(venture_entity_registry_get_default(), "journal_line"), ==, 0);
	venture_config_set_module_enabled(f->config, "ledger", TRUE);
}

static void
inject_line(VentureDatabase *db, VentureEntity *entity, gboolean created, gpointer data)
{
	Fixture *f = data;
	g_autoptr(VentureJournalLine) row = NULL;
	g_autoptr(GError) error = NULL;

	if (!VENTURE_IS_JOURNAL(entity) || !created)
		return;
	row = line(f, "1000", VENTURE_LEDGER_SIDE_DEBIT, 1, "USD");
	g_object_set(row, "journal-id", venture_entity_get_id(entity), NULL);
	g_assert_false(venture_database_save(db, VENTURE_ENTITY(row), NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
}

static gboolean
mangle_line(VentureDatabase *db, VentureEntity *entity, VentureEntity *previous, gpointer data, GError **error)
{
	g_autoptr(VentureMoney) amount = venture_money_new_for_currency(9999, "USD");
	VentureLedgerSide side;

	(void)db;
	(void)previous;
	(void)data;
	(void)error;
	g_object_get(entity, "side", &side, NULL);
	if (side == VENTURE_LEDGER_SIDE_DEBIT)
		g_object_set(entity, "book-amount", amount, NULL);
	return TRUE;
}

static void
test_reentrant_integrity(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureJournal) draft = header(f);
	g_autoptr(VentureJournal) posted = NULL;
	g_autoptr(GPtrArray) rows = lines(f, 10000, "USD");
	g_autoptr(GError) error = NULL;

	if (NULL == data)
		g_signal_connect(f->db, "entity-saved", G_CALLBACK(inject_line), f);
	else
		venture_database_add_save_validator(f->db, VENTURE_TYPE_JOURNAL_LINE, mangle_line, NULL, NULL);
	posted = venture_posting_service_post(venture_database_get_posting_service(f->db),
		draft, rows, NULL, NULL, &error);
	if (NULL == data)
	{
		g_assert_no_error(error);
		g_assert_nonnull(posted);
		g_assert_cmpint(count(f, VENTURE_TYPE_JOURNAL_LINE), ==, 2);
	}
	else
	{
		g_assert_null(posted);
		g_assert_nonnull(error);
		g_assert_cmpint(count(f, VENTURE_TYPE_JOURNAL), ==, 0);
	}
}

static GError *
nested_post(VenturePostingService *service, VentureJournal *journal, GPtrArray *rows, gpointer data)
{
	g_autoptr(VentureJournal) posted = NULL;
	g_autoptr(GError) error = NULL;

	(void)data;
	posted = venture_posting_service_post(service, journal, rows, NULL, NULL, &error);
	g_assert_null(posted);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
	/* Even an observer that ignores the error cannot restart writes outside
	 * the enclosing transaction that the nested failure rolled back. */
	return NULL;
}

static void
test_nested_failure(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureJournal) draft = header(f);
	g_autoptr(GPtrArray) rows = lines(f, 10000, "USD");
	g_autoptr(GError) error = NULL;
	VenturePostingService *service = venture_database_get_posting_service(f->db);

	(void)data;
	g_signal_connect(service, "posting", G_CALLBACK(nested_post), NULL);
	g_assert_null(venture_posting_service_post(service, draft, rows, NULL, NULL, &error));
	g_assert_nonnull(error);
	g_assert_cmpint(count(f, VENTURE_TYPE_JOURNAL), ==, 0);
	g_assert_cmpint(count(f, VENTURE_TYPE_JOURNAL_LINE), ==, 0);
	g_assert_cmpint(count(f, VENTURE_TYPE_LEDGER_ENTRY), ==, 0);
}

static GError *
date_inject(VenturePostingService *service, gint64 org, GDateTime *when, gpointer data)
{
	Fixture *f = data;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_JOURNAL);
	g_autoptr(VentureEntity) draft = venture_database_find_one(f->db, query, NULL);
	g_autoptr(VentureJournalLine) row = line(f, "1000", VENTURE_LEDGER_SIDE_DEBIT, 1, "USD");
	g_autoptr(GError) error = NULL;

	(void)service;
	(void)org;
	(void)when;
	g_object_set(row, "journal-id", venture_entity_get_id(draft), NULL);
	g_assert_false(venture_database_save(f->db, VENTURE_ENTITY(row), NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	return NULL;
}

static void
test_date_integrity(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureJournal) draft = header(f);
	g_autoptr(VentureJournal) posted = NULL;
	g_autoptr(GPtrArray) rows = lines(f, 10000, "USD");
	g_autoptr(GError) error = NULL;
	VenturePostingService *service = venture_database_get_posting_service(f->db);
	guint i;

	(void)data;
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(draft), NULL, &error));
	for (i = 0; i < rows->len; i++)
	{
		g_object_set(g_ptr_array_index(rows, i), "journal-id", venture_entity_get_id(VENTURE_ENTITY(draft)), NULL);
		g_assert_true(venture_database_save(f->db, g_ptr_array_index(rows, i), NULL, &error));
	}
	g_signal_connect(service, "date-postable", G_CALLBACK(date_inject), f);
	posted = venture_posting_service_post(service, draft, NULL, NULL, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(posted);
	g_assert_cmpint(count(f, VENTURE_TYPE_JOURNAL_LINE), ==, 2);
}

static gboolean
forge_posted_state(VentureDatabase *db, VentureEntity *entity, VentureEntity *previous,
	gpointer data, GError **error)
{
	(void)db;
	(void)previous;
	(void)data;
	(void)error;
	g_object_set(entity, "state", VENTURE_JOURNAL_POSTED, NULL);
	return TRUE;
}

static void
test_generic_validator_state(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureJournal) draft = header(f);
	g_autoptr(GError) error = NULL;

	(void)data;
	venture_database_add_save_validator(f->db, VENTURE_TYPE_JOURNAL,
		forge_posted_state, NULL, NULL);
	/* All saves pass validators; a validator cannot turn an empty generic
	 * draft into an authoritative posted journal behind the service. */
	g_assert_false(venture_database_save(f->db, VENTURE_ENTITY(draft), NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_assert_cmpint(count(f, VENTURE_TYPE_JOURNAL), ==, 0);
}

static gboolean
forge_identity(VentureDatabase *db, VentureEntity *entity, VentureEntity *previous,
	gpointer data, GError **error)
{
	(void)db;
	(void)previous;
	(void)data;
	(void)error;
	g_object_set(entity, "uuid", "unvalidated-identity", NULL);
	return TRUE;
}

static void
test_identity_integrity(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureJournal) draft = header(f);
	g_autoptr(GPtrArray) rows = lines(f, 10000, "USD");
	g_autoptr(GError) error = NULL;

	(void)data;
	venture_database_add_save_validator(f->db, VENTURE_TYPE_JOURNAL, forge_identity, NULL, NULL);
	g_assert_null(venture_posting_service_post(venture_database_get_posting_service(f->db),
		draft, rows, NULL, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
	g_assert_cmpint(count(f, VENTURE_TYPE_JOURNAL), ==, 0);
}

static void
test_balance_boundaries(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureJournal) draft = header(f);
	g_autoptr(GPtrArray) rows = lines(f, 10000, "USD");
	g_autoptr(VentureJournal) posted = NULL;
	g_autoptr(GDateTime) as_of = date("2026-02-01T00:00:00Z");
	g_autoptr(VentureMoney) balance = NULL;
	g_autoptr(GError) error = NULL;
	VenturePostingService *service = venture_database_get_posting_service(f->db);
	gint64 org = f->org;
	gboolean other_org = g_strcmp0(data, "organization") == 0;

	posted = venture_posting_service_post(service, draft, rows, NULL, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(posted);
	if (other_org)
	{
		g_autoptr(VentureOrganization) other = venture_organization_new();

		g_object_set(other, "name", "Other books", NULL);
		g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(other), NULL, &error));
		org = venture_entity_get_id(VENTURE_ENTITY(other));
	}
	else
	{
		g_autoptr(VentureMoney) amount = venture_money_new_for_currency(10000, "USD");
		guint i;

		g_clear_object(&draft);
		g_clear_pointer(&rows, g_ptr_array_unref);
		draft = header(f);
		rows = lines(f, 10000, "USD");
		g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(draft), NULL, &error));
		for (i = 0; i < rows->len; i++)
		{
			g_object_set(g_ptr_array_index(rows, i), "journal-id", venture_entity_get_id(VENTURE_ENTITY(draft)),
				"book-amount", amount, NULL);
			g_assert_true(venture_database_save(f->db, g_ptr_array_index(rows, i), NULL, &error));
		}
	}
	balance = venture_posting_service_account_balance(service, account(f, "1000"), org, "USD", as_of, &error);
	g_assert_no_error(error);
	g_assert_nonnull(balance);
	g_assert_cmpint(balance->amount, ==, other_org ? 0 : 10000);
}

static void
test_batch_identity(Fixture *f, gconstpointer data)
{
	g_autoptr(GPtrArray) entries = g_ptr_array_new_with_free_func(g_object_unref);
	g_autoptr(VentureMoney) amount = venture_money_new_for_currency(1000, "USD");
	g_autoptr(GDateTime) when = date("2026-01-10T00:00:00Z");
	g_autoptr(GError) error = NULL;
	const gchar *which = data;
	guint i;

	for (i = 0; i < 2; i++)
	{
		VentureLedgerEntry *entry = venture_ledger_entry_new();

		g_object_set(entry, "transaction-id", "stable-transaction",
			"occurred-at", when,
			"source-type", "organization", "source-id", f->org, "organization-id", f->org,
			"account-id", account(f, i == 0 ? "1000" : "4000"),
			"side", i == 0 ? VENTURE_LEDGER_SIDE_DEBIT : VENTURE_LEDGER_SIDE_CREDIT,
			"amount", amount, NULL);
		g_ptr_array_add(entries, entry);
	}
	if (g_strcmp0(which, "duplicate") != 0 && g_strcmp0(which, "reverse-retry") != 0)
	{
		if (g_strcmp0(which, "date") == 0)
			g_object_set(g_ptr_array_index(entries, 1), "occurred-at", NULL, NULL);
		else if (g_strcmp0(which, "source-type") == 0)
			g_object_set(g_ptr_array_index(entries, 1), "source-type", "expense", NULL);
		else if (g_strcmp0(which, "empty-transaction") == 0)
		{
			g_object_set(g_ptr_array_index(entries, 0), "transaction-id", "", NULL);
			g_object_set(g_ptr_array_index(entries, 1), "transaction-id", "", NULL);
		}
		else
			g_object_set(g_ptr_array_index(entries, 1), "source-id", f->org + 1, NULL);
		g_assert_false(venture_database_save_ledger_transaction(f->db, entries, NULL, &error));
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
		g_assert_cmpint(count(f, VENTURE_TYPE_JOURNAL), ==, 0);
	}
	else
	{
		g_assert_true(venture_database_save_ledger_transaction(f->db, entries, NULL, &error));
		if (g_strcmp0(which, "reverse-retry") == 0)
		{
			g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_JOURNAL);
			g_autoptr(VentureEntity) original = venture_database_find_one(f->db, query, &error);
			g_autoptr(VentureJournal) reversed = venture_posting_service_reverse(
				venture_database_get_posting_service(f->db), venture_entity_get_id(original), when,
				"Correct batch", NULL, &error);

			g_assert_no_error(error);
			g_assert_nonnull(reversed);
		}
		g_assert_false(venture_database_save_ledger_transaction(f->db, entries, NULL, &error));
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_ALREADY_EXISTS);
		g_assert_cmpint(count(f, VENTURE_TYPE_JOURNAL), ==,
			g_strcmp0(which, "reverse-retry") == 0 ? 2 : 1);
	}
}

static void
test_balance_scope(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureJournal) draft = header(f);
	g_autoptr(VentureJournal) usd = NULL;
	g_autoptr(VentureJournal) eur = NULL;
	g_autoptr(GPtrArray) rows = lines(f, 10000, "USD");
	g_autoptr(VentureOrganization) other = venture_organization_new();
	g_autoptr(GDateTime) as_of = date("2026-02-01T00:00:00Z");
	g_autoptr(VentureMoney) amount = venture_money_new_for_currency(20000, "EUR");
	g_autoptr(VentureMoney) balance = NULL;
	g_autoptr(GError) error = NULL;
	VenturePostingService *service = venture_database_get_posting_service(f->db);
	gint64 cash = account(f, "1000");
	guint i;

	(void)data;
	usd = venture_posting_service_post(service, draft, rows, NULL, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(usd);
	g_clear_object(&draft);
	draft = header(f);
	g_object_set(draft, "currency", "EUR", NULL);
	g_clear_pointer(&rows, g_ptr_array_unref);
	rows = lines(f, 10000, "USD");
	for (i = 0; i < rows->len; i++)
		g_object_set(g_ptr_array_index(rows, i), "amount", amount, NULL);
	eur = venture_posting_service_post(service, draft, rows, NULL, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(eur);

	g_clear_object(&draft);
	draft = header(f);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(draft), NULL, &error));
	g_clear_pointer(&amount, venture_money_free);
	amount = venture_money_new_for_currency(9000000, "USD");
	g_clear_pointer(&rows, g_ptr_array_unref);
	rows = lines(f, 10000, "USD");
	for (i = 0; i < rows->len; i++)
	{
		g_object_set(g_ptr_array_index(rows, i), "journal-id",
			venture_entity_get_id(VENTURE_ENTITY(draft)), "amount", amount,
			"book-amount", amount, NULL);
		g_assert_true(venture_database_save(f->db, g_ptr_array_index(rows, i), NULL, &error));
	}
	/* Neither a valued draft nor a mutable opening balance is evidence. */
	{
		g_autoptr(VentureEntity) cash_account = venture_database_get(f->db, VENTURE_TYPE_ACCOUNT, cash, &error);

		g_object_set(cash_account, "opening-balance", amount, NULL);
		g_assert_true(venture_database_save(f->db, cash_account, NULL, &error));
	}
	balance = venture_posting_service_account_balance(service, cash, f->org, "USD", as_of, &error);
	g_assert_no_error(error);
	g_assert_cmpint(balance->amount, ==, 10000);
	g_clear_pointer(&balance, venture_money_free);
	balance = venture_posting_service_account_balance(service, cash, f->org, "EUR", as_of, &error);
	g_assert_no_error(error);
	g_assert_cmpint(balance->amount, ==, 20000);
	g_object_set(other, "name", "Separate entity", NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(other), NULL, &error));
	g_clear_pointer(&balance, venture_money_free);
	balance = venture_posting_service_account_balance(service, cash,
		venture_entity_get_id(VENTURE_ENTITY(other)), "USD", as_of, &error);
	g_assert_no_error(error);
	g_assert_cmpint(balance->amount, ==, 0);
}

/* Moving an account must not remove one side of an immutable journal from
 * the original organization's trial balance. */
static void
test_posted_account_ownership(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureJournal) draft = header(f);
	g_autoptr(GPtrArray) rows = lines(f, 10000, "USD");
	g_autoptr(VentureJournal) posted = NULL;
	g_autoptr(VentureOrganization) other = venture_organization_new();
	g_autoptr(VentureEntity) cash = NULL;
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(GError) error = NULL;
	VentureReport *report;

	(void)data;
	posted = venture_posting_service_post(venture_database_get_posting_service(f->db),
		draft, rows, NULL, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(posted);
	g_object_set(other, "name", "Other entity", NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(other), NULL, &error));
	cash = venture_database_get(f->db, VENTURE_TYPE_ACCOUNT, account(f, "1000"), &error);
	venture_entity_set_organization_id(cash, venture_entity_get_id(VENTURE_ENTITY(other)));
	g_assert_false(venture_database_save(f->db, cash, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_clear_error(&error);
	g_assert_false(venture_database_purge(f->db, cash, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_clear_error(&error);
	venture_entity_set_organization_id(cash, f->org);
	g_object_set(cash, "name", "Renamed cash", NULL);
	g_assert_true(venture_database_save(f->db, cash, NULL, &error));
	g_assert_no_error(error);
	report = venture_report_registry_lookup(venture_context_get_report_registry(f->context), "trial_balance");
	result = venture_report_generate(report, f->context, NULL, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
#define ADD(name, fn) g_test_add("/ledger/" name, Fixture, NULL, setup, fn, teardown)
	ADD("module", test_module);
	ADD("records", test_records);
	ADD("generic-ledger-write", test_generic_ledger_write);
	ADD("post-and-lookup", test_post_and_lookup);
	ADD("draft-transition", test_draft_transition);
	ADD("reverse-and-balance", test_reverse_and_balance);
	ADD("posted-account-ownership", test_posted_account_ownership);
	ADD("plugin-rule", test_plugin_rule);
	ADD("exchange", test_exchange);
	ADD("approval-rule-revision", test_approval_rule_revision);
	ADD("rate-table", test_rate_table);
	ADD("source-saves", test_source_saves);
	ADD("source-veto", test_source_veto);
	ADD("invoice-rule", test_invoice_rule);
	ADD("trial-and-module-off", test_trial_and_module_off);
	ADD("reentrant-integrity", test_reentrant_integrity);
	ADD("batch-identity", test_batch_identity);
	ADD("nested-failure", test_nested_failure);
	ADD("date-integrity", test_date_integrity);
	ADD("generic-validator-state", test_generic_validator_state);
	ADD("identity-integrity", test_identity_integrity);
	ADD("balance-scope", test_balance_scope);
#define CASE(group, name, fn) g_test_add("/ledger/" group "/" name, Fixture, name, setup, fn, teardown)
	CASE("refuse", "unbalanced", test_refusals);
	CASE("refuse", "currency", test_refusals);
	CASE("refuse", "organization", test_refusals);
	CASE("refuse", "line-organization", test_refusals);
	CASE("refuse", "account", test_refusals);
	CASE("refuse", "source", test_refusals);
	CASE("refuse", "negative", test_refusals);
	CASE("refuse", "empty", test_refusals);
	CASE("refuse", "date", test_refusals);
	CASE("refuse", "overflow", test_refusals);
	CASE("immutable", "header-save", test_immutable);
	CASE("immutable", "header-delete", test_immutable);
	CASE("immutable", "header-purge", test_immutable);
	CASE("immutable", "line-save", test_immutable);
	CASE("immutable", "line-move", test_immutable);
	CASE("immutable", "line-delete", test_immutable);
	CASE("immutable", "line-purge", test_immutable);
	CASE("immutable", "projection-save", test_immutable);
	CASE("immutable", "projection-delete", test_immutable);
	CASE("immutable", "projection-purge", test_immutable);
	CASE("atomic", "veto", test_atomic_and_signals);
	CASE("atomic", "closed", test_atomic_and_signals);
	CASE("atomic", "storage", test_atomic_and_signals);
	CASE("atomic", "commit", test_atomic_and_signals);
	CASE("atomic", "rollback", test_atomic_and_signals);
	CASE("exchange", "wrong-currency", test_exchange);
	CASE("draft-consent", "sale", test_unvalued_source_approval);
	CASE("draft-consent", "expense", test_unvalued_source_approval);
	CASE("approved-exchange", "opaque", test_approved_exchange_boundary);
	CASE("approved-exchange", "foreign-table", test_approved_exchange_boundary);
	CASE("reentrant", "mutating-validator", test_reentrant_integrity);
	CASE("batch", "duplicate", test_batch_identity);
	CASE("batch", "date", test_batch_identity);
	CASE("batch", "source-type", test_batch_identity);
	CASE("batch", "empty-transaction", test_batch_identity);
	CASE("batch", "reverse-retry", test_batch_identity);
	CASE("balance", "draft", test_balance_boundaries);
	CASE("balance", "organization", test_balance_boundaries);
#undef CASE
#undef ADD
	return g_test_run();
}
