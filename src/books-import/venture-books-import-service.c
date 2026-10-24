/* SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A whole set of books moved over with its history: every contact, every
 * document since the source's first day, every payment, in date order,
 * through the services that own them. The day-one balances enter through an
 * ordinary cutover batch at the start date; everything after is replayed,
 * so the ledger is built by VENTURE rather than summarised into journals.
 *
 * The contract is the cutover's: preview writes no document, import is one
 * transaction, reconcile proves the ledger against the source's own
 * postings, activate makes the books the organization's, rollback unwinds
 * everything an inactive batch did.
 */
#include "venture.h"
#include <stdio.h>
#include <string.h>

struct _VentureBooksImportService
{
	GObject parent_instance;
	VentureDatabase *database;
	VentureEntity *writing;
};
G_DEFINE_FINAL_TYPE(VentureBooksImportService, venture_books_import_service, G_TYPE_OBJECT)

static void
get_property(GObject *object, guint id, GValue *value, GParamSpec *spec)
{
	if (id == 1)
		g_value_set_object(value, VENTURE_BOOKS_IMPORT_SERVICE(object)->database);
	else
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
}

static void
set_property(GObject *object, guint id, const GValue *value, GParamSpec *spec)
{
	VentureBooksImportService *self = VENTURE_BOOKS_IMPORT_SERVICE(object);
	if (id == 1)
	{
		self->database = g_value_get_object(value);
		if (self->database != NULL)
			g_object_add_weak_pointer(G_OBJECT(self->database), (gpointer *)&self->database);
	}
	else
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
}

static void
finalize(GObject *object)
{
	VentureBooksImportService *self = VENTURE_BOOKS_IMPORT_SERVICE(object);
	if (self->database != NULL)
		g_object_remove_weak_pointer(G_OBJECT(self->database), (gpointer *)&self->database);
	G_OBJECT_CLASS(venture_books_import_service_parent_class)->finalize(object);
}

static void
venture_books_import_service_class_init(VentureBooksImportServiceClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	object_class->get_property = get_property;
	object_class->set_property = set_property;
	object_class->finalize = finalize;
	g_object_class_install_property(object_class, 1,
		g_param_spec_object("database", "Database", "Owning database", VENTURE_TYPE_DATABASE,
			G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
}

static void
venture_books_import_service_init(VentureBooksImportService *self)
{
	(void)self;
}

VentureBooksImportService *
venture_books_import_service_get(VentureDatabase *database)
{
	VentureBooksImportService *self;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	self = g_object_get_data(G_OBJECT(database), "venture-books-import-service");
	if (self == NULL)
	{
		self = g_object_new(VENTURE_TYPE_BOOKS_IMPORT_SERVICE, "database", database, NULL);
		g_object_set_data_full(G_OBJECT(database), "venture-books-import-service", self, g_object_unref);
	}
	return self;
}

static gboolean
refuse(GError **error, const gchar *message)
{
	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "VentureBooksImportService: %s", message);
	return FALSE;
}

gboolean
venture_books_import_check_write(VentureDatabase *database, VentureEntity *record, gboolean removal, GError **error)
{
	const gchar *name;
	VentureBooksImportService *self;
	(void)removal;
	if (record == NULL || database == NULL)
		return TRUE;
	name = venture_entity_get_entity_name(record);
	if (g_strcmp0(name, "books_import") != 0 && g_strcmp0(name, "books_import_row") != 0)
		return TRUE;
	self = venture_books_import_service_get(database);
	if (self->writing == record)
	{
		self->writing = NULL;
		return TRUE;
	}
	return refuse(error, "books imports must be changed through VentureBooksImportService");
}

static gboolean
save_owned(VentureBooksImportService *self, VentureEntity *record, const VentureActor *actor, GError **error)
{
	gboolean ok;
	self->writing = record;
	ok = venture_database_save(self->database, record, actor, error);
	self->writing = NULL;
	return ok;
}

static JsonObject *
manifest_of(VentureEntity *batch, GError **error)
{
	g_autofree gchar *text = NULL;
	g_autoptr(JsonParser) parser = json_parser_new();
	g_object_get(batch, "manifest", &text, NULL);
	if (text == NULL || !json_parser_load_from_data(parser, text, -1, NULL) ||
		!JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser)))
	{
		refuse(error, "the batch manifest cannot be read");
		return NULL;
	}
	return json_object_ref(json_node_get_object(json_parser_get_root(parser)));
}

