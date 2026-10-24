/* SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Reconcile and activate. Included by venture-books-import-service.c.
 *
 * The proof is the source's own ledger, rebuilt from the backup by the same
 * plan the import replayed: every account of the organization at every
 * month end and at the export date must hold exactly what Zoho posted to
 * it, and each month's invoices, bills, payments and expenses must count
 * and total what the backup lists. An account no Zoho posting names --
 * the organization's own cash account a receipt passed through -- must be
 * back at zero. Nothing is netted across accounts or months.
 */

static gint64
special_account(Plan *plan, const gchar *key, GError **error)
{
	if (g_strcmp0(key, "@ar") == 0)
		return control_account(plan->self, plan->org, "receivables", "1100", error);
	if (g_strcmp0(key, "@ap") == 0)
		return control_account(plan->self, plan->org, "payables", "2000", error);
	if (g_strcmp0(key, "@tax") == 0)
		return control_account(plan->self, plan->org, "tax", "2100", error);
	return account_id(plan, key);
}

static gchar *
account_label(VentureBooksImportService *self, gint64 id)
{
	g_autoptr(VentureEntity) account = venture_database_get(self->database, VENTURE_TYPE_ACCOUNT, id, NULL);
	g_autofree gchar *name = NULL;
	g_autofree gchar *code = NULL;
	if (account == NULL)
		return g_strdup_printf("#%" G_GINT64_FORMAT, id);
	g_object_get(account, "name", &name, "code", &code, NULL);
	return g_strdup_printf("%s (%s)", name != NULL ? name : "?", code != NULL ? code : "?");
}

/* Every month end from the start to the last date, and the last date. */
static GPtrArray *
check_dates(Plan *plan)
{
	GPtrArray *dates = g_ptr_array_new_with_free_func(g_free);
	g_autofree gchar *last = NULL;
	g_autoptr(GDateTime) month = NULL;
	guint i;
	for (i = 0; i < plan->events->len; i++)
	{
		Event *event = g_ptr_array_index(plan->events, i);
		if (last == NULL || strcmp(event->date, last) > 0)
		{
			g_free(last);
			last = g_strdup(event->date);
		}
	}
	if (*plan->export_text != '\0')
	{
		g_free(last);
		last = g_strdup(plan->export_text);
	}
	if (last == NULL)
		last = g_strdup(plan->start_text);
	month = g_date_time_new_utc(g_date_time_get_year(plan->start), g_date_time_get_month(plan->start), 1, 0, 0, 0);
	for (;;)
	{
		g_autoptr(GDateTime) next = g_date_time_add_months(month, 1);
		g_autoptr(GDateTime) end = g_date_time_add_days(next, -1);
		g_autofree gchar *text = g_date_time_format(end, "%Y-%m-%d");
		if (strcmp(text, last) >= 0)
			break;
		g_ptr_array_add(dates, g_steal_pointer(&text));
		g_clear_pointer(&month, g_date_time_unref);
		month = g_steal_pointer(&next);
	}
	g_ptr_array_add(dates, g_steal_pointer(&last));
	return dates;
}

static gint64
ledger_balance(Plan *plan, gint64 account, const gchar *date, GError **error)
{
	g_autoptr(GDateTime) day = parse_date(date);
	g_autoptr(GDateTime) end = g_date_time_add_seconds(day, 86399);
	g_autoptr(VentureMoney) value = venture_posting_service_account_balance(venture_database_get_posting_service(plan->self->database),
		account, plan->org, plan->currency, end, error);
	return value != NULL ? venture_money_get_amount(value) : G_MININT64;
}

/* The organization's ledger read once: per account, every posted movement
 * in the book currency with its instant. Asking the posting service for
 * each account at each month end rereads every journal each time, which
 * on a two-year history is thousands of full scans. The same rules decide
 * what counts -- posted or reversed journals in the book currency, dated
 * at or before the instant -- and the last date is checked against the
 * posting service itself. */
typedef struct
{
	gint64 when;	/* unix seconds */
	gint64 cents;	/* debit positive */
} Movement;

