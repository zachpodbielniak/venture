/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <string.h>
G_DEFINE_INTERFACE(VentureImapClient, venture_imap_client, G_TYPE_OBJECT)
static void venture_imap_client_default_init(VentureImapClientInterface *iface) { }
gboolean venture_imap_client_connect(VentureImapClient *self, const gchar *host, guint16 port, const gchar *security, const gchar *username, const gchar *secret, GCancellable *cancellable, GError **error)
{
	g_return_val_if_fail(VENTURE_IS_IMAP_CLIENT(self), FALSE);
	return VENTURE_IMAP_CLIENT_GET_IFACE(self)->connect(self, host, port, security, username, secret, cancellable, error);
}
gboolean venture_imap_client_select(VentureImapClient *self, const gchar *folder, GCancellable *cancellable, GError **error)
{
	g_return_val_if_fail(VENTURE_IS_IMAP_CLIENT(self), FALSE);
	return VENTURE_IMAP_CLIENT_GET_IFACE(self)->select(self, folder, cancellable, error);
}
GArray *venture_imap_client_uids_after(VentureImapClient *self, guint32 last_uid, GCancellable *cancellable, GError **error)
{
	g_return_val_if_fail(VENTURE_IS_IMAP_CLIENT(self), NULL);
	return VENTURE_IMAP_CLIENT_GET_IFACE(self)->uids_after(self, last_uid, cancellable, error);
}
GBytes *venture_imap_client_fetch(VentureImapClient *self, guint32 uid, GCancellable *cancellable, GError **error)
{
	g_return_val_if_fail(VENTURE_IS_IMAP_CLIENT(self), NULL);
	return VENTURE_IMAP_CLIENT_GET_IFACE(self)->fetch(self, uid, cancellable, error);
}
void venture_imap_client_disconnect(VentureImapClient *self)
{
	g_return_if_fail(VENTURE_IS_IMAP_CLIENT(self));
	VENTURE_IMAP_CLIENT_GET_IFACE(self)->disconnect(self);
}

