/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
#include "pipelines/venture-pipelines-private.h"

struct _VentureDealService
{
	GObject parent_instance;
	GWeakRef database;
	/* A permit is consumed before validators or audit callbacks can run. */
	VentureEntity *permit;
};
G_DEFINE_FINAL_TYPE(VentureDealService, venture_deal_service, G_TYPE_OBJECT)

enum { PROP_0, PROP_DATABASE };
static void
set_property(GObject *object, guint id, const GValue *value, GParamSpec *pspec)
{
	if (PROP_DATABASE == id)
		g_weak_ref_set(&VENTURE_DEAL_SERVICE(object)->database, g_value_get_object(value));
	else
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
}
static void
get_property(GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
	if (PROP_DATABASE == id)
		g_value_take_object(value, g_weak_ref_get(&VENTURE_DEAL_SERVICE(object)->database));
	else
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
}
static void
finalize(GObject *object)
{
	g_weak_ref_clear(&VENTURE_DEAL_SERVICE(object)->database);
	G_OBJECT_CLASS(venture_deal_service_parent_class)->finalize(object);
}
static void
venture_deal_service_class_init(VentureDealServiceClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	object_class->set_property = set_property;
	object_class->get_property = get_property;
	object_class->finalize = finalize;
	g_object_class_install_property(object_class, PROP_DATABASE,
		g_param_spec_object("database", "Database", "Owning database", VENTURE_TYPE_DATABASE,
			G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
}
static void
venture_deal_service_init(VentureDealService *self)
{
	g_weak_ref_init(&self->database, NULL);
}
static gboolean
refuse(GError **error, const gchar *reason)
{
	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		"VentureDealService: %s", reason);
	return FALSE;
}
static gint64
integer(GObject *object, const gchar *field)
{
	gint64 value;
	g_object_get(object, field, &value, NULL);
	return value;
}
static gboolean
enabled(void)
{
	return G_TYPE_INVALID != venture_entity_registry_lookup(
		venture_entity_registry_get_default(), "pipeline");
}
static gboolean
save_permitted(VentureDealService *self, VentureDatabase *db, VentureEntity *entity,
	const VentureActor *actor, GError **error)
{
	gboolean ok;
	self->permit = entity;
	ok = venture_database_save(db, entity, actor, error);
	self->permit = NULL;
	return ok;
}

gint64
venture_deal_service_ensure_default(VentureDealService *self, gint64 org, GError **error)
{
	g_autoptr(VentureDatabase) db = g_weak_ref_get(&self->database);
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_PIPELINE);
	g_autoptr(VentureEntity) found = NULL;
	g_autoptr(VenturePipeline) pipeline = NULL;
	g_auto(GStrv) names = venture_enum_list_nicks(VENTURE_TYPE_DEAL_STAGE);
	gint64 id;
	guint i;

	if (!enabled())
	{
		refuse(error, "pipelines module is disabled");
		return 0;
	}
	if (!venture_database_begin(db, error))
		return 0;
	venture_query_set_organization(query, org);
	venture_query_add_filter_int(query, "default", VENTURE_FILTER_OP_EQ, 1, NULL);
	found = venture_database_find_one(db, query, error);
	if (NULL != found)
	{
		id = venture_entity_get_id(found);
		if (!venture_database_commit(db, error))
			return 0;
		return id;
	}
	if (NULL != error && NULL != *error)
		goto fail;
	pipeline = venture_pipeline_new();
	g_object_set(pipeline, "organization-id", org, "name", "Sales", "default", TRUE, "active", TRUE, NULL);
	if (!venture_database_save(db, VENTURE_ENTITY(pipeline), NULL, error))
		goto fail;
	id = venture_entity_get_id(VENTURE_ENTITY(pipeline));
	for (i = 0; NULL != names[i]; i++)
	{
		g_autoptr(VenturePipelineStage) stage = venture_pipeline_stage_new();
		g_object_set(stage, "organization-id", org, "pipeline-id", id,
			"name", names[i], "position", (gint64)i,
			"probability", (gint64)(i < 4 ? (i + 1) * 20 : (i == 4 ? 100 : 0)),
			"kind", i < 4 ? 0 : (i == 4 ? 1 : 2), NULL);
		if (!venture_database_save(db, VENTURE_ENTITY(stage), NULL, error))
			goto fail;
	}
	if (!venture_database_commit(db, error))
		return 0;
	return id;
fail:
	venture_database_rollback(db);
	return 0;
}

