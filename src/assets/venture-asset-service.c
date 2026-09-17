/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

static gboolean capitalize(VentureDatabase *db, VentureEntity *asset, GDateTime *date, const VentureActor *actor, GError **error);
static gboolean deferral_settle(VentureDeferralService *service, VentureEntity *deferral, const VentureActor *actor, GError **error);

struct _VentureAssetService
{
	GObject parent_instance;
	GWeakRef database;
	VentureEntity *permit;
};
G_DEFINE_FINAL_TYPE(VentureAssetService, venture_asset_service, G_TYPE_OBJECT)

static gboolean
refuse(GError **error, const gchar *message)
{
	venture_set_error_validation(error, "assets", "%s", message);
	return FALSE;
}

static VentureEntity *
load_record(VentureDatabase *db, GType type, gint64 id, GError **error)
{
	VentureEntity *entity = venture_database_get(db, type, id, error);
	if (entity == NULL || venture_entity_is_deleted(entity))
	{
		g_clear_object(&entity);
		if (error == NULL || *error == NULL)
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
				"No live %s record with id %" G_GINT64_FORMAT, g_type_name(type), id);
	}
	return entity;
}

static gboolean
transition_accumulate(GSignalInvocationHint *hint, GValue *result, const GValue *value, gpointer data)
{
	(void)hint;
	(void)data;
	g_value_set_boxed(result, g_value_get_boxed(value));
	return g_value_get_boxed(value) == NULL;
}

static gboolean
transition(GObject *service, VentureEntity *entity, const gchar *operation, GError **error)
{
	g_autoptr(VentureDatabase) database = NULL;
	g_autoptr(VentureEntity) snapshot = g_object_new(G_OBJECT_TYPE(entity), NULL);
	GError *veto = NULL;
	venture_entity_copy_properties_from(snapshot, entity, FALSE);
	g_object_get(service, "database", &database, NULL);
	if (database != NULL) venture_accounting_operation_suspend(database);
	g_signal_emit_by_name(service, "transition", snapshot, operation, &veto);
	if (database != NULL) venture_accounting_operation_resume(database);
	if (veto != NULL)
	{
		g_propagate_error(error, veto);
		return FALSE;
	}
	return TRUE;
}

static gboolean
validate(VentureDatabase *db, VentureEntity *entity, VentureEntity *previous,
	gpointer data, GError **error)
{
	VentureAssetService *self = data;
	gint state;
	(void)db;
	if (self->permit == entity)
	{
		self->permit = NULL;
		return TRUE;
	}
	if (!VENTURE_IS_FIXED_ASSET(entity))
		return refuse(error, "Schedule history requires VentureAssetService or VentureDeferralService");
	g_object_get(entity, "status", &state, NULL);
	if (state != VENTURE_ASSET_STATUS_DRAFT)
		return refuse(error, "Status changes require VentureAssetService");
	if (previous != NULL)
	{
		g_object_get(previous, "status", &state, NULL);
		if (state != VENTURE_ASSET_STATUS_DRAFT)
			return refuse(error, "An in-service asset is immutable; use VentureAssetService");
	}
	return TRUE;
}

static void
get_property(GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
	VentureAssetService *self = VENTURE_ASSET_SERVICE(object);
	if (id == 1)
		g_value_take_object(value, g_weak_ref_get(&self->database));
	else
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
}

static void
set_property(GObject *object, guint id, const GValue *value, GParamSpec *pspec)
{
	VentureAssetService *self = VENTURE_ASSET_SERVICE(object);
	if (id == 1)
		g_weak_ref_set(&self->database, g_value_get_object(value));
	else
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
}

static void
constructed(GObject *object)
{
	VentureAssetService *self = VENTURE_ASSET_SERVICE(object);
	g_autoptr(VentureDatabase) db = g_weak_ref_get(&self->database);
	G_OBJECT_CLASS(venture_asset_service_parent_class)->constructed(object);
	venture_database_add_save_validator(db, VENTURE_TYPE_FIXED_ASSET, validate, self, NULL);
	venture_database_add_save_validator(db, VENTURE_TYPE_DEPRECIATION_ENTRY, validate, self, NULL);
	venture_database_add_save_validator(db, VENTURE_TYPE_DEFERRAL, validate, self, NULL);
	venture_database_add_save_validator(db, VENTURE_TYPE_DEFERRAL_ENTRY, validate, self, NULL);
	venture_database_add_save_validator(db, VENTURE_TYPE_TAX_DEPRECIATION_ENTRY, validate, self, NULL);
}

static void
finalize(GObject *object)
{
	VentureAssetService *self = VENTURE_ASSET_SERVICE(object);
	g_weak_ref_clear(&self->database);
	G_OBJECT_CLASS(venture_asset_service_parent_class)->finalize(object);
}

