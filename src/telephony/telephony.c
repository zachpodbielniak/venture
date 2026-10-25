/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * The business line. A simulated call, or a trunk adapter in this process,
 * speaks through venture_telephony_service_voice_turn(). There is no SIP
 * socket here: the trunk is whatever hands these turns in. Fakes only.
 */
#include "telephony/telephony.h"
#include <string.h>

static const VentureFieldDecl voice_fields[] = {
	VENTURE_FIELD("mode", "Answer", "off, always, or after-hours", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_TEXT("services", "Services", "What the line may describe"),
	VENTURE_FIELD("service-area", "Service area", "Where the business will go", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("booking-rules", "Booking rules", "When a job may be promised")
};
VENTURE_DEFINE_ENTITY_WITH_CODE(VentureVoiceDesk, venture_voice_desk, voice_fields,
	venture_entity_class_set_labels(VENTURE_ENTITY_CLASS(klass), "Voice desk", "Voice desks");)

typedef struct {
	gchar *number;
	GString *transcript;
	gboolean disclosed;
	gboolean failed;
	gboolean urgent_told;
	gchar *summary;
} VoiceCall;

static GHashTable *calls;
static GMutex calls_lock;

static void
voice_call_free(gpointer data)
{
	VoiceCall *call = data;
	g_free(call->number);
	g_string_free(call->transcript, TRUE);
	g_free(call->summary);
	g_free(call);
}

static void
voice_ensure(void)
{
	static gsize once = 0;
	if (g_once_init_enter(&once)) {
		g_autoptr(GError) error = NULL;
		venture_data_class_declare_type(venture_voice_desk_get_type(), VENTURE_DATA_CLASS_TENANT);
		venture_entity_registry_register(venture_entity_registry_get_default(), venture_voice_desk_get_type(), &error);
		calls = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, voice_call_free);
		g_mutex_init(&calls_lock);
		if (error != NULL)
			g_error("Cannot register the voice desk: %s", error->message);
		g_once_init_leave(&once, 1);
	}
}

static void voice_register(void) __attribute__((constructor));
static void
voice_register(void)
{
	voice_ensure();
}

static gchar *
call_key(gint64 org, const gchar *call_id)
{
	return g_strdup_printf("%" G_GINT64_FORMAT ":%s", org, call_id);
}

static VentureEntity *
latest_desk(VentureDatabase *database, gint64 org, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_VOICE_DESK);
	venture_query_set_organization(query, org);
	venture_query_add_order(query, "id", VENTURE_SORT_DESCENDING, NULL);
	venture_query_set_limit(query, 1);
	return venture_database_find_one(database, query, error);
}

static gboolean
answering(VentureEntity *desk, VentureEntity *org, GDateTime *now, GError **error)
{
	g_autofree gchar *mode = NULL;
	g_autofree gchar *hours = NULL;
	g_autofree gchar *zone_name = NULL;
	g_autoptr(GTimeZone) zone = NULL;
	gboolean open = FALSE;
	g_object_get(desk, "mode", &mode, NULL);
	if (g_strcmp0(mode, "always") == 0)
		return TRUE;
	if (g_strcmp0(mode, "after-hours") != 0)
		return FALSE;
	g_object_get(org, "business-hours", &hours, "timezone", &zone_name, NULL);
	zone = g_time_zone_new_identifier(zone_name && *zone_name ? zone_name : "UTC");
	if (zone == NULL)
		zone = g_time_zone_new_utc();
	if (!venture_ai_messages_business_hours_open(hours, zone, now, &open, error))
		return FALSE;
	return !open;
}

static void
tell_owners(VentureDatabase *database, gint64 org, const gchar *title, const gchar *body)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_USER);
	g_autoptr(GPtrArray) users = NULL;
	g_autoptr(GError) error = NULL;
	guint i;
	venture_query_set_limit(query, 0);
	users = venture_database_find(database, query, &error);
	if (users == NULL)
		return;
	for (i = 0; i < users->len; i++) {
		VentureEntity *user = g_ptr_array_index(users, i);
		VentureUserRole role = VENTURE_USER_ROLE_VIEWER;
		gboolean active = FALSE;
		g_autoptr(VentureEntity) note = NULL;
		g_autoptr(GDateTime) stamp = venture_time_now();
		g_object_get(user, "role", &role, "active", &active, NULL);
		if (!active || role != VENTURE_USER_ROLE_OWNER)
			continue;
		note = VENTURE_ENTITY(venture_notification_new());
		g_object_set(note, "user-id", venture_entity_get_id(user), "kind", VENTURE_NOTIFICATION_KIND_SYSTEM,
			"title", title, "body", body, "occurred-at", stamp, NULL);
		venture_entity_set_organization_id(note, org);
		venture_database_save(database, note, NULL, NULL);
	}
}

