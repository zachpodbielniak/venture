/* SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Reading a Zoho Books backup (Settings > Data Backup: one CSV per object)
 * into a plan: the chart, the parties, every document as a dated event, and
 * the source ledger -- what Zoho itself posted -- that the replay must
 * reproduce account for account. Included by venture-books-import-service.c.
 *
 * Nothing here writes. The plan is built the same way for preview, import
 * and reconcile, so the three can never disagree about what the backup says.
 *
 * Zoho's posting rules, as its trial balance shows them:
 *  - an invoice debits receivables with its total and credits each line's
 *    account with quantity times rate; a line discount is a debit to the
 *    discount account; an adjustment credits its adjustment account;
 *  - a customer payment debits the deposit account with the amount less bank
 *    charges and the charges account with the charges, and credits
 *    receivables (an unused amount stays a customer credit, which VENTURE
 *    holds inside receivables too);
 *  - a bill credits payables with its total and debits each line's account
 *    with the line total, the purchase tax account with the line's tax, and
 *    the adjustment account with the adjustment;
 *  - a vendor payment debits payables and credits the paid-through account;
 *  - an expense debits its account with the total less tax and the purchase
 *    tax account with the tax, and credits the paid-through account;
 *  - deposits and transfers move money from one account to another;
 *  - drafts and voids post nothing.
 */

typedef struct
{
	gchar *file;
	GPtrArray *rows;	/* GStrv per data row */
	GHashTable *index;	/* folded header -> column + 1 */
	guint width;
} Table;

static void
table_free(gpointer data)
{
	Table *table = data;
	if (table == NULL)
		return;
	g_free(table->file);
	g_clear_pointer(&table->rows, g_ptr_array_unref);
	g_clear_pointer(&table->index, g_hash_table_unref);
	g_free(table);
}

/* Headers match ignoring case and surrounding space. A repeated header (the
 * credit note export has two "Issued Date" columns) keeps its first column. */
static gchar *
fold_header(const gchar *header)
{
	g_autofree gchar *stripped = g_strstrip(g_strdup(header != NULL ? header : ""));
	/* A UTF-8 byte order mark survives in the first header of some exports. */
	const gchar *text = g_str_has_prefix(stripped, "\xEF\xBB\xBF") ? stripped + 3 : stripped;
	return g_utf8_casefold(text, -1);
}

static const gchar *
cell(Table *table, guint row, const gchar *column)
{
	g_autofree gchar *key = fold_header(column);
	gpointer found;
	gchar **values;
	guint index;
	if (table == NULL || row >= table->rows->len)
		return "";
	found = g_hash_table_lookup(table->index, key);
	if (found == NULL)
		return "";
	index = GPOINTER_TO_UINT(found) - 1;
	values = g_ptr_array_index(table->rows, row);
	if (index >= g_strv_length(values))
		return "";
	return values[index];
}

static gboolean
has_column(Table *table, const gchar *column)
{
	g_autofree gchar *key = fold_header(column);
	return table != NULL && g_hash_table_contains(table->index, key);
}

static Table *
table_parse(const gchar *file, const gchar *text, GError **error)
{
	g_autoptr(GPtrArray) parsed = venture_csv_parse(text, error);
	Table *table;
	gchar **header;
	guint i;
	if (parsed == NULL)
	{
		if (error != NULL && *error != NULL)
			g_prefix_error(error, "%s: ", file);
		return NULL;
	}
	table = g_new0(Table, 1);
	table->file = g_strdup(file);
	table->rows = g_ptr_array_new_with_free_func((GDestroyNotify)g_strfreev);
	table->index = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	if (parsed->len == 0)
		return table;
	header = g_ptr_array_index(parsed, 0);
	table->width = g_strv_length(header);
	for (i = 0; header[i] != NULL; i++)
	{
		gchar *key = fold_header(header[i]);
		if (*key == '\0' || g_hash_table_contains(table->index, key))
			g_free(key);
		else
			g_hash_table_insert(table->index, key, GUINT_TO_POINTER(i + 1));
	}
	for (i = 1; i < parsed->len; i++)
		g_ptr_array_add(table->rows, g_strdupv(g_ptr_array_index(parsed, i)));
	return table;
}

/* --- Amounts and dates ---------------------------------------------------------------- */

/* Zoho prints amounts with up to six decimals ("213.500000"). Digits past
 * the currency's minor unit must be zero: a figure that cannot be held to
 * the cent is a located problem, never a rounding. */
static gboolean
parse_amount(const gchar *text, guint exponent, gint64 *out)
{
	const gchar *p = text != NULL ? text : "";
	gboolean negative = FALSE, digits = FALSE;
	gint64 whole = 0, fraction = 0;
	guint places = 0;
	while (g_ascii_isspace(*p))
		p++;
	*out = 0;
	if (*p == '\0')
		return TRUE;
	if (*p == '-')
	{
		negative = TRUE;
		p++;
	}
	for (; g_ascii_isdigit(*p) || *p == ','; p++)
	{
		if (*p == ',')
			continue;
		if (whole > G_MAXINT64 / 100)
			return FALSE;
		whole = whole * 10 + (*p - '0');
		digits = TRUE;
	}
	if (*p == '.')
	{
		for (p++; g_ascii_isdigit(*p); p++)
		{
			if (places < exponent)
			{
				fraction = fraction * 10 + (*p - '0');
				places++;
			}
			else if (*p != '0')
				return FALSE;
			digits = TRUE;
		}
	}
	while (g_ascii_isspace(*p))
		p++;
	if (*p != '\0' || !digits)
		return FALSE;
	for (; places < exponent; places++)
		fraction *= 10;
	{
		gint64 scale = 1;
		guint i;
		for (i = 0; i < exponent; i++)
			scale *= 10;
		*out = whole * scale + fraction;
	}
	if (negative)
		*out = -*out;
	return TRUE;
}

/* Quantities are held to thousandths, like invoice and bill lines. */
static gboolean
parse_thousandths(const gchar *text, gint64 *out)
{
	return parse_amount(text, 3, out);
}

/* Zoho dates are "YYYY-MM-DD", sometimes with a time. A date is midnight
 * UTC at its start, as everywhere else in the ledger. */
static GDateTime *
parse_date(const gchar *text)
{
	gint year, month, day;
	if (text == NULL || strlen(text) < 10 || sscanf(text, "%4d-%2d-%2d", &year, &month, &day) != 3)
		return NULL;
	if (!g_date_valid_dmy((GDateDay)day, (GDateMonth)month, (GDateYear)year))
		return NULL;
	return g_date_time_new_utc(year, month, day, 0, 0, 0);
}

/* --- The plan --------------------------------------------------------------------------- */

typedef enum
{
	ROLE_NONE,
	ROLE_RECEIVABLES,
	ROLE_PAYABLES
} AccountRole;

typedef struct
{
	gchar *name;		/* as Zoho prints it */
	gchar *type;		/* Zoho account type */
	gint kind;		/* VentureAccountKind */
	AccountRole role;
	gboolean active;
	gchar *code;		/* manifest mapping, or NULL */
	gint64 id;		/* resolved at import and reconcile */
	gboolean create;	/* preview: will be created */
} Account;

static void
account_free(gpointer data)
{
	Account *account = data;
	g_free(account->name);
	g_free(account->type);
	g_free(account->code);
	g_free(account);
}

typedef struct
{
	gchar *source_id;	/* Zoho contact id */
	gchar *name;
	gboolean vendor;
	gchar *email, *phone, *website, *address, *notes, *terms;
	gint64 id;		/* resolved company */
} Party;

static void
party_free(gpointer data)
{
	Party *party = data;
	g_free(party->source_id);
	g_free(party->name);
	g_free(party->email);
	g_free(party->phone);
	g_free(party->website);
	g_free(party->address);
	g_free(party->notes);
	g_free(party->terms);
	g_free(party);
}

