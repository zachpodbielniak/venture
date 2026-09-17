/* SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Opening vendor bills, customer/vendor credits and fixed assets. Included by
 * venture-cutover-service.c after its row and money helpers; every document
 * posts through its owning service, never through hand-written ledger rows.
 */

static gboolean add_money(VentureMoney **total, const VentureMoney *amount, GError **error);

static gboolean
refuse_rule(GError **error, const gchar *source_id, const gchar *rule, const gchar *message)
{
	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "VentureCutoverService: %s %s (rule: %s)",
		source_id != NULL ? source_id : "row", message, rule);
	return FALSE;
}

static const gchar *
row_currency(JsonObject *row, JsonObject *payload)
{
	const gchar *currency = obj_str(row, "currency");
	return currency != NULL ? currency : obj_str(payload, "currency");
}

static const gchar *
party_name(JsonArray *parties, const gchar *source_id, const gchar *fallback)
{
	guint i;
	if (parties == NULL || source_id == NULL)
		return fallback;
	for (i = 0; i < json_array_get_length(parties); i++)
	{
		JsonObject *party = json_array_get_object_element(parties, i);
		if (g_strcmp0(obj_str(party, "source_id"), source_id) == 0 && obj_str(party, "name") != NULL)
			return obj_str(party, "name");
	}
	return fallback;
}

static gint64
account_by_code(VentureCutoverService *self, gint64 org, const gchar *code, const gchar *what, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ACCOUNT);
	g_autoptr(GPtrArray) accounts = NULL;
	if (code == NULL || *code == '\0')
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"VentureCutoverService: %s account code is required", what);
		return 0;
	}
	venture_query_set_organization(query, org);
	if (!venture_query_add_filter_string(query, "code", VENTURE_FILTER_OP_EQ, code, error))
		return 0;
	accounts = venture_database_find(self->database, query, error);
	if (accounts == NULL)
		return 0;
	if (accounts->len == 0)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"VentureCutoverService: %s account code %s is missing from the chart", what, code);
		return 0;
	}
	return venture_entity_get_id(g_ptr_array_index(accounts, 0));
}

/* Vendors are suppliers to the payables service; a party created for a
 * bill or vendor credit is stamped as one. */
static gint64
find_or_create_vendor(VentureCutoverService *self, gint64 cutover_id, gint64 org, JsonObject *payload,
	const gchar *source_id, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) company = NULL;
	gint kind = 0;
	gint64 id = find_or_create_company(self, cutover_id, org,
		party_name(arr(payload, "vendors"), source_id, "Imported vendor"), source_id, "vendor", actor, error);
	if (id == 0)
		return 0;
	company = venture_database_get(self->database, VENTURE_TYPE_COMPANY, id, error);
	if (company == NULL)
		return 0;
	g_object_get(company, "kind", &kind, NULL);
	if (kind != VENTURE_COMPANY_KIND_SUPPLIER)
	{
		g_object_set(company, "kind", VENTURE_COMPANY_KIND_SUPPLIER, NULL);
		if (!venture_database_save(self->database, company, actor, error))
			return 0;
	}
	return id;
}

static gint64
find_or_create_customer(VentureCutoverService *self, gint64 cutover_id, gint64 org, JsonObject *payload,
	const gchar *source_id, const VentureActor *actor, GError **error)
{
	return find_or_create_company(self, cutover_id, org,
		party_name(arr(payload, "customers"), source_id, "Imported customer"), source_id, "customer", actor, error);
}

/* The bill's remaining balance from the payload alone: lines (net plus
 * frozen tax) less the amount already paid before the cutoff. */
