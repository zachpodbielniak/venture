/* SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Rolling back an imported batch. Included by venture-cutover-service.c.
 *
 * One walk serves the preflight and the rollback: with @dry set it only
 * collects blockers, without it performs each step through the owning
 * service. Two walks would be two opinions about what can be undone.
 *
 * Nothing is deleted. Cash applied to an imported document is refunded at the
 * document's latest event date -- allocations are immutable and a refund
 * cannot precede a later event on the same document. A real receipt or
 * payment that settled an imported document is then recorded again as an
 * unapplied deposit on the same date, because that cash really moved and
 * the bank statement will still show it. Credits are reversed where they were
 * used, documents are voided with their numbers suffixed so a corrected batch
 * can reuse them, opening journals are reversed at the cutoff, and
 * depreciation already run after the cutoff is reversed at its own date.
 *
 * What cannot be unwound is refused before anything is written: a credit note
 * from outside the batch applied to an imported document (an allocation of a
 * credit note cannot be unapplied), an imported credit or prepayment used
 * outside the batch in a way that moved cash, a disposed asset, and any step
 * whose date falls in a closed or locked period.
 */

typedef struct
{
	VentureCutoverService *self;
	VentureEntity *cutover;
	gint64 cutover_id;
	gint64 org;
	GDateTime *cutoff;
	gboolean dry;
	gchar *suffix;
	GHashTable *owned;
	GHashTable *checked_dates;
	GPtrArray *blockers;
	const VentureActor *actor;
} Rollback;

static void
rollback_clear(Rollback *walk)
{
	g_clear_pointer(&walk->suffix, g_free);
	g_clear_pointer(&walk->owned, g_hash_table_unref);
	g_clear_pointer(&walk->checked_dates, g_hash_table_unref);
	g_clear_pointer(&walk->cutoff, g_date_time_unref);
}

static void blocker(Rollback *walk, const gchar *format, ...) G_GNUC_PRINTF(2, 3);

static void
blocker(Rollback *walk, const gchar *format, ...)
{
	va_list args;
	va_start(args, format);
	g_ptr_array_add(walk->blockers, g_strdup_vprintf(format, args));
	va_end(args);
}

static gboolean
owned(Rollback *walk, const gchar *type, gint64 id)
{
	g_autofree gchar *key = g_strdup_printf("%s|%" G_GINT64_FORMAT, type, id);
	return g_hash_table_contains(walk->owned, key);
}

/* Every unwinding date is checked once against the fiscal calendar. */
static void
check_postable(Rollback *walk, GDateTime *date, const gchar *what)
{
	g_autofree gchar *text = g_date_time_format_iso8601(date);
	g_autoptr(GError) local = NULL;
	if (g_hash_table_contains(walk->checked_dates, text))
		return;
	g_hash_table_add(walk->checked_dates, g_strdup(text));
	if (!venture_period_guard_is_postable(VENTURE_PERIOD_GUARD(venture_database_get_period_guard(walk->self->database)),
		walk->self->database, walk->org, date, &local))
		blocker(walk, "%s must be unwound on %s, which cannot be posted: %s", what, text,
			local != NULL ? local->message : "the period is not open");
}

static GDateTime *
later(GDateTime *current, GDateTime *candidate)
{
	if (candidate != NULL && (current == NULL || g_date_time_compare(candidate, current) > 0))
	{
		if (current != NULL)
			g_date_time_unref(current);
		return g_date_time_ref(candidate);
	}
	return current;
}

static GDateTime *
latest_of(GPtrArray *records, GDateTime *start)
{
	GDateTime *latest = start != NULL ? g_date_time_ref(start) : NULL;
	guint i;
	for (i = 0; records != NULL && i < records->len; i++)
	{
		g_autoptr(GDateTime) date = NULL;
		g_object_get(g_ptr_array_index(records, i), "date", &date, NULL);
		latest = later(latest, date);
	}
	return latest;
}

/* The latest event on a document, and never before the cutoff: every
 * unwinding step for it is dated here so none precedes a later event. */
