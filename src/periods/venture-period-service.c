/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

struct _VenturePeriodService
{
	GObject parent_instance;
	GWeakRef database;
	GWeakRef context;
	VenturePeriodChecklist *checklist;
	VentureEntity *saving;
};
G_DEFINE_FINAL_TYPE(VenturePeriodService, venture_period_service, G_TYPE_OBJECT)

static void
service_get_property(GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
	VenturePeriodService *self = VENTURE_PERIOD_SERVICE(object);
	if (1 == id)
		g_value_take_object(value, g_weak_ref_get(&self->database));
	else if (2 == id)
		g_value_take_object(value, g_weak_ref_get(&self->context));
	else
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
}

static void
service_set_property(GObject *object, guint id, const GValue *value, GParamSpec *pspec)
{
	VenturePeriodService *self = VENTURE_PERIOD_SERVICE(object);
	if (1 == id)
		g_weak_ref_set(&self->database, g_value_get_object(value));
	else if (2 == id)
		g_weak_ref_set(&self->context, g_value_get_object(value));
	else
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
}

static void
service_finalize(GObject *object)
{
	VenturePeriodService *self = VENTURE_PERIOD_SERVICE(object);
	g_weak_ref_clear(&self->database);
	g_weak_ref_clear(&self->context);
	g_clear_object(&self->checklist);
	G_OBJECT_CLASS(venture_period_service_parent_class)->finalize(object);
}

