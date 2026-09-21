/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
#include "venture-activity-private.h"
#include <string.h>

struct _VentureActivityService
{
	GObject parent_instance;
	VentureDatabase *database;
	VentureEntity *writing;
	gboolean busy;
};
G_DEFINE_FINAL_TYPE(VentureActivityService, venture_activity_service, G_TYPE_OBJECT)

static gboolean
refuse(GError **error, VentureError code, const gchar *message)
{
	g_set_error(error, VENTURE_ERROR, code, "VentureActivityService: %s", message);
	return FALSE;
}

static void
get_property(GObject *object, guint id, GValue *value, GParamSpec *spec)
{
	if (id == 1)
		g_value_set_object(value, VENTURE_ACTIVITY_SERVICE(object)->database);
	else
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
}

static void
set_property(GObject *object, guint id, const GValue *value, GParamSpec *spec)
{
	VentureActivityService *self = VENTURE_ACTIVITY_SERVICE(object);
	if (id == 1)
	{
		self->database = g_value_get_object(value);
		g_object_add_weak_pointer(G_OBJECT(self->database), (gpointer *)&self->database);
	}
	else
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
}

static void
finalize(GObject *object)
{
	VentureActivityService *self = VENTURE_ACTIVITY_SERVICE(object);
	if (self->database != NULL)
		g_object_remove_weak_pointer(G_OBJECT(self->database), (gpointer *)&self->database);
	G_OBJECT_CLASS(venture_activity_service_parent_class)->finalize(object);
}

static gboolean
first_completion_error(GSignalInvocationHint *hint, GValue *accumulator, const GValue *value, gpointer data)
{
	if (g_value_get_boxed(value) == NULL)
		return TRUE;
	g_value_copy(value, accumulator);
	return FALSE;
}