static void
venture_asset_service_class_init(VentureAssetServiceClass *klass)
{
	GObjectClass *oc = G_OBJECT_CLASS(klass);
	oc->get_property = get_property;
	oc->set_property = set_property;
	oc->constructed = constructed;
	oc->finalize = finalize;
	/**
	 * VentureAssetService::transition:
	 * @self: service
	 * @asset: detached source snapshot; edits are ignored
	 * @operation: place, dispose, write-off, import-opening or rollback-opening
	 *
	 * RUN_LAST after validation, before financial writes, inside the
	 * transaction. Return an owned GError to veto. First error wins.
	 */
	g_signal_new("transition", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0,
		transition_accumulate, NULL, NULL, G_TYPE_ERROR, 2, VENTURE_TYPE_ENTITY, G_TYPE_STRING);
	g_object_class_install_property(oc, 1, g_param_spec_object("database", "Database",
		"Owning repository", VENTURE_TYPE_DATABASE,
		G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
}

static void
venture_asset_service_init(VentureAssetService *self)
{
	g_weak_ref_init(&self->database, NULL);
}

static gboolean
save_internal(VentureAssetService *self, VentureDatabase *db, VentureEntity *entity,
	const VentureActor *actor, GError **error)
{
	gboolean ok;
	self->permit = entity;
	ok = venture_database_save(db, entity, actor, error);
	self->permit = NULL;
	return ok;
}

static gboolean
check_account(VentureDatabase *db, gint64 id, gint64 org, GError **error)
{
	g_autoptr(VentureEntity) account = load_record(db, VENTURE_TYPE_ACCOUNT, id, error);
	gboolean active;
	if (account == NULL)
		return FALSE;
	g_object_get(account, "active", &active, NULL);
	if (!active || venture_entity_get_organization_id(account) != org)
		return refuse(error, "Choose active accounts in the asset's organization");
	return TRUE;
}

static gboolean
income_account(VentureDatabase *db, gint64 id, gboolean *income, GError **error)
{
	g_autoptr(VentureEntity) account = load_record(db, VENTURE_TYPE_ACCOUNT, id, error);
	gint kind;
	if (account == NULL)
		return FALSE;
	g_object_get(account, "kind", &kind, NULL);
	if (kind != VENTURE_ACCOUNT_KIND_EXPENSE && kind != VENTURE_ACCOUNT_KIND_INCOME)
		return refuse(error, "A deferral target must be an expense or income account");
	*income = kind == VENTURE_ACCOUNT_KIND_INCOME;
	return TRUE;
}

/* Preserve allocate_evenly's exact total, moving all residual cents into
 * the final installment as the register's monthly convention requires. */
static GPtrArray *
split(const VentureMoney *amount, guint months, GError **error)
{
	GPtrArray *parts = venture_money_allocate_evenly(amount, months, error);
	VentureMoney *last;
	guint i;
	if (parts == NULL)
		return NULL;
	last = g_ptr_array_index(parts, months - 1);
	for (i = 0; i + 1 < months; i++)
	{
		VentureMoney *part = g_ptr_array_index(parts, i);
		gint64 extra = part->amount - amount->amount / months;
		part->amount -= extra;
		last->amount += extra;
	}
	return parts;
}

/* The first open fiscal period supplies an effective service date. Missing
 * calendar coverage is a configuration error, never permission to post. */
static GDateTime *
first_open(VentureDatabase *db, gint64 org, GDateTime *requested, GError **error)
{
	g_autoptr(GError) refusal = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) periods = NULL;
	guint i;
	if (venture_period_guard_is_postable(VENTURE_PERIOD_GUARD(venture_database_get_period_guard(db)), db, org, requested, &refusal))
		return g_date_time_ref(requested);
	if (!g_error_matches(refusal, VENTURE_ERROR, VENTURE_ERROR_CONFLICT))
	{
		g_propagate_error(error, g_steal_pointer(&refusal));
		return NULL;
	}
	query = venture_query_new(VENTURE_TYPE_FISCAL_PERIOD);
	venture_query_set_organization(query, org);
	venture_query_set_limit(query, 0);
	if (!venture_query_add_order(query, "start-at", VENTURE_SORT_ASCENDING, error))
		return NULL;
	periods = venture_database_find(db, query, error);
	if (periods == NULL)
		return NULL;
	for (i = 0; i < periods->len; i++)
	{
		g_autoptr(GDateTime) start = NULL;
		gint state;
		g_object_get(g_ptr_array_index(periods, i), "state", &state, "start-at", &start, NULL);
		if (state == VENTURE_PERIOD_OPEN && start != NULL && g_date_time_compare(start, requested) > 0 &&
			venture_period_guard_is_postable(VENTURE_PERIOD_GUARD(venture_database_get_period_guard(db)), db, org, start, NULL))
			return g_steal_pointer(&start);
	}
	g_propagate_error(error, g_steal_pointer(&refusal));
	return NULL;
}

static gboolean
venture_asset_service_place_in_service_impl(VentureAssetService *self,
	VentureEntity *asset, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureDatabase) db = g_weak_ref_get(&self->database);
	g_autoptr(VentureEntity) current = NULL;
	g_autoptr(VentureMoney) cost = NULL;
	g_autoptr(VentureMoney) salvage = NULL;
	g_autoptr(VentureMoney) base = NULL;
	g_autoptr(GDateTime) start = NULL;
	g_autoptr(GDateTime) acquired = NULL;
	g_autoptr(GPtrArray) parts = NULL;
	gint64 months, account, accumulated, expense;
	gint state, method;
	guint i;
	gint64 org = venture_entity_get_organization_id(asset);
	if (!venture_entity_registry_is_type_enabled(venture_entity_registry_get_default(), "fixed_asset"))
		return refuse(error, "The assets module is disabled");
	if (!venture_database_begin(db, error))
		return FALSE;
	current = load_record(db, VENTURE_TYPE_FIXED_ASSET, venture_entity_get_id(asset), error);
	if (current == NULL)
		goto fail;
	g_object_get(current, "status", &state, NULL);
	if (state != VENTURE_ASSET_STATUS_DRAFT || venture_entity_get_version(current) != venture_entity_get_version(asset))
	{
		refuse(error, "VentureAssetService requires the current draft asset");
		goto fail;
	}
	venture_entity_copy_properties_from(current, asset, TRUE);
	g_object_get(asset, "cost", &cost, "salvage-value", &salvage, "in-service-at", &start,
		"acquired-at", &acquired, "useful-life-months", &months, "method", &method,
		"asset-account-id", &account, "accumulated-depreciation-account-id", &accumulated,
		"depreciation-expense-account-id", &expense, NULL);
	if (start == NULL)
		start = venture_time_now();
	if (cost == NULL || salvage == NULL || acquired == NULL ||
		cost->amount < 0 || salvage->amount < 0 || months < 1 || months > 1200 ||
		g_date_time_compare(start, acquired) < 0)
	{
		refuse(error, "An asset needs nonnegative cost and salvage, acquisition and service dates, and 1-1200 months");
		goto fail;
	}
	base = venture_money_subtract(cost, salvage, error);
	if (base == NULL)
		goto fail;
	if (base->amount < 0)
	{
		refuse(error, "Salvage cannot exceed cost");
		goto fail;
	}
	if (!check_account(db, account, org, error) || !check_account(db, accumulated, org, error) ||
		!check_account(db, expense, org, error))
		goto fail;
	{
		g_autoptr(GDateTime) effective = first_open(db, org, start, error);
		if (effective == NULL)
			goto fail;
		if (g_date_time_compare(effective, start) != 0)
		{
			g_autofree gchar *old_label = g_date_time_format(start, "%Y-%m");
			g_autofree gchar *new_label = g_date_time_format(effective, "%Y-%m");
			g_autofree gchar *note = g_strdup_printf("Requested %s was closed; scheduled into first open period %s.", old_label, new_label);
			g_object_set(current, "schedule-note", note, NULL);
		}
		g_clear_pointer(&start, g_date_time_unref);
		start = g_steal_pointer(&effective);
	}
	parts = split(base, (guint)months, error);
	if (parts == NULL)
		goto fail;
	if (method == VENTURE_ASSET_METHOD_DECLINING_BALANCE)
	{
		g_autoptr(VentureMoney) remaining = venture_money_copy(base);
		g_autoptr(VentureMoney) carrying = venture_money_copy(cost);
		for (i = 0; i < parts->len; i++)
		{
			g_autoptr(VentureMoney) charge = venture_money_multiply_rational(carrying, 2, months, error);
			g_autoptr(VentureMoney) next = NULL;
			g_autoptr(VentureMoney) book = NULL;
			if (charge == NULL)
				goto fail;
			if (i + 1 == parts->len || venture_money_compare(charge, remaining) > 0)
			{
				g_clear_pointer(&charge, venture_money_free);
				charge = venture_money_copy(remaining);
			}
			next = venture_money_subtract(remaining, charge, error);
			book = venture_money_subtract(carrying, charge, error);
			if (next == NULL || book == NULL)
				goto fail;
			g_clear_pointer(&remaining, venture_money_free);
			g_clear_pointer(&carrying, venture_money_free);
			remaining = g_steal_pointer(&next);
			carrying = g_steal_pointer(&book);
			venture_money_free(g_ptr_array_index(parts, i));
			g_ptr_array_index(parts, i) = g_steal_pointer(&charge);
		}
	}
	if (!transition(G_OBJECT(self), current, "place", error) || !capitalize(db, current, start, actor, error))
		goto fail;
	for (i = 0; method != VENTURE_ASSET_METHOD_NONE && i < parts->len; i++)
	{
		g_autoptr(VentureEntity) row = VENTURE_ENTITY(venture_depreciation_entry_new());
		g_autoptr(GDateTime) date = g_date_time_add_months(start, (gint)i);
		g_autofree gchar *period = g_date_time_format(date, "%Y-%m");
		g_object_set(row, "organization-id", org, "asset-id", venture_entity_get_id(asset),
			"period", period, "amount", g_ptr_array_index(parts, i), NULL);
		if (!save_internal(self, db, row, actor, error))
			goto fail;
	}
	{
		gint64 tax_months = 0;
		gint tax_method = VENTURE_ASSET_METHOD_NONE;
		gint tax_convention = VENTURE_ASSET_CONVENTION_FULL_MONTH;
		g_object_get(current, "tax-useful-life-months", &tax_months, "tax-method", &tax_method,
			"tax-convention", &tax_convention, NULL);
		if (tax_months >= 1 && tax_months <= 1200 && tax_method != VENTURE_ASSET_METHOD_NONE)
		{
			g_autoptr(GPtrArray) tax_parts = split(base, (guint)tax_months, error);
			guint t;
			if (tax_parts == NULL)
				goto fail;
			if (tax_method == VENTURE_ASSET_METHOD_DECLINING_BALANCE)
			{
				g_autoptr(VentureMoney) remaining = venture_money_copy(base);
				g_autoptr(VentureMoney) carrying = venture_money_copy(cost);
				for (t = 0; t < tax_parts->len; t++)
				{
					g_autoptr(VentureMoney) charge = venture_money_multiply_rational(carrying, 2, tax_months, error);
					g_autoptr(VentureMoney) next = NULL;
					g_autoptr(VentureMoney) book = NULL;
					if (charge == NULL)
						goto fail;
					if (t + 1 == tax_parts->len || venture_money_compare(charge, remaining) > 0)
					{
						g_clear_pointer(&charge, venture_money_free);
						charge = venture_money_copy(remaining);
					}
					next = venture_money_subtract(remaining, charge, error);
					book = venture_money_subtract(carrying, charge, error);
					if (next == NULL || book == NULL)
						goto fail;
					g_clear_pointer(&remaining, venture_money_free);
					g_clear_pointer(&carrying, venture_money_free);
					remaining = g_steal_pointer(&next);
					carrying = g_steal_pointer(&book);
					venture_money_free(g_ptr_array_index(tax_parts, t));
					g_ptr_array_index(tax_parts, t) = g_steal_pointer(&charge);
				}
			}
			if (tax_convention == VENTURE_ASSET_CONVENTION_HALF_YEAR && tax_parts->len > 1)
			{
				VentureMoney *first = g_ptr_array_index(tax_parts, 0);
				VentureMoney *last = g_ptr_array_index(tax_parts, tax_parts->len - 1);
				gint64 half = first->amount / 2;
				last->amount += first->amount - half;
				first->amount = half;
			}
			for (t = 0; t < tax_parts->len; t++)
			{
				g_autoptr(VentureEntity) row = VENTURE_ENTITY(venture_tax_depreciation_entry_new());
				g_autoptr(GDateTime) date = g_date_time_add_months(start, (gint)t);
				g_autofree gchar *period = g_date_time_format(date, "%Y-%m");
				g_object_set(row, "organization-id", org, "asset-id", venture_entity_get_id(asset),
					"period", period, "amount", g_ptr_array_index(tax_parts, t), NULL);
				if (!save_internal(self, db, row, actor, error))
					goto fail;
			}
		}
	}
	g_object_set(current, "operation", NULL, NULL);
	g_object_set(current, "status", VENTURE_ASSET_STATUS_IN_SERVICE, "in-service-at", start, NULL);
	if (!save_internal(self, db, current, actor, error) || !venture_database_commit(db, error))
		goto fail;
	venture_entity_copy_properties_from(asset, current, FALSE);
	return TRUE;
fail:
	venture_database_rollback(db);
	return FALSE;
}

gboolean
venture_assets_save(VentureDatabase *db, VentureEntity *entity,
	const VentureActor *actor, gboolean *handled, GError **error)
{
	VentureAssetService *self;
	g_autofree gchar *operation = NULL;
	*handled = FALSE;
	if (!VENTURE_IS_FIXED_ASSET(entity) && !VENTURE_IS_DEPRECIATION_ENTRY(entity) &&
		!VENTURE_IS_DEFERRAL(entity) && !VENTURE_IS_DEFERRAL_ENTRY(entity) &&
		!VENTURE_IS_TAX_DEPRECIATION_ENTRY(entity))
		return TRUE;
	if (!venture_entity_registry_is_type_enabled(venture_entity_registry_get_default(), "fixed_asset"))
		return refuse(error, "The assets module is disabled");
	self = venture_asset_service_get(db);
	if (VENTURE_IS_DEFERRAL(entity) && self->permit != entity && venture_entity_get_id(entity) == 0)
	{
		g_autoptr(VentureDeferralService) deferrals = venture_deferral_service_new(db);
		*handled = TRUE;
		return venture_deferral_service_schedule(deferrals, entity, actor, error);
	}
	if (VENTURE_IS_DEFERRAL(entity) && self->permit != entity)
	{
		g_object_get(entity, "operation", &operation, NULL);
		if (g_strcmp0(operation, "settle") == 0)
		{
			g_autoptr(VentureDeferralService) deferrals = venture_deferral_service_new(db);
			*handled = TRUE;
			return deferral_settle(deferrals, entity, actor, error);
		}
		return TRUE;
	}
	if (self->permit == entity || !VENTURE_IS_FIXED_ASSET(entity))
		return TRUE;
	g_object_get(entity, "operation", &operation, NULL);
	if (venture_string_is_empty(operation))
		return TRUE;
	*handled = TRUE;
	if (g_str_has_prefix(operation, "run-period:"))
		return venture_asset_service_run_period(self, operation + 11,
			venture_entity_get_organization_id(entity), FALSE, actor, error) >= 0;
	if (g_str_equal(operation, "dispose") || g_str_equal(operation, "write-off"))
		return venture_asset_service_dispose(self, entity, g_str_equal(operation, "write-off"), actor, error);
	if (g_str_equal(operation, "place"))
		return venture_asset_service_place_in_service(self, entity, actor, error);
	return refuse(error, "Unknown VentureAssetService operation");
}

struct _VentureDeferralService
{
	GObject parent_instance;
	GWeakRef database;
};
G_DEFINE_FINAL_TYPE(VentureDeferralService, venture_deferral_service, G_TYPE_OBJECT)

