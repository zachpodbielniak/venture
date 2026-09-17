/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

G_DEFINE_INTERFACE(VenturePeriodGuard, venture_period_guard, G_TYPE_OBJECT)
static void venture_period_guard_default_init(VenturePeriodGuardInterface *iface) { }

gboolean
venture_period_guard_is_postable(VenturePeriodGuard *self, VentureDatabase *database,
	gint64 organization_id, GDateTime *date, GError **error)
{
	VenturePeriodGuardInterface *iface;
	g_return_val_if_fail(VENTURE_IS_PERIOD_GUARD(self), FALSE);
	iface = VENTURE_PERIOD_GUARD_GET_IFACE(self);
	g_return_val_if_fail(NULL != iface->is_postable, FALSE);
	return iface->is_postable(self, database, organization_id, date, error);
}

static gboolean
default_is_postable(VenturePeriodGuard *guard, VentureDatabase *database,
	gint64 organization_id, GDateTime *date, GError **error)
{
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) periods = NULL;
	guint i;

	if (G_TYPE_INVALID == venture_entity_registry_lookup(
		venture_entity_registry_get_default(), "fiscal_period"))
		return TRUE;
	/* Zero is legacy unfiled data, never a wildcard over other companies. */
	query = venture_query_new(VENTURE_TYPE_FISCAL_PERIOD);
	venture_query_add_filter_int(query, "organization-id", VENTURE_FILTER_OP_EQ, organization_id, NULL);
	periods = venture_database_find(database, query, error);
	if (NULL == periods)
		return FALSE;
	if (0 == periods->len)
		return TRUE;
	for (i = 0; (NULL != date) && (i < periods->len); i++)
	{
		VentureEntity *period = g_ptr_array_index(periods, i);
		g_autoptr(GDateTime) start = NULL;
		g_autoptr(GDateTime) end = NULL;
		g_autofree gchar *name = NULL;
		gint state;
		g_object_get(period, "start-at", &start, "end-at", &end,
			"name", &name, "state", &state, NULL);
		if ((g_date_time_compare(date, start) >= 0) && (g_date_time_compare(date, end) < 0))
		{
			if (VENTURE_PERIOD_OPEN == state)
				return TRUE;
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT,
				"Period '%s' is %s for organization %" G_GINT64_FORMAT,
				name, venture_enum_to_nick(VENTURE_TYPE_PERIOD_STATE, state), organization_id);
			return FALSE;
		}
	}
	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		"Organization %" G_GINT64_FORMAT " requires a date within its configured fiscal periods", organization_id);
	return FALSE;
}

static void default_guard_iface_init(VenturePeriodGuardInterface *iface) { iface->is_postable = default_is_postable; }
struct _VentureDefaultPeriodGuard { GObject parent_instance; };
G_DEFINE_TYPE_WITH_CODE(VentureDefaultPeriodGuard, venture_default_period_guard, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(VENTURE_TYPE_PERIOD_GUARD, default_guard_iface_init))
static void venture_default_period_guard_class_init(VentureDefaultPeriodGuardClass *klass) { }
static void venture_default_period_guard_init(VentureDefaultPeriodGuard *self) { }

VenturePeriodGuard *
venture_default_period_guard_new(void)
{
	return g_object_new(VENTURE_TYPE_DEFAULT_PERIOD_GUARD, NULL);
}