static void
venture_activity_service_class_init(VentureActivityServiceClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	object_class->get_property = get_property;
	object_class->set_property = set_property;
	object_class->finalize = finalize;
	/**
	 * VentureActivityService::completing:
	 * @self: service
	 * @activity: detached snapshot of the proposed completed activity
	 *
	 * Emitted inside the transaction after checking the stored version and
	 * planned status, before any writes. Snapshot edits have no effect.
	 * Reentrant actions are refused. The first returned error vetoes completion.
	 * Returns: (transfer full) (nullable): a veto, or NULL to continue
	 */
	g_signal_new("completing", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0,
		first_completion_error, NULL, NULL, G_TYPE_ERROR, 1, VENTURE_TYPE_ENTITY);
	g_object_class_install_property(object_class, 1,
		g_param_spec_object("database", "Database", "Owning repository", VENTURE_TYPE_DATABASE,
			G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
}

static void
venture_activity_service_init(VentureActivityService *self)
{
	(void)self;
}

static gboolean
validate(VentureDatabase *db, VentureEntity *row, VentureEntity *previous, gpointer data, GError **error)
{
	VentureActivityService *self = data;
	g_autoptr(GDateTime) completed = NULL;
	g_autoptr(GDateTime) reminded = NULL;
	g_autoptr(GDateTime) starts = NULL;
	g_autoptr(GDateTime) ends = NULL;
	g_autoptr(GDateTime) due = NULL;
	g_autofree gchar *related = NULL;
	g_autofree gchar *old_related = NULL;
	gint64 related_id, old_id = 0;
	gint status, old_status = 0, recurrence;
	gboolean permitted = self->writing == row;
	guint i;
	const gchar *refs[] = { "contact-id", "company-id", "deal-id", "lead-id" };
	const gchar *targets[] = { "contact", "company", "deal", "lead" };
	/* Consume before any callback can borrow the authority. */
	if (permitted)
		self->writing = NULL;
	g_object_get(row, "status", &status, "completed-at", &completed, "reminded-at", &reminded,
		"starts-at", &starts, "ends-at", &ends, "due-at", &due, "recurrence", &recurrence,
		"related-type", &related, "related-id", &related_id, NULL);
	if (previous != NULL)
		g_object_get(previous, "status", &old_status, "related-type", &old_related, "related-id", &old_id, NULL);
	if (!permitted)
	{
		g_autoptr(JsonNode) diff = previous ? venture_entity_diff(previous, row) : NULL;
		JsonObject *changes = diff ? json_node_get_object(diff) : NULL;
		if (status == VENTURE_ACTIVITY_STATUS_DONE || old_status == VENTURE_ACTIVITY_STATUS_DONE ||
			(previous == NULL && (completed != NULL || reminded != NULL)) ||
			(changes != NULL && (json_object_has_member(changes, "completed-at") ||
			 json_object_has_member(changes, "reminded-at") || json_object_has_member(changes, "completed_at") ||
			 json_object_has_member(changes, "reminded_at"))))
			return refuse(error, VENTURE_ERROR_VALIDATION, "Use complete; completion and reminder stamps are service-owned");
	}
	if (!permitted)
	{
		guint count;
		GParamSpec **properties = g_object_class_list_properties(G_OBJECT_GET_CLASS(row), &count);
		gboolean changed = FALSE;
		for (i = 0; i < count; i++)
			if (g_str_has_prefix(properties[i]->name, "call-"))
			{
				GValue value = G_VALUE_INIT;
				g_value_init(&value, G_PARAM_SPEC_VALUE_TYPE(properties[i]));
				g_object_get_property(G_OBJECT(row), properties[i]->name, &value);
				if (!g_param_value_defaults(properties[i], &value))
					changed = TRUE;
				g_value_unset(&value);
			}
		g_free(properties);
		if (changed)
			return refuse(error, VENTURE_ERROR_VALIDATION, "Call evidence is written only through log_call or completion");
	}
	if (ends != NULL && (starts == NULL || g_date_time_compare(ends, starts) <= 0))
		return refuse(error, VENTURE_ERROR_VALIDATION, "Meeting end must follow its start");
	if (recurrence != VENTURE_ACTIVITY_RECURRENCE_NONE && due == NULL && starts == NULL)
		return refuse(error, VENTURE_ERROR_VALIDATION, "Recurring activities require a due or start time");
	if ((related != NULL && *related != '\0') != (related_id > 0))
		return refuse(error, VENTURE_ERROR_VALIDATION, "Related type and id must be supplied together");
	if (related_id > 0 && (old_id != related_id || g_strcmp0(old_related, related) != 0 ||
		(previous != NULL && venture_entity_get_organization_id(previous) != venture_entity_get_organization_id(row))))
	{
		g_autoptr(VentureEntity) target = NULL;
		GType type = venture_entity_registry_lookup(venture_entity_registry_get_default(), related);
		if (type == G_TYPE_INVALID || !(g_str_equal(related, "company") || g_str_equal(related, "contact") ||
			g_str_equal(related, "deal") || g_str_equal(related, "lead") || g_str_equal(related, "ticket") ||
			g_str_equal(related, "invoice")))
			return refuse(error, VENTURE_ERROR_VALIDATION, "Related type must be an enabled CRM, ticket, lead or invoice record");
		target = venture_database_get(db, type, related_id, error);
		if (target == NULL)
			return FALSE;
		if (venture_entity_is_deleted(target))
			return refuse(error, VENTURE_ERROR_VALIDATION, "Related record is deleted");
		if (venture_entity_get_organization_id(target) != venture_entity_get_organization_id(row))
			return refuse(error, VENTURE_ERROR_VALIDATION, "Related record belongs to another organization");
		for (i = 0; i < G_N_ELEMENTS(refs); i++)
			if (g_str_equal(related, targets[i]))
			{
				gint64 explicit_id;
				g_object_get(row, refs[i], &explicit_id, NULL);
				if (explicit_id != 0 && explicit_id != related_id)
					return refuse(error, VENTURE_ERROR_VALIDATION, "Related record conflicts with the explicit CRM reference");
				g_object_set(row, refs[i], related_id, NULL);
			}
	}
	/* Editing only the explicit reference must not detach it from a retained
	 * polymorphic subject; completion writes history using explicit refs. */
	for (i = 0; related_id > 0 && i < G_N_ELEMENTS(refs); i++)
		if (g_strcmp0(related, targets[i]) == 0)
		{
			gint64 explicit_id;
			g_object_get(row, refs[i], &explicit_id, NULL);
			if (explicit_id != related_id)
				return refuse(error, VENTURE_ERROR_VALIDATION, "Related record conflicts with the explicit CRM reference");
		}
	for (i = 0; i < G_N_ELEMENTS(refs); i++)
	{
		gint64 id;
		g_autoptr(VentureEntity) target = NULL;
		g_object_get(row, refs[i], &id, NULL);
		if (id == 0)
			continue;
		target = venture_database_get(db, venture_entity_registry_lookup_any(venture_entity_registry_get_default(), targets[i]), id, error);
		if (target == NULL)
			return FALSE;
		if (venture_entity_get_organization_id(target) != venture_entity_get_organization_id(row))
			return refuse(error, VENTURE_ERROR_VALIDATION, "CRM reference belongs to another organization");
	}
	return TRUE;
}

VentureActivityService *
venture_activity_service_new(VentureDatabase *database)
{
	VentureActivityService *self = g_object_new(VENTURE_TYPE_ACTIVITY_SERVICE, "database", database, NULL);
	venture_database_add_save_validator(database, VENTURE_TYPE_ACTIVITY, validate, self, NULL);
	return self;
}

static gboolean
write_record(VentureActivityService *self, VentureEntity *row, const VentureActor *actor, GError **error)
{
	gboolean ok;
	self->writing = row;
	ok = venture_database_save(self->database, row, actor, error);
	self->writing = NULL;
	return ok;
}

static GDateTime *
next_date(GDateTime *date, gint recurrence)
{
	if (date == NULL)
		return NULL;
	if (recurrence == VENTURE_ACTIVITY_RECURRENCE_MONTHLY)
		return g_date_time_add_months(date, 1);
	return g_date_time_add_days(date, recurrence == VENTURE_ACTIVITY_RECURRENCE_WEEKLY ? 7 : 1);
}

/* Metadata-derived inputs also define the normalized replay payload. */
static gboolean
call_prepare(VentureEntity *row, VentureEntity *subject, JsonObject *details,
	const VentureActor *actor, GError **error)
{
	g_autoptr(GPtrArray) fields = venture_activity_call_parameters();
	g_autoptr(VentureAction) schema = g_object_new(VENTURE_TYPE_ACTION, "data-class", VENTURE_DATA_CLASS_TENANT, "parameters", fields, NULL);
	g_autoptr(JsonNode) input = json_node_new(JSON_NODE_OBJECT);
	g_autoptr(GHashTable) params = NULL;
	g_autoptr(JsonNode) normalized = NULL;
	g_autoptr(JsonNode) identity = json_node_new(JSON_NODE_OBJECT);
	g_autofree gchar *encoded = NULL, *digest = NULL, *source = NULL, *external = NULL, *key_input = NULL, *key = NULL;
	g_autofree gchar *owner = NULL, *subject_text = NULL, *follow_owner = NULL, *follow_subject = NULL, *recording = NULL;
	g_autoptr(GDateTime) occurred = NULL, due = NULL, reminder = NULL, now = venture_time_now();
	JsonObject *canonical;
	gint outcome;
	guint i;
	json_node_set_object(input, details);
	params = venture_action_parameters_from_json(input, error);
	if (params == NULL || !venture_action_validate_parameters(schema, params, error))
		return FALSE;
	for (i = 0; i < fields->len; i++)
	{
		VentureFieldSpec *spec = g_ptr_array_index(fields, i);
		JsonNode *value = g_hash_table_lookup(params, spec->name);
		g_autofree gchar *field = g_strdup(spec->name);
		g_autofree gchar *text = NULL;
		if (value == NULL || JSON_NODE_HOLDS_NULL(value))
			continue;
		g_strdelimit(field, "_", '-');
		text = spec->kind == VENTURE_FIELD_KIND_INTEGER ? g_strdup_printf("%" G_GINT64_FORMAT, json_node_get_int(value)) : g_strdup(json_node_get_string(value));
		if (!venture_entity_set_field_from_string(row, field, text, error))
			return FALSE;
	}
	g_object_get(row, "owner", &owner, "subject", &subject_text, "call-occurred-at", &occurred,
		"call-outcome", &outcome, "call-followup-due-at", &due, "call-followup-remind-at", &reminder,
		"call-followup-owner", &follow_owner, "call-followup-subject", &follow_subject,
		"call-external-source", &source, "call-external-id", &external, "call-recording-url", &recording, NULL);
	if (occurred == NULL || g_date_time_compare(occurred, now) > 0 || outcome == VENTURE_CALL_OUTCOME_UNKNOWN)
		return refuse(error, VENTURE_ERROR_VALIDATION, "A logged call needs an actual occurrence and a structured result");
	if (recording != NULL && *recording != '\0')
	{
		g_autoptr(GUri) uri = g_uri_parse(recording, G_URI_FLAGS_NONE, NULL);
		if (uri == NULL || g_strcmp0(g_uri_get_scheme(uri), "https") != 0 ||
			g_uri_get_host(uri) == NULL || g_uri_get_userinfo(uri) != NULL ||
			g_uri_get_query(uri) != NULL || g_uri_get_fragment(uri) != NULL)
			return refuse(error, VENTURE_ERROR_VALIDATION, "Recording must be a stable HTTPS URL without credentials, query or fragment");
	}
	if (venture_string_is_empty(owner))
		g_object_set(row, "owner", actor != NULL && actor->name != NULL ? actor->name : "Unassigned", NULL);
	if (due == NULL && (reminder != NULL || !venture_string_is_empty(follow_owner) || !venture_string_is_empty(follow_subject)))
		return refuse(error, VENTURE_ERROR_VALIDATION, "Followup settings require a due time");
	if (due != NULL)
	{
		if (g_date_time_compare(due, occurred) < 0 || (reminder != NULL &&
			(g_date_time_compare(reminder, occurred) < 0 || g_date_time_compare(reminder, due) > 0)))
			return refuse(error, VENTURE_ERROR_VALIDATION, "Followup due and reminder must follow the call; reminder cannot follow due");
		if (venture_string_is_empty(follow_owner))
		{
			g_clear_pointer(&owner, g_free);
			g_object_get(row, "owner", &owner, NULL);
			g_object_set(row, "call-followup-owner", owner, NULL);
		}
		if (venture_string_is_empty(follow_subject))
		{
			g_autofree gchar *label = g_strconcat("Follow up: ", subject_text, NULL);
			g_object_set(row, "call-followup-subject", label, NULL);
		}
		if (reminder == NULL)
			g_object_set(row, "call-followup-remind-at", due, NULL);
	}
	if (venture_string_is_empty(source) != venture_string_is_empty(external))
		return refuse(error, VENTURE_ERROR_VALIDATION, "External source and identity must be supplied together");
	normalized = venture_serializable_to_json(VENTURE_SERIALIZABLE(row), FALSE);
	json_node_take_object(identity, json_object_new());
	canonical = json_node_get_object(identity);
	json_object_set_string_member(canonical, "subject_type", venture_entity_get_entity_name(subject));
	json_object_set_int_member(canonical, "subject_id", venture_entity_get_id(subject));
	for (i = 0; i < fields->len; i++)
	{
		VentureFieldSpec *spec = g_ptr_array_index(fields, i);
		JsonNode *value;
		if (g_str_equal(spec->name, "call_external_source") || g_str_equal(spec->name, "call_external_id"))
			continue;
		value = json_object_get_member(json_node_get_object(normalized), spec->name);
		if (value != NULL)
			json_object_set_member(canonical, spec->name, json_node_copy(value));
	}
	encoded = venture_json_to_string(identity, FALSE);
	digest = g_compute_checksum_for_string(G_CHECKSUM_SHA256, encoded, -1);
	if (venture_string_is_empty(source))
	{
		g_free(source);
		g_free(external);
		source = g_strdup("manual-v1");
		external = g_strdup(digest);
	}
	key_input = g_strdup_printf("%" G_GSIZE_FORMAT ":%s%s", strlen(source), source, external);
	key = g_compute_checksum_for_string(G_CHECKSUM_SHA256, key_input, -1);
	g_object_set(row, "call-request-hash", digest, "call-external-source", source,
		"call-external-id", external, "call-external-key", key, NULL);
	return TRUE;
}

static VentureEntity *
call_replay(VentureActivityService *self, VentureEntity *proposed, gboolean *found, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ACTIVITY);
	g_autofree gchar *source = NULL, *external = NULL, *digest = NULL, *previous = NULL;
	g_autoptr(VentureEntity) existing = NULL;
	gint status;
	*found = FALSE;
	g_object_get(proposed, "call-external-source", &source, "call-external-id", &external,
		"call-request-hash", &digest, NULL);
	venture_query_set_organization(query, venture_entity_get_organization_id(proposed));
	venture_query_set_include_deleted(query, TRUE);
	if (!venture_query_add_filter_string(query, "call-external-source", VENTURE_FILTER_OP_EQ, source, error) ||
		!venture_query_add_filter_string(query, "call-external-id", VENTURE_FILTER_OP_EQ, external, error))
	{
		*found = TRUE;
		return NULL;
	}
	existing = venture_database_find_one(self->database, query, error);
	if (existing == NULL)
	{
		*found = error != NULL && *error != NULL;
		return NULL;
	}
	*found = TRUE;
	g_object_get(existing, "call-request-hash", &previous, "status", &status, NULL);
	if (venture_entity_is_deleted(existing) || status != VENTURE_ACTIVITY_STATUS_DONE || g_strcmp0(digest, previous) != 0)
	{
		refuse(error, VENTURE_ERROR_CONFLICT, "Call identity already belongs to different or removed history");
		return NULL;
	}
	return g_steal_pointer(&existing);
}

static void
call_clear_fields(VentureEntity *row)
{
	guint count, i;
	GParamSpec **properties = g_object_class_list_properties(G_OBJECT_GET_CLASS(row), &count);
	for (i = 0; i < count; i++)
		if (g_str_has_prefix(properties[i]->name, "call-"))
		{
			GValue value = G_VALUE_INIT;
			g_value_init(&value, G_PARAM_SPEC_VALUE_TYPE(properties[i]));
			g_param_value_set_default(properties[i], &value);
			g_object_set_property(G_OBJECT(row), properties[i]->name, &value);
			g_value_unset(&value);
		}
	g_free(properties);
}

static VentureEntity *
activity_act_full(VentureActivityService *self, VentureEntity *activity,
	const gchar *action, const gchar *value, gboolean log_call, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) row = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	gint status, recurrence;
	gboolean complete = g_strcmp0(action, "complete") == 0;
	gboolean ok = FALSE;
	g_return_val_if_fail(VENTURE_IS_ACTIVITY_SERVICE(self), NULL);
	if (self->database == NULL || self->busy || !VENTURE_IS_ACTIVITY(activity) ||
		venture_entity_registry_lookup(venture_entity_registry_get_default(), "activity") == G_TYPE_INVALID)
	{
		refuse(error, VENTURE_ERROR_CONFLICT, "Service unavailable or reentrant action");
		return NULL;
	}
	if (!venture_database_begin(self->database, error))
		return NULL;
	self->busy = TRUE;
	if (log_call)
	{
		gboolean found;
		row = call_replay(self, activity, &found, error);
		if (found)
		{
			if (row == NULL)
				venture_database_rollback(self->database);
			else if (!venture_database_commit(self->database, error))
				g_clear_object(&row);
			self->busy = FALSE;
			return g_steal_pointer(&row);
		}
	}
	if (log_call && venture_entity_get_id(activity) == 0)
		row = venture_entity_duplicate(activity);
	else
		row = venture_database_get(self->database, VENTURE_TYPE_ACTIVITY, venture_entity_get_id(activity), error);
	if (row == NULL)
	{
		if (error == NULL || *error == NULL)
			refuse(error, VENTURE_ERROR_NOT_FOUND, "Activity no longer exists");
		goto done;
	}
	if (venture_entity_get_version(row) != venture_entity_get_version(activity))
	{
		refuse(error, VENTURE_ERROR_CONFLICT, "Activity changed; reload before acting");
		goto done;
	}
	if (log_call)
	{
		gint kind;
		g_object_get(row, "kind", &kind, NULL);
		if (kind != VENTURE_ACTIVITY_KIND_CALL)
		{
			refuse(error, VENTURE_ERROR_VALIDATION, "Log a call requires a call activity");
			goto done;
		}
		venture_entity_copy_properties_from(row, activity, FALSE);
	}
	g_object_get(row, "status", &status, "recurrence", &recurrence, NULL);
	if (status != VENTURE_ACTIVITY_STATUS_PLANNED || venture_entity_is_deleted(row))
	{
		refuse(error, VENTURE_ERROR_CONFLICT, "Only planned activities may be acted on");
		goto done;
	}
	if (complete)
		g_object_set(row, "status", VENTURE_ACTIVITY_STATUS_DONE, "completed-at", now, "outcome", value, NULL);
	else if (g_strcmp0(action, "cancel") == 0)
		g_object_set(row, "status", VENTURE_ACTIVITY_STATUS_CANCELLED, NULL);
	else if (g_strcmp0(action, "reassign") == 0)
	{
		if (value == NULL || *value == '\0')
		{
			refuse(error, VENTURE_ERROR_VALIDATION, "Reassignment requires an owner");
			goto done;
		}
		g_object_set(row, "owner", value, NULL);
	}
	else if (g_strcmp0(action, "snooze") == 0)
	{
		g_autoptr(GDateTime) until = value ? venture_time_from_string(value, NULL) : NULL;
		g_autoptr(GDateTime) starts = NULL;
		g_autoptr(GDateTime) ends = NULL;
		if (until == NULL)
		{
			refuse(error, VENTURE_ERROR_VALIDATION, "Snooze requires a valid until timestamp");
			goto done;
		}
		g_object_get(row, "starts-at", &starts, "ends-at", &ends, NULL);
		if (starts != NULL)
		{
			g_autoptr(GDateTime) end = ends ? g_date_time_add(until, g_date_time_difference(ends, starts)) : NULL;
			g_object_set(row, "starts-at", until, "ends-at", end, NULL);
		}
		g_object_set(row, "due-at", until, "remind-at", until, "reminded-at", NULL, NULL);
	}
	else
	{
		refuse(error, VENTURE_ERROR_VALIDATION, "Unknown activity action");
		goto done;
	}
	if (complete)
	{
		g_autoptr(VentureEntity) snapshot = g_object_new(VENTURE_TYPE_ACTIVITY, NULL);
		g_autoptr(GError) veto = NULL;
		venture_entity_copy_properties_from(snapshot, row, FALSE);
		g_signal_emit_by_name(self, "completing", snapshot, &veto);
		if (veto != NULL)
		{
			g_propagate_error(error, g_steal_pointer(&veto));
			goto done;
		}
	}
	if (complete)
	{
		gint64 contact, company, deal, lead;
		gint kind;
		g_autofree gchar *subject = NULL;
		g_autoptr(GDateTime) occurred = NULL;
		gint direction;
		g_object_get(row, "contact-id", &contact, "company-id", &company, "deal-id", &deal, "lead-id", &lead,
			"kind", &kind, "subject", &subject, "call-occurred-at", &occurred, "call-direction", &direction, NULL);
		if (kind == VENTURE_ACTIVITY_KIND_CALL && occurred == NULL)
		{
			occurred = g_date_time_ref(now);
			g_object_set(row, "call-occurred-at", occurred, NULL);
		}
		/* A call without a CRM relation is still one historical event. */
		if (log_call && !(contact || company || deal || lead))
		{
			refuse(error, VENTURE_ERROR_VALIDATION, "A logged call requires a lead, company, contact or deal");
			goto done;
		}
		if (contact || company || deal || lead)
		{
			g_autoptr(VentureInteraction) interaction = venture_interaction_new();
			const gchar *kind_name = kind == VENTURE_ACTIVITY_KIND_CALL ? "call" :
				kind == VENTURE_ACTIVITY_KIND_MEETING ? "meeting" : kind == VENTURE_ACTIVITY_KIND_EMAIL ? "email" : "note";
			venture_entity_set_organization_id(VENTURE_ENTITY(interaction), venture_entity_get_organization_id(row));
			g_object_set(interaction, "contact-id", contact, "company-id", company, "deal-id", deal,
				"lead-id", lead, "subject", subject, "body", value, "occurred-at", occurred ? occurred : now,
				"outbound", kind != VENTURE_ACTIVITY_KIND_CALL || direction == VENTURE_CALL_DIRECTION_OUTBOUND, NULL);
			if (!venture_entity_set_field_from_string(VENTURE_ENTITY(interaction), "kind", kind_name, error) ||
				!venture_database_save(self->database, VENTURE_ENTITY(interaction), actor, error))
				goto done;
			if (kind == VENTURE_ACTIVITY_KIND_CALL)
				g_object_set(row, "call-interaction-id", venture_entity_get_id(VENTURE_ENTITY(interaction)), NULL);
		}
		if (log_call)
		{
			g_autoptr(GDateTime) due = NULL;
			g_object_get(row, "call-followup-due-at", &due, NULL);
			if (due != NULL)
			{
				g_autoptr(VentureEntity) followup = venture_entity_duplicate(row);
				g_autoptr(GDateTime) remind = NULL;
				g_autofree gchar *owner = NULL, *label = NULL;
				g_object_get(row, "call-followup-owner", &owner, "call-followup-subject", &label,
					"call-followup-remind-at", &remind, NULL);
				call_clear_fields(followup);
				g_object_set(followup, "kind", VENTURE_ACTIVITY_KIND_FOLLOWUP, "status", VENTURE_ACTIVITY_STATUS_PLANNED,
					"subject", label, "owner", owner, "due-at", due, "remind-at", remind,
					"recurrence", VENTURE_ACTIVITY_RECURRENCE_NONE, "starts-at", NULL, "ends-at", NULL,
					"completed-at", NULL, "outcome", NULL, "reminded-at", NULL, NULL);
				if (!venture_database_save(self->database, followup, actor, error))
					goto done;
				g_object_set(row, "call-followup-id", venture_entity_get_id(followup), NULL);
			}
		}
		if (recurrence != VENTURE_ACTIVITY_RECURRENCE_NONE)
		{
			g_autoptr(VentureEntity) next = venture_entity_duplicate(row);
			g_autoptr(GDateTime) starts = NULL;
			g_autoptr(GDateTime) due = NULL;
			g_autoptr(GDateTime) anchor = NULL;
			GTimeSpan advance;
			const gchar *dates[] = { "due-at", "starts-at", "ends-at", "remind-at" };
			guint i;
			g_object_get(row, "starts-at", &starts, "due-at", &due, NULL);
			anchor = next_date(starts ? starts : due, recurrence);
			advance = g_date_time_difference(anchor, starts ? starts : due);
			call_clear_fields(next);
			g_object_set(next, "status", VENTURE_ACTIVITY_STATUS_PLANNED, "completed-at", NULL,
				"outcome", NULL, "reminded-at", NULL, NULL);
			for (i = 0; i < G_N_ELEMENTS(dates); i++)
			{
				g_autoptr(GDateTime) date = NULL;
				g_autoptr(GDateTime) shifted = NULL;
				g_object_get(row, dates[i], &date, NULL);
				/* One anchor preserves meeting duration and reminder lead time
				 * when a month has fewer days than the original schedule. */
				shifted = date ? g_date_time_add(date, advance) : NULL;
				g_object_set(next, dates[i], shifted, NULL);
			}
			if (!venture_database_save(self->database, next, actor, error))
				goto done;
		}
	}
	if (!write_record(self, row, actor, error))
		goto done;
	ok = venture_database_commit(self->database, error);
	self->busy = FALSE;
	return ok ? g_steal_pointer(&row) : NULL;
done:
	venture_database_rollback(self->database);
	self->busy = FALSE;
	return NULL;
}