static void
deferral_get_property(GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
	if (id == 1)
		g_value_take_object(value, g_weak_ref_get(&VENTURE_DEFERRAL_SERVICE(object)->database));
	else
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
}

static void
deferral_set_property(GObject *object, guint id, const GValue *value, GParamSpec *pspec)
{
	if (id == 1)
		g_weak_ref_set(&VENTURE_DEFERRAL_SERVICE(object)->database, g_value_get_object(value));
	else
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
}

static void
deferral_finalize(GObject *object)
{
	g_weak_ref_clear(&VENTURE_DEFERRAL_SERVICE(object)->database);
	G_OBJECT_CLASS(venture_deferral_service_parent_class)->finalize(object);
}

static void
venture_deferral_service_class_init(VentureDeferralServiceClass *klass)
{
	GObjectClass *oc = G_OBJECT_CLASS(klass);
	oc->get_property = deferral_get_property;
	oc->set_property = deferral_set_property;
	oc->finalize = deferral_finalize;
	/**
	 * VentureDeferralService::transition:
	 * @self: service
	 * @deferral: detached source snapshot; edits are ignored
	 * @operation: schedule or settle
	 *
	 * RUN_LAST before writes inside the transaction; first owned GError vetoes.
	 */
	g_signal_new("transition", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0,
		transition_accumulate, NULL, NULL, G_TYPE_ERROR, 2, VENTURE_TYPE_ENTITY, G_TYPE_STRING);
	g_object_class_install_property(oc, 1, g_param_spec_object("database", "Database",
		"Owning repository", VENTURE_TYPE_DATABASE,
		G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
}

static void
venture_deferral_service_init(VentureDeferralService *self)
{
	g_weak_ref_init(&self->database, NULL);
}

VentureDeferralService *
venture_deferral_service_new(VentureDatabase *database)
{
	return g_object_new(VENTURE_TYPE_DEFERRAL_SERVICE, "database", database, NULL);
}

/* Every leg uses the source row, so journal recovery is independent of
 * an in-memory object surviving a failed transaction. */
static void
leg(GPtrArray *entries, VentureEntity *source, const gchar *type,
	const gchar *transaction, GDateTime *date, gint64 account,
	VentureLedgerSide side, const VentureMoney *amount)
{
	VentureEntity *entry = VENTURE_ENTITY(venture_ledger_entry_new());
	g_object_set(entry, "organization-id", venture_entity_get_organization_id(source),
		"source-type", type, "source-id", venture_entity_get_id(source),
		"transaction-id", transaction, "occurred-at", date,
		"account-id", account, "side", side, "amount", amount, NULL);
	g_ptr_array_add(entries, entry);
}

static gboolean
post_pair(VentureDatabase *db, VentureEntity *source, const gchar *type,
	const gchar *transaction, GDateTime *date, gint64 debit, gint64 credit,
	const VentureMoney *amount, const VentureActor *actor, GError **error)
{
	g_autoptr(GPtrArray) entries = g_ptr_array_new_with_free_func(g_object_unref);
	leg(entries, source, type, transaction, date, debit, VENTURE_LEDGER_SIDE_DEBIT, amount);
	leg(entries, source, type, transaction, date, credit, VENTURE_LEDGER_SIDE_CREDIT, amount);
	return venture_posting_service_post_entries(venture_database_get_posting_service(db),
		entries, NULL, actor, error);
}

static gboolean
venture_deferral_service_schedule_impl(VentureDeferralService *service, VentureEntity *deferral,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureDatabase) db = g_weak_ref_get(&service->database);
	VentureAssetService *self = venture_asset_service_get(db);
	g_autoptr(VentureEntity) row = VENTURE_ENTITY(venture_deferral_new());
	g_autoptr(VentureMoney) total = NULL;
	g_autoptr(GDateTime) start = NULL;
	g_autoptr(GPtrArray) parts = NULL;
	gint64 months, source, target, funding, expense_id, invoice_id;
	gboolean income;
	g_autofree gchar *schedule_note = NULL;
	gint kind, status;
	gint64 org = venture_entity_get_organization_id(deferral);
	guint i;
	if (venture_entity_get_id(deferral) != 0)
		return refuse(error, "VentureDeferralService only schedules new deferrals");
	if (!venture_entity_registry_is_type_enabled(venture_entity_registry_get_default(), "deferral"))
		return refuse(error, "The assets module is disabled");
	g_object_get(deferral, "total", &total, "start", &start, "months", &months,
		"source-account-id", &source, "target-account-id", &target, "kind", &kind, "status", &status,
		"funding-account-id", &funding, "source-expense-id", &expense_id, "source-invoice-id", &invoice_id, NULL);
	if (total == NULL || total->amount <= 0 || start == NULL || months < 1 || months > 1200 ||
		status != VENTURE_DEFERRAL_STATUS_ACTIVE)
		return refuse(error, "VentureDeferralService requires a positive total, start, 1-1200 months and active status");
	if (!venture_database_begin(db, error))
		return FALSE;
	if (!check_account(db, source, org, error) || !check_account(db, target, org, error) ||
		!income_account(db, target, &income, error))
		goto fail;
	if (expense_id != 0 || invoice_id != 0)
	{
		g_autoptr(VentureEntity) linked = load_record(db,
			expense_id != 0 ? VENTURE_TYPE_EXPENSE : VENTURE_TYPE_INVOICE,
			expense_id != 0 ? expense_id : invoice_id, error);
		if (linked == NULL)
			goto fail;
		if ((expense_id != 0 && invoice_id != 0) || venture_entity_get_organization_id(linked) != org)
		{
			refuse(error, "Choose at most one source document in the same organization");
			goto fail;
		}
	}
	{
		g_autoptr(GDateTime) effective = first_open(db, org, start, error);
		if (effective == NULL)
			goto fail;
		if (g_date_time_compare(effective, start) != 0)
		{
			g_autofree gchar *old_label = g_date_time_format(start, "%Y-%m");
			g_autofree gchar *new_label = g_date_time_format(effective, "%Y-%m");
			schedule_note = g_strdup_printf("Requested %s was closed; scheduled into first open period %s.", old_label, new_label);
		}
		g_clear_pointer(&start, g_date_time_unref);
		start = g_steal_pointer(&effective);
	}
	parts = split(total, (guint)months, error);
	if (parts == NULL)
		goto fail;
	venture_entity_copy_properties_from(row, deferral, TRUE);
	g_object_set(row, "start", start, "operation", NULL, "schedule-note", schedule_note, NULL);
	if (!transition(G_OBJECT(service), row, "schedule", error))
		goto fail;
	if (!save_internal(self, db, row, actor, error))
		goto fail;
	if (kind == VENTURE_DEFERRAL_KIND_PREPAYMENT)
	{
		g_autofree gchar *transaction = g_strdup_printf("deferral-source:%" G_GINT64_FORMAT, venture_entity_get_id(row));
		if (expense_id != 0)
		{
			g_autoptr(VentureEntity) expense = load_record(db, VENTURE_TYPE_EXPENSE, expense_id, error);
			g_autoptr(VentureMoney) amount = NULL;
			if (expense == NULL)
				goto fail;
			g_object_get(expense, "amount", &amount, NULL);
			if (income || venture_entity_get_organization_id(expense) != org || !venture_money_equal(amount, total))
			{
				refuse(error, "A source expense must belong to the organization and match the prepayment total");
				goto fail;
			}
			funding = target;
		}
		if (funding == 0 || !check_account(db, funding, org, error))
		{
			if (funding == 0)
				refuse(error, "A prepayment requires a funding account or matching source expense");
			goto fail;
		}
		if (!post_pair(db, row, "deferral", transaction, start, income ? funding : source, income ? source : funding, total, actor, error))
			goto fail;
	}
	for (i = 0; i < parts->len; i++)
	{
		g_autoptr(VentureEntity) entry = VENTURE_ENTITY(venture_deferral_entry_new());
		g_autoptr(GDateTime) date = g_date_time_add_months(start, (gint)i);
		g_autofree gchar *period = g_date_time_format(date, "%Y-%m");
		g_object_set(entry, "organization-id", org, "deferral-id", venture_entity_get_id(row),
			"period", period, "amount", g_ptr_array_index(parts, i), NULL);
		if (!save_internal(self, db, entry, actor, error))
			goto fail;
	}
	if (!venture_database_commit(db, error))
		goto fail;
	venture_entity_copy_properties_from(deferral, row, FALSE);
	return TRUE;
fail:
	venture_database_rollback(db);
	return FALSE;
}

static GDateTime *
month_end(const gchar *period, GError **error)
{
	g_autofree gchar *text = NULL;
	g_autoptr(GDateTime) start = NULL;
	g_autoptr(GDateTime) next = NULL;
	guint i;
	if (period == NULL || strlen(period) != 7 || period[4] != '-')
	{
		refuse(error, "A run period must be YYYY-MM");
		return NULL;
	}
	for (i = 0; i < 7; i++)
		if (i != 4 && !g_ascii_isdigit(period[i]))
		{
			refuse(error, "A run period must be YYYY-MM");
			return NULL;
		}
	text = g_strdup_printf("%s-01T00:00:00Z", period);
	start = g_date_time_new_from_iso8601(text, NULL);
	if (start == NULL)
	{
		refuse(error, "Invalid calendar month");
		return NULL;
	}
	next = g_date_time_add_months(start, 1);
	if (next == NULL)
	{
		refuse(error, "Calendar month exceeds supported dates");
		return NULL;
	}
	return g_date_time_add_seconds(next, -1);
}

static GPtrArray *
scheduled(VentureDatabase *db, GType type, gint64 org, const gchar *period, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(type);
	venture_query_set_organization(query, org);
	venture_query_set_limit(query, 0);
	if (!venture_query_add_filter_int(query, "state", VENTURE_FILTER_OP_EQ, VENTURE_SCHEDULE_STATE_SCHEDULED, error) ||
		(period != NULL && !venture_query_add_filter_string(query, "period", VENTURE_FILTER_OP_EQ, period, error)))
		return NULL;
	return venture_database_find(db, query, error);
}

static gint
venture_asset_service_run_period_impl(VentureAssetService *self, const gchar *period,
	gint64 org, gboolean dry_run, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureDatabase) db = g_weak_ref_get(&self->database);
	g_autoptr(GDateTime) date = month_end(period, error);
	gint count = 0;
	guint kind;
	if (date == NULL)
		return -1;
	if (org <= 0 || !venture_entity_registry_is_type_enabled(venture_entity_registry_get_default(), "fixed_asset"))
	{
		refuse(error, "An enabled assets module and exact organization are required");
		return -1;
	}
	if (!venture_database_begin(db, error))
		return -1;
	if (!venture_period_guard_is_postable(VENTURE_PERIOD_GUARD(venture_database_get_period_guard(db)), db, org, date, error))
		goto fail;
	for (kind = 0; kind < 2; kind++)
	{
		GType type = kind == 0 ? VENTURE_TYPE_DEPRECIATION_ENTRY : VENTURE_TYPE_DEFERRAL_ENTRY;
		const gchar *source_type = kind == 0 ? "depreciation_entry" : "deferral_entry";
		g_autoptr(GPtrArray) rows = scheduled(db, type, org, period, error);
		guint i;
		if (rows == NULL)
			goto fail;
		for (i = 0; i < rows->len; i++)
		{
			VentureEntity *row = g_ptr_array_index(rows, i);
			g_autoptr(VentureEntity) parent = NULL;
			g_autoptr(VentureMoney) amount = NULL;
			g_autoptr(GPtrArray) journals = NULL;
			g_autofree gchar *transaction = NULL;
			gint64 parent_id, debit, credit;
			gint state;
			g_object_get(row, kind == 0 ? "asset-id" : "deferral-id", &parent_id, "amount", &amount, NULL);
			parent = load_record(db, kind == 0 ? VENTURE_TYPE_FIXED_ASSET : VENTURE_TYPE_DEFERRAL, parent_id, error);
			if (parent == NULL)
				goto fail;
			g_object_get(parent, "status", &state,
				kind == 0 ? "depreciation-expense-account-id" : "target-account-id", &debit,
				kind == 0 ? "accumulated-depreciation-account-id" : "source-account-id", &credit, NULL);
			if (venture_entity_get_organization_id(parent) != org || amount == NULL || amount->amount < 0 ||
				(kind == 0 && state != VENTURE_ASSET_STATUS_IN_SERVICE) ||
				(kind == 1 && state != VENTURE_DEFERRAL_STATUS_ACTIVE))
			{
				refuse(error, "Scheduled entry has an invalid amount or parent state/organization");
				goto fail;
			}
			if (!check_account(db, debit, org, error) || !check_account(db, credit, org, error))
				goto fail;
			if (kind == 1)
			{
				gboolean income;
				if (!income_account(db, debit, &income, error))
					goto fail;
				if (income)
				{
					gint64 swap = debit;
					debit = credit;
					credit = swap;
				}
			}
			count++;
			if (dry_run)
				continue;
			transaction = g_strdup_printf("assets:%s:%" G_GINT64_FORMAT, source_type, venture_entity_get_id(row));
			if (!post_pair(db, row, source_type, transaction, date, debit, credit, amount, actor, error))
				goto fail;
			journals = venture_posting_service_find_source(venture_database_get_posting_service(db),
				source_type, venture_entity_get_id(row), org, error);
			if (journals == NULL || journals->len != 1)
			{
				if (journals != NULL)
					refuse(error, "Expected exactly one journal for the scheduled entry");
				goto fail;
			}
			g_object_set(row, "journal-id", venture_entity_get_id(g_ptr_array_index(journals, 0)),
				"state", VENTURE_SCHEDULE_STATE_POSTED, NULL);
			if (!save_internal(self, db, row, actor, error))
				goto fail;
			if (kind == 1)
			{
				g_autoptr(VentureQuery) q = venture_query_new(VENTURE_TYPE_DEFERRAL_ENTRY);
				g_autoptr(GPtrArray) remaining = NULL;
				venture_query_set_organization(q, org);
				if (!venture_query_add_filter_int(q, "deferral-id", VENTURE_FILTER_OP_EQ, parent_id, error) ||
					!venture_query_add_filter_int(q, "state", VENTURE_FILTER_OP_EQ, VENTURE_SCHEDULE_STATE_SCHEDULED, error))
					goto fail;
				remaining = venture_database_find(db, q, error);
				if (remaining == NULL)
					goto fail;
				if (remaining->len == 0)
				{
					g_object_set(parent, "status", VENTURE_DEFERRAL_STATUS_COMPLETE, NULL);
					if (!save_internal(self, db, parent, actor, error))
						goto fail;
				}
			}
		}
	}
	if (!venture_database_commit(db, error))
		goto fail;
	return count;
fail:
	venture_database_rollback(db);
	return -1;
}