static GDateTime *
document_latest(Rollback *walk, gboolean vendor, gint64 id, GError **error)
{
	g_autoptr(GPtrArray) events = records_for(walk->self, vendor ? VENTURE_TYPE_VENDOR_BILL_EVENT : VENTURE_TYPE_INVOICE_EVENT,
		vendor ? "bill-id" : "invoice-id", id, error);
	g_autoptr(GPtrArray) allocations = NULL;
	GDateTime *latest;
	guint i;
	if (events == NULL)
		return NULL;
	allocations = records_for(walk->self, vendor ? VENTURE_TYPE_BILL_PAYMENT_ALLOCATION : VENTURE_TYPE_PAYMENT_ALLOCATION,
		vendor ? "bill-id" : "invoice-id", id, error);
	if (allocations == NULL)
		return NULL;
	latest = latest_of(events, walk->cutoff);
	{
		GDateTime *next = latest_of(allocations, latest);
		g_date_time_unref(latest);
		latest = next;
	}
	for (i = 0; i < allocations->len; i++)
	{
		g_autoptr(GPtrArray) refunds = records_for(walk->self, vendor ? VENTURE_TYPE_BILL_REFUND : VENTURE_TYPE_REFUND,
			"allocation-id", venture_entity_get_id(g_ptr_array_index(allocations, i)), error);
		GDateTime *next;
		if (refunds == NULL)
		{
			g_date_time_unref(latest);
			return NULL;
		}
		next = latest_of(refunds, latest);
		g_date_time_unref(latest);
		latest = next;
	}
	return latest;
}

static VentureMoney *
allocation_available(Rollback *walk, gboolean vendor, VentureEntity *allocation, GError **error)
{
	g_autoptr(GPtrArray) refunds = records_for(walk->self, vendor ? VENTURE_TYPE_BILL_REFUND : VENTURE_TYPE_REFUND,
		"allocation-id", venture_entity_get_id(allocation), error);
	g_autoptr(VentureMoney) available = NULL;
	guint i;
	if (refunds == NULL)
		return NULL;
	g_object_get(allocation, "amount", &available, NULL);
	for (i = 0; i < refunds->len; i++)
	{
		g_autoptr(VentureMoney) amount = NULL;
		VentureMoney *next;
		g_object_get(g_ptr_array_index(refunds, i), "amount", &amount, NULL);
		next = money_difference(available, amount);
		g_clear_pointer(&available, venture_money_free);
		available = next;
	}
	return g_steal_pointer(&available);
}

static gboolean
refund_allocation(Rollback *walk, gboolean vendor, VentureEntity *allocation, gint64 party,
	const VentureMoney *amount, GDateTime *date, GError **error)
{
	VentureEntity *refund = vendor ? VENTURE_ENTITY(venture_bill_refund_new()) : VENTURE_ENTITY(venture_refund_new());
	gboolean ok;
	g_object_set(refund, vendor ? "vendor-id" : "customer-id", party, "allocation-id", venture_entity_get_id(allocation),
		"date", date, "amount", amount, "reference", "Cutover rollback", NULL);
	venture_entity_set_organization_id(refund, walk->org);
	ok = venture_database_save(walk->self->database, refund, walk->actor, error);
	g_object_unref(refund);
	return ok;
}

static gboolean
refund_credit(Rollback *walk, gboolean vendor, VentureEntity *credit, gint64 party,
	const VentureMoney *amount, GDateTime *date, GError **error)
{
	VentureEntity *refund = vendor ? VENTURE_ENTITY(venture_bill_refund_new()) : VENTURE_ENTITY(venture_refund_new());
	gboolean ok;
	g_object_set(refund, vendor ? "vendor-id" : "customer-id", party, "credit-id", venture_entity_get_id(credit),
		"date", date, "amount", amount, "reference", "Cutover rollback", NULL);
	venture_entity_set_organization_id(refund, walk->org);
	ok = venture_database_save(walk->self->database, refund, walk->actor, error);
	g_object_unref(refund);
	return ok;
}

