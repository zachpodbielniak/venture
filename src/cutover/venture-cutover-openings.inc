/* SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Importing a parsed plan: parties, the chart, opening AR and AP, credits and
 * prepayments, bank balances, fixed assets and the trial balance. Included by
 * venture-cutover-service.c after the payload parser; every document posts
 * through its owning service, never through hand-written ledger rows.
 *
 * Every opening posting has the opening balance clearing account on its other
 * side and is dated at the cutoff instant. Subledger documents keep their
 * source dates for aging and statements, but none of them touches income,
 * expense or tax: those belong to the source system's books. Once the trial
 * balance is in, clearing nets to zero in every currency -- which is exactly
 * the proof that the subledgers agree with the source ledger.
 */

typedef struct
{
	GHashTable *by_external;
	GHashTable *by_name;
	GHashTable *by_created_name;
} PartyIndex;

static void
party_index_free(PartyIndex *index)
{
	if (index == NULL)
		return;
	g_clear_pointer(&index->by_external, g_hash_table_unref);
	g_clear_pointer(&index->by_name, g_hash_table_unref);
	g_clear_pointer(&index->by_created_name, g_hash_table_unref);
	g_free(index);
}
G_DEFINE_AUTOPTR_CLEANUP_FUNC(PartyIndex, party_index_free)

typedef struct
{
	Plan *plan;
	VentureEntity *cutover;
	gint64 cutover_id;
	gint64 clearing;
	PartyIndex *parties;
	const VentureActor *actor;
} Import;

/* Case and runs of whitespace do not distinguish two companies. */
static gchar *
normalize_name(const gchar *name)
{
	g_autofree gchar *folded = NULL;
	g_autoptr(GString) text = g_string_new(NULL);
	gboolean space = FALSE;
	const gchar *p;
	if (name == NULL)
		return NULL;
	folded = g_utf8_casefold(name, -1);
	for (p = folded; *p != '\0'; p = g_utf8_next_char(p))
	{
		gunichar c = g_utf8_get_char(p);
		if (g_unichar_isspace(c))
		{
			space = text->len > 0;
			continue;
		}
		if (space)
			g_string_append_c(text, ' ');
		space = FALSE;
		g_string_append_unichar(text, c);
	}
	return g_string_free(g_steal_pointer(&text), FALSE);
}

static void
index_add(GHashTable *table, const gchar *key, gint64 id)
{
	GArray *ids;
	if (key == NULL || *key == '\0')
		return;
	ids = g_hash_table_lookup(table, key);
	if (ids == NULL)
	{
		ids = g_array_new(FALSE, FALSE, sizeof(gint64));
		g_hash_table_insert(table, g_strdup(key), ids);
	}
	g_array_append_val(ids, id);
}

static PartyIndex *
party_index_build(VentureCutoverService *self, gint64 org, GError **error)
{
	g_autoptr(PartyIndex) index = g_new0(PartyIndex, 1);
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_COMPANY);
	g_autoptr(GPtrArray) companies = NULL;
	guint i;
	index->by_external = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_array_unref);
	index->by_name = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_array_unref);
	index->by_created_name = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	venture_query_set_organization(query, org);
	venture_query_set_limit(query, 0);
	companies = venture_database_find(self->database, query, error);
	if (companies == NULL)
		return NULL;
	for (i = 0; i < companies->len; i++)
	{
		VentureEntity *company = g_ptr_array_index(companies, i);
		g_autofree gchar *name = NULL;
		g_autofree gchar *external = NULL;
		g_autofree gchar *normal = NULL;
		g_object_get(company, "name", &name, "external-id", &external, NULL);
		normal = normalize_name(name);
		index_add(index->by_external, external, venture_entity_get_id(company));
		index_add(index->by_name, normal, venture_entity_get_id(company));
	}
	return g_steal_pointer(&index);
}

/* The live row that imported @source_id as @source_type: imported or
 * matched, not rolled back or skipped. */
static VentureEntity *
live_row(VentureCutoverService *self, gint64 org, const gchar *source_id, const gchar *source_type, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ACCOUNTING_CUTOVER_ROW);
	g_autoptr(GPtrArray) rows = NULL;
	guint i;
	if (source_id == NULL || *source_id == '\0')
		return NULL;
	venture_query_set_organization(query, org);
	venture_query_set_limit(query, 0);
	if (!venture_query_add_filter_string(query, "source-id", VENTURE_FILTER_OP_EQ, source_id, error))
		return NULL;
	rows = venture_database_find(self->database, query, error);
	if (rows == NULL)
		return NULL;
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(rows, i);
		g_autofree gchar *type = NULL;
		g_autofree gchar *status = NULL;
		gint64 record_id = 0;
		g_object_get(row, "source-type", &type, "record-id", &record_id, "status", &status, NULL);
		if (g_strcmp0(type, source_type) == 0 && record_id > 0 &&
			(g_strcmp0(status, "imported") == 0 || g_strcmp0(status, "matched") == 0))
			return g_object_ref(row);
	}
	return NULL;
}

static gint64
row_cutover(VentureEntity *row)
{
	gint64 id = 0;
	g_object_get(row, "cutover-id", &id, NULL);
	return id;
}

static gint64
row_record(VentureEntity *row)
{
	gint64 id = 0;
	g_object_get(row, "record-id", &id, NULL);
	return id;
}

/*
 * Resolves a row's party. Order: an earlier cutover row for the same source
 * id, a company whose external ID is the party's, a company with the same
 * name ignoring case and spacing. A row with only a name reuses one company
 * per name. With @create FALSE nothing is written: 0 means "will be created".
 * Returns: the company id, 0, or -1 with a located problem on @row.
 */