static GHashTable *
ledger_load(Plan *plan, GError **error)
{
	VentureDatabase *db = plan->self->database;
	g_autoptr(GHashTable) ledger = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, (GDestroyNotify)g_array_unref);
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_JOURNAL);
	g_autoptr(GPtrArray) journals = NULL;
	guint i, j;
	venture_query_set_limit(query, 0);
	venture_query_set_include_deleted(query, TRUE);
	venture_query_set_organization(query, plan->org);
	journals = venture_database_find(db, query, error);
	if (journals == NULL)
		return NULL;
	for (i = 0; i < journals->len; i++)
	{
		VentureEntity *journal = g_ptr_array_index(journals, i);
		g_autoptr(GDateTime) when = NULL;
		g_autofree gchar *currency = NULL;
		g_autoptr(VentureQuery) lines_query = venture_query_new(VENTURE_TYPE_JOURNAL_LINE);
		g_autoptr(GPtrArray) lines = NULL;
		VentureJournalState state;
		g_object_get(journal, "state", &state, "occurred-at", &when, "currency", &currency, NULL);
		if ((state != VENTURE_JOURNAL_POSTED && state != VENTURE_JOURNAL_REVERSED) || when == NULL ||
			g_strcmp0(currency, plan->currency) != 0)
			continue;
		venture_query_set_limit(lines_query, 0);
		if (!venture_query_add_filter_int(lines_query, "journal-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(journal), error))
			return NULL;
		lines = venture_database_find(db, lines_query, error);
		if (lines == NULL)
			return NULL;
		for (j = 0; j < lines->len; j++)
		{
			g_autoptr(VentureMoney) amount = NULL;
			gint64 account = 0;
			VentureLedgerSide side;
			Movement movement;
			GArray *movements;
			g_object_get(g_ptr_array_index(lines, j), "account-id", &account, "side", &side, "book-amount", &amount, NULL);
			if (amount == NULL)
				continue;
			movement.when = g_date_time_to_unix(when);
			movement.cents = side == VENTURE_LEDGER_SIDE_DEBIT ? venture_money_get_amount(amount) : -venture_money_get_amount(amount);
			movements = g_hash_table_lookup(ledger, &account);
			if (movements == NULL)
			{
				gint64 *key = g_new(gint64, 1);
				*key = account;
				movements = g_array_new(FALSE, FALSE, sizeof(Movement));
				g_hash_table_insert(ledger, key, movements);
			}
			g_array_append_val(movements, movement);
		}
	}
	return g_steal_pointer(&ledger);
}

/* The balance at the end of @date: every movement at or before 23:59:59. */
static gint64
ledger_at(GHashTable *ledger, gint64 account, const gchar *date)
{
	g_autoptr(GDateTime) day = parse_date(date);
	GArray *movements = g_hash_table_lookup(ledger, &account);
	gint64 end = g_date_time_to_unix(day) + 86399, sum = 0;
	guint i;
	for (i = 0; movements != NULL && i < movements->len; i++)
		if (g_array_index(movements, Movement, i).when <= end)
			sum += g_array_index(movements, Movement, i).cents;
	return sum;
}

typedef struct
{
	guint count;
	gint64 cents;
} Tally;

static void
tally(GHashTable *table, const gchar *key, gint64 cents)
{
	Tally *entry = g_hash_table_lookup(table, key);
	if (entry == NULL)
	{
		entry = g_new0(Tally, 1);
		g_hash_table_insert(table, g_strdup(key), entry);
	}
	entry->count++;
	entry->cents += cents;
}

static const gchar *
record_date_field(const gchar *type)
{
	if (g_strcmp0(type, "invoice") == 0)
		return "issued-at";
	if (g_strcmp0(type, "vendor_bill") == 0)
		return "bill-date";
	if (g_strcmp0(type, "expense") == 0)
		return "occurred-at";
	return "date";
}

/* What VENTURE holds for one imported document: its month and its total,
 * read from the record itself, not from the row that names it. */
static gboolean
record_figures(Plan *plan, const gchar *type, gint64 id, gchar **period, gint64 *cents, GError **error)
{
	VentureDatabase *db = plan->self->database;
	GType gtype = venture_entity_registry_lookup(venture_entity_registry_get_default(), type);
	g_autoptr(VentureEntity) record = NULL;
	g_autoptr(GDateTime) date = NULL;
	g_autoptr(VentureMoney) amount = NULL;
	*period = NULL;
	*cents = 0;
	if (gtype == G_TYPE_INVALID)
		return refuse(error, "an imported record type is not registered");
	record = venture_database_get(db, gtype, id, error);
	if (record == NULL)
		return FALSE;
	g_object_get(record, record_date_field(type), &date, NULL);
	if (date != NULL)
		*period = g_date_time_format(date, "%Y-%m");
	if (g_strcmp0(type, "invoice") == 0)
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_INVOICE_EVENT);
		g_autoptr(GPtrArray) events = NULL;
		guint i;
		venture_query_set_limit(query, 0);
		if (!venture_query_add_filter_int(query, "invoice-id", VENTURE_FILTER_OP_EQ, id, error))
			return FALSE;
		events = venture_database_find(db, query, error);
		if (events == NULL)
			return FALSE;
		for (i = 0; i < events->len && amount == NULL; i++)
		{
			g_autofree gchar *kind = NULL;
			g_object_get(g_ptr_array_index(events, i), "kind", &kind, NULL);
			if (g_strcmp0(kind, "issue") == 0)
				g_object_get(g_ptr_array_index(events, i), "amount", &amount, NULL);
		}
	}
	else if (g_strcmp0(type, "vendor_bill") == 0)
	{
		/* A bill's total is its lines', as payables computes it. */
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_VENDOR_BILL_LINE);
		g_autoptr(GPtrArray) lines = NULL;
		guint i;
		venture_query_set_limit(query, 0);
		if (!venture_query_add_filter_int(query, "bill-id", VENTURE_FILTER_OP_EQ, id, error))
			return FALSE;
		lines = venture_database_find(db, query, error);
		if (lines == NULL)
			return FALSE;
		for (i = 0; i < lines->len; i++)
		{
			g_autoptr(VentureMoney) line = venture_vendor_bill_line_get_amount(VENTURE_VENDOR_BILL_LINE(g_ptr_array_index(lines, i)), error);
			if (line == NULL)
				return FALSE;
			*cents += venture_money_get_amount(line);
		}
		return TRUE;
	}
	else
		g_object_get(record, "amount", &amount, NULL);
	if (amount != NULL)
		*cents = venture_money_get_amount(amount);
	return TRUE;
}

