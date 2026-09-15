/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
#include <string.h>

typedef struct
{
	gint64 account_id;
	gint64 journal_id;
	gint64 source_id;
	gchar *source_type;
	gchar *dimension;
	GDateTime *date;
	VentureMoney *amount;
	VentureLedgerSide side;
} Evidence;

typedef struct
{
	GPtrArray *accounts;
	GPtrArray *entries;
	GPtrArray *currencies;
	GDateTime *start;
	GDateTime *end;
	GHashTable *roles;
	GHashTable *defaults;
} Books;

struct _VentureLedgerBalances
{
	GObject parent_instance;
	VentureDatabase *database;
	gchar *dimension;
};
G_DEFINE_TYPE(VentureLedgerBalances, venture_ledger_balances, G_TYPE_OBJECT)

static void
balances_finalize(GObject *object)
{
	g_clear_object(&VENTURE_LEDGER_BALANCES(object)->database);
	g_free(VENTURE_LEDGER_BALANCES(object)->dimension);
	G_OBJECT_CLASS(venture_ledger_balances_parent_class)->finalize(object);
}

static void
balances_get_property(GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
	if (id == 1)
		g_value_set_object(value, VENTURE_LEDGER_BALANCES(object)->database);
	else
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
}

static void
balances_set_property(GObject *object, guint id, const GValue *value, GParamSpec *pspec)
{
	if (id == 1)
		g_set_object(&VENTURE_LEDGER_BALANCES(object)->database, g_value_get_object(value));
	else
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
}

