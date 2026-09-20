/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <string.h>

struct _VentureCalendarSyncService {
	GObject parent_instance;
	VentureDatabase *database;
	VentureCalDavClient *client;
	VentureConfig *config;
	gboolean busy;
};
G_DEFINE_FINAL_TYPE(VentureCalendarSyncService, venture_calendar_sync_service, G_TYPE_OBJECT)

static gboolean refuse(GError **error, VentureError code, const gchar *message)
{
	g_set_error(error, VENTURE_ERROR, code, "VentureCalendarSyncService: %s", message);
	return FALSE;
}
static gboolean module_on(void)
{
	return venture_entity_registry_lookup(venture_entity_registry_get_default(), "calendar_event") != G_TYPE_INVALID;
}
static void finalize(GObject *object)
{
	VentureCalendarSyncService *self = VENTURE_CALENDAR_SYNC_SERVICE(object);
	if (self->database) g_object_remove_weak_pointer(G_OBJECT(self->database), (gpointer *)&self->database);
	g_clear_object(&self->client);
	g_clear_object(&self->config);
	G_OBJECT_CLASS(venture_calendar_sync_service_parent_class)->finalize(object);
}
static void set_property(GObject *object, guint id, const GValue *value, GParamSpec *pspec)
{
	VentureCalendarSyncService *self = VENTURE_CALENDAR_SYNC_SERVICE(object);
	switch (id) {
	case 1:
		self->database = g_value_get_object(value);
		if (self->database) g_object_add_weak_pointer(G_OBJECT(self->database), (gpointer *)&self->database);
		break;
	case 2: g_set_object(&self->client, g_value_get_object(value)); break;
	case 3: g_set_object(&self->config, g_value_get_object(value)); break;
	default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
	}
}
static void get_property(GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
	VentureCalendarSyncService *self = VENTURE_CALENDAR_SYNC_SERVICE(object);
	switch (id) {
	case 1: g_value_set_object(value, self->database); break;
	case 2: g_value_set_object(value, self->client); break;
	case 3: g_value_set_object(value, self->config); break;
	default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
	}
}
static void venture_calendar_sync_service_class_init(VentureCalendarSyncServiceClass *klass)
{
	GObjectClass *object = G_OBJECT_CLASS(klass);
	object->finalize = finalize;
	object->set_property = set_property;
	object->get_property = get_property;
	g_object_class_install_property(object, 1, g_param_spec_object("database", "Database", "Weak owning database", VENTURE_TYPE_DATABASE, G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
	g_object_class_install_property(object, 2, g_param_spec_object("client", "Client", "CalDAV transport", VENTURE_TYPE_CALDAV_CLIENT, G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
	g_object_class_install_property(object, 3, g_param_spec_object("config", "Config", "Operator connector endpoint policy", VENTURE_TYPE_CONFIG, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
}
static void venture_calendar_sync_service_init(VentureCalendarSyncService *self) { self->config = venture_config_new(); }
VentureCalendarSyncService *venture_calendar_sync_service_new(VentureDatabase *database, VentureCalDavClient *client)
{
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	g_return_val_if_fail(VENTURE_IS_CALDAV_CLIENT(client), NULL);
	return g_object_new(VENTURE_TYPE_CALENDAR_SYNC_SERVICE, "database", database, "client", client, NULL);
}

gchar *venture_calendar_sync_event_uid(VentureEntity *activity)
{
	g_return_val_if_fail(VENTURE_IS_ENTITY(activity), NULL);
	return g_strdup_printf("%s@venture", venture_entity_get_uuid(activity));
}

/* --- Mapping between an activity and a VEVENT ----------------------------- */
/* Recurrence is not carried either way, for the reason the .ics export gives:
 * each stored occurrence is a separate event and recurrence is advanced by
 * completion, so a series written as RRULE would be duplicated by the next
 * occurrence, and a series read from the calendar is the calendar's to expand.
 * The RRULE text stays on the parsed event for display; it is never expanded. */
static VentureICalEvent *event_for_activity(VentureEntity *activity)
{
	VentureICalEvent *event = venture_ical_event_new();
	g_autoptr(GDateTime) starts = NULL, ends = NULL, due = NULL;
	gint status;
	g_object_get(activity, "subject", &event->summary, "body", &event->description, "starts-at", &starts, "ends-at", &ends,
		"due-at", &due, "status", &status, NULL);
	event->uid = venture_calendar_sync_event_uid(activity);
	event->starts = g_date_time_ref(starts ? starts : due);
	event->ends = ends ? g_date_time_ref(ends) : NULL;
	event->status = g_strdup(status == VENTURE_ACTIVITY_STATUS_CANCELLED || venture_entity_is_deleted(activity) ? "CANCELLED" : "CONFIRMED");
	event->last_modified = venture_entity_get_updated_at(activity) ? g_date_time_to_utc(venture_entity_get_updated_at(activity)) : NULL;
	event->sequence = venture_entity_get_version(activity);
	return event;
}
static gboolean pushable(VentureEntity *activity)
{
	g_autoptr(GDateTime) starts = NULL, due = NULL;
	gint kind;
	g_object_get(activity, "kind", &kind, "starts-at", &starts, "due-at", &due, NULL);
	return (kind == VENTURE_ACTIVITY_KIND_CALL || kind == VENTURE_ACTIVITY_KIND_MEETING) && (starts || due);
}
static void apply_event(VentureEntity *activity, const VentureICalEvent *event)
{
	gint status;
	g_object_get(activity, "status", &status, NULL);
	g_object_set(activity, "subject", event->summary && *event->summary ? event->summary : "(untitled)", "body", event->description,
		"starts-at", event->starts, "ends-at", event->ends, "due-at", event->starts, NULL);
	/* A done activity keeps its completion; planned and cancelled follow the calendar. */
	if (status != VENTURE_ACTIVITY_STATUS_DONE)
		g_object_set(activity, "status", !g_strcmp0(event->status, "CANCELLED") ? VENTURE_ACTIVITY_STATUS_CANCELLED : VENTURE_ACTIVITY_STATUS_PLANNED, NULL);
}
static void add_date(JsonObject *object, const gchar *name, GDateTime *when)
{
	g_autofree gchar *text = when ? venture_time_to_string(when) : NULL;
	if (text) json_object_set_string_member(object, name, text); else json_object_set_null_member(object, name);
}
static JsonObject *snapshot_activity(VentureEntity *activity)
{
	JsonObject *object = json_object_new();
	g_autofree gchar *subject = NULL, *body = NULL;
	g_autoptr(GDateTime) starts = NULL, ends = NULL, due = NULL;
	gint status;
	g_object_get(activity, "subject", &subject, "body", &body, "starts-at", &starts, "ends-at", &ends, "due-at", &due, "status", &status, NULL);
	json_object_set_string_member(object, "subject", subject ? subject : "");
	json_object_set_string_member(object, "body", body ? body : "");
	add_date(object, "starts_at", starts); add_date(object, "ends_at", ends); add_date(object, "due_at", due);
	json_object_set_string_member(object, "status", venture_enum_to_nick(venture_activity_status_get_type(), status));
	return object;
}
static JsonObject *snapshot_event(const VentureICalEvent *event)
{
	JsonObject *object = json_object_new();
	json_object_set_string_member(object, "subject", event->summary ? event->summary : "");
	json_object_set_string_member(object, "body", event->description ? event->description : "");
	add_date(object, "starts_at", event->starts); add_date(object, "ends_at", event->ends); add_date(object, "due_at", event->starts);
	json_object_set_string_member(object, "status", !g_strcmp0(event->status, "CANCELLED") ? "cancelled" : "planned");
	return object;
}
/* The loser of a conflict, and a cancellation caused by the other side, are
 * written to the activity's timeline as an audit note so nothing is lost
 * silently: GET /api/v1/activity/activity/:id shows it. */
static gboolean note_on_timeline(VentureDatabase *db, VentureEntity *activity, const gchar *note, JsonObject *previous, GError **error)
{
	g_autoptr(JsonNode) diff = json_node_new(JSON_NODE_OBJECT);
	g_autoptr(JsonObject) object = json_object_new();
	g_autoptr(VentureAuditEntry) entry = NULL;
	json_object_set_string_member(object, "note", note);
	if (previous) json_object_set_object_member(object, "previous", json_object_ref(previous));
	json_node_set_object(diff, object);
	entry = venture_audit_entry_new_for_change(VENTURE_AUDIT_ACTION_UPDATE, VENTURE_ACTOR_KIND_SYSTEM, "calendar", activity, diff);
	g_object_set(entry, "source", "calendar", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(entry), venture_entity_get_organization_id(activity));
	return venture_database_save(db, VENTURE_ENTITY(entry), NULL, error);
}

/* --- Links ---------------------------------------------------------------- */
typedef struct {
	VentureCalendarSyncService *self;
	VentureConnectorSession *connector;
	VentureEntity *account;
	const VentureActor *actor;
	gint64 org;
	gint64 private_owner;
	gchar *owner;
	gchar *path;
	GPtrArray *links;
	GHashTable *seen; /* link id -> present on the server this pass */
	gint changes;
} Pass;
static VentureEntity *link_by(Pass *pass, const gchar *field, const gchar *value)
{
	guint i;
	for (i = 0; value && i < pass->links->len; i++) {
		VentureEntity *link = g_ptr_array_index(pass->links, i);
		g_autofree gchar *have = NULL;
		g_object_get(link, field, &have, NULL);
		if (!g_strcmp0(have, value)) return link;
	}
	return NULL;
}
static VentureEntity *link_by_activity(Pass *pass, gint64 activity_id)
{
	guint i;
	for (i = 0; i < pass->links->len; i++) {
		VentureEntity *link = g_ptr_array_index(pass->links, i);
		gint64 have = 0;
		g_object_get(link, "activity-id", &have, NULL);
		if (have == activity_id) return link;
	}
	return NULL;
}
static gchar *href_for(Pass *pass, const gchar *uid)
{
	g_autofree gchar *safe = g_uri_escape_string(uid, "@", FALSE);
	return g_strdup_printf("%s%s%s.ics", pass->path, g_str_has_suffix(pass->path, "/") ? "" : "/", safe);
}
static VentureEntity *new_link(Pass *pass, VentureEntity *activity, const gchar *uid, const gchar *href)
{
	g_autofree gchar *key = g_strdup_printf("%" G_GINT64_FORMAT ":%s", venture_entity_get_id(pass->account), uid);
	return g_object_new(VENTURE_TYPE_CALENDAR_EVENT, "organization-id", pass->org, "account-id", venture_entity_get_id(pass->account),
		"activity-id", activity ? venture_entity_get_id(activity) : (gint64)0, "uid", uid, "uid-key", key, "href", href, NULL);
}
static void stamp_link(VentureEntity *link, VentureEntity *activity, const gchar *etag, GDateTime *remote_modified)
{
	static const gchar *fields[] = { "subject", "body", "starts-at", "ends-at", "due-at", "status" };
	g_autoptr(GDateTime) now = venture_time_now();
	guint i;
	/* The generic record views render these declared fields for both private
	 * and shared calendars; no protocol JSON is exposed as the operator UI. */
	for (i = 0; i < G_N_ELEMENTS(fields); i++) {
		GParamSpec *field = g_object_class_find_property(G_OBJECT_GET_CLASS(activity), fields[i]);
		g_auto(GValue) value = G_VALUE_INIT;
		g_value_init(&value, G_PARAM_SPEC_VALUE_TYPE(field));
		g_object_get_property(G_OBJECT(activity), fields[i], &value);
		g_object_set_property(G_OBJECT(link), fields[i], &value);
	}
	g_object_set(link, "etag", etag, "local-version", venture_entity_get_version(activity), "remote-modified-at", remote_modified, "synced-at", now, NULL);
}

/* --- Pushing one activity ------------------------------------------------- */
static gboolean push_activity(Pass *pass, VentureEntity *activity, VentureEntity *link, JsonObject *lost_remote, GError **error)
{
	VentureDatabase *db = pass->self->database;
	g_autoptr(VentureICalEvent) event = event_for_activity(activity);
	g_autofree gchar *ics = venture_ical_event_format(event, NULL);
	g_autofree gchar *etag = NULL, *have = NULL, *href = NULL;
	g_autoptr(VentureEntity) created = NULL;
	if (link) g_object_get(link, "etag", &have, "href", &href, NULL);
	else href = href_for(pass, event->uid);
	if (!venture_connector_session_validate(pass->connector, error)) return FALSE;
	etag = venture_caldav_client_put(pass->self->client, href, ics, link && have && *have ? have : NULL, NULL, error);
	if (!etag) return FALSE;
	if (!venture_database_begin(db, error)) return FALSE;
	if (!venture_connector_session_validate(pass->connector, error)) goto fail;
	if (!link) { created = new_link(pass, activity, event->uid, href); link = created; }
	stamp_link(link, activity, etag, event->last_modified);
	if (!venture_database_save(db, link, pass->actor, error)) goto fail;
	if (lost_remote && !note_on_timeline(db, activity, "Calendar copy was older and was overwritten by this activity; its values are kept here", lost_remote, error)) goto fail;
	if (!venture_database_commit(db, error)) return FALSE;
	if (created) g_ptr_array_add(pass->links, g_steal_pointer(&created));
	pass->changes++;
	return TRUE;
fail:
	venture_database_rollback(db);
	return FALSE;
}

/* Private calendars retain their own snapshot. A shared Activity would
 * expose its title through CRM reports, search and organization automation. */
static gboolean pull_private(Pass *pass, VentureEntity *link, const VentureICalEvent *event,
	const gchar *href, const gchar *etag, GError **error)
{
	VentureDatabase *db = pass->self->database;
	g_autoptr(VentureEntity) created = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	if (!venture_database_begin(db, error)) return FALSE;
	if (!venture_connector_session_validate(pass->connector, error)) goto fail;
	if (!link) { created = new_link(pass, NULL, event->uid, href); link = created; }
	else if (venture_entity_is_deleted(link) && !venture_database_restore(db, link, pass->actor, error)) goto fail;
	apply_event(link, event);
	g_object_set(link, "href", href, "etag", etag,
		"remote-modified-at", event->last_modified, "synced-at", now, NULL);
	if (!venture_database_save(db, link, pass->actor, error)) goto fail;
	if (!venture_database_commit(db, error)) return FALSE;
	g_hash_table_add(pass->seen, GINT_TO_POINTER((gint)venture_entity_get_id(link)));
	if (created) g_ptr_array_add(pass->links, g_steal_pointer(&created));
	pass->changes++;
	return TRUE;
fail:
	venture_database_rollback(db);
	return FALSE;
}

/* --- Pulling one VEVENT --------------------------------------------------- */
static gboolean pull_event(Pass *pass, const VentureCalDavItem *item, GError **error)
{
	VentureDatabase *db = pass->self->database;
	g_autofree gchar *ics = NULL, *etag = NULL;
	g_autoptr(VentureICalEvent) event = NULL;
	g_autoptr(VentureEntity) activity = NULL, created = NULL;
	g_autoptr(GError) parse_error = NULL;
	g_autoptr(JsonObject) previous = NULL;
	VentureEntity *link = link_by(pass, "href", item->href);
	gboolean local_changed = FALSE, remote_wins = TRUE;
	gint64 activity_id = 0, local_version = 0;
	if (link) g_hash_table_add(pass->seen, GINT_TO_POINTER((gint)venture_entity_get_id(link)));
	{
		g_autofree gchar *have = NULL;
		if (link) g_object_get(link, "etag", &have, NULL);
		if (link && !venture_entity_is_deleted(link) && !g_strcmp0(have, item->etag)) return TRUE; /* unchanged since the last pass */
	}
	if (!venture_connector_session_validate(pass->connector, error)) return FALSE;
	ics = venture_caldav_client_fetch(pass->self->client, item->href, &etag, NULL, error);
	if (!ics || !venture_connector_session_validate(pass->connector, error)) return FALSE;
	event = venture_ical_event_parse(ics, &parse_error);
	if (!event) {
		/* A VTODO or a journal entry is not ours to mirror; a malformed event is not either. */
		if (g_error_matches(parse_error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND) || g_error_matches(parse_error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION)) return TRUE;
		g_propagate_error(error, g_steal_pointer(&parse_error));
		return FALSE;
	}
	if (!link) {
		/* Known UID at a new path: the event was moved on the server; follow it. */
		link = link_by(pass, "uid", event->uid);
		if (link) g_object_set(link, "href", item->href, NULL);
	}
	if (pass->private_owner > 0) return pull_private(pass, link, event, item->href, etag, error);
	if (link) {
		g_hash_table_add(pass->seen, GINT_TO_POINTER((gint)venture_entity_get_id(link)));
		g_object_get(link, "activity-id", &activity_id, "local-version", &local_version, NULL);
		activity = venture_database_get(db, VENTURE_TYPE_ACTIVITY, activity_id, error);
		if (!activity) return FALSE;
		local_changed = venture_entity_get_version(activity) != local_version;
	} else if (g_str_has_suffix(event->uid, "@venture")) {
		/* Our own UID with no link: the activity was exported or the link was lost. */
		g_autofree gchar *uuid = g_strndup(event->uid, strlen(event->uid) - strlen("@venture"));
		activity = venture_database_get_by_uuid(db, VENTURE_TYPE_ACTIVITY, uuid, NULL);
		if (activity && venture_entity_get_organization_id(activity) != pass->org) g_clear_object(&activity);
		local_changed = activity != NULL;
	}
	if (activity && venture_entity_is_deleted(activity)) {
		/* Deleted here: the calendar side is cancelled in the push phase; nothing to pull. */
		if (link) { g_object_set(link, "etag", etag, NULL); return venture_database_save(db, link, pass->actor, error); }
		return TRUE;
	}
	if (local_changed) {
		/* Both sides moved: the later modification wins, the other is noted. */
		GDateTime *local_modified = venture_entity_get_updated_at(activity);
		remote_wins = event->last_modified && local_modified && g_date_time_compare(event->last_modified, local_modified) > 0;
		if (!remote_wins) {
			g_autoptr(JsonObject) lost = snapshot_event(event);
			gboolean pushed;
			/* Replace the server's copy where it lives, under the etag just read. */
			if (!link) { created = new_link(pass, activity, event->uid, item->href); link = created; }
			g_object_set(link, "etag", etag, "remote-modified-at", event->last_modified, NULL);
			pushed = push_activity(pass, activity, link, lost, error);
			if (pushed && created) { g_hash_table_add(pass->seen, GINT_TO_POINTER((gint)venture_entity_get_id(created))); g_ptr_array_add(pass->links, g_steal_pointer(&created)); }
			return pushed;
		}
		previous = snapshot_activity(activity);
	}
	if (!venture_database_begin(db, error)) return FALSE;
	if (!venture_connector_session_validate(pass->connector, error)) goto fail;
	if (!activity) {
		activity = g_object_new(VENTURE_TYPE_ACTIVITY, "organization-id", pass->org, "kind", VENTURE_ACTIVITY_KIND_MEETING, "owner", pass->owner, NULL);
		apply_event(activity, event);
		if (!venture_database_save(db, activity, pass->actor, error)) goto fail;
	} else {
		gint status;
		g_object_get(activity, "status", &status, NULL);
		/* A done activity keeps its values; the note is only written when something was replaced. */
		if (status != VENTURE_ACTIVITY_STATUS_DONE) {
			apply_event(activity, event);
			if (!venture_database_save(db, activity, pass->actor, error)) goto fail;
			if (previous && !note_on_timeline(db, activity, "Calendar copy was newer and replaced these values", previous, error)) goto fail;
		}
	}
	if (!link) { created = new_link(pass, activity, event->uid, item->href); link = created; }
	stamp_link(link, activity, etag, event->last_modified);
	if (!venture_database_save(db, link, pass->actor, error)) goto fail;
	if (!venture_database_commit(db, error)) return FALSE;
	if (created) { g_hash_table_add(pass->seen, GINT_TO_POINTER((gint)venture_entity_get_id(created))); g_ptr_array_add(pass->links, g_steal_pointer(&created)); }
	pass->changes++;
	return TRUE;
fail:
	venture_database_rollback(db);
	return FALSE;
}

/* --- An event removed on the server: cancel here, never delete ------------ */
static gboolean cancel_missing(Pass *pass, VentureEntity *link, GError **error)
{
	VentureDatabase *db = pass->self->database;
	g_autoptr(VentureEntity) activity = NULL;
	gint64 activity_id = 0;
	gint status;
	if (pass->private_owner > 0) {
		if (!venture_database_begin(db, error)) return FALSE;
		if (!venture_connector_session_validate(pass->connector, error) || !venture_database_delete(db, link, pass->actor, error)) goto fail;
		if (!venture_database_commit(db, error)) return FALSE;
		pass->changes++;
		return TRUE;
	}
	g_object_get(link, "activity-id", &activity_id, NULL);
	activity = venture_database_get(db, VENTURE_TYPE_ACTIVITY, activity_id, error);
	if (!activity) return FALSE;
	if (!venture_database_begin(db, error)) return FALSE;
	if (!venture_connector_session_validate(pass->connector, error)) goto fail;
	g_object_get(activity, "status", &status, NULL);
	if (!venture_entity_is_deleted(activity) && status == VENTURE_ACTIVITY_STATUS_PLANNED) {
		g_autoptr(JsonObject) previous = snapshot_activity(activity);
		g_object_set(activity, "status", VENTURE_ACTIVITY_STATUS_CANCELLED, NULL);
		if (!venture_database_save(db, activity, pass->actor, error)) goto fail;
		if (!note_on_timeline(db, activity, "Removed from the calendar; the activity was cancelled rather than deleted", previous, error)) goto fail;
	}
	if (!venture_database_delete(db, link, pass->actor, error)) goto fail;
	if (!venture_database_commit(db, error)) return FALSE;
	pass->changes++;
	return TRUE;
fail:
	venture_database_rollback(db);
	return FALSE;
}

/* --- The pass ------------------------------------------------------------- */
static gboolean run_pass(Pass *pass, gchar **token_out, GError **error)
{
	VentureDatabase *db = pass->self->database;
	g_autoptr(GPtrArray) items = NULL, activities = NULL;
	g_autofree gchar *token = NULL;
	guint i;
	if (!venture_connector_session_validate(pass->connector, error)) return FALSE;
	token = venture_caldav_client_get_token(pass->self->client, pass->path, NULL, error);
	if (!token || !venture_connector_session_validate(pass->connector, error)) return FALSE;
	items = venture_caldav_client_list(pass->self->client, pass->path, NULL, error);
	if (!items || !venture_connector_session_validate(pass->connector, error)) return FALSE;
	for (i = 0; i < items->len; i++)
		if (!pull_event(pass, g_ptr_array_index(items, i), error)) return FALSE;
	/* Links whose event is gone from the server. Iterate a copy: cancelling removes nothing from the array, but be explicit. */
	for (i = 0; i < pass->links->len; i++) {
		VentureEntity *link = g_ptr_array_index(pass->links, i);
		if (venture_entity_is_deleted(link) || g_hash_table_contains(pass->seen, GINT_TO_POINTER((gint)venture_entity_get_id(link)))) continue;
		if (!cancel_missing(pass, link, error)) return FALSE;
	}
	if (pass->private_owner > 0) { *token_out = g_steal_pointer(&token); return TRUE; }
	/* Push: the owner's dated calls and meetings, and any linked activity that changed or was deleted here. */
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ACTIVITY);
		venture_query_set_organization(query, pass->org);
		venture_query_add_filter_string(query, "owner", VENTURE_FILTER_OP_EQ, pass->owner, NULL);
		venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
		venture_query_set_limit(query, 0);
		activities = venture_database_find(db, query, error);
		if (!activities) return FALSE;
	}
	for (i = 0; i < activities->len; i++) {
		VentureEntity *activity = g_ptr_array_index(activities, i);
		VentureEntity *link = link_by_activity(pass, venture_entity_get_id(activity));
		gint64 local_version = -1;
		gint status;
		if (!pushable(activity)) continue;
		g_object_get(activity, "status", &status, NULL);
		if (link) g_object_get(link, "local-version", &local_version, NULL);
		if (link && venture_entity_is_deleted(link)) continue;
		if (!link && status == VENTURE_ACTIVITY_STATUS_CANCELLED) continue; /* nothing on the calendar to cancel */
		if (link && local_version == venture_entity_get_version(activity)) continue;
		if (!push_activity(pass, activity, link, NULL, error)) return FALSE;
	}
	/* Deleted here while linked: the calendar copy becomes CANCELLED once. */
	for (i = 0; i < pass->links->len; i++) {
		VentureEntity *link = g_ptr_array_index(pass->links, i);
		g_autoptr(VentureEntity) activity = NULL;
		gint64 activity_id = 0, local_version = 0;
		if (venture_entity_is_deleted(link)) continue;
		g_object_get(link, "activity-id", &activity_id, "local-version", &local_version, NULL);
		activity = venture_database_get(db, VENTURE_TYPE_ACTIVITY, activity_id, NULL);
		if (!activity || !venture_entity_is_deleted(activity)) continue;
		if (local_version == venture_entity_get_version(activity)) continue;
		if (!push_activity(pass, activity, link, NULL, error)) return FALSE;
	}
	*token_out = g_steal_pointer(&token);
	return TRUE;
}

gint venture_calendar_sync_service_sync(VentureCalendarSyncService *self, VentureEntity *account, const VentureActor *actor, GError **error)
{
	g_autofree gchar *url = NULL, *username = NULL, *owner = NULL, *path = NULL, *token = NULL;
	g_autoptr(VentureConnectorSession) connector = NULL;
	VentureEntity *requested = account;
	g_autoptr(GError) local = NULL;
	g_autoptr(GDateTime) now = NULL;
	g_autoptr(GPtrArray) links = NULL;
	g_autoptr(GHashTable) seen = NULL;
	const gchar *secret;
	Pass pass;
	g_return_val_if_fail(VENTURE_IS_CALENDAR_SYNC_SERVICE(self), -1);
	g_return_val_if_fail(VENTURE_IS_CALENDAR_ACCOUNT(account), -1);
	if (!self->database) return refuse(error, VENTURE_ERROR_DATABASE, "The database has been closed"), -1;
	if (!module_on()) return refuse(error, VENTURE_ERROR_CONFIG, "The calendar module is disabled"), -1;
	if (self->busy) return refuse(error, VENTURE_ERROR_CONFLICT, "A sync is already in progress"), -1;
	if (venture_database_has_transaction(self->database)) return refuse(error, VENTURE_ERROR_CONFLICT, "Sync requires a committed database"), -1;
	if (!venture_entity_get_id(account)) return refuse(error, VENTURE_ERROR_VALIDATION, "Sync requires a saved account"), -1;
	connector = venture_connector_open(self->database, self->config, account, error);
	if (!connector) return -1;
	account = venture_connector_session_get_account(connector);
	secret = venture_connector_session_get_password(connector);
	g_object_get(account, "url", &url, "username", &username, "owner", &owner, "calendar-path", &path, NULL);
	if (venture_string_is_empty(url)) return refuse(error, VENTURE_ERROR_CONFIG, "The account has no CalDAV URL"), -1;
	if (venture_string_is_empty(owner)) return refuse(error, VENTURE_ERROR_CONFIG, "The account has no owner"), -1;
	if (venture_string_is_empty(path)) return refuse(error, VENTURE_ERROR_CONFIG, "The account has no calendar path"), -1;
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_CALENDAR_EVENT);
		venture_query_set_organization(query, venture_entity_get_organization_id(account));
		venture_query_add_filter_int(query, "account-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(account), NULL);
		venture_query_set_include_deleted(query, venture_access_policy_record_is_personal(venture_database_get_access_policy(self->database), account));
		venture_query_set_limit(query, 0);
		links = venture_database_find(self->database, query, error);
		if (!links) return -1;
	}
	seen = g_hash_table_new(g_direct_hash, g_direct_equal);
	memset(&pass, 0, sizeof pass);
	pass.self = self; pass.connector = connector; pass.account = account; pass.actor = actor;
	pass.org = venture_entity_get_organization_id(account);
	pass.private_owner = venture_access_policy_get_personal_owner(venture_database_get_access_policy(self->database), account);
	pass.owner = owner; pass.path = path; pass.links = links; pass.seen = seen;
	self->busy = TRUE;
	if (venture_caldav_client_connect(self->client, url, username, secret, NULL, &local)) {
		run_pass(&pass, &token, &local);
		venture_caldav_client_disconnect(self->client);
	}
	self->busy = FALSE;
	now = venture_time_now();
	/* The outcome is recorded on the account either way, so the list shows a failing calendar. */
	{
		g_autoptr(GError) note_error = NULL;
		g_object_set(account, "last-synced-at", now, "last-error", local ? local->message : NULL, NULL);
		if (token) g_object_set(account, "sync-token", token, NULL);
		if (!venture_database_save(self->database, account, actor, &note_error) && !local) { g_propagate_error(error, g_steal_pointer(&note_error)); return -1; }
	}
	if (requested != account) venture_entity_copy_properties_from(requested, account, FALSE);
	if (local) { g_propagate_error(error, g_steal_pointer(&local)); return -1; }
	return pass.changes;
}

JsonNode *venture_calendar_sync_service_sweep(VentureCalendarSyncService *self, gint64 organization_id, guint limit, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_CALENDAR_ACCOUNT);
	g_autoptr(GPtrArray) accounts = NULL;
	g_autoptr(JsonBuilder) builder = json_builder_new();
	guint i, synced = 0;
	gint changes = 0;
	g_return_val_if_fail(VENTURE_IS_CALENDAR_SYNC_SERVICE(self), NULL);
	if (organization_id <= 0) return refuse(error, VENTURE_ERROR_VALIDATION, "An exact organization is required"), NULL;
	if (!module_on()) return refuse(error, VENTURE_ERROR_CONFIG, "The calendar module is disabled"), NULL;
	venture_query_set_organization(query, organization_id);
	venture_query_add_filter_string(query, "active", VENTURE_FILTER_OP_EQ, "true", NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	accounts = venture_database_find(self->database, query, error);
	if (!accounts) return NULL;
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "errors");
	json_builder_begin_array(builder);
	for (i = 0; i < accounts->len && synced < limit; i++) {
		g_autoptr(GError) local = NULL;
		gint n = venture_calendar_sync_service_sync(self, g_ptr_array_index(accounts, i), actor, &local);
		synced++;
		if (n < 0) {
			json_builder_begin_object(builder);
			json_builder_set_member_name(builder, "account"); json_builder_add_int_value(builder, venture_entity_get_id(g_ptr_array_index(accounts, i)));
			json_builder_set_member_name(builder, "error"); json_builder_add_string_value(builder, local ? local->message : "unknown");
			json_builder_end_object(builder);
		} else changes += n;
	}
	json_builder_end_array(builder);
	json_builder_set_member_name(builder, "accounts"); json_builder_add_int_value(builder, synced);
	json_builder_set_member_name(builder, "changes"); json_builder_add_int_value(builder, changes);
	json_builder_end_object(builder);
	return json_builder_get_root(builder);
}