/* The same cash, recorded again as an unapplied deposit on the same day. */
static gboolean
redeposit(Rollback *walk, gboolean vendor, VentureEntity *payment, gint64 party, const VentureMoney *amount,
	GDateTime *date, const gchar *number, GError **error)
{
	g_autofree gchar *method = NULL;
	g_autofree gchar *reference = g_strdup_printf("Cutover rollback: unapplied from %s", number != NULL ? number : "an imported document");
	VentureEntity *copy = vendor ? VENTURE_ENTITY(venture_bill_payment_new()) : VENTURE_ENTITY(venture_payment_new());
	gboolean ok;
	g_object_get(payment, "method", &method, NULL);
	g_object_set(copy, vendor ? "vendor-id" : "customer-id", party, "amount", amount, "date", date,
		"method", method, "reference", reference, NULL);
	venture_entity_set_organization_id(copy, walk->org);
	ok = vendor
		? venture_payables_service_apply_payment(venture_payables_service_get(walk->self->database),
			VENTURE_BILL_PAYMENT(copy), NULL, walk->actor, error)
		: venture_settlement_service_apply_payment(venture_settlement_service_get(walk->self->database),
			VENTURE_PAYMENT(copy), NULL, walk->actor, error);
	g_object_unref(copy);
	return ok;
}

/* The payment record behind a cash allocation, whether it names the
 * payment or the payment's unused-cash credit. */
static VentureEntity *
allocation_payment(Rollback *walk, gboolean vendor, VentureEntity *allocation, VentureEntity **credit_out, GError **error)
{
	gint64 payment_id = 0, credit_id = 0;
	*credit_out = NULL;
	g_object_get(allocation, "payment-id", &payment_id, "credit-id", &credit_id, NULL);
	if (payment_id == 0 && credit_id != 0)
	{
		g_autoptr(VentureEntity) credit = venture_database_get(walk->self->database,
			vendor ? VENTURE_TYPE_VENDOR_CREDIT : VENTURE_TYPE_CUSTOMER_CREDIT, credit_id, error);
		if (credit == NULL)
			return NULL;
		g_object_get(credit, "payment-id", &payment_id, NULL);
		*credit_out = g_steal_pointer(&credit);
		if (payment_id == 0)
			return NULL;
	}
	return venture_database_get(walk->self->database, vendor ? VENTURE_TYPE_BILL_PAYMENT : VENTURE_TYPE_PAYMENT,
		payment_id, error);
}

/* Phase one: refund cash applied to imported documents. */
static gboolean
unwind_document_cash(Rollback *walk, gboolean vendor, VentureEntity *document, GError **error)
{
	g_autoptr(GPtrArray) allocations = records_for(walk->self, vendor ? VENTURE_TYPE_BILL_PAYMENT_ALLOCATION : VENTURE_TYPE_PAYMENT_ALLOCATION,
		vendor ? "bill-id" : "invoice-id", venture_entity_get_id(document), error);
	g_autoptr(GDateTime) latest = NULL;
	g_autofree gchar *number = NULL;
	gint64 party = 0;
	guint i;
	if (allocations == NULL)
		return FALSE;
	latest = document_latest(walk, vendor, venture_entity_get_id(document), error);
	if (latest == NULL)
		return FALSE;
	g_object_get(document, "number", &number, "company-id", &party, NULL);
	for (i = 0; i < allocations->len; i++)
	{
		VentureEntity *allocation = g_ptr_array_index(allocations, i);
		g_autoptr(VentureEntity) credit = NULL;
		g_autoptr(VentureEntity) payment = NULL;
		g_autoptr(VentureMoney) available = NULL;
		g_autoptr(GDateTime) date = NULL;
		g_autofree gchar *when = NULL;
		gint64 credit_id = 0;
		g_object_get(allocation, "date", &date, "credit-id", &credit_id, NULL);
		when = g_date_time_format(date, "%Y-%m-%d");
		payment = allocation_payment(walk, vendor, allocation, &credit, error);
		if (payment == NULL && error != NULL && *error != NULL)
			return FALSE;
		if (payment == NULL)
		{
			/* A credit note, or a write-off, applied to the document. */
			if (credit != NULL && owned(walk, venture_entity_get_entity_name(credit), venture_entity_get_id(credit)))
				continue;
			blocker(walk, "%s %s has %s %" G_GINT64_FORMAT " from outside this batch applied on %s; an applied credit cannot be unapplied",
				vendor ? "bill" : "invoice", number, vendor ? "vendor_credit" : "customer_credit", credit_id, when);
			continue;
		}
		available = allocation_available(walk, vendor, allocation, error);
		if (available == NULL)
			return FALSE;
		if (venture_money_get_amount(available) <= 0)
			continue;
		check_postable(walk, latest, number);
		if (walk->dry)
			continue;
		if (!refund_allocation(walk, vendor, allocation, party, available, latest, error))
			return FALSE;
		if (!owned(walk, venture_entity_get_entity_name(payment), venture_entity_get_id(payment)) &&
			!redeposit(walk, vendor, payment, party, available, latest, number, error))
			return FALSE;
	}
	return TRUE;
}

