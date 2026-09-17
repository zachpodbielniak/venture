/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <string.h>
#include <gmime/gmime.h>
#include <glib/gstdio.h>
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GMimeStream, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GMimeParser, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GMimeMessage, g_object_unref)

struct _VentureMailSyncService {
	GObject parent_instance;
	VentureDatabase *database;
	VentureImapClient *client;
	gchar *attachment_root;
	gboolean busy;
};
G_DEFINE_FINAL_TYPE(VentureMailSyncService, venture_mail_sync_service, G_TYPE_OBJECT)

static gboolean refuse(GError **error, VentureError code, const gchar *message)
{
	g_set_error(error, VENTURE_ERROR, code, "VentureMailSyncService: %s", message);
	return FALSE;
}
static gboolean module_on(void)
{
	return venture_entity_registry_lookup(venture_entity_registry_get_default(), "mail_inbound") != G_TYPE_INVALID;
}
static void finalize(GObject *object)
{
	VentureMailSyncService *self = VENTURE_MAIL_SYNC_SERVICE(object);
	if (self->database) g_object_remove_weak_pointer(G_OBJECT(self->database), (gpointer *)&self->database);
	g_clear_object(&self->client);
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
	g_mime_init();
}
static void venture_mail_sync_service_init(VentureMailSyncService *self) { }
VentureMailSyncService *venture_mail_sync_service_new(VentureDatabase *database, VentureImapClient *client)
{
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	g_return_val_if_fail(VENTURE_IS_IMAP_CLIENT(client), NULL);
	return g_object_new(VENTURE_TYPE_MAIL_SYNC_SERVICE, "database", database, "client", client, NULL);
}