static void
tally_free(gpointer data)
{
	g_free(data);
}

static gboolean
compare_documents(Plan *plan, VentureEntity *batch, GString *block, GPtrArray *failures, GError **error)
{
	static const struct { EventKind kind; const gchar *label; const gchar *record; } kinds[] = {
		{ EVENT_INVOICE, "invoices", "invoice" }, { EVENT_BILL, "bills", "vendor_bill" },
		{ EVENT_CUSTOMER_PAYMENT, "customer payments", "payment" },
		{ EVENT_VENDOR_PAYMENT, "vendor payments", "bill_payment" }, { EVENT_EXPENSE, "expenses", "expense" }
	};
	g_autoptr(GPtrArray) rows = batch_rows(plan->self, venture_entity_get_id(batch), error);
	guint k, i;
	if (rows == NULL)
		return FALSE;
	for (k = 0; k < G_N_ELEMENTS(kinds); k++)
	{
		g_autoptr(GHashTable) source = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, tally_free);
		g_autoptr(GHashTable) held = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, tally_free);
		g_autoptr(GList) months = NULL;
		GList *m;
		for (i = 0; i < plan->events->len; i++)
		{
			Event *event = g_ptr_array_index(plan->events, i);
			g_autofree gchar *period = NULL;
			if (event->kind != kinds[k].kind || !event->posts)
				continue;
			period = period_of(event->date);
			tally(source, period, event->cents);
		}
		for (i = 0; i < rows->len; i++)
		{
			VentureEntity *row = g_ptr_array_index(rows, i);
			g_autofree gchar *object = row_string(row, "source-object");
			g_autofree gchar *type = row_string(row, "record-type");
			g_autofree gchar *status = row_string(row, "status");
			g_autofree gchar *exception = row_string(row, "exception");
			g_autofree gchar *period = NULL;
			gint64 cents = 0;
			if (g_strcmp0(object, event_objects[kinds[k].kind]) != 0 || g_strcmp0(status, "imported") != 0 ||
				row_int(row, "record-id") <= 0 || (exception != NULL && *exception != '\0'))
				continue;
			if (!record_figures(plan, type, row_int(row, "record-id"), &period, &cents, error))
				return FALSE;
			if (kinds[k].kind == EVENT_EXPENSE)
			{
				/* An expense's purchase tax is its second record. */
				guint j;
				g_autofree gchar *source_id = row_string(row, "source-id");
				for (j = 0; j < rows->len; j++)
				{
					VentureEntity *tax = g_ptr_array_index(rows, j);
					g_autofree gchar *tax_object = row_string(tax, "source-object");
					g_autofree gchar *tax_source = row_string(tax, "source-id");
					g_autofree gchar *tax_period = NULL;
					gint64 tax_cents = 0;
					if (g_strcmp0(tax_object, "expense_tax") != 0 || g_strcmp0(tax_source, source_id) != 0)
						continue;
					if (!record_figures(plan, "expense", row_int(tax, "record-id"), &tax_period, &tax_cents, error))
						return FALSE;
					cents += tax_cents;
				}
			}
			tally(held, period != NULL ? period : "(undated)", cents);
		}
		months = g_hash_table_get_keys(source);
		for (m = g_hash_table_get_keys(held); m != NULL; m = g_list_delete_link(m, m))
			if (!g_hash_table_contains(source, m->data))
				months = g_list_prepend(months, m->data);
		months = g_list_sort(months, (GCompareFunc)strcmp);
		for (m = months; m != NULL; m = m->next)
		{
			Tally *want = g_hash_table_lookup(source, m->data);
			Tally *have = g_hash_table_lookup(held, m->data);
			Tally none = { 0, 0 };
			g_autofree gchar *want_total = NULL;
			g_autofree gchar *have_total = NULL;
			gboolean pass;
			if (want == NULL)
				want = &none;
			if (have == NULL)
				have = &none;
			pass = want->count == have->count && want->cents == have->cents;
			want_total = format_amount(plan, want->cents);
			have_total = format_amount(plan, have->cents);
			g_string_append_printf(block, "%s %s %s: Zoho %u for %s, VENTURE %u for %s\n", pass ? "PASS" : "FAIL",
				kinds[k].label, (const gchar *)m->data, want->count, want_total, have->count, have_total);
			if (!pass)
				g_ptr_array_add(failures, g_strdup_printf("%s %s", kinds[k].label, (const gchar *)m->data));
		}
	}
	return TRUE;
}