static void
venture_period_service_class_init(VenturePeriodServiceClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	object_class->get_property = service_get_property;
	object_class->set_property = service_set_property;
	object_class->finalize = service_finalize;
	g_object_class_install_property(object_class, 1,
		g_param_spec_object("database", "Database", "Owning repository",
			VENTURE_TYPE_DATABASE, G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
	g_object_class_install_property(object_class, 2,
		g_param_spec_object("context", "Context", "Reporting context", VENTURE_TYPE_CONTEXT,
			G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
}

static void
venture_period_service_init(VenturePeriodService *self)
{
	g_weak_ref_init(&self->database, NULL);
	g_weak_ref_init(&self->context, NULL);
	self->checklist = venture_period_checklist_new();
}

VenturePeriodChecklist *venture_period_service_get_checklist(VenturePeriodService *self)
{ return self->checklist; }

void venture_period_service_install(VentureContext *context)
{
	VenturePeriodService *self = venture_period_service_get(venture_context_get_database(context));
	g_object_set(self, "context", context, NULL);
}

VenturePeriodService *
venture_period_service_get(VentureDatabase *database)
{
	VenturePeriodService *self;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	self = g_object_get_data(G_OBJECT(database), "venture-period-service");
	if (NULL == self)
	{
		self = g_object_new(VENTURE_TYPE_PERIOD_SERVICE, "database", database, NULL);
		g_object_set_data_full(G_OBJECT(database), "venture-period-service", self, g_object_unref);
	}
	return self;
}

static gboolean
save_internal(VenturePeriodService *self, VentureDatabase *database,
	VentureEntity *entity, const VentureActor *actor, GError **error)
{
	VentureEntity *previous = self->saving;
	gboolean ok;
	self->saving = entity;
	ok = venture_database_save(database, entity, actor, error);
	self->saving = previous;
	return ok;
}

static gboolean
validate_calendar(VentureDatabase *database, VentureEntity *entity,
	VentureEntity *previous, GError **error)
{
	g_autoptr(GDateTime) start = NULL;
	g_autoptr(GDateTime) end = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	gint64 organization_id = venture_entity_get_organization_id(entity);
	guint i;
	gboolean adjacent = FALSE;

	g_object_get(entity, "start-at", &start, "end-at", &end, NULL);
	if ((organization_id <= 0) || (NULL == start) || (NULL == end) ||
		(g_date_time_compare(start, end) >= 0) || venture_entity_is_deleted(entity))
	{
		venture_set_error_validation(error, "start-at", "A fiscal calendar needs an organization and a nonempty date range");
		return FALSE;
	}
	if (NULL != previous)
	{
		g_autoptr(JsonNode) diff = venture_entity_diff(previous, entity);
		JsonObject *object = json_node_get_object(diff);
		if (json_object_has_member(object, "start_at") || json_object_has_member(object, "end_at") ||
			json_object_has_member(object, "organization_id") || json_object_has_member(object, "period_length") ||
			json_object_has_member(object, "fiscal_year_id"))
		{
			venture_set_error_validation(error, "start-at", "Saved fiscal calendar boundaries and organization are immutable");
			return FALSE;
		}
		return TRUE;
	}
	query = venture_query_new(G_OBJECT_TYPE(entity));
	venture_query_set_organization(query, organization_id);
	rows = venture_database_find(database, query, error);
	if (NULL == rows)
		return FALSE;
	for (i = 0; i < rows->len; i++)
	{
		g_autoptr(GDateTime) a = NULL;
		g_autoptr(GDateTime) b = NULL;
		g_object_get(g_ptr_array_index(rows, i), "start-at", &a, "end-at", &b, NULL);
		if ((g_date_time_compare(start, b) < 0) && (g_date_time_compare(end, a) > 0))
		{
			venture_set_error_validation(error, "start-at", "Fiscal periods for one organization may not overlap");
			return FALSE;
		}
		if ((0 == g_date_time_compare(start, b)) || (0 == g_date_time_compare(end, a)))
			adjacent = TRUE;
	}
	if ((rows->len > 0) && !adjacent)
	{
		venture_set_error_validation(error, "start-at", "Fiscal periods for one organization may not leave a gap");
		return FALSE;
	}
	if (VENTURE_IS_FISCAL_PERIOD(entity))
	{
		g_autoptr(VentureEntity) year = NULL;
		g_autoptr(GDateTime) a = NULL;
		g_autoptr(GDateTime) b = NULL;
		gint64 year_id;
		g_object_get(entity, "fiscal-year-id", &year_id, NULL);
		year = venture_database_get(database, VENTURE_TYPE_FISCAL_YEAR, year_id, error);
		if (NULL == year)
			return FALSE;
		g_object_get(year, "start-at", &a, "end-at", &b, NULL);
		if ((organization_id != venture_entity_get_organization_id(year)) ||
			(g_date_time_compare(start, a) < 0) || (g_date_time_compare(end, b) > 0) ||
			((0 == rows->len) && (0 != g_date_time_compare(start, a))))
		{
			venture_set_error_validation(error, "fiscal-year-id", "A period must tile its own organization's fiscal year without gaps");
			return FALSE;
		}
	}
	return TRUE;
}

/* The named permission is deliberately narrower than ordinary record edit:
 * active administrators and owners hold periods.reopen. Machine actors
 * acquire it only through a named human approval, never a token label. */
static gboolean
may_reopen(VentureDatabase *database, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_USER);
	g_autoptr(VentureEntity) user = NULL;
	const gchar *name = NULL;
	gint role = VENTURE_USER_ROLE_VIEWER;
	gboolean active = FALSE;
	if (NULL != actor)
	{
		if (!venture_string_is_empty(actor->approved_by))
			name = actor->approved_by;
		else if (VENTURE_ACTOR_KIND_USER == actor->kind)
			name = actor->name;
	}
	if (!venture_string_is_empty(name))
	{
		venture_query_add_filter_string(query, "username", VENTURE_FILTER_OP_EQ, name, NULL);
		user = venture_database_find_one(database, query, error);
		if ((NULL != error) && (NULL != *error))
			return FALSE;
		if (NULL != user)
			g_object_get(user, "role", &role, "active", &active, NULL);
	}
	if (active && ((VENTURE_USER_ROLE_OWNER == role) || (VENTURE_USER_ROLE_ADMIN == role)))
		return TRUE;
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED,
		"Reopening requires the periods.reopen permission (an active administrator or owner)");
	return FALSE;
}

static gboolean
validate_transition(VenturePeriodService *self, VentureDatabase *database,
	VentureEntity *entity, VentureEntity *previous, const VentureActor *actor, GError **error)
{
	gint before = VENTURE_PERIOD_OPEN;
	gint after;
	g_autoptr(JsonNode) diff = NULL;
	g_autofree gchar *name = NULL;
	g_autoptr(GDateTime) start = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	guint i;

	g_object_get(entity, "state", &after, "name", &name, "start-at", &start, NULL);
	if (NULL == previous)
	{
		g_autofree gchar *closed_by = NULL;
		g_autoptr(GDateTime) closed_at = NULL;
		g_object_get(entity, "closed-by", &closed_by, "closed-at", &closed_at, NULL);
		if ((VENTURE_PERIOD_OPEN != after) || !venture_string_is_empty(closed_by) || (NULL != closed_at))
		{
			venture_set_error_validation(error, "state", "New fiscal calendars must be open with no close stamp");
			return FALSE;
		}
		return TRUE;
	}
	g_object_get(previous, "state", &before, NULL);
	diff = venture_entity_diff(previous, entity);
	if (json_object_has_member(json_node_get_object(diff), "closed_by") ||
		json_object_has_member(json_node_get_object(diff), "closed_at"))
	{
		venture_set_error_validation(error, "closed-by", "Close stamps are derived from the actor and cannot be edited");
		return FALSE;
	}
	if (VENTURE_PERIOD_LOCKED == before)
	{
		if (json_object_get_size(json_node_get_object(diff)) > 0)
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT, "Period '%s' is locked", name);
			return FALSE;
		}
		return TRUE;
	}
	if (before == after)
		return TRUE;
	if (VENTURE_PERIOD_OPEN == after)
	{
		if (!may_reopen(database, actor, error))
			return FALSE;
		if (VENTURE_IS_FISCAL_PERIOD(entity))
		{
			g_autoptr(VentureEntity) year = NULL;
			gint64 year_id;
			gint state;
			g_object_get(entity, "fiscal-year-id", &year_id, NULL);
			year = venture_database_get(database, VENTURE_TYPE_FISCAL_YEAR, year_id, error);
			if (NULL == year)
				return FALSE;
			g_object_get(year, "state", &state, NULL);
			if (VENTURE_PERIOD_OPEN != state)
			{
				g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT, "Reopen the fiscal year before reopening its period");
				return FALSE;
			}
		}
		/* Retain the last close stamp; the state diff records the reopening
		 * actor and timestamp without discarding the evidence of close. */
		return TRUE;
	}
	if ((VENTURE_PERIOD_LOCKED == after) && (VENTURE_PERIOD_CLOSED != before))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT, "Close a period before locking it");
		return FALSE;
	}
	query = venture_query_new(VENTURE_TYPE_FISCAL_PERIOD);
	venture_query_set_organization(query, venture_entity_get_organization_id(entity));
	if (VENTURE_IS_FISCAL_YEAR(entity))
		venture_query_add_filter_int(query, "fiscal-year-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(entity), NULL);
	else
	{
		g_autofree gchar *date = venture_time_to_string(start);
		venture_query_add_filter_string(query, "start-at", VENTURE_FILTER_OP_LT, date, NULL);
	}
	venture_query_add_order(query, "start-at", VENTURE_SORT_ASCENDING, NULL);
	rows = venture_database_find(database, query, error);
	if (NULL == rows)
		return FALSE;
	for (i = 0; i < rows->len; i++)
	{
		gint state;
		g_autofree gchar *earlier = NULL;
		g_object_get(g_ptr_array_index(rows, i), "state", &state, "name", &earlier, NULL);
		if ((VENTURE_PERIOD_OPEN == state) ||
			(VENTURE_IS_FISCAL_YEAR(entity) && (VENTURE_PERIOD_LOCKED == after) && (VENTURE_PERIOD_LOCKED != state)))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT,
				"Close in order: period '%s' is %s", earlier, venture_enum_to_nick(VENTURE_TYPE_PERIOD_STATE, state));
			return FALSE;
		}
	}
	if (VENTURE_PERIOD_CLOSED == after)
	{
		g_autoptr(GDateTime) now = venture_time_now();
		const gchar *closer = (NULL != actor) ? actor->name : NULL;
		if ((NULL != actor) && !venture_string_is_empty(actor->approved_by))
			closer = actor->approved_by;
		if (venture_string_is_empty(closer))
		{
			venture_set_error_validation(error, "closed-by", "Closing requires a named actor");
			return FALSE;
		}
		if (VENTURE_IS_FISCAL_PERIOD(entity) && !venture_period_checklist_run(self->checklist, database, entity, error))
			return FALSE;
		g_object_set(entity, "closed-by", closer, "closed-at", now, NULL);
	}
	return TRUE;
}

