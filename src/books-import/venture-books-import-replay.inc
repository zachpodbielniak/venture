/* SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The replay: every document of the plan, in date order, through the
 * service that owns it. Included by venture-books-import-service.c.
 *
 * Where VENTURE's own posting differs from Zoho's -- a receipt lands in the
 * organization's cash account, a credit note debits its income account --
 * the document posts the ordinary way and a journal linked to the batch
 * moves the leg to the account Zoho used, on the document's date. Each such
 * journal is one document's correction, never a summary.
 */

typedef struct
{
	Plan *plan;
	VentureEntity *batch;
	const VentureActor *actor;
	Companies *companies;
	GHashTable *invoices;	/* Zoho invoice number -> invoice id */
	GHashTable *bills;	/* Zoho bill id -> bill id */
	GHashTable *credits;	/* Zoho credit note number -> credit id */
	GHashTable *vendor_credits;	/* Zoho vendor credit number -> credit id */
	GPtrArray *waiting;	/* Waiting: money that arrived before its document */
} Replay;

/* Part of a receipt Zoho applied to an invoice dated after the receipt:
 * the money waits as the receipt's credit and is applied when the invoice
 * is issued. */
typedef struct
{
	gchar *invoice;
	gint64 payment_id;
	gint64 cents;
} Waiting;

static void
waiting_free(gpointer data)
{
	Waiting *waiting = data;
	g_free(waiting->invoice);
	g_free(waiting);
}

static VentureMoney *
money(Plan *plan, gint64 cents)
{
	return venture_money_new(cents, plan->currency, (guint8)plan->exponent);
}

static gchar *
format_amount(Plan *plan, gint64 cents)
{
	gint64 scale = 1, whole, part;
	guint i;
	for (i = 0; i < plan->exponent; i++)
		scale *= 10;
	whole = (cents < 0 ? -cents : cents) / scale;
	part = (cents < 0 ? -cents : cents) % scale;
	if (plan->exponent == 0)
		return g_strdup_printf("%s%" G_GINT64_FORMAT " %s", cents < 0 ? "-" : "", whole, plan->currency);
	return g_strdup_printf("%s%" G_GINT64_FORMAT ".%0*" G_GINT64_FORMAT " %s", cents < 0 ? "-" : "", whole,
		(gint)plan->exponent, part, plan->currency);
}

static GDateTime *
event_date(Event *event)
{
	return parse_date(event->date);
}

static gchar *
period_of(const gchar *date)
{
	return g_strndup(date, 7);
}

static gboolean
note_event(Replay *replay, Event *event, const gchar *record_type, gint64 record_id, GError **error)
{
	g_autofree gchar *period = period_of(event->date);
	return add_row(replay->plan->self, replay->batch, event_objects[event->kind], event->source_id,
		"imported", record_type, record_id, period, event->cents, replay->plan->currency, TRUE,
		event->posts ? NULL : event->status, replay->actor, error);
}

static gboolean
note_record(Replay *replay, const gchar *object, const gchar *source_id, const gchar *record_type, gint64 record_id,
	gboolean created, GError **error)
{
	return add_row(replay->plan->self, replay->batch, object, source_id, created ? "imported" : "matched",
		record_type, record_id, NULL, 0, NULL, created, NULL, replay->actor, error);
}

/* One balanced journal of the batch: @legs alternate account id and signed
 * minor units (debit positive), terminated by a zero account. */
static gboolean
post_journal(Replay *replay, const gchar *date, const gchar *memo, const gint64 *legs, GError **error)
{
	Plan *plan = replay->plan;
	g_autoptr(VentureJournal) header = venture_journal_new();
	g_autoptr(VentureJournal) posted = NULL;
	g_autoptr(GPtrArray) lines = g_ptr_array_new_with_free_func(g_object_unref);
	g_autoptr(GDateTime) when = parse_date(date);
	guint i;
	for (i = 0; legs[i] != 0; i += 2)
	{
		VentureJournalLine *line;
		g_autoptr(VentureMoney) amount = NULL;
		if (legs[i + 1] == 0)
			continue;
		amount = money(plan, legs[i + 1] < 0 ? -legs[i + 1] : legs[i + 1]);
		line = venture_journal_line_new();
		g_object_set(line, "account-id", legs[i], "side", legs[i + 1] > 0 ? VENTURE_LEDGER_SIDE_DEBIT : VENTURE_LEDGER_SIDE_CREDIT,
			"amount", amount, "memo", memo, NULL);
		g_ptr_array_add(lines, line);
	}
	if (lines->len == 0)
		return TRUE;
	g_object_set(header, "organization-id", plan->org, "source-type", "books_import",
		"source-id", venture_entity_get_id(replay->batch), "occurred-at", when,
		"currency", plan->currency, "memo", memo, NULL);
	posted = venture_posting_service_post(venture_database_get_posting_service(plan->self->database),
		header, lines, NULL, replay->actor, error);
	return posted != NULL;
}

/* Moves @cents from @from to @to on @date: Dr @to, Cr @from. */
static gboolean
move(Replay *replay, const gchar *date, const gchar *memo, gint64 to, gint64 from, gint64 cents, GError **error)
{
	gint64 legs[5];
	if (to == from || cents == 0)
		return TRUE;
	legs[0] = to;
	legs[1] = cents;
	legs[2] = from;
	legs[3] = -cents;
	legs[4] = 0;
	return post_journal(replay, date, memo, legs, error);
}

/* The account a document's own posting used on @side: the cash account a
 * receipt landed in, the income account a credit note debited. */
static gint64
posted_account(Replay *replay, const gchar *source_type, gint64 source_id, VentureLedgerSide side,
	gint64 except, GError **error)
{
	Plan *plan = replay->plan;
	g_autoptr(GPtrArray) journals = venture_posting_service_find_source(venture_database_get_posting_service(plan->self->database),
		source_type, source_id, plan->org, error);
	guint i, j;
	if (journals == NULL)
		return 0;
	for (i = 0; i < journals->len; i++)
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_JOURNAL_LINE);
		g_autoptr(GPtrArray) lines = NULL;
		venture_query_set_limit(query, 0);
		if (!venture_query_add_filter_int(query, "journal-id", VENTURE_FILTER_OP_EQ,
			venture_entity_get_id(g_ptr_array_index(journals, i)), error))
			return 0;
		lines = venture_database_find(plan->self->database, query, error);
		if (lines == NULL)
			return 0;
		for (j = 0; j < lines->len; j++)
		{
			gint line_side;
			gint64 account = 0;
			g_object_get(g_ptr_array_index(lines, j), "side", &line_side, "account-id", &account, NULL);
			if (line_side == (gint)side && account != except)
				return account;
		}
	}
	return 0;
}

