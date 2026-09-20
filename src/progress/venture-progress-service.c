/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
#include <string.h>

struct _VentureProgressService
{
	GObject parent_instance;
	VentureDatabase *database;
	VentureEntity *writing;
};
G_DEFINE_FINAL_TYPE(VentureProgressService, venture_progress_service, G_TYPE_OBJECT)

static gboolean
refuse(GError **error, const gchar *message)
{
	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "VentureProgressService: %s", message);
	return FALSE;
}

static void
get_property(GObject *object, guint id, GValue *value, GParamSpec *spec)
{
	if (id == 1)
		g_value_set_object(value, VENTURE_PROGRESS_SERVICE(object)->database);
	else
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
}

static void
set_property(GObject *object, guint id, const GValue *value, GParamSpec *spec)
{
	VentureProgressService *self = VENTURE_PROGRESS_SERVICE(object);
	if (id == 1)
	{
		self->database = g_value_get_object(value);
		if (self->database != NULL)
			g_object_add_weak_pointer(G_OBJECT(self->database), (gpointer *)&self->database);
	}
	else
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
}

static void
finalize(GObject *object)
{
	VentureProgressService *self = VENTURE_PROGRESS_SERVICE(object);
	if (self->database != NULL)
		g_object_remove_weak_pointer(G_OBJECT(self->database), (gpointer *)&self->database);
	G_OBJECT_CLASS(venture_progress_service_parent_class)->finalize(object);
}

static void
venture_progress_service_class_init(VentureProgressServiceClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	object_class->get_property = get_property;
	object_class->set_property = set_property;
	object_class->finalize = finalize;
	g_object_class_install_property(object_class, 1,
		g_param_spec_object("database", "Database", "Owning database", VENTURE_TYPE_DATABASE,
			G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
}

static void
venture_progress_service_init(VentureProgressService *self)
{
	(void)self;
}

VentureProgressService *
venture_progress_service_get(VentureDatabase *database)
{
	VentureProgressService *self;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	self = g_object_get_data(G_OBJECT(database), "venture-progress-service");
	if (self == NULL)
	{
		self = g_object_new(VENTURE_TYPE_PROGRESS_SERVICE, "database", database, NULL);
		g_object_set_data_full(G_OBJECT(database), "venture-progress-service", self, g_object_unref);
	}
	return self;
}

gboolean
venture_progress_check_write(VentureDatabase *database, VentureEntity *record,
	gboolean removal, GError **error)
{
	const gchar *name;
	VentureProgressService *self;
	(void)removal;
	if (record == NULL || database == NULL)
		return TRUE;
	name = venture_entity_get_entity_name(record);
	if (g_strcmp0(name, "progress_billing") != 0 && g_strcmp0(name, "customer_retainer") != 0 &&
		g_strcmp0(name, "contract_retention") != 0)
		return TRUE;
	self = venture_progress_service_get(database);
	if (self->writing == record)
		return TRUE;
	return refuse(error, "progress, retainer and retention rows are owned by VentureProgressService");
}

static gint64
account_code(VentureProgressService *self, gint64 org, const gchar *role, const gchar *code, GError **error)
{
	g_autoptr(GError) local = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureEntity) row = NULL;
	g_autofree gchar *scoped = NULL;
	gint64 id;
	id = venture_setup_resolve_account(self->database, org, role, "organization", 0, NULL, &local);
	if (local != NULL)
	{
		g_propagate_error(error, g_steal_pointer(&local));
		return 0;
	}
	if (id != 0)
		return id;
	query = venture_query_new(VENTURE_TYPE_ACCOUNT);
	venture_query_set_organization(query, org);
	venture_query_add_filter_string(query, "code", VENTURE_FILTER_OP_EQ, code, NULL);
	row = venture_database_find_one(self->database, query, error);
	if (row != NULL)
		return venture_entity_get_id(row);
	if (error != NULL && *error != NULL)
		return 0;
	scoped = g_strdup_printf("%" G_GINT64_FORMAT ":%s", org, code);
	g_clear_object(&query);
	query = venture_query_new(VENTURE_TYPE_ACCOUNT);
	venture_query_set_organization(query, org);
	venture_query_add_filter_string(query, "code", VENTURE_FILTER_OP_EQ, scoped, NULL);
	row = venture_database_find_one(self->database, query, error);
	if (row == NULL)
	{
		refuse(error, "required control account is missing");
		return 0;
	}
	return venture_entity_get_id(row);
}

static gboolean
post_pair(VentureProgressService *self, gint64 org, const gchar *source_type, gint64 source_id,
	gint64 debit_id, gint64 credit_id, const VentureMoney *amount, const gchar *memo,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureJournal) journal = venture_journal_new();
	g_autoptr(VentureJournalLine) debit = venture_journal_line_new();
	g_autoptr(VentureJournalLine) credit = venture_journal_line_new();
	g_autoptr(GPtrArray) lines = g_ptr_array_new_with_free_func(g_object_unref);
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(VentureJournal) posted = NULL;
	g_object_set(journal, "source-type", source_type, "source-id", source_id, "occurred-at", now,
		"currency", venture_money_get_currency(amount), "organization-id", org, "memo", memo, NULL);
	g_object_set(debit, "account-id", debit_id, "side", VENTURE_LEDGER_SIDE_DEBIT, "amount", amount,
		"organization-id", org, NULL);
	g_object_set(credit, "account-id", credit_id, "side", VENTURE_LEDGER_SIDE_CREDIT, "amount", amount,
		"organization-id", org, NULL);
	g_ptr_array_add(lines, g_object_ref(debit));
	g_ptr_array_add(lines, g_object_ref(credit));
	posted = venture_posting_service_post(venture_database_get_posting_service(self->database),
		journal, lines, NULL, actor, error);
	return posted != NULL;
}

