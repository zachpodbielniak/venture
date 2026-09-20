/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <string.h>
#include <glib/gstdio.h>
#include "venture-test-util.h"
#include "venture-test-accounting.h"

typedef struct {
	VentureDatabase *db;
	VentureFakeImapClient *imap;
	VentureMailSyncService *service;
	gchar *root;
	gint64 org;
	gint64 company;
	gint64 contact;
} Fixture;

static void save(Fixture *f, VentureEntity *e)
{
	g_autoptr(GError) error = NULL;
	g_assert_true(venture_database_save(f->db, e, NULL, &error));
	g_assert_no_error(error);
}

static gint64 count_type(Fixture *f, const gchar *name)
{
	g_autoptr(VentureQuery) q = venture_query_new(venture_entity_registry_lookup(venture_entity_registry_get_default(), name));
	venture_query_set_organization(q, f->org);
	return venture_database_count(f->db, q, NULL);
}

/* Every row of a type in the organization, oldest first. */
static GPtrArray *rows(Fixture *f, GType type)
{
	g_autoptr(VentureQuery) q = venture_query_new(type);
	g_autoptr(GError) error = NULL;
	GPtrArray *found;
	venture_query_set_organization(q, f->org);
	venture_query_add_order(q, "id", VENTURE_SORT_ASCENDING, NULL);
	found = venture_database_find(f->db, q, &error);
	g_assert_no_error(error);
	return found;
}

static void setup(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) company = NULL, contact = NULL;
	f->db = venture_test_accounting_database(&error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ORGANIZATION);
		g_autoptr(VentureEntity) org = venture_database_find_one(f->db, query, &error);
		f->org = venture_entity_get_id(org);
	}
	f->root = g_dir_make_tmp("venture-mail-sync-XXXXXX", &error);
	g_assert_no_error(error);
	company = g_object_new(VENTURE_TYPE_COMPANY, "organization-id", f->org, "name", "Acme", NULL);
	save(f, company);
	f->company = venture_entity_get_id(company);
	contact = g_object_new(VENTURE_TYPE_CONTACT, "organization-id", f->org, "name", "Ada", "email", "Ada+crm@Example.test", "company-id", f->company, NULL);
	save(f, contact);
	f->contact = venture_entity_get_id(contact);
	f->imap = venture_fake_imap_client_new();
	f->service = venture_mail_sync_service_new(f->db, VENTURE_IMAP_CLIENT(f->imap));
	g_object_set(f->service, "attachment-root", f->root, NULL);
	g_setenv("VENTURE_IMAP_SECRET", "app-password", TRUE);
}

static void teardown(Fixture *f, gconstpointer data)
{
	g_autofree gchar *real = realpath(f->root, NULL);
	g_clear_object(&f->service);
	g_clear_object(&f->imap);
	venture_test_accounting_database_cleanup(f->db); g_clear_object(&f->db);
	if (real) venture_test_remove_within(f->root, real);
	g_free(f->root);
}

static VentureEntity *account(Fixture *f, const gchar *secret_env)
{
	VentureEntity *a = g_object_new(VENTURE_TYPE_MAIL_ACCOUNT, "organization-id", f->org,
		"address", "ops@venture.test", "imap-host", "imap.venture.test", "imap-port", (gint64)993, "imap-tls", "tls",
		"username", "ops@venture.test", "secret-env", secret_env, "folders", "INBOX",
		"capture-address", "receipts@venture.test", "capture-folder", "Receipts", "active", TRUE, NULL);
	save(f, a);
	return a;
}

/* A plain message; @id NULL leaves out the Message-ID header. */
static gchar *mail(const gchar *from, const gchar *to, const gchar *subject, const gchar *id, const gchar *headers, const gchar *body)
{
	return g_strdup_printf("From: %s\r\nTo: %s\r\nSubject: %s\r\nDate: Mon, 14 Sep 2026 10:00:00 +0000\r\n%s%s%s%s"
		"Content-Type: text/plain; charset=utf-8\r\n\r\n%s\r\n",
		from, to, subject, id ? "Message-ID: <" : "", id ? id : "", id ? ">\r\n" : "", headers ? headers : "", body);
}

static gint64 cursor_member(VentureEntity *a, const gchar *folder, const gchar *member)
{
	g_autofree gchar *text = NULL;
	g_autoptr(JsonNode) node = NULL;
	JsonNode *entry;
	g_object_get(a, "cursors", &text, NULL);
	node = json_from_string(text, NULL);
	g_assert_nonnull(node);
	entry = json_object_get_member(json_node_get_object(node), folder);
	g_assert_nonnull(entry);
	return venture_json_object_get_int(json_node_get_object(entry), member, 0);
}

static guint count_files(const gchar *root)
{
	g_autoptr(GDir) dir = g_dir_open(root, 0, NULL);
	guint n = 0;
	while (dir && g_dir_read_name(dir)) n++;
	return n;
}

static const gchar *msg_ada =
	"From: Ada <ada@example.test>\r\nTo: ops@venture.test\r\nSubject: Hello\r\nDate: Mon, 14 Sep 2026 10:00:00 +0000\r\n"
	"Message-ID: <one@example.test>\r\nContent-Type: text/plain\r\n\r\nFirst message.\r\n";
static const gchar *msg_reply =
	"From: ops@venture.test\r\nTo: Ada <ada@example.test>\r\nCc: stranger@else.test\r\nSubject: Re: Hello\r\nDate: Mon, 14 Sep 2026 11:00:00 +0000\r\n"
	"Message-ID: <two@venture.test>\r\nIn-Reply-To: <one@example.test>\r\nReferences: <one@example.test>\r\nContent-Type: text/plain\r\n\r\nReply.\r\n";
static const gchar *msg_receipt =
	"From: Shop <shop@store.test>\r\nTo: receipts@venture.test\r\nSubject: Your receipt\r\nDate: Tue, 15 Sep 2026 09:00:00 +0000\r\n"
	"Message-ID: <r1@store.test>\r\nMIME-Version: 1.0\r\nContent-Type: multipart/mixed; boundary=\"b\"\r\n\r\n"
	"--b\r\nContent-Type: text/plain\r\n\r\nThanks for your order.\r\n"
	"--b\r\nContent-Type: text/csv; name=\"receipt.csv\"\r\nContent-Disposition: attachment; filename=\"receipt.csv\"\r\n\r\nitem,amount\r\ntoner,12.50\r\n"
	"--b--\r\n";

/* --- A scripted IMAP server on loopback ----------------------------------------
 *
 * The fake client hides the protocol: a reply line too long, a folder name
 * the server cannot read, a UIDVALIDITY nobody parsed. This server speaks
 * enough IMAP4rev1 for the socket client to be driven over a real socket,
 * in plaintext through the client's test-only allow-plaintext switch, and
 * records every command it received. It runs on its own thread because the
 * sync blocks the calling one, which is also why every read has a timeout
 * and the accept a cancellable. */
typedef struct { guint32 uid; gchar *date; gchar *raw; guint64 claimed; } StubMessage;
typedef struct { gchar *wire; guint32 uidvalidity; GPtrArray *messages; } StubFolder;
typedef struct {
	GSocketListener *listener;
	GCancellable *cancellable;
	GThread *thread;
	GMutex lock;
	guint16 port;
	guint sessions;
	gchar *capabilities;
	gchar *secret;
	gboolean trickle;
	gsize garbage;
	GPtrArray *folders;
	/* Observed, read after stub_finish() joined the thread. */
	GPtrArray *commands;
	gchar *credentials;
	gsize garbage_sent;
} Stub;
static void stub_message_free(gpointer data)
{
	StubMessage *m = data;
	g_free(m->date); g_free(m->raw); g_free(m);
}
static void stub_folder_free(gpointer data)
{
	StubFolder *folder = data;
	g_free(folder->wire); g_ptr_array_unref(folder->messages); g_free(folder);
}
static void stub_init(Stub *s, const gchar *capabilities, const gchar *secret)
{
	g_autoptr(GError) error = NULL;
	memset(s, 0, sizeof *s);
	g_mutex_init(&s->lock);
	s->listener = g_socket_listener_new();
	s->port = g_socket_listener_add_any_inet_port(s->listener, NULL, &error);
	g_assert_no_error(error);
	s->cancellable = g_cancellable_new();
	s->capabilities = g_strdup(capabilities);
	s->secret = g_strdup(secret);
	s->folders = g_ptr_array_new_with_free_func(stub_folder_free);
	s->commands = g_ptr_array_new_with_free_func(g_free);
}
static StubFolder *stub_find(Stub *s, const gchar *wire)
{
	guint i;
	for (i = 0; i < s->folders->len; i++)
		if (!strcmp(((StubFolder *)g_ptr_array_index(s->folders, i))->wire, wire)) return g_ptr_array_index(s->folders, i);
	return NULL;
}
static StubFolder *stub_folder(Stub *s, const gchar *wire, guint32 uidvalidity)
{
	StubFolder *folder = stub_find(s, wire);
	if (!folder) {
		folder = g_new0(StubFolder, 1);
		folder->wire = g_strdup(wire);
		folder->messages = g_ptr_array_new_with_free_func(stub_message_free);
		g_ptr_array_add(s->folders, folder);
	}
	folder->uidvalidity = uidvalidity;
	return folder;
}
static void stub_add(Stub *s, const gchar *wire, guint32 uid, const gchar *date, const gchar *raw, guint64 claimed)
{
	StubMessage *m = g_new0(StubMessage, 1);
	StubFolder *folder = stub_find(s, wire);
	g_assert_nonnull(folder);
	m->uid = uid;
	m->date = g_strdup(date ? date : "14-Sep-2026 10:00:00 +0000");
	m->raw = g_strdup(raw);
	m->claimed = claimed;
	g_ptr_array_add(folder->messages, m);
}
static gboolean stub_send(GOutputStream *out, const gchar *text)
{
	return g_output_stream_write_all(out, text, strlen(text), NULL, NULL, NULL);
}
static gboolean stub_has_command(Stub *s, const gchar *fragment)
{
	guint i;
	for (i = 0; i < s->commands->len; i++) if (strstr(g_ptr_array_index(s->commands, i), fragment)) return TRUE;
	return FALSE;
}
static void stub_check_credentials(Stub *s, const gchar *user, const gchar *pass, const gchar *tag, GString *reply)
{
	g_free(s->credentials);
	s->credentials = g_strdup_printf("%s\n%s", user, pass);
	/* Real servers do repeat what they were sent; the client must not. */
	if (s->secret && g_strcmp0(pass, s->secret)) g_string_append_printf(reply, "%s NO [AUTHENTICATIONFAILED] Invalid credentials for %s\r\n", tag, pass);
	else g_string_append_printf(reply, "%s OK authenticated\r\n", tag);
}
/* LOGIN arguments as quoted strings, atoms or literals; a literal is sent
 * after the server's "+" and the command continues on the next line. */