static gint64
party_of(Replay *replay, Table *table, guint row, gboolean vendor, GError **error)
{
	Party *party = document_party(replay->plan, table, row, vendor);
	gboolean created = FALSE;
	if (party == NULL)
		return -1;
	if (party->id > 0)
		return party->id;
	party->id = resolve_party(replay->plan, replay->companies, party, TRUE, &created, replay->actor, error);
	if (party->id <= 0)
		return -1;
	if (vendor && !created)
	{
		/* A matched company used as a vendor must be a supplier, which
		 * payables requires. */
		g_autoptr(VentureEntity) company = venture_database_get(replay->plan->self->database, VENTURE_TYPE_COMPANY, party->id, error);
		gint kind = 0;
		if (company == NULL)
			return -1;
		g_object_get(company, "kind", &kind, NULL);
		if (kind != VENTURE_COMPANY_KIND_SUPPLIER)
		{
			g_object_set(company, "kind", VENTURE_COMPANY_KIND_SUPPLIER, NULL);
			if (!venture_database_save(replay->plan->self->database, company, replay->actor, error))
				return -1;
		}
	}
	if (!note_record(replay, vendor ? "vendor" : "customer", party->source_id, "company", party->id, created, error))
		return -1;
	return party->id;
}

static gboolean
number_taken(Plan *plan, GType type, const gchar *number, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(type);
	g_autoptr(GPtrArray) found = NULL;
	venture_query_set_organization(query, plan->org);
	if (!venture_query_add_filter_string(query, "number", VENTURE_FILTER_OP_EQ, number, error))
		return TRUE;
	found = venture_database_find(plan->self->database, query, error);
	return found == NULL || found->len > 0;
}

/* --- Invoices -------------------------------------------------------------------------------- */

static gboolean apply_waiting(Replay *replay, const gchar *number, gint64 invoice_id, const gchar *date, GError **error);

static gboolean
replay_invoice(Replay *replay, Event *event, GError **error)
{
	Plan *plan = replay->plan;
	Table *table = event->table;
	guint head = first_row(event->rows);
	const gchar *number = cell(table, head, "Invoice Number");
	g_autoptr(VentureInvoice) invoice = venture_invoice_new();
	g_autoptr(GDateTime) issued = event_date(event);
	g_autoptr(GDateTime) due = parse_date(cell(table, head, "Due Date"));
	g_autofree gchar *external = g_strdup_printf("zoho-inv-%" G_GINT64_FORMAT "-%s", venture_entity_get_id(replay->batch), event->source_id);
	gint64 customer = party_of(replay, table, head, FALSE, error);
	gint64 adjustment = 0;
	guint l;
	if (customer <= 0)
		return FALSE;
	if (number_taken(plan, VENTURE_TYPE_INVOICE, number, error))
	{
		if (error == NULL || *error == NULL)
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT,
				"VentureBooksImportService: invoice number %s is already used in this organization", number);
		return FALSE;
	}
	g_object_set(invoice, "number", number, "company-id", customer, "issued-at", issued,
		"due-at", due != NULL ? due : issued, "terms", cell(table, head, "Payment Terms Label"),
		"notes", cell(table, head, "Notes"), "external-id", external,
		"dunning-disabled", TRUE, "late-fee-exempt", TRUE, NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(invoice), plan->org);
	if (!venture_database_save(plan->self->database, VENTURE_ENTITY(invoice), replay->actor, error))
		return FALSE;
	for (l = 0; l < event->rows->len; l++)
	{
		guint row = g_array_index(event->rows, guint, l);
		g_autoptr(VentureInvoiceLine) line = venture_invoice_line_new();
		g_autoptr(VentureMoney) price = NULL;
		g_autoptr(VentureMoney) net = NULL;
		g_autoptr(VentureMoney) tax = NULL;
		const gchar *name = cell(table, row, "Item Name");
		const gchar *description = *cell(table, row, "Item Desc") != '\0' ? cell(table, row, "Item Desc") : name;
		gint64 quantity = 0, unit = 0, line_total, line_tax, gross, percent = 0;
		parse_thousandths(cell(table, row, "Quantity"), &quantity);
		parse_amount(cell(table, row, "Item Price"), plan->exponent, &unit);
		line_total = amount_at(plan, table, row, "Item Total");
		line_tax = amount_at(plan, table, row, "Item Tax Amount");
		gross = (gint64)(((__int128)quantity * unit + 500) / 1000);
		if (gross != line_total)
		{
			/* VENTURE keeps a line discount as a whole percent of the line;
			 * a discount that is not one is priced into the line instead. */
			if (gross != 0 && ((__int128)(gross - line_total) * 100) % gross == 0)
				percent = (gint64)(((__int128)(gross - line_total) * 100) / gross);
			else
			{
				quantity = 1000;
				unit = line_total;
				g_ptr_array_add(plan->warnings, g_strdup_printf("invoice %s: a line discount that is not a whole percent is priced into the line",
					number));
			}
		}
		price = money(plan, unit);
		net = money(plan, line_total);
		tax = money(plan, line_tax);
		g_object_set(line, "invoice-id", venture_entity_get_id(VENTURE_ENTITY(invoice)),
			"description", *description != '\0' ? description : "Item", "quantity", quantity / 1000.0,
			"unit-price", price, "discount-percent", percent, "income-amount", net, "tax-amount", tax,
			"position", (gint64)l, "income-account-id", account_id(plan, cell(table, row, "Account")), NULL);
		if (percent != 0)
			g_object_set(line, "discount-account-id", account_id(plan, plan->discount_account), NULL);
		venture_entity_set_organization_id(VENTURE_ENTITY(line), plan->org);
		if (!venture_database_save(plan->self->database, VENTURE_ENTITY(line), replay->actor, error))
			return FALSE;
	}
	parse_amount(cell(table, head, "Adjustment"), plan->exponent, &adjustment);
	if (adjustment != 0)
	{
		g_autoptr(VentureInvoiceLine) line = venture_invoice_line_new();
		g_autoptr(VentureMoney) price = money(plan, adjustment);
		g_autoptr(VentureMoney) zero = money(plan, 0);
		const gchar *account = *cell(table, head, "Adjustment Account") != '\0' ? cell(table, head, "Adjustment Account") : plan->adjustment_account;
		g_object_set(line, "invoice-id", venture_entity_get_id(VENTURE_ENTITY(invoice)),
			"description", *cell(table, head, "Adjustment Description") != '\0' ? cell(table, head, "Adjustment Description") : "Adjustment",
			"quantity", 1.0, "unit-price", price, "income-amount", price, "tax-amount", zero,
			"position", (gint64)event->rows->len, "income-account-id", account_id(plan, account), NULL);
		venture_entity_set_organization_id(VENTURE_ENTITY(line), plan->org);
		if (!venture_database_save(plan->self->database, VENTURE_ENTITY(line), replay->actor, error))
			return FALSE;
	}
	if (is_status(event->status, "Draft"))
		return note_event(replay, event, "invoice", venture_entity_get_id(VENTURE_ENTITY(invoice)), error);
	{
		g_autoptr(VentureEntity) stored = venture_database_get(plan->self->database, VENTURE_TYPE_INVOICE,
			venture_entity_get_id(VENTURE_ENTITY(invoice)), error);
		if (stored == NULL ||
			!venture_settlement_service_transition(venture_settlement_service_get(plan->self->database),
				VENTURE_INVOICE(stored), "sent", issued, replay->actor, error))
			return FALSE;
	}
	/* A Zoho void was issued and cancelled; it nets to nothing either way,
	 * and keeping both events keeps its history. */
	if (is_status(event->status, "Void"))
	{
		g_autoptr(VentureEntity) stored = venture_database_get(plan->self->database, VENTURE_TYPE_INVOICE,
			venture_entity_get_id(VENTURE_ENTITY(invoice)), error);
		if (stored == NULL ||
			!venture_settlement_service_transition(venture_settlement_service_get(plan->self->database),
				VENTURE_INVOICE(stored), "void", issued, replay->actor, error))
			return FALSE;
	}
	g_hash_table_insert(replay->invoices, g_strdup(number), (gpointer)(gintptr)venture_entity_get_id(VENTURE_ENTITY(invoice)));
	return note_event(replay, event, "invoice", venture_entity_get_id(VENTURE_ENTITY(invoice)), error) &&
		apply_waiting(replay, number, venture_entity_get_id(VENTURE_ENTITY(invoice)), event->date, error);
}

