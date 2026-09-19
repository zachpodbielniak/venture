/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
#include "sequences/venture-sequence-service-private.h"
#include "sequences/venture-sequence-tracking-private.h"
#include <string.h>

struct _VentureSequenceService
{
	GObject parent_instance;
	VentureDatabase *database;
	VentureEntity *writing;
	gboolean busy;
	/* Public address the open pixel and wrapped links are composed with. */
	gchar *base_url;
};
G_DEFINE_FINAL_TYPE(VentureSequenceService, venture_sequence_service, G_TYPE_OBJECT)

enum { PROP_0, PROP_DATABASE, PROP_BASE_URL };

static gboolean
refuse(GError **error, VentureError code, const gchar *message)
{
	g_set_error(error, VENTURE_ERROR, code, "VentureSequenceService: %s", message);
	return FALSE;
}

static void
get_property(GObject *object, guint id, GValue *value, GParamSpec *spec)
{
	if (id == PROP_DATABASE)
		g_value_set_object(value, VENTURE_SEQUENCE_SERVICE(object)->database);
	else if (id == PROP_BASE_URL)
		g_value_set_string(value, VENTURE_SEQUENCE_SERVICE(object)->base_url);
	else
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
}

static void
set_property(GObject *object, guint id, const GValue *value, GParamSpec *spec)
{
	VentureSequenceService *self = VENTURE_SEQUENCE_SERVICE(object);
	if (id == PROP_DATABASE)
	{
		self->database = g_value_get_object(value);
		if (self->database != NULL)
			g_object_add_weak_pointer(G_OBJECT(self->database), (gpointer *)&self->database);
	}
	else if (id == PROP_BASE_URL)
	{
		g_free(self->base_url);
		self->base_url = g_value_dup_string(value);
	}
	else
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
}

static void
finalize(GObject *object)
{
	VentureSequenceService *self = VENTURE_SEQUENCE_SERVICE(object);
	if (self->database != NULL)
		g_object_remove_weak_pointer(G_OBJECT(self->database), (gpointer *)&self->database);
	g_clear_pointer(&self->base_url, g_free);
	G_OBJECT_CLASS(venture_sequence_service_parent_class)->finalize(object);
}

