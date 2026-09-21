/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <string.h>
#include <stdio.h>
G_DEFINE_INTERFACE(VentureImapClient, venture_imap_client, G_TYPE_OBJECT)
static void venture_imap_client_default_init(VentureImapClientInterface *iface) { (void)iface; }
gboolean venture_imap_client_connect(VentureImapClient *self, const gchar *host, guint16 port, const gchar *security, const gchar *username, const gchar *secret, GCancellable *cancellable, GError **error)
{
	g_return_val_if_fail(VENTURE_IS_IMAP_CLIENT(self), FALSE);
	g_return_val_if_fail(VENTURE_IMAP_CLIENT_GET_IFACE(self)->connect != NULL, FALSE);
	return VENTURE_IMAP_CLIENT_GET_IFACE(self)->connect(self, host, port, security, username, secret, cancellable, error);
}
gboolean venture_imap_client_select(VentureImapClient *self, const gchar *folder, VentureImapFolderInfo *info, GCancellable *cancellable, GError **error)
{
	VentureImapFolderInfo ignored;
	g_return_val_if_fail(VENTURE_IS_IMAP_CLIENT(self), FALSE);
	g_return_val_if_fail(VENTURE_IMAP_CLIENT_GET_IFACE(self)->select != NULL, FALSE);
	memset(&ignored, 0, sizeof ignored);
	return VENTURE_IMAP_CLIENT_GET_IFACE(self)->select(self, folder, info ? info : &ignored, cancellable, error);
}
GArray *venture_imap_client_list(VentureImapClient *self, guint32 after_uid, guint max_entries, GCancellable *cancellable, GError **error)
{
	g_return_val_if_fail(VENTURE_IS_IMAP_CLIENT(self), NULL);
	g_return_val_if_fail(VENTURE_IMAP_CLIENT_GET_IFACE(self)->list != NULL, NULL);
	return VENTURE_IMAP_CLIENT_GET_IFACE(self)->list(self, after_uid, max_entries, cancellable, error);
}
gboolean venture_imap_client_since(VentureImapClient *self, GDateTime *since, guint32 *after_uid, GCancellable *cancellable, GError **error)
{
	g_return_val_if_fail(VENTURE_IS_IMAP_CLIENT(self), FALSE);
	g_return_val_if_fail(since != NULL && after_uid != NULL, FALSE);
	g_return_val_if_fail(VENTURE_IMAP_CLIENT_GET_IFACE(self)->since != NULL, FALSE);
	return VENTURE_IMAP_CLIENT_GET_IFACE(self)->since(self, since, after_uid, cancellable, error);
}
GBytes *venture_imap_client_fetch(VentureImapClient *self, guint32 uid, GCancellable *cancellable, GError **error)
{
	g_return_val_if_fail(VENTURE_IS_IMAP_CLIENT(self), NULL);
	g_return_val_if_fail(VENTURE_IMAP_CLIENT_GET_IFACE(self)->fetch != NULL, NULL);
	return VENTURE_IMAP_CLIENT_GET_IFACE(self)->fetch(self, uid, cancellable, error);
}
GBytes *venture_imap_client_fetch_truncated(VentureImapClient *self, guint32 uid, gsize text_limit, GCancellable *cancellable, GError **error)
{
	g_return_val_if_fail(VENTURE_IS_IMAP_CLIENT(self), NULL);
	g_return_val_if_fail(VENTURE_IMAP_CLIENT_GET_IFACE(self)->fetch_truncated != NULL, NULL);
	return VENTURE_IMAP_CLIENT_GET_IFACE(self)->fetch_truncated(self, uid, text_limit, cancellable, error);
}
void venture_imap_client_set_deadline(VentureImapClient *self, gint64 monotonic_deadline)
{
	g_return_if_fail(VENTURE_IS_IMAP_CLIENT(self));
	if (VENTURE_IMAP_CLIENT_GET_IFACE(self)->set_deadline) VENTURE_IMAP_CLIENT_GET_IFACE(self)->set_deadline(self, monotonic_deadline);
}
void venture_imap_client_disconnect(VentureImapClient *self)
{
	g_return_if_fail(VENTURE_IS_IMAP_CLIENT(self));
	g_return_if_fail(VENTURE_IMAP_CLIENT_GET_IFACE(self)->disconnect != NULL);
	VENTURE_IMAP_CLIENT_GET_IFACE(self)->disconnect(self);
}

/* --- Modified UTF-7 folder names (RFC 3501 section 5.1.3) ---------------- */
static void utf7_flush(GString *out, guint32 bits, guint nbits)
{
	static const gchar alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+,";
	/* The partial sextet is zero-padded; the "-" ends the run even when the
	 * next character is not base64, so a decoder never guesses. */
	if (nbits) g_string_append_c(out, alphabet[(bits << (6 - nbits)) & 0x3f]);
	g_string_append_c(out, '-');
}
gchar *venture_imap_utf7_encode(const gchar *utf8)
{
	static const gchar alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+,";
	GString *out;
	const gchar *p;
	guint32 bits = 0;
	guint nbits = 0;
	gboolean shifted = FALSE;
	if (!utf8 || !g_utf8_validate(utf8, -1, NULL)) return NULL;
	out = g_string_new(NULL);
	for (p = utf8; *p; p = g_utf8_next_char(p)) {
		gunichar c = g_utf8_get_char(p);
		guint16 units[2];
		guint n = 1, u;
		if (c >= 0x20 && c <= 0x7e) {
			if (shifted) { utf7_flush(out, bits, nbits); shifted = FALSE; bits = 0; nbits = 0; }
			/* A literal "&" would open a base64 run on the server. */
			if (c == '&') g_string_append(out, "&-");
			else g_string_append_c(out, (gchar)c);
			continue;
		}
		if (!shifted) { g_string_append_c(out, '&'); shifted = TRUE; }
		if (c >= 0x10000) {
			c -= 0x10000;
			units[0] = (guint16)(0xd800 | (c >> 10));
			units[1] = (guint16)(0xdc00 | (c & 0x3ff));
			n = 2;
		} else units[0] = (guint16)c;
		for (u = 0; u < n; u++) {
			bits = (bits << 16) | units[u];
			nbits += 16;
			while (nbits >= 6) {
				nbits -= 6;
				g_string_append_c(out, alphabet[(bits >> nbits) & 0x3f]);
			}
			bits &= (1u << nbits) - 1;
		}
	}
	if (shifted) utf7_flush(out, bits, nbits);
	return g_string_free(out, FALSE);
}

static gint entry_compare(gconstpointer a, gconstpointer b)
{
	guint32 x = ((const VentureImapEntry *)a)->uid, y = ((const VentureImapEntry *)b)->uid;
	return x < y ? -1 : x > y;
}