/* A posting the ledger still carries from a rolled-back batch: dated in
 * the past, reversed on the rollback day. */
typedef struct
{
	gchar *date;
	gchar *until;		/* rollback day, YYYY-MM-DD */
	gint64 account;
	gint64 cents;
} Earlier;

static void
earlier_free(gpointer data)
{
	Earlier *earlier = data;
	g_free(earlier->date);
	g_free(earlier->until);
	g_free(earlier);
}

/*
 * The ledger keeps everything a rolled-back batch posted -- immutably, at
 * the source dates -- and the reversal on the day it was rolled back. A
 * corrected batch of the same books is compared with the ledger less that
 * history: each rolled-back batch's own source ledger, from its posting
 * date until its rollback day. Only a batch whose replay matched its plan
 * comes out exactly; one that did not shows as a failure, never a pass.
 */
static GPtrArray *
rolled_back_history(Plan *plan, gint64 batch_id, GError **error)
{
	g_autoptr(GPtrArray) earlier = g_ptr_array_new_with_free_func(earlier_free);
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_BOOKS_IMPORT);
	g_autoptr(GPtrArray) batches = NULL;
	guint b, i;
	venture_query_set_organization(query, plan->org);
	venture_query_set_limit(query, 0);
	if (!venture_query_add_filter_string(query, "state", VENTURE_FILTER_OP_EQ, "rolled_back", error))
		return NULL;
	batches = venture_database_find(plan->self->database, query, error);
	if (batches == NULL)
		return NULL;
	for (b = 0; b < batches->len; b++)
	{
		VentureEntity *batch = g_ptr_array_index(batches, b);
		g_autoptr(GDateTime) when = NULL;
		g_autoptr(Plan) other = NULL;
		g_autofree gchar *until = NULL;
		if (venture_entity_get_id(batch) == batch_id)
			continue;
		g_object_get(batch, "rolled-back-at", &when, NULL);
		if (when == NULL)
			continue;
		until = g_date_time_format(when, "%Y-%m-%d");
		other = plan_of(plan->self, batch, error);
		if (other == NULL || !resolve_accounts(other, FALSE, NULL, error))
			return NULL;
		/* The opening comes first in the plan; its cutover rolled back at
		 * its own cutoff, so it left no history behind. */
		for (i = other->opening->len; i < other->expected->len; i++)
		{
			Posting *posting = g_ptr_array_index(other->expected, i);
			Earlier *entry = g_new0(Earlier, 1);
			entry->date = g_strdup(posting->date);
			entry->until = g_strdup(until);
			entry->account = special_account(other, posting->account, error);
			entry->cents = posting->cents;
			if (entry->account <= 0)
			{
				earlier_free(entry);
				if (error != NULL && *error != NULL)
					return NULL;
				continue;
			}
			g_ptr_array_add(earlier, entry);
		}
	}
	return g_steal_pointer(&earlier);
}