static VentureMoney *
bill_expected_balance(JsonObject *row, JsonObject *payload, VentureMoney **total_out,
	VentureMoney **paid_out, GError **error)
{
	JsonArray *lines = arr(row, "lines");
	g_autoptr(VentureMoney) total = NULL;
	g_autoptr(VentureMoney) paid = NULL;
	g_autoptr(VentureMoney) balance = NULL;
	const gchar *currency = row_currency(row, payload);
	guint i;
	if (lines == NULL || json_array_get_length(lines) == 0)
	{
		refuse_rule(error, obj_str(row, "source_id"), "open-ap-lines", "needs at least one line");
		return NULL;
	}
	for (i = 0; i < json_array_get_length(lines); i++)
	{
		JsonObject *line = json_array_get_object_element(lines, i);
		g_autoptr(VentureMoney) net = parse_money(obj_str(line, "amount"), currency, error);
		g_autoptr(VentureMoney) tax = NULL;
		if (net == NULL)
			return NULL;
		if (obj_str(line, "tax") != NULL)
		{
			tax = parse_money(obj_str(line, "tax"), currency, error);
			if (tax == NULL)
				return NULL;
		}
		if (!add_money(&total, net, error) || !add_money(&total, tax, error))
			return NULL;
	}
	if (obj_str(row, "paid") != NULL)
		paid = parse_money(obj_str(row, "paid"), currency, error);
	else
		paid = venture_money_new_zero(venture_money_get_currency(total));
	if (paid == NULL)
		return NULL;
	balance = venture_money_subtract(total, paid, error);
	if (balance == NULL)
		return NULL;
	if (venture_money_get_amount(paid) < 0 || venture_money_get_amount(balance) <= 0)
	{
		refuse_rule(error, obj_str(row, "source_id"), "open-ap-balance",
			"must have a nonnegative paid amount below its total; a settled bill is not open");
		return NULL;
	}
	if (obj_str(row, "balance") != NULL)
	{
		g_autoptr(VentureMoney) declared = parse_money(obj_str(row, "balance"), currency, error);
		if (declared == NULL)
			return NULL;
		if (!venture_money_equal(declared, balance))
		{
			refuse_rule(error, obj_str(row, "source_id"), "open-ap-balance",
				"declares a balance that is not its lines less the paid amount");
			return NULL;
		}
	}
	if (total_out != NULL)
		*total_out = g_steal_pointer(&total);
	if (paid_out != NULL)
		*paid_out = g_steal_pointer(&paid);
	return g_steal_pointer(&balance);
}

static gint64
equity_account(VentureCutoverService *self, gint64 org, GError **error)
{
	return account_by_code(self, org, "3000", "opening equity", error);
}

/* A partial payment made before the cutoff already sits inside the source
 * bank balance. The opening payment therefore debits AP through the payables
 * service and its cash leg is offset against opening equity, so cash is not
 * counted twice. */
static gboolean
offset_opening_payment(VentureCutoverService *self, VentureEntity *cutover, gint64 payment_id,
	const VentureMoney *paid, const gchar *number, GDateTime *cutoff, const VentureActor *actor, GError **error)
{
	gint64 org = venture_entity_get_organization_id(cutover);
	g_autoptr(GPtrArray) journals = NULL;
	g_autoptr(VentureJournal) header = venture_journal_new();
	g_autoptr(VentureJournal) posted = NULL;
	g_autoptr(GPtrArray) lines = g_ptr_array_new_with_free_func(g_object_unref);
	g_autofree gchar *memo = g_strdup_printf("Opening payment offset %s", number != NULL ? number : "");
	VentureJournalLine *line;
	gint64 cash_id = 0, equity_id;
	guint i, j;
	journals = venture_posting_service_find_source(venture_database_get_posting_service(self->database),
		"bill_payment", payment_id, org, error);
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
		journal_lines = venture_database_find(self->database, query, error);
		if (journal_lines == NULL)
			return FALSE;
		for (j = 0; j < journal_lines->len; j++)
		{
			gint side;
			gint64 account_id = 0;
			g_object_get(g_ptr_array_index(journal_lines, j), "side", &side, "account-id", &account_id, NULL);
			if (side == VENTURE_LEDGER_SIDE_CREDIT)
				cash_id = account_id;
		}
	}
	if (cash_id == 0)
		return refuse(error, "the opening payment did not post a cash leg");
	equity_id = equity_account(self, org, error);
	if (equity_id == 0)
		return FALSE;
	g_object_set(header, "organization-id", org, "source-type", "accounting_cutover",
		"source-id", venture_entity_get_id(cutover), "occurred-at", cutoff,
		"currency", venture_money_get_currency(paid), "memo", memo, NULL);
	line = venture_journal_line_new();
	g_object_set(line, "account-id", cash_id, "side", VENTURE_LEDGER_SIDE_DEBIT, "amount", paid, NULL);
	g_ptr_array_add(lines, line);
	line = venture_journal_line_new();
	g_object_set(line, "account-id", equity_id, "side", VENTURE_LEDGER_SIDE_CREDIT, "amount", paid, NULL);
	g_ptr_array_add(lines, line);
	posted = venture_posting_service_post(venture_database_get_posting_service(self->database),
		header, lines, NULL, actor, error);
	return posted != NULL;
}