/* --- Fake: folders of (uid, bytes) in memory ----------------------------- */
typedef struct { guint32 uid; GBytes *raw; guint64 size; GDateTime *date; } FakeMessage;
typedef struct { GArray *messages; guint32 uidvalidity; } FakeFolder;
struct _VentureFakeImapClient {
	GObject parent_instance;
	GHashTable *folders; /* name -> FakeFolder */
	GHashTable *refused; /* folder names whose select fails */
	GHashTable *fetch_errors; /* GUINT_TO_POINTER(uid) -> GError */
	GError *connect_error;
	gchar *selected;
	gint connects;
	gint fetches;
	gint64 deadline;
	gboolean connected;
};
static void fake_iface_init(VentureImapClientInterface *iface);
G_DEFINE_FINAL_TYPE_WITH_CODE(VentureFakeImapClient, venture_fake_imap_client, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(VENTURE_TYPE_IMAP_CLIENT, fake_iface_init))
static void fake_message_clear(FakeMessage *message)
{
	g_clear_pointer(&message->raw, g_bytes_unref);
	g_clear_pointer(&message->date, g_date_time_unref);
}
static void fake_folder_free(gpointer data)
{
	FakeFolder *folder = data;
	guint i;
	for (i = 0; i < folder->messages->len; i++) fake_message_clear(&g_array_index(folder->messages, FakeMessage, i));
	g_array_unref(folder->messages);
	g_free(folder);
}
static void fake_finalize(GObject *object)
{
	VentureFakeImapClient *self = VENTURE_FAKE_IMAP_CLIENT(object);
	g_hash_table_unref(self->folders);
	g_hash_table_unref(self->refused);
	g_hash_table_unref(self->fetch_errors);
	g_clear_error(&self->connect_error);
	g_free(self->selected);
	G_OBJECT_CLASS(venture_fake_imap_client_parent_class)->finalize(object);
}
static guint fake_response_ready;
static void venture_fake_imap_client_class_init(VentureFakeImapClientClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = fake_finalize;
	/**
	 * VentureFakeImapClient::response-ready:
	 * @self: fake transport
	 *
	 * Emitted synchronously after copying a successful fetch response, before
	 * returning it to the consumer. Fixtures may revoke authorization here
	 * to exercise state changes while a real provider request was pending.
	 */
	fake_response_ready = g_signal_new("response-ready", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
		0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}
static void venture_fake_imap_client_init(VentureFakeImapClient *self)
{
	self->folders = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, fake_folder_free);
	self->refused = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	self->fetch_errors = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, (GDestroyNotify)g_error_free);
}
static FakeFolder *fake_folder(VentureFakeImapClient *self, const gchar *name, gboolean create)
{
	FakeFolder *folder = g_hash_table_lookup(self->folders, name);
	if (!folder && create) {
		folder = g_new0(FakeFolder, 1);
		folder->messages = g_array_new(FALSE, TRUE, sizeof(FakeMessage));
		folder->uidvalidity = 1;
		g_hash_table_insert(self->folders, g_strdup(name), folder);
	}
	return folder;
}
static FakeMessage *fake_message(VentureFakeImapClient *self, const gchar *name, guint32 uid)
{
	FakeFolder *folder = name ? fake_folder(self, name, FALSE) : NULL;
	guint i;
	for (i = 0; folder && i < folder->messages->len; i++)
		if (g_array_index(folder->messages, FakeMessage, i).uid == uid) return &g_array_index(folder->messages, FakeMessage, i);
	return NULL;
}
static gboolean fake_expired(VentureFakeImapClient *self, GError **error)
{
	if (!self->deadline || g_get_monotonic_time() < self->deadline) return FALSE;
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_TIMEOUT, "IMAP: time budget exhausted");
	return TRUE;
}
static gboolean fake_connect(VentureImapClient *client, const gchar *host, guint16 port, const gchar *security, const gchar *username, const gchar *secret, GCancellable *cancellable, GError **error)
{
	VentureFakeImapClient *self = VENTURE_FAKE_IMAP_CLIENT(client);
	(void)host; (void)port; (void)security; (void)username; (void)cancellable;
	if (!secret || !*secret) { g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG, "IMAP secret is empty"); return FALSE; }
	if (fake_expired(self, error)) return FALSE;
	self->connects++;
	if (self->connect_error) { g_propagate_error(error, g_error_copy(self->connect_error)); return FALSE; }
	self->connected = TRUE;
	return TRUE;
}
static gboolean fake_select(VentureImapClient *client, const gchar *folder, VentureImapFolderInfo *info, GCancellable *cancellable, GError **error)
{
	VentureFakeImapClient *self = VENTURE_FAKE_IMAP_CLIENT(client);
	FakeFolder *found;
	guint i;
	(void)cancellable;
	if (!self->connected) { g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NETWORK, "IMAP: not connected"); return FALSE; }
	if (fake_expired(self, error)) return FALSE;
	g_clear_pointer(&self->selected, g_free);
	if (g_hash_table_contains(self->refused, folder)) { g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "IMAP: folder refused [NONEXISTENT]"); return FALSE; }
	self->selected = g_strdup(folder);
	found = fake_folder(self, folder, FALSE);
	memset(info, 0, sizeof *info);
	info->uidvalidity = found ? found->uidvalidity : 1;
	for (i = 0; found && i < found->messages->len; i++) {
		guint32 uid = g_array_index(found->messages, FakeMessage, i).uid;
		info->exists++;
		if (uid >= info->uidnext) info->uidnext = uid + 1;
	}
	if (!info->uidnext) info->uidnext = 1;
	return TRUE;
}
static GArray *fake_list(VentureImapClient *client, guint32 after_uid, guint max_entries, GCancellable *cancellable, GError **error)
{
	VentureFakeImapClient *self = VENTURE_FAKE_IMAP_CLIENT(client);
	FakeFolder *folder = self->selected ? fake_folder(self, self->selected, FALSE) : NULL;
	GArray *result;
	guint i;
	(void)cancellable;
	if (fake_expired(self, error)) return NULL;
	result = g_array_new(FALSE, FALSE, sizeof(VentureImapEntry));
	for (i = 0; folder && i < folder->messages->len; i++) {
		FakeMessage *message = &g_array_index(folder->messages, FakeMessage, i);
		VentureImapEntry entry;
		if (message->uid <= after_uid) continue;
		entry.uid = message->uid;
		entry.size = message->size ? message->size : g_bytes_get_size(message->raw);
		g_array_append_val(result, entry);
	}
	g_array_sort(result, entry_compare);
	if (result->len > max_entries) g_array_set_size(result, max_entries);
	return result;
}
static gboolean fake_since(VentureImapClient *client, GDateTime *since, guint32 *after_uid, GCancellable *cancellable, GError **error)
{
	VentureFakeImapClient *self = VENTURE_FAKE_IMAP_CLIENT(client);
	g_autoptr(GArray) entries = fake_list(client, 0, G_MAXUINT, cancellable, error);
	guint i;
	if (!entries) return FALSE;
	*after_uid = entries->len ? g_array_index(entries, VentureImapEntry, entries->len - 1).uid : 0;
	for (i = 0; i < entries->len; i++) {
		guint32 uid = g_array_index(entries, VentureImapEntry, i).uid;
		FakeMessage *message = fake_message(self, self->selected, uid);
		/* An undated message was received now, which is after any since. */
		if (!message->date || g_date_time_compare(message->date, since) >= 0) { *after_uid = uid - 1; break; }
	}
	return TRUE;
}
static FakeMessage *fake_fetchable(VentureFakeImapClient *self, guint32 uid, GError **error)
{
	FakeMessage *message;
	GError *injected = g_hash_table_lookup(self->fetch_errors, GUINT_TO_POINTER(uid));
	if (fake_expired(self, error)) return NULL;
	if (injected) { g_propagate_error(error, g_error_copy(injected)); return NULL; }
	message = fake_message(self, self->selected, uid);
	if (!message) g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_MAIL_PERMANENT, "IMAP: the server returned no content for UID %u", uid);
	else self->fetches++;
	return message;
}
static GBytes *fake_fetch(VentureImapClient *client, guint32 uid, GCancellable *cancellable, GError **error)
{
	FakeMessage *message = fake_fetchable(VENTURE_FAKE_IMAP_CLIENT(client), uid, error);
	GBytes *response = message ? g_bytes_ref(message->raw) : NULL;
	(void)cancellable;
	if (response) g_signal_emit(client, fake_response_ready, 0);
	return response;
}
static GBytes *fake_fetch_truncated(VentureImapClient *client, guint32 uid, gsize text_limit, GCancellable *cancellable, GError **error)
{
	FakeMessage *message = fake_fetchable(VENTURE_FAKE_IMAP_CLIENT(client), uid, error);
	const gchar *data, *split;
	gsize length, header;
	GBytes *response;
	(void)cancellable;
	if (!message) return NULL;
	data = g_bytes_get_data(message->raw, &length);
	split = g_strstr_len(data, (gssize)length, "\r\n\r\n");
	header = split ? (gsize)(split - data) + 4 : length;
	response = g_bytes_new(data, header + MIN(text_limit, length - header));
	g_signal_emit(client, fake_response_ready, 0);
	return response;
}
static void fake_set_deadline(VentureImapClient *client, gint64 monotonic_deadline)
{
	VENTURE_FAKE_IMAP_CLIENT(client)->deadline = monotonic_deadline;
}
static void fake_disconnect(VentureImapClient *client)
{
	VentureFakeImapClient *self = VENTURE_FAKE_IMAP_CLIENT(client);
	self->connected = FALSE;
	g_clear_pointer(&self->selected, g_free);
}
static void fake_iface_init(VentureImapClientInterface *iface)
{
	iface->connect = fake_connect; iface->select = fake_select; iface->list = fake_list; iface->since = fake_since;
	iface->fetch = fake_fetch; iface->fetch_truncated = fake_fetch_truncated; iface->set_deadline = fake_set_deadline;
	iface->disconnect = fake_disconnect;
}
VentureFakeImapClient *venture_fake_imap_client_new(void) { return g_object_new(VENTURE_TYPE_FAKE_IMAP_CLIENT, NULL); }
void venture_fake_imap_client_add_bytes(VentureFakeImapClient *self, const gchar *folder, guint32 uid, GBytes *raw)
{
	FakeMessage message;
	g_return_if_fail(VENTURE_IS_FAKE_IMAP_CLIENT(self));
	memset(&message, 0, sizeof message);
	message.uid = uid;
	message.raw = g_bytes_ref(raw);
	g_array_append_val(fake_folder(self, folder, TRUE)->messages, message);
}
void venture_fake_imap_client_add_message(VentureFakeImapClient *self, const gchar *folder, guint32 uid, const gchar *raw)
{
	g_autoptr(GBytes) bytes = g_bytes_new(raw, strlen(raw));
	venture_fake_imap_client_add_bytes(self, folder, uid, bytes);
}
void venture_fake_imap_client_clear_folder(VentureFakeImapClient *self, const gchar *folder)
{
	FakeFolder *found;
	guint i;
	g_return_if_fail(VENTURE_IS_FAKE_IMAP_CLIENT(self));
	found = fake_folder(self, folder, FALSE);
	for (i = 0; found && i < found->messages->len; i++) fake_message_clear(&g_array_index(found->messages, FakeMessage, i));
	if (found) g_array_set_size(found->messages, 0);
}
void venture_fake_imap_client_set_uidvalidity(VentureFakeImapClient *self, const gchar *folder, guint32 uidvalidity)
{
	g_return_if_fail(VENTURE_IS_FAKE_IMAP_CLIENT(self));
	fake_folder(self, folder, TRUE)->uidvalidity = uidvalidity;
}
void venture_fake_imap_client_set_size(VentureFakeImapClient *self, const gchar *folder, guint32 uid, guint64 size)
{
	FakeMessage *message;
	g_return_if_fail(VENTURE_IS_FAKE_IMAP_CLIENT(self));
	message = fake_message(self, folder, uid);
	g_return_if_fail(message != NULL);
	message->size = size;
}
void venture_fake_imap_client_set_internal_date(VentureFakeImapClient *self, const gchar *folder, guint32 uid, GDateTime *date)
{
	FakeMessage *message;
	g_return_if_fail(VENTURE_IS_FAKE_IMAP_CLIENT(self));
	message = fake_message(self, folder, uid);
	g_return_if_fail(message != NULL);
	g_clear_pointer(&message->date, g_date_time_unref);
	message->date = date ? g_date_time_ref(date) : NULL;
}
void venture_fake_imap_client_set_connect_error(VentureFakeImapClient *self, const GError *error)
{
	g_return_if_fail(VENTURE_IS_FAKE_IMAP_CLIENT(self));
	g_clear_error(&self->connect_error);
	if (error) self->connect_error = g_error_copy(error);
}
void venture_fake_imap_client_set_fetch_error(VentureFakeImapClient *self, guint32 uid, const GError *error)
{
	g_return_if_fail(VENTURE_IS_FAKE_IMAP_CLIENT(self));
	if (error) g_hash_table_replace(self->fetch_errors, GUINT_TO_POINTER(uid), g_error_copy(error));
	else g_hash_table_remove(self->fetch_errors, GUINT_TO_POINTER(uid));
}
void venture_fake_imap_client_refuse_folder(VentureFakeImapClient *self, const gchar *folder)
{
	g_return_if_fail(VENTURE_IS_FAKE_IMAP_CLIENT(self));
	g_hash_table_add(self->refused, g_strdup(folder));
}
gint venture_fake_imap_client_get_connects(VentureFakeImapClient *self)
{
	g_return_val_if_fail(VENTURE_IS_FAKE_IMAP_CLIENT(self), 0);
	return self->connects;
}
gint venture_fake_imap_client_get_fetches(VentureFakeImapClient *self)
{
	g_return_val_if_fail(VENTURE_IS_FAKE_IMAP_CLIENT(self), 0);
	return self->fetches;
}