/* --- Customer payments ------------------------------------------------------------------------ */

static gboolean
allocate_credit(Replay *replay, gint64 credit_id, gint64 invoice_id, const gchar *date, gint64 cents, GError **error)
{
	Plan *plan = replay->plan;
	g_autoptr(VenturePaymentAllocation) allocation = venture_payment_allocation_new();
	g_autoptr(VentureMoney) amount = money(plan, cents);
	g_autoptr(GDateTime) when = parse_date(date);
	g_object_set(allocation, "credit-id", credit_id, "invoice-id", invoice_id, "date", when, "amount", amount, NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(allocation), plan->org);
	return venture_database_save(plan->self->database, VENTURE_ENTITY(allocation), replay->actor, error);
}

static gint64
receipt_credit(Replay *replay, gint64 payment_id, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_CUSTOMER_CREDIT);
	g_autoptr(GPtrArray) found = NULL;
	venture_query_set_organization(query, replay->plan->org);
	if (!venture_query_add_filter_int(query, "payment-id", VENTURE_FILTER_OP_EQ, payment_id, error))
		return 0;
	found = venture_database_find(replay->plan->self->database, query, error);
	if (found == NULL || found->len != 1)
	{
		if (found != NULL)
			refuse(error, "a receipt with money left over has no unique credit");
		return 0;
	}
	return venture_entity_get_id(g_ptr_array_index(found, 0));
}

static gboolean
apply_waiting(Replay *replay, const gchar *number, gint64 invoice_id, const gchar *date, GError **error)
{
	guint i;
	for (i = 0; i < replay->waiting->len; i++)
	{
		Waiting *waiting = g_ptr_array_index(replay->waiting, i);
		gint64 credit;
		if (g_strcmp0(waiting->invoice, number) != 0 || waiting->cents == 0)
			continue;
		credit = receipt_credit(replay, waiting->payment_id, error);
		if (credit == 0 || !allocate_credit(replay, credit, invoice_id, date, waiting->cents, error))
			return FALSE;
		waiting->cents = 0;
	}
	return TRUE;
}

static gboolean
replay_customer_payment(Replay *replay, Event *event, GError **error)
{
	Plan *plan = replay->plan;
	Table *table = event->table;
	guint head = first_row(event->rows);
	g_autoptr(VenturePayment) payment = venture_payment_new();
	g_autoptr(GPtrArray) allocations = g_ptr_array_new_with_free_func(g_object_unref);
	g_autoptr(VentureMoney) amount = money(plan, event->cents);
	g_autoptr(GDateTime) when = event_date(event);
	g_autoptr(GPtrArray) later = g_ptr_array_new_with_free_func(waiting_free);
	g_autofree gchar *external = g_strdup_printf("zoho-pay-%" G_GINT64_FORMAT "-%s", venture_entity_get_id(replay->batch), event->source_id);
	g_autofree gchar *reference = NULL;
	g_autofree gchar *memo = NULL;
	const gchar *deposit = cell(table, head, "Deposit To");
	gint64 customer = party_of(replay, table, head, FALSE, error);
	gint64 charges = amount_at(plan, table, head, "Bank Charges");
	gint64 cash;
	guint l;
	if (customer <= 0)
		return FALSE;
	for (l = 0; l < event->rows->len; l++)
	{
		guint row = g_array_index(event->rows, guint, l);
		const gchar *number = cell(table, row, "Invoice Number");
		gint64 part = amount_at(plan, table, row, "Amount Applied to Invoice");
		gpointer invoice;
		if (part == 0)
			continue;
		invoice = g_hash_table_lookup(replay->invoices, number);
		if (invoice == NULL)
		{
			/* Paid before the invoice was issued: the money waits as a
			 * credit and settles the invoice on its date. */
			Waiting *waiting = g_new0(Waiting, 1);
			waiting->invoice = g_strdup(number);
			waiting->cents = part;
			g_ptr_array_add(later, waiting);
			continue;
		}
		{
			VenturePaymentAllocation *allocation = venture_payment_allocation_new();
			g_autoptr(VentureMoney) applied = money(plan, part);
			g_object_set(allocation, "invoice-id", (gint64)(gintptr)invoice, "amount", applied, "date", when, NULL);
			venture_entity_set_organization_id(VENTURE_ENTITY(allocation), plan->org);
			g_ptr_array_add(allocations, allocation);
		}
	}
	reference = *cell(table, head, "Reference Number") != '\0'
		? g_strdup_printf("Zoho payment %s, %s", cell(table, head, "Payment Number"), cell(table, head, "Reference Number"))
		: g_strdup_printf("Zoho payment %s", cell(table, head, "Payment Number"));
	g_object_set(payment, "customer-id", customer, "date", when, "amount", amount,
		"method", *cell(table, head, "Mode") != '\0' ? cell(table, head, "Mode") : "zoho_books",
		"reference", reference, "external-id", external, NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(payment), plan->org);
	/* Money the customer paid long ago: no receipt goes out for it. */
	venture_payment_mark_bookkeeping(payment);
	if (!venture_settlement_service_apply_payment(venture_settlement_service_get(plan->self->database),
		payment, allocations->len > 0 ? allocations : NULL, replay->actor, error))
		return FALSE;
	for (l = 0; l < later->len; l++)
	{
		Waiting *waiting = g_ptr_array_index(later, l);
		waiting->payment_id = venture_entity_get_id(VENTURE_ENTITY(payment));
		g_ptr_array_add(replay->waiting, g_ptr_array_steal_index(later, l));
		l--;
	}
	cash = posted_account(replay, "payment", venture_entity_get_id(VENTURE_ENTITY(payment)), VENTURE_LEDGER_SIDE_DEBIT, 0, error);
	if (cash == 0)
		return error != NULL && *error != NULL ? FALSE : refuse(error, "the receipt posted no cash leg");
	memo = g_strdup_printf("Zoho payment %s: deposit account and bank charges", cell(table, head, "Payment Number"));
	if (!move(replay, event->date, memo, account_id(plan, deposit), cash, event->cents, error) ||
		!move(replay, event->date, memo, account_id(plan, plan->bank_charges_account), account_id(plan, deposit), charges, error))
		return FALSE;
	return note_event(replay, event, "payment", venture_entity_get_id(VENTURE_ENTITY(payment)), error);
}

