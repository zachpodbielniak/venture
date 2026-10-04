/*
 * test-ledger-registry.c - Which saves post, decided by registration
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Posting on save used to be two hard-coded types. It is now a registry on
 * the database's posting service: a record type, the rule that builds its
 * lines, whether a version is postable and which date it posts on. Sale and
 * expense are registered first, through the same door, and must behave
 * exactly as they did. A record type defined here, which the ledger has
 * never heard of, registers and then posts, reposts on a financial change,
 * leaves an unchanged save alone, takes back everything it posted when it
 * stops being postable, and posts again when it is postable again.
 */

#include <venture.h>

#include <string.h>

/* ==========================================================================
 * A record type the ledger knows nothing about
 * ========================================================================== */

#define VENTURE_TYPE_LEDGER_TOY (venture_ledger_toy_get_type())

VENTURE_DECLARE_ENTITY(VentureLedgerToy, venture_ledger_toy, LEDGER_TOY)

static const VentureFieldDecl venture_ledger_toy_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", NULL),
	VENTURE_FIELD_MONEY("amount", "Amount", NULL),
	VENTURE_FIELD("state", "State", "planned, executed or cancelled",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("account-id", "Account", "Debited; 1000 when empty", "account",
	                  VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("occurred-at", "When", NULL, VENTURE_FIELD_KIND_DATETIME,
	              VENTURE_COLUMN_FLAG_INDEXED)
};

VENTURE_DEFINE_ENTITY(VentureLedgerToy, venture_ledger_toy, venture_ledger_toy_fields)

#define TOY_RULE_NAME "toy_leg"

/* How many times the toy rule has been built, to show a save builds it for
 * both versions -- which is why a rule must be pure. */
static guint toy_builds = 0;

/* Only an executed leg with an amount belongs in the books. */
static gboolean
toy_postable(VentureEntity *entity, gpointer user_data)
{
	g_autofree gchar *state = NULL;
	g_autoptr(VentureMoney) amount = NULL;

	g_assert_true(user_data == (gpointer)&toy_builds);
	g_object_get(entity, "state", &state, "amount", &amount, NULL);
	return 0 == g_strcmp0(state, "executed") && NULL != amount;
}

G_DECLARE_FINAL_TYPE(ToyRule, toy_rule, TOY, RULE, GObject)
struct _ToyRule { GObject parent_instance; };
static void toy_rule_iface(VenturePostingRuleInterface *iface);
G_DEFINE_FINAL_TYPE_WITH_CODE(ToyRule, toy_rule, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(VENTURE_TYPE_POSTING_RULE, toy_rule_iface))

static const gchar *
toy_rule_name(VenturePostingRule *rule)
{
	(void)rule;
	return TOY_RULE_NAME;
}

/* The chart's account with @code in @org; the rule only reads. */
static gint64
toy_account(VentureDatabase *db, gint64 org, const gchar *code, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ACCOUNT);
	g_autoptr(VentureEntity) found = NULL;

	venture_query_set_organization(query, org);
	venture_query_add_filter_string(query, "code", VENTURE_FILTER_OP_EQ, code, NULL);
	found = venture_database_find_one(db, query, error);
	if (NULL == found)
	{
		if (NULL == error || NULL == *error)
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "No account %s", code);
		return 0;
	}
	return venture_entity_get_id(found);
}

/* Dr the leg's account (or cash), Cr sales; an amount of zero builds no
 * lines at all, which is the "rule builds nothing" way to stop posting. */
static GPtrArray *
toy_rule_lines(VenturePostingRule *rule, VentureDatabase *db, VentureEntity *source, GError **error)
{
	g_autoptr(GPtrArray) rows = g_ptr_array_new_with_free_func(g_object_unref);
	g_autoptr(VentureMoney) amount = NULL;
	gint64 org = venture_entity_get_organization_id(source);
	gint64 debit = 0;
	gint64 credit;
	guint i;

	(void)rule;
	toy_builds++;
	g_object_get(source, "amount", &amount, "account-id", &debit, NULL);
	if (NULL == amount)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "A toy leg needs an amount");
		return NULL;
	}
	if (venture_money_is_zero(amount))
		return g_steal_pointer(&rows);
	if (debit <= 0)
		debit = toy_account(db, org, "1000", error);
	credit = toy_account(db, org, "4000", error);
	if (0 == debit || 0 == credit)
		return NULL;
	for (i = 0; i < 2; i++)
	{
		VentureJournalLine *row = venture_journal_line_new();

		g_object_set(row, "account-id", 0 == i ? debit : credit, "amount", amount,
			"organization-id", org,
			"side", 0 == i ? VENTURE_LEDGER_SIDE_DEBIT : VENTURE_LEDGER_SIDE_CREDIT, NULL);
		g_ptr_array_add(rows, row);
	}
	return g_steal_pointer(&rows);
}