typedef enum
{
	EVENT_INVOICE,
	EVENT_BILL,
	EVENT_CREDIT_NOTE,
	EVENT_VENDOR_CREDIT,
	EVENT_CUSTOMER_PAYMENT,
	EVENT_VENDOR_PAYMENT,
	EVENT_CREDIT_APPLICATION,
	EVENT_REFUND,
	EVENT_VENDOR_REFUND,
	EVENT_EXPENSE,
	EVENT_MONEY_MOVE,
	EVENT_JOURNAL
} EventKind;

static const gchar *const event_objects[] = {
	"invoice", "bill", "credit_note", "vendor_credit", "customer_payment", "vendor_payment",
	"credit_application", "refund", "vendor_refund", "expense", "money_move", "journal"
};

typedef struct
{
	EventKind kind;
	gchar *date;		/* YYYY-MM-DD */
	gchar *source_id;
	Table *table;
	GArray *rows;		/* guint row indices */
	guint order;		/* position in the backup, the tie-break */
	gint64 cents;		/* document total, for the monthly comparison */
	gboolean posts;		/* FALSE for drafts: kept, never posted */
	gchar *status;		/* Zoho status */
} Event;

static void
event_free(gpointer data)
{
	Event *event = data;
	g_free(event->date);
	g_free(event->source_id);
	g_free(event->status);
	g_array_unref(event->rows);
	g_free(event);
}

/* One leg of the source ledger: Zoho's own posting of a document. */
typedef struct
{
	gchar *date;
	gchar *account;		/* Zoho account name, or "@ar", "@ap", "@tax" */
	gint64 cents;		/* debit positive */
} Posting;

static void
posting_free(gpointer data)
{
	Posting *posting = data;
	g_free(posting->date);
	g_free(posting->account);
	g_free(posting);
}

typedef struct
{
	VentureBooksImportService *self;
	gint64 org;
	gchar *currency;
	guint exponent;
	GDateTime *start;
	GDateTime *export_date;
	gchar *start_text;
	gchar *export_text;
	GHashTable *tables;	/* file -> Table */
	GHashTable *accounts;	/* folded name -> Account */
	GHashTable *customers;	/* contact id -> Party */
	GHashTable *vendors;	/* contact id -> Party */
	GHashTable *customer_names;	/* folded name -> Party */
	GHashTable *vendor_names;	/* folded name -> Party */
	GPtrArray *events;
	GPtrArray *expected;	/* Posting */
	GPtrArray *opening;	/* Posting at the start: the day-one balances */
	GPtrArray *problems;
	GPtrArray *warnings;
	GPtrArray *unsupported;
	GPtrArray *persons;	/* Person: the people at each company */
	gchar *bank_charges_account;
	gchar *purchase_tax_account;
	gchar *discount_account;
	gchar *adjustment_account;
	gboolean contact_details;
	GHashTable *invoice_numbers;	/* Zoho invoice number -> Event */
	GHashTable *bill_ids;		/* Zoho bill id -> Event */
	GHashTable *credit_numbers;	/* Zoho credit note number -> Event */
} Plan;

static void
plan_free(Plan *plan)
{
	if (plan == NULL)
		return;
	g_free(plan->currency);
	g_clear_pointer(&plan->start, g_date_time_unref);
	g_clear_pointer(&plan->export_date, g_date_time_unref);
	g_free(plan->start_text);
	g_free(plan->export_text);
	g_clear_pointer(&plan->tables, g_hash_table_unref);
	g_clear_pointer(&plan->accounts, g_hash_table_unref);
	g_clear_pointer(&plan->customers, g_hash_table_unref);
	g_clear_pointer(&plan->vendors, g_hash_table_unref);
	g_clear_pointer(&plan->customer_names, g_hash_table_unref);
	g_clear_pointer(&plan->vendor_names, g_hash_table_unref);
	g_clear_pointer(&plan->events, g_ptr_array_unref);
	g_clear_pointer(&plan->expected, g_ptr_array_unref);
	g_clear_pointer(&plan->opening, g_ptr_array_unref);
	g_clear_pointer(&plan->problems, g_ptr_array_unref);
	g_clear_pointer(&plan->warnings, g_ptr_array_unref);
	g_clear_pointer(&plan->unsupported, g_ptr_array_unref);
	g_clear_pointer(&plan->persons, g_ptr_array_unref);
	g_free(plan->bank_charges_account);
	g_free(plan->purchase_tax_account);
	g_free(plan->discount_account);
	g_free(plan->adjustment_account);
	g_clear_pointer(&plan->invoice_numbers, g_hash_table_unref);
	g_clear_pointer(&plan->bill_ids, g_hash_table_unref);
	g_clear_pointer(&plan->credit_numbers, g_hash_table_unref);
	g_free(plan);
}
G_DEFINE_AUTOPTR_CLEANUP_FUNC(Plan, plan_free)

static void plan_problem(Plan *plan, const gchar *file, guint row, const gchar *format, ...) G_GNUC_PRINTF(4, 5);

static void
plan_problem(Plan *plan, const gchar *file, guint row, const gchar *format, ...)
{
	va_list args;
	g_autofree gchar *text = NULL;
	va_start(args, format);
	text = g_strdup_vprintf(format, args);
	va_end(args);
	/* Rows are numbered as a spreadsheet shows them: the header is row 1. */
	g_ptr_array_add(plan->problems, file != NULL
		? g_strdup_printf("%s row %u: %s", file, row + 2, text)
		: g_strdup(text));
}

static Table *
plan_table(Plan *plan, const gchar *file)
{
	return g_hash_table_lookup(plan->tables, file);
}

static gchar *
fold_name(const gchar *name)
{
	g_autofree gchar *stripped = g_strstrip(g_strdup(name != NULL ? name : ""));
	return g_utf8_casefold(stripped, -1);
}

static gint64
amount_at(Plan *plan, Table *table, guint row, const gchar *column)
{
	gint64 cents = 0;
	const gchar *text = cell(table, row, column);
	if (!parse_amount(text, plan->exponent, &cents))
		plan_problem(plan, table->file, row, "%s \"%s\" is not an amount in %s", column, text, plan->currency);
	return cents;
}

static void
expect(Plan *plan, const gchar *date, const gchar *account, gint64 cents)
{
	Posting *posting;
	if (cents == 0)
		return;
	posting = g_new0(Posting, 1);
	posting->date = g_strndup(date, 10);
	posting->account = g_strdup(account);
	posting->cents = cents;
	g_ptr_array_add(plan->expected, posting);
}

static Account *
plan_account(Plan *plan, const gchar *name)
{
	g_autofree gchar *key = fold_name(name);
	return g_hash_table_lookup(plan->accounts, key);
}

/* A document naming an account the chart export lacks is a located problem:
 * guessing its kind would put it on the wrong side of the balance sheet. */
static gboolean
need_account(Plan *plan, Table *table, guint row, const gchar *name)
{
	if (name == NULL || *name == '\0')
	{
		plan_problem(plan, table->file, row, "no account named");
		return FALSE;
	}
	if (plan_account(plan, name) == NULL)
	{
		plan_problem(plan, table->file, row, "account \"%s\" is not in Chart_of_Accounts.csv", name);
		return FALSE;
	}
	return TRUE;
}

/* The receivables or payables account a control-account type names. */
static const gchar *
control_key(Plan *plan, const gchar *name)
{
	Account *account = plan_account(plan, name);
	if (account != NULL && account->role == ROLE_RECEIVABLES)
		return "@ar";
	if (account != NULL && account->role == ROLE_PAYABLES)
		return "@ap";
	return name;
}

