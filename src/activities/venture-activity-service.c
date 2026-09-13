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
	const gchar *refs[] = { "contact-id", "company-id", "deal-id" };
	const gchar *targets[] = { "contact", "company", "deal" };
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

VentureEntity *
venture_activity_service_act(VentureActivityService *self, VentureEntity *activity,
	const gchar *action, const gchar *value, const VentureActor *actor, GError **error)
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
	row = venture_database_get(self->database, VENTURE_TYPE_ACTIVITY, venture_entity_get_id(activity), error);
	if (row == NULL)
		goto done;
	if (venture_entity_get_version(row) != venture_entity_get_version(activity))
	{
		refuse(error, VENTURE_ERROR_CONFLICT, "Activity changed; reload before acting");
		goto done;
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
	if (!write_record(self, row, actor, error))
		goto done;
	if (complete)
	{
		gint64 contact, company, deal;
		gint kind;
		g_autofree gchar *subject = NULL;
		g_object_get(row, "contact-id", &contact, "company-id", &company, "deal-id", &deal,
			"kind", &kind, "subject", &subject, NULL);
		if (contact || company || deal)
		{
			g_autoptr(VentureInteraction) interaction = venture_interaction_new();
			const gchar *kind_name = kind == VENTURE_ACTIVITY_KIND_CALL ? "call" :
				kind == VENTURE_ACTIVITY_KIND_MEETING ? "meeting" : kind == VENTURE_ACTIVITY_KIND_EMAIL ? "email" : "note";
			venture_entity_set_organization_id(VENTURE_ENTITY(interaction), venture_entity_get_organization_id(row));
			g_object_set(interaction, "contact-id", contact, "company-id", company, "deal-id", deal,
				"subject", subject, "body", value, "occurred-at", now, "outbound", TRUE, NULL);
			if (!venture_entity_set_field_from_string(VENTURE_ENTITY(interaction), "kind", kind_name, error) ||
				!venture_database_save(self->database, VENTURE_ENTITY(interaction), actor, error))
				goto done;
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
	ok = venture_database_commit(self->database, error);
	self->busy = FALSE;
	return ok ? g_steal_pointer(&row) : NULL;
done:
	venture_database_rollback(self->database);
	self->busy = FALSE;
	return NULL;
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