static gboolean
write_entry(VentureDealService *self, VentureDatabase *db, VentureDeal *deal,
	gint64 from, gint64 to, GDateTime *now, const gchar *note, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureDealStageEntry) entry = venture_deal_stage_entry_new();
	g_object_set(entry, "organization-id", venture_entity_get_organization_id(VENTURE_ENTITY(deal)),
		"deal-id", venture_entity_get_id(VENTURE_ENTITY(deal)), "from-stage", from,
		"to-stage", to, "entered-at", now, "note", note,
		"by", NULL != actor ? actor->name : "system", NULL);
	return save_permitted(self, db, VENTURE_ENTITY(entry), actor, error);
}

static VentureEntity *live_reference(VentureDatabase *db, GType type, gint64 id, gint64 org, GError **error);
static gboolean required_populated(VentureDeal *deal, const gchar *list, GError **error);

static gboolean
initialize_deal(VentureDealService *self, VentureDatabase *db, VentureEntity *input,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureDeal) deal = venture_deal_new();
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_PIPELINE_STAGE);
	g_autoptr(VentureEntity) stage = NULL;
	g_autoptr(VentureEntity) process = NULL;
	g_autoptr(GDateTime) now = g_date_time_new_now_utc();
	gint64 org, pipeline;
	gint legacy;

	if (0 != integer(G_OBJECT(input), "stage-id"))
		return refuse(error, "use move_stage to set stage_id");
	if (!venture_database_begin(db, error))
		return FALSE;
	venture_entity_copy_properties_from(VENTURE_ENTITY(deal), input, FALSE);
	org = venture_entity_get_organization_id(input);
	pipeline = integer(G_OBJECT(input), "pipeline-id");
	if (0 == pipeline)
		pipeline = venture_deal_service_ensure_default(self, org, error);
	if (0 == pipeline)
		goto fail;
	/* New records must meet the same pipeline admission rules as moves.
	 * Existing rows are a backfill and retain their historical state. */
	if (!venture_entity_is_persisted(input))
	{
		gboolean active;
		process = live_reference(db, VENTURE_TYPE_PIPELINE, pipeline, org, error);
		if (NULL == process)
			goto fail;
		g_object_get(process, "active", &active, NULL);
		if (!active)
		{
			refuse(error, "new deals require an active pipeline");
			goto fail;
		}
	}
	g_object_get(input, "stage", &legacy, NULL);
	venture_query_set_organization(query, org);
	venture_query_add_filter_int(query, "pipeline-id", VENTURE_FILTER_OP_EQ, pipeline, NULL);
	venture_query_add_filter_int(query, "position", VENTURE_FILTER_OP_EQ, legacy, NULL);
	stage = venture_database_find_one(db, query, error);
	if (NULL == stage && (NULL == error || NULL == *error))
	{
		/* Custom processes need not number their first stage zero. */
		g_clear_object(&query);
		query = venture_query_new(VENTURE_TYPE_PIPELINE_STAGE);
		venture_query_set_organization(query, org);
		venture_query_add_filter_int(query, "pipeline-id", VENTURE_FILTER_OP_EQ, pipeline, NULL);
		venture_query_add_filter_string(query, "kind", VENTURE_FILTER_OP_EQ, "open", NULL);
		venture_query_add_order(query, "position", VENTURE_SORT_ASCENDING, NULL);
		stage = venture_database_find_one(db, query, error);
	}
	if (NULL == stage)
	{
		if (NULL == error || NULL == *error)
			refuse(error, "pipeline has no initial stage");
		goto fail;
	}
	{
		gint kind;
		g_object_get(stage, "kind", &kind, NULL);
		if (!venture_entity_is_persisted(input))
		{
			g_autofree gchar *required = NULL;
			g_object_get(stage, "required-fields", &required, NULL);
			if (!required_populated(VENTURE_DEAL(input), required, error))
				goto fail;
			if (2 == kind)
			{
				g_autoptr(VentureEntity) reason = live_reference(db, VENTURE_TYPE_LOSS_REASON,
					integer(G_OBJECT(input), "loss-reason-id"), org, error);
				gboolean active;
				if (NULL == reason)
					goto fail;
				g_object_get(reason, "active", &active, NULL);
				if (!active)
				{
					refuse(error, "loss reason must be active");
					goto fail;
				}
			}
			g_object_set(deal, "closed-at", 0 == kind ? NULL : now, NULL);
		}
		legacy = kind == 1 ? VENTURE_DEAL_STAGE_WON : (kind == 2 ? VENTURE_DEAL_STAGE_LOST : (gint)CLAMP(integer(G_OBJECT(stage), "position"), 0, 3));
	}
	g_object_set(deal, "pipeline-id", pipeline, "stage-id", venture_entity_get_id(stage), "stage", legacy, NULL);
	if (!save_permitted(self, db, VENTURE_ENTITY(deal), actor, error) ||
		(!venture_entity_is_deleted(VENTURE_ENTITY(deal)) &&
		 !write_entry(self, db, deal, 0, venture_entity_get_id(stage), now, "Initial stage", actor, error)))
		goto fail;
	if (!venture_database_commit(db, error))
		return FALSE;
	venture_entity_copy_properties_from(input, VENTURE_ENTITY(deal), FALSE);
	return TRUE;