static gboolean
save_owned(VentureProgressService *self, VentureEntity *record, const VentureActor *actor, GError **error)
{
	gboolean ok;
	self->writing = record;
	ok = venture_database_save(self->database, record, actor, error);
	self->writing = NULL;
	return ok;
}


static gboolean
add_money(VentureMoney **sum, VentureMoney *value, GError **error)
{
	VentureMoney *next = venture_money_add(*sum, value, error);
	if (next == NULL)
		return FALSE;
	venture_money_free(*sum);
	*sum = next;
	return TRUE;
}

VentureMoney *
venture_progress_service_remaining(VentureProgressService *self, VentureQuote *quote, GError **error)
{
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(VentureMoney) total = NULL;
	g_autoptr(VentureMoney) billed = NULL;
	guint i;
	g_return_val_if_fail(VENTURE_IS_PROGRESS_SERVICE(self), NULL);
	g_return_val_if_fail(VENTURE_IS_QUOTE(quote), NULL);
	g_object_get(quote, "total", &total, NULL);
	if (total == NULL)
		return refuse(error, "quote has no total"), NULL;
	billed = venture_money_new_zero(venture_money_get_currency(total));
	query = venture_query_new(VENTURE_TYPE_PROGRESS_BILLING);
	venture_query_set_organization(query, venture_entity_get_organization_id(VENTURE_ENTITY(quote)));
	venture_query_set_limit(query, 0);
	venture_query_add_filter_int(query, "quote-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(VENTURE_ENTITY(quote)), NULL);
	rows = venture_database_find(self->database, query, error);
	if (rows == NULL)
		return NULL;
	for (i = 0; i < rows->len; i++)
	{
		g_autoptr(VentureMoney) slice = NULL;
		g_object_get(g_ptr_array_index(rows, i), "amount", &slice, NULL);
		if (slice != NULL && !add_money(&billed, slice, error))
			return NULL;
	}
	return venture_money_subtract(total, billed, error);
}
static VentureEntity *
venture_progress_service_invoice_impl(VentureProgressService *self, VentureQuote *quote, gint64 percent,
	const VentureMoney *amount, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureMoney) remaining = NULL;
	g_autoptr(VentureMoney) total = NULL;
	g_autoptr(VentureMoney) slice = NULL;
	g_autoptr(VentureInvoice) invoice = NULL;
	g_autoptr(VentureInvoiceLine) line = NULL;
	g_autoptr(VentureProgressBilling) billing = NULL;
	g_autoptr(GDateTime) now = NULL;
	g_autoptr(GDateTime) issue_date = NULL;
	g_autofree gchar *number = NULL;
	g_autofree gchar *quote_number = NULL;
	gint64 org, company;
	gint status;
	g_return_val_if_fail(VENTURE_IS_PROGRESS_SERVICE(self), NULL);
	g_object_get(quote, "status", &status, "total", &total, "number", &quote_number, "company-id", &company, NULL);
	if (status != VENTURE_QUOTE_ACCEPTED)
	{
		refuse(error, "progress invoicing requires an accepted quote");
		return NULL;
	}
	org = venture_entity_get_organization_id(VENTURE_ENTITY(quote));
	remaining = venture_progress_service_remaining(self, quote, error);
	if (remaining == NULL)
		return NULL;
	if (amount != NULL)
		slice = venture_money_copy((VentureMoney *)amount);
	else
	{
		if (percent < 1 || percent > 100)
		{
			refuse(error, "progress percent must be 1..100");
			return NULL;
		}
		slice = venture_money_multiply_rational(total, percent, 100, error);
	}
	if (slice == NULL)
		return NULL;
	{
		g_autoptr(VentureMoney) checked = venture_money_subtract(remaining, slice, error);
		if (checked == NULL)
			return NULL;
	}
	if (venture_money_get_amount(slice) <= 0 || venture_money_get_amount(slice) > venture_money_get_amount(remaining))
	{
		refuse(error, "progress amount exceeds remaining contract value");
		return NULL;
	}
	if (!venture_database_begin(self->database, error))
		return NULL;
	invoice = venture_invoice_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(invoice), org);
	/* Repeated equal instalments are valid; the percentage is not a document identity. */
	number = g_strdup_printf("PROG-%s-%s", quote_number, venture_entity_get_uuid(VENTURE_ENTITY(invoice)));
	g_object_set(invoice, "number", number, "company-id", company, NULL);
	if (!venture_database_save(self->database, VENTURE_ENTITY(invoice), actor, error))
		goto fail;
	line = venture_invoice_line_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(line), org);
	g_object_set(line, "invoice-id", venture_entity_get_id(VENTURE_ENTITY(invoice)),
		"description", "Progress billing", "quantity", 1.0, "unit-price", slice, "position", (gint64)1, NULL);
	if (!venture_database_save(self->database, VENTURE_ENTITY(line), actor, error))
		goto fail;
	now = venture_time_now();
	/* Business dates match date-picker receipts; creation/billing retain
	 * precise timestamps separately from the invoice's accounting date. */
	issue_date = venture_time_from_string("today", error);
	if (issue_date == NULL)
		goto fail;
	if (!venture_settlement_service_transition(venture_settlement_service_get(self->database),
		invoice, "sent", issue_date, actor, error))
		goto fail;
	billing = venture_progress_billing_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(billing), org);
	g_object_set(billing, "quote-id", venture_entity_get_id(VENTURE_ENTITY(quote)),
		"invoice-id", venture_entity_get_id(VENTURE_ENTITY(invoice)), "kind", "progress",
		"percent", percent, "amount", slice, "billed-at", now, NULL);
	if (!save_owned(self, VENTURE_ENTITY(billing), actor, error))
		goto fail;
	if (!venture_project_service_record_progress(venture_project_service_get(self->database),
		VENTURE_ENTITY(billing), actor, error))
		goto fail;
	if (!venture_database_commit(self->database, error))
		goto fail;
	return VENTURE_ENTITY(g_steal_pointer(&invoice));