/* --- Socket: the smallest IMAP4rev1 dialogue that does the job ----------- */
#define VENTURE_IMAP_MAX_LINE ((gsize)8192)
/* Larger than a line, so a line that fills it is known to be too long
 * before anything was allocated for it. */
#define VENTURE_IMAP_BUFFER ((gsize)32768)
#define VENTURE_IMAP_READ_TIMEOUT 60
#define VENTURE_IMAP_MAX_HEADER ((gsize)(1024 * 1024))
/* Literals in a reply that is not a message body: nothing this client asks
 * for needs more, so anything larger is a confused or hostile server. */
#define VENTURE_IMAP_MAX_SMALL_LITERAL ((gsize)65536)
#define VENTURE_IMAP_MAX_SEGMENTS 16
#define VENTURE_IMAP_MAX_REPLIES 20000
/* Everything one command may make us hold, beyond its largest literal:
 * room for headers and a listing. Per-literal and per-reply caps alone let
 * a server send twenty thousand replies of twenty megabytes each. */
#define VENTURE_IMAP_MAX_EXTRA_BYTES ((gsize)(4 * 1024 * 1024))
#define VENTURE_IMAP_LIST_WINDOW ((guint32)500)
typedef enum { IMAP_STATUS_OK, IMAP_STATUS_NO, IMAP_STATUS_BAD } ImapStatus;
/* One untagged reply: its text, with each literal read out of line and
 * labelled by the item name that preceded it (BODY[], BODY[HEADER], ...). */
typedef struct { GString *text; GPtrArray *labels; GPtrArray *literals; } ImapReply;
struct _VentureSocketImapClient {
	GObject parent_instance;
	GSocketConnection *connection;
	GIOStream *stream;
	GBufferedInputStream *in;
	GOutputStream *out;
	GHashTable *capabilities; /* upper-cased atoms */
	VentureImapFolderInfo info;
	gint64 deadline;
	guint tag;
	gboolean selected;
	gboolean allow_plaintext;
};
static void socket_iface_init(VentureImapClientInterface *iface);
G_DEFINE_FINAL_TYPE_WITH_CODE(VentureSocketImapClient, venture_socket_imap_client, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(VENTURE_TYPE_IMAP_CLIENT, socket_iface_init))
static void imap_reply_free(gpointer data)
{
	ImapReply *reply = data;
	g_string_free(reply->text, TRUE);
	g_ptr_array_unref(reply->labels);
	g_ptr_array_unref(reply->literals);
	g_free(reply);
}
static void socket_close(VentureSocketImapClient *self)
{
	if (self->stream) g_io_stream_close(self->stream, NULL, NULL);
	g_clear_object(&self->in);
	g_clear_object(&self->out);
	g_clear_object(&self->stream);
	g_clear_object(&self->connection);
	g_hash_table_remove_all(self->capabilities);
	memset(&self->info, 0, sizeof self->info);
	self->selected = FALSE;
}
static void socket_finalize(GObject *object)
{
	VentureSocketImapClient *self = VENTURE_SOCKET_IMAP_CLIENT(object);
	socket_close(self);
	g_hash_table_unref(self->capabilities);
	G_OBJECT_CLASS(venture_socket_imap_client_parent_class)->finalize(object);
}
static void socket_set_property(GObject *object, guint id, const GValue *value, GParamSpec *pspec)
{
	if (id == 1) VENTURE_SOCKET_IMAP_CLIENT(object)->allow_plaintext = g_value_get_boolean(value);
	else G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
}
static void socket_get_property(GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
	if (id == 1) g_value_set_boolean(value, VENTURE_SOCKET_IMAP_CLIENT(object)->allow_plaintext);
	else G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
}
static void venture_socket_imap_client_class_init(VentureSocketImapClientClass *klass)
{
	GObjectClass *object = G_OBJECT_CLASS(klass);
	object->finalize = socket_finalize;
	object->set_property = socket_set_property;
	object->get_property = socket_get_property;
	g_object_class_install_property(object, 1, g_param_spec_boolean("allow-plaintext", "Allow plaintext",
		"Tests only: permit security none against a scripted loopback server", FALSE,
		G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
}
static void venture_socket_imap_client_init(VentureSocketImapClient *self)
{
	self->capabilities = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
}
/* A refusal on a live session: the folder or the credentials, not the wire.
 * Server text is never echoed, because a rejected LOGIN line can carry the
 * credential; only a response code from a fixed vocabulary is named. */
static gboolean socket_refuse(GError **error, VentureError code, const gchar *what, const gchar *response_code)
{
	if (response_code) g_set_error(error, VENTURE_ERROR, code, "IMAP: %s [%s]", what, response_code);
	else g_set_error(error, VENTURE_ERROR, code, "IMAP: %s", what);
	return FALSE;
}
/* The session is unusable: close it so the next call fails fast rather than
 * reading the remains of a reply as the answer to a new command. */
static gboolean socket_broken(VentureSocketImapClient *self, GError **error, VentureError code, const gchar *what)
{
	socket_close(self);
	g_set_error(error, VENTURE_ERROR, code, "IMAP: %s", what);
	return FALSE;
}
static gboolean socket_io_failed(VentureSocketImapClient *self, GError **error)
{
	gboolean expired = self->deadline && g_get_monotonic_time() >= self->deadline;
	socket_close(self);
	if (expired && error && *error && g_error_matches(*error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT)) {
		g_clear_error(error);
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_TIMEOUT, "IMAP: time budget exhausted");
	}
	if (error && !*error) g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NETWORK, "IMAP: connection closed");
	return FALSE;
}
static guint socket_seconds_left(VentureSocketImapClient *self)
{
	gint64 left;
	if (!self->deadline) return VENTURE_IMAP_READ_TIMEOUT;
	left = self->deadline - g_get_monotonic_time();
	if (left <= 0) return 0;
	return (guint)MIN((gint64)VENTURE_IMAP_READ_TIMEOUT, (left + G_USEC_PER_SEC - 1) / G_USEC_PER_SEC);
}
/* Every read and write is re-armed with what is left of the deadline, so a
 * server trickling a byte just inside the per-read timeout still ends on
 * time rather than holding the main loop for as long as it likes. */