static void
toy_rule_iface(VenturePostingRuleInterface *iface)
{
	iface->get_name = toy_rule_name;
	iface->build_lines = toy_rule_lines;
}
static void toy_rule_init(ToyRule *self) { (void)self; }
static void toy_rule_class_init(ToyRuleClass *klass) { (void)klass; }

/* ==========================================================================
 * Fixture
 * ========================================================================== */

typedef struct
{
	VentureConfig	*config;
	VentureDatabase	*db;
	VentureContext	*context;
	gint64		 org;
	gint64		 venture;
	gint64		 purse;
} Fixture;

static void
save(Fixture *f, gpointer record)
{
	g_autoptr(GError) error = NULL;
	gboolean ok;

	ok = venture_database_save(f->db, VENTURE_ENTITY(record), NULL, &error);
	g_assert_no_error(error);
	g_assert_true(ok);
}

static void
field(gpointer record, const gchar *name, const gchar *value)
{
	g_autoptr(GError) error = NULL;

	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(record), name, value, &error));
	g_assert_no_error(error);
}

static VentureEntity *
record(Fixture *f, const gchar *name)
{
	VentureEntity *entity;

	entity = venture_entity_registry_create(venture_entity_registry_get_default(), name, NULL);
	g_assert_nonnull(entity);
	venture_entity_set_organization_id(entity, f->org);
	return entity;
}

static void
define_currency(Fixture *f, const gchar *code, gint64 exponent, const gchar *treatment)
{
	g_autoptr(VentureEntity) currency = record(f, "currency");

	g_object_set(currency, "code", code, "name", code, "exponent", exponent, NULL);
	field(currency, "book-treatment", treatment);
	save(f, currency);
}

static void
register_toy(Fixture *f)
{
	g_autoptr(GError) error = NULL;

	venture_posting_rule_registry_add(venture_posting_service_get_rules(
		venture_database_get_posting_service(f->db)), g_object_new(toy_rule_get_type(), NULL));
	g_assert_true(venture_ledger_register_source_type(f->db, VENTURE_TYPE_LEDGER_TOY, TOY_RULE_NAME,
		toy_postable, "occurred-at", VENTURE_LEDGER_SOURCE_NONE, &toy_builds, NULL, &error));
	g_assert_no_error(error);
}

static void
setup_common(Fixture *f, gboolean game)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureVenture) venture = venture_venture_new();

	venture_currency_clear_registered();
	f->config = venture_config_new();
	f->db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->db);
	f->org = venture_context_get_default_organization_id(f->context);
	g_object_set(venture, "name", "Evermoor", "venture-type", "books", "organization-id", f->org, NULL);
	save(f, venture);
	f->venture = venture_entity_get_id(VENTURE_ENTITY(venture));
	register_toy(f);
	if (game)
	{
		g_autoptr(VentureEntity) organization = NULL;
		g_autoptr(VentureEntity) rate = record(f, "exchange_rate");
		g_autoptr(VentureEntity) place = record(f, "location");

		/* GOLD keeps the books; SILVER converts into it at 1:10; TICKET is
		 * memo; BREWFEST is a book of its own. */
		define_currency(f, "GOLD", 4, "valued");
		define_currency(f, "SILVER", 4, "valued");
		define_currency(f, "TICKET", 0, "memo");
		define_currency(f, "BREWFEST", 0, "separate_book");
		organization = venture_database_get(f->db, VENTURE_TYPE_ORGANIZATION, f->org, &error);
		g_assert_no_error(error);
		g_object_set(organization, "default-currency", "GOLD", NULL);
		save(f, organization);
		g_object_set(rate, "from-currency", "SILVER", "to-currency", "GOLD",
			"rate-numerator", (gint64)1, "rate-denominator", (gint64)10, NULL);
		field(rate, "effective-at", "2026-01-01");
		save(f, rate);
		g_object_set(place, "name", "Aria", "kind", "character", NULL);
		save(f, place);
		g_assert_true(venture_holdings_account_for_location(f->db, f->org,
			venture_entity_get_id(place), TRUE, NULL, &f->purse, &error));
		g_assert_no_error(error);
	}
}