fail:
	venture_database_rollback(self->database);
	return NULL;
}

static VentureEntity *
venture_progress_service_collect_retainer_impl(VentureProgressService *self, gint64 organization_id,
	gint64 company_id, gint64 liability_account_id, const VentureMoney *amount,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureCustomerRetainer) retainer = NULL;
	g_autoptr(VentureEntity) liability = NULL;
	gint kind = 0;
	gboolean active = FALSE;
	gint64 cash;
	g_autoptr(GDateTime) now = NULL;
	g_return_val_if_fail(VENTURE_IS_PROGRESS_SERVICE(self), NULL);
	if (amount == NULL || venture_money_get_amount(amount) <= 0)
	{
		refuse(error, "retainer amount must be positive");
		return NULL;
	}
	if (!venture_database_begin(self->database, error))
		return NULL;
	liability = venture_database_get(self->database, VENTURE_TYPE_ACCOUNT, liability_account_id, error);
	if (liability == NULL)
		goto fail;
	g_object_get(liability, "kind", &kind, "active", &active, NULL);
	if (venture_entity_get_organization_id(liability) != organization_id ||
		venture_entity_is_deleted(liability) || !active || kind != VENTURE_ACCOUNT_KIND_LIABILITY)
	{
		refuse(error, "retainers require an active liability account in the same organization");
		goto fail;
	}
	cash = account_code(self, organization_id, "cash", "1000", error);
	if (cash == 0)
		goto fail;
	retainer = venture_customer_retainer_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(retainer), organization_id);
	now = venture_time_now();
	g_object_set(retainer, "company-id", company_id, "liability-account-id", liability_account_id,
		"amount", amount, "remaining", amount, "collected-at", now, NULL);
	if (!save_owned(self, VENTURE_ENTITY(retainer), actor, error))
		goto fail;
	if (!post_pair(self, organization_id, "customer_retainer", venture_entity_get_id(VENTURE_ENTITY(retainer)),
		cash, liability_account_id, amount, "Retainer collected", actor, error))
		goto fail;
	if (!venture_database_commit(self->database, error))
		goto fail;
	return VENTURE_ENTITY(g_steal_pointer(&retainer));