static gchar *
state_of(VentureEntity *batch)
{
	gchar *state = NULL;
	g_object_get(batch, "state", &state, NULL);
	return state;
}

static gboolean
append_report(VentureBooksImportService *self, VentureEntity *batch, const gchar *state, GString *block,
	const VentureActor *actor, GError **error)
{
	g_autofree gchar *report = NULL;
	g_autofree gchar *combined = NULL;
	g_object_get(batch, "report", &report, NULL);
	combined = g_strconcat(report != NULL ? report : "", block->str, NULL);
	g_object_set(batch, "report", combined, NULL);
	if (state != NULL)
		g_object_set(batch, "state", state, NULL);
	return save_owned(self, batch, actor, error);
}

static gchar *
stamp_now(void)
{
	g_autoptr(GDateTime) now = venture_time_now();
	return g_date_time_format_iso8601(now);
}

#include "venture-books-import-zoho.inc"

/* --- Rows ------------------------------------------------------------------------------ */

static gboolean
add_row(VentureBooksImportService *self, VentureEntity *batch, const gchar *object, const gchar *source_id,
	const gchar *status, const gchar *record_type, gint64 record_id, const gchar *period, gint64 cents,
	const gchar *currency, gboolean created, const gchar *exception, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureBooksImportRow) row = venture_books_import_row_new();
	g_object_set(row, "import-id", venture_entity_get_id(batch), "source-object", object, "source-id", source_id,
		"status", status, "record-type", record_type, "record-id", record_id, "period", period,
		"created", created, "exception", exception, NULL);
	if (currency != NULL)
	{
		g_autoptr(VentureMoney) amount = venture_money_new_for_currency(cents, currency);
		g_object_set(row, "amount", amount, NULL);
	}
	venture_entity_set_organization_id(VENTURE_ENTITY(row), venture_entity_get_organization_id(batch));
	return save_owned(self, VENTURE_ENTITY(row), actor, error);
}

static GPtrArray *
batch_rows(VentureBooksImportService *self, gint64 batch_id, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_BOOKS_IMPORT_ROW);
	venture_query_set_limit(query, 0);
	if (!venture_query_add_filter_int(query, "import-id", VENTURE_FILTER_OP_EQ, batch_id, error) ||
		!venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, error))
		return NULL;
	return venture_database_find(self->database, query, error);
}

static gchar *
row_string(VentureEntity *row, const gchar *field)
{
	gchar *value = NULL;
	g_object_get(row, field, &value, NULL);
	return value;
}

static gint64
row_int(VentureEntity *row, const gchar *field)
{
	gint64 value = 0;
	g_object_get(row, field, &value, NULL);
	return value;
}

/* A source id another live batch of the organization already imported.
 * Two batches replaying the same invoice would count it twice. */
static gint64
owned_elsewhere(VentureBooksImportService *self, gint64 org, gint64 batch_id, const gchar *object,
	const gchar *source_id, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_BOOKS_IMPORT_ROW);
	g_autoptr(GPtrArray) rows = NULL;
	guint i;
	venture_query_set_organization(query, org);
	venture_query_set_limit(query, 0);
	if (!venture_query_add_filter_string(query, "source-id", VENTURE_FILTER_OP_EQ, source_id, error))
		return -1;
	rows = venture_database_find(self->database, query, error);
	if (rows == NULL)
		return -1;
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(rows, i);
		g_autofree gchar *kind = row_string(row, "source-object");
		g_autofree gchar *status = row_string(row, "status");
		gint64 other = row_int(row, "import-id");
		if (other != batch_id && g_strcmp0(kind, object) == 0 &&
			(g_strcmp0(status, "imported") == 0 || g_strcmp0(status, "matched") == 0))
			return other;
	}
	return 0;
}