/* Phase two: an imported prepayment or opening payment applied outside the
 * batch is refunded where it was applied; what is still unused is refunded
 * too. Cash it already returned is real and blocks. */
static gboolean
unwind_payment(Rollback *walk, gboolean vendor, VentureEntity *payment, GError **error)
{
	g_autoptr(GPtrArray) credits = records_for(walk->self, vendor ? VENTURE_TYPE_VENDOR_CREDIT : VENTURE_TYPE_CUSTOMER_CREDIT,
		"payment-id", venture_entity_get_id(payment), error);
	g_autoptr(GPtrArray) refunds = NULL;
	g_autoptr(VentureMoney) remaining = NULL;
	g_autoptr(GDateTime) latest = g_date_time_ref(walk->cutoff);
	VentureEntity *credit;
	gint64 party = 0;
	guint pass, i;
	if (credits == NULL)
		return FALSE;
	if (credits->len != 1)
		return refuse(error, "an imported payment has no unique unused-cash credit");
	credit = g_ptr_array_index(credits, 0);
	g_object_get(payment, vendor ? "vendor-id" : "customer-id", &party, NULL);
	refunds = records_for(walk->self, vendor ? VENTURE_TYPE_BILL_REFUND : VENTURE_TYPE_REFUND, "credit-id",
		venture_entity_get_id(credit), error);
	if (refunds == NULL)
		return FALSE;
	if (refunds->len > 0)
	{
		g_autofree gchar *reference = NULL;
		g_object_get(payment, "reference", &reference, NULL);
		blocker(walk, "imported %s %" G_GINT64_FORMAT " (%s) was refunded after the cutoff; that cash left the bank and cannot be unwound",
			venture_entity_get_entity_name(payment), venture_entity_get_id(payment), reference != NULL ? reference : "");
		return TRUE;
	}
	for (pass = 0; pass < 2; pass++)
	{
		g_autoptr(GPtrArray) allocations = records_for(walk->self,
			vendor ? VENTURE_TYPE_BILL_PAYMENT_ALLOCATION : VENTURE_TYPE_PAYMENT_ALLOCATION,
			pass == 0 ? "payment-id" : "credit-id",
			pass == 0 ? venture_entity_get_id(payment) : venture_entity_get_id(credit), error);
		if (allocations == NULL)
			return FALSE;
		for (i = 0; i < allocations->len; i++)
		{
			VentureEntity *allocation = g_ptr_array_index(allocations, i);
			g_autoptr(VentureEntity) document = NULL;
			g_autoptr(VentureMoney) available = NULL;
			g_autoptr(GDateTime) date = NULL;
			g_autofree gchar *number = NULL;
			gint64 document_id = 0;
			g_object_get(allocation, vendor ? "bill-id" : "invoice-id", &document_id, "date", &date, NULL);
			latest = later(latest, date);
			if (owned(walk, vendor ? "vendor_bill" : "invoice", document_id))
				continue;
			document = venture_database_get(walk->self->database, vendor ? VENTURE_TYPE_VENDOR_BILL : VENTURE_TYPE_INVOICE,
				document_id, error);
			if (document == NULL)
				return FALSE;
			g_object_get(document, "number", &number, NULL);
			available = allocation_available(walk, vendor, allocation, error);
			if (available == NULL)
				return FALSE;
			if (venture_money_get_amount(available) <= 0)
				continue;
			{
				g_autoptr(GDateTime) when = document_latest(walk, vendor, document_id, error);
				if (when == NULL)
					return FALSE;
				check_postable(walk, when, number);
				latest = later(latest, when);
				if (!walk->dry && !refund_allocation(walk, vendor, allocation, party, available, when, error))
					return FALSE;
			}
		}
	}
	g_object_get(credit, "remaining", &remaining, NULL);
	if (remaining != NULL && venture_money_get_amount(remaining) > 0)
	{
		check_postable(walk, latest, "an imported prepayment");
		if (!walk->dry && !refund_credit(walk, vendor, credit, party, remaining, latest, error))
			return FALSE;
	}
	return TRUE;
}