fail:
	venture_database_rollback(db);
	return FALSE;
}

gboolean
venture_pipelines_save(VentureDatabase *db, VentureEntity *entity,
	const VentureActor *actor, gboolean *handled, GError **error)
{
	VentureDealService *self;
	g_autoptr(VentureEntity) previous = NULL;
	g_autoptr(JsonNode) diff = NULL;
	JsonObject *changes;
	*handled = FALSE;
	if (!VENTURE_IS_DEAL(entity) && !VENTURE_IS_DEAL_STAGE_ENTRY(entity))
		return TRUE;
	self = venture_database_get_deal_service(db);
	if (self->permit == entity)
	{
		self->permit = NULL;
		return TRUE;
	}
	if (VENTURE_IS_DEAL_STAGE_ENTRY(entity))
		return refuse(error, "stage history is service-only");
	if (!enabled())
		return TRUE;
	if (!venture_entity_is_persisted(entity))
	{
		*handled = TRUE;
		return initialize_deal(self, db, entity, actor, error);
	}
	previous = venture_database_get(db, VENTURE_TYPE_DEAL, venture_entity_get_id(entity), error);
	if (NULL == previous)
		return FALSE;
	diff = venture_entity_diff(previous, entity);
	changes = json_node_get_object(diff);
	if (json_object_has_member(changes, "stage") || json_object_has_member(changes, "stage_id") ||
		json_object_has_member(changes, "pipeline_id") || json_object_has_member(changes, "closed_at"))
		return refuse(error, "use move_stage to change a deal stage or pipeline");
	return TRUE;
}
static VentureEntity *
live_reference(VentureDatabase *db, GType type, gint64 id, gint64 org, GError **error)
{
	g_autoptr(VentureEntity) entity = venture_database_get(db, type, id, error);
	if (NULL == entity)
	{
		if (NULL == error || NULL == *error)
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "VentureDealService: reference not found");
		return NULL;
	}
	if (venture_entity_is_deleted(entity) || venture_entity_get_organization_id(entity) != org)
	{
		refuse(error, "reference is deleted or belongs to another organization");
		return NULL;
	}
	return g_steal_pointer(&entity);
}