static void
venture_ledger_balances_class_init(VentureLedgerBalancesClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	object_class->finalize = balances_finalize;
	object_class->get_property = balances_get_property;
	object_class->set_property = balances_set_property;
	/**
	 * VentureLedgerBalances:database:
	 *
	 * The repository whose posted journals supply the query.
	 */
	g_object_class_install_property(object_class, 1, g_param_spec_object("database",
		"Database", "Posted ledger repository", VENTURE_TYPE_DATABASE,
		G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
}

static void
venture_ledger_balances_init(VentureLedgerBalances *self)
{
	(void)self;
}

VentureLedgerBalances *
venture_ledger_balances_new(VentureDatabase *database)
{
	return g_object_new(VENTURE_TYPE_LEDGER_BALANCES, "database", database, NULL);
}

void
venture_ledger_balances_set_dimension(VentureLedgerBalances *self, const gchar *dimension)
{
	g_return_if_fail(VENTURE_IS_LEDGER_BALANCES(self));
	g_free(self->dimension);
	self->dimension = dimension && dimension[0] ? g_strdup(dimension) : NULL;
}

static void
evidence_free(gpointer data)
{
	Evidence *e = data;
	g_free(e->source_type);
	g_free(e->dimension);
	g_date_time_unref(e->date);
	venture_money_free(e->amount);
	g_free(e);
}

static void
books_free(Books *books)
{
	g_clear_pointer(&books->accounts, g_ptr_array_unref);
	g_clear_pointer(&books->entries, g_ptr_array_unref);
	g_clear_pointer(&books->currencies, g_ptr_array_unref);
	g_clear_pointer(&books->start, g_date_time_unref);
	g_clear_pointer(&books->end, g_date_time_unref);
	g_clear_pointer(&books->roles, g_hash_table_unref);
	g_clear_pointer(&books->defaults, g_hash_table_unref);
	g_free(books);
}
G_DEFINE_AUTOPTR_CLEANUP_FUNC(Books, books_free)

static gboolean
add(VentureMoney **sum, const VentureMoney *amount, gboolean subtract, GError **error)
{
	VentureMoney *next = subtract ? venture_money_subtract(*sum, amount, error) :
		venture_money_add(*sum, amount, error);
	if (next == NULL)
		return FALSE;
	venture_money_free(*sum);
	*sum = next;
	return TRUE;
}

static VentureEntity *
find_account(Books *books, gint64 id)
{
	guint i;
	for (i = 0; i < books->accounts->len; i++)
	{
		VentureEntity *account = g_ptr_array_index(books->accounts, i);
		if (venture_entity_get_id(account) == id)
			return account;
	}
	return NULL;
}

static gboolean
belongs(Books *books, gint64 child, gint64 ancestor)
{
	guint depth;
	for (depth = 0; child != 0 && depth <= books->accounts->len; depth++)
	{
		VentureEntity *account;
		if (child == ancestor)
			return TRUE;
		account = find_account(books, child);
		if (account == NULL)
			return FALSE;
		g_object_get(account, "parent-id", &child, NULL);
	}
	return FALSE;
}

static gint
compare_currency(gconstpointer a, gconstpointer b)
{
	return g_strcmp0(*(gchar * const *)a, *(gchar * const *)b);
}

static void
add_currency(Books *books, const gchar *currency)
{
	guint i;
	for (i = 0; i < books->currencies->len; i++)
		if (g_str_equal(g_ptr_array_index(books->currencies, i), currency))
			return;
	g_ptr_array_add(books->currencies, g_strdup(currency));
}

static Books *
read_books(VentureDatabase *db, gint64 org, const gchar *currency,
	VentureDateRange *period, GDateTime *as_of, const gchar *dimension, GError **error)
{
	g_autoptr(Books) books = g_new0(Books, 1);
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) journals = NULL;
	guint i;
	if (org <= 0 || period == NULL || (currency != NULL &&
		(strlen(currency) != 3 || !g_ascii_isupper(currency[0]) ||
		 !g_ascii_isupper(currency[1]) || !g_ascii_isupper(currency[2]))))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			"Statements require a legal entity, period and uppercase book currency");
		return NULL;
	}
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "journal") == 0)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED, "The ledger module is disabled");
		return NULL;
	}
	books->start = g_date_time_ref(venture_date_range_get_start(period));
	books->end = g_date_time_ref(venture_date_range_get_end(period));
	if (as_of != NULL && g_date_time_compare(as_of, books->end) < 0)
	{
		g_date_time_unref(books->end);
		books->end = g_date_time_add(as_of, 1);
	}
	books->entries = g_ptr_array_new_with_free_func(evidence_free);
	books->currencies = g_ptr_array_new_with_free_func(g_free);
	query = venture_query_new(VENTURE_TYPE_ACCOUNT);
	venture_query_set_limit(query, 0);
	venture_query_set_include_deleted(query, TRUE);
	venture_query_set_organization(query, org);
	venture_query_add_order(query, "code", VENTURE_SORT_ASCENDING, NULL);
	books->accounts = venture_database_find(db, query, error);
	if (books->accounts == NULL)
		return NULL;
	books->roles = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_hash_table_unref);
	books->defaults = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "accounting_control_map") != 0)
	{
		g_autoptr(VentureQuery) maps_query = venture_query_new(VENTURE_TYPE_ACCOUNTING_CONTROL_MAP);
		g_autoptr(GPtrArray) maps = NULL;
		guint m;
		venture_query_set_organization(maps_query, org);
		venture_query_set_limit(maps_query, 0);
		maps = venture_database_find(db, maps_query, error);
		if (maps == NULL)
			return NULL;
		for (m = 0; m < maps->len; m++)
		{
			VentureEntity *map = g_ptr_array_index(maps, m);
			g_autofree gchar *role = NULL;
			g_autofree gchar *subject = NULL;
			g_autoptr(GDateTime) from = NULL;
			gint64 account_id = 0, subject_id = 0;
			GHashTable *ids;
			g_object_get(map, "classification", &role, "account-id", &account_id,
				"subject-type", &subject, "subject-id", &subject_id, "effective-from", &from, NULL);
			if (role == NULL || account_id <= 0)
				continue;
			if (from != NULL && g_date_time_compare(from, books->end) >= 0)
				continue;
			ids = g_hash_table_lookup(books->roles, role);
			if (ids == NULL)
			{
				ids = g_hash_table_new(g_direct_hash, g_direct_equal);
				g_hash_table_insert(books->roles, g_strdup(role), ids);
			}
			g_hash_table_insert(ids, GSIZE_TO_POINTER((gsize)account_id), GINT_TO_POINTER(1));
			if (subject_id == 0 && (subject == NULL || subject[0] == '\0' || g_str_equal(subject, "organization")))
				g_hash_table_insert(books->defaults, g_strdup(role), GSIZE_TO_POINTER((gsize)account_id));
		}
	}
	/* A corrupt hierarchy must not silently double or drop a balance. */
	for (i = 0; i < books->accounts->len; i++)
	{
		VentureEntity *account = g_ptr_array_index(books->accounts, i);
		gint64 parent;
		g_object_get(account, "parent-id", &parent, NULL);
		if (parent != 0 && (find_account(books, parent) == NULL ||
			belongs(books, parent, venture_entity_get_id(account))))
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
				"Account parents must be acyclic and belong to the same organization");
			return NULL;
		}
	}
	g_clear_object(&query);
	query = venture_query_new(VENTURE_TYPE_JOURNAL);
	venture_query_set_limit(query, 0);
	venture_query_set_include_deleted(query, TRUE);
	venture_query_set_organization(query, org);
	venture_query_add_order(query, "occurred-at", VENTURE_SORT_ASCENDING, NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	journals = venture_database_find(db, query, error);
	if (journals == NULL)
		return NULL;
	for (i = 0; i < journals->len; i++)
	{
		VentureEntity *journal = g_ptr_array_index(journals, i);
		g_autoptr(GDateTime) date = NULL;
		g_autofree gchar *book_currency = NULL;
		g_autoptr(GPtrArray) lines = NULL;
		VentureJournalState state;
		guint j;
		gboolean tax_book = FALSE;
		g_object_get(journal, "state", &state, "occurred-at", &date, "currency", &book_currency,
			"tax-book", &tax_book, NULL);
		if (tax_book)
			continue;
		if ((state != VENTURE_JOURNAL_POSTED && state != VENTURE_JOURNAL_REVERSED) ||
			date == NULL || g_date_time_compare(date, books->end) >= 0 ||
			(currency != NULL && g_strcmp0(currency, book_currency) != 0))
			continue;
		add_currency(books, book_currency);
		g_clear_object(&query);
		query = venture_query_new(VENTURE_TYPE_JOURNAL_LINE);
		venture_query_set_limit(query, 0);
		venture_query_set_include_deleted(query, TRUE);
		venture_query_set_organization(query, org);
		venture_query_add_filter_int(query, "journal-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(journal), NULL);
		venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
		lines = venture_database_find(db, query, error);
		if (lines == NULL)
			return NULL;
		for (j = 0; j < lines->len; j++)
		{
			Evidence *e = g_new0(Evidence, 1);
			g_object_get(g_ptr_array_index(lines, j), "account-id", &e->account_id,
				"side", &e->side, "book-amount", &e->amount, "dimension", &e->dimension, NULL);
			g_object_get(journal, "source-type", &e->source_type, "source-id", &e->source_id, NULL);
			e->journal_id = venture_entity_get_id(journal);
			e->date = g_date_time_ref(date);
			if (dimension != NULL && dimension[0] != '\0' && g_strcmp0(e->dimension, dimension) != 0)
			{
				evidence_free(e);
				continue;
			}
			g_ptr_array_add(books->entries, e);
		}
	}
	if (currency != NULL || books->currencies->len == 0)
		add_currency(books, currency != NULL ? currency : venture_money_get_default_currency());
	g_ptr_array_sort(books->currencies, compare_currency);
	return g_steal_pointer(&books);
}

static gboolean
measure(Books *books, gint64 id, const gchar *currency, gboolean rollup,
	VentureMoney **opening, VentureMoney **debits, VentureMoney **credits,
	VentureMoney **closing, GError **error)
{
	guint i;
	*opening = venture_money_new_zero(currency);
	*debits = venture_money_new_zero(currency);
	*credits = venture_money_new_zero(currency);
	for (i = 0; i < books->entries->len; i++)
	{
		Evidence *e = g_ptr_array_index(books->entries, i);
		if (g_strcmp0(e->amount->currency, currency) != 0 ||
			(e->account_id != id && (!rollup || !belongs(books, e->account_id, id))))
			continue;
		if (g_date_time_compare(e->date, books->start) < 0)
		{
			if (!add(opening, e->amount, e->side == VENTURE_LEDGER_SIDE_CREDIT, error))
				return FALSE;
		}
		else if (!add(e->side == VENTURE_LEDGER_SIDE_DEBIT ? debits : credits, e->amount, FALSE, error))
			return FALSE;
	}
	*closing = venture_money_add(*opening, *debits, error);
	return *closing != NULL && add(closing, *credits, TRUE, error);
}

static void
text_column(VentureReportResult *r, const gchar *key, const gchar *label)
{
	venture_report_result_add_column(r, key, label, VENTURE_REPORT_COLUMN_TEXT);
}

static void
money_column(VentureReportResult *r, const gchar *key, const gchar *label)
{
	venture_report_result_add_column(r, key, label, VENTURE_REPORT_COLUMN_MONEY);
}

static void
set_id(VentureReportResult *r, const gchar *key, gint64 id)
{
	g_autofree gchar *text = id != 0 ? g_strdup_printf("%" G_GINT64_FORMAT, id) : g_strdup("");
	venture_report_result_set_text(r, key, text);
}

static void
account_row(VentureReportResult *r, VentureEntity *account, const gchar *currency)
{
	g_autofree gchar *code = NULL;
	g_autofree gchar *name = NULL;
	g_object_get(account, "code", &code, "name", &name, NULL);
	venture_report_result_begin_row(r);
	venture_report_result_set_text(r, "key", code);
	venture_report_result_set_text(r, "name", name);
	venture_report_result_set_text(r, "currency", currency);
	set_id(r, "account_id", venture_entity_get_id(account));
}

static VentureReportResult *
balance_rows(Books *books, VentureDateRange *period, gboolean rollup, GError **error)
{
	g_autoptr(VentureReportResult) r = venture_report_result_new("Account balances", period);
	guint c, i;
	text_column(r, "key", "Code");
	text_column(r, "name", "Account");
	text_column(r, "account_id", "Account ID");
	text_column(r, "currency", "Currency");
	money_column(r, "opening", "Opening");
	money_column(r, "debits", "Debits");
	money_column(r, "credits", "Credits");
	money_column(r, "closing", "Closing");
	for (c = 0; c < books->currencies->len; c++)
	{
		const gchar *currency = g_ptr_array_index(books->currencies, c);
		for (i = 0; i < books->accounts->len; i++)
		{
			VentureEntity *a = g_ptr_array_index(books->accounts, i);
			g_autoptr(VentureMoney) opening = NULL, debits = NULL, credits = NULL, closing = NULL;
			if (!measure(books, venture_entity_get_id(a), currency, rollup, &opening, &debits, &credits, &closing, error))
				return NULL;
			account_row(r, a, currency);
			venture_report_result_set_money(r, "opening", opening);
			venture_report_result_set_money(r, "debits", debits);
			venture_report_result_set_money(r, "credits", credits);
			venture_report_result_set_money(r, "closing", closing);
		}
	}
	return g_steal_pointer(&r);
}

VentureReportResult *
venture_ledger_balances_query(VentureLedgerBalances *self, gint64 organization_id,
	const gchar *currency, VentureDateRange *period, GDateTime *as_of,
	gboolean rollup, GError **error)
{
	g_autoptr(Books) books = NULL;
	g_autoptr(VentureReportResult) result = NULL;
	if (!venture_database_begin(self->database, error))
		return NULL;
	books = read_books(self->database, organization_id, currency, period, as_of, self->dimension, error);
	if (books != NULL)
		result = balance_rows(books, period, rollup, error);
	if (result == NULL)
	{
		venture_database_rollback(self->database);
		return NULL;
	}
	if (!venture_database_commit(self->database, error))
		return NULL;
	return g_steal_pointer(&result);
}

static void
statement_columns(VentureReportResult *r)
{
	text_column(r, "key", "Code");
	text_column(r, "name", "Line");
	text_column(r, "account_id", "Account ID");
	text_column(r, "currency", "Currency");
	money_column(r, "current", "Current");
}

static void
summary_row(VentureReportResult *r, const gchar *key, const gchar *name,
	const VentureMoney *amount)
{
	venture_report_result_begin_row(r);
	venture_report_result_set_text(r, "key", key);
	venture_report_result_set_text(r, "name", name);
	venture_report_result_set_text(r, "account_id", "");
	venture_report_result_set_text(r, "currency", amount->currency);
	venture_report_result_set_money(r, "current", amount);
}

static VentureAccountKind
account_kind(VentureEntity *account)
{
	VentureAccountKind kind;
	g_object_get(account, "kind", &kind, NULL);
	return kind;
}

static gboolean
is_profit_account(VentureEntity *account)
{
	VentureAccountKind kind = account_kind(account);
	return kind == VENTURE_ACCOUNT_KIND_INCOME || kind == VENTURE_ACCOUNT_KIND_EXPENSE;
}

static VentureReportResult *
financial_statement(Books *books, VentureDateRange *period, gboolean balance_sheet, GError **error)
{
	g_autoptr(VentureReportResult) r = venture_report_result_new(balance_sheet ? "Balance sheet" : "Income statement", period);
	guint c, i;
	statement_columns(r);
	text_column(r, "section", "Section");
	for (c = 0; c < books->currencies->len; c++)
	{
		const gchar *currency = g_ptr_array_index(books->currencies, c);
		g_autoptr(VentureMoney) assets = venture_money_new_zero(currency);
		g_autoptr(VentureMoney) liabilities = venture_money_new_zero(currency);
		g_autoptr(VentureMoney) equity = venture_money_new_zero(currency);
		g_autoptr(VentureMoney) income = venture_money_new_zero(currency);
		g_autoptr(VentureMoney) expenses = venture_money_new_zero(currency);
		g_autoptr(VentureMoney) retained = venture_money_new_zero(currency);
		g_autoptr(VentureMoney) net = NULL;
		g_autoptr(VentureMoney) difference = NULL;
		for (i = 0; i < books->accounts->len; i++)
		{
			VentureEntity *a = g_ptr_array_index(books->accounts, i);
			VentureAccountKind kind = account_kind(a);
			g_autoptr(VentureMoney) opening = NULL, debits = NULL, credits = NULL, closing = NULL;
			g_autoptr(VentureMoney) movement = NULL;
			VentureMoney **total;
			const gchar *section;
			gboolean normal = venture_account_kind_is_debit_normal(kind);
			if (!measure(books, venture_entity_get_id(a), currency, FALSE, &opening, &debits, &credits, &closing, error))
				return NULL;
			movement = venture_money_subtract(debits, credits, error);
			if (movement == NULL)
				return NULL;
			if (is_profit_account(a) && !add(&retained, opening, TRUE, error))
				return NULL;
			switch (kind)
			{
			case VENTURE_ACCOUNT_KIND_ASSET: total = &assets; section = "Assets"; break;
			case VENTURE_ACCOUNT_KIND_LIABILITY: total = &liabilities; section = "Liabilities"; break;
			case VENTURE_ACCOUNT_KIND_EQUITY: total = &equity; section = "Equity"; break;
			case VENTURE_ACCOUNT_KIND_INCOME: total = &income; section = "Income"; break;
			default: total = &expenses; section = "Expenses"; break;
			}
			if (!add(total, is_profit_account(a) ? movement : closing, !normal, error))
				return NULL;
			if (balance_sheet == !is_profit_account(a))
			{
				g_autoptr(VentureMoney) ro = NULL, rd = NULL, rc = NULL, rb = NULL;
				g_autoptr(VentureMoney) display = NULL;
				if (!measure(books, venture_entity_get_id(a), currency, TRUE, &ro, &rd, &rc, &rb, error))
					return NULL;
				if (balance_sheet)
					display = venture_money_multiply_int(rb, normal ? 1 : -1, error);
				else
				{
					g_autoptr(VentureMoney) m = venture_money_subtract(rd, rc, error);
					if (m != NULL)
						display = venture_money_multiply_int(m, normal ? 1 : -1, error);
				}
				if (display == NULL)
					return NULL;
				account_row(r, a, currency);
				venture_report_result_set_text(r, "section", section);
				venture_report_result_set_money(r, "current", display);
			}
		}
		net = venture_money_subtract(income, expenses, error);
		if (net == NULL)
			return NULL;
		if (balance_sheet)
		{
			if (!add(&equity, retained, FALSE, error) || !add(&equity, net, FALSE, error))
				return NULL;
			summary_row(r, "retained_income", "Prior-period unclosed earnings", retained);
			summary_row(r, "net_income", "Current-period net income", net);
			summary_row(r, "assets", "Total assets", assets);
			summary_row(r, "liabilities", "Total liabilities", liabilities);
			summary_row(r, "equity", "Total equity including earnings", equity);
			difference = venture_money_subtract(assets, liabilities, error);
			if (difference == NULL || !add(&difference, equity, TRUE, error))
				return NULL;
			if (difference->amount != 0)
			{
				g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_BALANCE, "Assets do not equal liabilities plus equity");
				return NULL;
			}
			summary_row(r, "difference", "Assets less liabilities and equity", difference);
		}
		else
		{
			summary_row(r, "income", "Total income", income);
			summary_row(r, "expenses", "Total expenses", expenses);
			summary_row(r, "net_income", "Net income", net);
		}
	}
	venture_report_result_append_note(r, "Account rows include descendants; section totals count each posted line once. Earnings are ledger income less ledger expenses for the selected period.");
	return g_steal_pointer(&r);
}