/* Phase three: an imported credit note reverses its opening posting where it
 * was last used. Used outside the batch, or refunded, it blocks. */
static gboolean
unwind_credit(Rollback *walk, gboolean vendor, VentureEntity *credit, GError **error)
{
	g_autoptr(GPtrArray) allocations = records_for(walk->self, vendor ? VENTURE_TYPE_BILL_PAYMENT_ALLOCATION : VENTURE_TYPE_PAYMENT_ALLOCATION,
		"credit-id", venture_entity_get_id(credit), error);
	g_autoptr(GPtrArray) refunds = NULL;
	g_autoptr(GDateTime) latest = NULL;
	g_autofree gchar *reference = NULL;
	g_autoptr(GPtrArray) journals = NULL;
	guint i;
	if (allocations == NULL)
		return FALSE;
	g_object_get(credit, "reference", &reference, NULL);
	refunds = records_for(walk->self, vendor ? VENTURE_TYPE_BILL_REFUND : VENTURE_TYPE_REFUND, "credit-id",
		venture_entity_get_id(credit), error);
	if (refunds == NULL)
		return FALSE;
	if (refunds->len > 0)
		blocker(walk, "imported credit %s was refunded as cash after the cutoff; that cannot be unwound", reference);
	latest = latest_of(allocations, walk->cutoff);
	for (i = 0; i < allocations->len; i++)
	{
		gint64 document_id = 0;
		g_object_get(g_ptr_array_index(allocations, i), vendor ? "bill-id" : "invoice-id", &document_id, NULL);
		if (!owned(walk, vendor ? "vendor_bill" : "invoice", document_id))
			blocker(walk, "imported credit %s is applied to %s %" G_GINT64_FORMAT ", which is not part of this batch; an applied credit cannot be unapplied",
				reference, vendor ? "vendor_bill" : "invoice", document_id);
	}
	check_postable(walk, latest, reference);
	if (walk->dry)
		return TRUE;
	journals = venture_posting_service_find_source(venture_database_get_posting_service(walk->self->database),
		venture_entity_get_entity_name(credit), venture_entity_get_id(credit), walk->org, error);
	if (journals == NULL)
		return FALSE;
	for (i = 0; i < journals->len; i++)
	{
		g_autoptr(VentureJournal) reversed = NULL;
		VentureJournalState state;
		gint64 reverses = 0;
		g_object_get(g_ptr_array_index(journals, i), "state", &state, "reverses-id", &reverses, NULL);
		if (state != VENTURE_JOURNAL_POSTED || reverses > 0)
			continue;
		reversed = venture_posting_service_reverse(venture_database_get_posting_service(walk->self->database),
			venture_entity_get_id(g_ptr_array_index(journals, i)), latest, "Cutover rollback", walk->actor, error);
		if (reversed == NULL)
			return FALSE;
	}
	return vendor
		? venture_payables_service_refresh_credit(venture_payables_service_get(walk->self->database),
			venture_entity_get_id(credit), walk->actor, error)
		: venture_settlement_service_refresh_credit(venture_settlement_service_get(walk->self->database),
			venture_entity_get_id(credit), walk->actor, error);
}