VentureEntity *
venture_activity_service_act(VentureActivityService *self, VentureEntity *activity,
	const gchar *action, const gchar *value, const VentureActor *actor, GError **error)
{
	return activity_act_full(self, activity, action, value, FALSE, actor, error);
}

VentureEntity *
venture_activity_service_log_call(VentureActivityService *self, VentureEntity *subject,
	JsonObject *details, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) live = NULL, proposed = NULL;
	g_autofree gchar *outcome = NULL;
	g_return_val_if_fail(VENTURE_IS_ACTIVITY_SERVICE(self), NULL);
	g_return_val_if_fail(VENTURE_IS_ENTITY(subject) && details != NULL, NULL);
	if (self->database == NULL || self->busy ||
		!(VENTURE_IS_ACTIVITY(subject) || VENTURE_IS_COMPANY(subject) || VENTURE_IS_CONTACT(subject) || VENTURE_IS_LEAD(subject)))
	{
		refuse(error, VENTURE_ERROR_CONFLICT, "Call subject or service is unavailable");
		return NULL;
	}
	live = venture_database_get(self->database, G_OBJECT_TYPE(subject), venture_entity_get_id(subject), error);
	if (live == NULL)
		return NULL;
	if (venture_entity_is_deleted(live) || venture_entity_get_organization_id(live) != venture_entity_get_organization_id(subject))
	{
		refuse(error, VENTURE_ERROR_NOT_FOUND, "Call subject is not available in this organization");
		return NULL;
	}
	proposed = VENTURE_ENTITY(venture_activity_new());
	if (VENTURE_IS_ACTIVITY(subject))
	{
		/* Only typed action inputs may change the saved plan. Keep the
		 * caller version so a stale request still conflicts below. */
		venture_entity_copy_properties_from(proposed, live, FALSE);
		g_object_set(proposed, "version", venture_entity_get_version(subject), NULL);
	}
	else
	{
		venture_entity_set_organization_id(proposed, venture_entity_get_organization_id(live));
		g_object_set(proposed, "kind", VENTURE_ACTIVITY_KIND_CALL,
			"related-type", venture_entity_get_entity_name(live), "related-id", venture_entity_get_id(live), NULL);
		/* Prepare the explicit relation before creating the historical row. */
		if (VENTURE_IS_COMPANY(live)) g_object_set(proposed, "company-id", venture_entity_get_id(live), NULL);
		if (VENTURE_IS_CONTACT(live)) g_object_set(proposed, "contact-id", venture_entity_get_id(live), NULL);
		if (VENTURE_IS_LEAD(live)) g_object_set(proposed, "lead-id", venture_entity_get_id(live), NULL);
	}
	if (!call_prepare(proposed, subject, details, actor, error))
		return NULL;
	g_object_get(proposed, "outcome", &outcome, NULL);
	return activity_act_full(self, proposed, "complete", outcome, TRUE, actor, error);
}

