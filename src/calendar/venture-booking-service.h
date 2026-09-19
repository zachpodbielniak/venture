/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_BOOKING_SERVICE_H
#define VENTURE_BOOKING_SERVICE_H
#include "calendar/venture-calendar-records.h"
#include "db/venture-database.h"
G_BEGIN_DECLS
#define VENTURE_TYPE_BOOKING_SERVICE (venture_booking_service_get_type())
G_DECLARE_FINAL_TYPE(VentureBookingService, venture_booking_service, VENTURE, BOOKING_SERVICE, GObject)
/**
 * venture_booking_service_new:
 * @database: owning database; used only on its main thread
 * Returns: (transfer full): the scheduling-link service
 */
VentureBookingService *venture_booking_service_new(VentureDatabase *database);
/**
 * venture_booking_service_find_page:
 * @self: the service
 * @slug: the public slug
 * @error: (out) (optional): query failure
 *
 * The public path carries no organization, so the first active page with
 * this slug across the install answers; keep slugs distinct per install.
 * Returns: (transfer full) (nullable): the active page, or %NULL
 */
VentureEntity *venture_booking_service_find_page(VentureBookingService *self, const gchar *slug, GError **error);
/**
 * venture_booking_service_slots:
 * @self: the service
 * @page: a saved booking_page
 * @now: (nullable): the reference instant, %NULL for now
 * @error: (out) (optional): configuration or query failure
 *
 * Free slots from @now to the page's horizon: each availability window in
 * the owner's timezone is cut into duration-long slots, and a slot is
 * dropped when, widened by the buffer, it overlaps any of the owner's
 * planned calls and meetings — which is where synced calendar events live.
 * Returns: (transfer full) (nullable): a JSON array of {"start", "end", "label"}
 */
JsonNode *venture_booking_service_slots(VentureBookingService *self, VentureEntity *page, GDateTime *now, GError **error);
/**
 * venture_booking_service_book:
 * @self: the service
 * @page: a saved booking_page
 * @start: ISO 8601 start of an offered slot
 * @name: the customer's name
 * @email: the customer's email
 * @notes: (nullable): what they wrote
 * @now: (nullable): the reference instant, %NULL for now
 * @actor: (nullable): audit actor
 * @error: (out) (optional): %VENTURE_ERROR_CONFLICT when the slot is not free, %VENTURE_ERROR_VALIDATION for bad input
 *
 * Books one slot: the contact is found by normalised email the way lead
 * deduplication matches, or created, and a planned meeting owned by the
 * page owner is written, both in one transaction. The slot is recomputed
 * first, so a second booking of the same time is refused.
 * Returns: (transfer full) (nullable): the new activity
 */
VentureEntity *venture_booking_service_book(VentureBookingService *self, VentureEntity *page, const gchar *start, const gchar *name, const gchar *email, const gchar *notes, GDateTime *now, const VentureActor *actor, GError **error);
G_END_DECLS
#endif