/* --- Accounts and parties ---------------------------------------------------------------- */

static VentureEntity *
account_with_code(VentureBooksImportService *self, gint64 org, const gchar *code, GError **error)
{
	g_autofree gchar *scoped = g_strdup_printf("%" G_GINT64_FORMAT ":%s", org, code);
	const gchar *codes[2];
	guint pass;
	codes[0] = code;
	codes[1] = scoped;
	for (pass = 0; pass < 2; pass++)
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ACCOUNT);
		g_autoptr(GPtrArray) accounts = NULL;
		venture_query_set_organization(query, org);
		if (!venture_query_add_filter_string(query, "code", VENTURE_FILTER_OP_EQ, codes[pass], error))
			return NULL;
		accounts = venture_database_find(self->database, query, error);
		if (accounts == NULL)
			return NULL;
		if (accounts->len > 0)
			return g_object_ref(g_ptr_array_index(accounts, 0));
	}
	return NULL;
}

/* The account a control role names, as the owning service resolves it:
 * the setup mapping, else the conventional code in the organization. */
static gint64
control_account(VentureBooksImportService *self, gint64 org, const gchar *role, const gchar *code, GError **error)
{
	g_autoptr(VentureEntity) account = NULL;
	gint64 id = venture_setup_resolve_account(self->database, org, role, "organization", 0, NULL, error);
	if (id != 0 || (error != NULL && *error != NULL))
		return id;
	account = account_with_code(self, org, code, error);
	return account != NULL ? venture_entity_get_id(account) : 0;
}

typedef struct
{
	GHashTable *by_name;	/* folded name -> GArray of ids */
	GHashTable *codes;	/* every code in the organization */
	gboolean scoped;	/* the chart uses "<org>:" codes */
} Chart;

static void
chart_free(Chart *chart)
{
	if (chart == NULL)
		return;
	g_clear_pointer(&chart->by_name, g_hash_table_unref);
	g_clear_pointer(&chart->codes, g_hash_table_unref);
	g_free(chart);
}
G_DEFINE_AUTOPTR_CLEANUP_FUNC(Chart, chart_free)

static Chart *
chart_load(VentureBooksImportService *self, gint64 org, GError **error)
{
	g_autoptr(Chart) chart = g_new0(Chart, 1);
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ACCOUNT);
	g_autoptr(GPtrArray) accounts = NULL;
	g_autofree gchar *prefix = g_strdup_printf("%" G_GINT64_FORMAT ":", org);
	guint i;
	chart->by_name = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_array_unref);
	chart->codes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	venture_query_set_organization(query, org);
	venture_query_set_limit(query, 0);
	accounts = venture_database_find(self->database, query, error);
	if (accounts == NULL)
		return NULL;
	for (i = 0; i < accounts->len; i++)
	{
		VentureEntity *account = g_ptr_array_index(accounts, i);
		g_autofree gchar *name = NULL;
		g_autofree gchar *code = NULL;
		gchar *key;
		GArray *ids;
		gint64 id = venture_entity_get_id(account);
		g_object_get(account, "name", &name, "code", &code, NULL);
		if (code != NULL)
		{
			if (g_str_has_prefix(code, prefix))
				chart->scoped = TRUE;
			g_hash_table_add(chart->codes, g_strdup(code));
		}
		key = fold_name(name);
		ids = g_hash_table_lookup(chart->by_name, key);
		if (ids == NULL)
		{
			ids = g_array_new(FALSE, FALSE, sizeof(gint64));
			g_hash_table_insert(chart->by_name, key, ids);
		}
		else
			g_free(key);
		g_array_append_val(ids, id);
	}
	return g_steal_pointer(&chart);
}

/* A free code in the kind's range: 1xxx assets, 2xxx liabilities and so on,
 * starting high so it never takes a code the starter chart uses. */