VentureEntity *
venture_activity_service_complete(VentureActivityService *self, VentureEntity *activity,
	const gchar *outcome, const VentureActor *actor, GError **error)
{
	return venture_activity_service_act(self, activity, "complete", outcome, actor, error);
}

GPtrArray *
venture_activity_service_list(VentureActivityService *self, gint64 organization, const gchar *owner,
	const gchar *queue, GDateTime *now, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ACTIVITY);
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GDateTime) clock = now ? g_date_time_ref(now) : venture_time_now();
	g_autoptr(GDateTime) day = g_date_time_new_utc(g_date_time_get_year(clock), g_date_time_get_month(clock), g_date_time_get_day_of_month(clock), 0, 0, 0);
	g_autoptr(GDateTime) tomorrow = g_date_time_add_days(day, 1);
	GPtrArray *result;
	guint i;
	if (organization <= 0 || self->database == NULL ||
		venture_entity_registry_lookup(venture_entity_registry_get_default(), "activity") == G_TYPE_INVALID)
	{
		refuse(error, VENTURE_ERROR_NOT_FOUND, "Activities require an enabled module and an organization");
		return NULL;
	}
	venture_query_set_organization(query, organization);
	venture_query_set_limit(query, 0);
	venture_query_add_order(query, "due-at", VENTURE_SORT_ASCENDING, NULL);
	if (owner != NULL)
		venture_query_add_filter_string(query, "owner", VENTURE_FILTER_OP_EQ, owner, NULL);
	rows = venture_database_find(self->database, query, error);
	if (rows == NULL)
		return NULL;
	result = g_ptr_array_new_with_free_func(g_object_unref);
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(rows, i);
		g_autoptr(GDateTime) due = NULL;
		gint status;
		gboolean include = FALSE;
		g_object_get(row, "status", &status, "due-at", &due, NULL);
		if (g_strcmp0(queue, "all") == 0)
			include = TRUE;
		else if (status == VENTURE_ACTIVITY_STATUS_PLANNED)
		{
			if (g_strcmp0(queue, "mine") == 0)
				include = TRUE;
			else if (due != NULL && g_strcmp0(queue, "overdue") == 0)
				include = g_date_time_compare(due, day) < 0;
			else if (due != NULL && g_strcmp0(queue, "today") == 0)
				include = g_date_time_compare(due, day) >= 0 && g_date_time_compare(due, tomorrow) < 0;
			else if (g_strcmp0(queue, "upcoming") == 0)
				include = due == NULL || g_date_time_compare(due, tomorrow) >= 0;
		}
		if (include)
			g_ptr_array_add(result, g_object_ref(row));
	}
	return result;
}

