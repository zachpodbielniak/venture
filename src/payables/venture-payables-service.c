/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

#include <string.h>

struct _VenturePayablesService
{
	GObject parent_instance;
	/* The database owns this service. A retained service cannot keep its
	 * database alive forever, or dereference it after destruction. */
	VentureDatabase *database;
	VentureEntity *writing;
	gboolean busy;
	gint64 cash_account;
	gint64 payable_account;
	gint64 expense_account;
};

/* Bind service-level account overrides as well as persisted policy to consent.
 * Nested operations inherit the already authorized whole-command scope. */
static VentureAccountingOperation *
accounting_operation(VenturePayablesService *self, const gchar *name, VentureEntity *subject,
	GPtrArray *details, GVariant *arguments, gint64 org, const VentureActor *actor, GError **error)
{
	return venture_accounting_operation_begin(self->database, name, subject, details,
		g_variant_new("(xxxv)", self->cash_account, self->payable_account, self->expense_account,
			arguments != NULL ? arguments : g_variant_new_tuple(NULL, 0)), org, actor, error);
}

/* ISO dates preserve explicit instants without introducing generated IDs. */
static GVariant *
operation_date(GDateTime *date, const gchar *state)
{
	g_autofree gchar *text = date != NULL ? g_date_time_format_iso8601(date) : NULL;
	return g_variant_new("(ss)", text != NULL ? text : "", state != NULL ? state : "");
}

enum
{
	PROP_0,
	PROP_DATABASE,
	PROP_CASH_ACCOUNT,
	PROP_PAYABLE_ACCOUNT,
	PROP_EXPENSE_ACCOUNT,
	N_PROPERTIES
};

G_DEFINE_FINAL_TYPE(VenturePayablesService, venture_payables_service, G_TYPE_OBJECT)

static gboolean reject_projection_journal(VentureDatabase *db, VentureEntity *journal,
	VentureEntity *previous, gpointer data, GError **error);

static gboolean check_bill_edit(VenturePayablesService *self, VentureEntity *record,
	VentureEntity *previous, GError **error);

static gboolean
refuse(GError **error, VentureError code, const gchar *message)
{
	g_set_error(error, VENTURE_ERROR, code, "VenturePayablesService: %s", message);
	return FALSE;
}

static void
service_get_property(GObject *object, guint id, GValue *value, GParamSpec *spec)
{
	VenturePayablesService *self;

	self = VENTURE_PAYABLES_SERVICE(object);
	switch (id)
	{
	case PROP_DATABASE: g_value_set_object(value, self->database); break;
	case PROP_CASH_ACCOUNT: g_value_set_int64(value, self->cash_account); break;
	case PROP_PAYABLE_ACCOUNT: g_value_set_int64(value, self->payable_account); break;
	case PROP_EXPENSE_ACCOUNT: g_value_set_int64(value, self->expense_account); break;
	default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
	}
}

static void
service_set_property(GObject *object, guint id, const GValue *value, GParamSpec *spec)
{
	VenturePayablesService *self;

	self = VENTURE_PAYABLES_SERVICE(object);
	switch (id)
	{
	case PROP_DATABASE:
		self->database = g_value_get_object(value);
		if (self->database != NULL)
			g_object_add_weak_pointer(G_OBJECT(self->database), (gpointer *)&self->database);
		if (self->database != NULL)
			venture_database_add_save_validator(self->database, VENTURE_TYPE_JOURNAL, reject_projection_journal, NULL, NULL);
		break;
	case PROP_CASH_ACCOUNT: self->cash_account = g_value_get_int64(value); break;
	case PROP_PAYABLE_ACCOUNT: self->payable_account = g_value_get_int64(value); break;
	case PROP_EXPENSE_ACCOUNT: self->expense_account = g_value_get_int64(value); break;
	default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
	}
}

static void
service_finalize(GObject *object)
{
	VenturePayablesService *self;

	self = VENTURE_PAYABLES_SERVICE(object);
	if (self->database != NULL)
		g_object_remove_weak_pointer(G_OBJECT(self->database), (gpointer *)&self->database);
	G_OBJECT_CLASS(venture_payables_service_parent_class)->finalize(object);
}

