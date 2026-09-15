/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

#include <string.h>

struct _VentureSettlementService
{
	GObject parent_instance;
	/* The database owns this service. A retained service cannot keep its
	 * database alive forever, or dereference it after destruction. */
	VentureDatabase *database;
	VentureInvoiceStateMachine *machine;
	VentureEntity *writing;
	gboolean busy;
	gint64 cash_account;
	gint64 receivable_account;
	gint64 income_account;
	gint64 tax_account;
};

enum
{
	PROP_0,
	PROP_DATABASE,
	PROP_STATE_MACHINE,
	PROP_CASH_ACCOUNT,
	PROP_RECEIVABLE_ACCOUNT,
	PROP_INCOME_ACCOUNT,
	PROP_TAX_ACCOUNT,
	N_PROPERTIES
};

G_DEFINE_FINAL_TYPE(VentureSettlementService, venture_settlement_service, G_TYPE_OBJECT)

static gboolean check_invoice_edit(VentureSettlementService *self, VentureEntity *record,
	VentureEntity *previous, GError **error);

static gboolean
refuse(GError **error, VentureError code, const gchar *message)
{
	g_set_error(error, VENTURE_ERROR, code, "VentureSettlementService: %s", message);
	return FALSE;
}

static void
service_get_property(GObject *object, guint id, GValue *value, GParamSpec *spec)
{
	VentureSettlementService *self;

	self = VENTURE_SETTLEMENT_SERVICE(object);
	switch (id)
	{
	case PROP_DATABASE: g_value_set_object(value, self->database); break;
	case PROP_STATE_MACHINE: g_value_set_object(value, self->machine); break;
	case PROP_CASH_ACCOUNT: g_value_set_int64(value, self->cash_account); break;
	case PROP_RECEIVABLE_ACCOUNT: g_value_set_int64(value, self->receivable_account); break;
	case PROP_INCOME_ACCOUNT: g_value_set_int64(value, self->income_account); break;
	case PROP_TAX_ACCOUNT: g_value_set_int64(value, self->tax_account); break;
	default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
	}
}

static void
service_set_property(GObject *object, guint id, const GValue *value, GParamSpec *spec)
{
	VentureSettlementService *self;

	self = VENTURE_SETTLEMENT_SERVICE(object);
	switch (id)
	{
	case PROP_DATABASE:
		self->database = g_value_get_object(value);
		if (self->database != NULL)
			g_object_add_weak_pointer(G_OBJECT(self->database), (gpointer *)&self->database);
		break;
	case PROP_CASH_ACCOUNT: self->cash_account = g_value_get_int64(value); break;
	case PROP_RECEIVABLE_ACCOUNT: self->receivable_account = g_value_get_int64(value); break;
	case PROP_INCOME_ACCOUNT: self->income_account = g_value_get_int64(value); break;
	case PROP_TAX_ACCOUNT: self->tax_account = g_value_get_int64(value); break;
	default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
	}
}

static void
service_finalize(GObject *object)
{
	VentureSettlementService *self;

	self = VENTURE_SETTLEMENT_SERVICE(object);
	if (self->database != NULL)
		g_object_remove_weak_pointer(G_OBJECT(self->database), (gpointer *)&self->database);
	g_clear_object(&self->machine);
	G_OBJECT_CLASS(venture_settlement_service_parent_class)->finalize(object);
}