static gboolean stub_login(Stub *s, GDataInputStream *in, GOutputStream *out, const gchar *tag, const gchar *arguments, GString *reply)
{
	g_autoptr(GPtrArray) args = g_ptr_array_new_with_free_func(g_free);
	g_autofree gchar *segment = g_strdup(arguments);
	for (;;) {
		const gchar *p = segment;
		guint64 literal = 0;
		gboolean more = FALSE;
		while (*p) {
			while (*p == ' ') p++;
			if (!*p) break;
			if (*p == '"') {
				GString *value = g_string_new(NULL);
				for (p++; *p && *p != '"'; p++) { if (*p == '\\' && p[1]) p++; g_string_append_c(value, *p); }
				if (*p == '"') p++;
				g_ptr_array_add(args, g_string_free(value, FALSE));
			} else if (*p == '{') {
				literal = g_ascii_strtoull(p + 1, NULL, 10);
				more = TRUE;
				break;
			} else {
				const gchar *start = p;
				while (*p && *p != ' ') p++;
				g_ptr_array_add(args, g_strndup(start, p - start));
			}
		}
		if (!more) break;
		if (!stub_send(out, "+ go ahead\r\n")) return FALSE;
		{
			gchar *bytes = g_malloc0(literal + 1);
			gsize got = 0;
			if (!g_input_stream_read_all(G_INPUT_STREAM(in), bytes, literal, &got, NULL, NULL) || got != literal) { g_free(bytes); return FALSE; }
			g_ptr_array_add(args, bytes);
		}
		g_free(segment);
		segment = g_data_input_stream_read_line(in, NULL, NULL, NULL);
		if (!segment) return FALSE;
	}
	stub_check_credentials(s, args->len > 0 ? g_ptr_array_index(args, 0) : "", args->len > 1 ? g_ptr_array_index(args, 1) : "", tag, reply);
	return TRUE;
}
static gboolean stub_handle(Stub *s, GDataInputStream *in, GOutputStream *out, const gchar *tag, const gchar *command, StubFolder **selected)
{
	g_autofree gchar *upper = g_ascii_strup(command, -1);
	g_autoptr(GString) reply = g_string_new(NULL);
	guint i;
	if (!strcmp(upper, "CAPABILITY")) {
		if (s->garbage) {
			/* A reply line with no end, to see how much of it the client
			 * swallows before refusing it. */
			const gsize chunk = 65536;
			gchar *filled = g_malloc(chunk);
			memset(filled, 'A', chunk);
			if (stub_send(out, "* CAPABILITY IMAP4rev1 "))
				while (s->garbage_sent < s->garbage && g_output_stream_write_all(out, filled, chunk, NULL, NULL, NULL)) s->garbage_sent += chunk;
			g_free(filled);
			return FALSE;
		}
		g_string_append_printf(reply, "* CAPABILITY %s\r\n%s OK done\r\n", s->capabilities, tag);
	} else if (g_str_has_prefix(upper, "AUTHENTICATE PLAIN")) {
		const gchar *initial = command + strlen("AUTHENTICATE PLAIN");
		g_autofree gchar *response = NULL;
		g_autofree guchar *decoded = NULL;
		gsize length = 0;
		const gchar *user = "", *pass = "";
		if (*initial == ' ') response = g_strdup(initial + 1);
		else {
			if (!stub_send(out, "+ \r\n")) return FALSE;
			response = g_data_input_stream_read_line(in, NULL, NULL, NULL);
			if (!response) return FALSE;
		}
		decoded = g_base64_decode(response, &length);
		decoded = g_realloc(decoded, length + 1);
		decoded[length] = '\0';
		/* authzid NUL user NUL password */
		if (length && memchr(decoded, '\0', length)) {
			user = (const gchar *)decoded + strlen((const gchar *)decoded) + 1;
			if ((gsize)(user - (const gchar *)decoded) < length) pass = user + strlen(user) + 1;
		}
		stub_check_credentials(s, user, pass, tag, reply);
	} else if (g_str_has_prefix(upper, "LOGIN ")) {
		if (strstr(s->capabilities, "LOGINDISABLED")) g_string_append_printf(reply, "%s NO [PRIVACYREQUIRED] login disabled\r\n", tag);
		else if (!stub_login(s, in, out, tag, command + strlen("LOGIN "), reply)) return FALSE;
	} else if (g_str_has_prefix(upper, "EXAMINE ")) {
		g_autofree gchar *name = g_strdup(command + strlen("EXAMINE "));
		StubFolder *folder;
		if (*name == '"') { memmove(name, name + 1, strlen(name)); if (*name && name[strlen(name) - 1] == '"') name[strlen(name) - 1] = '\0'; }
		folder = stub_find(s, name);
		*selected = folder;
		if (!folder) g_string_append_printf(reply, "%s NO [NONEXISTENT] no such mailbox\r\n", tag);
		else {
			guint32 next = 1;
			for (i = 0; i < folder->messages->len; i++) next = MAX(next, ((StubMessage *)g_ptr_array_index(folder->messages, i))->uid + 1);
			g_string_append_printf(reply, "* %u EXISTS\r\n* OK [UIDVALIDITY %u] UIDs valid\r\n* OK [UIDNEXT %u] next\r\n%s OK [READ-ONLY] examined\r\n",
				folder->messages->len, folder->uidvalidity, next, tag);
		}
	} else if (g_str_has_prefix(upper, "UID SEARCH")) {
		/* What the client used to ask: one line naming every UID. */
		g_string_append(reply, "* SEARCH");
		for (i = 0; *selected && i < (*selected)->messages->len; i++) g_string_append_printf(reply, " %u", ((StubMessage *)g_ptr_array_index((*selected)->messages, i))->uid);
		g_string_append_printf(reply, "\r\n%s OK searched\r\n", tag);
	} else if (g_str_has_prefix(upper, "UID FETCH ") && *selected) {
		g_auto(GStrv) parts = g_strsplit(command + strlen("UID FETCH "), " ", 2);
		guint64 low = g_ascii_strtoull(parts[0], NULL, 10), high = low;
		const gchar *colon = strchr(parts[0], ':');
		if (colon) high = colon[1] == '*' ? G_MAXUINT32 : g_ascii_strtoull(colon + 1, NULL, 10);
		for (i = 0; i < (*selected)->messages->len; i++) {
			StubMessage *m = g_ptr_array_index((*selected)->messages, i);
			const gchar *items = parts[1] ? parts[1] : "";
			if (m->uid < low || m->uid > high) continue;
			if (strstr(items, "BODY.PEEK[HEADER]")) {
				const gchar *split = strstr(m->raw, "\r\n\r\n");
				const gchar *limit = strstr(items, "<0.");
				gsize header = split ? (gsize)(split - m->raw) + 4 : strlen(m->raw);
				gsize text = MIN(strlen(m->raw) - header, limit ? (gsize)g_ascii_strtoull(limit + 3, NULL, 10) : G_MAXSIZE);
				g_string_append_printf(reply, "* %u FETCH (UID %u BODY[HEADER] {%" G_GSIZE_FORMAT "}\r\n", i + 1, m->uid, header);
				g_string_append_len(reply, m->raw, (gssize)header);
				g_string_append_printf(reply, " BODY[TEXT]<0> {%" G_GSIZE_FORMAT "}\r\n", text);
				g_string_append_len(reply, m->raw + header, (gssize)text);
				g_string_append(reply, ")\r\n");
			} else if (strstr(items, "BODY.PEEK[]")) {
				g_string_append_printf(reply, "* %u FETCH (UID %u BODY[] {%" G_GSIZE_FORMAT "}\r\n%s)\r\n", i + 1, m->uid, strlen(m->raw), m->raw);
			} else {
				g_string_append_printf(reply, "* %u FETCH (UID %u RFC822.SIZE %" G_GUINT64_FORMAT ")\r\n", i + 1, m->uid, m->claimed ? m->claimed : (guint64)strlen(m->raw));
			}
		}
		g_string_append_printf(reply, "%s OK fetched\r\n", tag);
	} else if (g_str_has_prefix(upper, "FETCH ") && *selected) {
		guint64 sequence = g_ascii_strtoull(command + strlen("FETCH "), NULL, 10);
		if (sequence >= 1 && sequence <= (*selected)->messages->len) {
			StubMessage *m = g_ptr_array_index((*selected)->messages, sequence - 1);
			if (strstr(upper, "INTERNALDATE")) g_string_append_printf(reply, "* %u FETCH (UID %u INTERNALDATE \"%s\")\r\n", (guint)sequence, m->uid, m->date);
			else g_string_append_printf(reply, "* %u FETCH (UID %u)\r\n", (guint)sequence, m->uid);
		}
		g_string_append_printf(reply, "%s OK fetched\r\n", tag);
	} else if (!strcmp(upper, "LOGOUT")) {
		g_string_append_printf(reply, "* BYE\r\n%s OK bye\r\n", tag);
		stub_send(out, reply->str);
		return FALSE;
	} else g_string_append_printf(reply, "%s BAD unknown command\r\n", tag);
	return stub_send(out, reply->str);
}
static void stub_session(Stub *s, GSocketConnection *connection)
{
	g_autoptr(GDataInputStream) in = g_data_input_stream_new(g_io_stream_get_input_stream(G_IO_STREAM(connection)));
	GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(connection));
	StubFolder *selected = NULL;
	g_socket_set_timeout(g_socket_connection_get_socket(connection), 10);
	g_data_input_stream_set_newline_type(in, G_DATA_STREAM_NEWLINE_TYPE_CR_LF);
	if (s->trickle) {
		/* A greeting a byte at a time, each well inside any per-read
		 * timeout; two hundred of them take fifty seconds. */
		g_autoptr(GString) greeting = g_string_new("* OK ");
		gsize i;
		for (i = 0; i < 200; i++) g_string_append_c(greeting, 'x');
		g_string_append(greeting, "\r\n");
		for (i = 0; i < greeting->len; i++) {
			if (!g_output_stream_write_all(out, greeting->str + i, 1, NULL, NULL, NULL)) return;
			g_usleep(250000);
		}
		return;
	}
	if (!stub_send(out, "* OK stub ready\r\n")) return;
	for (;;) {
		g_autofree gchar *line = g_data_input_stream_read_line(in, NULL, NULL, NULL);
		g_autofree gchar *tag = NULL;
		gchar *space;
		gboolean more;
		if (!line) return;
		space = strchr(line, ' ');
		if (!space) return;
		tag = g_strndup(line, space - line);
		g_mutex_lock(&s->lock);
		g_ptr_array_add(s->commands, g_strdup(space + 1));
		more = stub_handle(s, in, out, tag, space + 1, &selected);
		g_mutex_unlock(&s->lock);
		if (!more) return;
	}
}
static gpointer stub_serve(gpointer data)
{
	Stub *s = data;
	guint n;
	for (n = 0; n < s->sessions; n++) {
		g_autoptr(GSocketConnection) connection = g_socket_listener_accept(s->listener, NULL, s->cancellable, NULL);
		if (!connection) break;
		stub_session(s, connection);
		g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
	}
	return NULL;
}
static void stub_start(Stub *s, guint sessions)
{
	s->sessions = sessions;
	s->thread = g_thread_new("imap-stub", stub_serve, s);
}
/* Joins the server; a session the client never opened is cancelled rather
 * than waited for, so a failing test fails instead of hanging. */
static void stub_finish(Stub *s)
{
	g_cancellable_cancel(s->cancellable);
	if (s->thread) g_thread_join(s->thread);
	s->thread = NULL;
}
static void stub_clear(Stub *s)
{
	stub_finish(s);
	g_socket_listener_close(s->listener);
	g_clear_object(&s->listener);
	g_clear_object(&s->cancellable);
	g_ptr_array_unref(s->folders);
	g_ptr_array_unref(s->commands);
	g_free(s->capabilities); g_free(s->secret); g_free(s->credentials);
	g_mutex_clear(&s->lock);
}
static VentureMailSyncService *socket_service(Fixture *f)
{
	g_autoptr(VentureImapClient) client = g_object_new(VENTURE_TYPE_SOCKET_IMAP_CLIENT, "allow-plaintext", TRUE, NULL);
	VentureMailSyncService *service = venture_mail_sync_service_new(f->db, client);
	g_object_set(service, "attachment-root", f->root, NULL);
	return service;
}
static VentureEntity *socket_account(Fixture *f, Stub *stub, const gchar *folders, const gchar *secret_env)
{
	VentureEntity *a = g_object_new(VENTURE_TYPE_MAIL_ACCOUNT, "organization-id", f->org,
		"address", "ops@venture.test", "imap-host", "127.0.0.1", "imap-port", (gint64)stub->port, "imap-tls", "none",
		"username", "ops@venture.test", "secret-env", secret_env, "folders", folders, "active", TRUE, NULL);
	save(f, a);
	return a;
}

/* --- Records and small pieces --------------------------------------------------- */
static void test_records(void)
{
	VentureEntityRegistry *r = venture_entity_registry_get_default();
	g_assert_cmpuint(venture_entity_registry_lookup(r, "mail_account"), !=, G_TYPE_INVALID);
	g_assert_cmpuint(venture_entity_registry_lookup(r, "mail_inbound"), !=, G_TYPE_INVALID);
	g_assert_cmpuint(venture_entity_registry_lookup(r, "mail_unmatched_sender"), !=, G_TYPE_INVALID);
	{
		g_autoptr(VentureModuleRegistry) modules = venture_module_registry_new();
		venture_module_registry_register_builtins(modules);
		g_assert_nonnull(venture_module_registry_lookup(modules, "mail_sync"));
	}
}

/* Folder names travel as RFC 3501 modified UTF-7. Sent raw, a non-ASCII
 * folder never opened; an unescaped "&" opens a base64 run on the server. */
static void test_utf7(void)
{
	static const struct { const gchar *in, *out; } cases[] = {
		{ "INBOX", "INBOX" },
		{ "Entwürfe", "Entw&APw-rfe" },
		{ "Q&A", "Q&-A" },
		{ "&&", "&-&-" },
		{ "~peter/mail/台北/日本語", "~peter/mail/&U,BTFw-/&ZeVnLIqe-" },
		{ "\xf0\x9f\x98\x80", "&2D3eAA-" }
	};
	guint i;
	for (i = 0; i < G_N_ELEMENTS(cases); i++) {
		g_autofree gchar *encoded = venture_imap_utf7_encode(cases[i].in);
		g_assert_cmpstr(encoded, ==, cases[i].out);
	}
	g_assert_null(venture_imap_utf7_encode("bad\xff"));
}

/* HTML-only mail and HTML receipts are read through this; if it regresses
 * to markup, the timeline shows tags and a receipt's text is unsearchable. */
static void test_html_to_text(void)
{
	g_autofree gchar *text = venture_document_html_to_text(
		"<html><style>p { color: red }</style><p>Total&nbsp;&euro;12.50</p><br><div>Thanks &amp; bye</div>"
		"<script>alert(1)</script><!-- hidden -->&#x263A; a < b</html>", -1);
	g_assert_nonnull(strstr(text, "Total \xe2\x82\xac" "12.50"));
	g_assert_nonnull(strstr(text, "Thanks & bye"));
	g_assert_nonnull(strstr(text, "\xe2\x98\xba"));
	g_assert_nonnull(strstr(text, "a < b"));
	g_assert_null(strstr(text, "color"));
	g_assert_null(strstr(text, "alert"));
	g_assert_null(strstr(text, "hidden"));
	g_assert_null(strstr(text, "<p>"));
	g_assert_true(g_utf8_validate(text, -1, NULL));
}

/* --- Configuration and the original behaviour ----------------------------------- */
/* Rule 1: a missing secret fails the sync with an error naming the variable. */
static void test_missing_secret(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) a = account(f, "VENTURE_IMAP_MISSING");
	g_autoptr(GError) error = NULL;
	(void)data;
	g_unsetenv("VENTURE_IMAP_MISSING");
	g_assert_cmpint(venture_mail_sync_service_sync(f->service, a, NULL, &error), ==, -1);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
	g_assert_nonnull(strstr(error->message, "VENTURE_IMAP_MISSING"));
	g_assert_cmpint(venture_fake_imap_client_get_connects(f->imap), ==, 0);
}

/* A free-form secret_env would let a mail_account row name VENTURE_SMTP_PASSWORD
 * (or the session secret) and ship it to an attacker-controlled IMAP host. */
static void test_secret_env_prefix(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) a = account(f, "VENTURE_SMTP_PASSWORD");
	g_autoptr(GError) error = NULL;
	g_setenv("VENTURE_SMTP_PASSWORD", "not-for-imap", TRUE);
	g_assert_cmpint(venture_mail_sync_service_sync(f->service, a, NULL, &error), ==, -1);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
	g_assert_nonnull(strstr(error->message, "VENTURE_IMAP_"));
	g_assert_null(strstr(error->message, "not-for-imap"));
	g_assert_cmpint(venture_fake_imap_client_get_connects(f->imap), ==, 0);
	g_unsetenv("VENTURE_SMTP_PASSWORD");
}