static gboolean socket_arm(VentureSocketImapClient *self, GError **error)
{
	guint seconds = socket_seconds_left(self);
	if (!self->connection) return socket_broken(self, error, VENTURE_ERROR_NETWORK, "not connected");
	if (!seconds) return socket_broken(self, error, VENTURE_ERROR_TIMEOUT, "time budget exhausted");
	g_socket_set_timeout(g_socket_connection_get_socket(self->connection), seconds);
	return TRUE;
}
/* Reads one CRLF line without ever buffering more than the limit: the
 * newline is looked for in what is already buffered, and a buffer holding
 * more than a line's worth with no newline is refused before any copy. */
static gchar *socket_read_line(VentureSocketImapClient *self, GCancellable *cancellable, GError **error)
{
	if (!self->in) { socket_broken(self, error, VENTURE_ERROR_NETWORK, "not connected"); return NULL; }
	for (;;) {
		gsize available = 0;
		const guint8 *buffer = g_buffered_input_stream_peek_buffer(self->in, &available);
		const guint8 *newline = available ? memchr(buffer, '\n', MIN(available, VENTURE_IMAP_MAX_LINE + 2)) : NULL;
		gssize filled;
		if (newline) {
			gsize length = (gsize)(newline - buffer);
			gchar *line = g_strndup((const gchar *)buffer, length);
			gsize text = strlen(line);
			if (text && line[text - 1] == '\r') line[text - 1] = '\0';
			if (g_input_stream_skip(G_INPUT_STREAM(self->in), length + 1, cancellable, error) < 0) { g_free(line); socket_io_failed(self, error); return NULL; }
			return line;
		}
		if (available > VENTURE_IMAP_MAX_LINE + 1) { socket_broken(self, error, VENTURE_ERROR_NETWORK, "line too long"); return NULL; }
		if (!socket_arm(self, error)) return NULL;
		filled = g_buffered_input_stream_fill(self->in, -1, cancellable, error);
		if (filled <= 0) { socket_io_failed(self, error); return NULL; }
	}
}
static gboolean socket_read_exact(VentureSocketImapClient *self, guint8 *into, gsize count, GCancellable *cancellable, GError **error)
{
	gsize done = 0;
	while (done < count) {
		gssize n;
		if (!socket_arm(self, error)) return FALSE;
		n = g_input_stream_read(G_INPUT_STREAM(self->in), into + done, count - done, cancellable, error);
		if (n <= 0) return socket_io_failed(self, error);
		done += (gsize)n;
	}
	return TRUE;
}
static gboolean socket_send(VentureSocketImapClient *self, const gchar *tag, const gchar *text, GCancellable *cancellable, GError **error)
{
	GString *line = g_string_new(NULL);
	gboolean ok;
	if (tag) g_string_append_printf(line, "%s ", tag);
	g_string_append(line, text);
	g_string_append(line, "\r\n");
	ok = socket_arm(self, error) && g_output_stream_write_all(self->out, line->str, line->len, NULL, cancellable, error);
	/* The line may hold a password or its base64; do not leave it in freed memory. */
	memset(line->str, 0, line->len);
	g_string_free(line, TRUE);
	return ok || socket_io_failed(self, error);
}
static void socket_wire_streams(VentureSocketImapClient *self, GIOStream *stream)
{
	g_clear_object(&self->in);
	g_clear_object(&self->out);
	self->in = G_BUFFERED_INPUT_STREAM(g_buffered_input_stream_new_sized(g_io_stream_get_input_stream(stream), VENTURE_IMAP_BUFFER));
	/* Dropping the plaintext reader at STARTTLS must not close the socket's
	 * input stream, which the TLS connection is about to read from. */
	g_filter_input_stream_set_close_base_stream(G_FILTER_INPUT_STREAM(self->in), FALSE);
	self->out = g_object_ref(g_io_stream_get_output_stream(stream));
}
static void socket_note_capabilities(VentureSocketImapClient *self, const gchar *list)
{
	g_auto(GStrv) atoms = g_strsplit_set(list, " ]", -1);
	guint i;
	g_hash_table_remove_all(self->capabilities);
	for (i = 0; atoms[i]; i++) if (*atoms[i]) g_hash_table_add(self->capabilities, g_ascii_strup(atoms[i], -1));
}
static gboolean socket_has_capability(VentureSocketImapClient *self, const gchar *name)
{
	return g_hash_table_contains(self->capabilities, name);
}
static const gchar *const imap_response_codes[] = {
	"ALERT", "ALREADYEXISTS", "AUTHENTICATIONFAILED", "AUTHORIZATIONFAILED", "BADCHARSET", "CANNOT", "CLIENTBUG",
	"CONTACTADMIN", "CORRUPTION", "EXPIRED", "EXPUNGEISSUED", "INUSE", "LIMIT", "NONEXISTENT", "NOPERM", "OVERQUOTA",
	"PARSE", "PRIVACYREQUIRED", "SERVERBUG", "TRYCREATE", "UNAVAILABLE", NULL
};
/* @rest starts after the status word. Returns a constant, never server text. */
static const gchar *imap_response_code(const gchar *rest)
{
	guint i;
	while (*rest == ' ') rest++;
	if (*rest != '[') return NULL;
	for (i = 0; imap_response_codes[i]; i++) {
		gsize length = strlen(imap_response_codes[i]);
		if (!g_ascii_strncasecmp(rest + 1, imap_response_codes[i], length) && (rest[1 + length] == ']' || rest[1 + length] == ' '))
			return imap_response_codes[i];
	}
	return NULL;
}
static gchar *literal_label(const gchar *text, const gchar *brace)
{
	const gchar *end = brace, *start;
	while (end > text && end[-1] == ' ') end--;
	start = end;
	while (start > text && start[-1] != ' ' && start[-1] != '(') start--;
	return g_ascii_strup(start, end - start);
}
/* A line ending in {n} is followed by n raw bytes and then the rest of the
 * reply on the next line, which may announce another literal. */