static void
venture_sequence_service_class_init(VentureSequenceServiceClass *klass)
{
	GObjectClass *object = G_OBJECT_CLASS(klass);
	object->get_property = get_property;
	object->set_property = set_property;
	object->finalize = finalize;
	g_object_class_install_property(object, PROP_DATABASE,
		g_param_spec_object("database", "Database", "Weak reference to the owning repository",
		VENTURE_TYPE_DATABASE, G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
	/**
	 * VentureSequenceService:base-url:
	 *
	 * The installation's public base URL, without a trailing slash. Empty
	 * means the tracking pixel and wrapped links are written as relative
	 * paths, which only resolve inside the web UI.
	 */
	g_object_class_install_property(object, PROP_BASE_URL,
		g_param_spec_string("base-url", "Base URL", "Public address for tracking links", "",
		G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
}

static void
venture_sequence_service_init(VentureSequenceService *self)
{
	(void)self;
}

static gint64
number(VentureEntity *row, const gchar *field)
{
	gint64 result = 0;
	g_object_get(row, field, &result, NULL);
	return result;
}

static gint
choice(VentureEntity *row, const gchar *field)
{
	gint result = 0;
	g_object_get(row, field, &result, NULL);
	return result;
}

static gboolean
flag(VentureEntity *row, const gchar *field)
{
	gboolean result = FALSE;
	g_object_get(row, field, &result, NULL);
	return result;
}

static gchar *
string(VentureEntity *row, const gchar *field)
{
	gchar *result = NULL;
	g_object_get(row, field, &result, NULL);
	return result;
}

static gchar *
normalized_email(VentureEntity *row)
{
	g_autofree gchar *email = string(row, "email");
	if (email == NULL)
		return g_strdup("");
	return g_ascii_strdown(g_strstrip(email), -1);
}

static gboolean
persist(VentureSequenceService *self, VentureEntity *row, const VentureActor *actor, GError **error)
{
	gboolean ok;
	if (G_OBJECT_TYPE(row) == VENTURE_TYPE_SEQUENCE_ENROLLMENT)
	{
		gint status;
		g_object_get(row, "status", &status, NULL);
		if (status > 1)
			g_object_set(row, "active-key", NULL, NULL);
	}
	/* A single-use permit is consumed before callbacks can re-enter. */
	self->writing = row;
	ok = venture_database_save(self->database, row, actor, error);
	self->writing = NULL;
	return ok;
}

static GPtrArray *
rows(VentureSequenceService *self, GType type, gint64 org,
	const gchar *field, gint64 id, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(type);
	venture_query_set_limit(query, 0);
	if (!venture_query_add_filter_int(query, "organization-id", VENTURE_FILTER_OP_EQ, org, error))
		return NULL;
	if (field != NULL && !venture_query_add_filter_int(query, field, VENTURE_FILTER_OP_EQ, id, error))
		return NULL;
	if (type == VENTURE_TYPE_SEQUENCE_STEP)
		venture_query_add_order(query, "position", VENTURE_SORT_ASCENDING, NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	return venture_database_find(self->database, query, error);
}

static VentureEntity *
reference(VentureSequenceService *self, GType type, gint64 id, gint64 org, GError **error)
{
	VentureEntity *row = venture_database_get(self->database, type, id, error);
	if (row == NULL)
	{
		if (error == NULL || *error == NULL)
			refuse(error, VENTURE_ERROR_NOT_FOUND, "Referenced record does not exist");
		return NULL;
	}
	if (venture_entity_get_organization_id(row) != org)
	{
		g_object_unref(row);
		refuse(error, VENTURE_ERROR_VALIDATION, "References must belong to the same organization");
		return NULL;
	}
	return row;
}

static gboolean
is_suppressed(VentureSequenceService *self, VentureEntity *contact, gboolean *suppressed, GError **error)
{
	g_autofree gchar *email = normalized_email(contact);
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_SUPPRESSION);
	g_autoptr(GPtrArray) found = NULL;
	*suppressed = FALSE;
	venture_query_add_filter_int(query, "organization-id", VENTURE_FILTER_OP_EQ,
		venture_entity_get_organization_id(contact), NULL);
	venture_query_add_filter_string(query, "email", VENTURE_FILTER_OP_EQ, email, NULL);
	found = venture_database_find(self->database, query, error);
	if (found == NULL)
		return FALSE;
	*suppressed = found->len != 0;
	return TRUE;
}

static gboolean
validate_sequence(VentureEntity *row, GError **error)
{
	g_autofree gchar *tz = string(row, "timezone");
	g_autofree gchar *days = string(row, "weekdays");
	g_autoptr(GTimeZone) zone = NULL;
	g_auto(GStrv) tokens = NULL;
	guint i;
	gint64 start = number(row, "send-window-start");
	gint64 end = number(row, "send-window-end");
	if (venture_string_is_empty(tz))
	{
		g_object_set(row, "timezone", "UTC", NULL);
		g_free(tz);
		tz = g_strdup("UTC");
	}
	zone = g_time_zone_new_identifier(tz);
	if (zone == NULL || start < 0 || start > 23 || end < 1 || end > 24 || start >= end)
		return refuse(error, VENTURE_ERROR_VALIDATION, "Use a valid timezone and a nonempty same-day send window (0..24)");
	if (venture_string_is_empty(days))
		return refuse(error, VENTURE_ERROR_VALIDATION, "Weekdays must list ISO days 1..7, separated by commas");
	tokens = g_strsplit(days, ",", -1);
	for (i = 0; tokens[i] != NULL; i++)
		if (strlen(tokens[i]) != 1 || tokens[i][0] < '1' || tokens[i][0] > '7')
			return refuse(error, VENTURE_ERROR_VALIDATION, "Weekdays must list ISO days 1..7, separated by commas");
	return TRUE;
}

static GDateTime *
window_time(VentureEntity *sequence, GDateTime *time, GError **error)
{
	g_autofree gchar *tz = string(sequence, "timezone");
	g_autofree gchar *days = string(sequence, "weekdays");
	g_autoptr(GTimeZone) zone = g_time_zone_new_identifier(tz);
	g_autoptr(GDateTime) local = NULL;
	guint i;
	gint start = (gint)number(sequence, "send-window-start");
	gint end = (gint)number(sequence, "send-window-end");
	if (zone == NULL || time == NULL)
	{
		refuse(error, VENTURE_ERROR_VALIDATION, "Invalid scheduling timezone or timestamp");
		return NULL;
	}
	local = g_date_time_to_timezone(time, zone);
	if (local == NULL)
	{
		refuse(error, VENTURE_ERROR_VALIDATION, "Sending time exceeds the supported date range");
		return NULL;
	}
	for (i = 0; i < 8; i++)
	{
		gint hour = g_date_time_get_hour(local);
		if (strchr(days, '0' + g_date_time_get_day_of_week(local)) != NULL && hour < end)
		{
			if (hour >= start)
				return g_date_time_to_utc(local);
			{
				g_autoptr(GDateTime) open = g_date_time_new(zone,
					g_date_time_get_year(local), g_date_time_get_month(local),
					g_date_time_get_day_of_month(local), start, 0, 0);
				return g_date_time_to_utc(open);
			}
		}
		{
			g_autoptr(GDateTime) tomorrow = g_date_time_add_days(local, 1);
			/* A valid input at year 9999 can still have no next sending day. */
			if (tomorrow == NULL)
			{
				refuse(error, VENTURE_ERROR_VALIDATION, "Next sending day exceeds the supported date range");
				return NULL;
			}
			g_clear_pointer(&local, g_date_time_unref);
			local = g_date_time_new(zone, g_date_time_get_year(tomorrow),
				g_date_time_get_month(tomorrow), g_date_time_get_day_of_month(tomorrow), 0, 0, 0);
		}
	}
	refuse(error, VENTURE_ERROR_VALIDATION, "No available sending day");
	return NULL;
}

static gboolean
schedule(VentureEntity *sequence, VentureEntity *step, VentureEntity *enrollment,
	GDateTime *from, GError **error)
{
	g_autofree gchar *tz = string(sequence, "timezone");
	g_autoptr(GTimeZone) zone = g_time_zone_new_identifier(tz);
	g_autoptr(GDateTime) local = g_date_time_to_timezone(from, zone);
	g_autoptr(GDateTime) days = NULL;
	g_autoptr(GDateTime) delayed = NULL;
	g_autoptr(GDateTime) next = NULL;
	if (local == NULL)
		return refuse(error, VENTURE_ERROR_VALIDATION, "Enrollment time exceeds the supported date range");
	days = g_date_time_add_days(local, (gint)number(step, "delay-days"));
	if (days != NULL)
		delayed = g_date_time_add_hours(days, (gint)number(step, "delay-hours"));
	if (delayed == NULL)
		return refuse(error, VENTURE_ERROR_VALIDATION, "Delay exceeds the supported date range");
	next = window_time(sequence, delayed, error);
	if (next == NULL)
		return FALSE;
	g_object_set(enrollment, "current-step", number(step, "position"), "next-run-at", next, NULL);
	return TRUE;
}

static gboolean
prepare_enrollment(VentureSequenceService *self, VentureEntity *row,
	const VentureActor *actor, GError **error)
{
	gint64 org = venture_entity_get_organization_id(row);
	g_autoptr(VentureEntity) sequence = reference(self, VENTURE_TYPE_SEQUENCE, number(row, "sequence-id"), org, error);
	g_autoptr(VentureEntity) contact = NULL;
	g_autoptr(GPtrArray) enrolled = NULL;
	g_autoptr(GPtrArray) steps = NULL;
	g_autoptr(GDateTime) at = NULL;
	gboolean suppressed;
	guint i;
	if (sequence == NULL)
		return FALSE;
	if (!flag(sequence, "active") || choice(row, "status") != 0 || org <= 0)
		return refuse(error, VENTURE_ERROR_VALIDATION, "Enrollment needs an active sequence and active initial status");
	contact = reference(self, VENTURE_TYPE_CONTACT, number(row, "contact-id"), org, error);
	if (contact == NULL || !is_suppressed(self, contact, &suppressed, error))
		return FALSE;
	if (suppressed)
		return refuse(error, VENTURE_ERROR_VALIDATION, "The contact email is suppressed");
	if (number(row, "deal-id") != 0)
	{
		g_autoptr(VentureEntity) deal = reference(self, VENTURE_TYPE_DEAL, number(row, "deal-id"), org, error);
		if (deal == NULL)
			return FALSE;
		if (number(deal, "contact-id") != venture_entity_get_id(contact))
			return refuse(error, VENTURE_ERROR_VALIDATION, "Deal and enrollment must name the same contact");
		if (choice(deal, "stage") == VENTURE_DEAL_STAGE_WON && flag(sequence, "exit-on-deal-won"))
			return refuse(error, VENTURE_ERROR_VALIDATION, "The deal is already won");
	}
	{
		g_autofree gchar *key = g_strdup_printf("%" G_GINT64_FORMAT ":%" G_GINT64_FORMAT,
			venture_entity_get_id(sequence), venture_entity_get_id(contact));
		g_object_set(row, "active-key", key, NULL);
	}
	enrolled = rows(self, VENTURE_TYPE_SEQUENCE_ENROLLMENT, org, "sequence-id", venture_entity_get_id(sequence), error);
	if (enrolled == NULL)
		return FALSE;
	for (i = 0; i < enrolled->len; i++)
	{
		VentureEntity *other = g_ptr_array_index(enrolled, i);
		if (number(other, "contact-id") == venture_entity_get_id(contact) && choice(other, "status") <= 1)
			return refuse(error, VENTURE_ERROR_ALREADY_EXISTS, "Contact is already enrolled (active or paused)");
	}
	steps = rows(self, VENTURE_TYPE_SEQUENCE_STEP, org, "sequence-id", venture_entity_get_id(sequence), error);
	if (steps == NULL)
		return FALSE;
	g_object_get(row, "enrolled-at", &at, NULL);
	if (at == NULL)
		at = g_date_time_new_now_utc();
	g_object_set(row, "enrolled-at", at, "enrolled-by", actor != NULL && actor->name != NULL ? actor->name : "system", NULL);
	for (i = 0; i < steps->len; i++)
		if (flag(g_ptr_array_index(steps, i), "active"))
			return schedule(sequence, g_ptr_array_index(steps, i), row, at, error);
	return refuse(error, VENTURE_ERROR_VALIDATION, "Sequence has no active steps");
}

static gboolean
cancel_pending(VentureSequenceService *self, VentureEntity *enrollment, const gchar *reason,
	const VentureActor *actor, GError **error)
{
	g_autoptr(GPtrArray) deliveries = rows(self, VENTURE_TYPE_SEQUENCE_DELIVERY,
		venture_entity_get_organization_id(enrollment), "enrollment-id", venture_entity_get_id(enrollment), error);
	guint i;
	if (deliveries == NULL)
		return FALSE;
	for (i = 0; i < deliveries->len; i++)
	{
		VentureEntity *delivery = g_ptr_array_index(deliveries, i);
		if (choice(delivery, "state") == 0)
		{
			g_object_set(delivery, "state", 3, "error", reason, NULL);
			if (!persist(self, delivery, actor, error))
				return FALSE;
		}
	}
	return TRUE;
}

static gboolean
exit_row(VentureSequenceService *self, VentureEntity *row, const gchar *reason,
	gboolean goal, const VentureActor *actor, GError **error)
{
	g_autoptr(GDateTime) now = g_date_time_new_now_utc();
	g_object_set(row, "status", 3, "exit-reason", reason, "next-run-at", NULL, NULL);
	if (goal)
		g_object_set(row, "goal-met-at", now, NULL);
	return cancel_pending(self, row, reason, actor, error) && persist(self, row, actor, error);
}

static gboolean
apply_exits(VentureSequenceService *self, VentureEntity *source,
	VentureEntity *previous, const VentureActor *actor, GError **error)
{
	gint64 org = venture_entity_get_organization_id(source);
	g_autoptr(GPtrArray) enrolled = NULL;
	g_autofree gchar *email = NULL;
	gboolean suppression = G_OBJECT_TYPE(source) == VENTURE_TYPE_SUPPRESSION;
	gboolean reply = G_OBJECT_TYPE(source) == VENTURE_TYPE_INTERACTION && !flag(source, "outbound");
	gboolean meeting = G_OBJECT_TYPE(source) == VENTURE_TYPE_INTERACTION && choice(source, "kind") == VENTURE_INTERACTION_KIND_MEETING;
	gboolean won = G_OBJECT_TYPE(source) == VENTURE_TYPE_DEAL && choice(source, "stage") == VENTURE_DEAL_STAGE_WON;
	guint i;
	if (!suppression && !reply && !meeting && !won)
		return TRUE;
	if (won && previous != NULL && choice(previous, "stage") == VENTURE_DEAL_STAGE_WON)
		return TRUE;
	if (reply && previous != NULL && !flag(previous, "outbound"))
		return TRUE;
	if (suppression)
		email = normalized_email(source);
	enrolled = rows(self, VENTURE_TYPE_SEQUENCE_ENROLLMENT, org, NULL, 0, error);
	if (enrolled == NULL)
		return FALSE;
	for (i = 0; i < enrolled->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(enrolled, i);
		g_autoptr(VentureEntity) sequence = NULL;
		gboolean matches = FALSE;
		gboolean goal = FALSE;
		const gchar *reason = NULL;
		if (choice(row, "status") > 1 && !suppression)
			continue;
		sequence = reference(self, VENTURE_TYPE_SEQUENCE, number(row, "sequence-id"), org, error);
		if (sequence == NULL)
			return FALSE;
		if (suppression)
		{
			g_autoptr(VentureEntity) contact = reference(self, VENTURE_TYPE_CONTACT, number(row, "contact-id"), org, error);
			g_autofree gchar *contact_email = NULL;
			if (contact == NULL)
				return FALSE;
			contact_email = normalized_email(contact);
			matches = g_strcmp0(email, contact_email) == 0;
			reason = "suppressed";
		}
		else if ((reply || meeting) && number(row, "contact-id") == number(source, "contact-id"))
		{
			goal = (reply && choice(sequence, "goal") == 1) ||
				(choice(sequence, "goal") == 2 && choice(source, "kind") == VENTURE_INTERACTION_KIND_MEETING);
			matches = (reply && flag(sequence, "exit-on-reply")) || goal;
			reason = goal && choice(sequence, "goal") == 2 ? "meeting" : "reply";
		}
		else if (won && (number(row, "deal-id") == venture_entity_get_id(source) ||
			(number(row, "deal-id") == 0 && number(row, "contact-id") == number(source, "contact-id"))))
		{
			goal = choice(sequence, "goal") == 3;
			matches = flag(sequence, "exit-on-deal-won") || goal;
			reason = "deal_won";
		}
		if (matches && choice(row, "status") > 1)
		{
			if (!cancel_pending(self, row, reason, actor, error))
				return FALSE;
			continue;
		}
		if (matches && !exit_row(self, row, reason, goal, actor, error))
			return FALSE;
	}
	return TRUE;
}

static gboolean
adapter_update(VentureSequenceService *self, VentureEntity *row, VentureEntity *previous,
	const VentureActor *actor, GError **error)
{
	g_autoptr(JsonNode) diff = NULL;
	g_autoptr(GDateTime) now = NULL;
	g_autofree gchar *message = NULL;
	GList *keys, *item;
	gboolean valid = TRUE;
	if (previous == NULL)
		return refuse(error, VENTURE_ERROR_VALIDATION, "Delivery creation belongs to VentureSequenceService");
	if (venture_entity_get_organization_id(row) != venture_entity_get_organization_id(previous) ||
		g_strcmp0(venture_entity_get_uuid(row), venture_entity_get_uuid(previous)) != 0)
		return refuse(error, VENTURE_ERROR_VALIDATION, "Delivery identity is immutable");
	diff = venture_entity_diff(previous, row);
	if (json_object_get_size(json_node_get_object(diff)) == 0)
		return TRUE;
	keys = json_object_get_members(json_node_get_object(diff));
	for (item = keys; item != NULL; item = item->next)
		if (g_strcmp0(item->data, "state") && g_strcmp0(item->data, "executed_at") &&
			g_strcmp0(item->data, "error") && g_strcmp0(item->data, "external_message_id"))
			valid = FALSE;
	g_list_free(keys);
	if (!valid || choice(previous, "state") != 0 || (choice(row, "state") != 1 && choice(row, "state") != 2))
		return refuse(error, VENTURE_ERROR_VALIDATION, "Adapter may only finish a pending delivery; payload and history are immutable");
	message = string(row, "error");
	if (choice(row, "state") == 2 && venture_string_is_empty(message))
		return refuse(error, VENTURE_ERROR_VALIDATION, "Failed delivery requires an error");
	now = g_date_time_new_now_utc();
	g_object_set(row, "executed-at", now, NULL);
	if (!persist(self, row, actor, error))
		return FALSE;
	if (choice(row, "state") == 2)
	{
		g_autoptr(VentureEntity) enrollment = reference(self, VENTURE_TYPE_SEQUENCE_ENROLLMENT,
			number(row, "enrollment-id"), venture_entity_get_organization_id(row), error);
		if (enrollment == NULL)
			return FALSE;
		if (choice(enrollment, "status") != 3)
		{
			g_object_set(enrollment, "status", 4, "next-run-at", NULL, "exit-reason", "delivery_failed", NULL);
			return cancel_pending(self, enrollment, message, actor, error) && persist(self, enrollment, actor, error);
		}
	}
	return TRUE;
}

static gboolean
process_save(VentureSequenceService *self, VentureEntity *row, const VentureActor *actor,
	VentureSequenceSaveContinuation save, GError **error)
{
	g_autoptr(VentureEntity) previous = NULL;
	GType type = G_OBJECT_TYPE(row);
	if (venture_entity_is_persisted(row))
	{
		previous = venture_database_get(self->database, type, venture_entity_get_id(row), error);
		if (previous == NULL)
			return FALSE;
		if (venture_entity_get_version(previous) != venture_entity_get_version(row))
			return refuse(error, VENTURE_ERROR_CONFLICT, "Record changed since it was read");
	}
	if (previous != NULL && (type == VENTURE_TYPE_SEQUENCE || type == VENTURE_TYPE_SEQUENCE_STEP || type == VENTURE_TYPE_SUPPRESSION) &&
		venture_entity_get_organization_id(previous) != venture_entity_get_organization_id(row))
		return refuse(error, VENTURE_ERROR_VALIDATION, "Sequence organization cannot be changed");
	if (type == VENTURE_TYPE_SEQUENCE_STEP && previous != NULL && number(row, "sequence-id") != number(previous, "sequence-id"))
		return refuse(error, VENTURE_ERROR_VALIDATION, "A saved step cannot move to a different sequence");
	if (type == VENTURE_TYPE_SEQUENCE && !validate_sequence(row, error))
		return FALSE;
	if (type == VENTURE_TYPE_SEQUENCE_STEP)
	{
		gint64 org = venture_entity_get_organization_id(row);
		g_autoptr(VentureEntity) sequence = reference(self, VENTURE_TYPE_SEQUENCE, number(row, "sequence-id"), org, error);
		g_autoptr(GPtrArray) steps = NULL;
		guint i;
		if (sequence == NULL)
			return FALSE;
		if (number(row, "position") < 1 || number(row, "delay-days") < 0 ||
			number(row, "delay-hours") < 0 || number(row, "delay-days") > 36500 || number(row, "delay-hours") > 876000)
			return refuse(error, VENTURE_ERROR_VALIDATION, "Positions must be positive and delays between zero and 100 years");
		steps = rows(self, type, org, "sequence-id", number(row, "sequence-id"), error);
		if (steps == NULL)
			return FALSE;
		for (i = 0; i < steps->len; i++)
		{
			VentureEntity *other = g_ptr_array_index(steps, i);
			if (venture_entity_get_id(other) != venture_entity_get_id(row) && number(other, "position") == number(row, "position"))
				return refuse(error, VENTURE_ERROR_ALREADY_EXISTS, "Sequence step position is already occupied");
		}
	}
	if (type == VENTURE_TYPE_SEQUENCE_ENROLLMENT)
	{
		if (previous != NULL)
			return refuse(error, VENTURE_ERROR_VALIDATION, "Use VentureSequenceService to change an enrollment");
		{
			g_autoptr(GDateTime) goal = NULL;
			g_autoptr(GDateTime) next = NULL;
			g_autofree gchar *reason = string(row, "exit-reason");
			g_object_get(row, "goal-met-at", &goal, "next-run-at", &next, NULL);
			if (goal != NULL || next != NULL || number(row, "current-step") != 0 || !venture_string_is_empty(reason))
				return refuse(error, VENTURE_ERROR_VALIDATION, "Enrollment progress and goal state are derived by VentureSequenceService");
		}
		if (!prepare_enrollment(self, row, actor, error))
			return FALSE;
	}
	if (type == VENTURE_TYPE_SEQUENCE_DELIVERY)
		return adapter_update(self, row, previous, actor, error);
	if (type == VENTURE_TYPE_SEQUENCE_LINK || type == VENTURE_TYPE_SEQUENCE_TRACKING_EVENT)
		return refuse(error, VENTURE_ERROR_VALIDATION, "Tracking links and events are evidence written only by VentureSequenceService");
	if (type == VENTURE_TYPE_SUPPRESSION)
	{
		g_autofree gchar *email = normalized_email(row);
		g_autoptr(GDateTime) at = NULL;
		if (strchr(email, '@') == NULL)
			return refuse(error, VENTURE_ERROR_VALIDATION, "Suppression requires an email address");
		if (previous != NULL)
		{
			g_autofree gchar *old_email = normalized_email(previous);
			if (g_strcmp0(email, old_email) != 0)
				return refuse(error, VENTURE_ERROR_VALIDATION, "Suppression email cannot be rewritten");
		}
		g_object_get(row, "at", &at, NULL);
		if (at == NULL)
			at = g_date_time_new_now_utc();
		g_object_set(row, "email", email, "at", at, NULL);
	}
	/* Deals and interactions have already passed their pipeline/lead hooks.
	 * Re-entering the dispatcher would spend a consumed permit twice. Keep
	 * validators and signals in the ordinary downstream write, within this
	 * sequence transaction. */
	if (type == VENTURE_TYPE_DEAL || type == VENTURE_TYPE_INTERACTION)
		return save(self->database, row, actor, error) && apply_exits(self, row, previous, actor, error);
	return persist(self, row, actor, error) && apply_exits(self, row, previous, actor, error);
}

gboolean
venture_sequences_save_hook(VentureDatabase *database, VentureEntity *record,
	const VentureActor *actor, VentureSequenceSaveContinuation save, gboolean *handled, GError **error)
{
	VentureSequenceService *self;
	g_autoptr(VentureEntity) original = NULL;
	GType type = G_OBJECT_TYPE(record);
	gboolean ok;
	*handled = FALSE;
	if (type != VENTURE_TYPE_SEQUENCE && type != VENTURE_TYPE_SEQUENCE_STEP &&
		type != VENTURE_TYPE_SEQUENCE_ENROLLMENT && type != VENTURE_TYPE_SEQUENCE_DELIVERY &&
		type != VENTURE_TYPE_SUPPRESSION && type != VENTURE_TYPE_INTERACTION && type != VENTURE_TYPE_DEAL &&
		type != VENTURE_TYPE_SEQUENCE_LINK && type != VENTURE_TYPE_SEQUENCE_TRACKING_EVENT)
		return TRUE;
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "sequence") == G_TYPE_INVALID)
		return TRUE;
	self = venture_sequence_service_get(database);
	if (self->writing == record)
	{
		self->writing = NULL;
		return TRUE;
	}
	*handled = TRUE;
	if (self->busy)
		return refuse(error, VENTURE_ERROR_CONFLICT, "Reentrant sequence transition refused");
	if (!venture_database_begin(database, error))
		return FALSE;
	original = g_object_new(type, NULL);
	venture_entity_copy_properties_from(original, record, FALSE);
	self->busy = TRUE;
	ok = process_save(self, record, actor, save, error);
	self->busy = FALSE;
	if (!ok)
	{
		venture_database_rollback(database);
		venture_entity_copy_properties_from(record, original, FALSE);
		return FALSE;
	}
	return venture_database_commit(database, error);
}

gboolean
venture_sequence_service_enroll(VentureSequenceService *self, VentureEntity *enrollment,
	const VentureActor *actor, GError **error)
{
	g_return_val_if_fail(VENTURE_IS_SEQUENCE_SERVICE(self), FALSE);
	if (self->database == NULL || G_OBJECT_TYPE(enrollment) != VENTURE_TYPE_SEQUENCE_ENROLLMENT ||
		venture_entity_is_persisted(enrollment))
		return refuse(error, VENTURE_ERROR_INVALID_ARGUMENT, "Supply an unsaved enrollment to a live service");
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "sequence") == G_TYPE_INVALID)
		return refuse(error, VENTURE_ERROR_NOT_FOUND, "The sequences module is disabled");
	return venture_database_save(self->database, enrollment, actor, error);
}

