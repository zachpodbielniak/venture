/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_IMAP_CLIENT_H
#define VENTURE_IMAP_CLIENT_H
#include <gio/gio.h>
G_BEGIN_DECLS
/**
 * VENTURE_IMAP_MAX_MESSAGE:
 *
 * The largest message the client fetches whole. A larger one is listed
 * with its size and fetched as its header plus the start of its text.
 */
#define VENTURE_IMAP_MAX_MESSAGE ((gsize)(20 * 1024 * 1024))
/**
 * VentureImapFolderInfo:
 * @uidvalidity: the folder's UIDVALIDITY, or 0 when the server sent none
 * @uidnext: the predicted next UID, or 0 when the server sent none
 * @exists: how many messages the folder holds
 *
 * What EXAMINE reported. UIDs are only comparable while @uidvalidity is
 * unchanged; a server migration that renumbers them changes it.
 */
typedef struct {
	guint32 uidvalidity;
	guint32 uidnext;
	guint32 exists;
} VentureImapFolderInfo;
/**
 * VentureImapEntry:
 * @uid: message UID
 * @size: RFC822.SIZE in bytes
 *
 * One listed message, so a caller can refuse an oversized one before
 * fetching it.
 */
typedef struct {
	guint32 uid;
	guint64 size;
} VentureImapEntry;
#define VENTURE_TYPE_IMAP_CLIENT (venture_imap_client_get_type())
G_DECLARE_INTERFACE(VentureImapClient, venture_imap_client, VENTURE, IMAP_CLIENT, GObject)
/**
 * VentureImapClientInterface:
 * @connect: open and authenticate one session
 * @select: open a folder read-only and report its UIDVALIDITY, UIDNEXT and size
 * @list: at most a bounded number of UIDs above a high-water mark, ascending, with sizes
 * @since: the high-water mark to start after so only messages received on or after a date follow
 * @fetch: the raw RFC 5322 bytes of one UID
 * @fetch_truncated: the header and the first bytes of the text of one UID
 * @set_deadline: a monotonic instant after which every call fails with %VENTURE_ERROR_TIMEOUT
 * @disconnect: close the session; must be safe when not connected
 *
 * The transport behind the sync service. deps/mail-glib is sending-only, so
 * the socket implementation lives in src/mail/; tests use the fake or the
 * socket client against a scripted local server.
 *
 * Errors are classified by code, because the service decides from them
 * whether to stop, back off or skip one message:
 * %VENTURE_ERROR_UNAUTHENTICATED is a refused login, %VENTURE_ERROR_NETWORK,
 * %VENTURE_ERROR_TIMEOUT and any GIO or TLS error mean the session is gone,
 * %VENTURE_ERROR_NOT_FOUND is a refused folder on a live session, and
 * %VENTURE_ERROR_MAIL_PERMANENT means this one message cannot be fetched.
 * No error text ever echoes the server, whose reply to a refused login can
 * carry the credential; only a fixed RFC 5530 response code is named.
 */
struct _VentureImapClientInterface {
	GTypeInterface parent_iface;
	gboolean (*connect)(VentureImapClient *self, const gchar *host, guint16 port, const gchar *security, const gchar *username, const gchar *secret, GCancellable *cancellable, GError **error);
	gboolean (*select)(VentureImapClient *self, const gchar *folder, VentureImapFolderInfo *info, GCancellable *cancellable, GError **error);
	GArray *(*list)(VentureImapClient *self, guint32 after_uid, guint max_entries, GCancellable *cancellable, GError **error);
	gboolean (*since)(VentureImapClient *self, GDateTime *since, guint32 *after_uid, GCancellable *cancellable, GError **error);
	GBytes *(*fetch)(VentureImapClient *self, guint32 uid, GCancellable *cancellable, GError **error);
	GBytes *(*fetch_truncated)(VentureImapClient *self, guint32 uid, gsize text_limit, GCancellable *cancellable, GError **error);
	void (*set_deadline)(VentureImapClient *self, gint64 monotonic_deadline);
	void (*disconnect)(VentureImapClient *self);
};
/**
 * venture_imap_client_connect:
 * @self: the client
 * @host: server name
 * @port: server port
 * @security: tls or starttls
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
 * @folder: mailbox name in UTF-8; the socket client encodes it as modified UTF-7
 * @info: (out caller-allocates) (optional): what the server reported
 * @cancellable: (nullable): cancellation
 * @error: (out) (optional): failure
 * Returns: whether the folder is selected
 */
gboolean venture_imap_client_select(VentureImapClient *self, const gchar *folder, VentureImapFolderInfo *info, GCancellable *cancellable, GError **error);
/**
 * venture_imap_client_list:
 * @self: the client
 * @after_uid: high-water mark; zero means from the first message
 * @max_entries: most entries to return
 * @cancellable: (nullable): cancellation
 * @error: (out) (optional): failure
 *
 * No single server response grows with the mailbox: the socket client
 * pages UID ranges, so a first sync of a large inbox is a series of small
 * replies rather than one line naming every UID.
 * Returns: (transfer full) (element-type VentureImapEntry): ascending entries above the mark
 */
GArray *venture_imap_client_list(VentureImapClient *self, guint32 after_uid, guint max_entries, GCancellable *cancellable, GError **error);
/**
 * venture_imap_client_since:
 * @self: the client
 * @since: the earliest received date wanted
 * @after_uid: (out): the high-water mark to list after: one below the first
 *   message received on or after @since, the highest UID when none was, or
 *   zero for an empty folder
 * @cancellable: (nullable): cancellation
 * @error: (out) (optional): failure
 * Returns: whether @after_uid was set
 */