static gboolean
import_open_ap(VentureCutoverService *self, VentureEntity *cutover, JsonObject *payload,
	const VentureActor *actor, GError **error)
{
	JsonArray *open_ap = arr(payload, "open_ap");
	VenturePayablesService *payables = venture_payables_service_get(self->database);
	gint64 org = venture_entity_get_organization_id(cutover);
	gint64 cutover_id = venture_entity_get_id(cutover);
	g_autoptr(GDateTime) cutoff = NULL;
	guint i;
	if (open_ap == NULL)
		return TRUE;
	g_object_get(cutover, "cutoff", &cutoff, NULL);
	for (i = 0; i < json_array_get_length(open_ap); i++)
	{
		JsonObject *row = json_array_get_object_element(open_ap, i);
		JsonArray *lines;
		const gchar *source_id = obj_str(row, "source_id");
		const gchar *currency = row_currency(row, payload);
		g_autoptr(VentureVendorBill) bill = NULL;
		g_autoptr(VentureMoney) balance = NULL;
		g_autoptr(VentureMoney) total = NULL;
		g_autoptr(VentureMoney) paid = NULL;
		g_autoptr(GDateTime) bill_date = NULL;
		gint64 vendor_id, existing;
		guint l;
		existing = existing_record(self, org, source_id, "open_ap", error);
		if (existing < 0)
			return FALSE;
		if (existing > 0)
			continue;
		if (source_id == NULL || obj_str(row, "number") == NULL || obj_str(row, "date") == NULL)
			return refuse_rule(error, source_id, "open-ap-identity", "needs source_id, number and date");
		balance = bill_expected_balance(row, payload, &total, &paid, error);
		if (balance == NULL)
			return FALSE;
		vendor_id = find_or_create_vendor(self, cutover_id, org, payload, obj_str(row, "vendor_source_id"), actor, error);
		if (vendor_id == 0)
			return FALSE;
		bill_date = venture_time_from_string(obj_str(row, "date"), error);
		if (bill_date == NULL)
			return FALSE;
		bill = venture_vendor_bill_new();
		g_object_set(bill, "number", obj_str(row, "number"), "company-id", vendor_id,
			"currency", venture_money_get_currency(total), "status", "draft", "bill-date", bill_date,
			"memo", obj_str(row, "memo"), NULL);
		venture_entity_set_organization_id(VENTURE_ENTITY(bill), org);
		if (obj_str(row, "due_date") != NULL &&
			!venture_entity_set_field_from_string(VENTURE_ENTITY(bill), "due-date", obj_str(row, "due_date"), error))
			return FALSE;
		if (!venture_database_save(self->database, VENTURE_ENTITY(bill), actor, error))
			return FALSE;
		lines = arr(row, "lines");
		for (l = 0; l < json_array_get_length(lines); l++)
		{
			JsonObject *src = json_array_get_object_element(lines, l);
			g_autoptr(VentureVendorBillLine) line = venture_vendor_bill_line_new();
			g_autoptr(VentureMoney) net = parse_money(obj_str(src, "amount"), currency, error);
			g_autoptr(VentureMoney) tax = NULL;
			gint64 account_id = 0;
			if (net == NULL)
				return FALSE;
			tax = obj_str(src, "tax") != NULL ? parse_money(obj_str(src, "tax"), currency, error)
				: venture_money_new_zero(venture_money_get_currency(net));
			if (tax == NULL)
				return FALSE;
			if (obj_str(src, "account_code") != NULL)
			{
				account_id = account_by_code(self, org, obj_str(src, "account_code"), "bill line expense", error);
				if (account_id == 0)
					return FALSE;
			}
			g_object_set(line, "bill-id", venture_entity_get_id(VENTURE_ENTITY(bill)),
				"description", obj_str(src, "description") != NULL ? obj_str(src, "description") : "Opening balance",
				"quantity", "1", "unit-price", net, "tax-amount", tax, "account-id", account_id,
				"category", obj_str(src, "category"), "position", (gint64)l, NULL);
			venture_entity_set_organization_id(VENTURE_ENTITY(line), org);
			if (!venture_database_save(self->database, VENTURE_ENTITY(line), actor, error))
				return FALSE;
		}
		if (!venture_payables_service_transition(payables, bill, "approved", bill_date, actor, error))
			return FALSE;
		if (!add_row(self, cutover_id, org, source_id, "open_ap", "imported", NULL,
			"vendor_bill", venture_entity_get_id(VENTURE_ENTITY(bill)), actor, error))
			return FALSE;
		if (!venture_money_is_zero(paid))
		{
			g_autoptr(VentureBillPayment) payment = venture_bill_payment_new();
			g_autofree gchar *reference = g_strdup_printf("cutover:%s", source_id);
			g_object_set(payment, "vendor-id", vendor_id, "bill-id", venture_entity_get_id(VENTURE_ENTITY(bill)),
				"amount", paid, "date", cutoff, "method", "opening", "reference", reference, NULL);
			venture_entity_set_organization_id(VENTURE_ENTITY(payment), org);
			if (!venture_payables_service_apply_payment(payables, payment, NULL, actor, error))
				return FALSE;
			if (!offset_opening_payment(self, cutover, venture_entity_get_id(VENTURE_ENTITY(payment)), paid,
				obj_str(row, "number"), cutoff, actor, error))
				return FALSE;
			if (!add_row(self, cutover_id, org, source_id, "open_ap_payment", "imported", NULL,
				"bill_payment", venture_entity_get_id(VENTURE_ENTITY(payment)), actor, error))
				return FALSE;
		}
	}
	return TRUE;
}

