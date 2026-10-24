/* SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Rollback of a batch that is not active. Included by
 * venture-books-import-service.c.
 *
 * Nothing is deleted from the ledger. Everything the batch posted is
 * unwound by an ordinary posting dated on the rollback day: the cash on
 * imported documents is refunded, the documents are voided and give up
 * their numbers, the batch's own journals, expenses and capital postings
 * are reversed and the opening cutover is rolled back through its own
 * service. Drafts, which never posted, are removed. Companies and accounts
 * stay, so a corrected batch matches them instead of making them again.
 * The whole walk is one transaction.
 */

typedef struct
{
	VentureBooksImportService *self;
	gint64 org;
	gint64 batch_id;
	GDateTime *date;
	const VentureActor *actor;
} Unwind;

static GPtrArray *
records_where(VentureBooksImportService *self, GType type, const gchar *field, gint64 value, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(type);
	venture_query_set_limit(query, 0);
	if (!venture_query_add_filter_int(query, field, VENTURE_FILTER_OP_EQ, value, error))
		return NULL;
	return venture_database_find(self->database, query, error);
}

/* What is still applied of an allocation after its refunds. */
static gint64
still_applied(Unwind *walk, gboolean vendor, VentureEntity *allocation, GError **error)
{
	g_autoptr(GPtrArray) refunds = records_where(walk->self, vendor ? VENTURE_TYPE_BILL_REFUND : VENTURE_TYPE_REFUND,
		"allocation-id", venture_entity_get_id(allocation), error);
	g_autoptr(VentureMoney) amount = NULL;
	gint64 left;
	guint i;
	if (refunds == NULL)
		return -1;
	g_object_get(allocation, "amount", &amount, NULL);
	left = amount != NULL ? venture_money_get_amount(amount) : 0;
	for (i = 0; i < refunds->len; i++)
	{
		g_autoptr(VentureMoney) refunded = NULL;
		g_object_get(g_ptr_array_index(refunds, i), "amount", &refunded, NULL);
		if (refunded != NULL)
			left -= venture_money_get_amount(refunded);
	}
	return left;
}

static gboolean
save_refund(Unwind *walk, gboolean vendor, const gchar *link, gint64 link_id, gint64 party, gint64 cents,
	const gchar *currency, GError **error)
{
	g_autoptr(VentureEntity) refund = vendor ? VENTURE_ENTITY(venture_bill_refund_new()) : VENTURE_ENTITY(venture_refund_new());
	g_autoptr(VentureMoney) amount = venture_money_new_for_currency(cents, currency);
	g_autofree gchar *reference = g_strdup_printf("Books import %" G_GINT64_FORMAT " rollback", walk->batch_id);
	g_object_set(refund, vendor ? "vendor-id" : "customer-id", party, link, link_id, "date", walk->date,
		"amount", amount, "reference", reference, NULL);
	venture_entity_set_organization_id(refund, walk->org);
	return venture_database_save(walk->self->database, refund, walk->actor, error);
}

/* Every allocation of a receipt or vendor payment, its own and its
 * left-over credit's, refunded; then whatever credit is left. */
