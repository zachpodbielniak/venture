/* SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Reconciling an imported batch. Included by venture-cutover-service.c after
 * the importers.
 *
 * Every figure is compared as of the opening: all activity strictly before the
 * cutoff instant, plus the opening postings made at it. Activity recorded in
 * VENTURE at or after the cutoff is not drift -- a receipt dated on the
 * cutoff day is January's cash, not December's -- so it is left out on both
 * sides of every comparison. Totals are kept per currency and never netted.
 */

/* --- The ledger at the opening -------------------------------------------------- */

static gboolean
payment_is_opening(VentureCutoverService *self, gboolean vendor, gint64 payment_id)
{
	g_autoptr(VentureEntity) payment = NULL;
	g_autofree gchar *method = NULL;
	if (payment_id <= 0)
		return FALSE;
	payment = venture_database_get(self->database, vendor ? VENTURE_TYPE_BILL_PAYMENT : VENTURE_TYPE_PAYMENT, payment_id, NULL);
	if (payment == NULL)
		return FALSE;
	g_object_get(payment, "method", &method, NULL);
	return g_strcmp0(method, "opening") == 0;
}

/* Whether a record some cutover imported is still live: a rolled-back
 * batch's documents, payments and credits drop out of the opening. */
static gboolean
record_row_live(VentureCutoverService *self, const gchar *record_type, gint64 id)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ACCOUNTING_CUTOVER_ROW);
	g_autoptr(GPtrArray) rows = NULL;
	guint i;
	venture_query_set_limit(query, 0);
	if (id <= 0 || !venture_query_add_filter_int(query, "record-id", VENTURE_FILTER_OP_EQ, id, NULL))
		return FALSE;
	rows = venture_database_find(self->database, query, NULL);
	for (i = 0; rows != NULL && i < rows->len; i++)
	{
		g_autofree gchar *type = NULL;
		g_autofree gchar *status = NULL;
		g_object_get(g_ptr_array_index(rows, i), "record-type", &type, "status", &status, NULL);
		if (g_strcmp0(type, record_type) == 0 && g_strcmp0(status, "imported") == 0)
			return TRUE;
	}
	return FALSE;
}

/*
 * Whether a journal dated exactly at the cutoff is an opening posting of a
 * live batch, by what it was posted for. Everything else at that instant is
 * the new books' first activity. Journals whose sources are zero-net
 * transfers within one control account need no classification.
 */
static gboolean
journal_is_opening(VentureCutoverService *self, VentureEntity *journal)
{
	g_autofree gchar *type = NULL;
	gint64 id = 0;
	g_object_get(journal, "source-type", &type, "source-id", &id, NULL);
	if (g_strcmp0(type, "accounting_cutover") == 0)
	{
		g_autoptr(VentureEntity) batch = venture_database_get(self->database, VENTURE_TYPE_ACCOUNTING_CUTOVER, id, NULL);
		g_autofree gchar *state = NULL;
		if (batch == NULL)
			return FALSE;
		g_object_get(batch, "state", &state, NULL);
		return g_strcmp0(state, "rolled_back") != 0;
	}
	if (g_strcmp0(type, "invoice_event") == 0 || g_strcmp0(type, "vendor_bill_event") == 0)
	{
		gboolean vendor = g_strcmp0(type, "vendor_bill_event") == 0;
		g_autoptr(VentureEntity) event = venture_database_get(self->database,
			vendor ? VENTURE_TYPE_VENDOR_BILL_EVENT : VENTURE_TYPE_INVOICE_EVENT, id, NULL);
		gint64 document = 0;
		if (event == NULL)
			return FALSE;
		g_object_get(event, vendor ? "bill-id" : "invoice-id", &document, NULL);
		return record_row_live(self, vendor ? "vendor_bill" : "invoice", document);
	}
	if (g_strcmp0(type, "customer_credit") == 0 || g_strcmp0(type, "vendor_credit") == 0 ||
		g_strcmp0(type, "payment") == 0 || g_strcmp0(type, "bill_payment") == 0 ||
		g_strcmp0(type, "inventory_txn") == 0)
		return record_row_live(self, type, id);
	return FALSE;
}

/*
 * Book balances per account and currency as of the opening, keyed
 * "account-id|CUR". A rolled-back opening journal and its reversal both drop
 * out at the cutoff, so a corrected batch is not compared against the
 * batch it replaced.
 */