static gboolean
credit_is_vendor(JsonObject *row)
{
	const gchar *kind = obj_str(row, "kind");
	return g_strcmp0(kind, "vendor") == 0 || (kind == NULL && obj_str(row, "vendor_source_id") != NULL);
}

static gboolean
import_credits(VentureCutoverService *self, VentureEntity *cutover, JsonObject *payload,
	const VentureActor *actor, GError **error)
{
	JsonArray *credits = arr(payload, "credits");
	gint64 org = venture_entity_get_organization_id(cutover);
	gint64 cutover_id = venture_entity_get_id(cutover);
	guint i;
	if (credits == NULL)
		return TRUE;
	for (i = 0; i < json_array_get_length(credits); i++)
	{
		JsonObject *row = json_array_get_object_element(credits, i);
		const gchar *source_id = obj_str(row, "source_id");
		g_autoptr(VentureMoney) amount = NULL;
		g_autoptr(VentureMoney) open = NULL;
		g_autoptr(VentureEntity) credit = NULL;
		g_autoptr(GDateTime) date = NULL;
		gboolean vendor = credit_is_vendor(row);
		gint64 party, existing;
		existing = existing_record(self, org, source_id, "credit", error);
		if (existing < 0)
			return FALSE;
		if (existing > 0)
			continue;
		if (source_id == NULL || obj_str(row, "date") == NULL)
			return refuse_rule(error, source_id, "credit-identity", "needs source_id and date");
		{
			const gchar *kind = obj_str(row, "kind");

			if (kind != NULL && g_strcmp0(kind, "customer") != 0 && g_strcmp0(kind, "vendor") != 0)
				return refuse_rule(error, source_id, "credit-identity", "kind must be customer or vendor");
		}
		amount = parse_money(obj_str(row, "amount"), row_currency(row, payload), error);
		if (amount == NULL)
			return FALSE;
		date = venture_time_from_string(obj_str(row, "date"), error);
		if (date == NULL)
			return FALSE;
		if (vendor)
		{
			party = find_or_create_vendor(self, cutover_id, org, payload, obj_str(row, "vendor_source_id"), actor, error);
			if (party == 0)
				return FALSE;
			open = venture_payables_service_vendor_balance(venture_payables_service_get(self->database),
				org, party, NULL, venture_money_get_currency(amount), error);
		}
		else
		{
			party = find_or_create_customer(self, cutover_id, org, payload, obj_str(row, "customer_source_id"), actor, error);
			if (party == 0)
				return FALSE;
			open = venture_settlement_service_customer_balance(venture_settlement_service_get(self->database),
				org, party, NULL, venture_money_get_currency(amount), error);
		}
		if (open == NULL)
			return FALSE;
		if (venture_money_compare(amount, open) > 0)
			return refuse_rule(error, source_id, "credit-within-open-balance",
				"exceeds the counterpart's open balance");
		credit = vendor ? VENTURE_ENTITY(venture_vendor_credit_new()) : VENTURE_ENTITY(venture_customer_credit_new());
		g_object_set(credit, vendor ? "vendor-id" : "customer-id", party, "date", date, "amount", amount,
			"kind", "credit_note", "reference", obj_str(row, "number") != NULL ? obj_str(row, "number") : source_id, NULL);
		venture_entity_set_organization_id(credit, org);
		if (!venture_database_save(self->database, credit, actor, error))
			return FALSE;
		if (!add_row(self, cutover_id, org, source_id, "credit", "imported", NULL,
			venture_entity_get_entity_name(credit), venture_entity_get_id(credit), actor, error))
			return FALSE;
	}
	return TRUE;
}