gboolean
venture_sequences_check_removal(VentureEntity *record, GError **error)
{
	GType type = G_OBJECT_TYPE(record);
	if (type == VENTURE_TYPE_SEQUENCE_ENROLLMENT || type == VENTURE_TYPE_SEQUENCE_DELIVERY || type == VENTURE_TYPE_SUPPRESSION ||
		type == VENTURE_TYPE_SEQUENCE_LINK || type == VENTURE_TYPE_SEQUENCE_TRACKING_EVENT)
		return refuse(error, VENTURE_ERROR_VALIDATION, "Sequence history and suppression cannot be removed");
	return TRUE;
}

static gboolean
available(VentureSequenceService *self, gint64 org, GError **error)
{
	if (self->database == NULL || org <= 0)
		return refuse(error, VENTURE_ERROR_INVALID_ARGUMENT, "A live database and one organization are required");
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "sequence") == G_TYPE_INVALID)
		return refuse(error, VENTURE_ERROR_NOT_FOUND, "The sequences module is disabled");
	if (self->busy)
		return refuse(error, VENTURE_ERROR_CONFLICT, "Reentrant sequence transition refused");
	return TRUE;
}

gboolean
venture_sequence_service_transition(VentureSequenceService *self, gint64 organization_id,
	gint64 enrollment_id, const gchar *action, const gchar *reason,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) row = NULL;
	gboolean ok = FALSE;
	gint state;
	g_return_val_if_fail(VENTURE_IS_SEQUENCE_SERVICE(self), FALSE);
	if (!available(self, organization_id, error) || !venture_database_begin(self->database, error))
		return FALSE;
	self->busy = TRUE;
	row = reference(self, VENTURE_TYPE_SEQUENCE_ENROLLMENT, enrollment_id, organization_id, error);
	if (row == NULL)
		goto out;
	state = choice(row, "status");
	if (g_strcmp0(action, "pause") == 0 && (state == 0 || state == 1))
	{
		g_object_set(row, "status", 1, NULL);
		ok = persist(self, row, actor, error);
	}
	else if (g_strcmp0(action, "resume") == 0 && (state == 0 || state == 1))
	{
		g_autoptr(VentureEntity) contact = reference(self, VENTURE_TYPE_CONTACT, number(row, "contact-id"), organization_id, error);
		gboolean suppressed;
		if (contact == NULL || !is_suppressed(self, contact, &suppressed, error))
			goto out;
		if (suppressed)
		{
			refuse(error, VENTURE_ERROR_VALIDATION, "The contact email is suppressed");
			goto out;
		}
		g_object_set(row, "status", 0, NULL);
		ok = persist(self, row, actor, error);
	}
	else if ((g_strcmp0(action, "exit") == 0 || g_strcmp0(action, "goal") == 0) && state <= 1)
		ok = exit_row(self, row, venture_string_is_empty(reason) ? action : reason,
			g_strcmp0(action, "goal") == 0, actor, error);
	else
		refuse(error, VENTURE_ERROR_CONFLICT, "This enrollment cannot perform that transition");