fail:
	venture_database_rollback(self->database);
	return NULL;
}

static gboolean
venture_progress_service_release_retainer_impl(VentureProgressService *self, VentureCustomerRetainer *retainer,
	const VentureMoney *amount, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureMoney) remaining = NULL;
	g_autoptr(VentureMoney) next = NULL;
	g_autoptr(GDateTime) now = NULL;
	gint64 org, liability, income;
	g_return_val_if_fail(VENTURE_IS_PROGRESS_SERVICE(self), FALSE);
	g_object_get(retainer, "remaining", &remaining, "liability-account-id", &liability, NULL);
	if (amount == NULL || remaining == NULL || venture_money_get_amount(amount) <= 0 ||
		venture_money_get_amount(amount) > venture_money_get_amount(remaining))
		return refuse(error, "release cannot exceed remaining retainer");
	org = venture_entity_get_organization_id(VENTURE_ENTITY(retainer));
	if (!venture_database_begin(self->database, error))
		return FALSE;
	income = account_code(self, org, "income", "4000", error);
	if (income == 0)
		goto fail;
	if (!post_pair(self, org, "customer_retainer", venture_entity_get_id(VENTURE_ENTITY(retainer)),
		liability, income, amount, "Retainer released", actor, error))
		goto fail;
	next = venture_money_subtract(remaining, amount, error);
	if (next == NULL)
		goto fail;
	now = venture_time_now();
	g_object_set(retainer, "remaining", next, "released-at", now, NULL);
	if (!save_owned(self, VENTURE_ENTITY(retainer), actor, error) ||
		!venture_database_commit(self->database, error))
		goto fail;
	return TRUE;
fail:
	venture_database_rollback(self->database);
	return FALSE;
}

static VentureEntity *
venture_progress_service_hold_retention_impl(VentureProgressService *self, VentureQuote *quote,
	gint64 liability_account_id, const VentureMoney *amount, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureContractRetention) retention = NULL;
	g_autoptr(GDateTime) now = NULL;
	gint64 org, ar;
	gint status;
	g_return_val_if_fail(VENTURE_IS_PROGRESS_SERVICE(self), NULL);
	g_object_get(quote, "status", &status, NULL);
	if (status != VENTURE_QUOTE_ACCEPTED)
	{
		refuse(error, "retention requires an accepted quote");
		return NULL;
	}
	if (amount == NULL || venture_money_get_amount(amount) <= 0)
	{
		refuse(error, "retention amount must be positive");
		return NULL;
	}
	org = venture_entity_get_organization_id(VENTURE_ENTITY(quote));
	if (!venture_database_begin(self->database, error))
		return NULL;
	ar = account_code(self, org, "income", "4000", error);
	if (ar == 0)
		goto fail;
	retention = venture_contract_retention_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(retention), org);
	now = venture_time_now();
	g_object_set(retention, "quote-id", venture_entity_get_id(VENTURE_ENTITY(quote)),
		"liability-account-id", liability_account_id, "amount", amount, "remaining", amount,
		"held-at", now, NULL);
	if (!save_owned(self, VENTURE_ENTITY(retention), actor, error))
		goto fail;
	if (!post_pair(self, org, "contract_retention", venture_entity_get_id(VENTURE_ENTITY(retention)),
		ar, liability_account_id, amount, "Retention held", actor, error))
		goto fail;
	if (!venture_database_commit(self->database, error))
		goto fail;
	return VENTURE_ENTITY(g_steal_pointer(&retention));