/* Rules 2 and 3: UID high-water mark, raw documents, matched interactions, threads, unmatched senders. */
static void test_sync_matches_and_threads(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) a = account(f, "VENTURE_IMAP_SECRET");
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) found = NULL;
	venture_fake_imap_client_add_message(f->imap, "INBOX", 5, msg_ada);
	venture_fake_imap_client_add_message(f->imap, "INBOX", 7, msg_reply);
	g_assert_cmpint(venture_mail_sync_service_sync(f->service, a, NULL, &error), ==, 2);
	g_assert_no_error(error);
	g_assert_cmpint(count_type(f, "mail_inbound"), ==, 2);
	g_assert_cmpint(count_type(f, "document"), ==, 2);
	g_assert_cmpint(count_type(f, "interaction"), ==, 2);
	g_assert_cmpint(count_type(f, "mail_unmatched_sender"), ==, 1);
	g_assert_cmpint(cursor_member(a, "INBOX", "uid"), ==, 7);
	/* Same fake mailbox again: nothing new. */
	g_assert_cmpint(venture_mail_sync_service_sync(f->service, a, NULL, &error), ==, 0);
	g_assert_no_error(error);
	g_assert_cmpint(count_type(f, "mail_inbound"), ==, 2);
	g_assert_cmpint(count_type(f, "interaction"), ==, 2);
	g_assert_cmpint(count_type(f, "mail_unmatched_sender"), ==, 1);
	found = rows(f, VENTURE_TYPE_INTERACTION);
	{
		VentureEntity *first = g_ptr_array_index(found, 0), *second = g_ptr_array_index(found, 1);
		VentureInteractionKind kind;
		gint64 contact = 0, company = 0;
		gboolean outbound = TRUE;
		g_object_get(first, "kind", &kind, "contact-id", &contact, "company-id", &company, "outbound", &outbound, NULL);
		g_assert_cmpint(kind, ==, VENTURE_INTERACTION_KIND_EMAIL);
		g_assert_cmpint(contact, ==, f->contact);
		g_assert_cmpint(company, ==, f->company);
		g_assert_false(outbound);
		g_object_get(second, "outbound", &outbound, NULL);
		g_assert_true(outbound);
	}
	g_clear_pointer(&found, g_ptr_array_unref);
	found = rows(f, VENTURE_TYPE_MAIL_INBOUND);
	{
		g_autofree gchar *t1 = NULL, *t2 = NULL, *mid = NULL;
		gint64 interaction = 0;
		g_object_get(g_ptr_array_index(found, 0), "thread-id", &t1, "message-id", &mid, "interaction-id", &interaction, NULL);
		g_object_get(g_ptr_array_index(found, 1), "thread-id", &t2, NULL);
		g_assert_cmpstr(mid, ==, "one@example.test");
		g_assert_cmpstr(t1, ==, t2);
		g_assert_cmpint(interaction, >, 0);
	}
	g_clear_pointer(&found, g_ptr_array_unref);
	found = rows(f, VENTURE_TYPE_MAIL_UNMATCHED_SENDER);
	{
		g_autofree gchar *address = NULL;
		g_autoptr(VentureEntity) contact = NULL;
		gint64 seen = 0;
		g_object_get(g_ptr_array_index(found, 0), "address", &address, "seen", &seen, NULL);
		g_assert_cmpstr(address, ==, "stranger@else.test");
		g_assert_cmpint(seen, ==, 1);
		/* One click: the unmatched sender becomes a contact and leaves the list. */
		contact = venture_mail_sync_service_create_contact(f->service, g_ptr_array_index(found, 0), NULL, &error);
		g_assert_no_error(error);
		g_assert_nonnull(contact);
		g_assert_cmpint(count_type(f, "mail_unmatched_sender"), ==, 0);
		g_assert_cmpint(count_type(f, "contact"), ==, 2);
	}
}

/* Rule 4: capture-address mail becomes an inbox item with its attachment; same hash is not duplicated. */
static void test_capture(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) a = account(f, "VENTURE_IMAP_SECRET");
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureQuery) q = NULL;
	g_autoptr(VentureEntity) item = NULL, document = NULL;
	g_autofree gchar *source = NULL, *hash = NULL, *text = NULL, *title = NULL;
	g_autofree gchar *expected = g_compute_checksum_for_string(G_CHECKSUM_SHA256, "Total 99.00", -1);
	gint64 document_id = 0;
	/* The receipt was already captured by upload: the same content hash. */
	{
		g_autofree gchar *checksum = g_compute_checksum_for_string(G_CHECKSUM_SHA256, "item,amount\r\ntoner,12.50", -1);
		g_autoptr(VentureEntity) uploaded = g_object_new(VENTURE_TYPE_DOCUMENT, "organization-id", f->org, "title", "receipt.csv", "hash", checksum, NULL);
		g_autoptr(VentureEntity) prior = NULL;
		save(f, uploaded);
		prior = venture_capture_service_ingest_for_organization(venture_capture_service_get(f->db), f->org, "receipt", "receipt.csv", "upload",
			venture_entity_get_id(uploaded), NULL, NULL, NULL, NULL, NULL, &error);
		g_assert_no_error(error);
	}
	venture_fake_imap_client_add_message(f->imap, "INBOX", 1, msg_receipt);
	g_assert_cmpint(venture_mail_sync_service_sync(f->service, a, NULL, &error), ==, 1);
	g_assert_no_error(error);
	g_assert_cmpint(count_type(f, "capture_item"), ==, 1);
	g_assert_cmpint(count_type(f, "interaction"), ==, 0);
	/* A genuinely new receipt in the capture folder lands in the inbox with its attachment filed. */
	venture_fake_imap_client_add_message(f->imap, "Receipts", 3,
		"From: Shop <shop@store.test>\r\nTo: ops@venture.test\r\nSubject: Invoice 9\r\nMessage-ID: <r2@store.test>\r\nMIME-Version: 1.0\r\nContent-Type: multipart/mixed; boundary=\"b\"\r\n\r\n"
		"--b\r\nContent-Type: text/plain\r\n\r\nInvoice attached.\r\n--b\r\nContent-Type: text/plain; name=\"inv.txt\"\r\nContent-Disposition: attachment; filename=\"inv.txt\"\r\n\r\nTotal 99.00\r\n--b--\r\n");
	g_assert_cmpint(venture_mail_sync_service_sync(f->service, a, NULL, &error), ==, 1);
	g_assert_no_error(error);
	g_assert_cmpint(count_type(f, "capture_item"), ==, 2);
	q = venture_query_new(VENTURE_TYPE_CAPTURE_ITEM);
	venture_query_set_organization(q, f->org);
	venture_query_add_filter_string(q, "source", VENTURE_FILTER_OP_EQ, "email", NULL);
	item = venture_database_find_one(f->db, q, &error);
	g_assert_nonnull(item);
	g_object_get(item, "source", &source, "document-id", &document_id, "title", &title, NULL);
	g_assert_cmpstr(title, ==, "Invoice 9");
	g_assert_cmpint(document_id, >, 0);
	document = venture_database_get(f->db, VENTURE_TYPE_DOCUMENT, document_id, &error);
	g_object_get(document, "hash", &hash, "extracted-text", &text, NULL);
	g_assert_cmpstr(hash, ==, expected);
	g_assert_cmpstr(text, ==, "Total 99.00");
}

/* Rule 5: outbound mail through the outbox is an interaction on the recipient contact. */
static void test_outbound_recorded(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureLogMailer) mailer = venture_log_mailer_new();
	g_autoptr(VentureMailOutbox) outbox = venture_mail_outbox_new(f->db, VENTURE_MAILER(mailer));
	g_autoptr(VentureMailMessage) message = venture_mail_message_new();
	g_autoptr(VentureMailMessage) queued = NULL;
	g_autoptr(VentureQuery) q = NULL;
	g_autoptr(VentureEntity) interaction = NULL;
	g_autoptr(GError) error = NULL;
	gboolean outbound = FALSE;
	gint64 contact = 0;
	g_object_set(message, "organization-id", f->org, "to", "ada+crm@example.test", "subject", "Quote", "text-body", "Attached.", "idempotency-key", "q1", NULL);
	queued = venture_mail_outbox_enqueue(outbox, message, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpint(count_type(f, "interaction"), ==, 0);
	g_assert_cmpint(venture_mail_outbox_deliver_due(outbox, f->org, 10, NULL, NULL, &error), ==, 1);
	g_assert_no_error(error);
	g_assert_cmpint(count_type(f, "interaction"), ==, 1);
	q = venture_query_new(VENTURE_TYPE_INTERACTION);
	venture_query_set_organization(q, f->org);
	interaction = venture_database_find_one(f->db, q, &error);
	g_object_get(interaction, "outbound", &outbound, "contact-id", &contact, NULL);
	g_assert_true(outbound);
	g_assert_cmpint(contact, ==, f->contact);
	/* A second sweep does not deliver or record again. */
	g_assert_cmpint(venture_mail_outbox_deliver_due(outbox, f->org, 10, NULL, NULL, &error), ==, 0);
	g_assert_cmpint(count_type(f, "interaction"), ==, 1);
}

/* Rule 6: the sweep over an organization's accounts, and its refusal of a disabled account. */
static void test_sweep(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) a = account(f, "VENTURE_IMAP_SECRET");
	g_autoptr(VentureEntity) off = account(f, "VENTURE_IMAP_MISSING");
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) report = NULL;
	g_object_set(off, "active", FALSE, NULL);
	save(f, off);
	venture_fake_imap_client_add_message(f->imap, "INBOX", 2, msg_ada);
	report = venture_mail_sync_service_sweep(f->service, f->org, 100, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(report);
	g_assert_cmpint(venture_json_object_get_int(json_node_get_object(report), "accounts", 0), ==, 1);
	g_assert_cmpint(venture_json_object_get_int(json_node_get_object(report), "messages", 0), ==, 1);
}

/* A cursor reset behind an already-filed UID must still advance, or every
 * sweep re-fetches the same mail forever without creating rows. And the
 * UID key is checked before the fetch: a rerun over filed mail used to
 * download every message again only to find it filed. */
static void test_cursor_repairs_duplicate_uid(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) a = account(f, "VENTURE_IMAP_SECRET");
	g_autoptr(GError) error = NULL;
	venture_fake_imap_client_add_message(f->imap, "INBOX", 5, msg_ada);
	g_assert_cmpint(venture_mail_sync_service_sync(f->service, a, NULL, &error), ==, 1);
	g_assert_no_error(error);
	g_assert_cmpint(venture_fake_imap_client_get_fetches(f->imap), ==, 1);
	g_object_set(a, "cursors", "{}", NULL);
	save(f, a);
	g_assert_cmpint(venture_mail_sync_service_sync(f->service, a, NULL, &error), ==, 0);
	g_assert_no_error(error);
	g_assert_cmpint(venture_fake_imap_client_get_fetches(f->imap), ==, 1);
	g_assert_cmpint(count_type(f, "mail_inbound"), ==, 1);
	g_assert_cmpint(count_type(f, "interaction"), ==, 1);
	g_assert_cmpint(cursor_member(a, "INBOX", "uid"), ==, 5);
}

/* An untagged capture address still takes its +tag variants. */
static void test_capture_plus_tag(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) a = account(f, "VENTURE_IMAP_SECRET");
	g_autoptr(GError) error = NULL;
	venture_fake_imap_client_add_message(f->imap, "INBOX", 4,
		"From: Shop <shop@store.test>\r\nTo: receipts+toner@venture.test\r\nSubject: Tagged\r\nDate: Tue, 15 Sep 2026 09:00:00 +0000\r\n"
		"Message-ID: <tag@store.test>\r\nContent-Type: text/plain\r\n\r\nPlease file this.\r\n");
	g_assert_cmpint(venture_mail_sync_service_sync(f->service, a, NULL, &error), ==, 1);
	g_assert_no_error(error);
	g_assert_cmpint(count_type(f, "capture_item"), ==, 1);
	g_assert_cmpint(count_type(f, "interaction"), ==, 0);
}

/* A mailbox written before UIDVALIDITY was recorded: the cursor is a bare
 * number and the rows carry account:folder:uid keys. An upgrade that did
 * not read both would refetch and refile the whole folder. */
static void test_legacy_cursor_and_keys(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) a = account(f, "VENTURE_IMAP_SECRET");
	g_autoptr(VentureEntity) old = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *key = g_strdup_printf("%" G_GINT64_FORMAT ":INBOX:5", venture_entity_get_id(a));
	old = g_object_new(VENTURE_TYPE_MAIL_INBOUND, "organization-id", f->org, "account-id", venture_entity_get_id(a), "folder", "INBOX",
		"uid", (gint64)5, "uid-key", key, "message-id", "one@example.test", "thread-id", "one@example.test", "subject", "Hello", NULL);
	save(f, old);
	g_object_set(a, "cursors", "{\"INBOX\": 4}", NULL);
	save(f, a);
	venture_fake_imap_client_add_message(f->imap, "INBOX", 5, msg_ada);
	venture_fake_imap_client_add_message(f->imap, "INBOX", 6, msg_reply);
	g_assert_cmpint(venture_mail_sync_service_sync(f->service, a, NULL, &error), ==, 1);
	g_assert_no_error(error);
	g_assert_cmpint(venture_fake_imap_client_get_fetches(f->imap), ==, 1);
	g_assert_cmpint(count_type(f, "mail_inbound"), ==, 2);
	g_assert_cmpint(cursor_member(a, "INBOX", "uid"), ==, 6);
	g_assert_cmpint(cursor_member(a, "INBOX", "uidvalidity"), ==, 1);
}

/* --- Over a socket ----------------------------------------------------------------- */
/* The first sync of a real inbox. UID SEARCH answered on one line, refused
 * past 8 KiB (about 1,200 UIDs), so the cursor never moved and the account
 * failed forever. Listing pages UID ranges now, and the message budget
 * bounds one call; the next call resumes. */