/* Phase four: void the imported documents, freeing their numbers. */
static gboolean
unwind_document_void(Rollback *walk, gboolean vendor, VentureEntity *document, GError **error)
{
	g_autoptr(GDateTime) latest = document_latest(walk, vendor, venture_entity_get_id(document), error);
	g_autofree gchar *number = NULL;
	if (latest == NULL)
		return FALSE;
	g_object_get(document, "number", &number, NULL);
	check_postable(walk, latest, number);
	if (walk->dry)
		return TRUE;
	/* Reread: the refunds above re-derived its status and version. */
	{
		g_autoptr(VentureEntity) current = venture_database_get(walk->self->database,
			vendor ? VENTURE_TYPE_VENDOR_BILL : VENTURE_TYPE_INVOICE, venture_entity_get_id(document), error);
		if (current == NULL)
			return FALSE;
		return vendor
			? venture_payables_service_void_opening(venture_payables_service_get(walk->self->database),
				VENTURE_VENDOR_BILL(current), latest, walk->suffix, walk->actor, error)
			: venture_settlement_service_void_opening(venture_settlement_service_get(walk->self->database),
				VENTURE_INVOICE(current), latest, walk->suffix, walk->actor, error);
	}
}

/* Phase six: depreciation run after the cutoff is reversed at its own date,
 * then the asset is retired and its tag freed. */
static gboolean
unwind_asset(Rollback *walk, VentureEntity *asset, GError **error)
{
	g_autoptr(GDateTime) latest = g_date_time_ref(walk->cutoff);
	g_autofree gchar *tag = NULL;
	gint status;
	guint kind, i;
	g_object_get(asset, "status", &status, "tag", &tag, NULL);
	if (status != VENTURE_ASSET_STATUS_IN_SERVICE)
	{
		blocker(walk, "asset %s is no longer in service (disposed or written off after the cutoff); reverse that first", tag);
		return TRUE;
	}
	for (kind = 0; kind < 2; kind++)
	{
		g_autoptr(GPtrArray) entries = records_for(walk->self,
			kind == 0 ? VENTURE_TYPE_DEPRECIATION_ENTRY : VENTURE_TYPE_TAX_DEPRECIATION_ENTRY, "asset-id",
			venture_entity_get_id(asset), error);
		if (entries == NULL)
			return FALSE;
		for (i = 0; i < entries->len; i++)
		{
			g_autoptr(VentureEntity) journal = NULL;
			g_autoptr(GDateTime) date = NULL;
			g_autofree gchar *source = NULL;
			g_autoptr(VentureJournal) reversed = NULL;
			VentureJournalState state;
			gint64 journal_id = 0;
			gint entry_state;
			g_object_get(g_ptr_array_index(entries, i), "journal-id", &journal_id, "state", &entry_state, NULL);
			if (entry_state != VENTURE_SCHEDULE_STATE_POSTED || journal_id == 0)
				continue;
			journal = venture_database_get(walk->self->database, VENTURE_TYPE_JOURNAL, journal_id, error);
			if (journal == NULL)
				return FALSE;
			g_object_get(journal, "state", &state, "occurred-at", &date, "source-type", &source, NULL);
			/* The opening entry shares the batch's opening journal. */
			if (state != VENTURE_JOURNAL_POSTED || g_strcmp0(source, "accounting_cutover") == 0)
				continue;
			latest = later(latest, date);
			check_postable(walk, date, tag);
			if (walk->dry)
				continue;
			reversed = venture_posting_service_reverse(venture_database_get_posting_service(walk->self->database),
				journal_id, date, "Cutover rollback", walk->actor, error);
			if (reversed == NULL)
				return FALSE;
		}
	}
	if (walk->dry)
		return TRUE;
	return venture_asset_service_rollback_opening(venture_asset_service_get(walk->self->database),
		venture_entity_get_id(asset), latest, walk->suffix, walk->actor, error);
}

/* Phase six, for stock: the received quantity goes back out through the
 * inventory service's return, dated at the item's latest movement. Stock
 * already sold or moved on cannot be returned. */