static void
venture_settlement_service_class_init(VentureSettlementServiceClass *klass)
{
	GObjectClass *object_class;

	object_class = G_OBJECT_CLASS(klass);
	object_class->get_property = service_get_property;
	object_class->set_property = service_set_property;
	object_class->finalize = service_finalize;
	g_object_class_install_property(object_class, PROP_DATABASE,
		g_param_spec_object("database", "Database", "Owning database",
			VENTURE_TYPE_DATABASE, G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
	g_object_class_install_property(object_class, PROP_STATE_MACHINE,
		g_param_spec_object("state-machine", "State machine", "Invoice lifecycle",
			VENTURE_TYPE_INVOICE_STATE_MACHINE, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));
	g_object_class_install_property(object_class, PROP_CASH_ACCOUNT,
		g_param_spec_int64("cash-account-id", "Cash account", "Zero resolves code 1000 in the organization",
			0, G_MAXINT64, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
	g_object_class_install_property(object_class, PROP_RECEIVABLE_ACCOUNT,
		g_param_spec_int64("receivable-account-id", "Receivable account", "Zero resolves code 1100; unused receipts are credit balances here",
			0, G_MAXINT64, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
	g_object_class_install_property(object_class, PROP_INCOME_ACCOUNT,
		g_param_spec_int64("income-account-id", "Income account", "Zero resolves code 4000 in the organization",
			0, G_MAXINT64, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
	g_object_class_install_property(object_class, PROP_TAX_ACCOUNT,
		g_param_spec_int64("tax-account-id", "Tax account", "Zero resolves code 2100 sales tax payable",
			0, G_MAXINT64, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
}

static void
venture_settlement_service_init(VentureSettlementService *self)
{
	self->machine = venture_invoice_state_machine_new();
}

VentureSettlementService *
venture_settlement_service_get(VentureDatabase *database)
{
	VentureSettlementService *self;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	self = g_object_get_data(G_OBJECT(database), "venture-settlement-service");
	if (self == NULL)
	{
		self = g_object_new(VENTURE_TYPE_SETTLEMENT_SERVICE, "database", database, NULL);
		g_object_set_data_full(G_OBJECT(database), "venture-settlement-service", self, g_object_unref);
	}
	return self;
}

VentureInvoiceStateMachine *
venture_settlement_service_get_state_machine(VentureSettlementService *self)
{
	g_return_val_if_fail(VENTURE_IS_SETTLEMENT_SERVICE(self), NULL);
	return self->machine;
}

static GPtrArray *
find_rows(VentureSettlementService *self, GType type, const gchar *field,
	gint64 id, GDateTime *cutoff, GError **error)
{
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureEntity) prototype = g_object_new(type, NULL);

	/* A never-enabled optional module has no tables. A disabled module
	 * with existing history must still count toward invoice balances. */
	if (G_TYPE_INVALID == venture_entity_registry_lookup(venture_entity_registry_get_default(),
		venture_entity_get_entity_name(prototype)))
	{
		g_autoptr(OrmInspector) inspector = orm_inspector_new(venture_database_get_connection(self->database), error);
		if (NULL == inspector)
			return NULL;
		if (!orm_inspector_has_table(inspector, venture_entity_get_table_name(prototype), NULL, error))
			return (error != NULL && *error != NULL) ? NULL : g_ptr_array_new_with_free_func(g_object_unref);
	}

	query = venture_query_new(type);
	venture_query_set_limit(query, 0);
	venture_query_set_include_deleted(query, TRUE);
	if (field != NULL && !venture_query_add_filter_int(query, field, VENTURE_FILTER_OP_EQ, id, error))
		return NULL;
	if (cutoff != NULL)
	{
		g_autofree gchar *text = NULL;

		text = g_date_time_format_iso8601(cutoff);
		if (!venture_query_add_filter_string(query, "date", VENTURE_FILTER_OP_LT, text, error))
			return NULL;
	}
	return venture_database_find(self->database, query, error);
}

static gint64
get_id(VentureEntity *record, const gchar *field)
{
	gint64 id;

	g_object_get(record, field, &id, NULL);
	return id;
}

static gboolean
accumulate(VentureMoney **total, const VentureMoney *amount, gboolean subtract, GError **error)
{
	VentureMoney *next;

	if (amount == NULL)
		return refuse(error, VENTURE_ERROR_VALIDATION, "An event has no amount");
	if (*total == NULL)
		*total = venture_money_new(0, venture_money_get_currency(amount), venture_money_get_exponent(amount));
	next = subtract ? venture_money_subtract(*total, amount, error) : venture_money_add(*total, amount, error);
	if (next == NULL)
		return FALSE;
	venture_money_free(*total);
	*total = next;
	return TRUE;
}

static gboolean
accumulate_record(VentureMoney **total, VentureEntity *record, gboolean subtract, GError **error)
{
	g_autoptr(VentureMoney) amount = NULL;

	g_object_get(record, "amount", &amount, NULL);
	return accumulate(total, amount, subtract, error);
}

static VentureEntity *
issue_event(VentureSettlementService *self, gint64 invoice_id, GDateTime *cutoff, GError **error)
{
	g_autoptr(GPtrArray) events = NULL;
	guint i;

	events = find_rows(self, VENTURE_TYPE_INVOICE_EVENT, "invoice-id", invoice_id, cutoff, error);
	if (events == NULL)
		return NULL;
	for (i = 0; i < events->len; i++)
	{
		VentureEntity *event;
		g_autofree gchar *state = NULL;

		event = g_ptr_array_index(events, i);
		g_object_get(event, "kind", &state, NULL);
		if (g_strcmp0(state, "issue") == 0)
			return g_object_ref(event);
	}
	return NULL;
}

VentureMoney *
venture_settlement_service_invoice_balance(VentureSettlementService *self,
	gint64 invoice_id, GDateTime *as_of, GError **error)
{
	g_autoptr(GPtrArray) events = NULL;
	g_autoptr(GPtrArray) allocations = NULL;
	g_autoptr(VentureMoney) total = NULL;
	gboolean issued;
	gboolean voided;
	guint i;

	events = find_rows(self, VENTURE_TYPE_INVOICE_EVENT, "invoice-id", invoice_id, as_of, error);
	if (events == NULL)
		return NULL;
	issued = FALSE;
	voided = FALSE;
	for (i = 0; i < events->len; i++)
	{
		VentureEntity *event;
		g_autofree gchar *state = NULL;

		event = g_ptr_array_index(events, i);
		g_object_get(event, "kind", &state, NULL);
		if (g_strcmp0(state, "issue") == 0)
		{
			issued = TRUE;
			if (!accumulate_record(&total, event, FALSE, error))
				return NULL;
		}
		if (g_strcmp0(state, "void") == 0)
			voided = TRUE;
	}
	if (!issued)
		return venture_money_new_zero(NULL);
	if (voided)
		return venture_money_new(0, venture_money_get_currency(total), venture_money_get_exponent(total));
	allocations = find_rows(self, VENTURE_TYPE_PAYMENT_ALLOCATION, "invoice-id", invoice_id, as_of, error);
	if (allocations == NULL)
		return NULL;
	for (i = 0; i < allocations->len; i++)
	{
		VentureEntity *allocation;
		g_autoptr(GPtrArray) refunds = NULL;
		guint j;

		allocation = g_ptr_array_index(allocations, i);
		if (!accumulate_record(&total, allocation, TRUE, error))
			return NULL;
		refunds = find_rows(self, VENTURE_TYPE_REFUND, "allocation-id", venture_entity_get_id(allocation), as_of, error);
		if (refunds == NULL)
			return NULL;
		for (j = 0; j < refunds->len; j++)
			if (!accumulate_record(&total, g_ptr_array_index(refunds, j), FALSE, error))
				return NULL;
	}
	return g_steal_pointer(&total);
}

static gboolean
same_owner(VentureEntity *a, VentureEntity *b, GError **error)
{
	if (venture_entity_get_organization_id(a) == 0 ||
		venture_entity_get_organization_id(a) != venture_entity_get_organization_id(b))
		return refuse(error, VENTURE_ERROR_VALIDATION, "Records must belong to the same organization");
	return TRUE;
}

static gboolean
check_customer(VentureSettlementService *self, VentureEntity *record, GError **error)
{
	g_autoptr(VentureEntity) customer = NULL;

	customer = venture_database_get(self->database, VENTURE_TYPE_COMPANY, get_id(record, "customer-id"), error);
	return customer != NULL && same_owner(record, customer, error);
}

static gboolean
check_date(GDateTime *date, GDateTime *earliest, GError **error)
{
	g_autoptr(GDateTime) now = NULL;

	now = venture_time_now();
	if (date == NULL || (earliest != NULL && g_date_time_compare(date, earliest) < 0))
		return refuse(error, VENTURE_ERROR_VALIDATION, "The date must not precede the source event");
	if (g_date_time_compare(date, now) > 0)
		return refuse(error, VENTURE_ERROR_VALIDATION, "A financial event cannot be dated in the future");
	return TRUE;
}

static gboolean
check_amount(const VentureMoney *amount, GError **error)
{
	if (amount == NULL || venture_money_get_amount(amount) <= 0)
		return refuse(error, VENTURE_ERROR_VALIDATION, "The amount must be positive");
	return TRUE;
}

static gboolean
check_rows_not_later(GPtrArray *records, GDateTime *date, GError **error)
{
	guint i;

	for (i = 0; i < records->len; i++)
	{
		g_autoptr(GDateTime) event_date = NULL;

		g_object_get(g_ptr_array_index(records, i), "date", &event_date, NULL);
		if (event_date != NULL && g_date_time_compare(event_date, date) > 0)
			return refuse(error, VENTURE_ERROR_VALIDATION, "A settlement cannot precede a later event on the same invoice");
	}
	return TRUE;
}

static gboolean
check_invoice_chronology(VentureSettlementService *self, gint64 invoice_id, GDateTime *date, GError **error)
{
	g_autoptr(GPtrArray) events = NULL;
	g_autoptr(GPtrArray) allocations = NULL;
	guint i;

	events = find_rows(self, VENTURE_TYPE_INVOICE_EVENT, "invoice-id", invoice_id, NULL, error);
	if (events == NULL || !check_rows_not_later(events, date, error))
		return FALSE;
	allocations = find_rows(self, VENTURE_TYPE_PAYMENT_ALLOCATION, "invoice-id", invoice_id, NULL, error);
	if (allocations == NULL || !check_rows_not_later(allocations, date, error))
		return FALSE;
	for (i = 0; i < allocations->len; i++)
	{
		g_autoptr(GPtrArray) refunds = NULL;

		refunds = find_rows(self, VENTURE_TYPE_REFUND, "allocation-id",
			venture_entity_get_id(g_ptr_array_index(allocations, i)), NULL, error);
		if (refunds == NULL || !check_rows_not_later(refunds, date, error))
			return FALSE;
	}
	return TRUE;
}

static gboolean
within(const VentureMoney *amount, const VentureMoney *available, GError **error)
{
	g_autoptr(VentureMoney) left = NULL;

	if (!check_amount(amount, error))
		return FALSE;
	left = venture_money_subtract(available, amount, error);
	if (left == NULL)
		return FALSE;
	if (venture_money_get_amount(left) < 0)
		return refuse(error, VENTURE_ERROR_VALIDATION, "The amount exceeds the available balance");
	return TRUE;
}

static gboolean
write_record(VentureSettlementService *self, VentureEntity *record,
	const VentureActor *actor, GError **error)
{
	gboolean ok;

	/* A one-use permit for this exact object. A signal handler saving a
	 * different record during the transaction cannot borrow this authority. */
	self->writing = record;
	ok = venture_database_save(self->database, record, actor, error);
	self->writing = NULL;
	return ok;
}

gboolean
venture_receivables_is_projection_write(VentureDatabase *database, VentureEntity *record)
{
	VentureSettlementService *self = g_object_get_data(G_OBJECT(database), "venture-settlement-service");

	/* Only the current service-owned object may bypass source posting. The
	 * database hook consumes the permit before validators or signals run. */
	return VENTURE_IS_SALE(record) && self != NULL && self->busy && self->writing == record;
}

gboolean
venture_receivables_check_sale(VentureDatabase *database, VentureEntity *record, GError **error)
{
	g_autoptr(GPtrArray) allocations = NULL;

	if (!venture_entity_is_persisted(record))
		return TRUE;
	allocations = find_rows(venture_settlement_service_get(database), VENTURE_TYPE_PAYMENT_ALLOCATION,
		"sale-id", venture_entity_get_id(record), NULL, error);
	if (NULL == allocations)
		return FALSE;
	if (allocations->len != 0)
		return refuse(error, VENTURE_ERROR_PERMISSION_DENIED, "Settlement sales are derived; change them through a refund");
	g_clear_pointer(&allocations, g_ptr_array_unref);
	allocations = find_rows(venture_settlement_service_get(database), VENTURE_TYPE_REFUND,
		"sale-id", venture_entity_get_id(record), NULL, error);
	if (allocations == NULL)
		return FALSE;
	if (allocations->len != 0)
		return refuse(error, VENTURE_ERROR_PERMISSION_DENIED, "Refund cash adjustments are immutable");
	return TRUE;
}

static gint64
account_id(VentureSettlementService *self, gint64 configured, const gchar *code,
	VentureAccountKind kind, gint64 organization_id, GError **error)
{
	g_autoptr(VentureEntity) account = NULL;
	g_autoptr(VentureQuery) query = NULL;
	gboolean active;
	gint actual_kind;

	if (configured != 0)
		account = venture_database_get(self->database, VENTURE_TYPE_ACCOUNT, configured, error);
	else
	{
		query = venture_query_new(VENTURE_TYPE_ACCOUNT);
		venture_query_set_organization(query, organization_id);
		if (!venture_query_add_filter_string(query, "code", VENTURE_FILTER_OP_EQ, code, error))
			return 0;
		account = venture_database_find_one(self->database, query, error);
	}
	if (account == NULL)
	{
		if (error == NULL || *error == NULL)
			refuse(error, VENTURE_ERROR_CONFIG, "Configure the cash, receivable and income accounts for this organization");
		return 0;
	}
	g_object_get(account, "active", &active, "kind", &actual_kind, NULL);
	if (!active || actual_kind != (gint)kind || venture_entity_get_organization_id(account) != organization_id)
	{
		refuse(error, VENTURE_ERROR_VALIDATION, "The posting account must be active, of the right class, and in the same organization");
		return 0;
	}
	return venture_entity_get_id(account);
}

/**
 * venture_receivables_post_batch:
 * @self: the settlement service
 * @entries: (element-type VentureLedgerEntry): the balanced source batch
 * @actor: (nullable): the actor
 * @error: (out) (optional): the error
 *
 * The SINGLE posting seam for receivables. Re-point this one call at the
 * journal posting service when it lands; no other receivables code calls
 * venture_database_save_ledger_transaction(). The caller already owns the
 * encompassing settlement transaction.
 * Returns: TRUE if the balanced batch was saved
 */
static gboolean
venture_receivables_post_batch(VentureSettlementService *self, GPtrArray *entries,
	const VentureActor *actor, GError **error)
{
	return venture_posting_service_post_entries(venture_database_get_posting_service(self->database),
		entries, NULL, actor, error);
}

/* These are source-document legs, not a second posting engine. Unapplied
 * cash is a credit balance in AR. Allocation transfers between the unused
 * credit and the invoice within that control account, with zero net GL. */
static gboolean
resolve_code(VentureSettlementService *self, const gchar *code, gint64 organization_id,
	gint64 *account, GError **error)
{
	gint64 configured;
	VentureAccountKind kind;
	if (g_str_equal(code, "1000"))
	{
		configured = self->cash_account;
		kind = VENTURE_ACCOUNT_KIND_ASSET;
	}
	else if (g_str_equal(code, "1100"))
	{
		configured = self->receivable_account;
		kind = VENTURE_ACCOUNT_KIND_ASSET;
	}
	else if (g_str_equal(code, "2100"))
	{
		configured = self->tax_account;
		kind = VENTURE_ACCOUNT_KIND_LIABILITY;
	}
	else
	{
		configured = self->income_account;
		kind = VENTURE_ACCOUNT_KIND_INCOME;
	}
	*account = account_id(self, configured, code, kind, organization_id, error);
	return *account != 0;
}

static gboolean
add_leg(GPtrArray *entries, VentureEntity *source, GDateTime *date, const gchar *transaction,
	gint64 account, VentureLedgerSide side, const VentureMoney *amount)
{
	VentureLedgerEntry *entry;
	if (amount == NULL || venture_money_is_zero(amount))
		return TRUE;
	entry = venture_ledger_entry_new();
	g_object_set(entry, "transaction-id", transaction, "account-id", account,
		"side", side, "amount", amount, "occurred-at", date,
		"source-type", venture_entity_get_entity_name(source), "source-id", venture_entity_get_id(source), NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(entry), venture_entity_get_organization_id(source));
	g_ptr_array_add(entries, entry);
	return TRUE;
}

static gboolean
post_split(VentureSettlementService *self, VentureEntity *source, GDateTime *date,
	const VentureMoney *debit_ar, const VentureMoney *credit_income, const VentureMoney *credit_tax,
	gboolean reverse, const VentureActor *actor, GError **error)
{
	g_autoptr(GPtrArray) entries = g_ptr_array_new_with_free_func(g_object_unref);
	g_autofree gchar *transaction = NULL;
	gint64 ar = 0, income = 0, tax = 0;
	gint64 org = venture_entity_get_organization_id(source);
	if (debit_ar != NULL && !venture_money_is_zero(debit_ar) && !check_amount(debit_ar, error))
		return FALSE;
	if (!resolve_code(self, "1100", org, &ar, error) ||
		!resolve_code(self, "4000", org, &income, error))
		return FALSE;
	if (credit_tax != NULL && !venture_money_is_zero(credit_tax) &&
		!resolve_code(self, "2100", org, &tax, error))
		return FALSE;
	transaction = g_strdup_printf("receivables:%s:%s", venture_entity_get_entity_name(source),
		venture_entity_get_uuid(source));
	if (reverse)
	{
		add_leg(entries, source, date, transaction, income, VENTURE_LEDGER_SIDE_DEBIT, credit_income);
		add_leg(entries, source, date, transaction, tax, VENTURE_LEDGER_SIDE_DEBIT, credit_tax);
		add_leg(entries, source, date, transaction, ar, VENTURE_LEDGER_SIDE_CREDIT, debit_ar);
	}
	else
	{
		add_leg(entries, source, date, transaction, ar, VENTURE_LEDGER_SIDE_DEBIT, debit_ar);
		add_leg(entries, source, date, transaction, income, VENTURE_LEDGER_SIDE_CREDIT, credit_income);
		add_leg(entries, source, date, transaction, tax, VENTURE_LEDGER_SIDE_CREDIT, credit_tax);
	}
	return entries->len == 0 || venture_receivables_post_batch(self, entries, actor, error);
}

static gboolean
post(VentureSettlementService *self, VentureEntity *source, GDateTime *date,
	const VentureMoney *amount, const gchar *debit_code, const gchar *credit_code,
	const VentureActor *actor, GError **error)
{
	g_autoptr(GPtrArray) entries = NULL;
	g_autofree gchar *transaction = NULL;
	gint64 debit = 0, credit = 0;
	if (!check_amount(amount, error))
		return FALSE;
	if (!resolve_code(self, debit_code, venture_entity_get_organization_id(source), &debit, error) ||
		!resolve_code(self, credit_code, venture_entity_get_organization_id(source), &credit, error))
		return FALSE;
	entries = g_ptr_array_new_with_free_func(g_object_unref);
	transaction = g_strdup_printf("receivables:%s:%s", venture_entity_get_entity_name(source), venture_entity_get_uuid(source));
	add_leg(entries, source, date, transaction, debit, VENTURE_LEDGER_SIDE_DEBIT, amount);
	add_leg(entries, source, date, transaction, credit, VENTURE_LEDGER_SIDE_CREDIT, amount);
	return venture_receivables_post_batch(self, entries, actor, error);
}

static gboolean
begin_operation(VentureSettlementService *self, const gchar *type, GError **error)
{
	if (self->database == NULL)
		return refuse(error, VENTURE_ERROR_DATABASE, "The database has been closed");
	if (self->busy)
		return refuse(error, VENTURE_ERROR_CONFLICT, "A settlement is already in progress");
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), type) == G_TYPE_INVALID)
		return refuse(error, VENTURE_ERROR_CONFIG, "The receivables module is disabled (modules.receivables.enabled)");
	if (!venture_database_begin(self->database, error))
		return FALSE;
	self->busy = TRUE;
	return TRUE;
}

static gboolean
finish_operation(VentureSettlementService *self, gboolean ok, GError **error)
{
	if (ok)
		ok = venture_database_commit(self->database, error);
	else
		venture_database_rollback(self->database);
	self->busy = FALSE;
	return ok;
}

static VentureEntity *
snapshot(VentureEntity *record)
{
	VentureEntity *copy;

	copy = g_object_new(G_OBJECT_TYPE(record), NULL);
	venture_entity_copy_properties_from(copy, record, FALSE);
	return copy;
}

static gchar *
invoice_state(VentureEntity *invoice)
{
	gchar *state;
	gint status;

	g_object_get(invoice, "workflow-state", &state, "status", &status, NULL);
	if (state == NULL || *state == '\0')
	{
		g_free(state);
		state = g_strdup(venture_enum_to_nick(VENTURE_TYPE_INVOICE_STATUS, status));
	}
	return state;
}

static gboolean
derive_invoice(VentureSettlementService *self, VentureEntity *invoice, GDateTime *date,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) issued = NULL;
	g_autoptr(VentureMoney) total = NULL;
	g_autoptr(VentureMoney) balance = NULL;
	g_autofree gchar *from = NULL;
	const gchar *to;
	VentureInvoiceStatus status;

	issued = issue_event(self, venture_entity_get_id(invoice), NULL, error);
	if (issued == NULL)
		return refuse(error, VENTURE_ERROR_VALIDATION, "Issue the invoice before allocating a payment");
	g_object_get(issued, "amount", &total, NULL);
	balance = venture_settlement_service_invoice_balance(self, venture_entity_get_id(invoice), NULL, error);
	if (balance == NULL)
		return FALSE;
	status = venture_money_is_zero(balance) ? VENTURE_INVOICE_STATUS_PAID :
		(venture_money_equal(total, balance) ? VENTURE_INVOICE_STATUS_SENT : VENTURE_INVOICE_STATUS_PARTIALLY_PAID);
	to = venture_enum_to_nick(VENTURE_TYPE_INVOICE_STATUS, status);
	from = invoice_state(invoice);
	if (g_strcmp0(from, to) != 0 && !venture_invoice_state_machine_check(self->machine, VENTURE_INVOICE(invoice), from, to, error))
		return FALSE;
	g_object_set(invoice, "status", status, "workflow-state", to,
		"paid-at", status == VENTURE_INVOICE_STATUS_PAID ? date : NULL, NULL);
	return write_record(self, invoice, actor, error);
}

static gboolean
invoice_parts(VentureSettlementService *self, VentureEntity *invoice,
	VentureMoney **total, VentureMoney **net, VentureMoney **tax, VentureMoney **discount,
	VentureMoney **shipping, const VentureActor *actor, GError **error)
{
	g_autoptr(GPtrArray) lines = NULL;
	g_autoptr(VentureMoney) ship = NULL;
	const gchar *currency = NULL;
	guint i;
	lines = find_rows(self, VENTURE_TYPE_INVOICE_LINE, "invoice-id", venture_entity_get_id(invoice), NULL, error);
	if (lines == NULL)
		return FALSE;
	g_object_get(invoice, "shipping-amount", &ship, NULL);
	for (i = 0; i < lines->len; i++)
	{
		VentureEntity *line = g_ptr_array_index(lines, i);
		g_autoptr(VentureMoney) unit = NULL;
		g_autoptr(VentureMoney) subtotal = NULL;
		g_autoptr(VentureMoney) line_discount = NULL;
		g_autoptr(VentureMoney) line_net = NULL;
		g_autoptr(VentureMoney) line_tax = NULL;
		g_autoptr(VentureMoney) line_total = NULL;
		gdouble quantity;
		gint64 thousandths, discount_percent, tax_percent;
		if (venture_entity_is_deleted(line))
			continue;
		if (!same_owner(invoice, line, error))
			return FALSE;
		g_object_get(line, "quantity", &quantity, "unit-price", &unit,
			"discount-percent", &discount_percent, "tax-percent", &tax_percent, NULL);
		if (unit == NULL)
			return refuse(error, VENTURE_ERROR_VALIDATION, "The line has no unit price");
		currency = venture_money_get_currency(unit);
		thousandths = (gint64)(quantity * 1000.0 + ((quantity >= 0.0) ? 0.5 : -0.5));
		subtotal = venture_money_multiply_rational(unit, thousandths, 1000, error);
		if (subtotal == NULL ||
			!venture_quote_percentage_parts(subtotal, discount_percent, tax_percent,
				&line_discount, &line_net, &line_tax, &line_total, error))
			return FALSE;
		if (!accumulate(total, line_total, FALSE, error) ||
			!accumulate(net, line_net, FALSE, error) ||
			!accumulate(tax, line_tax, FALSE, error) ||
			!accumulate(discount, line_discount, FALSE, error))
			return FALSE;
		{
			g_autoptr(VentureMoney) zero_ship = venture_money_new_zero(currency);
			g_object_set(line, "income-amount", line_net, "discount-amount", line_discount,
				"tax-amount", line_tax, "shipping-amount", zero_ship, NULL);
		}
		if (!write_record(self, line, actor, error))
			return FALSE;
	}
	if (ship != NULL && !venture_money_is_zero(ship))
	{
		if (!accumulate(total, ship, FALSE, error) || !accumulate(net, ship, FALSE, error) ||
			!accumulate(shipping, ship, FALSE, error))
			return FALSE;
	}
	if (*total == NULL)
		return refuse(error, VENTURE_ERROR_VALIDATION, "The invoice has no amount");
	if (*net == NULL)
		*net = venture_money_new_zero(venture_money_get_currency(*total));
	if (*tax == NULL)
		*tax = venture_money_new_zero(venture_money_get_currency(*total));
	if (*discount == NULL)
		*discount = venture_money_new_zero(venture_money_get_currency(*total));
	if (*shipping == NULL)
		*shipping = ship != NULL ? venture_money_copy(ship) : venture_money_new_zero(venture_money_get_currency(*total));
	return check_amount(*total, error);
}

static gboolean
perform_transition(VentureSettlementService *self, VentureEntity *invoice,
	const gchar *state, GDateTime *date, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) previous = NULL;
	g_autoptr(VentureEntity) issued = NULL;
	g_autoptr(VentureInvoiceEvent) event = NULL;
	g_autoptr(VentureMoney) total = NULL;
	g_autoptr(GDateTime) issue_date = NULL;
	g_autoptr(GDateTime) due = NULL;
	g_autofree gchar *from = NULL;
	VentureInvoiceStatus phase;
	VentureInvoiceStatus old_phase;
	gboolean first_issue;

	previous = venture_database_get(self->database, VENTURE_TYPE_INVOICE, venture_entity_get_id(invoice), error);
	if (previous == NULL)
		return FALSE;
	if (venture_entity_get_version(previous) != venture_entity_get_version(invoice))
		return refuse(error, VENTURE_ERROR_CONFLICT, "The invoice changed; read it again");
	if (!check_invoice_edit(self, invoice, previous, error))
		return FALSE;
	if (!venture_invoice_state_machine_get_phase(self->machine, state, &phase, error))
		return FALSE;
	if (phase == VENTURE_INVOICE_STATUS_PAID || phase == VENTURE_INVOICE_STATUS_PARTIALLY_PAID)
		return refuse(error, VENTURE_ERROR_VALIDATION, "Financial status is derived; create a payment or allocation through this service");
	from = invoice_state(previous);
	if (!venture_invoice_state_machine_get_phase(self->machine, from, &old_phase, error))
		return FALSE;
	if (old_phase == VENTURE_INVOICE_STATUS_PAID || old_phase == VENTURE_INVOICE_STATUS_PARTIALLY_PAID)
		return refuse(error, VENTURE_ERROR_VALIDATION, "Refund allocations through VentureSettlementService to reopen a settled invoice");
	if (old_phase != VENTURE_INVOICE_STATUS_DRAFT && phase == VENTURE_INVOICE_STATUS_DRAFT)
		return refuse(error, VENTURE_ERROR_VALIDATION, "An issued invoice cannot become a draft");
	issued = issue_event(self, venture_entity_get_id(invoice), NULL, error);
	if (error != NULL && *error != NULL)
		return FALSE;
	if (issued != NULL)
		g_object_get(issued, "date", &issue_date, "amount", &total, NULL);
	if (!check_date(date, issue_date, error) ||
		!check_invoice_chronology(self, venture_entity_get_id(invoice), date, error) ||
		!venture_invoice_state_machine_check(self->machine, VENTURE_INVOICE(invoice), from, state, error))
		return FALSE;
	first_issue = issued == NULL && phase == VENTURE_INVOICE_STATUS_SENT;
	{
		g_autoptr(VentureMoney) net = NULL;
		g_autoptr(VentureMoney) tax = NULL;
		g_autoptr(VentureMoney) discount = NULL;
		g_autoptr(VentureMoney) shipping = NULL;
		if (first_issue)
		{
			if (!invoice_parts(self, invoice, &total, &net, &tax, &discount, &shipping, actor, error))
				return FALSE;
		}
		else if (issued != NULL)
		{
			g_object_get(issued, "net-amount", &net, "tax-amount", &tax,
				"discount-amount", &discount, "shipping-amount", &shipping, NULL);
			if (net == NULL && total != NULL)
				net = venture_money_copy(total);
		}
		if (phase == VENTURE_INVOICE_STATUS_VOID && total != NULL)
		{
			g_autoptr(VentureMoney) balance = NULL;
			balance = venture_settlement_service_invoice_balance(self, venture_entity_get_id(invoice), NULL, error);
			if (balance == NULL)
				return FALSE;
			if (!venture_money_equal(balance, total))
				return refuse(error, VENTURE_ERROR_VALIDATION, "Refund allocated money before voiding the invoice");
		}
		if (total == NULL)
			total = venture_money_new_zero(NULL);
		g_object_get(invoice, "due-at", &due, NULL);
		event = venture_invoice_event_new();
		g_object_set(event, "invoice-id", venture_entity_get_id(invoice),
			"customer-id", get_id(invoice, "company-id"), "date", date,
			"kind", first_issue ? "issue" : (phase == VENTURE_INVOICE_STATUS_VOID ? "void" : "transition"),
			"state", first_issue ? "sent" : state, "due-at", due, "amount", total,
			"net-amount", net, "tax-amount", tax, "discount-amount", discount, "shipping-amount", shipping,
			"venture-id", get_id(invoice, "venture-id"), NULL);
		venture_entity_set_organization_id(VENTURE_ENTITY(event), venture_entity_get_organization_id(invoice));
		if (!check_customer(self, VENTURE_ENTITY(event), error) ||
			!write_record(self, VENTURE_ENTITY(event), actor, error))
			return FALSE;
		if (first_issue && !post_split(self, VENTURE_ENTITY(event), date, total, net, tax, FALSE, actor, error))
			return FALSE;
		if (phase == VENTURE_INVOICE_STATUS_VOID && !venture_money_is_zero(total) &&
			!post_split(self, VENTURE_ENTITY(event), date, total, net, tax, TRUE, actor, error))
			return FALSE;
	}
	g_object_set(invoice, "status", phase, "workflow-state", state, "paid-at", NULL, NULL);
	if (first_issue)
		g_object_set(invoice, "issued-at", date, NULL);
	return write_record(self, invoice, actor, error);
}

gboolean
venture_settlement_service_transition(VentureSettlementService *self,
	VentureInvoice *invoice, const gchar *state, GDateTime *date,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) original = NULL;
	gboolean ok;

	if (!begin_operation(self, "invoice", error))
		return FALSE;
	original = snapshot(VENTURE_ENTITY(invoice));
	ok = perform_transition(self, VENTURE_ENTITY(invoice), state, date, actor, error);
	ok = finish_operation(self, ok, error);
	if (!ok)
		venture_entity_copy_properties_from(VENTURE_ENTITY(invoice), original, FALSE);
	return ok;
}

static VentureEntity *
payment_credit(VentureSettlementService *self, gint64 payment_id, GError **error)
{
	g_autoptr(GPtrArray) credits = NULL;

	credits = find_rows(self, VENTURE_TYPE_CUSTOMER_CREDIT, "payment-id", payment_id, NULL, error);
	if (credits == NULL)
		return NULL;
	if (credits->len != 1)
	{
		refuse(error, VENTURE_ERROR_VALIDATION, "The receipt has no unique source credit");
		return NULL;
	}
	return g_object_ref(g_ptr_array_index(credits, 0));
}

static VentureMoney *
credit_remaining(VentureSettlementService *self, VentureEntity *credit,
	GDateTime *cutoff, GError **error)
{
	g_autoptr(VentureMoney) remaining = NULL;
	g_autoptr(GPtrArray) allocations = NULL;
	g_autoptr(GPtrArray) refunds = NULL;
	gint64 payment_id;
	guint i;

	g_object_get(credit, "amount", &remaining, NULL);
	allocations = find_rows(self, VENTURE_TYPE_PAYMENT_ALLOCATION, "credit-id", venture_entity_get_id(credit), cutoff, error);
	if (allocations == NULL)
		return NULL;
	for (i = 0; i < allocations->len; i++)
		if (!accumulate_record(&remaining, g_ptr_array_index(allocations, i), TRUE, error))
			return NULL;
	payment_id = get_id(credit, "payment-id");
	if (payment_id != 0)
	{
		g_clear_pointer(&allocations, g_ptr_array_unref);
		allocations = find_rows(self, VENTURE_TYPE_PAYMENT_ALLOCATION, "payment-id", payment_id, cutoff, error);
		if (allocations == NULL)
			return NULL;
		for (i = 0; i < allocations->len; i++)
			if (!accumulate_record(&remaining, g_ptr_array_index(allocations, i), TRUE, error))
				return NULL;
	}
	refunds = find_rows(self, VENTURE_TYPE_REFUND, "credit-id", venture_entity_get_id(credit), cutoff, error);
	if (refunds == NULL)
		return NULL;
	for (i = 0; i < refunds->len; i++)
		if (!accumulate_record(&remaining, g_ptr_array_index(refunds, i), TRUE, error))
			return NULL;
	return g_steal_pointer(&remaining);
}

static gboolean
update_credit(VentureSettlementService *self, VentureEntity *credit,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureMoney) remaining = NULL;

	remaining = credit_remaining(self, credit, NULL, error);
	if (remaining == NULL)
		return FALSE;
	g_object_set(credit, "remaining", remaining, NULL);
	return write_record(self, credit, actor, error);
}

static gboolean
perform_allocation(VentureSettlementService *self, VentureEntity *allocation,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) credit = NULL;
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(VentureEntity) issued = NULL;
	g_autoptr(VentureMoney) remaining = NULL;
	g_autoptr(VentureMoney) balance = NULL;
	g_autoptr(VentureMoney) amount = NULL;
	g_autoptr(GDateTime) date = NULL;
	g_autoptr(GDateTime) source_date = NULL;
	g_autoptr(GDateTime) issue_date = NULL;
	gint64 payment_id;
	gint64 credit_id;
	gint status;

	if (venture_entity_is_persisted(allocation))
		return refuse(error, VENTURE_ERROR_VALIDATION, "Allocations are immutable; record a refund");
	payment_id = get_id(allocation, "payment-id");
	credit_id = get_id(allocation, "credit-id");
	if ((payment_id == 0) == (credit_id == 0))
		return refuse(error, VENTURE_ERROR_VALIDATION, "An allocation requires exactly one payment or credit");
	if (get_id(allocation, "sale-id") != 0)
		return refuse(error, VENTURE_ERROR_VALIDATION, "The sale is derived by the settlement service");
	credit = payment_id != 0 ? payment_credit(self, payment_id, error) :
		venture_database_get(self->database, VENTURE_TYPE_CUSTOMER_CREDIT, credit_id, error);
	if (credit == NULL)
		return FALSE;
	invoice = venture_database_get(self->database, VENTURE_TYPE_INVOICE, get_id(allocation, "invoice-id"), error);
	if (invoice == NULL || !same_owner(allocation, invoice, error) || !same_owner(allocation, credit, error))
		return FALSE;
	if (get_id(invoice, "company-id") != get_id(credit, "customer-id"))
		return refuse(error, VENTURE_ERROR_VALIDATION, "A payment or credit may only settle its own customer's invoices");
	g_object_get(invoice, "status", &status, NULL);
	if (status == VENTURE_INVOICE_STATUS_DRAFT || status == VENTURE_INVOICE_STATUS_VOID)
		return refuse(error, VENTURE_ERROR_VALIDATION, "Only an issued, non-void invoice accepts allocations");
	issued = issue_event(self, venture_entity_get_id(invoice), NULL, error);
	if (issued == NULL)
		return refuse(error, VENTURE_ERROR_VALIDATION, "The invoice has no issue event");
	g_object_get(allocation, "amount", &amount, "date", &date, NULL);
	g_object_get(credit, "date", &source_date, NULL);
	g_object_get(issued, "date", &issue_date, NULL);
	if (!check_date(date, source_date, error) || !check_date(date, issue_date, error))
		return FALSE;
	if (!check_invoice_chronology(self, venture_entity_get_id(invoice), date, error))
		return FALSE;
	remaining = credit_remaining(self, credit, NULL, error);
	if (remaining == NULL || !within(amount, remaining, error))
		return FALSE;
	balance = venture_settlement_service_invoice_balance(self, venture_entity_get_id(invoice), NULL, error);
	if (balance == NULL || !within(amount, balance, error))
		return FALSE;
	/* Legacy cash reports still read sale. Each applied cash amount creates
	 * one sale, including receipts applied from an earlier deposit. Credit
	 * notes have no cash proceeds and create no fictitious sale. */
	if (get_id(credit, "payment-id") != 0)
	{
		g_autoptr(VentureSale) sale = NULL;
		g_autofree gchar *external = g_strdup_printf("allocation:%s", venture_entity_get_uuid(allocation));

		sale = venture_sale_new();
		g_object_set(sale, "venture-id", get_id(invoice, "venture-id"), "gross", amount,
			"occurred-at", date, "channel", "invoice", "external-id", external, NULL);
		venture_entity_set_organization_id(VENTURE_ENTITY(sale), venture_entity_get_organization_id(invoice));
		if (!write_record(self, VENTURE_ENTITY(sale), actor, error))
			return FALSE;
		g_object_set(allocation, "sale-id", venture_entity_get_id(VENTURE_ENTITY(sale)), NULL);
	}
	return write_record(self, allocation, actor, error) &&
		post(self, allocation, date, amount, "1100", "1100", actor, error) &&
		update_credit(self, credit, actor, error) && derive_invoice(self, invoice, date, actor, error);
}

static gboolean
perform_credit(VentureSettlementService *self, VentureEntity *credit,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureMoney) amount = NULL;
	g_autoptr(GDateTime) date = NULL;
	g_autofree gchar *kind = NULL;

	if (venture_entity_is_persisted(credit))
		return refuse(error, VENTURE_ERROR_VALIDATION, "Credits and their derived remaining balances are immutable");
	g_object_get(credit, "amount", &amount, "date", &date, "kind", &kind, NULL);
	if (g_strcmp0(kind, "credit_note") != 0 || get_id(credit, "payment-id") != 0)
		return refuse(error, VENTURE_ERROR_VALIDATION, "Create a payment for a deposit or overpayment; only credit_note may be entered directly");
	if (!check_amount(amount, error) || !check_date(date, NULL, error) || !check_customer(self, credit, error))
		return FALSE;
	g_object_set(credit, "remaining", amount, NULL);
	{
		g_autoptr(VentureMoney) tax = NULL;
		g_autoptr(VentureMoney) net = NULL;
		g_object_get(credit, "tax-amount", &tax, NULL);
		if (tax != NULL && !venture_money_is_zero(tax))
		{
			net = venture_money_subtract(amount, tax, error);
			if (net == NULL)
				return FALSE;
			return write_record(self, credit, actor, error) &&
				post_split(self, credit, date, amount, net, tax, TRUE, actor, error);
		}
	}
	return write_record(self, credit, actor, error) && post(self, credit, date, amount, "4000", "1100", actor, error);
}