fail:
	venture_database_rollback(self->database);
	return NULL;
}

static gboolean
venture_progress_service_release_retention_impl(VentureProgressService *self, VentureContractRetention *retention,
	const VentureMoney *amount, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureMoney) remaining = NULL;
	g_autoptr(VentureMoney) next = NULL;
	g_autoptr(GDateTime) now = NULL;
	gint64 org, liability, income;
	g_return_val_if_fail(VENTURE_IS_PROGRESS_SERVICE(self), FALSE);
	g_object_get(retention, "remaining", &remaining, "liability-account-id", &liability, NULL);
	if (amount == NULL || remaining == NULL || venture_money_get_amount(amount) <= 0 ||
		venture_money_get_amount(amount) > venture_money_get_amount(remaining))
		return refuse(error, "release cannot exceed remaining retention");
	org = venture_entity_get_organization_id(VENTURE_ENTITY(retention));
	if (!venture_database_begin(self->database, error))
		return FALSE;
	income = account_code(self, org, "income", "4000", error);
	if (income == 0)
		goto fail;
	if (!post_pair(self, org, "contract_retention", venture_entity_get_id(VENTURE_ENTITY(retention)),
		liability, income, amount, "Retention released", actor, error))
		goto fail;
	next = venture_money_subtract(remaining, amount, error);
	if (next == NULL)
		goto fail;
	now = venture_time_now();
	g_object_set(retention, "remaining", next, "released-at", now, NULL);
	if (!save_owned(self, VENTURE_ENTITY(retention), actor, error) ||
		!venture_database_commit(self->database, error))
		goto fail;
	return TRUE;
fail:
	venture_database_rollback(self->database);
	return FALSE;
}

static gboolean
retainer_allowed(VentureAction *action, VentureEntity *entity,
	const VentureActor *actor, GError **error)
{
	static const gint roles[] = { VENTURE_ORGANIZATION_ROLE_OWNER,
		VENTURE_ORGANIZATION_ROLE_ADMIN, VENTURE_ORGANIZATION_ROLE_FINANCE };
	VentureProgressService *self = venture_action_get_data(action);
	VentureAccessPolicy *policy = venture_database_get_access_policy(self->database);
	const VentureAuthPrincipal *principal = venture_access_policy_get_actor(policy);
	(void)actor;
	if (!venture_action_require_organization(action, entity, self->database, error))
		return FALSE;
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "customer_retainer") == G_TYPE_INVALID)
		return refuse(error, "the quotes module is disabled");
	if (principal && !venture_access_policy_has_organization_role(policy, principal,
		venture_entity_get_organization_id(entity), roles, G_N_ELEMENTS(roles)))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED,
			"Organization finance authorization is required");
		return FALSE;
	}
	return TRUE;
}

static VentureEntity *
retainer_invoke(VentureAction *action, VentureEntity *entity, GHashTable *parameters,
	const VentureActor *actor, GError **error)
{
	VentureProgressService *self = venture_action_get_data(action);
	g_autoptr(VentureMoney) amount = venture_money_from_json(g_hash_table_lookup(parameters, "amount"), NULL, error);
	if (amount == NULL)
		return NULL;
	if (VENTURE_IS_COMPANY(entity))
	{
		JsonNode *account = g_hash_table_lookup(parameters, "liability_account_id");
		return venture_progress_service_collect_retainer(self, venture_entity_get_organization_id(entity),
			venture_entity_get_id(entity), json_node_get_int(account), amount, actor, error);
	}
	if (!venture_progress_service_release_retainer(self, VENTURE_CUSTOMER_RETAINER(entity), amount, actor, error))
		return NULL;
	return venture_database_get(self->database, G_OBJECT_TYPE(entity), venture_entity_get_id(entity), error);
}