static gboolean
code_matches(const gchar *actual, const gchar *code)
{
	if (g_strcmp0(actual, code) == 0)
		return TRUE;
	return actual != NULL && strchr(actual, ':') != NULL &&
		g_strcmp0(strrchr(actual, ':') + 1, code) == 0;
}

static gboolean
control_account(Books *books, gint64 id, const gchar *classification)
{
	static const struct { const gchar *role; const gchar *code; } fallback[] = {
		{ "cash", "1000" }, { "receivables", "1100" }, { "inventory", "1200" },
		{ "payables", "2000" }, { "tax", "2100" }
	};
	GHashTable *ids = books->roles != NULL ? g_hash_table_lookup(books->roles, classification) : NULL;
	guint depth;
	guint i;
	for (depth = 0; id != 0 && depth <= books->accounts->len; depth++)
	{
		VentureEntity *account = find_account(books, id);
		g_autofree gchar *actual = NULL;
		gint64 parent = 0;
		if (account == NULL)
			return FALSE;
		if (ids != NULL && g_hash_table_contains(ids, GSIZE_TO_POINTER((gsize)id)))
			return TRUE;
		g_object_get(account, "code", &actual, "parent-id", &parent, NULL);
		if (ids == NULL)
		{
			for (i = 0; i < G_N_ELEMENTS(fallback); i++)
				if (g_str_equal(classification, fallback[i].role) && code_matches(actual, fallback[i].code))
					return TRUE;
		}
		id = parent;
	}
	return FALSE;
}

static gint64
mapped_account(Books *books, const gchar *classification)
{
	gpointer value;
	if (books->defaults != NULL && g_hash_table_lookup_extended(books->defaults, classification, NULL, &value))
		return (gint64)GPOINTER_TO_SIZE(value);
	return 0;
}

static gboolean
is_cash_like(Books *books, VentureEntity *account)
{
	gboolean equivalent = FALSE;
	g_object_get(account, "cash-equivalent", &equivalent, NULL);
	return equivalent || control_account(books, venture_entity_get_id(account), "cash");
}

static const gchar *
cash_flow_section(Books *books, VentureEntity *account)
{
	g_autofree gchar *cls = NULL;
	VentureAccountKind kind;
	g_object_get(account, "cash-flow-class", &cls, NULL);
	if (cls != NULL && cls[0] != '\0')
		return g_intern_string(cls);
	if (is_profit_account(account))
		return "operating";
	if (control_account(books, venture_entity_get_id(account), "receivables") ||
		control_account(books, venture_entity_get_id(account), "payables") ||
		control_account(books, venture_entity_get_id(account), "inventory") ||
		control_account(books, venture_entity_get_id(account), "tax"))
		return "operating";
	kind = account_kind(account);
	if (kind == VENTURE_ACCOUNT_KIND_ASSET)
		return "investing";
	return "financing";
}