static void test_socket_paged_listing(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureMailSyncService) service = socket_service(f);
	g_autoptr(VentureEntity) a = NULL;
	g_autoptr(GError) error = NULL;
	Stub stub;
	guint32 uid;
	stub_init(&stub, "IMAP4rev1 AUTH=PLAIN SASL-IR", "app-password");
	stub_folder(&stub, "INBOX", 7);
	for (uid = 1; uid <= 3000; uid++) {
		g_autofree gchar *id = g_strdup_printf("bulk%u@example.test", uid);
		g_autofree gchar *raw = mail("Ada <ada@example.test>", "ops@venture.test", "Numbered", id, NULL, "Body.");
		stub_add(&stub, "INBOX", uid, NULL, raw, 0);
	}
	a = socket_account(f, &stub, "INBOX", "VENTURE_IMAP_SECRET");
	g_object_set(service, "message-budget", 5, NULL);
	stub_start(&stub, 2);
	g_assert_cmpint(venture_mail_sync_service_sync(service, a, NULL, &error), ==, 5);
	g_assert_no_error(error);
	g_assert_cmpint(venture_mail_sync_service_sync(service, a, NULL, &error), ==, 5);
	g_assert_no_error(error);
	stub_finish(&stub);
	g_assert_cmpint(count_type(f, "mail_inbound"), ==, 10);
	g_assert_cmpint(cursor_member(a, "INBOX", "uid"), ==, 10);
	g_assert_cmpint(cursor_member(a, "INBOX", "uidvalidity"), ==, 7);
	g_assert_false(stub_has_command(&stub, "SEARCH"));
	g_assert_true(stub_has_command(&stub, "UID FETCH 1:500 (UID RFC822.SIZE)"));
	g_assert_true(stub_has_command(&stub, "UID FETCH 6:505 (UID RFC822.SIZE)"));
	stub_clear(&stub);
}

/* UIDs need not be dense: a folder whose old mail was deleted can jump by
 * billions. Walking the gap a window at a time never reached the far side
 * within the time budget, so the cursor never moved. */
static void test_socket_uid_gap(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureMailSyncService) service = socket_service(f);
	g_autoptr(VentureEntity) a = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *far = mail("Ada <ada@example.test>", "ops@venture.test", "Far away", "far@example.test", NULL, "After the gap.");
	g_autofree gchar *near = mail("Ada <ada@example.test>", "ops@venture.test", "Near", "near@example.test", NULL, "Before the gap.");
	guint windows = 0, i;
	Stub stub;
	stub_init(&stub, "IMAP4rev1 AUTH=PLAIN SASL-IR", NULL);
	stub_folder(&stub, "INBOX", 9);
	stub_add(&stub, "INBOX", 3, NULL, near, 0);
	stub_add(&stub, "INBOX", 4000000000u, NULL, far, 0);
	a = socket_account(f, &stub, "INBOX", "VENTURE_IMAP_SECRET");
	g_object_set(service, "time-budget", 5, NULL);
	stub_start(&stub, 1);
	g_assert_cmpint(venture_mail_sync_service_sync(service, a, NULL, &error), ==, 2);
	g_assert_no_error(error);
	stub_finish(&stub);
	g_assert_cmpint(cursor_member(a, "INBOX", "uid"), ==, 4000000000u);
	for (i = 0; i < stub.commands->len; i++)
		if (strstr(g_ptr_array_index(stub.commands, i), "(UID RFC822.SIZE)")) windows++;
	g_assert_cmpuint(windows, <=, 4);
	stub_clear(&stub);
}

/* A server migration renumbers UIDs and changes UIDVALIDITY. Ignoring it,
 * the old cursor skipped the new mail and colliding keys called it filed:
 * silent loss. A changed UIDVALIDITY restarts the folder, and the
 * Message-ID keeps mail already filed from being filed twice. */
static void test_socket_uidvalidity(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureMailSyncService) service = socket_service(f);
	g_autoptr(VentureEntity) a = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) inbound = NULL;
	g_autofree gchar *second = mail("Ada <ada@example.test>", "ops@venture.test", "Second", "second@example.test", NULL, "Two.");
	g_autofree gchar *third = mail("Ada <ada@example.test>", "ops@venture.test", "Third", "third@example.test", NULL, "Three.");
	Stub stub;
	stub_init(&stub, "IMAP4rev1 AUTH=PLAIN SASL-IR", NULL);
	stub_folder(&stub, "INBOX", 100);
	stub_add(&stub, "INBOX", 1, NULL, msg_ada, 0);
	stub_add(&stub, "INBOX", 2, NULL, second, 0);
	a = socket_account(f, &stub, "INBOX", "VENTURE_IMAP_SECRET");
	stub_start(&stub, 2);
	g_assert_cmpint(venture_mail_sync_service_sync(service, a, NULL, &error), ==, 2);
	g_assert_no_error(error);
	g_assert_cmpint(count_type(f, "interaction"), ==, 2);
	g_mutex_lock(&stub.lock);
	stub_folder(&stub, "INBOX", 200);
	g_ptr_array_set_size(stub_find(&stub, "INBOX")->messages, 0);
	stub_add(&stub, "INBOX", 1, NULL, second, 0);
	stub_add(&stub, "INBOX", 2, NULL, third, 0);
	g_mutex_unlock(&stub.lock);
	g_assert_cmpint(venture_mail_sync_service_sync(service, a, NULL, &error), ==, 2);
	g_assert_no_error(error);
	stub_finish(&stub);
	g_assert_cmpint(count_type(f, "interaction"), ==, 3);
	g_assert_cmpint(count_type(f, "mail_inbound"), ==, 4);
	g_assert_cmpint(cursor_member(a, "INBOX", "uidvalidity"), ==, 200);
	g_assert_cmpint(cursor_member(a, "INBOX", "uid"), ==, 2);
	inbound = rows(f, VENTURE_TYPE_MAIL_INBOUND);
	{
		gint64 duplicate_of = 0, validity = 0;
		g_object_get(g_ptr_array_index(inbound, 2), "duplicate-of-id", &duplicate_of, "uid-validity", &validity, NULL);
		g_assert_cmpint(duplicate_of, ==, venture_entity_get_id(g_ptr_array_index(inbound, 1)));
		g_assert_cmpint(validity, ==, 200);
	}
	stub_clear(&stub);
}

/* Non-ASCII folder names go out as modified UTF-7, and a refused folder is
 * recorded without stopping the folders after it. It used to break the
 * loop, so one renamed folder silenced every folder behind it. */
static void test_socket_folders(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureMailSyncService) service = socket_service(f);
	g_autoptr(VentureEntity) a = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *last_error = NULL;
	g_autofree gchar *draft = mail("Ada <ada@example.test>", "ops@venture.test", "Draft", "draft@example.test", NULL, "Draft.");
	gint64 failures = -1;
	Stub stub;
	stub_init(&stub, "IMAP4rev1 AUTH=PLAIN SASL-IR", NULL);
	stub_folder(&stub, "INBOX", 1);
	stub_add(&stub, "INBOX", 1, NULL, msg_ada, 0);
	stub_folder(&stub, "Entw&APw-rfe", 1);
	stub_add(&stub, "Entw&APw-rfe", 1, NULL, draft, 0);
	a = socket_account(f, &stub, "Missing, Entwürfe, INBOX", "VENTURE_IMAP_SECRET");
	stub_start(&stub, 1);
	g_assert_cmpint(venture_mail_sync_service_sync(service, a, NULL, &error), ==, -1);
	g_assert_nonnull(error);
	g_assert_nonnull(strstr(error->message, "Missing"));
	g_assert_nonnull(strstr(error->message, "NONEXISTENT"));
	g_assert_null(strstr(error->message, "no such mailbox"));
	stub_finish(&stub);
	g_assert_true(stub_has_command(&stub, "EXAMINE \"Entw&APw-rfe\""));
	g_assert_cmpint(count_type(f, "mail_inbound"), ==, 2);
	g_object_get(a, "last-error", &last_error, "consecutive-failures", &failures, NULL);
	g_assert_nonnull(strstr(last_error, "Missing"));
	/* A folder is not the account: no backoff for it. */
	g_assert_cmpint(failures, ==, 0);
	stub_clear(&stub);
}

/* A server that never ends a line. The client used to buffer the whole
 * line before checking its length, so a hostile or broken server could
 * make it allocate without limit; it must stop at a few KiB. */
static void test_socket_long_line(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureMailSyncService) service = socket_service(f);
	g_autoptr(VentureEntity) a = NULL;
	g_autoptr(GError) error = NULL;
	Stub stub;
	stub_init(&stub, "IMAP4rev1", NULL);
	stub.garbage = 64 * 1024 * 1024;
	a = socket_account(f, &stub, "INBOX", "VENTURE_IMAP_SECRET");
	stub_start(&stub, 1);
	g_assert_cmpint(venture_mail_sync_service_sync(service, a, NULL, &error), ==, -1);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NETWORK);
	g_assert_nonnull(strstr(error->message, "line too long"));
	stub_finish(&stub);
	g_assert_cmpuint(stub.garbage_sent, <, 32 * 1024 * 1024);
	stub_clear(&stub);
}

/* A server that trickles its greeting a byte at a time never trips a
 * per-read timeout, and the sync runs on the main loop: every request the
 * server answers waits behind it. The time budget bounds the whole call. */
static void test_socket_deadline(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureMailSyncService) service = socket_service(f);
	g_autoptr(VentureEntity) a = NULL;
	g_autoptr(GError) error = NULL;
	gint64 started, failures = 0;
	Stub stub;
	stub_init(&stub, "IMAP4rev1", NULL);
	stub.trickle = TRUE;
	a = socket_account(f, &stub, "INBOX", "VENTURE_IMAP_SECRET");
	g_object_set(service, "time-budget", 1, NULL);
	stub_start(&stub, 1);
	started = g_get_monotonic_time();
	g_assert_cmpint(venture_mail_sync_service_sync(service, a, NULL, &error), ==, -1);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_TIMEOUT);
	g_assert_cmpint(g_get_monotonic_time() - started, <, 5 * G_USEC_PER_SEC);
	stub_finish(&stub);
	g_object_get(a, "consecutive-failures", &failures, NULL);
	g_assert_cmpint(failures, ==, 1);
	stub_clear(&stub);
}

/* AUTHENTICATE PLAIN carries UTF-8 credentials unchanged; LOGIN with a
 * non-ASCII password needs a literal; LOGINDISABLED is honoured; and a
 * refused login names only the fixed response code, never the server's
 * text, which here repeats the password. */
static void run_auth_case(Fixture *f, const gchar *capabilities, const gchar *expected_secret, gint expected, Stub *stub, GError **error)
{
	g_autoptr(VentureMailSyncService) service = socket_service(f);
	g_autoptr(VentureEntity) a = NULL;
	stub_init(stub, capabilities, expected_secret);
	stub_folder(stub, "INBOX", 1);
	a = socket_account(f, stub, "INBOX", "VENTURE_IMAP_UTF8");
	stub_start(stub, 1);
	g_assert_cmpint(venture_mail_sync_service_sync(service, a, NULL, error), ==, expected);
	stub_finish(stub);
}
static void test_socket_authentication(Fixture *f, gconstpointer data)
{
	static const gchar *const secret = "p\xc3\xa4sswo\xcc\x88rd";
	g_autofree gchar *wanted = g_strdup_printf("ops@venture.test\n%s", secret);
	g_autoptr(GError) error = NULL;
	Stub stub;
	g_setenv("VENTURE_IMAP_UTF8", secret, TRUE);
	/* SASL-IR: the credentials ride on the command. */
	run_auth_case(f, "IMAP4rev1 AUTH=PLAIN SASL-IR", secret, 0, &stub, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(stub.credentials, ==, wanted);
	g_assert_true(stub_has_command(&stub, "AUTHENTICATE PLAIN "));
	g_assert_false(stub_has_command(&stub, "LOGIN"));
	stub_clear(&stub);
	/* Without SASL-IR the server asks with "+". */
	run_auth_case(f, "IMAP4rev1 AUTH=PLAIN", secret, 0, &stub, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(stub.credentials, ==, wanted);
	stub_clear(&stub);
	/* LOGIN only: the 8-bit password goes as a literal. */
	run_auth_case(f, "IMAP4rev1", secret, 0, &stub, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(stub.credentials, ==, wanted);
	g_assert_true(stub_has_command(&stub, "LOGIN \"ops@venture.test\" {11}"));
	stub_clear(&stub);
	/* Neither mechanism is allowed: nothing is sent at all. */
	run_auth_case(f, "IMAP4rev1 LOGINDISABLED", secret, -1, &stub, &error);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_UNSUPPORTED);
	g_assert_null(stub.credentials);
	g_clear_error(&error);
	stub_clear(&stub);
	/* Refused: the code is named, the echoed password is not. */
	run_auth_case(f, "IMAP4rev1 AUTH=PLAIN SASL-IR", "something-else", -1, &stub, &error);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_UNAUTHENTICATED);
	g_assert_nonnull(strstr(error->message, "AUTHENTICATIONFAILED"));
	g_assert_null(strstr(error->message, secret));
	g_assert_null(strstr(error->message, "Invalid credentials"));
	stub_clear(&stub);
	g_unsetenv("VENTURE_IMAP_UTF8");
}

/* An oversized message is listed with its size and read as its header plus
 * the start of its text, so the folder moves past it with a timeline entry
 * instead of refusing it forever. sync_since starts a folder's first sync
 * at a date by searching INTERNALDATE, never with one giant SEARCH reply. */
static void test_socket_oversize_and_since(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureMailSyncService) service = socket_service(f);
	g_autoptr(VentureEntity) a = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) inbound = NULL;
	g_autoptr(GDateTime) since = g_date_time_new_utc(2026, 9, 1, 0, 0, 0);
	g_autofree gchar *old = mail("Ada <ada@example.test>", "ops@venture.test", "January", "jan@example.test", NULL, "Old.");
	g_autofree gchar *mid = mail("Ada <ada@example.test>", "ops@venture.test", "September", "sep@example.test", NULL, "Recent.");
	g_autofree gchar *big = mail("Ada <ada@example.test>", "ops@venture.test", "Huge scan", "big@example.test", NULL, "Start of a very large message.");
	Stub stub;
	stub_init(&stub, "IMAP4rev1 AUTH=PLAIN SASL-IR", NULL);
	stub_folder(&stub, "INBOX", 1);
	stub_add(&stub, "INBOX", 10, "01-Jan-2026 09:00:00 +0000", old, 0);
	stub_add(&stub, "INBOX", 20, "10-Sep-2026 09:00:00 +0200", mid, 0);
	stub_add(&stub, "INBOX", 30, "15-Sep-2026 09:00:00 -0700", big, 30 * 1024 * 1024);
	a = socket_account(f, &stub, "INBOX", "VENTURE_IMAP_SECRET");
	g_object_set(a, "sync-since", since, NULL);
	save(f, a);
	stub_start(&stub, 1);
	g_assert_cmpint(venture_mail_sync_service_sync(service, a, NULL, &error), ==, 2);
	g_assert_no_error(error);
	stub_finish(&stub);
	g_assert_true(stub_has_command(&stub, "FETCH 3 (UID INTERNALDATE)"));
	g_assert_true(stub_has_command(&stub, "UID FETCH 30 (BODY.PEEK[HEADER] BODY.PEEK[TEXT]<0.65536>)"));
	g_assert_false(stub_has_command(&stub, "UID FETCH 10 "));
	inbound = rows(f, VENTURE_TYPE_MAIL_INBOUND);
	g_assert_cmpuint(inbound->len, ==, 2);
	{
		g_autofree gchar *reason = NULL, *subject = NULL;
		gint64 uid = 0, interaction = 0;
		g_object_get(g_ptr_array_index(inbound, 1), "uid", &uid, "skip-reason", &reason, "subject", &subject, "interaction-id", &interaction, NULL);
		g_assert_cmpint(uid, ==, 30);
		g_assert_nonnull(strstr(reason, "Oversize"));
		g_assert_cmpstr(subject, ==, "Huge scan");
		g_assert_cmpint(interaction, >, 0);
	}
	stub_clear(&stub);
}