static gboolean
window_overlaps(VentureDatabase *database, gint64 org, GDateTime *start, GDateTime *end, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_JOB);
	g_autoptr(GPtrArray) jobs = NULL;
	guint i;
	venture_query_set_organization(query, org);
	venture_query_set_limit(query, 0);
	jobs = venture_database_find(database, query, error);
	if (jobs == NULL)
		return TRUE;
	for (i = 0; i < jobs->len; i++) {
		g_autoptr(GDateTime) other_start = NULL;
		g_autoptr(GDateTime) other_end = NULL;
		g_object_get(g_ptr_array_index(jobs, i), "window-start", &other_start, "window-end", &other_end, NULL);
		if (other_start == NULL)
			continue;
		if (other_end == NULL)
			other_end = g_date_time_add_hours(other_start, 2);
		if (g_date_time_compare(start, other_end) < 0 && g_date_time_compare(other_start, end) < 0)
			return TRUE;
	}
	return FALSE;
}

static gboolean
book_job(VentureDatabase *database, gint64 org, VentureEntity *contact, const gchar *title,
	GDateTime *start, GDateTime *end, GError **error)
{
	g_autoptr(VentureEntity) company = NULL;
	g_autoptr(VentureEntity) job = NULL;
	const VentureActor actor = { VENTURE_ACTOR_KIND_SYSTEM, "voice", NULL, NULL, NULL };
	gint64 company_id = 0;
	if (contact != NULL)
		g_object_get(contact, "company-id", &company_id, NULL);
	if (company_id <= 0) {
		company = g_object_new(VENTURE_TYPE_COMPANY, "organization-id", org, "name", "Phone caller", NULL);
		venture_entity_set_organization_id(company, org);
		if (!venture_database_save(database, company, &actor, error))
			return FALSE;
		company_id = venture_entity_get_id(company);
	}
	job = g_object_new(VENTURE_TYPE_JOB, "organization-id", org, "title", title, "company-id", company_id,
		"window-start", start, "window-end", end, "contact-id", contact ? venture_entity_get_id(contact) : (gint64)0,
		"status", VENTURE_JOB_BOOKED, NULL);
	venture_entity_set_organization_id(job, org);
	return venture_database_save(database, job, &actor, error);
}

static gboolean
log_voice_call(VentureDatabase *database, VentureEntity *subject, const gchar *call_id, const gchar *transcript,
	GDateTime *now, GError **error)
{
	g_autoptr(JsonObject) details = json_object_new();
	g_autoptr(VentureEntity) logged = NULL;
	g_autofree gchar *when = venture_time_to_string(now);
	const VentureActor actor = { VENTURE_ACTOR_KIND_SYSTEM, "voice", NULL, NULL, NULL };
	json_object_set_string_member(details, "subject", "Inbound call");
	json_object_set_string_member(details, "owner", "Unassigned");
	json_object_set_string_member(details, "call_direction", "inbound");
	json_object_set_int_member(details, "call_duration", 0);
	json_object_set_string_member(details, "call_occurred_at", when);
	json_object_set_string_member(details, "call_outcome", "reached");
	json_object_set_string_member(details, "call_external_source", "venture-voice-v1");
	json_object_set_string_member(details, "call_external_id", call_id);
	json_object_set_string_member(details, "call_transcript", transcript);
	logged = venture_activity_service_log_call(venture_database_get_activity_service(database), subject, details, &actor, error);
	return logged != NULL;
}

static VoiceCall *
ensure_call(gint64 org, const gchar *call_id, const gchar *number)
{
	gchar *key = call_key(org, call_id);
	VoiceCall *call;
	g_mutex_lock(&calls_lock);
	call = g_hash_table_lookup(calls, key);
	if (call == NULL) {
		call = g_new0(VoiceCall, 1);
		call->number = g_strdup(number ? number : "");
		call->transcript = g_string_new("");
		g_hash_table_insert(calls, key, call);
		key = NULL;
	}
	g_mutex_unlock(&calls_lock);
	g_free(key);
	return call;
}