static gchar *
next_code(Chart *chart, gint64 org, gint kind)
{
	static const gint bases[] = { 1700, 2700, 3700, 4700, 6700 };
	gint base = kind >= 0 && kind < (gint)G_N_ELEMENTS(bases) ? bases[kind] : 6700;
	gint n;
	for (n = base; n < base + 300; n++)
	{
		gchar *code = chart->scoped ? g_strdup_printf("%" G_GINT64_FORMAT ":%d", org, n) : g_strdup_printf("%d", n);
		if (!g_hash_table_contains(chart->codes, code))
		{
			g_hash_table_add(chart->codes, g_strdup(code));
			return code;
		}
		g_free(code);
	}
	return NULL;
}

/*
 * Resolves every Zoho account to a VENTURE account, in this order: the
 * receivables or payables control account for Zoho's control types; the
 * code the manifest maps it to; the one account of the same name; a new
 * account, created only when @create. Two accounts of the same name are a
 * problem to resolve by mapping, never a guess.
 */
static gboolean
resolve_accounts(Plan *plan, gboolean create, const VentureActor *actor, GError **error)
{
	VentureBooksImportService *self = plan->self;
	g_autoptr(Chart) chart = chart_load(self, plan->org, error);
	GHashTableIter iter;
	gpointer value;
	if (chart == NULL)
		return FALSE;
	g_hash_table_iter_init(&iter, plan->accounts);
	while (g_hash_table_iter_next(&iter, NULL, &value))
	{
		Account *account = value;
		g_autofree gchar *key = fold_name(account->name);
		GArray *ids;
		account->id = 0;
		account->create = FALSE;
		if (account->role == ROLE_RECEIVABLES || account->role == ROLE_PAYABLES)
		{
			account->id = account->role == ROLE_RECEIVABLES
				? control_account(self, plan->org, "receivables", "1100", error)
				: control_account(self, plan->org, "payables", "2000", error);
			if (error != NULL && *error != NULL)
				return FALSE;
			if (account->id != 0)
				continue;
		}
		if (account->code != NULL)
		{
			g_autoptr(VentureEntity) found = account_with_code(self, plan->org, account->code, error);
			if (found == NULL)
			{
				if (error != NULL && *error != NULL)
					return FALSE;
				plan_problem(plan, NULL, 0, "accounts maps \"%s\" to code %s, which the chart does not have",
					account->name, account->code);
				continue;
			}
			account->id = venture_entity_get_id(found);
			continue;
		}
		ids = g_hash_table_lookup(chart->by_name, key);
		if (ids != NULL && ids->len > 1)
		{
			plan_problem(plan, NULL, 0, "%u accounts are named \"%s\"; map it to one code under accounts",
				ids->len, account->name);
			continue;
		}
		if (ids != NULL)
		{
			g_autoptr(VentureEntity) found = venture_database_get(self->database, VENTURE_TYPE_ACCOUNT,
				g_array_index(ids, gint64, 0), error);
			gint kind = -1;
			if (found == NULL)
				return FALSE;
			g_object_get(found, "kind", &kind, NULL);
			if (kind != account->kind)
				g_ptr_array_add(plan->warnings, g_strdup_printf("account \"%s\" exists with another kind; it is used as it is",
					account->name));
			account->id = g_array_index(ids, gint64, 0);
			continue;
		}
		account->create = TRUE;
		if (create)
		{
			g_autoptr(VentureEntity) made = VENTURE_ENTITY(venture_account_new());
			g_autofree gchar *code = next_code(chart, plan->org, account->kind);
			if (code == NULL)
				return refuse(error, "no free account code is left in the range");
			g_object_set(made, "code", code, "name", account->name, "kind", account->kind, "active", TRUE, NULL);
			venture_entity_set_organization_id(made, plan->org);
			if (!venture_database_save(self->database, made, actor, error))
				return FALSE;
			account->id = venture_entity_get_id(made);
		}
	}
	return TRUE;
}

static gint64
account_id(Plan *plan, const gchar *name)
{
	Account *account = plan_account(plan, name);
	return account != NULL ? account->id : 0;
}

