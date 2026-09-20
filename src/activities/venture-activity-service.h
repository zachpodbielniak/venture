/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_ACTIVITY_SERVICE_H
#define VENTURE_ACTIVITY_SERVICE_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
#define VENTURE_TYPE_ACTIVITY_SERVICE (venture_activity_service_get_type())
G_DECLARE_FINAL_TYPE(VentureActivityService, venture_activity_service, VENTURE, ACTIVITY_SERVICE, GObject)
/**
 * venture_database_get_activity_service:
 * @database: owning repository
 *
 * Returns: (transfer none): its activity service
 */
VentureActivityService *venture_database_get_activity_service(VentureDatabase *database);
/**
 * venture_activity_service_complete:
 * @self: service
 * @activity: saved planned activity, with the expected version
 * @outcome: (nullable): what happened
 * @actor: (nullable): audit actor
 * @error: (out) (optional): failure
 *
 * Completes, records the interaction and advances recurrence atomically.
 * The input stays unchanged, including on failure.
 * Returns: (transfer full) (nullable): the completed record
 */
VentureEntity *venture_activity_service_complete(VentureActivityService *self, VentureEntity *activity,
	const gchar *outcome, const VentureActor *actor, GError **error);
/**
 * venture_activity_service_log_call:
 * @self: the shared activity service
 * @subject: a saved company, contact, lead or planned call activity
 * @details: call inputs in wire spelling, as described by the log_call action
 * @actor: (nullable): authenticated audit attribution
 * @error: (out) (optional): validation, scope, stale request or replay conflict
 *
 * Completes one call through the ordinary completion transaction, records
 * one interaction and optionally creates a followup. Exact external or
 * content-derived request replays return their existing result. Input
 * records stay unchanged. No recording URL is fetched.
 * Returns: (transfer full) (nullable): the completed call activity
 */
VentureEntity *venture_activity_service_log_call(VentureActivityService *self,
	VentureEntity *subject, JsonObject *details, const VentureActor *actor, GError **error);
/**
 * venture_activity_actions_register:
 * @database: owning repository
 *
 * Registers metadata-derived log_call actions on CRM records and activities.
 */
void venture_activity_actions_register(VentureDatabase *database);
/**
 * venture_activity_call_timeline:
 * @context: application services and module selection
 * @target_type: company, contact, lead or deal
 * @target_id: verified subject identity
 * @limit: maximum number of call events
 * @error: (out) (optional): scope or query failure
 *
 * Returns one summary for each completed call with retained interaction
 * evidence. The subject and call records are checked through the repository.
 * Recordings and transcripts are excluded from timeline excerpts.
 * Returns: (transfer full) (nullable): a JSON array of historical call events
 */
JsonNode *venture_activity_call_timeline(VentureContext *context, const gchar *target_type,
	gint64 target_id, guint limit, GError **error);
/**
 * venture_activity_service_act:
 * @self: service
 * @activity: saved activity, with the expected version
 * @action: complete, snooze, reassign or cancel
 * @value: (nullable): outcome, ISO until timestamp, or owner
 * @actor: (nullable): audit actor
 * @error: (out) (optional): failure
 *
 * Returns: (transfer full) (nullable): the changed record; input stays unchanged
 */
VentureEntity *venture_activity_service_act(VentureActivityService *self, VentureEntity *activity,
	const gchar *action, const gchar *value, const VentureActor *actor, GError **error);
/**
 * venture_activity_service_sweep:
 * @self: service
 * @organization: exact owning organization
 * @limit: maximum reminders, zero for 200
 * @error: (out) (optional): failure
 *
 * Delivers due reminders once, within one transaction, on the calling thread.
 * Returns: number delivered, or -1 on failure
 */
gint venture_activity_service_sweep(VentureActivityService *self, gint64 organization, guint limit, GError **error);
/**
 * venture_activity_service_list:
 * @self: service
 * @organization: exact organization
 * @owner: (nullable): username, NULL for all owners
 * @queue: all, mine, overdue, today or upcoming
 * @now: (nullable): UTC reference instant, NULL for now
 * @error: (out) (optional): failure
 *
 * Returns: (transfer full) (element-type VentureEntity) (nullable): activities sorted by due time
 */
GPtrArray *venture_activity_service_list(VentureActivityService *self, gint64 organization,
	const gchar *owner, const gchar *queue, GDateTime *now, GError **error);
/**
 * venture_activity_service_calendar:
 * @self: service
 * @organization: exact organization
 * @owner: (nullable): owner filter
 * @error: (out) (optional): failure
 *
 * Returns: (transfer full) (nullable): RFC 5545 calendar, with CRLF and folded content lines
 */
gchar *venture_activity_service_calendar(VentureActivityService *self, gint64 organization,
	const gchar *owner, GError **error);
/**
 * venture_activity_calendar_append_line:
 * @calendar: the text being built
 * @name: property name, for example SUMMARY
 * @value: (nullable): the value
 * @escape: whether to backslash-escape commas, semicolons and backslashes
 *
 * Appends one RFC 5545 content line the way the .ics export writes its
 * own: CRLF-terminated, newlines written as \n, folded at 75 octets
 * without cutting a UTF-8 sequence. Exposed so the calendar sync module
 * writes VEVENTs with the same rules instead of a second folder.
 */
void venture_activity_calendar_append_line(GString *calendar, const gchar *name,
	const gchar *value, gboolean escape);
/**
 * venture_activity_calendar_append_date:
 * @calendar: the text being built
 * @name: property name, for example DTSTART
 * @date: an instant; written in UTC with a Z suffix
 */
void venture_activity_calendar_append_date(GString *calendar, const gchar *name,
	GDateTime *date);
G_END_DECLS

#endif