/* --- Parsed message ------------------------------------------------------ */
typedef struct { gchar *name; gchar *mime; GBytes *data; } Attachment;
typedef struct {
	gchar *from; gchar *from_name; GPtrArray *addresses; /* every From/To/Cc, lower-cased */
	gchar *message_id; gchar *in_reply_to; GPtrArray *references;
	gchar *subject; gchar *text; GDateTime *date;
	GPtrArray *attachments; gboolean to_capture;
} Parsed;
static void attachment_free(gpointer data)
{
	Attachment *a = data;
	g_free(a->name); g_free(a->mime); g_clear_pointer(&a->data, g_bytes_unref); g_free(a);
}
static void parsed_clear(Parsed *p)
{
	g_free(p->from); g_free(p->from_name); g_clear_pointer(&p->addresses, g_ptr_array_unref);
	g_free(p->message_id); g_free(p->in_reply_to); g_clear_pointer(&p->references, g_ptr_array_unref);
	g_free(p->subject); g_free(p->text); g_clear_pointer(&p->date, g_date_time_unref);
	g_clear_pointer(&p->attachments, g_ptr_array_unref);
}
static gchar *strip_brackets(const gchar *id)
{
	g_autofree gchar *copy = g_strdup(id ? id : "");
	gchar *s = g_strstrip(copy);
	if (*s == '<') s++;
	if (*s && s[strlen(s) - 1] == '>') s[strlen(s) - 1] = '\0';
	return g_strdup(s);
}
static void collect_addresses(InternetAddressList *list, GPtrArray *into, const gchar *capture, gboolean *to_capture)
{
	gint i;
	for (i = 0; list && i < internet_address_list_length(list); i++) {
		InternetAddress *address = internet_address_list_get_address(list, i);
		const gchar *addr = INTERNET_ADDRESS_IS_MAILBOX(address) ? internet_address_mailbox_get_addr(INTERNET_ADDRESS_MAILBOX(address)) : NULL;
		if (!addr) continue;
		g_ptr_array_add(into, g_ascii_strdown(addr, -1));
		if (capture && *capture && !g_ascii_strcasecmp(addr, capture)) *to_capture = TRUE;
	}
}
static void walk_part(GMimeObject *parent, GMimeObject *part, gpointer data)
{
	Parsed *p = data;
	const gchar *filename;
	if (!GMIME_IS_PART(part)) return;
	filename = g_mime_part_get_filename(GMIME_PART(part));
	if (filename || g_mime_part_is_attachment(GMIME_PART(part))) {
		g_autoptr(GMimeStream) stream = g_mime_stream_mem_new();
		GMimeDataWrapper *content = g_mime_part_get_content(GMIME_PART(part));
		GMimeContentType *type = g_mime_object_get_content_type(part);
		Attachment *a = g_new0(Attachment, 1);
		GByteArray *bytes;
		if (content) g_mime_data_wrapper_write_to_stream(content, stream);
		bytes = g_mime_stream_mem_get_byte_array(GMIME_STREAM_MEM(stream));
		a->name = g_strdup(filename ? filename : "attachment");
		a->mime = type ? g_mime_content_type_get_mime_type(type) : g_strdup("application/octet-stream");
		a->data = g_bytes_new(bytes->data, bytes->len);
		g_ptr_array_add(p->attachments, a);
		return;
	}
	if (!p->text && GMIME_IS_TEXT_PART(part) && g_mime_content_type_is_type(g_mime_object_get_content_type(part), "text", "plain"))
		p->text = g_mime_text_part_get_text(GMIME_TEXT_PART(part));
}
static gboolean parse_message(GBytes *raw, const gchar *capture_address, Parsed *p, GError **error)
{
	g_autoptr(GMimeStream) stream = NULL;
	g_autoptr(GMimeParser) parser = NULL;
	g_autoptr(GMimeMessage) message = NULL;
	InternetAddressList *from;
	const gchar *header;
	gsize length;
	gconstpointer data = g_bytes_get_data(raw, &length);
	memset(p, 0, sizeof *p);
	p->addresses = g_ptr_array_new_with_free_func(g_free);
	p->references = g_ptr_array_new_with_free_func(g_free);
	p->attachments = g_ptr_array_new_with_free_func(attachment_free);
	stream = g_mime_stream_mem_new_with_buffer(data, length);
	parser = g_mime_parser_new_with_stream(stream);
	message = g_mime_parser_construct_message(parser, NULL);
	if (!message) return refuse(error, VENTURE_ERROR_VALIDATION, "Message could not be parsed");
	from = g_mime_message_get_from(message);
	if (from && internet_address_list_length(from) > 0) {
		InternetAddress *first = internet_address_list_get_address(from, 0);
		if (INTERNET_ADDRESS_IS_MAILBOX(first)) p->from = g_ascii_strdown(internet_address_mailbox_get_addr(INTERNET_ADDRESS_MAILBOX(first)), -1);
		p->from_name = g_strdup(internet_address_get_name(first));
	}
	collect_addresses(from, p->addresses, capture_address, &p->to_capture);
	collect_addresses(g_mime_message_get_to(message), p->addresses, capture_address, &p->to_capture);
	collect_addresses(g_mime_message_get_cc(message), p->addresses, capture_address, &p->to_capture);
	p->message_id = strip_brackets(g_mime_message_get_message_id(message));
	header = g_mime_object_get_header(GMIME_OBJECT(message), "In-Reply-To");
	if (header) p->in_reply_to = strip_brackets(header);
	header = g_mime_object_get_header(GMIME_OBJECT(message), "References");
	if (header) {
		g_auto(GStrv) parts = g_strsplit_set(header, " \t\r\n,", -1);
		guint i;
		for (i = 0; parts[i]; i++) if (*parts[i]) g_ptr_array_add(p->references, strip_brackets(parts[i]));
	}
	p->subject = g_strdup(g_mime_message_get_subject(message));
	{
		GDateTime *date = g_mime_message_get_date(message);
		p->date = date ? g_date_time_to_utc(date) : g_date_time_new_now_utc();
	}
	g_mime_message_foreach(message, walk_part, p);
	if (!p->text && GMIME_IS_TEXT_PART(g_mime_message_get_mime_part(message)))
		p->text = g_mime_text_part_get_text(GMIME_TEXT_PART(g_mime_message_get_mime_part(message)));
	return TRUE;
}