/* --- Bills -------------------------------------------------------------------------------------- */

static gchar *
bill_number(Plan *plan, Event *event)
{
	const gchar *number = cell(event->table, first_row(event->rows), "Bill Number");
	const gchar *id = event->source_id;
	gsize length = strlen(id);
	const gchar *tail = length > 6 ? id + length - 6 : id;
	guint i, same = 0;
	if (*number == '\0')
		return g_strdup_printf("ZB-%s", tail);
	/* Zoho numbers bills per vendor; VENTURE numbers them per organization. */
	for (i = 0; i < plan->events->len; i++)
	{
		Event *other = g_ptr_array_index(plan->events, i);
		if (other->kind == EVENT_BILL && g_strcmp0(cell(other->table, first_row(other->rows), "Bill Number"), number) == 0)
			same++;
	}
	return same > 1 ? g_strdup_printf("%s-%s", number, tail) : g_strdup(number);
}

static gboolean
bill_line(Replay *replay, gint64 bill_id, guint position, const gchar *description, const gchar *quantity,
	gint64 unit, gint64 account, GError **error)
{
	Plan *plan = replay->plan;
	g_autoptr(VentureVendorBillLine) line = venture_vendor_bill_line_new();
	g_autoptr(VentureMoney) price = money(plan, unit);
	g_autoptr(VentureMoney) zero = money(plan, 0);
	g_object_set(line, "bill-id", bill_id, "description", *description != '\0' ? description : "Item",
		"quantity", quantity, "unit-price", price, "tax-amount", zero, "account-id", account,
		"position", (gint64)position, NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(line), plan->org);
	return venture_database_save(plan->self->database, VENTURE_ENTITY(line), replay->actor, error);
}

static gboolean
replay_bill(Replay *replay, Event *event, GError **error)
{
	Plan *plan = replay->plan;
	Table *table = event->table;
	guint head = first_row(event->rows);
	g_autoptr(VentureVendorBill) bill = venture_vendor_bill_new();
	g_autoptr(GDateTime) dated = event_date(event);
	g_autoptr(GDateTime) due = parse_date(cell(table, head, "Due Date"));
	g_autofree gchar *number = bill_number(plan, event);
	const gchar *adjustment_account = *cell(table, head, "Adjustment Account") != '\0' ? cell(table, head, "Adjustment Account") : "Other Expenses";
	gint64 vendor = party_of(replay, table, head, TRUE, error);
	gint64 tax = 0, adjustment = amount_at(plan, table, head, "Adjustment");
	gint64 bill_id;
	guint l, position = 0;
	if (vendor <= 0)
		return FALSE;
	if (number_taken(plan, VENTURE_TYPE_VENDOR_BILL, number, error))
	{
		if (error == NULL || *error == NULL)
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT,
				"VentureBooksImportService: bill number %s is already used in this organization", number);
		return FALSE;
	}
	g_object_set(bill, "number", number, "company-id", vendor, "bill-date", dated, "due-date", due != NULL ? due : dated,
		"currency", plan->currency, "status", "draft", "memo", cell(table, head, "Vendor Notes"), NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(bill), plan->org);
	if (!venture_database_save(plan->self->database, VENTURE_ENTITY(bill), replay->actor, error))
		return FALSE;
	bill_id = venture_entity_get_id(VENTURE_ENTITY(bill));
	for (l = 0; l < event->rows->len; l++)
	{
		guint row = g_array_index(event->rows, guint, l);
		gint64 quantity = 0, rate = 0, total = amount_at(plan, table, row, "Item Total");
		const gchar *description = *cell(table, row, "Description") != '\0' ? cell(table, row, "Description") : cell(table, row, "Item Name");
		parse_thousandths(cell(table, row, "Quantity"), &quantity);
		parse_amount(cell(table, row, "Rate"), plan->exponent, &rate);
		tax += amount_at(plan, table, row, "Tax Amount");
		if (total == 0)
			continue;
		/* Quantity and rate when they make the line's total exactly, else
		 * the total as one unit: the ledger must get Zoho's figure. */
		if (quantity > 0 && (gint64)(((__int128)quantity * rate + 500) / 1000) == total)
		{
			g_autofree gchar *text = g_strdup_printf("%" G_GINT64_FORMAT ".%03" G_GINT64_FORMAT, quantity / 1000, quantity % 1000);
			if (!bill_line(replay, bill_id, position++, description, text, rate, account_id(plan, cell(table, row, "Account")), error))
				return FALSE;
		}
		else if (!bill_line(replay, bill_id, position++, description, "1", total, account_id(plan, cell(table, row, "Account")), error))
			return FALSE;
	}
	/* Purchase tax is Zoho's own expense line. A negative adjustment (Zoho
	 * rounds bill totals) rides on that line and a dated journal moves it to
	 * the adjustment account, since a bill line cannot be negative. */
	if (tax != 0 || adjustment != 0)
	{
		gint64 carried = tax + adjustment;
		gint64 tax_account = account_id(plan, plan->purchase_tax_account);
		if (carried <= 0)
			return refuse(error, "a bill adjustment larger than its tax is not supported");
		if (!bill_line(replay, bill_id, position++, tax != 0 ? "Tax" : "Adjustment", "1", carried,
			tax != 0 ? tax_account : account_id(plan, adjustment_account), error))
			return FALSE;
	}
	if (is_status(event->status, "Draft"))
		return note_event(replay, event, "vendor_bill", bill_id, error);
	{
		g_autoptr(VentureEntity) stored = venture_database_get(plan->self->database, VENTURE_TYPE_VENDOR_BILL, bill_id, error);
		if (stored == NULL ||
			!venture_payables_service_transition(venture_payables_service_get(plan->self->database),
				VENTURE_VENDOR_BILL(stored), "approved", dated, replay->actor, error))
			return FALSE;
	}
	if (tax != 0 && adjustment != 0)
	{
		g_autofree gchar *memo = g_strdup_printf("Zoho bill %s: adjustment", number);
		if (!move(replay, event->date, memo, account_id(plan, plan->purchase_tax_account),
			account_id(plan, adjustment_account), -adjustment, error))
			return FALSE;
	}
	if (is_status(event->status, "Void"))
	{
		g_autoptr(VentureEntity) stored = venture_database_get(plan->self->database, VENTURE_TYPE_VENDOR_BILL, bill_id, error);
		if (stored == NULL ||
			!venture_payables_service_transition(venture_payables_service_get(plan->self->database),
				VENTURE_VENDOR_BILL(stored), "void", dated, replay->actor, error))
			return FALSE;
	}
	g_hash_table_insert(replay->bills, g_strdup(event->source_id), (gpointer)(gintptr)bill_id);
	return note_event(replay, event, "vendor_bill", bill_id, error);
}