static VentureReportResult *
cash_flow(Books *books, VentureDateRange *period, GError **error)
{
	static const gchar *const keys[] = { "receivables", "payables", "inventory", "tax_payable" };
	static const gchar *const codes[] = { "receivables", "payables", "inventory", "tax" };
	static const gchar *const labels[] = { "Change in receivables", "Change in payables", "Change in inventory", "Change in tax payable" };
	g_autoptr(VentureReportResult) r = venture_report_result_new("Cash flow (indirect)", period);
	guint c, i;
	statement_columns(r);
	for (c = 0; c < books->currencies->len; c++)
	{
		const gchar *currency = g_ptr_array_index(books->currencies, c);
		g_autoptr(VentureMoney) start = venture_money_new_zero(currency);
		g_autoptr(VentureMoney) end = venture_money_new_zero(currency);
		g_autoptr(VentureMoney) net = venture_money_new_zero(currency);
		g_autoptr(VentureMoney) adjustments = venture_money_new_zero(currency);
		g_autoptr(VentureMoney) operating = venture_money_new_zero(currency);
		g_autoptr(VentureMoney) investing = venture_money_new_zero(currency);
		g_autoptr(VentureMoney) financing = venture_money_new_zero(currency);
		g_autoptr(VentureMoney) movement = NULL, calculated = NULL, difference = NULL;
		g_autoptr(GPtrArray) controls = g_ptr_array_new_with_free_func((GDestroyNotify)venture_money_free);
		guint k;
		for (k = 0; k < G_N_ELEMENTS(keys); k++)
			g_ptr_array_add(controls, venture_money_new_zero(currency));
		for (i = 0; i < books->accounts->len; i++)
		{
			VentureEntity *a = g_ptr_array_index(books->accounts, i);
			gint64 id = venture_entity_get_id(a);
			g_autoptr(VentureMoney) opening = NULL, debits = NULL, credits = NULL, closing = NULL;
			g_autoptr(VentureMoney) change = NULL;
			if (!measure(books, id, currency, FALSE, &opening, &debits, &credits, &closing, error))
				return NULL;
			change = venture_money_subtract(credits, debits, error);
			if (change == NULL)
				return NULL;
			if (is_cash_like(books, a))
			{
				if (!add(&start, opening, FALSE, error) || !add(&end, closing, FALSE, error))
					return NULL;
			}
			else if (is_profit_account(a))
			{
				if (!add(&net, change, FALSE, error))
					return NULL;
				if (!add(&operating, change, FALSE, error))
					return NULL;
			}
			else
			{
				const gchar *section = cash_flow_section(books, a);
				if (!add(&adjustments, change, FALSE, error))
					return NULL;
				if (g_strcmp0(section, "investing") == 0)
				{
					if (!add(&investing, change, FALSE, error))
						return NULL;
				}
				else if (g_strcmp0(section, "financing") == 0)
				{
					if (!add(&financing, change, FALSE, error))
						return NULL;
				}
				else if (!add(&operating, change, FALSE, error))
					return NULL;
				for (k = 0; k < G_N_ELEMENTS(keys); k++)
					if (control_account(books, id, codes[k]))
						break;
				if (k < G_N_ELEMENTS(keys))
				{
					VentureMoney *sum = g_ptr_array_index(controls, k);
					VentureMoney *next = venture_money_add(sum, change, error);
					if (next == NULL)
						return NULL;
					venture_money_free(sum);
					g_ptr_array_index(controls, k) = next;
				}
				/* Direct account adjustments expose the evidence, including
				 * investing/financing balances instead of a balancing plug. */
				account_row(r, a, currency);
				venture_report_result_set_money(r, "current", change);
			}
		}
		summary_row(r, "net_income", "Net income", net);
		for (k = 0; k < G_N_ELEMENTS(keys); k++)
			summary_row(r, keys[k], labels[k], g_ptr_array_index(controls, k));
		summary_row(r, "operating", "Operating cash flow", operating);
		summary_row(r, "investing", "Investing cash flow", investing);
		summary_row(r, "financing", "Financing cash flow", financing);
		summary_row(r, "adjustments", "Total non-cash balance movements", adjustments);
		calculated = venture_money_add(net, adjustments, error);
		movement = venture_money_subtract(end, start, error);
		if (calculated == NULL || movement == NULL)
			return NULL;
		difference = venture_money_subtract(calculated, movement, error);
		if (difference == NULL)
			return NULL;
		if (difference->amount != 0)
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_BALANCE, "Indirect cash flow does not tie to Cash");
			return NULL;
		}
		summary_row(r, "cash_start", "Cash at start", start);
		summary_row(r, "cash_end", "Cash at end", end);
		summary_row(r, "cash_movement", "Net cash movement", calculated);
		summary_row(r, "difference", "Difference from Cash movement", difference);
	}
	venture_report_result_append_note(r, "Indirect method: net income plus credit-minus-debit movements in every non-cash balance-sheet account. Cash includes accounts marked cash-equivalent. Movements are classified operating, investing or financing from cash-flow-class or the account class. Control lines remain subtotals, not additional flows.");
	return g_steal_pointer(&r);
}

static VentureReportResult *
general_ledger(Books *books, VentureDateRange *period, gint64 requested, GError **error)
{
	g_autoptr(VentureReportResult) r = venture_report_result_new("General ledger", period);
	guint c, i, j;
	statement_columns(r);
	text_column(r, "journal_id", "Journal ID");
	text_column(r, "date", "Accounting date");
	text_column(r, "source_type", "Source type");
	text_column(r, "source_id", "Source ID");
	money_column(r, "debits", "Debit");
	money_column(r, "credits", "Credit");
	if (requested != 0 && find_account(books, requested) == NULL)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Account does not belong to this organization");
		return NULL;
	}
	for (c = 0; c < books->currencies->len; c++)
	{
		const gchar *currency = g_ptr_array_index(books->currencies, c);
		for (i = 0; i < books->accounts->len; i++)
		{
			VentureEntity *a = g_ptr_array_index(books->accounts, i);
			gint64 id = venture_entity_get_id(a);
			g_autoptr(VentureMoney) running = NULL, debits = NULL, credits = NULL, closing = NULL;
			g_autoptr(VentureMoney) zero = venture_money_new_zero(currency);
			if (requested != 0 && requested != id)
				continue;
			if (!measure(books, id, currency, FALSE, &running, &debits, &credits, &closing, error))
				return NULL;
			for (j = 0; j < books->entries->len; j++)
			{
				Evidence *e = g_ptr_array_index(books->entries, j);
				g_autofree gchar *date = NULL;
				if (e->account_id != id || g_strcmp0(currency, e->amount->currency) != 0 ||
					g_date_time_compare(e->date, books->start) < 0)
					continue;
				if (!add(&running, e->amount, e->side == VENTURE_LEDGER_SIDE_CREDIT, error))
					return NULL;
				account_row(r, a, currency);
				set_id(r, "journal_id", e->journal_id);
				set_id(r, "source_id", e->source_id);
				venture_report_result_set_text(r, "source_type", e->source_type);
				date = venture_time_to_string(e->date);
				venture_report_result_set_text(r, "date", date);
				venture_report_result_set_money(r, "debits", e->side == VENTURE_LEDGER_SIDE_DEBIT ? e->amount : zero);
				venture_report_result_set_money(r, "credits", e->side == VENTURE_LEDGER_SIDE_CREDIT ? e->amount : zero);
				venture_report_result_set_money(r, "current", running);
			}
		}
	}
	venture_report_result_append_note(r, "Running balances are debit minus credit, ordered by accounting date, journal ID and line ID within each account and currency.");
	return g_steal_pointer(&r);
}

typedef struct
{
	gchar *key;
	gchar *type;
	gint64 id;
	VentureMoney *ledger;
	VentureMoney *operational;
} Source;

static void
source_free(gpointer data)
{
	Source *s = data;
	g_free(s->key);
	g_free(s->type);
	venture_money_free(s->ledger);
	venture_money_free(s->operational);
	g_free(s);
}

static Source *
source_get(GPtrArray *sources, const gchar *type, gint64 id, const gchar *currency)
{
	Source *s;
	guint i;
	for (i = 0; i < sources->len; i++)
	{
		s = g_ptr_array_index(sources, i);
		if (s->id == id && g_strcmp0(s->type, type) == 0 && g_strcmp0(s->ledger->currency, currency) == 0)
			return s;
	}
	s = g_new0(Source, 1);
	s->id = id;
	s->type = g_strdup(type);
	s->key = g_strdup_printf("%s:%" G_GINT64_FORMAT, type, id);
	s->ledger = venture_money_new_zero(currency);
	s->operational = venture_money_new_zero(currency);
	g_ptr_array_add(sources, s);
	return s;
}