/* --- Fake: folders of (uid, bytes) in memory ----------------------------- */
typedef struct { guint32 uid; GBytes *raw; } FakeMessage;
struct _VentureFakeImapClient {
	GObject parent_instance;
	GHashTable *folders; /* name -> GArray of FakeMessage */
	gchar *selected;
	gint connects;
	gboolean connected;
};
static void fake_iface_init(VentureImapClientInterface *iface);
G_DEFINE_FINAL_TYPE_WITH_CODE(VentureFakeImapClient, venture_fake_imap_client, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(VENTURE_TYPE_IMAP_CLIENT, fake_iface_init))
static void fake_folder_free(gpointer data)
{
	GArray *messages = data;
	guint i;
	for (i = 0; i < messages->len; i++) g_bytes_unref(g_array_index(messages, FakeMessage, i).raw);
	g_array_unref(messages);
}
static void fake_finalize(GObject *object)
{
	VentureFakeImapClient *self = VENTURE_FAKE_IMAP_CLIENT(object);
	g_hash_table_unref(self->folders);
	g_free(self->selected);
	G_OBJECT_CLASS(venture_fake_imap_client_parent_class)->finalize(object);
}
static void venture_fake_imap_client_class_init(VentureFakeImapClientClass *klass) { G_OBJECT_CLASS(klass)->finalize = fake_finalize; }
static void venture_fake_imap_client_init(VentureFakeImapClient *self)
{
	self->folders = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, fake_folder_free);
}
static gboolean fake_connect(VentureImapClient *client, const gchar *host, guint16 port, const gchar *security, const gchar *username, const gchar *secret, GCancellable *cancellable, GError **error)
{
	VentureFakeImapClient *self = VENTURE_FAKE_IMAP_CLIENT(client);
	if (!secret || !*secret) { g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG, "IMAP secret is empty"); return FALSE; }
	self->connects++;
	self->connected = TRUE;
	return TRUE;
}
static gboolean fake_select(VentureImapClient *client, const gchar *folder, GCancellable *cancellable, GError **error)
{
	VentureFakeImapClient *self = VENTURE_FAKE_IMAP_CLIENT(client);
	if (!self->connected) { g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_FAILED, "Not connected"); return FALSE; }
	g_free(self->selected);
	self->selected = g_strdup(folder);
	return TRUE;
}
static gint fake_compare(gconstpointer a, gconstpointer b)
{
	guint32 x = *(const guint32 *)a, y = *(const guint32 *)b;
	return x < y ? -1 : x > y;
}
static GArray *fake_uids_after(VentureImapClient *client, guint32 last_uid, GCancellable *cancellable, GError **error)
{
	VentureFakeImapClient *self = VENTURE_FAKE_IMAP_CLIENT(client);
	GArray *result = g_array_new(FALSE, FALSE, sizeof(guint32));
	GArray *messages = self->selected ? g_hash_table_lookup(self->folders, self->selected) : NULL;
	guint i;
	for (i = 0; messages && i < messages->len; i++) {
		guint32 uid = g_array_index(messages, FakeMessage, i).uid;
		if (uid > last_uid) g_array_append_val(result, uid);
	}
	g_array_sort(result, fake_compare);
	return result;
}
static GBytes *fake_fetch(VentureImapClient *client, guint32 uid, GCancellable *cancellable, GError **error)
{
	VentureFakeImapClient *self = VENTURE_FAKE_IMAP_CLIENT(client);
	GArray *messages = self->selected ? g_hash_table_lookup(self->folders, self->selected) : NULL;
	guint i;
	for (i = 0; messages && i < messages->len; i++)
		if (g_array_index(messages, FakeMessage, i).uid == uid) return g_bytes_ref(g_array_index(messages, FakeMessage, i).raw);
	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "No message with UID %u", uid);
	return NULL;
}
static void fake_disconnect(VentureImapClient *client)
{
	VentureFakeImapClient *self = VENTURE_FAKE_IMAP_CLIENT(client);
	self->connected = FALSE;
	g_clear_pointer(&self->selected, g_free);
}
static void fake_iface_init(VentureImapClientInterface *iface)
{
	iface->connect = fake_connect; iface->select = fake_select; iface->uids_after = fake_uids_after;
	iface->fetch = fake_fetch; iface->disconnect = fake_disconnect;
}
VentureFakeImapClient *venture_fake_imap_client_new(void) { return g_object_new(VENTURE_TYPE_FAKE_IMAP_CLIENT, NULL); }
void venture_fake_imap_client_add_message(VentureFakeImapClient *self, const gchar *folder, guint32 uid, const gchar *raw)
{
	GArray *messages;
	FakeMessage message;
	g_return_if_fail(VENTURE_IS_FAKE_IMAP_CLIENT(self));
	messages = g_hash_table_lookup(self->folders, folder);
	if (!messages) {
		messages = g_array_new(FALSE, FALSE, sizeof(FakeMessage));
		g_hash_table_insert(self->folders, g_strdup(folder), messages);
	}
	message.uid = uid;
	message.raw = g_bytes_new(raw, strlen(raw));
	g_array_append_val(messages, message);
}
gint venture_fake_imap_client_get_connects(VentureFakeImapClient *self)
{
	g_return_val_if_fail(VENTURE_IS_FAKE_IMAP_CLIENT(self), 0);
	return self->connects;
}

/* --- Socket: the smallest IMAP4rev1 dialogue that does the job ----------- */
struct _VentureSocketImapClient {
	GObject parent_instance;
	GSocketConnection *connection;
	GIOStream *stream;
	GDataInputStream *in;
	GOutputStream *out;
	guint tag;
};
static void socket_iface_init(VentureImapClientInterface *iface);
G_DEFINE_FINAL_TYPE_WITH_CODE(VentureSocketImapClient, venture_socket_imap_client, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(VENTURE_TYPE_IMAP_CLIENT, socket_iface_init))
static void socket_close(VentureSocketImapClient *self)
{
	if (self->stream) g_io_stream_close(self->stream, NULL, NULL);
	g_clear_object(&self->in);
	g_clear_object(&self->out);
	g_clear_object(&self->stream);
	g_clear_object(&self->connection);
}
static void socket_finalize(GObject *object)
{
	socket_close(VENTURE_SOCKET_IMAP_CLIENT(object));
	G_OBJECT_CLASS(venture_socket_imap_client_parent_class)->finalize(object);
}
static void venture_socket_imap_client_class_init(VentureSocketImapClientClass *klass) { G_OBJECT_CLASS(klass)->finalize = socket_finalize; }
static void venture_socket_imap_client_init(VentureSocketImapClient *self) { }
static gboolean socket_protocol_error(GError **error, const gchar *what)
{
	/* Server text is never echoed: a rejected LOGIN line would carry the credential. */
	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_FAILED, "IMAP: %s", what);
	return FALSE;
}
static gchar *socket_read_line(VentureSocketImapClient *self, GCancellable *cancellable, GError **error)
{
	gsize length = 0;
	gchar *line = g_data_input_stream_read_line(self->in, &length, cancellable, error);
	if (!line) { if (error && !*error) socket_protocol_error(error, "connection closed"); return NULL; }
	if (length && line[length - 1] == '\r') line[length - 1] = '\0';
	return line;
}
static void socket_wire_streams(VentureSocketImapClient *self, GIOStream *stream)
{
	g_clear_object(&self->in);
	g_clear_object(&self->out);
	self->in = g_data_input_stream_new(g_io_stream_get_input_stream(stream));
	g_data_input_stream_set_newline_type(self->in, G_DATA_STREAM_NEWLINE_TYPE_LF);
	self->out = g_object_ref(g_io_stream_get_output_stream(stream));
}
/* Sends one tagged command and collects untagged replies until the tagged
 * response. Literal replies ({n}\r\n) are appended verbatim to @literal. */