static gboolean
compare_ledger(Plan *plan, gint64 batch_id, GString *block, GPtrArray *failures, GError **error)
{
	g_autoptr(GPtrArray) earlier = rolled_back_history(plan, batch_id, error);
	g_autoptr(GHashTable) ledger = ledger_load(plan, error);
	g_autoptr(GPtrArray) dates = check_dates(plan);
	g_autoptr(GArray) ids = g_array_new(FALSE, FALSE, sizeof(gint64));
	g_autoptr(GArray) postings = g_array_new(FALSE, FALSE, sizeof(gint64));
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ACCOUNT);
	g_autoptr(GPtrArray) chart = NULL;
	guint d, i;
	if (earlier == NULL || ledger == NULL)
		return FALSE;
	/* Resolve every posting's account once. */
	for (i = 0; i < plan->expected->len; i++)
	{
		Posting *posting = g_ptr_array_index(plan->expected, i);
		gint64 id = special_account(plan, posting->account, error);
		if (id <= 0)
		{
			if (error != NULL && *error != NULL)
				return FALSE;
			g_ptr_array_add(failures, g_strdup_printf("account %s is missing", posting->account));
			g_string_append_printf(block, "FAIL account %s is missing from the chart\n", posting->account);
			id = 0;
		}
		g_array_append_val(postings, id);
	}
	/* Every account of the organization takes part, so money left in an
	 * account Zoho never named is found too. */
	venture_query_set_organization(query, plan->org);
	venture_query_set_limit(query, 0);
	chart = venture_database_find(plan->self->database, query, error);
	if (chart == NULL)
		return FALSE;
	for (i = 0; i < chart->len; i++)
	{
		gint64 id = venture_entity_get_id(g_ptr_array_index(chart, i));
		g_array_append_val(ids, id);
	}
	for (d = 0; d < dates->len; d++)
	{
		const gchar *date = g_ptr_array_index(dates, d);
		g_autoptr(GHashTable) expected = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, g_free);
		guint failed = 0;
		for (i = 0; i < plan->expected->len; i++)
		{
			Posting *posting = g_ptr_array_index(plan->expected, i);
			gint64 id = g_array_index(postings, gint64, i);
			gint64 *sum;
			if (id == 0 || strcmp(posting->date, date) > 0)
				continue;
			sum = g_hash_table_lookup(expected, &id);
			if (sum == NULL)
			{
				gint64 *key = g_new(gint64, 1);
				*key = id;
				sum = g_new0(gint64, 1);
				g_hash_table_insert(expected, key, sum);
			}
			*sum += posting->cents;
		}
		for (i = 0; i < ids->len; i++)
		{
			gint64 id = g_array_index(ids, gint64, i);
			gint64 *want = g_hash_table_lookup(expected, &id);
			gint64 have = ledger_at(ledger, id, date);
			gint64 target = want != NULL ? *want : 0;
			guint e;
			if (d == dates->len - 1)
			{
				/* The last date, from the posting service itself. */
				gint64 posted = ledger_balance(plan, id, date, error);
				if (posted == G_MININT64)
					return FALSE;
				if (posted != have)
				{
					g_autofree gchar *label = account_label(plan->self, id);
					g_string_append_printf(block, "FAIL account %s at %s: the ledger read disagrees with the posting service\n",
						label, date);
					g_ptr_array_add(failures, g_strdup_printf("ledger read for %s", label));
				}
			}
			for (e = 0; e < earlier->len; e++)
			{
				Earlier *entry = g_ptr_array_index(earlier, e);
				if (entry->account == id && strcmp(entry->date, date) <= 0 && strcmp(entry->until, date) > 0)
					have -= entry->cents;
			}
			if (have != target)
			{
				g_autofree gchar *label = account_label(plan->self, id);
				g_autofree gchar *want_text = format_amount(plan, target);
				g_autofree gchar *have_text = format_amount(plan, have);
				g_autofree gchar *diff_text = format_amount(plan, have - target);
				g_string_append_printf(block, "FAIL account %s at %s: Zoho %s, VENTURE %s, difference %s\n",
					label, date, want_text, have_text, diff_text);
				if (failed++ == 0)
					g_ptr_array_add(failures, g_strdup_printf("account %s at %s", label, date));
			}
		}
		if (failed == 0)
			g_string_append_printf(block, "PASS ledger %s: every account ties\n", date);
	}
	/* The last date's trial balance, account by account, as evidence. */
	{
		const gchar *last = g_ptr_array_index(dates, dates->len - 1);
		gint64 debit = 0, credit = 0;
		g_autofree gchar *debits = NULL;
		g_autofree gchar *credits = NULL;
		g_string_append_printf(block, "Trial balance at %s:\n", last);
		for (i = 0; i < ids->len; i++)
		{
			gint64 id = g_array_index(ids, gint64, i);
			gint64 have = ledger_at(ledger, id, last);
			g_autofree gchar *label = NULL;
			g_autofree gchar *text = NULL;
			if (have == 0)
				continue;
			label = account_label(plan->self, id);
			text = format_amount(plan, have < 0 ? -have : have);
			g_string_append_printf(block, "  %-48s %s %s\n", label, have > 0 ? "Dr" : "Cr", text);
			if (have > 0)
				debit += have;
			else
				credit -= have;
		}
		debits = format_amount(plan, debit);
		credits = format_amount(plan, credit);
		g_string_append_printf(block, "  Totals: Dr %s, Cr %s\n", debits, credits);
	}
	return TRUE;
}