static gboolean
required_populated(VentureDeal *deal, const gchar *list, GError **error)
{
	g_auto(GStrv) fields = g_strsplit(NULL != list ? list : "", ",", -1);
	guint i;
	for (i = 0; NULL != fields[i]; i++)
	{
		g_autofree gchar *field = g_strdelimit(g_strdup(g_strstrip(fields[i])), "_", '-');
		GParamSpec *spec;
		GValue value = G_VALUE_INIT;
		gboolean populated;
		if ('\0' == *field)
			continue;
		spec = g_object_class_find_property(G_OBJECT_GET_CLASS(deal), field);
		if (NULL == spec)
			return refuse(error, "required_fields names an unknown deal field");
		g_value_init(&value, G_PARAM_SPEC_VALUE_TYPE(spec));
		g_object_get_property(G_OBJECT(deal), field, &value);
		populated = TRUE;
		if (G_VALUE_HOLDS_STRING(&value))
		{
			g_autofree gchar *text = g_value_dup_string(&value);
			populated = NULL != text && '\0' != *g_strstrip(text);
		}
		else if (G_VALUE_HOLDS_INT64(&value))
			populated = 0 != g_value_get_int64(&value);
		else if (G_VALUE_HOLDS_BOXED(&value))
			populated = NULL != g_value_get_boxed(&value);
		g_value_unset(&value);
		if (!populated)
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
				"VentureDealService: required field %s is empty", field);
			return FALSE;
		}
	}
	return TRUE;
}

VentureDeal *
venture_deal_service_move_stage(VentureDealService *self, VentureDeal *input,
	gint64 stage_id, const gchar *note, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureDatabase) db = g_weak_ref_get(&self->database);
	g_autoptr(VentureEntity) previous = NULL;
	g_autoptr(VentureEntity) stage = NULL;
	g_autoptr(VentureEntity) pipeline = NULL;
	g_autoptr(VentureDeal) deal = venture_deal_new();
	g_autoptr(GDateTime) now = g_date_time_new_now_utc();
	g_autofree gchar *required = NULL;
	gint64 org, pipeline_id, from, position;
	gint kind, legacy;
	gboolean active, overridden;

	g_return_val_if_fail(VENTURE_IS_DEAL(input), NULL);
	if (!enabled())
	{
		refuse(error, "pipelines module is disabled");
		return NULL;
	}
	if (!venture_database_begin(db, error))
		return NULL;
	org = venture_entity_get_organization_id(VENTURE_ENTITY(input));
	previous = live_reference(db, VENTURE_TYPE_DEAL,
		venture_entity_get_id(VENTURE_ENTITY(input)), org, error);
	if (NULL == previous)
		goto fail;
	if (venture_entity_get_version(previous) != venture_entity_get_version(VENTURE_ENTITY(input)))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT, "VentureDealService: deal changed; reload before moving");
		goto fail;
	}
	pipeline_id = integer(G_OBJECT(previous), "pipeline-id");
	if (pipeline_id != integer(G_OBJECT(input), "pipeline-id") ||
		integer(G_OBJECT(previous), "stage-id") != integer(G_OBJECT(input), "stage-id"))
	{
		refuse(error, "input stage and pipeline must match the stored deal");
		goto fail;
	}
	stage = live_reference(db, VENTURE_TYPE_PIPELINE_STAGE, stage_id, org, error);
	pipeline = live_reference(db, VENTURE_TYPE_PIPELINE, pipeline_id, org, error);
	if (NULL == stage || NULL == pipeline)
		goto fail;
	g_object_get(pipeline, "active", &active, NULL);
	if (!active || integer(G_OBJECT(stage), "pipeline-id") != pipeline_id)
	{
		refuse(error, "destination must belong to the deal's active pipeline");
		goto fail;
	}
	g_object_get(stage, "kind", &kind, "required-fields", &required, "position", &position, NULL);
	if (!required_populated(input, required, error))
		goto fail;
	if (2 == kind)
	{
		g_autoptr(VentureEntity) reason = NULL;
		gint64 reason_id = integer(G_OBJECT(input), "loss-reason-id");
		if (0 == reason_id)
		{
			refuse(error, "a loss reason is required for a lost stage");
			goto fail;
		}
		reason = live_reference(db, VENTURE_TYPE_LOSS_REASON, reason_id, org, error);
		if (NULL == reason)
			goto fail;
		g_object_get(reason, "active", &active, NULL);
		if (!active)
		{
			refuse(error, "loss reason must be active");
			goto fail;
		}
	}
	from = integer(G_OBJECT(previous), "stage-id");
	venture_entity_copy_properties_from(VENTURE_ENTITY(deal), VENTURE_ENTITY(input), FALSE);
	if (from == stage_id)
	{
		if (!venture_database_commit(db, error))
			return NULL;
		return g_steal_pointer(&deal);
	}
	legacy = 1 == kind ? VENTURE_DEAL_STAGE_WON : (2 == kind ? VENTURE_DEAL_STAGE_LOST : (gint)CLAMP(position, 0, 3));
	g_object_get(deal, "probability-overridden", &overridden, NULL);
	g_object_set(deal, "stage-id", stage_id, "stage", legacy,
		"closed-at", 0 == kind ? NULL : now, NULL);
	if (!overridden)
		g_object_set(deal, "probability", integer(G_OBJECT(stage), "probability"), NULL);
	if (!save_permitted(self, db, VENTURE_ENTITY(deal), actor, error) ||
		!write_entry(self, db, deal, from, stage_id, now, note, actor, error))
		goto fail;
	if (!venture_database_commit(db, error))
		return NULL;
	return g_steal_pointer(&deal);