static gboolean socket_command(VentureSocketImapClient *self, const gchar *command, GPtrArray *untagged, GByteArray *literal, GCancellable *cancellable, GError **error)
{
	g_autofree gchar *tag = g_strdup_printf("A%04u", ++self->tag);
	g_autofree gchar *line = g_strdup_printf("%s %s\r\n", tag, command);
	if (!g_output_stream_write_all(self->out, line, strlen(line), NULL, cancellable, error)) return FALSE;
	for (;;) {
		g_autofree gchar *reply = socket_read_line(self, cancellable, error);
		const gchar *brace;
		if (!reply) return FALSE;
		if (g_str_has_prefix(reply, tag) && reply[strlen(tag)] == ' ') {
			const gchar *status = reply + strlen(tag) + 1;
			if (g_str_has_prefix(status, "OK")) return TRUE;
			return socket_protocol_error(error, g_str_has_prefix(status, "NO") ? "command refused" : "bad command");
		}
		brace = strrchr(reply, '{');
		if (brace && literal && g_str_has_suffix(reply, "}")) {
			gsize size = (gsize)g_ascii_strtoull(brace + 1, NULL, 10);
			gsize old = literal->len;
			g_byte_array_set_size(literal, (guint)(old + size));
			if (size && !g_input_stream_read_all(G_INPUT_STREAM(self->in), literal->data + old, size, NULL, cancellable, error)) return FALSE;
			{ g_autofree gchar *rest = socket_read_line(self, cancellable, error); if (!rest) return FALSE; }
		}
		if (untagged) g_ptr_array_add(untagged, g_steal_pointer(&reply));
	}
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
static gboolean socket_connect(VentureImapClient *client, const gchar *host, guint16 port, const gchar *security, const gchar *username, const gchar *secret, GCancellable *cancellable, GError **error)
{
	VentureSocketImapClient *self = VENTURE_SOCKET_IMAP_CLIENT(client);
	g_autoptr(GSocketClient) socket_client = g_socket_client_new();
	g_autofree gchar *greeting = NULL, *user = NULL, *pass = NULL, *login = NULL;
	gboolean tls = !g_strcmp0(security, "tls"), starttls = !g_strcmp0(security, "starttls");
	socket_close(self);
	if (!tls && !starttls && g_strcmp0(security, "none")) return socket_protocol_error(error, "security must be tls, starttls or none");
	if (!g_strcmp0(security, "none") && username && *username) return socket_protocol_error(error, "authentication over an unencrypted connection is refused");
	g_socket_client_set_timeout(socket_client, 60);
	if (tls) g_socket_client_set_tls(socket_client, TRUE);
	self->connection = g_socket_client_connect_to_host(socket_client, host, port, cancellable, error);
	if (!self->connection) return FALSE;
	self->stream = g_object_ref(G_IO_STREAM(self->connection));
	socket_wire_streams(self, self->stream);
	greeting = socket_read_line(self, cancellable, error);
	if (!greeting) return FALSE;
	if (!g_str_has_prefix(greeting, "* OK") && !g_str_has_prefix(greeting, "* PREAUTH")) return socket_protocol_error(error, "unexpected greeting");
	if (starttls) {
		GIOStream *upgraded;
		if (!socket_command(self, "STARTTLS", NULL, NULL, cancellable, error)) return FALSE;
		upgraded = g_tls_client_connection_new(self->stream, G_SOCKET_CONNECTABLE(g_network_address_new(host, port)), error);
		if (!upgraded) return FALSE;
		if (!g_tls_connection_handshake(G_TLS_CONNECTION(upgraded), cancellable, error)) { g_object_unref(upgraded); return FALSE; }
		g_object_unref(self->stream);
		self->stream = upgraded;
		socket_wire_streams(self, self->stream);
	}
	user = socket_quote(username);
	pass = socket_quote(secret);
	login = g_strdup_printf("LOGIN %s %s", user, pass);
	if (!socket_command(self, login, NULL, NULL, cancellable, error)) { socket_close(self); return FALSE; }
	return TRUE;
}
static gboolean socket_select(VentureImapClient *client, const gchar *folder, GCancellable *cancellable, GError **error)
{
	VentureSocketImapClient *self = VENTURE_SOCKET_IMAP_CLIENT(client);
	g_autofree gchar *quoted = socket_quote(folder), *command = NULL;
	if (!self->out) return socket_protocol_error(error, "not connected");
	command = g_strdup_printf("EXAMINE %s", quoted);
	return socket_command(self, command, NULL, NULL, cancellable, error);
}
static GArray *socket_uids_after(VentureImapClient *client, guint32 last_uid, GCancellable *cancellable, GError **error)
{
	VentureSocketImapClient *self = VENTURE_SOCKET_IMAP_CLIENT(client);
	g_autoptr(GPtrArray) lines = g_ptr_array_new_with_free_func(g_free);
	g_autofree gchar *command = g_strdup_printf("UID SEARCH UID %u:*", last_uid + 1);
	GArray *result;
	guint i;
	if (!self->out) { socket_protocol_error(error, "not connected"); return NULL; }
	if (!socket_command(self, command, lines, NULL, cancellable, error)) return NULL;
	result = g_array_new(FALSE, FALSE, sizeof(guint32));
	for (i = 0; i < lines->len; i++) {
		const gchar *line = g_ptr_array_index(lines, i);
		g_auto(GStrv) parts = NULL;
		guint j;
		if (!g_str_has_prefix(line, "* SEARCH")) continue;
		parts = g_strsplit(line + 8, " ", -1);
		for (j = 0; parts[j]; j++) {
			guint64 uid = g_ascii_strtoull(parts[j], NULL, 10);
			guint32 narrow = (guint32)uid;
			/* n:* also returns the last message when nothing is above n. */
			if (uid > last_uid && uid <= G_MAXUINT32) g_array_append_val(result, narrow);
		}
	}
	g_array_sort(result, fake_compare);
	return result;
}
static GBytes *socket_fetch(VentureImapClient *client, guint32 uid, GCancellable *cancellable, GError **error)
{
	VentureSocketImapClient *self = VENTURE_SOCKET_IMAP_CLIENT(client);
	g_autoptr(GByteArray) literal = g_byte_array_new();
	g_autofree gchar *command = g_strdup_printf("UID FETCH %u (BODY.PEEK[])", uid);
	if (!self->out) { socket_protocol_error(error, "not connected"); return NULL; }
	if (!socket_command(self, command, NULL, literal, cancellable, error)) return NULL;
	if (!literal->len) { socket_protocol_error(error, "empty fetch"); return NULL; }
	return g_byte_array_free_to_bytes(g_steal_pointer(&literal));
}
static void socket_disconnect(VentureImapClient *client)
{
	VentureSocketImapClient *self = VENTURE_SOCKET_IMAP_CLIENT(client);
	if (self->out) socket_command(self, "LOGOUT", NULL, NULL, NULL, NULL);
	socket_close(self);
}
static void socket_iface_init(VentureImapClientInterface *iface)
{
	iface->connect = socket_connect; iface->select = socket_select; iface->uids_after = socket_uids_after;
	iface->fetch = socket_fetch; iface->disconnect = socket_disconnect;
}
VentureSocketImapClient *venture_socket_imap_client_new(void) { return g_object_new(VENTURE_TYPE_SOCKET_IMAP_CLIENT, NULL); }