static gint
venture_asset_service_run_tax_period_impl(VentureAssetService *self, const gchar *period,
	gint64 org, gboolean dry_run, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureDatabase) db = g_weak_ref_get(&self->database);
	g_autoptr(GDateTime) date = month_end(period, error);
	g_autoptr(GPtrArray) rows = NULL;
	gint count = 0;
	guint i;
	if (date == NULL)
		return -1;
	if (org <= 0 || !venture_entity_registry_is_type_enabled(venture_entity_registry_get_default(), "fixed_asset"))
	{
		refuse(error, "An enabled assets module and exact organization are required");
		return -1;
	}
	if (!venture_database_begin(db, error))
		return -1;
	if (!venture_period_guard_is_postable(VENTURE_PERIOD_GUARD(venture_database_get_period_guard(db)), db, org, date, error))
		goto fail;
	rows = scheduled(db, VENTURE_TYPE_TAX_DEPRECIATION_ENTRY, org, period, error);
	if (rows == NULL)
		goto fail;
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(rows, i);
		g_autoptr(VentureEntity) parent = NULL;
		g_autoptr(VentureMoney) amount = NULL;
		g_autoptr(GPtrArray) journals = NULL;
		g_autofree gchar *transaction = NULL;
		gint64 parent_id, debit, credit;
		gint state;
		g_object_get(row, "asset-id", &parent_id, "amount", &amount, NULL);
		parent = load_record(db, VENTURE_TYPE_FIXED_ASSET, parent_id, error);
		if (parent == NULL)
			goto fail;
		g_object_get(parent, "status", &state, "depreciation-expense-account-id", &debit,
			"accumulated-depreciation-account-id", &credit, NULL);
		if (venture_entity_get_organization_id(parent) != org || amount == NULL || amount->amount < 0 ||
			state != VENTURE_ASSET_STATUS_IN_SERVICE)
		{
			refuse(error, "Scheduled tax entry has an invalid amount or parent state/organization");
			goto fail;
		}
		if (!check_account(db, debit, org, error) || !check_account(db, credit, org, error))
			goto fail;
		count++;
		if (dry_run)
			continue;
		transaction = g_strdup_printf("assets:tax_depreciation_entry:%" G_GINT64_FORMAT, venture_entity_get_id(row));
		if (!post_pair(db, row, "tax_depreciation_entry", transaction, date, debit, credit, amount, actor, error))
			goto fail;
		journals = venture_posting_service_find_source(venture_database_get_posting_service(db),
			"tax_depreciation_entry", venture_entity_get_id(row), org, error);
		if (journals == NULL || journals->len != 1)
		{
			if (journals != NULL)
				refuse(error, "Expected exactly one journal for the scheduled tax entry");
			goto fail;
		}
		g_object_set(row, "journal-id", venture_entity_get_id(g_ptr_array_index(journals, 0)),
			"state", VENTURE_SCHEDULE_STATE_POSTED, NULL);
		if (!save_internal(self, db, row, actor, error))
			goto fail;
	}
	if (!venture_database_commit(db, error))
		goto fail;
	return count;
fail:
	venture_database_rollback(db);
	return -1;
}

gboolean
venture_assets_check_removal(VentureDatabase *database, VentureEntity *entity, GError **error)
{
	gint state;
	if (VENTURE_IS_FIXED_ASSET(entity))
	{
		g_autoptr(VentureEntity) stored = venture_database_get(database, VENTURE_TYPE_FIXED_ASSET, venture_entity_get_id(entity), error);
		if (stored == NULL)
			return refuse(error, "The stored asset no longer exists");
		g_object_get(stored, "status", &state, NULL);
		if (state != VENTURE_ASSET_STATUS_DRAFT)
			return refuse(error, "Retain asset history; use VentureAssetService disposal or write-off");
	}
	else if (VENTURE_IS_DEPRECIATION_ENTRY(entity) || VENTURE_IS_DEFERRAL(entity) || VENTURE_IS_DEFERRAL_ENTRY(entity) ||
		VENTURE_IS_TAX_DEPRECIATION_ENTRY(entity))
		return refuse(error, "Retain schedule history; use VentureAssetService or VentureDeferralService");
	return TRUE;
}