static gboolean
import_assets(VentureCutoverService *self, VentureEntity *cutover, JsonObject *payload,
	const VentureActor *actor, GError **error)
{
	JsonArray *assets = arr(payload, "assets");
	gint64 org = venture_entity_get_organization_id(cutover);
	gint64 cutover_id = venture_entity_get_id(cutover);
	g_autoptr(GDateTime) cutoff = NULL;
	guint i;
	if (assets == NULL)
		return TRUE;
	g_object_get(cutover, "cutoff", &cutoff, NULL);
	for (i = 0; i < json_array_get_length(assets); i++)
	{
		JsonObject *row = json_array_get_object_element(assets, i);
		const gchar *source_id = obj_str(row, "source_id");
		const gchar *currency = row_currency(row, payload);
		g_autoptr(VentureMoney) cost = NULL;
		g_autoptr(VentureMoney) salvage = NULL;
		g_autoptr(VentureMoney) accumulated = NULL;
		g_autoptr(VentureEntity) asset = NULL;
		gint64 asset_account, accumulated_account, expense_account, equity_id, existing;
		existing = existing_record(self, org, source_id, "asset", error);
		if (existing < 0)
			return FALSE;
		if (existing > 0)
			continue;
		if (source_id == NULL || obj_str(row, "in_service_at") == NULL || !json_object_has_member(row, "useful_life_months"))
			return refuse_rule(error, source_id, "asset-identity", "needs source_id, in_service_at and useful_life_months");
		cost = parse_money(obj_str(row, "cost"), currency, error);
		if (cost == NULL)
			return FALSE;
		salvage = obj_str(row, "salvage") != NULL ? parse_money(obj_str(row, "salvage"), currency, error)
			: venture_money_new_zero(venture_money_get_currency(cost));
		if (salvage == NULL)
			return FALSE;
		accumulated = obj_str(row, "accumulated_depreciation") != NULL
			? parse_money(obj_str(row, "accumulated_depreciation"), currency, error)
			: venture_money_new_zero(venture_money_get_currency(cost));
		if (accumulated == NULL)
			return FALSE;
		asset_account = account_by_code(self, org, obj_str(row, "asset_account_code"), "asset", error);
		if (asset_account == 0)
			return FALSE;
		accumulated_account = account_by_code(self, org, obj_str(row, "accumulated_depreciation_account_code"),
			"accumulated depreciation", error);
		if (accumulated_account == 0)
			return FALSE;
		expense_account = account_by_code(self, org, obj_str(row, "depreciation_expense_account_code"),
			"depreciation expense", error);
		if (expense_account == 0)
			return FALSE;
		equity_id = equity_account(self, org, error);
		if (equity_id == 0)
			return FALSE;
		asset = VENTURE_ENTITY(venture_fixed_asset_new());
		g_object_set(asset, "name", obj_str(row, "name") != NULL ? obj_str(row, "name") : source_id,
			"tag", obj_str(row, "tag") != NULL ? obj_str(row, "tag") : source_id,
			"category", obj_str(row, "category"), "cost", cost, "salvage-value", salvage,
			"useful-life-months", json_object_get_int_member(row, "useful_life_months"),
			"asset-account-id", asset_account, "accumulated-depreciation-account-id", accumulated_account,
			"depreciation-expense-account-id", expense_account, "status", VENTURE_ASSET_STATUS_DRAFT, NULL);
		venture_entity_set_organization_id(asset, org);
		if (!venture_entity_set_field_from_string(asset, "in-service-at", obj_str(row, "in_service_at"), error) ||
			!venture_entity_set_field_from_string(asset, "acquired-at",
				obj_str(row, "acquired_at") != NULL ? obj_str(row, "acquired_at") : obj_str(row, "in_service_at"), error) ||
			!venture_entity_set_field_from_string(asset, "method",
				obj_str(row, "method") != NULL ? obj_str(row, "method") : "straight_line", error))
			return FALSE;
		if (!venture_database_save(self->database, asset, actor, error))
			return FALSE;
		if (!venture_asset_service_import_opening(venture_asset_service_get(self->database), asset, accumulated,
			cutoff, equity_id, "accounting_cutover", cutover_id, actor, error))
			return FALSE;
		if (!add_row(self, cutover_id, org, source_id, "asset", "imported", NULL,
			"fixed_asset", venture_entity_get_id(asset), actor, error))
			return FALSE;
	}
	return TRUE;
}