out:
	self->busy = FALSE;
	if (!ok)
	{
		venture_database_rollback(self->database);
		return FALSE;
	}
	return venture_database_commit(self->database, error);
}

static gchar *
render(const gchar *template, JsonObject *contact, JsonObject *company,
	JsonObject *deal, GError **error)
{
	g_autoptr(GString) output = g_string_new(NULL);
	const gchar *cursor = template != NULL ? template : "";
	while (*cursor != '\0')
	{
		const gchar *open = strchr(cursor, '{');
		const gchar *close;
		g_autofree gchar *key = NULL;
		JsonObject *object = contact;
		JsonNode *value;
		gchar *field;
		if (open == NULL)
		{
			g_string_append(output, cursor);
			break;
		}
		g_string_append_len(output, cursor, open - cursor);
		close = strchr(open + 1, '}');
		if (close == NULL)
		{
			refuse(error, VENTURE_ERROR_VALIDATION, "Unclosed template placeholder");
			return NULL;
		}
		key = g_strndup(open + 1, close - open - 1);
		field = strchr(key, '.');
		if (field != NULL)
		{
			*field++ = '\0';
			if (g_strcmp0(key, "company") == 0)
				object = company;
			else if (g_strcmp0(key, "deal") == 0)
				object = deal;
			else if (g_strcmp0(key, "contact") != 0)
				object = NULL;
		}
		else
			field = key;
		g_strdelimit(field, "-", '_');
		if (object == NULL || !json_object_has_member(object, field))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
				"VentureSequenceService: Missing or unavailable placeholder {%.*s}", (gint)(close - open - 1), open + 1);
			return NULL;
		}
		value = json_object_get_member(object, field);
		if (JSON_NODE_HOLDS_VALUE(value) && json_node_get_value_type(value) == G_TYPE_STRING)
			g_string_append(output, json_node_get_string(value));
		else if (!JSON_NODE_HOLDS_NULL(value))
		{
			g_autofree gchar *text = venture_json_to_string(value, FALSE);
			g_string_append(output, text);
		}
		cursor = close + 1;
	}
	return g_string_free(g_steal_pointer(&output), FALSE);
}

