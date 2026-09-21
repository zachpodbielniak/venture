/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <string.h>
#include <gmime/gmime.h>
#include <glib/gstdio.h>
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GMimeStream, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GMimeParser, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GMimeMessage, g_object_unref)

#define VENTURE_MAIL_SYNC_DEFAULT_BUDGET 200
/* Under the CLI's HTTP timeout, so a sweep answers the command that asked. */
#define VENTURE_MAIL_SYNC_DEFAULT_SECONDS 45
#define VENTURE_MAIL_SYNC_TRUNCATED_TEXT ((gsize)65536)
#define VENTURE_MAIL_SYNC_LIST_PAGE 100
#define VENTURE_MAIL_SYNC_BACKOFF_BASE 60
#define VENTURE_MAIL_SYNC_BACKOFF_MAX 3600
#define VENTURE_MAIL_SYNC_NAME_BYTES 96
#define VENTURE_MAIL_SYNC_BACKFILL_LIMIT 500

struct _VentureMailSyncService {
	GObject parent_instance;
	VentureDatabase *database;
	VentureImapClient *client;
	VentureContext *context;
	VentureConfig *config;
	gchar *attachment_root;
	guint message_budget;
	guint time_budget;
	gboolean busy;
};
G_DEFINE_FINAL_TYPE(VentureMailSyncService, venture_mail_sync_service, G_TYPE_OBJECT)