static gboolean
replay_vendor_payment(Replay *replay, Event *event, GError **error)
{
	Plan *plan = replay->plan;
	Table *table = event->table;
	guint head = first_row(event->rows);
	g_autoptr(VentureBillPayment) payment = venture_bill_payment_new();
	g_autoptr(GPtrArray) allocations = g_ptr_array_new_with_free_func(g_object_unref);
	g_autoptr(VentureMoney) amount = money(plan, event->cents);
	g_autoptr(GDateTime) when = event_date(event);
	g_autofree gchar *external = g_strdup_printf("zoho-vpay-%" G_GINT64_FORMAT "-%s", venture_entity_get_id(replay->batch), event->source_id);
	g_autofree gchar *reference = NULL;
	g_autofree gchar *memo = NULL;
	gint64 vendor = party_of(replay, table, head, TRUE, error);
	gint64 cash;
	guint l;
	if (vendor <= 0)
		return FALSE;
	for (l = 0; l < event->rows->len; l++)
	{
		guint row = g_array_index(event->rows, guint, l);
		gint64 part = amount_at(plan, table, row, "Bill Amount");
		gpointer bill;
		if (part == 0)
			continue;
		bill = g_hash_table_lookup(replay->bills, cell(table, row, "Bill ID"));
		if (bill == NULL)
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
				"VentureBooksImportService: vendor payment %s pays a bill not yet imported; a payment before its bill is not supported",
				cell(table, head, "Payment Number"));
			return FALSE;
		}
		{
			VentureBillPaymentAllocation *allocation = venture_bill_payment_allocation_new();
			g_autoptr(VentureMoney) applied = money(plan, part);
			g_object_set(allocation, "bill-id", (gint64)(gintptr)bill, "amount", applied, "date", when, NULL);
			venture_entity_set_organization_id(VENTURE_ENTITY(allocation), plan->org);
			g_ptr_array_add(allocations, allocation);
		}
	}
	reference = *cell(table, head, "Reference Number") != '\0'
		? g_strdup_printf("Zoho payment %s, %s", cell(table, head, "Payment Number"), cell(table, head, "Reference Number"))
		: g_strdup_printf("Zoho payment %s", cell(table, head, "Payment Number"));
	g_object_set(payment, "vendor-id", vendor, "date", when, "amount", amount,
		"method", *cell(table, head, "Mode") != '\0' ? cell(table, head, "Mode") : "zoho_books",
		"reference", reference, "external-id", external, NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(payment), plan->org);
	if (!venture_payables_service_apply_payment(venture_payables_service_get(plan->self->database),
		payment, allocations->len > 0 ? allocations : NULL, replay->actor, error))
		return FALSE;
	cash = posted_account(replay, "bill_payment", venture_entity_get_id(VENTURE_ENTITY(payment)), VENTURE_LEDGER_SIDE_CREDIT, 0, error);
	if (cash == 0)
		return error != NULL && *error != NULL ? FALSE : refuse(error, "the vendor payment posted no cash leg");
	memo = g_strdup_printf("Zoho vendor payment %s: paid-through account", cell(table, head, "Payment Number"));
	if (!move(replay, event->date, memo, cash, account_id(plan, cell(table, head, "Paid Through")), event->cents, error))
		return FALSE;
	return note_event(replay, event, "bill_payment", venture_entity_get_id(VENTURE_ENTITY(payment)), error);
}

/* --- Expenses, money moves, journals ------------------------------------------------------------- */

static gboolean
save_expense(Replay *replay, Event *event, const gchar *suffix, gint64 cents, gint64 account, gint64 cash,
	gint64 *id, GError **error)
{
	Plan *plan = replay->plan;
	Table *table = event->table;
	guint row = first_row(event->rows);
	g_autoptr(VentureExpense) expense = venture_expense_new();
	g_autoptr(VentureMoney) amount = money(plan, cents);
	g_autoptr(GDateTime) when = event_date(event);
	g_autofree gchar *external = g_strdup_printf("zoho-exp-%" G_GINT64_FORMAT "-%s%s", venture_entity_get_id(replay->batch), event->source_id, suffix);
	const gchar *description = cell(table, row, "Expense Description");
	g_object_set(expense, "description", *description != '\0' ? description : cell(table, row, "Expense Account"),
		"vendor", cell(table, row, "Vendor"), "amount", amount, "occurred-at", when,
		"category", cell(table, row, "Expense Account"), "expense-account-id", account, "cash-account-id", cash,
		"payment-method", "zoho_books", "external-id", external, NULL);
	if (*cell(table, row, "Reference#") != '\0')
		g_object_set(expense, "notes", cell(table, row, "Reference#"), NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(expense), plan->org);
	if (!venture_database_save(plan->self->database, VENTURE_ENTITY(expense), replay->actor, error))
		return FALSE;
	*id = venture_entity_get_id(VENTURE_ENTITY(expense));
	return TRUE;
}

static gboolean
replay_expense(Replay *replay, Event *event, GError **error)
{
	Plan *plan = replay->plan;
	Table *table = event->table;
	guint row = first_row(event->rows);
	gint64 tax = amount_at(plan, table, row, "Tax Amount");
	gint64 cash = account_id(plan, cell(table, row, "Paid Through"));
	gint64 id = 0, tax_id = 0;
	/* The tax Zoho books to its purchase tax account is an expense of its
	 * own, so each account carries exactly Zoho's figure. */
	if (!save_expense(replay, event, "", event->cents - tax, account_id(plan, cell(table, row, "Expense Account")), cash, &id, error))
		return FALSE;
	if (tax != 0 && !save_expense(replay, event, "-tax", tax, account_id(plan, plan->purchase_tax_account), cash, &tax_id, error))
		return FALSE;
	if (tax_id != 0 && !add_row(plan->self, replay->batch, "expense_tax", event->source_id, "imported", "expense", tax_id,
		NULL, 0, NULL, TRUE, NULL, replay->actor, error))
		return FALSE;
	return note_event(replay, event, "expense", id, error);
}