static gboolean
already_delivered(GPtrArray *history, VentureEntity *step)
{
	guint i;
	for (i = 0; i < history->len; i++)
		if (number(g_ptr_array_index(history, i), "step-id") == venture_entity_get_id(step))
			return TRUE;
	return FALSE;
}

static VentureEntity *
new_delivery(VentureEntity *row, VentureEntity *step, GDateTime *scheduled, GDateTime *executed)
{
	g_autofree gchar *key = g_strdup_printf("%" G_GINT64_FORMAT ":%" G_GINT64_FORMAT,
		venture_entity_get_id(row), venture_entity_get_id(step));
	return g_object_new(VENTURE_TYPE_SEQUENCE_DELIVERY,
		"organization-id", venture_entity_get_organization_id(row),
		"enrollment-id", venture_entity_get_id(row), "step-id", venture_entity_get_id(step),
		"channel", choice(step, "channel"), "delivery-key", key,
		"scheduled-at", scheduled, "executed-at", executed, NULL);
}

/* Tracking needs both the sequence's switch and the organization's consent. */
static gboolean
tracking_allowed(VentureSequenceService *self, VentureEntity *sequence, gboolean *allowed, GError **error)
{
	g_autoptr(VentureEntity) organization = NULL;
	*allowed = FALSE;
	if (!flag(sequence, "tracking"))
		return TRUE;
	organization = venture_database_get(self->database, VENTURE_TYPE_ORGANIZATION,
		venture_entity_get_organization_id(sequence), error);
	/* No organization row means nobody consented: tracking stays off. */
	if (organization == NULL)
		return error == NULL || *error == NULL;
	*allowed = flag(organization, "sequence-tracking");
	return TRUE;
}