typedef struct
{
	GHashTable *by_external;	/* external id -> company id */
	GHashTable *by_name;		/* folded name -> GArray of ids */
} Companies;

static void
companies_free(Companies *companies)
{
	if (companies == NULL)
		return;
	g_clear_pointer(&companies->by_external, g_hash_table_unref);
	g_clear_pointer(&companies->by_name, g_hash_table_unref);
	g_free(companies);
}
G_DEFINE_AUTOPTR_CLEANUP_FUNC(Companies, companies_free)

static Companies *
companies_load(VentureBooksImportService *self, gint64 org, GError **error)
{
	g_autoptr(Companies) companies = g_new0(Companies, 1);
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_COMPANY);
	g_autoptr(GPtrArray) found = NULL;
	guint i;
	companies->by_external = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	companies->by_name = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_array_unref);
	venture_query_set_organization(query, org);
	venture_query_set_limit(query, 0);
	found = venture_database_find(self->database, query, error);
	if (found == NULL)
		return NULL;
	for (i = 0; i < found->len; i++)
	{
		VentureEntity *company = g_ptr_array_index(found, i);
		g_autofree gchar *name = NULL;
		g_autofree gchar *external = NULL;
		gint64 id = venture_entity_get_id(company);
		gchar *key;
		GArray *ids;
		g_object_get(company, "name", &name, "external-id", &external, NULL);
		if (external != NULL && *external != '\0')
			g_hash_table_insert(companies->by_external, g_strdup(external), (gpointer)(gintptr)id);
		key = fold_name(name);
		ids = g_hash_table_lookup(companies->by_name, key);
		if (ids == NULL)
		{
			ids = g_array_new(FALSE, FALSE, sizeof(gint64));
			g_hash_table_insert(companies->by_name, key, ids);
		}
		else
			g_free(key);
		g_array_append_val(ids, id);
	}
	return g_steal_pointer(&companies);
}

/* The external id a Zoho contact is known by: the same one the opening
 * cutover converter uses, so a company an earlier cutover made is found. */
static gchar *
party_external(Party *party)
{
	return g_strdup_printf("%s%s", party->vendor ? "zoho-v-" : "zoho-c-", party->source_id);
}

/*
 * Resolves a party: the company carrying its external id, else the one
 * company of the same name, else -- when @create -- a new company with the
 * contact's details. Email and phone come over only when the manifest asks
 * for contact_details: until the installation's mail is proven to go
 * somewhere safe, an imported customer must not be reachable.
 */
static gint64
resolve_party(Plan *plan, Companies *companies, Party *party, gboolean create, gboolean *created,
	const VentureActor *actor, GError **error)
{
	VentureBooksImportService *self = plan->self;
	g_autofree gchar *external = party_external(party);
	g_autofree gchar *key = fold_name(party->name);
	gpointer found = g_hash_table_lookup(companies->by_external, external);
	GArray *ids;
	*created = FALSE;
	if (found != NULL)
		return (gint64)(gintptr)found;
	ids = g_hash_table_lookup(companies->by_name, key);
	if (ids != NULL && ids->len > 1)
	{
		plan_problem(plan, NULL, 0, "%u companies are named \"%s\"; give the right one external id %s",
			ids->len, party->name, external);
		return -1;
	}
	if (ids != NULL)
		return g_array_index(ids, gint64, 0);
	if (!create)
		return 0;
	{
		g_autoptr(VentureCompany) company = venture_company_new();
		g_autoptr(GString) notes = g_string_new(party->notes != NULL ? party->notes : "");
		if (party->terms != NULL && *party->terms != '\0')
			g_string_append_printf(notes, "%sPayment terms (Zoho Books): %s", notes->len > 0 ? "\n" : "", party->terms);
		g_object_set(company, "name", party->name, "external-id", external, "active", TRUE, "source", "zoho_books",
			"kind", party->vendor ? VENTURE_COMPANY_KIND_SUPPLIER : VENTURE_COMPANY_KIND_CUSTOMER,
			"address", party->address, "notes", notes->len > 0 ? notes->str : NULL, NULL);
		if (party->website != NULL && *party->website != '\0')
			g_object_set(company, "website", party->website, NULL);
		if (plan->contact_details)
		{
			if (party->email != NULL && *party->email != '\0')
				g_object_set(company, "email", party->email, NULL);
			if (party->phone != NULL && *party->phone != '\0')
				g_object_set(company, "phone", party->phone, NULL);
		}
		/* Imported overdue invoices must not start dunning or late fees the
		 * day the books arrive; the owner turns collections back on. */
		g_object_set(company, "dunning-opt-out", TRUE, "late-fee-exempt", TRUE, NULL);
		venture_entity_set_organization_id(VENTURE_ENTITY(company), plan->org);
		if (!venture_database_save(self->database, VENTURE_ENTITY(company), actor, error))
			return -1;
		g_hash_table_insert(companies->by_external, g_strdup(external),
			(gpointer)(gintptr)venture_entity_get_id(VENTURE_ENTITY(company)));
		*created = TRUE;
		return venture_entity_get_id(VENTURE_ENTITY(company));
	}
}