static void
retainer_actions_register(VentureDatabase *database)
{
	static const struct { const gchar *type, *name, *label, *description; } actions[] = {
		{ "company", "collect_retainer", "Collect customer retainer", "Record received cash against a customer liability; this does not charge a payment provider" },
		{ "customer_retainer", "release", "Release earned retainer", "Recognize earned revenue from the remaining liability; this does not settle an invoice" }
	};
	guint i;
	for (i = 0; i < G_N_ELEMENTS(actions); i++)
	{
		g_autoptr(GPtrArray) parameters = g_ptr_array_new_with_free_func((GDestroyNotify)venture_field_spec_free);
		g_autoptr(VentureAction) action = NULL;
		g_autoptr(GError) error = NULL;
		VentureFieldSpec *field = venture_field_spec_new("amount", "Amount with currency", VENTURE_FIELD_KIND_MONEY);
		field->required = TRUE;
		g_ptr_array_add(parameters, field);
		if (i == 0)
		{
			field = venture_field_spec_new("liability_account_id", "Customer liability account", VENTURE_FIELD_KIND_REFERENCE);
			field->reference_type = g_strdup("account");
			field->required = TRUE;
			g_ptr_array_add(parameters, field);
		}
		action = g_object_new(VENTURE_TYPE_ACTION, "data-class", VENTURE_DATA_CLASS_TENANT,
			"type-name", actions[i].type, "name", actions[i].name, "label", actions[i].label,
			"description", actions[i].description, "parameters", parameters,
			"stageable", FALSE, "service-transaction", TRUE, "roles", VENTURE_USER_ROLE_EDITOR, NULL);
		if (!venture_action_registry_register(venture_database_get_action_registry(database), action,
			retainer_allowed, retainer_invoke, venture_progress_service_get(database), NULL, &error))
			g_error("Retainer action registration: %s", error->message);
	}
}

static gboolean
progress_allowed(VentureAction *action, VentureEntity *entity, const VentureActor *actor, GError **error)
{
	(void)action;
	(void)entity;
	(void)actor;
	(void)error;
	return TRUE;
}

static VentureEntity *
progress_invoke(VentureAction *action, VentureEntity *entity, GHashTable *params,
	const VentureActor *actor, GError **error)
{
	g_autofree gchar *name = NULL;
	JsonNode *percent;
	g_object_get(action, "name", &name, NULL);
	percent = params ? g_hash_table_lookup(params, "percent") : NULL;
	if (g_strcmp0(name, "progress_invoice") == 0)
		return venture_progress_service_invoice(venture_action_get_data(action),
			VENTURE_QUOTE(entity), percent ? json_node_get_int(percent) : 0, NULL, actor, error);
	return NULL;
}

void
venture_progress_actions_register(VentureDatabase *database)
{
	g_autoptr(VentureAction) action = NULL;
	g_autoptr(GPtrArray) parameters = g_ptr_array_new_with_free_func((GDestroyNotify)venture_field_spec_free);
	g_autoptr(GError) error = NULL;
	g_ptr_array_add(parameters, venture_field_spec_new("percent", "Percent of original", VENTURE_FIELD_KIND_INTEGER));
	action = g_object_new(VENTURE_TYPE_ACTION, "data-class", VENTURE_DATA_CLASS_TENANT, "type-name", "quote", "name", "progress_invoice",
		"label", "Progress invoice", "description", "Invoice a slice of remaining contract value",
		"parameters", parameters, "stageable", TRUE, "roles", VENTURE_USER_ROLE_EDITOR, NULL);
	venture_action_registry_register(venture_database_get_action_registry(database), action,
		progress_allowed, progress_invoke, venture_progress_service_get(database), NULL, &error);
	if (error != NULL)
		g_error("Progress action registration: %s", error->message);
	retainer_actions_register(database);
}