static gint64
resolve_party(Plan *plan, PartyIndex *index, Row *row, Import *import, GError **error)
{
	const gchar *type = row->vendor ? "vendor" : "customer";
	JsonObject *details = row->party_source != NULL
		? g_hash_table_lookup(row->vendor ? plan->vendors : plan->customers, row->party_source) : NULL;
	g_autofree gchar *name = NULL;
	g_autofree gchar *external = NULL;
	g_autofree gchar *normal = NULL;
	g_autoptr(VentureEntity) live = NULL;
	g_autoptr(VentureCompany) company = NULL;
	GArray *ids;
	gint64 id = 0;
	if (row->party_source != NULL)
	{
		live = live_row(plan->self, plan->org, row->party_source, type, error);
		if (live == NULL && error != NULL && *error != NULL)
			return -1;
		if (live != NULL)
			id = row_record(live);
	}
	if (details != NULL)
	{
		JsonNode *node = member(details, "name");
		if (node != NULL && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING)
			name = g_strdup(json_node_get_string(node));
		node = member(details, "external_id");
		if (node != NULL && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING)
			external = g_strdup(json_node_get_string(node));
	}
	if (name == NULL)
		name = g_strdup(row->party_name);
	if (external == NULL)
		external = g_strdup(row->party_source);
	normal = normalize_name(name);
	if (id == 0 && external != NULL && (ids = g_hash_table_lookup(index->by_external, external)) != NULL)
	{
		if (ids->len > 1)
		{
			row_error(plan, row, "%s external id %s matches %u companies; make it unique", type, external, ids->len);
			return -1;
		}
		id = g_array_index(ids, gint64, 0);
	}
	if (id == 0 && row->party_source == NULL && normal != NULL)
	{
		gint64 *created = g_hash_table_lookup(index->by_created_name, normal);
		if (created != NULL)
			id = *created;
	}
	if (id == 0 && normal != NULL && (ids = g_hash_table_lookup(index->by_name, normal)) != NULL)
	{
		if (ids->len > 1)
		{
			row_error(plan, row, "%s %s matches %u companies by name; give them distinct external ids or map the row to one",
				type, name, ids->len);
			return -1;
		}
		id = g_array_index(ids, gint64, 0);
	}
	if (id > 0 && row->vendor && import == NULL)
	{
		g_autoptr(VentureEntity) existing = venture_database_get(plan->self->database, VENTURE_TYPE_COMPANY, id, NULL);
		gint kind = 0;
		if (existing != NULL)
			g_object_get(existing, "kind", &kind, NULL);
		if (existing != NULL && kind != VENTURE_COMPANY_KIND_SUPPLIER)
			row_warning(plan, row, "company %" G_GINT64_FORMAT " matched for this vendor will be marked a supplier", id);
	}
	if (import == NULL)
	{
		if (id == 0 && name == NULL)
			row_warning(plan, row, "%s %s is not listed under %ss; the company will be named after its source id",
				type, row->party_source, type);
		return id;
	}
	if (id == 0)
	{
		gint64 *slot;
		company = venture_company_new();
		g_object_set(company, "name", name != NULL ? name : row->party_source, "external-id", external,
			"active", TRUE, NULL);
		if (row->vendor)
			g_object_set(company, "kind", VENTURE_COMPANY_KIND_SUPPLIER, NULL);
		if (details != NULL)
		{
			static const gchar *const copied[] = { "email", "phone", "website", "address" };
			g_autofree gchar *terms = NULL;
			gboolean exempt = FALSE;
			guint i;
			for (i = 0; i < G_N_ELEMENTS(copied); i++)
			{
				JsonNode *node = member(details, copied[i]);
				if (node != NULL && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING)
					g_object_set(company, copied[i], json_node_get_string(node), NULL);
			}
			/* Companies have no terms field; keep the source's terms where
			 * a person collecting on the account will read them. */
			terms = field_text(plan, row, details, "", "terms", FALSE);
			if (terms != NULL)
			{
				g_autofree gchar *note = g_strdup_printf("Payment terms (%s): %s", plan->source, terms);
				g_object_set(company, "notes", note, NULL);
			}
			if (field_bool(plan, row, details, "tax_exempt", &exempt) && exempt)
				g_object_set(company, "tax-exempt", TRUE, NULL);
		}
		venture_entity_set_organization_id(VENTURE_ENTITY(company), plan->org);
		if (!venture_database_save(plan->self->database, VENTURE_ENTITY(company), import->actor, error))
			return -1;
		id = venture_entity_get_id(VENTURE_ENTITY(company));
		index_add(index->by_external, external, id);
		index_add(index->by_name, normal, id);
		if (row->party_source == NULL && normal != NULL)
		{
			slot = g_new(gint64, 1);
			*slot = id;
			g_hash_table_insert(index->by_created_name, g_strdup(normal), slot);
		}
		if (row->party_source != NULL && !add_row(plan->self, import->cutover_id, plan->org, row->party_source, type,
			"imported", NULL, "company", id, import->actor, error))
			return -1;
	}
	else if (live == NULL && row->party_source != NULL)
	{
		if (!add_row(plan->self, import->cutover_id, plan->org, row->party_source, type, "matched", NULL,
			"company", id, import->actor, error))
			return -1;
	}
	if (row->vendor)
	{
		g_autoptr(VentureEntity) existing = venture_database_get(plan->self->database, VENTURE_TYPE_COMPANY, id, error);
		gint kind = 0;
		if (existing == NULL)
			return -1;
		g_object_get(existing, "kind", &kind, NULL);
		/* Payables requires a supplier; receivables does not look at kind. */
		if (kind != VENTURE_COMPANY_KIND_SUPPLIER)
		{
			g_object_set(existing, "kind", VENTURE_COMPANY_KIND_SUPPLIER, NULL);
			if (!venture_database_save(plan->self->database, existing, import->actor, error))
				return -1;
		}
	}
	return id;
}

/* A party that could not be resolved at import either raised a GError or
 * left a located problem on its row; report whichever it was. */
static gboolean
party_failed(Plan *plan, GError **error)
{
	if (error != NULL && *error != NULL)
		return FALSE;
	if (plan_refuse_errors(plan, error))
		refuse(error, "a party could not be resolved");
	return FALSE;
}

/* --- Accounts ------------------------------------------------------------------ */

static gint64
import_account(Import *import, const gchar *code, const gchar *what, GError **error)
{
	g_autoptr(VentureEntity) account = account_with_code(import->plan->self, import->plan->org, code, error);
	if (account == NULL)
	{
		if (error == NULL || *error == NULL)
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
				"VentureCutoverService: %s account code %s is missing from the chart", what, code != NULL ? code : "(none)");
		return 0;
	}
	return venture_entity_get_id(account);
}

/* The clearing account holds the other side of every opening posting until
 * the trial balance moves it into the accounts it belongs to. It is created
 * inside the import transaction when the chart lacks it. */
static gint64
clearing_account(Plan *plan, gboolean create, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) account = account_with_code(plan->self, plan->org, plan->clearing_code, error);
	gint kind = 0;
	gboolean active = FALSE;
	if (account == NULL && error != NULL && *error != NULL)
		return 0;
	if (account == NULL)
	{
		if (!create)
			return 0;
		account = VENTURE_ENTITY(venture_account_new());
		g_object_set(account, "code", plan->clearing_code, "name", "Opening balance clearing",
			"kind", VENTURE_ACCOUNT_KIND_EQUITY, "active", TRUE, NULL);
		venture_entity_set_organization_id(account, plan->org);
		if (!venture_database_save(plan->self->database, account, actor, error))
			return 0;
		return venture_entity_get_id(account);
	}
	g_object_get(account, "kind", &kind, "active", &active, NULL);
	if (kind != VENTURE_ACCOUNT_KIND_EQUITY || !active)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"VentureCutoverService: account %s is not an active equity account; choose another clearing_account_code",
			plan->clearing_code);
		return 0;
	}
	return venture_entity_get_id(account);
}