gint
venture_activity_service_sweep(VentureActivityService *self, gint64 organization, guint limit, GError **error)
{
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	guint i;
	gint delivered = 0;
	gboolean committed;
	if (self->database == NULL || self->busy)
	{
		refuse(error, VENTURE_ERROR_CONFLICT, "Service unavailable or reentrant reminder sweep");
		return -1;
	}
	if (!venture_database_begin(self->database, error))
		return -1;
	self->busy = TRUE;
	rows = venture_activity_service_list(self, organization, NULL, "mine", now, error);
	if (rows == NULL)
		goto fail;
	for (i = 0; i < rows->len && delivered < (gint)(limit ? limit : 200); i++)
	{
		VentureEntity *row = g_ptr_array_index(rows, i);
		g_autoptr(GDateTime) remind = NULL;
		g_autoptr(GDateTime) sent = NULL;
		g_autofree gchar *owner = NULL;
		g_autofree gchar *subject = NULL;
		g_autoptr(VentureQuery) query = NULL;
		g_autoptr(VentureEntity) user = NULL;
		g_autoptr(VentureNotification) notification = NULL;
		g_object_get(row, "remind-at", &remind, "reminded-at", &sent, "owner", &owner, "subject", &subject, NULL);
		if (remind == NULL || sent != NULL || g_date_time_compare(remind, now) > 0 || owner == NULL || *owner == '\0')
			continue;
		query = venture_query_new(VENTURE_TYPE_USER);
		venture_query_set_organization(query, organization);
		venture_query_add_filter_string(query, "username", VENTURE_FILTER_OP_EQ, owner, NULL);
		venture_query_add_filter_int(query, "active", VENTURE_FILTER_OP_EQ, TRUE, NULL);
		user = venture_database_find_one(self->database, query, NULL);
		if (user == NULL)
			continue;
		notification = venture_notification_new();
		venture_entity_set_organization_id(VENTURE_ENTITY(notification), organization);
		g_object_set(notification, "kind", VENTURE_NOTIFICATION_KIND_SYSTEM, "user-id", venture_entity_get_id(user), "title", subject,
			"body", "Planned activity reminder", "target-type", "activity", "target-id", venture_entity_get_id(row),
			"target-label", subject, "actor", "system", "occurred-at", now, NULL);
		if (!venture_database_save(self->database, VENTURE_ENTITY(notification), NULL, error))
			goto fail;
		g_object_set(row, "reminded-at", now, NULL);
		if (!write_record(self, row, NULL, error))
			goto fail;
		delivered++;
	}
	committed = venture_database_commit(self->database, error);
	self->busy = FALSE;
	return committed ? delivered : -1;
fail:
	venture_database_rollback(self->database);
	self->busy = FALSE;
	return -1;
}