static gboolean
replay_money_move(Replay *replay, Event *event, GError **error)
{
	Plan *plan = replay->plan;
	Table *table = event->table;
	guint row = first_row(event->rows);
	Account *from = plan_account(plan, cell(table, row, "From Account"));
	Account *to = plan_account(plan, cell(table, row, "To Account"));
	g_autofree gchar *memo = g_strdup_printf("Zoho %s%s%s", cell(table, row, "Transaction Type"),
		*cell(table, row, "Description") != '\0' ? ": " : "", cell(table, row, "Description"));
	g_autoptr(GDateTime) when = event_date(event);
	gboolean cashlike = from->kind != VENTURE_ACCOUNT_KIND_INCOME && from->kind != VENTURE_ACCOUNT_KIND_EXPENSE &&
		to->kind != VENTURE_ACCOUNT_KIND_INCOME && to->kind != VENTURE_ACCOUNT_KIND_EXPENSE;
	/* Owner money and transfers go through the capital service, so they
	 * show as what they are; anything it refuses is an ordinary journal. */
	if (cashlike && venture_entity_registry_lookup(venture_entity_registry_get_default(), "equity_transaction") != G_TYPE_INVALID)
	{
		g_autoptr(VentureMoney) amount = money(plan, event->cents);
		VentureEquityKind kind = from->kind == VENTURE_ACCOUNT_KIND_EQUITY ? VENTURE_EQUITY_KIND_CONTRIBUTION
			: to->kind == VENTURE_ACCOUNT_KIND_EQUITY ? VENTURE_EQUITY_KIND_DRAW : VENTURE_EQUITY_KIND_TRANSFER;
		g_autoptr(VentureEntity) row_record = venture_capital_service_post(venture_capital_service_get(plan->self->database),
			plan->org, kind, amount, when, memo, to->id, from->id, replay->actor, error);
		if (row_record == NULL)
			return FALSE;
		return note_event(replay, event, "equity_transaction", venture_entity_get_id(row_record), error);
	}
	if (!move(replay, event->date, memo, to->id, from->id, event->cents, error))
		return FALSE;
	return note_event(replay, event, "journal", 0, error);
}

static gboolean
replay_journal(Replay *replay, Event *event, GError **error)
{
	Plan *plan = replay->plan;
	Table *table = event->table;
	g_autofree gint64 *legs = g_new0(gint64, event->rows->len * 2 + 1);
	g_autofree gchar *memo = g_strdup_printf("Zoho journal %s%s%s", cell(table, first_row(event->rows), "Journal Number"),
		*cell(table, first_row(event->rows), "Notes") != '\0' ? ": " : "", cell(table, first_row(event->rows), "Notes"));
	guint l, n = 0;
	for (l = 0; l < event->rows->len; l++)
	{
		guint row = g_array_index(event->rows, guint, l);
		legs[n++] = account_id(plan, cell(table, row, "Account"));
		legs[n++] = amount_at(plan, table, row, "Debit") - amount_at(plan, table, row, "Credit");
	}
	legs[n] = 0;
	return post_journal(replay, event->date, memo, legs, error) && note_event(replay, event, "journal", 0, error);
}

/* --- Credits ------------------------------------------------------------------------------------- */

static gboolean
replay_credit_note(Replay *replay, Event *event, GError **error)
{
	Plan *plan = replay->plan;
	Table *table = event->table;
	guint head = first_row(event->rows);
	g_autoptr(VentureCustomerCredit) credit = NULL;
	g_autoptr(VentureMoney) amount = money(plan, event->cents);
	g_autoptr(GDateTime) when = event_date(event);
	const gchar *number = cell(table, head, "Credit Note Number");
	g_autofree gchar *memo = g_strdup_printf("Zoho credit note %s: line accounts", number);
	gint64 customer = party_of(replay, table, head, FALSE, error);
	gint64 income;
	guint l;
	if (customer <= 0)
		return FALSE;
	if (!event->posts)
		return note_event(replay, event, "customer_credit", 0, error);
	credit = venture_customer_credit_new();
	g_object_set(credit, "customer-id", customer, "kind", "credit_note", "date", when, "amount", amount,
		"remaining", amount, "reference", number, NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(credit), plan->org);
	if (!venture_database_save(plan->self->database, VENTURE_ENTITY(credit), replay->actor, error))
		return FALSE;
	income = posted_account(replay, "customer_credit", venture_entity_get_id(VENTURE_ENTITY(credit)), VENTURE_LEDGER_SIDE_DEBIT, 0, error);
	if (income == 0)
		return error != NULL && *error != NULL ? FALSE : refuse(error, "the credit note posted no income leg");
	for (l = 0; l < event->rows->len; l++)
	{
		guint row = g_array_index(event->rows, guint, l);
		if (!move(replay, event->date, memo, account_id(plan, cell(table, row, "Account")), income,
			amount_at(plan, table, row, "Item Total"), error))
			return FALSE;
	}
	g_hash_table_insert(replay->credits, g_strdup(number), (gpointer)(gintptr)venture_entity_get_id(VENTURE_ENTITY(credit)));
	return note_event(replay, event, "customer_credit", venture_entity_get_id(VENTURE_ENTITY(credit)), error);
}

static gboolean
replay_credit_application(Replay *replay, Event *event, GError **error)
{
	Table *table = event->table;
	guint row = first_row(event->rows);
	gpointer credit = g_hash_table_lookup(replay->credits, cell(table, row, "Credit Note Number"));
	gpointer invoice = g_hash_table_lookup(replay->invoices, cell(table, row, "Invoice Number"));
	if (credit == NULL || invoice == NULL)
		return refuse(error, "a credit application names a credit note or invoice that was not imported before it");
	return allocate_credit(replay, (gint64)(gintptr)credit, (gint64)(gintptr)invoice, event->date, event->cents, error) &&
		note_event(replay, event, "payment_allocation", 0, error);
}