/* --- Filing bytes as documents, the way the upload route does ------------ */
static VentureEntity *file_document(VentureMailSyncService *self, gint64 org, const gchar *title, const gchar *kind, const gchar *mime, GBytes *data, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) document = NULL;
	g_autofree gchar *safe = NULL, *token = NULL, *stored = NULL, *path = NULL, *checksum = NULL, *extracted = NULL;
	gsize length;
	gconstpointer bytes = g_bytes_get_data(data, &length);
	if (!self->attachment_root) { refuse(error, VENTURE_ERROR_CONFIG, "Attachment storage is not configured"); return NULL; }
	if (g_mkdir_with_parents(self->attachment_root, 0700)) { refuse(error, VENTURE_ERROR_FAILED, "Cannot create attachment storage"); return NULL; }
	safe = g_strcanon(g_strdup(title && *title ? title : "message"), "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-", '_');
	token = venture_generate_token(8);
	stored = g_strdup_printf("%s_%s", token, safe);
	path = g_build_filename(self->attachment_root, stored, NULL);
	if (!g_file_set_contents(path, bytes ? bytes : "", (gssize)length, error)) return NULL;
	checksum = g_compute_checksum_for_bytes(G_CHECKSUM_SHA256, data);
	/* Text arrives as searchable text; a PDF or image is stored and read later, as an upload is. */
	if (mime && (g_str_has_prefix(mime, "text/") || g_str_has_prefix(mime, "message/")) && bytes && length && g_utf8_validate(bytes, (gssize)length, NULL))
		extracted = g_strndup(bytes, length);
	document = g_object_new(VENTURE_TYPE_DOCUMENT, "organization-id", org, "title", title, "kind", kind, "path", path,
		"mime-type", mime, "size-bytes", (gint64)length, "hash", checksum, "extracted-text", extracted, NULL);
	if (!venture_database_save(self->database, document, actor, error)) return NULL;
	return g_steal_pointer(&document);
}
static gboolean hash_already_captured(VentureMailSyncService *self, gint64 org, GBytes *data, GError **error)
{
	g_autofree gchar *checksum = g_compute_checksum_for_bytes(G_CHECKSUM_SHA256, data);
	g_autoptr(VentureQuery) documents = venture_query_new(VENTURE_TYPE_DOCUMENT);
	g_autoptr(GPtrArray) rows = NULL;
	guint i;
	venture_query_set_organization(documents, org);
	venture_query_add_filter_string(documents, "hash", VENTURE_FILTER_OP_EQ, checksum, NULL);
	rows = venture_database_find(self->database, documents, error);
	if (!rows) return FALSE;
	for (i = 0; i < rows->len; i++) {
		g_autoptr(VentureQuery) items = venture_query_new(VENTURE_TYPE_CAPTURE_ITEM);
		venture_query_set_organization(items, org);
		venture_query_add_filter_int(items, "document-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(g_ptr_array_index(rows, i)), NULL);
		if (venture_database_count(self->database, items, NULL) > 0) return TRUE;
	}
	return FALSE;
}

/* --- Matching ------------------------------------------------------------ */
static GPtrArray *matching_contacts(VentureDatabase *db, gint64 org, GPtrArray *addresses, GPtrArray *unmatched, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_CONTACT);
	g_autoptr(GPtrArray) contacts = NULL;
	GPtrArray *matched = g_ptr_array_new_with_free_func(g_object_unref);
	guint i, c;
	venture_query_set_organization(query, org);
	venture_query_set_limit(query, 0);
	contacts = venture_database_find(db, query, error);
	if (!contacts) { g_ptr_array_unref(matched); return NULL; }
	for (i = 0; i < addresses->len; i++) {
		g_autofree gchar *wanted = venture_lead_normalize_email(g_ptr_array_index(addresses, i));
		gboolean found = FALSE;
		if (!*wanted) continue;
		for (c = 0; c < contacts->len; c++) {
			VentureEntity *contact = g_ptr_array_index(contacts, c);
			g_autofree gchar *email = NULL, *normal = NULL;
			guint m;
			g_object_get(contact, "email", &email, NULL);
			normal = venture_lead_normalize_email(email);
			if (g_strcmp0(normal, wanted)) continue;
			found = TRUE;
			for (m = 0; m < matched->len; m++) if (g_ptr_array_index(matched, m) == contact) break;
			if (m == matched->len) g_ptr_array_add(matched, g_object_ref(contact));
		}
		if (!found && unmatched) g_ptr_array_add(unmatched, g_strdup(g_ptr_array_index(addresses, i)));
	}
	return matched;
}
static gchar *resolve_thread(VentureDatabase *db, gint64 org, Parsed *p, GError **error)
{
	g_autoptr(GPtrArray) candidates = g_ptr_array_new();
	guint i;
	if (p->in_reply_to && *p->in_reply_to) g_ptr_array_add(candidates, p->in_reply_to);
	for (i = 0; i < p->references->len; i++) g_ptr_array_add(candidates, g_ptr_array_index(p->references, i));
	for (i = 0; i < candidates->len; i++) {
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_MAIL_INBOUND);
		g_autoptr(VentureEntity) earlier = NULL;
		venture_query_set_organization(query, org);
		venture_query_add_filter_string(query, "message-id", VENTURE_FILTER_OP_EQ, g_ptr_array_index(candidates, i), NULL);
		earlier = venture_database_find_one(db, query, error);
		if (error && *error) return NULL;
		if (earlier) { gchar *thread = NULL; g_object_get(earlier, "thread-id", &thread, NULL); if (thread && *thread) return thread; g_free(thread); }
	}
	/* No known ancestor: the oldest reference is the root, else this message. */
	if (p->references->len) return g_strdup(g_ptr_array_index(p->references, 0));
	if (p->in_reply_to && *p->in_reply_to) return g_strdup(p->in_reply_to);
	return g_strdup(p->message_id && *p->message_id ? p->message_id : "");
}
static VentureEntity *record_interaction(VentureDatabase *db, gint64 org, VentureEntity *contact, const gchar *subject, const gchar *body, GDateTime *when, gboolean outbound, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) interaction = g_object_new(VENTURE_TYPE_INTERACTION, "organization-id", org, NULL);
	gint64 company = 0;
	g_object_get(contact, "company-id", &company, NULL);
	g_object_set(interaction, "contact-id", venture_entity_get_id(contact), "company-id", company, "kind", VENTURE_INTERACTION_KIND_EMAIL,
		"subject", subject && *subject ? subject : "(no subject)", "body", body, "occurred-at", when, "outbound", outbound, NULL);
	if (!venture_database_save(db, interaction, actor, error)) return NULL;
	return g_steal_pointer(&interaction);
}
static gboolean note_unmatched(VentureDatabase *db, gint64 org, const gchar *address, const gchar *name, const gchar *subject, GDateTime *when, const VentureActor *actor, GError **error)
{
	g_autofree gchar *normal = venture_lead_normalize_email(address);
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_MAIL_UNMATCHED_SENDER);
	g_autoptr(VentureEntity) row = NULL;
	gint64 seen = 0;
	if (!*normal) return TRUE;
	venture_query_set_organization(query, org);
	venture_query_add_filter_string(query, "address", VENTURE_FILTER_OP_EQ, normal, NULL);
	row = venture_database_find_one(db, query, error);
	if (error && *error) return FALSE;
	if (!row) row = g_object_new(VENTURE_TYPE_MAIL_UNMATCHED_SENDER, "organization-id", org, "address", normal, NULL);
	else g_object_get(row, "seen", &seen, NULL);
	g_object_set(row, "seen", seen + 1, "last-subject", subject, "last-seen-at", when, NULL);
	if (name && *name) g_object_set(row, "name", name, NULL);
	return venture_database_save(db, row, actor, error);
}

