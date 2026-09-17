/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_IMAP_CLIENT_H
#define VENTURE_IMAP_CLIENT_H
#include <gio/gio.h>
G_BEGIN_DECLS
#define VENTURE_TYPE_IMAP_CLIENT (venture_imap_client_get_type())
G_DECLARE_INTERFACE(VentureImapClient, venture_imap_client, VENTURE, IMAP_CLIENT, GObject)
/**
 * VentureImapClientInterface:
 * @connect: open and authenticate one session
 * @select: open a folder read-only
 * @uids_after: UIDs in the selected folder greater than a high-water mark, ascending
 * @fetch: the raw RFC 5322 bytes of one UID
 * @disconnect: close the session; must be safe when not connected
 *
 * The transport behind the sync service. deps/mail-glib is sending-only, so
 * the socket implementation lives in src/mail/; tests use the fake.
 */
struct _VentureImapClientInterface {
	GTypeInterface parent_iface;
	gboolean (*connect)(VentureImapClient *self, const gchar *host, guint16 port, const gchar *security, const gchar *username, const gchar *secret, GCancellable *cancellable, GError **error);
	gboolean (*select)(VentureImapClient *self, const gchar *folder, GCancellable *cancellable, GError **error);
	GArray *(*uids_after)(VentureImapClient *self, guint32 last_uid, GCancellable *cancellable, GError **error);
	GBytes *(*fetch)(VentureImapClient *self, guint32 uid, GCancellable *cancellable, GError **error);
	void (*disconnect)(VentureImapClient *self);
};
/**
 * venture_imap_client_connect:
 * @self: the client
 * @host: server name
 * @port: server port
 * @security: tls, starttls or none
 * @username: login name
 * @secret: password or app token, never stored by the client
 * @cancellable: (nullable): cancellation
 * @error: (out) (optional): connection or authentication failure
 * Returns: whether the session is authenticated
 */
gboolean venture_imap_client_connect(VentureImapClient *self, const gchar *host, guint16 port, const gchar *security, const gchar *username, const gchar *secret, GCancellable *cancellable, GError **error);
/**
 * venture_imap_client_select:
 * @self: the client
 * @folder: mailbox name
 * @cancellable: (nullable): cancellation
 * @error: (out) (optional): failure
 * Returns: whether the folder is selected
 */
gboolean venture_imap_client_select(VentureImapClient *self, const gchar *folder, GCancellable *cancellable, GError **error);
/**
 * venture_imap_client_uids_after:
 * @self: the client
 * @last_uid: high-water mark; zero means everything
 * @cancellable: (nullable): cancellation
 * @error: (out) (optional): failure
 * Returns: (transfer full) (element-type guint32): ascending UIDs above the mark
 */
GArray *venture_imap_client_uids_after(VentureImapClient *self, guint32 last_uid, GCancellable *cancellable, GError **error);
/**
 * venture_imap_client_fetch:
 * @self: the client
 * @uid: message UID in the selected folder
 * @cancellable: (nullable): cancellation
 * @error: (out) (optional): failure
 * Returns: (transfer full): the raw message
 */
GBytes *venture_imap_client_fetch(VentureImapClient *self, guint32 uid, GCancellable *cancellable, GError **error);
/**
 * venture_imap_client_disconnect:
 * @self: the client
 */
void venture_imap_client_disconnect(VentureImapClient *self);

#define VENTURE_TYPE_FAKE_IMAP_CLIENT (venture_fake_imap_client_get_type())
G_DECLARE_FINAL_TYPE(VentureFakeImapClient, venture_fake_imap_client, VENTURE, FAKE_IMAP_CLIENT, GObject)
/**
 * venture_fake_imap_client_new:
 * Returns: (transfer full): an in-memory mailbox for tests; no network
 */
VentureFakeImapClient *venture_fake_imap_client_new(void);
/**
 * venture_fake_imap_client_add_message:
 * @self: the fake
 * @folder: mailbox name
 * @uid: message UID
 * @raw: RFC 5322 text
 */
void venture_fake_imap_client_add_message(VentureFakeImapClient *self, const gchar *folder, guint32 uid, const gchar *raw);
/**
 * venture_fake_imap_client_get_connects:
 * @self: the fake
 * Returns: how many sessions were opened
 */
gint venture_fake_imap_client_get_connects(VentureFakeImapClient *self);

#define VENTURE_TYPE_SOCKET_IMAP_CLIENT (venture_socket_imap_client_get_type())
G_DECLARE_FINAL_TYPE(VentureSocketImapClient, venture_socket_imap_client, VENTURE, SOCKET_IMAP_CLIENT, GObject)
/**
 * venture_socket_imap_client_new:
 * Returns: (transfer full): a minimal IMAP4rev1 client over GIO sockets and TLS
 */
VentureSocketImapClient *venture_socket_imap_client_new(void);
G_END_DECLS
#endif
