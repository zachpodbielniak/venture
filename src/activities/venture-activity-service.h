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
G_END_DECLS
#endif