/* --- One message, one transaction ---------------------------------------- */
static gboolean process_message(VentureMailSyncService *self, VentureEntity *account, const gchar *folder, guint32 uid, GBytes *raw, JsonObject *cursors, const VentureActor *actor, GError **error)
{
	Parsed p;
	g_autofree gchar *own = NULL, *capture_address = NULL, *capture_folder = NULL, *key = NULL, *thread = NULL, *body = NULL, *raw_title = NULL, *cursor_text = NULL;
	g_autoptr(VentureEntity) inbound = NULL, raw_document = NULL;
	g_autoptr(GPtrArray) matched = NULL, unmatched = NULL;
	g_autoptr(JsonNode) cursor_node = NULL;
	gint64 org = venture_entity_get_organization_id(account);
	gboolean outbound, capture;
	guint i;
	g_object_get(account, "address", &own, "capture-address", &capture_address, "capture-folder", &capture_folder, NULL);
	if (!parse_message(raw, capture_address, &p, error)) return FALSE;
	if (!venture_database_begin(self->database, error)) { parsed_clear(&p); return FALSE; }
	key = g_strdup_printf("%" G_GINT64_FORMAT ":%s:%u", venture_entity_get_id(account), folder, uid);
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_MAIL_INBOUND);
		venture_query_set_organization(query, org);
		venture_query_add_filter_string(query, "uid-key", VENTURE_FILTER_OP_EQ, key, NULL);
		if (venture_database_count(self->database, query, NULL) > 0) { venture_database_rollback(self->database); parsed_clear(&p); return TRUE; }
	}
	outbound = own && p.from && !g_ascii_strcasecmp(own, p.from);
	capture = p.to_capture || (capture_folder && *capture_folder && !g_strcmp0(capture_folder, folder));
	raw_title = g_strdup_printf("%s.eml", p.subject && *p.subject ? p.subject : "message");
	raw_document = file_document(self, org, raw_title, "email", "message/rfc822", raw, actor, error);
	if (!raw_document) goto fail;
	thread = resolve_thread(self->database, org, &p, error);
	if (!thread) goto fail;
	inbound = g_object_new(VENTURE_TYPE_MAIL_INBOUND, "organization-id", org, "account-id", venture_entity_get_id(account), "folder", folder,
		"uid", (gint64)uid, "uid-key", key, "message-id", p.message_id, "thread-id", thread, "from-address", p.from, "subject", p.subject,
		"received-at", p.date, "document-id", venture_entity_get_id(raw_document), "outbound", outbound, NULL);
	body = g_strdup_printf("From: %s\nMessage-ID: %s\nThread: %s\n\n%s", p.from ? p.from : "", p.message_id, thread, p.text ? p.text : "");
	if (!capture) {
		/* The account's own address is never a contact match nor an unmatched sender. */
		g_autoptr(GPtrArray) others = g_ptr_array_new_with_free_func(g_free);
		for (i = 0; i < p.addresses->len; i++)
			if (!own || g_ascii_strcasecmp(own, g_ptr_array_index(p.addresses, i))) g_ptr_array_add(others, g_strdup(g_ptr_array_index(p.addresses, i)));
		unmatched = g_ptr_array_new_with_free_func(g_free);
		matched = matching_contacts(self->database, org, others, unmatched, error);
		if (!matched) goto fail;
		for (i = 0; i < matched->len; i++) {
			g_autoptr(VentureEntity) interaction = record_interaction(self->database, org, g_ptr_array_index(matched, i), p.subject, body, p.date, outbound, actor, error);
			if (!interaction) goto fail;
			if (i == 0) g_object_set(inbound, "contact-id", venture_entity_get_id(g_ptr_array_index(matched, 0)), "interaction-id", venture_entity_get_id(interaction), NULL);
		}
		for (i = 0; i < unmatched->len; i++) {
			const gchar *address = g_ptr_array_index(unmatched, i);
			if (!note_unmatched(self->database, org, address, !g_strcmp0(address, p.from) ? p.from_name : NULL, p.subject, p.date, actor, error)) goto fail;
		}
	} else if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "capture_item") != G_TYPE_INVALID) {
		/* Each attachment is a receipt; a bare body is the receipt itself. Same bytes as an upload: skip. */
		VentureCaptureService *capture_service = venture_capture_service_get(self->database);
		gboolean any = FALSE;
		for (i = 0; i < p.attachments->len; i++) {
			Attachment *a = g_ptr_array_index(p.attachments, i);
			g_autoptr(VentureEntity) document = NULL, item = NULL;
			g_autoptr(GError) dup_error = NULL;
			gboolean duplicate = hash_already_captured(self, org, a->data, &dup_error);
			if (dup_error) { g_propagate_error(error, g_steal_pointer(&dup_error)); goto fail; }
			if (duplicate) continue;
			document = file_document(self, org, a->name, "receipt", a->mime, a->data, actor, error);
			if (!document) goto fail;
			item = venture_capture_service_ingest_for_organization(capture_service, org, "receipt", p.subject && *p.subject ? p.subject : a->name, "email",
				venture_entity_get_id(document), p.from_name && *p.from_name ? p.from_name : p.from, NULL, p.date, p.text, actor, error);
			if (!item) goto fail;
			if (!any) g_object_set(inbound, "capture-item-id", venture_entity_get_id(item), NULL);
			any = TRUE;
		}
		if (!any && !p.attachments->len && p.text && *p.text) {
			g_autoptr(GBytes) text = g_bytes_new(p.text, strlen(p.text));
			g_autoptr(GError) dup_error = NULL;
			gboolean duplicate = hash_already_captured(self, org, text, &dup_error);
			if (dup_error) { g_propagate_error(error, g_steal_pointer(&dup_error)); goto fail; }
			if (!duplicate) {
				g_autoptr(VentureEntity) document = file_document(self, org, raw_title, "receipt", "text/plain", text, actor, error);
				g_autoptr(VentureEntity) item = NULL;
				if (!document) goto fail;
				item = venture_capture_service_ingest_for_organization(capture_service, org, "receipt", p.subject && *p.subject ? p.subject : raw_title, "email",
					venture_entity_get_id(document), p.from_name && *p.from_name ? p.from_name : p.from, NULL, p.date, p.text, actor, error);
				if (!item) goto fail;
				g_object_set(inbound, "capture-item-id", venture_entity_get_id(item), NULL);
			}
		}
	}
	if (!venture_database_save(self->database, inbound, actor, error)) goto fail;
	json_object_set_int_member(cursors, folder, uid);
	cursor_node = json_node_new(JSON_NODE_OBJECT);
	json_node_set_object(cursor_node, cursors);
	cursor_text = json_to_string(cursor_node, FALSE);
	g_object_set(account, "cursors", cursor_text, NULL);
	if (!venture_database_save(self->database, account, actor, error)) goto fail;
	if (!venture_database_commit(self->database, error)) { parsed_clear(&p); return FALSE; }
	parsed_clear(&p);
	return TRUE;