/* Runs the comparison, appends it and reports whether everything tied. */
static gboolean
reconcile_run(VentureBooksImportService *self, VentureEntity *batch, gboolean *passed, gchar **summary,
	const VentureActor *actor, GError **error)
{
	g_autoptr(Plan) plan = plan_of(self, batch, error);
	g_autoptr(GString) block = g_string_new(NULL);
	g_autoptr(GPtrArray) failures = g_ptr_array_new_with_free_func(g_free);
	g_autofree gchar *stamp = stamp_now();
	g_autofree gchar *state = state_of(batch);
	guint i;
	if (plan == NULL || !resolve_accounts(plan, FALSE, actor, error))
		return FALSE;
	g_string_append_printf(block, "\n== Reconcile %s ==\n", stamp);
	if (!compare_ledger(plan, venture_entity_get_id(batch), block, failures, error) || !compare_documents(plan, batch, block, failures, error))
		return FALSE;
	*passed = failures->len == 0;
	g_string_append_printf(block, "Result: %s\n", *passed ? "reconciled" : "FAILED");
	if (summary != NULL)
	{
		g_autoptr(GString) text = g_string_new(NULL);
		for (i = 0; i < failures->len && i < 3; i++)
			g_string_append_printf(text, "%s%s", i > 0 ? "; " : "", (const gchar *)g_ptr_array_index(failures, i));
		*summary = g_string_free(g_steal_pointer(&text), FALSE);
	}
	/* A reconciled batch that no longer ties falls back to imported, so it
	 * cannot be activated on stale figures. */
	return append_report(self, batch, *passed ? "reconciled" : (g_strcmp0(state, "reconciled") == 0 ? "imported" : NULL),
		block, actor, error);
}