static gint
zoho_kind(const gchar *type, AccountRole *role)
{
	static const struct { const gchar *type; gint kind; AccountRole role; } kinds[] = {
		{ "Accounts Receivable", VENTURE_ACCOUNT_KIND_ASSET, ROLE_RECEIVABLES },
		{ "Accounts Payable", VENTURE_ACCOUNT_KIND_LIABILITY, ROLE_PAYABLES },
		{ "Bank", VENTURE_ACCOUNT_KIND_ASSET, ROLE_NONE },
		{ "Cash", VENTURE_ACCOUNT_KIND_ASSET, ROLE_NONE },
		{ "Payment Clearing", VENTURE_ACCOUNT_KIND_ASSET, ROLE_NONE },
		{ "Other Current Asset", VENTURE_ACCOUNT_KIND_ASSET, ROLE_NONE },
		{ "Fixed Asset", VENTURE_ACCOUNT_KIND_ASSET, ROLE_NONE },
		{ "Other Asset", VENTURE_ACCOUNT_KIND_ASSET, ROLE_NONE },
		{ "Stock", VENTURE_ACCOUNT_KIND_ASSET, ROLE_NONE },
		{ "Input Tax", VENTURE_ACCOUNT_KIND_ASSET, ROLE_NONE },
		{ "Intangible Asset", VENTURE_ACCOUNT_KIND_ASSET, ROLE_NONE },
		{ "Non Current Asset", VENTURE_ACCOUNT_KIND_ASSET, ROLE_NONE },
		{ "Deferred Tax Asset", VENTURE_ACCOUNT_KIND_ASSET, ROLE_NONE },
		{ "Other Current Liability", VENTURE_ACCOUNT_KIND_LIABILITY, ROLE_NONE },
		{ "Credit Card", VENTURE_ACCOUNT_KIND_LIABILITY, ROLE_NONE },
		{ "Long Term Liability", VENTURE_ACCOUNT_KIND_LIABILITY, ROLE_NONE },
		{ "Other Liability", VENTURE_ACCOUNT_KIND_LIABILITY, ROLE_NONE },
		{ "Non Current Liability", VENTURE_ACCOUNT_KIND_LIABILITY, ROLE_NONE },
		{ "Output Tax", VENTURE_ACCOUNT_KIND_LIABILITY, ROLE_NONE },
		{ "Deferred Tax Liability", VENTURE_ACCOUNT_KIND_LIABILITY, ROLE_NONE },
		{ "Equity", VENTURE_ACCOUNT_KIND_EQUITY, ROLE_NONE },
		{ "Income", VENTURE_ACCOUNT_KIND_INCOME, ROLE_NONE },
		{ "Other Income", VENTURE_ACCOUNT_KIND_INCOME, ROLE_NONE },
		{ "Expense", VENTURE_ACCOUNT_KIND_EXPENSE, ROLE_NONE },
		{ "Cost Of Goods Sold", VENTURE_ACCOUNT_KIND_EXPENSE, ROLE_NONE },
		{ "Other Expense", VENTURE_ACCOUNT_KIND_EXPENSE, ROLE_NONE }
	};
	guint i;
	*role = ROLE_NONE;
	for (i = 0; i < G_N_ELEMENTS(kinds); i++)
		if (g_ascii_strcasecmp(kinds[i].type, type) == 0)
		{
			*role = kinds[i].role;
			return kinds[i].kind;
		}
	return -1;
}

static void
plan_chart(Plan *plan, JsonObject *map)
{
	Table *chart = plan_table(plan, "Chart_of_Accounts.csv");
	guint i;
	if (chart == NULL)
	{
		plan_problem(plan, NULL, 0, "the backup has no Chart_of_Accounts.csv");
		return;
	}
	for (i = 0; i < chart->rows->len; i++)
	{
		const gchar *name = cell(chart, i, "Account Name");
		const gchar *type = cell(chart, i, "Account Type");
		Account *account;
		AccountRole role;
		gint kind = zoho_kind(type, &role);
		if (*name == '\0')
			continue;
		if (kind < 0)
		{
			plan_problem(plan, chart->file, i, "account \"%s\" has type \"%s\", which has no VENTURE kind", name, type);
			continue;
		}
		if (plan_account(plan, name) != NULL)
		{
			plan_problem(plan, chart->file, i, "account \"%s\" appears twice", name);
			continue;
		}
		account = g_new0(Account, 1);
		account->name = g_strdup(name);
		account->type = g_strdup(type);
		account->kind = kind;
		account->role = role;
		account->active = g_ascii_strcasecmp(cell(chart, i, "Account Status"), "Inactive") != 0;
		if (map != NULL && json_object_has_member(map, name))
		{
			JsonNode *node = json_object_get_member(map, name);
			if (JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING)
				account->code = g_strdup(json_node_get_string(node));
		}
		g_hash_table_insert(plan->accounts, fold_name(name), account);
	}
}

static gchar *
address_of(Table *table, guint row)
{
	static const gchar *const parts[] = {
		"Billing Address", "Billing Street2", "Billing City", "Billing State", "Billing Code", "Billing Country"
	};
	g_autoptr(GString) text = g_string_new(NULL);
	guint i;
	for (i = 0; i < G_N_ELEMENTS(parts); i++)
	{
		const gchar *value = cell(table, row, parts[i]);
		if (*value == '\0')
			continue;
		if (text->len > 0)
			g_string_append(text, ", ");
		g_string_append(text, value);
	}
	return text->len > 0 ? g_string_free(g_steal_pointer(&text), FALSE) : NULL;
}

static void
plan_parties(Plan *plan, const gchar *file, gboolean vendor)
{
	Table *table = plan_table(plan, file);
	guint i;
	if (table == NULL)
		return;
	for (i = 0; i < table->rows->len; i++)
	{
		const gchar *id = cell(table, i, vendor ? "Contact ID" : "Contact ID");
		const gchar *name = cell(table, i, "Display Name");
		Party *party;
		gint64 opening = 0;
		if (*name == '\0')
			name = cell(table, i, "Company Name");
		if (*id == '\0' || *name == '\0')
		{
			plan_problem(plan, file, i, "a contact needs a Contact ID and a Display Name");
			continue;
		}
		if (!parse_amount(cell(table, i, "Opening Balance"), plan->exponent, &opening) || opening != 0)
			plan_problem(plan, file, i, "%s has an opening balance; give it as an opening row against the receivables or payables account instead",
				name);
		party = g_new0(Party, 1);
		party->source_id = g_strdup(id);
		party->name = g_strdup(name);
		party->vendor = vendor;
		party->email = g_strdup(cell(table, i, "EmailID"));
		party->phone = g_strdup(*cell(table, i, "Phone") != '\0' ? cell(table, i, "Phone") : cell(table, i, "MobilePhone"));
		party->website = g_strdup(cell(table, i, "Website"));
		party->address = address_of(table, i);
		party->notes = g_strdup(cell(table, i, "Notes"));
		party->terms = g_strdup(cell(table, i, "Payment Terms Label"));
		g_hash_table_insert(vendor ? plan->vendor_names : plan->customer_names, fold_name(name), party);
		g_hash_table_insert(vendor ? plan->vendors : plan->customers, g_strdup(id), party);
	}
}

/* A person at a company: a contact row's primary contact, or a row of the
 * contact persons export. */
typedef struct
{
	gchar *source_id;
	Party *party;
	gchar *name;
	gchar *email, *phone, *role;
} Person;

static void
person_free(gpointer data)
{
	Person *person = data;
	g_free(person->source_id);
	g_free(person->name);
	g_free(person->email);
	g_free(person->phone);
	g_free(person->role);
	g_free(person);
}

static gchar *
person_name(Table *table, guint row)
{
	g_autofree gchar *joined = g_strjoin(" ", cell(table, row, "Salutation"), cell(table, row, "First Name"),
		cell(table, row, "Last Name"), NULL);
	return g_strstrip(g_strdup(joined));
}