static gboolean
perform_payment(VentureSettlementService *self, VentureEntity *payment,
	GPtrArray *allocations, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureCustomerCredit) credit = NULL;
	g_autoptr(VentureMoney) amount = NULL;
	g_autoptr(GDateTime) date = NULL;
	g_autofree gchar *external = NULL;
	g_autofree gchar *key = NULL;
	gint64 invoice_id;
	guint i;

	if (venture_entity_is_persisted(payment))
		return refuse(error, VENTURE_ERROR_VALIDATION, "Payments are immutable; allocate unused funds or record a refund");
	g_object_get(payment, "amount", &amount, "date", &date, "external-id", &external, NULL);
	if (!check_amount(amount, error) || !check_date(date, NULL, error) || !check_customer(self, payment, error))
		return FALSE;
	if (external != NULL && *external != '\0')
	{
		g_autoptr(VentureQuery) query = NULL;
		gint64 count;

		query = venture_query_new(VENTURE_TYPE_PAYMENT);
		venture_query_set_include_deleted(query, TRUE);
		venture_query_set_organization(query, venture_entity_get_organization_id(payment));
		if (!venture_query_add_filter_string(query, "external-id", VENTURE_FILTER_OP_EQ, external, error))
			return FALSE;
		count = venture_database_count(self->database, query, error);
		if (count < 0)
			return FALSE;
		if (count > 0)
			return refuse(error, VENTURE_ERROR_ALREADY_EXISTS, "That external ID already exists in this organization");
		key = g_strdup_printf("%" G_GINT64_FORMAT ":%s", venture_entity_get_organization_id(payment), external);
	}
	g_object_set(payment, "external-key", key, NULL);
	if (!write_record(self, payment, actor, error))
		return FALSE;
	credit = venture_customer_credit_new();
	invoice_id = get_id(payment, "invoice-id");
	g_object_set(credit, "customer-id", get_id(payment, "customer-id"), "date", date,
		"amount", amount, "remaining", amount, "payment-id", venture_entity_get_id(payment),
		"kind", invoice_id != 0 || (allocations != NULL && allocations->len > 0) ? "overpayment" : "deposit", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(credit), venture_entity_get_organization_id(payment));
	if (!write_record(self, VENTURE_ENTITY(credit), actor, error) ||
		!post(self, payment, date, amount, "1000", "1100", actor, error) ||
		!post(self, VENTURE_ENTITY(credit), date, amount, "1100", "1100", actor, error))
		return FALSE;
	if (allocations != NULL)
	{
		if (invoice_id != 0)
			return refuse(error, VENTURE_ERROR_VALIDATION, "Supply invoice-id or an allocation batch, not both");
		for (i = 0; i < allocations->len; i++)
		{
			VentureEntity *allocation;

			allocation = g_ptr_array_index(allocations, i);
			if (!VENTURE_IS_PAYMENT_ALLOCATION(allocation) || get_id(allocation, "payment-id") != 0 || get_id(allocation, "credit-id") != 0)
				return refuse(error, VENTURE_ERROR_VALIDATION, "A receipt batch requires new allocations without an existing source");
			g_object_set(allocation, "payment-id", venture_entity_get_id(payment), "date", date, NULL);
			venture_entity_set_organization_id(allocation, venture_entity_get_organization_id(payment));
			if (!perform_allocation(self, allocation, actor, error))
				return FALSE;
		}
	}
	else if (invoice_id != 0)
	{
		g_autoptr(VenturePaymentAllocation) allocation = NULL;
		g_autoptr(VentureMoney) balance = NULL;
		g_autoptr(VentureMoney) difference = NULL;
		const VentureMoney *applied;

		balance = venture_settlement_service_invoice_balance(self, invoice_id, NULL, error);
		if (balance == NULL || !check_amount(balance, error))
			return FALSE;
		difference = venture_money_subtract(amount, balance, error);
		if (difference == NULL)
			return FALSE;
		applied = venture_money_get_amount(difference) < 0 ? amount : balance;
		allocation = venture_payment_allocation_new();
		g_object_set(allocation, "payment-id", venture_entity_get_id(payment), "invoice-id", invoice_id,
			"amount", applied, "date", date, NULL);
		venture_entity_set_organization_id(VENTURE_ENTITY(allocation), venture_entity_get_organization_id(payment));
		if (!perform_allocation(self, VENTURE_ENTITY(allocation), actor, error))
			return FALSE;
	}
	return TRUE;
}