static gboolean
replay_refund(Replay *replay, Event *event, GError **error)
{
	Plan *plan = replay->plan;
	Table *table = event->table;
	guint row = first_row(event->rows);
	gpointer credit = g_hash_table_lookup(replay->credits, cell(table, row, "Credit Note Number"));
	g_autoptr(VentureRefund) refund = venture_refund_new();
	g_autoptr(VentureMoney) amount = money(plan, event->cents);
	g_autoptr(GDateTime) when = event_date(event);
	g_autoptr(VentureEntity) stored = NULL;
	gint64 customer = 0, cash;
	if (credit == NULL)
		return refuse(error, "a refund names a credit note that was not imported before it");
	stored = venture_database_get(plan->self->database, VENTURE_TYPE_CUSTOMER_CREDIT, (gint64)(gintptr)credit, error);
	if (stored == NULL)
		return FALSE;
	g_object_get(stored, "customer-id", &customer, NULL);
	g_object_set(refund, "credit-id", (gint64)(gintptr)credit, "customer-id", customer, "date", when, "amount", amount,
		"reference", cell(table, row, "Reference Number"), NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(refund), plan->org);
	if (!venture_database_save(plan->self->database, VENTURE_ENTITY(refund), replay->actor, error))
		return FALSE;
	cash = posted_account(replay, "refund", venture_entity_get_id(VENTURE_ENTITY(refund)), VENTURE_LEDGER_SIDE_CREDIT, 0, error);
	if (cash == 0)
		return error != NULL && *error != NULL ? FALSE : refuse(error, "the refund posted no cash leg");
	return move(replay, event->date, "Zoho refund: refund account", cash,
		account_id(plan, cell(table, row, "Refund From Account")), event->cents, error) &&
		note_event(replay, event, "refund", venture_entity_get_id(VENTURE_ENTITY(refund)), error);
}

static gboolean
replay_vendor_credit(Replay *replay, Event *event, GError **error)
{
	Plan *plan = replay->plan;
	Table *table = event->table;
	guint head = first_row(event->rows);
	g_autoptr(VentureVendorCredit) credit = NULL;
	g_autoptr(VentureMoney) amount = money(plan, event->cents);
	g_autoptr(GDateTime) when = event_date(event);
	const gchar *number = cell(table, head, "Vendor Credit Number");
	g_autofree gchar *memo = g_strdup_printf("Zoho vendor credit %s: line accounts", number);
	gint64 vendor = party_of(replay, table, head, TRUE, error);
	gint64 expense;
	guint l;
	if (vendor <= 0)
		return FALSE;
	if (!event->posts)
		return note_event(replay, event, "vendor_credit", 0, error);
	credit = venture_vendor_credit_new();
	g_object_set(credit, "vendor-id", vendor, "kind", "credit_note", "date", when, "amount", amount,
		"remaining", amount, "reference", number, NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(credit), plan->org);
	if (!venture_database_save(plan->self->database, VENTURE_ENTITY(credit), replay->actor, error))
		return FALSE;
	expense = posted_account(replay, "vendor_credit", venture_entity_get_id(VENTURE_ENTITY(credit)), VENTURE_LEDGER_SIDE_CREDIT, 0, error);
	if (expense == 0)
		return error != NULL && *error != NULL ? FALSE : refuse(error, "the vendor credit posted no expense leg");
	for (l = 0; l < event->rows->len; l++)
	{
		guint row = g_array_index(event->rows, guint, l);
		if (!move(replay, event->date, memo, expense, account_id(plan, cell(table, row, "Account")),
			amount_at(plan, table, row, "Item Total"), error))
			return FALSE;
	}
	g_hash_table_insert(replay->vendor_credits, g_strdup(number), (gpointer)(gintptr)venture_entity_get_id(VENTURE_ENTITY(credit)));
	return note_event(replay, event, "vendor_credit", venture_entity_get_id(VENTURE_ENTITY(credit)), error);
}

static gboolean
replay_vendor_refund(Replay *replay, Event *event, GError **error)
{
	Plan *plan = replay->plan;
	Table *table = event->table;
	guint row = first_row(event->rows);
	gpointer credit = g_hash_table_lookup(replay->vendor_credits, cell(table, row, "Vendor Credit Number"));
	g_autoptr(VentureBillRefund) refund = venture_bill_refund_new();
	g_autoptr(VentureMoney) amount = money(plan, event->cents);
	g_autoptr(GDateTime) when = event_date(event);
	g_autoptr(VentureEntity) stored = NULL;
	gint64 vendor = 0, cash;
	if (credit == NULL)
		return refuse(error, "a vendor refund names a vendor credit that was not imported before it");
	stored = venture_database_get(plan->self->database, VENTURE_TYPE_VENDOR_CREDIT, (gint64)(gintptr)credit, error);
	if (stored == NULL)
		return FALSE;
	g_object_get(stored, "vendor-id", &vendor, NULL);
	g_object_set(refund, "credit-id", (gint64)(gintptr)credit, "vendor-id", vendor, "date", when, "amount", amount,
		"reference", cell(table, row, "Reference Number"), NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(refund), plan->org);
	if (!venture_database_save(plan->self->database, VENTURE_ENTITY(refund), replay->actor, error))
		return FALSE;
	cash = posted_account(replay, "bill_refund", venture_entity_get_id(VENTURE_ENTITY(refund)), VENTURE_LEDGER_SIDE_DEBIT, 0, error);
	if (cash == 0)
		return error != NULL && *error != NULL ? FALSE : refuse(error, "the vendor refund posted no cash leg");
	return move(replay, event->date, "Zoho vendor refund: refund account", account_id(plan, cell(table, row, "Refund Account")),
		cash, event->cents, error) &&
		note_event(replay, event, "bill_refund", venture_entity_get_id(VENTURE_ENTITY(refund)), error);
}

/* --- The opening ------------------------------------------------------------------------------- */

/* The day-one balances as an ordinary cutover batch at the start: trial
 * balance rows against the opening clearing account, which nets to zero. */
static gboolean
replay_opening(Replay *replay, GError **error)
{
	Plan *plan = replay->plan;
	VentureCutoverService *cutover = venture_cutover_service_get(plan->self->database);
	g_autoptr(JsonObject) payload = json_object_new();
	g_autoptr(VentureEntity) batch = NULL;
	JsonArray *rows = json_array_new();
	guint i;
	if (plan->opening->len == 0)
	{
		json_array_unref(rows);
		return TRUE;
	}
	json_object_set_string_member(payload, "source", "zoho_books");
	json_object_set_string_member(payload, "cutoff", plan->start_text);
	json_object_set_string_member(payload, "currency", plan->currency);
	for (i = 0; i < plan->opening->len; i++)
	{
		Posting *posting = g_ptr_array_index(plan->opening, i);
		g_autoptr(VentureEntity) account = venture_database_get(plan->self->database, VENTURE_TYPE_ACCOUNT,
			account_id(plan, posting->account), error);
		g_autofree gchar *code = NULL;
		g_autofree gchar *amount = format_amount(plan, posting->cents < 0 ? -posting->cents : posting->cents);
		JsonObject *row = json_object_new();
		if (account == NULL)
		{
			json_object_unref(row);
			json_array_unref(rows);
			return FALSE;
		}
		g_object_get(account, "code", &code, NULL);
		json_object_set_string_member(row, "account_code", code);
		json_object_set_string_member(row, "name", posting->account);
		json_object_set_string_member(row, posting->cents > 0 ? "debit" : "credit", amount);
		json_array_add_object_element(rows, row);
	}
	json_object_set_array_member(payload, "trial_balance", rows);
	batch = venture_cutover_service_preview(cutover, plan->org, payload, replay->actor, error);
	if (batch == NULL || !venture_cutover_service_import(cutover, VENTURE_ACCOUNTING_CUTOVER(batch), replay->actor, error))
		return FALSE;
	g_object_set(replay->batch, "cutover-id", venture_entity_get_id(batch), NULL);
	return note_record(replay, "opening", plan->start_text, "accounting_cutover", venture_entity_get_id(batch), TRUE, error);
}