static gboolean
import_chart(Import *import, GError **error)
{
	Plan *plan = import->plan;
	guint i;
	for (i = 0; i < plan->chart->len; i++)
	{
		ChartRow *chart = g_ptr_array_index(plan->chart, i);
		g_autoptr(VentureEntity) account = account_with_code(plan->self, plan->org, chart->code, error);
		if (account != NULL)
			continue;
		if (error != NULL && *error != NULL)
			return FALSE;
		account = VENTURE_ENTITY(venture_account_new());
		g_object_set(account, "code", chart->code, "name", chart->name, "kind", chart->kind,
			"active", chart->active, NULL);
		venture_entity_set_organization_id(account, plan->org);
		if (!venture_database_save(plan->self->database, account, import->actor, error))
			return FALSE;
	}
	return TRUE;
}

/*
 * Receivables, payables and every bank account a bank row names: accounts a
 * subledger establishes. A trial balance row on one of them is asserted at
 * reconcile, never posted, or the balance would be counted twice.
 */
static gint64
control_account(Plan *plan, const gchar *role, const gchar *code, GError **error)
{
	g_autofree gchar *scoped = g_strdup_printf("%" G_GINT64_FORMAT ":%s", plan->org, code);
	g_autoptr(VentureEntity) account = NULL;
	gint64 id = venture_setup_resolve_account(plan->self->database, plan->org, role, "organization", 0, NULL, error);
	if (id != 0 || (error != NULL && *error != NULL))
		return id;
	account = account_with_code(plan->self, plan->org, code, error);
	if (account == NULL && (error == NULL || *error == NULL))
		account = account_with_code(plan->self, plan->org, scoped, error);
	return account != NULL ? venture_entity_get_id(account) : 0;
}

static GHashTable *
control_accounts(Plan *plan, GError **error)
{
	g_autoptr(GHashTable) controls = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, g_free);
	gint64 ids[2];
	guint i;
	ids[0] = control_account(plan, "receivables", "1100", error);
	if (error != NULL && *error != NULL)
		return NULL;
	ids[1] = control_account(plan, "payables", "2000", error);
	if (error != NULL && *error != NULL)
		return NULL;
	for (i = 0; i < 2; i++)
		if (ids[i] > 0)
			g_hash_table_insert(controls, g_memdup2(&ids[i], sizeof(gint64)), g_strdup(i == 0 ? "receivables" : "payables"));
	for (i = 0; i < plan->bank->len; i++)
	{
		Row *row = g_ptr_array_index(plan->bank, i);
		g_autoptr(VentureEntity) account = row->account_code != NULL
			? account_with_code(plan->self, plan->org, row->account_code, NULL) : NULL;
		gint64 id;
		if (account == NULL)
			continue;
		id = venture_entity_get_id(account);
		g_hash_table_insert(controls, g_memdup2(&id, sizeof(gint64)), g_strdup("bank"));
	}
	if (plan->inventory->len > 0)
	{
		gint64 id = control_account(plan, "inventory", "1200", error);
		if (error != NULL && *error != NULL)
			return NULL;
		if (id > 0)
			g_hash_table_insert(controls, g_memdup2(&id, sizeof(gint64)), g_strdup("inventory"));
	}
	for (i = 0; i < plan->assets->len; i++)
	{
		Row *row = g_ptr_array_index(plan->assets, i);
		const gchar *codes[2];
		guint c;
		codes[0] = row->account_code;
		codes[1] = row->accumulated_code;
		for (c = 0; c < 2; c++)
		{
			g_autoptr(VentureEntity) account = codes[c] != NULL
				? account_with_code(plan->self, plan->org, codes[c], NULL) : NULL;
			gint64 id;
			if (account == NULL)
				continue;
			id = venture_entity_get_id(account);
			g_hash_table_insert(controls, g_memdup2(&id, sizeof(gint64)),
				g_strdup(c == 0 ? "fixed asset cost" : "accumulated depreciation"));
		}
	}
	return g_steal_pointer(&controls);
}

/* --- Ownership -------------------------------------------------------------------- */

/*
 * Whether @row is this batch's to import. A source id already imported by
 * this batch is done; one imported by another live batch is recorded here as
 * skipped with the owner named, so reconcile can leave it out of this batch's
 * expectations instead of failing on a document it never wrote.
 */
static gboolean
claim(Import *import, const gchar *source_id, const gchar *source_type, gboolean *mine, GError **error)
{
	Plan *plan = import->plan;
	g_autoptr(VentureEntity) live = live_row(plan->self, plan->org, source_id, source_type, error);
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autofree gchar *note = NULL;
	guint i;
	*mine = FALSE;
	if (live == NULL)
	{
		if (error != NULL && *error != NULL)
			return FALSE;
		*mine = TRUE;
		return TRUE;
	}
	if (row_cutover(live) == import->cutover_id)
		return TRUE;
	query = venture_query_new(VENTURE_TYPE_ACCOUNTING_CUTOVER_ROW);
	venture_query_set_limit(query, 0);
	if (!venture_query_add_filter_int(query, "cutover-id", VENTURE_FILTER_OP_EQ, import->cutover_id, error) ||
		!venture_query_add_filter_string(query, "source-id", VENTURE_FILTER_OP_EQ, source_id, error))
		return FALSE;
	rows = venture_database_find(plan->self->database, query, error);
	if (rows == NULL)
		return FALSE;
	for (i = 0; i < rows->len; i++)
	{
		g_autofree gchar *type = NULL;
		g_autofree gchar *status = NULL;
		g_object_get(g_ptr_array_index(rows, i), "source-type", &type, "status", &status, NULL);
		if (g_strcmp0(type, source_type) == 0 && g_strcmp0(status, "skipped") == 0)
			return TRUE;
	}
	note = g_strdup_printf("already imported by cutover batch %" G_GINT64_FORMAT, row_cutover(live));
	return add_row(plan->self, import->cutover_id, plan->org, source_id, source_type, "skipped", note,
		NULL, 0, import->actor, error);
}

/* --- Opening cash legs ---------------------------------------------------------- */

/*
 * A payment made or received before the cutoff already sits inside the
 * source bank balance. The opening payment settles the document through its
 * service, which moves cash; this journal moves that cash leg back against
 * clearing, so cash is not counted twice and the document still shows paid.
 */