gboolean
venture_settlement_service_apply_payment(VentureSettlementService *self,
	VenturePayment *payment, GPtrArray *allocations, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) original = NULL;
	g_autoptr(GPtrArray) originals = NULL;
	gboolean ok;
	guint i;

	if (!begin_operation(self, "payment", error))
		return FALSE;
	original = snapshot(VENTURE_ENTITY(payment));
	originals = g_ptr_array_new_with_free_func(g_object_unref);
	if (allocations != NULL)
		for (i = 0; i < allocations->len; i++)
		{
			if (!VENTURE_IS_PAYMENT_ALLOCATION(g_ptr_array_index(allocations, i)))
			{
				refuse(error, VENTURE_ERROR_VALIDATION, "The batch may only contain payment allocations");
				return finish_operation(self, FALSE, error);
			}
			g_ptr_array_add(originals, snapshot(g_ptr_array_index(allocations, i)));
		}
	ok = perform_payment(self, VENTURE_ENTITY(payment), allocations, actor, error);
	ok = finish_operation(self, ok, error);
	if (!ok)
	{
		venture_entity_copy_properties_from(VENTURE_ENTITY(payment), original, FALSE);
		for (i = 0; i < originals->len; i++)
			venture_entity_copy_properties_from(g_ptr_array_index(allocations, i), g_ptr_array_index(originals, i), FALSE);
	}
	return ok;
}