static gboolean
venture_asset_service_dispose_impl(VentureAssetService *self, VentureEntity *asset,
	gboolean write_off, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureDatabase) db = g_weak_ref_get(&self->database);
	g_autoptr(VentureEntity) current = NULL;
	g_autoptr(VentureMoney) cost = NULL;
	g_autoptr(VentureMoney) total = NULL;
	g_autoptr(VentureMoney) proceeds = NULL;
	g_autoptr(VentureMoney) book = NULL;
	g_autoptr(VentureMoney) loss = NULL;
	g_autoptr(GDateTime) date = NULL;
	g_autoptr(GDateTime) start = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_DEPRECIATION_ENTRY);
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GPtrArray) entries = g_ptr_array_new_with_free_func(g_object_unref);
	g_autofree gchar *transaction = NULL;
	gint64 org, asset_account, accumulated_account, cash, gain_loss;
	gint state;
	guint i;
	if (!venture_entity_registry_is_type_enabled(venture_entity_registry_get_default(), "fixed_asset"))
		return refuse(error, "The assets module is disabled");
	if (!venture_database_begin(db, error))
		return FALSE;
	current = load_record(db, VENTURE_TYPE_FIXED_ASSET, venture_entity_get_id(asset), error);
	if (current == NULL)
		goto fail;
	org = venture_entity_get_organization_id(current);
	g_object_get(current, "status", &state, "cost", &cost, "in-service-at", &start,
		"asset-account-id", &asset_account, "accumulated-depreciation-account-id", &accumulated_account,
		"proceeds-account-id", &cash, "gain-loss-account-id", &gain_loss, NULL);
	g_object_get(asset, "disposed-at", &date, "disposal-proceeds", &proceeds, NULL);
	if (date == NULL)
		date = venture_time_now();
	if (write_off && proceeds == NULL && cost != NULL)
		proceeds = venture_money_new(0, cost->currency, cost->exponent);
	if (gain_loss == 0)
		g_object_get(current, "depreciation-expense-account-id", &gain_loss, NULL);
	if (state != VENTURE_ASSET_STATUS_IN_SERVICE || venture_entity_get_version(asset) != venture_entity_get_version(current) ||
		org != venture_entity_get_organization_id(asset) || date == NULL || start == NULL || g_date_time_compare(date, start) < 0 ||
		cost == NULL || proceeds == NULL || proceeds->amount < 0 || (write_off && proceeds->amount != 0))
	{
		refuse(error, "VentureAssetService requires the current in-service asset, a valid disposal date and nonnegative proceeds (zero for write-off)");
		goto fail;
	}
	if (!venture_period_guard_is_postable(VENTURE_PERIOD_GUARD(venture_database_get_period_guard(db)), db, org, date, error))
		goto fail;
	total = venture_money_new(0, cost->currency, cost->exponent);
	venture_query_set_organization(query, org);
	venture_query_set_limit(query, 0);
	if (!venture_query_add_filter_int(query, "asset-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(current), error))
		goto fail;
	rows = venture_database_find(db, query, error);
	if (rows == NULL)
		goto fail;
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(rows, i);
		gint entry_state;
		g_object_get(row, "state", &entry_state, NULL);
		if (entry_state == VENTURE_SCHEDULE_STATE_POSTED)
		{
			g_autoptr(VentureMoney) amount = NULL;
			g_autoptr(VentureMoney) next = NULL;
			g_autofree gchar *period = NULL;
			g_autoptr(GDateTime) posted_date = NULL;
			g_object_get(row, "amount", &amount, "period", &period, NULL);
			posted_date = month_end(period, error);
			if (posted_date == NULL)
				goto fail;
			if (g_date_time_compare(posted_date, date) > 0)
			{
				refuse(error, "Disposal cannot precede posted depreciation");
				goto fail;
			}
			next = venture_money_add(total, amount, error);
			if (next == NULL)
				goto fail;
			g_clear_pointer(&total, venture_money_free);
			total = g_steal_pointer(&next);
		}
	}
	book = venture_money_subtract(cost, total, error);
	if (book == NULL)
		goto fail;
	loss = venture_money_subtract(book, proceeds, error);
	if (loss == NULL)
		goto fail;
	transaction = g_strdup_printf("assets:disposal:%" G_GINT64_FORMAT, venture_entity_get_id(current));
	leg(entries, current, "fixed_asset", transaction, date, asset_account, VENTURE_LEDGER_SIDE_CREDIT, cost);
	leg(entries, current, "fixed_asset", transaction, date, accumulated_account, VENTURE_LEDGER_SIDE_DEBIT, total);
	if (proceeds->amount != 0)
		leg(entries, current, "fixed_asset", transaction, date, cash, VENTURE_LEDGER_SIDE_DEBIT, proceeds);
	{
		g_autoptr(VentureMoney) absolute = venture_money_abs(loss);
		leg(entries, current, "fixed_asset", transaction, date, gain_loss,
			loss->amount < 0 ? VENTURE_LEDGER_SIDE_CREDIT : VENTURE_LEDGER_SIDE_DEBIT, absolute);
	}
	g_object_set(current, "disposed-at", date, "disposal-proceeds", proceeds, NULL);
	if (!transition(G_OBJECT(self), current, write_off ? "write-off" : "dispose", error) ||
		!venture_posting_service_post_entries(venture_database_get_posting_service(db), entries, NULL, actor, error))
		goto fail;
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(rows, i);
		gint entry_state;
		g_object_get(row, "state", &entry_state, NULL);
		if (entry_state == VENTURE_SCHEDULE_STATE_SCHEDULED)
		{
			g_object_set(row, "state", VENTURE_SCHEDULE_STATE_SKIPPED, NULL);
			if (!save_internal(self, db, row, actor, error))
				goto fail;
		}
	}
	g_object_set(current, "status", write_off ? VENTURE_ASSET_STATUS_WRITTEN_OFF : VENTURE_ASSET_STATUS_DISPOSED,
		"disposed-at", date, "disposal-proceeds", proceeds, NULL);
	if (!save_internal(self, db, current, actor, error) || !venture_database_commit(db, error))
		goto fail;
	venture_entity_copy_properties_from(asset, current, FALSE);
	return TRUE;
fail:
	venture_database_rollback(db);
	return FALSE;
}

VentureEntity *
venture_asset_service_create_from_expense(VentureAssetService *self, VentureEntity *expense,
	const gchar *tag, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureDatabase) db = g_weak_ref_get(&self->database);
	g_autoptr(VentureEntity) source = NULL;
	g_autoptr(VentureEntity) asset = VENTURE_ENTITY(venture_fixed_asset_new());
	g_autoptr(VentureMoney) amount = NULL;
	g_autoptr(VentureMoney) zero = NULL;
	g_autoptr(GDateTime) date = NULL;
	g_autofree gchar *name = NULL;
	gint deductibility;
	gint64 venture;
	if (!venture_database_begin(db, error))
		return NULL;
	source = load_record(db, VENTURE_TYPE_EXPENSE, venture_entity_get_id(expense), error);
	if (source == NULL)
		goto fail;
	g_object_get(source, "amount", &amount, "occurred-at", &date, "description", &name,
		"deductibility", &deductibility, "venture-id", &venture, NULL);
	if (deductibility != VENTURE_DEDUCTIBILITY_CAPITAL || amount == NULL || amount->amount < 0 || date == NULL ||
		venture_entity_get_version(source) != venture_entity_get_version(expense))
	{
		refuse(error, "VentureAssetService requires a current, dated capital expense");
		goto fail;
	}
	zero = venture_money_new(0, amount->currency, amount->exponent);
	g_object_set(asset, "organization-id", venture_entity_get_organization_id(source), "name", name, "tag", tag,
		"venture-id", venture, "source-expense-id", venture_entity_get_id(source), "cost", amount,
		"salvage-value", zero, "acquired-at", date, NULL);
	if (!venture_database_save(db, asset, actor, error) || !venture_database_commit(db, error))
		goto fail;
	return g_steal_pointer(&asset);
fail:
	venture_database_rollback(db);
	return NULL;
}

static gboolean
deferral_settle(VentureDeferralService *service, VentureEntity *deferral,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureDatabase) db = g_weak_ref_get(&service->database);
	VentureAssetService *self = venture_asset_service_get(db);
	g_autoptr(VentureEntity) current = NULL;
	g_autoptr(VentureMoney) total = NULL;
	g_autoptr(GDateTime) date = NULL;
	g_autoptr(GDateTime) prior = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_DEFERRAL_ENTRY);
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GPtrArray) journals = NULL;
	g_autofree gchar *transaction = NULL;
	gint64 source, cash, org, target;
	gboolean income;
	gint kind, status;
	guint i;
	if (!venture_database_begin(db, error))
		return FALSE;
	current = load_record(db, VENTURE_TYPE_DEFERRAL, venture_entity_get_id(deferral), error);
	if (current == NULL)
		goto fail;
	org = venture_entity_get_organization_id(current);
	g_object_get(current, "kind", &kind, "status", &status, "source-account-id", &source, "total", &total, "settled-at", &prior, "target-account-id", &target, NULL);
	g_object_get(deferral, "settlement-account-id", &cash, "settled-at", &date, NULL);
	if (date == NULL)
		date = venture_time_now();
	if (kind != VENTURE_DEFERRAL_KIND_ACCRUAL || status != VENTURE_DEFERRAL_STATUS_COMPLETE || prior != NULL ||
		venture_entity_get_version(current) != venture_entity_get_version(deferral) || org != venture_entity_get_organization_id(deferral))
	{
		refuse(error, "VentureDeferralService settles a fully released, current accrual once");
		goto fail;
	}
	venture_query_set_organization(query, org);
	venture_query_set_limit(query, 0);
	if (!venture_query_add_filter_int(query, "deferral-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(current), error))
		goto fail;
	rows = venture_database_find(db, query, error);
	if (rows == NULL)
		goto fail;
	for (i = 0; i < rows->len; i++)
	{
		g_autofree gchar *period = NULL;
		g_autoptr(GDateTime) posted_date = NULL;
		g_object_get(g_ptr_array_index(rows, i), "period", &period, NULL);
		posted_date = month_end(period, error);
		if (posted_date == NULL)
			goto fail;
		if (g_date_time_compare(date, posted_date) < 0)
		{
			refuse(error, "Settlement cannot precede an accrual release");
			goto fail;
		}
	}
	if (!transition(G_OBJECT(service), current, "settle", error))
		goto fail;
	transaction = g_strdup_printf("assets:settlement:%" G_GINT64_FORMAT, venture_entity_get_id(current));
	if (!income_account(db, target, &income, error) ||
		!post_pair(db, current, "deferral", transaction, date, income ? cash : source, income ? source : cash, total, actor, error))
		goto fail;
	journals = venture_posting_service_find_source(venture_database_get_posting_service(db), "deferral", venture_entity_get_id(current), org, error);
	if (journals == NULL || journals->len != 1)
	{
		if (journals != NULL)
			refuse(error, "Expected one accrual settlement journal");
		goto fail;
	}
	g_object_set(current, "settled-at", date, "settlement-account-id", cash,
		"settlement-journal-id", venture_entity_get_id(g_ptr_array_index(journals, 0)), "operation", NULL, NULL);
	if (!save_internal(self, db, current, actor, error) || !venture_database_commit(db, error))
		goto fail;
	venture_entity_copy_properties_from(deferral, current, FALSE);
	return TRUE;
fail:
	venture_database_rollback(db);
	return FALSE;
}

/* Reclassify the actual source debit accounts, rather than guessing the
 * expense rule's chart. A profile that already capitalized needs no transfer. */
static gboolean
capitalize(VentureDatabase *db, VentureEntity *asset, GDateTime *date,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) expense = NULL;
	g_autoptr(VentureMoney) cost = NULL;
	g_autoptr(VentureMoney) source_amount = NULL;
	g_autoptr(VentureMoney) debits = NULL;
	g_autoptr(GPtrArray) journals = NULL;
	g_autoptr(GPtrArray) lines = NULL;
	g_autoptr(GPtrArray) entries = g_ptr_array_new_with_free_func(g_object_unref);
	g_autoptr(VentureQuery) query = NULL;
	g_autofree gchar *transaction = NULL;
	gint64 source_id, asset_account, journal_id = 0;
	gint64 org = venture_entity_get_organization_id(asset);
	gint deductibility;
	gboolean already_capitalized = TRUE;
	guint i;
	g_object_get(asset, "source-expense-id", &source_id, "asset-account-id", &asset_account, "cost", &cost, NULL);
	if (source_id == 0)
		return TRUE;
	expense = load_record(db, VENTURE_TYPE_EXPENSE, source_id, error);
	if (expense == NULL)
		return FALSE;
	g_object_get(expense, "amount", &source_amount, "deductibility", &deductibility, NULL);
	if (venture_entity_get_organization_id(expense) != org || !venture_money_equal(source_amount, cost) || deductibility != VENTURE_DEDUCTIBILITY_CAPITAL)
		return refuse(error, "The source capital expense must match the asset's organization and cost");
	{
		g_autoptr(VentureQuery) used_query = venture_query_new(VENTURE_TYPE_FIXED_ASSET);
		g_autoptr(GPtrArray) used = NULL;
		venture_query_set_organization(used_query, org);
		venture_query_set_include_deleted(used_query, TRUE);
		venture_query_set_limit(used_query, 0);
		if (!venture_query_add_filter_int(used_query, "source-expense-id", VENTURE_FILTER_OP_EQ, source_id, error))
			return FALSE;
		used = venture_database_find(db, used_query, error);
		if (used == NULL)
			return FALSE;
		for (i = 0; i < used->len; i++)
		{
			VentureEntity *other = g_ptr_array_index(used, i);
			gint state;
			g_object_get(other, "status", &state, NULL);
			if (venture_entity_get_id(other) != venture_entity_get_id(asset) && state != VENTURE_ASSET_STATUS_DRAFT)
				return refuse(error, "The expense has already been placed in an asset register");
		}
	}
	journals = venture_posting_service_find_source(venture_database_get_posting_service(db), "expense", source_id, org, error);
	if (journals == NULL)
		return FALSE;
	for (i = 0; i < journals->len; i++)
	{
		VentureEntity *journal = g_ptr_array_index(journals, i);
		gint state;
		gint64 reverses;
		g_object_get(journal, "state", &state, "reverses-id", &reverses, NULL);
		if (state == VENTURE_JOURNAL_POSTED && reverses == 0)
		{
			if (journal_id != 0)
				return refuse(error, "The source expense has multiple active journals; review its capitalization");
			journal_id = venture_entity_get_id(journal);
		}
	}
	if (journal_id == 0)
		return refuse(error, "The source expense needs a current posted journal before capitalization");
	query = venture_query_new(VENTURE_TYPE_JOURNAL_LINE);
	venture_query_set_organization(query, org);
	venture_query_set_limit(query, 0);
	if (!venture_query_add_filter_int(query, "journal-id", VENTURE_FILTER_OP_EQ, journal_id, error))
		return FALSE;
	lines = venture_database_find(db, query, error);
	if (lines == NULL)
		return FALSE;
	debits = venture_money_new(0, cost->currency, cost->exponent);
	transaction = g_strdup_printf("assets:capitalization:%" G_GINT64_FORMAT, venture_entity_get_id(asset));
	leg(entries, asset, "fixed_asset", transaction, date, asset_account, VENTURE_LEDGER_SIDE_DEBIT, cost);
	for (i = 0; i < lines->len; i++)
	{
		VentureEntity *line = g_ptr_array_index(lines, i);
		gint side;
		gint64 account_id;
		g_autoptr(VentureMoney) amount = NULL;
		g_autoptr(VentureMoney) next = NULL;
		g_object_get(line, "side", &side, "account-id", &account_id, "book-amount", &amount, NULL);
		if (side != VENTURE_LEDGER_SIDE_DEBIT)
			continue;
		next = venture_money_add(debits, amount, error);
		if (next == NULL)
			return FALSE;
		g_clear_pointer(&debits, venture_money_free);
		debits = g_steal_pointer(&next);
		if (account_id != asset_account)
			already_capitalized = FALSE;
		leg(entries, asset, "fixed_asset", transaction, date, account_id, VENTURE_LEDGER_SIDE_CREDIT, amount);
	}
	if (!venture_money_equal(debits, cost))
		return refuse(error, "Source journal debits do not match the asset's book cost");
	return already_capitalized || venture_posting_service_post_entries(venture_database_get_posting_service(db), entries, NULL, actor, error);
}

