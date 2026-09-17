/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#define STR(n, l) VENTURE_FIELD(n, l, NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE)
#define INT(n, l) VENTURE_FIELD(n, l, NULL, VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE)
#define DATE(n, l) VENTURE_FIELD(n, l, NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_INDEXED)
static const VentureFieldDecl message_fields[] = {
	VENTURE_FIELD_NAME("to", "To", "Comma-separated mailboxes"),
	STR("cc", "Cc"), STR("bcc", "Bcc"), STR("reply-to", "Reply to"),
	VENTURE_FIELD_NAME("subject", "Subject", NULL),
	VENTURE_FIELD_TEXT("text-body", "Text body", NULL),
	/* Bearer links belong only in delivery, never generated records or audit. */
	VENTURE_FIELD("private-text-body", "Private text body", "Service-owned delivery content", VENTURE_FIELD_KIND_TEXT, VENTURE_COLUMN_FLAG_SENSITIVE),
	VENTURE_FIELD_TEXT("html-body", "HTML body", NULL),
	/* The HTML twin of the private text body: an HTML-only template's bearer
	 * link would otherwise vanish when the private text replaces the bodies. */
	VENTURE_FIELD("private-html-body", "Private HTML body", "Service-owned delivery content", VENTURE_FIELD_KIND_TEXT, VENTURE_COLUMN_FLAG_SENSITIVE),
	VENTURE_FIELD("attachments", "Attachments", "Document references: JSON array of type and id", VENTURE_FIELD_KIND_JSON, VENTURE_COLUMN_FLAG_NONE),
	STR("related-type", "Related type"), INT("related-id", "Related record"),
	STR("state", "State"), INT("attempts", "Attempts"),
	DATE("next-attempt-at", "Next attempt"), DATE("lease-until", "Lease expires"),
	VENTURE_FIELD_TEXT("last-error", "Last error", NULL),
	STR("message-id", "Message-ID"), DATE("sent-at", "Sent at"),
	VENTURE_FIELD("idempotency-key", "Idempotency key", "Unique within the organization, including retained rows", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION)
};
VENTURE_DEFINE_ENTITY(VentureMailMessage, venture_mail_message, message_fields)
static const VentureFieldDecl template_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", NULL),
	VENTURE_FIELD_NAME("subject", "Subject", NULL),
	VENTURE_FIELD_TEXT("text-body", "Text body", "Record fields in {braces}"),
	VENTURE_FIELD_TEXT("html-body", "HTML body", "Substituted record values are HTML escaped")
};
VENTURE_DEFINE_ENTITY(VentureMailTemplate, venture_mail_template, template_fields)