static gboolean
perform_refund(VentureSettlementService *self, VentureEntity *refund,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) allocation = NULL;
	g_autoptr(VentureEntity) credit = NULL;
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(VentureMoney) available = NULL;
	g_autoptr(VentureMoney) amount = NULL;
	g_autoptr(GDateTime) date = NULL;
	g_autoptr(GDateTime) source_date = NULL;
	gint64 allocation_id;
	gint64 credit_id;

	if (get_id(refund, "sale-id") != 0)
		return refuse(error, VENTURE_ERROR_VALIDATION, "The refund cash adjustment is derived");
	if (venture_entity_is_persisted(refund))
		return refuse(error, VENTURE_ERROR_VALIDATION, "Refunds are immutable");
	allocation_id = get_id(refund, "allocation-id");
	credit_id = get_id(refund, "credit-id");
	if ((allocation_id == 0) == (credit_id == 0))
		return refuse(error, VENTURE_ERROR_VALIDATION, "A refund requires exactly one allocation or unused credit");
	g_object_get(refund, "amount", &amount, "date", &date, NULL);
	if (allocation_id != 0)
	{
		g_autoptr(GPtrArray) refunds = NULL;
		gint64 payment_id;
		guint i;

		allocation = venture_database_get(self->database, VENTURE_TYPE_PAYMENT_ALLOCATION, allocation_id, error);
		if (allocation == NULL || !same_owner(refund, allocation, error))
			return FALSE;
		payment_id = get_id(allocation, "payment-id");
		credit = payment_id != 0 ? payment_credit(self, payment_id, error) :
			venture_database_get(self->database, VENTURE_TYPE_CUSTOMER_CREDIT, get_id(allocation, "credit-id"), error);
		if (credit == NULL)
			return FALSE;
		if (get_id(credit, "payment-id") == 0)
			return refuse(error, VENTURE_ERROR_VALIDATION, "An applied credit note is not a cash receipt to refund");
		invoice = venture_database_get(self->database, VENTURE_TYPE_INVOICE, get_id(allocation, "invoice-id"), error);
		if (invoice == NULL)
			return FALSE;
		g_object_get(allocation, "amount", &available, "date", &source_date, NULL);
		refunds = find_rows(self, VENTURE_TYPE_REFUND, "allocation-id", allocation_id, NULL, error);
		if (refunds == NULL)
			return FALSE;
		for (i = 0; i < refunds->len; i++)
			if (!accumulate_record(&available, g_ptr_array_index(refunds, i), TRUE, error))
				return FALSE;
	}
	else
	{
		credit = venture_database_get(self->database, VENTURE_TYPE_CUSTOMER_CREDIT, credit_id, error);
		if (credit == NULL)
			return FALSE;
		g_object_get(credit, "date", &source_date, NULL);
		available = credit_remaining(self, credit, NULL, error);
		if (available == NULL)
			return FALSE;
	}
	if (!same_owner(refund, credit, error) || !check_customer(self, refund, error))
		return FALSE;
	if (get_id(refund, "customer-id") != get_id(credit, "customer-id"))
		return refuse(error, VENTURE_ERROR_VALIDATION, "The refund must belong to the source customer");
	if (!check_date(date, source_date, error) || !within(amount, available, error))
		return FALSE;
	if (invoice != NULL && !check_invoice_chronology(self, venture_entity_get_id(invoice), date, error))
		return FALSE;
	if (!write_record(self, refund, actor, error) || !post(self, refund, date, amount, "1100", "1000", actor, error))
		return FALSE;
	if (allocation != NULL)
	{
		g_autoptr(VentureSale) adjustment = venture_sale_new();
		g_autoptr(VentureMoney) zero = venture_money_new_zero(amount->currency);
		g_autoptr(VentureMoney) negative = venture_money_subtract(zero, amount, error);
		g_autofree gchar *external = g_strdup_printf("refund:%s", venture_entity_get_uuid(refund));

		/* A later refund belongs to its own cash-report period. Rewriting
		 * the old sale would change the figures of an already closed month. */
		if (negative == NULL)
			return FALSE;
		g_object_set(adjustment, "organization-id", venture_entity_get_organization_id(refund),
			"venture-id", get_id(invoice, "venture-id"), "occurred-at", date,
			"gross", negative, "channel", "invoice", "external-id", external, NULL);
		if (!write_record(self, VENTURE_ENTITY(adjustment), actor, error))
			return FALSE;
		g_object_set(refund, "sale-id", venture_entity_get_id(VENTURE_ENTITY(adjustment)), NULL);
		if (!write_record(self, refund, actor, error) || !derive_invoice(self, invoice, date, actor, error))
			return FALSE;
	}
	return update_credit(self, credit, actor, error);
}