static gint
source_compare(gconstpointer a, gconstpointer b)
{
	const Source *sa = *(Source * const *)a;
	const Source *sb = *(Source * const *)b;
	gint compared = g_strcmp0(sa->ledger->currency, sb->ledger->currency);
	return compared != 0 ? compared : g_strcmp0(sa->key, sb->key);
}

static VentureReportResult *
reconciliation(Books *books, VentureDatabase *db, gint64 org,
	VentureDateRange *period, JsonObject *options, GError **error)
{
	g_autoptr(GPtrArray) sources = g_ptr_array_new_with_free_func(source_free);
	g_autoptr(VentureReportResult) r = venture_report_result_new("P&L reconciliation", period);
	const gchar *currency = venture_json_object_get_string(options, "currency", NULL);
	guint i, t;
	statement_columns(r);
	text_column(r, "source_type", "Source type");
	text_column(r, "source_id", "Source ID");
	money_column(r, "ledger", "Ledger profit contribution");
	money_column(r, "operational", "Sales/expenses profit contribution");
	money_column(r, "difference", "Difference");
	for (i = 0; i < books->entries->len; i++)
	{
		Evidence *e = g_ptr_array_index(books->entries, i);
		VentureEntity *a = find_account(books, e->account_id);
		Source *s;
		if (a == NULL || !is_profit_account(a) || g_date_time_compare(e->date, books->start) < 0)
			continue;
		s = source_get(sources, e->source_type, e->source_id, e->amount->currency);
		if (!add(&s->ledger, e->amount, e->side == VENTURE_LEDGER_SIDE_DEBIT, error))
			return NULL;
	}
	/* Operational documents are read only for reconciliation. They never
	 * supply the statements' recognized revenue, costs or balances. */
	for (t = 0; t < 2; t++)
	{
		g_autoptr(VentureQuery) q = venture_query_new(t == 0 ? VENTURE_TYPE_SALE : VENTURE_TYPE_EXPENSE);
		g_autoptr(GPtrArray) rows = NULL;
		venture_query_set_organization(q, org);
		venture_query_set_limit(q, 0);
		if (!venture_period_report_scope(q, options, error))
			return NULL;
		rows = venture_database_find(db, q, error);
		if (rows == NULL)
			return NULL;
		for (i = 0; i < rows->len; i++)
		{
			VentureEntity *row = g_ptr_array_index(rows, i);
			g_autoptr(GDateTime) date = NULL;
			g_autoptr(VentureMoney) amount = NULL;
			Source *s;
			g_object_get(row, "occurred-at", &date, NULL);
			if (date == NULL || g_date_time_compare(date, books->start) < 0 || g_date_time_compare(date, books->end) >= 0)
				continue;
			if (t == 0)
				{
				amount = venture_sale_get_net(VENTURE_SALE(row), error);
				if (amount == NULL)
					return NULL;
			}
			else
				g_object_get(row, "amount", &amount, NULL);
			if (amount == NULL || (currency != NULL && g_strcmp0(amount->currency, currency) != 0))
				continue;
			s = source_get(sources, t == 0 ? "sale" : "expense", venture_entity_get_id(row), amount->currency);
			if (!add(&s->operational, amount, t != 0, error))
				return NULL;
		}
	}
	g_ptr_array_sort(sources, source_compare);
	for (i = 0; i < sources->len; i++)
	{
		Source *s = g_ptr_array_index(sources, i);
		g_autoptr(VentureMoney) difference = venture_money_subtract(s->ledger, s->operational, error);
		if (difference == NULL)
			return NULL;
		if (difference->amount == 0)
			continue;
		summary_row(r, s->key, s->key, difference);
		venture_report_result_set_text(r, "source_type", s->type);
		set_id(r, "source_id", s->id);
		venture_report_result_set_money(r, "ledger", s->ledger);
		venture_report_result_set_money(r, "operational", s->operational);
		venture_report_result_set_money(r, "difference", difference);
	}
	venture_report_result_append_note(r, "Differences by source document: posted income less posted expenses versus the operational sale net or full expense amount, in the same date range and currency. Invoice accruals, cash projections, manual journals and deleted documents can differ. This report does not replace the ledger.");
	return g_steal_pointer(&r);
}

static const gchar *
row_text(VentureReportResult *r, guint row, const gchar *key)
{
	const GValue *v = venture_report_result_get_cell(r, row, key);
	return v != NULL && G_VALUE_HOLDS_STRING(v) ? g_value_get_string(v) : NULL;
}

static const VentureMoney *
row_money(VentureReportResult *r, guint row, const gchar *key)
{
	const GValue *v = venture_report_result_get_cell(r, row, key);
	return v != NULL && G_VALUE_HOLDS(v, VENTURE_TYPE_MONEY) ? g_value_get_boxed(v) : NULL;
}

static void
include_prior_sources(VentureReportResult *current, VentureReportResult *prior)
{
	g_auto(GStrv) columns = venture_report_result_get_column_keys(prior);
	guint i, j, k;
	for (i = 0; i < venture_report_result_get_row_count(prior); i++)
	{
		const gchar *currency = row_text(prior, i, "currency");
		const gchar *key = row_text(prior, i, "key");
		g_autoptr(VentureMoney) zero = venture_money_new_zero(currency);
		for (j = 0; j < venture_report_result_get_row_count(current); j++)
			if (g_strcmp0(key, row_text(current, j, "key")) == 0 &&
				g_strcmp0(currency, row_text(current, j, "currency")) == 0)
				break;
		if (j != venture_report_result_get_row_count(current))
			continue;
		venture_report_result_begin_row(current);
		for (k = 0; columns[k] != NULL; k++)
		{
			const GValue *value = venture_report_result_get_cell(prior, i, columns[k]);
			if (value == NULL)
				continue;
			if (G_VALUE_HOLDS(value, VENTURE_TYPE_MONEY))
				venture_report_result_set_money(current, columns[k], zero);
			else if (G_VALUE_HOLDS_STRING(value))
				venture_report_result_set_text(current, columns[k], g_value_get_string(value));
		}
	}
}

static VentureReportResult *
with_comparative(VentureReportResult *current, VentureReportResult *prior,
	VentureDateRange *period, const gchar *value_key, const gchar *prior_key, GError **error)
{
	g_autoptr(VentureReportResult) r = venture_report_result_new(venture_report_result_get_title(current), period);
	g_autoptr(JsonNode) json = venture_report_result_to_json(current);
	JsonObject *object = json_node_get_object(json);
	JsonArray *columns = json_object_get_array_member(object, "columns");
	guint i, j, k;
	GPtrArray *metrics = venture_report_result_get_metrics(current);
	for (i = 0; i < metrics->len; i++)
		venture_report_result_add_metric(r, venture_metric_copy(g_ptr_array_index(metrics, i)));
	for (i = 0; i < json_array_get_length(columns); i++)
	{
		JsonObject *column = json_array_get_object_element(columns, i);
		const gchar *key = json_object_get_string_member(column, "key");
		const gchar *label = json_object_get_string_member(column, "label");
		const gchar *kind = json_object_get_string_member(column, "kind");
		venture_report_result_add_column(r, key, label,
			g_str_equal(kind, "money") ? VENTURE_REPORT_COLUMN_MONEY : VENTURE_REPORT_COLUMN_TEXT);
	}
	money_column(r, "prior", "Prior");
	money_column(r, "delta", "Delta");
	for (j = 0; j < venture_report_result_get_row_count(current); j++)
	{
		const gchar *currency = row_text(current, j, "currency");
		const gchar *key = row_text(current, j, "key");
		const VentureMoney *value = row_money(current, j, value_key);
		const VentureMoney *previous = NULL;
		g_autoptr(VentureMoney) zero = venture_money_new_zero(currency);
		g_autoptr(VentureMoney) delta = NULL;
		for (k = 0; k < venture_report_result_get_row_count(prior); k++)
			if (g_strcmp0(key, row_text(prior, k, "key")) == 0 &&
				g_strcmp0(currency, row_text(prior, k, "currency")) == 0)
				previous = row_money(prior, k, prior_key);
		if (previous == NULL)
			previous = zero;
		delta = venture_money_subtract(value != NULL ? value : zero, previous, error);
		if (delta == NULL)
			return NULL;
		venture_report_result_begin_row(r);
		for (i = 0; i < json_array_get_length(columns); i++)
		{
			JsonObject *column = json_array_get_object_element(columns, i);
			const gchar *column_key = json_object_get_string_member(column, "key");
			const GValue *v = venture_report_result_get_cell(current, j, column_key);
			if (v == NULL)
				continue;
			if (G_VALUE_HOLDS(v, VENTURE_TYPE_MONEY))
				venture_report_result_set_money(r, column_key, g_value_get_boxed(v));
			else if (G_VALUE_HOLDS_STRING(v))
				venture_report_result_set_text(r, column_key, g_value_get_string(v));
		}
		venture_report_result_set_money(r, "prior", previous);
		venture_report_result_set_money(r, "delta", delta);
	}
	if (json_object_has_member(object, "note"))
		venture_report_result_set_note(r, json_object_get_string_member(object, "note"));
	return g_steal_pointer(&r);
}