static void
add_person(Plan *plan, Party *party, Table *table, guint row, const gchar *source_id)
{
	Person *person;
	g_autofree gchar *name = NULL;
	guint i;
	if (party == NULL || source_id == NULL || *source_id == '\0')
		return;
	for (i = 0; i < plan->persons->len; i++)
		if (g_strcmp0(((Person *)g_ptr_array_index(plan->persons, i))->source_id, source_id) == 0)
			return;
	/* A salutation alone is not a name; the email stands in when there is
	 * no name at all, so the person can still be told apart. */
	name = person_name(table, row);
	if (*cell(table, row, "First Name") == '\0' && *cell(table, row, "Last Name") == '\0')
	{
		g_free(name);
		name = g_strdup(cell(table, row, "EmailID"));
	}
	if (name == NULL || *name == '\0')
		return;
	person = g_new0(Person, 1);
	person->source_id = g_strdup(source_id);
	person->party = party;
	person->name = g_steal_pointer(&name);
	person->email = g_strdup(cell(table, row, "EmailID"));
	person->phone = g_strdup(*cell(table, row, "Phone") != '\0' ? cell(table, row, "Phone") : cell(table, row, "MobilePhone"));
	person->role = g_strdup(cell(table, row, "Designation"));
	g_ptr_array_add(plan->persons, person);
}

/* The people: each contact row's primary contact, then every row of the
 * contact persons exports, linked to its company by the contact id. */
static void
plan_persons(Plan *plan)
{
	static const struct { const gchar *file; gboolean vendor; const gchar *link; } exports[] = {
		{ "Contacts.csv", FALSE, "Contact ID" }, { "Vendors.csv", TRUE, "Contact ID" },
		{ "Contact_Persons.csv", FALSE, "Customer ID" }, { "Vendor_Contact_Persons.csv", TRUE, "Vendor ID" }
	};
	guint e, i;
	for (e = 0; e < G_N_ELEMENTS(exports); e++)
	{
		Table *table = plan_table(plan, exports[e].file);
		gboolean persons_file = g_str_has_suffix(exports[e].file, "Persons.csv");
		for (i = 0; table != NULL && i < table->rows->len; i++)
		{
			Party *party = g_hash_table_lookup(exports[e].vendor ? plan->vendors : plan->customers,
				cell(table, i, exports[e].link));
			g_autofree gchar *fallback = NULL;
			const gchar *id;
			if (party == NULL)
			{
				if (persons_file)
					plan_problem(plan, table->file, i, "a contact person names a company the backup does not hold");
				continue;
			}
			id = cell(table, i, persons_file ? "Contact Person ID" : "Primary Contact ID");
			if (*id == '\0' && !persons_file &&
				(*cell(table, i, "First Name") != '\0' || *cell(table, i, "Last Name") != '\0'))
				id = fallback = g_strdup_printf("%s-primary", party->source_id);
			add_person(plan, party, table, i, id);
		}
	}
}

/* A document's party: by the contact id its export carries, else by name. */
static Party *
document_party(Plan *plan, Table *table, guint row, gboolean vendor)
{
	const gchar *id = cell(table, row, vendor ? "Vendor ID" : "Customer ID");
	const gchar *name = cell(table, row, vendor ? "Vendor Name" : "Customer Name");
	g_autofree gchar *key = NULL;
	Party *party = *id != '\0' ? g_hash_table_lookup(vendor ? plan->vendors : plan->customers, id) : NULL;
	if (party != NULL)
		return party;
	key = fold_name(name);
	party = g_hash_table_lookup(vendor ? plan->vendor_names : plan->customer_names, key);
	if (party == NULL)
		plan_problem(plan, table->file, row, "%s \"%s\" is not in %s", vendor ? "vendor" : "customer", name,
			vendor ? "Vendors.csv" : "Contacts.csv");
	return party;
}

static Event *
event_new(Plan *plan, EventKind kind, Table *table, guint row, const gchar *date_column, const gchar *source_id)
{
	const gchar *date = cell(table, row, date_column);
	g_autoptr(GDateTime) when = parse_date(date);
	Event *event;
	if (when == NULL)
	{
		plan_problem(plan, table->file, row, "%s \"%s\" is not a date", date_column, date);
		return NULL;
	}
	if (g_date_time_compare(when, plan->start) < 0)
	{
		plan_problem(plan, table->file, row, "%s %s is before the start %s; move the start earlier",
			date_column, date, plan->start_text);
		return NULL;
	}
	if (plan->export_date != NULL && g_date_time_compare(when, plan->export_date) > 0)
	{
		plan_problem(plan, table->file, row, "%s %s is after the export date %s", date_column, date, plan->export_text);
		return NULL;
	}
	event = g_new0(Event, 1);
	event->kind = kind;
	event->date = g_strndup(date, 10);
	event->source_id = g_strdup(source_id);
	event->table = table;
	event->rows = g_array_new(FALSE, FALSE, sizeof(guint));
	event->order = plan->events->len;
	event->posts = TRUE;
	g_ptr_array_add(plan->events, event);
	return event;
}

/* Rows sharing a document id, in backup order. */
static GPtrArray *
group_rows(Table *table, const gchar *column)
{
	GPtrArray *groups = g_ptr_array_new_with_free_func((GDestroyNotify)g_array_unref);
	g_autoptr(GHashTable) seen = g_hash_table_new(g_str_hash, g_str_equal);
	guint i;
	if (table == NULL)
		return groups;
	for (i = 0; i < table->rows->len; i++)
	{
		const gchar *id = cell(table, i, column);
		GArray *rows = g_hash_table_lookup(seen, id);
		if (rows == NULL)
		{
			rows = g_array_new(FALSE, FALSE, sizeof(guint));
			g_ptr_array_add(groups, rows);
			g_hash_table_insert(seen, (gpointer)id, rows);
		}
		g_array_append_val(rows, i);
	}
	return groups;
}

static guint
first_row(GArray *rows)
{
	return g_array_index(rows, guint, 0);
}

static gboolean
is_status(const gchar *status, const gchar *name)
{
	return g_ascii_strcasecmp(status, name) == 0;
}

static void
plan_invoices(Plan *plan)
{
	Table *table = plan_table(plan, "Invoice.csv");
	g_autoptr(GPtrArray) groups = group_rows(table, "Invoice ID");
	guint g, l;
	for (g = 0; g < groups->len; g++)
	{
		GArray *rows = g_ptr_array_index(groups, g);
		guint head = first_row(rows);
		const gchar *status = cell(table, head, "Invoice Status");
		const gchar *number = cell(table, head, "Invoice Number");
		Event *event;
		gint64 total, lines_total = 0, adjustment, shipping;
		if (document_party(plan, table, head, FALSE) == NULL)
			continue;
		event = event_new(plan, EVENT_INVOICE, table, head, "Invoice Date", cell(table, head, "Invoice ID"));
		if (event == NULL)
			continue;
		g_array_append_vals(event->rows, rows->data, rows->len);
		event->status = g_strdup(status);
		event->posts = !is_status(status, "Draft") && !is_status(status, "Void");
		total = amount_at(plan, table, head, "Total");
		adjustment = amount_at(plan, table, head, "Adjustment");
		shipping = amount_at(plan, table, head, "Shipping Charge");
		event->cents = total;
		if (*number == '\0')
			plan_problem(plan, table->file, head, "an invoice needs an Invoice Number");
		else if (g_hash_table_contains(plan->invoice_numbers, number))
			plan_problem(plan, table->file, head, "invoice number %s appears on two invoices", number);
		else
			g_hash_table_insert(plan->invoice_numbers, (gpointer)number, event);
		for (l = 0; l < rows->len; l++)
		{
			guint row = g_array_index(rows, guint, l);
			gint64 quantity = 0, price = 0, line_total = amount_at(plan, table, row, "Item Total");
			gint64 tax = amount_at(plan, table, row, "Item Tax Amount");
			if (!parse_thousandths(cell(table, row, "Quantity"), &quantity) || quantity <= 0)
				plan_problem(plan, table->file, row, "Quantity \"%s\" must be positive, up to three decimals",
					cell(table, row, "Quantity"));
			price = amount_at(plan, table, row, "Item Price");
			lines_total += line_total + tax;
			if (!need_account(plan, table, row, cell(table, row, "Account")))
				continue;
			if (event->posts)
			{
				/* Quantity times rate, to the cent the way Zoho rounds it. */
				gint64 gross = (gint64)(((__int128)quantity * price + (quantity * price >= 0 ? 500 : -500)) / 1000);
				expect(plan, event->date, cell(table, row, "Account"), -gross);
				if (gross != line_total)
					expect(plan, event->date, plan->discount_account, gross - line_total);
				expect(plan, event->date, "@tax", -tax);
			}
		}
		if (adjustment != 0 && (*cell(table, head, "Adjustment Account") == '\0'
			? need_account(plan, table, head, plan->adjustment_account)
			: need_account(plan, table, head, cell(table, head, "Adjustment Account"))) && event->posts)
			expect(plan, event->date, *cell(table, head, "Adjustment Account") != '\0'
				? cell(table, head, "Adjustment Account") : plan->adjustment_account, -adjustment);
		if (shipping != 0)
			plan_problem(plan, table->file, head, "a shipping charge on an invoice is not supported; add it as a line");
		/* The entity-level discount column is informational: Zoho also
		 * prices it into each line's total, which is where it is read. */
		if (lines_total + adjustment != total)
			plan_problem(plan, table->file, head, "invoice %s: lines and adjustment do not add up to its total", number);
		if (event->posts)
			expect(plan, event->date, "@ar", total);
	}
}