/* Bind consent before this operation creates derived rows or enters nested
 * transactions. All generated financial effects share this root proposal. */
VentureEntity *
venture_progress_service_invoice(VentureProgressService *self, VentureQuote *quote, gint64 percent,
	const VentureMoney *amount, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureAccountingOperation) operation = NULL;
	VentureDatabase * db = self->database;
	GVariantBuilder arguments;
	g_autoptr(VentureEntity) result = NULL;
	g_autofree gchar *amount_text = NULL;
	if (db == NULL)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Database is unavailable");
		return NULL;
	}
	g_return_val_if_fail(VENTURE_IS_ENTITY(quote), NULL);
	amount_text = amount != NULL ? venture_money_to_string(amount) : NULL;
	g_variant_builder_init(&arguments, G_VARIANT_TYPE_VARDICT);
	g_variant_builder_add(&arguments, "{sv}", "percent", g_variant_new_int64((gint64)percent));
	g_variant_builder_add(&arguments, "{sv}", "amount", g_variant_new_maybe(G_VARIANT_TYPE_STRING, amount_text != NULL ? g_variant_new_string(amount_text) : NULL));
	operation = venture_accounting_operation_begin(db, "progress-invoice", VENTURE_ENTITY(quote), NULL,
		g_variant_builder_end(&arguments), venture_entity_get_organization_id(VENTURE_ENTITY(quote)), actor, error);
	if (operation == NULL)
		return NULL;
	result = venture_progress_service_invoice_impl(self, quote, percent, amount, actor, error);
	if (result == NULL)
		return NULL;
	if (!venture_accounting_operation_finish(operation, error))
		return NULL;
	return g_steal_pointer(&result);
}

/* Bind consent before this operation creates derived rows or enters nested
 * transactions. All generated financial effects share this root proposal. */
VentureEntity *
venture_progress_service_hold_retention(VentureProgressService *self, VentureQuote *quote,
	gint64 liability_account_id, const VentureMoney *amount, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureAccountingOperation) operation = NULL;
	VentureDatabase * db = self->database;
	GVariantBuilder arguments;
	g_autoptr(VentureEntity) result = NULL;
	g_autofree gchar *amount_text = NULL;
	if (db == NULL)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Database is unavailable");
		return NULL;
	}
	g_return_val_if_fail(VENTURE_IS_ENTITY(quote), NULL);
	amount_text = amount != NULL ? venture_money_to_string(amount) : NULL;
	g_variant_builder_init(&arguments, G_VARIANT_TYPE_VARDICT);
	g_variant_builder_add(&arguments, "{sv}", "liability_account_id", g_variant_new_int64((gint64)liability_account_id));
	g_variant_builder_add(&arguments, "{sv}", "amount", g_variant_new_maybe(G_VARIANT_TYPE_STRING, amount_text != NULL ? g_variant_new_string(amount_text) : NULL));
	operation = venture_accounting_operation_begin(db, "progress-hold-retention", VENTURE_ENTITY(quote), NULL,
		g_variant_builder_end(&arguments), venture_entity_get_organization_id(VENTURE_ENTITY(quote)), actor, error);
	if (operation == NULL)
		return NULL;
	result = venture_progress_service_hold_retention_impl(self, quote, liability_account_id, amount, actor, error);
	if (result == NULL)
		return NULL;
	if (!venture_accounting_operation_finish(operation, error))
		return NULL;
	return g_steal_pointer(&result);
}

/* Bind consent before this operation creates derived rows or enters nested
 * transactions. All generated financial effects share this root proposal. */