static gboolean
venture_deferral_service_cancel_invoice_impl(VentureDeferralService *service,
	gint64 invoice_id, GDateTime *date, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureDatabase) db = g_weak_ref_get(&service->database);
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_DEFERRAL);
	g_autoptr(GPtrArray) deferrals = NULL;
	VentureAssetService *self;
	guint i;
	if (db == NULL)
		return refuse(error, "The database has been closed");
	/* Optional modules may never have created tables; disabled existing history still counts. */
	if (!venture_entity_registry_is_type_enabled(venture_entity_registry_get_default(), "deferral"))
	{
		g_autoptr(OrmInspector) inspector = orm_inspector_new(venture_database_get_connection(db), error);
		g_autoptr(VentureEntity) prototype = VENTURE_ENTITY(venture_deferral_new());
		if (inspector == NULL)
			return FALSE;
		if (!orm_inspector_has_table(inspector, venture_entity_get_table_name(prototype), NULL, error))
			return error == NULL || *error == NULL;
	}
	self = venture_asset_service_get(db);
	if (!venture_database_begin(db, error))
		return FALSE;
	venture_query_set_limit(query, 0);
	if (!venture_query_add_filter_int(query, "source-invoice-id", VENTURE_FILTER_OP_EQ, invoice_id, error))
		goto fail;
	deferrals = venture_database_find(db, query, error);
	if (deferrals == NULL)
		goto fail;
	for (i = 0; i < deferrals->len; i++)
	{
		VentureEntity *deferral = g_ptr_array_index(deferrals, i);
		g_autoptr(VentureQuery) entries_query = venture_query_new(VENTURE_TYPE_DEFERRAL_ENTRY);
		g_autoptr(GPtrArray) entries = NULL;
		gint status;
		guint j;
		g_object_get(deferral, "status", &status, NULL);
		if (status == VENTURE_DEFERRAL_STATUS_CANCELLED)
			continue;
		venture_query_set_limit(entries_query, 0);
		if (!venture_query_add_filter_int(entries_query, "deferral-id", VENTURE_FILTER_OP_EQ,
			venture_entity_get_id(deferral), error))
			goto fail;
		entries = venture_database_find(db, entries_query, error);
		if (entries == NULL)
			goto fail;
		for (j = 0; j < entries->len; j++)
		{
			VentureEntity *entry = g_ptr_array_index(entries, j);
			gint state;
			gint64 journal_id;
			g_object_get(entry, "state", &state, "journal-id", &journal_id, NULL);
			/* Keep the posted row as evidence, with the reversal linked through its journal. */
			if (state == VENTURE_SCHEDULE_STATE_POSTED)
			{
				g_autoptr(VentureJournal) reversal = venture_posting_service_reverse(
					venture_database_get_posting_service(db), journal_id, date,
					"Void invoice recognition", actor, error);
				if (reversal == NULL)
					goto fail;
			}
			else if (state == VENTURE_SCHEDULE_STATE_SCHEDULED)
			{
				g_object_set(entry, "state", VENTURE_SCHEDULE_STATE_SKIPPED, NULL);
				if (!save_internal(self, db, entry, actor, error))
					goto fail;
			}
		}
		g_object_set(deferral, "status", VENTURE_DEFERRAL_STATUS_CANCELLED, NULL);
		if (!save_internal(self, db, deferral, actor, error))
			goto fail;
	}
	if (!venture_database_commit(db, error))
		goto fail;
	return TRUE;
fail:
	venture_database_rollback(db);
	return FALSE;
}

/* Bind consent before this operation creates derived rows or enters nested
 * transactions. All generated financial effects share this root proposal. */
gboolean
venture_asset_service_place_in_service(VentureAssetService *self,
	VentureEntity *asset, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureAccountingOperation) operation = NULL;
	g_autoptr(VentureDatabase) db = g_weak_ref_get(&self->database);
	GVariantBuilder arguments;
	gboolean result;
	if (db == NULL)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Database is unavailable");
		return FALSE;
	}
	g_return_val_if_fail(VENTURE_IS_ENTITY(asset), FALSE);
	g_variant_builder_init(&arguments, G_VARIANT_TYPE_VARDICT);
	operation = venture_accounting_operation_begin(db, "asset-place-in-service", VENTURE_ENTITY(asset), NULL,
		g_variant_builder_end(&arguments), venture_entity_get_organization_id(VENTURE_ENTITY(asset)), actor, error);
	if (operation == NULL)
		return FALSE;
	result = venture_asset_service_place_in_service_impl(self, asset, actor, error);
	if (!result)
		return FALSE;
	if (!venture_accounting_operation_finish(operation, error))
		return FALSE;
	return result;
}

/* Bind consent before this operation creates derived rows or enters nested
 * transactions. All generated financial effects share this root proposal. */
gboolean
venture_asset_service_dispose(VentureAssetService *self, VentureEntity *asset,
	gboolean write_off, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureAccountingOperation) operation = NULL;
	g_autoptr(VentureDatabase) db = g_weak_ref_get(&self->database);
	GVariantBuilder arguments;
	gboolean result;
	if (db == NULL)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Database is unavailable");
		return FALSE;
	}
	g_return_val_if_fail(VENTURE_IS_ENTITY(asset), FALSE);
	g_variant_builder_init(&arguments, G_VARIANT_TYPE_VARDICT);
	g_variant_builder_add(&arguments, "{sv}", "write_off", g_variant_new_boolean(write_off));
	operation = venture_accounting_operation_begin(db, "asset-dispose", VENTURE_ENTITY(asset), NULL,
		g_variant_builder_end(&arguments), venture_entity_get_organization_id(VENTURE_ENTITY(asset)), actor, error);
	if (operation == NULL)
		return FALSE;
	result = venture_asset_service_dispose_impl(self, asset, write_off, actor, error);
	if (!result)
		return FALSE;
	if (!venture_accounting_operation_finish(operation, error))
		return FALSE;
	return result;
}

/* Bind consent before this operation creates derived rows or enters nested
 * transactions. All generated financial effects share this root proposal. */