static void
plan_bills(Plan *plan)
{
	Table *table = plan_table(plan, "Bill.csv");
	g_autoptr(GPtrArray) groups = group_rows(table, "Bill ID");
	guint g, l;
	for (g = 0; g < groups->len; g++)
	{
		GArray *rows = g_ptr_array_index(groups, g);
		guint head = first_row(rows);
		const gchar *status = cell(table, head, "Bill Status");
		Event *event;
		gint64 total, lines_total = 0, adjustment;
		const gchar *adjustment_account = cell(table, head, "Adjustment Account");
		if (document_party(plan, table, head, TRUE) == NULL)
			continue;
		event = event_new(plan, EVENT_BILL, table, head, "Bill Date", cell(table, head, "Bill ID"));
		if (event == NULL)
			continue;
		g_array_append_vals(event->rows, rows->data, rows->len);
		event->status = g_strdup(status);
		event->posts = !is_status(status, "Draft") && !is_status(status, "Void");
		total = amount_at(plan, table, head, "Total");
		adjustment = amount_at(plan, table, head, "Adjustment");
		event->cents = total;
		g_hash_table_insert(plan->bill_ids, (gpointer)cell(table, head, "Bill ID"), event);
		if (*adjustment_account == '\0')
			adjustment_account = "Other Expenses";
		for (l = 0; l < rows->len; l++)
		{
			guint row = g_array_index(rows, guint, l);
			gint64 line_total = amount_at(plan, table, row, "Item Total");
			gint64 tax = amount_at(plan, table, row, "Tax Amount");
			if (line_total < 0 || tax < 0)
				plan_problem(plan, table->file, row, "a negative bill line is not supported; record a vendor credit");
			lines_total += line_total + tax;
			if (!need_account(plan, table, row, cell(table, row, "Account")))
				continue;
			if (tax != 0 && !need_account(plan, table, row, plan->purchase_tax_account))
				continue;
			if (event->posts)
			{
				expect(plan, event->date, cell(table, row, "Account"), line_total);
				expect(plan, event->date, plan->purchase_tax_account, tax);
			}
		}
		if (adjustment != 0 && need_account(plan, table, head, adjustment_account) && event->posts)
			expect(plan, event->date, adjustment_account, adjustment);
		if (lines_total + adjustment != total)
			plan_problem(plan, table->file, head, "bill %s: lines, tax and adjustment do not add up to its total",
				cell(table, head, "Bill Number"));
		if (total <= 0)
			plan_problem(plan, table->file, head, "a bill must have a positive total");
		if (event->posts)
			expect(plan, event->date, "@ap", -total);
	}
}

static void
plan_customer_payments(Plan *plan)
{
	Table *table = plan_table(plan, "Customer_Payment.csv");
	g_autoptr(GPtrArray) groups = group_rows(table, "CustomerPayment ID");
	guint g, l;
	for (g = 0; g < groups->len; g++)
	{
		GArray *rows = g_ptr_array_index(groups, g);
		guint head = first_row(rows);
		Event *event;
		gint64 amount, unused, charges, applied = 0;
		const gchar *deposit = cell(table, head, "Deposit To");
		if (document_party(plan, table, head, FALSE) == NULL)
			continue;
		event = event_new(plan, EVENT_CUSTOMER_PAYMENT, table, head, "Date", cell(table, head, "CustomerPayment ID"));
		if (event == NULL)
			continue;
		g_array_append_vals(event->rows, rows->data, rows->len);
		amount = amount_at(plan, table, head, "Amount");
		unused = amount_at(plan, table, head, "Unused Amount");
		charges = amount_at(plan, table, head, "Bank Charges");
		event->cents = amount;
		if (amount <= 0)
			plan_problem(plan, table->file, head, "a payment must have a positive amount");
		for (l = 0; l < rows->len; l++)
		{
			guint row = g_array_index(rows, guint, l);
			gint64 part = amount_at(plan, table, row, "Amount Applied to Invoice");
			const gchar *number = cell(table, row, "Invoice Number");
			if (part == 0)
				continue;
			if (!g_hash_table_contains(plan->invoice_numbers, number))
				plan_problem(plan, table->file, row, "the payment is applied to invoice %s, which the backup does not hold", number);
			applied += part;
		}
		if (applied + unused != amount)
			plan_problem(plan, table->file, head, "applied and unused amounts do not add up to the payment");
		if (!need_account(plan, table, head, deposit))
			continue;
		if (charges != 0 && !need_account(plan, table, head, plan->bank_charges_account))
			continue;
		expect(plan, event->date, deposit, amount - charges);
		expect(plan, event->date, plan->bank_charges_account, charges);
		expect(plan, event->date, "@ar", -amount);
	}
}

static void
plan_vendor_payments(Plan *plan)
{
	Table *table = plan_table(plan, "Vendor_Payment.csv");
	g_autoptr(GPtrArray) groups = group_rows(table, "VendorPayment ID");
	guint g, l;
	for (g = 0; g < groups->len; g++)
	{
		GArray *rows = g_ptr_array_index(groups, g);
		guint head = first_row(rows);
		Event *event;
		gint64 amount, unused, applied = 0;
		const gchar *paid = cell(table, head, "Paid Through");
		if (document_party(plan, table, head, TRUE) == NULL)
			continue;
		event = event_new(plan, EVENT_VENDOR_PAYMENT, table, head, "Date", cell(table, head, "VendorPayment ID"));
		if (event == NULL)
			continue;
		g_array_append_vals(event->rows, rows->data, rows->len);
		amount = amount_at(plan, table, head, "Amount");
		unused = amount_at(plan, table, head, "Unused Amount");
		event->cents = amount;
		if (amount <= 0)
			plan_problem(plan, table->file, head, "a payment must have a positive amount");
		if (amount_at(plan, table, head, "Bank Charges") != 0)
			plan_problem(plan, table->file, head, "bank charges on a vendor payment are not supported; record them as an expense");
		for (l = 0; l < rows->len; l++)
		{
			guint row = g_array_index(rows, guint, l);
			gint64 part = amount_at(plan, table, row, "Bill Amount");
			if (part == 0)
				continue;
			if (!g_hash_table_contains(plan->bill_ids, cell(table, row, "Bill ID")))
				plan_problem(plan, table->file, row, "the payment is applied to bill %s, which the backup does not hold",
					cell(table, row, "Bill Number"));
			applied += part;
		}
		if (applied + unused != amount)
			plan_problem(plan, table->file, head, "applied and unused amounts do not add up to the payment");
		if (!need_account(plan, table, head, paid))
			continue;
		expect(plan, event->date, paid, -amount);
		expect(plan, event->date, "@ap", amount);
	}
}