static gint64
code_account(Books *books, const gchar *code)
{
	const gchar *role = g_str_equal(code, "1000") ? "cash" :
		(g_str_equal(code, "1100") ? "receivables" :
		(g_str_equal(code, "2000") ? "payables" :
		(g_str_equal(code, "2100") ? "tax" :
		(g_str_equal(code, "4000") ? "income" :
		(g_str_equal(code, "6900") || g_str_equal(code, "5000") ? "expense" : NULL)))));
	guint i;
	if (role != NULL)
	{
		gint64 mapped = mapped_account(books, role);
		if (mapped != 0)
			return mapped;
	}
	for (i = 0; i < books->accounts->len; i++)
	{
		VentureEntity *account = g_ptr_array_index(books->accounts, i);
		g_autofree gchar *actual = NULL;
		g_object_get(account, "code", &actual, NULL);
		if (code_matches(actual, code))
			return venture_entity_get_id(account);
	}
	return 0;
}

static gboolean
push_cash_entry(Books *books, gint64 account_id, GDateTime *date, const gchar *source_type,
	gint64 source_id, VentureLedgerSide side, const VentureMoney *amount, GError **error)
{
	Evidence *e;
	if (amount == NULL || venture_money_is_zero(amount))
		return TRUE;
	if (account_id == 0)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"Cash-basis conversion is missing a control account");
		return FALSE;
	}
	e = g_new0(Evidence, 1);
	e->account_id = account_id;
	e->source_id = source_id;
	e->source_type = g_strdup(source_type);
	e->date = g_date_time_ref(date);
	e->amount = venture_money_copy((VentureMoney *)amount);
	e->side = side;
	g_ptr_array_add(books->entries, e);
	return TRUE;
}


static gboolean
add_money_local(VentureMoney **sum, const VentureMoney *value, GError **error)
{
	VentureMoney *next;
	if (*sum == NULL)
	{
		*sum = venture_money_copy((VentureMoney *)value);
		return TRUE;
	}
	next = venture_money_add(*sum, value, error);
	if (next == NULL)
		return FALSE;
	venture_money_free(*sum);
	*sum = next;
	return TRUE;
}

static gint64
bill_expense_account(Books *books, VentureDatabase *db, VentureEntity *allocation, GError **error)
{
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) lines = NULL;
	gint64 bill_id = 0;
	guint i;
	g_object_get(allocation, "bill-id", &bill_id, NULL);
	if (bill_id > 0)
	{
		query = venture_query_new(VENTURE_TYPE_VENDOR_BILL_LINE);
		venture_query_set_limit(query, 0);
		if (!venture_query_add_filter_int(query, "bill-id", VENTURE_FILTER_OP_EQ, bill_id, error))
			return -1;
		lines = venture_database_find(db, query, error);
		if (lines == NULL)
			return -1;
		for (i = 0; i < lines->len; i++)
		{
			gint64 account_id = 0;
			g_object_get(g_ptr_array_index(lines, i), "account-id", &account_id, NULL);
			if (account_id > 0)
				return account_id;
		}
	}
	{
		gint64 fallback = code_account(books, "6900");
		if (fallback == 0)
			fallback = code_account(books, "5000");
		if (fallback == 0)
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
				"Cash-basis bill payments need an expense account");
			return -1;
		}
		return fallback;
	}
}