static gboolean socket_collect_literals(VentureSocketImapClient *self, ImapReply *reply, gsize max_literal, gsize *retained, GCancellable *cancellable, GError **error)
{
	guint segments = 0;
	for (;;) {
		const gchar *text = reply->text->str, *brace;
		gchar *end = NULL;
		guint64 size;
		guint8 *bytes;
		if (!reply->text->len || text[reply->text->len - 1] != '}') return TRUE;
		brace = strrchr(text, '{');
		if (!brace) return TRUE;
		size = g_ascii_strtoull(brace + 1, &end, 10);
		if (end == brace + 1 || *end != '}') return TRUE;
		if (++segments > VENTURE_IMAP_MAX_SEGMENTS) return socket_broken(self, error, VENTURE_ERROR_NETWORK, "reply has too many literals");
		/* The unread bytes are still on the wire, so the session cannot
		 * continue; the caller learns this one message is the problem. */
		if (size > max_literal) return socket_broken(self, error, VENTURE_ERROR_MAIL_PERMANENT, "message too large");
		if (*retained + (gsize)size > max_literal + VENTURE_IMAP_MAX_EXTRA_BYTES)
			return socket_broken(self, error, VENTURE_ERROR_NETWORK, "reply too large");
		*retained += (gsize)size;
		bytes = g_malloc((gsize)size ? (gsize)size : 1);
		if (size && !socket_read_exact(self, bytes, (gsize)size, cancellable, error)) { g_free(bytes); return FALSE; }
		g_ptr_array_add(reply->labels, literal_label(text, brace));
		g_ptr_array_add(reply->literals, g_bytes_new_take(bytes, (gsize)size));
		{
			g_autofree gchar *rest = socket_read_line(self, cancellable, error);
			if (!rest) return FALSE;
			if (reply->text->len > VENTURE_IMAP_MAX_LINE * VENTURE_IMAP_MAX_SEGMENTS) return socket_broken(self, error, VENTURE_ERROR_NETWORK, "line too long");
			g_string_append(reply->text, rest);
		}
	}
}
/* Runs one tagged command. @parts[0] is the command and each "+"
 * continuation is answered with the next part, which is how a literal or a
 * SASL response goes out. Returns FALSE only when the session failed; a
 * NO or BAD is reported through @status for the caller to classify. */
static gboolean socket_run(VentureSocketImapClient *self, const gchar *const *parts, gsize max_literal, GPtrArray *replies, ImapStatus *status, const gchar **code, GCancellable *cancellable, GError **error)
{
	g_autofree gchar *tag = g_strdup_printf("A%04u", ++self->tag);
	guint next = 1, received = 0;
	gsize retained = 0;
	*status = IMAP_STATUS_BAD;
	*code = NULL;
	if (!self->out) return socket_broken(self, error, VENTURE_ERROR_NETWORK, "not connected");
	if (!socket_send(self, tag, parts[0], cancellable, error)) return FALSE;
	for (;;) {
		g_autofree gchar *line = socket_read_line(self, cancellable, error);
		if (!line) return FALSE;
		if (line[0] == '+' && (line[1] == ' ' || line[1] == '\0')) {
			if (!parts[next]) return socket_broken(self, error, VENTURE_ERROR_NETWORK, "unexpected continuation");
			if (!socket_send(self, NULL, parts[next++], cancellable, error)) return FALSE;
			continue;
		}
		if (g_str_has_prefix(line, tag) && line[strlen(tag)] == ' ') {
			const gchar *rest = line + strlen(tag) + 1;
			if (!g_ascii_strncasecmp(rest, "OK", 2) && (rest[2] == ' ' || !rest[2])) *status = IMAP_STATUS_OK;
			else if (!g_ascii_strncasecmp(rest, "NO", 2) && (rest[2] == ' ' || !rest[2])) *status = IMAP_STATUS_NO;
			rest += MIN(strlen(rest), (gsize)(*status == IMAP_STATUS_BAD ? 3 : 2));
			*code = imap_response_code(rest);
			if (*status == IMAP_STATUS_OK) {
				const gchar *capability = strstr(rest, "[CAPABILITY ");
				if (capability) socket_note_capabilities(self, capability + strlen("[CAPABILITY "));
			}
			return TRUE;
		}
		if (++received > VENTURE_IMAP_MAX_REPLIES) return socket_broken(self, error, VENTURE_ERROR_NETWORK, "too many replies");
		{
			ImapReply *reply = g_new0(ImapReply, 1);
			reply->text = g_string_new(line);
			reply->labels = g_ptr_array_new_with_free_func(g_free);
			reply->literals = g_ptr_array_new_with_free_func((GDestroyNotify)g_bytes_unref);
			if (!socket_collect_literals(self, reply, max_literal, &retained, cancellable, error)) { imap_reply_free(reply); return FALSE; }
			/* Only what is kept counts: a reply nobody asked to keep is freed below. */
			if (replies) {
				retained += reply->text->len;
				if (retained > max_literal + VENTURE_IMAP_MAX_EXTRA_BYTES) { imap_reply_free(reply); return socket_broken(self, error, VENTURE_ERROR_NETWORK, "reply too large"); }
			} else retained = 0;
			if (g_str_has_prefix(reply->text->str, "* CAPABILITY ")) socket_note_capabilities(self, reply->text->str + strlen("* CAPABILITY "));
			if (replies) g_ptr_array_add(replies, reply);
			else imap_reply_free(reply);
		}
	}
}
static gboolean socket_simple(VentureSocketImapClient *self, const gchar *command, GPtrArray *replies, gsize max_literal, ImapStatus *status, const gchar **code, GCancellable *cancellable, GError **error)
{
	const gchar *parts[2];
	parts[0] = command;
	parts[1] = NULL;
	return socket_run(self, parts, max_literal, replies, status, code, cancellable, error);
}
static gboolean socket_quoted_safe(const gchar *value)
{
	const gchar *p;
	for (p = value ? value : ""; *p; p++)
		if (*p == '\r' || *p == '\n') return FALSE;
	return TRUE;
}
static gchar *socket_quote(const gchar *value)
{
	GString *s = g_string_new("\"");
	const gchar *p;
	for (p = value ? value : ""; *p; p++) {
		if (*p == '"' || *p == '\\') g_string_append_c(s, '\\');
		g_string_append_c(s, *p);
	}
	g_string_append_c(s, '"');
	return g_string_free(s, FALSE);
}
static void socket_wipe(gchar *text)
{
	if (text) memset(text, 0, strlen(text));
}
/* An astring as a quoted string when it is 7-bit, otherwise as a literal:
 * a quoted string may only carry 7-bit text, so a non-ASCII password sent
 * quoted is a password the server reads differently. A literal ends the
 * current part; the server's "+" asks for the next. */
static GString *login_append_astring(GString *current, GPtrArray *parts, const gchar *value)
{
	const guchar *p;
	gboolean seven_bit = TRUE;
	for (p = (const guchar *)value; *p; p++) if (*p > 0x7f) seven_bit = FALSE;
	if (seven_bit) {
		gchar *quoted = socket_quote(value);
		g_string_append(current, quoted);
		socket_wipe(quoted);
		g_free(quoted);
		return current;
	}
	g_string_append_printf(current, "{%" G_GSIZE_FORMAT "}", strlen(value));
	g_ptr_array_add(parts, g_string_free(current, FALSE));
	return g_string_new(value);
}
static void parts_wipe_free(gpointer data)
{
	socket_wipe(data);
	g_free(data);
}
static gboolean socket_login(VentureSocketImapClient *self, const gchar *username, const gchar *secret, ImapStatus *status, const gchar **code, GCancellable *cancellable, GError **error)
{
	g_autoptr(GPtrArray) parts = g_ptr_array_new_with_free_func(parts_wipe_free);
	GString *current = g_string_new("LOGIN ");
	current = login_append_astring(current, parts, username ? username : "");
	g_string_append_c(current, ' ');
	current = login_append_astring(current, parts, secret);
	g_ptr_array_add(parts, g_string_free(current, FALSE));
	g_ptr_array_add(parts, NULL);
	return socket_run(self, (const gchar *const *)parts->pdata, VENTURE_IMAP_MAX_SMALL_LITERAL, NULL, status, code, cancellable, error);
}
/* RFC 4616 PLAIN: an empty authorization identity, the username and the
 * secret, NUL separated, base64 encoded. Unlike LOGIN it carries UTF-8
 * credentials unchanged. With SASL-IR the response rides on the command. */