static void
plan_expenses(Plan *plan)
{
	Table *table = plan_table(plan, "Expense.csv");
	guint i;
	if (table == NULL)
		return;
	for (i = 0; table != NULL && i < table->rows->len; i++)
	{
		const gchar *account = cell(table, i, "Expense Account");
		const gchar *paid = cell(table, i, "Paid Through");
		g_autofree gchar *source = NULL;
		Event *event;
		gint64 total = amount_at(plan, table, i, "Total");
		gint64 tax = amount_at(plan, table, i, "Tax Amount");
		/* Expenses carry no id in the backup; the entry number, else the
		 * row, is stable for a given export. */
		source = *cell(table, i, "Entry Number") != '\0'
			? g_strdup_printf("entry-%s", cell(table, i, "Entry Number"))
			: g_strdup_printf("row-%u", i + 2);
		event = event_new(plan, EVENT_EXPENSE, table, i, "Expense Date", source);
		if (event == NULL)
			continue;
		g_array_append_val(event->rows, i);
		event->cents = total;
		if (total <= 0 || tax < 0 || tax >= total)
			plan_problem(plan, table->file, i, "an expense needs a positive total above its tax");
		if (!need_account(plan, table, i, account) || !need_account(plan, table, i, paid) ||
			(tax != 0 && !need_account(plan, table, i, plan->purchase_tax_account)))
			continue;
		expect(plan, event->date, account, total - tax);
		expect(plan, event->date, plan->purchase_tax_account, tax);
		expect(plan, event->date, control_key(plan, paid), -total);
	}
}

static void
plan_money_moves(Plan *plan, const gchar *file)
{
	Table *table = plan_table(plan, file);
	guint i;
	for (i = 0; table != NULL && i < table->rows->len; i++)
	{
		g_autofree gchar *source = g_strdup_printf("%s-row-%u", g_str_has_prefix(file, "Deposit") ? "deposit" : "transfer", i + 2);
		const gchar *from = cell(table, i, "From Account");
		const gchar *to = cell(table, i, "To Account");
		gint64 total = amount_at(plan, table, i, "Total");
		Event *event = event_new(plan, EVENT_MONEY_MOVE, table, i, "Transaction Date", source);
		if (event == NULL)
			continue;
		g_array_append_val(event->rows, i);
		event->cents = total;
		if (total <= 0)
			plan_problem(plan, table->file, i, "a transfer needs a positive total");
		if (!need_account(plan, table, i, from) || !need_account(plan, table, i, to))
			continue;
		expect(plan, event->date, control_key(plan, to), total);
		expect(plan, event->date, control_key(plan, from), -total);
	}
}

static void
plan_journals(Plan *plan)
{
	Table *table = plan_table(plan, "Journal.csv");
	g_autoptr(GPtrArray) groups = group_rows(table, "Journal ID");
	guint g, l;
	for (g = 0; g < groups->len; g++)
	{
		GArray *rows = g_ptr_array_index(groups, g);
		guint head = first_row(rows);
		gint64 sum = 0;
		Event *event;
		if (!is_status(cell(table, head, "Status"), "Published") && *cell(table, head, "Status") != '\0')
		{
			g_ptr_array_add(plan->warnings, g_strdup_printf("Journal.csv row %u: journal %s is %s and is not posted",
				head + 2, cell(table, head, "Journal Number"), cell(table, head, "Status")));
			continue;
		}
		event = event_new(plan, EVENT_JOURNAL, table, head, "Journal Date", cell(table, head, "Journal ID"));
		if (event == NULL)
			continue;
		g_array_append_vals(event->rows, rows->data, rows->len);
		for (l = 0; l < rows->len; l++)
		{
			guint row = g_array_index(rows, guint, l);
			gint64 debit = amount_at(plan, table, row, "Debit");
			gint64 credit = amount_at(plan, table, row, "Credit");
			const gchar *account = cell(table, row, "Account");
			if (!need_account(plan, table, row, account))
				continue;
			if (*cell(table, row, "Contact Name") != '\0' && plan_account(plan, account) != NULL &&
				plan_account(plan, account)->role != ROLE_NONE)
				plan_problem(plan, table->file, row, "a journal line against receivables or payables for a contact is not supported");
			sum += debit - credit;
			expect(plan, event->date, account, debit - credit);
		}
		if (sum != 0)
			plan_problem(plan, table->file, head, "journal %s does not balance", cell(table, head, "Journal Number"));
	}
}

static void
plan_credit_notes(Plan *plan)
{
	Table *table = plan_table(plan, "Credit_Note.csv");
	g_autoptr(GPtrArray) groups = group_rows(table, "CreditNotes ID");
	Table *applied = plan_table(plan, "Creditnotes_Invoice.csv");
	Table *refunds = plan_table(plan, "Refund.csv");
	guint g, l;
	for (g = 0; g < groups->len; g++)
	{
		GArray *rows = g_ptr_array_index(groups, g);
		guint head = first_row(rows);
		const gchar *status = cell(table, head, "Credit Note Status");
		Event *event;
		gint64 total;
		if (document_party(plan, table, head, FALSE) == NULL)
			continue;
		event = event_new(plan, EVENT_CREDIT_NOTE, table, head, "Credit Note Date", cell(table, head, "CreditNotes ID"));
		if (event == NULL)
			continue;
		g_array_append_vals(event->rows, rows->data, rows->len);
		event->status = g_strdup(status);
		event->posts = !is_status(status, "Draft") && !is_status(status, "Void");
		total = amount_at(plan, table, head, "Total");
		event->cents = total;
		g_hash_table_insert(plan->credit_numbers, (gpointer)cell(table, head, "Credit Note Number"), event);
		for (l = 0; l < rows->len; l++)
		{
			guint row = g_array_index(rows, guint, l);
			if (amount_at(plan, table, row, "Item Tax Amount") != 0)
				plan_problem(plan, table->file, row, "tax on a credit note line is not supported");
			if (!need_account(plan, table, row, cell(table, row, "Account")))
				continue;
			if (event->posts)
				expect(plan, event->date, cell(table, row, "Account"), amount_at(plan, table, row, "Item Total"));
		}
		if (event->posts)
			expect(plan, event->date, "@ar", -total);
	}
	for (l = 0; applied != NULL && l < applied->rows->len; l++)
	{
		g_autofree gchar *source = g_strdup_printf("credit-application-row-%u", l + 2);
		Event *event;
		if (!g_hash_table_contains(plan->credit_numbers, cell(applied, l, "Credit Note Number")) ||
			!g_hash_table_contains(plan->invoice_numbers, cell(applied, l, "Invoice Number")))
		{
			plan_problem(plan, applied->file, l, "a credit application names a credit note or invoice the backup does not hold");
			continue;
		}
		event = event_new(plan, EVENT_CREDIT_APPLICATION, applied, l, "Date", source);
		if (event == NULL)
			continue;
		g_array_append_val(event->rows, l);
		event->cents = amount_at(plan, applied, l, "Amount");
	}
	for (l = 0; refunds != NULL && l < refunds->rows->len; l++)
	{
		g_autofree gchar *source = g_strdup_printf("refund-row-%u", l + 2);
		const gchar *from = cell(refunds, l, "Refund From Account");
		Event *event;
		gint64 amount = amount_at(plan, refunds, l, "Amount");
		if (!g_hash_table_contains(plan->credit_numbers, cell(refunds, l, "Credit Note Number")))
		{
			plan_problem(plan, refunds->file, l, "a refund names a credit note the backup does not hold");
			continue;
		}
		event = event_new(plan, EVENT_REFUND, refunds, l, "Date", source);
		if (event == NULL)
			continue;
		g_array_append_val(event->rows, l);
		event->cents = amount;
		if (!need_account(plan, refunds, l, from))
			continue;
		expect(plan, event->date, "@ar", amount);
		expect(plan, event->date, from, -amount);
	}
}