gboolean
venture_deferral_service_schedule(VentureDeferralService *service, VentureEntity *deferral,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureAccountingOperation) operation = NULL;
	g_autoptr(VentureDatabase) db = g_weak_ref_get(&service->database);
	GVariantBuilder arguments;
	gboolean result;
	if (db == NULL)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Database is unavailable");
		return FALSE;
	}
	g_return_val_if_fail(VENTURE_IS_ENTITY(deferral), FALSE);
	g_variant_builder_init(&arguments, G_VARIANT_TYPE_VARDICT);
	operation = venture_accounting_operation_begin(db, "deferral-schedule", VENTURE_ENTITY(deferral), NULL,
		g_variant_builder_end(&arguments), venture_entity_get_organization_id(VENTURE_ENTITY(deferral)), actor, error);
	if (operation == NULL)
		return FALSE;
	result = venture_deferral_service_schedule_impl(service, deferral, actor, error);
	if (!result)
		return FALSE;
	if (!venture_accounting_operation_finish(operation, error))
		return FALSE;
	return result;
}

/* Bind consent before this operation creates derived rows or enters nested
 * transactions. All generated financial effects share this root proposal. */
gint
venture_asset_service_run_period(VentureAssetService *self, const gchar *period,
	gint64 org, gboolean dry_run, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureAccountingOperation) operation = NULL;
	g_autoptr(VentureDatabase) db = g_weak_ref_get(&self->database);
	GVariantBuilder arguments;
	gint result;
	if (db == NULL)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Database is unavailable");
		return -1;
	}
	g_variant_builder_init(&arguments, G_VARIANT_TYPE_VARDICT);
	g_variant_builder_add(&arguments, "{sv}", "period", g_variant_new_maybe(G_VARIANT_TYPE_STRING, period != NULL ? g_variant_new_string(period) : NULL));
	g_variant_builder_add(&arguments, "{sv}", "org", g_variant_new_int64((gint64)org));
	g_variant_builder_add(&arguments, "{sv}", "dry_run", g_variant_new_boolean(dry_run));
	operation = venture_accounting_operation_begin(db, "asset-run-period", NULL, NULL,
		g_variant_builder_end(&arguments), org, actor, error);
	if (operation == NULL)
		return -1;
	result = venture_asset_service_run_period_impl(self, period, org, dry_run, actor, error);
	if (result < 0)
		return -1;
	if (!venture_accounting_operation_finish(operation, error))
		return -1;
	return result;
}

/* Bind consent before this operation creates derived rows or enters nested
 * transactions. All generated financial effects share this root proposal. */
gint
venture_asset_service_run_tax_period(VentureAssetService *self, const gchar *period,
	gint64 org, gboolean dry_run, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureAccountingOperation) operation = NULL;
	g_autoptr(VentureDatabase) db = g_weak_ref_get(&self->database);
	GVariantBuilder arguments;
	gint result;
	if (db == NULL)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Database is unavailable");
		return -1;
	}
	g_variant_builder_init(&arguments, G_VARIANT_TYPE_VARDICT);
	g_variant_builder_add(&arguments, "{sv}", "period", g_variant_new_maybe(G_VARIANT_TYPE_STRING, period != NULL ? g_variant_new_string(period) : NULL));
	g_variant_builder_add(&arguments, "{sv}", "org", g_variant_new_int64((gint64)org));
	g_variant_builder_add(&arguments, "{sv}", "dry_run", g_variant_new_boolean(dry_run));
	operation = venture_accounting_operation_begin(db, "asset-run-tax-period", NULL, NULL,
		g_variant_builder_end(&arguments), org, actor, error);
	if (operation == NULL)
		return -1;
	result = venture_asset_service_run_tax_period_impl(self, period, org, dry_run, actor, error);
	if (result < 0)
		return -1;
	if (!venture_accounting_operation_finish(operation, error))
		return -1;
	return result;
}

/* Bind consent before this operation creates derived rows or enters nested
 * transactions. All generated financial effects share this root proposal. */
gboolean
venture_deferral_service_cancel_invoice(VentureDeferralService *service,
	gint64 invoice_id, GDateTime *date, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureAccountingOperation) operation = NULL;
	g_autoptr(VentureDatabase) db = g_weak_ref_get(&service->database);
	GVariantBuilder arguments;
	g_autoptr(VentureEntity) subject = NULL;
	gboolean result;
	g_autofree gchar *date_text = NULL;
	if (db == NULL)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Database is unavailable");
		return FALSE;
	}
	subject = venture_database_get(db, VENTURE_TYPE_INVOICE, invoice_id, error);
	if (subject == NULL)
		return FALSE;
	date_text = date != NULL ? g_date_time_format_iso8601(date) : NULL;
	g_variant_builder_init(&arguments, G_VARIANT_TYPE_VARDICT);
	g_variant_builder_add(&arguments, "{sv}", "invoice_id", g_variant_new_int64((gint64)invoice_id));
	g_variant_builder_add(&arguments, "{sv}", "date", g_variant_new_maybe(G_VARIANT_TYPE_STRING, date_text != NULL ? g_variant_new_string(date_text) : NULL));
	operation = venture_accounting_operation_begin(db, "deferral-cancel-invoice", subject, NULL,
		g_variant_builder_end(&arguments), venture_entity_get_organization_id(subject), actor, error);
	if (operation == NULL)
		return FALSE;
	result = venture_deferral_service_cancel_invoice_impl(service, invoice_id, date, actor, error);
	if (!result)
		return FALSE;
	if (!venture_accounting_operation_finish(operation, error))
		return FALSE;
	return result;
}

/* The month holding the last instant before the cutoff is the final month of
 * source history; scheduling resumes with the month after it. */
static GDateTime *
first_period_after(GDateTime *cutoff)
{
	g_autoptr(GDateTime) last = g_date_time_add_seconds(cutoff, -1);
	g_autoptr(GDateTime) month = g_date_time_new_utc(g_date_time_get_year(last), g_date_time_get_month(last), 1, 0, 0, 0);
	return g_date_time_add_months(month, 1);
}

static gboolean
venture_asset_service_import_opening_impl(VentureAssetService *self, VentureEntity *asset,
	const VentureMoney *accumulated, GDateTime *cutoff, gint64 equity_account_id,
	const gchar *source_type, gint64 source_id, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureDatabase) db = g_weak_ref_get(&self->database);
	g_autoptr(VentureEntity) current = NULL;
	g_autoptr(VentureMoney) cost = NULL;
	g_autoptr(VentureMoney) salvage = NULL;
	g_autoptr(VentureMoney) base = NULL;
	g_autoptr(VentureMoney) remaining = NULL;
	g_autoptr(VentureMoney) book = NULL;
	g_autoptr(GDateTime) start = NULL;
	g_autoptr(GDateTime) acquired = NULL;
	g_autoptr(GDateTime) first = NULL;
	g_autoptr(GPtrArray) parts = NULL;
	g_autofree gchar *tag = NULL;
	g_autofree gchar *note = NULL;
	gint64 months, account, accumulated_account, expense, journal_id = 0, elapsed, left;
	gint state, method;
	guint i;
	gint64 org = venture_entity_get_organization_id(asset);
	if (!venture_entity_registry_is_type_enabled(venture_entity_registry_get_default(), "fixed_asset"))
		return refuse(error, "The assets module is disabled");
	if (accumulated == NULL || cutoff == NULL)
		return refuse(error, "An opening import needs the accumulated depreciation and the cutoff");
	if (!venture_database_begin(db, error))
		return FALSE;
	current = load_record(db, VENTURE_TYPE_FIXED_ASSET, venture_entity_get_id(asset), error);
	if (current == NULL)
		goto fail;
	g_object_get(current, "status", &state, NULL);
	if (state != VENTURE_ASSET_STATUS_DRAFT || venture_entity_get_version(current) != venture_entity_get_version(asset))
	{
		refuse(error, "VentureAssetService requires the current draft asset");
		goto fail;
	}
	venture_entity_copy_properties_from(current, asset, TRUE);
	g_object_get(asset, "cost", &cost, "salvage-value", &salvage, "in-service-at", &start,
		"acquired-at", &acquired, "useful-life-months", &months, "method", &method, "tag", &tag,
		"asset-account-id", &account, "accumulated-depreciation-account-id", &accumulated_account,
		"depreciation-expense-account-id", &expense, NULL);
	if (cost == NULL || salvage == NULL || acquired == NULL || start == NULL ||
		cost->amount < 0 || salvage->amount < 0 || months < 1 || months > 1200 ||
		g_date_time_compare(start, acquired) < 0)
	{
		refuse(error, "An asset needs nonnegative cost and salvage, acquisition and service dates, and 1-1200 months");
		goto fail;
	}
	if (g_date_time_compare(start, cutoff) > 0)
	{
		refuse(error, "An opening asset must be in service on or before the cutoff");
		goto fail;
	}
	base = venture_money_subtract(cost, salvage, error);
	if (base == NULL)
		goto fail;
	if (base->amount < 0)
	{
		refuse(error, "Salvage cannot exceed cost");
		goto fail;
	}
	remaining = venture_money_subtract(base, accumulated, error);
	book = venture_money_subtract(cost, accumulated, error);
	if (remaining == NULL || book == NULL)
		goto fail;
	if (accumulated->amount < 0 || remaining->amount < 0)
	{
		refuse(error, "Accumulated depreciation must lie between zero and cost less salvage (rule: opening-accumulated-within-basis)");
		goto fail;
	}
	if (!check_account(db, account, org, error) || !check_account(db, accumulated_account, org, error) ||
		!check_account(db, expense, org, error) || !check_account(db, equity_account_id, org, error))
		goto fail;
	first = first_period_after(cutoff);
	elapsed = ((gint64)g_date_time_get_year(first) * 12 + g_date_time_get_month(first)) -
		((gint64)g_date_time_get_year(start) * 12 + g_date_time_get_month(start));
	left = months - elapsed;
	if (left > 0 && method != VENTURE_ASSET_METHOD_NONE && remaining->amount > 0)
	{
		parts = split(remaining, (guint)left, error);
		if (parts == NULL)
			goto fail;
		if (method == VENTURE_ASSET_METHOD_DECLINING_BALANCE)
		{
			g_autoptr(VentureMoney) open = venture_money_copy(remaining);
			g_autoptr(VentureMoney) carrying = venture_money_copy(book);
			for (i = 0; i < parts->len; i++)
			{
				g_autoptr(VentureMoney) charge = venture_money_multiply_rational(carrying, 2, months, error);
				g_autoptr(VentureMoney) next = NULL;
				g_autoptr(VentureMoney) after = NULL;
				if (charge == NULL)
					goto fail;
				if (i + 1 == parts->len || venture_money_compare(charge, open) > 0)
				{
					g_clear_pointer(&charge, venture_money_free);
					charge = venture_money_copy(open);
				}
				next = venture_money_subtract(open, charge, error);
				after = venture_money_subtract(carrying, charge, error);
				if (next == NULL || after == NULL)
					goto fail;
				g_clear_pointer(&open, venture_money_free);
				g_clear_pointer(&carrying, venture_money_free);
				open = g_steal_pointer(&next);
				carrying = g_steal_pointer(&after);
				venture_money_free(g_ptr_array_index(parts, i));
				g_ptr_array_index(parts, i) = g_steal_pointer(&charge);
			}
		}
	}
	if (!transition(G_OBJECT(self), current, "import-opening", error))
		goto fail;
	if (cost->amount > 0)
	{
		g_autoptr(VentureJournal) header = venture_journal_new();
		g_autoptr(VentureJournal) posted = NULL;
		g_autoptr(GPtrArray) lines = g_ptr_array_new_with_free_func(g_object_unref);
		g_autofree gchar *memo = g_strdup_printf("Opening fixed asset %s", tag != NULL ? tag : "");
		VentureJournalLine *line;
		g_object_set(header, "organization-id", org, "source-type", source_type, "source-id", source_id,
			"occurred-at", cutoff, "currency", cost->currency, "memo", memo, NULL);
		line = venture_journal_line_new();
		g_object_set(line, "account-id", account, "side", VENTURE_LEDGER_SIDE_DEBIT, "amount", cost, NULL);
		g_ptr_array_add(lines, line);
		if (accumulated->amount > 0)
		{
			line = venture_journal_line_new();
			g_object_set(line, "account-id", accumulated_account, "side", VENTURE_LEDGER_SIDE_CREDIT,
				"amount", accumulated, NULL);
			g_ptr_array_add(lines, line);
		}
		if (book->amount > 0)
		{
			line = venture_journal_line_new();
			g_object_set(line, "account-id", equity_account_id, "side", VENTURE_LEDGER_SIDE_CREDIT,
				"amount", book, NULL);
			g_ptr_array_add(lines, line);
		}
		posted = venture_posting_service_post(venture_database_get_posting_service(db), header, lines, NULL, actor, error);
		if (posted == NULL)
			goto fail;
		journal_id = venture_entity_get_id(VENTURE_ENTITY(posted));
	}
	if (accumulated->amount > 0)
	{
		g_autoptr(VentureEntity) row = VENTURE_ENTITY(venture_depreciation_entry_new());
		g_autoptr(GDateTime) last = g_date_time_add_months(first, -1);
		g_autofree gchar *period = g_date_time_format(last, "%Y-%m");
		g_object_set(row, "organization-id", org, "asset-id", venture_entity_get_id(asset),
			"period", period, "amount", accumulated, "journal-id", journal_id,
			"state", VENTURE_SCHEDULE_STATE_POSTED, NULL);
		if (!save_internal(self, db, row, actor, error))
			goto fail;
	}
	for (i = 0; parts != NULL && i < parts->len; i++)
	{
		g_autoptr(VentureEntity) row = VENTURE_ENTITY(venture_depreciation_entry_new());
		g_autoptr(GDateTime) date = g_date_time_add_months(first, (gint)i);
		g_autofree gchar *period = g_date_time_format(date, "%Y-%m");
		g_object_set(row, "organization-id", org, "asset-id", venture_entity_get_id(asset),
			"period", period, "amount", g_ptr_array_index(parts, i), NULL);
		if (!save_internal(self, db, row, actor, error))
			goto fail;
	}
	{
		g_autofree gchar *label = g_date_time_format(first, "%Y-%m");
		note = g_strdup_printf("Imported at cutover: %" G_GINT64_FORMAT " months of source history carried as an opening balance; schedule resumes %s for %" G_GINT64_FORMAT " months.",
			elapsed, label, left > 0 ? left : 0);
	}
	g_object_set(current, "operation", NULL, "schedule-note", note, NULL);
	g_object_set(current, "status", VENTURE_ASSET_STATUS_IN_SERVICE, "in-service-at", start, NULL);
	if (!save_internal(self, db, current, actor, error) || !venture_database_commit(db, error))
		goto fail;
	venture_entity_copy_properties_from(asset, current, FALSE);
	return TRUE;