fail:
	venture_database_rollback(self->database);
	parsed_clear(&p);
	return FALSE;
}

gint venture_mail_sync_service_sync(VentureMailSyncService *self, VentureEntity *account, const VentureActor *actor, GError **error)
{
	g_autofree gchar *host = NULL, *security = NULL, *username = NULL, *secret_env = NULL, *folders = NULL, *cursor_text = NULL;
	g_auto(GStrv) names = NULL;
	g_autoptr(JsonNode) cursor_node = NULL;
	g_autoptr(JsonObject) cursors = NULL;
	g_autoptr(GDateTime) now = NULL;
	g_autoptr(GError) local = NULL;
	const gchar *secret;
	gint64 port = 0;
	gint count = 0;
	guint f;
	g_return_val_if_fail(VENTURE_IS_MAIL_SYNC_SERVICE(self), -1);
	g_return_val_if_fail(VENTURE_IS_MAIL_ACCOUNT(account), -1);
	if (!self->database) return refuse(error, VENTURE_ERROR_DATABASE, "The database has been closed"), -1;
	if (!module_on()) return refuse(error, VENTURE_ERROR_CONFIG, "The mail_sync module is disabled"), -1;
	if (self->busy) return refuse(error, VENTURE_ERROR_CONFLICT, "A sync is already in progress"), -1;
	if (venture_database_has_transaction(self->database)) return refuse(error, VENTURE_ERROR_CONFLICT, "Sync requires a committed database"), -1;
	if (!venture_entity_get_id(account)) return refuse(error, VENTURE_ERROR_VALIDATION, "Sync requires a saved account"), -1;
	g_object_get(account, "imap-host", &host, "imap-port", &port, "imap-tls", &security, "username", &username,
		"secret-env", &secret_env, "folders", &folders, "cursors", &cursor_text, NULL);
	if (venture_string_is_empty(secret_env)) return refuse(error, VENTURE_ERROR_CONFIG, "The account names no secret_env variable"), -1;
	secret = g_getenv(secret_env);
	if (!secret || !*secret) {
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG, "VentureMailSyncService: environment variable %s is not set; it must hold the IMAP password or app token", secret_env);
		return -1;
	}
	if (venture_string_is_empty(host)) return refuse(error, VENTURE_ERROR_CONFIG, "The account has no IMAP host"), -1;
	if (port <= 0 || port > 65535) port = !g_strcmp0(security, "tls") ? 993 : 143;
	cursor_node = cursor_text && *cursor_text ? json_from_string(cursor_text, NULL) : NULL;
	cursors = cursor_node && JSON_NODE_HOLDS_OBJECT(cursor_node) ? json_object_ref(json_node_get_object(cursor_node)) : json_object_new();
	names = g_strsplit(venture_string_is_empty(folders) ? "INBOX" : folders, ",", -1);
	self->busy = TRUE;
	if (!venture_imap_client_connect(self->client, host, (guint16)port, venture_string_is_empty(security) ? "tls" : security, username, secret, NULL, &local)) goto done;
	for (f = 0; names[f] && !local; f++) {
		const gchar *folder = g_strstrip(names[f]);
		g_autoptr(GArray) uids = NULL;
		guint32 last = (guint32)(json_object_has_member(cursors, folder) ? json_object_get_int_member(cursors, folder) : 0);
		guint i;
		if (!*folder) continue;
		if (!venture_imap_client_select(self->client, folder, NULL, &local)) break;
		uids = venture_imap_client_uids_after(self->client, last, NULL, &local);
		if (!uids) break;
		for (i = 0; i < uids->len; i++) {
			guint32 uid = g_array_index(uids, guint32, i);
			g_autoptr(GBytes) raw = venture_imap_client_fetch(self->client, uid, NULL, &local);
			if (!raw) break;
			if (!process_message(self, account, folder, uid, raw, cursors, actor, &local)) break;
			count++;
		}
	}
	venture_imap_client_disconnect(self->client);