static gboolean
apply_cash_basis(Books *books, VentureDatabase *db, gint64 org, GError **error)
{
	g_autoptr(GPtrArray) kept = g_ptr_array_new();
	g_autoptr(GPtrArray) allocations = NULL;
	g_autoptr(VentureQuery) query = NULL;
	gint64 income = code_account(books, "4000");
	gint64 tax = code_account(books, "2100");
	gint64 ar = code_account(books, "1100");
	gint64 ap = code_account(books, "2000");
	guint i;
	for (i = 0; i < books->entries->len; i++)
	{
		Evidence *e = g_ptr_array_index(books->entries, i);
		if (g_strcmp0(e->source_type, "invoice_event") == 0 ||
			g_strcmp0(e->source_type, "vendor_bill_event") == 0)
		{
			evidence_free(e);
			continue;
		}
		g_ptr_array_add(kept, e);
	}
	g_ptr_array_set_free_func(books->entries, NULL);
	g_ptr_array_set_size(books->entries, 0);
	g_ptr_array_set_free_func(books->entries, evidence_free);
	for (i = 0; i < kept->len; i++)
		g_ptr_array_add(books->entries, g_ptr_array_index(kept, i));
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "payment_allocation") == G_TYPE_INVALID)
		return TRUE;
	query = venture_query_new(VENTURE_TYPE_PAYMENT_ALLOCATION);
	venture_query_set_organization(query, org);
	venture_query_set_limit(query, 0);
	allocations = venture_database_find(db, query, error);
	if (allocations == NULL)
		return FALSE;
	for (i = 0; i < allocations->len; i++)
	{
		VentureEntity *allocation = g_ptr_array_index(allocations, i);
		g_autoptr(GDateTime) date = NULL;
		g_autoptr(VentureMoney) amount = NULL;
		g_autoptr(VentureQuery) events = NULL;
		g_autoptr(GPtrArray) found = NULL;
		g_autoptr(VentureMoney) levy = NULL;
		g_autoptr(VentureMoney) total = NULL;
		g_autoptr(VentureMoney) income_share = NULL;
		g_autoptr(VentureMoney) tax_share = NULL;
		gint64 invoice_id = 0;
		guint j;
		gint64 payment_id = 0;
		g_object_get(allocation, "date", &date, "amount", &amount, "invoice-id", &invoice_id,
			"payment-id", &payment_id, NULL);
		if (payment_id <= 0)
			continue;
		if (date == NULL || g_date_time_compare(date, books->end) >= 0)
			continue;
		events = venture_query_new(VENTURE_TYPE_INVOICE_EVENT);
		venture_query_set_limit(events, 0);
		if (!venture_query_add_filter_int(events, "invoice-id", VENTURE_FILTER_OP_EQ, invoice_id, NULL))
			return FALSE;
		found = venture_database_find(db, events, error);
		if (found == NULL)
			return FALSE;
		for (j = 0; j < found->len; j++)
		{
			g_autofree gchar *kind = NULL;
			g_object_get(g_ptr_array_index(found, j), "kind", &kind, "tax-amount", &levy, "amount", &total, NULL);
			if (g_strcmp0(kind, "issue") == 0)
				break;
			g_clear_pointer(&levy, venture_money_free);
			g_clear_pointer(&total, venture_money_free);
		}
		if (total == NULL || venture_money_is_zero(total) || amount == NULL)
			continue;
		if (levy != NULL && !venture_money_is_zero(levy))
			tax_share = venture_money_multiply_rational(amount, venture_money_get_amount(levy),
				venture_money_get_amount(total), error);
		else
			tax_share = venture_money_new_zero(venture_money_get_currency(amount));
		if (tax_share == NULL)
			return FALSE;
		income_share = venture_money_subtract(amount, tax_share, error);
		if (income_share == NULL)
			return FALSE;
		if (!push_cash_entry(books, income, date, "payment_allocation",
			venture_entity_get_id(allocation), VENTURE_LEDGER_SIDE_CREDIT, income_share, error) ||
			!push_cash_entry(books, tax, date, "payment_allocation",
			venture_entity_get_id(allocation), VENTURE_LEDGER_SIDE_CREDIT, tax_share, error) ||
			!push_cash_entry(books, ar, date, "payment_allocation",
			venture_entity_get_id(allocation), VENTURE_LEDGER_SIDE_DEBIT, amount, error))
			return FALSE;
	}
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "bill_payment_allocation") != G_TYPE_INVALID)
	{
		g_autoptr(VentureQuery) bills = venture_query_new(VENTURE_TYPE_BILL_PAYMENT_ALLOCATION);
		g_autoptr(GPtrArray) bill_rows = NULL;
		venture_query_set_organization(bills, org);
		venture_query_set_limit(bills, 0);
		bill_rows = venture_database_find(db, bills, error);
		if (bill_rows == NULL)
			return FALSE;
		for (i = 0; i < bill_rows->len; i++)
		{
			VentureEntity *row = g_ptr_array_index(bill_rows, i);
			g_autoptr(GDateTime) date = NULL;
			g_autoptr(VentureMoney) amount = NULL;
			gint64 bill_expense;
			g_object_get(row, "date", &date, "amount", &amount, NULL);
			if (date == NULL || g_date_time_compare(date, books->end) >= 0)
				continue;
			bill_expense = bill_expense_account(books, db, row, error);
			if (bill_expense < 0)
				return FALSE;
			{
				g_autoptr(VentureQuery) lines_q = NULL;
				g_autoptr(GPtrArray) lines = NULL;
				g_autoptr(VentureMoney) levy = NULL;
				g_autoptr(VentureMoney) total = NULL;
				g_autoptr(VentureMoney) tax_share = NULL;
				g_autoptr(VentureMoney) expense_share = NULL;
				gint64 bill_id = 0;
				gint64 recoverable = code_account(books, "1300");
				guint j;
				g_object_get(row, "bill-id", &bill_id, NULL);
				levy = venture_money_new_zero(venture_money_get_currency(amount));
				total = venture_money_copy(amount);
				if (bill_id > 0)
				{
					lines_q = venture_query_new(VENTURE_TYPE_VENDOR_BILL_LINE);
					venture_query_set_limit(lines_q, 0);
					if (venture_query_add_filter_int(lines_q, "bill-id", VENTURE_FILTER_OP_EQ, bill_id, NULL))
						lines = venture_database_find(db, lines_q, error);
					if (lines == NULL)
						return FALSE;
					g_clear_pointer(&total, venture_money_free);
					total = venture_money_new_zero(venture_money_get_currency(amount));
					for (j = 0; j < lines->len; j++)
					{
						VentureEntity *line = g_ptr_array_index(lines, j);
						g_autoptr(VentureMoney) line_tax = NULL;
						g_autoptr(VentureMoney) line_total = venture_vendor_bill_line_get_amount(
							VENTURE_VENDOR_BILL_LINE(line), error);
						gint64 tax_code_id = 0;
						gboolean rec = FALSE;
						if (line_total == NULL)
							return FALSE;
						if (!add_money_local(&total, line_total, error))
							return FALSE;
						g_object_get(line, "tax-amount", &line_tax, "tax-code-id", &tax_code_id, NULL);
						if (tax_code_id > 0)
						{
							g_autoptr(VentureEntity) code = venture_database_get(db,
								VENTURE_TYPE_TAX_CODE, tax_code_id, error);
							if (code == NULL)
								return FALSE;
							g_object_get(code, "recoverable", &rec, NULL);
						}
						if (rec && line_tax != NULL && !add_money_local(&levy, line_tax, error))
							return FALSE;
					}
				}
				if (levy != NULL && total != NULL && !venture_money_is_zero(levy) &&
					!venture_money_is_zero(total) && recoverable != 0)
					tax_share = venture_money_multiply_rational(amount, venture_money_get_amount(levy),
						venture_money_get_amount(total), error);
				else
					tax_share = venture_money_new_zero(venture_money_get_currency(amount));
				if (tax_share == NULL)
					return FALSE;
				expense_share = venture_money_subtract(amount, tax_share, error);
				if (expense_share == NULL)
					return FALSE;
				if (!push_cash_entry(books, bill_expense, date, "bill_payment_allocation",
					venture_entity_get_id(row), VENTURE_LEDGER_SIDE_DEBIT, expense_share, error) ||
					!push_cash_entry(books, recoverable != 0 ? recoverable : bill_expense, date,
						"bill_payment_allocation", venture_entity_get_id(row),
						VENTURE_LEDGER_SIDE_DEBIT, tax_share, error) ||
					!push_cash_entry(books, ap, date, "bill_payment_allocation",
					venture_entity_get_id(row), VENTURE_LEDGER_SIDE_CREDIT, amount, error))
					return FALSE;
			}
		}
	}
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "refund") != G_TYPE_INVALID)
	{
		g_autoptr(VentureQuery) refunds = venture_query_new(VENTURE_TYPE_REFUND);
		g_autoptr(GPtrArray) refund_rows = NULL;
		venture_query_set_organization(refunds, org);
		venture_query_set_limit(refunds, 0);
		refund_rows = venture_database_find(db, refunds, error);
		if (refund_rows == NULL)
			return FALSE;
		for (i = 0; i < refund_rows->len; i++)
		{
			VentureEntity *refund = g_ptr_array_index(refund_rows, i);
			g_autoptr(GDateTime) date = NULL;
			g_autoptr(VentureMoney) amount = NULL;
			g_autoptr(VentureEntity) allocation = NULL;
			g_autoptr(VentureQuery) events = NULL;
			g_autoptr(GPtrArray) found = NULL;
			g_autoptr(VentureMoney) levy = NULL;
			g_autoptr(VentureMoney) total = NULL;
			g_autoptr(VentureMoney) tax_share = NULL;
			g_autoptr(VentureMoney) income_share = NULL;
			gint64 allocation_id = 0, invoice_id = 0, payment_id = 0;
			guint j;
			g_object_get(refund, "date", &date, "amount", &amount, "allocation-id", &allocation_id, NULL);
			if (date == NULL || g_date_time_compare(date, books->end) >= 0 || allocation_id <= 0)
				continue;
			allocation = venture_database_get(db, VENTURE_TYPE_PAYMENT_ALLOCATION, allocation_id, error);
			if (allocation == NULL)
				return FALSE;
			g_object_get(allocation, "invoice-id", &invoice_id, "payment-id", &payment_id, NULL);
			if (payment_id <= 0 || invoice_id <= 0)
				continue;
			events = venture_query_new(VENTURE_TYPE_INVOICE_EVENT);
			venture_query_set_limit(events, 0);
			if (!venture_query_add_filter_int(events, "invoice-id", VENTURE_FILTER_OP_EQ, invoice_id, NULL))
				return FALSE;
			found = venture_database_find(db, events, error);
			if (found == NULL)
				return FALSE;
			for (j = 0; j < found->len; j++)
			{
				g_autofree gchar *kind = NULL;
				g_object_get(g_ptr_array_index(found, j), "kind", &kind, "tax-amount", &levy, "amount", &total, NULL);
				if (g_strcmp0(kind, "issue") == 0)
					break;
				g_clear_pointer(&levy, venture_money_free);
				g_clear_pointer(&total, venture_money_free);
			}
			if (total == NULL || venture_money_is_zero(total) || amount == NULL)
				continue;
			if (levy != NULL && !venture_money_is_zero(levy))
				tax_share = venture_money_multiply_rational(amount, venture_money_get_amount(levy),
					venture_money_get_amount(total), error);
			else
				tax_share = venture_money_new_zero(venture_money_get_currency(amount));
			if (tax_share == NULL)
				return FALSE;
			income_share = venture_money_subtract(amount, tax_share, error);
			if (income_share == NULL)
				return FALSE;
			if (!push_cash_entry(books, income, date, "refund",
				venture_entity_get_id(refund), VENTURE_LEDGER_SIDE_DEBIT, income_share, error) ||
				!push_cash_entry(books, tax, date, "refund",
				venture_entity_get_id(refund), VENTURE_LEDGER_SIDE_DEBIT, tax_share, error) ||
				!push_cash_entry(books, ar, date, "refund",
				venture_entity_get_id(refund), VENTURE_LEDGER_SIDE_CREDIT, amount, error))
				return FALSE;
		}
	}
	return TRUE;
}