gboolean
venture_books_import_service_reconcile(VentureBooksImportService *self, VentureBooksImport *batch,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) stored = NULL;
	g_autofree gchar *state = NULL;
	g_autofree gchar *summary = NULL;
	gboolean passed = FALSE;
	g_return_val_if_fail(VENTURE_IS_BOOKS_IMPORT_SERVICE(self), FALSE);
	stored = venture_database_get(self->database, VENTURE_TYPE_BOOKS_IMPORT, venture_entity_get_id(VENTURE_ENTITY(batch)), error);
	if (stored == NULL)
		return FALSE;
	state = state_of(stored);
	if (g_strcmp0(state, "imported") != 0 && g_strcmp0(state, "reconciled") != 0)
		return refuse(error, "reconcile after import");
	if (!reconcile_run(self, stored, &passed, &summary, actor, error))
		return FALSE;
	venture_entity_copy_properties_from(VENTURE_ENTITY(batch), stored, FALSE);
	if (!passed)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"VentureBooksImportService: the books do not reconcile: %s", summary);
		return FALSE;
	}
	return TRUE;
}

gboolean
venture_books_import_service_activate(VentureBooksImportService *self, VentureBooksImport *batch,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) stored = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	g_autofree gchar *state = NULL;
	g_autofree gchar *summary = NULL;
	g_autoptr(GString) block = g_string_new(NULL);
	g_autofree gchar *stamp = stamp_now();
	gboolean passed = FALSE;
	gint64 cutover_id = 0;
	g_return_val_if_fail(VENTURE_IS_BOOKS_IMPORT_SERVICE(self), FALSE);
	stored = venture_database_get(self->database, VENTURE_TYPE_BOOKS_IMPORT, venture_entity_get_id(VENTURE_ENTITY(batch)), error);
	if (stored == NULL)
		return FALSE;
	state = state_of(stored);
	if (g_strcmp0(state, "active") == 0)
		return TRUE;
	if (g_strcmp0(state, "reconciled") != 0)
		return refuse(error, "activate a reconciled batch");
	/* The books as they are now, not as they were at the last reconcile. */
	if (!reconcile_run(self, stored, &passed, &summary, actor, error))
		return FALSE;
	if (!passed)
	{
		venture_entity_copy_properties_from(VENTURE_ENTITY(batch), stored, FALSE);
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"VentureBooksImportService: the books moved since reconcile: %s", summary);
		return FALSE;
	}
	g_object_get(stored, "cutover-id", &cutover_id, NULL);
	if (cutover_id > 0)
	{
		VentureCutoverService *cutover = venture_cutover_service_get(self->database);
		g_autoptr(VentureEntity) opening = venture_database_get(self->database, VENTURE_TYPE_ACCOUNTING_CUTOVER, cutover_id, error);
		if (opening == NULL ||
			!venture_cutover_service_reconcile(cutover, VENTURE_ACCOUNTING_CUTOVER(opening), actor, error) ||
			!venture_cutover_service_activate(cutover, VENTURE_ACCOUNTING_CUTOVER(opening), actor, error))
			return FALSE;
	}
	/* Its books are now the ledger: its P&L and dashboard read them, not
	 * the sale and expense records an operational business keeps. */
	{
		g_autoptr(VentureEntity) organization = venture_database_get(self->database, VENTURE_TYPE_ORGANIZATION,
			venture_entity_get_organization_id(stored), error);
		if (organization == NULL)
			return FALSE;
		g_object_set(organization, "pnl-basis", "ledger", NULL);
		if (!venture_database_save(self->database, organization, actor, error))
			return FALSE;
	}
	g_string_append_printf(block, "\n== Activated %s ==\nThe imported books are the organization's own; "
		"its profit and loss now reads the ledger.\n", stamp);
	g_object_set(stored, "activated-at", now, NULL);
	if (!append_report(self, stored, "active", block, actor, error))
		return FALSE;
	venture_entity_copy_properties_from(VENTURE_ENTITY(batch), stored, FALSE);
	return TRUE;
}