static gboolean
generate_periods(VenturePeriodService *self, VentureDatabase *database,
	VentureEntity *year, const VentureActor *actor, GError **error)
{
	g_autoptr(GDateTime) start = NULL;
	g_autofree gchar *name = NULL;
	gint length;
	gint months;
	gint i;

	g_object_get(year, "start-at", &start, "name", &name, "period-length", &length, NULL);
	months = (VENTURE_PERIOD_QUARTERLY == length) ? 3 : 1;
	for (i = 0; i < 12; i += months)
	{
		g_autoptr(GDateTime) a = g_date_time_add_months(start, i);
		g_autoptr(GDateTime) b = g_date_time_add_months(start, i + months);
		g_autofree gchar *label = g_strdup_printf("%s P%02d", name, i / months + 1);
		g_autoptr(VentureFiscalPeriod) period = g_object_new(VENTURE_TYPE_FISCAL_PERIOD,
			"name", label, "start-at", a, "end-at", b,
			"organization-id", venture_entity_get_organization_id(year),
			"fiscal-year-id", venture_entity_get_id(year), NULL);
		if (!save_internal(self, database, VENTURE_ENTITY(period), actor, error))
			return FALSE;
	}
	return TRUE;
}

gboolean
venture_periods_save(VentureDatabase *database, VentureEntity *entity,
	const VentureActor *actor, gboolean *handled, GError **error)
{
	VenturePeriodService *self;
	g_autoptr(VentureEntity) working = NULL;
	g_autoptr(VentureEntity) previous = NULL;
	g_autoptr(GPtrArray) snapshots = NULL;
	gboolean created;
	gboolean ok;

	*handled = FALSE;
	if (!VENTURE_IS_FISCAL_YEAR(entity) && !VENTURE_IS_FISCAL_PERIOD(entity) &&
		!VENTURE_IS_REPORT_SNAPSHOT(entity))
		return TRUE;
	self = venture_period_service_get(database);
	if (self->saving == entity)
		return TRUE;
	*handled = TRUE;
	if (G_TYPE_INVALID == venture_entity_registry_lookup(venture_entity_registry_get_default(), "fiscal_year"))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "The periods module is disabled");
		return FALSE;
	}
	if (VENTURE_IS_REPORT_SNAPSHOT(entity))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED,
			"Report snapshots are immutable evidence written only by period close");
		return FALSE;
	}
	working = g_object_new(G_OBJECT_TYPE(entity), NULL);
	venture_entity_copy_properties_from(working, entity, FALSE);
	created = !venture_entity_is_persisted(entity);
	if (created && VENTURE_IS_FISCAL_YEAR(entity))
	{
		g_autoptr(GDateTime) start = NULL;
		g_autoptr(GDateTime) end = NULL;
		g_autoptr(GDateTime) expected = NULL;
		g_object_get(working, "start-at", &start, "end-at", &end, NULL);
		if (NULL != start)
		{
			expected = g_date_time_add_years(start, 1);
			if ((NULL != end) && (0 != g_date_time_compare(end, expected)))
			{
				venture_set_error_validation(error, "end-at", "A fiscal year must cover twelve calendar months");
				return FALSE;
			}
			g_object_set(working, "end-at", expected, NULL);
		}
	}
	if (!venture_database_begin(database, error))
		return FALSE;
	if (!created)
	{
		previous = venture_database_get(database, G_OBJECT_TYPE(entity), venture_entity_get_id(entity), error);
		if (NULL == previous)
		{
			venture_database_rollback(database);
			return FALSE;
		}
	}
	ok = validate_calendar(database, working, previous, error);
	if (ok)
		ok = validate_transition(self, database, working, previous, actor, error);
	if (ok && !created && VENTURE_IS_FISCAL_PERIOD(working))
	{
		gint before;
		gint after;
		g_object_get(previous, "state", &before, NULL);
		g_object_get(working, "state", &after, NULL);
		if ((VENTURE_PERIOD_OPEN == before) && (VENTURE_PERIOD_CLOSED == after))
		{
			g_autoptr(VentureContext) context = g_weak_ref_get(&self->context);
			if (NULL == context)
			{
				g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG, "Period close needs a reporting context to preserve snapshots");
				ok = FALSE;
			}
			else
			{
				snapshots = venture_period_report_snapshots(context, working, error);
				ok = NULL != snapshots;
			}
		}
	}
	if (ok)
		ok = save_internal(self, database, working, actor, error);
	if (ok && (NULL != snapshots))
	{
		guint i;
		for (i = 0; ok && (i < snapshots->len); i++)
			ok = save_internal(self, database, g_ptr_array_index(snapshots, i), actor, error);
	}
	if (ok && created && VENTURE_IS_FISCAL_YEAR(entity))
		ok = generate_periods(self, database, working, actor, error);
	if (!ok)
	{
		venture_database_rollback(database);
		return FALSE;
	}
	if (!venture_database_commit(database, error))
		return FALSE;
	venture_entity_copy_properties_from(entity, working, FALSE);
	return TRUE;
}