static gboolean
preview_opening_rows(VentureCutoverService *self, gint64 cutover_id, gint64 org, JsonObject *payload,
	const VentureActor *actor, GError **error)
{
	static const gchar *const sections[] = { "open_ap", "credits", "assets" };
	static const gchar *const types[] = { "open_ap", "credit", "asset" };
	guint s, i;
	for (s = 0; s < G_N_ELEMENTS(sections); s++)
	{
		JsonArray *rows = arr(payload, sections[s]);
		if (rows == NULL)
			continue;
		for (i = 0; i < json_array_get_length(rows); i++)
		{
			JsonObject *row = json_array_get_object_element(rows, i);
			if (!add_row(self, cutover_id, org, obj_str(row, "source_id"), types[s], "preview", NULL, NULL, 0, actor, error))
				return FALSE;
		}
	}
	return TRUE;
}

/* Net book value the register reports for one migrated asset: cost less the
 * depreciation posted on or before the cutoff, which is its opening entry. */
static VentureMoney *
asset_register_nbv(VentureCutoverService *self, gint64 asset_id, GDateTime *cutoff, GError **error)
{
	g_autoptr(VentureEntity) asset = venture_database_get(self->database, VENTURE_TYPE_FIXED_ASSET, asset_id, error);
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_DEPRECIATION_ENTRY);
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(VentureMoney) cost = NULL;
	g_autoptr(VentureMoney) nbv = NULL;
	guint i;
	if (asset == NULL)
		return NULL;
	g_object_get(asset, "cost", &cost, NULL);
	nbv = venture_money_copy(cost);
	venture_query_set_limit(query, 0);
	if (!venture_query_add_filter_int(query, "asset-id", VENTURE_FILTER_OP_EQ, asset_id, error))
		return NULL;
	rows = venture_database_find(self->database, query, error);
	if (rows == NULL)
		return NULL;
	for (i = 0; i < rows->len; i++)
	{
		g_autoptr(VentureEntity) journal = NULL;
		g_autoptr(GDateTime) date = NULL;
		g_autoptr(VentureMoney) amount = NULL;
		g_autoptr(VentureMoney) next = NULL;
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
		next = venture_money_subtract(nbv, amount, error);
		if (next == NULL)
			return NULL;
		venture_money_free(nbv);
		nbv = g_steal_pointer(&next);
	}
	return g_steal_pointer(&nbv);
}