static void
plan_vendor_credits(Plan *plan)
{
	Table *table = plan_table(plan, "Vendor_Credits.csv");
	g_autoptr(GPtrArray) groups = group_rows(table, "Vendor Credit ID");
	Table *refunds = plan_table(plan, "Vendor_Credits_Refund.csv");
	guint g, l;
	for (g = 0; g < groups->len; g++)
	{
		GArray *rows = g_ptr_array_index(groups, g);
		guint head = first_row(rows);
		const gchar *status = cell(table, head, "Vendor Credit Status");
		Event *event;
		gint64 total;
		if (document_party(plan, table, head, TRUE) == NULL)
			continue;
		event = event_new(plan, EVENT_VENDOR_CREDIT, table, head, "Vendor Credit Date", cell(table, head, "Vendor Credit ID"));
		if (event == NULL)
			continue;
		g_array_append_vals(event->rows, rows->data, rows->len);
		event->status = g_strdup(status);
		event->posts = !is_status(status, "Draft") && !is_status(status, "Void");
		total = amount_at(plan, table, head, "Total");
		event->cents = total;
		g_hash_table_insert(plan->credit_numbers, (gpointer)cell(table, head, "Vendor Credit Number"), event);
		for (l = 0; l < rows->len; l++)
		{
			guint row = g_array_index(rows, guint, l);
			if (amount_at(plan, table, row, "Item Tax Amount") != 0)
				plan_problem(plan, table->file, row, "tax on a vendor credit line is not supported");
			if (!need_account(plan, table, row, cell(table, row, "Account")))
				continue;
			if (event->posts)
				expect(plan, event->date, cell(table, row, "Account"), -amount_at(plan, table, row, "Item Total"));
		}
		if (event->posts)
			expect(plan, event->date, "@ap", total);
		if (amount_at(plan, table, head, "Balance") != total && event->posts)
			plan_problem(plan, table->file, head, "an applied vendor credit is not supported: the backup does not say which bills it settled");
	}
	for (l = 0; refunds != NULL && l < refunds->rows->len; l++)
	{
		g_autofree gchar *source = g_strdup_printf("vendor-refund-row-%u", l + 2);
		const gchar *to = cell(refunds, l, "Refund Account");
		Event *event;
		gint64 amount = amount_at(plan, refunds, l, "Amount");
		if (!g_hash_table_contains(plan->credit_numbers, cell(refunds, l, "Vendor Credit Number")))
		{
			plan_problem(plan, refunds->file, l, "a vendor refund names a vendor credit the backup does not hold");
			continue;
		}
		event = event_new(plan, EVENT_VENDOR_REFUND, refunds, l, "Date", source);
		if (event == NULL)
			continue;
		g_array_append_val(event->rows, l);
		event->cents = amount;
		if (!need_account(plan, refunds, l, to))
			continue;
		expect(plan, event->date, to, amount);
		expect(plan, event->date, "@ap", -amount);
	}
}

/* The day-one balances: rows of the manifest's "opening" naming a Zoho
 * account and a debit or a credit. They enter through a cutover batch at
 * the start date, exactly as an opening-balance migration would. */
static void
plan_opening(Plan *plan, JsonObject *manifest)
{
	JsonArray *rows;
	gint64 sum = 0;
	guint i;
	if (!json_object_has_member(manifest, "opening"))
		return;
	if (!JSON_NODE_HOLDS_ARRAY(json_object_get_member(manifest, "opening")))
	{
		plan_problem(plan, NULL, 0, "opening must be an array of {account, debit | credit}");
		return;
	}
	rows = json_object_get_array_member(manifest, "opening");
	for (i = 0; i < json_array_get_length(rows); i++)
	{
		JsonNode *node = json_array_get_element(rows, i);
		JsonObject *row;
		const gchar *name, *debit, *credit;
		gint64 dr = 0, cr = 0;
		Posting *posting;
		if (!JSON_NODE_HOLDS_OBJECT(node))
		{
			plan_problem(plan, NULL, 0, "opening[%u] is not an object", i);
			continue;
		}
		row = json_node_get_object(node);
		name = json_object_get_string_member_with_default(row, "account", "");
		debit = json_object_get_string_member_with_default(row, "debit", "");
		credit = json_object_get_string_member_with_default(row, "credit", "");
		if (plan_account(plan, name) == NULL)
		{
			plan_problem(plan, NULL, 0, "opening[%u]: account \"%s\" is not in Chart_of_Accounts.csv", i, name);
			continue;
		}
		if (!parse_amount(debit, plan->exponent, &dr) || !parse_amount(credit, plan->exponent, &cr) || dr < 0 || cr < 0 ||
			(dr != 0 && cr != 0))
		{
			plan_problem(plan, NULL, 0, "opening[%u]: give one nonnegative debit or credit", i);
			continue;
		}
		if (plan_account(plan, name)->role != ROLE_NONE && dr - cr != 0)
		{
			plan_problem(plan, NULL, 0, "opening[%u]: open receivables and payables at the start are not supported; "
				"import them as documents", i);
			continue;
		}
		posting = g_new0(Posting, 1);
		posting->date = g_strdup(plan->start_text);
		posting->account = g_strdup(plan_account(plan, name)->name);
		posting->cents = dr - cr;
		sum += posting->cents;
		g_ptr_array_add(plan->opening, posting);
		expect(plan, plan->start_text, posting->account, posting->cents);
	}
	if (sum != 0)
		plan_problem(plan, NULL, 0, "the opening rows do not balance: debits less credits is %" G_GINT64_FORMAT " minor units", sum);
}

/* Files with no home in VENTURE yet are listed, not refused: the owner sees
 * exactly what stays behind. An empty file is not worth a line. */
static void
plan_unsupported(Plan *plan)
{
	static const struct { const gchar *file; const gchar *why; } homeless[] = {
		{ "Activity Logs.csv", "Zoho's own audit trail; VENTURE keeps its own from the import onward" },
		{ "Budget.csv", "budgets are set up for the year ahead, not imported" },
		{ "Bill_Of_Entry.csv", "import customs entries have no VENTURE record" },
		{ "Delivery_Challan.csv", "delivery challans have no VENTURE record" },
		{ "Exchange_Rate.csv", "single-currency books need no rates" },
		{ "Inventory_Adjustment.csv", "stock adjustments need the goods module's own counts" },
		{ "Price_lists.csv", "price lists are set up in VENTURE" },
		{ "Purchase_Price_lists.csv", "price lists are set up in VENTURE" },
		{ "Replenishment.csv", "reorder rules are set up in VENTURE" },
		{ "Sales_Receipt.csv", "sales receipts are not imported yet" },
		{ "Task.csv", "tasks are imported by the CRM migration" },
		{ "Tax.csv", "taxes are set up in VENTURE's tax codes" },
		{ "Tax_Authority.csv", "tax authorities are set up in VENTURE's tax codes" },
		{ "Tax_Exemption.csv", "exemption reasons are set up in VENTURE" },
		{ "Tax_Group.csv", "tax groups are set up in VENTURE's tax codes" },
		{ "Recurring_Invoice.csv", "recurring profiles are set up again in VENTURE's recurring schedules; their past invoices are imported" },
		{ "Recurring_Bill.csv", "recurring profiles are set up again in VENTURE's recurring schedules; their past bills are imported" },
		{ "Recurring_Expense.csv", "recurring profiles are set up again in VENTURE's recurring schedules; their past expenses are imported" },
		{ "Fixed_Asset.csv", "fixed assets are placed in service in VENTURE's asset register" },
		{ "Projects.csv", "projects are not imported yet" },
		{ "Project_Tasks.csv", "project tasks have no VENTURE record" },
		{ "Timesheet.csv", "timesheets are not imported yet" },
		{ "Quote.csv", "quotes are not imported yet" },
		{ "Sales_Order.csv", "sales orders are not imported yet" },
		{ "Purchase_Order.csv", "purchase orders are not imported yet" },
		{ "Item.csv", "items are not imported yet; invoice and bill lines keep their descriptions" },
	};
	guint i;
	for (i = 0; i < G_N_ELEMENTS(homeless); i++)
	{
		Table *table = plan_table(plan, homeless[i].file);
		if (table != NULL && table->rows->len > 0)
			g_ptr_array_add(plan->unsupported, g_strdup_printf("%s: %u rows not imported (%s)",
				homeless[i].file, table->rows->len, homeless[i].why));
	}
}