/* --- Failures that used to stall an account ------------------------------------------- */
static gboolean refuse_poison(VentureDatabase *db, VentureEntity *entity, VentureEntity *previous, gpointer data, GError **error)
{
	g_autofree gchar *subject = NULL;
	(void)db; (void)previous; (void)data;
	g_object_get(entity, "subject", &subject, NULL);
	if (g_strcmp0(subject, "Poison") && g_strcmp0(subject, "Refuse me")) return TRUE;
	venture_set_error_validation(error, "subject", "refused by a test validator");
	return FALSE;
}
/* One message a save refuses used to stop the folder at that UID forever,
 * and the raw file written before the refusal stayed on disk each time.
 * Three attempts file a stub with the reason and the folder moves on; a
 * rolled-back attempt leaves no file behind and no stale account version. */
static void test_poison_message(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) a = account(f, "VENTURE_IMAP_SECRET");
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) inbound = NULL;
	g_autofree gchar *poison = mail("Ada <ada@example.test>", "ops@venture.test", "Poison", "poison@example.test", NULL, "Bad.");
	g_autofree gchar *after = mail("Ada <ada@example.test>", "ops@venture.test", "After", "after@example.test", NULL, "Fine.");
	gint i;
	venture_database_add_save_validator(f->db, VENTURE_TYPE_INTERACTION, refuse_poison, NULL, NULL);
	venture_fake_imap_client_add_message(f->imap, "INBOX", 1, msg_ada);
	venture_fake_imap_client_add_message(f->imap, "INBOX", 2, poison);
	venture_fake_imap_client_add_message(f->imap, "INBOX", 3, after);
	for (i = 1; i <= 2; i++) {
		g_assert_cmpint(venture_mail_sync_service_sync(f->service, a, NULL, &error), ==, -1);
		g_assert_nonnull(strstr(error->message, "UID 2"));
		g_clear_error(&error);
		g_assert_cmpint(count_type(f, "mail_inbound"), ==, 1);
		g_assert_cmpint(cursor_member(a, "INBOX", "uid"), ==, 1);
		g_assert_cmpint(cursor_member(a, "INBOX", "attempts"), ==, i);
		g_assert_cmpuint(count_files(f->root), ==, (guint)count_type(f, "document"));
	}
	g_assert_cmpint(venture_mail_sync_service_sync(f->service, a, NULL, &error), ==, 2);
	g_assert_no_error(error);
	g_assert_cmpint(cursor_member(a, "INBOX", "uid"), ==, 3);
	g_assert_cmpint(count_type(f, "interaction"), ==, 2);
	g_assert_cmpuint(count_files(f->root), ==, (guint)count_type(f, "document"));
	inbound = rows(f, VENTURE_TYPE_MAIL_INBOUND);
	g_assert_cmpuint(inbound->len, ==, 3);
	{
		g_autofree gchar *reason = NULL, *subject = NULL;
		gint64 document = -1;
		g_object_get(g_ptr_array_index(inbound, 1), "skip-reason", &reason, "document-id", &document, "subject", &subject, NULL);
		g_assert_nonnull(strstr(reason, "Skipped after 3 attempts"));
		g_assert_cmpint(document, ==, 0);
		g_assert_cmpstr(subject, ==, "Poison");
	}
}

/* An empty fetch (BODY[] NIL, a message expunged meanwhile) is the
 * message's problem and is skipped after its attempts; a dropped
 * connection is the wire's and spends no attempt. A listed size over the
 * limit is read truncated at once. */
static void test_fetch_failures_and_oversize(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) a = account(f, "VENTURE_IMAP_SECRET");
	g_autoptr(GError) error = NULL, empty = g_error_new_literal(VENTURE_ERROR, VENTURE_ERROR_MAIL_PERMANENT, "IMAP: the server returned no content for this message");
	g_autoptr(GError) dropped = g_error_new_literal(VENTURE_ERROR, VENTURE_ERROR_NETWORK, "IMAP: connection closed");
	g_autoptr(GPtrArray) inbound = NULL;
	g_autofree gchar *huge = mail("Ada <ada@example.test>", "ops@venture.test", "Huge", "huge@example.test", NULL, "Header survives.");
	gint64 failures = 0;
	gint i;
	venture_fake_imap_client_add_message(f->imap, "INBOX", 1, msg_ada);
	venture_fake_imap_client_set_fetch_error(f->imap, 1, dropped);
	g_assert_cmpint(venture_mail_sync_service_sync(f->service, a, NULL, &error), ==, -1);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NETWORK);
	g_clear_error(&error);
	g_assert_cmpint(cursor_member(a, "INBOX", "attempts"), ==, 0);
	g_object_get(a, "consecutive-failures", &failures, NULL);
	g_assert_cmpint(failures, ==, 1);
	venture_fake_imap_client_set_fetch_error(f->imap, 1, empty);
	for (i = 0; i < 2; i++) {
		g_assert_cmpint(venture_mail_sync_service_sync(f->service, a, NULL, &error), ==, -1);
		g_clear_error(&error);
	}
	venture_fake_imap_client_add_message(f->imap, "INBOX", 2, huge);
	venture_fake_imap_client_set_size(f->imap, "INBOX", 2, 25 * 1024 * 1024);
	g_assert_cmpint(venture_mail_sync_service_sync(f->service, a, NULL, &error), ==, 2);
	g_assert_no_error(error);
	g_object_get(a, "consecutive-failures", &failures, NULL);
	g_assert_cmpint(failures, ==, 0);
	inbound = rows(f, VENTURE_TYPE_MAIL_INBOUND);
	g_assert_cmpuint(inbound->len, ==, 2);
	{
		g_autofree gchar *first = NULL, *second = NULL;
		g_object_get(g_ptr_array_index(inbound, 0), "skip-reason", &first, NULL);
		g_object_get(g_ptr_array_index(inbound, 1), "skip-reason", &second, NULL);
		g_assert_nonnull(strstr(first, "no content"));
		g_assert_nonnull(strstr(second, "Oversize"));
	}
	g_assert_cmpint(count_type(f, "interaction"), ==, 1);
}

/* A 300-byte CJK subject and a long attachment name made file names over
 * NAME_MAX; the write failed with ENAMETOOLONG on every attempt. */
static void test_long_names(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) a = account(f, "VENTURE_IMAP_SECRET");
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) documents = NULL;
	g_autoptr(GString) subject = g_string_new(NULL), name = g_string_new(NULL);
	g_autofree gchar *timeline = NULL, *capture = NULL;
	guint i;
	for (i = 0; i < 100; i++) g_string_append(subject, "\xe6\x97\xa5");
	for (i = 0; i < 250; i++) g_string_append_c(name, 'b');
	g_string_append(name, ".txt");
	timeline = mail("Ada <ada@example.test>", "ops@venture.test", subject->str, "cjk@example.test", NULL, "Long subject.");
	capture = g_strdup_printf("From: Shop <shop@store.test>\r\nTo: receipts@venture.test\r\nSubject: %s\r\nMessage-ID: <long@store.test>\r\nMIME-Version: 1.0\r\n"
		"Content-Type: multipart/mixed; boundary=\"b\"\r\n\r\n--b\r\nContent-Type: text/plain\r\n\r\nSee attached.\r\n"
		"--b\r\nContent-Type: text/plain\r\nContent-Disposition: attachment; filename=\"%s\"\r\n\r\nTotal 5.00\r\n--b--\r\n", subject->str, name->str);
	venture_fake_imap_client_add_message(f->imap, "INBOX", 1, timeline);
	venture_fake_imap_client_add_message(f->imap, "INBOX", 2, capture);
	g_assert_cmpint(venture_mail_sync_service_sync(f->service, a, NULL, &error), ==, 2);
	g_assert_no_error(error);
	documents = rows(f, VENTURE_TYPE_DOCUMENT);
	g_assert_cmpuint(documents->len, ==, 3);
	for (i = 0; i < documents->len; i++) {
		g_autofree gchar *path = NULL, *title = NULL, *base = NULL;
		g_object_get(g_ptr_array_index(documents, i), "path", &path, "title", &title, NULL);
		base = g_path_get_basename(path);
		g_assert_cmpuint(strlen(base), <=, 255);
		g_assert_true(g_file_test(path, G_FILE_TEST_IS_REGULAR));
		/* The title keeps what the name had to drop. */
		g_assert_cmpuint(strlen(title), >=, 254);
	}
}

/* The unique index on an unmatched sender's address includes deleted
 * rows. A deleted sender, or one turned into a contact that was later
 * deleted, came back as an insert that violated it and stalled the account.
 * Dismissed addresses stay an ignore list. */
static void test_unmatched_lifecycle(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) a = account(f, "VENTURE_IMAP_SECRET");
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) senders = NULL;
	g_autoptr(VentureEntity) sender = NULL, contact = NULL, dismissed = NULL;
	g_autofree gchar *first = mail("Stranger <stranger@else.test>", "ops@venture.test", "One", "s1@else.test", NULL, "One.");
	g_autofree gchar *older = g_strdup_printf("From: stranger@else.test\r\nTo: ops@venture.test\r\nSubject: Older\r\nDate: Sun, 13 Sep 2026 10:00:00 +0000\r\n"
		"Message-ID: <s2@else.test>\r\nContent-Type: text/plain\r\n\r\nOlder.\r\n");
	g_autofree gchar *third = mail("stranger@else.test", "ops@venture.test", "Three", "s3@else.test", NULL, "Three.");
	g_autofree gchar *other1 = mail("Other <other@else.test>", "ops@venture.test", "Other one", "o1@else.test", NULL, "Hello from other.");
	g_autofree gchar *other2 = mail("Other <other@else.test>", "ops@venture.test", "Other two", "o2@else.test", NULL, "Again.");
	g_autofree gchar *other3 = mail("Other <other@else.test>", "ops@venture.test", "Other three", "o3@else.test", NULL, "Once more.");
	g_autoptr(GDateTime) last = NULL;
	gint64 seen = 0, contact_id = 0;
	gboolean is_dismissed = FALSE;
	venture_fake_imap_client_add_message(f->imap, "INBOX", 1, first);
	g_assert_cmpint(venture_mail_sync_service_sync(f->service, a, NULL, &error), ==, 1);
	g_assert_no_error(error);
	senders = rows(f, VENTURE_TYPE_MAIL_UNMATCHED_SENDER);
	sender = g_object_ref(g_ptr_array_index(senders, 0));
	g_assert_true(venture_database_delete(f->db, sender, NULL, &error));
	g_assert_no_error(error);
	venture_fake_imap_client_add_message(f->imap, "INBOX", 2, older);
	g_assert_cmpint(venture_mail_sync_service_sync(f->service, a, NULL, &error), ==, 1);
	g_assert_no_error(error);
	g_clear_object(&sender);
	sender = venture_database_get(f->db, VENTURE_TYPE_MAIL_UNMATCHED_SENDER, venture_entity_get_id(g_ptr_array_index(senders, 0)), &error);
	g_assert_false(venture_entity_is_deleted(sender));
	g_object_get(sender, "seen", &seen, "last-seen-at", &last, NULL);
	g_assert_cmpint(seen, ==, 2);
	/* last_seen_at never moves backwards for older mail. */
	g_assert_cmpint(g_date_time_get_day_of_month(last), ==, 14);
	dismissed = venture_mail_sync_service_dismiss(f->service, sender, NULL, &error);
	g_assert_no_error(error);
	venture_fake_imap_client_add_message(f->imap, "INBOX", 3, third);
	g_assert_cmpint(venture_mail_sync_service_sync(f->service, a, NULL, &error), ==, 1);
	g_assert_no_error(error);
	g_clear_object(&sender);
	sender = venture_database_get(f->db, VENTURE_TYPE_MAIL_UNMATCHED_SENDER, venture_entity_get_id(dismissed), &error);
	g_object_get(sender, "seen", &seen, "dismissed", &is_dismissed, NULL);
	g_assert_cmpint(seen, ==, 3);
	g_assert_true(is_dismissed);
	/* Two messages from another stranger, then a contact: both land on its
	 * timeline, and deleting the contact brings the sender row back. */
	venture_fake_imap_client_add_message(f->imap, "INBOX", 4, other1);
	venture_fake_imap_client_add_message(f->imap, "INBOX", 5, other2);
	g_assert_cmpint(venture_mail_sync_service_sync(f->service, a, NULL, &error), ==, 2);
	g_assert_no_error(error);
	g_clear_pointer(&senders, g_ptr_array_unref);
	senders = rows(f, VENTURE_TYPE_MAIL_UNMATCHED_SENDER);
	g_assert_cmpuint(senders->len, ==, 2);
	contact = venture_mail_sync_service_create_contact(f->service, g_ptr_array_index(senders, 1), NULL, &error);
	g_assert_no_error(error);
	{
		g_autoptr(VentureQuery) q = venture_query_new(VENTURE_TYPE_INTERACTION);
		venture_query_set_organization(q, f->org);
		venture_query_add_filter_int(q, "contact-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(contact), NULL);
		g_assert_cmpint(venture_database_count(f->db, q, NULL), ==, 2);
	}
	{
		g_autoptr(GPtrArray) inbound = rows(f, VENTURE_TYPE_MAIL_INBOUND);
		g_object_get(g_ptr_array_index(inbound, 3), "contact-id", &contact_id, NULL);
		g_assert_cmpint(contact_id, ==, venture_entity_get_id(contact));
	}
	g_assert_true(venture_database_delete(f->db, contact, NULL, &error));
	g_assert_no_error(error);
	venture_fake_imap_client_add_message(f->imap, "INBOX", 6, other3);
	g_assert_cmpint(venture_mail_sync_service_sync(f->service, a, NULL, &error), ==, 1);
	g_assert_no_error(error);
	g_assert_cmpint(count_type(f, "mail_unmatched_sender"), ==, 2);
}