static GHashTable *
opening_ledger(VentureCutoverService *self, gint64 org, GDateTime *cutoff, GError **error)
{
	g_autoptr(GHashTable) ledger = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)venture_money_free);
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_JOURNAL);
	g_autoptr(GPtrArray) journals = NULL;
	guint i, j;
	venture_query_set_organization(query, org);
	venture_query_set_include_deleted(query, TRUE);
	venture_query_set_limit(query, 0);
	journals = venture_database_find(self->database, query, error);
	if (journals == NULL)
		return NULL;
	for (i = 0; i < journals->len; i++)
	{
		VentureEntity *journal = g_ptr_array_index(journals, i);
		g_autoptr(GDateTime) when = NULL;
		g_autoptr(VentureQuery) lines_query = NULL;
		g_autoptr(GPtrArray) lines = NULL;
		g_autofree gchar *currency = NULL;
		VentureJournalState state;
		gint64 reverses = 0;
		gint order;
		g_object_get(journal, "state", &state, "occurred-at", &when, "currency", &currency, "reverses-id", &reverses, NULL);
		if ((state != VENTURE_JOURNAL_POSTED && state != VENTURE_JOURNAL_REVERSED) || when == NULL)
			continue;
		order = g_date_time_compare(when, cutoff);
		if (order > 0)
			continue;
		if (order == 0 && (state != VENTURE_JOURNAL_POSTED || reverses > 0 || !journal_is_opening(self, journal)))
			continue;
		lines_query = venture_query_new(VENTURE_TYPE_JOURNAL_LINE);
		venture_query_set_limit(lines_query, 0);
		if (!venture_query_add_filter_int(lines_query, "journal-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(journal), error))
			return NULL;
		lines = venture_database_find(self->database, lines_query, error);
		if (lines == NULL)
			return NULL;
		for (j = 0; j < lines->len; j++)
		{
			g_autoptr(VentureMoney) amount = NULL;
			g_autofree gchar *key = NULL;
			VentureMoney *next;
			gint64 account = 0;
			gint side;
			g_object_get(g_ptr_array_index(lines, j), "account-id", &account, "side", &side, "book-amount", &amount, NULL);
			if (amount == NULL)
				continue;
			key = g_strdup_printf("%" G_GINT64_FORMAT "|%s", account, venture_money_get_currency(amount));
			next = side == VENTURE_LEDGER_SIDE_DEBIT ? money_sum(g_hash_table_lookup(ledger, key), amount)
				: money_difference(g_hash_table_lookup(ledger, key), amount);
			if (next == NULL)
			{
				refuse(error, "an opening ledger total overflowed");
				return NULL;
			}
			g_hash_table_insert(ledger, g_steal_pointer(&key), next);
		}
	}
	return g_steal_pointer(&ledger);
}

static VentureMoney *
ledger_balance(GHashTable *ledger, gint64 account, const gchar *currency)
{
	g_autofree gchar *key = g_strdup_printf("%" G_GINT64_FORMAT "|%s", account, currency);
	VentureMoney *found = g_hash_table_lookup(ledger, key);
	return found != NULL ? venture_money_copy(found) : venture_money_new_zero(currency);
}

/* --- Subledgers at the opening ----------------------------------------------------- */

static gboolean
before_or_opening(VentureCutoverService *self, gboolean vendor, VentureEntity *record, GDateTime *cutoff)
{
	g_autoptr(GDateTime) date = NULL;
	gint order;
	g_object_get(record, "date", &date, NULL);
	if (date == NULL)
		return FALSE;
	order = g_date_time_compare(date, cutoff);
	if (order < 0)
		return TRUE;
	if (order > 0)
		return FALSE;
	/* An allocation at the cutoff counts only when it is the opening
	 * payment that records what was paid in the source system. */
	if (g_object_class_find_property(G_OBJECT_GET_CLASS(record), "payment-id") != NULL)
	{
		gint64 payment = 0;
		g_object_get(record, "payment-id", &payment, NULL);
		return payment_is_opening(self, vendor, payment);
	}
	return FALSE;
}

static GPtrArray *
records_for(VentureCutoverService *self, GType type, const gchar *field, gint64 id, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(type);
	venture_query_set_limit(query, 0);
	venture_query_set_include_deleted(query, TRUE);
	if (!venture_query_add_filter_int(query, field, VENTURE_FILTER_OP_EQ, id, error))
		return NULL;
	return venture_database_find(self->database, query, error);
}

/* An invoice or bill's balance as of the opening: its issue less allocations
 * before the cutoff and the opening payment, plus refunds of those. */
static VentureMoney *
opening_document_balance(VentureCutoverService *self, gboolean vendor, gint64 id, GDateTime *cutoff, GError **error)
{
	g_autoptr(GPtrArray) events = records_for(self, vendor ? VENTURE_TYPE_VENDOR_BILL_EVENT : VENTURE_TYPE_INVOICE_EVENT,
		vendor ? "bill-id" : "invoice-id", id, error);
	g_autoptr(GPtrArray) allocations = NULL;
	g_autoptr(VentureMoney) balance = NULL;
	gboolean voided = FALSE;
	guint i, j;
	if (events == NULL)
		return NULL;
	for (i = 0; i < events->len; i++)
	{
		VentureEntity *event = g_ptr_array_index(events, i);
		g_autofree gchar *kind = NULL;
		g_autoptr(VentureMoney) amount = NULL;
		g_autoptr(GDateTime) date = NULL;
		g_object_get(event, "kind", &kind, "amount", &amount, "date", &date, NULL);
		if (date == NULL || g_date_time_compare(date, cutoff) >= 0)
			continue;
		if (g_strcmp0(kind, "issue") == 0 && amount != NULL)
			balance = venture_money_copy(amount);
		else if (g_strcmp0(kind, "void") == 0)
			voided = TRUE;
	}
	if (balance == NULL)
	{
		refuse(error, "an imported document has no issue event before the cutoff");
		return NULL;
	}
	if (voided)
		return venture_money_new_zero(venture_money_get_currency(balance));
	allocations = records_for(self, vendor ? VENTURE_TYPE_BILL_PAYMENT_ALLOCATION : VENTURE_TYPE_PAYMENT_ALLOCATION,
		vendor ? "bill-id" : "invoice-id", id, error);
	if (allocations == NULL)
		return NULL;
	for (i = 0; i < allocations->len; i++)
	{
		VentureEntity *allocation = g_ptr_array_index(allocations, i);
		g_autoptr(GPtrArray) refunds = NULL;
		g_autoptr(VentureMoney) amount = NULL;
		VentureMoney *next;
		if (!before_or_opening(self, vendor, allocation, cutoff))
			continue;
		g_object_get(allocation, "amount", &amount, NULL);
		next = money_difference(balance, amount);
		g_clear_pointer(&balance, venture_money_free);
		balance = next;
		refunds = records_for(self, vendor ? VENTURE_TYPE_BILL_REFUND : VENTURE_TYPE_REFUND, "allocation-id",
			venture_entity_get_id(allocation), error);
		if (refunds == NULL || balance == NULL)
			return NULL;
		for (j = 0; j < refunds->len; j++)
		{
			g_autoptr(VentureMoney) refunded = NULL;
			g_autoptr(GDateTime) date = NULL;
			g_object_get(g_ptr_array_index(refunds, j), "amount", &refunded, "date", &date, NULL);
			if (date == NULL || g_date_time_compare(date, cutoff) >= 0)
				continue;
			next = money_sum(balance, refunded);
			g_clear_pointer(&balance, venture_money_free);
			balance = next;
		}
	}
	return g_steal_pointer(&balance);
}

/* A credit's unapplied amount as of the opening. A prepayment's credit is
 * dated at the cutoff and nothing can have used it before. */
static VentureMoney *
opening_credit_remaining(VentureCutoverService *self, gboolean vendor, VentureEntity *credit, GDateTime *cutoff, GError **error)
{
	g_autoptr(VentureMoney) remaining = NULL;
	g_autoptr(GPtrArray) allocations = NULL;
	g_autoptr(GPtrArray) refunds = NULL;
	gint64 payment = 0;
	guint i, pass;
	g_object_get(credit, "amount", &remaining, "payment-id", &payment, NULL);
	if (remaining == NULL)
	{
		refuse(error, "an imported credit has no amount");
		return NULL;
	}
	for (pass = 0; pass < 2; pass++)
	{
		const gchar *field = pass == 0 ? "credit-id" : "payment-id";
		gint64 id = pass == 0 ? venture_entity_get_id(credit) : payment;
		if (id <= 0)
			continue;
		g_clear_pointer(&allocations, g_ptr_array_unref);
		allocations = records_for(self, vendor ? VENTURE_TYPE_BILL_PAYMENT_ALLOCATION : VENTURE_TYPE_PAYMENT_ALLOCATION,
			field, id, error);
		if (allocations == NULL)
			return NULL;
		for (i = 0; i < allocations->len; i++)
		{
			VentureEntity *allocation = g_ptr_array_index(allocations, i);
			g_autoptr(VentureMoney) amount = NULL;
			g_autoptr(GDateTime) date = NULL;
			VentureMoney *next;
			g_object_get(allocation, "amount", &amount, "date", &date, NULL);
			if (date == NULL || g_date_time_compare(date, cutoff) >= 0)
				continue;
			next = money_difference(remaining, amount);
			g_clear_pointer(&remaining, venture_money_free);
			remaining = next;
		}
	}
	refunds = records_for(self, vendor ? VENTURE_TYPE_BILL_REFUND : VENTURE_TYPE_REFUND, "credit-id",
		venture_entity_get_id(credit), error);
	if (refunds == NULL)
		return NULL;
	for (i = 0; i < refunds->len; i++)
	{
		g_autoptr(VentureMoney) amount = NULL;
		g_autoptr(GDateTime) date = NULL;
		VentureMoney *next;
		g_object_get(g_ptr_array_index(refunds, i), "amount", &amount, "date", &date, NULL);
		if (date == NULL || g_date_time_compare(date, cutoff) >= 0)
			continue;
		next = money_difference(remaining, amount);
		g_clear_pointer(&remaining, venture_money_free);
		remaining = next;
	}
	return g_steal_pointer(&remaining);
}

/* Net book value the register reports for one migrated asset: cost less the
 * depreciation posted on or before the cutoff, which is its opening entry. */
static VentureMoney *
asset_register_nbv(VentureCutoverService *self, gint64 asset_id, GDateTime *cutoff, GError **error)
{
	g_autoptr(VentureEntity) asset = venture_database_get(self->database, VENTURE_TYPE_FIXED_ASSET, asset_id, error);
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(VentureMoney) cost = NULL;
	g_autoptr(VentureMoney) nbv = NULL;
	guint i;
	if (asset == NULL)
		return NULL;
	g_object_get(asset, "cost", &cost, NULL);
	nbv = venture_money_copy(cost);
	rows = records_for(self, VENTURE_TYPE_DEPRECIATION_ENTRY, "asset-id", asset_id, error);
	if (rows == NULL)
		return NULL;
	for (i = 0; i < rows->len; i++)
	{
		g_autoptr(VentureEntity) journal = NULL;
		g_autoptr(GDateTime) date = NULL;
		g_autoptr(VentureMoney) amount = NULL;
		VentureMoney *next;
		gint state, journal_state;
		gint64 journal_id = 0;
		g_object_get(g_ptr_array_index(rows, i), "state", &state, "journal-id", &journal_id, "amount", &amount, NULL);
		if (state != VENTURE_SCHEDULE_STATE_POSTED || journal_id == 0)
			continue;
		journal = venture_database_get(self->database, VENTURE_TYPE_JOURNAL, journal_id, error);
		if (journal == NULL)
			return NULL;
		g_object_get(journal, "occurred-at", &date, "state", &journal_state, NULL);
		if (journal_state != VENTURE_JOURNAL_POSTED || date == NULL || g_date_time_compare(date, cutoff) > 0)
			continue;
		next = money_difference(nbv, amount);
		g_clear_pointer(&nbv, venture_money_free);
		nbv = next;
	}
	return g_steal_pointer(&nbv);
}

/* The value an opening stock receipt carries into the books: its quantity at
 * its unit cost, provided it is dated at or before the cutoff. */
static VentureMoney *
opening_stock_value(VentureCutoverService *self, gint64 txn_id, GDateTime *cutoff, GError **error)
{
	g_autoptr(VentureEntity) txn = venture_database_get(self->database, VENTURE_TYPE_INVENTORY_TXN, txn_id, error);
	g_autoptr(VentureMoney) unit = NULL;
	g_autoptr(GDateTime) when = NULL;
	gint64 quantity = 0;
	if (txn == NULL)
		return NULL;
	g_object_get(txn, "unit-cost", &unit, "quantity", &quantity, "occurred-at", &when, NULL);
	if (unit == NULL)
	{
		refuse(error, "an opening stock receipt has no unit cost");
		return NULL;
	}
	if (when == NULL || g_date_time_compare(when, cutoff) > 0)
		return venture_money_new_zero(venture_money_get_currency(unit));
	return venture_money_multiply_int(unit, quantity, error);
}

/* --- Checks ------------------------------------------------------------------------ */

typedef struct
{
	gchar *label;
	const gchar *actual_name;
	VentureMoney *expected;
	VentureMoney *actual;
	GPtrArray *details;
} Check;

static void
check_free(gpointer data)
{
	Check *check = data;
	g_free(check->label);
	g_clear_pointer(&check->expected, venture_money_free);
	g_clear_pointer(&check->actual, venture_money_free);
	g_ptr_array_unref(check->details);
	g_free(check);
}

typedef struct
{
	GPtrArray *checks;
	GHashTable *by_label;
	GPtrArray *owned;
	GPtrArray *notes;
} Checks;

static void
checks_free(Checks *checks)
{
	g_ptr_array_unref(checks->checks);
	g_hash_table_unref(checks->by_label);
	g_ptr_array_unref(checks->owned);
	g_ptr_array_unref(checks->notes);
	g_free(checks);
}
G_DEFINE_AUTOPTR_CLEANUP_FUNC(Checks, checks_free)

static Checks *
checks_new(void)
{
	Checks *checks = g_new0(Checks, 1);
	checks->checks = g_ptr_array_new_with_free_func(check_free);
	checks->by_label = g_hash_table_new(g_str_hash, g_str_equal);
	checks->owned = g_ptr_array_new_with_free_func(g_free);
	checks->notes = g_ptr_array_new_with_free_func(g_free);
	return checks;
}

static gboolean
check_passes(const Check *check)
{
	return check->expected != NULL && check->actual != NULL && venture_money_equal(check->expected, check->actual);
}

static gchar *
difference_text(const VentureMoney *expected, const VentureMoney *actual)
{
	g_autoptr(VentureMoney) difference = money_difference(actual, expected);
	return difference != NULL ? venture_money_to_string(difference) : g_strdup("not comparable");
}

/* Adds @expected and @actual into the running check @label, keeping the
 * first failing row of each as evidence. @actual may be NULL for a row that
 * was never imported. */
static void
check_add(Checks *checks, const gchar *label, const gchar *actual_name, const gchar *row_label,
	const VentureMoney *expected, const VentureMoney *actual)
{
	Check *check = g_hash_table_lookup(checks->by_label, label);
	VentureMoney *next;
	if (check == NULL)
	{
		check = g_new0(Check, 1);
		check->label = g_strdup(label);
		check->actual_name = actual_name;
		check->expected = venture_money_new_zero(venture_money_get_currency(expected));
		check->actual = venture_money_new_zero(venture_money_get_currency(expected));
		check->details = g_ptr_array_new_with_free_func(g_free);
		g_ptr_array_add(checks->checks, check);
		g_hash_table_insert(checks->by_label, check->label, check);
	}
	next = money_sum(check->expected, expected);
	g_clear_pointer(&check->expected, venture_money_free);
	check->expected = next;
	if (actual != NULL && check->actual != NULL)
	{
		next = money_sum(check->actual, actual);
		g_clear_pointer(&check->actual, venture_money_free);
		check->actual = next;
	}
	if (row_label != NULL && (actual == NULL || !venture_money_equal(expected, actual)))
	{
		g_autofree gchar *e = venture_money_to_string(expected);
		g_autofree gchar *a = actual != NULL ? venture_money_to_string(actual) : g_strdup("nothing imported");
		g_autofree gchar *d = actual != NULL ? difference_text(expected, actual) : g_strdup("n/a");
		if (actual == NULL)
		{
			g_clear_pointer(&check->actual, venture_money_free);
		}
		g_ptr_array_add(check->details, g_strdup_printf("%s: expected %s, %s %s, difference %s", row_label, e,
			actual_name, a, d));
	}
}

static GHashTable *
batch_rows(VentureCutoverService *self, gint64 cutover_id, GError **error)
{
	g_autoptr(GHashTable) rows = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
	g_autoptr(GPtrArray) found = NULL;
	guint i;
	if (!cutover_rows(self, cutover_id, &found, error))
		return NULL;
	for (i = 0; i < found->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(found, i);
		g_autofree gchar *type = NULL;
		g_autofree gchar *source = NULL;
		g_autofree gchar *status = NULL;
		g_object_get(row, "source-type", &type, "source-id", &source, "status", &status, NULL);
		if (g_strcmp0(status, "imported") != 0 && g_strcmp0(status, "skipped") != 0)
			continue;
		g_hash_table_insert(rows, g_strdup_printf("%s|%s", type, source != NULL ? source : ""), g_object_ref(row));
	}
	return g_steal_pointer(&rows);
}

static gboolean
subledger_checks(VentureCutoverService *self, Plan *plan, GHashTable *rows, Checks *checks, GError **error)
{
	static const struct { const gchar *section; const gchar *type; } kinds[] = {
		{ "open_ar", "open_ar" }, { "open_ap", "open_ap" }, { "credits", "credit" }, { "assets", "asset" },
		{ "inventory", "inventory" }
	};
	guint k, i;
	for (k = 0; k < G_N_ELEMENTS(kinds); k++)
	{
		GPtrArray *section = k == 0 ? plan->open_ar : (k == 1 ? plan->open_ap : (k == 2 ? plan->credits :
			(k == 3 ? plan->assets : plan->inventory)));
		for (i = 0; i < section->len; i++)
		{
			Row *row = g_ptr_array_index(section, i);
			g_autofree gchar *key = g_strdup_printf("%s|%s", kinds[k].type, row->source_id);
			g_autofree gchar *label = NULL;
			g_autofree gchar *where = g_strdup_printf("%s[%d] %s", row->section, row->position, row->source_id);
			g_autofree gchar *status = NULL;
			g_autofree gchar *record_type = NULL;
			g_autofree gchar *exception = NULL;
			g_autoptr(VentureMoney) actual = NULL;
			VentureEntity *batch_row;
			gint64 record_id = 0;
			if (!row->ok || row->open == NULL)
				continue;
			label = g_strdup_printf("%s %s", kinds[k].section, venture_money_get_currency(row->open));
			batch_row = g_hash_table_lookup(rows, key);
			if (batch_row == NULL)
			{
				check_add(checks, label, "subledger", where, row->open, NULL);
				continue;
			}
			g_object_get(batch_row, "status", &status, "record-type", &record_type, "record-id", &record_id,
				"exception", &exception, NULL);
			if (g_strcmp0(status, "skipped") == 0)
			{
				g_ptr_array_add(checks->owned, g_strdup_printf("%s: %s", where, exception != NULL ? exception : "owned elsewhere"));
				continue;
			}
			if (g_strcmp0(record_type, "invoice") == 0 || g_strcmp0(record_type, "vendor_bill") == 0)
				actual = opening_document_balance(self, g_strcmp0(record_type, "vendor_bill") == 0, record_id, plan->cutoff, error);
			else if (g_strcmp0(record_type, "fixed_asset") == 0)
				actual = asset_register_nbv(self, record_id, plan->cutoff, error);
			else if (g_strcmp0(record_type, "inventory_txn") == 0)
				actual = opening_stock_value(self, record_id, plan->cutoff, error);
			else
			{
				gboolean vendor = g_strcmp0(record_type, "vendor_credit") == 0 || g_strcmp0(record_type, "bill_payment") == 0;
				g_autoptr(VentureEntity) credit = NULL;
				if (g_strcmp0(record_type, "payment") == 0 || g_strcmp0(record_type, "bill_payment") == 0)
				{
					g_autoptr(GPtrArray) credits = records_for(self, vendor ? VENTURE_TYPE_VENDOR_CREDIT : VENTURE_TYPE_CUSTOMER_CREDIT,
						"payment-id", record_id, error);
					if (credits == NULL)
						return FALSE;
					if (credits->len > 0)
						credit = g_object_ref(g_ptr_array_index(credits, 0));
				}
				else
					credit = venture_database_get(self->database, vendor ? VENTURE_TYPE_VENDOR_CREDIT : VENTURE_TYPE_CUSTOMER_CREDIT,
						record_id, error);
				if (credit == NULL)
					return refuse(error, "an imported credit or prepayment is missing");
				actual = opening_credit_remaining(self, vendor, credit, plan->cutoff, error);
				/* A negative receivable is owed back: compare it as one. */
				if (actual != NULL && venture_money_get_amount(row->open) < 0)
				{
					VentureMoney *negated = venture_money_negate(actual);
					g_clear_pointer(&actual, venture_money_free);
					actual = negated;
				}
			}
			if (actual == NULL)
				return FALSE;
			check_add(checks, label, "subledger", where, row->open, actual);
		}
	}
	return TRUE;
}

/*
 * The ledger side. Bank accounts are compared per GL account and currency
 * against every bank row the payload maps there. With a trial balance, every
 * row is compared with the opening ledger -- a control account proves its
 * subledger, any other account proves the trial balance journal -- a control
 * account holding a balance the trial balance does not name fails, and the
 * clearing account must be zero in every currency.
 */
static gboolean
ledger_checks(VentureCutoverService *self, Plan *plan, Checks *checks, GError **error)
{
	g_autoptr(GHashTable) ledger = opening_ledger(self, plan->org, plan->cutoff, error);
	g_autoptr(GHashTable) controls = NULL;
	g_autoptr(GHashTable) named = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	g_autoptr(GHashTable) currencies = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	g_autoptr(VentureEntity) clearing = NULL;
	GHashTableIter iter;
	gpointer key, value;
	guint i;
	if (ledger == NULL)
		return FALSE;
	controls = control_accounts(plan, error);
	if (controls == NULL)
		return FALSE;
	clearing = account_with_code(self, plan->org, plan->clearing_code, NULL);
	for (i = 0; i < plan->bank->len; i++)
	{
		Row *row = g_ptr_array_index(plan->bank, i);
		g_autoptr(VentureEntity) account = NULL;
		g_autoptr(VentureMoney) zero = NULL;
		g_autofree gchar *label = NULL;
		if (!row->ok || row->total == NULL)
			continue;
		account = account_with_code(self, plan->org, row->account_code, NULL);
		label = g_strdup_printf("bank %s %s", row->account_code, venture_money_get_currency(row->total));
		zero = venture_money_new_zero(venture_money_get_currency(row->total));
		check_add(checks, label, "ledger", NULL, row->total, zero);
		if (account != NULL)
		{
			Check *check = g_hash_table_lookup(checks->by_label, label);
			g_autoptr(VentureMoney) balance = ledger_balance(ledger, venture_entity_get_id(account),
				venture_money_get_currency(row->total));
			g_clear_pointer(&check->actual, venture_money_free);
			check->actual = g_steal_pointer(&balance);
		}
	}
	if (plan->trial_balance->len == 0)
	{
		if (clearing != NULL)
		{
			g_hash_table_iter_init(&iter, ledger);
			while (g_hash_table_iter_next(&iter, &key, &value))
			{
				g_autofree gchar *prefix = g_strdup_printf("%" G_GINT64_FORMAT "|", venture_entity_get_id(clearing));
				g_autofree gchar *text = NULL;
				if (!g_str_has_prefix(key, prefix) || venture_money_is_zero(value))
					continue;
				text = venture_money_to_string(value);
				g_ptr_array_add(checks->notes, g_strdup_printf(
					"No trial balance: opening clearing %s holds %s. Import the trial balance to move it into the accounts it belongs to.",
					plan->clearing_code, text));
			}
		}
		return TRUE;
	}
	for (i = 0; i < plan->trial_balance->len; i++)
	{
		Row *row = g_ptr_array_index(plan->trial_balance, i);
		g_autoptr(VentureEntity) account = NULL;
		g_autoptr(VentureMoney) balance = NULL;
		g_autofree gchar *label = NULL;
		gint64 id;
		if (!row->ok || row->total == NULL)
			continue;
		account = account_with_code(self, plan->org, row->account_code, NULL);
		if (account == NULL)
			continue;
		id = venture_entity_get_id(account);
		g_hash_table_add(currencies, g_strdup(venture_money_get_currency(row->total)));
		g_hash_table_add(named, g_strdup_printf("%" G_GINT64_FORMAT "|%s", id, venture_money_get_currency(row->total)));
		if (g_hash_table_contains(controls, &id))
			label = g_strdup_printf("trial_balance %s (%s) %s", row->account_code,
				(const gchar *)g_hash_table_lookup(controls, &id), venture_money_get_currency(row->total));
		else
			label = g_strdup_printf("trial_balance %s %s", row->account_code, venture_money_get_currency(row->total));
		balance = ledger_balance(ledger, id, venture_money_get_currency(row->total));
		check_add(checks, label, "ledger", NULL, row->total, balance);
	}
	/* A control account the trial balance forgot still holds subledger
	 * postings; without this the clearing check would be the only symptom. */
	g_hash_table_iter_init(&iter, ledger);
	while (g_hash_table_iter_next(&iter, &key, &value))
	{
		g_auto(GStrv) parts = g_strsplit(key, "|", 2);
		gint64 id = g_ascii_strtoll(parts[0], NULL, 10);
		if (!g_hash_table_contains(controls, &id) || g_hash_table_contains(named, key) || venture_money_is_zero(value))
			continue;
		{
			g_autoptr(VentureEntity) account = venture_database_get(self->database, VENTURE_TYPE_ACCOUNT, id, NULL);
			g_autofree gchar *code = NULL;
			g_autofree gchar *label = NULL;
			g_autoptr(VentureMoney) zero = venture_money_new_zero(parts[1]);
			if (account != NULL)
				g_object_get(account, "code", &code, NULL);
			label = g_strdup_printf("trial_balance %s (%s, missing from the trial balance) %s", code != NULL ? code : parts[0],
				(const gchar *)g_hash_table_lookup(controls, &id), parts[1]);
			check_add(checks, label, "ledger", NULL, zero, value);
		}
	}
	if (clearing != NULL)
	{
		g_hash_table_iter_init(&iter, ledger);
		while (g_hash_table_iter_next(&iter, &key, &value))
		{
			g_auto(GStrv) parts = g_strsplit(key, "|", 2);
			if (g_ascii_strtoll(parts[0], NULL, 10) == venture_entity_get_id(clearing))
				g_hash_table_add(currencies, g_strdup(parts[1]));
		}
		g_hash_table_iter_init(&iter, currencies);
		while (g_hash_table_iter_next(&iter, &key, NULL))
		{
			g_autoptr(VentureMoney) zero = venture_money_new_zero(key);
			g_autoptr(VentureMoney) balance = ledger_balance(ledger, venture_entity_get_id(clearing), key);
			g_autofree gchar *label = g_strdup_printf("clearing %s %s", plan->clearing_code, (const gchar *)key);
			check_add(checks, label, "ledger", NULL, zero, balance);
		}
	}
	return TRUE;
}

/*
 * Runs every check and appends one dated block to @report. Returns FALSE
 * only for a failure to read; @passed says whether the books agree, and
 * @summary names the first failing checks for the refusal.
 */
static gboolean
reconcile_run(VentureCutoverService *self, VentureEntity *cutover, Plan *plan, GString *report,
	gboolean *passed, gchar **summary, GError **error)
{
	g_autoptr(Checks) checks = checks_new();
	g_autoptr(GHashTable) rows = batch_rows(self, venture_entity_get_id(cutover), error);
	g_autoptr(GDateTime) now = venture_time_now();
	g_autofree gchar *stamp = g_date_time_format_iso8601(now);
	g_autoptr(GString) failures = g_string_new(NULL);
	guint i, j, failed = 0;
	if (rows == NULL || !subledger_checks(self, plan, rows, checks, error) || !ledger_checks(self, plan, checks, error))
		return FALSE;
	g_string_append_printf(report, "\n== Reconcile %s ==\n", stamp);
	for (i = 0; i < checks->checks->len; i++)
	{
		Check *check = g_ptr_array_index(checks->checks, i);
		g_autofree gchar *e = venture_money_to_string(check->expected);
		g_autofree gchar *a = check->actual != NULL ? venture_money_to_string(check->actual) : g_strdup("incomplete");
		gboolean pass = check_passes(check) && check->details->len == 0;
		if (pass)
			g_string_append_printf(report, "PASS %s: expected %s, %s %s\n", check->label, e, check->actual_name, a);
		else
		{
			g_autofree gchar *d = check->actual != NULL ? difference_text(check->expected, check->actual) : g_strdup("n/a");
			g_string_append_printf(report, "FAIL %s: expected %s, %s %s, difference %s\n", check->label, e,
				check->actual_name, a, d);
			for (j = 0; j < check->details->len; j++)
				g_string_append_printf(report, "    %s\n", (const gchar *)g_ptr_array_index(check->details, j));
			if (failed < 3)
				g_string_append_printf(failures, "%s%s expected %s, %s %s (difference %s)", failed > 0 ? "; " : "",
					check->label, e, check->actual_name, a, d);
			failed++;
		}
	}
	if (checks->owned->len > 0)
	{
		g_string_append(report, "Owned by other batches and not counted here:\n");
		for (i = 0; i < checks->owned->len; i++)
			g_string_append_printf(report, "- %s\n", (const gchar *)g_ptr_array_index(checks->owned, i));
	}
	for (i = 0; i < checks->notes->len; i++)
		g_string_append_printf(report, "%s\n", (const gchar *)g_ptr_array_index(checks->notes, i));
	if (failed == 0 && plan->trial_balance->len > 0)
		g_string_append(report, "Trial balance ties at cutoff: every account and opening clearing agree.\n");
	g_string_append_printf(report, "Result: %s\n", failed == 0 ? "reconciled" : "FAILED");
	*passed = failed == 0;
	if (failed > 3)
		g_string_append_printf(failures, "; and %u more", failed - 3);
	*summary = g_string_free(g_steal_pointer(&failures), FALSE);
	return TRUE;
}

/*
 * Preview predicts the ledger checks from the payload alone: what each
 * control account will hold once the subledgers are imported, against the
 * trial balance's figure for it. A mismatch is a warning -- import proceeds,
 * reconcile will fail -- because the operator may want to see the result.
 */
static void
plan_predict(Plan *plan)
{
	g_autoptr(GHashTable) expected = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)venture_money_free);
	g_autoptr(GHashTable) controls = NULL;
	g_autoptr(GError) local = NULL;
	GHashTableIter iter;
	gpointer key, value;
	gint64 receivable, payable;
	guint i;
	if (plan->trial_balance->len == 0)
		return;
	controls = control_accounts(plan, &local);
	if (controls == NULL)
		return;
	receivable = control_account(plan, "receivables", "1100", NULL);
	payable = control_account(plan, "payables", "2000", NULL);
