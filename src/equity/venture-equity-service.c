/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

struct _VentureCapitalService
{
	GObject parent_instance;
	VentureDatabase *database;
	VentureEntity *permit;
};
G_DEFINE_FINAL_TYPE(VentureCapitalService, venture_capital_service, G_TYPE_OBJECT)

static void
set_property(GObject *object, guint id, const GValue *value, GParamSpec *spec)
{
	if (id == 1)
		VENTURE_CAPITAL_SERVICE(object)->database = g_value_get_object(value);
	else
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
}

static void
get_property(GObject *object, guint id, GValue *value, GParamSpec *spec)
{
	if (id == 1)
		g_value_set_object(value, VENTURE_CAPITAL_SERVICE(object)->database);
	else
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
}

static void
venture_capital_service_class_init(VentureCapitalServiceClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	object_class->get_property = get_property;
	object_class->set_property = set_property;
	g_object_class_install_property(object_class, 1,
		g_param_spec_object("database", "Database", "Owning repository", VENTURE_TYPE_DATABASE,
			G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
}

static void
venture_capital_service_init(VentureCapitalService *self)
{
	(void)self;
}

VentureCapitalService *
venture_capital_service_get(VentureDatabase *database)
{
	VentureCapitalService *self;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	self = g_object_get_data(G_OBJECT(database), "venture-capital-service");
	if (self == NULL)
	{
		self = g_object_new(VENTURE_TYPE_CAPITAL_SERVICE, "database", database, NULL);
		g_object_set_data_full(G_OBJECT(database), "venture-capital-service", self, g_object_unref);
	}
	return self;
}

static gboolean
enabled(GError **error)
{
	if (venture_entity_registry_is_type_enabled(venture_entity_registry_get_default(), "equity_transaction"))
		return TRUE;
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED, "The equity module is disabled");
	return FALSE;
}

gboolean
venture_equity_check_write(VentureDatabase *database, VentureEntity *record,
	gboolean removal, GError **error)
{
	VentureCapitalService *self;
	if (!VENTURE_IS_EQUITY_TRANSACTION(record))
		return TRUE;
	if (!enabled(error))
		return FALSE;
	self = venture_capital_service_get(database);
	if (self->permit == record)
	{
		self->permit = NULL;
		return TRUE;
	}
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		"Owner equity, loans and transfers require VentureCapitalService");
	(void)removal;
	return FALSE;
}

static gboolean
refuse_expense(VentureDatabase *database, gint64 account_id, GError **error)
{
	g_autoptr(VentureEntity) account = NULL;
	gint kind;
	if (account_id <= 0)
		return TRUE;
	account = venture_database_get(database, VENTURE_TYPE_ACCOUNT, account_id, error);
	if (account == NULL)
		return FALSE;
	g_object_get(account, "kind", &kind, NULL);
	if (kind == VENTURE_ACCOUNT_KIND_EXPENSE || kind == VENTURE_ACCOUNT_KIND_INCOME)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"Refusing an unlabeled expense-account dump; map cash, equity or liability");
		return FALSE;
	}
	return TRUE;
}

static gint64
mapped(VentureDatabase *database, gint64 organization_id, const gchar *classification, GError **error)
{
	gint64 id = venture_setup_resolve_account(database, organization_id, classification, "organization", 0, NULL, error);
	if (id <= 0)
	{
		if (error != NULL && *error == NULL)
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
				"Map a %s control account before posting owner equity", classification);
		return 0;
	}
	return id;
}

static gboolean
post_pair(VentureDatabase *database, VentureEntity *source, GDateTime *when,
	gint64 debit, gint64 credit, const VentureMoney *amount, const VentureActor *actor, GError **error)
{
	g_autoptr(GPtrArray) entries = g_ptr_array_new_with_free_func(g_object_unref);
	g_autofree gchar *transaction = g_strdup_printf("equity:%" G_GINT64_FORMAT, venture_entity_get_id(source));
	VentureEntity *debit_entry = VENTURE_ENTITY(venture_ledger_entry_new());
	VentureEntity *credit_entry = VENTURE_ENTITY(venture_ledger_entry_new());
	g_object_set(debit_entry, "organization-id", venture_entity_get_organization_id(source),
		"source-type", "equity_transaction", "source-id", venture_entity_get_id(source),
		"transaction-id", transaction, "occurred-at", when, "account-id", debit,
		"side", VENTURE_LEDGER_SIDE_DEBIT, "amount", amount, NULL);
	g_object_set(credit_entry, "organization-id", venture_entity_get_organization_id(source),
		"source-type", "equity_transaction", "source-id", venture_entity_get_id(source),
		"transaction-id", transaction, "occurred-at", when, "account-id", credit,
		"side", VENTURE_LEDGER_SIDE_CREDIT, "amount", amount, NULL);
	g_ptr_array_add(entries, debit_entry);
	g_ptr_array_add(entries, credit_entry);
	return venture_posting_service_post_entries(venture_database_get_posting_service(database),
		entries, NULL, actor, error);
}