static gboolean
execute_step(VentureSequenceService *self, VentureEntity *row, VentureEntity *sequence, VentureEntity *step,
	GDateTime *scheduled, GDateTime *as_of, const VentureActor *actor, gboolean *failed, GError **error)
{
	g_autoptr(VentureEntity) delivery = new_delivery(row, step, scheduled, as_of);
	g_autoptr(GPtrArray) urls = g_ptr_array_new_with_free_func(g_free);
	g_autofree gchar *token = NULL;
	g_autoptr(VentureEntity) contact = NULL;
	g_autoptr(VentureEntity) company = NULL;
	g_autoptr(VentureEntity) deal = NULL;
	g_autoptr(JsonNode) contact_json = NULL;
	g_autoptr(JsonNode) company_json = NULL;
	g_autoptr(JsonNode) deal_json = NULL;
	g_autoptr(GError) render_error = NULL;
	g_autofree gchar *subject_template = string(step, "subject");
	g_autofree gchar *body_template = string(step, "body");
	g_autofree gchar *subject = NULL;
	g_autofree gchar *body = NULL;
	gint64 org = venture_entity_get_organization_id(row);
	gint channel = choice(step, "channel");
	*failed = FALSE;
	contact = reference(self, VENTURE_TYPE_CONTACT, number(row, "contact-id"), org, error);
	if (contact == NULL)
		return FALSE;
	contact_json = venture_serializable_to_json(VENTURE_SERIALIZABLE(contact), FALSE);
	if (number(contact, "company-id") != 0)
	{
		company = reference(self, VENTURE_TYPE_COMPANY, number(contact, "company-id"), org, error);
		if (company == NULL)
			return FALSE;
		company_json = venture_serializable_to_json(VENTURE_SERIALIZABLE(company), FALSE);
	}
	if (number(row, "deal-id") != 0)
	{
		deal = reference(self, VENTURE_TYPE_DEAL, number(row, "deal-id"), org, error);
		if (deal == NULL)
			return FALSE;
		deal_json = venture_serializable_to_json(VENTURE_SERIALIZABLE(deal), FALSE);
	}
	subject = render(subject_template, json_node_get_object(contact_json),
		company_json != NULL ? json_node_get_object(company_json) : NULL,
		deal_json != NULL ? json_node_get_object(deal_json) : NULL, &render_error);
	if (subject != NULL)
		body = render(body_template, json_node_get_object(contact_json),
			company_json != NULL ? json_node_get_object(company_json) : NULL,
			deal_json != NULL ? json_node_get_object(deal_json) : NULL, &render_error);
	if (render_error == NULL && channel == 0)
	{
		g_autofree gchar *email = normalized_email(contact);
		if (strchr(email, '@') == NULL)
			refuse(&render_error, VENTURE_ERROR_VALIDATION, "Email step requires a contact email address");
	}
	if (render_error == NULL && (channel == 1 || channel == 2))
	{
		g_autoptr(VentureEntity) owner = NULL;
		owner = reference(self, VENTURE_TYPE_USER, number(row, "owner-id"), org, &render_error);
		if (owner != NULL)
		{
			g_autoptr(VentureEntity) notification = g_object_new(VENTURE_TYPE_NOTIFICATION,
				"organization-id", org, "user-id", venture_entity_get_id(owner),
				"title", venture_string_is_empty(subject) ? (channel == 1 ? "Call contact" : "Send SMS") : subject,
				"body", body, "target-type", "sequence_enrollment",
				"target-id", venture_entity_get_id(row), "occurred-at", as_of, NULL);
			if (!venture_database_save(self->database, notification, actor, error))
				return FALSE;
		}
	}
	if (render_error != NULL)
	{
		*failed = TRUE;
		g_object_set(delivery, "state", 2, "error", render_error->message, NULL);
	}
	else
	{
		g_object_set(delivery, "subject", subject, "body", body, "state", channel == 0 ? 0 : (channel == 3 ? 3 : 1), NULL);
		if (channel == 3)
			g_object_set(delivery, "error", "Wait completed; no delivery required", NULL);
		if (channel == 0)
		{
			gboolean allowed;
			g_object_set(delivery, "executed-at", NULL, NULL);
			if (!tracking_allowed(self, sequence, &allowed, error))
				return FALSE;
			if (allowed)
			{
				g_autofree gchar *wrapped = NULL;
				token = venture_sequence_tracking_token(error);
				if (token == NULL)
					return FALSE;
				wrapped = venture_sequence_tracking_wrap(body, self->base_url, token, urls);
				g_object_set(delivery, "body", wrapped, "tracking-token", token, NULL);
			}
		}
	}
	if (!persist(self, delivery, actor, error))
		return FALSE;
	{
		guint i;
		for (i = 0; i < urls->len; i++)
		{
			g_autoptr(VentureEntity) link = g_object_new(VENTURE_TYPE_SEQUENCE_LINK,
				"organization-id", org, "delivery-id", venture_entity_get_id(delivery),
				"position", (gint64)(i + 1), "url", g_ptr_array_index(urls, i), NULL);
			if (!persist(self, link, actor, error))
				return FALSE;
		}
	}
	return TRUE;
}