static gboolean
unwind_payment(Unwind *walk, gboolean vendor, gint64 payment_id, GError **error)
{
	GType allocation_type = vendor ? VENTURE_TYPE_BILL_PAYMENT_ALLOCATION : VENTURE_TYPE_PAYMENT_ALLOCATION;
	GType credit_type = vendor ? VENTURE_TYPE_VENDOR_CREDIT : VENTURE_TYPE_CUSTOMER_CREDIT;
	g_autoptr(VentureEntity) payment = venture_database_get(walk->self->database,
		vendor ? VENTURE_TYPE_BILL_PAYMENT : VENTURE_TYPE_PAYMENT, payment_id, error);
	g_autoptr(GPtrArray) own = NULL;
	g_autoptr(GPtrArray) credits = NULL;
	g_autoptr(VentureMoney) amount = NULL;
	gint64 party = 0;
	guint i, j;
	if (payment == NULL)
		return FALSE;
	g_object_get(payment, vendor ? "vendor-id" : "customer-id", &party, "amount", &amount, NULL);
	own = records_where(walk->self, allocation_type, "payment-id", payment_id, error);
	credits = records_where(walk->self, credit_type, "payment-id", payment_id, error);
	if (own == NULL || credits == NULL)
		return FALSE;
	for (i = 0; i < own->len; i++)
	{
		gint64 left = still_applied(walk, vendor, g_ptr_array_index(own, i), error);
		if (left < 0 || (left > 0 && !save_refund(walk, vendor, "allocation-id",
			venture_entity_get_id(g_ptr_array_index(own, i)), party, left, venture_money_get_currency(amount), error)))
			return FALSE;
	}
	for (j = 0; j < credits->len; j++)
	{
		VentureEntity *credit = g_ptr_array_index(credits, j);
		g_autoptr(GPtrArray) applied = records_where(walk->self, allocation_type, "credit-id", venture_entity_get_id(credit), error);
		g_autoptr(VentureEntity) fresh = NULL;
		g_autoptr(VentureMoney) remaining = NULL;
		gboolean ok;
		if (applied == NULL)
			return FALSE;
		for (i = 0; i < applied->len; i++)
		{
			gint64 left = still_applied(walk, vendor, g_ptr_array_index(applied, i), error);
			if (left < 0 || (left > 0 && !save_refund(walk, vendor, "allocation-id",
				venture_entity_get_id(g_ptr_array_index(applied, i)), party, left, venture_money_get_currency(amount), error)))
				return FALSE;
		}
		ok = vendor
			? venture_payables_service_refresh_credit(venture_payables_service_get(walk->self->database),
				venture_entity_get_id(credit), walk->actor, error)
			: venture_settlement_service_refresh_credit(venture_settlement_service_get(walk->self->database),
				venture_entity_get_id(credit), walk->actor, error);
		if (!ok)
			return FALSE;
		fresh = venture_database_get(walk->self->database, credit_type, venture_entity_get_id(credit), error);
		if (fresh == NULL)
			return FALSE;
		g_object_get(fresh, "remaining", &remaining, NULL);
		if (remaining != NULL && venture_money_get_amount(remaining) > 0 &&
			!save_refund(walk, vendor, "credit-id", venture_entity_get_id(credit), party,
				venture_money_get_amount(remaining), venture_money_get_currency(remaining), error))
			return FALSE;
	}
	return TRUE;
}

static gboolean
reverse_source(Unwind *walk, const gchar *source_type, gint64 source_id, GError **error)
{
	VenturePostingService *posting = venture_database_get_posting_service(walk->self->database);
	g_autoptr(GPtrArray) journals = venture_posting_service_find_source(posting, source_type, source_id, walk->org, error);
	g_autofree gchar *memo = g_strdup_printf("Books import %" G_GINT64_FORMAT " rollback", walk->batch_id);
	g_autoptr(GHashTable) reversed = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	guint i;
	if (journals == NULL)
		return FALSE;
	/* A journal already carrying a reversal is left alone, so a repeated
	 * walk never reverses a reversal. */
	for (i = 0; i < journals->len; i++)
	{
		VentureEntity *journal = g_ptr_array_index(journals, i);
		gint64 original = 0;
		if (g_object_class_find_property(G_OBJECT_GET_CLASS(journal), "reverses-id") != NULL)
			g_object_get(journal, "reverses-id", &original, NULL);
		if (original > 0)
		{
			gint64 *key = g_new(gint64, 1);
			*key = original;
			g_hash_table_add(reversed, key);
		}
	}
	for (i = 0; i < journals->len; i++)
	{
		VentureEntity *journal = g_ptr_array_index(journals, i);
		gint64 id = venture_entity_get_id(journal), original = 0;
		g_autoptr(VentureJournal) reversal = NULL;
		if (g_object_class_find_property(G_OBJECT_GET_CLASS(journal), "reverses-id") != NULL)
			g_object_get(journal, "reverses-id", &original, NULL);
		if (original > 0 || g_hash_table_contains(reversed, &id))
			continue;
		reversal = venture_posting_service_reverse(posting, id, walk->date, memo, walk->actor, error);
		if (reversal == NULL)
			return FALSE;
	}
	return TRUE;
}