gboolean
venture_telephony_service_voice_turn(VentureDatabase *database, gint64 organization_id,
	const gchar *call_id, const gchar *caller_number, const gchar *caller_said, const gchar *model_text,
	GDateTime *now, gchar **spoken, GError **error)
{
	g_autoptr(VentureEntity) desk = NULL;
	g_autoptr(VentureEntity) org = NULL;
	g_autofree gchar *services = NULL;
	g_autofree gchar *area = NULL;
	g_autofree gchar *rules = NULL;
	g_autofree gchar *name = NULL;
	VoiceCall *call;
	GString *line;
	gboolean failed;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), FALSE);
	g_return_val_if_fail(spoken != NULL, FALSE);
	*spoken = NULL;
	voice_ensure();
	if (call_id == NULL || *call_id == '\0')
		return FALSE;
	desk = latest_desk(database, organization_id, error);
	if (error && *error)
		return FALSE;
	org = venture_database_get(database, VENTURE_TYPE_ORGANIZATION, organization_id, error);
	if (org == NULL || desk == NULL || !answering(desk, org, now, error))
		return FALSE;
	call = ensure_call(organization_id, call_id, caller_number);
	failed = model_text == NULL || *model_text == '\0';
	line = g_string_new("");
	if (!call->disclosed) {
		g_object_get(org, "name", &name, NULL);
		g_object_get(desk, "services", &services, "service-area", &area, "booking-rules", &rules, NULL);
		g_string_append(line, "This call is recorded. ");
		if (name && *name)
			g_string_append_printf(line, "%s. ", name);
		if (services && *services)
			g_string_append_printf(line, "Services: %s. ", services);
		if (area && *area)
			g_string_append_printf(line, "Area: %s. ", area);
		if (rules && *rules)
			g_string_append_printf(line, "Booking: %s. ", rules);
		call->disclosed = TRUE;
	}
	if (failed) {
		g_string_append(line, "I didn't catch that. Please leave a message and we will call you back.");
		call->failed = TRUE;
		g_free(call->summary);
		call->summary = g_strdup_printf("Message taken: %s", caller_said ? caller_said : "");
	} else {
		g_string_append(line, model_text);
		if (call->summary == NULL)
			call->summary = g_strdup(model_text);
	}
	if (caller_said && *caller_said) {
		g_string_append_printf(call->transcript, "Caller: %s\n", caller_said);
		if (caller_said && g_strrstr(caller_said, "urgent") && !call->urgent_told) {
			tell_owners(database, organization_id, "Urgent call", caller_said);
			call->urgent_told = TRUE;
		}
	}
	g_string_append_printf(call->transcript, "Line: %s\n", line->str);
	*spoken = g_string_free(line, FALSE);
	return TRUE;
}

gboolean
venture_telephony_service_voice_finish(VentureDatabase *database, gint64 organization_id,
	const gchar *call_id, const gchar *book_title, GDateTime *window_start, GDateTime *window_end,
	gboolean urgent, GDateTime *now, GError **error)
{
	g_autofree gchar *key = NULL;
	g_autoptr(VentureEntity) contact = NULL;
	g_autoptr(VentureEntity) lead = NULL;
	g_autoptr(GDateTime) end = NULL;
	VoiceCall *call;
	const VentureActor actor = { VENTURE_ACTOR_KIND_SYSTEM, "voice", NULL, NULL, NULL };
	gboolean booked = book_title != NULL && window_start != NULL;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), FALSE);
	voice_ensure();
	key = call_key(organization_id, call_id);
	g_mutex_lock(&calls_lock);
	call = g_hash_table_lookup(calls, key);
	g_mutex_unlock(&calls_lock);
	if (call == NULL) {
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "That call was not answered");
		return FALSE;
	}
	if (booked) {
		if (window_end == NULL)
			end = g_date_time_add_hours(window_start, 2);
		if (window_overlaps(database, organization_id, window_start, end ? end : window_end, error)) {
			if (error && *error)
				return FALSE;
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT, "That arrival window is already booked");
			return FALSE;
		}
	}
	contact = venture_telephony_service_match_contact(venture_telephony_service_get(database), organization_id, call->number, error);
	if (error && *error)
		return FALSE;
	if (contact == NULL) {
		g_autofree gchar *notes = g_strdup_printf("Summary: %s\nTranscript: %s",
			call->summary ? call->summary : "Call answered", call->transcript->str);
		lead = g_object_new(VENTURE_TYPE_LEAD, "organization-id", organization_id, "name", call->number,
			"phone", call->number, "source", "phone", "notes", notes, NULL);
		venture_entity_set_organization_id(lead, organization_id);
		if (!venture_database_save(database, lead, &actor, error))
			return FALSE;
		if (!log_voice_call(database, lead, call_id, call->transcript->str, now, error))
			return FALSE;
	} else if (!log_voice_call(database, contact, call_id, call->transcript->str, now, error))
		return FALSE;
	if (booked && !book_job(database, organization_id, contact, book_title, window_start, end ? end : window_end, error))
		return FALSE;
	if (urgent && !call->urgent_told)
		tell_owners(database, organization_id, "Urgent call", call->transcript->str);
	tell_owners(database, organization_id, "Call ended", call->summary ? call->summary : "Call answered");
	g_mutex_lock(&calls_lock);
	g_hash_table_remove(calls, key);
	g_mutex_unlock(&calls_lock);
	return TRUE;
}