/* --- Preview ------------------------------------------------------------------------------ */

static guint
count_events(Plan *plan, EventKind kind, gboolean posting_only)
{
	guint i, n = 0;
	for (i = 0; i < plan->events->len; i++)
	{
		Event *event = g_ptr_array_index(plan->events, i);
		if (event->kind == kind && (!posting_only || event->posts))
			n++;
	}
	return n;
}

static void
report_plan(Plan *plan, GString *block)
{
	static const struct { EventKind kind; const gchar *label; } kinds[] = {
		{ EVENT_INVOICE, "invoices" }, { EVENT_CUSTOMER_PAYMENT, "customer payments" },
		{ EVENT_CREDIT_NOTE, "credit notes" }, { EVENT_CREDIT_APPLICATION, "credit applications" },
		{ EVENT_REFUND, "refunds" }, { EVENT_BILL, "bills" }, { EVENT_VENDOR_PAYMENT, "vendor payments" },
		{ EVENT_VENDOR_CREDIT, "vendor credits" }, { EVENT_VENDOR_REFUND, "vendor refunds" },
		{ EVENT_EXPENSE, "expenses" }, { EVENT_MONEY_MOVE, "deposits and transfers" }, { EVENT_JOURNAL, "journals" }
	};
	GHashTableIter iter;
	gpointer value;
	guint i, create = 0;
	g_string_append_printf(block, "Source zoho_books, start %s, export date %s, currency %s\n",
		plan->start_text, *plan->export_text != '\0' ? plan->export_text : "(none)", plan->currency);
	g_string_append_printf(block, "Contacts: %u customers, %u vendors\n",
		g_hash_table_size(plan->customers), g_hash_table_size(plan->vendors));
	for (i = 0; i < G_N_ELEMENTS(kinds); i++)
	{
		guint all = count_events(plan, kinds[i].kind, FALSE);
		guint posting = count_events(plan, kinds[i].kind, TRUE);
		if (all == 0)
			continue;
		if (all != posting)
			g_string_append_printf(block, "%s: %u (%u posting, %u draft or void)\n", kinds[i].label, all, posting, all - posting);
		else
			g_string_append_printf(block, "%s: %u\n", kinds[i].label, all);
	}
	g_string_append_printf(block, "Opening rows: %u\n", plan->opening->len);
	g_hash_table_iter_init(&iter, plan->accounts);
	while (g_hash_table_iter_next(&iter, NULL, &value))
		if (((Account *)value)->create)
			create++;
	g_string_append_printf(block, "Accounts: %u in the backup, %u to create\n", g_hash_table_size(plan->accounts), create);
	for (i = 0; i < plan->problems->len; i++)
		g_string_append_printf(block, "PROBLEM %s\n", (const gchar *)g_ptr_array_index(plan->problems, i));
	for (i = 0; i < plan->warnings->len; i++)
		g_string_append_printf(block, "WARNING %s\n", (const gchar *)g_ptr_array_index(plan->warnings, i));
	for (i = 0; i < plan->unsupported->len; i++)
		g_string_append_printf(block, "NOT IMPORTED %s\n", (const gchar *)g_ptr_array_index(plan->unsupported, i));
}