done:
	self->busy = FALSE;
	now = g_date_time_new_now_utc();
	/* The outcome is recorded on the account either way, so the list shows a failing mailbox. */
	{
		g_autoptr(GError) note_error = NULL;
		g_object_set(account, "last-synced-at", now, "last-error", local ? local->message : NULL, NULL);
		if (!venture_database_save(self->database, account, actor, &note_error) && !local) { g_propagate_error(error, g_steal_pointer(&note_error)); return -1; }
	}
	if (local) { g_propagate_error(error, g_steal_pointer(&local)); return -1; }
	return count;
}

JsonNode *venture_mail_sync_service_sweep(VentureMailSyncService *self, gint64 organization_id, guint limit, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_MAIL_ACCOUNT);
	g_autoptr(GPtrArray) accounts = NULL;
	g_autoptr(JsonBuilder) builder = json_builder_new();
	guint i, synced = 0;
	gint messages = 0;
	g_return_val_if_fail(VENTURE_IS_MAIL_SYNC_SERVICE(self), NULL);
	if (organization_id <= 0) return refuse(error, VENTURE_ERROR_VALIDATION, "An exact organization is required"), NULL;
	if (!module_on()) return refuse(error, VENTURE_ERROR_CONFIG, "The mail_sync module is disabled"), NULL;
	venture_query_set_organization(query, organization_id);
	venture_query_add_filter_string(query, "active", VENTURE_FILTER_OP_EQ, "true", NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	accounts = venture_database_find(self->database, query, error);
	if (!accounts) return NULL;
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "errors");
	json_builder_begin_array(builder);
	for (i = 0; i < accounts->len && synced < limit; i++) {
		g_autoptr(GError) local = NULL;
		gint n = venture_mail_sync_service_sync(self, g_ptr_array_index(accounts, i), actor, &local);
		synced++;
		if (n < 0) {
			json_builder_begin_object(builder);
			json_builder_set_member_name(builder, "account"); json_builder_add_int_value(builder, venture_entity_get_id(g_ptr_array_index(accounts, i)));
			json_builder_set_member_name(builder, "error"); json_builder_add_string_value(builder, local ? local->message : "unknown");
			json_builder_end_object(builder);
		} else messages += n;
	}
	json_builder_end_array(builder);
	json_builder_set_member_name(builder, "accounts"); json_builder_add_int_value(builder, synced);
	json_builder_set_member_name(builder, "messages"); json_builder_add_int_value(builder, messages);
	json_builder_end_object(builder);
	return json_builder_get_root(builder);
}