static gboolean refuse(GError **error, VentureError code, const gchar *message)
{
	g_set_error(error, VENTURE_ERROR, code, "VentureMailSyncService: %s", message);
	return FALSE;
}
static gboolean type_on(const gchar *name)
{
	return venture_entity_registry_lookup(venture_entity_registry_get_default(), name) != G_TYPE_INVALID;
}
static gboolean module_on(void)
{
	return type_on("mail_inbound");
}
static void finalize(GObject *object)
{
	VentureMailSyncService *self = VENTURE_MAIL_SYNC_SERVICE(object);
	if (self->database) g_object_remove_weak_pointer(G_OBJECT(self->database), (gpointer *)&self->database);
	if (self->context) g_object_remove_weak_pointer(G_OBJECT(self->context), (gpointer *)&self->context);
	g_clear_object(&self->client);
	g_clear_object(&self->config);
	g_free(self->attachment_root);
	G_OBJECT_CLASS(venture_mail_sync_service_parent_class)->finalize(object);
}
static void set_property(GObject *object, guint id, const GValue *value, GParamSpec *pspec)
{
	VentureMailSyncService *self = VENTURE_MAIL_SYNC_SERVICE(object);
	switch (id) {
	case 1:
		self->database = g_value_get_object(value);
		if (self->database) g_object_add_weak_pointer(G_OBJECT(self->database), (gpointer *)&self->database);
		break;
	case 2: g_set_object(&self->client, g_value_get_object(value)); break;
	case 3: g_free(self->attachment_root); self->attachment_root = g_value_dup_string(value); break;
	case 4: self->message_budget = g_value_get_uint(value); break;
	case 5: self->time_budget = g_value_get_uint(value); break;
	case 6:
		if (self->context) g_object_remove_weak_pointer(G_OBJECT(self->context), (gpointer *)&self->context);
		self->context = g_value_get_object(value);
		if (self->context) g_object_add_weak_pointer(G_OBJECT(self->context), (gpointer *)&self->context);
		break;
	case 7: g_set_object(&self->config, g_value_get_object(value)); break;
	default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
	}
}
static void get_property(GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
	VentureMailSyncService *self = VENTURE_MAIL_SYNC_SERVICE(object);
	switch (id) {
	case 1: g_value_set_object(value, self->database); break;
	case 2: g_value_set_object(value, self->client); break;
	case 3: g_value_set_string(value, self->attachment_root); break;
	case 4: g_value_set_uint(value, self->message_budget); break;
	case 5: g_value_set_uint(value, self->time_budget); break;
	case 6: g_value_set_object(value, self->context); break;
	case 7: g_value_set_object(value, self->config); break;
	default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
	}
}
static void venture_mail_sync_service_class_init(VentureMailSyncServiceClass *klass)
{
	GObjectClass *object = G_OBJECT_CLASS(klass);
	object->finalize = finalize;
	object->set_property = set_property;
	object->get_property = get_property;
	g_object_class_install_property(object, 1, g_param_spec_object("database", "Database", "Weak owning database", VENTURE_TYPE_DATABASE, G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
	g_object_class_install_property(object, 2, g_param_spec_object("client", "Client", "IMAP transport", VENTURE_TYPE_IMAP_CLIENT, G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
	g_object_class_install_property(object, 3, g_param_spec_string("attachment-root", "Attachment root", "Directory that receives raw messages and attachments", NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
	g_object_class_install_property(object, 4, g_param_spec_uint("message-budget", "Message budget", "Messages one sync or sweep call reads; the cursor resumes on the next call", 1, 100000, VENTURE_MAIL_SYNC_DEFAULT_BUDGET, G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));
	g_object_class_install_property(object, 5, g_param_spec_uint("time-budget", "Time budget", "Seconds one sync or sweep call may hold the main loop", 1, 3600, VENTURE_MAIL_SYNC_DEFAULT_SECONDS, G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));
	g_object_class_install_property(object, 6, g_param_spec_object("context", "Context", "Weak; notifies admins when an account is switched off", VENTURE_TYPE_CONTEXT, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
	g_object_class_install_property(object, 7, g_param_spec_object("config", "Config", "Operator connector endpoint policy", VENTURE_TYPE_CONFIG, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
	g_mime_init();
}
static void venture_mail_sync_service_init(VentureMailSyncService *self) { self->config = venture_config_new(); }
VentureMailSyncService *venture_mail_sync_service_new(VentureDatabase *database, VentureImapClient *client)
{
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	g_return_val_if_fail(VENTURE_IS_IMAP_CLIENT(client), NULL);
	return g_object_new(VENTURE_TYPE_MAIL_SYNC_SERVICE, "database", database, "client", client, NULL);
}
VentureMailSyncService *venture_mail_sync_service_new_for_context(VentureContext *context)
{
	g_autoptr(VentureSocketImapClient) client = NULL;
	g_autofree gchar *root = NULL;
	VentureMailSyncService *service;
	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);
	/* A fresh client per service: the socket client holds a session only
	 * for the length of one call. */
	client = venture_socket_imap_client_new();
	root = g_build_filename(venture_config_get_state_dir(venture_context_get_config(context)), "attachments", NULL);
	service = venture_mail_sync_service_new(venture_context_get_database(context), VENTURE_IMAP_CLIENT(client));
	g_object_set(service, "attachment-root", root, "context", context, "config", venture_context_get_config(context), NULL);
	return service;
}

/* --- Addresses ------------------------------------------------------------ */
static gchar *address_lower(const gchar *value)
{
	g_autofree gchar *copy = g_strdup(value ? value : "");
	return g_ascii_strdown(g_strstrip(copy), -1);
}
/* A capture address is compared exactly, case aside. An untagged one also
 * takes its +tag variants (receipts+toner@ for receipts@), but a tagged one
 * never matches its untagged base: with +tag normalisation on both sides,
 * account ops@ and capture ops+receipts@ were the same address and the
 * whole inbox became capture items. */
static gboolean capture_matches(const gchar *address, const gchar *capture)
{
	g_autofree gchar *have = address_lower(address), *wanted = address_lower(capture), *normal = NULL;
	const gchar *at;
	if (!*wanted || !*have) return FALSE;
	if (!strcmp(have, wanted)) return TRUE;
	at = strchr(wanted, '@');
	if (!at || memchr(wanted, '+', (gsize)(at - wanted))) return FALSE;
	normal = venture_lead_normalize_email(have);
	return !strcmp(normal, wanted);
}
static GPtrArray *split_list(const gchar *text, const gchar *separators)
{
	g_auto(GStrv) parts = g_strsplit_set(text ? text : "", separators, -1);
	GPtrArray *items = g_ptr_array_new_with_free_func(g_free);
	guint i;
	for (i = 0; parts[i]; i++) {
		gchar *item = address_lower(parts[i]);
		if (*item) g_ptr_array_add(items, item);
		else g_free(item);
	}
	return items;
}

/* --- Parsed message ------------------------------------------------------ */
/* Per-message bounds. One message is filed in one transaction on the main
 * loop, and the sync's time budget is only consulted between messages, so
 * a single hostile message -- a million References ids, a hundred thousand
 * recipients, thousands of tiny attachments -- must not be able to hold the
 * loop for minutes. A deadline checked inside a message would not help: the
 * message would roll back and be retried, and overrun, on every sync. */
#define MAIL_SYNC_MAX_REFERENCES (20)
#define MAIL_SYNC_MAX_ADDRESSES (100)
#define MAIL_SYNC_MAX_ATTACHMENTS (25)
typedef struct { gchar *name; gchar *mime; GBytes *data; } Attachment;
typedef struct {
	gchar *from; gchar *from_name; GPtrArray *addresses; /* normalised From, To and Cc, once each */
	GHashTable *address_set; /* the same strings, borrowed, for the once-each check */
	gchar *message_id; gchar *in_reply_to; GPtrArray *references;
	gchar *subject;
	gchar *plain; gchar *html; /* the message's own body parts */
	gchar *forwarded_plain; gchar *forwarded_html; /* from an attached message/rfc822 */
	gchar *text; /* what the timeline shows: the plain part, else the HTML as text */
	GDateTime *date;
	GPtrArray *attachments; gboolean to_capture; gboolean bulk;
} Parsed;
static void attachment_free(gpointer data)
{
	Attachment *a = data;
	g_free(a->name); g_free(a->mime); g_clear_pointer(&a->data, g_bytes_unref); g_free(a);
}
static void parsed_clear(Parsed *p)
{
	g_free(p->from); g_free(p->from_name); g_clear_pointer(&p->address_set, g_hash_table_unref);
	g_clear_pointer(&p->addresses, g_ptr_array_unref);
	g_free(p->message_id); g_free(p->in_reply_to); g_clear_pointer(&p->references, g_ptr_array_unref);
	g_free(p->subject); g_free(p->plain); g_free(p->html); g_free(p->forwarded_plain); g_free(p->forwarded_html);
	g_free(p->text); g_clear_pointer(&p->date, g_date_time_unref);
	g_clear_pointer(&p->attachments, g_ptr_array_unref);
	memset(p, 0, sizeof *p);
}
/* Mail declares charsets it does not use. PostgreSQL refuses invalid UTF-8
 * outright and SQLite stores it as invalid JSON, so nothing reaches a save
 * unvalidated; undeclared 8-bit text is most often windows-1252. */
static gchar *valid_text(const gchar *text)
{
	gchar *converted;
	if (!text) return NULL;
	if (g_utf8_validate(text, -1, NULL)) return g_strdup(text);
	converted = g_convert(text, -1, "UTF-8", "WINDOWS-1252", NULL, NULL, NULL);
	return converted ? converted : g_utf8_make_valid(text, -1);
}
static gchar *strip_brackets(const gchar *id)
{
	g_autofree gchar *copy = g_strdup(id ? id : "");
	gchar *s = g_strstrip(copy);
	if (*s == '<') s++;
	if (*s && s[strlen(s) - 1] == '>') s[strlen(s) - 1] = '\0';
	return valid_text(s);
}
static void collect_address(Parsed *p, const gchar *addr, const gchar *capture)
{
	g_autofree gchar *normal = NULL;
	if (!addr) return;
	if (capture && capture_matches(addr, capture)) p->to_capture = TRUE;
	normal = venture_lead_normalize_email(addr);
	if (!*normal || !g_utf8_validate(normal, -1, NULL)) return;
	if (g_hash_table_contains(p->address_set, normal)) return;
	/* Nobody writes to a hundred people one at a time. Past the cap the
	 * message is treated as bulk: known parties already collected still
	 * get the timeline entry, and nobody else becomes an unmatched sender. */
	if (p->addresses->len >= MAIL_SYNC_MAX_ADDRESSES) { p->bulk = TRUE; return; }
	g_hash_table_add(p->address_set, normal);
	g_ptr_array_add(p->addresses, g_steal_pointer(&normal));
}
static void collect_addresses(Parsed *p, InternetAddressList *list, const gchar *capture)
{
	gint i;
	for (i = 0; list && i < internet_address_list_length(list); i++) {
		InternetAddress *address = internet_address_list_get_address(list, i);
		if (INTERNET_ADDRESS_IS_MAILBOX(address)) collect_address(p, internet_address_mailbox_get_addr(INTERNET_ADDRESS_MAILBOX(address)), capture);
		else if (INTERNET_ADDRESS_IS_GROUP(address)) collect_addresses(p, internet_address_group_get_members(INTERNET_ADDRESS_GROUP(address)), capture);
	}
}
/* List-Id, Precedence and Auto-Submitted mark mail no person wrote to you:
 * it may still land on a known contact's timeline, but it never asks for a
 * new contact. */
static gboolean message_is_bulk(GMimeObject *message)
{
	const gchar *precedence = g_mime_object_get_header(message, "Precedence");
	const gchar *automatic = g_mime_object_get_header(message, "Auto-Submitted");
	if (g_mime_object_get_header(message, "List-Id")) return TRUE;
	if (precedence) {
		g_autofree gchar *value = address_lower(precedence);
		if (!strcmp(value, "bulk") || !strcmp(value, "list") || !strcmp(value, "junk")) return TRUE;
	}
	if (automatic) {
		g_autofree gchar *value = address_lower(automatic);
		if (*value && g_ascii_strncasecmp(value, "no", 2)) return TRUE;
		if (strlen(value) > 2 && value[2] != ';' && value[2] != ' ') return TRUE;
	}
	return FALSE;
}
/* Parts that ride along with mail but are nobody's receipt: signatures,
 * calendar invitations and Outlook's TNEF wrapper. */
static gboolean part_is_noise(const gchar *mime, const gchar *filename)
{
	static const gchar *const types[] = {
		"application/pkcs7-signature", "application/x-pkcs7-signature", "application/pgp-signature",
		"text/calendar", "application/ics", "application/ms-tnef", "application/vnd.ms-tnef", NULL
	};
	g_autofree gchar *name = filename ? g_ascii_strdown(filename, -1) : NULL;
	if (mime && g_strv_contains(types, mime)) return TRUE;
	return name && (!strcmp(name, "smime.p7s") || !strcmp(name, "winmail.dat") || g_str_has_suffix(name, ".ics"));
}
static void take_part(Parsed *p, GMimePart *part, gboolean forwarded)
{
	GMimeObject *object = GMIME_OBJECT(part);
	GMimeContentType *type = g_mime_object_get_content_type(object);
	g_autofree gchar *mime = NULL;
	const gchar *filename = g_mime_part_get_filename(part);
	const gchar *disposition = g_mime_object_get_disposition(object);
	gboolean attachment = disposition && !g_ascii_strcasecmp(disposition, "attachment");
	gchar **slot = NULL;
	if (type) {
		gchar *declared = g_mime_content_type_get_mime_type(type);
		mime = g_ascii_strdown(declared, -1);
		g_free(declared);
	} else mime = g_strdup("application/octet-stream");
	if (part_is_noise(mime, filename)) return;
	/* An inline image the HTML refers to by Content-ID is a logo or a
	 * signature picture, not something anybody sent as a document. */
	if (!attachment && g_mime_object_get_content_id(object)) return;
	if (!attachment && !filename && GMIME_IS_TEXT_PART(part)) {
		if (!strcmp(mime, "text/plain")) slot = forwarded ? &p->forwarded_plain : &p->plain;
		else if (!strcmp(mime, "text/html")) slot = forwarded ? &p->forwarded_html : &p->html;
		if (slot && !*slot) {
			g_autofree gchar *text = g_mime_text_part_get_text(GMIME_TEXT_PART(part));
			*slot = valid_text(text ? text : "");
		}
		if (slot) return;
	}
	if ((filename || attachment) && p->attachments->len >= MAIL_SYNC_MAX_ATTACHMENTS) return;
	if (filename || attachment) {
		g_autoptr(GMimeStream) stream = g_mime_stream_mem_new();
		GMimeDataWrapper *content = g_mime_part_get_content(part);
		Attachment *a = g_new0(Attachment, 1);
		GByteArray *bytes;
		if (content) g_mime_data_wrapper_write_to_stream(content, stream);
		bytes = g_mime_stream_mem_get_byte_array(GMIME_STREAM_MEM(stream));
		a->name = valid_text(filename ? filename : "attachment");
		a->mime = g_steal_pointer(&mime);
		a->data = g_bytes_new(bytes->data, bytes->len);
		g_ptr_array_add(p->attachments, a);
	}
}
/* g_mime_message_foreach() walks multiparts but stops at message/rfc822,
 * which is exactly where a forwarded receipt lives. */
static void walk_mime(Parsed *p, GMimeObject *object, gboolean forwarded, guint level)
{
	if (!object || level > 16) return;
	if (GMIME_IS_MULTIPART(object)) {
		gint i;
		for (i = 0; i < g_mime_multipart_get_count(GMIME_MULTIPART(object)); i++)
			walk_mime(p, g_mime_multipart_get_part(GMIME_MULTIPART(object), i), forwarded, level + 1);
	} else if (GMIME_IS_MESSAGE_PART(object)) {
		GMimeMessage *inner = g_mime_message_part_get_message(GMIME_MESSAGE_PART(object));
		if (inner) walk_mime(p, g_mime_message_get_mime_part(inner), TRUE, level + 1);
	} else if (GMIME_IS_PART(object)) take_part(p, GMIME_PART(object), forwarded);
}
static gboolean parse_message(GBytes *raw, const gchar *capture_address, Parsed *p, GError **error)
{
	static const gchar *fallback[] = { "utf-8", "windows-1252", NULL };
	g_autoptr(GMimeStream) stream = NULL;
	g_autoptr(GMimeParser) parser = NULL;
	g_autoptr(GMimeMessage) message = NULL;
	GMimeParserOptions *options;
	InternetAddressList *from;
	const gchar *header;
	gsize length;
	gconstpointer data = g_bytes_get_data(raw, &length);
	memset(p, 0, sizeof *p);
	p->addresses = g_ptr_array_new_with_free_func(g_free);
	p->address_set = g_hash_table_new(g_str_hash, g_str_equal);
	p->references = g_ptr_array_new_with_free_func(g_free);
	p->attachments = g_ptr_array_new_with_free_func(attachment_free);
	stream = g_mime_stream_mem_new_with_buffer(data, length);
	parser = g_mime_parser_new_with_stream(stream);
	options = g_mime_parser_options_new();
	g_mime_parser_options_set_fallback_charsets(options, fallback);
	message = g_mime_parser_construct_message(parser, options);
	if (!message) { g_mime_parser_options_free(options); return refuse(error, VENTURE_ERROR_VALIDATION, "Message could not be parsed"); }
	from = g_mime_message_get_from(message);
	if (from && internet_address_list_length(from) > 0) {
		InternetAddress *first = internet_address_list_get_address(from, 0);
		if (INTERNET_ADDRESS_IS_MAILBOX(first)) {
			gchar *normal = venture_lead_normalize_email(internet_address_mailbox_get_addr(INTERNET_ADDRESS_MAILBOX(first)));
			p->from = valid_text(normal);
			g_free(normal);
		}
		p->from_name = valid_text(internet_address_get_name(first));
	}
	collect_addresses(p, from, capture_address);
	collect_addresses(p, g_mime_message_get_to(message), capture_address);
	collect_addresses(p, g_mime_message_get_cc(message), capture_address);
	/* A capture address in Bcc shows only in the delivery headers. */
	if (capture_address && *capture_address) {
		const gchar *const delivery[] = { "Delivered-To", "X-Original-To" };
		guint i;
		for (i = 0; i < G_N_ELEMENTS(delivery); i++) {
			header = g_mime_object_get_header(GMIME_OBJECT(message), delivery[i]);
			if (header && capture_matches(header, capture_address)) p->to_capture = TRUE;
		}
	}
	p->message_id = strip_brackets(g_mime_message_get_message_id(message));
	if (!*p->message_id) {
		/* Without one, every such message shared the thread "" and the
		 * Message-ID lookup that deduplicates copies matched them all. A
		 * hash of the bytes is stable across folders and reruns. */
		g_autofree gchar *checksum = g_compute_checksum_for_bytes(G_CHECKSUM_SHA256, raw);
		g_free(p->message_id);
		p->message_id = g_strdup_printf("%.32s@venture.invalid", checksum);
	}
	header = g_mime_object_get_header(GMIME_OBJECT(message), "In-Reply-To");
	if (header) p->in_reply_to = strip_brackets(header);
	header = g_mime_object_get_header(GMIME_OBJECT(message), "References");
	if (header) {
		g_auto(GStrv) parts = g_strsplit_set(header, " \t\r\n,", -1);
		g_autoptr(GPtrArray) all = g_ptr_array_new();
		guint i;
		for (i = 0; parts[i]; i++) if (*parts[i]) g_ptr_array_add(all, parts[i]);
		/* Each reference costs a lookup. Keep the root, which names the
		 * thread when nothing else is known, and the most recent ancestors,
		 * the way RFC 5322 suggests a long References header be trimmed. */
		for (i = 0; i < all->len; i++)
			if (i == 0 || all->len - i < MAIL_SYNC_MAX_REFERENCES)
				g_ptr_array_add(p->references, strip_brackets(g_ptr_array_index(all, i)));
	}
	p->subject = valid_text(g_mime_message_get_subject(message));
	{
		GDateTime *date = g_mime_message_get_date(message);
		/* GMime 3.2 returns a date owned by the message; do not unref it. */
		p->date = date ? g_date_time_to_utc(date) : g_date_time_new_now_utc();
	}
	/* Kept if the recipient cap already marked it bulk. */
	p->bulk = p->bulk || message_is_bulk(GMIME_OBJECT(message));
	walk_mime(p, g_mime_message_get_mime_part(message), FALSE, 0);
	/* HTML-only mail is common; its text is what the timeline shows. */
	if (p->plain && *p->plain) p->text = g_strdup(p->plain);
	else if (p->html) p->text = venture_document_html_to_text(p->html, -1);
	else if (p->forwarded_plain) p->text = g_strdup(p->forwarded_plain);
	else if (p->forwarded_html) p->text = venture_document_html_to_text(p->forwarded_html, -1);
	g_mime_parser_options_free(options);
	return TRUE;
}

/* --- One call's budget and one account's run ------------------------------- */
typedef struct {
	guint messages_left;
	gint64 deadline;
	gboolean deferred;
} SyncBudget;
typedef struct { guint32 uid; guint32 uidvalidity; guint32 failed_uid; guint attempts; gboolean legacy; } FolderCursor;
typedef struct {
	VentureMailSyncService *self;
	VentureDatabase *db;
	VentureConnectorSession *connector;
	VentureEntity *account; /* re-read under the lease; the caller's copy is updated at the end */
	const VentureActor *actor;
	SyncBudget *budget;
	JsonObject *cursors;
	gint64 org;
	gint64 account_id;
	gint64 private_owner;
	gchar *host; gchar *security; gchar *username; const gchar *secret; gint64 port;
	gchar *own_normal;
	gchar *capture_address;
	gchar *capture_folder;
	GPtrArray *folders;
	GPtrArray *ignore; /* GPatternSpec */
	GPtrArray *internal; /* lower-case domains */
	GDateTime *since;
	GHashTable *contacts; /* normalised email -> GPtrArray of contacts, built once per run */
	GHashTable *leads;
	GPtrArray *written; /* files written in the open transaction */
	GString *problems; /* folder and message failures that did not stop the account */
	gint filed;
} SyncRun;
static void sync_run_clear(SyncRun *run)
{
	g_clear_object(&run->account);
	g_clear_object(&run->connector);
	g_clear_pointer(&run->cursors, json_object_unref);
	g_free(run->host); g_free(run->security); g_free(run->username);
	g_free(run->own_normal); g_free(run->capture_address); g_free(run->capture_folder);
	g_clear_pointer(&run->folders, g_ptr_array_unref);
	g_clear_pointer(&run->ignore, g_ptr_array_unref);
	g_clear_pointer(&run->internal, g_ptr_array_unref);
	g_clear_pointer(&run->since, g_date_time_unref);
	g_clear_pointer(&run->contacts, g_hash_table_unref);
	g_clear_pointer(&run->leads, g_hash_table_unref);
	g_clear_pointer(&run->written, g_ptr_array_unref);
	if (run->problems) g_string_free(run->problems, TRUE);
	memset(run, 0, sizeof *run);
}
static gboolean budget_spent(SyncBudget *budget)
{
	if (budget->messages_left && g_get_monotonic_time() < budget->deadline) return FALSE;
	budget->deferred = TRUE;
	return TRUE;
}
static void note_problem(SyncRun *run, const gchar *format, ...) G_GNUC_PRINTF(2, 3);
static void note_problem(SyncRun *run, const gchar *format, ...)
{
	va_list args;
	if (run->problems->len) g_string_append(run->problems, "; ");
	va_start(args, format);
	g_string_append_vprintf(run->problems, format, args);
	va_end(args);
}
static gboolean address_ignored(SyncRun *run, const gchar *normal)
{
	guint i;
	for (i = 0; i < run->ignore->len; i++) if (g_pattern_spec_match_string(g_ptr_array_index(run->ignore, i), normal)) return TRUE;
	return FALSE;
}
static gboolean address_internal(SyncRun *run, const gchar *normal)
{
	const gchar *at = strrchr(normal, '@');
	guint i;
	if (!at) return FALSE;
	for (i = 0; i < run->internal->len; i++) {
		const gchar *domain = g_ptr_array_index(run->internal, i);
		gsize length = strlen(at + 1), wanted = strlen(domain);
		if (*domain == '@') { domain++; wanted--; }
		if (!strcmp(at + 1, domain)) return TRUE;
		if (length > wanted && at[length - wanted] == '.' && !strcmp(at + 1 + length - wanted, domain)) return TRUE;
	}
	return FALSE;
}

/* --- Cursors: {"INBOX": {"uid", "uidvalidity", "failed_uid", "attempts"}} -- */
static void cursor_get(JsonObject *cursors, const gchar *folder, FolderCursor *cursor)
{
	JsonNode *node = json_object_has_member(cursors, folder) ? json_object_get_member(cursors, folder) : NULL;
	JsonObject *object;
	memset(cursor, 0, sizeof *cursor);
	if (!node) return;
	if (JSON_NODE_HOLDS_VALUE(node)) {
		/* Accounts synced before UIDVALIDITY was kept stored the bare UID,
		 * and their rows carry keys without it. */
		cursor->uid = (guint32)CLAMP(json_node_get_int(node), 0, (gint64)G_MAXUINT32);
		cursor->legacy = cursor->uid > 0;
		return;
	}
	if (!JSON_NODE_HOLDS_OBJECT(node)) return;
	object = json_node_get_object(node);
	cursor->uid = (guint32)CLAMP(venture_json_object_get_int(object, "uid", 0), 0, (gint64)G_MAXUINT32);
	cursor->uidvalidity = (guint32)CLAMP(venture_json_object_get_int(object, "uidvalidity", 0), 0, (gint64)G_MAXUINT32);
	cursor->failed_uid = (guint32)CLAMP(venture_json_object_get_int(object, "failed_uid", 0), 0, (gint64)G_MAXUINT32);
	cursor->attempts = (guint)CLAMP(venture_json_object_get_int(object, "attempts", 0), 0, 1000);
	cursor->legacy = venture_json_object_get_bool(object, "legacy", FALSE);
}
static void cursor_set(JsonObject *cursors, const gchar *folder, const FolderCursor *cursor)
{
	JsonObject *object = json_object_new();
	json_object_set_int_member(object, "uid", cursor->uid);
	json_object_set_int_member(object, "uidvalidity", cursor->uidvalidity);
	if (cursor->legacy) json_object_set_boolean_member(object, "legacy", TRUE);
	if (cursor->failed_uid) {
		json_object_set_int_member(object, "failed_uid", cursor->failed_uid);
		json_object_set_int_member(object, "attempts", cursor->attempts);
	}
	json_object_set_object_member(cursors, folder, object);
}
static gboolean store_cursors(SyncRun *run, GError **error)
{
	g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);
	g_autofree gchar *text = NULL;
	json_node_set_object(node, run->cursors);
	text = json_to_string(node, FALSE);
	g_object_set(run->account, "cursors", text, NULL);
	return venture_database_save(run->db, run->account, run->actor, error);
}
/* A save bumps the object's version before its UPDATE, so after a rolled
 * back transaction that saved the account the in-memory row is a version
 * ahead of the database and every later save would conflict. */
static void refresh_account(SyncRun *run)
{
	g_autoptr(VentureEntity) fresh = venture_database_get(run->db, VENTURE_TYPE_MAIL_ACCOUNT, run->account_id, NULL);
	if (fresh) g_set_object(&run->account, fresh);
}
static gchar *uid_key(gint64 account, const gchar *folder, guint32 uidvalidity, guint32 uid)
{
	/* The folder goes last so a name containing ":" cannot make two keys
	 * equal, and the "v2:" prefix keeps these apart from the older
	 * account:folder:uid keys, which start with a digit. */
	return g_strdup_printf("v2:%" G_GINT64_FORMAT ":%u:%u:%s", account, uidvalidity, uid, folder);
}
static gint uid_filed(SyncRun *run, const gchar *folder, const FolderCursor *cursor, guint32 uidvalidity, guint32 uid, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_MAIL_INBOUND);
	g_autoptr(GPtrArray) keys = g_ptr_array_new_with_free_func(g_free);
	gint64 count;
	g_ptr_array_add(keys, uid_key(run->account_id, folder, uidvalidity, uid));
	if (cursor->legacy) {
		g_ptr_array_add(keys, g_strdup_printf("%" G_GINT64_FORMAT ":%s:%u", run->account_id, folder, uid));
		if (uidvalidity) g_ptr_array_add(keys, uid_key(run->account_id, folder, 0, uid));
	}
	venture_query_set_organization(query, run->org);
	/* The unique index covers deleted rows too; so must the check. */
	venture_query_set_include_deleted(query, TRUE);
	venture_query_add_filter(query, "uid-key", VENTURE_FILTER_OP_IN, keys, NULL);
	count = venture_database_count(run->db, query, error);
	return count < 0 ? -1 : count > 0;
}

/* --- Filing bytes as documents, the way the upload route does ------------ */
static gchar *stored_name(const gchar *title)
{
	g_autofree gchar *safe = g_strcanon(g_strdup(title && *title ? title : "message"), "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-", '_');
	g_autofree gchar *token = venture_generate_token(8);
	const gchar *dot = strrchr(safe, '.');
	gsize extension = dot && dot != safe && strlen(dot) <= 10 ? strlen(dot) : 0;
	gsize base = strlen(safe) - extension;
	/* The subject is unbounded and NAME_MAX is 255 bytes; the document row
	 * keeps the title whole, so the file name only has to be unique. The
	 * canonicalised name is ASCII, so any byte is a character boundary. */
	if (base > VENTURE_MAIL_SYNC_NAME_BYTES) base = VENTURE_MAIL_SYNC_NAME_BYTES;
	return g_strdup_printf("%s_%.*s%s", token, (gint)base, safe, extension ? dot : "");
}
static VentureEntity *file_document(SyncRun *run, const gchar *title, const gchar *kind, const gchar *mime, GBytes *data, const gchar *extracted, GError **error)
{
	VentureMailSyncService *self = run->self;
	g_autoptr(VentureEntity) document = NULL;
	g_autofree gchar *stored = NULL, *path = NULL, *checksum = NULL;
	gsize length;
	gconstpointer bytes = g_bytes_get_data(data, &length);
	if (!self->attachment_root) { refuse(error, VENTURE_ERROR_CONFIG, "Attachment storage is not configured"); return NULL; }
	if (g_mkdir_with_parents(self->attachment_root, 0700)) { refuse(error, VENTURE_ERROR_CONFIG, "Cannot create attachment storage"); return NULL; }
	stored = stored_name(title);
	path = g_build_filename(self->attachment_root, stored, NULL);
	if (!g_file_set_contents(path, bytes ? bytes : "", (gssize)length, error)) return NULL;
	/* Removed again if the transaction does not commit. */
	g_ptr_array_add(run->written, g_strdup(path));
	checksum = g_compute_checksum_for_bytes(G_CHECKSUM_SHA256, data);
	document = g_object_new(VENTURE_TYPE_DOCUMENT, "organization-id", run->org, "title", title, "kind", kind, "path", path,
		"private-owner-id", run->private_owner, "mime-type", mime, "size-bytes", (gint64)length, "hash", checksum, "extracted-text", extracted, NULL);
	if (!venture_document_service_save_attachment(venture_document_service_get(run->db), document, self->attachment_root, run->actor, error)) return NULL;
	return g_steal_pointer(&document);
}
static void discard_written(SyncRun *run)
{
	guint i;
	for (i = 0; i < run->written->len; i++) g_unlink(g_ptr_array_index(run->written, i));
	g_ptr_array_set_size(run->written, 0);
}
static gboolean hash_already_captured(SyncRun *run, GBytes *data, GError **error)
{
	g_autofree gchar *checksum = g_compute_checksum_for_bytes(G_CHECKSUM_SHA256, data);
	g_autoptr(VentureQuery) documents = venture_query_new(VENTURE_TYPE_DOCUMENT);
	g_autoptr(GPtrArray) rows = NULL;
	guint i;
	venture_query_set_organization(documents, run->org);
	venture_query_add_filter_string(documents, "hash", VENTURE_FILTER_OP_EQ, checksum, NULL);
	rows = venture_database_find(run->db, documents, error);
	if (!rows) return FALSE;
	for (i = 0; i < rows->len; i++) {
		g_autoptr(VentureQuery) items = venture_query_new(VENTURE_TYPE_CAPTURE_ITEM);
		venture_query_set_organization(items, run->org);
		venture_query_add_filter_int(items, "document-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(g_ptr_array_index(rows, i)), NULL);
		if (venture_database_count(run->db, items, NULL) > 0) return TRUE;
	}
	return FALSE;
}

/* --- Matching ------------------------------------------------------------ */
/* One query per run instead of one per address per message: every contact
 * in the organization, keyed by the normalised address. */
static GHashTable *email_index(VentureDatabase *db, GType type, gint64 org, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(type);
	g_autoptr(GPtrArray) rows = NULL;
	GHashTable *index;
	guint i;
	venture_query_set_organization(query, org);
	venture_query_set_limit(query, 0);
	rows = venture_database_find(db, query, error);
	if (!rows) return NULL;
	index = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_ptr_array_unref);
	for (i = 0; i < rows->len; i++) {
		VentureEntity *row = g_ptr_array_index(rows, i);
		g_autofree gchar *email = NULL, *normal = NULL;
		GPtrArray *bucket;
		g_object_get(row, "email", &email, NULL);
		normal = venture_lead_normalize_email(email);
		if (!*normal) continue;
		bucket = g_hash_table_lookup(index, normal);
		if (!bucket) {
			bucket = g_ptr_array_new_with_free_func(g_object_unref);
			g_hash_table_insert(index, g_strdup(normal), bucket);
		}
		g_ptr_array_add(bucket, g_object_ref(row));
	}
	return index;
}
/* The outbox path records one message, so it asks only for rows whose
 * address could match: the local part and domain as a LIKE superset, then
 * the same normalised comparison. */
static GPtrArray *find_by_email(VentureDatabase *db, GType type, gint64 org, const gchar *normal, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(type);
	g_autoptr(GPtrArray) rows = NULL;
	g_autofree gchar *pattern = NULL;
	const gchar *at = strrchr(normal, '@');
	GPtrArray *matched = g_ptr_array_new_with_free_func(g_object_unref);
	guint i;
	if (!at || at == normal) return matched;
	pattern = g_strdup_printf("%%%.*s%%@%s%%", (gint)(at - normal), normal, at + 1);
	venture_query_set_organization(query, org);
	venture_query_add_filter_string(query, "email", VENTURE_FILTER_OP_ILIKE, pattern, NULL);
	rows = venture_database_find(db, query, error);
	if (!rows) { g_ptr_array_unref(matched); return NULL; }
	for (i = 0; i < rows->len; i++) {
		g_autofree gchar *email = NULL, *candidate = NULL;
		g_object_get(g_ptr_array_index(rows, i), "email", &email, NULL);
		candidate = venture_lead_normalize_email(email);
		if (!strcmp(candidate, normal)) g_ptr_array_add(matched, g_object_ref(g_ptr_array_index(rows, i)));
	}
	return matched;
}
static gint64 live_reference(VentureDatabase *db, GType type, gint64 id)
{
	g_autoptr(VentureEntity) row = id > 0 ? venture_database_get(db, type, id, NULL) : NULL;
	return row && !venture_entity_is_deleted(row) ? id : 0;
}
/* A deal an earlier message in the thread was already filed against. */
static gint64 thread_deal(VentureDatabase *db, gint64 org, const gchar *thread, GError **error)
{
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	guint i;
	if (!thread || !*thread || !type_on("deal")) return 0;
	query = venture_query_new(VENTURE_TYPE_MAIL_INBOUND);
	venture_query_set_organization(query, org);
	venture_query_add_filter_string(query, "thread-id", VENTURE_FILTER_OP_EQ, thread, NULL);
	venture_query_add_filter_int(query, "interaction-id", VENTURE_FILTER_OP_GT, 0, NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	rows = venture_database_find(db, query, error);
	if (!rows) return -1;
	for (i = 0; i < rows->len; i++) {
		gint64 interaction_id = 0, deal = 0;
		g_autoptr(VentureEntity) interaction = NULL;
		g_object_get(g_ptr_array_index(rows, i), "interaction-id", &interaction_id, NULL);
		interaction = venture_database_get(db, VENTURE_TYPE_INTERACTION, interaction_id, NULL);
		if (interaction) g_object_get(interaction, "deal-id", &deal, NULL);
		if (live_reference(db, VENTURE_TYPE_DEAL, deal)) return deal;
	}
	return 0;
}
/* The contact's one open deal; with two or more nothing says which. */
static gint64 open_deal(VentureDatabase *db, gint64 org, gint64 contact, GError **error)
{
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	gint64 found = 0;
	guint i, open = 0;
	if (!contact || !type_on("deal")) return 0;
	query = venture_query_new(VENTURE_TYPE_DEAL);
	venture_query_set_organization(query, org);
	venture_query_add_filter_int(query, "contact-id", VENTURE_FILTER_OP_EQ, contact, NULL);
	rows = venture_database_find(db, query, error);
	if (!rows) return -1;
	for (i = 0; i < rows->len; i++) {
		VentureDealStage stage = VENTURE_DEAL_STAGE_LEAD;
		g_object_get(g_ptr_array_index(rows, i), "stage", &stage, NULL);
		if (stage == VENTURE_DEAL_STAGE_WON || stage == VENTURE_DEAL_STAGE_LOST) continue;
		open++;
		found = venture_entity_get_id(g_ptr_array_index(rows, i));
	}
	return open == 1 ? found : 0;
}
static gchar *resolve_thread(VentureDatabase *db, gint64 org, gint64 private_owner, Parsed *p, GError **error)
{
	g_autoptr(GPtrArray) candidates = g_ptr_array_new();
	guint i;
	if (p->in_reply_to && *p->in_reply_to) g_ptr_array_add(candidates, p->in_reply_to);
	for (i = 0; i < p->references->len; i++) g_ptr_array_add(candidates, g_ptr_array_index(p->references, i));
	for (i = 0; i < candidates->len; i++) {
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_MAIL_INBOUND);
		g_autoptr(GPtrArray) earlier = NULL;
		guint j;
		venture_query_set_organization(query, org);
		venture_query_add_filter_string(query, "message-id", VENTURE_FILTER_OP_EQ, g_ptr_array_index(candidates, i), NULL);
		earlier = venture_database_find(db, query, error);
		if (!earlier) return NULL;
		for (j = 0; j < earlier->len; j++) {
			VentureEntity *row = g_ptr_array_index(earlier, j);
			gchar *thread = NULL;
			if (venture_access_policy_get_personal_owner(venture_database_get_access_policy(db), row) != private_owner) continue;
			g_object_get(row, "thread-id", &thread, NULL);
			if (thread && *thread) return thread;
			g_free(thread);
		}
	}
	/* No known ancestor: the oldest reference is the root, else this message. */
	if (p->references->len) return g_strdup(g_ptr_array_index(p->references, 0));
	if (p->in_reply_to && *p->in_reply_to) return g_strdup(p->in_reply_to);
	return g_strdup(p->message_id);
}
/* The earliest row already filed with this Message-ID, from any folder or
 * account: a Gmail label, a watched Sent folder, the outbox. A stub filed
 * after repeated failures carries no content and is not a match. */
static VentureEntity *find_existing(VentureDatabase *db, gint64 org, gint64 private_owner, const gchar *message_id, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_MAIL_INBOUND);
	g_autoptr(GPtrArray) rows = NULL;
	guint i;
	venture_query_set_organization(query, org);
	venture_query_add_filter_string(query, "message-id", VENTURE_FILTER_OP_EQ, message_id, NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	rows = venture_database_find(db, query, error);
	if (!rows) return NULL;
	for (i = 0; i < rows->len; i++) {
		VentureEntity *row = g_ptr_array_index(rows, i);
		g_autofree gchar *reason = NULL;
		gint64 document = 0;
		if (venture_access_policy_get_personal_owner(venture_database_get_access_policy(db), row) != private_owner) continue;
		g_object_get(row, "skip-reason", &reason, "document-id", &document, NULL);
		if (!venture_string_is_empty(reason) && !document) continue;
		return g_object_ref(row);
	}
	return NULL;
}
static VentureEntity *record_interaction(VentureDatabase *db, gint64 org, VentureEntity *contact, VentureEntity *lead, const gchar *subject, const gchar *body, GDateTime *when, gboolean outbound, gint64 deal, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) interaction = g_object_new(VENTURE_TYPE_INTERACTION, "organization-id", org, NULL);
	if (contact) {
		gint64 company = 0;
		g_object_get(contact, "company-id", &company, NULL);
		/* Copying a deleted company would fail the reference check on the
		 * save and stall the mailbox (or, in the outbox, the batch). */
		g_object_set(interaction, "contact-id", venture_entity_get_id(contact), "company-id", live_reference(db, VENTURE_TYPE_COMPANY, company), NULL);
	}
	if (lead) g_object_set(interaction, "lead-id", venture_entity_get_id(lead), NULL);
	if (deal > 0) g_object_set(interaction, "deal-id", deal, NULL);
	g_object_set(interaction, "kind", VENTURE_INTERACTION_KIND_EMAIL, "subject", subject && *subject ? subject : "(no subject)",
		"body", body, "occurred-at", when, "outbound", outbound, NULL);
	if (!venture_database_save(db, interaction, actor, error)) return NULL;
	return g_steal_pointer(&interaction);
}
/* Looked up including deleted rows, because the unique index includes
 * them: a sender someone deleted used to come back as an insert that
 * violated the index and stalled the account on that message forever. */
static gboolean note_unmatched(SyncRun *run, const gchar *address, const gchar *name, const gchar *subject, GDateTime *when, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_MAIL_UNMATCHED_SENDER);
	g_autoptr(VentureEntity) row = NULL;
	g_autoptr(GDateTime) last = NULL;
	gboolean dismissed = FALSE;
	gint64 seen = 0;
	venture_query_set_organization(query, run->org);
	venture_query_set_include_deleted(query, TRUE);
	venture_query_add_filter_string(query, "address", VENTURE_FILTER_OP_EQ, address, NULL);
	row = venture_database_find_one(run->db, query, error);
	if (error && *error) return FALSE;
	if (!row) row = g_object_new(VENTURE_TYPE_MAIL_UNMATCHED_SENDER, "organization-id", run->org, "address", address, NULL);
	else g_object_get(row, "seen", &seen, "dismissed", &dismissed, "last-seen-at", &last, NULL);
	if (venture_entity_is_deleted(row)) {
		/* Deleted and dismissed is still an ignore-list entry. */
		if (dismissed) return TRUE;
		if (!venture_database_restore(run->db, row, run->actor, error)) return FALSE;
	}
	g_object_set(row, "seen", seen + 1, "last-subject", subject, NULL);
	if (!last || g_date_time_compare(when, last) > 0) g_object_set(row, "last-seen-at", when, NULL);
	if (name && *name) g_object_set(row, "name", name, NULL);
	return venture_database_save(run->db, row, run->actor, error);
}

/* --- One message, one transaction ---------------------------------------- */
static gchar *timeline_body(const gchar *from, const gchar *message_id, const gchar *thread, const gchar *text, const gchar *skip_reason)
{
	return g_strdup_printf("From: %s\nMessage-ID: %s\nThread: %s\n\n%s%s%s", from ? from : "", message_id ? message_id : "", thread ? thread : "",
		text ? text : "", skip_reason ? "\n\n" : "", skip_reason ? skip_reason : "");
}
static gboolean file_participants(SyncRun *run, Parsed *p, const gchar *thread, const gchar *body, gboolean outbound, VentureEntity *inbound, GError **error)
{
	g_autoptr(GHashTable) recorded = g_hash_table_new(g_direct_hash, g_direct_equal);
	gint64 deal_in_thread = -1;
	guint i, j;
	if (!run->contacts && !(run->contacts = email_index(run->db, VENTURE_TYPE_CONTACT, run->org, error))) return FALSE;
	if (type_on("lead") && !run->leads && !(run->leads = email_index(run->db, VENTURE_TYPE_LEAD, run->org, error))) return FALSE;
	for (i = 0; i < p->addresses->len; i++) {
		const gchar *address = g_ptr_array_index(p->addresses, i);
		GPtrArray *contacts, *leads;
		/* The account's own address is never a contact match nor an unmatched sender. */
		if (run->own_normal && !g_strcmp0(run->own_normal, address)) continue;
		if (address_ignored(run, address)) continue;
		contacts = g_hash_table_lookup(run->contacts, address);
		leads = run->leads && !contacts ? g_hash_table_lookup(run->leads, address) : NULL;
		for (j = 0; contacts && j < contacts->len; j++) {
			VentureEntity *contact = g_ptr_array_index(contacts, j);
			g_autoptr(VentureEntity) interaction = NULL;
			gint64 deal;
			if (!g_hash_table_add(recorded, contact)) continue;
			if (deal_in_thread < 0 && (deal_in_thread = thread_deal(run->db, run->org, thread, error)) < 0) return FALSE;
			deal = deal_in_thread ? deal_in_thread : open_deal(run->db, run->org, venture_entity_get_id(contact), error);
			if (deal < 0) return FALSE;
			interaction = record_interaction(run->db, run->org, contact, NULL, p->subject, body, p->date, outbound, deal, run->actor, error);
			if (!interaction) return FALSE;
			{
				gint64 current = 0;
				g_object_get(inbound, "interaction-id", &current, NULL);
				if (!current) g_object_set(inbound, "contact-id", venture_entity_get_id(contact), "interaction-id", venture_entity_get_id(interaction), NULL);
			}
		}
		/* A lead is a known party too: the inquiry's history belongs on it
		 * rather than on a list of strangers. */
		for (j = 0; leads && j < leads->len; j++) {
			VentureEntity *lead = g_ptr_array_index(leads, j);
			g_autoptr(VentureEntity) interaction = NULL;
			gint64 current = 0;
			if (!g_hash_table_add(recorded, lead)) continue;
			interaction = record_interaction(run->db, run->org, NULL, lead, p->subject, body, p->date, outbound, 0, run->actor, error);
			if (!interaction) return FALSE;
			g_object_get(inbound, "interaction-id", &current, NULL);
			if (!current) g_object_set(inbound, "interaction-id", venture_entity_get_id(interaction), NULL);
		}
		if (contacts || leads || p->bulk || address_internal(run, address)) continue;
		if (!note_unmatched(run, address, !g_strcmp0(address, p->from) ? p->from_name : NULL, p->subject, p->date, error)) return FALSE;
	}
	return TRUE;
}
static gboolean file_capture(SyncRun *run, Parsed *p, VentureEntity *inbound, GError **error)
{
	VentureCaptureService *capture_service = venture_capture_service_get(run->db);
	const gchar *vendor = p->from_name && *p->from_name ? p->from_name : p->from;
	const gchar *content = NULL, *mime = NULL;
	gboolean any = FALSE;
	guint i;
	/* Each attachment is a receipt. Same bytes as an upload: skip. */
	for (i = 0; i < p->attachments->len; i++) {
		Attachment *a = g_ptr_array_index(p->attachments, i);
		g_autoptr(VentureEntity) document = NULL, item = NULL;
		g_autoptr(GError) dup_error = NULL;
		g_autofree gchar *extracted = NULL;
		gboolean duplicate = hash_already_captured(run, a->data, &dup_error);
		if (dup_error) { g_propagate_error(error, g_steal_pointer(&dup_error)); return FALSE; }
		if (duplicate) continue;
		extracted = venture_document_extract_text(a->mime, a->name, a->data);
		document = file_document(run, a->name, "receipt", a->mime, a->data, extracted, error);
		if (!document) return FALSE;
		item = venture_capture_service_ingest_for_organization(capture_service, run->org, "receipt", p->subject && *p->subject ? p->subject : a->name, "email",
			venture_entity_get_id(document), vendor, NULL, p->date, p->text, run->actor, error);
		if (!item) return FALSE;
		if (!any) g_object_set(inbound, "capture-item-id", venture_entity_get_id(item), NULL);
		any = TRUE;
	}
	if (p->attachments->len) return TRUE;
	/* No attachment: the message is the receipt. A forwarded message is
	 * what was forwarded, and an HTML-only receipt is filed as its HTML. */
	if (p->forwarded_html && *p->forwarded_html) { content = p->forwarded_html; mime = "text/html"; }
	else if (p->forwarded_plain && *p->forwarded_plain) { content = p->forwarded_plain; mime = "text/plain"; }
	else if (p->plain && *p->plain) { content = p->plain; mime = "text/plain"; }
	else if (p->html && *p->html) { content = p->html; mime = "text/html"; }
	if (content) {
		g_autoptr(GBytes) bytes = g_bytes_new(content, strlen(content));
		g_autoptr(GError) dup_error = NULL;
		g_autoptr(VentureEntity) document = NULL, item = NULL;
		g_autofree gchar *title = g_strdup_printf("%s.%s", p->subject && *p->subject ? p->subject : "message", !strcmp(mime, "text/html") ? "html" : "txt");
		g_autofree gchar *extracted = NULL;
		gboolean duplicate = hash_already_captured(run, bytes, &dup_error);
		if (dup_error) { g_propagate_error(error, g_steal_pointer(&dup_error)); return FALSE; }
		if (duplicate) return TRUE;
		extracted = venture_document_extract_text(mime, title, bytes);
		document = file_document(run, title, "receipt", mime, bytes, extracted, error);
		if (!document) return FALSE;
		item = venture_capture_service_ingest_for_organization(capture_service, run->org, "receipt", p->subject && *p->subject ? p->subject : title, "email",
			venture_entity_get_id(document), vendor, NULL, p->date, p->text, run->actor, error);
		if (!item) return FALSE;
		g_object_set(inbound, "capture-item-id", venture_entity_get_id(item), NULL);
	}
	return TRUE;
}
/* Every write for one message. Runs inside the caller's transaction. */
static gboolean file_parsed(SyncRun *run, const gchar *folder, guint32 uidvalidity, guint32 uid, GBytes *raw, const gchar *skip_reason, Parsed *p, GError **error)
{
	g_autofree gchar *key = uid_key(run->account_id, folder, uidvalidity, uid), *thread = NULL, *body = NULL, *raw_title = NULL;
	g_autoptr(VentureEntity) inbound = NULL, raw_document = NULL, existing = NULL;
	gboolean outbound = run->own_normal && *run->own_normal && p->from && !g_strcmp0(run->own_normal, p->from);
	gboolean capture = p->to_capture || (run->capture_folder && *run->capture_folder && !g_strcmp0(run->capture_folder, folder));
	existing = find_existing(run->db, run->org, run->private_owner, p->message_id, error);
	if (error && *error) return FALSE;
	if (existing) g_object_get(existing, "thread-id", &thread, NULL);
	else if (!(thread = resolve_thread(run->db, run->org, run->private_owner, p, error))) return FALSE;
	inbound = g_object_new(VENTURE_TYPE_MAIL_INBOUND, "organization-id", run->org, "account-id", run->account_id, "folder", folder,
		"uid", (gint64)uid, "uid-validity", (gint64)uidvalidity, "uid-key", key, "message-id", p->message_id, "thread-id", thread,
		"from-address", p->from, "subject", p->subject && *p->subject ? p->subject : "(no subject)", "received-at", p->date,
		"outbound", outbound, "skip-reason", skip_reason, NULL);
	raw_title = g_strdup_printf("%s.eml", p->subject && *p->subject ? p->subject : "message");
	if (existing) {
		/* Another copy of mail already filed: record where this one sits,
		 * point at what the first copy created, and create nothing new. */
		gint64 root = 0, document = 0, contact = 0, interaction = 0, item = 0;
		g_object_get(existing, "duplicate-of-id", &root, "document-id", &document, "contact-id", &contact, "interaction-id", &interaction, "capture-item-id", &item, NULL);
		g_object_set(inbound, "duplicate-of-id", root ? root : venture_entity_get_id(existing), "contact-id", live_reference(run->db, VENTURE_TYPE_CONTACT, contact),
			"interaction-id", live_reference(run->db, VENTURE_TYPE_INTERACTION, interaction), NULL);
		if (item && type_on("capture_item")) g_object_set(inbound, "capture-item-id", live_reference(run->db, VENTURE_TYPE_CAPTURE_ITEM, item), NULL);
		document = live_reference(run->db, VENTURE_TYPE_DOCUMENT, document);
		if (!document) {
			/* The outbox recorded the send without the bytes; this copy has them. */
			raw_document = file_document(run, raw_title, "email", "message/rfc822", raw, p->text, error);
			if (!raw_document) return FALSE;
			document = venture_entity_get_id(raw_document);
		}
		g_object_set(inbound, "document-id", document, NULL);
		return venture_database_save(run->db, inbound, run->actor, error);
	}
	/* The searchable text of an email is its body, never the raw MIME. */
	raw_document = file_document(run, raw_title, "email", "message/rfc822", raw, p->text, error);
	if (!raw_document) return FALSE;
	g_object_set(inbound, "document-id", venture_entity_get_id(raw_document), NULL);
	/* Private mail remains an inbox record and an owned raw document. Its
	 * participants and receipt attachments are not organization CRM input. */
	if (run->private_owner > 0) return venture_database_save(run->db, inbound, run->actor, error);
	body = timeline_body(p->from, p->message_id, thread, p->text, skip_reason);
	/* A truncated capture message has lost its attachments; filing its
	 * remains as a receipt would invent one, so it stays a row whose skip
	 * reason says why. */
	if (!capture) {
		if (!file_participants(run, p, thread, body, outbound, inbound, error)) return FALSE;
	} else if (!skip_reason && type_on("capture_item")) {
		if (!file_capture(run, p, inbound, error)) return FALSE;
	}
	return venture_database_save(run->db, inbound, run->actor, error);
}
static gboolean commit_or_roll_back(SyncRun *run, gboolean ok, GError **error)
{
	if (ok && venture_database_commit(run->db, error)) {
		g_ptr_array_set_size(run->written, 0);
		return TRUE;
	}
	if (venture_database_has_transaction(run->db)) venture_database_rollback(run->db);
	discard_written(run);
	refresh_account(run);
	return FALSE;
}
static gboolean file_message(SyncRun *run, const gchar *folder, guint32 uidvalidity, FolderCursor *cursor, guint32 uid, GBytes *raw, const gchar *skip_reason, GError **error)
{
	FolderCursor before = *cursor;
	Parsed p;
	gboolean ok;
	if (!parse_message(raw, run->capture_address, &p, error)) { parsed_clear(&p); return FALSE; }
	if (!venture_database_begin(run->db, error)) { parsed_clear(&p); return FALSE; }
	if (!venture_connector_session_validate(run->connector, error)) { venture_database_rollback(run->db); parsed_clear(&p); return FALSE; }
	ok = file_parsed(run, folder, uidvalidity, uid, raw, skip_reason, &p, error);
	if (ok) {
		cursor->uid = uid; cursor->failed_uid = 0; cursor->attempts = 0;
		cursor_set(run->cursors, folder, cursor);
		ok = store_cursors(run, error);
	}
	parsed_clear(&p);
	if (commit_or_roll_back(run, ok, error)) return TRUE;
	*cursor = before;
	cursor_set(run->cursors, folder, cursor);
	return FALSE;
}
/* After repeated failures: a row that says why, so the folder moves on and
 * the mail is not silently lost. It has no document and creates nothing. */
static gboolean file_stub(SyncRun *run, const gchar *folder, guint32 uidvalidity, FolderCursor *cursor, guint32 uid, GBytes *raw, const gchar *reason, GError **error)
{
	FolderCursor before = *cursor;
	g_autofree gchar *key = uid_key(run->account_id, folder, uidvalidity, uid);
	g_autoptr(VentureEntity) inbound = NULL;
	g_autoptr(GDateTime) now = g_date_time_new_now_utc();
	Parsed p;
	gboolean parsed = raw && parse_message(raw, NULL, &p, NULL), ok;
	if (!raw) memset(&p, 0, sizeof p);
	inbound = g_object_new(VENTURE_TYPE_MAIL_INBOUND, "organization-id", run->org, "account-id", run->account_id, "folder", folder,
		"uid", (gint64)uid, "uid-validity", (gint64)uidvalidity, "uid-key", key, "skip-reason", reason,
		"subject", parsed && p.subject && *p.subject ? p.subject : "(unreadable message)", "message-id", parsed ? p.message_id : NULL,
		"thread-id", parsed ? p.message_id : NULL, "from-address", parsed ? p.from : NULL, "received-at", parsed ? p.date : now, NULL);
	parsed_clear(&p);
	if (!venture_database_begin(run->db, error)) return FALSE;
	ok = venture_connector_session_validate(run->connector, error) && venture_database_save(run->db, inbound, run->actor, error);
	if (ok) {
		cursor->uid = uid; cursor->failed_uid = 0; cursor->attempts = 0;
		cursor_set(run->cursors, folder, cursor);
		ok = store_cursors(run, error);
	}
	if (commit_or_roll_back(run, ok, error)) return TRUE;
	*cursor = before;
	cursor_set(run->cursors, folder, cursor);
	return FALSE;
}

/* --- Classifying what went wrong --------------------------------------------- */
/* The IMAP session is gone or never became usable: stop the account and
 * count it towards backoff. */
static gboolean error_is_session(const GError *error)
{
	if (!error) return FALSE;
	if (error->domain == G_IO_ERROR || error->domain == G_TLS_ERROR || error->domain == G_RESOLVER_ERROR) return TRUE;
	return error->domain == VENTURE_ERROR && (error->code == VENTURE_ERROR_NETWORK || error->code == VENTURE_ERROR_TIMEOUT ||
		error->code == VENTURE_ERROR_UNAUTHENTICATED || error->code == VENTURE_ERROR_UNSUPPORTED);
}
/* Worth trying the same message again on the next call: the wire, a locked
 * database, a record somebody edited meanwhile, missing storage. Anything
 * else is the message's own and repeats on every attempt. */
static gboolean error_is_transient(const GError *error)
{
	if (!error || error_is_session(error)) return TRUE;
	if (error->domain == G_FILE_ERROR) return error->code != G_FILE_ERROR_NAMETOOLONG && error->code != G_FILE_ERROR_INVAL;
	if (error->domain != VENTURE_ERROR) return FALSE;
	switch (error->code) {
	case VENTURE_ERROR_MAIL_TRANSIENT:
	case VENTURE_ERROR_CONFLICT:
	case VENTURE_ERROR_CONFIG:
		return TRUE;
	case VENTURE_ERROR_DATABASE:
		/* A constraint the row breaks fails identically every time. */
		return !(strstr(error->message, "UNIQUE") || strstr(error->message, "unique") || strstr(error->message, "constraint") ||
			strstr(error->message, "duplicate key") || strstr(error->message, "violates"));
	default:
		return FALSE;
	}
}

/* --- Folders and messages ------------------------------------------------------ */
typedef enum { OUTCOME_CONTINUE, OUTCOME_FOLDER_STOPPED, OUTCOME_ACCOUNT_STOPPED } SyncOutcome;
static SyncOutcome message_failed(SyncRun *run, const gchar *folder, guint32 uidvalidity, FolderCursor *cursor, guint32 uid, GBytes *raw, const GError *failure, GError **error)
{
	if (cursor->failed_uid == uid) cursor->attempts++;
	else { cursor->failed_uid = uid; cursor->attempts = 1; }
	if (cursor->attempts >= VENTURE_MAIL_SYNC_MAX_ATTEMPTS) {
		g_autofree gchar *reason = g_strdup_printf("Skipped after %u attempts: %s", cursor->attempts, failure->message);
		if (!file_stub(run, folder, uidvalidity, cursor, uid, raw, reason, error)) return OUTCOME_ACCOUNT_STOPPED;
		run->filed++;
		return OUTCOME_CONTINUE;
	}
	cursor_set(run->cursors, folder, cursor);
	if (!store_cursors(run, error)) { refresh_account(run); return OUTCOME_ACCOUNT_STOPPED; }
	note_problem(run, "%s UID %u: %s (attempt %u of %d)", folder, uid, failure->message, cursor->attempts, VENTURE_MAIL_SYNC_MAX_ATTEMPTS);
	return OUTCOME_FOLDER_STOPPED;
}
static SyncOutcome sync_message(SyncRun *run, const gchar *folder, const VentureImapFolderInfo *info, FolderCursor *cursor, const VentureImapEntry *entry, GError **error)
{
	g_autoptr(GError) local = NULL;
	g_autoptr(GBytes) raw = NULL;
	g_autofree gchar *skip_reason = NULL;
	gint filed;
	if (!venture_connector_session_validate(run->connector, error)) return OUTCOME_ACCOUNT_STOPPED;
	run->budget->messages_left--;
	/* Checked before the fetch: a rerun over filed mail costs a query per
	 * message, not a download. */
	filed = uid_filed(run, folder, cursor, info->uidvalidity, entry->uid, error);
	if (filed < 0) return OUTCOME_ACCOUNT_STOPPED;
	if (filed) {
		/* Still advance the high-water mark so a repaired cursor does not
		 * walk the same UIDs forever. */
		FolderCursor before = *cursor;
		cursor->uid = entry->uid;
		if (cursor->failed_uid == entry->uid) { cursor->failed_uid = 0; cursor->attempts = 0; }
		cursor_set(run->cursors, folder, cursor);
		if (store_cursors(run, error)) return OUTCOME_CONTINUE;
		*cursor = before;
		cursor_set(run->cursors, folder, cursor);
		refresh_account(run);
		return OUTCOME_ACCOUNT_STOPPED;
	}
	if (entry->size > VENTURE_IMAP_MAX_MESSAGE) {
		/* Refusing it outright would stop the folder here forever. The
		 * header and the start of the text still make a timeline entry. */
		skip_reason = g_strdup_printf("Oversize: %" G_GUINT64_FORMAT " bytes; only the header and the first %" G_GSIZE_FORMAT " KiB of text were read",
			entry->size, VENTURE_MAIL_SYNC_TRUNCATED_TEXT / 1024);
		raw = venture_imap_client_fetch_truncated(run->self->client, entry->uid, VENTURE_MAIL_SYNC_TRUNCATED_TEXT, NULL, &local);
	} else raw = venture_imap_client_fetch(run->self->client, entry->uid, NULL, &local);
	if (!venture_connector_session_validate(run->connector, error)) return OUTCOME_ACCOUNT_STOPPED;
	if (!raw) {
		if (error_is_transient(local)) { g_propagate_error(error, g_steal_pointer(&local)); return OUTCOME_ACCOUNT_STOPPED; }
		return message_failed(run, folder, info->uidvalidity, cursor, entry->uid, NULL, local, error);
	}
	if (file_message(run, folder, info->uidvalidity, cursor, entry->uid, raw, skip_reason, &local)) {
		run->filed++;
		return OUTCOME_CONTINUE;
	}
	if (error_is_transient(local)) { g_propagate_error(error, g_steal_pointer(&local)); return OUTCOME_ACCOUNT_STOPPED; }
	return message_failed(run, folder, info->uidvalidity, cursor, entry->uid, raw, local, error);
}
static SyncOutcome sync_folder(SyncRun *run, const gchar *folder, GError **error)
{
	VentureImapClient *client = run->self->client;
	g_autoptr(GError) local = NULL;
	VentureImapFolderInfo info;
	FolderCursor cursor;
	if (!venture_connector_session_validate(run->connector, error)) return OUTCOME_ACCOUNT_STOPPED;
	if (!venture_imap_client_select(client, folder, &info, NULL, &local)) {
		if (error_is_session(local)) { g_propagate_error(error, g_steal_pointer(&local)); return OUTCOME_ACCOUNT_STOPPED; }
		/* One missing or renamed folder must not stop the ones after it. */
		note_problem(run, "%s: %s", folder, local->message);
		return OUTCOME_FOLDER_STOPPED;
	}
	cursor_get(run->cursors, folder, &cursor);
	if (cursor.uidvalidity && info.uidvalidity && cursor.uidvalidity != info.uidvalidity) {
		/* The server renumbered the folder. The old mark would skip new
		 * mail and the old keys would call it filed; start again and let
		 * the Message-ID keep filed mail from being filed twice. */
		memset(&cursor, 0, sizeof cursor);
		cursor.uidvalidity = info.uidvalidity;
	} else if (!cursor.uidvalidity && info.uidvalidity) {
		cursor.uidvalidity = info.uidvalidity;
		if (cursor.uid) cursor.legacy = TRUE;
	}
	if (!cursor.uid && run->since) {
		guint32 after = 0;
		if (!venture_imap_client_since(client, run->since, &after, NULL, &local)) {
			if (error_is_session(local)) { g_propagate_error(error, g_steal_pointer(&local)); return OUTCOME_ACCOUNT_STOPPED; }
			note_problem(run, "%s: %s", folder, local->message);
			return OUTCOME_FOLDER_STOPPED;
		}
		cursor.uid = after;
	}
	cursor_set(run->cursors, folder, &cursor);
	for (;;) {
		g_autoptr(GArray) entries = NULL;
		guint i;
		if (budget_spent(run->budget)) return OUTCOME_CONTINUE;
		entries = venture_imap_client_list(client, cursor.uid, MIN((guint)VENTURE_MAIL_SYNC_LIST_PAGE, run->budget->messages_left), NULL, &local);
		if (!entries) {
			if (error_is_session(local)) { g_propagate_error(error, g_steal_pointer(&local)); return OUTCOME_ACCOUNT_STOPPED; }
			note_problem(run, "%s: %s", folder, local->message);
			return OUTCOME_FOLDER_STOPPED;
		}
		if (!entries->len) return OUTCOME_CONTINUE;
		for (i = 0; i < entries->len; i++) {
			SyncOutcome outcome;
			if (budget_spent(run->budget)) return OUTCOME_CONTINUE;
			outcome = sync_message(run, folder, &info, &cursor, &g_array_index(entries, VentureImapEntry, i), error);
			if (outcome != OUTCOME_CONTINUE) return outcome;
		}
	}
}

/* --- One account under a lease ------------------------------------------------ */
static gboolean take_lease(SyncRun *run, GError **error)
{
	g_autoptr(GDateTime) now = g_date_time_new_now_utc();
	g_autoptr(GDateTime) lease = NULL, until = NULL;
	g_autofree gchar *cursor_text = NULL;
	g_autoptr(JsonNode) cursor_node = NULL;
	gint64 seconds = MAX((gint64)0, (run->budget->deadline - g_get_monotonic_time()) / G_USEC_PER_SEC);
	run->account = venture_database_get(run->db, VENTURE_TYPE_MAIL_ACCOUNT, run->account_id, NULL);
	if (!run->account || venture_entity_is_deleted(run->account)) return refuse(error, VENTURE_ERROR_NOT_FOUND, "The mail account no longer exists");
	run->org = venture_entity_get_organization_id(run->account);
	g_object_get(run->account, "sync-lease-until", &lease, "cursors", &cursor_text, NULL);
	/* Overlapping callers (a second process, or a request served from a
	 * nested main loop) would fetch and file the same UIDs twice. */
	if (lease && g_date_time_compare(lease, now) > 0) return refuse(error, VENTURE_ERROR_CONFLICT, "Another sync holds this account's lease");
	cursor_node = cursor_text && *cursor_text ? json_from_string(cursor_text, NULL) : NULL;
	run->cursors = cursor_node && JSON_NODE_HOLDS_OBJECT(cursor_node) ? json_object_ref(json_node_get_object(cursor_node)) : json_object_new();
	until = g_date_time_add_seconds(now, (gdouble)(seconds + 60));
	g_object_set(run->account, "sync-lease-until", until, NULL);
	return venture_database_save(run->db, run->account, run->actor, error);
}
static gboolean prepare(SyncRun *run, GError **error)
{
	g_autofree gchar *folders = NULL, *address = NULL, *ignore = NULL, *internal = NULL;
	g_autoptr(GPtrArray) patterns = NULL;
	guint i;
	run->connector = venture_connector_open(run->db, run->self->config, run->account, error);
	if (!run->connector) return FALSE;
	run->org = venture_entity_get_organization_id(venture_connector_session_get_account(run->connector));
	run->private_owner = venture_access_policy_get_personal_owner(venture_database_get_access_policy(run->db), venture_connector_session_get_account(run->connector));
	run->secret = venture_connector_session_get_password(run->connector);
	g_object_get(run->account, "imap-host", &run->host, "imap-port", &run->port, "imap-tls", &run->security, "username", &run->username,
		"folders", &folders, "address", &address, "capture-address", &run->capture_address,
		"capture-folder", &run->capture_folder, "ignore-patterns", &ignore, "internal-domains", &internal, "sync-since", &run->since, NULL);
	run->folders = g_ptr_array_new_with_free_func(g_free);
	run->ignore = g_ptr_array_new_with_free_func((GDestroyNotify)g_pattern_spec_free);
	run->internal = split_list(internal, ",");
	patterns = split_list(ignore, ",\n");
	for (i = 0; i < patterns->len; i++) g_ptr_array_add(run->ignore, g_pattern_spec_new(g_ptr_array_index(patterns, i)));
	run->own_normal = venture_lead_normalize_email(address);
	if (venture_string_is_empty(run->host)) return refuse(error, VENTURE_ERROR_CONFIG, "The account has no IMAP host");
	if (run->port <= 0 || run->port > 65535) run->port = !g_strcmp0(run->security, "starttls") ? 143 : 993;
	if (venture_string_is_empty(run->security)) { g_free(run->security); run->security = g_strdup("tls"); }
	/* Rows saved before the validator existed can still hold this. */
	if (!venture_string_is_empty(run->capture_address) && capture_matches(address, run->capture_address))
		return refuse(error, VENTURE_ERROR_CONFIG, "capture_address matches the account's own address, so every message would become a capture item");
	{
		g_auto(GStrv) parts = g_strsplit(venture_string_is_empty(folders) ? "INBOX" : folders, ",", -1);
		for (i = 0; parts[i]; i++) {
			gchar *name = g_strstrip(parts[i]);
			if (*name && !g_ptr_array_find_with_equal_func(run->folders, name, g_str_equal, NULL)) g_ptr_array_add(run->folders, g_strdup(name));
		}
	}
	/* A capture folder nobody listed would never be read. */
	if (!venture_string_is_empty(run->capture_folder)) {
		g_autofree gchar *copy = g_strstrip(g_strdup(run->capture_folder));
		if (*copy && !g_ptr_array_find_with_equal_func(run->folders, copy, g_str_equal, NULL)) g_ptr_array_add(run->folders, g_steal_pointer(&copy));
	}
	if (!run->folders->len) g_ptr_array_add(run->folders, g_strdup("INBOX"));
	return TRUE;
}
/* Records the outcome on the account and releases the lease. A failure to
 * reach the mailbox backs off exponentially; enough in a row ending in a
 * refused login switches the account off, because a provider locks a
 * mailbox that keeps presenting a wrong password. */
static gboolean apply_outcome(SyncRun *run, const GError *stopped, gboolean session_failed, gboolean auth_failed, gboolean *deactivated, GError **error)
{
	g_autoptr(GDateTime) now = g_date_time_new_now_utc();
	g_autoptr(GString) message = g_string_new(NULL);
	gint64 failures = 0;
	gboolean active = FALSE;
	g_object_get(run->account, "consecutive-failures", &failures, "active", &active, NULL);
	if (stopped) g_string_append(message, stopped->message);
	if (run->problems->len) g_string_append_printf(message, "%s%s", message->len ? "; " : "", run->problems->str);
	*deactivated = FALSE;
	if (session_failed) {
		g_autoptr(GDateTime) next = NULL;
		gint64 delay;
		failures = MAX((gint64)0, failures) + 1;
		delay = MIN((gint64)VENTURE_MAIL_SYNC_BACKOFF_MAX, (gint64)VENTURE_MAIL_SYNC_BACKOFF_BASE << MIN(failures - 1, (gint64)10));
		next = g_date_time_add_seconds(now, (gdouble)delay);
		g_object_set(run->account, "consecutive-failures", failures, "next-attempt-at", next, NULL);
		if (auth_failed && active && failures >= VENTURE_MAIL_SYNC_AUTH_FAILURE_LIMIT) {
			g_object_set(run->account, "active", FALSE, NULL);
			*deactivated = TRUE;
		}
	} else if (!stopped) g_object_set(run->account, "consecutive-failures", (gint64)0, "next-attempt-at", NULL, NULL);
	g_object_set(run->account, "last-synced-at", now, "last-error", message->len ? message->str : NULL, "sync-lease-until", NULL, NULL);
	{
		g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);
		g_autofree gchar *text = NULL;
		json_node_set_object(node, run->cursors);
		text = json_to_string(node, FALSE);
		g_object_set(run->account, "cursors", text, NULL);
	}
	return venture_database_save(run->db, run->account, run->actor, error);
}
static gboolean finish(SyncRun *run, VentureEntity *account, const GError *stopped, gboolean session_failed, gboolean auth_failed, GError **error)
{
	g_autoptr(GError) save_error = NULL;
	gboolean deactivated = FALSE;
	if (!apply_outcome(run, stopped, session_failed, auth_failed, &deactivated, &save_error)) {
		/* Somebody edited the account while it synced: apply the outcome
		 * to what they saved rather than losing either. */
		g_clear_error(&save_error);
		refresh_account(run);
		apply_outcome(run, stopped, session_failed, auth_failed, &deactivated, &save_error);
	}
	if (deactivated && run->self->context) {
		g_autofree gchar *address = NULL, *title = NULL, *body = NULL;
		g_object_get(run->account, "address", &address, NULL);
		title = g_strdup_printf("Mail account switched off: %s", address ? address : "");
		body = g_strdup_printf("%d syncs in a row failed, the last with a refused login. Fix the credentials and switch it back on.", VENTURE_MAIL_SYNC_AUTH_FAILURE_LIMIT);
		venture_notify_broadcast(run->self->context, VENTURE_USER_ROLE_ADMIN, VENTURE_NOTIFICATION_KIND_SYSTEM, title, body,
			"mail_account", run->account_id, address, NULL, NULL);
	}
	if (account != run->account) venture_entity_copy_properties_from(account, run->account, FALSE);
	if (stopped) { g_propagate_error(error, g_error_copy(stopped)); return FALSE; }
	if (run->problems->len) { g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_FAILED, "VentureMailSyncService: %s", run->problems->str); return FALSE; }
	if (save_error) { g_propagate_error(error, g_steal_pointer(&save_error)); return FALSE; }
	return TRUE;
}
static gboolean sync_account(VentureMailSyncService *self, VentureEntity *account, const VentureActor *actor, SyncBudget *budget, gint *filed, GError **error)
{
	g_autoptr(GError) stopped = NULL;
	gboolean session_failed = FALSE, auth_failed = FALSE, ok;
	SyncRun run;
	guint i;
	*filed = 0;
	if (!self->database) return refuse(error, VENTURE_ERROR_DATABASE, "The database has been closed");
	if (!module_on()) return refuse(error, VENTURE_ERROR_CONFIG, "The mail_sync module is disabled");
	if (self->busy) return refuse(error, VENTURE_ERROR_CONFLICT, "A sync is already in progress");
	if (venture_database_has_transaction(self->database)) return refuse(error, VENTURE_ERROR_CONFLICT, "Sync requires a committed database");
	if (!venture_entity_get_id(account)) return refuse(error, VENTURE_ERROR_VALIDATION, "Sync requires a saved account");
	memset(&run, 0, sizeof run);
	run.self = self;
	run.db = self->database;
	run.actor = actor;
	run.budget = budget;
	run.account_id = venture_entity_get_id(account);
	run.org = venture_entity_get_organization_id(account);
	run.written = g_ptr_array_new_with_free_func(g_free);
	run.problems = g_string_new(NULL);
	self->busy = TRUE;
	if (!take_lease(&run, error)) {
		self->busy = FALSE;
		sync_run_clear(&run);
		return FALSE;
	}
	venture_imap_client_set_deadline(self->client, budget->deadline);
	if (!prepare(&run, &stopped)) session_failed = TRUE;
	else if (!venture_imap_client_connect(self->client, run.host, (guint16)run.port, run.security, run.username, run.secret, NULL, &stopped)) {
		session_failed = TRUE;
		auth_failed = g_error_matches(stopped, VENTURE_ERROR, VENTURE_ERROR_UNAUTHENTICATED);
	} else {
		for (i = 0; i < run.folders->len && !budget->deferred; i++) {
			if (sync_folder(&run, g_ptr_array_index(run.folders, i), &stopped) != OUTCOME_ACCOUNT_STOPPED) continue;
			/* Out of time mid-mailbox is not the mailbox's fault: the
			 * cursor resumes next call and nothing backs off. */
			if (g_error_matches(stopped, VENTURE_ERROR, VENTURE_ERROR_TIMEOUT)) { budget->deferred = TRUE; g_clear_error(&stopped); }
			else session_failed = error_is_session(stopped);
			break;
		}
		venture_imap_client_disconnect(self->client);
	}
	venture_imap_client_set_deadline(self->client, 0);
	*filed = run.filed;
	ok = finish(&run, account, stopped, session_failed, auth_failed, error);
	self->busy = FALSE;
	sync_run_clear(&run);
	return ok;
}
static void budget_start(VentureMailSyncService *self, SyncBudget *budget)
{
	budget->messages_left = self->message_budget;
	budget->deadline = g_get_monotonic_time() + (gint64)self->time_budget * G_USEC_PER_SEC;
	budget->deferred = FALSE;
}
gint venture_mail_sync_service_sync(VentureMailSyncService *self, VentureEntity *account, const VentureActor *actor, GError **error)
{
	SyncBudget budget;
	gint filed = 0;
	g_return_val_if_fail(VENTURE_IS_MAIL_SYNC_SERVICE(self), -1);
	g_return_val_if_fail(VENTURE_IS_MAIL_ACCOUNT(account), -1);
	budget_start(self, &budget);
	return sync_account(self, account, actor, &budget, &filed, error) ? filed : -1;
}

/* --- Reports ------------------------------------------------------------------- */
typedef struct { JsonBuilder *errors; guint accounts; guint skipped; gint messages; } Report;
static void report_account(Report *report, VentureMailSyncService *self, VentureEntity *account, const VentureActor *actor, SyncBudget *budget)
{
	g_autoptr(GError) local = NULL;
	gint filed = 0;
	gboolean ok = sync_account(self, account, actor, budget, &filed, &local);
	report->messages += filed;
	/* Another caller's lease is not a failure of this account. */
	if (g_error_matches(local, VENTURE_ERROR, VENTURE_ERROR_CONFLICT)) { report->skipped++; return; }
	report->accounts++;
	if (ok) return;
	json_builder_begin_object(report->errors);
	json_builder_set_member_name(report->errors, "account"); json_builder_add_int_value(report->errors, venture_entity_get_id(account));
	json_builder_set_member_name(report->errors, "error"); json_builder_add_string_value(report->errors, local ? local->message : "unknown");
	json_builder_end_object(report->errors);
}
static JsonNode *report_finish(Report *report, SyncBudget *budget)
{
	json_builder_end_array(report->errors);
	json_builder_set_member_name(report->errors, "accounts"); json_builder_add_int_value(report->errors, report->accounts);
	json_builder_set_member_name(report->errors, "messages"); json_builder_add_int_value(report->errors, report->messages);
	json_builder_set_member_name(report->errors, "skipped"); json_builder_add_int_value(report->errors, report->skipped);
	json_builder_set_member_name(report->errors, "deferred"); json_builder_add_boolean_value(report->errors, budget->deferred);
	json_builder_end_object(report->errors);
	return json_builder_get_root(report->errors);
}
static void report_start(Report *report)
{
	memset(report, 0, sizeof *report);
	report->errors = json_builder_new();
	json_builder_begin_object(report->errors);
	json_builder_set_member_name(report->errors, "errors");
	json_builder_begin_array(report->errors);
}
JsonNode *venture_mail_sync_service_sync_now(VentureMailSyncService *self, VentureEntity *account, const VentureActor *actor, GError **error)
{
	SyncBudget budget;
	Report report;
	JsonNode *node;
	g_return_val_if_fail(VENTURE_IS_MAIL_SYNC_SERVICE(self), NULL);
	g_return_val_if_fail(VENTURE_IS_MAIL_ACCOUNT(account), NULL);
	if (!module_on()) return refuse(error, VENTURE_ERROR_CONFIG, "The mail_sync module is disabled"), NULL;
	budget_start(self, &budget);
	report_start(&report);
	report_account(&report, self, account, actor, &budget);
	node = report_finish(&report, &budget);
	g_object_unref(report.errors);
	return node;
}
JsonNode *venture_mail_sync_service_sweep(VentureMailSyncService *self, gint64 organization_id, guint limit, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_MAIL_ACCOUNT);
	g_autoptr(GPtrArray) accounts = NULL;
	g_autoptr(GDateTime) now = g_date_time_new_now_utc();
	SyncBudget budget;
	Report report;
	JsonNode *node;
	guint i;
	g_return_val_if_fail(VENTURE_IS_MAIL_SYNC_SERVICE(self), NULL);
	if (organization_id <= 0) return refuse(error, VENTURE_ERROR_VALIDATION, "An exact organization is required"), NULL;
	if (!module_on()) return refuse(error, VENTURE_ERROR_CONFIG, "The mail_sync module is disabled"), NULL;
	if (!self->database) return refuse(error, VENTURE_ERROR_DATABASE, "The database has been closed"), NULL;
	venture_query_set_organization(query, organization_id);
	venture_query_add_filter_string(query, "active", VENTURE_FILTER_OP_EQ, "true", NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	accounts = venture_database_find(self->database, query, error);
	if (!accounts) return NULL;
	budget_start(self, &budget);
	report_start(&report);
	for (i = 0; i < accounts->len && report.accounts < limit; i++) {
		VentureEntity *account = g_ptr_array_index(accounts, i);
		g_autoptr(GDateTime) next = NULL;
		g_object_get(account, "next-attempt-at", &next, NULL);
		if (next && g_date_time_compare(next, now) > 0) { report.skipped++; continue; }
		if (budget_spent(&budget)) break;
		report_account(&report, self, account, actor, &budget);
	}
	node = report_finish(&report, &budget);
	g_object_unref(report.errors);
	return node;
}

/* --- Unmatched senders ----------------------------------------------------------- */
VentureEntity *venture_mail_sync_service_create_contact(VentureMailSyncService *self, VentureEntity *sender, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) contact = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autofree gchar *address = NULL, *name = NULL;
	gint64 org;
	guint i;
	g_return_val_if_fail(VENTURE_IS_MAIL_SYNC_SERVICE(self), NULL);
	g_return_val_if_fail(VENTURE_IS_MAIL_UNMATCHED_SENDER(sender), NULL);
	if (!self->database) return refuse(error, VENTURE_ERROR_DATABASE, "The database has been closed"), NULL;
	g_object_get(sender, "address", &address, "name", &name, NULL);
	org = venture_entity_get_organization_id(sender);
	if (!venture_database_begin(self->database, error)) return NULL;
	contact = g_object_new(VENTURE_TYPE_CONTACT, "organization-id", org, "name", name && *name ? name : address, "email", address, "source", "email", NULL);
	if (!venture_database_save(self->database, contact, actor, error) || !venture_database_delete(self->database, sender, actor, error)) goto fail;
	/* The mail that put the address on the list belongs on the new
	 * contact's timeline, not only the mail after it. */
	query = venture_query_new(VENTURE_TYPE_MAIL_INBOUND);
	venture_query_set_organization(query, org);
	venture_query_add_filter_string(query, "from-address", VENTURE_FILTER_OP_EQ, address, NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	venture_query_set_limit(query, VENTURE_MAIL_SYNC_BACKFILL_LIMIT);
	rows = venture_database_find(self->database, query, error);
	if (!rows) goto fail;
	for (i = 0; i < rows->len; i++) {
		VentureEntity *row = g_ptr_array_index(rows, i);
		g_autoptr(VentureEntity) interaction = NULL, document = NULL;
		g_autofree gchar *subject = NULL, *message_id = NULL, *thread = NULL, *text = NULL, *body = NULL, *reason = NULL, *from = NULL;
		g_autoptr(GDateTime) when = NULL;
		gint64 document_id = 0, item = 0, duplicate = 0, contact_id = 0;
		gboolean outbound = FALSE;
		g_object_get(row, "subject", &subject, "message-id", &message_id, "thread-id", &thread, "received-at", &when, "document-id", &document_id,
			"capture-item-id", &item, "duplicate-of-id", &duplicate, "contact-id", &contact_id, "outbound", &outbound, "skip-reason", &reason, "from-address", &from, NULL);
		/* A capture item was never timeline mail, and a copy's original is backfilled itself. */
		if (item || duplicate || (!document_id && !venture_string_is_empty(reason))) continue;
		document = document_id ? venture_database_get(self->database, VENTURE_TYPE_DOCUMENT, document_id, NULL) : NULL;
		if (document) g_object_get(document, "extracted-text", &text, NULL);
		body = timeline_body(from, message_id, thread, text, reason);
		interaction = record_interaction(self->database, org, contact, NULL, subject, body, when, outbound, 0, actor, error);
		if (!interaction) goto fail;
		if (!contact_id) {
			g_object_set(row, "contact-id", venture_entity_get_id(contact), "interaction-id", venture_entity_get_id(interaction), NULL);
			if (!venture_database_save(self->database, row, actor, error)) goto fail;
		}
	}
	if (!venture_database_commit(self->database, error)) return NULL;
	return g_steal_pointer(&contact);
fail:
	venture_database_rollback(self->database);
	return NULL;
}
VentureEntity *venture_mail_sync_service_dismiss(VentureMailSyncService *self, VentureEntity *sender, const VentureActor *actor, GError **error)
{
	g_return_val_if_fail(VENTURE_IS_MAIL_SYNC_SERVICE(self), NULL);
	g_return_val_if_fail(VENTURE_IS_MAIL_UNMATCHED_SENDER(sender), NULL);
	if (!self->database) return refuse(error, VENTURE_ERROR_DATABASE, "The database has been closed"), NULL;
	g_object_set(sender, "dismissed", TRUE, NULL);
	if (!venture_database_save(self->database, sender, actor, error)) return NULL;
	return g_object_ref(sender);
}

/* --- The outbox's side --------------------------------------------------------- */
gboolean venture_mail_sync_record_outbound(VentureDatabase *database, VentureMailMessage *message, GError **error)
{
	g_autofree gchar *to = NULL, *cc = NULL, *subject = NULL, *body = NULL, *message_id = NULL, *text = NULL, *key = NULL;
	g_autoptr(GPtrArray) addresses = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GDateTime) sent_at = NULL;
	g_autoptr(VentureEntity) inbound = NULL;
	gint64 org = venture_entity_get_organization_id(VENTURE_ENTITY(message));
	gboolean keep_row = FALSE;
	guint i, j;
	if (!module_on() || !type_on("contact") || org <= 0) return TRUE;
	g_object_get(message, "to", &to, "cc", &cc, "subject", &subject, "text-body", &text, "message-id", &message_id, "sent-at", &sent_at, NULL);
	{
		g_autofree gchar *joined = g_strdup_printf("%s,%s", to ? to : "", cc ? cc : "");
		g_auto(GStrv) parts = g_strsplit(joined, ",", -1);
		for (i = 0; parts[i]; i++) {
			gchar *address = g_strstrip(parts[i]), *open = strrchr(address, '<');
			g_autofree gchar *normal = NULL;
			if (open) { address = open + 1; if (strchr(address, '>')) *strchr(address, '>') = '\0'; }
			normal = venture_lead_normalize_email(address);
			if (*normal && !g_ptr_array_find_with_equal_func(addresses, normal, g_str_equal, NULL)) g_ptr_array_add(addresses, g_steal_pointer(&normal));
		}
	}
	if (!venture_database_begin(database, error)) return FALSE;
	/* Only an organization that syncs mail needs the Message-ID kept. */
	{
		g_autoptr(VentureQuery) accounts = venture_query_new(VENTURE_TYPE_MAIL_ACCOUNT);
		venture_query_set_organization(accounts, org);
		keep_row = venture_database_count(database, accounts, NULL) > 0 && message_id && *message_id;
	}
	if (keep_row) {
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_MAIL_INBOUND);
		key = g_strdup_printf("outbox:%" G_GINT64_FORMAT, venture_entity_get_id(VENTURE_ENTITY(message)));
		venture_query_set_organization(query, org);
		venture_query_set_include_deleted(query, TRUE);
		venture_query_add_filter_string(query, "uid-key", VENTURE_FILTER_OP_EQ, key, NULL);
		if (venture_database_count(database, query, NULL) > 0) return venture_database_commit(database, error);
	}
	/* The private body is a credential and never reaches the timeline. */
	body = g_strdup_printf("Message-ID: %s\n\n%s", message_id ? message_id : "", text ? text : "");
	if (keep_row) {
		g_autofree gchar *stripped = strip_brackets(message_id);
		inbound = g_object_new(VENTURE_TYPE_MAIL_INBOUND, "organization-id", org, "folder", "", "uid-key", key, "message-id", stripped,
			"thread-id", stripped, "subject", subject && *subject ? subject : "(no subject)", "received-at", sent_at, "outbound", TRUE, NULL);
	}
	/* Addresses are unique after normalisation, and a row has one address,
	 * so no party is recorded twice. */
	for (i = 0; i < addresses->len; i++) {
		const gchar *address = g_ptr_array_index(addresses, i);
		g_autoptr(GPtrArray) contacts = find_by_email(database, VENTURE_TYPE_CONTACT, org, address, error);
		g_autoptr(GPtrArray) leads = NULL;
		if (!contacts) goto fail;
		if (!contacts->len && type_on("lead") && !(leads = find_by_email(database, VENTURE_TYPE_LEAD, org, address, error))) goto fail;
		for (j = 0; j < contacts->len + (leads ? leads->len : 0); j++) {
			VentureEntity *party = j < contacts->len ? g_ptr_array_index(contacts, j) : g_ptr_array_index(leads, j - contacts->len);
			gboolean is_contact = j < contacts->len;
			g_autoptr(VentureEntity) interaction = NULL;
			gint64 deal = 0;
			if (is_contact && (deal = open_deal(database, org, venture_entity_get_id(party), error)) < 0) goto fail;
			interaction = record_interaction(database, org, is_contact ? party : NULL, is_contact ? NULL : party, subject, body, sent_at, TRUE, deal, NULL, error);
			if (!interaction) goto fail;
			if (inbound) {
				gint64 current = 0;
				g_object_get(inbound, "interaction-id", &current, NULL);
				if (!current) g_object_set(inbound, "contact-id", is_contact ? venture_entity_get_id(party) : (gint64)0, "interaction-id", venture_entity_get_id(interaction), NULL);
			}
		}
	}
	if (inbound && !venture_database_save(database, inbound, NULL, error)) goto fail;
	return venture_database_commit(database, error);
fail:
	venture_database_rollback(database);
	return FALSE;
}

/* --- Save validation --------------------------------------------------------------- */
static gboolean account_validate(VentureDatabase *db, VentureEntity *entity, VentureEntity *previous, gpointer data, GError **error)
{
	g_autofree gchar *address = NULL, *capture = NULL, *old_address = NULL, *old_capture = NULL;
	(void)db; (void)data;
	g_object_get(entity, "address", &address, "capture-address", &capture, NULL);
	/* Only a change is judged, so the sync can still record on a row
	 * saved before this check existed; the sync refuses to read it. */
	if (previous) {
		g_object_get(previous, "address", &old_address, "capture-address", &old_capture, NULL);
		if (!g_strcmp0(address, old_address) && !g_strcmp0(capture, old_capture)) return TRUE;
	}
	if (venture_string_is_empty(capture) || !capture_matches(address, capture)) return TRUE;
	venture_set_error_validation(error, "capture_address", "must differ from the account address, or every message the account receives becomes a capture item");
	return FALSE;
}
void venture_mail_sync_install_validators(VentureDatabase *database)
{
	g_return_if_fail(VENTURE_IS_DATABASE(database));
	venture_database_add_save_validator(database, VENTURE_TYPE_MAIL_ACCOUNT, account_validate, NULL, NULL);
}