static void
setup_plain(Fixture *f, gconstpointer data)
{
	(void)data;
	setup_common(f, FALSE);
}

static void
setup_game(Fixture *f, gconstpointer data)
{
	(void)data;
	setup_common(f, TRUE);
}

static void
teardown(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureConfig) everything = NULL;
	g_autoptr(VentureModuleRegistry) registry = NULL;

	(void)data;
	g_clear_object(&f->context);
	g_clear_object(&f->db);
	g_clear_object(&f->config);
	/* The currency registry and the entity registry's module mask are
	 * process-wide; a test that switched the ledger off must not leave it
	 * off for the next. */
	venture_currency_clear_registered();
	everything = venture_config_new();
	registry = venture_module_registry_new();
	venture_module_registry_register_builtins(registry);
	venture_module_registry_configure(registry, everything, NULL);
	venture_module_registry_apply(registry, venture_entity_registry_get_default());
}

/* ==========================================================================
 * Reading the books
 * ========================================================================== */

static GPtrArray *
journals_for(Fixture *f, gpointer source)
{
	g_autoptr(GError) error = NULL;
	GPtrArray *journals;

	journals = venture_posting_service_find_source(venture_database_get_posting_service(f->db),
		venture_entity_get_entity_name(VENTURE_ENTITY(source)),
		venture_entity_get_id(VENTURE_ENTITY(source)), f->org, &error);
	g_assert_no_error(error);
	g_assert_nonnull(journals);
	return journals;
}

/* Journals of @source with @rule in @state; a NULL @rule counts any. */
static guint
count_journals(Fixture *f, gpointer source, const gchar *rule, VentureJournalState state)
{
	g_autoptr(GPtrArray) journals = journals_for(f, source);
	guint found = 0;
	guint i;

	for (i = 0; i < journals->len; i++)
	{
		g_autofree gchar *name = NULL;
		VentureJournalState got;

		g_object_get(g_ptr_array_index(journals, i), "rule-name", &name, "state", &got, NULL);
		if ((NULL == rule || 0 == g_strcmp0(rule, name)) && got == state)
			found++;
	}
	return found;
}

static guint
all_journals(Fixture *f, gpointer source)
{
	g_autoptr(GPtrArray) journals = journals_for(f, source);

	return journals->len;
}

/* The one journal @source has posted under @rule and not reversed. */
static VentureEntity *
current_journal(Fixture *f, gpointer source, const gchar *rule)
{
	g_autoptr(GPtrArray) journals = journals_for(f, source);
	VentureEntity *current = NULL;
	guint i;

	for (i = 0; i < journals->len; i++)
	{
		g_autofree gchar *name = NULL;
		VentureJournalState state;

		g_object_get(g_ptr_array_index(journals, i), "rule-name", &name, "state", &state, NULL);
		if (0 == g_strcmp0(rule, name) && VENTURE_JOURNAL_POSTED == state)
		{
			g_assert_null(current);
			current = g_object_ref(g_ptr_array_index(journals, i));
		}
	}
	g_assert_nonnull(current);
	return current;
}

/* The debit side of @journal in original minor units, and its currency. */
static gint64
debits_of(Fixture *f, gpointer journal, gchar **out_currency)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_JOURNAL_LINE);
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GError) error = NULL;
	gint64 total = 0;
	guint i;

	venture_query_set_limit(query, 0);
	venture_query_add_filter_int(query, "journal-id", VENTURE_FILTER_OP_EQ,
		venture_entity_get_id(VENTURE_ENTITY(journal)), NULL);
	rows = venture_database_find(f->db, query, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(rows->len, >=, 2);
	for (i = 0; i < rows->len; i++)
	{
		g_autoptr(VentureMoney) amount = NULL;
		VentureLedgerSide side;

		g_object_get(g_ptr_array_index(rows, i), "amount", &amount, "side", &side, NULL);
		if (VENTURE_LEDGER_SIDE_DEBIT == side)
			total += amount->amount;
	}
	if (NULL != out_currency)
		g_object_get(journal, "currency", out_currency, NULL);
	return total;
}

/* The memo movements @source's rule wrote. */
static guint
movements_of(Fixture *f, gpointer source)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_HOLDING_TXN);
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GError) error = NULL;

	venture_query_set_limit(query, 0);
	venture_query_add_filter_string(query, "source-type", VENTURE_FILTER_OP_EQ,
		venture_entity_get_entity_name(VENTURE_ENTITY(source)), NULL);
	venture_query_add_filter_int(query, "source-id", VENTURE_FILTER_OP_EQ,
		venture_entity_get_id(VENTURE_ENTITY(source)), NULL);
	rows = venture_database_find(f->db, query, &error);
	g_assert_no_error(error);
	return rows->len;
}