fail:
	venture_database_rollback(db);
	return FALSE;
}

gboolean
venture_asset_service_import_opening(VentureAssetService *self, VentureEntity *asset,
	const VentureMoney *accumulated, GDateTime *cutoff, gint64 equity_account_id,
	const gchar *source_type, gint64 source_id, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureAccountingOperation) operation = NULL;
	g_autoptr(VentureDatabase) db = g_weak_ref_get(&self->database);
	GVariantBuilder arguments;
	gboolean result;
	if (db == NULL)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Database is unavailable");
		return FALSE;
	}
	g_return_val_if_fail(VENTURE_IS_ENTITY(asset), FALSE);
	g_variant_builder_init(&arguments, G_VARIANT_TYPE_VARDICT);
	if (accumulated != NULL)
		g_variant_builder_add(&arguments, "{sv}", "accumulated", g_variant_new_int64(accumulated->amount));
	operation = venture_accounting_operation_begin(db, "asset-import-opening", VENTURE_ENTITY(asset), NULL,
		g_variant_builder_end(&arguments), venture_entity_get_organization_id(VENTURE_ENTITY(asset)), actor, error);
	if (operation == NULL)
		return FALSE;
	result = venture_asset_service_import_opening_impl(self, asset, accumulated, cutoff, equity_account_id,
		source_type, source_id, actor, error);
	if (!result)
		return FALSE;
	if (!venture_accounting_operation_finish(operation, error))
		return FALSE;
	return result;
}

static gboolean
venture_asset_service_rollback_opening_impl(VentureAssetService *self, gint64 asset_id,
	GDateTime *date, const gchar *tag_suffix, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureDatabase) db = g_weak_ref_get(&self->database);
	g_autoptr(VentureEntity) current = NULL;
	g_autoptr(VentureMoney) cost = NULL;
	g_autoptr(VentureMoney) zero = NULL;
	g_autofree gchar *note = NULL;
	g_autofree gchar *combined = NULL;
	gint state;
	guint kind;
	if (!venture_entity_registry_is_type_enabled(venture_entity_registry_get_default(), "fixed_asset"))
		return refuse(error, "The assets module is disabled");
	if (date == NULL)
		return refuse(error, "A rollback date is required");
	if (!venture_database_begin(db, error))
		return FALSE;
	current = load_record(db, VENTURE_TYPE_FIXED_ASSET, asset_id, error);
	if (current == NULL)
		goto fail;
	g_object_get(current, "status", &state, "cost", &cost, "schedule-note", &note, NULL);
	if (state != VENTURE_ASSET_STATUS_IN_SERVICE || cost == NULL)
	{
		refuse(error, "Only an in-service asset can have its opening import rolled back");
		goto fail;
	}
	if (!transition(G_OBJECT(self), current, "rollback-opening", error))
		goto fail;
	for (kind = 0; kind < 2; kind++)
	{
		g_autoptr(GPtrArray) rows = scheduled(db, kind == 0 ? VENTURE_TYPE_DEPRECIATION_ENTRY :
			VENTURE_TYPE_TAX_DEPRECIATION_ENTRY, venture_entity_get_organization_id(current), NULL, error);
		guint i;
		if (rows == NULL)
			goto fail;
		for (i = 0; i < rows->len; i++)
		{
			VentureEntity *row = g_ptr_array_index(rows, i);
			gint64 owner = 0;
			g_object_get(row, "asset-id", &owner, NULL);
			if (owner != asset_id)
				continue;
			g_object_set(row, "state", VENTURE_SCHEDULE_STATE_SKIPPED, NULL);
			if (!save_internal(self, db, row, actor, error))
				goto fail;
		}
	}
	zero = venture_money_new(0, cost->currency, cost->exponent);
	combined = g_strdup_printf("%s%sCutover rollback: opening journal reversed, schedule skipped.",
		note != NULL ? note : "", note != NULL ? " " : "");
	g_object_set(current, "status", VENTURE_ASSET_STATUS_WRITTEN_OFF, "disposed-at", date,
		"disposal-proceeds", zero, "schedule-note", combined, NULL);
	/* Tags are unique per organization, retired assets included; freeing the
	 * tag is what lets a corrected migration place the same asset again. */
	if (tag_suffix != NULL && *tag_suffix != '\0')
	{
		g_autofree gchar *tag = NULL;
		g_autofree gchar *renamed = NULL;
		g_object_get(current, "tag", &tag, NULL);
		renamed = g_strconcat(tag != NULL ? tag : "", tag_suffix, NULL);
		g_object_set(current, "tag", renamed, NULL);
	}
	if (!save_internal(self, db, current, actor, error) || !venture_database_commit(db, error))
		goto fail;
	return TRUE;
fail:
	venture_database_rollback(db);
	return FALSE;
}

gboolean
venture_asset_service_rollback_opening(VentureAssetService *self, gint64 asset_id,
	GDateTime *date, const gchar *tag_suffix, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureAccountingOperation) operation = NULL;
	g_autoptr(VentureDatabase) db = g_weak_ref_get(&self->database);
	g_autoptr(VentureEntity) subject = NULL;
	GVariantBuilder arguments;
	gboolean result;
	if (db == NULL)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Database is unavailable");
		return FALSE;
	}
	subject = venture_database_get(db, VENTURE_TYPE_FIXED_ASSET, asset_id, error);
	if (subject == NULL)
		return FALSE;
	g_variant_builder_init(&arguments, G_VARIANT_TYPE_VARDICT);
	if (tag_suffix != NULL)
		g_variant_builder_add(&arguments, "{sv}", "tag_suffix", g_variant_new_string(tag_suffix));
	operation = venture_accounting_operation_begin(db, "asset-rollback-opening", subject, NULL,
		g_variant_builder_end(&arguments), venture_entity_get_organization_id(subject), actor, error);
	if (operation == NULL)
		return FALSE;
	result = venture_asset_service_rollback_opening_impl(self, asset_id, date, tag_suffix, actor, error);
	if (!result)
		return FALSE;
	if (!venture_accounting_operation_finish(operation, error))
		return FALSE;
	return result;
}
