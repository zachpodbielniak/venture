/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_CALENDAR_SYNC_SERVICE_H
#define VENTURE_CALENDAR_SYNC_SERVICE_H
#include "calendar/venture-caldav-client.h"
#include "calendar/venture-calendar-records.h"
#include "calendar/venture-icalendar.h"
#include "db/venture-database.h"
G_BEGIN_DECLS
#define VENTURE_TYPE_CALENDAR_SYNC_SERVICE (venture_calendar_sync_service_get_type())
G_DECLARE_FINAL_TYPE(VentureCalendarSyncService, venture_calendar_sync_service, VENTURE, CALENDAR_SYNC_SERVICE, GObject)
/**
 * venture_calendar_sync_service_new:
 * @database: owning database; used only on its main thread
 * @client: CalDAV transport; the libsoup client in production, the fake in tests
 * Returns: (transfer full): the calendar sync service
 */
VentureCalendarSyncService *venture_calendar_sync_service_new(VentureDatabase *database, VentureCalDavClient *client);
/**
 * venture_calendar_sync_service_sync:
 * @self: the service
 * @account: a saved calendar_account
 * @actor: (nullable): audit actor
 * @error: (out) (optional): configuration, transport or persistence failure
 *
 * One two-way pass. Pulls new and changed VEVENTs into activities of kind
 * meeting owned by the account's owner, keyed by UID; pushes the owner's
 * dated calls and meetings as VEVENTs whose UID is derived from the
 * activity uuid; cancels rather than deletes on either side; resolves an
 * event changed on both sides by last-modified, keeping the loser's values
 * on the activity's timeline. Each change is its own transaction, so a
 * rerun against an unchanged calendar writes nothing.
 * Returns: changes applied (pulled + pushed + cancelled), or -1
 */
gint venture_calendar_sync_service_sync(VentureCalendarSyncService *self, VentureEntity *account, const VentureActor *actor, GError **error);
/**
 * venture_calendar_sync_service_sweep:
 * @self: the service
 * @organization_id: exact owning organization
 * @limit: maximum accounts to sync
 * @actor: (nullable): audit actor
 * @error: (out) (optional): query failure; per-account failures are reported in the result
 * Returns: (transfer full) (nullable): {"accounts", "changes", "errors": [{"account", "error"}]}
 */
JsonNode *venture_calendar_sync_service_sweep(VentureCalendarSyncService *self, gint64 organization_id, guint limit, const VentureActor *actor, GError **error);
/**
 * venture_calendar_sync_event_uid:
 * @activity: a saved activity
 *
 * The UID a pushed activity carries, the same one the .ics export uses.
 * Returns: (transfer full): "<uuid>@venture"
 */
gchar *venture_calendar_sync_event_uid(VentureEntity *activity);
G_END_DECLS
#endif