static gboolean
offset_opening_cash(Import *import, const gchar *source_type, gint64 source_id, const VentureMoney *amount,
	VentureLedgerSide cash_side, const gchar *memo, GError **error)
{
	Plan *plan = import->plan;
	g_autoptr(GPtrArray) journals = venture_posting_service_find_source(venture_database_get_posting_service(plan->self->database),
		source_type, source_id, plan->org, error);
	g_autoptr(VentureJournal) header = venture_journal_new();
	g_autoptr(VentureJournal) posted = NULL;
	g_autoptr(GPtrArray) lines = g_ptr_array_new_with_free_func(g_object_unref);
	VentureJournalLine *line;
	gint64 cash_id = 0;
	guint i, j;
	if (journals == NULL)
		return FALSE;
	for (i = 0; i < journals->len && cash_id == 0; i++)
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_JOURNAL_LINE);
		g_autoptr(GPtrArray) journal_lines = NULL;
		venture_query_set_limit(query, 0);
		if (!venture_query_add_filter_int(query, "journal-id", VENTURE_FILTER_OP_EQ,
			venture_entity_get_id(g_ptr_array_index(journals, i)), error))
			return FALSE;
		journal_lines = venture_database_find(plan->self->database, query, error);
		if (journal_lines == NULL)
			return FALSE;
		for (j = 0; j < journal_lines->len; j++)
		{
			gint side;
			gint64 account_id = 0;
			g_object_get(g_ptr_array_index(journal_lines, j), "side", &side, "account-id", &account_id, NULL);
			if (side == (gint)cash_side)
				cash_id = account_id;
		}
	}
	if (cash_id == 0)
		return refuse(error, "the opening payment did not post a cash leg");
	g_object_set(header, "organization-id", plan->org, "source-type", "accounting_cutover",
		"source-id", import->cutover_id, "occurred-at", plan->cutoff,
		"currency", venture_money_get_currency(amount), "memo", memo, NULL);
	line = venture_journal_line_new();
	g_object_set(line, "account-id", cash_id, "side",
		cash_side == VENTURE_LEDGER_SIDE_DEBIT ? VENTURE_LEDGER_SIDE_CREDIT : VENTURE_LEDGER_SIDE_DEBIT, "amount", amount, NULL);
	g_ptr_array_add(lines, line);
	line = venture_journal_line_new();
	g_object_set(line, "account-id", import->clearing, "side", cash_side, "amount", amount, NULL);
	g_ptr_array_add(lines, line);
	posted = venture_posting_service_post(venture_database_get_posting_service(plan->self->database),
		header, lines, NULL, import->actor, error);
	return posted != NULL;
}

/* An opening receipt or payment through the owning service, dated at the
 * cutoff, then its cash offset. @document_id may be zero for a prepayment. */
static gboolean
opening_payment(Import *import, Row *row, gboolean vendor, gint64 party, gint64 document_id,
	const VentureMoney *amount, const gchar *source_type, GError **error)
{
	Plan *plan = import->plan;
	g_autofree gchar *reference = NULL;
	g_autofree gchar *memo = NULL;
	g_autofree gchar *date = row->date != NULL ? g_date_time_format(row->date, "%Y-%m-%d") : NULL;
	VentureEntity *payment;
	gboolean ok;
	reference = date != NULL && document_id == 0
		? g_strdup_printf("cutover:%s received %s", row->source_id, date)
		: g_strdup_printf("cutover:%s", row->source_id);
	memo = g_strdup_printf("Opening %s offset %s", document_id != 0 ? "payment" : "prepayment",
		row->number != NULL ? row->number : row->source_id);
	payment = vendor ? VENTURE_ENTITY(venture_bill_payment_new()) : VENTURE_ENTITY(venture_payment_new());
	g_object_set(payment, vendor ? "vendor-id" : "customer-id", party, "amount", amount, "date", plan->cutoff,
		"method", "opening", "reference", reference, NULL);
	if (document_id != 0)
		g_object_set(payment, vendor ? "bill-id" : "invoice-id", document_id, NULL);
	venture_entity_set_organization_id(payment, plan->org);
	ok = vendor
		? venture_payables_service_apply_payment(venture_payables_service_get(plan->self->database),
			VENTURE_BILL_PAYMENT(payment), NULL, import->actor, error)
		: venture_settlement_service_apply_payment(venture_settlement_service_get(plan->self->database),
			VENTURE_PAYMENT(payment), NULL, import->actor, error);
	if (ok)
		ok = offset_opening_cash(import, vendor ? "bill_payment" : "payment", venture_entity_get_id(payment), amount,
			vendor ? VENTURE_LEDGER_SIDE_CREDIT : VENTURE_LEDGER_SIDE_DEBIT, memo, error);
	if (ok)
		ok = add_row(plan->self, import->cutover_id, plan->org, row->source_id, source_type, "imported", NULL,
			vendor ? "bill_payment" : "payment", venture_entity_get_id(payment), import->actor, error);
	g_object_unref(payment);
	return ok;
}

static gboolean
opening_credit(Import *import, Row *row, gint64 party, const VentureMoney *amount, const gchar *source_type, GError **error)
{
	Plan *plan = import->plan;
	VentureEntity *credit = row->vendor ? VENTURE_ENTITY(venture_vendor_credit_new()) : VENTURE_ENTITY(venture_customer_credit_new());
	gboolean ok;
	g_object_set(credit, row->vendor ? "vendor-id" : "customer-id", party, "date", row->date, "amount", amount,
		"kind", "credit_note", "reference", row->number != NULL ? row->number : row->source_id, NULL);
	venture_entity_set_organization_id(credit, plan->org);
	ok = row->vendor
		? venture_payables_service_credit_opening(venture_payables_service_get(plan->self->database),
			VENTURE_VENDOR_CREDIT(credit), plan->cutoff, import->clearing, import->actor, error)
		: venture_settlement_service_credit_opening(venture_settlement_service_get(plan->self->database),
			VENTURE_CUSTOMER_CREDIT(credit), plan->cutoff, import->clearing, import->actor, error);
	if (ok)
		ok = add_row(plan->self, import->cutover_id, plan->org, row->source_id, source_type, "imported", NULL,
			venture_entity_get_entity_name(credit), venture_entity_get_id(credit), import->actor, error);
	g_object_unref(credit);
	return ok;
}

/* --- Sections ------------------------------------------------------------------------ */