static gboolean
reconcile_openings(VentureCutoverService *self, VentureEntity *cutover, JsonObject *payload,
	GPtrArray *rows, GDateTime *cutoff, GString *report, GError **error)
{
	g_autoptr(VentureMoney) expected_ap = NULL;
	g_autoptr(VentureMoney) imported_ap = NULL;
	g_autoptr(VentureMoney) expected_credits = NULL;
	g_autoptr(VentureMoney) imported_credits = NULL;
	g_autoptr(VentureMoney) expected_nbv = NULL;
	g_autoptr(VentureMoney) imported_nbv = NULL;
	JsonArray *section;
	guint i;
	section = arr(payload, "open_ap");
	for (i = 0; section != NULL && i < json_array_get_length(section); i++)
	{
		g_autoptr(VentureMoney) balance = bill_expected_balance(json_array_get_object_element(section, i),
			payload, NULL, NULL, error);
		if (balance == NULL || !add_money(&expected_ap, balance, error))
			return FALSE;
	}
	section = arr(payload, "credits");
	for (i = 0; section != NULL && i < json_array_get_length(section); i++)
	{
		JsonObject *row = json_array_get_object_element(section, i);
		g_autoptr(VentureMoney) amount = parse_money(obj_str(row, "amount"), row_currency(row, payload), error);
		if (amount == NULL || !add_money(&expected_credits, amount, error))
			return FALSE;
	}
	section = arr(payload, "assets");
	for (i = 0; section != NULL && i < json_array_get_length(section); i++)
	{
		JsonObject *row = json_array_get_object_element(section, i);
		g_autoptr(VentureMoney) cost = parse_money(obj_str(row, "cost"), row_currency(row, payload), error);
		g_autoptr(VentureMoney) accumulated = NULL;
		g_autoptr(VentureMoney) nbv = NULL;
		if (cost == NULL)
			return FALSE;
		accumulated = obj_str(row, "accumulated_depreciation") != NULL
			? parse_money(obj_str(row, "accumulated_depreciation"), row_currency(row, payload), error)
			: venture_money_new_zero(venture_money_get_currency(cost));
		if (accumulated == NULL)
			return FALSE;
		nbv = venture_money_subtract(cost, accumulated, error);
		if (nbv == NULL || !add_money(&expected_nbv, nbv, error))
			return FALSE;
	}
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(rows, i);
		g_autofree gchar *type = NULL;
		g_autofree gchar *status = NULL;
		gint64 record_id = 0;
		g_object_get(row, "source-type", &type, "record-id", &record_id, "status", &status, NULL);
		if (record_id <= 0 || g_strcmp0(status, "imported") != 0)
			continue;
		if (g_strcmp0(type, "open_ap") == 0)
		{
			g_autoptr(VentureMoney) balance = venture_payables_service_bill_balance(
				venture_payables_service_get(self->database), record_id, NULL, error);
			if (balance == NULL || !add_money(&imported_ap, balance, error))
				return FALSE;
		}
		else if (g_strcmp0(type, "credit") == 0)
		{
			g_autofree gchar *record_type = NULL;
			g_autoptr(VentureEntity) credit = NULL;
			g_autoptr(VentureMoney) remaining = NULL;
			g_object_get(row, "record-type", &record_type, NULL);
			credit = venture_database_get(self->database, g_strcmp0(record_type, "vendor_credit") == 0
				? VENTURE_TYPE_VENDOR_CREDIT : VENTURE_TYPE_CUSTOMER_CREDIT, record_id, error);
			if (credit == NULL)
				return FALSE;
			g_object_get(credit, "remaining", &remaining, NULL);
			if (!add_money(&imported_credits, remaining, error))
				return FALSE;
		}
		else if (g_strcmp0(type, "asset") == 0)
		{
			g_autoptr(VentureMoney) nbv = asset_register_nbv(self, record_id, cutoff, error);
			if (nbv == NULL || !add_money(&imported_nbv, nbv, error))
				return FALSE;
		}
	}
	if (expected_ap != NULL)
	{
		if (imported_ap == NULL || !venture_money_equal(expected_ap, imported_ap))
			return refuse(error, "opening AP does not match approved bill balances");
		g_string_append_printf(report, "Open AP %" G_GINT64_FORMAT "\n", venture_money_get_amount(expected_ap));
	}
	if (expected_credits != NULL)
	{
		if (imported_credits == NULL || !venture_money_equal(expected_credits, imported_credits))
			return refuse(error, "opening credits do not match unapplied credit balances");
		g_string_append_printf(report, "Credits %" G_GINT64_FORMAT "\n", venture_money_get_amount(expected_credits));
	}
	if (expected_nbv != NULL)
	{
		if (imported_nbv == NULL || !venture_money_equal(expected_nbv, imported_nbv))
			return refuse(error, "opening asset net book value does not match the register");
		g_string_append_printf(report, "Asset NBV %" G_GINT64_FORMAT "\n", venture_money_get_amount(expected_nbv));
	}
	return TRUE;
}

static gboolean
reverse_source_journals(VentureCutoverService *self, const gchar *source_type, gint64 source_id, gint64 org,
	GDateTime *when, const VentureActor *actor, GError **error)
{
	g_autoptr(GPtrArray) journals = venture_posting_service_find_source(
		venture_database_get_posting_service(self->database), source_type, source_id, org, error);
	guint i;
	if (journals == NULL)
		return FALSE;
	for (i = 0; i < journals->len; i++)
	{
		VentureEntity *journal = g_ptr_array_index(journals, i);
		g_autoptr(VentureJournal) reversed = NULL;
		VentureJournalState state;
		g_object_get(journal, "state", &state, NULL);
		if (state != VENTURE_JOURNAL_POSTED)
			continue;
		reversed = venture_posting_service_reverse(venture_database_get_posting_service(self->database),
			venture_entity_get_id(journal), when, "Cutover rollback", actor, error);
		if (reversed == NULL)
			return FALSE;
	}
	return TRUE;
}