/* Fold by octets without cutting a UTF-8 sequence. Continuation whitespace
 * counts toward the next line's 75-octet limit. */
static void
calendar_line(GString *calendar, const gchar *name, const gchar *value, gboolean escape)
{
	g_autoptr(GString) line = g_string_new(name);
	const gchar *p;
	guint width = 0;
	g_string_append_c(line, ':');
	for (p = value ? value : ""; *p; p++)
	{
		if (escape && (*p == '\\' || *p == ';' || *p == ','))
			g_string_append_c(line, '\\');
		if (*p == '\r')
			continue;
		if (*p == '\n')
			g_string_append(line, "\\n");
		else
			g_string_append_c(line, *p);
	}
	for (p = line->str; *p; )
	{
		const gchar *next = g_utf8_next_char(p);
		gsize size = next - p;
		if (width + size > 75)
		{
			g_string_append(calendar, "\r\n ");
			width = 1;
		}
		g_string_append_len(calendar, p, size);
		width += size;
		p = next;
	}
	g_string_append(calendar, "\r\n");
}

static void
calendar_date(GString *calendar, const gchar *name, GDateTime *date)
{
	g_autoptr(GDateTime) utc = g_date_time_to_utc(date);
	g_autofree gchar *value = g_date_time_format(utc, "%Y%m%dT%H%M%SZ");
	calendar_line(calendar, name, value, FALSE);
}