static gboolean
import_open_ar(Import *import, GError **error)
{
	Plan *plan = import->plan;
	guint i;
	for (i = 0; i < plan->open_ar->len; i++)
	{
		Row *row = g_ptr_array_index(plan->open_ar, i);
		g_autoptr(VentureInvoice) invoice = NULL;
		g_autoptr(GPtrArray) lines = NULL;
		VentureInvoiceLine *line;
		gboolean mine;
		gint64 customer, tax_percent = 0;
		if (!claim(import, row->source_id, "open_ar", &mine, error))
			return FALSE;
		if (!mine)
			continue;
		customer = resolve_party(plan, import->parties, row, import, error);
		if (customer <= 0)
			return party_failed(plan, error);
		if (venture_money_get_amount(row->total) < 0)
		{
			g_autoptr(VentureMoney) owed = venture_money_negate(row->total);
			if (!opening_credit(import, row, customer, owed, "open_ar", error))
				return FALSE;
			continue;
		}
		if (!venture_money_is_zero(row->net))
		{
			__int128 scaled = (__int128)venture_money_get_amount(row->tax) * 100;
			__int128 percent = scaled / venture_money_get_amount(row->net);
			if (scaled % venture_money_get_amount(row->net) == 0 && percent >= 0 && percent <= 100)
				tax_percent = (gint64)percent;
		}
		invoice = venture_invoice_new();
		g_object_set(invoice, "number", row->number, "company-id", customer, "issued-at", row->date,
			"due-at", row->due, "terms", row->terms, "notes", row->memo, NULL);
		if (row->tax_exempt)
			g_object_set(invoice, "tax-exempt", TRUE, "tax-exempt-reason", row->tax_exempt_reason, NULL);
		venture_entity_set_organization_id(VENTURE_ENTITY(invoice), plan->org);
		lines = g_ptr_array_new_with_free_func(g_object_unref);
		line = venture_invoice_line_new();
		/* Mapped tax is frozen on the line rather than rounded through an
		 * integer percent; the percent is informational only. */
		g_object_set(line, "description", "Opening balance", "quantity", 1.0, "tax-percent", tax_percent,
			"unit-price", row->net, "income-amount", row->net, "tax-amount", row->tax, NULL);
		g_ptr_array_add(lines, line);
		if (!venture_settlement_service_issue_opening(venture_settlement_service_get(plan->self->database), invoice, lines,
			plan->cutoff, import->clearing, import->actor, error))
			return FALSE;
		if (!add_row(plan->self, import->cutover_id, plan->org, row->source_id, "open_ar", "imported", NULL,
			"invoice", venture_entity_get_id(VENTURE_ENTITY(invoice)), import->actor, error))
			return FALSE;
		if (!venture_money_is_zero(row->paid) &&
			!opening_payment(import, row, FALSE, customer, venture_entity_get_id(VENTURE_ENTITY(invoice)), row->paid,
				"open_ar_payment", error))
			return FALSE;
	}
	return TRUE;
}

static gboolean
import_open_ap(Import *import, GError **error)
{
	Plan *plan = import->plan;
	guint i, l;
	for (i = 0; i < plan->open_ap->len; i++)
	{
		Row *row = g_ptr_array_index(plan->open_ap, i);
		g_autoptr(VentureVendorBill) bill = NULL;
		g_autoptr(GPtrArray) lines = NULL;
		gboolean mine;
		gint64 vendor;
		if (!claim(import, row->source_id, "open_ap", &mine, error))
			return FALSE;
		if (!mine)
			continue;
		vendor = resolve_party(plan, import->parties, row, import, error);
		if (vendor <= 0)
			return party_failed(plan, error);
		bill = venture_vendor_bill_new();
		g_object_set(bill, "number", row->number, "company-id", vendor, "currency", venture_money_get_currency(row->total),
			"status", "draft", "bill-date", row->date, "due-date", row->due, "memo", row->memo, NULL);
		venture_entity_set_organization_id(VENTURE_ENTITY(bill), plan->org);
		lines = g_ptr_array_new_with_free_func(g_object_unref);
		for (l = 0; l < row->lines->len; l++)
		{
			BillLine *source = g_ptr_array_index(row->lines, l);
			VentureVendorBillLine *line = venture_vendor_bill_line_new();
			gint64 account_id = 0;
			if (source->account_code != NULL)
			{
				account_id = import_account(import, source->account_code, "bill line expense", error);
				if (account_id == 0)
				{
					g_object_unref(line);
					return FALSE;
				}
			}
			g_object_set(line, "description", source->description != NULL ? source->description : "Opening balance",
				"quantity", "1", "unit-price", source->amount, "tax-amount", source->tax, "account-id", account_id,
				"category", source->category, "position", (gint64)l, NULL);
			g_ptr_array_add(lines, line);
		}
		if (!venture_payables_service_approve_opening(venture_payables_service_get(plan->self->database), bill, lines,
			plan->cutoff, import->clearing, import->actor, error))
			return FALSE;
		if (!add_row(plan->self, import->cutover_id, plan->org, row->source_id, "open_ap", "imported", NULL,
			"vendor_bill", venture_entity_get_id(VENTURE_ENTITY(bill)), import->actor, error))
			return FALSE;
		if (!venture_money_is_zero(row->paid) &&
			!opening_payment(import, row, TRUE, vendor, venture_entity_get_id(VENTURE_ENTITY(bill)), row->paid,
				"open_ap_payment", error))
			return FALSE;
	}
	return TRUE;
}

/* Credit notes stay unapplied for later settlements. A prepayment is money
 * received or paid before the cutoff with no document yet: an unapplied
 * receipt or payment through the service, its cash offset against clearing,
 * so it can later settle an invoice or be refunded like any deposit. */
static gboolean
import_credits(Import *import, GError **error)
{
	Plan *plan = import->plan;
	guint i;
	for (i = 0; i < plan->credits->len; i++)
	{
		Row *row = g_ptr_array_index(plan->credits, i);
		gboolean mine;
		gint64 party;
		if (!claim(import, row->source_id, "credit", &mine, error))
			return FALSE;
		if (!mine)
			continue;
		party = resolve_party(plan, import->parties, row, import, error);
		if (party <= 0)
			return party_failed(plan, error);
		if (row->prepayment)
		{
			if (!opening_payment(import, row, row->vendor, party, 0, row->total, "credit", error))
				return FALSE;
		}
		else if (!opening_credit(import, row, party, row->total, "credit", error))
			return FALSE;
	}
	return TRUE;
}