static gboolean
has_changed(VentureEntity *previous, VentureEntity *record, const gchar *const *fields)
{
	g_autoptr(JsonNode) diff = NULL;
	guint i;

	diff = venture_entity_diff(previous, record);
	for (i = 0; fields[i] != NULL; i++)
		if (json_object_has_member(json_node_get_object(diff), fields[i]))
			return TRUE;
	return FALSE;
}

static gboolean
check_invoice_edit(VentureSettlementService *self, VentureEntity *record,
	VentureEntity *previous, GError **error)
{
	static const gchar *const financial[] = {
		"company_id", "organization_id", "venture_id", "issued_at", "due_at", "number", NULL
	};
	static const gchar *const derived[] = { "paid_at", "workflow_state", "deleted_at", NULL };
	gint status;
	gint old_status;

	g_object_get(record, "status", &status, NULL);
	old_status = VENTURE_INVOICE_STATUS_DRAFT;
	if (previous != NULL)
		g_object_get(previous, "status", &old_status, NULL);
	if ((previous == NULL || status != old_status) &&
		(status == VENTURE_INVOICE_STATUS_PAID || status == VENTURE_INVOICE_STATUS_PARTIALLY_PAID))
		return refuse(error, VENTURE_ERROR_VALIDATION, "Use VentureSettlementService to apply a payment; status=paid is derived from allocations");
	if (previous == NULL)
	{
		g_autoptr(GDateTime) paid = NULL;
		g_autofree gchar *workflow = NULL;

		g_object_get(record, "paid-at", &paid, "workflow-state", &workflow, NULL);
		if (status != VENTURE_INVOICE_STATUS_DRAFT || paid != NULL || (workflow != NULL && *workflow != '\0'))
			return refuse(error, VENTURE_ERROR_VALIDATION, "Create a draft, add its lines, then issue it through VentureSettlementService");
	}
	else
	{
		if (has_changed(previous, record, derived))
			return refuse(error, VENTURE_ERROR_VALIDATION, "Paid time, workflow state and deletion are controlled by VentureSettlementService");
		if (old_status != VENTURE_INVOICE_STATUS_DRAFT && has_changed(previous, record, financial))
			return refuse(error, VENTURE_ERROR_VALIDATION, "The issued invoice's customer, dates and amount are frozen");
	}
	return TRUE;
}