static gboolean socket_auth_plain(VentureSocketImapClient *self, const gchar *username, const gchar *secret, ImapStatus *status, const gchar **code, GCancellable *cancellable, GError **error)
{
	gsize user_length = strlen(username ? username : ""), secret_length = strlen(secret);
	gsize total = user_length + secret_length + 2;
	guchar *message = g_malloc(total);
	gchar *encoded, *command = NULL;
	const gchar *parts[3];
	gboolean ok;
	message[0] = '\0';
	memcpy(message + 1, username ? username : "", user_length);
	message[1 + user_length] = '\0';
	memcpy(message + 2 + user_length, secret, secret_length);
	encoded = g_base64_encode(message, total);
	memset(message, 0, total);
	g_free(message);
	if (socket_has_capability(self, "SASL-IR")) {
		command = g_strconcat("AUTHENTICATE PLAIN ", encoded, NULL);
		parts[0] = command; parts[1] = NULL;
	} else {
		parts[0] = "AUTHENTICATE PLAIN"; parts[1] = encoded; parts[2] = NULL;
	}
	ok = socket_run(self, parts, VENTURE_IMAP_MAX_SMALL_LITERAL, NULL, status, code, cancellable, error);
	socket_wipe(command);
	socket_wipe(encoded);
	g_free(command);
	g_free(encoded);
	return ok;
}
/* PLAIN when offered, LOGIN otherwise unless the server disabled it. A
 * token-based mechanism such as XOAUTH2 would be one more branch here,
 * chosen by the account rather than by what the server advertises, since
 * it needs a refreshed token where these take the stored secret. */
static gboolean socket_authenticate(VentureSocketImapClient *self, const gchar *username, const gchar *secret, GCancellable *cancellable, GError **error)
{
	ImapStatus status;
	const gchar *code = NULL;
	gboolean ran;
	if (socket_has_capability(self, "AUTH=PLAIN")) ran = socket_auth_plain(self, username, secret, &status, &code, cancellable, error);
	else if (!socket_has_capability(self, "LOGINDISABLED")) ran = socket_login(self, username, secret, &status, &code, cancellable, error);
	else return socket_refuse(error, VENTURE_ERROR_UNSUPPORTED, "the server offers no authentication this client supports", NULL);
	if (!ran) return FALSE;
	if (status == IMAP_STATUS_OK) return TRUE;
	/* UNAVAILABLE is the server saying "later", not "wrong password":
	 * backing off is right, switching the account off is not. */
	if (!g_strcmp0(code, "UNAVAILABLE")) return socket_refuse(error, VENTURE_ERROR_NETWORK, "authentication unavailable", code);
	return socket_refuse(error, VENTURE_ERROR_UNAUTHENTICATED, "authentication refused", code);
}
static gboolean socket_connect(VentureImapClient *client, const gchar *host, guint16 port, const gchar *security, const gchar *username, const gchar *secret, GCancellable *cancellable, GError **error)
{
	VentureSocketImapClient *self = VENTURE_SOCKET_IMAP_CLIENT(client);
	g_autoptr(GSocketClient) socket_client = g_socket_client_new();
	g_autofree gchar *greeting = NULL;
	gboolean tls = !g_strcmp0(security, "tls"), starttls = !g_strcmp0(security, "starttls"), plain = !g_strcmp0(security, "none");
	gboolean preauth;
	ImapStatus status;
	const gchar *code = NULL;
	guint seconds;
	socket_close(self);
	if (!tls && !starttls && !plain) return socket_refuse(error, VENTURE_ERROR_CONFIG, "security must be tls or starttls", NULL);
	/* This client always authenticates. LOGIN over none would put the
	 * password on the wire; an empty username used to skip that check. */
	if (plain && !self->allow_plaintext) return socket_refuse(error, VENTURE_ERROR_CONFIG, "authentication over an unencrypted connection is refused", NULL);
	if (!secret || !socket_quoted_safe(username) || !socket_quoted_safe(secret)) return socket_refuse(error, VENTURE_ERROR_CONFIG, "credentials contain a line break", NULL);
	seconds = socket_seconds_left(self);
	if (!seconds) return socket_refuse(error, VENTURE_ERROR_TIMEOUT, "time budget exhausted", NULL);
	g_socket_client_set_timeout(socket_client, seconds);
	if (tls) g_socket_client_set_tls(socket_client, TRUE);
	self->connection = g_socket_client_connect_to_host(socket_client, host, port, cancellable, error);
	if (!self->connection) return socket_io_failed(self, error);
	self->stream = g_object_ref(G_IO_STREAM(self->connection));
	socket_wire_streams(self, self->stream);
	greeting = socket_read_line(self, cancellable, error);
	if (!greeting) return FALSE;
	preauth = g_str_has_prefix(greeting, "* PREAUTH");
	if (!preauth && !g_str_has_prefix(greeting, "* OK")) return socket_broken(self, error, VENTURE_ERROR_NETWORK, "unexpected greeting");
	if (starttls) {
		g_autoptr(GSocketConnectable) identity = g_network_address_new(host, port);
		GIOStream *upgraded;
		/* A session that is already authenticated cannot negotiate TLS,
		 * so PREAUTH here is a downgrade, whoever sent it. */
		if (preauth) return socket_broken(self, error, VENTURE_ERROR_NETWORK, "the server skipped authentication before STARTTLS");
		if (!socket_simple(self, "STARTTLS", NULL, VENTURE_IMAP_MAX_SMALL_LITERAL, &status, &code, cancellable, error)) return FALSE;
		if (status != IMAP_STATUS_OK) { socket_close(self); return socket_refuse(error, VENTURE_ERROR_NETWORK, "STARTTLS refused", code); }
		upgraded = g_tls_client_connection_new(self->stream, identity, error);
		if (!upgraded) return socket_io_failed(self, error);
		if (!socket_arm(self, error)) { g_object_unref(upgraded); return FALSE; }
		if (!g_tls_connection_handshake(G_TLS_CONNECTION(upgraded), cancellable, error)) { g_object_unref(upgraded); return socket_io_failed(self, error); }
		g_object_unref(self->stream);
		self->stream = upgraded;
		socket_wire_streams(self, self->stream);
	}
	/* Asked after TLS: anything advertised before it may have been written
	 * by whoever sat on the plaintext connection. */
	g_hash_table_remove_all(self->capabilities);
	if (!socket_simple(self, "CAPABILITY", NULL, VENTURE_IMAP_MAX_SMALL_LITERAL, &status, &code, cancellable, error)) return FALSE;
	if (preauth) return TRUE;
	if (!socket_authenticate(self, username, secret, cancellable, error)) { socket_close(self); return FALSE; }
	return TRUE;
}
static gboolean parse_uint32(const gchar *text, guint32 *value)
{
	gchar *end = NULL;
	guint64 parsed;
	if (!g_ascii_isdigit(*text)) return FALSE;
	parsed = g_ascii_strtoull(text, &end, 10);
	if (parsed > G_MAXUINT32) return FALSE;
	*value = (guint32)parsed;
	return TRUE;
}
static gboolean socket_select(VentureImapClient *client, const gchar *folder, VentureImapFolderInfo *info, GCancellable *cancellable, GError **error)
{
	VentureSocketImapClient *self = VENTURE_SOCKET_IMAP_CLIENT(client);
	g_autoptr(GPtrArray) replies = g_ptr_array_new_with_free_func(imap_reply_free);
	g_autofree gchar *wire = NULL, *quoted = NULL, *command = NULL;
	ImapStatus status;
	const gchar *code = NULL;
	guint i;
	if (!self->out) return socket_broken(self, error, VENTURE_ERROR_NETWORK, "not connected");
	if (!socket_quoted_safe(folder)) return socket_refuse(error, VENTURE_ERROR_CONFIG, "folder contains a line break", NULL);
	wire = venture_imap_utf7_encode(folder);
	if (!wire) return socket_refuse(error, VENTURE_ERROR_CONFIG, "folder name is not UTF-8", NULL);
	quoted = socket_quote(wire);
	command = g_strdup_printf("EXAMINE %s", quoted);
	self->selected = FALSE;
	memset(&self->info, 0, sizeof self->info);
	if (!socket_simple(self, command, replies, VENTURE_IMAP_MAX_SMALL_LITERAL, &status, &code, cancellable, error)) return FALSE;
	if (status != IMAP_STATUS_OK) return socket_refuse(error, VENTURE_ERROR_NOT_FOUND, "folder refused", code);
	for (i = 0; i < replies->len; i++) {
		const gchar *text = ((ImapReply *)g_ptr_array_index(replies, i))->text->str;
		const gchar *found;
		if ((found = strstr(text, "[UIDVALIDITY "))) parse_uint32(found + strlen("[UIDVALIDITY "), &self->info.uidvalidity);
		else if ((found = strstr(text, "[UIDNEXT "))) parse_uint32(found + strlen("[UIDNEXT "), &self->info.uidnext);
		else if (g_str_has_prefix(text, "* ") && g_str_has_suffix(text, " EXISTS")) parse_uint32(text + 2, &self->info.exists);
	}
	self->selected = TRUE;
	*info = self->info;
	return TRUE;
}
/* The number after @name among a FETCH reply's items, e.g. UID or RFC822.SIZE. */
static gboolean fetch_item(const gchar *text, const gchar *name, guint64 *value)
{
	const gchar *open = strchr(text, '(');
	g_auto(GStrv) tokens = NULL;
	guint i, j;
	if (!open) return FALSE;
	tokens = g_strsplit_set(open + 1, " ()", -1);
	for (i = 0; tokens[i]; i++) {
		gchar *end = NULL;
		if (g_ascii_strcasecmp(tokens[i], name)) continue;
		for (j = i + 1; tokens[j] && !*tokens[j]; j++) ;
		if (!tokens[j] || !g_ascii_isdigit(*tokens[j])) return FALSE;
		*value = g_ascii_strtoull(tokens[j], &end, 10);
		return end && !*end;
	}
	return FALSE;
}
/* The message sequence number of a "* n FETCH" reply. */
static gboolean fetch_sequence(const gchar *text, guint32 *sequence)
{
	return g_str_has_prefix(text, "* ") && strstr(text, " FETCH ") && parse_uint32(text + 2, sequence);
}
/* The UID of one message by sequence number, or 0 when the server named none. */
static gboolean socket_sequence_uid(VentureSocketImapClient *self, guint32 sequence, guint32 *uid, GCancellable *cancellable, GError **error)
{
	g_autoptr(GPtrArray) replies = g_ptr_array_new_with_free_func(imap_reply_free);
	g_autofree gchar *command = g_strdup_printf("FETCH %u (UID)", sequence);
	ImapStatus status;
	const gchar *code = NULL;
	guint i;
	*uid = 0;
	if (!socket_simple(self, command, replies, VENTURE_IMAP_MAX_SMALL_LITERAL, &status, &code, cancellable, error)) return FALSE;
	if (status != IMAP_STATUS_OK) return socket_refuse(error, VENTURE_ERROR_NETWORK, "FETCH refused", code);
	for (i = 0; i < replies->len; i++) {
		const gchar *text = ((ImapReply *)g_ptr_array_index(replies, i))->text->str;
		guint64 found = 0;
		guint32 number;
		if (fetch_sequence(text, &number) && number == sequence && fetch_item(text, "UID", &found) && found <= G_MAXUINT32) *uid = (guint32)found;
	}
	return TRUE;
}
/* The lowest UID at or above @at_least, or 0 when every UID is below it.
 * UIDs rise with sequence numbers, so this is a binary search of small
 * replies: a gap of millions of UIDs costs a few dozen round trips rather
 * than a window per few hundred UIDs, which never reached the far side
 * within the time budget and left the cursor where it was. */