/* Duplicates against other live batches: replaying them twice would count
 * every one of them twice. */
static gboolean
check_owned(Plan *plan, gint64 batch_id, GError **error)
{
	guint i;
	for (i = 0; i < plan->events->len; i++)
	{
		Event *event = g_ptr_array_index(plan->events, i);
		gint64 other = owned_elsewhere(plan->self, plan->org, batch_id, event_objects[event->kind], event->source_id, error);
		if (other < 0)
			return FALSE;
		if (other > 0)
		{
			plan_problem(plan, NULL, 0, "%s %s was already imported by books import %" G_GINT64_FORMAT "; roll that batch back first",
				event_objects[event->kind], event->source_id, other);
			return TRUE;
		}
	}
	return TRUE;
}

static void
preview_parties(Plan *plan, const VentureActor *actor)
{
	g_autoptr(Companies) companies = companies_load(plan->self, plan->org, NULL);
	GHashTable *sides[2];
	guint s;
	if (companies == NULL)
		return;
	sides[0] = plan->customers;
	sides[1] = plan->vendors;
	for (s = 0; s < 2; s++)
	{
		GHashTableIter iter;
		gpointer value;
		g_hash_table_iter_init(&iter, sides[s]);
		while (g_hash_table_iter_next(&iter, NULL, &value))
		{
			gboolean created;
			resolve_party(plan, companies, value, FALSE, &created, actor, NULL);
		}
	}
}

VentureEntity *
venture_books_import_service_preview(VentureBooksImportService *self, gint64 organization_id,
	JsonObject *manifest, const VentureActor *actor, GError **error)
{
	g_autoptr(Plan) plan = NULL;
	g_autoptr(VentureBooksImport) batch = NULL;
	g_autoptr(JsonGenerator) generator = json_generator_new();
	g_autoptr(JsonNode) root = json_node_new(JSON_NODE_OBJECT);
	g_autoptr(GString) block = g_string_new(NULL);
	g_autofree gchar *text = NULL;
	g_autofree gchar *stamp = stamp_now();
	g_return_val_if_fail(VENTURE_IS_BOOKS_IMPORT_SERVICE(self), NULL);
	if (organization_id <= 0)
	{
		refuse(error, "a migration lands in one legal entity; select it first");
		return NULL;
	}
	if (manifest == NULL)
	{
		refuse(error, "a manifest is required");
		return NULL;
	}
	plan = plan_build(self, organization_id, manifest, error);
	if (plan == NULL || !resolve_accounts(plan, FALSE, actor, error) || !check_owned(plan, 0, error))
		return NULL;
	preview_parties(plan, actor);
	json_node_set_object(root, manifest);
	json_generator_set_root(generator, root);
	text = json_generator_to_data(generator, NULL);
	g_string_append_printf(block, "== Preview %s ==\n", stamp);
	report_plan(plan, block);
	g_string_append_printf(block, "Result: %s\n", plan->problems->len == 0 ? "ready to import" : "fix the problems first");
	batch = venture_books_import_new();
	g_object_set(batch, "source", "zoho_books", "state", "preview", "start", plan->start,
		"export-date", plan->export_date, "manifest", text, "report", block->str, NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(batch), organization_id);
	if (!save_owned(self, VENTURE_ENTITY(batch), actor, error))
		return NULL;
	return VENTURE_ENTITY(g_steal_pointer(&batch));
}

static Plan *
plan_of(VentureBooksImportService *self, VentureEntity *batch, GError **error)
{
	g_autoptr(JsonObject) manifest = manifest_of(batch, error);
	if (manifest == NULL)
		return NULL;
	return plan_build(self, venture_entity_get_organization_id(batch), manifest, error);
}