static gboolean
check_line(VentureSettlementService *self, VentureEntity *record, VentureEntity *previous, GError **error)
{
	g_autoptr(VentureEntity) invoice = NULL;
	gint status;

	invoice = venture_database_get(self->database, VENTURE_TYPE_INVOICE, get_id(record, "invoice-id"), error);
	if (invoice == NULL || !same_owner(record, invoice, error))
		return FALSE;
	g_object_get(invoice, "status", &status, NULL);
	if (status != VENTURE_INVOICE_STATUS_DRAFT)
		return refuse(error, VENTURE_ERROR_VALIDATION, "Lines on an issued invoice are frozen; use VentureSettlementService for credits");
	if (previous != NULL && get_id(previous, "invoice-id") != get_id(record, "invoice-id"))
	{
		g_clear_object(&invoice);
		invoice = venture_database_get(self->database, VENTURE_TYPE_INVOICE, get_id(previous, "invoice-id"), error);
		if (invoice == NULL)
			return FALSE;
		g_object_get(invoice, "status", &status, NULL);
		if (status != VENTURE_INVOICE_STATUS_DRAFT)
			return refuse(error, VENTURE_ERROR_VALIDATION, "An issued invoice line cannot be moved to a draft");
	}
	return TRUE;
}

static gboolean
is_history(VentureEntity *record)
{
	return VENTURE_IS_PAYMENT(record) || VENTURE_IS_PAYMENT_ALLOCATION(record) ||
		VENTURE_IS_CUSTOMER_CREDIT(record) || VENTURE_IS_REFUND(record) || VENTURE_IS_INVOICE_EVENT(record);
}

gboolean
venture_receivables_check_removal(VentureDatabase *database, VentureEntity *record, GError **error)
{
	g_autoptr(VentureEntity) stored = NULL;
	gint status;

	if (VENTURE_IS_SALE(record))
		return venture_receivables_check_sale(database, record, error);
	if (is_history(record))
		return refuse(error, VENTURE_ERROR_VALIDATION, "Settlement history cannot be deleted, restored or purged");
	if (!VENTURE_IS_INVOICE(record) && !VENTURE_IS_INVOICE_LINE(record))
		return TRUE;
	stored = venture_database_get(database, VENTURE_IS_INVOICE(record) ? VENTURE_TYPE_INVOICE : VENTURE_TYPE_INVOICE_LINE,
		venture_entity_get_id(record), error);
	if (stored == NULL)
		return FALSE;
	if (VENTURE_IS_INVOICE_LINE(record))
		return check_line(venture_settlement_service_get(database), stored, NULL, error);
	{
		g_autoptr(JsonNode) diff = NULL;

		diff = venture_entity_diff(stored, record);
		if (json_object_get_size(json_node_get_object(diff)) != 0)
			return refuse(error, VENTURE_ERROR_VALIDATION, "Save invoice edits before requesting deletion");
	}
	g_object_get(stored, "status", &status, NULL);
	if (status != VENTURE_INVOICE_STATUS_DRAFT)
		return refuse(error, VENTURE_ERROR_VALIDATION, "Issued invoices are history; void them through VentureSettlementService");
	return TRUE;
}