static gboolean
unwind_document(Unwind *walk, gboolean vendor, gint64 id, GError **error)
{
	VentureDatabase *db = walk->self->database;
	g_autoptr(VentureEntity) record = venture_database_get(db, vendor ? VENTURE_TYPE_VENDOR_BILL : VENTURE_TYPE_INVOICE, id, error);
	g_autofree gchar *suffix = g_strdup_printf("-rb%" G_GINT64_FORMAT, walk->batch_id);
	gboolean draft, voided;
	if (record == NULL)
		return FALSE;
	if (vendor)
	{
		g_autofree gchar *status = NULL;
		g_object_get(record, "status", &status, NULL);
		draft = g_strcmp0(status, "draft") == 0;
		voided = g_strcmp0(status, "void") == 0;
	}
	else
	{
		gint status = 0;
		g_object_get(record, "status", &status, NULL);
		draft = status == VENTURE_INVOICE_STATUS_DRAFT;
		voided = status == VENTURE_INVOICE_STATUS_VOID;
	}
	/* A draft never posted: it goes, after giving up its number, which a
	 * removed record still holds. */
	if (draft)
	{
		g_autofree gchar *number = NULL;
		g_autofree gchar *renamed = NULL;
		g_object_get(record, "number", &number, NULL);
		renamed = g_strconcat(number != NULL ? number : "", suffix, NULL);
		g_object_set(record, "number", renamed, NULL);
		return venture_database_save(db, record, walk->actor, error) &&
			venture_database_delete(db, record, walk->actor, error);
	}
	if (!voided)
	{
		gboolean ok = vendor
			? venture_payables_service_transition(venture_payables_service_get(db), VENTURE_VENDOR_BILL(record), "void",
				walk->date, walk->actor, error)
			: venture_settlement_service_transition(venture_settlement_service_get(db), VENTURE_INVOICE(record), "void",
				walk->date, walk->actor, error);
		if (!ok)
			return FALSE;
	}
	return vendor
		? venture_payables_service_free_number(venture_payables_service_get(db), VENTURE_VENDOR_BILL(record), suffix, walk->actor, error)
		: venture_settlement_service_free_number(venture_settlement_service_get(db), VENTURE_INVOICE(record), suffix, walk->actor, error);
}

static gboolean
unwind_expense(Unwind *walk, gint64 id, GError **error)
{
	g_autoptr(VentureEntity) expense = venture_database_get(walk->self->database, VENTURE_TYPE_EXPENSE, id, error);
	if (expense == NULL)
		return FALSE;
	/* The ledger keeps the expense's journal and its reversal; the record
	 * goes, so operational reports stop counting it. */
	return reverse_source(walk, "expense", id, error) &&
		venture_database_delete(walk->self->database, expense, walk->actor, error);
}

GPtrArray *
venture_books_import_service_rollback_preflight(VentureBooksImportService *self, VentureBooksImport *batch, GError **error)
{
	g_autoptr(GPtrArray) blockers = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GPtrArray) rows = NULL;
	g_autofree gchar *state = state_of(VENTURE_ENTITY(batch));
	guint i;
	if (g_strcmp0(state, "active") == 0)
		g_ptr_array_add(blockers, g_strdup("the batch is active: the imported books are the organization's own"));
	if (g_strcmp0(state, "rolled_back") == 0)
		return g_steal_pointer(&blockers);
	rows = batch_rows(self, venture_entity_get_id(VENTURE_ENTITY(batch)), error);
	if (rows == NULL)
		return NULL;
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(rows, i);
		g_autofree gchar *object = row_string(row, "source-object");
		g_autofree gchar *source = row_string(row, "source-id");
		if (g_strcmp0(object, "credit_application") == 0 || g_strcmp0(object, "refund") == 0 ||
			g_strcmp0(object, "vendor_refund") == 0)
			g_ptr_array_add(blockers, g_strdup_printf("%s %s: an applied or refunded credit cannot be unapplied; "
				"undo it by hand first", object, source));
	}
	return g_steal_pointer(&blockers);
}