VentureFiscalYear *
venture_period_service_generate(VenturePeriodService *self, gint64 organization_id,
	const gchar *name, GDateTime *start, VenturePeriodLength length,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureDatabase) database = g_weak_ref_get(&self->database);
	g_autoptr(VentureFiscalYear) year = NULL;
	year = g_object_new(VENTURE_TYPE_FISCAL_YEAR, "organization-id", organization_id,
		"name", name, "start-at", start, "period-length", length, NULL);
	if (!venture_database_save(database, VENTURE_ENTITY(year), actor, error))
		return NULL;
	return g_steal_pointer(&year);
}

gboolean
venture_periods_check_removal(VentureDatabase *database, VentureEntity *entity, GError **error)
{
	if (VENTURE_IS_FISCAL_YEAR(entity) || VENTURE_IS_FISCAL_PERIOD(entity) ||
		VENTURE_IS_REPORT_SNAPSHOT(entity))
	{
		venture_set_error_validation(error, NULL, "Fiscal calendars and report snapshots cannot be deleted or restored");
		return FALSE;
	}
	{
		g_autoptr(VentureEntity) previous = NULL;
		if (venture_entity_is_persisted(entity))
		{
			previous = venture_database_get(database, G_OBJECT_TYPE(entity), venture_entity_get_id(entity), error);
			if (NULL == previous)
				return FALSE;
		}
		return venture_periods_validate_financial(database, entity, previous, error);
	}
}