struct _VenturePeriodGuardRegistry { GObject parent_instance; GPtrArray *guards; };
static gboolean
registry_is_postable(VenturePeriodGuard *guard, VentureDatabase *database,
	gint64 organization_id, GDateTime *date, GError **error)
{
	VenturePeriodGuardRegistry *self = VENTURE_PERIOD_GUARD_REGISTRY(guard);
	guint i;
	for (i = 0; i < self->guards->len; i++)
		if (!venture_period_guard_is_postable(g_ptr_array_index(self->guards, i),
			database, organization_id, date, error))
			return FALSE;
	return TRUE;
}
static void registry_iface_init(VenturePeriodGuardInterface *iface) { iface->is_postable = registry_is_postable; }
G_DEFINE_TYPE_WITH_CODE(VenturePeriodGuardRegistry, venture_period_guard_registry, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(VENTURE_TYPE_PERIOD_GUARD, registry_iface_init))
static void
registry_finalize(GObject *object)
{
	g_ptr_array_unref(VENTURE_PERIOD_GUARD_REGISTRY(object)->guards);
	G_OBJECT_CLASS(venture_period_guard_registry_parent_class)->finalize(object);
}
static void venture_period_guard_registry_class_init(VenturePeriodGuardRegistryClass *klass)
{ G_OBJECT_CLASS(klass)->finalize = registry_finalize; }
static void venture_period_guard_registry_init(VenturePeriodGuardRegistry *self)
{ self->guards = g_ptr_array_new_with_free_func(g_object_unref); }

VenturePeriodGuardRegistry *venture_period_guard_registry_new(void)
{ return g_object_new(VENTURE_TYPE_PERIOD_GUARD_REGISTRY, NULL); }

void
venture_period_guard_registry_add(VenturePeriodGuardRegistry *self, VenturePeriodGuard *guard)
{
	g_return_if_fail(VENTURE_IS_PERIOD_GUARD_REGISTRY(self));
	g_return_if_fail(VENTURE_IS_PERIOD_GUARD(guard));
	g_ptr_array_add(self->guards, guard);
}

VenturePeriodGuardRegistry *
venture_database_get_period_guard(VentureDatabase *database)
{
	VenturePeriodGuardRegistry *registry;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	registry = g_object_get_data(G_OBJECT(database), "venture-period-guards");
	if (NULL == registry)
	{
		registry = venture_period_guard_registry_new();
		venture_period_guard_registry_add(registry, venture_default_period_guard_new());
		g_object_set_data_full(G_OBJECT(database), "venture-period-guards", registry, g_object_unref);
	}
	return registry;
}

static gboolean
guard_record(VentureDatabase *database, VentureEntity *entity, GError **error)
{
	g_autoptr(GDateTime) date = NULL;
	g_autoptr(VentureEntity) invoice = NULL;
	const gchar *field;
	if (VENTURE_IS_INVOICE_LINE(entity))
	{
		gint64 id;
		g_object_get(entity, "invoice-id", &id, NULL);
		invoice = venture_database_get(database, VENTURE_TYPE_INVOICE, id, error);
		if (NULL == invoice)
			return FALSE;
		entity = invoice;
	}
	field = VENTURE_IS_INVOICE(entity) ? "issued-at" : "occurred-at";
	/* A migrated invoice entered this ledger at its cutover instant; its
	 * source issue date may sit in a period that was never this system's. */
	if (VENTURE_IS_INVOICE(entity))
		g_object_get(entity, "opening-at", &date, NULL);
	if (NULL == date)
		g_object_get(entity, field, &date, NULL);
	return venture_period_guard_is_postable(VENTURE_PERIOD_GUARD(venture_database_get_period_guard(database)),
		database, venture_entity_get_organization_id(entity), date, error);
}

gboolean
venture_periods_validate_financial(VentureDatabase *database, VentureEntity *entity,
	VentureEntity *previous, GError **error)
{
	if (!VENTURE_IS_SALE(entity) && !VENTURE_IS_EXPENSE(entity) && !VENTURE_IS_INVOICE(entity) &&
		!VENTURE_IS_LEDGER_ENTRY(entity) && !VENTURE_IS_INVOICE_LINE(entity))
		return TRUE;
	/* Checking the stored row prevents moving a locked entry out of its
	 * period or organization and changing it in the same request. */
	if ((NULL != previous) && !guard_record(database, previous, error))
		return FALSE;
	return guard_record(database, entity, error);
}