/* A contact's deleted company failed the interaction's reference check:
 * inbound stalled, and the outbox rolled back the sent state of mail the
 * relay had accepted, so the operator's retry mailed the customer twice. */
static void test_deleted_company(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) a = account(f, "VENTURE_IMAP_SECRET");
	g_autoptr(VentureEntity) company = venture_database_get(f->db, VENTURE_TYPE_COMPANY, f->company, NULL);
	g_autoptr(VentureLogMailer) mailer = venture_log_mailer_new();
	g_autoptr(VentureMailOutbox) outbox = venture_mail_outbox_new(f->db, VENTURE_MAILER(mailer));
	g_autoptr(VentureMailMessage) message = venture_mail_message_new(), refused = venture_mail_message_new();
	g_autoptr(VentureMailMessage) queued = NULL, queued_refused = NULL;
	g_autoptr(GPtrArray) interactions = NULL;
	g_autoptr(VentureEntity) stored = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *state = NULL;
	gint64 company_id = -1;
	g_assert_true(venture_database_delete(f->db, company, NULL, &error));
	venture_fake_imap_client_add_message(f->imap, "INBOX", 1, msg_ada);
	g_assert_cmpint(venture_mail_sync_service_sync(f->service, a, NULL, &error), ==, 1);
	g_assert_no_error(error);
	interactions = rows(f, VENTURE_TYPE_INTERACTION);
	g_object_get(g_ptr_array_index(interactions, 0), "company-id", &company_id, NULL);
	g_assert_cmpint(company_id, ==, 0);
	g_object_set(message, "organization-id", f->org, "to", "ada@example.test", "subject", "Quote", "text-body", "Attached.", "idempotency-key", "dc1", NULL);
	queued = venture_mail_outbox_enqueue(outbox, message, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpint(venture_mail_outbox_deliver_due(outbox, f->org, 10, NULL, NULL, &error), ==, 1);
	g_assert_no_error(error);
	g_assert_cmpint(count_type(f, "interaction"), ==, 2);
	/* A timeline that cannot be written is logged; the send stands. */
	venture_database_add_save_validator(f->db, VENTURE_TYPE_INTERACTION, refuse_poison, NULL, NULL);
	g_object_set(refused, "organization-id", f->org, "to", "ada@example.test", "subject", "Refuse me", "text-body", "Hi.", "idempotency-key", "dc2", NULL);
	queued_refused = venture_mail_outbox_enqueue(outbox, refused, NULL, &error);
	g_assert_no_error(error);
	g_test_expect_message("Venture", G_LOG_LEVEL_WARNING, "*was sent but its timeline entry was not recorded*");
	g_assert_cmpint(venture_mail_outbox_deliver_due(outbox, f->org, 10, NULL, NULL, &error), ==, 1);
	g_test_assert_expected_messages();
	g_assert_no_error(error);
	stored = venture_database_get(f->db, VENTURE_TYPE_MAIL_MESSAGE, venture_entity_get_id(VENTURE_ENTITY(queued_refused)), &error);
	g_object_get(stored, "state", &state, NULL);
	g_assert_cmpstr(state, ==, "sent");
	g_assert_cmpint(count_type(f, "interaction"), ==, 2);
}

/* Account ops@ with capture ops+receipts@ normalised to the same address,
 * and the whole inbox became capture items. The capture address is exact
 * now, and one the account's own mail would match is refused at save. */
static void test_capture_address_exact(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) a = g_object_new(VENTURE_TYPE_MAIL_ACCOUNT, "organization-id", f->org, "address", "ops@venture.test",
		"imap-host", "imap.venture.test", "imap-tls", "tls", "username", "ops", "secret-env", "VENTURE_IMAP_SECRET", "folders", "INBOX",
		"capture-address", "ops+receipts@venture.test", "active", TRUE, NULL);
	g_autoptr(VentureEntity) tagged_account = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *plain = mail("Ada <ada@example.test>", "ops@venture.test", "Plain", "plain@example.test", NULL, "For the timeline.");
	g_autofree gchar *receipt = mail("Shop <shop@store.test>", "ops+receipts@venture.test", "Receipt", "rcpt@store.test", NULL, "Total 3.00");
	save(f, a);
	venture_fake_imap_client_add_message(f->imap, "INBOX", 1, plain);
	venture_fake_imap_client_add_message(f->imap, "INBOX", 2, receipt);
	g_assert_cmpint(venture_mail_sync_service_sync(f->service, a, NULL, &error), ==, 2);
	g_assert_no_error(error);
	g_assert_cmpint(count_type(f, "interaction"), ==, 1);
	g_assert_cmpint(count_type(f, "capture_item"), ==, 1);
	g_object_set(a, "capture-address", "OPS@Venture.test", NULL);
	g_assert_false(venture_database_save(f->db, a, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	tagged_account = g_object_new(VENTURE_TYPE_MAIL_ACCOUNT, "organization-id", f->org, "address", "ops+inbox@venture.test",
		"imap-host", "imap.venture.test", "capture-address", "ops@venture.test", NULL);
	g_assert_false(venture_database_save(f->db, tagged_account, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
}

/* A first sync used to fetch the whole mailbox in one call, holding the
 * main loop for as long as that took. The message budget bounds a call and
 * the cursor resumes; the lease keeps a second caller off an account that
 * is mid-sync. */
static void test_budget_and_lease(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) a = account(f, "VENTURE_IMAP_SECRET");
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) now = g_date_time_new_now_utc();
	g_autoptr(GDateTime) later = g_date_time_add_minutes(now, 10);
	g_autoptr(JsonNode) report = NULL;
	guint32 uid;
	for (uid = 1; uid <= 5; uid++) {
		g_autofree gchar *id = g_strdup_printf("budget%u@example.test", uid);
		g_autofree gchar *raw = mail("Ada <ada@example.test>", "ops@venture.test", "Budget", id, NULL, "Hi.");
		venture_fake_imap_client_add_message(f->imap, "INBOX", uid, raw);
	}
	g_object_set(f->service, "message-budget", 2, NULL);
	g_assert_cmpint(venture_mail_sync_service_sync(f->service, a, NULL, &error), ==, 2);
	g_assert_cmpint(cursor_member(a, "INBOX", "uid"), ==, 2);
	g_assert_cmpint(venture_mail_sync_service_sync(f->service, a, NULL, &error), ==, 2);
	g_assert_cmpint(venture_mail_sync_service_sync(f->service, a, NULL, &error), ==, 1);
	g_assert_cmpint(venture_mail_sync_service_sync(f->service, a, NULL, &error), ==, 0);
	g_assert_no_error(error);
	g_assert_cmpint(count_type(f, "mail_inbound"), ==, 5);
	g_object_set(a, "sync-lease-until", later, NULL);
	save(f, a);
	g_assert_cmpint(venture_mail_sync_service_sync(f->service, a, NULL, &error), ==, -1);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
	g_assert_cmpint(venture_fake_imap_client_get_connects(f->imap), ==, 4);
	report = venture_mail_sync_service_sweep(f->service, f->org, 100, NULL, NULL);
	g_assert_cmpint(venture_json_object_get_int(json_node_get_object(report), "skipped", 0), ==, 1);
	g_assert_cmpint(venture_json_object_get_int(json_node_get_object(report), "accounts", 0), ==, 0);
	g_assert_cmpint(venture_fake_imap_client_get_connects(f->imap), ==, 4);
}

/* The same Message-ID under two Gmail labels, or in a watched Sent folder
 * after the outbox logged the send, was filed and put on the timeline twice. */
static void test_message_id_dedupe(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) a = account(f, "VENTURE_IMAP_SECRET");
	g_autoptr(VentureLogMailer) mailer = venture_log_mailer_new();
	g_autoptr(VentureMailOutbox) outbox = venture_mail_outbox_new(f->db, VENTURE_MAILER(mailer));
	g_autoptr(VentureMailMessage) message = venture_mail_message_new();
	g_autoptr(VentureMailMessage) queued = NULL;
	g_autoptr(GPtrArray) inbound = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *message_id = NULL, *sent = NULL;
	gint64 first_interaction = 0, copy_interaction = 0, duplicate_of = 0, document = 0;
	g_object_set(a, "folders", "INBOX,[Gmail]/All Mail,Sent", NULL);
	save(f, a);
	venture_fake_imap_client_add_message(f->imap, "INBOX", 1, msg_ada);
	venture_fake_imap_client_add_message(f->imap, "[Gmail]/All Mail", 7, msg_ada);
	g_assert_cmpint(venture_mail_sync_service_sync(f->service, a, NULL, &error), ==, 2);
	g_assert_no_error(error);
	g_assert_cmpint(count_type(f, "interaction"), ==, 1);
	g_assert_cmpint(count_type(f, "document"), ==, 1);
	inbound = rows(f, VENTURE_TYPE_MAIL_INBOUND);
	g_object_get(g_ptr_array_index(inbound, 0), "interaction-id", &first_interaction, NULL);
	g_object_get(g_ptr_array_index(inbound, 1), "interaction-id", &copy_interaction, "duplicate-of-id", &duplicate_of, NULL);
	g_assert_cmpint(copy_interaction, ==, first_interaction);
	g_assert_cmpint(duplicate_of, ==, venture_entity_get_id(g_ptr_array_index(inbound, 0)));
	g_object_set(message, "organization-id", f->org, "to", "ada@example.test", "subject", "Quote", "text-body", "Attached.", "idempotency-key", "dd1", NULL);
	queued = venture_mail_outbox_enqueue(outbox, message, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpint(venture_mail_outbox_deliver_due(outbox, f->org, 10, NULL, NULL, &error), ==, 1);
	g_assert_cmpint(count_type(f, "interaction"), ==, 2);
	g_assert_cmpint(count_type(f, "mail_inbound"), ==, 3);
	g_object_get(queued, "message-id", &message_id, NULL);
	sent = mail("ops@venture.test", "Ada <ada@example.test>", "Quote", message_id, NULL, "Attached.");
	venture_fake_imap_client_add_message(f->imap, "Sent", 3, sent);
	g_assert_cmpint(venture_mail_sync_service_sync(f->service, a, NULL, &error), ==, 1);
	g_assert_no_error(error);
	g_assert_cmpint(count_type(f, "interaction"), ==, 2);
	g_clear_pointer(&inbound, g_ptr_array_unref);
	inbound = rows(f, VENTURE_TYPE_MAIL_INBOUND);
	g_assert_cmpuint(inbound->len, ==, 4);
	g_object_get(g_ptr_array_index(inbound, 3), "duplicate-of-id", &duplicate_of, "document-id", &document, NULL);
	g_assert_cmpint(duplicate_of, ==, venture_entity_get_id(g_ptr_array_index(inbound, 2)));
	/* The outbox kept no bytes; the Sent copy files them. */
	g_assert_cmpint(document, >, 0);
}

/* Undeclared 8-bit mail. PostgreSQL refuses invalid UTF-8 (a stalled
 * account) and SQLite stored it as invalid JSON. */
static void test_invalid_utf8(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) a = account(f, "VENTURE_IMAP_SECRET");
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) inbound = NULL, senders = NULL, interactions = NULL;
	static const gchar raw[] = "From: Caf\xe9 Owner <cafe@else.test>\r\nTo: ops@venture.test, Ada <ada@example.test>\r\nSubject: Men\xfc\r\n"
		"Message-ID: <latin@else.test>\r\nContent-Type: text/plain\r\n\r\nCaf\xe9 au lait \xff\r\n";
	g_autoptr(GBytes) bytes = g_bytes_new_static(raw, sizeof raw - 1);
	g_autofree gchar *subject = NULL, *name = NULL, *body = NULL;
	venture_fake_imap_client_add_bytes(f->imap, "INBOX", 1, bytes);
	g_assert_cmpint(venture_mail_sync_service_sync(f->service, a, NULL, &error), ==, 1);
	g_assert_no_error(error);
	inbound = rows(f, VENTURE_TYPE_MAIL_INBOUND);
	senders = rows(f, VENTURE_TYPE_MAIL_UNMATCHED_SENDER);
	interactions = rows(f, VENTURE_TYPE_INTERACTION);
	g_object_get(g_ptr_array_index(inbound, 0), "subject", &subject, NULL);
	g_object_get(g_ptr_array_index(senders, 0), "name", &name, NULL);
	g_object_get(g_ptr_array_index(interactions, 0), "body", &body, NULL);
	g_assert_true(g_utf8_validate(subject, -1, NULL));
	g_assert_true(g_utf8_validate(name, -1, NULL));
	g_assert_true(g_utf8_validate(body, -1, NULL));
	g_assert_cmpstr(subject, ==, "Men\xc3\xbc");
	g_assert_nonnull(strstr(name, "Caf\xc3\xa9"));
	g_assert_nonnull(strstr(body, "Caf\xc3\xa9 au lait"));
}

/* Broken credentials retried LOGIN every sweep until the provider locked
 * the mailbox. Failures back off exponentially, the sweep honours it, and
 * five in a row ending in a refused login switch the account off and tell
 * the admins. Success resets the count. */