static gboolean
post_opening_pair(Import *import, gint64 account, const VentureMoney *amount, const gchar *memo, GError **error)
{
	Plan *plan = import->plan;
	g_autoptr(VentureJournal) header = venture_journal_new();
	g_autoptr(VentureJournal) posted = NULL;
	g_autoptr(GPtrArray) lines = g_ptr_array_new_with_free_func(g_object_unref);
	g_autoptr(VentureMoney) magnitude = venture_money_abs(amount);
	gboolean debit = venture_money_get_amount(amount) > 0;
	VentureJournalLine *line;
	g_object_set(header, "organization-id", plan->org, "source-type", "accounting_cutover",
		"source-id", import->cutover_id, "occurred-at", plan->cutoff,
		"currency", venture_money_get_currency(amount), "memo", memo, NULL);
	line = venture_journal_line_new();
	g_object_set(line, "account-id", account, "side", debit ? VENTURE_LEDGER_SIDE_DEBIT : VENTURE_LEDGER_SIDE_CREDIT,
		"amount", magnitude, NULL);
	g_ptr_array_add(lines, line);
	line = venture_journal_line_new();
	g_object_set(line, "account-id", import->clearing, "side", debit ? VENTURE_LEDGER_SIDE_CREDIT : VENTURE_LEDGER_SIDE_DEBIT,
		"amount", magnitude, NULL);
	g_ptr_array_add(lines, line);
	posted = venture_posting_service_post(venture_database_get_posting_service(plan->self->database),
		header, lines, NULL, import->actor, error);
	return posted != NULL;
}

static gboolean
import_bank(Import *import, GError **error)
{
	Plan *plan = import->plan;
	guint i;
	for (i = 0; i < plan->bank->len; i++)
	{
		Row *row = g_ptr_array_index(plan->bank, i);
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_BANK_ACCOUNT);
		g_autoptr(VentureEntity) bank = NULL;
		g_autoptr(GPtrArray) existing = NULL;
		g_autofree gchar *memo = NULL;
		gboolean mine;
		gint64 account;
		guint b;
		if (!claim(import, row->source_id, "bank", &mine, error))
			return FALSE;
		if (!mine)
			continue;
		account = import_account(import, row->account_code, "bank", error);
		if (account == 0)
			return FALSE;
		/* A rolled-back batch leaves its bank account behind; a corrected
		 * batch reuses it rather than growing a second "Checking". */
		venture_query_set_organization(query, plan->org);
		venture_query_set_limit(query, 0);
		if (!venture_query_add_filter_string(query, "name", VENTURE_FILTER_OP_EQ, row->name, error))
			return FALSE;
		existing = venture_database_find(plan->self->database, query, error);
		if (existing == NULL)
			return FALSE;
		for (b = 0; b < existing->len && bank == NULL; b++)
		{
			gint64 linked = 0;
			g_object_get(g_ptr_array_index(existing, b), "account-id", &linked, NULL);
			if (linked == account)
				bank = g_object_ref(g_ptr_array_index(existing, b));
		}
		if (bank == NULL)
		{
			bank = VENTURE_ENTITY(venture_bank_account_new());
			g_object_set(bank, "name", row->name, "currency", venture_money_get_currency(row->total),
				"account-id", account, NULL);
			venture_entity_set_organization_id(bank, plan->org);
			if (!save_owned(plan->self, bank, import->actor, error))
				return FALSE;
		}
		memo = g_strdup_printf("Opening bank balance %s", row->name);
		if (!venture_money_is_zero(row->total) && !post_opening_pair(import, account, row->total, memo, error))
			return FALSE;
		if (!add_row(plan->self, import->cutover_id, plan->org, row->source_id, "bank", "imported", NULL,
			"bank_account", venture_entity_get_id(bank), import->actor, error))
			return FALSE;
	}
	return TRUE;
}

static gboolean
import_assets(Import *import, GError **error)
{
	Plan *plan = import->plan;
	guint i;
	for (i = 0; i < plan->assets->len; i++)
	{
		Row *row = g_ptr_array_index(plan->assets, i);
		g_autoptr(VentureEntity) asset = NULL;
		gint64 asset_account, accumulated_account, expense_account;
		gboolean mine;
		if (!claim(import, row->source_id, "asset", &mine, error))
			return FALSE;
		if (!mine)
			continue;
		asset_account = import_account(import, row->account_code, "asset", error);
		accumulated_account = asset_account != 0 ? import_account(import, row->accumulated_code, "accumulated depreciation", error) : 0;
		expense_account = accumulated_account != 0 ? import_account(import, row->expense_code, "depreciation expense", error) : 0;
		if (expense_account == 0)
			return FALSE;
		asset = VENTURE_ENTITY(venture_fixed_asset_new());
		g_object_set(asset, "name", row->name, "tag", row->tag, "category", row->category, "cost", row->total,
			"salvage-value", row->salvage, "useful-life-months", row->life_months, "in-service-at", row->date,
			"acquired-at", row->acquired, "asset-account-id", asset_account,
			"accumulated-depreciation-account-id", accumulated_account,
			"depreciation-expense-account-id", expense_account, "status", VENTURE_ASSET_STATUS_DRAFT, NULL);
		venture_entity_set_organization_id(asset, plan->org);
		if (!venture_entity_set_field_from_string(asset, "method", row->method, error) ||
			!venture_database_save(plan->self->database, asset, import->actor, error))
			return FALSE;
		if (!venture_asset_service_import_opening(venture_asset_service_get(plan->self->database), asset, row->accumulated,
			plan->cutoff, import->clearing, "accounting_cutover", import->cutover_id, import->actor, error))
			return FALSE;
		if (!add_row(plan->self, import->cutover_id, plan->org, row->source_id, "asset", "imported", NULL,
			"fixed_asset", venture_entity_get_id(asset), import->actor, error))
			return FALSE;
	}
	return TRUE;
}

/* The inventory item a row names by SKU, and location when given. Returns
 * the id, or 0 with a located problem. */
static gint64
inventory_item_for(Plan *plan, Row *row)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_INVENTORY_ITEM);
	g_autoptr(GPtrArray) items = NULL;
	gint64 id = 0;
	guint i, matches = 0;
	venture_query_set_organization(query, plan->org);
	venture_query_set_limit(query, 0);
	if (!venture_query_add_filter_string(query, "sku", VENTURE_FILTER_OP_EQ, row->tag, NULL))
		return 0;
	items = venture_database_find(plan->self->database, query, NULL);
	for (i = 0; items != NULL && i < items->len; i++)
	{
		g_autofree gchar *location = NULL;
		g_object_get(g_ptr_array_index(items, i), "location", &location, NULL);
		if (row->category != NULL && g_strcmp0(location, row->category) != 0)
			continue;
		id = venture_entity_get_id(g_ptr_array_index(items, i));
		matches++;
	}
	if (matches == 1)
		return id;
	if (matches == 0)
		row_error(plan, row, "no inventory item has SKU %s%s%s; create the product and its inventory item first",
			row->tag, row->category != NULL ? " at " : "", row->category != NULL ? row->category : "");
	else
		row_error(plan, row, "SKU %s matches %u inventory items; give location to choose one", row->tag, matches);
	return 0;
}

