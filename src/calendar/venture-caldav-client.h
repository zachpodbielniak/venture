/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_CALDAV_CLIENT_H
#define VENTURE_CALDAV_CLIENT_H
#include <gio/gio.h>
G_BEGIN_DECLS
/**
 * VentureCalDavItem:
 * @href: the event's path on the server
 * @etag: the server's entity tag for it
 *
 * One member of a calendar collection, as a PROPFIND lists it.
 */
typedef struct {
	gchar *href;
	gchar *etag;
} VentureCalDavItem;
#define VENTURE_TYPE_CALDAV_ITEM (venture_caldav_item_get_type())
GType venture_caldav_item_get_type(void) G_GNUC_CONST;
/**
 * venture_caldav_item_new:
 * @href: the event's path
 * @etag: (nullable): its entity tag
 * Returns: (transfer full): a new item
 */
VentureCalDavItem *venture_caldav_item_new(const gchar *href, const gchar *etag);
/**
 * venture_caldav_item_copy:
 * @item: an item
 * Returns: (transfer full): a copy
 */
VentureCalDavItem *venture_caldav_item_copy(const VentureCalDavItem *item);
/**
 * venture_caldav_item_free:
 * @item: (transfer full): an item
 */
void venture_caldav_item_free(VentureCalDavItem *item);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(VentureCalDavItem, venture_caldav_item_free)

#define VENTURE_TYPE_CALDAV_CLIENT (venture_caldav_client_get_type())
G_DECLARE_INTERFACE(VentureCalDavClient, venture_caldav_client, VENTURE, CALDAV_CLIENT, GObject)
/**
 * VentureCalDavClientInterface:
 * @connect: remember the base URL and credential for one session
 * @get_token: the collection's ctag, which changes whenever any member does
 * @list: every member of the collection with its etag
 * @fetch: one member's iCalendar text and current etag
 * @put: create (no etag) or conditionally replace (If-Match) one member
 * @disconnect: forget the credential; must be safe when not connected
 *
 * The transport behind the calendar sync service. The libsoup
 * implementation speaks the four CalDAV requests the service needs; tests
 * use the in-memory fake. Nothing here deletes: the service cancels events
 * rather than removing them.
 */
struct _VentureCalDavClientInterface {
	GTypeInterface parent_iface;
	gboolean (*connect)(VentureCalDavClient *self, const gchar *url, const gchar *username, const gchar *secret, GCancellable *cancellable, GError **error);
	gchar *(*get_token)(VentureCalDavClient *self, const gchar *calendar_path, GCancellable *cancellable, GError **error);
	GPtrArray *(*list)(VentureCalDavClient *self, const gchar *calendar_path, GCancellable *cancellable, GError **error);
	gchar *(*fetch)(VentureCalDavClient *self, const gchar *href, gchar **etag, GCancellable *cancellable, GError **error);
	gchar *(*put)(VentureCalDavClient *self, const gchar *href, const gchar *ics, const gchar *etag, GCancellable *cancellable, GError **error);
	void (*disconnect)(VentureCalDavClient *self);
};
/**
 * venture_caldav_client_connect:
 * @self: the client
 * @url: the CalDAV base URL
 * @username: login name
 * @secret: password or app token, never stored by the service
 * @cancellable: (nullable): cancellation
 * @error: (out) (optional): configuration failure
 * Returns: whether the session is ready
 */
gboolean venture_caldav_client_connect(VentureCalDavClient *self, const gchar *url, const gchar *username, const gchar *secret, GCancellable *cancellable, GError **error);
/**
 * venture_caldav_client_get_token:
 * @self: the client
 * @calendar_path: the collection
 * @cancellable: (nullable): cancellation
 * @error: (out) (optional): failure
 * Returns: (transfer full): the collection's ctag
 */
gchar *venture_caldav_client_get_token(VentureCalDavClient *self, const gchar *calendar_path, GCancellable *cancellable, GError **error);
/**
 * venture_caldav_client_list:
 * @self: the client
 * @calendar_path: the collection
 * @cancellable: (nullable): cancellation
 * @error: (out) (optional): failure
 * Returns: (transfer full) (element-type VentureCalDavItem): the members
 */