static gint64
held(Fixture *f, const gchar *currency)
{
	g_autoptr(VentureMoney) balance = NULL;
	g_autoptr(GError) error = NULL;

	balance = venture_holdings_balance(f->db, f->purse, currency, 0, &error);
	g_assert_no_error(error);
	g_assert_nonnull(balance);
	return venture_money_get_amount(balance);
}

static VentureEntity *
toy(Fixture *f, const gchar *state, const gchar *amount)
{
	VentureEntity *leg = VENTURE_ENTITY(venture_ledger_toy_new());

	venture_entity_set_organization_id(leg, f->org);
	g_object_set(leg, "name", "Leg", "state", state, NULL);
	field(leg, "occurred-at", "2026-03-02");
	if (NULL != amount)
		field(leg, "amount", amount);
	return leg;
}

/* ==========================================================================
 * Registration
 * ========================================================================== */

/*
 * The registry refuses what it cannot honour: a type registered twice
 * (two rules would post one record twice), something that is not a record,
 * a missing rule name, and a date that is not a date-time of the type (a
 * misspelt one would otherwise date every journal by creation time). The
 * built-ins are found under their own rule names, and a type nobody
 * registered never posts. If this regresses, a module installing twice
 * double-posts, or a typo posts on the wrong day with no error.
 */
