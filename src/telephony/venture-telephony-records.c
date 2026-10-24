/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>

/* Phone calls and text messages (docs/telephony.org). Calls are not here:
 * every call is an ordinary call-log activity written through log_call.
 * These are the connection to a phone provider, the user-to-extension
 * setting and the text messages themselves. */

GType
venture_text_direction_get_type(void)
{
	static gsize type_id = 0;
	if (g_once_init_enter(&type_id))
	{
		static const GEnumValue values[] = {
			{ VENTURE_TEXT_DIRECTION_OUTBOUND, "VENTURE_TEXT_DIRECTION_OUTBOUND", "outbound" },
			{ VENTURE_TEXT_DIRECTION_INBOUND, "VENTURE_TEXT_DIRECTION_INBOUND", "inbound" },
			{ 0, NULL, NULL }
		};
		GType id = g_enum_register_static("VentureTextDirection", values);
		g_once_init_leave(&type_id, id);
	}
	return type_id;
}

/* One phone provider account for one organization. The credentials are an
 * encrypted integration binding; this row holds only what is safe to show,
 * and the hash of the inbound webhook address token (the token itself is
 * shown once, when it is made). Written only by the telephony service. */
static const VentureFieldDecl account_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", "What you call this line, for example Office phones"),
	VENTURE_FIELD("provider", "Provider", "The connector: 3cx or voipms", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("base-url", "Address", "The provider's API address; for 3CX your phone system's own address", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("phone-number", "Texting number", "The number texts are sent from, in international form, for example +15550100100", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_REF("connection-id", "Credentials", "The encrypted integration binding", "integration_connection", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("webhook-token-hash", "Webhook token hash", NULL, VENTURE_FIELD_KIND_STRING,
		VENTURE_COLUMN_FLAG_UNIQUE | VENTURE_COLUMN_FLAG_SENSITIVE | VENTURE_COLUMN_FLAG_TECHNICAL),
	VENTURE_FIELD_TEXT("webhook-sources", "Allowed senders", "Addresses the provider calls from, one per line (an address or address/prefix); empty allows any"),
	VENTURE_FIELD_TEXT("media-hosts", "Picture hosts", "Hosts inbound pictures may be fetched from, one per line"),
	VENTURE_FIELD("active", "Active", NULL, VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_TEXT("last-error", "Last error", "The last refusal from the provider, in plain words")
};
VENTURE_DEFINE_ENTITY_WITH_CODE(VentureTelephonyAccount, venture_telephony_account, account_fields,
	venture_entity_class_set_labels(VENTURE_ENTITY_CLASS(klass), "Phone line", "Phone lines");)

/* Which phone extension a person answers, in one organization. A
 * click-to-call rings this extension first and then dials out. */
static const VentureFieldDecl extension_fields[] = {
	VENTURE_FIELD_REF("user-id", "Person", NULL, "user", VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("extension", "Extension", "Their extension on the phone system, for example 101", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("user-key", "One per person", NULL, VENTURE_FIELD_KIND_STRING,
		VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION | VENTURE_COLUMN_FLAG_TECHNICAL)
};
VENTURE_DEFINE_ENTITY_WITH_CODE(VentureTelephonyExtension, venture_telephony_extension, extension_fields,
	venture_entity_class_set_labels(VENTURE_ENTITY_CLASS(klass), "Phone extension", "Phone extensions");)

/* One text message, either way. The message key is the replay identity:
 * an inbound message is keyed by its line and the provider's message id,
 * so a webhook delivered twice is one message. Written only by the
 * telephony service; its send path is the only thing that hands one to a
 * provider. */
static const VentureFieldDecl text_fields[] = {
	VENTURE_FIELD_REF("account-id", "Line", NULL, "telephony_account", VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_REF("contact-id", "Contact", NULL, "contact", VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_ENUM("direction", "Direction", NULL, venture_text_direction_get_type, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("from-number", "From", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("to-number", "To", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_TEXT("body", "Message", NULL),
	VENTURE_FIELD_TEXT("media", "Pictures", "Outbound: the hosted picture addresses sent, one per line"),
	VENTURE_FIELD("media-documents", "Received pictures", "Inbound: the private documents the pictures were stored as", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("state", "State", "queued, held, sent, delivered, failed or received", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("hold-until", "Held until", "Quiet hours: the earliest time it may be sent", VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("provider-message-id", "Provider message id", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("message-key", "Message identity", NULL, VENTURE_FIELD_KIND_STRING,
		VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION | VENTURE_COLUMN_FLAG_TECHNICAL),
	VENTURE_FIELD("origin", "Sent by", "manual, keyword, sequence or campaign, with its identity", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("keyword", "Keyword", "Inbound: stop, help or start when the message was one", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("auto-reply", "Automatic reply", "A reply to STOP, HELP or START, sent without consent", VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("error", "Why", "Why it was held or failed, in plain words"),
	VENTURE_FIELD("sent-at", "Sent at", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("delivered-at", "Delivered at", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("received-at", "Received at", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("inbox-ready", "For the inbox", "Inbound and not yet answered", VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_INDEXED)
};
VENTURE_DEFINE_ENTITY_WITH_CODE(VentureTextMessage, venture_text_message, text_fields,
	venture_entity_class_set_labels(VENTURE_ENTITY_CLASS(klass), "Text message", "Text messages");)