gchar *
venture_activity_service_calendar(VentureActivityService *self, gint64 organization,
	const gchar *owner, GError **error)
{
	g_autoptr(GPtrArray) rows = venture_activity_service_list(self, organization, owner, "all", NULL, error);
	g_autoptr(GString) calendar = g_string_new("BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//VENTURE//Activities//EN\r\nCALSCALE:GREGORIAN\r\n");
	g_autoptr(GDateTime) now = venture_time_now();
	guint i;
	if (rows == NULL)
		return NULL;
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(rows, i);
		g_autoptr(GDateTime) starts = NULL;
		g_autoptr(GDateTime) ends = NULL;
		g_autoptr(GDateTime) due = NULL;
		g_autofree gchar *subject = NULL;
		g_autofree gchar *body = NULL;
		g_autofree gchar *uid = NULL;
		gint kind, status;
		g_object_get(row, "kind", &kind, "status", &status, "starts-at", &starts,
			"ends-at", &ends, "due-at", &due, "subject", &subject, "body", &body, NULL);
		if ((kind != VENTURE_ACTIVITY_KIND_CALL && kind != VENTURE_ACTIVITY_KIND_MEETING) || (starts == NULL && due == NULL))
			continue;
		uid = g_strdup_printf("%s@venture", venture_entity_get_uuid(row));
		g_string_append(calendar, "BEGIN:VEVENT\r\n");
		calendar_line(calendar, "UID", uid, FALSE);
		calendar_date(calendar, "DTSTAMP", now);
		calendar_date(calendar, "DTSTART", starts ? starts : due);
		if (ends != NULL)
			calendar_date(calendar, "DTEND", ends);
		calendar_line(calendar, "SUMMARY", subject, TRUE);
		calendar_line(calendar, "DESCRIPTION", body, TRUE);
		calendar_line(calendar, "STATUS", status == VENTURE_ACTIVITY_STATUS_CANCELLED ? "CANCELLED" : "CONFIRMED", FALSE);
		g_string_append(calendar, "END:VEVENT\r\n");
	}
	g_string_append(calendar, "END:VCALENDAR\r\n");
	return g_string_free(g_steal_pointer(&calendar), FALSE);
}

/* The same folding and escaping for any module that writes a calendar. */
void
venture_activity_calendar_append_line(GString *calendar, const gchar *name,
	const gchar *value, gboolean escape)
{
	g_return_if_fail(calendar != NULL && name != NULL);
	calendar_line(calendar, name, value, escape);
}

void
venture_activity_calendar_append_date(GString *calendar, const gchar *name,
	GDateTime *date)
{
	g_return_if_fail(calendar != NULL && name != NULL && date != NULL);
	calendar_date(calendar, name, date);
}