VentureEntity *venture_mail_sync_service_create_contact(VentureMailSyncService *self, VentureEntity *sender, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) contact = NULL;
	g_autofree gchar *address = NULL, *name = NULL;
	g_return_val_if_fail(VENTURE_IS_MAIL_SYNC_SERVICE(self), NULL);
	g_return_val_if_fail(VENTURE_IS_MAIL_UNMATCHED_SENDER(sender), NULL);
	if (!self->database) return refuse(error, VENTURE_ERROR_DATABASE, "The database has been closed"), NULL;
	g_object_get(sender, "address", &address, "name", &name, NULL);
	if (!venture_database_begin(self->database, error)) return NULL;
	contact = g_object_new(VENTURE_TYPE_CONTACT, "organization-id", venture_entity_get_organization_id(sender),
		"name", name && *name ? name : address, "email", address, "source", "email", NULL);
	if (!venture_database_save(self->database, contact, actor, error) || !venture_database_delete(self->database, sender, actor, error)) {
		venture_database_rollback(self->database); return NULL;
	}
	if (!venture_database_commit(self->database, error)) return NULL;
	return g_steal_pointer(&contact);
}

gboolean venture_mail_sync_record_outbound(VentureDatabase *database, VentureMailMessage *message, GError **error)
{
	g_autofree gchar *to = NULL, *cc = NULL, *subject = NULL, *body = NULL, *message_id = NULL, *text = NULL;
	g_autoptr(GPtrArray) addresses = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GPtrArray) matched = NULL;
	g_autoptr(GDateTime) sent_at = NULL;
	gint64 org = venture_entity_get_organization_id(VENTURE_ENTITY(message));
	guint i;
	if (!module_on() || venture_entity_registry_lookup(venture_entity_registry_get_default(), "contact") == G_TYPE_INVALID) return TRUE;
	g_object_get(message, "to", &to, "cc", &cc, "subject", &subject, "text-body", &text, "message-id", &message_id, "sent-at", &sent_at, NULL);
	{
		g_autofree gchar *joined = g_strdup_printf("%s,%s", to ? to : "", cc ? cc : "");
		g_auto(GStrv) parts = g_strsplit(joined, ",", -1);
		for (i = 0; parts[i]; i++) {
			gchar *address = g_strstrip(parts[i]), *open = strrchr(address, '<');
			if (open) { address = open + 1; if (strchr(address, '>')) *strchr(address, '>') = '\0'; }
			if (*address) g_ptr_array_add(addresses, g_ascii_strdown(address, -1));
		}
	}
	matched = matching_contacts(database, org, addresses, NULL, error);
	if (!matched) return FALSE;
	/* The private body is a credential and never reaches the timeline. */
	body = g_strdup_printf("Message-ID: %s\n\n%s", message_id ? message_id : "", text ? text : "");
	for (i = 0; i < matched->len; i++) {
		g_autoptr(VentureEntity) interaction = record_interaction(database, org, g_ptr_array_index(matched, i), subject, body, sent_at, TRUE, NULL, error);
		if (!interaction) return FALSE;
	}
	return TRUE;
}
