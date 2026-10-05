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
/**
 * venture_booking_service_reserve:
 * @self: the service
 * @page: saved booking target; re-read under the save lock
 * @start: offered ISO 8601 slot start
 * @seconds: hold duration, 1 to 2700 seconds
 * @now: (nullable): reference clock
 * @error: (out) (optional): return location for an error
 *
 * Reserves one seat as a private working copy, without a contact, meeting,
 * audit entry or business signal. Safe inside an existing transaction.
 * Returns: (transfer full) (nullable): the saved hold
 */
VentureEntity *venture_booking_service_reserve(VentureBookingService *self, VentureEntity *page,
	const gchar *start, guint seconds, GDateTime *now, GError **error);
/**
 * venture_booking_service_confirm:
 * @self: the service
 * @reservation: saved, unexpired hold
 * @name: bounded respondent name
 * @email: respondent inbox
 * @notes: (nullable): bounded meeting notes
 * @origin: (nullable): configured public origin, or the page's origin
 * @now: (nullable): reference clock
 * @actor: (nullable): audit actor
 * @error: (out) (optional): return location for an error
 *
 * Rechecks the hold and availability, creates the contact and meeting, and
 * queues a private confirmation when mail and a public origin are configured.
 * Returns: (transfer full) (nullable): the booked activity
 */
VentureEntity *venture_booking_service_confirm(VentureBookingService *self, VentureEntity *reservation,
	const gchar *name, const gchar *email, const gchar *notes, const gchar *origin,
	GDateTime *now, const VentureActor *actor, GError **error);
/**
 * venture_booking_service_manage_url:
 * @self: the service
 * @reservation: current saved reservation
 * @origin: configured HTTPS origin, or loopback HTTP for local use
 * @error: (out) (optional): return location for an error
 *
 * The private signed capability permits cancellation or rescheduling. Do not
 * put it in public record fields, audit text, logs or generic action results.
 * Returns: (transfer full) (nullable): private management URL
 */
gchar *venture_booking_service_manage_url(VentureBookingService *self, VentureEntity *reservation,
	const gchar *origin, GError **error);
/**
 * venture_booking_service_lookup_capability:
 * @self: the service
 * @page: owning booking target
 * @capability: private signed token
 * @now: (nullable): reference clock
 * @error: (out) (optional): uniform not-found or database failure
 *
 * Verifies the token without consuming it or changing the booking.
 * Returns: (transfer full) (nullable): current private reservation
 */
VentureEntity *venture_booking_service_lookup_capability(VentureBookingService *self, VentureEntity *page,
	const gchar *capability, GDateTime *now, GError **error);
/**
 * venture_booking_service_manage:
 * @self: the service
 * @page: owning booking target
 * @capability: private signed token
 * @start: (nullable): new offered start, or %NULL to cancel
 * @now: (nullable): reference clock
 * @actor: (nullable): audit actor
 * @error: (out) (optional): return location for an error
 *
 * Rechecks capacity under the save lock and rotates the capability on success.
 * Returns: (transfer full) (nullable): updated meeting
 */
VentureEntity *venture_booking_service_manage(VentureBookingService *self, VentureEntity *page,
	const gchar *capability, const gchar *start, GDateTime *now, const VentureActor *actor, GError **error);
/**
 * venture_booking_service_release:
 * @self: the service
 * @reservation: saved hold
 * @error: (out) (optional): return location for an error
 *
 * Releases an unconfirmed hold. Confirmed bookings are left unchanged.
 * Returns: whether the operation succeeded
 */
gboolean venture_booking_service_release(VentureBookingService *self, VentureEntity *reservation, GError **error);
/**
 * venture_booking_service_install:
 * @database: owning main-thread database
 *
 * Installs target and reservation save validators once per database.
 */
void venture_booking_service_install(VentureDatabase *database);
G_END_DECLS
#endif