GPtrArray *venture_caldav_client_list(VentureCalDavClient *self, const gchar *calendar_path, GCancellable *cancellable, GError **error);
/**
 * venture_caldav_client_fetch:
 * @self: the client
 * @href: a member path
 * @etag: (out) (optional) (transfer full): its current etag
 * @cancellable: (nullable): cancellation
 * @error: (out) (optional): failure
 * Returns: (transfer full): the iCalendar text
 */
gchar *venture_caldav_client_fetch(VentureCalDavClient *self, const gchar *href, gchar **etag, GCancellable *cancellable, GError **error);
/**
 * venture_caldav_client_put:
 * @self: the client
 * @href: a member path
 * @ics: the iCalendar text
 * @etag: (nullable): the etag the caller last saw; %NULL creates and refuses to overwrite
 * @cancellable: (nullable): cancellation
 * @error: (out) (optional): failure, %VENTURE_ERROR_CONFLICT when the precondition fails
 * Returns: (transfer full): the new etag
 */
gchar *venture_caldav_client_put(VentureCalDavClient *self, const gchar *href, const gchar *ics, const gchar *etag, GCancellable *cancellable, GError **error);
/**
 * venture_caldav_client_disconnect:
 * @self: the client
 */
void venture_caldav_client_disconnect(VentureCalDavClient *self);

#define VENTURE_TYPE_FAKE_CALDAV_CLIENT (venture_fake_caldav_client_get_type())
G_DECLARE_FINAL_TYPE(VentureFakeCalDavClient, venture_fake_caldav_client, VENTURE, FAKE_CALDAV_CLIENT, GObject)
/**
 * venture_fake_caldav_client_new:
 * Returns: (transfer full): an in-memory calendar for tests; no network
 */
VentureFakeCalDavClient *venture_fake_caldav_client_new(void);
/**
 * venture_fake_caldav_client_set_remote:
 * @self: the fake
 * @href: member path
 * @ics: iCalendar text
 *
 * What another calendar client would do: create or change an event on
 * the server, giving it a new etag and moving the collection's ctag.
 */
void venture_fake_caldav_client_set_remote(VentureFakeCalDavClient *self, const gchar *href, const gchar *ics);
/**
 * venture_fake_caldav_client_remove_remote:
 * @self: the fake
 * @href: member path
 *
 * What another calendar client would do: delete the event on the server.
 */
void venture_fake_caldav_client_remove_remote(VentureFakeCalDavClient *self, const gchar *href);
/**
 * venture_fake_caldav_client_get_remote:
 * @self: the fake
 * @href: member path
 * Returns: (transfer none) (nullable): the stored iCalendar text
 */
const gchar *venture_fake_caldav_client_get_remote(VentureFakeCalDavClient *self, const gchar *href);
/**
 * venture_fake_caldav_client_get_hrefs:
 * @self: the fake
 * Returns: (transfer full): the member paths, sorted
 */
GStrv venture_fake_caldav_client_get_hrefs(VentureFakeCalDavClient *self);
/**
 * venture_fake_caldav_client_get_connects:
 * @self: the fake
 * Returns: how many sessions were opened
 */
gint venture_fake_caldav_client_get_connects(VentureFakeCalDavClient *self);
/**
 * venture_fake_caldav_client_get_puts:
 * @self: the fake
 * Returns: how many PUTs the service issued
 */
gint venture_fake_caldav_client_get_puts(VentureFakeCalDavClient *self);
/**
 * venture_fake_caldav_client_get_fetches:
 * @self: the fake
 * Returns: how many members the service fetched
 */
gint venture_fake_caldav_client_get_fetches(VentureFakeCalDavClient *self);

#define VENTURE_TYPE_SOUP_CALDAV_CLIENT (venture_soup_caldav_client_get_type())
G_DECLARE_FINAL_TYPE(VentureSoupCalDavClient, venture_soup_caldav_client, VENTURE, SOUP_CALDAV_CLIENT, GObject)
/**
 * venture_soup_caldav_client_new:
 * Returns: (transfer full): a CalDAV client over libsoup with HTTP basic authentication
 */
VentureSoupCalDavClient *venture_soup_caldav_client_new(void);
G_END_DECLS
#endif