/* Rollback of the three opening kinds. Bills refund their opening payment
 * and are voided through the payables service; credits have their posting
 * reversed; assets are retired after the generic loop reversed their
 * opening journal. Nothing is deleted. */
static gboolean
rollback_opening_row(VentureCutoverService *self, VentureEntity *cutover, const gchar *record_type,
	gint64 record_id, GDateTime *cutoff, const VentureActor *actor, GError **error)
{
	gint64 org = venture_entity_get_organization_id(cutover);
	if (g_strcmp0(record_type, "vendor_bill") == 0)
	{
		VenturePayablesService *payables = venture_payables_service_get(self->database);
		g_autoptr(VentureEntity) bill = NULL;
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_BILL_PAYMENT_ALLOCATION);
		g_autoptr(GPtrArray) allocations = NULL;
		g_autofree gchar *status = NULL;
		guint i;
		venture_query_set_limit(query, 0);
		if (!venture_query_add_filter_int(query, "bill-id", VENTURE_FILTER_OP_EQ, record_id, error))
			return FALSE;
		allocations = venture_database_find(self->database, query, error);
		if (allocations == NULL)
			return FALSE;
		for (i = 0; i < allocations->len; i++)
		{
			VentureEntity *allocation = g_ptr_array_index(allocations, i);
			g_autoptr(VentureBillRefund) refund = NULL;
			g_autoptr(VentureEntity) payment = NULL;
			g_autoptr(VentureMoney) amount = NULL;
			gint64 payment_id = 0, vendor_id = 0;
			g_object_get(allocation, "payment-id", &payment_id, "amount", &amount, NULL);
			if (payment_id == 0)
				continue;
			payment = venture_database_get(self->database, VENTURE_TYPE_BILL_PAYMENT, payment_id, error);
			if (payment == NULL)
				return FALSE;
			g_object_get(payment, "vendor-id", &vendor_id, NULL);
			refund = venture_bill_refund_new();
			g_object_set(refund, "vendor-id", vendor_id, "allocation-id", venture_entity_get_id(allocation),
				"date", cutoff, "amount", amount, "reference", "Cutover rollback", NULL);
			venture_entity_set_organization_id(VENTURE_ENTITY(refund), org);
			if (!venture_database_save(self->database, VENTURE_ENTITY(refund), actor, error))
				return FALSE;
		}
		bill = venture_database_get(self->database, VENTURE_TYPE_VENDOR_BILL, record_id, error);
		if (bill == NULL)
			return FALSE;
		g_object_get(bill, "status", &status, NULL);
		if (g_strcmp0(status, "void") == 0)
			return TRUE;
		return venture_payables_service_transition(payables, VENTURE_VENDOR_BILL(bill), "void", cutoff, actor, error);
	}
	if (g_strcmp0(record_type, "vendor_credit") == 0 || g_strcmp0(record_type, "customer_credit") == 0)
	{
		if (!reverse_source_journals(self, record_type, record_id, org, cutoff, actor, error))
			return FALSE;
		return g_strcmp0(record_type, "vendor_credit") == 0
			? venture_payables_service_refresh_credit(venture_payables_service_get(self->database),
				record_id, actor, error)
			: venture_settlement_service_refresh_credit(venture_settlement_service_get(self->database),
				record_id, actor, error);
	}
	if (g_strcmp0(record_type, "fixed_asset") == 0)
	{
		g_autoptr(VentureEntity) asset = venture_database_get(self->database, VENTURE_TYPE_FIXED_ASSET, record_id, error);
		gint state;
		if (asset == NULL)
			return FALSE;
		g_object_get(asset, "status", &state, NULL);
		if (state != VENTURE_ASSET_STATUS_IN_SERVICE)
			return TRUE;
		return venture_asset_service_rollback_opening(venture_asset_service_get(self->database), record_id,
			cutoff, actor, error);
	}
	return TRUE;
}