static gint
event_compare(gconstpointer a, gconstpointer b)
{
	const Event *x = *(const Event *const *)a;
	const Event *y = *(const Event *const *)b;
	gint by_date = strcmp(x->date, y->date);
	if (by_date != 0)
		return by_date;
	/* Documents before the money that settles them, then the order the
	 * backup lists them in. */
	if (x->kind != y->kind)
		return (gint)x->kind - (gint)y->kind;
	return (gint)x->order - (gint)y->order;
}

static const gchar *
setting(JsonObject *manifest, const gchar *name, const gchar *fallback)
{
	return json_object_get_string_member_with_default(manifest, name, fallback);
}

/*
 * Builds the plan. Refuses only what cannot be read at all: an unknown
 * source, a missing or unreadable start, no files, a file that is not CSV.
 * Everything row-shaped becomes a located problem on the plan.
 */
static Plan *
plan_build(VentureBooksImportService *self, gint64 org, JsonObject *manifest, GError **error)
{
	g_autoptr(Plan) plan = g_new0(Plan, 1);
	g_autoptr(VentureMoney) unit = NULL;
	JsonObject *files;
	JsonObject *map = NULL;
	GList *names, *l;
	const gchar *source = setting(manifest, "source", "");
	if (g_strcmp0(source, "zoho_books") != 0)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"VentureBooksImportService: source must be zoho_books, not \"%s\"", source);
		return NULL;
	}
	plan->self = self;
	plan->org = org;
	plan->currency = g_strdup(setting(manifest, "currency", venture_money_get_default_currency()));
	unit = venture_money_new_zero(plan->currency);
	plan->exponent = venture_money_get_exponent(unit);
	plan->start_text = g_strdup(setting(manifest, "start", ""));
	plan->start = parse_date(plan->start_text);
	if (plan->start == NULL)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"VentureBooksImportService: start must be the source's first day, YYYY-MM-DD");
		return NULL;
	}
	plan->export_text = g_strdup(setting(manifest, "export_date", ""));
	plan->export_date = *plan->export_text != '\0' ? parse_date(plan->export_text) : NULL;
	if (*plan->export_text != '\0' && plan->export_date == NULL)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"VentureBooksImportService: export_date must be YYYY-MM-DD");
		return NULL;
	}
	if (plan->export_date != NULL)
	{
		g_autoptr(GDateTime) now = venture_time_now();
		if (g_date_time_compare(plan->export_date, now) > 0)
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
				"VentureBooksImportService: export_date is in the future");
			return NULL;
		}
	}
	if (!json_object_has_member(manifest, "files") || !JSON_NODE_HOLDS_OBJECT(json_object_get_member(manifest, "files")))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"VentureBooksImportService: files must map each backup file name to its CSV text");
		return NULL;
	}
	if (json_object_has_member(manifest, "accounts") && JSON_NODE_HOLDS_OBJECT(json_object_get_member(manifest, "accounts")))
		map = json_object_get_object_member(manifest, "accounts");
	plan->bank_charges_account = g_strdup(setting(manifest, "bank_charges_account", "Bank Fees and Charges"));
	plan->purchase_tax_account = g_strdup(setting(manifest, "purchase_tax_account", "Tax Paid Expense"));
	plan->discount_account = g_strdup(setting(manifest, "discount_account", "Discount"));
	plan->adjustment_account = g_strdup(setting(manifest, "adjustment_account", "Other Charges"));
	plan->contact_details = json_object_get_boolean_member_with_default(manifest, "contact_details", FALSE);
	plan->tables = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, table_free);
	plan->accounts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, account_free);
	plan->customers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, party_free);
	plan->vendors = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, party_free);
	plan->customer_names = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	plan->vendor_names = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	plan->events = g_ptr_array_new_with_free_func(event_free);
	plan->expected = g_ptr_array_new_with_free_func(posting_free);
	plan->opening = g_ptr_array_new_with_free_func(posting_free);
	plan->problems = g_ptr_array_new_with_free_func(g_free);
	plan->warnings = g_ptr_array_new_with_free_func(g_free);
	plan->unsupported = g_ptr_array_new_with_free_func(g_free);
	plan->persons = g_ptr_array_new_with_free_func(person_free);
	plan->invoice_numbers = g_hash_table_new(g_str_hash, g_str_equal);
	plan->bill_ids = g_hash_table_new(g_str_hash, g_str_equal);
	plan->credit_numbers = g_hash_table_new(g_str_hash, g_str_equal);
	files = json_object_get_object_member(manifest, "files");
	names = json_object_get_members(files);
	for (l = names; l != NULL; l = l->next)
	{
		const gchar *name = l->data;
		JsonNode *node = json_object_get_member(files, name);
		Table *table;
		if (!JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_STRING)
		{
			g_list_free(names);
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
				"VentureBooksImportService: files.%s must be CSV text", name);
			return NULL;
		}
		table = table_parse(name, json_node_get_string(node), error);
		if (table == NULL)
		{
			g_list_free(names);
			g_prefix_error(error, "VentureBooksImportService: ");
			return NULL;
		}
		g_hash_table_insert(plan->tables, g_strdup(name), table);
	}
	g_list_free(names);
	plan_chart(plan, map);
	if (plan_account(plan, plan->bank_charges_account) == NULL && plan_table(plan, "Customer_Payment.csv") != NULL &&
		has_column(plan_table(plan, "Customer_Payment.csv"), "Bank Charges"))
		g_ptr_array_add(plan->warnings, g_strdup_printf("bank charges account \"%s\" is not in the chart; payments with bank charges will be refused",
			plan->bank_charges_account));
	plan_parties(plan, "Contacts.csv", FALSE);
	plan_parties(plan, "Vendors.csv", TRUE);
	plan_persons(plan);
	plan_opening(plan, manifest);
	plan_invoices(plan);
	plan_bills(plan);
	plan_credit_notes(plan);
	plan_vendor_credits(plan);
	plan_customer_payments(plan);
	plan_vendor_payments(plan);
	plan_expenses(plan);
	plan_money_moves(plan, "Deposit.csv");
	plan_money_moves(plan, "Transfer_Fund.csv");
	plan_journals(plan);
	plan_unsupported(plan);
	g_ptr_array_sort(plan->events, event_compare);
	if (json_object_has_member(manifest, "unsupported") && JSON_NODE_HOLDS_ARRAY(json_object_get_member(manifest, "unsupported")))
	{
		JsonArray *extra = json_object_get_array_member(manifest, "unsupported");
		guint i;
		for (i = 0; i < json_array_get_length(extra); i++)
		{
			JsonNode *node = json_array_get_element(extra, i);
			if (JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING)
				g_ptr_array_add(plan->unsupported, g_strdup(json_node_get_string(node)));
		}
	}
	return g_steal_pointer(&plan);
}