static gboolean socket_next_uid(VentureSocketImapClient *self, guint32 at_least, guint32 *uid, GCancellable *cancellable, GError **error)
{
	guint32 low = 1, high = self->info.exists, found = 0;
	*uid = 0;
	if (!high) return TRUE;
	if (!socket_sequence_uid(self, high, &found, cancellable, error)) return FALSE;
	if (found < at_least) return TRUE;
	while (low < high) {
		guint32 middle = low + (high - low) / 2;
		if (!socket_sequence_uid(self, middle, &found, cancellable, error)) return FALSE;
		if (found >= at_least) high = middle;
		else low = middle + 1;
	}
	return socket_sequence_uid(self, low, uid, cancellable, error);
}
/* Pages UID ranges rather than asking for every UID above the mark in one
 * SEARCH reply, which is a single line that grows with the mailbox and was
 * refused at about 1,200 messages. A window that finds nothing jumps to the
 * next UID that exists; the window starting there is still a UID range, so
 * a message expunged meanwhile shifts nothing out of it. */
static GArray *socket_list(VentureImapClient *client, guint32 after_uid, guint max_entries, GCancellable *cancellable, GError **error)
{
	VentureSocketImapClient *self = VENTURE_SOCKET_IMAP_CLIENT(client);
	GArray *result;
	guint32 upper = 0, start;
	if (!self->selected) { socket_broken(self, error, VENTURE_ERROR_NETWORK, "no folder selected"); return NULL; }
	result = g_array_new(FALSE, FALSE, sizeof(VentureImapEntry));
	if (!self->info.exists || after_uid == G_MAXUINT32 || !max_entries) return result;
	if (self->info.uidnext) upper = self->info.uidnext - 1;
	else if (!socket_sequence_uid(self, self->info.exists, &upper, cancellable, error)) { g_array_unref(result); return NULL; }
	for (start = after_uid + 1; start <= upper && start && result->len < max_entries;) {
		g_autoptr(GPtrArray) replies = g_ptr_array_new_with_free_func(imap_reply_free);
		g_autoptr(GArray) found = g_array_new(FALSE, FALSE, sizeof(VentureImapEntry));
		g_autofree gchar *command = NULL;
		guint32 end = upper - start < VENTURE_IMAP_LIST_WINDOW - 1 ? upper : start + VENTURE_IMAP_LIST_WINDOW - 1;
		ImapStatus status;
		const gchar *code = NULL;
		guint i;
		command = g_strdup_printf("UID FETCH %u:%u (UID RFC822.SIZE)", start, end);
		if (!socket_simple(self, command, replies, VENTURE_IMAP_MAX_SMALL_LITERAL, &status, &code, cancellable, error)) { g_array_unref(result); return NULL; }
		if (status != IMAP_STATUS_OK) { g_array_unref(result); socket_refuse(error, VENTURE_ERROR_NETWORK, "UID FETCH refused", code); return NULL; }
		for (i = 0; i < replies->len; i++) {
			const gchar *text = ((ImapReply *)g_ptr_array_index(replies, i))->text->str;
			guint64 uid = 0, size = 0;
			VentureImapEntry entry;
			guint32 sequence;
			/* Unsolicited flag updates for other messages are replies too. */
			if (!fetch_sequence(text, &sequence) || !fetch_item(text, "UID", &uid) || !fetch_item(text, "RFC822.SIZE", &size)) continue;
			if (uid < start || uid > end) continue;
			entry.uid = (guint32)uid;
			entry.size = size;
			g_array_append_val(found, entry);
		}
		g_array_sort(found, entry_compare);
		for (i = 0; i < found->len && result->len < max_entries; i++) g_array_append_val(result, g_array_index(found, VentureImapEntry, i));
		if (end == G_MAXUINT32) break;
		if (!found->len && end < upper) {
			guint32 next = 0;
			if (!socket_next_uid(self, end + 1, &next, cancellable, error)) { g_array_unref(result); return NULL; }
			if (!next) break;
			start = MAX(next, end + 1);
		} else start = end + 1;
	}
	return result;
}
static GDateTime *parse_internal_date(const gchar *text)
{
	static const gchar *const months[] = { "jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec" };
	gchar month[4];
	gint day, year, hour, minute, second, zone, index;
	g_autoptr(GTimeZone) tz = NULL;
	if (sscanf(text, "%d-%3s-%d %d:%d:%d %d", &day, month, &year, &hour, &minute, &second, &zone) != 7) return NULL;
	for (index = 0; index < 12; index++) if (!g_ascii_strcasecmp(month, months[index])) break;
	if (index == 12 || ABS(zone) > 2400) return NULL;
	tz = g_time_zone_new_offset((zone < 0 ? -1 : 1) * (ABS(zone) / 100 * 3600 + ABS(zone) % 100 * 60));
	return g_date_time_new(tz, year, index + 1, day, hour, minute, second);
}
/* UID and INTERNALDATE of one message by sequence number. */
static gboolean socket_sequence_date(VentureSocketImapClient *self, guint32 sequence, guint32 *uid, GDateTime **date, GCancellable *cancellable, GError **error)
{
	g_autoptr(GPtrArray) replies = g_ptr_array_new_with_free_func(imap_reply_free);
	g_autofree gchar *command = g_strdup_printf("FETCH %u (UID INTERNALDATE)", sequence);
	ImapStatus status;
	const gchar *code = NULL;
	guint i;
	if (!socket_simple(self, command, replies, VENTURE_IMAP_MAX_SMALL_LITERAL, &status, &code, cancellable, error)) return FALSE;
	if (status != IMAP_STATUS_OK) return socket_refuse(error, VENTURE_ERROR_NETWORK, "FETCH refused", code);
	for (i = 0; i < replies->len; i++) {
		const gchar *text = ((ImapReply *)g_ptr_array_index(replies, i))->text->str;
		const gchar *quoted = strstr(text, "INTERNALDATE \"");
		guint64 found = 0;
		guint32 number;
		if (!fetch_sequence(text, &number) || number != sequence || !quoted || !fetch_item(text, "UID", &found) || found > G_MAXUINT32) continue;
		*date = parse_internal_date(quoted + strlen("INTERNALDATE \""));
		*uid = (guint32)found;
		if (*date) return TRUE;
	}
	return socket_refuse(error, VENTURE_ERROR_NETWORK, "the server reported no date for a message", NULL);
}
/* Messages are appended in arrival order, so INTERNALDATE rises with the
 * sequence number and a binary search finds the first one on or after
 * @since in a handful of small replies. A SEARCH SINCE reply would name
 * every matching UID on one line. */