static void test_auth_backoff(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) a = account(f, "VENTURE_IMAP_SECRET");
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureContext) context = venture_context_new(config, f->db);
	g_autoptr(VentureUser) admin = venture_user_new();
	g_autoptr(GError) refused = g_error_new_literal(VENTURE_ERROR, VENTURE_ERROR_UNAUTHENTICATED, "IMAP: authentication refused [AUTHENTICATIONFAILED]");
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) report = NULL;
	g_autoptr(GDateTime) now = g_date_time_new_now_utc();
	gint64 failures = 0;
	gboolean active = FALSE;
	gint i;
	g_object_set(admin, "username", "root", "role", VENTURE_USER_ROLE_ADMIN, "active", TRUE, NULL);
	save(f, VENTURE_ENTITY(admin));
	g_object_set(f->service, "context", context, NULL);
	venture_fake_imap_client_set_connect_error(f->imap, refused);
	for (i = 1; i <= 4; i++) {
		g_autoptr(GDateTime) next = NULL;
		g_assert_cmpint(venture_mail_sync_service_sync(f->service, a, NULL, &error), ==, -1);
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_UNAUTHENTICATED);
		g_clear_error(&error);
		g_object_get(a, "consecutive-failures", &failures, "next-attempt-at", &next, "active", &active, NULL);
		g_assert_cmpint(failures, ==, i);
		g_assert_nonnull(next);
		g_assert_cmpint(g_date_time_difference(next, now), >=, (60 << (i - 1)) * G_USEC_PER_SEC - G_USEC_PER_SEC);
		g_assert_true(active);
	}
	report = venture_mail_sync_service_sweep(f->service, f->org, 100, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpint(venture_json_object_get_int(json_node_get_object(report), "skipped", 0), ==, 1);
	g_assert_cmpint(venture_fake_imap_client_get_connects(f->imap), ==, 4);
	g_assert_cmpint(count_type(f, "notification"), ==, 0);
	g_assert_cmpint(venture_mail_sync_service_sync(f->service, a, NULL, &error), ==, -1);
	g_clear_error(&error);
	g_object_get(a, "active", &active, NULL);
	g_assert_false(active);
	g_assert_cmpint(venture_notify_unread_count(context, venture_entity_get_id(VENTURE_ENTITY(admin))), ==, 1);
	venture_fake_imap_client_set_connect_error(f->imap, NULL);
	g_object_set(a, "active", TRUE, NULL);
	save(f, a);
	g_assert_cmpint(venture_mail_sync_service_sync(f->service, a, NULL, &error), ==, 0);
	g_assert_no_error(error);
	{
		g_autoptr(GDateTime) next = NULL;
		g_object_get(a, "consecutive-failures", &failures, "next-attempt-at", &next, NULL);
		g_assert_cmpint(failures, ==, 0);
		g_assert_null(next);
	}
}

/* HTML-only mail left the timeline body empty, and an HTML-only receipt
 * sent to the capture address was never captured. */
static void test_html_only(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) a = account(f, "VENTURE_IMAP_SECRET");
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) interactions = NULL, items = NULL;
	g_autoptr(VentureEntity) document = NULL;
	g_autofree gchar *body = NULL, *mime = NULL, *text = NULL;
	gint64 document_id = 0;
	venture_fake_imap_client_add_message(f->imap, "INBOX", 1,
		"From: Ada <ada@example.test>\r\nTo: ops@venture.test\r\nSubject: Styled\r\nMessage-ID: <html1@example.test>\r\nMIME-Version: 1.0\r\n"
		"Content-Type: text/html; charset=utf-8\r\n\r\n<html><body><p>Hello <b>Ops</b></p></body></html>\r\n");
	venture_fake_imap_client_add_message(f->imap, "INBOX", 2,
		"From: Shop <shop@store.test>\r\nTo: receipts@venture.test\r\nSubject: Order 7\r\nMessage-ID: <html2@store.test>\r\nMIME-Version: 1.0\r\n"
		"Content-Type: text/html; charset=utf-8\r\n\r\n<table><tr><td>Total</td><td>42.00</td></tr></table>\r\n");
	g_assert_cmpint(venture_mail_sync_service_sync(f->service, a, NULL, &error), ==, 2);
	g_assert_no_error(error);
	interactions = rows(f, VENTURE_TYPE_INTERACTION);
	g_object_get(g_ptr_array_index(interactions, 0), "body", &body, NULL);
	g_assert_nonnull(strstr(body, "Hello Ops"));
	g_assert_null(strstr(body, "<p>"));
	items = rows(f, VENTURE_TYPE_CAPTURE_ITEM);
	g_assert_cmpuint(items->len, ==, 1);
	g_object_get(g_ptr_array_index(items, 0), "document-id", &document_id, NULL);
	document = venture_database_get(f->db, VENTURE_TYPE_DOCUMENT, document_id, &error);
	g_object_get(document, "mime-type", &mime, "extracted-text", &text, NULL);
	g_assert_cmpstr(mime, ==, "text/html");
	g_assert_nonnull(strstr(text, "Total 42.00"));
}

/* Inline logos, S/MIME signatures, invitations and winmail.dat became
 * capture items, while a receipt forwarded as message/rfc822 was dropped. */
static void test_capture_attachments(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) a = account(f, "VENTURE_IMAP_SECRET");
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) items = NULL;
	g_autoptr(VentureEntity) document = NULL;
	g_autofree gchar *title = NULL, *text = NULL;
	gint64 document_id = 0;
	venture_fake_imap_client_add_message(f->imap, "INBOX", 1,
		"From: Me <me@else.test>\r\nTo: receipts@venture.test\r\nSubject: Fwd: Your ink\r\nMessage-ID: <fwd@else.test>\r\nMIME-Version: 1.0\r\n"
		"Content-Type: multipart/mixed; boundary=\"outer\"\r\n\r\n"
		"--outer\r\nContent-Type: text/plain\r\n\r\nSee below.\r\n"
		"--outer\r\nContent-Type: image/png; name=\"logo.png\"\r\nContent-Disposition: inline; filename=\"logo.png\"\r\nContent-ID: <logo@else.test>\r\n"
		"Content-Transfer-Encoding: base64\r\n\r\niVBORw0KGgo=\r\n"
		"--outer\r\nContent-Type: application/pkcs7-signature; name=\"smime.p7s\"\r\nContent-Disposition: attachment; filename=\"smime.p7s\"\r\n\r\nsig\r\n"
		"--outer\r\nContent-Type: text/calendar; name=\"invite.ics\"\r\nContent-Disposition: attachment; filename=\"invite.ics\"\r\n\r\nBEGIN:VCALENDAR\r\n"
		"--outer\r\nContent-Type: application/ms-tnef; name=\"winmail.dat\"\r\nContent-Disposition: attachment; filename=\"winmail.dat\"\r\n\r\ntnef\r\n"
		"--outer\r\nContent-Type: message/rfc822\r\n\r\n"
		"From: Ink Shop <ink@shop.test>\r\nTo: me@else.test\r\nSubject: Your ink\r\nMessage-ID: <inner@shop.test>\r\nMIME-Version: 1.0\r\n"
		"Content-Type: multipart/mixed; boundary=\"inner\"\r\n\r\n"
		"--inner\r\nContent-Type: text/plain\r\n\r\nThanks.\r\n"
		"--inner\r\nContent-Type: text/csv; name=\"receipt.csv\"\r\nContent-Disposition: attachment; filename=\"receipt.csv\"\r\n\r\nitem,amount\r\nink,9.00\r\n"
		"--inner--\r\n"
		"--outer--\r\n");
	g_assert_cmpint(venture_mail_sync_service_sync(f->service, a, NULL, &error), ==, 1);
	g_assert_no_error(error);
	items = rows(f, VENTURE_TYPE_CAPTURE_ITEM);
	g_assert_cmpuint(items->len, ==, 1);
	g_object_get(g_ptr_array_index(items, 0), "document-id", &document_id, NULL);
	document = venture_database_get(f->db, VENTURE_TYPE_DOCUMENT, document_id, &error);
	g_object_get(document, "title", &title, "extracted-text", &text, NULL);
	g_assert_cmpstr(title, ==, "receipt.csv");
	g_assert_nonnull(strstr(text, "ink,9.00"));
}

/* message/rfc822 matched the text-like MIME prefix, so the raw email --
 * headers, boundaries, base64 -- was stored as the document's searchable
 * text. A missing Message-ID put every such message in thread "" and
 * would have made them all copies of one another. */
static void test_raw_text_and_missing_message_id(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) a = account(f, "VENTURE_IMAP_SECRET");
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) documents = NULL, inbound = NULL;
	g_autofree gchar *text = NULL, *t1 = NULL, *t2 = NULL, *id3 = NULL;
	g_autofree gchar *anonymous1 = mail("Ada <ada@example.test>", "ops@venture.test", "No id one", NULL, NULL, "First anonymous.");
	g_autofree gchar *anonymous2 = mail("Ada <ada@example.test>", "ops@venture.test", "No id two", NULL, NULL, "Second anonymous.");
	gint64 duplicate_of = 0;
	g_object_set(a, "folders", "INBOX,Archive", NULL);
	save(f, a);
	venture_fake_imap_client_add_message(f->imap, "INBOX", 1, msg_ada);
	venture_fake_imap_client_add_message(f->imap, "INBOX", 2, anonymous1);
	venture_fake_imap_client_add_message(f->imap, "INBOX", 3, anonymous2);
	venture_fake_imap_client_add_message(f->imap, "Archive", 1, anonymous1);
	g_assert_cmpint(venture_mail_sync_service_sync(f->service, a, NULL, &error), ==, 4);
	g_assert_no_error(error);
	documents = rows(f, VENTURE_TYPE_DOCUMENT);
	g_object_get(g_ptr_array_index(documents, 0), "extracted-text", &text, NULL);
	g_assert_nonnull(strstr(text, "First message."));
	g_assert_null(strstr(text, "Message-ID"));
	inbound = rows(f, VENTURE_TYPE_MAIL_INBOUND);
	g_object_get(g_ptr_array_index(inbound, 1), "thread-id", &t1, NULL);
	g_object_get(g_ptr_array_index(inbound, 2), "thread-id", &t2, "message-id", &id3, NULL);
	g_assert_cmpstr(t1, !=, t2);
	g_assert_true(g_str_has_suffix(id3, "@venture.invalid"));
	g_assert_cmpint(count_type(f, "interaction"), ==, 3);
	g_object_get(g_ptr_array_index(inbound, 3), "duplicate-of-id", &duplicate_of, NULL);
	g_assert_cmpint(duplicate_of, ==, venture_entity_get_id(g_ptr_array_index(inbound, 1)));
}

/* Ignore patterns, internal domains and bulk mail never become unmatched
 * senders, and a capture folder nobody listed is still read. */
static void test_ignore_rules(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) a = account(f, "VENTURE_IMAP_SECRET");
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) senders = NULL;
	g_autofree gchar *noreply = mail("noreply@shop.test", "ops@venture.test", "Shipped", "n1@shop.test", NULL, "Shipped.");
	g_autofree gchar *alerts = mail("alerts@notifications.example", "ops@venture.test", "Alert", "n2@notifications.example", NULL, "Alert.");
	g_autofree gchar *colleague = mail("colleague@venture.test", "ops@venture.test", "Lunch", "n3@venture.test", NULL, "Lunch?");
	g_autofree gchar *listed = mail("news@list.test", "ops@venture.test", "Digest", "n4@list.test", "List-Id: <news.list.test>\r\n", "News.");
	g_autofree gchar *automatic = mail("bot@auto.test", "ops@venture.test", "Out of office", "n5@auto.test", "Auto-Submitted: auto-replied\r\n", "Away.");
	g_autofree gchar *bulk = mail("bulk@mass.test", "ops@venture.test", "Offer", "n6@mass.test", "Precedence: bulk\r\n", "Buy.");
	g_autofree gchar *person = mail("Real Person <real@else.test>", "ops@venture.test", "Question", "n7@else.test", "Auto-Submitted: no\r\n", "Hello?");
	g_object_set(a, "ignore-patterns", "noreply@*\n*@notifications.example", "internal-domains", "venture.test", NULL);
	save(f, a);
	venture_fake_imap_client_add_message(f->imap, "INBOX", 1, noreply);
	venture_fake_imap_client_add_message(f->imap, "INBOX", 2, alerts);
	venture_fake_imap_client_add_message(f->imap, "INBOX", 3, colleague);
	venture_fake_imap_client_add_message(f->imap, "INBOX", 4, listed);
	venture_fake_imap_client_add_message(f->imap, "INBOX", 5, automatic);
	venture_fake_imap_client_add_message(f->imap, "INBOX", 6, bulk);
	venture_fake_imap_client_add_message(f->imap, "INBOX", 7, person);
	venture_fake_imap_client_add_message(f->imap, "Receipts", 1, msg_receipt);
	g_assert_cmpint(venture_mail_sync_service_sync(f->service, a, NULL, &error), ==, 8);
	g_assert_no_error(error);
	senders = rows(f, VENTURE_TYPE_MAIL_UNMATCHED_SENDER);
	g_assert_cmpuint(senders->len, ==, 1);
	{
		g_autofree gchar *address = NULL;
		g_object_get(g_ptr_array_index(senders, 0), "address", &address, NULL);
		g_assert_cmpstr(address, ==, "real@else.test");
	}
	g_assert_cmpint(count_type(f, "capture_item"), ==, 1);
}

/*
 * One hostile message cannot make one sync do unbounded work: past a
 * hundred recipients it is bulk (nobody becomes an unmatched sender, where
 * each used to be a lookup and a save), a References header of thousands
 * of ids is trimmed to its root and latest ancestors, and at most
 * twenty-five attachments become capture items.
 */