/*
 * Stock on hand is received through the inventory service at the cutoff, so
 * it has a FIFO cost layer like any purchase. The receipt credits goods
 * received not invoiced; this moves that credit to clearing, because nothing
 * was received from a supplier here.
 */
static gboolean
import_inventory(Import *import, GError **error)
{
	Plan *plan = import->plan;
	guint i;
	for (i = 0; i < plan->inventory->len; i++)
	{
		Row *row = g_ptr_array_index(plan->inventory, i);
		g_autofree gchar *reference = NULL;
		g_autofree gchar *memo = NULL;
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_INVENTORY_TXN);
		g_autoptr(GPtrArray) txns = NULL;
		VentureEntity *txn;
		gboolean mine;
		gint64 item;
		if (!claim(import, row->source_id, "inventory", &mine, error))
			return FALSE;
		if (!mine)
			continue;
		item = inventory_item_for(plan, row);
		if (item == 0)
			return party_failed(plan, error);
		reference = g_strdup_printf("cutover:%s", row->source_id);
		if (!venture_inventory_service_receive(venture_inventory_service_get(plan->self->database), item, row->quantity,
			row->net, plan->cutoff, 0, reference, import->actor, error))
			return FALSE;
		venture_query_set_organization(query, plan->org);
		venture_query_set_limit(query, 0);
		if (!venture_query_add_filter_int(query, "inventory-item-id", VENTURE_FILTER_OP_EQ, item, error) ||
			!venture_query_add_filter_string(query, "reference", VENTURE_FILTER_OP_EQ, reference, error) ||
			!venture_query_add_order(query, "id", VENTURE_SORT_DESCENDING, error))
			return FALSE;
		txns = venture_database_find(plan->self->database, query, error);
		if (txns == NULL || txns->len == 0)
			return txns == NULL ? FALSE : refuse(error, "the opening inventory receipt was not recorded");
		txn = g_ptr_array_index(txns, 0);
		memo = g_strdup_printf("Opening inventory %s", row->tag);
		/* The receipt's credit leg is goods received not invoiced; offset it. */
		if (!offset_opening_cash(import, "inventory_txn", venture_entity_get_id(txn), row->total,
			VENTURE_LEDGER_SIDE_CREDIT, memo, error))
			return FALSE;
		if (!add_row(plan->self, import->cutover_id, plan->org, row->source_id, "inventory", "imported", NULL,
			"inventory_txn", venture_entity_get_id(txn), import->actor, error))
			return FALSE;
	}
	return TRUE;
}

/*
 * One journal per currency at the cutoff for the accounts no subledger owns:
 * loans, cards, prepaids, tax payable, retained earnings, equity. Control
 * accounts are skipped here and asserted at reconcile. The balancing leg is
 * clearing, which then nets the subledger legs to zero.
 */
static gboolean
import_trial_balance(Import *import, GError **error)
{
	Plan *plan = import->plan;
	g_autoptr(GHashTable) controls = NULL;
	g_autoptr(GHashTable) currencies = g_hash_table_new(g_str_hash, g_str_equal);
	GHashTableIter iter;
	gpointer key;
	guint i;
	if (plan->trial_balance->len == 0)
		return TRUE;
	controls = control_accounts(plan, error);
	if (controls == NULL)
		return FALSE;
	for (i = 0; i < plan->trial_balance->len; i++)
	{
		Row *row = g_ptr_array_index(plan->trial_balance, i);
		g_hash_table_add(currencies, (gpointer)venture_money_get_currency(row->total));
	}
	g_hash_table_iter_init(&iter, currencies);
	while (g_hash_table_iter_next(&iter, &key, NULL))
	{
		const gchar *currency = key;
		g_autofree gchar *source_id = g_strdup_printf("trial_balance:%s", currency);
		g_autoptr(VentureJournal) header = venture_journal_new();
		g_autoptr(VentureJournal) posted = NULL;
		g_autoptr(GPtrArray) lines = g_ptr_array_new_with_free_func(g_object_unref);
		g_autoptr(VentureMoney) sum = venture_money_new_zero(currency);
		VentureJournalLine *line;
		gboolean mine;
		if (!claim(import, source_id, "trial_balance", &mine, error))
			return FALSE;
		if (!mine)
			continue;
		for (i = 0; i < plan->trial_balance->len; i++)
		{
			Row *row = g_ptr_array_index(plan->trial_balance, i);
			g_autoptr(VentureMoney) magnitude = NULL;
			g_autoptr(VentureMoney) next = NULL;
			gint64 account;
			if (g_strcmp0(venture_money_get_currency(row->total), currency) != 0 || venture_money_is_zero(row->total))
				continue;
			account = import_account(import, row->account_code, "trial balance", error);
			if (account == 0)
				return FALSE;
			if (g_hash_table_contains(controls, &account))
				continue;
			magnitude = venture_money_abs(row->total);
			line = venture_journal_line_new();
			g_object_set(line, "account-id", account, "amount", magnitude, "side",
				venture_money_get_amount(row->total) > 0 ? VENTURE_LEDGER_SIDE_DEBIT : VENTURE_LEDGER_SIDE_CREDIT, NULL);
			g_ptr_array_add(lines, line);
			next = venture_money_add(sum, row->total, error);
			if (next == NULL)
				return FALSE;
			g_clear_pointer(&sum, venture_money_free);
			sum = g_steal_pointer(&next);
		}
		if (lines->len == 0)
		{
			if (!add_row(plan->self, import->cutover_id, plan->org, source_id, "trial_balance", "imported",
				"every row is a control account", "accounting_cutover", import->cutover_id, import->actor, error))
				return FALSE;
			continue;
		}
		if (!venture_money_is_zero(sum))
		{
			g_autoptr(VentureMoney) magnitude = venture_money_abs(sum);
			line = venture_journal_line_new();
			g_object_set(line, "account-id", import->clearing, "amount", magnitude, "side",
				venture_money_get_amount(sum) > 0 ? VENTURE_LEDGER_SIDE_CREDIT : VENTURE_LEDGER_SIDE_DEBIT, NULL);
			g_ptr_array_add(lines, line);
		}
		g_object_set(header, "organization-id", plan->org, "source-type", "accounting_cutover",
			"source-id", import->cutover_id, "occurred-at", plan->cutoff, "currency", currency,
			"memo", "Opening trial balance", NULL);
		posted = venture_posting_service_post(venture_database_get_posting_service(plan->self->database),
			header, lines, NULL, import->actor, error);
		if (posted == NULL)
			return FALSE;
		if (!add_row(plan->self, import->cutover_id, plan->org, source_id, "trial_balance", "imported", NULL,
			"journal", venture_entity_get_id(VENTURE_ENTITY(posted)), import->actor, error))
			return FALSE;
	}
	return TRUE;
}