static gboolean
unwind_inventory(Rollback *walk, VentureEntity *txn, GError **error)
{
	VentureInventoryService *inventory = venture_inventory_service_get(walk->self->database);
	g_autoptr(VentureMoney) unit = NULL;
	g_autoptr(GPtrArray) movements = NULL;
	g_autoptr(GDateTime) latest = g_date_time_ref(walk->cutoff);
	g_autoptr(VentureEntity) item = NULL;
	g_autofree gchar *sku = NULL;
	gint64 item_id = 0, quantity = 0, on_hand;
	guint i;
	g_object_get(txn, "inventory-item-id", &item_id, "quantity", &quantity, "unit-cost", &unit, NULL);
	item = venture_database_get(walk->self->database, VENTURE_TYPE_INVENTORY_ITEM, item_id, error);
	if (item == NULL)
		return FALSE;
	g_object_get(item, "sku", &sku, NULL);
	on_hand = venture_inventory_service_on_hand(inventory, item_id, NULL, error);
	if (error != NULL && *error != NULL)
		return FALSE;
	if (on_hand < quantity)
	{
		blocker(walk, "inventory %s: %" G_GINT64_FORMAT " of the %" G_GINT64_FORMAT " units imported are still on hand; stock sold or moved after the cutoff cannot be returned",
			sku, on_hand, quantity);
		return TRUE;
	}
	movements = records_for(walk->self, VENTURE_TYPE_INVENTORY_TXN, "inventory-item-id", item_id, error);
	if (movements == NULL)
		return FALSE;
	for (i = 0; i < movements->len; i++)
	{
		g_autoptr(GDateTime) when = NULL;
		g_object_get(g_ptr_array_index(movements, i), "occurred-at", &when, NULL);
		latest = later(latest, when);
	}
	check_postable(walk, latest, sku);
	if (walk->dry)
		return TRUE;
	return venture_inventory_service_restore(inventory, item_id, quantity, unit, latest, 0, "Cutover rollback",
		walk->actor, error);
}