#define PREDICT(account_id, money, negate) \
	do { \
		g_autofree gchar *slot = g_strdup_printf("%" G_GINT64_FORMAT "|%s", (gint64)(account_id), venture_money_get_currency(money)); \
		VentureMoney *sum = (negate) ? money_difference(g_hash_table_lookup(expected, slot), (money)) \
			: money_sum(g_hash_table_lookup(expected, slot), (money)); \
		g_hash_table_insert(expected, g_steal_pointer(&slot), sum); \
	} while (0)
	for (i = 0; i < plan->open_ar->len; i++)
	{
		Row *row = g_ptr_array_index(plan->open_ar, i);
		if (row->ok && row->open != NULL && receivable > 0)
			PREDICT(receivable, row->open, FALSE);
	}
	for (i = 0; i < plan->open_ap->len; i++)
	{
		Row *row = g_ptr_array_index(plan->open_ap, i);
		if (row->ok && row->open != NULL && payable > 0)
			PREDICT(payable, row->open, TRUE);
	}
	for (i = 0; i < plan->credits->len; i++)
	{
		Row *row = g_ptr_array_index(plan->credits, i);
		if (!row->ok || row->open == NULL)
			continue;
		if (row->vendor && payable > 0)
			PREDICT(payable, row->open, FALSE);
		else if (!row->vendor && receivable > 0)
			PREDICT(receivable, row->open, TRUE);
	}
	for (i = 0; i < plan->bank->len; i++)
	{
		Row *row = g_ptr_array_index(plan->bank, i);
		g_autoptr(VentureEntity) account = row->ok ? account_with_code(plan->self, plan->org, row->account_code, NULL) : NULL;
		if (account != NULL)
			PREDICT(venture_entity_get_id(account), row->total, FALSE);
	}
	for (i = 0; i < plan->assets->len; i++)
	{
		Row *row = g_ptr_array_index(plan->assets, i);
		g_autoptr(VentureEntity) cost = row->ok ? account_with_code(plan->self, plan->org, row->account_code, NULL) : NULL;
		g_autoptr(VentureEntity) accumulated = row->ok ? account_with_code(plan->self, plan->org, row->accumulated_code, NULL) : NULL;
		if (cost != NULL)
			PREDICT(venture_entity_get_id(cost), row->total, FALSE);
		if (accumulated != NULL)
			PREDICT(venture_entity_get_id(accumulated), row->accumulated, TRUE);
	}
	if (plan->inventory->len > 0)
	{
		gint64 stock = control_account(plan, "inventory", "1200", NULL);
		for (i = 0; stock > 0 && i < plan->inventory->len; i++)
		{
			Row *row = g_ptr_array_index(plan->inventory, i);
			if (row->ok && row->open != NULL)
				PREDICT(stock, row->open, FALSE);
		}
	}