static gboolean
replay_event(Replay *replay, Event *event, GError **error)
{
	switch (event->kind)
	{
	case EVENT_INVOICE: return replay_invoice(replay, event, error);
	case EVENT_BILL: return replay_bill(replay, event, error);
	case EVENT_CREDIT_NOTE: return replay_credit_note(replay, event, error);
	case EVENT_VENDOR_CREDIT: return replay_vendor_credit(replay, event, error);
	case EVENT_CUSTOMER_PAYMENT: return replay_customer_payment(replay, event, error);
	case EVENT_VENDOR_PAYMENT: return replay_vendor_payment(replay, event, error);
	case EVENT_CREDIT_APPLICATION: return replay_credit_application(replay, event, error);
	case EVENT_REFUND: return replay_refund(replay, event, error);
	case EVENT_VENDOR_REFUND: return replay_vendor_refund(replay, event, error);
	case EVENT_EXPENSE: return replay_expense(replay, event, error);
	case EVENT_MONEY_MOVE: return replay_money_move(replay, event, error);
	case EVENT_JOURNAL: return replay_journal(replay, event, error);
	default: return refuse(error, "unknown event");
	}
}

/* Every contact comes over, not only those with documents: the backup is
 * the business's whole customer and vendor list. */
static gboolean
replay_contacts(Replay *replay, GHashTable *side, GError **error)
{
	GHashTableIter iter;
	gpointer value;
	g_hash_table_iter_init(&iter, side);
	while (g_hash_table_iter_next(&iter, NULL, &value))
	{
		Party *party = value;
		gboolean created = FALSE;
		if (party->id > 0)
			continue;
		party->id = resolve_party(replay->plan, replay->companies, party, TRUE, &created, replay->actor, error);
		if (party->id <= 0)
			return error != NULL && *error != NULL ? FALSE : refuse(error, "a contact could not be matched or created");
		if (!note_record(replay, party->vendor ? "vendor" : "customer", party->source_id, "company", party->id, created, error))
			return FALSE;
	}
	return TRUE;
}

static gboolean
import_impl(VentureBooksImportService *self, VentureEntity *batch, const VentureActor *actor, GError **error)
{
	g_autoptr(Plan) plan = plan_of(self, batch, error);
	g_autoptr(Companies) companies = NULL;
	g_autoptr(GString) block = g_string_new(NULL);
	g_autofree gchar *stamp = stamp_now();
	Replay replay;
	guint i;
	gboolean ok = FALSE;
	if (plan == NULL || !resolve_accounts(plan, FALSE, actor, error) || !check_owned(plan, venture_entity_get_id(batch), error))
		return FALSE;
	preview_parties(plan, actor);
	if (!refuse_problems(plan, error))
		return FALSE;
	memset(&replay, 0, sizeof replay);
	replay.plan = plan;
	replay.batch = batch;
	replay.actor = actor;
	replay.invoices = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	replay.bills = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	replay.credits = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	replay.vendor_credits = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	replay.waiting = g_ptr_array_new_with_free_func(waiting_free);
	if (!venture_database_begin(self->database, error))
		goto done;
	companies = companies_load(self, plan->org, error);
	replay.companies = companies;
	if (companies == NULL || !resolve_accounts(plan, TRUE, actor, error) ||
		!replay_contacts(&replay, plan->customers, error) || !replay_contacts(&replay, plan->vendors, error) ||
		!replay_opening(&replay, error))
		goto fail;
	for (i = 0; i < plan->events->len; i++)
		if (!replay_event(&replay, g_ptr_array_index(plan->events, i), error))
		{
			Event *event = g_ptr_array_index(plan->events, i);
			g_prefix_error(error, "%s row %u (%s %s): ", event->table->file, first_row(event->rows) + 2,
				event_objects[event->kind], event->date);
			goto fail;
		}
	for (i = 0; i < replay.waiting->len; i++)
		if (((Waiting *)g_ptr_array_index(replay.waiting, i))->cents != 0)
		{
			refuse(error, "a payment is applied to an invoice the backup never issued");
			goto fail;
		}
	g_string_append_printf(block, "\n== Import %s ==\n", stamp);
	g_string_append_printf(block, "Replayed %u documents in date order from %s.\n", plan->events->len, plan->start_text);
	for (i = 0; i < plan->warnings->len; i++)
		g_string_append_printf(block, "WARNING %s\n", (const gchar *)g_ptr_array_index(plan->warnings, i));
	if (!append_report(self, batch, "imported", block, actor, error) || !venture_database_commit(self->database, error))
		goto fail;
	ok = TRUE;
	goto done;
fail:
	venture_database_rollback(self->database);
done:
	g_hash_table_unref(replay.invoices);
	g_hash_table_unref(replay.bills);
	g_hash_table_unref(replay.credits);
	g_hash_table_unref(replay.vendor_credits);
	g_ptr_array_unref(replay.waiting);
	return ok;
}

gboolean
venture_books_import_service_import(VentureBooksImportService *self, VentureBooksImport *batch,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureAccountingOperation) operation = NULL;
	g_autoptr(VentureEntity) stored = NULL;
	g_autofree gchar *state = NULL;
	GVariantBuilder arguments;
	g_return_val_if_fail(VENTURE_IS_BOOKS_IMPORT_SERVICE(self), FALSE);
	stored = venture_database_get(self->database, VENTURE_TYPE_BOOKS_IMPORT, venture_entity_get_id(VENTURE_ENTITY(batch)), error);
	if (stored == NULL)
		return FALSE;
	state = state_of(stored);
	/* One transaction made the whole import, so a second call on an
	 * imported batch has nothing left to do. */
	if (g_strcmp0(state, "imported") == 0 || g_strcmp0(state, "reconciled") == 0)
		return TRUE;
	if (g_strcmp0(state, "preview") != 0)
		return refuse(error, "only a previewed batch can be imported");
	g_variant_builder_init(&arguments, G_VARIANT_TYPE_VARDICT);
	operation = venture_accounting_operation_begin(self->database, "books-import", stored, NULL,
		g_variant_builder_end(&arguments), venture_entity_get_organization_id(stored), actor, error);
	if (operation == NULL)
		return FALSE;
	if (!import_impl(self, stored, actor, error))
		return FALSE;
	if (!venture_accounting_operation_finish(operation, error))
		return FALSE;
	venture_entity_copy_properties_from(VENTURE_ENTITY(batch), stored, FALSE);
	return TRUE;
}