gboolean
venture_books_import_service_rollback(VentureBooksImportService *self, VentureBooksImport *batch,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) stored = NULL;
	g_autoptr(GPtrArray) blockers = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GString) block = g_string_new(NULL);
	g_autofree gchar *state = NULL;
	g_autofree gchar *stamp = stamp_now();
	Unwind walk;
	gint64 cutover_id = 0;
	guint i, pass;
	g_return_val_if_fail(VENTURE_IS_BOOKS_IMPORT_SERVICE(self), FALSE);
	stored = venture_database_get(self->database, VENTURE_TYPE_BOOKS_IMPORT, venture_entity_get_id(VENTURE_ENTITY(batch)), error);
	if (stored == NULL)
		return FALSE;
	state = state_of(stored);
	if (g_strcmp0(state, "rolled_back") == 0)
		return TRUE;
	blockers = venture_books_import_service_rollback_preflight(self, VENTURE_BOOKS_IMPORT(stored), error);
	if (blockers == NULL)
		return FALSE;
	if (blockers->len > 0)
	{
		g_autoptr(GString) text = g_string_new("VentureBooksImportService: rollback refused: ");
		for (i = 0; i < blockers->len; i++)
			g_string_append_printf(text, "%s%s", i > 0 ? "; " : "", (const gchar *)g_ptr_array_index(blockers, i));
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, text->str);
		return FALSE;
	}
	rows = batch_rows(self, venture_entity_get_id(stored), error);
	if (rows == NULL)
		return FALSE;
	walk.self = self;
	walk.org = venture_entity_get_organization_id(stored);
	walk.batch_id = venture_entity_get_id(stored);
	walk.date = venture_settlement_service_today(venture_settlement_service_get(self->database));
	walk.actor = actor;
	if (!venture_database_begin(self->database, error))
	{
		g_date_time_unref(walk.date);
		return FALSE;
	}
	/* Cash first, so every document is back at its full balance; then the
	 * documents; then everything the batch posted itself. */
	for (pass = 0; pass < 3; pass++)
		for (i = 0; i < rows->len; i++)
		{
			VentureEntity *row = g_ptr_array_index(rows, i);
			g_autofree gchar *type = row_string(row, "record-type");
			g_autofree gchar *object = row_string(row, "source-object");
			gint64 id = row_int(row, "record-id");
			gboolean ok = TRUE;
			if (id <= 0)
				continue;
			if (pass == 0 && g_strcmp0(type, "payment") == 0)
				ok = unwind_payment(&walk, FALSE, id, error);
			else if (pass == 0 && g_strcmp0(type, "bill_payment") == 0)
				ok = unwind_payment(&walk, TRUE, id, error);
			else if (pass == 1 && g_strcmp0(type, "invoice") == 0)
				ok = unwind_document(&walk, FALSE, id, error);
			else if (pass == 1 && g_strcmp0(type, "vendor_bill") == 0)
				ok = unwind_document(&walk, TRUE, id, error);
			else if (pass == 2 && g_strcmp0(type, "expense") == 0)
				ok = unwind_expense(&walk, id, error);
			else if (pass == 2 && (g_strcmp0(type, "customer_credit") == 0 || g_strcmp0(type, "vendor_credit") == 0))
				ok = reverse_source(&walk, type, id, error) &&
					(g_strcmp0(type, "customer_credit") == 0
						? venture_settlement_service_refresh_credit(venture_settlement_service_get(self->database), id, actor, error)
						: venture_payables_service_refresh_credit(venture_payables_service_get(self->database), id, actor, error));
			else if (pass == 2 && g_strcmp0(type, "equity_transaction") == 0)
				ok = reverse_source(&walk, "equity_transaction", id, error);
			else if (pass == 2 && g_strcmp0(type, "contact") == 0 && row_bool(row, "created"))
			{
				g_autoptr(VentureEntity) contact = venture_database_get(self->database, VENTURE_TYPE_CONTACT, id, error);
				ok = contact != NULL && venture_database_delete(self->database, contact, actor, error);
			}
			(void)object;
			if (!ok)
				goto fail;
		}
	if (!reverse_source(&walk, "books_import", walk.batch_id, error))
		goto fail;
	g_object_get(stored, "cutover-id", &cutover_id, NULL);
	if (cutover_id > 0)
	{
		g_autoptr(VentureEntity) opening = venture_database_get(self->database, VENTURE_TYPE_ACCOUNTING_CUTOVER, cutover_id, error);
		if (opening == NULL ||
			!venture_cutover_service_rollback(venture_cutover_service_get(self->database), VENTURE_ACCOUNTING_CUTOVER(opening),
				actor, error))
			goto fail;
	}
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(rows, i);
		g_object_set(row, "status", "rolled_back", NULL);
		if (!save_owned(self, row, actor, error))
			goto fail;
	}
	g_string_append_printf(block, "\n== Rollback %s ==\nEvery imported document is void or removed and every posting reversed; "
		"companies and accounts are kept for a corrected batch.\n", stamp);
	g_object_set(stored, "rolled-back-at", walk.date, NULL);
	if (!append_report(self, stored, "rolled_back", block, actor, error) || !venture_database_commit(self->database, error))
		goto fail;
	g_date_time_unref(walk.date);
	venture_entity_copy_properties_from(VENTURE_ENTITY(batch), stored, FALSE);
	return TRUE;
fail:
	venture_database_rollback(self->database);
	g_date_time_unref(walk.date);
	return FALSE;
}