static void
test_register(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;

	(void)data;
	g_assert_cmpstr(venture_ledger_lookup_source_type(f->db, VENTURE_TYPE_SALE), ==, "sale");
	g_assert_cmpstr(venture_ledger_lookup_source_type(f->db, VENTURE_TYPE_EXPENSE), ==, "expense");
	g_assert_cmpstr(venture_ledger_lookup_source_type(f->db, VENTURE_TYPE_LEDGER_TOY), ==, TOY_RULE_NAME);
	g_assert_null(venture_ledger_lookup_source_type(f->db, VENTURE_TYPE_JOURNAL));
	g_assert_null(venture_ledger_lookup_source_type(f->db, VENTURE_TYPE_INVOICE));

	g_assert_false(venture_ledger_register_source_type(f->db, VENTURE_TYPE_LEDGER_TOY, TOY_RULE_NAME,
		toy_postable, "occurred-at", VENTURE_LEDGER_SOURCE_NONE, &toy_builds, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_ALREADY_EXISTS);
	g_clear_error(&error);
	g_assert_false(venture_ledger_register_source_type(f->db, VENTURE_TYPE_SALE, "other",
		NULL, NULL, VENTURE_LEDGER_SOURCE_NONE, NULL, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_ALREADY_EXISTS);
	g_clear_error(&error);
	g_assert_false(venture_ledger_register_source_type(f->db, G_TYPE_OBJECT, "other",
		NULL, NULL, VENTURE_LEDGER_SOURCE_NONE, NULL, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);
	g_assert_false(venture_ledger_register_source_type(f->db, VENTURE_TYPE_INVOICE, "",
		NULL, NULL, VENTURE_LEDGER_SOURCE_NONE, NULL, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);
	g_assert_false(venture_ledger_register_source_type(f->db, VENTURE_TYPE_INVOICE, "invoice",
		NULL, "ocurred-at", VENTURE_LEDGER_SOURCE_NONE, NULL, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);
	/* A property that exists but is not a date is no date. */
	g_assert_false(venture_ledger_register_source_type(f->db, VENTURE_TYPE_INVOICE, "invoice",
		NULL, "number", VENTURE_LEDGER_SOURCE_NONE, NULL, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);
	/* None of the refusals registered anything. */
	g_assert_null(venture_ledger_lookup_source_type(f->db, VENTURE_TYPE_INVOICE));
}

/*
 * The helper sale and expense use answers from the record alone: set or
 * not, and never for a property the record does not have.
 */
static void
test_postable_when_set(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) sale = record(f, "sale");

	(void)data;
	g_assert_false(venture_ledger_postable_when_set(sale, (gpointer)"gross"));
	field(sale, "gross", "0 USD");
	g_assert_true(venture_ledger_postable_when_set(sale, (gpointer)"gross"));
	g_assert_false(venture_ledger_postable_when_set(sale, (gpointer)"no-such-property"));
	/* An integer is not "set" or "unset"; the helper only reads values. */
	g_assert_false(venture_ledger_postable_when_set(sale, (gpointer)"quantity"));
	g_object_set(sale, "channel", "faire", NULL);
	g_assert_true(venture_ledger_postable_when_set(sale, (gpointer)"channel"));
}

/* ==========================================================================
 * Sale and expense: exactly as before
 * ========================================================================== */

/*
 * A sale, through every step its posting has always taken: no gross, no
 * journal; a gross posts once; a note edited posts nothing; a gross
 * changed reverses and reposts; a gross of zero still posts its (zero)
 * evidence rather than being taken out of the books; clearing a posted
 * gross is refused; deleting keeps every journal. If this regresses, the
 * registry changed the behaviour of the two types it replaced.
 */
static void
test_sale_as_before(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) sale = record(f, "sale");
	g_autoptr(VentureEntity) current = NULL;
	g_autoptr(GError) error = NULL;

	(void)data;
	g_object_set(sale, "venture-id", f->venture, "channel", "faire", NULL);
	field(sale, "occurred-at", "2026-01-10");
	save(f, sale);
	g_assert_cmpuint(all_journals(f, sale), ==, 0);

	field(sale, "gross", "50 USD");
	save(f, sale);
	g_assert_cmpuint(count_journals(f, sale, "sale", VENTURE_JOURNAL_POSTED), ==, 1);
	g_assert_cmpuint(all_journals(f, sale), ==, 1);
	current = current_journal(f, sale, "sale");
	g_assert_cmpint(debits_of(f, current, NULL), ==, 5000);
	g_clear_object(&current);

	g_object_set(sale, "channel", "the faire, corrected", NULL);
	save(f, sale);
	g_assert_cmpuint(all_journals(f, sale), ==, 1);

	field(sale, "gross", "60 USD");
	save(f, sale);
	g_assert_cmpuint(count_journals(f, sale, "sale", VENTURE_JOURNAL_REVERSED), ==, 1);
	g_assert_cmpuint(count_journals(f, sale, "sale", VENTURE_JOURNAL_POSTED), ==, 1);
	g_assert_cmpuint(all_journals(f, sale), ==, 3);
	current = current_journal(f, sale, "sale");
	g_assert_cmpint(debits_of(f, current, NULL), ==, 6000);
	g_clear_object(&current);

	/* Zero is still an amount: the sale stays in the books at zero. */
	field(sale, "gross", "0 USD");
	save(f, sale);
	g_assert_cmpuint(count_journals(f, sale, "sale", VENTURE_JOURNAL_POSTED), ==, 1);
	g_assert_cmpuint(all_journals(f, sale), ==, 5);
	current = current_journal(f, sale, "sale");
	g_assert_cmpint(debits_of(f, current, NULL), ==, 0);
	g_clear_object(&current);

	g_object_set(sale, "gross", NULL, NULL);
	g_assert_false(venture_database_save(f->db, sale, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	g_assert_cmpuint(all_journals(f, sale), ==, 5);

	g_assert_true(venture_database_delete(f->db, sale, NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(all_journals(f, sale), ==, 5);
	g_assert_cmpuint(count_journals(f, sale, "sale", VENTURE_JOURNAL_POSTED), ==, 1);
}

/* The same steps for an expense, whose amount is `amount`. */
static void
test_expense_as_before(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) expense = record(f, "expense");
	g_autoptr(VentureEntity) current = NULL;
	g_autoptr(GError) error = NULL;

	(void)data;
	g_object_set(expense, "venture-id", f->venture, "description", "Materials", NULL);
	field(expense, "occurred-at", "2026-01-10");
	save(f, expense);
	g_assert_cmpuint(all_journals(f, expense), ==, 0);
	field(expense, "amount", "40 USD");
	save(f, expense);
	g_assert_cmpuint(all_journals(f, expense), ==, 1);
	g_object_set(expense, "description", "Materials, receipt attached", NULL);
	save(f, expense);
	g_assert_cmpuint(all_journals(f, expense), ==, 1);
	field(expense, "amount", "45 USD");
	save(f, expense);
	g_assert_cmpuint(all_journals(f, expense), ==, 3);
	current = current_journal(f, expense, "expense");
	g_assert_cmpint(debits_of(f, current, NULL), ==, 4500);
	g_object_set(expense, "amount", NULL, NULL);
	g_assert_false(venture_database_save(f->db, expense, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	g_assert_true(venture_database_delete(f->db, expense, NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(all_journals(f, expense), ==, 3);
}

/*
 * The currency rule still decides where a sale or expense lands: a valued
 * currency with a rate converts into the book journal, a separate book
 * keeps a journal of its own, and a memo sale into a holding is a
 * movement that an edited amount replaces rather than adds to.
 */
static void
test_currencies_as_before(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) silver = record(f, "expense");
	g_autoptr(VentureEntity) brew = record(f, "expense");
	g_autoptr(VentureEntity) tickets = record(f, "sale");
	g_autoptr(VentureEntity) current = NULL;
	g_autofree gchar *currency = NULL;
	g_autofree gchar *book = NULL;

	(void)data;
	g_object_set(silver, "venture-id", f->venture, "description", "Silver ore", NULL);
	field(silver, "occurred-at", "2026-03-02");
	field(silver, "amount", "10 SILVER");
	save(f, silver);
	current = current_journal(f, silver, "expense");
	g_object_get(current, "currency", &currency, NULL);
	g_assert_cmpstr(currency, ==, "GOLD");
	g_clear_object(&current);
	g_clear_pointer(&currency, g_free);

	g_object_set(brew, "venture-id", f->venture, "description", "Festival mug", NULL);
	field(brew, "occurred-at", "2026-03-02");
	field(brew, "amount", "5 BREWFEST");
	save(f, brew);
	current = current_journal(f, brew, "expense");
	g_assert_cmpint(debits_of(f, current, &book), ==, 5);
	g_assert_cmpstr(book, ==, "BREWFEST");
	g_clear_object(&current);

	g_object_set(tickets, "venture-id", f->venture, "cash-account-id", f->purse, NULL);
	field(tickets, "occurred-at", "2026-03-03");
	field(tickets, "gross", "2 TICKET");
	save(f, tickets);
	g_assert_cmpuint(count_journals(f, tickets, "sale", VENTURE_JOURNAL_POSTED), ==, 0);
	g_assert_cmpint(held(f, "TICKET"), ==, 2);
	field(tickets, "gross", "3 TICKET");
	save(f, tickets);
	g_assert_cmpint(held(f, "TICKET"), ==, 3);
	g_assert_cmpuint(movements_of(f, tickets), ==, 1);
	g_object_set(tickets, "channel", "noted", NULL);
	save(f, tickets);
	g_assert_cmpint(held(f, "TICKET"), ==, 3);
	g_assert_cmpuint(movements_of(f, tickets), ==, 1);
}

/* ==========================================================================
 * A registered type
 * ========================================================================== */

/*
 * The toy posts on its first postable save, leaves an unchanged save
 * alone (building its rule for both versions to find that out), reverses
 * and reposts a changed amount, takes everything back when it is
 * cancelled, does nothing on a further save while cancelled, posts again
 * when executed again, takes everything back when its rule builds no
 * lines (an amount of zero), and keeps its journals when deleted. If this
 * regresses, either a record that left the books stays in them, or one
 * that never posted gets written to.
 */
static void
test_toy_lifecycle(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) leg = toy(f, "planned", "100 USD");
	g_autoptr(VentureEntity) current = NULL;
	g_autoptr(GError) error = NULL;
	guint builds;

	(void)data;
	save(f, leg);
	g_object_set(leg, "name", "Leg, renamed", NULL);
	save(f, leg);
	g_assert_cmpuint(all_journals(f, leg), ==, 0);

	g_object_set(leg, "state", "executed", NULL);
	save(f, leg);
	g_assert_cmpuint(all_journals(f, leg), ==, 1);
	current = current_journal(f, leg, TOY_RULE_NAME);
	g_assert_cmpint(debits_of(f, current, NULL), ==, 10000);
	g_clear_object(&current);

	/* Unchanged: the rule is built for this version and the stored one,
	 * and the two agree, so nothing posts. */
	builds = toy_builds;
	g_object_set(leg, "name", "Leg, noted", NULL);
	save(f, leg);
	g_assert_cmpuint(toy_builds - builds, ==, 2);
	g_assert_cmpuint(all_journals(f, leg), ==, 1);

	field(leg, "amount", "150 USD");
	save(f, leg);
	g_assert_cmpuint(count_journals(f, leg, TOY_RULE_NAME, VENTURE_JOURNAL_REVERSED), ==, 1);
	g_assert_cmpuint(all_journals(f, leg), ==, 3);
	current = current_journal(f, leg, TOY_RULE_NAME);
	g_assert_cmpint(debits_of(f, current, NULL), ==, 15000);
	g_clear_object(&current);

	/* Cancelled: not postable, so what it posted is reversed. */
	g_object_set(leg, "state", "cancelled", NULL);
	save(f, leg);
	g_assert_cmpuint(count_journals(f, leg, TOY_RULE_NAME, VENTURE_JOURNAL_POSTED), ==, 0);
	g_assert_cmpuint(all_journals(f, leg), ==, 4);
	g_object_set(leg, "name", "Leg, cancelled", NULL);
	save(f, leg);
	g_assert_cmpuint(all_journals(f, leg), ==, 4);

	g_object_set(leg, "state", "executed", NULL);
	save(f, leg);
	g_assert_cmpuint(count_journals(f, leg, TOY_RULE_NAME, VENTURE_JOURNAL_POSTED), ==, 1);
	g_assert_cmpuint(all_journals(f, leg), ==, 5);
	current = current_journal(f, leg, TOY_RULE_NAME);
	g_assert_cmpint(debits_of(f, current, NULL), ==, 15000);
	g_clear_object(&current);

	/* Postable, but the rule builds nothing: also out of the books. */
	field(leg, "amount", "0 USD");
	save(f, leg);
	g_assert_cmpuint(count_journals(f, leg, TOY_RULE_NAME, VENTURE_JOURNAL_POSTED), ==, 0);
	g_assert_cmpuint(all_journals(f, leg), ==, 6);

	field(leg, "amount", "20 USD");
	save(f, leg);
	g_assert_cmpuint(all_journals(f, leg), ==, 7);
	g_assert_true(venture_database_delete(f->db, leg, NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(all_journals(f, leg), ==, 7);
	g_assert_cmpuint(count_journals(f, leg, TOY_RULE_NAME, VENTURE_JOURNAL_POSTED), ==, 1);
}

/*
 * A toy that is postable on its very first save posts then; one that is
 * created cancelled never posts and writes nothing on later saves either.
 */
static void
test_toy_first_save(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) posted = toy(f, "executed", "7 USD");
	g_autoptr(VentureEntity) never = toy(f, "cancelled", "7 USD");

	(void)data;
	save(f, posted);
	g_assert_cmpuint(count_journals(f, posted, TOY_RULE_NAME, VENTURE_JOURNAL_POSTED), ==, 1);
	save(f, never);
	g_object_set(never, "name", "Still cancelled", NULL);
	save(f, never);
	g_assert_cmpuint(all_journals(f, never), ==, 0);
	g_assert_cmpuint(movements_of(f, never), ==, 0);
}

/*
 * A toy paid into a holding in a memo currency is a movement, not a
 * journal. Cancelling clears the movement, so the purse no longer holds
 * the tickets; executing again puts them back once. A separate-book toy
 * posts a journal in its own currency and is reversed the same way. If
 * this regresses, a cancelled trade still counts in the purse.
 */
static void
test_toy_currencies(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) memo = toy(f, "executed", "4 TICKET");
	g_autoptr(VentureEntity) apart = toy(f, "executed", "9 BREWFEST");
	g_autoptr(VentureEntity) current = NULL;
	g_autofree gchar *book = NULL;

	(void)data;
	g_object_set(memo, "account-id", f->purse, NULL);
	save(f, memo);
	g_assert_cmpuint(all_journals(f, memo), ==, 0);
	g_assert_cmpuint(movements_of(f, memo), ==, 1);
	g_assert_cmpint(held(f, "TICKET"), ==, 4);

	g_object_set(memo, "state", "cancelled", NULL);
	save(f, memo);
	g_assert_cmpuint(movements_of(f, memo), ==, 0);
	g_assert_cmpint(held(f, "TICKET"), ==, 0);

	g_object_set(memo, "state", "executed", NULL);
	save(f, memo);
	g_assert_cmpuint(movements_of(f, memo), ==, 1);
	g_assert_cmpint(held(f, "TICKET"), ==, 4);

	save(f, apart);
	current = current_journal(f, apart, TOY_RULE_NAME);
	g_assert_cmpint(debits_of(f, current, &book), ==, 9);
	g_assert_cmpstr(book, ==, "BREWFEST");
	g_clear_object(&current);
	g_object_set(apart, "state", "cancelled", NULL);
	save(f, apart);
	g_assert_cmpuint(count_journals(f, apart, TOY_RULE_NAME, VENTURE_JOURNAL_POSTED), ==, 0);
	g_assert_cmpuint(count_journals(f, apart, TOY_RULE_NAME, VENTURE_JOURNAL_REVERSED), ==, 1);
}

static void
actor_named(VentureActor *actor, const gchar *name)
{
	actor->kind = VENTURE_ACTOR_KIND_USER;
	actor->name = name;
	actor->prompt = NULL;
	actor->request_id = NULL;
	actor->approved_by = NULL;
}

/*
 * Under a second-actor posting rule, a toy that never posted stays
 * writable without consent, a save that posts it needs a second person,
 * and so does the save that takes it back out of the books: a reversal is
 * a posting. If the unposting save did not cross the consent boundary
 * before its transaction, the second person's approval would be refused
 * too, and a cancelled trade could never leave the books.
 */
static void
test_toy_consent(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) leg = toy(f, "planned", "30 USD");
	g_autoptr(VentureEntity) rule = VENTURE_ENTITY(venture_accounting_approval_rule_new());
	g_autoptr(GError) error = NULL;
	VentureActor actor;

	(void)data;
	g_object_set(rule, "organization-id", f->org, "action", "post", "require-second-actor", TRUE, NULL);
	save(f, rule);
	actor_named(&actor, "alice");
	g_assert_true(venture_database_save(f->db, leg, &actor, &error));
	g_assert_no_error(error);
	g_object_set(leg, "name", "Draft leg", NULL);
	g_assert_true(venture_database_save(f->db, leg, &actor, &error));
	g_assert_no_error(error);

	g_object_set(leg, "state", "executed", NULL);
	g_assert_false(venture_database_save(f->db, leg, &actor, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_clear_error(&error);
	actor_named(&actor, "bob");
	g_assert_true(venture_database_save(f->db, leg, &actor, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(count_journals(f, leg, TOY_RULE_NAME, VENTURE_JOURNAL_POSTED), ==, 1);

	g_object_set(leg, "state", "cancelled", NULL);
	actor_named(&actor, "alice");
	g_assert_false(venture_database_save(f->db, leg, &actor, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_clear_error(&error);
	g_assert_cmpuint(count_journals(f, leg, TOY_RULE_NAME, VENTURE_JOURNAL_POSTED), ==, 1);
	actor_named(&actor, "bob");
	g_assert_true(venture_database_save(f->db, leg, &actor, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(count_journals(f, leg, TOY_RULE_NAME, VENTURE_JOURNAL_POSTED), ==, 0);
	g_assert_cmpuint(count_journals(f, leg, TOY_RULE_NAME, VENTURE_JOURNAL_REVERSED), ==, 1);
}

/*
 * With the ledger switched off nothing posts, registered or not, and the
 * record saves as an ordinary record.
 */
static void
test_toy_ledger_off(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) leg = toy(f, "executed", "5 USD");

	(void)data;
	venture_config_set_module_enabled(f->config, "ledger", FALSE);
	save(f, leg);
	g_assert_cmpint(venture_entity_get_id(leg), >, 0);
	venture_config_set_module_enabled(f->config, "ledger", TRUE);
	g_assert_cmpuint(all_journals(f, leg), ==, 0);
	/* Its next save posts it: it left no trace. */
	g_object_set(leg, "name", "Posted late", NULL);
	save(f, leg);
	g_assert_cmpuint(count_journals(f, leg, TOY_RULE_NAME, VENTURE_JOURNAL_POSTED), ==, 1);
}

int
main(int argc, char **argv)
{
	g_autoptr(GError) error = NULL;

	g_test_init(&argc, &argv, NULL);
	g_assert_true(venture_entity_registry_register(venture_entity_registry_get_default(),
		VENTURE_TYPE_LEDGER_TOY, &error));
	g_assert_no_error(error);
#define PLAIN(name, fn) g_test_add("/ledger-registry/" name, Fixture, NULL, setup_plain, fn, teardown)
#define GAME(name, fn) g_test_add("/ledger-registry/" name, Fixture, NULL, setup_game, fn, teardown)
	PLAIN("register", test_register);
	PLAIN("postable-when-set", test_postable_when_set);
	PLAIN("sale-as-before", test_sale_as_before);
	PLAIN("expense-as-before", test_expense_as_before);
	GAME("currencies-as-before", test_currencies_as_before);
	PLAIN("toy-lifecycle", test_toy_lifecycle);
	PLAIN("toy-first-save", test_toy_first_save);
	GAME("toy-currencies", test_toy_currencies);
	PLAIN("toy-consent", test_toy_consent);
	PLAIN("toy-ledger-off", test_toy_ledger_off);
#undef GAME
#undef PLAIN
	return g_test_run();
}