static VentureReportResult *
generate_one(const gchar *name, Books *books, VentureDatabase *db, gint64 org,
	VentureDateRange *period, JsonObject *options, GError **error)
{
	if (g_str_equal(name, "account_balances"))
		return balance_rows(books, period, TRUE, error);
	if (g_str_equal(name, "general_ledger"))
		return general_ledger(books, period, venture_json_object_get_int(options, "account_id", 0), error);
	if (g_str_equal(name, "cash_flow"))
		return cash_flow(books, period, error);
	if (g_str_equal(name, "pnl_reconciliation"))
		return reconciliation(books, db, org, period, options, error);
	return financial_statement(books, period, g_str_equal(name, "balance_sheet"), error);
}

static VentureReportResult *
generate(const gchar *name, VentureContext *context, VentureDateRange *period,
	JsonObject *options, GError **error)
{
	VentureDatabase *db = venture_context_get_database(context);
	g_autoptr(JsonObject) defaults = json_object_new();
	g_autoptr(JsonObject) previous_options = json_object_new();
	g_autoptr(GDateTime) as_of = NULL;
	g_autoptr(Books) books = NULL, previous = NULL;
	g_autoptr(VentureDateRange) prior_period = NULL;
	g_autoptr(VentureReportResult) result = NULL, prior = NULL;
	const gchar *currency;
	const gchar *compare;
	gint64 org;
	guint i;
	if (options == NULL)
		options = defaults;
	org = venture_json_object_get_int(options, "organization_id", 0);
	if (org == 0)
		org = venture_context_get_default_organization_id(context);
	currency = venture_json_object_get_string(options, "currency", NULL);
	compare = venture_json_object_get_string(options, "compare_to", NULL);
	if (json_object_has_member(options, "as_of"))
	{
		as_of = venture_period_report_as_of(options, error);
		if (as_of == NULL)
			return NULL;
	}
	if (compare != NULL)
	{
		prior_period = venture_context_parse_period(context, compare, error);
		if (prior_period == NULL)
			return NULL;
		if (g_date_time_compare(venture_date_range_get_end(prior_period), venture_date_range_get_start(period)) > 0)
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT, "compare_to must end before the current period starts");
			return NULL;
		}
	}
	if (!venture_database_begin(db, error))
		return NULL;
	books = read_books(db, org, currency, period, as_of,
		venture_json_object_get_string(options, "dimension", NULL), error);
	if (books == NULL)
		goto fail;
	{
		const gchar *basis = venture_json_object_get_string(options, "basis", "accrual");
		if (basis != NULL && g_strcmp0(basis, "accrual") != 0 && g_strcmp0(basis, "cash") != 0)
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
				"basis must be cash or accrual");
			goto fail;
		}
		if (g_strcmp0(basis, "cash") == 0 && !apply_cash_basis(books, db, org, error))
			goto fail;
		if (g_strcmp0(basis, "cash") == 0)
			json_object_set_string_member(previous_options, "basis", "cash");
	}
	if (prior_period != NULL)
	{
		previous = read_books(db, org, currency, prior_period, NULL,
			venture_json_object_get_string(options, "dimension", NULL), error);
		if (previous == NULL)
			goto fail;
		if (g_strcmp0(venture_json_object_get_string(options, "basis", "accrual"), "cash") == 0 &&
			!apply_cash_basis(previous, db, org, error))
			goto fail;
		for (i = 0; i < previous->currencies->len; i++)
			add_currency(books, g_ptr_array_index(previous->currencies, i));
		for (i = 0; i < books->currencies->len; i++)
			add_currency(previous, g_ptr_array_index(books->currencies, i));
		g_ptr_array_sort(books->currencies, compare_currency);
		g_ptr_array_sort(previous->currencies, compare_currency);
	}
	result = generate_one(name, books, db, org, period, options, error);
	if (result == NULL)
		goto fail;
	if (g_str_equal(name, "income_statement"))
	{
		g_autoptr(VentureReportResult) differences = reconciliation(books, db, org, period, options, error);
		if (differences == NULL)
			goto fail;
		if (venture_report_result_get_row_count(differences) > 0)
			venture_report_result_append_note(result, "Ledger income differs by source from the sales/expenses P&L; see pnl_reconciliation for the documents and amounts.");
	}
	if (prior_period != NULL)
	{
		g_autoptr(VentureReportResult) compared = NULL;
		gboolean balances = g_str_equal(name, "account_balances");
		gboolean ledger = g_str_equal(name, "general_ledger");
		if (currency != NULL)
			json_object_set_string_member(previous_options, "currency", currency);
		prior = ledger ? balance_rows(previous, prior_period, FALSE, error) :
			generate_one(name, previous, db, org, prior_period, previous_options, error);
		if (prior == NULL)
			goto fail;
		if (g_str_equal(name, "pnl_reconciliation"))
			include_prior_sources(result, prior);
		compared = with_comparative(result, prior, period, balances ? "closing" : "current",
			(balances || ledger) ? "closing" : "current", error);
		if (compared == NULL)
			goto fail;
		g_set_object(&result, compared);
		if (ledger)
			venture_report_result_append_note(result, "Prior is the account's prior-period closing balance; delta compares each current running balance with that closing balance.");
	}
	if (!venture_database_commit(db, error))
		return NULL;
	return g_steal_pointer(&result);
fail:
	venture_database_rollback(db);
	return NULL;
}

#define VENTURE_TYPE_STATEMENT_REPORT (venture_statement_report_get_type())
G_DECLARE_FINAL_TYPE(VentureStatementReport, venture_statement_report, VENTURE, STATEMENT_REPORT, VentureReport)
struct _VentureStatementReport { VentureReport parent_instance; };
G_DEFINE_TYPE(VentureStatementReport, venture_statement_report, VENTURE_TYPE_REPORT)

static VentureReportResult *
statement_generate(VentureReport *self, VentureContext *context,
	VentureDateRange *period, JsonObject *options, GError **error)
{
	return generate(venture_report_get_name(self), context, period, options, error);
}

static JsonNode *
statement_parameters(VentureReport *self)
{
	(void)self;
	return venture_json_parse(
		"{\"type\":\"object\",\"properties\":{"
		"\"organization_id\":{\"type\":\"integer\",\"description\":\"One exact legal entity; defaults to the context organization\"},"
		"\"currency\":{\"type\":\"string\",\"description\":\"Uppercase book currency; omit to report every currency separately\"},"
		"\"compare_to\":{\"type\":\"string\",\"description\":\"Prior period label, ending before this period starts\"},"
		"\"account_id\":{\"type\":\"integer\",\"description\":\"General ledger only: one account in this organization\"},"
		"\"basis\":{\"type\":\"string\",\"description\":\"accrual (default, posted recognition) or cash (receipts and payments)\"}}}", NULL);
}

static void
venture_statement_report_class_init(VentureStatementReportClass *klass)
{
	VentureReportClass *report_class = VENTURE_REPORT_CLASS(klass);
	report_class->generate = statement_generate;
	report_class->describe_parameters = statement_parameters;
}

static void
venture_statement_report_init(VentureStatementReport *self)
{
	(void)self;
}

void
venture_statements_register_reports(VentureReportRegistry *registry)
{
	static const struct { const gchar *name; const gchar *title; } reports[] = {
		{ "balance_sheet", "Balance sheet" },
		{ "income_statement", "Income statement" },
		{ "cash_flow", "Cash flow" },
		{ "general_ledger", "General ledger" },
		{ "account_balances", "Account balances" },
		{ "pnl_reconciliation", "P&L reconciliation" }
	};
	guint i;
	for (i = 0; i < G_N_ELEMENTS(reports); i++)
	{
		VentureReport *report = g_object_new(VENTURE_TYPE_STATEMENT_REPORT,
			"name", reports[i].name, "title", reports[i].title,
			"description", "Posted ledger evidence, per legal entity and currency, with date cutoffs and prior-period comparatives", NULL);
		g_object_set(report, "financial", TRUE, NULL);
		venture_report_registry_add(registry, report);
	}
}