VentureEntity *
venture_progress_service_collect_retainer(VentureProgressService *self, gint64 organization_id,
	gint64 company_id, gint64 liability_account_id, const VentureMoney *amount,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureAccountingOperation) operation = NULL;
	VentureDatabase * db = self->database;
	GVariantBuilder arguments;
	g_autoptr(VentureEntity) result = NULL;
	g_autofree gchar *amount_text = NULL;
	if (db == NULL)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Database is unavailable");
		return NULL;
	}
	amount_text = amount != NULL ? venture_money_to_string(amount) : NULL;
	g_variant_builder_init(&arguments, G_VARIANT_TYPE_VARDICT);
	g_variant_builder_add(&arguments, "{sv}", "organization_id", g_variant_new_int64((gint64)organization_id));
	g_variant_builder_add(&arguments, "{sv}", "company_id", g_variant_new_int64((gint64)company_id));
	g_variant_builder_add(&arguments, "{sv}", "liability_account_id", g_variant_new_int64((gint64)liability_account_id));
	g_variant_builder_add(&arguments, "{sv}", "amount", g_variant_new_maybe(G_VARIANT_TYPE_STRING, amount_text != NULL ? g_variant_new_string(amount_text) : NULL));
	operation = venture_accounting_operation_begin(db, "progress-collect-retainer", NULL, NULL,
		g_variant_builder_end(&arguments), organization_id, actor, error);
	if (operation == NULL)
		return NULL;
	result = venture_progress_service_collect_retainer_impl(self, organization_id, company_id, liability_account_id, amount, actor, error);
	if (result == NULL)
		return NULL;
	if (!venture_accounting_operation_finish(operation, error))
		return NULL;
	return g_steal_pointer(&result);
}

/* Bind consent before this operation creates derived rows or enters nested
 * transactions. All generated financial effects share this root proposal. */
gboolean
venture_progress_service_release_retainer(VentureProgressService *self, VentureCustomerRetainer *retainer,
	const VentureMoney *amount, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureAccountingOperation) operation = NULL;
	VentureDatabase * db = self->database;
	GVariantBuilder arguments;
	gboolean result;
	g_autofree gchar *amount_text = NULL;
	if (db == NULL)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Database is unavailable");
		return FALSE;
	}
	g_return_val_if_fail(VENTURE_IS_ENTITY(retainer), FALSE);
	amount_text = amount != NULL ? venture_money_to_string(amount) : NULL;
	g_variant_builder_init(&arguments, G_VARIANT_TYPE_VARDICT);
	g_variant_builder_add(&arguments, "{sv}", "amount", g_variant_new_maybe(G_VARIANT_TYPE_STRING, amount_text != NULL ? g_variant_new_string(amount_text) : NULL));
	operation = venture_accounting_operation_begin(db, "progress-release-retainer", VENTURE_ENTITY(retainer), NULL,
		g_variant_builder_end(&arguments), venture_entity_get_organization_id(VENTURE_ENTITY(retainer)), actor, error);
	if (operation == NULL)
		return FALSE;
	result = venture_progress_service_release_retainer_impl(self, retainer, amount, actor, error);
	if (!result)
		return FALSE;
	if (!venture_accounting_operation_finish(operation, error))
		return FALSE;
	return result;
}

/* Bind consent before this operation creates derived rows or enters nested
 * transactions. All generated financial effects share this root proposal. */
gboolean
venture_progress_service_release_retention(VentureProgressService *self, VentureContractRetention *retention,
	const VentureMoney *amount, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureAccountingOperation) operation = NULL;
	VentureDatabase * db = self->database;
	GVariantBuilder arguments;
	gboolean result;
	g_autofree gchar *amount_text = NULL;
	if (db == NULL)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Database is unavailable");
		return FALSE;
	}
	g_return_val_if_fail(VENTURE_IS_ENTITY(retention), FALSE);
	amount_text = amount != NULL ? venture_money_to_string(amount) : NULL;
	g_variant_builder_init(&arguments, G_VARIANT_TYPE_VARDICT);
	g_variant_builder_add(&arguments, "{sv}", "amount", g_variant_new_maybe(G_VARIANT_TYPE_STRING, amount_text != NULL ? g_variant_new_string(amount_text) : NULL));
	operation = venture_accounting_operation_begin(db, "progress-release-retention", VENTURE_ENTITY(retention), NULL,
		g_variant_builder_end(&arguments), venture_entity_get_organization_id(VENTURE_ENTITY(retention)), actor, error);
	if (operation == NULL)
		return FALSE;
	result = venture_progress_service_release_retention_impl(self, retention, amount, actor, error);
	if (!result)
		return FALSE;
	if (!venture_accounting_operation_finish(operation, error))
		return FALSE;
	return result;
}