fail:
	venture_database_rollback(db);
	return NULL;
}

gboolean
venture_pipelines_migrate(VentureDatabase *db, GError **error)
{
	g_autoptr(OrmResult) pending = NULL;
	g_autoptr(VentureQuery) organizations_query = venture_query_new(VENTURE_TYPE_ORGANIZATION);
	g_autoptr(GPtrArray) organizations = NULL;
	VentureDealService *self;
	guint i;

	if (!enabled())
		return TRUE;
	if (!venture_database_begin(db, error))
		return FALSE;
	pending = venture_database_query_raw(db, "SELECT pending FROM venture_pipeline_upgrade", NULL, error);
	if (NULL == pending)
		goto fail;
	if (!orm_result_next(pending))
		return venture_database_commit(db, error);
	g_clear_object(&pending);
	self = venture_database_get_deal_service(db);
	organizations = venture_database_find(db, organizations_query, error);
	if (NULL == organizations)
		goto fail;
	for (i = 0; i < organizations->len; i++)
	{
		VentureEntity *org = g_ptr_array_index(organizations, i);
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_DEAL);
		g_autoptr(GPtrArray) deals = NULL;
		guint j;
		gint64 org_id = venture_entity_get_id(org);
		if (0 == venture_deal_service_ensure_default(self, org_id, error))
			goto fail;
		venture_query_set_organization(query, org_id);
		venture_query_set_include_deleted(query, TRUE);
		deals = venture_database_find(db, query, error);
		if (NULL == deals)
			goto fail;
		for (j = 0; j < deals->len; j++)
		{
			VentureEntity *deal = g_ptr_array_index(deals, j);
			if (0 == integer(G_OBJECT(deal), "stage-id") &&
				!initialize_deal(self, db, deal, NULL, error))
				goto fail;
		}
	}
	if (!venture_database_execute(db, "DELETE FROM venture_pipeline_upgrade", NULL, error))
		goto fail;
	return venture_database_commit(db, error);
fail:
	venture_database_rollback(db);
	return FALSE;
}

gboolean
venture_pipelines_check_removal(VentureEntity *entity, GError **error)
{
	if (VENTURE_IS_DEAL_STAGE_ENTRY(entity))
		return refuse(error, "stage history cannot be deleted, restored or purged");
	return TRUE;
}