#undef PREDICT
	for (i = 0; i < plan->trial_balance->len; i++)
	{
		Row *row = g_ptr_array_index(plan->trial_balance, i);
		g_autoptr(VentureEntity) account = row->ok ? account_with_code(plan->self, plan->org, row->account_code, NULL) : NULL;
		g_autofree gchar *slot = NULL;
		VentureMoney *predicted;
		gint64 id;
		if (account == NULL)
			continue;
		id = venture_entity_get_id(account);
		if (!g_hash_table_contains(controls, &id))
			continue;
		slot = g_strdup_printf("%" G_GINT64_FORMAT "|%s", id, venture_money_get_currency(row->total));
		predicted = g_hash_table_lookup(expected, slot);
		if (predicted == NULL || !venture_money_equal(predicted, row->total))
		{
			g_autofree gchar *t = venture_money_to_string(row->total);
			g_autofree gchar *p = predicted != NULL ? venture_money_to_string(predicted) : g_strdup("nothing");
			row_warning(plan, row, "control account %s: the trial balance says %s but the imported subledgers will hold %s; reconcile will fail",
				row->account_code, t, p);
		}
		g_hash_table_remove(expected, slot);
	}
	g_hash_table_iter_init(&iter, expected);
	while (g_hash_table_iter_next(&iter, &key, &value))
	{
		g_auto(GStrv) parts = g_strsplit(key, "|", 2);
		g_autoptr(VentureEntity) account = NULL;
		g_autofree gchar *code = NULL;
		g_autofree gchar *text = NULL;
		if (venture_money_is_zero(value))
			continue;
		account = venture_database_get(plan->self->database, VENTURE_TYPE_ACCOUNT, g_ascii_strtoll(parts[0], NULL, 10), NULL);
		if (account != NULL)
			g_object_get(account, "code", &code, NULL);
		text = venture_money_to_string(value);
		plan_note(plan, "trial_balance", -1, NULL, TRUE,
			"control account %s will hold %s from the subledgers but the trial balance has no row for it in %s; reconcile will fail",
			code != NULL ? code : parts[0], text, parts[1]);
	}
}