static gint
run_one(VentureSequenceService *self, gint64 org, gint64 id, GDateTime *as_of,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) row = reference(self, VENTURE_TYPE_SEQUENCE_ENROLLMENT, id, org, error);
	g_autoptr(VentureEntity) sequence = NULL;
	g_autoptr(VentureEntity) contact = NULL;
	g_autoptr(GDateTime) scheduled = NULL;
	g_autoptr(GDateTime) window = NULL;
	g_autoptr(GPtrArray) steps = NULL;
	g_autoptr(GPtrArray) history = NULL;
	VentureEntity *selected = NULL;
	guint i, selected_index = 0;
	gboolean suppressed, failed;
	if (row == NULL)
		return -1;
	g_object_get(row, "next-run-at", &scheduled, NULL);
	if (choice(row, "status") != 0 || scheduled == NULL || g_date_time_compare(scheduled, as_of) > 0)
		return 0;
	sequence = reference(self, VENTURE_TYPE_SEQUENCE, number(row, "sequence-id"), org, error);
	if (sequence == NULL)
		return -1;
	/* get() retains soft-deleted records for history; deletion must still
	 * stop a previously enrolled journey before any message is queued. */
	if (venture_entity_is_deleted(sequence))
		return exit_row(self, row, "sequence_deleted", FALSE, actor, error) ? 0 : -1;
	if (!flag(sequence, "active"))
		return 0;
	contact = reference(self, VENTURE_TYPE_CONTACT, number(row, "contact-id"), org, error);
	if (contact == NULL || !is_suppressed(self, contact, &suppressed, error))
		return -1;
	if (venture_entity_is_deleted(contact))
		return exit_row(self, row, "contact_deleted", FALSE, actor, error) ? 0 : -1;
	if (suppressed)
		return exit_row(self, row, "suppressed", FALSE, actor, error) ? 0 : -1;
	window = window_time(sequence, as_of, error);
	if (window == NULL)
		return -1;
	if (g_date_time_compare(window, as_of) > 0)
	{
		g_object_set(row, "next-run-at", window, NULL);
		return persist(self, row, actor, error) ? 0 : -1;
	}
	steps = rows(self, VENTURE_TYPE_SEQUENCE_STEP, org, "sequence-id", venture_entity_get_id(sequence), error);
	if (steps == NULL)
		return -1;
	history = rows(self, VENTURE_TYPE_SEQUENCE_DELIVERY, org, "enrollment-id", id, error);
	if (history == NULL)
		return -1;
	for (i = 0; i < steps->len; i++)
	{
		VentureEntity *step = g_ptr_array_index(steps, i);
		if (!flag(step, "active") || already_delivered(history, step))
			continue;
		if (number(step, "position") < number(row, "current-step"))
		{
			g_autoptr(VentureEntity) skipped = new_delivery(row, step, scheduled, as_of);
			g_object_set(skipped, "state", 3, "error", "Inserted before the enrollment's current step", NULL);
			if (!persist(self, skipped, actor, error))
				return -1;
		}
		else if (selected == NULL)
		{
			selected = step;
			selected_index = i;
		}
	}
	if (selected == NULL)
	{
		g_object_set(row, "status", 2, "next-run-at", NULL, NULL);
		return persist(self, row, actor, error) ? 0 : -1;
	}
	if (!execute_step(self, row, sequence, selected, scheduled, as_of, actor, &failed, error))
		return -1;
	g_object_set(row, "current-step", number(selected, "position"), NULL);
	if (failed)
	{
		g_object_set(row, "status", 4, "next-run-at", NULL, "exit-reason", "step_failed", NULL);
		return persist(self, row, actor, error) ? 1 : -1;
	}
	for (i = selected_index + 1; i < steps->len; i++)
	{
		VentureEntity *next = g_ptr_array_index(steps, i);
		if (flag(next, "active") && !already_delivered(history, next))
			return schedule(sequence, next, row, as_of, error) && persist(self, row, actor, error) ? 1 : -1;
	}
	g_object_set(row, "status", 2, "next-run-at", NULL, NULL);
	return persist(self, row, actor, error) ? 1 : -1;
}

gint
venture_sequence_service_run_due(VentureSequenceService *self, gint64 organization_id,
	GDateTime *as_of, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) due = NULL;
	g_autofree gchar *cutoff = NULL;
	guint i;
	gint processed = 0;
	g_return_val_if_fail(VENTURE_IS_SEQUENCE_SERVICE(self), -1);
	if (!available(self, organization_id, error))
		return -1;
	if (as_of == NULL)
	{
		refuse(error, VENTURE_ERROR_INVALID_ARGUMENT, "An execution timestamp is required");
		return -1;
	}
	query = venture_query_new(VENTURE_TYPE_SEQUENCE_ENROLLMENT);
	venture_query_set_limit(query, 1000);
	venture_query_add_filter_int(query, "organization-id", VENTURE_FILTER_OP_EQ, organization_id, NULL);
	venture_query_add_filter_string(query, "status", VENTURE_FILTER_OP_EQ, "active", NULL);
	cutoff = venture_time_to_string(as_of);
	venture_query_add_filter_string(query, "next-run-at", VENTURE_FILTER_OP_LTE, cutoff, NULL);
	venture_query_add_order(query, "next-run-at", VENTURE_SORT_ASCENDING, NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	due = venture_database_find(self->database, query, error);
	if (due == NULL)
		return -1;
	for (i = 0; i < due->len; i++)
	{
		gint count;
		if (!venture_database_begin(self->database, error))
			return -1;
		self->busy = TRUE;
		count = run_one(self, organization_id, venture_entity_get_id(g_ptr_array_index(due, i)), as_of, actor, error);
		self->busy = FALSE;
		if (count < 0)
		{
			venture_database_rollback(self->database);
			return -1;
		}
		if (!venture_database_commit(self->database, error))
			return -1;
		processed += count;
	}
	return processed;
}