static gboolean
rollback_walk(Rollback *walk, GError **error)
{
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GPtrArray) journals = NULL;
	guint phase, i;
	if (!cutover_rows(walk->self, walk->cutover_id, &rows, error))
		return FALSE;
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(rows, i);
		g_autofree gchar *status = NULL;
		g_autofree gchar *type = NULL;
		gint64 id = 0;
		g_object_get(row, "status", &status, "record-type", &type, "record-id", &id, NULL);
		if (g_strcmp0(status, "imported") != 0 || id <= 0)
			continue;
		g_hash_table_add(walk->owned, g_strdup_printf("%s|%" G_GINT64_FORMAT, type, id));
	}
	for (phase = 0; phase < 5; phase++)
	{
		for (i = 0; i < rows->len; i++)
		{
			VentureEntity *row = g_ptr_array_index(rows, i);
			g_autofree gchar *status = NULL;
			g_autofree gchar *type = NULL;
			g_autoptr(VentureEntity) record = NULL;
			gboolean vendor;
			gint64 id = 0;
			GType gtype = G_TYPE_INVALID;
			g_object_get(row, "status", &status, "record-type", &type, "record-id", &id, NULL);
			if (g_strcmp0(status, "imported") != 0 || id <= 0)
				continue;
			vendor = g_strcmp0(type, "vendor_bill") == 0 || g_strcmp0(type, "vendor_credit") == 0 || g_strcmp0(type, "bill_payment") == 0;
			if (g_strcmp0(type, "invoice") == 0)
				gtype = VENTURE_TYPE_INVOICE;
			else if (g_strcmp0(type, "vendor_bill") == 0)
				gtype = VENTURE_TYPE_VENDOR_BILL;
			else if (g_strcmp0(type, "payment") == 0)
				gtype = VENTURE_TYPE_PAYMENT;
			else if (g_strcmp0(type, "bill_payment") == 0)
				gtype = VENTURE_TYPE_BILL_PAYMENT;
			else if (g_strcmp0(type, "customer_credit") == 0)
				gtype = VENTURE_TYPE_CUSTOMER_CREDIT;
			else if (g_strcmp0(type, "vendor_credit") == 0)
				gtype = VENTURE_TYPE_VENDOR_CREDIT;
			else if (g_strcmp0(type, "fixed_asset") == 0)
				gtype = VENTURE_TYPE_FIXED_ASSET;
			else if (g_strcmp0(type, "inventory_txn") == 0)
				gtype = VENTURE_TYPE_INVENTORY_TXN;
			else
				continue;
			record = venture_database_get(walk->self->database, gtype, id, error);
			if (record == NULL)
				return FALSE;
			switch (phase)
			{
			case 0:
				if ((gtype == VENTURE_TYPE_INVOICE || gtype == VENTURE_TYPE_VENDOR_BILL) &&
					!unwind_document_cash(walk, vendor, record, error))
					return FALSE;
				break;
			case 1:
				if ((gtype == VENTURE_TYPE_PAYMENT || gtype == VENTURE_TYPE_BILL_PAYMENT) &&
					!unwind_payment(walk, vendor, record, error))
					return FALSE;
				break;
			case 2:
				if ((gtype == VENTURE_TYPE_CUSTOMER_CREDIT || gtype == VENTURE_TYPE_VENDOR_CREDIT) &&
					!unwind_credit(walk, vendor, record, error))
					return FALSE;
				break;
			case 3:
				if (gtype == VENTURE_TYPE_INVOICE || gtype == VENTURE_TYPE_VENDOR_BILL)
				{
					g_autofree gchar *document_status = NULL;
					gint invoice_status = 0;
					if (gtype == VENTURE_TYPE_INVOICE)
						g_object_get(record, "status", &invoice_status, NULL);
					else
						g_object_get(record, "status", &document_status, NULL);
					if (invoice_status == VENTURE_INVOICE_STATUS_VOID || g_strcmp0(document_status, "void") == 0)
						break;
					if (!unwind_document_void(walk, vendor, record, error))
						return FALSE;
				}
				break;
			default:
				if (gtype == VENTURE_TYPE_FIXED_ASSET && !unwind_asset(walk, record, error))
					return FALSE;
				if (gtype == VENTURE_TYPE_INVENTORY_TXN && !unwind_inventory(walk, record, error))
					return FALSE;
				break;
			}
		}
		/* Phase five, between the documents and the assets: every journal
		 * the batch posted itself -- bank, trial balance, payment offsets,
		 * asset openings -- is reversed at the cutoff. */
		if (phase == 3)
		{
			journals = venture_posting_service_find_source(venture_database_get_posting_service(walk->self->database),
				"accounting_cutover", walk->cutover_id, walk->org, error);
			if (journals == NULL)
				return FALSE;
			for (i = 0; i < journals->len; i++)
			{
				g_autoptr(VentureJournal) reversed = NULL;
				VentureJournalState state;
				gint64 reverses = 0;
				g_object_get(g_ptr_array_index(journals, i), "state", &state, "reverses-id", &reverses, NULL);
				if (state != VENTURE_JOURNAL_POSTED || reverses > 0)
					continue;
				check_postable(walk, walk->cutoff, "the batch's opening journals");
				if (walk->dry)
					continue;
				reversed = venture_posting_service_reverse(venture_database_get_posting_service(walk->self->database),
					venture_entity_get_id(g_ptr_array_index(journals, i)), walk->cutoff, "Cutover rollback", walk->actor, error);
				if (reversed == NULL)
					return FALSE;
			}
		}
	}
	return TRUE;
}

static void
rollback_prepare(Rollback *walk, VentureCutoverService *self, VentureEntity *cutover, gboolean dry,
	GPtrArray *blockers, const VentureActor *actor)
{
	walk->self = self;
	walk->cutover = cutover;
	walk->cutover_id = venture_entity_get_id(cutover);
	walk->org = venture_entity_get_organization_id(cutover);
	g_object_get(cutover, "cutoff", &walk->cutoff, NULL);
	walk->dry = dry;
	walk->suffix = g_strdup_printf("-rb%" G_GINT64_FORMAT, walk->cutover_id);
	walk->owned = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	walk->checked_dates = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	walk->blockers = blockers;
	walk->actor = actor;
}
