/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <mail-glib.h>
#include <glib/gstdio.h>
#include <unistd.h>
#include <string.h>
struct _VentureSmtpMailer {
	GObject parent_instance;
	VentureConfig *config;
	gchar *profile, *from, *from_name, *reply_to;
};
static void smtp_iface(VentureMailerInterface *iface);
G_DEFINE_FINAL_TYPE_WITH_CODE(VentureSmtpMailer, venture_smtp_mailer, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(VENTURE_TYPE_MAILER, smtp_iface))
static void configured(GObject *object)
{
	VentureSmtpMailer *self = VENTURE_SMTP_MAILER(object);
	g_autoptr(JsonBuilder) b = json_builder_new();
	g_autoptr(JsonNode) node = NULL;
	g_autofree gchar *host = NULL, *security = NULL, *username = NULL, *env = NULL, *password = NULL;
	gint64 port;
	g_object_get(self->config, "mail-host", &host, "mail-port", &port, "mail-security", &security,
		"mail-username", &username, "mail-password-env", &env, "mail-from-address", &self->from,
		"mail-from-name", &self->from_name, "mail-reply-to", &self->reply_to, NULL);
	json_builder_begin_object(b);
	json_builder_set_member_name(b, "provider"); json_builder_add_string_value(b, "venture");
	json_builder_set_member_name(b, "profiles"); json_builder_begin_object(b);
	json_builder_set_member_name(b, "venture"); json_builder_begin_object(b);
#define VALUE(k,v) json_builder_set_member_name(b, k); json_builder_add_string_value(b, v)
	VALUE("host", host);
	VALUE("tls", !g_strcmp0(security, "tls") ? "implicit" : security);
	VALUE("auth", username && *username ? "plain" : "none");
	json_builder_set_member_name(b, "port"); json_builder_add_int_value(b, port);
	json_builder_set_member_name(b, "retries"); json_builder_add_int_value(b, 0);
	json_builder_set_member_name(b, "timeout"); json_builder_add_int_value(b, 30);
	if (username && *username) {
		password = g_strdup_printf("env:%s", env ? env : "");
		VALUE("username", username);
		VALUE("password", password);
	}
#undef VALUE
	json_builder_end_object(b); json_builder_end_object(b); json_builder_end_object(b);
	node = json_builder_get_root(b);
	self->profile = json_to_string(node, FALSE);
	G_OBJECT_CLASS(venture_smtp_mailer_parent_class)->constructed(object);
}
static gboolean addresses(MailMessage *message, GMimeAddressType type, const gchar *value, GError **error)
{
	InternetAddressList *list;
	gint i;
	gboolean ok = TRUE;
	if (!value || !*value) return TRUE;
	if (strchr(value, '\r') || strchr(value, '\n')) {
		g_set_error_literal(error, MAIL_ERROR, MAIL_ERROR_MESSAGE, "Invalid recipient header"); return FALSE;
	}
	list = internet_address_list_parse(NULL, value);
	if (!list || !internet_address_list_length(list)) {
		g_clear_object(&list);
		g_set_error_literal(error, MAIL_ERROR, MAIL_ERROR_MESSAGE, "Invalid recipient address"); return FALSE;
	}
	for (i = 0; i < internet_address_list_length(list); i++) {
		InternetAddress *address = internet_address_list_get_address(list, i);
		if (!INTERNET_ADDRESS_IS_MAILBOX(address)) {
			g_set_error_literal(error, MAIL_ERROR, MAIL_ERROR_MESSAGE, "Mailbox groups are not supported"); ok = FALSE; break;
		}
		if (!mail_message_add_address(message, type, internet_address_get_name(address),
			internet_address_mailbox_get_addr(INTERNET_ADDRESS_MAILBOX(address)), error)) { ok = FALSE; break; }
	}
	g_object_unref(list);
	return ok;
}
static gboolean smtp_send(VentureMailer *mailer, VentureMailMessage *message, GCancellable *cancellable, GError **error)
{
	VentureSmtpMailer *self = VENTURE_SMTP_MAILER(mailer);
	g_autoptr(MailConfig) config = NULL;
	g_autoptr(MailTransport) transport = NULL;
	g_autoptr(MailMessage) mail = mail_message_new();
	g_autoptr(GError) local = NULL;
	g_autoptr(GBytes) rendered = NULL, wire = NULL;
	g_autofree gchar *path = NULL, *to = NULL, *cc = NULL, *bcc = NULL, *reply = NULL;
	g_autofree gchar *subject = NULL, *text = NULL, *html = NULL, *id = NULL;
	MailReceipt *receipt = NULL;
	GMimeStream *input = NULL, *output = NULL;
	GMimeParser *parser = NULL;
	GMimeMessage *mime = NULL;
	GMimeFormatOptions *options = NULL;
	const gchar *bytes;
	gsize length;
	gint fd;
	VentureError code;
	gboolean ok = FALSE;
	fd = g_file_open_tmp("venture-mail-config-XXXXXX", &path, &local);
	if (fd < 0) goto out;
	close(fd);
	/* Only the environment variable NAME is written, never its value. */
	if (g_file_set_contents(path, self->profile, -1, &local)) config = mail_config_load(path, cancellable, &local);
	g_unlink(path);
	if (!config) goto out;
	transport = mail_transport_new(config, &local);
	if (!transport) goto out;
	g_object_get(message, "to", &to, "cc", &cc, "bcc", &bcc, "reply-to", &reply,
		"subject", &subject, "text-body", &text, "html-body", &html, "message-id", &id, NULL);
	if (!mail_message_add_address(mail, GMIME_ADDRESS_TYPE_FROM, self->from_name, self->from, &local) ||
		!addresses(mail, GMIME_ADDRESS_TYPE_TO, to, &local) || !addresses(mail, GMIME_ADDRESS_TYPE_CC, cc, &local) ||
		!addresses(mail, GMIME_ADDRESS_TYPE_BCC, bcc, &local) ||
		!addresses(mail, GMIME_ADDRESS_TYPE_REPLY_TO, reply && *reply ? reply : self->reply_to, &local) ||
		!mail_message_set_subject(mail, subject, &local) ||
		!mail_message_set_bodies(mail, text ? text : "", html && *html ? html : NULL, &local)) goto out;
	{
		const gchar *snapshot = venture_entity_get_attribute(VENTURE_ENTITY(message), "_mail_attachments");
		if (snapshot && *snapshot) {
			g_autoptr(JsonNode) attachments = json_from_string(snapshot, &local);
			JsonArray *array;
			guint i;
			if (!attachments || !JSON_NODE_HOLDS_ARRAY(attachments)) goto out;
			array = json_node_get_array(attachments);
			for (i = 0; i < json_array_get_length(array); i++) {
				JsonObject *attachment = json_array_get_object_element(array, i);
				gsize size;
				guchar *decoded;
				g_autoptr(GBytes) content = NULL;
				decoded = g_base64_decode(venture_json_object_get_string(attachment, "data", ""), &size);
				content = g_bytes_new_take(decoded, size);
				if (!mail_message_add_attachment(mail, venture_json_object_get_string(attachment, "name", "attachment"),
					venture_json_object_get_string(attachment, "mime", "application/octet-stream"), content, NULL, &local)) goto out;
			}
		}
	}
	rendered = mail_message_render(mail, cancellable, &local);
	if (!rendered) goto out;
	/* mail-glib's public single-attempt delivery interface accepts a MIME
	 * snapshot. GMime sets the durable ID without private MailMessage access. */
	bytes = g_bytes_get_data(rendered, &length);
	input = g_mime_stream_mem_new_with_buffer(bytes, length);
	parser = g_mime_parser_new_with_stream(input);
	mime = g_mime_parser_construct_message(parser, NULL);
	if (id && *id) {
		if (strpbrk(id, "\r\n <>\t") || !strchr(id, '@')) {
			g_set_error_literal(&local, MAIL_ERROR, MAIL_ERROR_MESSAGE, "Invalid Message-ID"); goto out;
		}
		g_mime_message_set_message_id(mime, id);
	}
	output = g_mime_stream_mem_new();
	options = g_mime_format_options_new();
	g_mime_format_options_set_newline_format(options, GMIME_NEWLINE_FORMAT_DOS);
	g_mime_format_options_add_hidden_header(options, "Bcc");
	if (g_mime_object_write_to_stream(GMIME_OBJECT(mime), options, output) < 0) {
		g_set_error_literal(&local, MAIL_ERROR, MAIL_ERROR_MESSAGE, "MIME serialization failed"); goto out;
	}
	{
		GByteArray *array = g_mime_stream_mem_get_byte_array(GMIME_STREAM_MEM(output));
		wire = g_bytes_new(array->data, array->len);
	}
	receipt = MAIL_DELIVERY_GET_IFACE(transport)->deliver(MAIL_DELIVERY(transport), mail, wire, 1, NULL, cancellable, &local);
	ok = receipt != NULL;
out:
	mail_receipt_free(receipt);
	g_clear_object(&mime); g_clear_object(&parser); g_clear_object(&input); g_clear_object(&output);
	if (options) g_mime_format_options_free(options);
	if (ok) return TRUE;
	code = VENTURE_ERROR_MAIL_PERMANENT;
	if (g_error_matches(local, MAIL_ERROR, MAIL_ERROR_UNCERTAIN)) code = VENTURE_ERROR_MAIL_UNCERTAIN;
	else if (g_error_matches(local, MAIL_ERROR, MAIL_ERROR_TEMPORARY) || (local && local->domain == G_IO_ERROR)) code = VENTURE_ERROR_MAIL_TRANSIENT;
	/* Do not persist arbitrary transport messages: a provider may echo an
	 * AUTH value. The stable error code retains the operator's next action. */
	g_set_error_literal(error, VENTURE_ERROR, code,
		code == VENTURE_ERROR_MAIL_UNCERTAIN ? "SMTP acceptance uncertain; deliberate retry required" :
		code == VENTURE_ERROR_MAIL_TRANSIENT ? "SMTP connection or transient transport failure" : "SMTP configuration, message or permanent transport failure");
	return FALSE;
}
static void smtp_iface(VentureMailerInterface *iface) { iface->send = smtp_send; }
static void smtp_finalize(GObject *object)
{
	VentureSmtpMailer *self = VENTURE_SMTP_MAILER(object);
	g_clear_object(&self->config);
	g_free(self->profile); g_free(self->from); g_free(self->from_name); g_free(self->reply_to);
	G_OBJECT_CLASS(venture_smtp_mailer_parent_class)->finalize(object);
}
static void smtp_set(GObject *object, guint id, const GValue *value, GParamSpec *pspec)
{
	if (id == 1) VENTURE_SMTP_MAILER(object)->config = g_value_dup_object(value);
	else G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
}
static void smtp_get(GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
	if (id == 1) g_value_set_object(value, VENTURE_SMTP_MAILER(object)->config);
	else G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
}
static void venture_smtp_mailer_class_init(VentureSmtpMailerClass *klass)
{
	GObjectClass *object = G_OBJECT_CLASS(klass);
	object->constructed = configured; object->finalize = smtp_finalize;
	object->set_property = smtp_set; object->get_property = smtp_get;
	g_object_class_install_property(object, 1, g_param_spec_object("config", "Configuration", "Mail section", VENTURE_TYPE_CONFIG, G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
}
static void venture_smtp_mailer_init(VentureSmtpMailer *self) { }
VentureSmtpMailer *venture_smtp_mailer_new(VentureConfig *config) { return g_object_new(VENTURE_TYPE_SMTP_MAILER, "config", config, NULL); }