static void test_hostile_message_bounds(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) a = account(f, "VENTURE_IMAP_SECRET");
	g_autoptr(GError) error = NULL;
	g_autoptr(GString) to = g_string_new(NULL);
	g_autoptr(GString) references = g_string_new("References:");
	g_autoptr(GString) receipt = g_string_new(NULL);
	g_autofree gchar *wide = NULL;
	g_autofree gchar *thread = NULL;
	g_autoptr(GPtrArray) inbound = NULL;
	guint i;
	(void)data;
	for (i = 0; i < 150; i++)
		g_string_append_printf(to, "%srcpt%u@wide.test", i ? ", " : "", i);
	for (i = 0; i < 5000; i++)
		g_string_append_printf(references, " <ref%u@wide.test>", i);
	g_string_append(references, "\r\n");
	wide = mail("sender@wide.test", to->str, "Everyone", "wide1@wide.test", references->str, "Hello all.");
	g_string_append(receipt, "From: Shop <shop@store.test>\r\nTo: receipts@venture.test\r\nSubject: Many\r\n"
		"Date: Tue, 15 Sep 2026 09:00:00 +0000\r\nMessage-ID: <many@store.test>\r\nMIME-Version: 1.0\r\n"
		"Content-Type: multipart/mixed; boundary=\"b\"\r\n\r\n--b\r\nContent-Type: text/plain\r\n\r\nReceipts.\r\n");
	for (i = 0; i < 40; i++)
		g_string_append_printf(receipt, "--b\r\nContent-Type: text/csv; name=\"r%u.csv\"\r\n"
			"Content-Disposition: attachment; filename=\"r%u.csv\"\r\n\r\nitem,amount\r\nthing%u,1.00\r\n", i, i, i);
	g_string_append(receipt, "--b--\r\n");
	venture_fake_imap_client_add_message(f->imap, "INBOX", 1, wide);
	venture_fake_imap_client_add_message(f->imap, "Receipts", 1, receipt->str);
	g_assert_cmpint(venture_mail_sync_service_sync(f->service, a, NULL, &error), ==, 2);
	g_assert_no_error(error);
	g_assert_cmpint(count_type(f, "mail_unmatched_sender"), ==, 0);
	g_assert_cmpint(count_type(f, "capture_item"), ==, 25);
	inbound = rows(f, VENTURE_TYPE_MAIL_INBOUND);
	for (i = 0; i < inbound->len; i++)
	{
		g_autofree gchar *id = NULL;
		g_object_get(g_ptr_array_index(inbound, i), "message-id", &id, NULL);
		if (g_strcmp0(id, "wide1@wide.test") == 0)
			g_object_get(g_ptr_array_index(inbound, i), "thread-id", &thread, NULL);
	}
	g_assert_cmpstr(thread, ==, "ref0@wide.test");
}

/* sync_since with the fake: a folder's first sync starts at the date. */
static void test_sync_since(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) a = account(f, "VENTURE_IMAP_SECRET");
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) since = g_date_time_new_utc(2026, 9, 1, 0, 0, 0);
	g_autoptr(GDateTime) before = g_date_time_new_utc(2026, 8, 1, 0, 0, 0), after = g_date_time_new_utc(2026, 9, 2, 0, 0, 0);
	g_autofree gchar *old = mail("Ada <ada@example.test>", "ops@venture.test", "Old", "old@example.test", NULL, "Old.");
	g_autofree gchar *fresh = mail("Ada <ada@example.test>", "ops@venture.test", "Fresh", "fresh@example.test", NULL, "Fresh.");
	g_object_set(a, "sync-since", since, NULL);
	save(f, a);
	venture_fake_imap_client_add_message(f->imap, "INBOX", 1, old);
	venture_fake_imap_client_set_internal_date(f->imap, "INBOX", 1, before);
	venture_fake_imap_client_add_message(f->imap, "INBOX", 2, fresh);
	venture_fake_imap_client_set_internal_date(f->imap, "INBOX", 2, after);
	g_assert_cmpint(venture_mail_sync_service_sync(f->service, a, NULL, &error), ==, 1);
	g_assert_no_error(error);
	g_assert_cmpint(cursor_member(a, "INBOX", "uid"), ==, 2);
}

/* A lead's mail went to the unmatched list, deals were never linked, and
 * a sender turned into a contact kept none of its earlier mail. */
static void test_leads_and_deals(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) a = account(f, "VENTURE_IMAP_SECRET");
	g_autoptr(VentureEntity) lead = g_object_new(VENTURE_TYPE_LEAD, "organization-id", f->org, "name", "Prospect", "email", "lead@prospect.test", NULL);
	g_autoptr(VentureEntity) deal = NULL, second_deal = NULL;
	g_autoptr(GPtrArray) interactions = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *from_lead = mail("Lead <lead@prospect.test>", "ops@venture.test", "Interested", "lead1@prospect.test", NULL, "Tell me more.");
	g_autofree gchar *first = mail("Ada <ada@example.test>", "ops@venture.test", "Proposal", "deal1@example.test", NULL, "About the deal.");
	g_autofree gchar *unrelated = mail("Ada <ada@example.test>", "ops@venture.test", "Other topic", "deal2@example.test", NULL, "Something else.");
	g_autofree gchar *reply = mail("Ada <ada@example.test>", "ops@venture.test", "Re: Proposal", "deal3@example.test", "In-Reply-To: <deal1@example.test>\r\n", "Reply.");
	gint64 lead_id = 0, deal_id = -1;
	save(f, lead);
	deal = g_object_new(VENTURE_TYPE_DEAL, "organization-id", f->org, "name", "Website", "contact-id", f->contact, "stage", VENTURE_DEAL_STAGE_QUALIFIED, NULL);
	save(f, deal);
	venture_fake_imap_client_add_message(f->imap, "INBOX", 1, from_lead);
	venture_fake_imap_client_add_message(f->imap, "INBOX", 2, first);
	g_assert_cmpint(venture_mail_sync_service_sync(f->service, a, NULL, &error), ==, 2);
	g_assert_no_error(error);
	g_assert_cmpint(count_type(f, "mail_unmatched_sender"), ==, 0);
	second_deal = g_object_new(VENTURE_TYPE_DEAL, "organization-id", f->org, "name", "Support", "contact-id", f->contact, "stage", VENTURE_DEAL_STAGE_PROPOSAL, NULL);
	save(f, second_deal);
	venture_fake_imap_client_add_message(f->imap, "INBOX", 3, unrelated);
	venture_fake_imap_client_add_message(f->imap, "INBOX", 4, reply);
	g_assert_cmpint(venture_mail_sync_service_sync(f->service, a, NULL, &error), ==, 2);
	g_assert_no_error(error);
	interactions = rows(f, VENTURE_TYPE_INTERACTION);
	g_assert_cmpuint(interactions->len, ==, 4);
	g_object_get(g_ptr_array_index(interactions, 0), "lead-id", &lead_id, NULL);
	g_assert_cmpint(lead_id, ==, venture_entity_get_id(lead));
	g_object_get(g_ptr_array_index(interactions, 1), "deal-id", &deal_id, NULL);
	g_assert_cmpint(deal_id, ==, venture_entity_get_id(deal));
	/* Two open deals and no thread: nothing says which. */
	g_object_get(g_ptr_array_index(interactions, 2), "deal-id", &deal_id, NULL);
	g_assert_cmpint(deal_id, ==, 0);
	/* The thread already names one. */
	g_object_get(g_ptr_array_index(interactions, 3), "deal-id", &deal_id, NULL);
	g_assert_cmpint(deal_id, ==, venture_entity_get_id(deal));
}

/* Migration 000340 verifies the indexes and columns the sync relies on,
 * and must pass with the module off, where the tables do not exist. */
static void test_migration(Fixture *f, gconstpointer data)
{
	static const gchar *const checks[] = {
		"SELECT CAST(COUNT(*) AS BIGINT) FROM sqlite_master WHERE type = 'index' AND name IN ('uq_mail_inbounds_organization_uid_key', 'idx_mail_inbounds_from_address', 'uq_mail_unmatched_senders_organization_address')",
		"SELECT CAST(COUNT(*) AS BIGINT) FROM schema_migrations WHERE version = 340"
	};
	static const gint64 expected[] = { 3, 1 };
	g_autoptr(GError) error = NULL;
	guint i;
	for (i = 0; i < G_N_ELEMENTS(checks); i++) {
		const gchar *sql = checks[i];
		g_autoptr(OrmResult) result = NULL;
		if (i == 0 && venture_database_get_backend(f->db) == VENTURE_DATABASE_BACKEND_POSTGRES)
			sql = "SELECT CAST(COUNT(*) AS BIGINT) FROM pg_indexes WHERE schemaname=current_schema() AND indexname IN ('uq_mail_inbounds_organization_uid_key', 'idx_mail_inbounds_from_address', 'uq_mail_unmatched_senders_organization_address')";
		result = venture_database_query_raw(f->db, sql, NULL, &error);
		g_assert_no_error(error);
		g_assert_true(orm_result_next(result));
		g_assert_cmpint(orm_row_get_integer(orm_result_get_row(result), 0), ==, expected[i]);
	}
	{
		g_autoptr(VentureConfig) config = venture_config_new();
		g_autoptr(VentureConfig) everything = NULL;
		g_autoptr(VentureModuleRegistry) registry = NULL;
		g_autoptr(VentureDatabase) db = NULL;
		g_autoptr(VentureContext) context = NULL;
		g_autoptr(OrmResult) result = NULL;
		venture_config_set_module_enabled(config, "mail_sync", FALSE);
		db = venture_database_new("sqlite://:memory:", &error);
		g_assert_no_error(error);
		context = venture_context_new(config, db);
		g_assert_true(venture_database_migrate(db, venture_entity_registry_get_default(), &error));
		g_assert_no_error(error);
		result = venture_database_query_raw(db, "SELECT CAST(COUNT(*) AS BIGINT) FROM sqlite_master WHERE type = 'table' AND name = 'mail_inbounds'", NULL, &error);
		g_assert_no_error(error);
		g_assert_true(orm_result_next(result));
		g_assert_cmpint(orm_row_get_integer(orm_result_get_row(result), 0), ==, 0);
		g_clear_object(&result);
		g_clear_object(&context);
		g_clear_object(&db);
		everything = venture_config_new();
		registry = venture_module_registry_new();
		venture_module_registry_register_builtins(registry);
		g_assert_true(venture_module_registry_configure(registry, everything, NULL));
		venture_module_registry_apply(registry, venture_entity_registry_get_default());
	}
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/mail-sync/records", test_records);
	g_test_add_func("/mail-sync/utf7", test_utf7);
	g_test_add_func("/mail-sync/html-to-text", test_html_to_text);
	g_test_add("/mail-sync/missing-secret", Fixture, NULL, setup, test_missing_secret, teardown);
	g_test_add("/mail-sync/secret-env-prefix", Fixture, NULL, setup, test_secret_env_prefix, teardown);
	g_test_add("/mail-sync/matches-and-threads", Fixture, NULL, setup, test_sync_matches_and_threads, teardown);
	g_test_add("/mail-sync/capture", Fixture, NULL, setup, test_capture, teardown);
	g_test_add("/mail-sync/capture-plus-tag", Fixture, NULL, setup, test_capture_plus_tag, teardown);
	g_test_add("/mail-sync/outbound", Fixture, NULL, setup, test_outbound_recorded, teardown);
	g_test_add("/mail-sync/sweep", Fixture, NULL, setup, test_sweep, teardown);
	g_test_add("/mail-sync/cursor-repairs-duplicate", Fixture, NULL, setup, test_cursor_repairs_duplicate_uid, teardown);
	g_test_add("/mail-sync/legacy-cursor-and-keys", Fixture, NULL, setup, test_legacy_cursor_and_keys, teardown);
	g_test_add("/mail-sync/socket/paged-listing", Fixture, NULL, setup, test_socket_paged_listing, teardown);
	g_test_add("/mail-sync/socket/uid-gap", Fixture, NULL, setup, test_socket_uid_gap, teardown);
	g_test_add("/mail-sync/socket/uidvalidity", Fixture, NULL, setup, test_socket_uidvalidity, teardown);
	g_test_add("/mail-sync/socket/folders", Fixture, NULL, setup, test_socket_folders, teardown);
	g_test_add("/mail-sync/socket/long-line", Fixture, NULL, setup, test_socket_long_line, teardown);
	g_test_add("/mail-sync/socket/deadline", Fixture, NULL, setup, test_socket_deadline, teardown);
	g_test_add("/mail-sync/socket/authentication", Fixture, NULL, setup, test_socket_authentication, teardown);
	g_test_add("/mail-sync/socket/oversize-and-since", Fixture, NULL, setup, test_socket_oversize_and_since, teardown);
	g_test_add("/mail-sync/poison-message", Fixture, NULL, setup, test_poison_message, teardown);
	g_test_add("/mail-sync/fetch-failures-and-oversize", Fixture, NULL, setup, test_fetch_failures_and_oversize, teardown);
	g_test_add("/mail-sync/long-names", Fixture, NULL, setup, test_long_names, teardown);
	g_test_add("/mail-sync/unmatched-lifecycle", Fixture, NULL, setup, test_unmatched_lifecycle, teardown);
	g_test_add("/mail-sync/deleted-company", Fixture, NULL, setup, test_deleted_company, teardown);
	g_test_add("/mail-sync/capture-address-exact", Fixture, NULL, setup, test_capture_address_exact, teardown);
	g_test_add("/mail-sync/budget-and-lease", Fixture, NULL, setup, test_budget_and_lease, teardown);
	g_test_add("/mail-sync/message-id-dedupe", Fixture, NULL, setup, test_message_id_dedupe, teardown);
	g_test_add("/mail-sync/invalid-utf8", Fixture, NULL, setup, test_invalid_utf8, teardown);
	g_test_add("/mail-sync/auth-backoff", Fixture, NULL, setup, test_auth_backoff, teardown);
	g_test_add("/mail-sync/html-only", Fixture, NULL, setup, test_html_only, teardown);
	g_test_add("/mail-sync/capture-attachments", Fixture, NULL, setup, test_capture_attachments, teardown);
	g_test_add("/mail-sync/raw-text-and-missing-message-id", Fixture, NULL, setup, test_raw_text_and_missing_message_id, teardown);
	g_test_add("/mail-sync/ignore-rules", Fixture, NULL, setup, test_ignore_rules, teardown);
	g_test_add("/mail-sync/hostile-message-bounds", Fixture, NULL, setup, test_hostile_message_bounds, teardown);
	g_test_add("/mail-sync/sync-since", Fixture, NULL, setup, test_sync_since, teardown);
	g_test_add("/mail-sync/leads-and-deals", Fixture, NULL, setup, test_leads_and_deals, teardown);
	g_test_add("/mail-sync/migration", Fixture, NULL, setup, test_migration, teardown);
	return g_test_run();
}