static void
venture_payables_service_class_init(VenturePayablesServiceClass *klass)
{
	GObjectClass *object_class;

	object_class = G_OBJECT_CLASS(klass);
	object_class->get_property = service_get_property;
	object_class->set_property = service_set_property;
	object_class->finalize = service_finalize;
	g_object_class_install_property(object_class, PROP_DATABASE,
		g_param_spec_object("database", "Database", "Owning database",
			VENTURE_TYPE_DATABASE, G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
	g_object_class_install_property(object_class, PROP_CASH_ACCOUNT,
		g_param_spec_int64("cash-account-id", "Cash account", "Zero resolves code 1000 in the organization",
			0, G_MAXINT64, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
	g_object_class_install_property(object_class, PROP_PAYABLE_ACCOUNT,
		g_param_spec_int64("payable-account-id", "Payable account", "Zero resolves code 2000; unapplied supplier payments reduce this liability",
			0, G_MAXINT64, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
	g_object_class_install_property(object_class, PROP_EXPENSE_ACCOUNT,
		g_param_spec_int64("expense-account-id", "Expense account", "Zero resolves code 6900 in the organization",
			0, G_MAXINT64, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
}

static void
venture_payables_service_init(VenturePayablesService *self)
{
	(void)self;
}

VenturePayablesService *
venture_payables_service_get(VentureDatabase *database)
{
	return venture_database_get_payables_service(database);
}

static const gchar *const phases[] = { "draft", "approved", "partially_paid", "paid", "void" };
typedef enum { VENTURE_VENDOR_BILL_STATUS_DRAFT, VENTURE_VENDOR_BILL_STATUS_SENT,
	VENTURE_VENDOR_BILL_STATUS_PARTIALLY_PAID, VENTURE_VENDOR_BILL_STATUS_PAID,
	VENTURE_VENDOR_BILL_STATUS_VOID } VentureVendorBillStatus;

static gint
bill_phase(VentureEntity *bill)
{
	g_autofree gchar *status = NULL;
	guint i;
	g_object_get(bill, "status", &status, NULL);
	for (i = 0; i < G_N_ELEMENTS(phases); i++)
		if (g_strcmp0(status, phases[i]) == 0)
			return (gint)i;
	return -1;
}

static GPtrArray *
find_rows(VenturePayablesService *self, GType type, const gchar *field,
	gint64 id, GDateTime *cutoff, GError **error)
{
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureEntity) prototype = g_object_new(type, NULL);

	/* A never-enabled optional module has no tables. A disabled module
	 * with existing history must still count toward bill balances. */
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
	if (field != NULL)
	{
		const gchar *reference = venture_entity_class_get_reference(VENTURE_ENTITY_GET_CLASS(prototype), field);
		if (reference != NULL)
		{
			g_autoptr(VentureEntity) parent = NULL;
			GType parent_type = venture_entity_registry_lookup_any(venture_entity_registry_get_default(), reference);
			parent = venture_database_get(self->database, parent_type, id, error);
			if (parent == NULL)
				return NULL;
			venture_query_set_organization(query, venture_entity_get_organization_id(parent));
		}
	}
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
issue_event(VenturePayablesService *self, gint64 bill_id, GDateTime *cutoff, GError **error)
{
	g_autoptr(GPtrArray) events = NULL;
	guint i;

	events = find_rows(self, VENTURE_TYPE_VENDOR_BILL_EVENT, "bill-id", bill_id, cutoff, error);
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
venture_payables_service_bill_balance(VenturePayablesService *self,
	gint64 bill_id, GDateTime *as_of, GError **error)
{
	g_autoptr(GPtrArray) events = NULL;
	g_autoptr(GPtrArray) allocations = NULL;
	g_autoptr(VentureMoney) total = NULL;
	gboolean issued;
	gboolean voided;
	guint i;

	events = find_rows(self, VENTURE_TYPE_VENDOR_BILL_EVENT, "bill-id", bill_id, as_of, error);
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
	allocations = find_rows(self, VENTURE_TYPE_BILL_PAYMENT_ALLOCATION, "bill-id", bill_id, as_of, error);
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
		refunds = find_rows(self, VENTURE_TYPE_BILL_REFUND, "allocation-id", venture_entity_get_id(allocation), as_of, error);
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
check_vendor(VenturePayablesService *self, VentureEntity *record, GError **error)
{
	g_autoptr(VentureEntity) vendor = NULL;

	gint kind;
	vendor = venture_database_get(self->database, VENTURE_TYPE_COMPANY, get_id(record, "vendor-id"), error);
	if (vendor == NULL || !same_owner(record, vendor, error))
		return FALSE;
	g_object_get(vendor, "kind", &kind, NULL);
	if (kind != VENTURE_COMPANY_KIND_SUPPLIER)
		return refuse(error, VENTURE_ERROR_VALIDATION, "A bill vendor must be a supplier company");
	return TRUE;
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
			return refuse(error, VENTURE_ERROR_VALIDATION, "A settlement cannot precede a later event on the same bill");
	}
	return TRUE;
}

static gboolean
check_bill_chronology(VenturePayablesService *self, gint64 bill_id, GDateTime *date, GError **error)
{
	g_autoptr(GPtrArray) events = NULL;
	g_autoptr(GPtrArray) allocations = NULL;
	guint i;

	events = find_rows(self, VENTURE_TYPE_VENDOR_BILL_EVENT, "bill-id", bill_id, NULL, error);
	if (events == NULL || !check_rows_not_later(events, date, error))
		return FALSE;
	allocations = find_rows(self, VENTURE_TYPE_BILL_PAYMENT_ALLOCATION, "bill-id", bill_id, NULL, error);
	if (allocations == NULL || !check_rows_not_later(allocations, date, error))
		return FALSE;
	for (i = 0; i < allocations->len; i++)
	{
		g_autoptr(GPtrArray) refunds = NULL;

		refunds = find_rows(self, VENTURE_TYPE_BILL_REFUND, "allocation-id",
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
write_record(VenturePayablesService *self, VentureEntity *record,
	const VentureActor *actor, GError **error)
{
	gboolean ok;

	if (!venture_entity_is_persisted(record) && g_object_class_find_property(G_OBJECT_GET_CLASS(record), "date") != NULL)
	{
		g_autoptr(GDateTime) date = NULL;
		g_object_get(record, "date", &date, NULL);
		if (!venture_period_guard_is_postable(VENTURE_PERIOD_GUARD(venture_database_get_period_guard(self->database)),
			self->database, venture_entity_get_organization_id(record), date, error))
			return FALSE;
	}
	/* A one-use permit for this exact object. A signal handler saving a
	 * different record during the transaction cannot borrow this authority. */
	self->writing = record;
	ok = venture_database_save(self->database, record, actor, error);
	self->writing = NULL;
	return ok;
}

gboolean
venture_payables_is_projection_write(VentureDatabase *database, VentureEntity *record)
{
	VenturePayablesService *self = venture_database_get_payables_service(database);

	/* Only the current service-owned object may bypass source posting. The
	 * database hook consumes the permit before validators or signals run. */
	return VENTURE_IS_EXPENSE(record) && self != NULL && self->busy && self->writing == record;
}

static gboolean
is_bill_expense(VentureEntity *record)
{
	g_autofree gchar *external = NULL;
	if (!VENTURE_IS_EXPENSE(record))
		return FALSE;
	g_object_get(record, "external-id", &external, NULL);
	return external != NULL && (g_str_has_prefix(external, "bill_line:") || g_str_has_prefix(external, "bill_cash:"));
}

gboolean
venture_payables_check_expense(VentureDatabase *database, VentureEntity *record, GError **error)
{
	g_autoptr(VentureEntity) stored = NULL;
	if (!venture_entity_is_persisted(record))
		return !is_bill_expense(record) || refuse(error, VENTURE_ERROR_PERMISSION_DENIED, "Use the paid bill-line conversion");
	stored = venture_database_get(database, VENTURE_TYPE_EXPENSE, venture_entity_get_id(record), error);
	if (stored == NULL)
		return FALSE;
	if (is_bill_expense(record) || is_bill_expense(stored))
		return refuse(error, VENTURE_ERROR_PERMISSION_DENIED, "Paid bill-line expenses are immutable cash-report projections");
	return TRUE;
}

static gint64
account_id(VenturePayablesService *self, gint64 configured, const gchar *code,
	VentureAccountKind kind, gint64 organization_id, GError **error)
{
	g_autoptr(VentureEntity) account = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autofree gchar *scoped_code = NULL;
	gboolean active;
	gint actual_kind;

	if (configured == 0)
	{
		const gchar *role = g_str_equal(code, "1000") ? "cash" :
			(g_str_equal(code, "2000") ? "payables" :
			(g_str_equal(code, "6900") ? "expense" : NULL));
		if (role != NULL)
		{
			gint64 mapped = venture_setup_resolve_account(self->database, organization_id,
				role, "organization", 0, NULL, error);
			if (mapped != 0)
				configured = mapped;
			else if (error != NULL && *error != NULL)
				return 0;
		}
	}
	if (configured != 0)
		account = venture_database_get(self->database, VENTURE_TYPE_ACCOUNT, configured, error);
	else
	{
		query = venture_query_new(VENTURE_TYPE_ACCOUNT);
		venture_query_set_organization(query, organization_id);
		if (!venture_query_add_filter_string(query, "code", VENTURE_FILTER_OP_EQ, code, error))
			return 0;
		account = venture_database_find_one(self->database, query, error);
		/* Core posting rules namespace secondary charts. Reuse that account
		 * before creating another control account for the same legal entity. */
		if (account == NULL && (error == NULL || *error == NULL))
		{
			scoped_code = g_strdup_printf("%" G_GINT64_FORMAT ":%s", organization_id, code);
			g_clear_object(&query);
			query = venture_query_new(VENTURE_TYPE_ACCOUNT);
			venture_query_set_organization(query, organization_id);
			if (!venture_query_add_filter_string(query, "code", VENTURE_FILTER_OP_EQ, scoped_code, error))
				return 0;
			account = venture_database_find_one(self->database, query, error);
		}
	}
	if (account == NULL && configured == 0 && (error == NULL || *error == NULL))
	{
		account = VENTURE_ENTITY(venture_account_new());
		g_object_set(account, "organization-id", organization_id, "code", scoped_code,
			"name", g_str_equal(code, "6900") ? "General expenses" :
			(g_str_equal(code, "2000") ? "Accounts payable" :
			(g_str_equal(code, "1300") ? "Recoverable tax" : "Cash")),
			"kind", kind, "active", TRUE, NULL);
		if (!venture_database_save(self->database, account, NULL, error))
			return 0;
	}
	if (account == NULL)
	{
		if (error == NULL || *error == NULL)
			refuse(error, VENTURE_ERROR_CONFIG, "Configure the cash, payable and expense accounts for this organization");
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
 * venture_payables_post_batch:
 * @self: the settlement service
 * @entries: (element-type VentureLedgerEntry): the balanced source batch
 * @actor: (nullable): the actor
 * @error: (out) (optional): the error
 *
 * The single posting seam for payables. The caller owns the encompassing
 * transaction; the journal service validates and persists the balanced legs.
 * Returns: TRUE if the balanced batch was saved
 */
static gboolean
venture_payables_post_batch(VenturePayablesService *self, GPtrArray *entries,
	const VentureActor *actor, GError **error)
{
	return venture_posting_service_post_entries(venture_database_get_posting_service(self->database),
		entries, NULL, actor, error);
}

/* These are source-document legs, not a second posting engine. Unapplied
 * cash is a debit balance in AP. Allocation transfers between the unused
 * credit and the bill within that control account, with zero net GL. */
static gboolean
post(VenturePayablesService *self, VentureEntity *source, GDateTime *date,
	const VentureMoney *amount, const gchar *debit_code, const gchar *credit_code,
	const VentureActor *actor, GError **error)
{
	g_autoptr(GPtrArray) entries = NULL;
	g_autofree gchar *transaction = NULL;
	gint64 accounts[2];
	const gchar *codes[2];
	guint i;

	if (!check_amount(amount, error))
		return FALSE;
	codes[0] = debit_code;
	codes[1] = credit_code;
	for (i = 0; i < 2; i++)
	{
		gint64 configured;
		VentureAccountKind kind;

		configured = g_str_equal(codes[i], "1000") ? self->cash_account :
			(g_str_equal(codes[i], "2000") ? self->payable_account : self->expense_account);
		kind = g_str_equal(codes[i], "6900") ? VENTURE_ACCOUNT_KIND_EXPENSE : (g_str_equal(codes[i], "2000") ? VENTURE_ACCOUNT_KIND_LIABILITY : VENTURE_ACCOUNT_KIND_ASSET);
		accounts[i] = account_id(self, configured, codes[i], kind, venture_entity_get_organization_id(source), error);
		if (accounts[i] == 0)
			return FALSE;
	}
	entries = g_ptr_array_new_with_free_func(g_object_unref);
	transaction = g_strdup_printf("payables:%s:%s", venture_entity_get_entity_name(source), venture_entity_get_uuid(source));
	for (i = 0; i < 2; i++)
	{
		VentureLedgerEntry *entry;

		entry = venture_ledger_entry_new();
		g_object_set(entry, "transaction-id", transaction, "account-id", accounts[i],
			"side", i == 0 ? VENTURE_LEDGER_SIDE_DEBIT : VENTURE_LEDGER_SIDE_CREDIT,
			"amount", amount, "occurred-at", date,
			"source-type", venture_entity_get_entity_name(source), "source-id", venture_entity_get_id(source), NULL);
		venture_entity_set_organization_id(VENTURE_ENTITY(entry), venture_entity_get_organization_id(source));
		g_ptr_array_add(entries, entry);
	}
	return venture_payables_post_batch(self, entries, actor, error);
}

static gboolean
begin_operation(VenturePayablesService *self, const gchar *type, GError **error)
{
	if (self->database == NULL)
		return refuse(error, VENTURE_ERROR_DATABASE, "The database has been closed");
	if (self->busy)
		return refuse(error, VENTURE_ERROR_CONFLICT, "A settlement is already in progress");
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), type) == G_TYPE_INVALID)
		return refuse(error, VENTURE_ERROR_CONFIG, "The payables module is disabled (modules.payables.enabled)");
	if (!venture_database_begin(self->database, error))
		return FALSE;
	self->busy = TRUE;
	return TRUE;
}

static gboolean
finish_operation(VenturePayablesService *self, gboolean ok, GError **error)
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

static gboolean
derive_bill(VenturePayablesService *self, VentureEntity *bill, GDateTime *date,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) issued = NULL;
	g_autoptr(VentureMoney) total = NULL;
	g_autoptr(VentureMoney) balance = NULL;
	VentureVendorBillStatus status;

	issued = issue_event(self, venture_entity_get_id(bill), NULL, error);
	if (issued == NULL)
		return refuse(error, VENTURE_ERROR_VALIDATION, "Issue the bill before allocating a payment");
	g_object_get(issued, "amount", &total, NULL);
	balance = venture_payables_service_bill_balance(self, venture_entity_get_id(bill), NULL, error);
	if (balance == NULL)
		return FALSE;
	status = venture_money_is_zero(balance) ? VENTURE_VENDOR_BILL_STATUS_PAID :
		(venture_money_equal(total, balance) ? VENTURE_VENDOR_BILL_STATUS_SENT : VENTURE_VENDOR_BILL_STATUS_PARTIALLY_PAID);
	g_object_set(bill, "status", phases[status], NULL);
	return write_record(self, bill, actor, error);
}

static VentureMoney *
bill_total(VenturePayablesService *self, VentureEntity *bill, GError **error)
{
	g_autoptr(GPtrArray) lines = NULL;
	g_autoptr(VentureMoney) total = NULL;
	guint i;
	g_autofree gchar *currency = NULL;

	g_object_get(bill, "currency", &currency, NULL);
	lines = find_rows(self, VENTURE_TYPE_VENDOR_BILL_LINE, "bill-id", venture_entity_get_id(bill), NULL, error);
	if (lines == NULL)
		return NULL;
	for (i = 0; i < lines->len; i++)
	{
		VentureEntity *line;
		g_autoptr(VentureMoney) amount = NULL;

		line = g_ptr_array_index(lines, i);
		if (venture_entity_is_deleted(line))
			continue;
		if (!same_owner(bill, line, error))
			return NULL;
		amount = venture_vendor_bill_line_get_amount(VENTURE_VENDOR_BILL_LINE(line), error);
		if (amount != NULL && g_strcmp0(currency, venture_money_get_currency(amount)) != 0)
		{
			refuse(error, VENTURE_ERROR_VALIDATION, "Bill and line currencies must match");
			return NULL;
		}
		if (amount == NULL || !check_amount(amount, error) || !accumulate(&total, amount, FALSE, error))
			return NULL;
	}
	if (!check_amount(total, error))
		return NULL;
	return g_steal_pointer(&total);
}

static gboolean
post_bill(VenturePayablesService *self, VentureEntity *bill, VentureEntity *event,
	GDateTime *date, gboolean approval, const VentureActor *actor, GError **error)
{
	g_autoptr(GPtrArray) lines = NULL;
	g_autoptr(GPtrArray) entries = g_ptr_array_new_with_free_func(g_object_unref);
	g_autoptr(VentureMoney) total = NULL;
	g_autofree gchar *transaction = g_strdup_printf("payables:bill:%s", venture_entity_get_uuid(event));
	gint64 org = venture_entity_get_organization_id(bill);
	gint64 payable;
	guint i;
	VentureLedgerEntry *entry;

	payable = account_id(self, self->payable_account, "2000", VENTURE_ACCOUNT_KIND_LIABILITY, org, error);
	if (payable == 0)
		return FALSE;
	lines = find_rows(self, VENTURE_TYPE_VENDOR_BILL_LINE, "bill-id", venture_entity_get_id(bill), NULL, error);
	if (lines == NULL)
		return FALSE;
	for (i = 0; i < lines->len; i++)
	{
		VentureEntity *line = g_ptr_array_index(lines, i);
		g_autoptr(VentureMoney) amount = NULL;
		g_autoptr(VentureMoney) tax = NULL;
		g_autoptr(VentureMoney) net = NULL;
		g_autoptr(VentureEntity) tax_code = NULL;
		gboolean recoverable = FALSE;
		gint64 account;
		gint64 tax_code_id;
		if (venture_entity_is_deleted(line))
			continue;
		amount = venture_vendor_bill_line_get_amount(VENTURE_VENDOR_BILL_LINE(line), error);
		if (amount == NULL || !accumulate(&total, amount, FALSE, error))
			return FALSE;
		g_object_get(line, "tax-amount", &tax, NULL);
		tax_code_id = get_id(line, "tax-code-id");
		if (tax_code_id != 0)
		{
			tax_code = venture_database_get(self->database, VENTURE_TYPE_TAX_CODE, tax_code_id, error);
			if (tax_code == NULL)
				return FALSE;
			g_object_get(tax_code, "recoverable", &recoverable, NULL);
		}
		if (tax != NULL && !venture_money_is_zero(tax))
			net = venture_money_subtract(amount, tax, error);
		else
			net = g_steal_pointer(&amount);
		if (net == NULL)
			return FALSE;
		if (get_id(line, "account-id") != 0)
		{
			g_autoptr(VentureEntity) chosen = venture_database_get(self->database, VENTURE_TYPE_ACCOUNT,
				get_id(line, "account-id"), error);
			gboolean active = FALSE;
			if (chosen == NULL)
				return FALSE;
			g_object_get(chosen, "active", &active, NULL);
			if (!active || venture_entity_get_organization_id(chosen) != org)
				return refuse(error, VENTURE_ERROR_VALIDATION,
					"The posting account must be active, of the right class, and in the same organization");
			account = venture_entity_get_id(chosen);
		}
		else
			account = account_id(self, self->expense_account, "6900", VENTURE_ACCOUNT_KIND_EXPENSE, org, error);
		if (account == 0)
			return FALSE;
		entry = venture_ledger_entry_new();
		g_object_set(entry, "organization-id", org, "transaction-id", transaction, "account-id", account,
			"side", approval ? VENTURE_LEDGER_SIDE_DEBIT : VENTURE_LEDGER_SIDE_CREDIT,
			"amount", recoverable ? net : (amount != NULL ? amount : net), "occurred-at", date,
			"source-type", "vendor_bill_event", "source-id", venture_entity_get_id(event), NULL);
		g_ptr_array_add(entries, entry);
		if (recoverable && tax != NULL && !venture_money_is_zero(tax))
		{
			gint64 recoverable_account = account_id(self, 0, "1300", VENTURE_ACCOUNT_KIND_ASSET, org, error);
			if (recoverable_account == 0)
				return FALSE;
			entry = venture_ledger_entry_new();
			g_object_set(entry, "organization-id", org, "transaction-id", transaction,
				"account-id", recoverable_account,
				"side", approval ? VENTURE_LEDGER_SIDE_DEBIT : VENTURE_LEDGER_SIDE_CREDIT,
				"amount", tax, "occurred-at", date, "source-type", "vendor_bill_event",
				"source-id", venture_entity_get_id(event), NULL);
			g_ptr_array_add(entries, entry);
		}
	}
	if (total == NULL)
		return refuse(error, VENTURE_ERROR_VALIDATION, "A bill requires expense lines");
	entry = venture_ledger_entry_new();
	g_object_set(entry, "organization-id", org, "transaction-id", transaction, "account-id", payable,
		"side", approval ? VENTURE_LEDGER_SIDE_CREDIT : VENTURE_LEDGER_SIDE_DEBIT,
		"amount", total, "occurred-at", date, "source-type", "vendor_bill_event",
		"source-id", venture_entity_get_id(event), NULL);
	g_ptr_array_add(entries, entry);
	return venture_payables_post_batch(self, entries, actor, error);
}

static gboolean
perform_transition(VenturePayablesService *self, VentureEntity *bill, VentureVendorBillEvent *request,
	const gchar *state, GDateTime *date, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) previous = NULL;
	g_autoptr(VentureEntity) issued = NULL;
	g_autoptr(VentureVendorBillEvent) event = NULL;
	g_autoptr(VentureMoney) total = NULL;
	g_autoptr(VentureMoney) balance = NULL;
	g_autoptr(GDateTime) earliest = NULL;
	g_autoptr(GDateTime) due = NULL;
	gboolean approval = g_strcmp0(state, "approved") == 0;
	gint old_phase;

	previous = venture_database_get(self->database, VENTURE_TYPE_VENDOR_BILL, venture_entity_get_id(bill), error);
	if (previous == NULL)
		return FALSE;
	if (venture_entity_get_version(previous) != venture_entity_get_version(bill))
		return refuse(error, VENTURE_ERROR_CONFLICT, "The bill changed; read it again");
	if (!check_bill_edit(self, bill, previous, error))
		return FALSE;
	old_phase = bill_phase(previous);
	if ((approval && old_phase != VENTURE_VENDOR_BILL_STATUS_DRAFT) ||
		(!approval && (g_strcmp0(state, "void") != 0 || old_phase != VENTURE_VENDOR_BILL_STATUS_SENT)))
		return refuse(error, VENTURE_ERROR_VALIDATION, "Approve a draft or void an unpaid approved bill");
	g_object_get(bill, "bill-date", &earliest, "due-date", &due, NULL);
	if (!check_date(date, earliest, error) || !check_bill_chronology(self, venture_entity_get_id(bill), date, error))
		return FALSE;
	if (approval && !venture_purchasing_check_bill_approval(self->database, bill, error))
		return FALSE;
	if (approval)
		total = bill_total(self, bill, error);
	else
	{
		issued = issue_event(self, venture_entity_get_id(bill), NULL, error);
		if (issued == NULL)
			return refuse(error, VENTURE_ERROR_VALIDATION, "The bill has no approval event");
		g_object_get(issued, "amount", &total, NULL);
		balance = venture_payables_service_bill_balance(self, venture_entity_get_id(bill), NULL, error);
		if (balance == NULL || !venture_money_equal(balance, total))
			return refuse(error, VENTURE_ERROR_VALIDATION, "Only an unpaid bill can be voided");
	}
	if (total == NULL)
		return FALSE;
	event = request != NULL ? g_object_ref(request) : venture_vendor_bill_event_new();
	g_object_set(event, "bill-id", venture_entity_get_id(bill), "vendor-id", get_id(bill, "company-id"),
		"date", date, "kind", approval ? "issue" : "void", "state", state,
		"due-date", due, "amount", total, "venture-id", get_id(bill, "venture-id"), NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(event), venture_entity_get_organization_id(bill));
	if (!check_vendor(self, VENTURE_ENTITY(event), error) || !write_record(self, VENTURE_ENTITY(event), actor, error) ||
		!post_bill(self, bill, VENTURE_ENTITY(event), date, approval, actor, error))
		return FALSE;
	g_object_set(bill, "status", state, NULL);
	return write_record(self, bill, actor, error);
}

gboolean
venture_payables_service_transition(VenturePayablesService *self,
	VentureVendorBill *bill, const gchar *state, GDateTime *date,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureAccountingOperation) operation = NULL;
	g_autoptr(VentureEntity) original = NULL;
	gboolean ok;

	operation = accounting_operation(self, "payables.transition", VENTURE_ENTITY(bill), NULL,
		operation_date(date, state), venture_entity_get_organization_id(VENTURE_ENTITY(bill)), actor, error);
	if (operation == NULL) return FALSE;
	if (!begin_operation(self, "vendor_bill", error))
		return FALSE;
	original = snapshot(VENTURE_ENTITY(bill));
	ok = perform_transition(self, VENTURE_ENTITY(bill), NULL, state, date, actor, error);
	ok = finish_operation(self, ok, error);
	if (ok) ok = venture_accounting_operation_finish(operation, error);
	if (!ok)
		venture_entity_copy_properties_from(VENTURE_ENTITY(bill), original, FALSE);
	return ok;
}

static VentureEntity *
payment_credit(VenturePayablesService *self, gint64 payment_id, GError **error)
{
	g_autoptr(GPtrArray) credits = NULL;

	credits = find_rows(self, VENTURE_TYPE_VENDOR_CREDIT, "payment-id", payment_id, NULL, error);
	if (credits == NULL)
		return NULL;
	if (credits->len != 1)
	{
		refuse(error, VENTURE_ERROR_VALIDATION, "The payment has no unique source credit");
		return NULL;
	}
	return g_object_ref(g_ptr_array_index(credits, 0));
}

static VentureMoney *
credit_remaining(VenturePayablesService *self, VentureEntity *credit,
	GDateTime *cutoff, GError **error)
{
	g_autoptr(VentureMoney) remaining = NULL;
	g_autoptr(GPtrArray) allocations = NULL;
	g_autoptr(GPtrArray) refunds = NULL;
	gint64 payment_id;
	guint i;

	g_object_get(credit, "amount", &remaining, NULL);
	/* A reversed credit-note posting (cutover rollback) still has its
	 * original amount and no allocations; treat it as consumed. */
	if (remaining != NULL && get_id(credit, "payment-id") == 0 &&
		venture_posting_service_source_has_reversal(venture_database_get_posting_service(self->database),
			venture_entity_get_entity_name(credit), venture_entity_get_id(credit),
			venture_entity_get_organization_id(credit), error))
		return venture_money_new_zero(venture_money_get_currency(remaining));
	if (error != NULL && *error != NULL)
		return NULL;
	allocations = find_rows(self, VENTURE_TYPE_BILL_PAYMENT_ALLOCATION, "credit-id", venture_entity_get_id(credit), cutoff, error);
	if (allocations == NULL)
		return NULL;
	for (i = 0; i < allocations->len; i++)
		if (!accumulate_record(&remaining, g_ptr_array_index(allocations, i), TRUE, error))
			return NULL;
	payment_id = get_id(credit, "payment-id");
	if (payment_id != 0)
	{
		g_clear_pointer(&allocations, g_ptr_array_unref);
		allocations = find_rows(self, VENTURE_TYPE_BILL_PAYMENT_ALLOCATION, "payment-id", payment_id, cutoff, error);
		if (allocations == NULL)
			return NULL;
		for (i = 0; i < allocations->len; i++)
			if (!accumulate_record(&remaining, g_ptr_array_index(allocations, i), TRUE, error))
				return NULL;
	}
	refunds = find_rows(self, VENTURE_TYPE_BILL_REFUND, "credit-id", venture_entity_get_id(credit), cutoff, error);
	if (refunds == NULL)
		return NULL;
	for (i = 0; i < refunds->len; i++)
		if (!accumulate_record(&remaining, g_ptr_array_index(refunds, i), TRUE, error))
			return NULL;
	return g_steal_pointer(&remaining);
}

static gboolean
update_credit(VenturePayablesService *self, VentureEntity *credit,
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
project_cash_change(VenturePayablesService *self, VentureEntity *bill,
	VentureEntity *source, const VentureMoney *amount, GDateTime *date,
	gboolean refund, const VentureActor *actor, GError **error)
{
	g_autoptr(GPtrArray) lines = NULL;
	g_autoptr(GPtrArray) shares = NULL;
	g_autofree gint64 *ratios = NULL;
	guint i;
	lines = find_rows(self, VENTURE_TYPE_VENDOR_BILL_LINE, "bill-id", venture_entity_get_id(bill), NULL, error);
	if (lines == NULL)
		return FALSE;
	ratios = g_new0(gint64, lines->len);
	for (i = 0; i < lines->len; i++)
	{
		VentureEntity *line = g_ptr_array_index(lines, i);
		g_autoptr(VentureMoney) value = NULL;
		if (venture_entity_is_deleted(line))
			continue;
		value = venture_vendor_bill_line_get_amount(VENTURE_VENDOR_BILL_LINE(line), error);
		if (value == NULL)
			return FALSE;
		ratios[i] = venture_money_get_amount(value);
	}
	shares = venture_money_allocate(amount, ratios, lines->len, error);
	if (shares == NULL)
		return FALSE;
	for (i = 0; i < lines->len; i++)
	{
		VentureEntity *line = g_ptr_array_index(lines, i);
		const VentureMoney *share = g_ptr_array_index(shares, i);
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_EXPENSE);
		g_autoptr(VentureEntity) original = NULL;
		g_autoptr(VentureExpense) adjustment = NULL;
		g_autoptr(VentureMoney) value = NULL;
		g_autofree gchar *key = g_strdup_printf("bill_line:%s", venture_entity_get_uuid(line));
		g_autofree gchar *external = NULL;
		g_autofree gchar *description = NULL;
		g_autofree gchar *category = NULL;
		g_autofree gchar *vendor = NULL;
		gint deductibility;
		gint64 business_use;
		gint64 tax_category;
		if (venture_money_is_zero(share))
			continue;
		venture_query_set_organization(query, venture_entity_get_organization_id(bill));
		if (!venture_query_add_filter_string(query, "external-id", VENTURE_FILTER_OP_EQ, key, error))
			return FALSE;
		original = venture_database_find_one(self->database, query, error);
		if (original == NULL)
		{
			if (error != NULL && *error != NULL)
				return FALSE;
			continue;
		}
		value = venture_money_multiply_int(share, refund ? -1 : 1, error);
		if (value == NULL)
			return FALSE;
		external = g_strdup_printf("bill_cash:%s:%s:%s", venture_entity_get_entity_name(source),
			venture_entity_get_uuid(source), venture_entity_get_uuid(line));
		g_object_get(original, "description", &description, "category", &category, "vendor", &vendor,
			"deductibility", &deductibility, "business-use-percent", &business_use, "tax-category-id", &tax_category, NULL);
		adjustment = venture_expense_new();
		g_object_set(adjustment, "organization-id", venture_entity_get_organization_id(bill),
			"venture-id", get_id(original, "venture-id"), "description", description, "category", category,
			"vendor", vendor, "deductibility", deductibility, "business-use-percent", business_use,
			"tax-category-id", tax_category, "external-id", external, "amount", value, "occurred-at", date, NULL);
		if (!venture_period_guard_is_postable(VENTURE_PERIOD_GUARD(venture_database_get_period_guard(self->database)),
			self->database, venture_entity_get_organization_id(bill), date, error) ||
			!write_record(self, VENTURE_ENTITY(adjustment), actor, error))
			return FALSE;
	}
	return TRUE;
}

static gboolean
perform_allocation(VenturePayablesService *self, VentureEntity *allocation,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) credit = NULL;
	g_autoptr(VentureEntity) bill = NULL;
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
	credit = payment_id != 0 ? payment_credit(self, payment_id, error) :
		venture_database_get(self->database, VENTURE_TYPE_VENDOR_CREDIT, credit_id, error);
	if (credit == NULL)
		return FALSE;
	bill = venture_database_get(self->database, VENTURE_TYPE_VENDOR_BILL, get_id(allocation, "bill-id"), error);
	if (bill == NULL || !same_owner(allocation, bill, error) || !same_owner(allocation, credit, error))
		return FALSE;
	if (get_id(bill, "company-id") != get_id(credit, "vendor-id"))
		return refuse(error, VENTURE_ERROR_VALIDATION, "A payment or credit may only settle its own vendor's bills");
	status = bill_phase(bill);
	if (status == VENTURE_VENDOR_BILL_STATUS_DRAFT || status == VENTURE_VENDOR_BILL_STATUS_VOID)
		return refuse(error, VENTURE_ERROR_VALIDATION, "Only an issued, non-void bill accepts allocations");
	issued = issue_event(self, venture_entity_get_id(bill), NULL, error);
	if (issued == NULL)
		return refuse(error, VENTURE_ERROR_VALIDATION, "The bill has no issue event");
	g_object_get(allocation, "amount", &amount, "date", &date, NULL);
	g_object_get(credit, "date", &source_date, NULL);
	g_object_get(issued, "date", &issue_date, NULL);
	if (!check_date(date, source_date, error) || !check_date(date, issue_date, error))
		return FALSE;
	if (!check_bill_chronology(self, venture_entity_get_id(bill), date, error))
		return FALSE;
	remaining = credit_remaining(self, credit, NULL, error);
	if (remaining == NULL || !within(amount, remaining, error))
		return FALSE;
	balance = venture_payables_service_bill_balance(self, venture_entity_get_id(bill), NULL, error);
	if (balance == NULL || !within(amount, balance, error))
		return FALSE;
	return write_record(self, allocation, actor, error) &&
		(get_id(credit, "payment-id") == 0 || project_cash_change(self, bill, allocation, amount, date, FALSE, actor, error)) &&
		post(self, allocation, date, amount, "2000", "2000", actor, error) &&
		update_credit(self, credit, actor, error) && derive_bill(self, bill, date, actor, error);
}

static gboolean
perform_credit(VenturePayablesService *self, VentureEntity *credit,
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
	if (!check_amount(amount, error) || !check_date(date, NULL, error) || !check_vendor(self, credit, error))
		return FALSE;
	g_object_set(credit, "remaining", amount, NULL);
	return write_record(self, credit, actor, error) && post(self, credit, date, amount, "2000", "6900", actor, error);
}

static gboolean
perform_payment(VenturePayablesService *self, VentureEntity *payment,
	GPtrArray *allocations, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureVendorCredit) credit = NULL;
	g_autoptr(VentureMoney) amount = NULL;
	g_autoptr(GDateTime) date = NULL;
	g_autofree gchar *external = NULL;
	g_autofree gchar *key = NULL;
	gint64 bill_id;
	guint i;

	if (venture_entity_is_persisted(payment))
		return refuse(error, VENTURE_ERROR_VALIDATION, "Payments are immutable; allocate unused funds or record a refund");
	g_object_get(payment, "amount", &amount, "date", &date, "external-id", &external, NULL);
	if (!check_amount(amount, error) || !check_date(date, NULL, error) || !check_vendor(self, payment, error))
		return FALSE;
	if (external != NULL && *external != '\0')
	{
		g_autoptr(VentureQuery) query = NULL;
		gint64 count;

		query = venture_query_new(VENTURE_TYPE_BILL_PAYMENT);
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
	credit = venture_vendor_credit_new();
	bill_id = get_id(payment, "bill-id");
	g_object_set(credit, "vendor-id", get_id(payment, "vendor-id"), "date", date,
		"amount", amount, "remaining", amount, "payment-id", venture_entity_get_id(payment),
		"kind", bill_id != 0 || (allocations != NULL && allocations->len > 0) ? "overpayment" : "deposit", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(credit), venture_entity_get_organization_id(payment));
	if (!write_record(self, VENTURE_ENTITY(credit), actor, error) ||
		!post(self, payment, date, amount, "2000", "1000", actor, error) ||
		!post(self, VENTURE_ENTITY(credit), date, amount, "2000", "2000", actor, error))
		return FALSE;
	if (allocations != NULL)
	{
		if (bill_id != 0)
			return refuse(error, VENTURE_ERROR_VALIDATION, "Supply bill-id or an allocation batch, not both");
		for (i = 0; i < allocations->len; i++)
		{
			VentureEntity *allocation;

			allocation = g_ptr_array_index(allocations, i);
			if (!VENTURE_IS_BILL_PAYMENT_ALLOCATION(allocation) || get_id(allocation, "payment-id") != 0 || get_id(allocation, "credit-id") != 0)
				return refuse(error, VENTURE_ERROR_VALIDATION, "A payment batch requires new allocations without an existing source");
			g_object_set(allocation, "payment-id", venture_entity_get_id(payment), "date", date, NULL);
			venture_entity_set_organization_id(allocation, venture_entity_get_organization_id(payment));
			if (!perform_allocation(self, allocation, actor, error))
				return FALSE;
		}
	}
	else if (bill_id != 0)
	{
		g_autoptr(VentureBillPaymentAllocation) allocation = NULL;
		g_autoptr(VentureMoney) balance = NULL;
		g_autoptr(VentureMoney) difference = NULL;
		const VentureMoney *applied;

		balance = venture_payables_service_bill_balance(self, bill_id, NULL, error);
		if (balance == NULL || !check_amount(balance, error))
			return FALSE;
		difference = venture_money_subtract(amount, balance, error);
		if (difference == NULL)
			return FALSE;
		applied = venture_money_get_amount(difference) < 0 ? amount : balance;
		allocation = venture_bill_payment_allocation_new();
		g_object_set(allocation, "payment-id", venture_entity_get_id(payment), "bill-id", bill_id,
			"amount", applied, "date", date, NULL);
		venture_entity_set_organization_id(VENTURE_ENTITY(allocation), venture_entity_get_organization_id(payment));
		if (!perform_allocation(self, VENTURE_ENTITY(allocation), actor, error))
			return FALSE;
	}
	return TRUE;
}

gboolean
venture_payables_service_apply_payment(VenturePayablesService *self,
	VentureBillPayment *payment, GPtrArray *allocations, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureAccountingOperation) operation = NULL;
	g_autoptr(VentureEntity) approval = NULL;
	g_autoptr(VentureEntity) original = NULL;
	g_autoptr(GPtrArray) originals = NULL;
	gboolean ok;
	guint i;

	operation = accounting_operation(self, "payables.apply_payment", VENTURE_ENTITY(payment), allocations,
		NULL, venture_entity_get_organization_id(VENTURE_ENTITY(payment)), actor, error);
	if (operation == NULL) return FALSE;
	if (!venture_accounting_approval_allow(self->database, "pay", VENTURE_ENTITY(payment),
		allocations, actor, &approval, error))
		return FALSE;
	if (!begin_operation(self, "bill_payment", error))
		return FALSE;
	original = snapshot(VENTURE_ENTITY(payment));
	originals = g_ptr_array_new_with_free_func(g_object_unref);
	if (allocations != NULL)
		for (i = 0; i < allocations->len; i++)
		{
			if (!VENTURE_IS_BILL_PAYMENT_ALLOCATION(g_ptr_array_index(allocations, i)))
			{
				refuse(error, VENTURE_ERROR_VALIDATION, "The batch may only contain payment allocations");
				return finish_operation(self, FALSE, error);
			}
			g_ptr_array_add(originals, snapshot(g_ptr_array_index(allocations, i)));
		}
	ok = perform_payment(self, VENTURE_ENTITY(payment), allocations, actor, error);
	if (ok)
		ok = venture_accounting_approval_consume(self->database, approval, actor, error);
	ok = finish_operation(self, ok, error);
	if (ok) ok = venture_accounting_operation_finish(operation, error);
	if (!ok)
	{
		venture_entity_copy_properties_from(VENTURE_ENTITY(payment), original, FALSE);
		for (i = 0; i < originals->len; i++)
			venture_entity_copy_properties_from(g_ptr_array_index(allocations, i), g_ptr_array_index(originals, i), FALSE);
	}
	return ok;
}

static gboolean
perform_refund(VenturePayablesService *self, VentureEntity *refund,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) allocation = NULL;
	g_autoptr(VentureEntity) credit = NULL;
	g_autoptr(VentureEntity) bill = NULL;
	g_autoptr(VentureMoney) available = NULL;
	g_autoptr(VentureMoney) amount = NULL;
	g_autoptr(GDateTime) date = NULL;
	g_autoptr(GDateTime) source_date = NULL;
	gint64 allocation_id;
	gint64 credit_id;

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

		allocation = venture_database_get(self->database, VENTURE_TYPE_BILL_PAYMENT_ALLOCATION, allocation_id, error);
		if (allocation == NULL || !same_owner(refund, allocation, error))
			return FALSE;
		payment_id = get_id(allocation, "payment-id");
		credit = payment_id != 0 ? payment_credit(self, payment_id, error) :
			venture_database_get(self->database, VENTURE_TYPE_VENDOR_CREDIT, get_id(allocation, "credit-id"), error);
		if (credit == NULL)
			return FALSE;
		if (get_id(credit, "payment-id") == 0)
			return refuse(error, VENTURE_ERROR_VALIDATION, "An applied credit note has no cash payment to refund");
		bill = venture_database_get(self->database, VENTURE_TYPE_VENDOR_BILL, get_id(allocation, "bill-id"), error);
		if (bill == NULL)
			return FALSE;
		g_object_get(allocation, "amount", &available, "date", &source_date, NULL);
		refunds = find_rows(self, VENTURE_TYPE_BILL_REFUND, "allocation-id", allocation_id, NULL, error);
		if (refunds == NULL)
			return FALSE;
		for (i = 0; i < refunds->len; i++)
			if (!accumulate_record(&available, g_ptr_array_index(refunds, i), TRUE, error))
				return FALSE;
	}
	else
	{
		credit = venture_database_get(self->database, VENTURE_TYPE_VENDOR_CREDIT, credit_id, error);
		if (credit == NULL)
			return FALSE;
		g_object_get(credit, "date", &source_date, NULL);
		available = credit_remaining(self, credit, NULL, error);
		if (available == NULL)
			return FALSE;
	}
	if (!same_owner(refund, credit, error) || !check_vendor(self, refund, error))
		return FALSE;
	if (get_id(refund, "vendor-id") != get_id(credit, "vendor-id"))
		return refuse(error, VENTURE_ERROR_VALIDATION, "The refund must belong to the source vendor");
	if (!check_date(date, source_date, error) || !within(amount, available, error))
		return FALSE;
	if (bill != NULL && !check_bill_chronology(self, venture_entity_get_id(bill), date, error))
		return FALSE;
	if (!write_record(self, refund, actor, error) || !post(self, refund, date, amount, "1000", "2000", actor, error))
		return FALSE;
	if (allocation != NULL && (!project_cash_change(self, bill, refund, amount, date, TRUE, actor, error) ||
		!derive_bill(self, bill, date, actor, error)))
		return FALSE;
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
check_bill_edit(VenturePayablesService *self, VentureEntity *record,
	VentureEntity *previous, GError **error)
{
	static const gchar *const financial[] = {
		"company_id", "organization_id", "venture_id", "bill_date", "due_date", "number", "currency", "deleted_at", NULL
	};
	gint phase = bill_phase(record);
	gint old = previous != NULL ? bill_phase(previous) : VENTURE_VENDOR_BILL_STATUS_DRAFT;
	if (phase < 0 || phase != old)
		return refuse(error, VENTURE_ERROR_VALIDATION, "Financial status is derived; approve, pay or void through VenturePayablesService");
	if (previous != NULL && old != VENTURE_VENDOR_BILL_STATUS_DRAFT && has_changed(previous, record, financial))
		return refuse(error, VENTURE_ERROR_VALIDATION, "The approved bill's vendor, amount, currency and dates are frozen");
	return TRUE;
}

static gboolean
check_line(VenturePayablesService *self, VentureEntity *record, VentureEntity *previous, GError **error)
{
	g_autoptr(VentureEntity) bill = NULL;
	gint status;

	bill = venture_database_get(self->database, VENTURE_TYPE_VENDOR_BILL, get_id(record, "bill-id"), error);
	if (bill == NULL || !same_owner(record, bill, error))
		return FALSE;
	status = bill_phase(bill);
	if (status != VENTURE_VENDOR_BILL_STATUS_DRAFT)
		return refuse(error, VENTURE_ERROR_VALIDATION, "Lines on an issued bill are frozen; use VenturePayablesService for credits");
	if (previous != NULL && get_id(previous, "bill-id") != get_id(record, "bill-id"))
	{
		g_clear_object(&bill);
		bill = venture_database_get(self->database, VENTURE_TYPE_VENDOR_BILL, get_id(previous, "bill-id"), error);
		if (bill == NULL)
			return FALSE;
		status = bill_phase(bill);
		if (status != VENTURE_VENDOR_BILL_STATUS_DRAFT)
			return refuse(error, VENTURE_ERROR_VALIDATION, "An issued bill line cannot be moved to a draft");
	}
	return TRUE;
}

static gboolean
is_history(VentureEntity *record)
{
	return VENTURE_IS_BILL_PAYMENT(record) || VENTURE_IS_BILL_PAYMENT_ALLOCATION(record) ||
		VENTURE_IS_VENDOR_CREDIT(record) || VENTURE_IS_BILL_REFUND(record) || VENTURE_IS_VENDOR_BILL_EVENT(record);
}

gboolean
venture_payables_check_removal(VentureDatabase *database, VentureEntity *record, GError **error)
{
	g_autoptr(VentureEntity) stored = NULL;
	gint status;

	if (VENTURE_IS_EXPENSE(record))
		return venture_payables_check_expense(database, record, error);
	if (is_history(record))
		return refuse(error, VENTURE_ERROR_VALIDATION, "Settlement history cannot be deleted, restored or purged");
	if (!VENTURE_IS_VENDOR_BILL(record) && !VENTURE_IS_VENDOR_BILL_LINE(record))
		return TRUE;
	stored = venture_database_get(database, VENTURE_IS_VENDOR_BILL(record) ? VENTURE_TYPE_VENDOR_BILL : VENTURE_TYPE_VENDOR_BILL_LINE,
		venture_entity_get_id(record), error);
	if (stored == NULL)
		return FALSE;
	{
		g_autoptr(VentureEntity) parent = NULL;
		g_autoptr(GDateTime) date = NULL;
		VentureEntity *bill = stored;
		if (VENTURE_IS_VENDOR_BILL_LINE(stored))
		{
			parent = venture_database_get(database, VENTURE_TYPE_VENDOR_BILL, get_id(stored, "bill-id"), error);
			if (parent == NULL)
				return FALSE;
			bill = parent;
		}
		g_object_get(bill, "bill-date", &date, NULL);
		if (!venture_period_guard_is_postable(VENTURE_PERIOD_GUARD(venture_database_get_period_guard(database)),
			database, venture_entity_get_organization_id(bill), date, error))
			return FALSE;
	}
	if (VENTURE_IS_VENDOR_BILL_LINE(record))
		return check_line(venture_payables_service_get(database), stored, NULL, error);
	{
		g_autoptr(JsonNode) diff = NULL;

		diff = venture_entity_diff(stored, record);
		if (json_object_get_size(json_node_get_object(diff)) != 0)
			return refuse(error, VENTURE_ERROR_VALIDATION, "Save bill edits before requesting deletion");
	}
	status = bill_phase(stored);
	if (status != VENTURE_VENDOR_BILL_STATUS_DRAFT)
		return refuse(error, VENTURE_ERROR_VALIDATION, "Issued bills are history; void them through VenturePayablesService");
	return TRUE;
}

gboolean
venture_payables_save_hook(VentureDatabase *database, VentureEntity *record,
	const VentureActor *actor, gboolean *handled, gboolean *authorized, GError **error)
{
	g_autoptr(VentureAccountingOperation) operation = NULL;
	VenturePayablesService *self;
	g_autoptr(VentureEntity) previous = NULL;
	g_autoptr(VentureEntity) original = NULL;
	g_autoptr(VentureEntity) approval = NULL;
	gboolean ok;

	*handled = FALSE;
	*authorized = FALSE;
	if (!is_history(record) && !VENTURE_IS_VENDOR_BILL(record) && !VENTURE_IS_VENDOR_BILL_LINE(record) && !VENTURE_IS_EXPENSE(record))
		return TRUE;
	self = venture_payables_service_get(database);
	if (self->writing == record)
	{
		self->writing = NULL;
		*authorized = TRUE;
		return TRUE;
	}
	if (VENTURE_IS_EXPENSE(record))
		return venture_payables_check_expense(database, record, error);
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
	if (VENTURE_IS_VENDOR_BILL(record) || VENTURE_IS_VENDOR_BILL_LINE(record))
	{
		VentureEntity *versions[2];
		guint i;
		versions[0] = record;
		versions[1] = previous;
		for (i = 0; i < 2; i++)
		{
			g_autoptr(VentureEntity) parent = NULL;
			g_autoptr(GDateTime) date = NULL;
			VentureEntity *bill = versions[i];
			if (bill == NULL)
				continue;
			if (VENTURE_IS_VENDOR_BILL_LINE(bill))
			{
				parent = venture_database_get(database, VENTURE_TYPE_VENDOR_BILL, get_id(bill, "bill-id"), error);
				if (parent == NULL)
					return FALSE;
				bill = parent;
			}
			g_object_get(bill, "bill-date", &date, NULL);
			if (!venture_period_guard_is_postable(VENTURE_PERIOD_GUARD(venture_database_get_period_guard(database)),
				database, venture_entity_get_organization_id(bill), date, error))
				return FALSE;
		}
	}
	if (VENTURE_IS_VENDOR_BILL_LINE(record))
		return check_line(self, record, previous, error);
	if (VENTURE_IS_VENDOR_BILL(record))
		return check_bill_edit(self, record, previous, error);

	*handled = TRUE;
	if (VENTURE_IS_VENDOR_BILL_EVENT(record))
	{
		g_autoptr(VentureEntity) bill = NULL;
		g_autoptr(GDateTime) date = NULL;
		g_autofree gchar *kind = NULL;
		if (previous != NULL)
			return refuse(error, VENTURE_ERROR_VALIDATION, "Bill events are immutable");
		g_object_get(record, "kind", &kind, "date", &date, NULL);
		if (g_strcmp0(kind, "approve") != 0 && g_strcmp0(kind, "void") != 0)
			return refuse(error, VENTURE_ERROR_VALIDATION, "Request approve or void through VenturePayablesService");
		bill = venture_database_get(database, VENTURE_TYPE_VENDOR_BILL, get_id(record, "bill-id"), error);
		if (bill == NULL || !same_owner(record, bill, error))
			return FALSE;
		operation = accounting_operation(self, "payables.save_event", record, NULL, NULL,
			venture_entity_get_organization_id(record), actor, error);
		if (operation == NULL) return FALSE;
		if (!begin_operation(self, "vendor_bill", error))
			return FALSE;
		original = snapshot(record);
		ok = perform_transition(self, bill, VENTURE_VENDOR_BILL_EVENT(record),
			g_str_equal(kind, "approve") ? "approved" : "void", date, actor, error);
		ok = finish_operation(self, ok, error);
		if (ok) ok = venture_accounting_operation_finish(operation, error);
		if (!ok)
			venture_entity_copy_properties_from(record, original, FALSE);
		return ok;
	}
	if (VENTURE_IS_BILL_PAYMENT(record))
		return venture_payables_service_apply_payment(self, VENTURE_BILL_PAYMENT(record), NULL, actor, error);
	operation = accounting_operation(self, "payables.save", record, NULL, NULL,
		venture_entity_get_organization_id(record), actor, error);
	if (operation == NULL) return FALSE;
	if (!venture_accounting_approval_allow(database, "pay", record, NULL, actor, &approval, error))
		return FALSE;
	if (!begin_operation(self, "bill_payment", error))
		return FALSE;
	original = snapshot(record);
	if (VENTURE_IS_BILL_PAYMENT_ALLOCATION(record))
		ok = perform_allocation(self, record, actor, error);
	else if (VENTURE_IS_VENDOR_CREDIT(record))
		ok = perform_credit(self, record, actor, error);
	else if (VENTURE_IS_BILL_REFUND(record))
		ok = perform_refund(self, record, actor, error);
	else
		ok = refuse(error, VENTURE_ERROR_VALIDATION, "Bill events can only be written by VenturePayablesService");
	if (ok)
		ok = venture_accounting_approval_consume(database, approval, actor, error);
	ok = finish_operation(self, ok, error);
	if (ok) ok = venture_accounting_operation_finish(operation, error);
	if (!ok)
		venture_entity_copy_properties_from(record, original, FALSE);
	return ok;
}

static gboolean
settle_bill_impl(VenturePayablesService *self,
	gint64 bill_id, GDateTime *date, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) bill = NULL;
	g_autoptr(VentureMoney) balance = NULL;
	g_autoptr(VentureBillPayment) payment = NULL;
	gboolean ok;

	if (!begin_operation(self, "bill_payment", error))
		return FALSE;
	bill = venture_database_get(self->database, VENTURE_TYPE_VENDOR_BILL, bill_id, error);
	ok = bill != NULL;
	if (ok)
	{
		balance = venture_payables_service_bill_balance(self, bill_id, NULL, error);
		ok = balance != NULL && check_amount(balance, error);
	}
	if (ok)
	{
		payment = venture_bill_payment_new();
		g_object_set(payment, "vendor-id", get_id(bill, "company-id"), "bill-id", bill_id,
			"date", date, "method", "manual", "amount", balance, NULL);
		venture_entity_set_organization_id(VENTURE_ENTITY(payment), venture_entity_get_organization_id(bill));
		ok = perform_payment(self, VENTURE_ENTITY(payment), NULL, actor, error);
	}
	return finish_operation(self, ok, error);
}

/* The command owns approval before constructing any payment or credit. */
gboolean
venture_payables_service_settle_bill(VenturePayablesService *self,
	gint64 bill_id, GDateTime *date, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) subject = NULL;
	g_autoptr(VentureAccountingOperation) operation = NULL;
	subject = venture_database_get(self->database, VENTURE_TYPE_VENDOR_BILL, bill_id, error);
	if (subject == NULL) return FALSE;
	operation = accounting_operation(self, "payables.settle_bill", subject, NULL, operation_date(date, NULL),
		venture_entity_get_organization_id(subject), actor, error);
	if (operation == NULL) return FALSE;
	return settle_bill_impl(self, bill_id, date, actor, error) &&
		venture_accounting_operation_finish(operation, error);
}

VentureMoney *
venture_payables_service_vendor_balance(VenturePayablesService *self,
	gint64 organization_id, gint64 vendor_id, GDateTime *as_of,
	const gchar *currency, GError **error)
{
	GType types[4];
	g_autoptr(VentureMoney) total = NULL;
	guint t;

	types[0] = VENTURE_TYPE_VENDOR_BILL_EVENT;
	types[1] = VENTURE_TYPE_BILL_PAYMENT;
	types[2] = VENTURE_TYPE_VENDOR_CREDIT;
	types[3] = VENTURE_TYPE_BILL_REFUND;
	total = venture_money_new_zero(currency);
	for (t = 0; t < G_N_ELEMENTS(types); t++)
	{
		g_autoptr(GPtrArray) records = NULL;
		guint i;

		records = find_rows(self, types[t], "vendor-id", vendor_id, as_of, error);
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
venture_payables_service_refresh_credit(VenturePayablesService *self, gint64 credit_id,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureAccountingOperation) operation = NULL;
	g_autoptr(VentureEntity) credit = NULL;
	gboolean ok;

	g_return_val_if_fail(VENTURE_IS_PAYABLES_SERVICE(self), FALSE);
	credit = venture_database_get(self->database, VENTURE_TYPE_VENDOR_CREDIT, credit_id, error);
	if (credit == NULL)
		return FALSE;
	operation = accounting_operation(self, "payables.refresh_credit", credit, NULL, NULL,
		venture_entity_get_organization_id(credit), actor, error);
	if (operation == NULL)
		return FALSE;
	if (!begin_operation(self, "bill_payment", error))
		return FALSE;
	ok = update_credit(self, credit, actor, error);
	ok = finish_operation(self, ok, error);
	if (ok)
		ok = venture_accounting_operation_finish(operation, error);
	return ok;
}

VentureEntity *
venture_payables_service_prepare_action(VenturePayablesService *self,
	gint64 bill_id, const gchar *action, JsonNode *options, GError **error)
{
	g_autoptr(VentureEntity) bill = NULL;
	g_autoptr(VentureEntity) request = NULL;
	g_autoptr(GDateTime) date = NULL;
	g_autoptr(VentureMoney) amount = NULL;
	gboolean pay = g_strcmp0(action, "pay") == 0;
	if (!pay && g_strcmp0(action, "approve") != 0 && g_strcmp0(action, "void") != 0)
	{
		refuse(error, VENTURE_ERROR_INVALID_ARGUMENT, "Use approve, pay or void");
		return NULL;
	}
	bill = venture_database_get(self->database, VENTURE_TYPE_VENDOR_BILL, bill_id, error);
	if (bill == NULL)
		return NULL;
	request = g_object_new(pay ? VENTURE_TYPE_BILL_PAYMENT : VENTURE_TYPE_VENDOR_BILL_EVENT, NULL);
	if (options != NULL && !venture_serializable_from_json(VENTURE_SERIALIZABLE(request), options, error))
		return NULL;
	/* The path fixes the document and its legal entity, even when the body
	 * contains reference or identity fields of its own. */
	g_object_set(request, "id", (gint64)0, "organization-id", venture_entity_get_organization_id(bill),
		"bill-id", bill_id, "vendor-id", get_id(bill, "company-id"), NULL);
	g_object_get(request, "date", &date, NULL);
	if (date == NULL)
	{
		/* A repeated HTTP command must keep the same accounting day. */
		date = venture_time_from_string("today", NULL);
		g_object_set(request, "date", date, NULL);
	}
	if (pay)
	{
		g_autofree gchar *method = NULL;
		g_object_get(request, "amount", &amount, "method", &method, NULL);
		if (amount == NULL)
		{
			amount = venture_payables_service_bill_balance(self, bill_id, NULL, error);
			if (amount == NULL)
				return NULL;
			g_object_set(request, "amount", amount, NULL);
		}
		if (venture_string_is_empty(method))
			g_object_set(request, "method", "manual", NULL);
	}
	else
		g_object_set(request, "kind", action, "state", g_str_equal(action, "approve") ? "approved" : "void", NULL);
	return g_steal_pointer(&request);
}

static gboolean
reject_projection_journal(VentureDatabase *db, VentureEntity *journal,
	VentureEntity *previous, gpointer data, GError **error)
{
	g_autofree gchar *source_type = NULL;
	g_autoptr(VentureEntity) expense = NULL;
	gint64 source_id;
	g_object_get(journal, "source-type", &source_type, "source-id", &source_id, NULL);
	if (g_strcmp0(source_type, "expense") != 0 || source_id == 0)
		return TRUE;
	expense = venture_database_get(db, VENTURE_TYPE_EXPENSE, source_id, error);
	if (expense == NULL)
		return FALSE;
	if (is_bill_expense(expense))
		return refuse(error, VENTURE_ERROR_PERMISSION_DENIED, "Bill-line expenses cannot be reposted; bill payments already posted their cash");
	return TRUE;
}

gboolean
venture_payables_expense_hook(VentureDatabase *database, VentureEntity *record,
	const VentureActor *actor, gboolean *handled, GError **error)
{
	g_autoptr(VentureAccountingOperation) operation = NULL;
	VenturePayablesService *self;
	g_autoptr(VentureEntity) line = NULL;
	g_autoptr(VentureEntity) bill = NULL;
	g_autoptr(VentureEntity) original = NULL;
	g_autoptr(GPtrArray) allocations = NULL;
	g_autoptr(VentureMoney) amount = NULL;
	g_autoptr(VentureMoney) balance = NULL;
	g_autoptr(GDateTime) date = NULL;
	g_autofree gchar *external = NULL;
	g_autofree gchar *description = NULL;
	g_autofree gchar *category = NULL;
	guint i;
	gboolean ok = FALSE;
	*handled = FALSE;
	if (!VENTURE_IS_EXPENSE(record))
		return TRUE;
	self = venture_payables_service_get(database);
	if (self->writing == record)
		return TRUE;
	if (venture_entity_is_persisted(record))
		return venture_payables_check_expense(database, record, error);
	if (!is_bill_expense(record))
		return TRUE;
	*handled = TRUE;
	operation = accounting_operation(self, "payables.convert_expense", record, NULL, NULL,
		venture_entity_get_organization_id(record), actor, error);
	if (operation == NULL) return FALSE;
	if (!begin_operation(self, "vendor_bill", error))
		return FALSE;
	original = snapshot(record);
	g_object_get(record, "external-id", &external, NULL);
	if (!g_str_has_prefix(external, "bill_line:"))
	{
		refuse(error, VENTURE_ERROR_PERMISSION_DENIED, "Only VenturePayablesService may create cash adjustments");
		goto done;
	}
	line = venture_database_get_by_uuid(database, VENTURE_TYPE_VENDOR_BILL_LINE, external + strlen("bill_line:"), error);
	if (line == NULL || !same_owner(record, line, error))
		goto done;
	bill = venture_database_get(database, VENTURE_TYPE_VENDOR_BILL, get_id(line, "bill-id"), error);
	if (bill == NULL || bill_phase(bill) != VENTURE_VENDOR_BILL_STATUS_PAID)
	{
		if (error == NULL || *error == NULL)
			refuse(error, VENTURE_ERROR_VALIDATION, "Only a fully paid bill line can become a cash expense");
		goto done;
	}
	balance = venture_payables_service_bill_balance(self, venture_entity_get_id(bill), NULL, error);
	if (balance == NULL || !venture_money_is_zero(balance))
		goto done;
	allocations = find_rows(self, VENTURE_TYPE_BILL_PAYMENT_ALLOCATION, "bill-id", venture_entity_get_id(bill), NULL, error);
	if (allocations == NULL)
		goto done;
	for (i = 0; i < allocations->len; i++)
	{
		VentureEntity *allocation = g_ptr_array_index(allocations, i);
		g_autoptr(VentureEntity) credit = NULL;
		g_autoptr(GPtrArray) refunds = NULL;
		g_autoptr(GDateTime) applied = NULL;
		gint64 payment_id = get_id(allocation, "payment-id");
		if (payment_id == 0)
		{
			credit = venture_database_get(database, VENTURE_TYPE_VENDOR_CREDIT, get_id(allocation, "credit-id"), error);
			if (credit == NULL)
				goto done;
			payment_id = get_id(credit, "payment-id");
		}
		refunds = find_rows(self, VENTURE_TYPE_BILL_REFUND, "allocation-id", venture_entity_get_id(allocation), NULL, error);
		if (refunds == NULL)
			goto done;
		g_object_get(allocation, "date", &applied, NULL);
		if (payment_id == 0 || refunds->len != 0 || (date != NULL && !g_date_time_equal(date, applied)))
		{
			refuse(error, VENTURE_ERROR_VALIDATION, "One immutable bill-line expense requires cash allocations on one date and no refunds; use dated subledger reports otherwise");
			goto done;
		}
		if (date == NULL)
			date = g_date_time_ref(applied);
	}
	if (date == NULL)
	{
		refuse(error, VENTURE_ERROR_VALIDATION, "The bill has no cash allocation");
		goto done;
	}
	amount = venture_vendor_bill_line_get_amount(VENTURE_VENDOR_BILL_LINE(line), error);
	if (amount == NULL)
		goto done;
	g_object_get(line, "description", &description, "category", &category, NULL);
	g_object_set(record, "description", description, "category", category, "amount", amount,
		"occurred-at", date, "venture-id", get_id(bill, "venture-id"), NULL);
	if (!venture_period_guard_is_postable(VENTURE_PERIOD_GUARD(venture_database_get_period_guard(database)),
		database, venture_entity_get_organization_id(record), date, error))
		goto done;
	ok = write_record(self, record, actor, error);
done:
	ok = finish_operation(self, ok, error);
	if (ok) ok = venture_accounting_operation_finish(operation, error);
	if (!ok)
		venture_entity_copy_properties_from(record, original, FALSE);
	return ok;
}

static gboolean
adapter_known(const gchar *adapter)
{
	return g_strcmp0(adapter, "manual") == 0 || g_strcmp0(adapter, "transfer") == 0 ||
		g_strcmp0(adapter, "card") == 0 || g_strcmp0(adapter, "check") == 0 ||
		g_strcmp0(adapter, "ach") == 0;
}

gboolean
venture_payables_service_execute_payment(VenturePayablesService *self,
	const gchar *adapter, VentureBillPayment *payment, GPtrArray *allocations,
	const VentureActor *actor, GError **error)
{
	g_autofree gchar *method = NULL;
	if (adapter == NULL || !adapter_known(adapter))
		return refuse(error, VENTURE_ERROR_VALIDATION,
			"Payment adapter is manual, transfer, card, check or ach");
	g_object_get(payment, "method", &method, NULL);
	if (venture_string_is_empty(method))
		g_object_set(payment, "method", adapter, NULL);
	return venture_payables_service_apply_payment(self, payment, allocations, actor, error);
}

gboolean
venture_payables_service_pay_bills(VenturePayablesService *self, GArray *bill_ids,
	GDateTime *date, const gchar *method, const gchar *adapter, const gchar *reference,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureAccountingOperation) operation = NULL;
	g_autoptr(VentureEntity) subject = NULL;
	g_autofree gchar *approval_date = NULL;
	g_autoptr(GHashTable) groups = NULL;
	g_autoptr(GPtrArray) payments = g_ptr_array_new_with_free_func(g_object_unref);
	g_autoptr(GPtrArray) batches = g_ptr_array_new_with_free_func((GDestroyNotify)g_ptr_array_unref);
	GHashTableIter iter;
	gpointer key;
	gpointer value;
	const gchar *used;
	guint i;
	if (adapter == NULL)
		adapter = "transfer";
	if (!adapter_known(adapter))
		return refuse(error, VENTURE_ERROR_VALIDATION,
			"Payment adapter is manual, transfer, card, check or ach");
	if (bill_ids == NULL || bill_ids->len == 0)
		return refuse(error, VENTURE_ERROR_VALIDATION, "Select at least one approved bill");
	if (date == NULL)
		return refuse(error, VENTURE_ERROR_VALIDATION, "A payment date is required");
	used = !venture_string_is_empty(method) ? method : adapter;
	subject = venture_database_get(self->database, VENTURE_TYPE_VENDOR_BILL,
		g_array_index(bill_ids, gint64, 0), error);
	if (subject == NULL) return FALSE;
	approval_date = g_date_time_format_iso8601(date);
	operation = accounting_operation(self, "payables.pay_bills", subject, NULL,
		g_variant_new("(@axssss)", g_variant_new_fixed_array(G_VARIANT_TYPE_INT64,
			bill_ids->data, bill_ids->len, sizeof(gint64)), approval_date, used, adapter,
			reference != NULL ? reference : ""), venture_entity_get_organization_id(subject), actor, error);
	if (operation == NULL) return FALSE;

	groups = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, (GDestroyNotify)g_ptr_array_unref);
	for (i = 0; i < bill_ids->len; i++)
	{
		gint64 bill_id = g_array_index(bill_ids, gint64, i);
		g_autoptr(VentureEntity) bill = NULL;
		g_autoptr(VentureMoney) balance = NULL;
		g_autofree gchar *status = NULL;
		GPtrArray *bucket;
		gint64 vendor;
		bill = venture_database_get(self->database, VENTURE_TYPE_VENDOR_BILL, bill_id, error);
		if (bill == NULL || !same_owner(subject, bill, error))
			return FALSE;
		g_object_get(bill, "status", &status, NULL);
		if (g_strcmp0(status, "approved") != 0 && g_strcmp0(status, "partially_paid") != 0)
		{
			refuse(error, VENTURE_ERROR_VALIDATION, "The workbench pays approved or partially paid bills");
			return FALSE;
		}
		balance = venture_payables_service_bill_balance(self, bill_id, NULL, error);
		if (balance == NULL)
			return FALSE;
		if (venture_money_is_zero(balance))
		{
			refuse(error, VENTURE_ERROR_VALIDATION, "A selected bill has no outstanding balance");
			return FALSE;
		}
		vendor = get_id(bill, "company-id");
		bucket = g_hash_table_lookup(groups, (gpointer)(guintptr)vendor);
		if (bucket == NULL)
		{
			bucket = g_ptr_array_new_with_free_func(g_object_unref);
			g_hash_table_insert(groups, (gpointer)(guintptr)vendor, bucket);
		}
		{
			VentureEntity *allocation = VENTURE_ENTITY(venture_bill_payment_allocation_new());
			g_object_set(allocation, "bill-id", bill_id, "amount", balance, "date", date, NULL);
			venture_entity_set_organization_id(allocation, venture_entity_get_organization_id(bill));
			g_ptr_array_add(bucket, allocation);
		}
	}
	g_hash_table_iter_init(&iter, groups);
	while (g_hash_table_iter_next(&iter, &key, &value))
	{
		GPtrArray *bucket = value;
		g_autoptr(VentureBillPayment) payment = venture_bill_payment_new();
		g_autoptr(VentureMoney) total = NULL;
		guint a;
		for (a = 0; a < bucket->len; a++)
		{
			g_autoptr(VentureMoney) amount = NULL;
			g_object_get(g_ptr_array_index(bucket, a), "amount", &amount, NULL);
			if (total == NULL)
				total = venture_money_copy(amount);
			else if (!accumulate(&total, amount, FALSE, error))
				return FALSE;
		}
		g_object_set(payment, "vendor-id", (gint64)(guintptr)key, "date", date, "amount", total,
			"method", used, "reference", reference, NULL);
		venture_entity_set_organization_id(VENTURE_ENTITY(payment),
			venture_entity_get_organization_id(g_ptr_array_index(bucket, 0)));
		{
			g_autoptr(VentureEntity) approval = NULL;

			/* Consent must survive a refusal. Build every proposal before
			 * opening the all-or-nothing payment transaction. */
			if (!venture_accounting_approval_allow(self->database, "pay",
				VENTURE_ENTITY(payment), bucket, actor, &approval, error))
				return FALSE;
		}
		g_ptr_array_add(payments, g_steal_pointer(&payment));
		g_ptr_array_add(batches, g_ptr_array_ref(bucket));
	}
	if (!venture_database_begin(self->database, error))
		return FALSE;
	for (i = 0; i < payments->len; i++)
	{
		/* Reuse the guarded operation, including approval consumption and
		 * live balance checks; an error rolls back earlier vendor payments. */
		if (!venture_payables_service_apply_payment(self, g_ptr_array_index(payments, i),
			g_ptr_array_index(batches, i), actor, error))
		{
			venture_database_rollback(self->database);
			return FALSE;
		}
	}
	return venture_database_commit(self->database, error) &&
		venture_accounting_operation_finish(operation, error);
}
