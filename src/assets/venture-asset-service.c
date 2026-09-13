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
	parts = split(base, (guint)months, error);
	if (parts == NULL)
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
	venture_entity_copy_properties_from(current, asset, TRUE);
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
	if (self->permit == entity || !VENTURE_IS_FIXED_ASSET(entity))
		return TRUE;
	g_object_get(entity, "operation", &operation, NULL);
	if (venture_string_is_empty(operation))
		return TRUE;
	*handled = TRUE;
	if (g_str_equal(operation, "place"))
		return venture_asset_service_place_in_service(self, entity, actor, error);
	return refuse(error, "Unknown VentureAssetService operation");
}