static VentureEntity *
delivery_by_token(VentureSequenceService *self, const gchar *token, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_SEQUENCE_DELIVERY);
	VentureEntity *delivery;
	if (venture_string_is_empty(token) || strlen(token) != 64)
	{
		refuse(error, VENTURE_ERROR_NOT_FOUND, "Unknown tracking token");
		return NULL;
	}
	venture_query_add_filter_string(query, "tracking-token", VENTURE_FILTER_OP_EQ, token, NULL);
	delivery = venture_database_find_one(self->database, query, error);
	if (delivery == NULL && (error == NULL || *error == NULL))
		refuse(error, VENTURE_ERROR_NOT_FOUND, "Unknown tracking token");
	return delivery;
}

/* One tracking event plus its timeline interaction. The interaction is
 * outbound so it never counts as a reply and never exits an enrollment. */
static gboolean
record_hit(VentureSequenceService *self, VentureEntity *delivery, gint kind, gint64 position,
	const gchar *url, GDateTime *now, const VentureActor *actor, GError **error)
{
	gint64 org = venture_entity_get_organization_id(delivery);
	g_autoptr(VentureEntity) enrollment = reference(self, VENTURE_TYPE_SEQUENCE_ENROLLMENT, number(delivery, "enrollment-id"), org, error);
	g_autoptr(VentureEntity) contact = NULL;
	g_autoptr(VentureEntity) event = NULL;
	g_autoptr(VentureEntity) interaction = NULL;
	g_autofree gchar *key = NULL;
	g_autofree gchar *subject = string(delivery, "subject");
	g_autofree gchar *title = NULL;
	if (enrollment == NULL)
		return FALSE;
	contact = reference(self, VENTURE_TYPE_CONTACT, number(enrollment, "contact-id"), org, error);
	if (contact == NULL)
		return FALSE;
	if (kind == 0)
	{
		g_autofree gchar *day = g_date_time_format(now, "%Y-%m-%d");
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_SEQUENCE_TRACKING_EVENT);
		g_autoptr(VentureEntity) existing = NULL;
		key = g_strdup_printf("o:%" G_GINT64_FORMAT ":%s", venture_entity_get_id(delivery), day);
		venture_query_add_filter_int(query, "organization-id", VENTURE_FILTER_OP_EQ, org, NULL);
		venture_query_add_filter_string(query, "dedupe-key", VENTURE_FILTER_OP_EQ, key, NULL);
		existing = venture_database_find_one(self->database, query, error);
		if (existing != NULL)
			return TRUE;
		if (error != NULL && *error != NULL)
			return FALSE;
	}
	event = g_object_new(VENTURE_TYPE_SEQUENCE_TRACKING_EVENT, "organization-id", org,
		"delivery-id", venture_entity_get_id(delivery), "enrollment-id", venture_entity_get_id(enrollment),
		"step-id", number(delivery, "step-id"), "contact-id", venture_entity_get_id(contact),
		"kind", kind, "link-position", position, "occurred-at", now, "dedupe-key", key, NULL);
	if (!persist(self, event, actor, error))
		return FALSE;
	title = kind == 0 ? g_strdup_printf("Opened: %s", subject != NULL ? subject : "")
		: g_strdup_printf("Clicked link %" G_GINT64_FORMAT ": %s", position, url != NULL ? url : "");
	interaction = g_object_new(VENTURE_TYPE_INTERACTION, "organization-id", org,
		"contact-id", venture_entity_get_id(contact), "company-id", number(contact, "company-id"),
		"deal-id", number(enrollment, "deal-id"), "kind", VENTURE_INTERACTION_KIND_OUTREACH,
		"outbound", TRUE, "subject", title, "body", url, "occurred-at", now, NULL);
	return persist(self, interaction, actor, error);
}

static gboolean
tracking_ready(VentureSequenceService *self, GError **error)
{
	if (self->database == NULL)
		return refuse(error, VENTURE_ERROR_INVALID_ARGUMENT, "A live database is required");
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "sequence") == G_TYPE_INVALID)
		return refuse(error, VENTURE_ERROR_NOT_FOUND, "The sequences module is disabled");
	if (self->busy)
		return refuse(error, VENTURE_ERROR_CONFLICT, "Reentrant sequence transition refused");
	return TRUE;
}

gboolean
venture_sequence_service_record_open(VentureSequenceService *self, const gchar *token,
	GDateTime *now, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) delivery = NULL;
	gboolean ok;
	g_return_val_if_fail(VENTURE_IS_SEQUENCE_SERVICE(self), FALSE);
	g_return_val_if_fail(now != NULL, FALSE);
	if (!tracking_ready(self, error) || !venture_database_begin(self->database, error))
		return FALSE;
	self->busy = TRUE;
	delivery = delivery_by_token(self, token, error);
	ok = delivery != NULL && record_hit(self, delivery, 0, 0, NULL, now, actor, error);
	self->busy = FALSE;
	if (!ok)
	{
		venture_database_rollback(self->database);
		return FALSE;
	}
	return venture_database_commit(self->database, error);
}

gchar *
venture_sequence_service_record_click(VentureSequenceService *self, const gchar *token,
	gint64 position, GDateTime *now, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) delivery = NULL;
	g_autoptr(VentureEntity) link = NULL;
	g_autofree gchar *url = NULL;
	gboolean ok = FALSE;
	g_return_val_if_fail(VENTURE_IS_SEQUENCE_SERVICE(self), NULL);
	g_return_val_if_fail(now != NULL, NULL);
	if (!tracking_ready(self, error) || !venture_database_begin(self->database, error))
		return NULL;
	self->busy = TRUE;
	delivery = delivery_by_token(self, token, error);
	if (delivery != NULL)
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_SEQUENCE_LINK);
		venture_query_add_filter_int(query, "organization-id", VENTURE_FILTER_OP_EQ, venture_entity_get_organization_id(delivery), NULL);
		venture_query_add_filter_int(query, "delivery-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(delivery), NULL);
		venture_query_add_filter_int(query, "position", VENTURE_FILTER_OP_EQ, position, NULL);
		link = venture_database_find_one(self->database, query, error);
		if (link == NULL && (error == NULL || *error == NULL))
			refuse(error, VENTURE_ERROR_NOT_FOUND, "Unknown tracked link");
	}
	if (link != NULL)
	{
		url = string(link, "url");
		ok = record_hit(self, delivery, 1, position, url, now, actor, error);
	}
	self->busy = FALSE;
	if (!ok)
	{
		venture_database_rollback(self->database);
		return NULL;
	}
	if (!venture_database_commit(self->database, error))
		return NULL;
	return g_steal_pointer(&url);
}