static gboolean socket_since(VentureImapClient *client, GDateTime *since, guint32 *after_uid, GCancellable *cancellable, GError **error)
{
	VentureSocketImapClient *self = VENTURE_SOCKET_IMAP_CLIENT(client);
	g_autoptr(GDateTime) date = NULL;
	guint32 low = 1, high, uid = 0;
	if (!self->selected) return socket_broken(self, error, VENTURE_ERROR_NETWORK, "no folder selected");
	*after_uid = 0;
	if (!self->info.exists) return TRUE;
	high = self->info.exists;
	if (!socket_sequence_date(self, high, &uid, &date, cancellable, error)) return FALSE;
	if (g_date_time_compare(date, since) < 0) { *after_uid = uid; return TRUE; }
	while (low < high) {
		guint32 middle = low + (high - low) / 2;
		g_clear_pointer(&date, g_date_time_unref);
		if (!socket_sequence_date(self, middle, &uid, &date, cancellable, error)) return FALSE;
		if (g_date_time_compare(date, since) >= 0) high = middle;
		else low = middle + 1;
	}
	g_clear_pointer(&date, g_date_time_unref);
	if (!socket_sequence_date(self, low, &uid, &date, cancellable, error)) return FALSE;
	*after_uid = uid ? uid - 1 : 0;
	return TRUE;
}
/* The literal labelled @label in the FETCH reply for @uid. */
static GBytes *fetch_literal(GPtrArray *replies, guint32 uid, const gchar *label)
{
	guint i, j;
	for (i = 0; i < replies->len; i++) {
		ImapReply *reply = g_ptr_array_index(replies, i);
		guint64 found = 0;
		guint32 sequence;
		if (!fetch_sequence(reply->text->str, &sequence)) continue;
		if (fetch_item(reply->text->str, "UID", &found) && found != uid) continue;
		for (j = 0; j < reply->labels->len; j++)
			if (!g_strcmp0(g_ptr_array_index(reply->labels, j), label)) return g_bytes_ref(g_ptr_array_index(reply->literals, j));
	}
	return NULL;
}
static GBytes *socket_fetch(VentureImapClient *client, guint32 uid, GCancellable *cancellable, GError **error)
{
	VentureSocketImapClient *self = VENTURE_SOCKET_IMAP_CLIENT(client);
	g_autoptr(GPtrArray) replies = g_ptr_array_new_with_free_func(imap_reply_free);
	g_autofree gchar *command = g_strdup_printf("UID FETCH %u (BODY.PEEK[])", uid);
	GBytes *body;
	ImapStatus status;
	const gchar *code = NULL;
	if (!self->selected) { socket_broken(self, error, VENTURE_ERROR_NETWORK, "no folder selected"); return NULL; }
	if (!socket_simple(self, command, replies, VENTURE_IMAP_MAX_MESSAGE, &status, &code, cancellable, error)) return NULL;
	if (status != IMAP_STATUS_OK) { socket_refuse(error, VENTURE_ERROR_MAIL_PERMANENT, "message refused", code); return NULL; }
	body = fetch_literal(replies, uid, "BODY[]");
	/* BODY[] NIL, or no reply at all for a message expunged meanwhile. */
	if (!body || !g_bytes_get_size(body)) {
		g_clear_pointer(&body, g_bytes_unref);
		socket_refuse(error, VENTURE_ERROR_MAIL_PERMANENT, "the server returned no content for this message", NULL);
	}
	return body;
}
static GBytes *socket_fetch_truncated(VentureImapClient *client, guint32 uid, gsize text_limit, GCancellable *cancellable, GError **error)
{
	VentureSocketImapClient *self = VENTURE_SOCKET_IMAP_CLIENT(client);
	g_autoptr(GPtrArray) replies = g_ptr_array_new_with_free_func(imap_reply_free);
	g_autofree gchar *command = g_strdup_printf("UID FETCH %u (BODY.PEEK[HEADER] BODY.PEEK[TEXT]<0.%" G_GSIZE_FORMAT ">)", uid, text_limit);
	g_autoptr(GBytes) header = NULL, text = NULL;
	GByteArray *joined;
	ImapStatus status;
	const gchar *code = NULL;
	if (!self->selected) { socket_broken(self, error, VENTURE_ERROR_NETWORK, "no folder selected"); return NULL; }
	if (!socket_simple(self, command, replies, MAX(VENTURE_IMAP_MAX_HEADER, text_limit), &status, &code, cancellable, error)) return NULL;
	if (status != IMAP_STATUS_OK) { socket_refuse(error, VENTURE_ERROR_MAIL_PERMANENT, "message refused", code); return NULL; }
	header = fetch_literal(replies, uid, "BODY[HEADER]");
	text = fetch_literal(replies, uid, "BODY[TEXT]<0>");
	if (!header || !g_bytes_get_size(header)) { socket_refuse(error, VENTURE_ERROR_MAIL_PERMANENT, "the server returned no header for this message", NULL); return NULL; }
	joined = g_byte_array_new();
	g_byte_array_append(joined, g_bytes_get_data(header, NULL), (guint)g_bytes_get_size(header));
	if (text) g_byte_array_append(joined, g_bytes_get_data(text, NULL), (guint)MIN(g_bytes_get_size(text), text_limit));
	return g_byte_array_free_to_bytes(joined);
}
static void socket_set_deadline(VentureImapClient *client, gint64 monotonic_deadline)
{
	VENTURE_SOCKET_IMAP_CLIENT(client)->deadline = monotonic_deadline;
}
static void socket_disconnect(VentureImapClient *client)
{
	VentureSocketImapClient *self = VENTURE_SOCKET_IMAP_CLIENT(client);
	ImapStatus status;
	const gchar *code = NULL;
	if (self->out && socket_seconds_left(self)) socket_simple(self, "LOGOUT", NULL, VENTURE_IMAP_MAX_SMALL_LITERAL, &status, &code, NULL, NULL);
	socket_close(self);
}
static void socket_iface_init(VentureImapClientInterface *iface)
{
	iface->connect = socket_connect; iface->select = socket_select; iface->list = socket_list; iface->since = socket_since;
	iface->fetch = socket_fetch; iface->fetch_truncated = socket_fetch_truncated; iface->set_deadline = socket_set_deadline;
	iface->disconnect = socket_disconnect;
}
VentureSocketImapClient *venture_socket_imap_client_new(void) { return g_object_new(VENTURE_TYPE_SOCKET_IMAP_CLIENT, NULL); }
