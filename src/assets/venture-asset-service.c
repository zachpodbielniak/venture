/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

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
	g_autoptr(VentureEntity) account = venture_database_get(db, VENTURE_TYPE_ACCOUNT, id, error);
	gboolean active;
	if (account == NULL)
		return FALSE;
	g_object_get(account, "active", &active, NULL);
	if (!active || venture_entity_get_organization_id(account) != org)
		return refuse(error, "Choose active accounts in the asset's organization");
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

gboolean
venture_asset_service_place_in_service(VentureAssetService *self,
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
	current = venture_database_get(db, VENTURE_TYPE_FIXED_ASSET, venture_entity_get_id(asset), error);
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
	if (cost == NULL || salvage == NULL || start == NULL || acquired == NULL ||
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
	g_object_set(current, "operation", NULL, NULL);
	g_object_set(current, "status", VENTURE_ASSET_STATUS_IN_SERVICE, "in-service-at", start, NULL);
	if (!save_internal(self, db, current, actor, error) || !venture_database_commit(db, error))
		goto fail;
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
		!VENTURE_IS_DEFERRAL(entity) && !VENTURE_IS_DEFERRAL_ENTRY(entity))
		return TRUE;
	self = venture_asset_service_get(db);
	if (VENTURE_IS_DEFERRAL(entity) && self->permit != entity && venture_entity_get_id(entity) == 0)
	{
		g_autoptr(VentureDeferralService) deferrals = venture_deferral_service_new(db);
		*handled = TRUE;
		return venture_deferral_service_schedule(deferrals, entity, actor, error);
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

gboolean
venture_deferral_service_schedule(VentureDeferralService *service, VentureEntity *deferral,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureDatabase) db = g_weak_ref_get(&service->database);
	VentureAssetService *self = venture_asset_service_get(db);
	g_autoptr(VentureEntity) row = VENTURE_ENTITY(venture_deferral_new());
	g_autoptr(VentureMoney) total = NULL;
	g_autoptr(GDateTime) start = NULL;
	g_autoptr(GPtrArray) parts = NULL;
	gint64 months, source, target;
	gint kind, status;
	gint64 org = venture_entity_get_organization_id(deferral);
	guint i;
	if (venture_entity_get_id(deferral) != 0)
		return refuse(error, "VentureDeferralService only schedules new deferrals");
	if (!venture_entity_registry_is_type_enabled(venture_entity_registry_get_default(), "deferral"))
		return refuse(error, "The assets module is disabled");
	g_object_get(deferral, "total", &total, "start", &start, "months", &months,
		"source-account-id", &source, "target-account-id", &target, "kind", &kind, "status", &status, NULL);
	if (total == NULL || total->amount <= 0 || start == NULL || months < 1 || months > 1200 ||
		status != VENTURE_DEFERRAL_STATUS_ACTIVE)
		return refuse(error, "VentureDeferralService requires a positive total, start, 1-1200 months and active status");
	if (!venture_database_begin(db, error))
		return FALSE;
	if (!check_account(db, source, org, error) || !check_account(db, target, org, error))
		goto fail;
	parts = split(total, (guint)months, error);
	if (parts == NULL)
		goto fail;
	venture_entity_copy_properties_from(row, deferral, TRUE);
	if (!save_internal(self, db, row, actor, error))
		goto fail;
	if (kind == VENTURE_DEFERRAL_KIND_PREPAYMENT)
	{
		g_autofree gchar *transaction = g_strdup_printf("deferral-source:%" G_GINT64_FORMAT, venture_entity_get_id(row));
		/* The source expense already debited expense. This journal moves
		 * that cost to prepaid; the monthly release recognizes it once. */
		if (!post_pair(db, row, "deferral", transaction, start, source, target, total, actor, error))
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

gint
venture_asset_service_run_period(VentureAssetService *self, const gchar *period,
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
			parent = venture_database_get(db, kind == 0 ? VENTURE_TYPE_FIXED_ASSET : VENTURE_TYPE_DEFERRAL, parent_id, error);
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

gboolean
venture_assets_check_removal(VentureEntity *entity, GError **error)
{
	gint state;
	if (VENTURE_IS_FIXED_ASSET(entity))
	{
		g_object_get(entity, "status", &state, NULL);
		if (state != VENTURE_ASSET_STATUS_DRAFT)
			return refuse(error, "Retain asset history; use VentureAssetService disposal or write-off");
	}
	else if (VENTURE_IS_DEPRECIATION_ENTRY(entity) || VENTURE_IS_DEFERRAL(entity) || VENTURE_IS_DEFERRAL_ENTRY(entity))
		return refuse(error, "Retain schedule history; use VentureAssetService or VentureDeferralService");
	return TRUE;
}

gboolean
venture_asset_service_dispose(VentureAssetService *self, VentureEntity *asset,
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
	current = venture_database_get(db, VENTURE_TYPE_FIXED_ASSET, venture_entity_get_id(asset), error);
	if (current == NULL)
		goto fail;
	org = venture_entity_get_organization_id(current);
	g_object_get(current, "status", &state, "cost", &cost, "in-service-at", &start,
		"asset-account-id", &asset_account, "accumulated-depreciation-account-id", &accumulated_account,
		"proceeds-account-id", &cash, "gain-loss-account-id", &gain_loss, NULL);
	g_object_get(asset, "disposed-at", &date, "disposal-proceeds", &proceeds, NULL);
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
	if (!venture_posting_service_post_entries(venture_database_get_posting_service(db), entries, NULL, actor, error))
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
	source = venture_database_get(db, VENTURE_TYPE_EXPENSE, venture_entity_get_id(expense), error);
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