static gboolean
refuse_problems(Plan *plan, GError **error)
{
	g_autoptr(GString) text = NULL;
	guint i;
	if (plan->problems->len == 0)
		return TRUE;
	text = g_string_new("VentureBooksImportService: import refused before its first write: ");
	for (i = 0; i < plan->problems->len && i < 20; i++)
		g_string_append_printf(text, "%s%s", i > 0 ? "; " : "", (const gchar *)g_ptr_array_index(plan->problems, i));
	if (plan->problems->len > 20)
		g_string_append_printf(text, "; and %u more", plan->problems->len - 20);
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, text->str);
	return FALSE;
}

#include "venture-books-import-replay.inc"
#include "venture-books-import-reconcile.inc"
#include "venture-books-import-rollback.inc"

/* --- Actions ------------------------------------------------------------------------------- */

static gboolean
books_allowed(VentureAction *action, VentureEntity *entity, const VentureActor *actor, GError **error)
{
	(void)action;
	(void)entity;
	(void)actor;
	(void)error;
	return TRUE;
}

static VentureEntity *
record_preflight(VentureBooksImportService *self, VentureEntity *batch, const VentureActor *actor, GError **error)
{
	g_autoptr(GPtrArray) blockers = venture_books_import_service_rollback_preflight(self, VENTURE_BOOKS_IMPORT(batch), error);
	g_autoptr(GString) block = g_string_new(NULL);
	g_autofree gchar *stamp = stamp_now();
	guint i;
	if (blockers == NULL)
		return NULL;
	g_string_append_printf(block, "\n== Rollback preflight %s ==\n", stamp);
	if (blockers->len == 0)
		g_string_append(block, "No blockers: rollback can unwind every imported record.\n");
	for (i = 0; i < blockers->len; i++)
		g_string_append_printf(block, "BLOCKER %s\n", (const gchar *)g_ptr_array_index(blockers, i));
	if (!append_report(self, batch, NULL, block, actor, error))
		return NULL;
	return g_object_ref(batch);
}

static VentureEntity *
books_invoke(VentureAction *action, VentureEntity *entity, GHashTable *params, const VentureActor *actor, GError **error)
{
	g_autofree gchar *name = NULL;
	VentureBooksImportService *self = venture_action_get_data(action);
	VentureBooksImport *batch = VENTURE_BOOKS_IMPORT(entity);
	(void)params;
	g_object_get(action, "name", &name, NULL);
	if (g_strcmp0(name, "import") == 0)
		return venture_books_import_service_import(self, batch, actor, error) ? g_object_ref(entity) : NULL;
	if (g_strcmp0(name, "reconcile") == 0)
		return venture_books_import_service_reconcile(self, batch, actor, error) ? g_object_ref(entity) : NULL;
	if (g_strcmp0(name, "activate") == 0)
		return venture_books_import_service_activate(self, batch, actor, error) ? g_object_ref(entity) : NULL;
	if (g_strcmp0(name, "rollback_preflight") == 0)
		return record_preflight(self, entity, actor, error);
	if (g_strcmp0(name, "rollback") == 0)
		return venture_books_import_service_rollback(self, batch, actor, error) ? g_object_ref(entity) : NULL;
	return NULL;
}

void
venture_books_import_actions_register(VentureDatabase *database)
{
	VentureActionRegistry *registry = venture_database_get_action_registry(database);
	static const gchar *const names[] = { "import", "reconcile", "activate", "rollback_preflight", "rollback" };
	guint i;
	for (i = 0; i < G_N_ELEMENTS(names); i++)
	{
		g_autoptr(VentureAction) action = g_object_new(VENTURE_TYPE_ACTION, "data-class", VENTURE_DATA_CLASS_TENANT,
			"type-name", "books_import", "name", names[i], "label", names[i], "description", "Books import action",
			"stageable", FALSE, "service-transaction", TRUE, "roles", VENTURE_USER_ROLE_EDITOR, NULL);
		g_autoptr(GError) error = NULL;
		venture_action_registry_register(registry, action, books_allowed, books_invoke,
			venture_books_import_service_get(database), NULL, &error);
	}
}