gboolean venture_imap_client_since(VentureImapClient *self, GDateTime *since, guint32 *after_uid, GCancellable *cancellable, GError **error);
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
 * venture_imap_client_fetch_truncated:
 * @self: the client
 * @uid: message UID in the selected folder
 * @text_limit: most bytes of the body to include
 * @cancellable: (nullable): cancellation
 * @error: (out) (optional): failure
 *
 * For a message too large to fetch whole: its header block followed by the
 * first @text_limit bytes of its body, which still parses as a message.
 * Returns: (transfer full): the truncated message
 */
GBytes *venture_imap_client_fetch_truncated(VentureImapClient *self, guint32 uid, gsize text_limit, GCancellable *cancellable, GError **error);
/**
 * venture_imap_client_set_deadline:
 * @self: the client
 * @monotonic_deadline: a g_get_monotonic_time() instant, or 0 for none
 *
 * A per-read timeout alone lets a server that trickles one byte a minute
 * hold the caller forever; the deadline bounds the whole session.
 */
void venture_imap_client_set_deadline(VentureImapClient *self, gint64 monotonic_deadline);
/**
 * venture_imap_client_disconnect:
 * @self: the client
 */
void venture_imap_client_disconnect(VentureImapClient *self);
/**
 * venture_imap_utf7_encode:
 * @utf8: a folder name in UTF-8
 *
 * RFC 3501 section 5.1.3 modified UTF-7: printable ASCII stays itself,
 * "&" becomes "&-", everything else is UTF-16 in a "&...-" run of base64
 * using "," for "/".
 * Returns: (transfer full) (nullable): the wire name, or %NULL for invalid UTF-8
 */
gchar *venture_imap_utf7_encode(const gchar *utf8);

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
 * venture_fake_imap_client_add_bytes:
 * @self: the fake
 * @folder: mailbox name
 * @uid: message UID
 * @raw: RFC 5322 bytes, which need not be valid UTF-8
 */
void venture_fake_imap_client_add_bytes(VentureFakeImapClient *self, const gchar *folder, guint32 uid, GBytes *raw);
/**
 * venture_fake_imap_client_clear_folder:
 * @self: the fake
 * @folder: mailbox name
 *
 * Empties a folder, the way a server migration that renumbers UIDs does.
 */
void venture_fake_imap_client_clear_folder(VentureFakeImapClient *self, const gchar *folder);
/**
 * venture_fake_imap_client_set_uidvalidity:
 * @self: the fake
 * @folder: mailbox name
 * @uidvalidity: value EXAMINE reports; 0 reports none
 */
void venture_fake_imap_client_set_uidvalidity(VentureFakeImapClient *self, const gchar *folder, guint32 uidvalidity);
/**
 * venture_fake_imap_client_set_size:
 * @self: the fake
 * @folder: mailbox name
 * @uid: message UID
 * @size: the RFC822.SIZE to list instead of the real length
 */
void venture_fake_imap_client_set_size(VentureFakeImapClient *self, const gchar *folder, guint32 uid, guint64 size);
/**
 * venture_fake_imap_client_set_internal_date:
 * @self: the fake
 * @folder: mailbox name
 * @uid: message UID
 * @date: when the server received it
 */
void venture_fake_imap_client_set_internal_date(VentureFakeImapClient *self, const gchar *folder, guint32 uid, GDateTime *date);
/**
 * venture_fake_imap_client_set_connect_error:
 * @self: the fake
 * @error: (nullable): what every connect fails with, or %NULL to succeed
 */
void venture_fake_imap_client_set_connect_error(VentureFakeImapClient *self, const GError *error);
/**
 * venture_fake_imap_client_set_fetch_error:
 * @self: the fake
 * @uid: message UID in any folder
 * @error: (nullable): what fetching it fails with, or %NULL to succeed
 */
void venture_fake_imap_client_set_fetch_error(VentureFakeImapClient *self, guint32 uid, const GError *error);
/**
 * venture_fake_imap_client_refuse_folder:
 * @self: the fake
 * @folder: mailbox name whose select fails with %VENTURE_ERROR_NOT_FOUND
 */
void venture_fake_imap_client_refuse_folder(VentureFakeImapClient *self, const gchar *folder);
/**
 * venture_fake_imap_client_get_connects:
 * @self: the fake
 * Returns: how many sessions were opened
 */
gint venture_fake_imap_client_get_connects(VentureFakeImapClient *self);
/**
 * venture_fake_imap_client_get_fetches:
 * @self: the fake
 * Returns: how many message bodies were fetched, whole or truncated
 */
gint venture_fake_imap_client_get_fetches(VentureFakeImapClient *self);

#define VENTURE_TYPE_SOCKET_IMAP_CLIENT (venture_socket_imap_client_get_type())
G_DECLARE_FINAL_TYPE(VentureSocketImapClient, venture_socket_imap_client, VENTURE, SOCKET_IMAP_CLIENT, GObject)
/**
 * venture_socket_imap_client_new:
 *
 * The construct-only "allow-plaintext" property lets a test talk to a
 * scripted server on loopback with security "none". Nothing outside the
 * test suite sets it: an account's security field never reaches it.
 * Returns: (transfer full): a minimal IMAP4rev1 client over GIO sockets and TLS
 */
VentureSocketImapClient *venture_socket_imap_client_new(void);
G_END_DECLS
#endif