gboolean
venture_receivables_save_hook(VentureDatabase *database, VentureEntity *record,
	const VentureActor *actor, gboolean *handled, gboolean *authorized, GError **error)
{
	VentureSettlementService *self;
	g_autoptr(VentureEntity) previous = NULL;
	g_autoptr(VentureEntity) original = NULL;
	gboolean ok;

	*handled = FALSE;
	*authorized = FALSE;
	if (!is_history(record) && !VENTURE_IS_INVOICE(record) && !VENTURE_IS_INVOICE_LINE(record) && !VENTURE_IS_SALE(record))
		return TRUE;
	self = venture_settlement_service_get(database);
	if (self->writing == record)
	{
		self->writing = NULL;
		*authorized = TRUE;
		return TRUE;
	}
	if (VENTURE_IS_SALE(record))
		return venture_receivables_check_sale(database, record, error);
	if (self->busy)
		return refuse(error, VENTURE_ERROR_CONFLICT, "A settlement is already in progress");
	if (venture_entity_is_persisted(record))
	{
		previous = venture_database_get(database, G_OBJECT_TYPE(record), venture_entity_get_id(record), error);
		if (previous == NULL)
			return FALSE;
		if (venture_entity_get_version(previous) != venture_entity_get_version(record))
			return refuse(error, VENTURE_ERROR_CONFLICT, "The record changed; read it again");
	}
	if (VENTURE_IS_INVOICE_LINE(record))
		return check_line(self, record, previous, error);
	if (VENTURE_IS_INVOICE(record))
	{
		gint status;
		gint old_status;
		g_autoptr(GDateTime) date = NULL;

		if (!check_invoice_edit(self, record, previous, error))
			return FALSE;
		g_object_get(record, "status", &status, NULL);
		old_status = VENTURE_INVOICE_STATUS_DRAFT;
		if (previous != NULL)
			g_object_get(previous, "status", &old_status, NULL);
		if (status == old_status)
			return TRUE;
		*handled = TRUE;
		if (status == VENTURE_INVOICE_STATUS_SENT && old_status == VENTURE_INVOICE_STATUS_DRAFT)
			g_object_get(record, "issued-at", &date, NULL);
		if (date == NULL)
			date = venture_time_now();
		return venture_settlement_service_transition(self, VENTURE_INVOICE(record),
			venture_enum_to_nick(VENTURE_TYPE_INVOICE_STATUS, status), date, actor, error);
	}
	*handled = TRUE;
	if (VENTURE_IS_PAYMENT(record))
		return venture_settlement_service_apply_payment(self, VENTURE_PAYMENT(record), NULL, actor, error);
	if (!begin_operation(self, "payment", error))
		return FALSE;
	original = snapshot(record);
	if (VENTURE_IS_PAYMENT_ALLOCATION(record))
		ok = perform_allocation(self, record, actor, error);
	else if (VENTURE_IS_CUSTOMER_CREDIT(record))
		ok = perform_credit(self, record, actor, error);
	else if (VENTURE_IS_REFUND(record))
		ok = perform_refund(self, record, actor, error);
	else
		ok = refuse(error, VENTURE_ERROR_VALIDATION, "Invoice events can only be written by VentureSettlementService");
	ok = finish_operation(self, ok, error);
	if (!ok)
		venture_entity_copy_properties_from(record, original, FALSE);
	return ok;
}

gboolean
venture_settlement_service_settle_invoice(VentureSettlementService *self,
	gint64 invoice_id, GDateTime *date, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(VentureMoney) balance = NULL;
	g_autoptr(VenturePayment) payment = NULL;
	gboolean ok;

	if (!begin_operation(self, "payment", error))
		return FALSE;
	invoice = venture_database_get(self->database, VENTURE_TYPE_INVOICE, invoice_id, error);
	ok = invoice != NULL;
	if (ok)
	{
		balance = venture_settlement_service_invoice_balance(self, invoice_id, NULL, error);
		ok = balance != NULL && check_amount(balance, error);
	}
	if (ok)
	{
		payment = venture_payment_new();
		g_object_set(payment, "customer-id", get_id(invoice, "company-id"), "invoice-id", invoice_id,
			"date", date, "method", "manual", "amount", balance, NULL);
		venture_entity_set_organization_id(VENTURE_ENTITY(payment), venture_entity_get_organization_id(invoice));
		ok = perform_payment(self, VENTURE_ENTITY(payment), NULL, actor, error);
	}
	return finish_operation(self, ok, error);
}

VentureMoney *
venture_settlement_service_customer_balance(VentureSettlementService *self,
	gint64 organization_id, gint64 customer_id, GDateTime *as_of,
	const gchar *currency, GError **error)
{
	GType types[4];
	g_autoptr(VentureMoney) total = NULL;
	guint t;

	types[0] = VENTURE_TYPE_INVOICE_EVENT;
	types[1] = VENTURE_TYPE_PAYMENT;
	types[2] = VENTURE_TYPE_CUSTOMER_CREDIT;
	types[3] = VENTURE_TYPE_REFUND;
	total = venture_money_new_zero(currency);
	for (t = 0; t < G_N_ELEMENTS(types); t++)
	{
		g_autoptr(GPtrArray) records = NULL;
		guint i;

		records = find_rows(self, types[t], "customer-id", customer_id, as_of, error);
		if (records == NULL)
			return NULL;
		for (i = 0; i < records->len; i++)
		{
			VentureEntity *record;
			g_autoptr(VentureMoney) amount = NULL;
			g_autofree gchar *kind = NULL;
			gboolean subtract;

			record = g_ptr_array_index(records, i);
			if (venture_entity_get_organization_id(record) != organization_id)
				continue;
			if (t == 2 && get_id(record, "payment-id") != 0)
				continue;
			subtract = t == 1 || t == 2;
			if (t == 0)
			{
				g_object_get(record, "kind", &kind, NULL);
				if (g_strcmp0(kind, "issue") != 0 && g_strcmp0(kind, "void") != 0)
					continue;
				subtract = g_strcmp0(kind, "void") == 0;
			}
			g_object_get(record, "amount", &amount, NULL);
			if (amount == NULL)
			{
				refuse(error, VENTURE_ERROR_VALIDATION, "An event has no monetary amount");
				return NULL;
			}
			if (g_strcmp0(venture_money_get_currency(amount), venture_money_get_currency(total)) != 0)
				continue;
			if (!accumulate(&total, amount, subtract, error))
				return NULL;
		}
	}
	return g_steal_pointer(&total);
}

gboolean
venture_settlement_service_correct_tax_allocation(VentureSettlementService *self,
	gint64 organization_id, GDateTime *date, const VentureActor *actor, GError **error)
{
	g_autoptr(GPtrArray) events = NULL;
	guint i;
	if (!begin_operation(self, "invoice", error))
		return FALSE;
	events = find_rows(self, VENTURE_TYPE_INVOICE_EVENT, NULL, 0, NULL, error);
	if (events == NULL)
		return finish_operation(self, FALSE, error);
	for (i = 0; i < events->len; i++)
	{
		VentureEntity *event = g_ptr_array_index(events, i);
		g_autoptr(VentureEntity) invoice = NULL;
		g_autoptr(VentureMoney) tax = NULL;
		g_autoptr(VentureMoney) net = NULL;
		g_autoptr(VentureMoney) total = NULL;
		g_autoptr(VentureMoney) discount = NULL;
		g_autoptr(VentureMoney) shipping = NULL;
		g_autofree gchar *kind = NULL;
		g_autofree gchar *transaction = NULL;
		g_autoptr(GPtrArray) entries = NULL;
		gint64 income = 0, tax_account = 0;
		if (venture_entity_get_organization_id(event) != organization_id)
			continue;
		g_object_get(event, "kind", &kind, "tax-amount", &tax, NULL);
		if (g_strcmp0(kind, "issue") != 0)
			continue;
		if (tax != NULL && !venture_money_is_zero(tax))
			continue;
		invoice = venture_database_get(self->database, VENTURE_TYPE_INVOICE, get_id(event, "invoice-id"), error);
		if (invoice == NULL)
			return finish_operation(self, FALSE, error);
		if (!invoice_parts(self, invoice, &total, &net, &tax, &discount, &shipping, actor, error))
			return finish_operation(self, FALSE, error);
		if (tax == NULL || venture_money_is_zero(tax))
			continue;
		g_object_set(event, "net-amount", net, "tax-amount", tax, "discount-amount", discount,
			"shipping-amount", shipping, NULL);
		if (!write_record(self, event, actor, error))
			return finish_operation(self, FALSE, error);
		if (!resolve_code(self, "4000", organization_id, &income, error) ||
			!resolve_code(self, "2100", organization_id, &tax_account, error))
			return finish_operation(self, FALSE, error);
		transaction = g_strdup_printf("receivables:tax-correction:%s", venture_entity_get_uuid(event));
		entries = g_ptr_array_new_with_free_func(g_object_unref);
		add_leg(entries, event, date, transaction, income, VENTURE_LEDGER_SIDE_DEBIT, tax);
		add_leg(entries, event, date, transaction, tax_account, VENTURE_LEDGER_SIDE_CREDIT, tax);
		if (!venture_receivables_post_batch(self, entries, actor, error))
		{
			if (error != NULL && *error != NULL && (*error)->code == VENTURE_ERROR_ALREADY_EXISTS)
				g_clear_error(error);
			else
				return finish_operation(self, FALSE, error);
		}
	}
	return finish_operation(self, TRUE, error);
}

gboolean venture_settlement_service_record_mail(VentureSettlementService *self, VentureInvoice *invoice, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureInvoiceEvent) event = venture_invoice_event_new();
	g_autoptr(GDateTime) now = venture_time_now();
	g_autofree gchar *state = invoice_state(VENTURE_ENTITY(invoice));
	gboolean ok;
	if (!begin_operation(self, "invoice", error)) return FALSE;
	g_object_set(event, "organization-id", venture_entity_get_organization_id(VENTURE_ENTITY(invoice)),
		"invoice-id", venture_entity_get_id(VENTURE_ENTITY(invoice)),
		"customer-id", get_id(VENTURE_ENTITY(invoice), "company-id"), "date", now,
		"kind", "mail_queued", "state", state, NULL);
	ok = check_customer(self, VENTURE_ENTITY(event), error) && write_record(self, VENTURE_ENTITY(event), actor, error);
	return finish_operation(self, ok, error);
}