static VentureEntity *
venture_capital_service_post_impl(VentureCapitalService *self, gint64 organization_id,
	VentureEquityKind kind, const VentureMoney *amount, GDateTime *when, const gchar *memo,
	gint64 debit_account_id, gint64 credit_account_id, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) row = NULL;
	g_autoptr(GPtrArray) journals = NULL;
	g_autoptr(GDateTime) effective = NULL;
	gint64 debit = debit_account_id;
	gint64 credit = credit_account_id;
	g_return_val_if_fail(VENTURE_IS_CAPITAL_SERVICE(self), NULL);
	if (!enabled(error))
		return NULL;
	if (organization_id <= 0 || amount == NULL || amount->amount <= 0)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"An equity posting needs a legal entity and a positive amount");
		return NULL;
	}
	effective = when != NULL ? g_date_time_ref(when) : venture_time_now();
	when = effective;
	if (kind == VENTURE_EQUITY_KIND_CONTRIBUTION)
	{
		if (debit == 0)
			debit = mapped(self->database, organization_id, "cash", error);
		if (credit == 0)
			credit = mapped(self->database, organization_id, "retained_earnings", error);
	}
	else if (kind == VENTURE_EQUITY_KIND_DRAW)
	{
		if (debit == 0)
			debit = mapped(self->database, organization_id, "owner_draws", error);
		if (credit == 0)
			credit = mapped(self->database, organization_id, "cash", error);
	}
	else if (kind == VENTURE_EQUITY_KIND_LOAN_PROCEED)
	{
		if (debit == 0)
			debit = mapped(self->database, organization_id, "cash", error);
		if (credit == 0)
			credit = mapped(self->database, organization_id, "loans", error);
	}
	else if (kind == VENTURE_EQUITY_KIND_LOAN_PAYMENT)
	{
		if (debit == 0)
			debit = mapped(self->database, organization_id, "loans", error);
		if (credit == 0)
			credit = mapped(self->database, organization_id, "cash", error);
	}
	else if (kind == VENTURE_EQUITY_KIND_TRANSFER)
	{
		if (debit == 0)
			debit = mapped(self->database, organization_id, "cash", error);
		if (credit == 0)
			credit = mapped(self->database, organization_id, "cash", error);
	}
	if (debit <= 0 || credit <= 0)
		return NULL;
	if (!refuse_expense(self->database, debit, error) || !refuse_expense(self->database, credit, error))
		return NULL;
	if (!venture_database_begin(self->database, error))
		return NULL;
	row = VENTURE_ENTITY(venture_equity_transaction_new());
	g_object_set(row, "organization-id", organization_id, "kind", kind, "amount", amount,
		"occurred-at", when, "memo", memo, "debit-account-id", debit, "credit-account-id", credit, NULL);
	self->permit = row;
	if (!venture_database_save(self->database, row, actor, error))
		goto fail;
	if (!post_pair(self->database, row, when, debit, credit, amount, actor, error))
		goto fail;
	journals = venture_posting_service_find_source(venture_database_get_posting_service(self->database),
		"equity_transaction", venture_entity_get_id(row), organization_id, error);
	if (journals == NULL || journals->len != 1)
		goto fail;
	g_object_set(row, "journal-id", venture_entity_get_id(g_ptr_array_index(journals, 0)), NULL);
	self->permit = row;
	if (!venture_database_save(self->database, row, actor, error) || !venture_database_commit(self->database, error))
		goto fail;
	return g_steal_pointer(&row);
fail:
	self->permit = NULL;
	venture_database_rollback(self->database);
	return NULL;
}

/* Bind consent before this operation creates derived rows or enters nested
 * transactions. All generated financial effects share this root proposal. */
VentureEntity *
venture_capital_service_post(VentureCapitalService *self, gint64 organization_id,
	VentureEquityKind kind, const VentureMoney *amount, GDateTime *when, const gchar *memo,
	gint64 debit_account_id, gint64 credit_account_id, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureAccountingOperation) operation = NULL;
	VentureDatabase * db = self->database;
	GVariantBuilder arguments;
	g_autoptr(VentureEntity) result = NULL;
	g_autofree gchar *amount_text = NULL;
	g_autofree gchar *when_text = NULL;
	if (!enabled(error))
		return NULL;
	if (organization_id <= 0 || amount == NULL || amount->amount <= 0)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"An equity posting needs a legal entity and a positive amount");
		return NULL;
	}
	if (db == NULL)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Database is unavailable");
		return NULL;
	}
	amount_text = amount != NULL ? venture_money_to_string(amount) : NULL;
	when_text = when != NULL ? g_date_time_format_iso8601(when) : NULL;
	g_variant_builder_init(&arguments, G_VARIANT_TYPE_VARDICT);
	g_variant_builder_add(&arguments, "{sv}", "organization_id", g_variant_new_int64((gint64)organization_id));
	g_variant_builder_add(&arguments, "{sv}", "kind", g_variant_new_int64((gint64)kind));
	g_variant_builder_add(&arguments, "{sv}", "amount", g_variant_new_maybe(G_VARIANT_TYPE_STRING, amount_text != NULL ? g_variant_new_string(amount_text) : NULL));
	g_variant_builder_add(&arguments, "{sv}", "when", g_variant_new_maybe(G_VARIANT_TYPE_STRING, when_text != NULL ? g_variant_new_string(when_text) : NULL));
	g_variant_builder_add(&arguments, "{sv}", "memo", g_variant_new_maybe(G_VARIANT_TYPE_STRING, memo != NULL ? g_variant_new_string(memo) : NULL));
	g_variant_builder_add(&arguments, "{sv}", "debit_account_id", g_variant_new_int64((gint64)debit_account_id));
	g_variant_builder_add(&arguments, "{sv}", "credit_account_id", g_variant_new_int64((gint64)credit_account_id));
	operation = venture_accounting_operation_begin(db, "capital-post", NULL, NULL,
		g_variant_builder_end(&arguments), organization_id, actor, error);
	if (operation == NULL)
		return NULL;
	result = venture_capital_service_post_impl(self, organization_id, kind, amount, when, memo, debit_account_id, credit_account_id, actor, error);
	if (result == NULL)
		return NULL;
	if (!venture_accounting_operation_finish(operation, error))
		return NULL;
	return g_steal_pointer(&result);
}
