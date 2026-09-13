/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
#include <string.h>

struct _VentureSequenceService
{
	GObject parent_instance;
	VentureDatabase *database;
	VentureEntity *writing;
	gboolean busy;
};
G_DEFINE_FINAL_TYPE(VentureSequenceService, venture_sequence_service, G_TYPE_OBJECT)

enum { PROP_0, PROP_DATABASE };

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
	else
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
}

static void
finalize(GObject *object)
{
	VentureSequenceService *self = VENTURE_SEQUENCE_SERVICE(object);
	if (self->database != NULL)
		g_object_remove_weak_pointer(G_OBJECT(self->database), (gpointer *)&self->database);
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
	return g_ascii_strdown(g_strstrip(email != NULL ? email : (email = g_strdup(""))), -1);
}

static gboolean
persist(VentureSequenceService *self, VentureEntity *row, const VentureActor *actor, GError **error)
{
	gboolean ok;
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
		return NULL;
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
	gboolean won = G_OBJECT_TYPE(source) == VENTURE_TYPE_DEAL && choice(source, "stage") == VENTURE_DEAL_STAGE_WON;
	guint i;
	if (!suppression && !reply && !won)
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
		else if (reply && number(row, "contact-id") == number(source, "contact-id"))
		{
			goal = choice(sequence, "goal") == 1 ||
				(choice(sequence, "goal") == 2 && choice(source, "kind") == VENTURE_INTERACTION_KIND_MEETING);
			matches = flag(sequence, "exit-on-reply") || goal;
			reason = goal && choice(sequence, "goal") == 2 ? "meeting" : "reply";
		}
		else if (won && (number(row, "deal-id") == venture_entity_get_id(source) ||
			(number(row, "deal-id") == 0 && number(row, "contact-id") == number(source, "contact-id"))))
		{
			goal = choice(sequence, "goal") == 3;
			matches = flag(sequence, "exit-on-deal-won") || goal;
			reason = "deal_won";
		}
		if (matches && !exit_row(self, row, reason, goal, actor, error))
			return FALSE;
	}
	return TRUE;
}

static gboolean
process_save(VentureSequenceService *self, VentureEntity *row, const VentureActor *actor, GError **error)
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
		if (!prepare_enrollment(self, row, actor, error))
			return FALSE;
	}
	if (type == VENTURE_TYPE_SEQUENCE_DELIVERY)
		return refuse(error, VENTURE_ERROR_VALIDATION, "Delivery creation belongs to VentureSequenceService");
	if (type == VENTURE_TYPE_SUPPRESSION)
	{
		g_autofree gchar *email = normalized_email(row);
		g_autoptr(GDateTime) at = NULL;
		if (strchr(email, '@') == NULL)
			return refuse(error, VENTURE_ERROR_VALIDATION, "Suppression requires an email address");
		g_object_get(row, "at", &at, NULL);
		if (at == NULL)
			at = g_date_time_new_now_utc();
		g_object_set(row, "email", email, "at", at, NULL);
	}
	return persist(self, row, actor, error) && apply_exits(self, row, previous, actor, error);
}

gboolean
venture_sequences_save_hook(VentureDatabase *database, VentureEntity *record,
	const VentureActor *actor, gboolean *handled, GError **error)
{
	VentureSequenceService *self;
	GType type = G_OBJECT_TYPE(record);
	gboolean ok;
	*handled = FALSE;
	if (type != VENTURE_TYPE_SEQUENCE && type != VENTURE_TYPE_SEQUENCE_STEP &&
		type != VENTURE_TYPE_SEQUENCE_ENROLLMENT && type != VENTURE_TYPE_SEQUENCE_DELIVERY &&
		type != VENTURE_TYPE_SUPPRESSION && type != VENTURE_TYPE_INTERACTION && type != VENTURE_TYPE_DEAL)
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
	self->busy = TRUE;
	ok = process_save(self, record, actor, error);
	self->busy = FALSE;
	if (!ok)
	{
		venture_database_rollback(database);
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
	if (type == VENTURE_TYPE_SEQUENCE_ENROLLMENT || type == VENTURE_TYPE_SEQUENCE_DELIVERY || type == VENTURE_TYPE_SUPPRESSION)
		return refuse(error, VENTURE_ERROR_VALIDATION, "Sequence history and suppression cannot be removed");
	return TRUE;
}