/* --- Database-aware validation -------------------------------------------------------- */

static void
check_existing_number(Plan *plan, Row *row, GType type, const gchar *field, const gchar *value,
	const gchar *source_type, const gchar *what)
{
	g_autoptr(VentureQuery) query = venture_query_new(type);
	g_autoptr(GPtrArray) found = NULL;
	g_autoptr(VentureEntity) live = NULL;
	if (value == NULL || !row->ok)
		return;
	venture_query_set_organization(query, plan->org);
	venture_query_set_include_deleted(query, TRUE);
	venture_query_set_limit(query, 0);
	if (!venture_query_add_filter_string(query, field, VENTURE_FILTER_OP_EQ, value, NULL))
		return;
	found = venture_database_find(plan->self->database, query, NULL);
	if (found == NULL || found->len == 0)
		return;
	/* The same source row already imported is reuse, not a collision. */
	live = live_row(plan->self, plan->org, row->source_id, source_type, NULL);
	if (live != NULL && row_record(live) == venture_entity_get_id(g_ptr_array_index(found, 0)))
		return;
	row_error(plan, row, "%s %s already exists (%s %" G_GINT64_FORMAT "); a rolled-back batch frees its numbers, anything else must be renamed",
		what, value, venture_entity_get_entity_name(g_ptr_array_index(found, 0)),
		venture_entity_get_id(g_ptr_array_index(found, 0)));
}

static void
note_owned(Plan *plan, Row *row, const gchar *source_type)
{
	g_autoptr(VentureEntity) live = NULL;
	if (row->source_id == NULL || !row->ok)
		return;
	live = live_row(plan->self, plan->org, row->source_id, source_type, NULL);
	if (live != NULL)
		row_warning(plan, row, "already imported by cutover batch %" G_GINT64_FORMAT "; this batch will skip it",
			row_cutover(live));
}

/*
 * The read-only checks that need the database: party matches, numbers and
 * tags already taken, rows another batch owns, and the clearing account.
 */
static void
plan_check_database(Plan *plan)
{
	g_autoptr(PartyIndex) parties = party_index_build(plan->self, plan->org, NULL);
	g_autoptr(GError) local = NULL;
	GPtrArray *sections[3];
	guint s, i;
	sections[0] = plan->open_ar;
	sections[1] = plan->open_ap;
	sections[2] = plan->credits;
	for (s = 0; s < G_N_ELEMENTS(sections); s++)
		for (i = 0; i < sections[s]->len; i++)
		{
			Row *row = g_ptr_array_index(sections[s], i);
			if (row->ok && parties != NULL)
				resolve_party(plan, parties, row, NULL, NULL);
		}
	for (i = 0; i < plan->open_ar->len; i++)
	{
		Row *row = g_ptr_array_index(plan->open_ar, i);
		note_owned(plan, row, "open_ar");
		if (row->total != NULL && venture_money_get_amount(row->total) > 0)
			check_existing_number(plan, row, VENTURE_TYPE_INVOICE, "number", row->number, "open_ar", "invoice number");
	}
	for (i = 0; i < plan->open_ap->len; i++)
	{
		Row *row = g_ptr_array_index(plan->open_ap, i);
		note_owned(plan, row, "open_ap");
		if (venture_entity_registry_is_type_enabled(venture_entity_registry_get_default(), "vendor_bill"))
			check_existing_number(plan, row, VENTURE_TYPE_VENDOR_BILL, "number", row->number, "open_ap", "bill number");
	}
	for (i = 0; i < plan->credits->len; i++)
		note_owned(plan, g_ptr_array_index(plan->credits, i), "credit");
	for (i = 0; i < plan->inventory->len; i++)
	{
		Row *row = g_ptr_array_index(plan->inventory, i);
		note_owned(plan, row, "inventory");
		if (row->ok && venture_entity_registry_is_type_enabled(venture_entity_registry_get_default(), "inventory_item"))
			inventory_item_for(plan, row);
	}
	for (i = 0; i < plan->bank->len; i++)
		note_owned(plan, g_ptr_array_index(plan->bank, i), "bank");
	for (i = 0; i < plan->assets->len; i++)
	{
		Row *row = g_ptr_array_index(plan->assets, i);
		note_owned(plan, row, "asset");
		if (venture_entity_registry_is_type_enabled(venture_entity_registry_get_default(), "fixed_asset"))
			check_existing_number(plan, row, VENTURE_TYPE_FIXED_ASSET, "tag", row->tag, "asset", "asset tag");
	}
	clearing_account(plan, FALSE, NULL, &local);
	if (local != NULL)
		plan_note(plan, "clearing_account_code", -1, NULL, FALSE, "%s", local->message);
}

/* Credits used to be capped at the party's open balance, which refused a
 * customer's retainer with no invoice. The cap is now a warning: a credit
 * larger than what the party owes is an ordinary unapplied balance. */
static void
plan_check_credit_caps(Plan *plan)
{
	g_autoptr(GHashTable) owed = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)venture_money_free);
	guint i;
	for (i = 0; i < plan->open_ar->len + plan->open_ap->len; i++)
	{
		Row *row = g_ptr_array_index(i < plan->open_ar->len ? plan->open_ar : plan->open_ap,
			i < plan->open_ar->len ? i : i - plan->open_ar->len);
		g_autofree gchar *key = NULL;
		VentureMoney *sum;
		if (!row->ok || row->open == NULL)
			continue;
		key = g_strdup_printf("%d|%s|%s|%s", row->vendor, row->party_source != NULL ? row->party_source : "",
			row->party_name != NULL ? row->party_name : "", venture_money_get_currency(row->open));
		sum = money_sum(g_hash_table_lookup(owed, key), row->open);
		g_hash_table_insert(owed, g_steal_pointer(&key), sum);
	}
	for (i = 0; i < plan->credits->len; i++)
	{
		Row *row = g_ptr_array_index(plan->credits, i);
		g_autofree gchar *key = NULL;
		VentureMoney *open;
		if (!row->ok || row->total == NULL || row->prepayment)
			continue;
		key = g_strdup_printf("%d|%s|%s|%s", row->vendor, row->party_source != NULL ? row->party_source : "",
			row->party_name != NULL ? row->party_name : "", venture_money_get_currency(row->total));
		open = g_hash_table_lookup(owed, key);
		if (open == NULL || venture_money_compare(row->total, open) > 0)
			row_warning(plan, row, "exceeds the counterpart's open documents in this payload; it imports as an unapplied credit (rule: credit-within-open-balance)");
	}
}
